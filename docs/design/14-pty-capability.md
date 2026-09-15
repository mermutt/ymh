# 14 — PTY Capability

`00-architecture.md` §17 sketches a `PtySession` as a capability separate from
one-shot process execution, and `07-tools-execution.md` §6.5 pins the
`PtyService`/`PtySession` seam (so no tool changes when it lands) while leaving
PTY-backed tooling to Phase 2. This spec makes that seam real: it pins the
concrete `PtySession`/`PtyService` interfaces, the spawn options, the async I/O
model on the daemon's loop, the caps and permission gating, the process-tree
ownership and reaping rules (the 07 half of `04` OQ-6), and the model-facing
`terminal` tool.

This is a **design-only** artifact. It is `written`, not `verified`; no code may
be written for this component until the gate in `HANDOFF.md` §7 passes
(`DESIGN_STATUS.md`). The design-first rule (`AGENTS.md`) applies: the
`PtySession`, `PtyService`, `PtyRequest`, and `terminal`-tool signatures here are
**pinned** and must not churn after verification.

This spec **extends** 07; it does not restate it. Where 07 already pins a rule
(`resolve()` path safety, `OutputSink`/`OutputRing`, `ResourceGovernor` caps,
live-event coalescing, subprocess ownership), this spec cites it and pins only
the PTY-specific consequences.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
AgentLoop (06)
   │  dispatches a `terminal` ToolCall (07 §3)
   ▼
terminal tool (this spec, registered in ToolRegistry 07 §4)
   │  ctx.execution().pty() ──► PtyService (this spec)
   │  ctx.resolve(cwd)      ──► ExecutionEnvironment::resolve (07 §6.2, §18)
   │  ctx.governor()        ──► ResourceGovernor::tryAcquirePty (04 §8, F8)
   │  ctx.output()          ──► OutputSink (07 §5.2)
   │  ctx.permission()      ──► §19 seam (09)
   ▼
PtySession (this spec)
   │  master fd  ──► PtyPump on the daemon's io_context (04 §3.4, 11 §3.2)
   │  slave fd   ──► child process group (setpgid + TIOCSCTTY)
   │  output     ──► PtyOutputRing (bounded) + PtyEventSink (live TerminalOutput)
   │  exit       ──► pidfd / specific-pid waitpid (E8; no global SIGCHLD reaper)
   ▼
ToolResult  ──► loop appends payload::ToolResult (01 §4.5, 06 §5.1)
```

PTY is a **capability of the execution environment**, not a tool concern: the
`terminal` tool never opens a master fd, forks, or reaps. It drives the pinned
`PtyService`/`PtySession` seam, exactly as the `shell` tool drives
`ProcessService` (07 §6.4). This keeps PTY/FS/shell movable together to a remote
or sandbox provider later (§18, `§4.4`, `D6`).

### 1.2 Owned responsibilities

- The concrete `PtySession` lifecycle: spawn, `write`, `resize`, `terminate`,
  output consumption, exit status (07 §6.5 forward-declared the seam; this spec
  owns its realization).
- `PtyService`: the per-daemon factory and the daemon-local `PtySession`
  registry (`find`/`list`/`closeSession`).
- `PtyRequest` spawn options (argv, env, cwd, initial size, TERM) and the spawn
  sequence (`openpty`/`forkpty`, `setsid`, `TIOCSCTTY`, `O_NOCTTY`, fd hygiene).
- The async I/O model: the `PtyPump` on the daemon's single `io_context`, the
  `Executor` marshalling seam, and the thread-affinity rules.
- The bounded PTY output ring and the live `TerminalOutput` emission seam
  (`PtyEventSink`), including coalescing and truncation.
- PTY cap accounting: `PtySlot` held for the **PTY lifetime** (not the tool
  call), and the per-daemon/per-session caps (F8, §9.11).
- PTY process-tree ownership: process group, `pidfd`/specific-pid reaping,
  teardown on `terminate`, session end, and daemon shutdown (the 07 half of
  `04` OQ-6; `11` E8).
- The model-facing `terminal` tool schema and its action state machine.
- PTY failure modes (`P-F1…`) and the PTY test plan.

### 1.3 Boundaries — deferred to other specs

- **`resolve()` and rooting.** Owned by 07 §6.2 / `§18`; this spec calls
  `resolve()` and never re-implements containment.
- **`OutputSink`/`OutputRing`, live coalescing, `ToolConfig`.** Owned by 07
  §5.2/§5.5/§8.2; this spec reuses the sink discipline and adds only the PTY
  ring (which is a distinct buffer, §5.4).
- **`ResourceGovernor` and `ResourceCaps`.** Owned by 04 §8/§2.1; this spec
  consumes `tryAcquirePty`/`releasePty` and never re-implements a cap.
- **Daemon lifecycle, `io_context` ownership, signal policy, destruction
  order.** Owned by 04 §3/§3.6 and 11 §3.2/§3.3/§11; this spec is a consumer.
- **Permission policy rules and decision flow.** Owned by 09; this spec only
  supplies the `terminal` tool's `destructive` hint and the argument projection.
- **Tool registry and `ToolContext`.** Owned by 07 §3/§5; this spec registers
  one tool and consumes the context.
- **UI projection of terminal output (ANSI rendering).** Owned by 10
  (`§20.10`, `TerminalLayer` `§20.19`); this spec emits bounded live events and
  a bounded durable result, never `UiEvent`s.
- **Transport / RPC surface.** Owned by 05; `TerminalOutput` reaches the
  supervisor as a live event, not a new RPC.

### 1.4 Seam ownership relative to 01/04/05/06/07/09/10/11

| Concern | Owner | This spec's role |
|---|---|---|
| `ExecutionEnvironment::pty()` | 07 §6.1 (`§18`) | realizes `LocalPtyService` behind it |
| `PtyService`/`PtySession` seam | 07 §6.5 | pins the concrete lifecycle/additive methods |
| `OutputSink`/`OutputRing`, UTF-8 boundary | 07 §5.2/§8.2 | adds `PtyOutputRing` + live emission |
| `ResourceGovernor::tryAcquirePty` | 04 §8 | acquires at `open`, releases at PTY close |
| `io_context` / `Executor` post | 04 §3.4, 11 §3.2 (E2/E6) | pump is loop-affine; tools marshal via `post()` |
| SIGCHLD / one reaper | 11 §3.3 (E8), `signal_policy.hpp` | specific-pid reap only; no global sweep |
| Graceful shutdown step 4 | 04 §3.5 | supplies PTY terminate/reap mechanics |
| `TerminalOutput` live event | 00 §8.1, 05, 10 | emits it via `PtyEventSink` |
| Permission gating | 09 | `terminal` is `ASK`; `ReadOnly` denies |
| `payload::ToolResult` append | 01 §4.5, 06 | returns a result; never appends |
| Destruction ordering | 11 §11.2 (E18) | PTY sessions outlive tools, die before store close |

### 1.5 Relationship to spec 07

07 §6.5 pins the seam and states the Phase-2 schedule. This spec is the
realization of that seam and **amends nothing in 07's pinned signatures**:
`PtySession`'s four §17 methods and `PtyService::open` keep their exact shapes.
All additions in this spec are **additive** (the same pattern 06 used for the
`§10.1` handle and 11 used for post-freeze amendments) and are marked as such.
One recorded interface delta is required at the environment wiring level
(§4.5): `LocalEnvironment` must accept an injected `PtyService*` so the daemon
can supply a loop-aware `LocalPtyService` instead of the v1
`UnavailablePtyService`.

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId` is frozen by 01 §2.1; `ToolCallId`/`TurnId`/`StepId` by 01; the
governor and caps by 04 §2.1. They are reproduced for reference and **not**
redefined. PTY-local types:

```cpp
namespace ymh {

// Daemon-local, monotonic, never durable, never a pid (pid reuse makes a pid an
// unsafe identity). Minted by the owning PtyService. 0 means "invalid".
struct PtySessionId {
    std::uint64_t value = 0;
    auto operator<=>(const PtySessionId&) const = default;
};

// Terminal lifecycle. `Exited` still retains buffered output; `Closed` is
// terminal (master fd closed, child reaped).
enum class PtyState : std::uint8_t {
    Starting,   // master created; child not yet confirmed as controlling proc
    Running,    // slave is the child's controlling terminal
    Exited,     // child reaped; ring may still hold unread output
    Closed,     // master closed; id no longer resolvable
};

// Exit status, mapped exactly like ProcessResult (07 §6.4).
struct PtyExit {
    int  exit_code = -1;
    bool signalled = false;
    int  signal    = 0;
};

// Terminal geometry (character cells, not pixels).
struct PtySize {
    int rows = 24;
    int cols = 80;
};

} // namespace ymh
```

Rules:

- **`PtySessionId` is not durable and not model-authoritative.** The model
  echoes the id it received from `open`; the daemon validates it against the
  owning session's open set (`PtyService::find`). A stale/foreign id is a
  validation error (`P-F9`), never a cross-session handle.
- **`PtyState` is monotonic**: `Starting → Running → Exited → Closed`. There is
  no resurrection; a new terminal is a new `PtySessionId`.
- **A pid is not an identity.** `PtySession::pid()` is diagnostic only (logs,
  tests, the permission projection). Nothing routes by pid.

### 2.2 PTY error taxonomy

Every fallible PTY operation surfaces a code so callers, the tool layer, and
tests share names. `PtyError` derives from the execution error family so the
registry maps it to `ToolResult{outcome = Error, error = to_string(code)}` via
the existing 07 §3.1 path (E-F16).

```cpp
namespace ymh {

enum class PtyErrorCode : std::uint8_t {
    SpawnFailed,    // openpty/forkpty/execve failed (E-F5 analogue)
    NotFound,       // unknown, foreign, or already-Closed PtySessionId
    Closed,         // master closed or slave gone; the session is terminal
    Eio,            // read/write returned EIO (terminal hangup, P-F2)
    WouldBlock,     // bounded outbound queue full (write backpressure, P-F4)
    CapExceeded,    // PTY cap exhausted, global or per-session (P-F6, F8)
    ResizeFailed,   // TIOCSWINSZ failed for a non-fatal reason
    Internal,       // invariant violation / unreachable
};

class PtyError : public std::runtime_error {
public:
    PtyError(PtyErrorCode code, const std::string& what);
    [[nodiscard]] PtyErrorCode code() const noexcept;
};

[[nodiscard]] std::string_view to_string(PtyErrorCode) noexcept;

} // namespace ymh
```

Rules:

- **`Eio` is terminal, not transient.** Once the master reports `EIO`, the child
  has no slave; the pump drains, closes, and transitions to `Exited`. The tool
  never retries a write after `EIO` (`P-F2`).
- **`WouldBlock` is the backpressure signal**, not a crash. The tool maps it to
  a bounded-output error (`P-F4`); the session stays `Running`.
- **`Timeout` is deliberately absent.** A `read` with a bounded wait that
  returns no bytes is `Ok` with empty output (the tool decides), never an error
  (`P-F5`). Only spawn failure, closed master, and cap exhaustion are errors.
- **No `errno` crosses the seam.** The concrete message is short and redacted;
  the code is what callers switch on (`11` E12 spirit).

---

## 3. The `PtySession` interface (pinned)

### 3.1 Class shape

`§17`'s four methods are preserved verbatim; this spec adds identity, state,
output consumption, and exit accessors **additively**.

```cpp
namespace ymh {

// Declared before PtySession because `read` returns Task<PtyRead>.
struct PtyRead {
    std::string data;       // valid UTF-8; ANSI/control bytes preserved
    bool        truncated = false;   // ring evicted older bytes since last read
    bool        eof       = false;   // child exited and ring drained
};

class PtySession {
public:
    virtual ~PtySession() = default;

    // ---- §17 (preserved) ---------------------------------------------------
    virtual void                write(std::string_view) = 0;
    virtual Stream<std::string> output() = 0;
    virtual void                resize(int rows, int cols) = 0;
    virtual void                terminate() = 0;   // SIGHUP -> grace -> SIGKILL

    // ---- additive (this spec) ---------------------------------------------
    virtual PtySessionId id() const noexcept = 0;
    virtual SessionId    session() const noexcept = 0;
    virtual int          pid() const noexcept = 0;       // diagnostic only
    virtual PtyState     state() const noexcept = 0;

    // Bounded, optionally-waiting pull from the session's output ring. Blocks
    // the calling tool thread (a TurnExecutor worker), never the daemon loop.
    // Consumes what it returns. `eof` is set once the child has exited and the
    // ring is drained.
    virtual Task<PtyRead> read(std::size_t max_bytes,
                               std::chrono::milliseconds wait) = 0;

    // Blocking exit wait, bounded by `timeout` (0 => wait until exit). Returns
    // the cached status once the child has been reaped. Used by `close`/tests.
    virtual Task<PtyExit> wait(std::chrono::milliseconds timeout) = 0;
};

} // namespace ymh
```

Rules:

- **`write`/`resize`/`terminate` are thread-safe** and may be called from a
  TurnExecutor worker (the tool thread). They marshal onto the daemon loop; the
  caller does not touch the master fd (`P11`).
- **`output()` is the live push channel** (§3.3), not the tool's read path. The
  `terminal` tool uses `read()`; the UI/live layer subscribes to `output()`.
- **`read` is the only consumer of the ring.** Interleaving `read` calls from
  two threads is not supported; per-session tool calls are serialized in v1
  (07 §10), so this holds (`P15`).
- **`terminate` is idempotent.** Calling it on an `Exited`/`Closed` session is a
  no-op; it never throws for a missing child (`P9`).
- **A `PtySession` is owned by its `PtyService`**, not by the tool. The tool
  holds only a borrowed pointer obtained via `find()` and valid for the
  duration of one serialized call (`P15`).

### 3.2 `write`

```text
write(bytes):
  state = state()
  if state == Exited or state == Closed -> throw PtyError{Closed}
  if bytes.empty()                       -> return (no-op)
  post to the loop:
     if outbound_queue.size() + bytes > write_queue_cap -> signal WouldBlock
     else append; arm async_write if idle
  if the post could not be made (loop stopped) -> throw PtyError{Internal}
```

- **Bounded outbound queue.** `write_queue_cap` defaults to 256 KiB per session
  (a `ToolConfig`-adjacent constant, §6.2). Exceeding it yields
  `PtyError{WouldBlock}` on a **subsequent** `write`; the pump drains the queue
  as the child reads. A single `write` larger than the cap is chunked, not
  rejected.
- **No partial-write surprises.** `write` is all-or-nothing from the caller's
  view: either the whole chunk is queued or `WouldBlock` is raised.
- **Backpressure is observable but never fatal.** The `terminal` tool maps
  `WouldBlock` to a bounded error result (`P-F4`); the PTY remains `Running`.

### 3.3 `output()` and the `Stream<T>` model

07 §6.5 forward-declares `template <class T> class Stream;` and returns
`Stream<std::string>`; no component defines it. PTY is its first consumer, so
this spec pins the minimal shape. It is a **bounded, loop-affine, single-consumer
push stream** backed by shared state; the session holds the producer side.

```cpp
namespace ymh {

template <class T>
class StreamWriter;   // producer side, owned by PtySession

// A lightweight handle over shared stream state (copyable, move-only semantics
// not required). Exactly one consumer may subscribe.
template <class T>
class Stream {
public:
    using Handler = std::function<void(const T&)>;

    Stream() = default;
    Stream(Stream&&) noexcept = default;
    Stream& operator=(Stream&&) noexcept = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Single consumer. The handler runs on the daemon loop, serially, in
    // order. Registering again replaces the previous handler (no fan-out).
    Subscription subscribe(Handler handler);

    [[nodiscard]] bool        closed() const noexcept;   // EOF seen
    [[nodiscard]] std::size_t buffered() const noexcept; // chunks pending
};

} // namespace ymh
```

Rules:

- **Loop-affine delivery.** Chunks are delivered on the daemon loop; a handler
  must be cheap and non-blocking (mirrors 08 §3.3's `StreamSink` rule).
- **Single consumer, replace-on-resubscribe.** Two subscribers do not fan out;
  the second replaces the first. The `terminal` tool is not a subscriber (it
  uses `read()`); the live emitter is.
- **`Stream<T>` is live-only.** It is not durable and not itself an event;
  nothing here is appended to the session log (`P8`).
- **If a general async stream lands later**, it must satisfy this shape; this
  spec is the recorded owner of the forward declaration's definition until then
  (decision (c)).

### 3.4 `resize`

```text
resize(rows, cols):
  if rows <= 0 or cols <= 0  -> throw ToolError{InvalidArguments}
  if state == Closed         -> no-op (idempotent; P-F3)
  post to the loop:
     winsize ws{rows, cols, 0, 0}
     ioctl(master, TIOCSWINSZ, &ws)   # kernel raises SIGWINCH in the fg group
     on EIO/ENOTTY/ESRCH -> tolerate (child gone / non-tty), never crash
```

- **Resize races are tolerated, not errors.** `TIOCSWINSZ` after child exit or
  on a closed master returns `EIO`/`ESRCH`/`ENOTTY`; the session transitions
  normally and the call is a no-op (`P-F3`). A `ResizeFailed` error is reserved
  for an unexpected failure on a live session.
- **The kernel delivers `SIGWINCH`** to the foreground process group; ymh does
  not signal the child directly for resizes.
- **Initial size is applied at spawn** (`PtyRequest::rows/cols`, §4.2) before
  the child execs, so the first paint is correct.

### 3.5 `terminate`

```text
terminate():
  if state is Exited/Closed -> return (idempotent)
  post to the loop:
     1. close(master)                  # delivers SIGHUP to the fg group
     2. signal the child's process group: kill(-pgid, SIGHUP)
     3. wait terminate_grace (ToolConfig, default 2 s)
     4. if still alive: kill(-pgid, SIGKILL)
     5. reap by specific pid (P6); state := Closed
```

- **Close-master-first.** Closing the master makes the slave read `EIO` and
  delivers `SIGHUP` to the controlling process group; the explicit
  `kill(-pgid, SIGHUP)` covers children that re-opened a tty or ignored the
  hangup.
- **Grace then SIGKILL.** The grace is `ToolConfig::terminate_grace` (07 §5.5,
  default 2 s), matching `ProcessService`'s teardown.
- **Reaping is specific-pid only** (E8). No global `waitpid(-1)` sweep, ever
  (`P6`, `P-F5`).
- **`terminate` never blocks the loop.** The grace wait is a timer on the loop;
  the SIGKILL and reap run in a loop-posted continuation.

### 3.6 Exit status and `wait()`

- The child's exit is observed by the pump: on `pidfd` readiness (Linux ≥ 5.3)
  or, as a fallback, by a dedicated per-session reap step (specific pid). The
  status is stored on the session and exposed by `wait()`.
- **`wait(timeout)`** returns the cached `PtyExit` once reaped; a `timeout` of 0
  waits indefinitely (bounded by the caller's cancellation token). A timeout
  expiry returns `PtyExit{exit_code = -1}` with `signalled = false` and does
  **not** close the session — the caller decides whether to `terminate`.
- **Natural exit keeps the ring.** A child that exits on its own leaves the
  master readable until drained; the pump drains to `EIO`/EOF, sets
  `eof = true`, and transitions to `Exited`. `read` still returns buffered
  output until the ring is empty (`P-F5`, mirrors 07 §9.3's drain-to-EOF rule).

### 3.7 Lifecycle state machine

```text
        open()                child confirmed
Starting ──────────────► Running ──────────────► Exited ──────────► Closed
   │  spawn failure          │  child exits          │  close/drain     ▲
   └──────────► (no session)  │  or terminate()       └──────────────────┘
                              └──────────────────────────────────────────►
                                     terminate() closes from Running/Exited
```

- **`Starting`** exists only between `open()`'s return and the pump's first
  successful `TIOCSCTTY`/pidfd registration; a tool never observes it for long,
  but `state()` is well-defined throughout.
- **`Exited`** retains the ring and the exit status; `read`/`wait` are valid.
- **`Closed`** is terminal: `find()` no longer resolves the id, the governor
  slot is released, and the pid has been reaped.
- **Slot release happens exactly at `Closed`**, not before (`P5`, §6.2).

---

## 4. `PtyService` and the factory (pinned)

### 4.1 Class shape

`PtyService::open` keeps 07 §6.5's exact shape. This spec adds the daemon-local
registry accessors the `terminal` tool needs.

```cpp
namespace ymh {

class PtyService {
public:
    virtual ~PtyService() = default;

    // 07 §6.5 (preserved).
    virtual Task<std::unique_ptr<PtySession>> open(const PtyRequest&,
                                                   CancellationToken) = 0;

    // ---- additive (this spec) ---------------------------------------------
    // Borrowed pointer, valid while the session is open. Session-scoped: a
    // session may only resolve its own ids (P15).
    virtual PtySession* find(PtySessionId) noexcept = 0;

    // Open sessions owned by one session, in id order (deterministic).
    virtual std::vector<PtySessionId> list(SessionId) const = 0;

    // Terminate + reap every PTY of one session. Idempotent, noexcept; called
    // on session end/close and on cancellation scoping (P10, F9).
    virtual void closeSession(SessionId) noexcept = 0;
};

} // namespace ymh
```

Rules:

- **`open` throws `PtyError{CapExceeded}` when `tryAcquirePty` fails** (`P-F6`);
  it never partially spawns.
- **`find` is session-agnostic at the signature level** but the *tool* must pass
  the current session's id; the implementation rejects a foreign id with
  `nullptr` (then the tool raises `PtyError{NotFound}`, `P-F9`). The
  `PtySessionId` alone is not a capability (decision (g)).
- **`list` is deterministic** (ascending `PtySessionId`) so golden/replay tests
  are stable.
- **`closeSession` is noexcept and idempotent**; it is the single cleanup entry
  point used by session end, daemon shutdown, and the cancellation scope
  (`P10`).

### 4.2 `PtyRequest` — spawn options

07 §6.5's `PtyRequest` is `{executable, argv, cwd, rows, cols}`. This spec adds
session, environment, and TERM **additively**.

```cpp
namespace ymh {

struct PtyRequest {
    // 07 §6.5 (preserved) ---------------------------------------------------
    std::string              executable;   // absolute or PATH-resolved
    std::vector<std::string> argv;         // argv-first; never a shell string
    std::filesystem::path    cwd;          // MUST be resolve()d under root()
    int                      rows = 24;
    int                      cols = 80;

    // additive (this spec) --------------------------------------------------
    SessionId                session;      // owning session (caps + routing)
    std::vector<std::pair<std::string, std::string>> environment;  // overlay
    std::string              term{"xterm-256color"};
};

} // namespace ymh
```

Rules:

- **`cwd` is a `resolve()`d path under `root()`.** `open` re-checks containment
  and throws `ToolError{PathEscape}` if `cwd` is not rooted; `getcwd()` is never
  a resolution base (`P1`, 07 X1/X2, `§18`).
- **`executable` is absolute or PATH-resolved by the service**, never a shell
  string. When the `terminal` tool is asked for a shell command, it passes
  `/bin/bash` + `{"-lc", <command>}` explicitly, exactly like `shell`
  (07 §6.4, `§16`).
- **`environment` is an overlay**, not the full environment: the child inherits
  the daemon environment minus the daemon's own secrets, plus these pairs.
  `TERM` is set from `term`; `PWD` is set to the resolved `cwd`.
- **`session` is mandatory** (no default); it is the cap and routing key.

### 4.3 Spawn sequence (pinned)

```text
LocalPtyService::open(request, cancel):
 1. validate: executable non-empty, argv non-empty, cwd non-empty
 2. resolve/verify cwd containment (P1)
 3. acquire the PTY slot: tryAcquirePty(request.session) -> else CapExceeded
      hold it in a PtySlot owned by the new PtySession (released at Closed)
 4. openpty(&master, &slave, nullptr, nullptr, &ws)   # ws from rows/cols
      failure -> release slot; throw PtyError{SpawnFailed}
 5. mark master O_NONBLOCK + O_CLOEXEC; ensure slave is NOT O_CLOEXEC until dup
 6. fork()
      child:
        setsid()                         # new session, no controlling terminal
        ioctl(slave, TIOCSCTTY, 0)       # explicitly claim the slave as ctty
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2)
        close master, close slave (if > 2)
        chdir(resolved_cwd)
        setenv TERM/PWD (overlay)
        close all fds > 2 not O_CLOEXEC   # 04 §3.2 fd hygiene
        execve(executable, argv, env)
          failure -> _exit(127)
      parent:
        close slave
        register master + pidfd on the daemon loop (PtyPump)
        state := Running
 7. return the PtySession
```

Rules:

- **The slave is opened with `O_NOCTTY` semantics** — more precisely, the daemon
  never acquires a controlling terminal because the child `setsid()`s and
  explicitly claims the slave with `TIOCSCTTY`; the parent closes the slave
  immediately. This is the `04 §3.2` H3 rule ("the daemon never opens a tty
  without `O_NOCTTY`") applied to the PTY: the daemon's only long-lived fd is
  the master, which is not a controlling terminal (`P4`).
- **The child is a process-group leader** (`setsid` makes it a session/group
  leader), so `kill(-pgid, …)` signals the whole terminal tree (07 §9.2, X13).
- **fd hygiene matches `04 §3.2`.** `O_CLOEXEC` on every daemon fd; the slave is
  duped to 0/1/2 then closed.
- **`execve` failure is not silent**: the child `_exit(127)`s; the pump observes
  the exit and the session reports `PtyExit{exit_code = 127}`. A `fork` failure
  is `SpawnFailed`.
- **No `forkpty` shortcut is required.** `openpty` + `fork` + explicit
  `setsid`/`TIOCSCTTY` is pinned because it makes the process-group and
  controlling-terminal behavior observable and testable; `forkpty` is an
  acceptable internal optimization only if it produces the same observable
  state (decision (e)).

### 4.4 `LocalPtyService` and `UnavailablePtyService`

```cpp
namespace ymh {

// The v1 realization. Owns every PtySession and the per-session registry.
class LocalPtyService final : public PtyService {
public:
    // `loop` is the daemon's single io_context adapter (Executor, §5.1);
    // `governor` is the one per-daemon governor (04 §8); `events` is the live
    // emission seam (§5.4).
    LocalPtyService(Executor& loop,
                    ResourceGovernor& governor,
                    PtyEventSink& events,
                    ToolConfig config = {});
    ~LocalPtyService() override;   // closes all sessions; no live child remains
    // ... PtyService overrides ...
};

// v1 default until the daemon injects a LocalPtyService (07 §6.5).
class UnavailablePtyService final : public PtyService {
public:
    Task<std::unique_ptr<PtySession>> open(const PtyRequest&,
                                           CancellationToken) override {
        throw ToolError{ToolErrorCode::Internal, "PTY is not available in v1"};
    }
    PtySession* find(PtySessionId) noexcept override { return nullptr; }
    std::vector<PtySessionId> list(SessionId) const override { return {}; }
    void closeSession(SessionId) noexcept override {}
};

} // namespace ymh
```

Rules:

- **`LocalPtyService` outlives every `PtySession` it owns** and is destroyed
  before `ResourceGovernor` and the `EventBus` (11 E18 destruction order). Its
  destructor calls `closeSession` for every session and asserts no live child.
- **`UnavailablePtyService` stays the v1 default** so the environment compiles
  and the `terminal` tool is simply absent from the registry while PTY is
  unbuilt (07 §4.3: Phase-2 tools are absent until the backing service exists).
- **The `terminal` tool is registered only when `pty()` is a real service.**
  Registration is decided by the daemon at startup (`pty().find` presence or a
  capability flag); the model is never offered a tool that cannot run.

### 4.5 Wiring the loop-aware service into the environment

The v1 `LocalEnvironment` constructs an internal `UnavailablePtyService`
(`include/ymh/execution/environment.hpp`). A loop-aware `LocalPtyService`
cannot be constructed by `LocalEnvironment` itself (it needs the daemon's
`io_context`, the governor, and the event sink), so this spec pins an
**additive** injection point:

```cpp
class LocalEnvironment final : public ExecutionEnvironment {
public:
    explicit LocalEnvironment(std::filesystem::path root,
                              SandboxMode mode = SandboxMode::Workspace,
                              ToolConfig config = {},
                              PtyService* pty = nullptr);   // additive
    // pty == nullptr  => internal UnavailablePtyService (v1 default)
    // pty != nullptr  => borrowed; the daemon owns the LocalPtyService and
    //                    must outlive the environment (11 E18)
};
```

Rules:

- **Borrowed, not owned.** The daemon constructs one `LocalPtyService` (with its
  `io_` adapter, governor, and event sink) and passes it to
  `LocalEnvironment`; `WorkspaceRuntime` must expose it so the environment can
  be built with it (an additive injection seam, the same philosophy as 11 §3.5's
  store injection, D9).
- **This is the only interface delta.** It is additive with a default; existing
  constructions compile unchanged. It is recorded here because 04/07's frozen
  text does not name it; a future errata pass (like 11) may restate it in 04's
  seam table. It does **not** change any 07 signature (`P16`).
- **`lsp()`'s `nullptr` pattern is the precedent** (07 §6.1): an absent
  capability is null/unavailable, never a fake that silently no-ops.

---

## 5. Async I/O model on the daemon loop

### 5.1 `Executor` — the marshalling seam

The daemon has one `asio::io_context` (04 §3.4) and a bounded `TurnExecutor`
for agent turns (11 §3.2, E6). Tools run on `TurnExecutor` workers; the PTY pump
must run on the loop so it never blocks a worker and never races the transport.
This spec pins a minimal `Executor` seam so the execution layer does not include
Asio headers directly.

```cpp
namespace ymh {

// Thin adapter over the daemon's single io_context. Owned by 04; consumed by
// this spec. `post` is thread-safe and never starts a thread (11 E1/E2).
class Executor {
public:
    virtual ~Executor() = default;

    virtual void post(std::function<void()>) = 0;

    // True iff the calling thread is the loop's runner thread. Used by asserts
    // and TSan tests (11 E2).
    virtual bool onLoopThread() const noexcept = 0;
};

} // namespace ymh
```

Rules:

- **`post` is the only cross-thread entry.** `PtySession::write`/`resize`/
  `terminate` called from a tool worker `post` a closure onto the loop; they
  never touch the master fd from the worker (`P11`).
- **The concrete adapter is 04's** (an `AsioExecutor` over `io_context`). This
  spec does not define it; it pins only the seam.
- **A `post` after loop stop is dropped** (returns without invoking). The PTY
  methods detect a stopped loop via the service and raise `PtyError{Internal}`
  rather than leaking a closure (mirrors 11's post-after-stop rule).

### 5.2 The `PtyPump`

One pump per `PtySession`, running entirely on the loop.

```text
PtyPump (loop-affine, one per session)
 ├── master read:  asio::posix::stream_descriptor.async_read_some
 ├── master write: asio::posix::stream_descriptor.async_write_some
 ├── child exit:   pidfd readiness (preferred) or a specific-pid reap step
 ├── resize:       ioctl(TIOCSWINSZ) posted from the tool worker
 └── terminate:    timer chain (SIGHUP -> grace -> SIGKILL -> reap)
```

- **Reads are bounded.** Each `async_read_some` reads at most 64 KiB; the pump
  sanitizes to UTF-8 (`sanitize_utf8`, 07 §5.2), appends to the bounded
  `PtyOutputRing`, pushes to the live `Stream<std::string>`, and emits a
  coalesced live `TerminalOutput` via `PtyEventSink` (§5.4).
- **Writes drain a bounded queue** (§3.2). The pump arms `async_write_some` only
  when the queue is non-empty and no write is in flight.
- **Child exit** is observed without a global SIGCHLD handler: on Linux ≥ 5.3
  the pump registers the child's `pidfd` with the loop and, on readiness, calls
  `waitpid(pid, WNOHANG)` (specific pid). On platforms without `pidfd`, the
  service uses a dedicated, bounded reap step that calls `waitpid(pid, …)` for
  that one child — still never `waitpid(-1)` (`P6`, 11 E8, M-F5).
- **The pump is the sole reader and sole writer of the master fd.** No other
  code path reads or writes it (`P11`).
- **The pump never blocks the loop.** No `read`/`write`/`waitpid` blocking call
  runs on the loop; `pidfd` readiness and async I/O are the mechanism.

### 5.3 Tool-thread interaction

A `terminal` tool call runs on a `TurnExecutor` worker (11 E6). It interacts
with the session through the thread-safe façade:

```text
tool worker                          daemon loop
  write(bytes)  ──post──────────────► enqueue + async_write
  resize(r,c)   ──post──────────────► ioctl(TIOCSWINSZ)
  read(max,wait)─┐                     pump appends to ring + notifies
                │  (blocks on a       │
                │   condition var /   │
                └── future) ◄─────────┘  notify on chunk/eof/timeout
  terminate()   ──post──────────────► timer chain -> reap
```

- **`read` blocks the worker, never the loop.** The pump notifies a per-session
  condition variable (or resolves a promise) when a chunk is appended, the
  child exits, or the wait timer fires; the worker wakes and drains the ring.
  This mirrors `ProcessService::run` blocking its worker (07 §6.4).
- **The wait is bounded and cancellable.** `read` observes the tool's
  `CancellationToken`; cancellation wakes the worker and returns
  `outcome = Cancelled` (F9).
- **No lock is held across `post`.** The façade takes the session mutex only to
  touch the queue/ring; the loop callback re-takes it briefly.

### 5.4 Live emission: `PtyEventSink`

```cpp
namespace ymh {

// Injected by the daemon at LocalPtyService construction. Live-only,
// best-effort, session-routed. The daemon wires it to the EventBus (§8.1) and
// the supervisor's TerminalLayer (10 §20.19). Never durable.
class PtyEventSink {
public:
    virtual ~PtyEventSink() = default;

    // Coalesced UTF-8 chunk for live `TerminalOutput` (00 §8.1). Called on the
    // loop. Best-effort: dropped if the session is closing (F3).
    virtual void onPtyOutput(SessionId, PtySessionId, std::string_view utf8) = 0;

    // Child exit, for the UI's terminal-closed indicator.
    virtual void onPtyExit(SessionId, PtySessionId, const PtyExit&) = 0;
};

} // namespace ymh
```

Rules:

- **Coalescing.** Live chunks are batched at
  `ToolConfig::output_flush_interval` (default 33 ms) or
  `ToolConfig::output_flush_bytes` (default 64 KiB), whichever first — the
  exact discipline 07 §8.2 pins for tool output.
- **Best-effort only.** A live chunk for a closing/closed session is dropped;
  the durable `ToolResult` is the authoritative record (`F3`, `P8`).
- **No durable append.** `PtyEventSink` publishes live events; the loop remains
  the sole appender of `payload::ToolResult` (X8).

---

## 6. Safety and policy

### 6.1 Path safety

- **`resolve()` only.** Every `PtyRequest::cwd` is produced by
  `ExecutionEnvironment::resolve()`; `open` re-verifies containment and throws
  `ToolError{PathEscape}` otherwise (07 §6.2, X1/X2, `§18`). `getcwd()` is never
  a resolution base in PTY or tool code (`P1`).
- **No implicit inheritance.** The child never inherits the daemon's cwd
  implicitly; `chdir` in the child uses the resolved path (`§18`).
- **The daemon `chdir()`s once at startup** (04 §3.3 step 6, `§54 D18`); PTY
  spawn does not chdir the daemon.
- **`ReadOnly` forbids PTY entirely.** A PTY spawns a process, so `ReadOnly`
  denies the `terminal` tool before execution (09 §3.5 hard deny; 07 §6.8).

### 6.2 Resource caps (F8, §9.11)

Caps are per host and enforced by the single `ResourceGovernor` (04 §8); this
spec never re-implements a cap.

```text
max_global_ptys     8     F8   (04 §2.1)
max_session_ptys    2     F8   (04 §2.1)
```

- **Acquire at `open`, release at `Closed`.** Unlike a subprocess slot (held for
  one call, 07 §7.2), a PTY slot is held for the **PTY lifetime** because the
  terminal is persistent across tool calls. The `PtySession` owns a `PtySlot`
  RAII guard; the guard releases on natural exit, `terminate`, `closeSession`,
  daemon shutdown, and every exception path (`P5`, X11).
- **Acquire before spawn.** `tryAcquirePty` runs before `openpty`/`fork`; a
  `false` returns `PtyError{CapExceeded}` with **no** child created (`P-F6`).
- **Exhaustion is durable.** The loop appends the `ToolResult` carrying
  `ResourceExhausted`, so a cap rejection is never a silent drop (04 §8, X11).
- **No double-counting.** `closeSession` releases each slot exactly once;
  `terminate` on an already-`Closed` session is a no-op and does not
  double-release.

### 6.3 Permission gating (`§19`, spec 09)

- **`terminal` is `ASK` by default**, like `shell` (07 §4.3, `§19`). It is a
  `destructive = true` tool (it spawns an arbitrary process).
- **The decision is made before execution.** The loop appends `ToolCall` →
  `evaluate` → `PermissionDecision` → `execute`; a `Deny` produces
  `ToolResult{Denied}` and `execute` is never called (07 X7, 06 §5.1). No PTY
  is spawned on a denied call (`P-F13`).
- **`ReadOnly` hard-denies `terminal`** regardless of any rule (09 §3.5;
  `ReadOnly` + mutating tool → `Deny`, not grantable).
- **The permission projection shows the command, never the output.** For an
  `open` action the projected `command` is the shell string (or the argv
  joined for display); PTY *output* is never part of the permission request and
  is never shown to a human approver (09 decision (p)).
- **Background sessions do not deadlock.** A background session's `terminal`
  `ASK` follows the same surface-via-attention / auto-deny-on-timeout path as
  every other `ASK` (09, F2); PTY adds no new permission path (`P12`).

### 6.4 Sandbox modes

| Mode | PTY policy |
|---|---|
| `Workspace` (v1 default) | PTY allowed subject to permission + caps; child in its own group; cwd rooted |
| `ReadOnly` | `terminal` denied at the policy layer; `pty().open` is unreachable |
| `Unrestricted` | PTY allowed; `resolve()` skips containment but the permission decision is still required (07 §6.8, X2) |

- **No OS jail in v1.** `SandboxMode` is root containment + permission + caps,
  not a seccomp/namespace jail (07 decision (f), `§52`). A PTY child runs with
  the daemon's uid.
- **fd/memory soft limits** (`setrlimit(RLIMIT_NOFILE/RLIMIT_AS)`, 07 §7.2) are
  applied to PTY children in Phase 2 alongside subprocess children.

### 6.5 Logging and redaction

- **Never dump PTY output to normal logs.** `host.log` (11 §12.2) records PTY
  metadata only: session id, `PtySessionId`, pid, bytes produced, truncation
  flag, exit status. The bytes live in the session event log (`TerminalOutput`
  live, `ToolResult` durable), which is the authoritative trace (04 §40,
  `§46`).
- **The `PtyEventSink` and `ToolResult` are the only byte sinks.** A
  `spdlog` call in the PTY path must not include `read.data` or a write buffer
  (`P13`).
- **Redaction applies to the projection, not the trace.** Provider/secret
  redaction (08 L12) governs what reaches the model prompt and logs; the
  session event log keeps the bounded, sanitized terminal bytes because the UI
  must render them. If an operator enables output redaction, it applies at the
  event-projection boundary, not inside the pump.
- **ANSI/control characters are preserved** in the ring and `ToolResult.output`
  (the TUI renders them); the *log* path must never interpolate them raw. The
  TUI's `TerminalLayer` (10 §20.19) owns render-time sanitization.

---

## 7. Lifecycle, reaping, and teardown

### 7.1 Ownership in the daemon tree (04 OQ-6)

```text
WorkspaceHost daemon (04 owns)  ── setsid, process group, SIG_DFL SIGCHLD
   │
   ├── Tool subprocess A (07 owns) ── own group (setpgid)
   ├── Tool subprocess B (07 owns)
   └── PTY session C     (14 owns) ── master fd + slave (child setsid + TIOCSCTTY)
          └── foreground group ── SIGHUP on master close / explicit kill
```

`04` OQ-6 splits ownership: 04 owns the daemon process tree; spec 07 owns the
tool subprocesses and PTYs. This spec is the PTY half of that split. It
introduces no new process-level owner.

### 7.2 Reaping — the `ReapGuard`/`SignalPolicy` interplay

- **No global SIGCHLD reaper.** `signal_policy.hpp` pins SIGCHLD at `SIG_DFL`
  (or a no-op `signal_set` that never calls `waitpid`); `SIG_IGN`/`SA_NOCLDWAIT`
  is forbidden (11 §3.3, E8). This spec **honors** that: it never installs a
  SIGCHLD handler and never sweeps the child table (`P6`).
- **One reaper per child.** The PTY child is reaped by the pump/service via a
  specific-pid `waitpid(pid, WNOHANG)` (on `pidfd` readiness) or a specific-pid
  blocking reap on a dedicated step. It is the only path that produces the
  `PtyExit` (E8).
- **`reap_guard.hpp` is unrelated.** The existing `reapClaimUnderLock` helper is
  a registry-claim TOCTOU guard (11 §12.5, D26), not a child reaper. PTY child
  reaping does not use it; the name overlap is explicitly disambiguated here to
  avoid a future "global reaper" regression (`P-F5`).
- **`SIGKILL` cannot be ignored**, so teardown always terminates; the reap then
  collects the status.

### 7.3 Teardown on session end and daemon shutdown

- **Session end/close:** the daemon calls `PtyService::closeSession(id)` for the
  ending session (01 §9.8 lifecycle). Every PTY of that session is terminated
  (SIGHUP → grace → SIGKILL) and reaped; slots are released. No PTY outlives its
  session (`P10`).
- **Daemon graceful shutdown (04 §3.5 step 4):** this spec supplies the
  mechanics — for each live PTY: close master → SIGHUP the group → wait
  `terminate_grace` → SIGKILL → reap. The bounded wait is `04`'s
  `shutdown_grace` (default 10 s). After `Stopped`, no PTY child remains (`P10`).
- **Cancellation scoping (F9):** cancelling a turn aborts the in-flight
  `terminal` action (a blocking `read`/`open`), but **does not** close a
  persistent PTY. The PTY lives until `close`, session end, or shutdown. This
  is deliberate: a turn cancel is not a terminal-close (decision (h)). A session
  delete/close is what calls `closeSession`.
- **Destruction order (11 E18):** `LocalPtyService` is destroyed before the
  `EventBus` and before `store.close()`, so no pump can emit after the bus is
  gone and no child outlives the store.

### 7.4 Crash and orphan behavior

- **Daemon crash:** PTY children reparent to `init`; closing the master (process
  death) delivers `SIGHUP` to their foreground groups. This spec does **not**
  attempt cross-process recovery; an orphan is `init`'s concern. No PID files,
  no "kill by `ps` evidence" (04 §7.3/§7.5, 07 §9.4).
- **Detach:** a supervisor detach does not touch PTYs; the daemon keeps its
  in-flight sessions and their terminals running (`§9.9`, D23).
- **`LocalPtyService` destructor** closes all sessions and asserts none remain
  live; a leak is a defect (`P10`).

### 7.5 Background output between tool calls

A persistent PTY keeps producing output while no tool call is active (§9.9's
"a workspace runs whenever it has pending work").

- **The pump keeps running** and appends to the bounded `PtyOutputRing`.
- **Live events continue best-effort** at the coalesced cadence, so an attached
  TUI sees activity without a `read` call.
- **The ring is bounded**; overflow evicts the oldest bytes and latches
  `truncated`, which the next `read` reports (`P-F4`, F5).
- **No durable growth.** Between calls nothing is appended to the session log;
  only the bounded ring and live events exist (`P8`).
- **No automatic PTY close on idleness** in v1. An idle terminal holds its cap
  slot until closed/session end; caps bound the cost (`max_session_ptys = 2`).
  Idle-reaping is a recorded open question (§14.2 OQ-P1).

---

## 8. Model-facing tools

### 8.1 Tool-surface decision

Two process tools exist, with **different capabilities and no overlap**:

| Tool | Backing | Shape | Use |
|---|---|---|---|
| `shell` | `ProcessService` (non-PTY) | one-shot, argv `/bin/bash -lc` | non-interactive commands; exit code + bounded output (07 §4.3, already MVP) |
| `terminal` | `PtyService` (PTY) | persistent, action state machine | interactive/REPL/curses/ssh, resize, incremental I/O |

**Decision (a).** Keep the one-shot `shell` non-PTY and unchanged; add a
persistent `terminal` tool. Rationale: PTY is a separate capability (`§17`);
one-shot `shell`'s timeout/exit-code semantics are wrong for an interactive
terminal; a persistent terminal needs an identity and a lifecycle, which a
single-shot tool cannot model. This also matches `§14.3`'s "avoid a separate
tool for every trivial operation" by using **one** `terminal` tool with an
`action` discriminator rather than six per-action tools.

**Decision (b).** `terminal` is one tool with
`action ∈ {open, write, read, resize, close, list}`. The pinned JSON Schema
subset (07 §3.2) has no `oneOf`/`anyOf`, so `required` is `["action"]` only and
the tool validates per-action requirements, returning `InvalidArguments` on a
mismatch. This is the same pattern the existing tools use for optional fields.

### 8.2 `terminal` tool schema

```text
terminal {
  action: "open" | "write" | "read" | "resize" | "close" | "list"   (required)

  # open
  command?: string            # run via /bin/bash -lc "<command>"
  argv?: array<string>        # argv form; mutually exclusive with `command`
  cwd?: string                # root-relative; resolve()d
  rows?: integer              # default 24
  cols?: integer              # default 80
  term?: string               # default "xterm-256color"

  # write / read / resize / close
  terminal_id?: string        # the id returned by open
  input?: string              # write: bytes to send
  append_newline?: boolean    # write: default true
  wait_ms?: integer           # read: bounded wait for output; default 0
  max_bytes?: integer         # read: cap on returned bytes; default 64 KiB
  signal?: "term" | "kill"    # close: default "term"
}
```

`additionalProperties: false`; all fields optional except `action`. Per-action
rules:

```text
open    requires exactly one of {command, argv}; returns {terminal_id, pid, rows, cols}
write   requires terminal_id + input; returns bytes queued (bounded status)
read    requires terminal_id; returns data (+ truncated/eof flags)
resize  requires terminal_id + rows + cols; returns the new geometry
close   requires terminal_id; returns the exit status
list    requires nothing; returns the session's open terminal ids
```

- **`command` vs `argv`.** `argv` is preferred when a program + args suffice
  (`§16`). `command` is explicit: `/bin/bash` + `{"-lc", command}` as separate
  argv elements, never concatenated (07 §6.4).
- **`cwd`** defaults to the workspace root; it is `resolve()`d (07 §6.2).
- **`read` never blocks indefinitely.** `wait_ms` is bounded by a tool constant
  (default max 30 s, §14.2 OQ-P2); `wait_ms = 0` drains the ring immediately.
  A wait that expires with no data is `Ok` with empty `data`.
- **`max_bytes`** is clamped to `tool_result_max_bytes` and the ring capacity;
  the returned `data` is a valid-UTF-8 tail snapshot.
- **`close`** terminates and reaps; `signal = "kill"` skips the SIGHUP grace.
- **`list`** returns only the calling session's ids (deterministic order).

### 8.3 Output streaming into the event log

```text
PTY pump ──► PtyOutputRing (bounded, tail)          [read path]
        └──► PtyEventSink ──► live TerminalOutput   [live path]
                                   │
tool returns ToolResult.output ◄── read() snapshot  [durable path]
                                   │
loop appends payload::ToolResult ──┘
```

- **Live:** `TerminalOutput` (00 §8.1) is emitted via `PtyEventSink` at the
  coalesced cadence; the supervisor projects it through `TerminalLayer` (10
  §20.19). Live events are best-effort and never durable (`P8`).
- **Durable:** each `terminal` action returns a bounded `ToolResult.output`
  snapshot (from `read`/`close`); the loop appends it. The tool never appends
  (X8).
- **Between calls:** live events continue; nothing durable accumulates (§7.5).
- **No new durable event type.** PTY output rides the existing live
  `TerminalOutput` and the existing durable `ToolResult`; this spec adds no
  `EventType` (decision (d)). Replay reconstructs terminal state from the
  bounded `ToolResult` snapshots, not from a per-chunk log.

### 8.4 Truncation, control characters, and redaction

- **Truncation.** The PTY ring keeps the tail (07 §5.2); `read` sets
  `truncated` when bytes were evicted. The durable snapshot is clamped on the
  **serialized** payload size below `tool_result_max_bytes` (X10, 07 §5.2); a
  `terminal` result can never trigger `PayloadTooLarge`.
- **UTF-8 boundary.** Raw PTY bytes are lossy-converted to valid UTF-8 before
  entering the ring or a live event (`sanitize_utf8`, 07 §5.2/§8.2); invalid
  sequences become U+FFFD and set the loss flag (`P14`, `P-F11`).
- **ANSI/control bytes are preserved** for rendering; the TUI sanitizes at
  render time (10 §20.19). The log path never interpolates them (§6.5).
- **Redaction.** The `terminal` tool's permission projection shows the command
  (or argv), never the output; PTY output never reaches `host.log` (§6.5,
  `P13`).

### 8.5 Permission defaults

```text
terminal   = "ask"     # all actions; destructive
ReadOnly   + terminal  -> Deny (hard, not grantable)
```

- A single `ASK` for all actions is the v1 default because the policy rule
  language keys on `tool`/`path`/`command`, not on the `action` field (09 §3.3,
  OQ-P3). `list`/`read` on an already-approved terminal still re-prompt; the
  operator can grant `Session`/`Always` once (09 `GrantScope`).
- **`open` projects its command** so a rule like
  `{tool = "terminal", command = "git push*", effect = "deny"}` works for the
  common case.
- Per-action permission granularity is deferred (OQ-P3); it would require
  either per-action tools or an action-aware `PermissionRequest`.

### 8.6 Interaction with `shell`

- `shell` remains the tool for non-interactive commands; `terminal` is for
  interactive use. They share no code except `resolve()` and the governor.
- A model that only needs a command's exit code uses `shell`; a model that needs
  to send input, resize, or read incrementally uses `terminal`.
- Both pass the same permission gate; both bound output the same way.

---

## 9. Concurrency and threading

- **One loop, one pump per PTY.** The pump is affine to the daemon's single
  `io_context` (04 §3.4, 11 §3.2); there is no thread per PTY beyond the pump's
  loop registration. The `pidfd` fallback's dedicated reap step is bounded by
  the PTY cap (`max_global_ptys = 8`).
- **Tools run on `TurnExecutor`.** A `terminal` action runs on a bounded worker
  (11 E6/E7); it blocks that worker for the duration of the call (like
  `ProcessService::run`). The loop stays responsive.
- **Per-session serialization.** In v1 the loop iterates a step's tool calls
  sequentially (07 §10), so at most one `terminal` action runs per session at a
  time; `find`/`read` are therefore single-consumer per session (`P15`).
  Parallel tool calls (Phase 2) must respect F8 and per-session ordering.
- **Cross-thread marshalling.** `write`/`resize`/`terminate` `post` to the loop;
  `read` waits on a condition variable/promise. No master-fd access off the loop
  (`P11`).
- **Cap counters** are loop-affine acquire/release through the governor's
  existing synchronization (04 §8); PTY adds no new global lock.
- **`PtyService` registry** is guarded by one service-level mutex; lookups are
  short and never hold the lock across a `post`.

---

## 10. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**P1 — Rooted spawn.** Every `PtyRequest::cwd` is produced by
`ExecutionEnvironment::resolve()` and re-verified for containment; `getcwd()` is
never a resolution base in PTY or tool code. (07 X1/X2, `§18`, F1)

**P2 — Capability seam.** No tool opens a PTY, forks a PTY child, or reaps one;
all PTY access goes through `ExecutionEnvironment::pty()` →
`PtyService`/`PtySession`. (`§17`, `§18`, `§4.4`)

**P3 — argv-first.** A PTY child is spawned from `executable` + `argv`; a shell
command is passed as one `/bin/bash -lc` argv element, never concatenated.
(07 §6.4, `§16`)

**P4 — No controlling terminal for the daemon.** The child `setsid()`s and
claims the slave with `TIOCSCTTY`; the daemon's only long-lived PTY fd is the
master, which is not a controlling terminal. (04 §3.2 H3, `§17`)

**P5 — Caps before spawn, held for the PTY lifetime.** `tryAcquirePty` runs
before `openpty`/`fork`; the `PtySlot` is held until `Closed` and released on
every path (exit, terminate, closeSession, shutdown, exception). (04 §8, 07 X11,
F8)

**P6 — One reaper per child.** The PTY child is reaped by a specific-pid
`waitpid`; there is no global SIGCHLD handler and no `waitpid(-1)` sweep. (11
E8, M-F5, `signal_policy.hpp`)

**P7 — Bounded output.** PTY output is bounded by `PtyOutputRing` (tail-keeping)
and the durable snapshot is clamped on the serialized payload size; no PTY path
allocates unbounded memory. (07 X10, F5)

**P8 — Live-only streaming.** PTY output is emitted as live `TerminalOutput`
events; the loop is the sole durable appender of `payload::ToolResult`. (00
§8.1, 07 X8)

**P9 — Idempotent terminate.** `terminate` on an `Exited`/`Closed` session is a
no-op; close-master → SIGHUP → grace → SIGKILL is the pinned order. (`§17`,
07 §9.3)

**P10 — No orphan after close.** Every PTY is terminated and reaped on session
end, `closeSession`, or daemon shutdown; after `Stopped` no PTY child remains.
(04 §3.5, 07 X13)

**P11 — Loop-affine fd ownership.** The pump is the sole reader/writer of the
master fd, on the daemon loop; cross-thread calls marshal via `Executor::post`.
(04 §3.4, 11 E2/E6)

**P12 — Permission before spawn.** A `terminal` call never spawns a PTY without
a recorded `PermissionDecision`; `ReadOnly` denies it. (07 X7, 09)

**P13 — No secret dump.** PTY bytes never reach `host.log`; logs carry metadata
only. (04 §40, `§46`, 08 L12)

**P14 — UTF-8 boundary.** Bytes entering the ring or a live event are
lossy-converted to valid UTF-8; the durable `ToolResult.output` is valid UTF-8.
(07 §5.2/§8.2, 01 S10)

**P15 — Session-scoped handles.** A `PtySessionId` resolves only within its
owning session; `find` is borrowed and valid for one serialized call. (07 §10)

**P16 — No signature churn.** `PtySession`'s four §17 methods and
`PtyService::open` are byte-identical to 07 §6.5; all additions are additive.
(07 §6.5, `§1.5`)

**P17 — Deterministic ids.** `PtySessionId` is monotonic per daemon; `list` is
ordered; golden/replay tests are stable. (07 X14)

---

## 11. Failure modes

### 11.1 Shared findings (F1–F12, `§54`)

| # | Finding | PTY consequence |
|---|---|---|
| F1 | path/process isolation | `cwd` via `resolve()`; child in its own group; daemon never chdirs for a PTY (P1) |
| F2 | background permission | a background session's `terminal` `ASK` uses the existing attention/auto-deny path; no deadlock (P12, §6.3) |
| F3 | late event after close | live `TerminalOutput` is best-effort and dropped for a closing session; the durable result is the record (P8) |
| F4 | edge-triggered attention | out of scope; PTY emits no attention state (spec 10) |
| F5 | output ring buffers | `PtyOutputRing` is bounded and tail-keeping (P7) |
| F6 | input/keybinding focus | out of scope (spec 10) |
| F7 | per-session dirty flags | out of scope; live coalescing is per-session (spec 10) |
| F8 | resource caps | global/per-session PTY caps; `PtySlot` held for the PTY lifetime (P5) |
| F9 | cancellation scoping | cancelling a turn aborts the in-flight action, not a persistent PTY; `closeSession` is session-scoped (§7.3) |
| F10 | resume-suspended | a daemon restart reopens suspended; PTYs do not survive a daemon restart (they are daemon-local, §7.4) |
| F11 | subagent ID duality | out of scope; a subagent's PTYs are its own session's (P15) |
| F12 | flash clock in model | out of scope (spec 10) |

### 11.2 Component-local failure modes (`P-F#`)

**P-F1 — Spawn failure.** `openpty`/`fork`/`execve` fails. Detection: syscall
error. Handling: release the slot; throw `PtyError{SpawnFailed}`; the registry
maps it to `Error` (E-F5 analogue). No session is returned.

**P-F2 — EIO on write/read.** The master reports `EIO` (slave gone). Detection:
`read`/`write` errno. Handling: drain, close, transition to `Exited`, reap;
subsequent writes throw `PtyError{Closed}`; the tool reports the child's exit
status. Never retried.

**P-F3 — Resize race.** `TIOCSWINSZ` on a just-exited child or a closed master.
Detection: `EIO`/`ESRCH`/`ENOTTY`. Handling: tolerate as a no-op; never crash;
`ResizeFailed` only on an unexpected live-session failure.

**P-F4 — Output backpressure.** The ring is full (tail eviction + `truncated`)
or the outbound queue is full (`WouldBlock`). Detection: capacity checks.
Handling: evict oldest + latch `truncated`; `write` raises `PtyError{WouldBlock}`
on a subsequent call; the tool returns a bounded error result; the session stays
`Running`.

**P-F5 — Child death.** The child exits naturally or is signalled. Detection:
`pidfd` readiness / specific-pid `waitpid`. Handling: drain to EOF, cache
`PtyExit`, set `eof`, transition to `Exited`, release the slot on `Closed`;
never a global sweep (E8, M-F5).

**P-F6 — Cap exceeded.** `tryAcquirePty` returns `false`. Detection: governor.
Handling: throw `PtyError{CapExceeded}` before any spawn; the loop appends a
`ToolResult` with `ResourceExhausted` (durable, X11).

**P-F7 — Daemon shutdown mid-PTY.** Shutdown begins while PTYs are live.
Detection: `Draining` state. Handling: close master → SIGHUP → bounded wait
(`shutdown_grace`) → SIGKILL → reap for each session; after `Stopped`, none
remain (P10). A session whose PTY does not die in grace is force-closed and the
daemon still completes steps 5–7 of 04 §3.5 (D-F16).

**P-F8 — fd leak / double close.** A master/slave fd is leaked or closed twice.
Detection: RAII fd wrappers; fd-count assertion in tests; `EBADF` on reuse.
Handling: RAII only; no raw `close` in the pump path; the destructor asserts no
live fd.

**P-F9 — Stale/foreign `terminal_id`.** The model passes an id that is unknown,
belongs to another session, or is `Closed`. Detection: `PtyService::find` →
`nullptr`. Handling: `PtyError{NotFound}` → `ToolResult{Error}`; no
cross-session access (P15).

**P-F10 — Write to an exited session.** `write` after `Exited`/`Closed`.
Detection: state check. Handling: throw `PtyError{Closed}`; the tool reports it;
never a silent drop.

**P-F11 — Invalid UTF-8 output.** The child emits non-UTF-8 bytes. Detection:
`sanitize_utf8` loss flag. Handling: lossy-convert to U+FFFD; set `truncated`
on the read result; the durable snapshot is valid UTF-8 (P14).

**P-F12 — Late live event after close.** The pump emits a live chunk for a
closing session. Detection: session state at emit. Handling: drop best-effort;
the durable result is authoritative (F3).

**P-F13 — Permission denied.** The loop records `Deny` before `execute`.
Detection: loop ordering. Handling: `ToolResult{Denied}`; `execute` never runs;
no PTY is spawned (P12).

**P-F14 — Concurrent read/write ordering.** Two `read`s race, or a `write`
races a `read`. Detection: per-session serialization in v1. Handling: the tool
is the only reader; `write`/`read` interleaving is defined by the pump's FIFO
ring; parallel tool calls (Phase 2) must respect per-session ordering (07 §10).

**P-F15 — Log injection.** ANSI/control bytes in PTY output reach `host.log`.
Detection: log-path audit. Handling: log metadata only; bytes go only to the
event log / TUI (P13).

**P-F16 — Slot leak on exception.** An exception between acquire and session
construction leaks a PTY slot. Detection: RAII guard; cap accounting test.
Handling: `PtySlot` releases on every path; `open` acquires before spawn and
transfers the guard to the session only on success (P5).

**P-F17 — Loop stopped mid-open.** `open` runs while the daemon is draining.
Detection: service state. Handling: throw `PtyError{Internal}` (or
`CapExceeded` during drain); no partial spawn; the slot is released.

### 11.3 Failure-mode coverage matrix

| Failure | Test layer |
|---|---|
| P-F1 | unit (fake openpty failure) + integration |
| P-F2 | integration (child closes slave) |
| P-F3 | integration (resize after exit) |
| P-F4 | unit (ring cap, queue cap) |
| P-F5 | integration (natural exit, drain-to-EOF) |
| P-F6 | unit (governor cap) |
| P-F7 | integration (shutdown mid-PTY) |
| P-F8 | unit (fd count) + ASan/LSan |
| P-F9 | unit (foreign/stale id) |
| P-F10 | unit (write after exit) |
| P-F11 | unit (non-UTF-8 fixture) |
| P-F12 | integration (close while emitting) |
| P-F13 | integration (policy deny) |
| P-F14 | TSan (concurrent read/write) |
| P-F15 | static/log audit |
| P-F16 | unit (throw between acquire/spawn) |
| P-F17 | integration (drain mid-open) |

---

## 12. dsh (DeepSeek Harness) mapping

DeepSeek Harness's capability-seam design keeps filesystem and subprocess
providers in one execution world so moving to a remote sandbox can move Bash,
PTY, and LSP together (`§4.4`, `§55`). This spec realizes the PTY half of that
seam.

| dsh concept | ymh PTY realization |
|---|---|
| execution world | `ExecutionEnvironment` (`§18`); `pty()` returns the PTY capability |
| capability seam | `PtyService`/`PtySession`; a remote/sandbox provider replaces `LocalPtyService` without tool changes |
| service for capabilities | `pty()->open(...)`; events for interception (permission via `§19`) |
| session/turn/step taxonomy | PTY output rides live `TerminalOutput` and durable `ToolResult`; no new event type |
| append-only session log | `ToolResult` snapshots are durable; live terminal chunks are not (P8) |
| plugin loop separation | the `terminal` tool is a `Tool`; the pump is a service, not the loop (X8) |
| provider-agnostic seams | PTY output is bytes; the UI renders via `TerminalLayer` (10) |

**Omitted deliberately** (matching `§55`): a dsh-compatible PTY plugin config,
hot reload of the PTY provider, and a browser terminal client.

---

## 13. Test plan

Strategy is `§44`: unit, integration (fake PTY / fake shell / `FakeLLM`),
golden, replay, TSan, and a separate live PTY/real-LLM layer. The deterministic
layers run offline against fakes; the live layer is opt-in and API-key gated
(`§44`, `§45`).

### 13.1 Unit tests

- **`PtyRequest` validation**
  - empty `executable`/`argv`/`cwd` → `ToolError{InvalidArguments}`;
    `cwd` outside root → `ToolError{PathEscape}` (P1).
  - `rows`/`cols` non-positive → `InvalidArguments`.
- **`PtyError` mapping**
  - each `PtyErrorCode` → `to_string`; `PtyError` → `ToolResult{Error}` via the
    registry path (E-F16 analogue).
- **`PtyOutputRing`**
  - under cap ⇒ no truncation; over cap ⇒ tail retained + `truncated`;
    `read` consumes; interleaved stdout semantics; `max_bytes` clamp.
  - non-UTF-8 fixture ⇒ valid UTF-8 + loss flag (P14, P-F11).
- **`Stream<T>`**
  - single consumer; resubscribe replaces; loop-affine handler ordering;
    `closed()` after `finish`; `buffered()` bound.
- **`PtySessionId` / `list`**
  - monotonic minting; `list` ordered; foreign id → `nullptr` (P-F9, P17).
- **Caps**
  - `tryAcquirePty`/`releasePty` global + per-session; `PtySlot` release on
    success/throw/terminate/close; no double-release (P5, P-F6, P-F16).
- **State machine**
  - `Starting→Running→Exited→Closed`; `write` after `Exited` → `Closed`;
    `terminate` idempotent (P9, P-F10).
- **`resize` validation**
  - non-positive → `InvalidArguments`; resize on `Closed` → no-op (P-F3).

### 13.2 Integration tests (fake PTY, hermetic)

- **Fake `PtyService`.** A scripted `FakePtySession` (no fork) injected via
  `LocalEnvironment`'s `pty` parameter; drive the `terminal` tool's action state
  machine end to end and assert `ToolResult` contents and the registry.
- **Fake PTY + `FakeLLM`.** A scripted turn: `terminal{open}` →
  `terminal{write}` → `terminal{read}` → `terminal{close}`; assert the live
  `TerminalOutput` batches and the durable `ToolResult` sequence, and that the
  loop is the only appender (X8, P8).
- **Permission gating.** `ReadOnly` → `terminal` denied, `execute` not called,
  no `open` (P-F13, P12); `ASK` auto-deny on timeout in a background session
  (F2).
- **Session-end cleanup.** Ending a session calls `closeSession`; every PTY is
  terminated/reaped; slots released (P10).
- **Cap exhaustion.** At `max_session_ptys`, the next `open` yields
  `ResourceExhausted`; a durable `ToolResult` is appended (P-F6, X11).
- **Cancellation scoping.** Cancelling session A's turn aborts A's in-flight
  `terminal` action and does not touch session B's PTY (F9, P10).
- **Shutdown mid-PTY.** Trigger `host.shutdown` with a live PTY; assert the
  child is terminated within `shutdown_grace` and none remain after `Stopped`
  (P-F7, P10).
- **Loop affinity.** Assert `Executor::onLoopThread()` inside the pump; assert a
  tool worker never touches the master fd (P11).

### 13.3 Real-process PTY tests (CI)

These run a real child under a real PTY; they are hermetic (no network) and run
in the default suite.

- **Basic I/O.** `/bin/sh -c 'echo hello; exit 3'` → `read` returns `hello`,
  `close` returns exit code 3.
- **`cat` round-trip.** `write("abc\n")` → `read` returns `abc`.
- **Geometry.** `stty size` reports the initial `rows`/`cols`; after
  `resize(40, 120)`, a `SIGWINCH`-aware child observes the new size.
- **Teardown.** `terminate()` closes the master, delivers SIGHUP, and reaps;
  `wait()` returns `signalled = true, signal = SIGHUP` when the child did not
  handle it.
- **No zombies.** After a batch of open/close cycles, `waitpid(-1, WNOHANG)`
  returns `ECHILD` (no children left) and the daemon's fd count is unchanged
  (P6, P-F8).
- **Natural exit drain.** A child that prints then exits leaves output readable
  until `eof` (P-F5).
- **Cap enforcement.** Opening `max_session_ptys + 1` fails cleanly (P-F6).
- **EIO.** Closing the slave from inside the child causes `read`/`write` to
  report `EIO`/`Closed` without crashing (P-F2).

### 13.4 TSan on the I/O path

- Build the execution/PTY targets with `-fsanitize=thread` and run a test that
  hammers `write`/`read`/`resize`/`close` from a worker while the pump runs on
  the loop; assert no data races and correct ordering (P11, P-F14).
- Assert no `post()` after loop stop and no handler invocation after the bus is
  destroyed (11 E1/E2, P-F12).

### 13.5 Golden tests (UI-visible)

- Given a recorded live `TerminalOutput` stream, render via spec 10's
  `TerminalLayer` and compare the terminal representation (00 §44 golden
  pattern). Assert ANSI escapes are interpreted, not emitted raw, and control
  bytes are sanitized at render time (10 §20.19).
- A truncation fixture renders the tail + a truncation marker.

### 13.6 Replay tests

- A recorded session containing `terminal` `ToolCall`/`ToolResult` pairs
  projects to the same UI state on replay; live-only `TerminalOutput` is not
  replayed (P8).

### 13.7 Live end-to-end tests (real LLM, PTY-driven, `§44`)

- Spawn the real `ymh` binary under a PTY (`forkpty`/`posix_openpt` or
  `tmux send-keys` + `capture-pane`), prompt a task that needs an interactive
  terminal, and assert observable behavior (terminal opened, output streamed,
  permission prompt handled). Opt-in (`YMH_LIVE_LLM=1` + API key), skipped when
  absent; the daemon is `setsid`'d and never reads the driver's PTY (04 §13.5).

### 13.8 Invariant coverage

| Invariant | Covered by |
|---|---|
| P1 | unit `resolve()`/escape table |
| P2 | static check: no `openpty`/`fork` outside `execution/pty` |
| P3 | unit argv-first + static check |
| P4 | integration `stty`/ctty test |
| P5 | unit cap acquire/release + integration exhaustion |
| P6 | integration no-zombies + static SIGCHLD audit |
| P7 | unit ring cap |
| P8 | integration no-append; static check |
| P9 | unit terminate idempotence |
| P10 | integration session-end + shutdown |
| P11 | TSan + loop-affinity assert |
| P12 | integration policy deny |
| P13 | log audit / static check |
| P14 | unit non-UTF-8 fixture |
| P15 | unit foreign-id; integration serialization |
| P16 | docs/interface diff review |
| P17 | unit id monotonicity + ordered `list` |

---

## 14. Decisions and open questions

### 14.1 Decisions (pinned by this spec)

- **(a) `shell` stays non-PTY; a persistent `terminal` tool is added.** The two
  have distinct capabilities and no overlap (§8.1); PTY is a separate capability
  (`§17`).
- **(b) `terminal` is one tool with an `action` discriminator**, not six
  per-action tools, matching `§14.3`'s small-surface rule; per-action
  requirements are validated in the tool because the pinned JSON Schema subset
  has no `oneOf` (§8.2).
- **(c) `Stream<T>` is pinned here** (bounded, loop-affine, single-consumer push
  stream) because PTY is the first consumer of 07 §6.5's forward declaration
  (§3.3). A future general async stream must satisfy this shape.
- **(d) No new durable event type.** PTY output rides live `TerminalOutput` and
  durable `ToolResult` snapshots; replay reconstructs from snapshots (§8.3).
- **(e) `openpty` + `fork` + `setsid` + `TIOCSCTTY` is the pinned spawn
  sequence**; `forkpty` is an internal optimization only if observably identical
  (§4.3, P4).
- **(f) The PTY pump is loop-affine and marshalled via an `Executor` seam**
  (04 §3.4, 11 E2/E6); tools block a `TurnExecutor` worker, never the loop
  (§5.1, P11).
- **(g) `PtySessionId` is a daemon-local handle, not a capability.** `find` is
  session-scoped; a foreign id is rejected (§2.1, P15).
- **(h) Cancelling a turn aborts the in-flight action but does not close a
  persistent PTY.** PTYs close on `close`, session end, or shutdown (§7.3).
- **(i) The PTY cap slot is held for the PTY lifetime**, not the tool call — a
  deliberate refinement of 07 §7.2 for persistent terminals (§6.2, P5).
- **(j) `terminal` is `ASK` as a whole in v1**; per-action permission is
  deferred (OQ-P3), and `ReadOnly` hard-denies it (§8.5, P12).
- **(k) Reaping is specific-pid only** (`pidfd` preferred, dedicated reap step
  as fallback); no SIGCHLD handler and no `waitpid(-1)` (11 E8, P6).
- **(l) Live PTY output is emitted through a `PtyEventSink` seam** wired by the
  daemon to the `EventBus`; the pump never appends durable events (§5.4, P8).
- **(m) PTY output bytes never reach `host.log`**; logs carry metadata only
  (§6.5, P13).
- **(n) `LocalEnvironment` gains an additive `PtyService*` injection** so the
  daemon can supply a loop-aware service; `nullptr` keeps
  `UnavailablePtyService` (§4.5). This is the only interface delta and it does
  not touch any 07 signature (P16).
- **(o) The `terminal` tool is registered only when a real `PtyService` is
  injected**; the model is never offered a PTY tool backed by
  `UnavailablePtyService` (§4.4, 07 §4.3).

### 14.2 Open questions

- **OQ-P1 — Idle PTY reaping.** Should an idle terminal (no read/write for N
  minutes) be auto-closed to free a cap slot, or is explicit `close` the only
  path? v1 keeps it open; a future policy could reap idle terminals with a
  live notice. (Leaning: explicit close only in v1.)
- **OQ-P2 — `read` wait ceiling.** The maximum `wait_ms` for a `terminal read`
  is a tool constant; the value (candidate 30 s) and whether the model may
  exceed it with a permission grant are unpinned.
- **OQ-P3 — Per-action permission.** Should `read`/`list` be `ALLOW` while
  `open`/`write`/`close` stay `ASK`? The current policy language keys on
  `tool`/`path`/`command`, not `action`; per-action tools or an action-aware
  `PermissionRequest` (09 §3.2) would be needed.
- **OQ-P4 — PTY over a remote `ExecutionEnvironment`.** `SSHEnvironment`
  (`§47` Mode B) must move PTY with FS/shell; the `PtySession` seam already
  allows it, but the `Executor`/loop ownership on a remote provider is unpinned.
- **OQ-P5 — `pidfd` fallback portability.** The dedicated reap step for
  platforms without `pidfd` (Linux < 5.3, non-Linux) needs a pinned thread
  budget; v1 targets Linux ≥ 5.3.
- **OQ-P6 — Durable terminal transcript.** Should a `terminal` session's full
  output be persisted (e.g. a per-terminal event or artifact) so a resumed
  session can scroll back? v1 keeps only bounded `ToolResult` snapshots
  (decision (d)).
- **OQ-P7 — Window-size propagation from the TUI.** The initial `rows`/`cols`
  come from the model or a default; a future refinement propagates the
  supervisor's actual terminal size when the PTY is opened for display.

---

## 15. References

- `docs/design/00-architecture.md` — §4.3 (services vs events), §4.4
  (capability boundary), §8.1 (`TerminalOutput` live event), §9.9 (background
  execution), §9.11 (resource caps), §14 (tool system), §16 (process/shell),
  §17 (PTY), §18 (execution environment), §19 (permissions), §40 (logging),
  §44 (testing), §45 (Fake LLM), §51 (Phase 2), §54 (F1–F12, D6/D18/D23), §55
  (dsh comparison).
- `docs/design/04-workspace-host-daemon.md` — §2.1 (`ResourceCaps`), §3.2
  (spawn/fd hygiene), §3.3 (startup), §3.4 (one `io_context`), §3.5 (graceful
  shutdown), §3.6 (signals), §4.1 (`HostConfig`), §8 (`ResourceGovernor`),
  OQ-6, decision (r).
- `docs/design/07-tools-execution.md` — §3 (`Tool`), §4.3 (initial tool set),
  §5 (`ToolContext`, `OutputSink`, `ToolConfig`), §6.1–§6.5
  (`ExecutionEnvironment`, `resolve()`, `ProcessService`, `PtyService`), §6.8
  (sandbox modes), §7 (caps), §8.2 (coalescing), §9 (subprocess/PTY ownership),
  §10 (concurrency), §11 (X1–X16), §12 (E-F#), §15 (decisions).
- `docs/design/09-permissions.md` — §3.3 (rule model), §3.1 (policy), §3.5
  (`ReadOnly` hard deny), decision (p) (argument projection).
- `docs/design/11-m2-errata.md` — §3.2 (`TurnExecutor`, E6/E7), §3.3 (child
  status, E8, M-F5), §11 (destruction order, E18), §12.2 (`host.log` redaction),
  §14 (E1–E21), §15 (M-F1–M-F12).
- `docs/design/12-m1-drift-errata.md` — M1 spec-text ↔ code reconciliation
  (the existing `PtyService` stub and `signal_policy.hpp`).
- `include/ymh/execution/services.hpp` — the forward-declared
  `PtySession`/`PtyService` seam this spec realizes.
- `include/ymh/execution/signal_policy.hpp` — the SIGCHLD policy PTY must honor.
- `include/ymh/execution/resource_governor.hpp` — `PtySlot` and the PTY caps.

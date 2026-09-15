# 04 — WorkspaceHost Daemon

**Component 04 of 10.** `WorkspaceHost` is the per-workspace daemon process: it
owns exactly one workspace and one cwd, hosts an in-process `SessionManager`
with N sessions, and exposes them to one or more supervisor TUI processes over
the §9.6 Unix-socket JSON-RPC 2.0 transport (framing owned by spec 05). It is
the Milestone-2 fork point of §57 Step 13 / §58: the runtime that Milestone 1
runs in-process is moved behind a `setsid` boundary so that it survives TUI
exit (§54 D23) and so that path/process isolation is structural (F1, §54 D18).

This document pins the daemon lifecycle (spawn/`setsid`/socket, startup order,
attach/detach, survive-TUI-exit, crash/orphan handling, graceful shutdown), the
`WorkspaceHost`/launcher interfaces, invariants (`H1`–`H21`), failure modes
(`F1`–`F12` plus local `D-F#`), the DeepSeek Harness (dsh) mapping, and the test
plan. It follows `00-architecture.md` (cited inline as `§n`), `01-session.md`
(`01 §n`), `02-persistence.md` (`02 §n`), and `03-workspace-registry.md`
(`03 §n`); where it cannot follow them it records the conflict under §14 rather
than choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** The invariants in this spec are numbered `H1`, `H2`, … (for
> "host"), so they cannot collide with the `D1`–`D23` design decisions in
> `00-architecture.md` §54. The two namespaces are disjoint: this spec always
> writes architecture decisions with the `§54` prefix (`§54 D18`) and daemon
> invariants bare (`H4`). See decision (o).

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   Supervisor TUI (spec 10)          CLI (ymh list / ymh show)      Live PTY test (§44)
        │  attach / detach / host.shutdown    │ read-only WAL             │ drives TUI
        ▼                                     │                          │
   ┌──────────────────────────────────────────────────────────────────────────┐
   │  WorkspaceHost daemon  (this spec)                                        │
   │    one workspace · one cwd · setsid · <workspace>/.ymh/host.sock          │
   │    ┌────────────────────────────────────────────────────────────────┐    │
   │    │ SessionManager (01 §8, §9.6)  N sessions                        │    │
   │    │ EventBus (SessionId-routed, §8.3) · ExecutionEnvironment (§18)  │    │
   │    │ ResourceGovernor (per-host caps, §9.11)                         │    │
   │    └────────────────────────────────────────────────────────────────┘    │
   └───────┬───────────────────────────────┬───────────────────────────────┬──┘
           │ claimHost / heartbeat /       │ open (writer) / close          │ JSON-RPC frames
           │ releaseHost  (spec 03)        │ (spec 02)                      │ (spec 05)
           ▼                               ▼                                ▼
   registry.db (§9.10)            <workspace>/.ymh/sessions.db       Unix-socket wire
                                  <workspace>/.ymh/sessions.lock     (§9.6, §43)
```

The daemon is the **agent runtime**: the supervisor never owns the agent
(§54 D1), the session event log is the durable truth (§54 D2), and the daemon is
the canonical writer for its own workspace's session-list rows (03 R16). The
daemon is headless: it must run with no terminal and no FTXUI (§4.1, §42).

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 04)

This spec pins:

- **Spawn** — `fork` + `setsid`, `umask`, stdio redirection, single `chdir()` to
  the workspace root, then socket bind (§9.7, §18).
- **Startup order** — chdir → mint boot nonce → open store → reconcile →
  `claimHost` → heartbeat (5 s) → serve.
- **Socket ownership** — v1 default `<workspace>/.ymh/host.sock`, bind/replace
  rules, unlink on shutdown (spec 05 owns the final convention; 03 §6.5, §15.1(l)).
- **Attach / detach** — multiple supervisors may attach; detach does **not**
  terminate the daemon; the daemon survives TUI exit (§54 D23, §9.8).
- **Crash / orphan handling** — sidecar `flock` + boot nonce liveness (02 §5.1,
  03 §6.3); never kill on `ps` evidence; reap only on lock-absence + boot-nonce
  mismatch under the §54 D22 write lock (§9.10, 03 R11).
- **Graceful shutdown** — SIGTERM/SIGINT → stop accepting → drain/close sessions
  → flush (02) → `releaseHost` (03) → unlink socket → exit.
- **Signal policy** — SIGHUP, SIGPIPE, SIGCHLD (§9.7, §40).
- The daemon hosts one in-process `SessionManager` with N sessions (§9.6) and
  owns its workspace's `ExecutionEnvironment` (§18).
- Per-host resource caps apply to the daemon and its sessions (§9.11, F8).

### 1.3 Boundaries — deferred to other specs

- **Transport and protocol (spec 05).** JSON-RPC 2.0 framing, the method catalog
  (`host.attach`, `host.detach`, `host.shutdown`, `event.stream`,
  `session.create`, …), `SessionEnvelope`, event multiplexing, per-session
  ordered delivery, the authenticated handshake, and the **final** socket-path
  convention. This spec treats them as a named contract and does not redefine
  them (§9.6, §43).
- **Registry (spec 03).** `WorkspaceRegistry`, `claimHost`/`heartbeat`/
  `releaseHost`, `probeWorkspaceLock`, the §54 D22 write lock, lazy claim clearing,
  and the process-scan fallback. This spec consumes them (§9.10).
- **Persistence (spec 02).** `SessionPersistence`, the sidecar `sessions.lock`
  flock, boot nonce, lease TTL/steal, flush/checkpoint, crash recovery. This
  spec drives them (§9.7).
- **Session model (spec 01).** `Session`, `SessionManager`, `SessionHeader`,
  create/resume/fork/replay, `EventBus` delivery. This spec constructs and owns
  the manager (§9.6).
- **Agent / tools / LLM / permissions (specs 06–09).** Agents, tool execution,
  `ExecutionEnvironment` internals, provider adapters, permission decisions.
  The daemon owns the *process* that hosts them, not their logic.
- **Supervisor/TUI (spec 10).** `WorkspaceModel`, `SessionUiState`,
  `DaemonStatus`, the tree switcher, focus, and the attach UX (§20.22).

### 1.4 Seam ownership relative to 01/02/03/05

| Concern | Owner | Daemon's role |
|---|---|---|
| Process/cwd isolation, `setsid`, signals | **04 (this)** | the daemon |
| Socket bind/unlink, spawn, attach/detach, orphan policy | **04 (this)** | the daemon + supervisor lifecycle |
| Host claim / heartbeat / release | 03 | daemon calls them |
| Sidecar flock + boot nonce | 02 (stamps) / **04** (mints) | daemon mints the nonce and passes `PersistenceConfig::boot_id`; 02 stamps it |
| Store open/close/flush | 02 | daemon calls `open`/`close` |
| `SessionManager` construction | 01 (shape), **04** (ownership) | daemon owns one instance |
| RPC framing, handshake, method catalog | 05 | daemon serves; supervisor connects |
| `chdir` | **04 (this)** | exactly once, at startup |
| Path resolution | §18 / spec 07 | never via `getcwd()` |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId`/`WorkspaceId` are frozen by 01 §2.1; `HostPid`/`HostBootId`/
`HostClaim` by 03 §2.1; `BootId` by 02 §2.1. They are reproduced for reference
and **not** redefined. Daemon-local types:

```cpp
namespace ymh {

// ---- daemon-local ---------------------------------------------------------

// Lifecycle state of a WorkspaceHost process (see §4.4).
enum class HostState : std::uint8_t {
    Starting,   // args validated; chdir done; not yet serving
    Serving,    // socket bound, claim written, heartbeat running
    Draining,   // shutdown requested; not accepting; closing sessions
    Stopped,    // clean exit
    Failed,     // startup/serve failure; exiting non-zero
};

// Process exit codes. Distinct so the launcher and tests can assert the cause.
enum class HostExitCode : std::uint8_t {
    Ok                = 0,
    WorkspaceBusy     = 10,   // another daemon holds sessions.lock (02 §5.1)
    WorkspaceMissing  = 11,   // workspace root absent / not a directory (01 S14)
    StoreOpenFailed   = 12,   // 02 StoreOpenError / SchemaVersionError
    RegistryFailed    = 13,   // 03 RegistryError (open, corrupt, schema, lock)
    SocketBindFailed  = 14,
    SocketPathTooLong = 15,   // sun_path limit (D-F6)
    StartupRejected   = 16,   // invalid args / unwritable log sink
};

// Why a shutdown was requested (for the audit log and the exit code path).
enum class ShutdownReason : std::uint8_t {
    ClientRequest,   // host.shutdown over the wire (spec 05)
    Signal,          // SIGTERM / SIGINT
    StartupFailure,  // startup step failed after partial state was created
};

// An attached supervisor. Spec 05 owns the authenticated handshake and the
// wire identity; the daemon only needs a stable handle for fan-out bookkeeping.
struct ClientId {
    std::uint64_t value;
    auto operator<=>(const ClientId&) const = default;
};

// Per-host resource caps (§9.11). The daemon owns one instance and enforces it;
// the supervisor adds only cross-host caps (spec 10).
struct ResourceCaps {
    std::size_t max_llm_concurrency{4};          // bounded LLM worker pool (§35)
    std::size_t max_global_subprocesses{32};     // F8
    std::size_t max_session_subprocesses{8};     // F8
    std::size_t max_global_ptys{8};              // F8
    std::size_t max_session_ptys{2};             // F8
    std::size_t session_output_ring_bytes{1u << 20};   // F5, §20.10
    std::size_t active_output_ring_bytes{4u << 20};    // F5
};

} // namespace ymh
```

`HostPid` is signed and `> 0`; "no host" is a SQL `NULL` / `std::nullopt`, never
`0` (03 R3). `HostBootId` is the daemon's boot nonce, minted once at startup and
injected into the store via `PersistenceConfig::boot_id` before `open()`; it is
shared by the store's lease rows and the registry claim (02 §4.1, §5.2, 03 §6.1,
H5, decision (n)).

### 2.2 Host error taxonomy

Every fallible daemon operation surfaces a code so callers and tests share
names. Startup failures map to `HostExitCode`; runtime failures surface over the
transport (spec 05) as JSON-RPC errors.

```cpp
namespace ymh {

enum class HostErrorCode : std::uint8_t {
    AlreadyRunning,     // this workspace already has a live daemon (flock held)
    WorkspaceMissing,   // canonical root absent / not a directory
    StoreUnavailable,   // 02 StoreOpenError / SchemaVersionError
    RegistryUnavailable,// 03 RegistryError
    SocketUnavailable,  // bind/listen failed
    SocketPathTooLong,  // canonical socket path exceeds sun_path
    LogSinkUnwritable,  // the stdio sink could not be opened
    NotServing,         // operation attempted before Serving or after Draining
    ShutdownInProgress, // operation attempted while Draining
    AttachRejected,     // handshake/auth failure (spec 05 owns the reason)
    HostUnreachable,    // a held lock but the socket refuses for the retry budget
};

class HostError final : public std::runtime_error {
public:
    HostError(HostErrorCode code, std::string message);
    HostErrorCode code() const noexcept;
};

} // namespace ymh
```

`AlreadyRunning` is the flock-loss path (02 §5.1). The daemon never degrades
silently: a startup failure is loud and non-zero (D-F3, D-F4).

---

## 3. Process model and daemon lifecycle

### 3.1 One workspace per process (§54 D18)

A `WorkspaceHost` owns exactly one workspace, so `chdir()` to that workspace at
daemon startup is safe: no other workspace shares the process (§9.7, §54 D18,
F1). Every session in the daemon is rooted at the same workspace cwd; cross-
project reads and writes cannot follow. There is exactly one `sessions.db` and
one `sessions.lock` per daemon (02 §1, 03 §3.5).

The daemon does **not** call libc `daemon(3)`: that helper `chdir("/")`s, which
would defeat the workspace root. The fork/`setsid` sequence is hand-rolled
(§3.2).

### 3.2 Spawn: `fork` + `setsid`

The supervisor (or a test launcher) starts a daemon by forking the `ymh` binary
and re-execing it in host mode. The pinned sequence, executed in the child
between `fork()` and `execve()`:

```text
fork()                                  # child is never a process-group leader
  child:
    setsid()                            # new session; no controlling terminal
    umask(0o077)                        # socket/state owner-only (H7)
    chdir is NOT done here             # the daemon chdirs exactly once (§3.3)
    open log_sink (O_WRONLY|O_CREAT|O_APPEND, 0600)
      on failure -> _exit(HostExitCode::StartupRejected)
    dup2(log_fd, 1); dup2(log_fd, 2)    # stdout/stderr -> log sink
    open /dev/null (O_RDONLY); dup2(devnull, 0)
    close all fds > 2 not marked O_CLOEXEC
    exe := readlink("/proc/self/exe")   # absolute; never resolved via PATH
      on failure -> _exit(HostExitCode::StartupRejected)
    execve(exe,
           [exe, "--host",
            "--workspace", <WorkspaceId>,
            "--root", <canonical workspace root>,
            "--socket", <socket path>],
           environ)
      on failure -> _exit(HostExitCode::StartupRejected)
  parent:
    record child pid; do NOT wait (the daemon outlives this supervisor)
```

Rules:

- **One fork, one `setsid`.** A single fork suffices because `setsid` detaches
  from the controlling terminal and starts a new session; a double fork is only
  needed to prevent re-acquiring a terminal, which cannot happen because the
  daemon never opens a tty without `O_NOCTTY` (H3; PTY slaves in spec 07 open
  with `O_NOCTTY`).
- **stdio is the log sink.** The daemon has no terminal; fd 0 is `/dev/null` and
  fds 1–2 are the log sink, so an early abort or a library `abort()` message is
  captured. spdlog (§40) writes to the same sink after startup.
- **`umask(0o077)`** makes the bound socket owner-only (`0777 & ~umask`); `.ymh/`
  itself is created `0700` by 02 §4.3.
- **The re-exec path is absolute.** The child resolves its own executable with
  `readlink("/proc/self/exe")` (or requires an absolute `argv[0]`) and passes
  that path as both `execve`'s file and `argv[0]`; it never re-execs the literal
  `"ymh"` through `PATH`, which could resolve to a different binary if `PATH`
  changed between spawn and exec.
- **The child does not `chdir`.** The daemon performs its single `chdir` in
  `run()` (H4) so the chdir is observable, testable, and paired with argument
  validation. Because `execve` preserves cwd, the child's inherited cwd is
  irrelevant.
- **The parent does not wait.** The daemon is not a child to be reaped; it is a
  peer that outlives the supervisor (H8, §54 D23). The supervisor reaps only if
  it spawned a daemon that exits during startup (the readiness handshake,
  spec 05).

### 3.3 Startup order (pinned)

```text
WorkspaceHost::run() -> HostExitCode
 1. validate args; canonicalize workspace_root; require an existing directory
      failure -> WorkspaceMissing (never create .ymh for a missing root, 02 §4.3)
 2. chdir(workspace_root)                     # ONCE; the only chdir in the daemon (H1, H2)
 3. bootId := mint HostBootId()               # UUIDv4, once (02 §5.2, 03 §6.1)
    persistence.boot_id := bootId             # supplied via PersistenceConfig (02 §4.1)
 4. store := SessionPersistence::open(persistence)
      acquires the sidecar flock LOCK_EX|LOCK_NB on <root>/.ymh/sessions.lock (02 §5.1)
      open() stamps the lock diagnostic {pid, boot_id, acquired_at} from
        persistence.boot_id (best-effort; 02 §4.3 step 7) -- the store never mints it
      failure EWOULDBLOCK -> WorkspaceBusy   (another daemon owns this workspace)
      failure otherwise   -> StoreOpenFailed
 5. registry := WorkspaceRegistry::open(registry_config)  (03 §4)
      failure -> RegistryFailed
    reconcile(): junction rows <-> sessions table, under the marker protocol
      (03 decision (i), 03 R9/R-F12); registry is the canonical writer (03 R16)
 6. build the in-process runtime:
      env      := LocalEnvironment(root = workspace_root)        (§18)
      bus      := EventBus()                                     (§8.3)
      governor := ResourceGovernor(caps)                         (§9.11)
      sessions := SessionManager(store, bus, env, governor)      (01 §8, §9.6)
      reopen all existing sessions SUSPENDED (Idle); none active (F10, §9.9; H17, decision (m))
 7. bind + listen on socket_path (after replacing a stale socket, §5.2)
      failure -> SocketBindFailed / SocketPathTooLong
 8. claimHost({workspace, getpid(), bootId, socket_path})       (03 §6.2, H5)
      failure -> RegistryFailed; close store (releases flock) and exit
 9. start the heartbeat timer (every 5 s, §9.10) and lease renewal (02 §5.5)
10. state := Serving; serve the Asio loop until a shutdown trigger (§3.4)
```

The order is load-bearing:

- **`chdir` precedes store open** so every relative path the store or a later
  tool might touch is already rooted at the workspace; the store's own paths are
  absolute and rooted at `workspace_root` regardless (02 §4.1, H2).
- **Store open precedes `claimHost`** because acquiring the sidecar flock is what
  makes the claim authoritative: a daemon-written claim must always name the
  current lock holder (03 R6, §6.3 rule 6).
- **Boot nonce precedes store open and lease acquisition** so the sidecar-lock
  diagnostic and every lease row carry the daemon's nonce (02 §4.3 step 7, §5.2,
  H5).
- **Reconcile precedes serve** so a reconnecting supervisor never observes a
  junction row whose session is absent from the store (03 R-F12).
- **Bind precedes `claimHost`** so the socket named in the claim is already
  connectable when the claim becomes visible; a supervisor that reads the claim
  and immediately connects never races a not-yet-bound socket. (The reverse order
  would publish a claim pointing at a dead socket.)

> **Ordering note (nonce vs. store open).** The daemon mints `HostBootId` after
> `chdir` and **before** `SessionPersistence::open()`, then injects it through
> `PersistenceConfig::boot_id`. This satisfies 02 §4.3 step 7 and §5.2 (the
> sidecar-lock diagnostic must carry the nonce when the store opens) and 03 §6.1
> (the registry's `host_boot_id` must equal the leases' `holder_boot_id`). The
> store never mints the nonce; it stamps the caller-supplied value. The authoring
> brief's "chdir → open DB → mint" order is superseded by the verified specs'
> requirement; the seam is `PersistenceConfig::boot_id` (decision (n)).

### 3.4 Serve

The daemon runs **one Asio `io_context`** (§35) with:

```text
accept loop            one Unix-domain acceptor (spec 05 frames each connection)
signal_set             SIGTERM, SIGINT, SIGHUP, SIGCHLD (§3.6)
heartbeat timer        every heartbeat_interval (default 5 s, §9.10)
lease renewal timer    every renew_interval (02 §5.5)
session timers         per-session chunk-flush/compaction (specs 02/06)
```

The daemon never blocks on a client: a slow or stuck supervisor is a bounded
outbound queue, not backpressure on the agent loop (H9, D-F9). It never blocks
on a non-active session (§54 D20 applies to the supervisor; the daemon applies
the analogous rule to its accept loop).

### 3.5 Graceful shutdown

On SIGTERM, SIGINT, or a `host.shutdown` request (spec 05):

```text
state := Draining
 1. stop accepting          close the acceptor; reject new attaches
 2. stop the heartbeat and lease-renewal timers
 3. drain/close sessions    for each session: cancel in-flight turn (§34),
                            flush any pending AssistantChunk batch (02 §6.2),
                            append SessionEnded ONLY for Delete, never for
                            detach/close (01 I16, §9.8)
 4. terminate children      signal the daemon's tool subprocess/PTY process
                            group; bounded wait (spec 07 owns the mechanics)
 5. store.close()           checkpoint WAL, release all leases, unlock the
                            sidecar flock (02 §4.3)
 6. releaseHost(workspace, bootId)   (03 §4.2; only if the claim still matches)
 7. unlink(socket_path)     remove the Unix socket file (H7)
 8. state := Stopped; return HostExitCode::Ok
```

- **Bounded by `shutdown_grace`** (default 10 s). If sessions do not quiesce in
  time, the daemon force-closes them and still performs steps 5–7 (D-F16). The
  flock and the claim are always released, because leaving them would block the
  next spawn; the socket is always unlinked.
- **`releaseHost` matches `(pid, boot_id)`** and is a no-op if the daemon was
  superseded (03 §4.4). A superseded daemon still unlinks its socket only if the
  socket path still resolves to its own inode (best-effort; the next spawn
  replaces a stale socket, §5.2).
- **The socket is unlinked last**, after `releaseHost`, so the claim and the
  socket disappear in an order that never publishes a live claim over a dead
  socket.

### 3.6 Signals

| Signal | Policy | Rationale |
|---|---|---|
| **SIGTERM** | graceful shutdown (§3.5) | the normal stop path (`host.shutdown` and `kill`) |
| **SIGINT** | graceful shutdown (§3.5) | the daemon has no controlling terminal, so SIGINT is only ever explicit; treat it like SIGTERM |
| **SIGHUP** | **ignored**, logged once | the daemon is `setsid`'d, so terminal hangup does not reach it; an explicit `kill -HUP` must not kill a live workspace. Hot reload is deferred (§55, §54 D11) |
| **SIGPIPE** | **ignored** process-wide (`SIG_IGN`), plus `MSG_NOSIGNAL` on socket writes | a detached client must not kill the daemon (D-F8) |
| **SIGCHLD** | **handled, not ignored**; reaped with `waitpid(WNOHANG)` from an async-signal-safe self-pipe | the daemon forks tool subprocesses and PTYs (spec 07) and needs their exit status. `SIG_IGN`/`SA_NOCLDWAIT` would auto-reap and destroy `waitpid`, so it is rejected |
| others | default | not the daemon's concern |

Signal handlers are installed via `asio::signal_set` (async-signal-safe,
integrated with the one loop). The SIGCHLD handler writes one byte to a self-
pipe; the loop drains it and reaps all exited children. No allocation and no
logging occur in the handler.

### 3.7 What the daemon hosts

The daemon is a thin process shell around the runtime; it owns no agent logic:

- **One in-process `SessionManager`** (§9.6, 01 §8) with N sessions. Each session
  owns its `SessionId`, a child `Context` (§6), an `AgentHandle` (§10.1, spec 06),
  and its policy (§19, spec 09). The `SessionManager` is one instance per daemon,
  never per session.
- **One workspace-rooted `ExecutionEnvironment`** (§18): the daemon constructs it
  once with `root() == workspace_root` and shares it across sessions, because one
  daemon owns one cwd (§54 D18). `resolve()` is the only path base; `getcwd()` is
  never a resolution base (H2, §9.7, §18).
- **One shared `EventBus`** with per-session ordered mailboxes (§8.3).
- **One `ResourceGovernor`** over `ResourceCaps` (§9.11, H16).
- **The `HostEventForwarder`** (§9.6) that serializes core `Event`s into
  `event.stream` notifications for attached supervisors (spec 05 frames them).
- **A single-active-session arbiter.** At most one session is active per
  workspace; the daemon serializes activation and starts the next session that
  has pending work once the active session finishes or blocks awaiting input.
  UI focus, attach, and detach never suspend or gate work; suspension happens
  only for explicit reasons (awaiting input, user action, resource caps).
  `session.activate` / `session.suspend` (spec 05 owns the RPC surface) are
  explicit controls, not a mirror of UI focus (§9.9, §34, decision (m)).

Agents, tools, providers, and permission policy are in-process but owned by
specs 06–09; the daemon only guarantees their process, cwd, caps, and lifecycle.

---

## 4. The `WorkspaceHost` interface (pinned)

### 4.1 Configuration

```cpp
namespace ymh {

struct HostConfig {
    WorkspaceId                workspace;        // UUIDv4, never the path (01 §2.1)
    std::filesystem::path      workspace_root;   // canonical realpath; chdir target
    std::filesystem::path      socket_path;      // v1 default <root>/.ymh/host.sock
    std::filesystem::path      log_sink;         // v1 default <root>/.ymh/host.log

    RegistryConfig             registry;         // 03 §4.1 (paths + bootstrap)
    PersistenceConfig          persistence;      // 02 §4.1 (db/lock + lease timing);
                                                 //   run() sets boot_id before open()

    ResourceCaps               caps;             // §9.11, H16
    std::chrono::milliseconds  heartbeat_interval{5'000};   // §9.10
    std::chrono::milliseconds  shutdown_grace{10'000};      // §3.5, D-F16

    // Test/debug: run in the caller's process (no fork, no setsid). The live
    // product always runs foreground == false. See H19 and §13.1.
    bool                       foreground{false};
};

} // namespace ymh
```

`workspace_root`, `socket_path`, and `log_sink` are computed by the **supervisor
launcher** (or the test) from the canonical workspace root; the daemon never
derives them from `getcwd()` (H2, 03 R16). `socket_path` defaults to
`<root>/.ymh/host.sock` and `log_sink` to `<root>/.ymh/host.log` as **v1
defaults, not protocol commitments** (03 §6.5, §15.1(l); OQ-3); `log_sink` is
**provisional** pending spec 10's final logging path (OQ-5). The daemon sets
`persistence.boot_id` in `run()` before calling `SessionPersistence::open()`
(§3.3 step 3, decision (n)).

### 4.2 Class shape

```cpp
namespace ymh {

class WorkspaceHost {
public:
    // Validates config and constructs the daemon object; does NOT chdir, open
    // the store, or bind. Throws HostError{WorkspaceMissing} on a bad root.
    static std::unique_ptr<WorkspaceHost> create(HostConfig);

    ~WorkspaceHost();

    // Blocking entry point. Runs the §3.3 startup sequence and then the Asio
    // loop until a shutdown trigger; returns the process exit code. Called by
    // the --host entry point and, with foreground == true, by tests.
    HostExitCode run();

    // Idempotent; safe to call from a signal handler bridge or a client request.
    void requestShutdown(ShutdownReason) noexcept;

    // ---- introspection (all noexcept) -------------------------------------
    HostState                    state() const noexcept;
    WorkspaceId                  workspace() const noexcept;
    HostBootId                   bootId() const noexcept;   // valid after §3.3 step 3
    HostPid                      pid() const noexcept;       // getpid()
    const std::filesystem::path& socketPath() const noexcept;

    // ---- owned runtime (valid only while Serving) -------------------------
    SessionManager&       sessions();
    WorkspaceRegistry&    registry();
    SessionPersistence&   store();
    ExecutionEnvironment& environment();
    ResourceGovernor&     caps();

    // Number of currently attached supervisors. Zero is a normal, long-lived
    // state: the daemon does not stop when the last client detaches (H8, H9).
    std::size_t attachedClients() const noexcept;

    // Single-active-session control (decision (m)). The daemon owns the
    // invariant that at most one session is active per workspace and activates
    // the next session with pending work when the active one finishes or blocks
    // awaiting input. Spec 05 frames activateSession/suspendSession as explicit
    // session.activate / session.suspend controls; they are not a mirror of UI
    // focus, and attach/detach never calls them (§9.9, §34).
    void                     activateSession(SessionId);
    void                     suspendSession(SessionId) noexcept;
    std::optional<SessionId> activeSession() const noexcept;

    // Spec 05 calls these as frames arrive; the daemon owns the bookkeeping.
    ClientId onClientAttached(ClientId);      // returns the assigned id
    void     onClientDetached(ClientId) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ymh
```

`create()` performs no side effects beyond validation so a supervisor can
construct a host object for introspection without starting it. `run()` is the
only entry point that mutates process state (chdir, flock, socket). This split is
what lets the deterministic suite drive the exact startup order in-process
(H19, §13.1).

### 4.3 In-process run entry point (test seam)

`foreground == true` skips the fork/`setsid`/exec wrapper: the caller's process
becomes the daemon. Everything else (chdir, flock, claim, heartbeat, socket,
shutdown) is identical. This is the path the integration suite uses; the live PTY
layer starts the **real** binary via the launcher (§44, §13.5). The two paths must
not diverge: `run()` contains no `#ifdef TEST`.

### 4.4 State machine

```text
        create()
           │
           ▼
      ┌─────────┐  startup step fails   ┌────────┐
      │ Starting│ ────────────────────► │ Failed │ → exit non-zero
      └────┬────┘                       └────────┘
           │ all of §3.3 steps 1–8 ok
           ▼
      ┌─────────┐  shutdown trigger      ┌─────────┐  steps 1–8 of §3.5
      │ Serving │ ─────────────────────► │ Draining│ ──────────────────► Stopped → exit 0
      └─────────┘                        └─────────┘
           ▲                                  │
           └──────── (no path back) ──────────┘
```

Transitions are one-way. `Serving` is the only state in which RPC requests are
accepted; `Draining` rejects new attaches and new session work. `Failed` is
terminal and always exits non-zero (D-F3, D-F4, D-F5).

---

## 5. Socket bind and ownership

### 5.1 v1 default path

The v1 default is:

```text
<workspace>/.ymh/host.sock
```

This is the **same v1 default** the registry's process-scan fallback derives
independently of `argv` (03 §6.5, §15.1(l)). Spec 05 owns the final convention;
if it changes, this default and 03's derivation change together (OQ-3). The path
is also recorded in `workspaces.host_socket` (03 §3.3).

### 5.2 Bind / replace rules

```text
bind(socket_path):
  if path length (bytes) > sizeof(sockaddr_un::sun_path) - 1:
      -> HostError{SocketPathTooLong}            # never truncate (D-F6)
  if a file exists at socket_path:
      attempt connect(socket_path, SOCK_STREAM, non-blocking)
        connect succeeds  -> another daemon is live
                             -> HostError{AlreadyRunning}   # race lost (D-F1)
        connect ECONNREFUSED -> stale socket from a crashed daemon
                             -> unlink(socket_path)         # we hold the flock, so we are the owner
        other errno          -> HostError{SocketUnavailable}
  socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0)
  bind(); listen(backlog)
  chmod(socket_path, 0600)                        # owner-only (H7)
```

The sidecar flock (02 §5.1) is the **authority** for "another daemon is live":
a daemon that holds the flock owns the workspace and may replace any socket file.
The connect-probe is defense-in-depth against a socket file left by a process
that never held the flock (e.g. a crashed launcher). `AlreadyRunning` here is a
rare belt-and-suspenders case; the common race is lost at flock acquisition
(D-F1).

### 5.3 Unlink on shutdown

`Serving → Draining` unlinks `socket_path` after `releaseHost` (§3.5 step 7). A
crash cannot unlink; the next spawn's connect-probe handles the stale file
(§5.2, D-F7). The daemon never unlinks a socket it does not own: if a
superseding daemon has already rebound the path, the unlink is skipped when the
inode differs (best-effort, `lstat` + inode compare).

### 5.4 Path length

`sun_path` is ~108 bytes on Linux. The daemon checks the canonical socket path
length **before** `bind` and fails loud with `SocketPathTooLong` (D-F6); it never
truncates a path, because truncation would silently bind a different socket and
split the host from its claim. A future abstract-namespace or shortened-hash
scheme is deferred (OQ-3).

---

## 6. Spawn / attach / detach (supervisor side)

The daemon owns its lifecycle; the supervisor owns **discovery + attach**. The
interfaces below live in the supervisor but are pinned here because they define
the daemon's observable contract.

### 6.1 The launcher seam

```cpp
namespace ymh {

// Spawns and signals daemon processes. Real implementation: fork + setsid +
// execve (§3.2). Tests inject a fake launcher or run the host in-process.
class HostLauncher {
public:
    virtual ~HostLauncher() = default;

    struct SpawnResult {
        HostPid                pid;          // child pid (> 0)
        HostBootId             bootId;       // provisional until host.hello (spec 05)
        std::filesystem::path  socketPath;   // the path the daemon will bind
    };

    // Fork+setsid+exec. Returns as soon as the child is exec'd; readiness is
    // established by connecting and completing the spec 05 handshake.
    virtual SpawnResult spawn(const HostConfig&) = 0;

    // SIGTERM. Used for an explicit supervisor-driven stop, never for orphan
    // reaping (H11).
    virtual void requestStop(HostPid, ShutdownReason) = 0;

    // True iff kill(pid, 0) does not report ESRCH. A hint only (03 §6.3).
    virtual bool isAlive(HostPid) const = 0;
};

class ForkExecLauncher final : public HostLauncher { /* ... */ };

} // namespace ymh
```

`ForkExecLauncher::spawn` implements §3.2. It never waits on the child and never
reaps it except during the startup handshake window.

### 6.2 `ensureRunning`

```cpp
namespace ymh {

// Spec 05 owns HostConnection. Forward-declared here to avoid a cycle.
class HostConnection;

struct AttachResult {
    std::unique_ptr<HostConnection> connection;
    bool                            spawned;   // true iff this call spawned it
};

// Outcome of a lazy stale-claim clear (§7.3). Never the outcome of a signal.
enum class ReapResult : std::uint8_t {
    Live,     // the sidecar lock is held; the workspace is live, nothing changed
    Reaped,   // the claim was stale and was cleared under the §54 D22 write lock
    Absent,   // no claim was recorded; nothing to do
};

class HostLifecycle {
public:
    HostLifecycle(HostLauncher&, WorkspaceRegistry&);

    // Attach to a live daemon or spawn one. NEVER kills a process. May clear a
    // stale claim (lazy reap) under the §54 D22 write lock (03 §6.4, H10, H11).
    AttachResult ensureRunning(WorkspaceId);

    // Detach this supervisor. Does NOT terminate the daemon (H8, H9).
    void detach(WorkspaceId, ClientId);

    // Explicit graceful stop (host.shutdown, spec 05). Distinct from detach.
    void requestGracefulStop(WorkspaceId);

private:
    // Clears a claim only when lock-absence AND boot-nonce mismatch hold,
    // under the §54 D22 registry write lock (03 R5/R11).
    ReapResult reapIfStale(const WorkspaceRecord&);
};

} // namespace ymh
```

`ensureRunning` algorithm:

```text
row := registry.findById(ws)                 # 03 read (WAL, no lock)
if row has no host claim:
    spawn (§6.1) -> wait for socket + host.hello (spec 05) -> attach
else:
    lock := probeWorkspaceLock(row.canonical_path)      # 03 §6.3
    if lock == HeldBy:
        try attach(row.host_socket)
          success        -> attach
          ECONNREFUSED   -> retry within a bounded budget, then surface
                            HostError{HostUnreachable} and DO NOT spawn a
                            second daemon (a held lock means the workspace is
                            live; spawning would only lose the flock race) (H10)
    if lock == Absent:
        reapIfStale(row)                      # lazy clear under §54 D22 lock
        spawn -> attach
```

Two supervisors may race the spawn; the sidecar flock resolves it: one daemon
wins `LOCK_EX`, the loser exits `HostExitCode::WorkspaceBusy`, and the losing
supervisor re-enters `ensureRunning`, sees the winner's claim, and attaches
(D-F1). No process is killed.

### 6.3 Attach

- A supervisor connects to `host_socket` and completes the authenticated
  handshake (spec 05). The daemon assigns a `ClientId` and registers it.
- Multiple supervisors may attach to one daemon simultaneously (§9.6). Each
  receives the events it subscribes to; the daemon fans out per-session in
  publish order (§8.3).
- Attach is **idempotent per client**: a reconnect with the same authenticated
  identity supersedes the old stream (spec 05 owns the identity; the daemon
  replaces the stale `ClientId`).
- Attach replays nothing implicitly; the supervisor asks for the current state /
  a cursor (spec 05 owns `event.stream` cursors). This keeps the daemon from
  buffering unbounded history per client (F5, D-F9).

### 6.4 Detach

- `host.detach` (spec 05) or a dropped connection removes the `ClientId`.
- Detach **never** emits `SessionEnded` and **never** terminates the daemon
  (§9.8, 01 I16, H9). Detach is "this TUI stops observing", not "the session
  stops" (§54 D23, §9.8).
- On detach, per-session mailboxes that were feeding the client drain; a
  detached session's in-flight turn keeps running headless (§9.9, §54 D23). Late
  events are retained by the supervisor-side tombstone until the mailbox drains
  and the handle is terminal (F3, §9.8); the daemon's job is only to keep
  appending truthfully.

### 6.5 Survive TUI exit (§54 D23)

The daemon is `setsid`'d and holds no reference to the supervisor's terminal.
When the TUI exits (cleanly or by crash) its socket closes; the daemon observes a
client detach and continues. In-flight sessions keep running and keep appending.
This is the whole point of the fork (§57 Step 13, §58, §9.9). The daemon exits
**only** on `host.shutdown`, SIGTERM, or SIGINT (§3.5); zero attached clients is
not a trigger.

### 6.6 Multiple supervisors

- N supervisors × one daemon: all attach to the same socket; the daemon fans out.
- One supervisor × N daemons: the supervisor tracks a `WorkspaceModel` per
  workspace with a `DaemonStatus` of Attached/Connecting/Detached/Dead (§20.22).
- Focus is supervisor-local **display state only** (§9.6, §9.10, 03 R12). The
  daemon serializes activation and keeps exactly one session active per
  workspace; UI focus, attach, and detach never suspend, cancel, or gate work.
  The daemon activates the next session with pending work when the active one
  finishes or blocks awaiting input (§9.9, decision (m)).

---

## 7. Crash and orphan handling

### 7.1 Daemon crash

A crash (`SIGKILL`, `SIGSEGV`, `abort`) skips all of §3.5. The consequences are
all self-healing:

| State | After a crash | Recovered by |
|---|---|---|
| sidecar `sessions.lock` | kernel releases the `flock` automatically (§9.7, 02 §5.1) | next daemon's `open()` |
| `sessions.db` WAL | SQLite replays the WAL on the next open (02 §7.2) | next daemon's `open()` |
| `session_leases` rows | stale `(pid, boot_id)` rows remain | next daemon acquires the flock, then touches/steals each session lazily (02 §5.3, §7.5) |
| registry host claim | stale `host_pid/boot_id/socket/heartbeat` remains | lazy clear on next supervisor access (03 §6.4, D-F13) |
| socket file | stale inode remains | next spawn's connect-probe (§5.2, D-F7) |
| tool children | reparented to `init` | OS; spec 07 owns their cleanup (OQ-6) |

The daemon never writes a clean-shutdown marker; WAL atomicity plus the kernel
flock are the recovery contract (02 §13.1(f), 03 §10).

### 7.2 Liveness: lock-primary, boot-nonce cross-check

Liveness is established exactly as §9.7/§9.10 and 03 §6.3 specify:

```text
probeWorkspaceLock(canonical_path) -> {Absent, HeldBy(pid, boot_id)}
killHint(pid) -> Alive | Dead | Unknown        # kill(pid,0); ESRCH => Dead only
```

Rules (H10, H11):

1. **A held sidecar flock always means the workspace is live.** The daemon holds
   it for its whole lifetime (02 §5.1); the kernel releases it only on process
   death.
2. **`kill(pid, 0)` and heartbeat staleness are hints, never tests.** Only
   `ESRCH` proves death; `EPERM` means alive-but-not-ours; a `SIGSTOP`ped daemon
   returns `0` and still holds the flock (D-F15).
3. **A stale claim is cleared only when the lock is absent AND the recorded
   `host_boot_id` no longer matches a live holder's nonce** (03 R5, D-F13).
4. **A held lock with a mismatched diagnostic is still live.** The diagnostic is
   best-effort (02 §5.1); the row is not re-claimed from it. The holder's own
   `claimHost`/heartbeat updates it (03 §6.3 rule 5, D-F5).

### 7.3 Orphan reaping (§9.10, pinned)

"Reaping an orphan" in ymh means **clearing a stale host registration**, not
sending a signal. Pinned verbatim to §9.10 and 03 §6.5:

```text
reapIfStale(row):
  if row has no claim:            return Absent
  if probeWorkspaceLock(row.canonical_path) == HeldBy:
      return Live                              # never touch a live workspace
  # lock is Absent -> no daemon holds this workspace; the claim is stale
  if row.host_boot_id still matches a live holder's nonce:
      return Live                              # defensive; cannot happen with Absent lock
  registry.reapHost(row.workspace, row.host_boot_id)   # 03 §6.4; takes the
                                                       # §54 D22 write lock internally
  return Reaped
```

`reapHost` is 03's lazy-clear path (03 §6.4); whether 03 exposes it as its own
method or folds it into `releaseHost` (03 §4.2) is 03's surface, not this
spec's. It is a registry mutation and therefore acquires `flock(LOCK_EX)` on
`registry.lock` for its duration (03 §5.1, R2). This spec does not redefine it —
03 owns the method; 04 only pins the **conditions** under which it may be
called.

Hard rules (H11, 03 R11):

- A process scan is **never** the sole liveness test and **never** authorizes a
  kill. `ps`/procfs output is self-reported, spoofable, and racy (03 §6.5,
  §9.10).
- The daemon and the supervisor **never kill a daemon on `ps` evidence**. If a
  live process is found that holds no lock, it is mid-start or confused: do not
  signal it, do not clear its claim while its lock is held (03 R6, D-F14).
- A **user-requested** stop is `host.shutdown` over the authenticated handshake
  (spec 05) or `HostLauncher::requestStop` (SIGTERM) to a **confirmed** pid; it
  is never driven by a scan.
- The reaper's mutation takes the §54 D22 write lock before it writes (03 R11, §6.4).

### 7.4 Process-scan fallback boundary

The daemon does not scan. The supervisor's process-scan fallback (03 §6.5) may
enumerate candidate `ymh --host` pids only when the registry is missing, corrupt,
or pre-`initialized`; it derives the socket/flock path independently and confirms
by handshake or flock probe. The daemon's only obligation is that its `argv[0]`
basename is `ymh` (it is the absolute re-exec path, §3.2) and its `--host
--workspace <uuid> --socket <path>` triple is present, so a scan can seed
candidates. The daemon never trusts or acts on scan output (H11).

### 7.5 Orphaned sockets, sessions, and children

- **Socket** — handled at bind (§5.2). A stale socket is unlinked only after a
  connect-probe fails, and only by a daemon that holds the flock.
- **Junction session with no store row** — reconciled at startup (§3.3 step 5)
  under the `pending_mutation` protocol (03 decision (i), 03 R-F12). A dangling
  row is removed; it is never reported as open.
- **Store session absent from the junction** — the daemon adds the junction row
  as the canonical writer (03 R16).
- **Active session after a restart** — none. Every session reopens suspended
  (§3.3 step 6) purely to avoid an auto-resume LLM burst (F10); the daemon then
  activates a session that has pending work, if any (decision (m)).
- **Tool children after a crash** — reparented to `init`; the daemon does not
  adopt them. On graceful shutdown it terminates its process group (§3.5 step 4).
  Ownership is split so both specs do not claim the same children: **04 owns the
  daemon's process tree** (the daemon and its process group); **spec 07 owns the
  tool subprocesses and PTYs** (OQ-6).

---

## 8. Resource caps and scheduling

Caps are **per host**, because each daemon owns its own LLM pool, subprocesses,
PTYs, and buffers (§9.11). The daemon instantiates exactly one
`ResourceGovernor` from `HostConfig::caps` and passes it to the session/agent
layer:

```cpp
namespace ymh {

class ResourceGovernor {          // one per WorkspaceHost (§9.11, F8, H16)
public:
    // Bounded worker pool for LLM calls (§35); never a thread per call.
    LLMPool& llm();

    // Global semaphores (daemon-wide) and per-session counters (F8).
    bool tryAcquireSubprocess(SessionId);
    void releaseSubprocess(SessionId);
    bool tryAcquirePty(SessionId);
    void releasePty(SessionId);

    // Per-session output ring buffers (F5, §20.10): full output materialized
    // only for the active/expanded session; others keep a bounded ring.
    OutputRing& ringFor(SessionId);
};

} // namespace ymh
```

`LLMPool` is owned by spec 06 (`06 §5.9`) and `OutputRing` by spec 07 (§5.2); 02 owns only `AssistantChunk` coalescing. This spec owns only the **one governor per daemon** and the `ResourceCaps` values. The governor is the single place a per-host cap is
enforced, so no tool or provider re-implements a cap.

Rules:

- **F8** — bound tool subprocesses and PTYs, not just LLM calls: global
  subprocess cap, per-session subprocess cap, global PTY cap, per-session PTY
  cap (§9.11). A bounded LLM cap does **not** bound tool subprocesses.
- **F5** — per-session output ring buffers; full tool output is materialized only
  for the active session and expanded subagents (§9.11, §20.10).
- The supervisor adds only cross-host caps (`maxAttachedHosts`,
  `maxTotalSessions`, TUI memory); it never enforces LLM/subprocess/PTY caps
  (§9.11, spec 10).
- The daemon exposes its caps so the supervisor can refuse to spawn an unbounded
  number of daemons; the actual system-wide bound is "cap the number of live
  daemons in the registry" (§9.11, 03 F8 row).
- Caps apply to **the daemon and its sessions**; a session exceeding a cap waits
  or is rejected, and the rejection is a durable event (spec 01), never a silent
  drop.

---

## 9. Concurrency and threading

- **One main Asio loop per daemon** (§35): the accept loop, signal set, timers,
  and event fan-out share one `io_context`. No thread per session, no thread per
  client.
- **Per-session ordered delivery** is the `EventBus` mailbox contract (§8.3);
  cross-session interleaving is unconstrained.
- **In-process writes** to a session are serialized by 01 I18 (per-session
  `appendMutex_`) and 02 §8 (writer mutex). The daemon does not add a second
  write path.
- **Cross-process writes** are the lease (02) and, for registry mutations, the
  §54 D22 flock (03).
- **The accept loop never blocks** on a slow client: outbound queues are bounded
  and a persistently slow client is dropped (detach), never the session (D-F9).
- **Heartbeat and lease renewal** are timer-driven on the main loop, not threads
  (02 §8, §9.10).
- **SIGCHLD reaping** is done from the loop after the self-pipe wakes it, never
  inside the handler (§3.6).

---

## 10. Invariants

Numbered `H1`–`H21` (see the naming note at the top). Any code that can violate
one is a defect.

**H1 — One workspace, one cwd, one daemon.** A `WorkspaceHost` owns exactly one
workspace; `chdir()` happens exactly once, at startup, and never again. (§9.7,
§54 D18, F1)

**H2 — No ambient cwd.** No daemon, session, tool, LSP, git, or subprocess path
is resolved via `getcwd()`; resolution goes through
`ExecutionEnvironment::resolve()` / `root()`. (§9.7, §18, 01 I15)

**H3 — Detached session, no controlling terminal.** The daemon is `setsid`'d;
fd 0 is `/dev/null` and fds 1–2 are the log sink; any tty it opens uses
`O_NOCTTY`. (§9.7, §3.2)

**H4 — Fixed startup order.** chdir → mint boot nonce → store open (flock) →
reconcile → bind → `claimHost` → heartbeat → serve. The order is load-bearing
(§3.3).

**H5 — One boot nonce per daemon.** `HostBootId` is minted once and is the same
value in `session_leases.holder_boot_id` (02) and
`workspaces.host_boot_id` (03). (§9.7, §9.10, 03 §6.1)

**H6 — One store, one sidecar flock.** The daemon holds exactly one
`sessions.db` writer and one `LOCK_EX` on `<root>/.ymh/sessions.lock` for its
whole lifetime; `EWOULDBLOCK` is a hard startup failure. (02 §4.3, §5.1)

**H7 — Socket is owner-only and single-writer.** The socket is bound under
`umask(0o077)` and `chmod 0600`; a stale socket is replaced only after a failed
connect-probe and only by the flock holder. (§5.2)

**H8 — The daemon survives detach and TUI exit.** Zero attached clients is a
normal state; the daemon never exits because a supervisor left. (§9.8, §9.9,
§54 D23)

**H9 — Detach ≠ shutdown.** Only `host.shutdown`, SIGTERM, or SIGINT stops the
daemon; detach only removes a client. Detach never emits `SessionEnded`.
(§9.8, 01 I16)

**H10 — Liveness is lock-primary.** A held sidecar flock always means live;
`kill(pid,0)` and heartbeat staleness are hints, never tests. (§9.7, §9.10,
03 R5)

**H11 — Never kill on `ps` evidence.** Orphan reaping clears a stale claim only
on lock-absence + boot-nonce mismatch, under the §54 D22 write lock; a process scan
seeds candidates only and never authorizes a signal. (§9.10, 03 R11)

**H12 — Graceful shutdown is ordered and bounded.** stop accepting → drain/close
sessions → flush (02) → `releaseHost` (03) → unlink socket → exit, within
`shutdown_grace`. (§3.5, D-F16)

**H13 — Signal policy.** SIGTERM/SIGINT graceful; SIGHUP ignored; SIGPIPE
ignored; SIGCHLD reaped (not ignored). (§3.6)

**H14 — One in-process `SessionManager` per daemon.** N sessions live in one
manager; agents/tools are in-process (specs 06/07). (§9.6)

**H15 — One workspace-rooted `ExecutionEnvironment`.** Constructed once per
daemon and shared by its sessions; `root()` is the canonical workspace root.
(§9.6, §18, §54 D18)

**H16 — Per-host resource caps.** LLM pool, global/per-session subprocess and PTY
caps, and output rings are enforced by the daemon's `ResourceGovernor` (§9.11,
F5, F8).

**H17 — Sessions reopen suspended; activation is daemon-driven.** On startup,
existing sessions are loaded `Idle` purely so a restart spawns no LLM burst
(F10); no session is activated by UI focus. The daemon keeps exactly one active
session per workspace and activates the next session with pending work when the
active one finishes or blocks awaiting input. Attach, detach, and focus never
suspend, cancel, or gate work; suspension has explicit causes only (awaiting
input, user action, resource caps). (§9.9, §9.10, decision (m))

**H18 — No sensitive log content.** The daemon never writes full prompts or
sensitive tool output to normal logs; the session event log is the authoritative
trace. (§40)

**H19 — Daemonization is behind the launcher seam.** `WorkspaceHost::run()` is a
single code path for `foreground` and daemon modes; the deterministic suite runs
it in-process, the live layer runs the real binary. (§44, §13.1/§13.5)

**H20 — Daemon registry writes are row-domain-limited.** The daemon writes only
its own claim/heartbeat and its own workspace's junction rows; it never edits
another workspace's rows or `canonical_path`. (03 R16, §9.10)

**H21 — Clean exit leaves no manual cleanup.** Graceful shutdown releases the
flock, releases the claim, and unlinks the socket; a crash leaves state that the
kernel and the next spawn recover automatically. (§3.5, §7.1)

---

## 11. Failure modes

### 11.1 Shared findings (F1–F12, §54)

The daemon's responsibilities for the existing findings:

| F# | Finding | Daemon-layer handling |
|---|---|---|
| **F1** | path/process isolation | one workspace per process; `chdir` once; `setsid`; `resolve()` only (H1, H2, §9.7) |
| **F2** | background permission | a background session hitting `ASK` records the decision as a durable event and the request is forwarded to an attached client; a detached daemon never blocks the loop waiting for a permission answer (spec 09 owns the policy; §9.9, D-F8) |
| **F3** | late event after close | detach does not close; the daemon keeps appending; per-session mailbox drain is the supervisor tombstone's concern (H9, §8.3, §9.8) |
| **F4** | edge-triggered attention | out of scope; the daemon emits core events and holds no attention state (spec 10, §20.23) |
| **F5** | output ring buffers | per-session rings live in `ResourceGovernor`; full output only for active/expanded sessions (H16, §9.11) |
| **F6** | input/keybinding focus | out of scope; focus is supervisor-local and never sent to the daemon (H8, §20.22) |
| **F7** | per-session dirty flags | out of scope; the daemon emits per-session ordered events; dirty flags are the UI model's (§20.23) |
| **F8** | resource caps | the daemon owns and enforces all per-host caps (H16, §9.11) |
| **F9** | cancellation scoping | a cancel names one session; the daemon cancels only that session's in-flight turn (§34, spec 06) |
| **F10** | resume-suspended | sessions reopen `Idle`; no auto-resume burst on start (H17, §9.9) |
| **F11** | subagent ID duality | subagents are ordinary in-process sessions in the same daemon; parent id is display routing only (§20.25, spec 06) |
| **F12** | flash clock in model | out of scope; no UI timers in the daemon (spec 10) |

**Explicitly out of scope for this component:** F2 (policy), F4, F6, F7, F11
(display), F12. The daemon supplies the process, cwd, caps, and event fan-out
those components use; it owns none of their logic.

### 11.2 Component-local failure modes (`D-F#`)

Component-local to the daemon; not part of the top-level F1–F12 set. Covered by
§13.6.

| D-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **D-F1** | Spawn race / workspace busy | `flock(LOCK_EX|LOCK_NB)` returns `EWOULDBLOCK` | Exit `WorkspaceBusy`; the supervisor attaches to the winner (H6, §6.2) |
| **D-F2** | Workspace root missing/not a dir | `stat`/`is_directory` fails at startup | Exit `WorkspaceMissing`; never create `.ymh/` (02 §4.3) |
| **D-F3** | Store open failure | 02 `StoreOpenError` / `SchemaVersionError` | Exit `StoreOpenFailed`; no partial host state (H6, H21) |
| **D-F4** | Registry open/corrupt/schema failure | 03 `RegistryError` | Exit `RegistryFailed`; fail loud, preserve files (03 §10) |
| **D-F5** | Stale claim with a held lock | lock held, diagnostic `(pid,boot_id)` differs | Treat as live; never re-claim from the diagnostic (H10, §7.2) |
| **D-F6** | Socket path too long | canonical path > `sun_path - 1` | Exit `SocketPathTooLong`; never truncate (§5.4) |
| **D-F7** | Stale socket after a crash | file exists, connect returns `ECONNREFUSED` | Unlink (we hold the flock) and rebind (§5.2) |
| **D-F8** | Supervisor disconnect mid-stream | client fd closes / `host.detach` | Detach that client; daemon and sessions continue (H8, H9) |
| **D-F9** | Slow/stuck client | bounded outbound queue full | Drop the client (detach); never stall the agent loop (§9) |
| **D-F10** | Lease lost to another holder | 02 `isLeaseHolder` false / pre-`COMMIT` check fails | Degrade the affected session to read-only, notify clients, keep serving (02 §5.7) |
| **D-F11** | Heartbeat write fails transiently | 03 `heartbeat` throws / no row matched | Retry with backoff; staleness is a hint only; never self-reap (H10) |
| **D-F12** | Clock skew | `host_heartbeat` far in the past while the lock is held | Hint only; never clears a claim (03 R-F17) |
| **D-F13** | Orphan claim (dead daemon) | lock absent + boot-nonce mismatch | Lazy clear under the §54 D22 write lock; never kill (H11, §7.3) |
| **D-F14** | Spoofed/racy `ps` | candidate `argv` disagrees with the derived socket/flock | Seed pids only; confirm by handshake/flock; never kill (H11, §7.4) |
| **D-F15** | `SIGSTOP`ped daemon / split-brain | flock still held; heartbeat stale; `kill` returns 0 | Never reap or steal; the frozen holder is alive (H10, §7.2) |
| **D-F16** | Shutdown timeout | sessions do not quiesce within `shutdown_grace` | Force-close sessions; still flush, `releaseHost`, unlink, exit 0 (H12) |
| **D-F17** | Crash during shutdown | killed after `releaseHost`, before unlink | Stale socket handled by the next spawn's connect-probe (D-F7) |
| **D-F18** | Log sink unwritable | open of `log_sink` fails in the fork child | `_exit(StartupRejected)`; never run with an unwritable sink (§3.2) |
| **D-F19** | Socket bind fails | `bind`/`listen` errno (perm, address in use) | Exit `SocketBindFailed`; release the flock and claim first (§3.3) |
| **D-F20** | Double start, same workspace | two daemons both reach `open()` | One wins the flock; the loser exits `WorkspaceBusy`; no orphan (D-F1) |
| **D-F21** | Startup failure after claim | a step after `claimHost` fails | Release the claim, close the store, unlink the socket, exit non-zero (H21) |
| **D-F22** | Reconnect supersedes a stream | same authenticated identity attaches again | Replace the old `ClientId`; the old stream is dropped (§6.3) |

---

## 12. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (§55). The daemon maps onto it as
follows.

| dsh concept | ymh WorkspaceHost daemon | Reference |
|---|---|---|
| Workspace runtime process | one `WorkspaceHost` per workspace | §9.6, §54 D17 |
| Service container / context | in-process `SessionManager` + `Context` + `EventBus` | §6, §8.3, §9.6 |
| Session manager | one per daemon, N sessions | §9.6, 01 §8 |
| Supervisor / aggregator | the TUI attaches to N daemons | §9.6, §20.22 |
| Service discovery | registry row + socket + authenticated handshake | 03 §6, §9.10, spec 05 |
| Process/service liveness | sidecar `flock` + boot-nonce cross-check | 02 §5.1, 03 §6.3 |
| Graceful shutdown | signal set → drain → flush → `releaseHost` → unlink | §3.5, §3.6 |
| Per-host resource scheduler | `ResourceGovernor` over `ResourceCaps` | §9.11, F8 |
| Remote runtime transport | TCP deferred; Unix socket v1 | §9.6, §43, §47 |
| Hot reload / dynamic plugins | not modeled (accepted for v1) | §55, §54 D11 |
| Session event log | owned by spec 01/02; the daemon persists and forwards | §9.1, §8.3 |

**Deliberate omissions** (accepted for v1, §55): no distributed registry, no
hot plugin reload, no offline session cache (a live host is required, §9.10),
and no TCP transport. The daemon is a small, headless, single-workspace process;
it is not a general service mesh.

---

## 13. Test plan

Strategy is §44: unit, integration (fake LLM / fake FS / fake shell), golden,
replay, and a separate live PTY/real-LLM layer (§44, §45). The deterministic
layers run offline against the Fake LLM (§45); the live layer is opt-in and
API-key gated. Daemon tests use a temporary real workspace directory with a real
`sessions.db`/`sessions.lock` (where the subject is flock/SQLite semantics), a
**fake launcher** (in-process `WorkspaceHost` with `foreground == true`), a
**fake registry** where the subject is a consumer, and a **fake process table**
for scan-fallback boundaries.

### 13.1 Unit tests

- **Config / validation**
  - `create()` rejects a missing/non-directory root with
    `HostError{WorkspaceMissing}` and creates no `.ymh/` (D-F2).
  - Socket-path length is checked before bind; an over-long path yields
    `SocketPathTooLong`, never a truncated bind (D-F6, §5.4).
  - `HostConfig` defaults: `heartbeat_interval == 5 s`, socket
    `<root>/.ymh/host.sock`, log `<root>/.ymh/host.log`.
- **State machine**
  - `Starting → Serving` only after steps 1–8; `Serving → Draining → Stopped`;
    any startup failure goes to `Failed` and a non-zero `HostExitCode` (§4.4).
  - Requests before `Serving` or during `Draining` return `NotServing` /
    `ShutdownInProgress`.
- **Startup order (fake store/registry recording side effects)**
  - The recorded call order is exactly chdir → mint nonce → store open →
    reconcile → bind → `claimHost` → heartbeat → serve (H4).
  - The boot nonce passed to the store equals the one passed to `claimHost`
    (H5).
  - `chdir` is called exactly once (H1).
- **Socket bind/replace (fake FS)**
  - Stale socket + refused connect ⇒ unlink + rebind; live socket ⇒
    `AlreadyRunning` (D-F7, D-F19).
  - `umask(0o077)` + `chmod 0600` yields an owner-only socket (H7).
- **Signals (injectable)**
  - SIGTERM and SIGINT route to graceful shutdown; SIGHUP is ignored (logged);
    SIGPIPE is ignored; a synthetic SIGCHLD triggers a `waitpid(WNOHANG)` reap
    (H13, §3.6).
- **Log sink**
  - An unwritable `log_sink` aborts the child with `StartupRejected` before exec
    (D-F18).
- **No ambient cwd**
  - A trapping `getcwd()` proves no daemon path resolution calls it (H2).

### 13.2 Integration tests (Fake LLM, §45)

- **Spawn + attach (in-process host).** `foreground == true`, a fake launcher, a
  real temp store and registry: start, attach a fake client, create a session,
  assert `attachedClients() == 1` and the junction row exists (H14, H20).
- **Detach ≠ stop.** Detach the client; assert the daemon is still `Serving`,
  the session keeps appending, and `attachedClients() == 0` (H8, H9).
- **Multi-attach.** Two fake clients attach; both receive per-session events in
  publish order; a cancel from one is scoped to the named session (F9).
- **Survive TUI exit.** Start a host via the launcher, kill the supervisor
  process, assert the daemon pid is alive and the socket still connects (§6.5).
- **Graceful shutdown.** SIGTERM the daemon; assert the order: stop accepting →
  flush → `releaseHost` → flock released → socket unlinked; exit code 0 (H12,
  H21).
- **Reconcile.** A junction row with no store session and a store session with no
  junction row are reconciled at startup under the marker protocol (03 R-F12).
- **Reopen suspended.** Pre-seed sessions; on start, `list()` returns them and
  none auto-runs (H17, F10).
- **Lease loss.** Force `isLeaseHolder` false for one session; the daemon
  degrades that session read-only and keeps serving the rest (D-F10).

### 13.3 Crash / orphan tests

- **SIGKILL crash + lazy reap.** Kill the daemon; assert the kernel released the
  flock, the claim is stale, and the supervisor clears it under the §54 D22 lock on
  next access (H10, H11, D-F13).
- **SIGSTOP guard.** `SIGSTOP` the daemon; assert a concurrent would-be reaper
  does **not** clear the claim even with a stale heartbeat (D-F15).
- **PID reuse.** Reuse a pid with a different boot nonce; assert it never matches
  the claim (H10, 03 R-F5).
- **Spawn race.** Two launchers race one workspace; exactly one daemon serves;
  the loser exits `WorkspaceBusy`; the losing supervisor attaches (D-F1).
- **Stale socket.** `SIGKILL` the daemon; the next spawn unlinks the refused
  socket and rebinds (D-F7).
- **Spoofed scan.** A fake process table advertises a `ymh --host` pid whose
  derived socket/flock disagrees; the supervisor confirms nothing and kills
  nothing (H11, D-F14).
- **Shutdown timeout.** A session that refuses to quiesce forces the bounded
  path; the flock is released, `releaseHost` runs, the socket is unlinked, exit 0
  (D-F16).
- **Crash during shutdown.** Kill between `releaseHost` and unlink; the next
  spawn recovers the stale socket (D-F17).
- **Startup failure after claim.** Inject a failure after `claimHost`; assert the
  claim is released, the store is closed, and the socket is unlinked (D-F21).

### 13.4 Golden / replay tests

- **Startup-sequence golden.** A recorded ordered side-effect log from a fixed
  config is byte-identical across runs (H4).
- **Lifecycle golden.** spawn → attach → detach → re-attach → shutdown produces
  the same ordered event/state trace across runs (H8, H9, H12).
- **Shutdown-order golden.** The exact order stop-accept → drain → flush →
  release → unlink is asserted against a stable fixture (H12).
- **Replay.** A recorded session event stream delivered through the daemon's
  fan-out projects to the same state on every replay (H14, §44).

### 13.5 Live end-to-end tests (real LLM, PTY-driven, §44)

Milestone-2 gated (§57 Step 13, §58). The live layer starts the **real** `ymh`
binary under a PTY and drives it as a human would:

- Start the TUI under a PTY (`forkpty`/`posix_openpt` or `tmux send-keys` +
  `capture-pane`), create two workspaces, and assert two daemon processes exist,
  each with a bound socket and a live registry claim (§3.2, §3.3).
- Exit the TUI; assert both daemons are still alive and their sessions keep
  running; re-open the TUI and assert re-attach restores the state (§6.5).
- Kill one daemon; assert the supervisor reaps its stale claim on next access and
  re-spawns it, without killing the live daemon (H11, §7.3).
- Drive the real LLM through the supervisor; assert the session streams, a tool
  call renders, and a permission prompt is handled (§9.6, §43).
- Skipped (not failed) without an API key plus an explicit opt-in flag; tolerant
  of model nondeterminism — assert structure and invariants, never prose (§44).
- **Daemonization must not break the harness.** The live PTY driver attaches to
  the *supervisor's* terminal; the daemon is `setsid`'d and never reads the PTY,
  so `capture-pane` output is unaffected by daemonization (H3, H19).

### 13.6 Failure-mode coverage matrix

| Failure | Test layer | Asserts |
|---|---|---|
| F1 | unit + integration | one workspace/cwd; single chdir; `resolve()` only (H1, H2) |
| F2 | integration | a detached daemon never blocks on `ASK`; the request is forwarded/recorded |
| F3 | integration | detach keeps appending; no `SessionEnded` (H9) |
| F8 | unit | caps enforced by `ResourceGovernor`; no cap logic in the supervisor (H16) |
| F10 | integration | sessions reopen `Idle`; no auto-run (H17) |
| D-F1 | crash | spawn race ⇒ one daemon, loser `WorkspaceBusy` |
| D-F2 | unit | missing root ⇒ `WorkspaceMissing`, no `.ymh/` |
| D-F3 | unit | store failure ⇒ `StoreOpenFailed` |
| D-F4 | unit | registry failure ⇒ `RegistryFailed`, files preserved |
| D-F5 | integration | held lock + mismatched diagnostic ⇒ live (H10) |
| D-F6 | unit | over-long socket path ⇒ `SocketPathTooLong` |
| D-F7 | crash | stale socket ⇒ unlink + rebind |
| D-F8 | integration | client disconnect ⇒ detach; daemon continues |
| D-F9 | integration | slow client dropped; loop unblocked |
| D-F10 | integration | lease loss ⇒ read-only session, daemon serving |
| D-F11 | unit | transient heartbeat failure retried; no self-reap |
| D-F12 | unit | clock skew is a hint only |
| D-F13 | crash | dead daemon claim cleared under §54 D22 lock; no kill |
| D-F14 | unit + crash | spoofed scan seeds only; no kill (H11) |
| D-F15 | crash | `SIGSTOP`ped holder not reaped |
| D-F16 | crash | shutdown timeout still flushes/releases/unlinks |
| D-F17 | crash | crash after `releaseHost` ⇒ next spawn recovers socket |
| D-F18 | unit | unwritable log sink ⇒ `StartupRejected` |
| D-F19 | unit | bind failure ⇒ `SocketBindFailed`; flock/claim released |
| D-F20 | crash | double start ⇒ one winner (D-F1) |
| D-F21 | integration | post-claim startup failure cleans up |
| D-F22 | integration | reconnect supersedes the old stream |

### 13.7 Invariant coverage

Every invariant `H1`–`H21` maps to at least one test, or is marked
covered-by-construction (CBC) with the reason it cannot be violated.

| Invariant | Test layer | Coverage |
|---|---|---|
| H1 one workspace/one cwd | unit + integration | single chdir; two workspaces ⇒ two daemons |
| H2 no ambient cwd | unit | trapping `getcwd()`; resolution via `resolve()` |
| H3 detached session | unit | `setsid`; fd 0 `/dev/null`; fds 1–2 log sink |
| H4 fixed startup order | unit | side-effect log golden |
| H5 one boot nonce | unit | store nonce == claim nonce |
| H6 one store/one flock | integration | second `open()` ⇒ `WorkspaceBusy` |
| H7 owner-only socket | unit | mode 0600; stale-socket replace |
| H8 survives detach | integration | detach ⇒ still `Serving` |
| H9 detach ≠ shutdown | integration | no `SessionEnded`; only shutdown stops |
| H10 lock-primary liveness | crash | `SIGSTOP` + PID reuse + skew (D-F12/D-F15) |
| H11 never kill on `ps` | unit + crash | spoofed scan cannot kill (D-F14) |
| H12 ordered shutdown | crash | shutdown-order golden + timeout (D-F16) |
| H13 signal policy | unit | TERM/INT/SIGHUP/SIGPIPE/SIGCHLD routing |
| H14 one `SessionManager` | integration | N sessions in one manager |
| H15 one `ExecutionEnvironment` | integration | `root()` == workspace root for all sessions |
| H16 per-host caps | unit | caps enforced; per-host independence |
| H17 reopen suspended; daemon-driven activation | integration | no auto-run (F10); attach/focus do not start or suspend work |
| H18 no sensitive logs | CBC | log sink carries no prompt/tool payload by construction (§40) |
| H19 launcher seam | integration + live | same `run()` for foreground and daemon |
| H20 row-domain writes | integration | daemon cannot edit other workspaces' rows (03 R16) |
| H21 no manual cleanup | crash + integration | clean exit vs. crash recovery paths |

---

## 14. Decisions and open questions

### 14.1 Decisions (pinned by this spec)

- **(a) Spawn is hand-rolled `fork` + `setsid` + `execve`, never `daemon(3)`.**
  `daemon(3)` `chdir("/")`s and would defeat the workspace root; the hand-rolled
  sequence keeps the single `chdir` in `run()` (H1, §3.2).
- **(b) The daemon is a single-fork `setsid` process, not a double-fork.** A
  single fork is never a process-group leader, so `setsid` succeeds; the daemon
  never opens a tty without `O_NOCTTY`, so it cannot re-acquire a controlling
  terminal (§3.2, H3).
- **(c) stdio is the log sink; fd 0 is `/dev/null`.** The daemon is headless; an
  unwritable sink is a hard startup failure (D-F18, §3.2, §40).
- **(d) Startup order is chdir → mint nonce → store → reconcile → bind → claim →
  heartbeat → serve.** Bind precedes claim so a published claim always points at
  a connectable socket (§3.3, H4).
- **(e) The socket is bound under `umask(0o077)` and `chmod 0600`, and a stale
  socket is replaced only after a refused connect and only by the flock holder.**
  The flock is the authority; the connect-probe is defense-in-depth (§5.2, H7).
- **(f) Detach never stops the daemon; only `host.shutdown`/SIGTERM/SIGINT do.**
  This is §54 D23 and the §9.8 detach/delete separation (H8, H9, §6.4).
- **(g) Liveness is lock-primary; the daemon never self-reaps on heartbeat
  staleness or `kill` hints.** A `SIGSTOP`ped daemon is alive and holds its
  flock (H10, D-F15, 03 §6.3).
- **(h) Orphan reaping clears a stale claim; it never signals a process on `ps`
  evidence.** Reap requires lock-absence + boot-nonce mismatch under the §54 D22
  write lock (H11, §7.3, 03 R11).
- **(i) SIGCHLD is reaped, not ignored.** `SIG_IGN`/`SA_NOCLDWAIT` would destroy
  `waitpid` and the exit status tool execution needs (H13, §3.6).
- **(j) Graceful shutdown is bounded by `shutdown_grace` and always releases the
  flock, the claim, and the socket.** Leaving any of them blocks the next spawn
  (H12, H21, D-F16).
- **(k) `<workspace>/.ymh/host.sock` and `<workspace>/.ymh/host.log` are v1
  defaults, not protocol commitments.** Spec 05 owns the final socket convention
  (OQ-3); spec 10 owns the final logging path, so `host.log` is **provisional**
  (OQ-5). (H7, §5.1)
- **(l) `foreground == true` is the deterministic-test path and shares one
  `run()` with the daemon path.** There is no `#ifdef TEST` divergence (H19,
  §4.3, §13).
- **(m) Activation is daemon-driven; UI focus is display-only.** The daemon keeps
  exactly one active session per workspace and activates the next session that
  has pending work when the active one finishes or blocks awaiting input. Attach,
  detach, and focus never suspend, cancel, or gate work; "suspended" means only an
  explicit reason (awaiting input, user action, resource caps).
  `session.activate` / `session.suspend` (spec 05) are explicit controls, not a
  mirror of UI focus. Resolves OQ-4.
- **(n) The boot nonce is minted by the daemon and injected via
  `PersistenceConfig::boot_id`.** The store never mints it; `open()` stamps the
  caller-supplied value into the sidecar-lock diagnostic and the lease rows (H5,
  §3.3, 02 §4.1/§5.1/§5.2). Resolves OQ-1.
- **(o) Daemon invariants are labeled `H1`–`H21`.** This removes the collision
  with `00-architecture.md` §54's `D1`–`D23` decisions (header naming note).
  Resolves OQ-2.
- **(p) The final socket convention is deferred to spec 05.**
  `<workspace>/.ymh/host.sock` stands as the non-committal v1 default (H7, §5).
  OQ-3.
- **(q) The final daemon log path is deferred to spec 10.**
  `<workspace>/.ymh/host.log` is the provisional v1 default (H18, §3.2, §40).
  OQ-5.
- **(r) Child/PTY ownership is split.** 04 owns the daemon's process tree (the
  daemon and its process group); spec 07 owns the tool subprocesses and PTYs
  (§3.5 step 4, §7.5). OQ-6.

### 14.2 Open questions

OQ-1 (nonce seam), OQ-2 (label collision), and OQ-4 (§9.9 focus) are **resolved**
— see decisions (n), (o), and (m). The questions below remain open and are
deferred to their owning specs.

- **OQ-3 — Final socket convention and path-length handling (deferred to spec 05).**
  This spec uses `<workspace>/.ymh/host.sock` (matching 03 §6.5) as the
  non-committal v1 default and fails loud on paths longer than `sun_path`. If
  spec 05 chooses a different convention (e.g. an abstract-namespace name or a
  hashed short path under the state dir), §5, 03 §6.5, and 03 §15.1(l) must
  change together.
- **OQ-5 — Daemon log sink location (deferred to spec 10; default provisional).**
  §40 names categories and spdlog but does not fix a daemon log path, and spec 10
  owns the logging surface. This spec's default `<workspace>/.ymh/host.log` is
  **provisional**; a per-workspace state-dir log
  (`<state>/ymh/workspaces/<id>/host.log`) is an alternative. Whichever spec 10
  chooses, the sink must obey §40 (no prompts/sensitive tool output) and be
  owner-only.
- **OQ-6 — Orphaned tool children and PTYs after a daemon crash (ownership
  pinned; mechanics deferred to spec 07).** On crash, the daemon's tool
  subprocesses/PTYs are reparented to `init`; on graceful shutdown the daemon
  signals its process group. **04 owns the daemon process tree** (the daemon and
  its process group); **spec 07 owns the tool subprocesses and PTYs** (process
  groups, `PR_SET_CHILD_SUBREAPER`, PTY teardown). This spec pins only the
  daemon's signal policy (SIGCHLD reaped, §3.6) and the shutdown step
  (§3.5 step 4).

---

## 15. References

- `00-architecture.md` §4.1 (agent ≠ UI), §4.2 (event stream is the runtime
  spine), §4.4 (execution environment as a capability boundary), §6 (runtime
  context), §8.1–§8.3 (event system, `SessionId`-routed delivery), §9.2 (schema),
  §9.6 (supervisor/daemon topology, `SessionManager`), §9.7 (isolation, path
  safety, write lease), §9.8 (detach/delete/archive), §9.9 (background execution
  and attention), §9.10 (shared workspace registry, liveness, process-scan
  fallback), §9.11 (resource limits), §10.1 (`AgentHandle`), §16–§18 (process,
  PTY, execution environment), §19 (permissions), §20.22 (`WorkspaceModel`,
  focused session), §20.23–§20.25 (attention, switcher, subagent fan-in), §34
  (cancellation), §35 (concurrency), §40 (logging), §41–§43 (CLI, headless, RPC),
  §44–§45 (testing, Fake LLM), §47 (SSH), §49 (source tree), §54 (decisions
  D1–D23, F1–F12), §55–§56 (dsh comparison), §57 Step 13, §58 (milestones).
- `01-session.md` §2.1 (`SessionId`/`WorkspaceId`), §3 (`SessionHeader`), §6
  (append protocol), §7 (`SessionStore`/`SessionPersistence`/`SessionHandle`),
  §8 (`SessionManager`), §9 (lifecycle), §10 (write lease, path safety), §11
  (concurrency), §16.1 (pinned decisions).
- `02-persistence.md` §2.1 (`BootId`, `HolderIdentity`), §3.1–§3.2 (per-workspace
  DB, pragmas), §4.1 (`PersistenceConfig`), §4.3 (open/close policy), §5 (write
  lease, sidecar flock, boot nonce, liveness), §6 (flush/checkpoint), §7 (crash
  recovery), §8 (concurrency), §13.1 (pinned decisions).
- `03-workspace-registry.md` §2.1 (`HostPid`, `HostBootId`, `HostClaim`), §3.3
  (host columns), §4.2 (`WorkspaceRegistry`), §4.4 (mutation semantics), §5
  (single writer), §6 (host registration, heartbeat, liveness, lazy clearing,
  process-scan fallback), §10 (crash recovery), §11 (`R1`–`R16`), §12.2
  (`R-F#`), §13 (dsh mapping), §15.1 (pinned decisions).
- `HANDOFF.md` §2 (the rule), §5 items 1–3 (naming, bootstrap discovery,
  `SessionHeader`), §6 row 04 (component plan), §7 (definition of verified).
- `DESIGN_STATUS.md` (written/verified tracker).

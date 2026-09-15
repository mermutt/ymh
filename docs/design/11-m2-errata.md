# 11 — Milestone 2 Interface-Freeze Errata

```
Status: written · verified: — · reviewer: —
Component: 11 (errata) — amends 03/04/05/09/10 by reference
Depends on: 00-architecture.md §9.6/§9.10/§57 Step 13/§58; specs 01, 02, 03, 04, 05, 06, 09, 10
Scope: Milestone 2 (supervisor TUI + per-workspace WorkspaceHost daemons)
```

This errata is the **freeze artifact** for the Milestone 2 (M2) split. A five-analyst
adversarial review of the verified component specs against the M1 code on disk
produced 26 defects/gaps (`D1`–`D26`). This document pins the corrected
interfaces and decisions so Wave-1 code can proceed. It does **not** rewrite the
amended specs; it amends them by reference, and each amendment names the exact
§/line it supersedes. Where a signature is code-adjacent it is pinned here; where
a behaviour is already correct it is restated as an invariant.

The M1 code is the ground truth for what exists: `include/ymh/transport/*`,
`include/ymh/agent/workspace_runtime.hpp`, `include/ymh/registry/*`,
`include/ymh/session/*`, `include/ymh/policy/permission_policy.hpp`,
`src/cli/wiring.cpp`, `src/transport/*`, `src/agent/workspace_runtime.cpp`. The
M2 code is *not* written yet; this document is the last design gate before it is.

Naming note (as in every spec): defect ids are `D1`–`D26`; invariants local to
this errata are `E1`–`E20`; failure modes are `M-F1`–`M-F12`. The review defect
ids `D1`–`D26` are **distinct from** architecture decision ids of the same
spelling — e.g. 03's "D22" is the registry single-writer `flock` (§5), not review
defect `D22`. Where a bare `D<n>` could be ambiguous, the decision id is written
"D22 (03 §5)".

---

## 1. Scope & amendment map

This errata touches five verified specs. One line each:

| Spec | Amendment |
|---|---|
| `03-workspace-registry.md` | Clarify the §5.3 row-domain table (host columns are a **shared/provisional** domain, not daemon-exclusive) and restate §6.2's supervisor-provisional → daemon-authoritative claim handshake (D21). |
| `04-workspace-host-daemon.md` | Add the M2 loop/turn-executor split, replace §3.6's global SIGCHLD reaper with `process.cpp`-owned specific-pid `waitpid`, add `WorkspaceRuntimeErrorCode::WorkspaceBusy`, pin store-opened-exactly-once + provider ordering, and pin destruction ordering (D6–D10, D23, D24). |
| `05-transport.md` | Add `TransportServer::post()`/`io()`, pin `ProtocolServer` single-thread confinement + uid default + connect-probe/inode-unlink + ping/idle keepalive (D1–D5, D20). |
| `09-permissions.md` | Pin the async `PermissionBroker` interface, live Interactive-subscriber count, fail-closed timeout auto-deny, re-broadcast on attach, first-wins, and the turn-thread ≠ transport-thread boundary (D18, D19). |
| `10-supervisor-tui.md` | Pin M2 entry (`ymh` no-args), read-only browse without a daemon, multi-workspace model, and non-blocking bootstrap discovery (D22, D26). |

Additionally `01-session.md` / `02-persistence.md` receive two additive store
seams (`headSequence`, bounded `readAfter`; §5) and the boot-nonce threading
(§6). These are additive (no existing signature changes) and are recorded here
rather than as a separate errata.

`D17` (boot nonce) and `D8` (exit/error codes) are cross-cutting and are frozen
in W0; every later wave depends on them.

---

## 2. Transport hardening (amends 05 §3.1–§3.3, §4.2, §4.5, §9)

### 2.1 Defects

- **D1** — `TransportServer` (pinned `include/ymh/transport/transport_server.hpp:27-53`)
  exposes no way to marshal a callback onto its `io_` thread, and no executor
  accessor. `ProtocolServer` documents itself as single-thread ("every
  `receiveBytes`, `onEventCommitted`, and host notification runs on the owning
  thread", `protocol_server.hpp:12-14`), yet the daemon's EventBus/PermissionGate
  callbacks fire on the **turn thread** (§3.2). Calling `onEventCommitted` /
  `onPermissionRequest` / `onSessionClosed` directly from the turn thread races
  the fan-out loops at `protocol_server.cpp:690-707` and `:776`.
- **D2** — `TransportServer::stop()` unlinks `socket_path_` unconditionally after
  join (`src/transport/transport_server.cpp:208-212`). 04 §5.3 requires the
  unlink to be skipped when a superseding daemon has already rebound the path
  (inode compare). Today `stop()` can delete a live successor's socket.
- **D3** — `TransportServer::start()` calls `std::filesystem::remove(socket_path_)`
  before bind (`src/transport/transport_server.cpp:170`) with no connect-probe.
  04 §5.2 requires: if a file exists, non-blocking `connect()`; success ⇒
  `AlreadyRunning`; `ECONNREFUSED` ⇒ unlink (stale); other errno ⇒
  `SocketUnavailable`. The current code unconditionally clobbers.
- **D4** — `ProtocolServerConfig::uid` defaults to `0`
  (`include/ymh/transport/protocol_server.hpp:40`). The same-UID trust boundary
  (05 §4.2) must compare against the daemon's real uid; a `0` default makes the
  check either always-fail or, worse, match a peer uid of 0 in a container.
- **D5** — No keepalive is wired. `TransportLimits::idle_timeout` is 30 s
  (`protocol.hpp:191`) but there is no `host.ping` path (05 §4.5), so a healthy
  idle supervisor is indistinguishable from a half-closed socket until the timer
  fires.

### 2.2 Pinned `TransportServer` public API additions

```cpp
// include/ymh/transport/transport_server.hpp  (additions, D1)
namespace ymh::protocol {

class TransportServer {
public:
    TransportServer(ProtocolServer& server, std::string socket_path,
                    TransportLimits limits = {});
    ~TransportServer();

    TransportServer(const TransportServer&) = delete;
    TransportServer& operator=(const TransportServer&) = delete;

    void start();
    void stop();

    // D1: marshal `fn` onto the transport's io thread. Safe from any thread.
    // Returns false (and drops `fn`) before start() or after stop() has begun;
    // callers must NOT assume execution from the return value alone.
    bool post(std::function<void()> fn);

    // D1: the transport's io_context, for constructing dependent steady_timers
    // and for scheduling on the same thread. Never run() it from the caller;
    // the transport owns the single runner thread.
    [[nodiscard]] asio::io_context& io() noexcept;

    [[nodiscard]] bool running() const noexcept;   // started_ && !stopping_
    [[nodiscard]] const std::string& socketPath() const noexcept;

private:
    // unchanged fields; add:
    //   std::mutex post_mutex_;            // guards post() vs. start()/stop() handoff
    //   std::optional<ino_t> own_inode_;   // D2: recorded at bind; unlink only on match
};

} // namespace ymh::protocol
```

**Invariants.**

- **E1 — One runner thread.** `io_.run()` is executed by exactly one thread,
  started in `start()` and joined in `stop()`. `post()` never starts a runner.
- **E2 — Confinement.** Every `ProtocolServer` public method that mutates
  connection/fan-out state is called only on the transport io thread. The daemon
  obeys this by wrapping the EventBus / permission callbacks:

  ```cpp
  // daemon wiring (D1). `transport` is the TransportServer.
  bus.subscribe([transport = transport_.get(), server = server_.get()](const EventRecord& r) {
      transport->post([server, r] { server->onEventCommitted(r); });
  });
  gate.set_attention_hook([transport = transport_.get(), server = server_.get()](
                              const PermissionRequestId&, const PermissionRequest& req) {
      transport->post([server, req] { server->onPermissionRequest(to_wire(req)); });
  });
  ```

  The two hooks above are illustrative, not exhaustive: **every** `ProtocolServer`
  entry point listed at `protocol_server.hpp:53-63` is called only on the io
  thread, marshalled with `post()` — `onEventCommitted`, `onPermissionRequest`,
  `onSessionClosed`, `onLeaseLost`, and `onDaemonShuttingDown`. A direct call
  from the turn thread is a defect (M-F1).

  `post()` returning `false` after shutdown is the correct drop behaviour: the
  daemon is draining and the event is already durable in the store, so a
  reconnecting supervisor replays it from its cursor (§8).

- **E3 — uid.** `ProtocolServerConfig::uid` is set from `::getuid()` at daemon
  construction; the literal `0` default in the header is a sentinel only and is
  never passed to a live server.

### 2.3 Socket lifecycle (D2, D3)

`start()` becomes (replacing `transport_server.cpp:168-189`):

```text
start():
  if path length > sun_path - 1:            -> throw SocketPathTooLong
  if lstat(socket_path, &st) == 0:           # D2: st is defined only on success
      non-blocking connect(socket_path)
        success        -> throw AlreadyRunning
        ECONNREFUSED   -> unlink(socket_path)          # stale; flock holder owns it
        other errno    -> throw SocketUnavailable
  open(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC); bind(); chmod(0600); listen()
  if lstat(socket_path, &st) == 0: own_inode_ := st.st_ino   # D2
  else:                            own_inode_.reset()        # unknown inode: never unlink
  do_accept(); thread_ = std::thread([this]{ io_.run(); })
```

`stop()` becomes (replacing `transport_server.cpp:191-213`):

```text
stop():
  if stopping_.exchange(true): return
  asio::post(io_, [this]{ close acceptor; close all sessions; });
  join runner thread
  if started_ and own_inode_ and lstat(socket_path, &st) == 0 and st.st_ino == *own_inode_:
      unlink(socket_path)                              # D2: never a successor's socket
  started_ = false
```

**Invariant E4 — Owned unlink.** `stop()` unlinks only a path whose inode equals
the inode recorded at bind. If a successor rebound the path, the inode differs and
the unlink is skipped. If the bind-time `lstat` failed, `own_inode_` stays empty
and `stop()` skips the unlink entirely (fail-safe: never delete an unproven
path). This mirrors 04 §5.3 (`04-workspace-host-daemon.md:630-636`)
and 04 decision (e) (`:1365-1367`).

### 2.4 Keepalive (D5)

- The supervisor sends `host.ping` every `ping_interval` (default **10 s**), a
  normal request/response; the daemon replies `pong`. Ping participates in the
  same frame budget and never gates session work (05 §4.5,
  `05-transport.md:539-545`; D20).
- The daemon closes a connection that has sent **no frame for `idle_timeout`
  (default 30 s)** — already enforced by the per-session `idle_timer_`
  (`transport_server.cpp:81-93`), but the timer must be **re-armed on any inbound
  frame** (it currently is: `:76`), and must not fire while a `host.ping` reply is
  in flight.
- `TransportLimits` gains no new field; `ping_interval` is a supervisor-side
  constant (`10` §9.2 keybinding namespace is unaffected). 30 s idle > 10 s ping
  gives three missed pings before a drop, matching the registry's
  `heartbeat_stale_hint` ratio (03 §6.2).

**Invariant E5 — Liveness without work-gating.** A ping/pong never changes
`HostState`, never activates a session, and never counts as a subscription.

---

## 3. Daemon loop & turn executor (amends 04 §3.3, §3.4, §3.6, §9)

### 3.1 Defects

- **D6** — 04 §9 (`04-workspace-host-daemon.md:968-985`) says "one main Asio loop
  per daemon" and "no thread per session", but never says where the **agent turn
  body** runs. `AgentLoop::activate()` runs the turn synchronously
  (`src/agent/agent_loop.cpp:149-162`). If invoked on the io thread, one tool call
  or LLM wait stalls accept, heartbeat, lease renewal, and every other session's
  stream. M2 must pin a bounded worker.
- **D7** — 04 §3.6 mandates a **global SIGCHLD reaper** ("reaped with
  `waitpid(WNOHANG)` from an async-signal-safe self-pipe", `:416`, `:419-422`;
  decision (i) `:1376-1377`). But `src/execution/process.cpp` already owns child
  status with **specific-pid** `waitpid(pid, …)` (`process.cpp:114-141`). A global
  reaper races it: it can reap a tool child first and destroy the exit status the
  tool layer needs (and `SIG_IGN`/`SA_NOCLDWAIT` is correctly rejected). This is
  the single most dangerous M2 conflict.
- **D8** — `WorkspaceRuntimeErrorCode` (`workspace_runtime.hpp:53-58`) has
  `WorkspaceMissing`, `StoreUnavailable`, `ProviderSetupFailed`, `Internal` — but
  no `WorkspaceBusy`. `HostExitCode::WorkspaceBusy` exists in the spec
  (`04:147`) but the runtime's `StoreUnavailable` conflates "unopenable" with
  "another daemon holds the flock" (02 §5.1 `EWOULDBLOCK`).
- **D9** — The store must be opened **exactly once** per daemon (02 §4.3;
  `WorkspaceRuntime::create` does this at `workspace_runtime.cpp:117-123`), but
  there is no injection seam to substitute a fake store in tests without opening
  the real SQLite file.
- **D10** — Provider construction order relative to bind is unpinned. Today
  `WorkspaceRuntime::create` opens the store, then builds the provider, then
  builds `Impl` (`workspace_runtime.cpp:117-149`). If a null provider were
  tolerated, the daemon could bind a socket and serve before it can serve a turn.

### 3.2 Loop ownership (D6)

One `asio::io_context` per daemon owns: the acceptor (via `TransportServer`), the
`signal_set`, the heartbeat timer, the lease-renewal timer, and all `post()`ed
transport callbacks. The **agent turn never runs on it.**

```cpp
// daemon-internal (not a public seam); lives in WorkspaceHost::Impl.
class TurnExecutor {
public:
    // max_workers = 04 ResourceCaps::max_llm_concurrency (default 4, 04 §2.1);
    // queue_capacity defaults to 4, matching max_llm_concurrency. The queue is
    // the only backpressure: submit() never spawns a thread.
    explicit TurnExecutor(std::size_t max_workers = 4,
                          std::size_t queue_capacity = 4);

    // Submit a turn body. Returns false when the bounded queue is full
    // (caller maps to RpcCode::InternalError / AgentErrorCode::InboxFull).
    bool submit(std::function<void()> body);

    // Drain: stop accepting, join workers within `grace` (shutdown_grace, 04 §3.5).
    void drain(std::chrono::milliseconds grace);
    [[nodiscard]] std::size_t inFlight() const noexcept;

private:
    std::vector<std::thread>       workers_;
    std::deque<std::function<void()>> queue_;
    std::size_t                    queue_capacity_;
    std::mutex                     mutex_;
    std::condition_variable        cv_;
    bool                           stopping_{false};
};
```

- **E6 — Turn off-loop.** `HostRuntime`'s `agent.prompt` / `agent.followup` /
  `agent.steer` handlers, and the `AgentRegistry::activateSession` call, are
  marshalled onto `TurnExecutor`; the transport io thread returns the RPC
  acknowledgement immediately. `host.shutdown` drains the executor (04 §3.5
  step 3, `:383-386`).
- **E7 — Bounded.** `max_workers` = `ResourceCaps::max_llm_concurrency` (04 §2.1,
  `:173`); the queue is bounded by `queue_capacity` (default 4, the same
  `max_llm_concurrency` value); overflow is a typed error, never an unbounded
  thread spawn.

### 3.3 Child status ownership (D7) — amends 04 §3.6

**Decision (pinned).** There is **no global SIGCHLD reaper.** 04 §3.6's SIGCHLD
row (`:416`) and the self-pipe paragraph (`:419-422`), and 04 decision (i)
(`:1376-1377`), are amended by reference:

- The daemon does **not** install a global SIGCHLD handler. It either leaves the
  disposition at `SIG_DFL` or registers a no-op `asio::signal_set` that merely
  drains the notification and **never** calls `waitpid`. `SIG_IGN`/`SA_NOCLDWAIT`
  is still forbidden: it auto-reaps and destroys `waitpid` (04 §3.6 `:416`).
- Child status is owned exclusively by `src/execution/process.cpp`, which reaps
  by specific pid (`reap(int pid)`, `process.cpp:114-141`). `process.cpp` already
  loops on `EINTR` and uses `WNOHANG` for a bounded poll (`:129-141`).
- The daemon's tool/PTY teardown (04 §3.5 step 4) signals the child's **process
  group** and then waits via the process layer's specific-pid path; it never
  sweeps the whole child table.

**Invariant E8 — One reaper.** Exactly one code path calls `waitpid` for a given
child, and it is the path that owns that child's `ProcessResult`. A global sweep
is a defect.

> Note: the daemon still forks (the launcher, 04 §3.2) and the daemon's own
> launcher child is reaped by the supervisor's `HostLifecycle`, not by the
> daemon. Inside the daemon, only tool children exist, and they belong to
> `process.cpp`.

### 3.4 Exit/error codes (D8)

```cpp
// include/ymh/agent/workspace_runtime.hpp  (additive, D8)
enum class WorkspaceRuntimeErrorCode : std::uint8_t {
    WorkspaceMissing,
    StoreUnavailable,
    WorkspaceBusy,        // NEW: sessions.lock EWOULDBLOCK (02 §5.1)
    ProviderSetupFailed,
    Internal,
};
```

Mapping (pinned):

| `WorkspaceRuntimeErrorCode` | `HostExitCode` (04:145) | `HostErrorCode` (04:200) |
|---|---|---|
| `WorkspaceMissing` | `WorkspaceMissing` (11) | `WorkspaceMissing` |
| `StoreUnavailable` | `StoreOpenFailed` (12) | `StoreUnavailable` |
| `WorkspaceBusy` | `WorkspaceBusy` (10) | `AlreadyRunning` |
| `ProviderSetupFailed` | `StartupRejected` (16) | `NotServing` |
| `Internal` | `StartupRejected` (16) | `NotServing` |

`WorkspaceRuntime::create` must distinguish the held-flock case from other open
failures, but `SessionPersistence::open` currently throws the **same**
`StoreOpenError` for a held `sessions.lock` and for every other failure
(`session_persistence.cpp:568-609`; the lock case is `:588-591`). Pin a code on
the exception so the seam is observable:

```cpp
// include/ymh/session/errors.hpp  (additive, D8)
enum class StoreOpenErrorCode : std::uint8_t {
    Locked,        // flock(LOCK_EX|LOCK_NB) -> EWOULDBLOCK (02 §5.1)
    Unavailable,   // missing root / mkdir / open / sqlite open / schema
};

class StoreOpenError : public StoreError {
public:
    explicit StoreOpenError(StoreOpenErrorCode code, const std::string& what);
    [[nodiscard]] StoreOpenErrorCode code() const noexcept;
};
```

`open()` throws `StoreOpenError{Locked, …}` on `EWOULDBLOCK` (replacing the bare
throw at `session_persistence.cpp:591`) and `StoreOpenError{Unavailable, …}` on
every other open failure. `WorkspaceRuntime::create` then maps
`code() == StoreOpenErrorCode::Locked` → `WorkspaceBusy`, and every other
`StoreOpenError`/`SchemaVersionError` → `StoreUnavailable`.

Note on the wire: `WorkspaceBusy` carries `HostErrorCode::AlreadyRunning`, which
maps to `RpcCode::InvalidRequest` with `data.kind="AlreadyRunning"` (05 §7.1
`:757`; `protocol.cpp:101-102`) — **not** `AppCode::NotServing` (see §4.4).

### 3.5 Store-opened-exactly-once + injection seam (D9)

`WorkspaceRuntime::create` remains the **only** place `SessionPersistence::open`
is called for a workspace. The store is passed into `Impl` by move
(`workspace_runtime.cpp:144-148`); no second open exists. Add an injection seam
mirroring `provider_factory`:

```cpp
struct WorkspaceRuntimeOptions {
    // ... existing fields ...
    // Test hook: when set, use this store instead of opening the real SQLite
    // file. Must return a non-final SessionStore: `SessionPersistence` is
    // `final` and only `open()`/`openReadOnly()`-constructible, so it cannot be
    // faked (session_persistence.hpp:71-74). The caller owns the store and must
    // keep it alive for the runtime's lifetime. Never set in production
    // (H6: one store/one flock).
    std::function<std::unique_ptr<SessionStore>()> store_factory;
};
```

`WorkspaceRuntime::store()` is widened from `SessionPersistence&` to
`SessionStore&` (`workspace_runtime.hpp:101`); the runtime still holds the
concrete `SessionPersistence` internally when it opened one itself, and the
transport needs only the `SessionStore` seam (`session.hpp:94-123`).

- **E9 — Exactly once.** In production `store_factory` is unset and the store is
  opened once, after `chdir`, before `claimHost` (04 §3.3 steps 4/8).
- The injection seam is the M2 equivalent of the M1 in-memory `SessionStore` fake
  (01 §15.1) but at the `WorkspaceRuntime` boundary, so the daemon integration
  tests can run `foreground == true` without touching a real DB.

### 3.6 Provider ordering + null policy (D10)

- **E10 — Provider before bind.** The provider is constructed (factory or
  registry) before the socket is bound. If construction fails or yields null, the
  daemon exits `HostExitCode::StartupRejected` **without binding**. This preserves
  04 §3.3's "bind precedes claim" (`:1362-1364`) while adding "provider precedes
  bind" — a published claim must point at a daemon that can actually serve a
  turn.
- **Provider-null policy:** a null provider is a **startup failure**, never a
  deferred runtime error. `WorkspaceRuntime::create` currently falls through to
  the registry when the factory returns null (`workspace_runtime.cpp:132-138`);
  if both yield null, that is `ProviderSetupFailed` → `StartupRejected`.
  `WorkspaceRuntime::provider()` may still be null only when the runtime was
  built with a store-only test seam and `attach_permission_gate == false`; the
  daemon path never allows it.

---

## 4. `HostRuntime : protocol::TransportHost` (amends 04 §3.7, 05 §7)

### 4.1 Defect (D11)

The adapter that binds the ~30 `protocol::TransportHost` virtuals
(`include/ymh/transport/host.hpp:57-96`) to `WorkspaceRuntime` +
`WorkspaceRegistry` + `SessionManager` + `AgentRegistry` is unspecified. Without
it, every daemon handler is ad hoc and the cursor/error seams drift.

### 4.2 Pinned class sketch

```cpp
// include/ymh/agent/host_runtime.hpp   (new; daemon-owned)
namespace ymh {

struct HostIdentity {
    WorkspaceId  workspace;
    HostBootId   boot_id;
    HostPid      pid{0};
    std::filesystem::path socket_path;
};

class HostRuntime final : public protocol::TransportHost {
public:
    HostRuntime(WorkspaceRuntime& runtime,
                WorkspaceRegistry& registry,          // read-mostly; junction writes
                HostIdentity identity,
                protocol::ProtocolServer& server,      // for cursor + notice fan-out
                TurnExecutor& turns,
                PermissionBroker& broker);
    ~HostRuntime() override;

    // ---- TransportHost overrides (host.hpp:57-96) --------------------------
    protocol::HostState hostState() const override;
    protocol::HostStatusInfo hostStatus() const override;
    void requestShutdown(std::string reason) override;

    std::vector<protocol::WorkspaceSummary> listWorkspaces() override;
    protocol::WorkspaceDetail showWorkspace(const protocol::WorkspaceId&) override;

    std::vector<protocol::SessionSummary> listSessions() override;
    protocol::SessionDetail showSession(const protocol::SessionId&) override;

    protocol::SessionCreated createSession(const nlohmann::json& params) override;
    protocol::SessionResumed resumeSession(const protocol::SessionId&) override;
    protocol::SessionCreated forkSession(const protocol::SessionId&, std::int64_t seed) override;
    void closeSession(const protocol::SessionId&) override;
    void deleteSession(const protocol::SessionId&) override;
    void activateSession(const protocol::SessionId&) override;
    void suspendSession(const protocol::SessionId&) override;

    void agentPrompt(const protocol::SessionId&, const nlohmann::json&) override;
    void agentFollowup(const protocol::SessionId&, const nlohmann::json&) override;
    void agentSteer(const protocol::SessionId&, const nlohmann::json&) override;
    void agentInject(const protocol::SessionId&, const nlohmann::json&) override;
    bool agentCancel(const protocol::SessionId&, const std::optional<std::string>&) override;
    std::string agentStatus(const protocol::SessionId&) override;

    bool decidePermission(const std::string& request_id,
                          protocol::PermissionAnswer, protocol::PermissionScope) override;

    bool sessionExists(const protocol::SessionId&) const override;
    std::vector<EventRecord> readEvents(const protocol::SessionId&, Sequence after,
                                        std::size_t limit) override;
    Sequence headSequence(const protocol::SessionId&) const override;
    std::optional<Sequence> resolveCursor(const protocol::SessionId&,
                                          const protocol::EventCursor&) const override;
    protocol::EventCursor cursorFor(const protocol::SessionId&, Sequence) const override;

private:
    // D11: typed mapping helpers (see §4.4).
    [[noreturn]] static void throw_rpc(protocol::AppCode code, std::string kind,
                                       nlohmann::json data = {});
    [[noreturn]] static void throw_rpc(protocol::RpcCode code, std::string kind,
                                       nlohmann::json data = {});

    WorkspaceRuntime&          runtime_;
    WorkspaceRegistry&         registry_;
    HostIdentity               identity_;
    protocol::ProtocolServer&  server_;
    TurnExecutor&              turns_;
    PermissionBroker&          broker_;
};

} // namespace ymh
```

### 4.3 Method → domain mapping

| `TransportHost` virtual | Domain call | Notes |
|---|---|---|
| `hostState` | `WorkspaceHost` state (Serving/Draining/Stopped) | daemon-owned |
| `hostStatus` | identity + `activeSession()` + `attachedClients()` | 04 §4.2 |
| `requestShutdown` | `WorkspaceHost::requestShutdown(ShutdownReason)` | reply-before-stop, §11 |
| `listWorkspaces` | `WorkspaceRegistry::listWorkspaces()` (read, no flock) | `WorkspaceSummary` |
| `showWorkspace` | `findById` + store session counts | `UnknownWorkspace` if absent |
| `listSessions` | `SessionManager::list()` joined with `registry.listSessions()` for ordinal/archived | order = junction (§5.3) |
| `showSession` | `store().load(id)` + `deriveMessages` | `UnknownSession` |
| `createSession` | `AgentRegistry::create(SessionOptions)` + `registry.addSession` | junction write (§5.3) |
| `resumeSession` | `AgentRegistry::resume(id)`; reopen Idle | 04 §3.3 step 6 |
| `forkSession` | `SessionManager::forkSession(parent, seed)` | `InvalidForkBoundary` |
| `closeSession` | `SessionManager::closeSession` (no `SessionEnded`) | 01 I16 |
| `deleteSession` | `AgentRegistry::dispose` + `SessionManager::deleteSession` + `registry.removeSession` | junction write |
| `activateSession` | `AgentRegistry::activateSession` on `TurnExecutor` | never cancels (§12) |
| `suspendSession` | `AgentRegistry::suspendSession` (explicit only) | cancels `"superseded"` |
| `agentPrompt` | activate-if-idle, then `Agent::prompt` on `TurnExecutor` | §12.3 |
| `agentFollowup` / `agentSteer` / `agentInject` | `Agent` inbox methods on `TurnExecutor` | bounded inbox |
| `agentCancel` | `Agent::cancel` | returns whether a turn was in flight |
| `agentStatus` | `AgentLoop::status()` → `"Idle"`/`"Running"` | 01 §9.8 |
| `decidePermission` | `PermissionBroker::onDecision` | first-wins (§7) |
| `sessionExists` | `store().load(id).has_value()` | read-only |
| `readEvents` | bounded store read (§5) | never unbounded |
| `headSequence` | `store().headSequence(id)` (§5) | |
| `resolveCursor` | store-derived, §4.3.1 | `nullopt` ⇒ `CursorInvalid` |
| `cursorFor` | store-derived, §4.3.1 | |

#### 4.3.1 Cursor mapping (D12)

The wire never carries a numeric `Sequence` (05 T5, `05-transport.md:1229-1230`).
The cursor is a **store-derived, daemon-instance-independent** position (05 §5.5,
`:681-691`). Pin:

```text
cursorFor(session, seq):
    token := base64url( "s1:" + session.value + ":" + decimal(seq) )
    return EventCursor{token}                 # opaque; no Sequence on the wire

resolveCursor(session, cursor):
    parse token
      malformed                          -> nullopt            # CursorInvalid
      embedded session != session        -> nullopt            # CursorInvalid
      seq < 0                            -> nullopt
      head := try headSequence(session)  # 05 §5.5 :686-688
      head absent (UnknownSession thrown) -> nullopt           # deleted session:
                                                               # CursorInvalid
      seq > head                         -> clamp to head      # a position past the
                                                               # head is "after all",
                                                               # not invalid
      otherwise                          -> seq
```

`headSequence` throws `UnknownSession` when the id is not in the store (§5.2), so
`resolveCursor` must **catch it and return `nullopt`** — never let it escape.
A cursor for a deleted session is `CursorInvalid` (fail to `beginning`), not a
`UnknownSession` error (05 §5.5 `:686-688`; M-F8).

- **E11 — `CursorInvalid` ⇒ `beginning`, never `now`.** On `nullopt`, the
  transport raises `AppCode::CursorInvalid`; the supervisor must re-subscribe
  with `from: beginning` (05 §5.5 `:688-691`, §8.5 `:1164-1166`). The daemon
  never silently resets to `now`.
- Cursors survive daemon restart (05 T23): the token names a store position, so
  `host_boot_id` is irrelevant to it (`:684-685`).
- The prefix `s1:` is a cursor **format version**; a future change is a new
  prefix and the old prefix resolves to `nullopt` (fail to `beginning`).

#### 4.3.2 Bounded `readEvents` (D13)

`readEvents(session, after, limit)` caps `limit` at the daemon's replay batch
(05 §7.7 uses `kReplayBatch = 256`, `protocol_server.cpp:19`). `HostRuntime`
clamps `limit` to `[1, 256]` and reads **only** through the bounded store seam
(§5.1). An unbounded read is a defect: it can exceed `max_frame_bytes` and
violate the backpressure contract (05 §8.3).

### 4.4 Typed error mapping (D14)

Every domain failure is converted to exactly one `RpcCode`|`AppCode` with a stable
`data.kind` (05 §7.1, `:753-771`; §2.2, `:256-259`). The `HostErrorCode` half is
already pinned at `05-transport.md:756-771`; this errata adds the `AgentErrorCode`
and store/registry rows.

| Domain error | Wire code | `data.kind` |
|---|---|---|
| `AgentErrorCode::UnknownSession` | `AppCode::UnknownSession` | `UnknownSession` |
| `AgentErrorCode::LeaseHeldByOther` / `LeaseLost` | `AppCode::LeaseLost` | `LeaseLost` |
| `AgentErrorCode::StoreUnavailable` | `AppCode::StoreUnavailable` | `StoreUnavailable` |
| `AgentErrorCode::InboxFull` | `RpcCode::InternalError` | `InboxFull` |
| `AgentErrorCode::AgentDisposed` | `AppCode::SessionNotActive` | `AgentDisposed` |
| `AgentErrorCode::StepLimitExceeded` / `CompactionFailed` / `ContextAssemblyFailed` | `RpcCode::InternalError` | the enum name |
| `AgentErrorCode::ProviderFailed` | `RpcCode::InternalError` | `ProviderFailed` |
| `AgentErrorCode::Cancelled` | (result, not error) | `Cancelled` |
| `UnknownSession` (store, thrown; `session.hpp:79`) | `AppCode::UnknownSession` | `UnknownSession` |
| `LeaseLost` (`errors.hpp:21`) | `AppCode::LeaseLost` | `LeaseLost` |
| `InvalidForkBoundary` (`session.hpp:73`) | `AppCode::InvalidForkBoundary` | `InvalidForkBoundary` |
| `DependentSessionError` (`errors.hpp:41`) | `AppCode::DependentSession` | `DependentSession` |
| `PayloadTooLarge` (`errors.hpp:36`) | `AppCode::PayloadTooLarge` | `PayloadTooLarge` |
| `CorruptionError` (`errors.hpp:26`) | `AppCode::StoreUnavailable` | `StoreUnavailable` |
| `RegistryErrorCode::UnknownWorkspace` | `AppCode::UnknownWorkspace` | `UnknownWorkspace` |
| `RegistryErrorCode::OpenFailed` / `Corrupt` / `SchemaVersion` / `LockUnavailable` | `AppCode::RegistryUnavailable` | `RegistryUnavailable` |
| `RegistryErrorCode::HostClaimed` / `MutationInProgress` | `RpcCode::InternalError` | the enum name |
| `WorkspaceRuntimeErrorCode::WorkspaceBusy` | `RpcCode::InvalidRequest` | `AlreadyRunning` |
| `WorkspaceRuntimeErrorCode::WorkspaceMissing` | `AppCode::UnknownWorkspace` | `UnknownWorkspace` |
| frame > `max_frame_bytes` | `AppCode::FrameTooLarge` | `FrameTooLarge` |
| profile-gated method | `AppCode::MethodNotAllowedForProfile` | the method name |
| subscription cap | `AppCode::SubscriptionLimit` | `SubscriptionLimit` |
| bad params | `RpcCode::InvalidParams` | `InvalidParams` |
| unknown method | `RpcCode::MethodNotFound` | the method name |
| malformed frame / JSON | `RpcCode::ParseError` | `ParseError` |
| pre-hello method | `AppCode::HandshakeRequired` | `HandshakeRequired` |

Notes on the codes: `InternalError` exists only under `RpcCode`
(`protocol.hpp:140-146`), not `AppCode` (`protocol.hpp:149-169`), so every
"internal" row above uses `RpcCode::InternalError`. `WorkspaceBusy` is
`RpcCode::InvalidRequest` with `data.kind="AlreadyRunning"` (05 §7.1 `:757`;
`protocol.cpp:101-102`). M1 has no `StoreError::X` enumerators: store failures
are **exception classes** caught by concrete type (`errors.hpp:11-44`,
`session.hpp:73-82`), so those rows name the class, not an enum member.
`StoreOpenError` is startup-only and never reaches the wire (05 §7.1
`:773-777`).

**Invariant E12 — No raw leak.** `message` is human-readable and never contains a
raw `errno`, SQLite string, or filesystem path; `data.kind` is the stable token
(05 §2.2, `:256-259`).

---

## 5. Store seams (amends 01 §7, 02 §4.2; additive)

### 5.1 Defect (D15)

`SessionStore` (`include/ymh/session/session.hpp:94-123`; spec 01 §7,
`01-session.md:835-856`) has `read(id, after)` and `readRange`, but **no**
`headSequence` and **no bounded** `readAfter`. The transport needs both: the
daemon's `TransportHost::headSequence` (`host.hpp:92`) and cursor clamping
(§4.3.1) require the head, and bounded replay requires a limit.

### 5.2 Pinned additions

```cpp
// include/ymh/session/session.hpp  (SessionStore, additive)
inline constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

class SessionStore {
public:
    // ... existing pure virtuals unchanged ...

    // D15: highest committed Sequence for `id`; 0 when the session has no
    // events. Throws UnknownSession when the id is not in the store.
    // Default derivation keeps existing implementers compiling; the durable
    // store overrides it with an indexed MAX(seq).
    [[nodiscard]] virtual Sequence headSequence(SessionId id) const {
        if (!load(id).has_value()) {
            throw UnknownSession("unknown session");
        }
        Sequence head = 0;
        for (const EventRecord& record : read(id, 0)) {
            head = std::max(head, record.seq);
        }
        return head;
    }

    // D15: at most `limit` committed events with seq > after, ascending.
    // limit == 0 returns empty. Implementations must stop reading at `limit`
    // (no full-table scan then truncate).
    // Default derivation keeps existing implementers compiling; the durable
    // store overrides it with `LIMIT ?`.
    [[nodiscard]] virtual EventRange readAfter(SessionId id, Sequence after,
                                               std::size_t limit) const {
        EventRange all = read(id, after);
        if (limit < all.size()) {
            all.resize(limit);
        }
        return all;
    }
};
```

`SessionPersistence` (`session_persistence.hpp:71-110`) overrides both:

```cpp
[[nodiscard]] Sequence   headSequence(SessionId id) const override;
[[nodiscard]] EventRange readAfter(SessionId id, Sequence after,
                                   std::size_t limit) const override;
```

`readAfter` is the bounded form of the existing `read`; the SQL adds `LIMIT ?`
and uses the existing `(session_id, seq)` index. `read(id, after)` stays for
back-compat and is defined as `readAfter(id, after, kUnbounded)` internally, but
the **transport never calls it**.

The two virtuals are **not** pure: adding them as pure would break the two M1
fakes — `MemorySessionStore` (`tests/support/test_env.hpp:63`) and `FakeStore`
(`tests/unit/session_test.cpp:35`) — plus every other `SessionStore` in tests
(`MemorySessionStore` is reused by `tests/support/agent_test_env.hpp:107`). The
defaults above keep them compiling unchanged; a test that exercises the bounded
path overrides `readAfter` to assert the `limit` stop, and one that exercises
`headSequence` may override it to avoid the scan. `SessionPersistence` is the
only production implementation and overrides both.

### 5.3 Junction writes + ordering (D16)

- **`session.create`** — after `SessionManager::createSession` commits the header
  to the workspace store, the daemon writes the junction row:
  `registry.addSession(workspace, session)` (03 §4.2 `:228`). The store write and
  the junction write are **not** one transaction (different DBs); the junction is
  the authoritative open-set ordering, so a crash between them is reconciled by
  `WorkspaceRegistry::resolvePendingMutations` under the D22 lock (03 §8). Pin:
  the daemon writes the junction **after** the store commit and, if the junction
  write fails, raises `RegistryUnavailable` but leaves the session valid in the
  store (it will be imported on the next reconcile).
- **`session.delete`** — the daemon erases the store row then
  `registry.removeSession(workspace, session)`. Same crash window; same recovery.
- **`session.list`** — the order is the **junction order** (`ordinal` ascending,
  03 §4.2 `:201`, R8), never the store's `createdAt` and never lexical id. Sessions
  present in the store but absent from the junction (crash window) are surfaced
  after the junction-ordered set, sorted by `createdAt`, and flagged for
  reconcile. This makes `session.list` stable across a crash.

---

## 6. Boot nonce (amends 02 §4.1, 03 §6.1, 05 §2.1; D17)

### 6.1 Defect

`make_boot_id()` (`src/cli/wiring.cpp:43-48`) returns
`pid + "-" + steady_clock + "-" + counter`, minted **per call**. It is not a
UUIDv4, not minted once, and is not threaded to both `PersistenceConfig::boot_id`
and `claimHost`. The three mirrored types (`ymh::BootId` 02 §2.1,
`ymh::HostBootId` 03 §2.1, `protocol::HostBootId` 05 §2.1) are duplicated with no
conversion at the seams.

### 6.2 Pinned boot nonce

```cpp
// src/cli/wiring.cpp — replace make_boot_id() (D17)
namespace ymh {

// Minted EXACTLY ONCE per process at daemon startup (04 §3.3 step 3), after
// chdir, before store open. UUIDv4 (03 §9.7 / 02 §5.2).
[[nodiscard]] BootId mint_boot_id();

// One-way adapters at the seams; never mint in a conversion.
[[nodiscard]] HostBootId to_host_boot_id(const BootId&);
[[nodiscard]] BootId to_boot_id(const HostBootId&);
[[nodiscard]] protocol::HostBootId to_protocol_boot_id(const HostBootId&);

} // namespace ymh
```

- **E13 — Mint once.** The value is stored in `HostIdentity.boot_id` and read by
  reference everywhere else. No component calls `mint_boot_id()` a second time.
- Threading (pinned). `HostIdentity.boot_id` is a `HostBootId` (registry type,
  §4.2) but `WorkspaceRuntimeOptions::boot_id` is a `BootId` (store type,
  `workspace_runtime.hpp:68`), so the conversion is explicit:
  `HostIdentity.boot_id` → `to_boot_id(...)` → `WorkspaceRuntimeOptions::boot_id`
  → `PersistenceConfig::boot_id` (02 §4.1, `:409`), **and**
  `HostIdentity.boot_id` → `HostClaim.bootId` → `registry.claimHost`
  (03 §6.2, `:584-591`) with no conversion (`HostClaim::bootId` is already
  `HostBootId`, `registry.hpp:59`).
- `make_boot_id()` is deleted; no caller survives M2.
- **E14 — Three types, three conversions.** `ymh::BootId` (store/lease),
  `ymh::HostBootId` (registry), `protocol::HostBootId` (wire) stay distinct;
  conversions happen only at `to_host_boot_id` / `to_boot_id` /
  `to_protocol_boot_id`. They are wire-identical strings.

---

## 7. Permissions (amends 09 §4.2–§4.4; D18, D19)

### 7.1 Defects

- **D18** — `PermissionBroker` is sketched in 09 §4.2 (`09-permissions.md:614-646`)
  but its threading and the "live Interactive-subscriber count" it needs are
  unpinned. `PermissionGate` (M1, `permission_policy.hpp:181-218`) already
  implements the transport-free core with `resolve(..., subscribers_attached)`.
- **D19** — fail-closed timeout auto-deny, re-broadcast of pending requests on
  attach, and first-wins are stated in prose (09 §4.1 `:584-607`, 05 §7.6
  `:986-995`) but not frozen as one interface. The turn thread must never block
  the transport thread while waiting.

### 7.2 Pinned `PermissionBroker`

```cpp
// include/ymh/policy/permission_broker.hpp   (new; wraps PermissionGate, D18)
namespace ymh {

// 09 §4.2's ctor takes a `Clock&` but never defines the type; M1 only has
// file-local `using Clock = std::chrono::steady_clock;` (e.g.
// permission_policy.cpp:14, process.cpp:26). Pin it as a public type here so the
// timeout is injectable and testable.
using Clock = std::chrono::steady_clock;

class PermissionBroker {
public:
    PermissionBroker(PermissionPolicy& policy,
                     protocol::TransportServer& transport,
                     Clock& clock,
                     PermissionConfig config);

    // Called ONLY for an Ask verdict, from the TURN thread. Registers the
    // request, posts permission.request onto the transport thread, and returns
    // a Task resolved by onDecision / timeout / cancellation. Never throws;
    // failure resolves fail-closed (Deny). (09 §4.2)
    Task<PermissionOutcome> resolve(const PermissionRequest&, CancellationToken);

    // Transport callback, runs on the TRANSPORT thread at the single-threaded
    // dispatch point. First decision wins; later calls are ignored. (09 §4.4)
    void onDecision(const protocol::PermissionDecisionParams&);

    // A newly attached/subscribed Interactive client receives the session's
    // pending requests so a late-attaching supervisor can answer. (09 (o))
    std::vector<protocol::PermissionRequest> pending(SessionId) const;

    std::size_t pendingCount() const noexcept;

    // Live Interactive subscriber count for a session. `ProtocolServer` has no
    // session-scoped count today (only `subscriptionCount(ClientId)` and
    // `attachedClients()`, both reading `connections_` with no lock;
    // protocol_server.hpp:65-69), so pin a new method:
    //   ProtocolServer::sessionSubscriberCount(SessionId) const
    // It counts attached Interactive connections with a subscription to that
    // session, and carries the same single-thread contract as every other
    // ProtocolServer accessor: **io thread only** (protocol_server.hpp:12-14).
    // Because `resolve` runs on the turn thread, the daemon publishes the count
    // as a transport-updated atomic snapshot and injects a thread-safe reader
    // here; the reader must NOT call `sessionSubscriberCount` off the io thread.
    // Zero ⇒ resolve() must not wait: it auto-denies (fail-closed).
    using SubscriberCount = std::function<std::size_t(SessionId)>;
    void set_subscriber_count(SubscriberCount count);
};

} // namespace ymh
```

### 7.3 Rules (frozen)

- **D19.1 — Fail-closed timeout auto-deny.** `PermissionConfig::permission_timeout`
  (default 5 min, `permission_policy.hpp:93`) bounds the wait. On expiry the
  broker resolves `Deny` with reason `"timeout"` and the loop appends the durable
  `payload::PermissionDecision` (09 §4.1 `:604`). Cancellation resolves `Deny`
  with reason `"cancelled"`. Neither throws.
- **D19.2 — First-wins.** `onDecision` applies the first decision atomically at
  the transport dispatch point and ignores later ones (05 §7.6 `:994-995`); an
  unknown/expired `request_id` is `InvalidParams` (idempotent at the policy
  layer).
- **D19.3 — Live subscriber count.** The count is
  `ProtocolServer::sessionSubscriberCount(SessionId)` (new, §7.2): the number of
  attached Interactive connections subscribed to that session. It is computed on
  the **io thread** and published as an atomic snapshot the daemon reads on the
  turn thread; the injected `SubscriberCount` must be that thread-safe reader,
  not a direct call to the io-thread-confined accessor. When the count is zero
  (headless or all clients detached), `resolve` does not block: it auto-denies
  (F2, 09 §5.2; 05 §7.6 `:986-990`). When `PermissionConfig` is absent, the
  gate's `subscribers_attached == false` path is the same fail-closed policy.
  (`attachedClients()` / `subscriptionCount(ClientId)` are not session-scoped and
  are not used for this decision.)
- **D19.4 — Re-broadcast on attach (broadcast, not targeted).** M1's
  `ProtocolServer::onPermissionRequest` fans a request out to **every** attached
  Interactive client subscribed to the session (`protocol_server.cpp:776-791`);
  there is no per-client send seam. On a new Interactive subscribe the daemon
  calls `pending(session)` and re-issues each pending request through that same
  broadcast path (09 (o); 10 §11.1 `:1128-1150`). A supervisor that already holds
  the request dedupes by `request_id` and by the live `PermissionRequested`
  event id (`UiEventAdapter::apply`, §8.2 D20.6); duplicate delivery is harmless,
  and first-wins (`onDecision`) ignores any later decision. A targeted per-client
  send is deferred; the pinned v1 contract is broadcast + `request_id` dedupe.
  **Trigger seam (pinned, additive).** `handle_subscribe` exposes no host hook
  today (`protocol_server.cpp:488-559`), so the daemon cannot observe a new
  Interactive attach; §7.2 pins only the broker-side `set_subscriber_count`. Pin a
  `ProtocolServer` subscribe observer (additive — no existing method changes):

  ```cpp
  // include/ymh/transport/protocol_server.hpp (new)
  using SubscribeObserver = std::function<void(const SessionId&, ClientId)>;
  void set_subscribe_observer(SubscribeObserver observer);
  ```

  The observer is invoked as `on_subscribed(session, client)`, exactly once at
  the tail of `handle_subscribe`, after the subscription is installed
  (`protocol_server.cpp:558`), and only for `Interactive` connections; **not** on
  the `replay_only` early return (`session.replay`, `:551-557`). Thread contract:
  runs **synchronously on the io/dispatch thread**, inside the `event.subscribe`
  dispatch; it must not block, must not re-enter request dispatch
  (`handle_method`), and must not call `TransportServer::stop()` — enqueue-only
  same-thread callbacks such as `onPermissionRequest` are allowed. The daemon's
  handler calls `broker.pending(session)` and re-issues each request via
  `onPermissionRequest` (`:776-791`). Because the observer fires after the
  `SubscribeResult` and the replay batch are enqueued, the re-broadcast requests
  follow the replay in order.
- **D19.5 — Thread boundary.** `resolve` is called from the **turn thread**; it
  posts the wire request via `TransportServer::post()` (§2.2) and returns a Task
  without blocking. `onDecision` runs on the **transport thread**. The two never
  share a mutex held across a blocking wait; the broker's own mutex guards only
  the pending map and is released before any `post()`. A blocking wait on the
  transport thread is a defect (05 §9 `:1201-1203`, 04 D-F8).

---

## 8. Reconnect (amends 05 §4.4, §8.5; D20)

### 8.1 Defect

05 §4.4/§8.5 describe supersede and cursor resume, but the supervisor-side
protocol is not pinned: how many phases, what `resume_hint` means in v1, where
`ClientInstanceId` lives, how cursors are remembered, and how the attach is
cross-checked against the registry.

### 8.2 Pinned reconnect

- **D20.1 — Two phases.**
  1. **Hello.** `HostConnection::connect` → `handshake(profile, instance)`
     (`host_connection.hpp:35-37`). The daemon supersedes any prior connection
     with the same `ClientInstanceId` (05 §4.4 `:520-528`).
  2. **Per-session subscribe.** For each session the supervisor tracks, it calls
     `event.subscribe{ session, from: cursor(last_processed) }`. There is no bulk
     resume.
- **D20.2 — `resume_hint` unused in v1.** `HelloParams::resume_hint`
  (`protocol.hpp:252`) is accepted and ignored; cursors are per-session and only
  meaningful at `event.subscribe`. Documented as reserved.
- **D20.3 — `ClientInstanceId` persistence.** Minted once per supervisor process
  (UUIDv4) and persisted supervisor-local at
  `<XDG_STATE_HOME:-~/.local/state>/ymh/supervisor.json` as
  `{"client_instance": "..."}`. It is **supervisor-local state, not registry
  state** (03 R1/R12). On restart the same id is reused so supersede works; if
  the file is missing/corrupt, a fresh id is minted (reconnect degrades to a
  clean attach).
- **D20.4 — In-memory per-session cursor.** The supervisor keeps
  `std::map<SessionId, EventCursor>` updated from `StreamNotification::cursor`
  (05 §7.7; `protocol.hpp:286`). It is **not** persisted; after a supervisor
  restart the map is empty and each session re-subscribes from `beginning`.
- **D20.5 — `CursorInvalid` → `beginning`, never `now`.** On
  `AppCode::CursorInvalid` (05 §5.5 `:688-691`) the supervisor drops the cursor
  and re-subscribes `from: beginning`. It never uses `from: now` to recover (that
  would silently lose events).
- **D20.6 — Idempotent apply.** The UI projection keys events by `Event.id`
  (UUIDv4) and ignores an id it has already applied, so a replay that overlaps
  the live tail is harmless (05 T8). `UiEventAdapter::apply` is the single
  idempotency point (10 §5.3).
- **D20.7 — Attach identity cross-check.** After hello, the supervisor asserts
  `HelloResult.workspace == registry_row.id` and
  `HelloResult.boot_id == registry_row.host->bootId` (03 §6.1 `:572-580`). A
  mismatch means it attached to a stale/reused socket: close and re-enter
  `ensureRunning` (04 §6.2). Never bind a `WorkspaceModel` to a mismatched hello.

---

## 9. Claim protocol (amends 03 §5.3, §6.2; D21)

### 9.1 Restatement of 03 §6.2 (unchanged semantics)

The claim is the tuple `(workspace_id, host_pid, host_boot_id, host_socket,
host_heartbeat)` in the `workspaces` row (03 §6.1 `:572-580`). The handshake is:

```text
supervisor (spawn path):  claimHost({ws, child_pid, boot, socket})   # PROVISIONAL
daemon   (after flock):   claimHost({ws, getpid(), boot, socket})    # AUTHORITATIVE
daemon   (every 5 s):     heartbeat(claim)                           # refreshes
daemon   (shutdown):      releaseHost(ws, boot)
supervisor (reap):        reapHost(ws)                               # lock-gated
```

- A supervisor-written claim is **provisional** and never establishes liveness
  by itself; liveness requires the sidecar `sessions.lock` flock (03 R5,
  `:588-591`).
- The daemon's own claim (written after it holds the flock) **supersedes** the
  supervisor's on the first tick.
- **Boot-nonce source (pinned).** 04 §3.3 step 3 mints `HostBootId` *inside* the
  daemon, after `chdir`, before store open (`04-workspace-host-daemon.md:305-310`),
  so the supervisor cannot read it before writing the provisional claim. Pin
  **supervisor mint-and-pass**: the supervisor mints the nonce once at spawn
  (`mint_boot_id`, §6.2), writes it into the provisional `claimHost`, and passes
  it to the daemon (spawn argv/env); 04 §3.3 step 3 then **adopts** the supplied
  nonce instead of minting a second one (amending step 3 by reference; E13
  "mint once"). If no nonce is supplied at spawn, the daemon mints one and its
  authoritative claim supersedes the provisional placeholder; in that fallback the
  supervisor writes a non-empty sentinel (03 §4.2 `:494` requires a non-empty
  `bootId`).
- `releaseHost` matches `(pid, boot_id)` and is a no-op if superseded (04 §3.5
  `:400-403`).

### 9.2 03 §5.3 table clarification

The row-domain table at `03-workspace-registry.md:554-566` already assigns the
supervisor the host columns (`host_pid`/`host_boot_id`/`host_socket`/
`host_heartbeat` via `claimHost`/`releaseHost`/`heartbeat`, line 559) and gives
the daemon only "its own host claim/heartbeat at startup/shutdown" (line 558).
This is a **clarification, not a contradiction**: the phrase "its own host
claim/heartbeat" can be misread as daemon-exclusive, but the host columns are a
**shared, provisional-first domain**. The table is restated below to make that
explicit (03 §5.3 is amended by reference to read):

| Writer | Rows it may mutate | Rows it must never mutate |
|---|---|---|
| `WorkspaceHost` daemon | its `workspace_sessions` rows; the host columns of **its own** workspace via `claimHost`/`heartbeat`/`releaseHost` | other workspaces' junction rows; `canonical_path`; `registry_meta` (except bootstrap if first writer) |
| Supervisor | the host columns of a workspace it is spawning via a **provisional** `claimHost`; `display_title`; `removeWorkspace`; `registerWorkspace`; `reapHost` under the D22 lock | any `workspace_sessions` row of a live workspace |
| CLI `ymh workspace add` | `registerWorkspace` (+ its sessions, under the marker protocol) | host columns of a live workspace |
| Any first writer | `registry_meta` bootstrap keys (§7), written **last** and only under the D22 lock | — |

Two constraints the table must state explicitly:

1. **Provisional ≠ authoritative.** The supervisor's host-column write is a
   spawn-time placeholder; it never asserts liveness and is always superseded by
   the daemon's flock-backed claim (03 §6.2).
2. **`registry_meta['initialized']` is written last** under the D22 lock; the
   "any first writer" row must not be read as license for a lock-free bootstrap
   (03 §7.1).

---

## 10. Entry & browse (amends 10 §2.3, §4.1; D22)

### 10.1 Defect

M1's `run_cli` (`src/cli/cli.cpp:286-314`) dispatches `Tui` straight into an
in-process `WorkspaceRuntime` and `list`/`show` into read-only stores. M2 must
route through the daemon, must not spawn a daemon for read-only browse, and must
not let `ymh run` or an in-process TUI collide with a live daemon's flock.

### 10.2 Pinned entry

```text
ymh                      # no args → TUI
  root := canonicalize(cwd)
  row  := registry.openReadOnly().findByCanonicalPath(root)
  if row absent: row := registerWorkspace(root)        # under D22 lock
  attach := HostLifecycle::ensureRunning(row.id)        # 04 §6.2
  UiModel.attach(row.id, attach.connection)             # 10 §4.1
  run_tui()

ymh run "<task>"
  row := findByCanonicalPath(cwd)
  if row live (sidecar lock held):                       # D22
      conn := ensureRunning(row.id)                      # attach, do NOT open store
      s := conn.request("session.create", {})            # result carries the SessionId
      conn.request("agent.prompt", {session: s, message: task})
  else:
      run_headless()                                     # in-process, M1 path
```

`agent.prompt` takes a `SessionId` (`host.hpp:79`; 05 §7.4 `:932`), never a
literal `new`; a fresh session is always created by `session.create` first.

- **E15 — Read-only browse never spawns.** `ymh list` / `ymh show` / `ymh replay`
  open `SessionPersistence::openReadOnly` (`session_persistence.hpp:74`; already
  done at `src/cli/session_cli.cpp:185,229,278`) and `WorkspaceRegistry::openReadOnly`
  (03 §4.3, `registry.hpp:188`). They never call `ensureRunning`, never bind a
  socket, and never take the write lock.
- **E16 — Flock collision is impossible by construction.** `ymh run` (or the
  in-process TUI) opens the store write-lock only when no live daemon holds the
  sidecar flock. If a live daemon is detected, the command **attaches** instead.
  If it nonetheless loses the flock race, `WorkspaceRuntime::create` returns
  `WorkspaceBusy` (§3.4) and the command exits `HostExitCode::WorkspaceBusy`.
  The store is not untouched: `SessionPersistence::open` necessarily opens the
  sidecar lock and attempts `flock` (`session_persistence.cpp:586-591`), but the
  attempt fails on `EWOULDBLOCK` before any session read/write, takes no lease,
  and modifies no session row.
- **E17 — No path resolution in the supervisor.** `cwd` is display-only; the
  supervisor never `chdir()`s and never resolves tool paths (10 U7,
  `10-supervisor-tui.md:1265`; 00 §9.7).

### 10.3 Browse (D22, D26)

`ymh workspace list` / the switcher overlay read
`WorkspaceRegistry::openReadOnly().listWorkspaces()` (03 §4.3) — no daemon, no
flock. `WorkspaceModel` is built from the registry row plus, when attached, the
hello identity (§8.2 D20.7).

---

## 11. Lifecycle & destruction ordering (amends 04 §3.5; D23)

### 11.1 Defect

No component pins who owns whom, or the order of `host.shutdown` reply vs. socket
stop vs. `HostNotice` emission. Getting this wrong means a shutdown reply is
written to a closed socket, or a `DaemonShuttingDown` notice is lost.

### 11.2 Pinned ownership

Construction order (outermost first) and destruction order (reverse):

```text
WorkspaceRuntime                 # owns store, env, bus, sessions, agents, tools, provider
  └─ HostRuntime : TransportHost # adapter; holds refs to runtime + registry
       └─ ProtocolServer         # fan-out, dispatch, cursors
            └─ TransportServer   # acceptor, io thread, socket
```

- **E18 — Lifetimes.** `WorkspaceRuntime` outlives `HostRuntime`; `HostRuntime`
  outlives `ProtocolServer`; `ProtocolServer` outlives `TransportServer`. This is
  already documented for `TransportServer` (`transport_server.hpp:10`); the
  errata extends it up the stack. All are owned by `WorkspaceHost::Impl`, which
  declares them in the order above so C++ destroys them in reverse.
- `TurnExecutor` is joined (`drain`) **before** `TransportServer::stop()`, so no
  turn thread can call `post()` after the transport is gone.

### 11.3 `host.shutdown` reply-before-stop (D23)

The RPC dispatch already auto-replies: it calls `host_.requestShutdown(reason)`
and then `respond(...)` on the io thread (`protocol_server.cpp:347-355`). The
handler therefore must **not** call `TransportServer::stop()` itself: `stop()`
posts a close lambda onto `io_` and then joins its own runner thread
(`transport_server.cpp:191-206`), which self-joins (deadlock) when invoked from
the io thread. Pin a **non-io shutdown coordinator** (the daemon's main thread, or
a dedicated joinable thread that is not the io runner) to own steps 2–5:

```text
on host.shutdown (io thread, inside dispatch):
  state := Draining
  1. requestShutdown(reason) -> arms the coordinator; returns
     # the dispatch's automatic respond(result={accepted:true}) is written next
     # (protocol_server.cpp:352); the handler does not respond itself
  2. coordinator (non-io): transport.post([server]{ server->onDaemonShuttingDown(...) })
     # marshalled via post() (E2); queued behind the dispatch, so the notice is
     # enqueued after the reply. Idempotent: dispatch already emitted it
     # (protocol_server.cpp:353), so this second call is a no-op (E21)
  3. coordinator: wait until reply + notice are flushed via
     ProtocolServer::waitForDrain(shutdown_grace) (new; see E19), which returns
     when every client's outbound queue is empty or the grace expires
  4. coordinator: TransportServer::stop()  # close acceptor/sockets, join runner
  5. coordinator: drain TurnExecutor; store.close(); releaseHost; unlink socket
     (04 §3.5)
```

- **E19 — Reply before stop, off the io thread.** The automatic shutdown reply
  and the `DaemonShuttingDown` notice are enqueued before the transport is
  stopped; the coordinator waits for the outbound queue to drain (bounded by
  `shutdown_grace`, 04 §3.5 `:396-399`) so the client observes a clean stop, not
  an EOF. `TransportServer::stop()` runs **only** on the coordinator (never on the
  io thread), so the runner join cannot self-deadlock.
  **Drain primitive (pinned, additive).** The existing accessors are per-client
  and io-thread-confined (`outstandingBytes(ClientId)`, `attachedClients()`;
  `protocol_server.hpp:65-70`), so the coordinator cannot poll them off the io
  thread. Pin:

  ```cpp
  // include/ymh/transport/protocol_server.hpp (new)
  // Blocks the calling thread until every attached client's outbound queue is
  // empty, or `grace` elapses. Returns true iff fully drained.
  bool waitForDrain(std::chrono::milliseconds grace);
  ```

  Thread contract: **callable from the coordinator (non-io) thread**. Implemented
  over an atomic aggregate of outstanding bytes plus a condition variable
  signalled from `pump()` / `onFrameWritten()` on the io thread; it never reads
  `connections_` off the io thread and never blocks the io thread. On `false`
  (grace expiry) the coordinator force-closes and still completes step 5 (04
  D-F16).
- **E20 — Notice sequencing.** `HostNotice{DaemonShuttingDown}` is emitted
  **after** the reply is enqueued and **before** `stop()`; `SessionClosed` /
  `LeaseLost` notices emitted during drain follow it (05 §5.3 `:605-637`).
- **E21 — Single `DaemonShuttingDown` emission (dedupe, not removal).** Dispatch
  already emits the notice on the io thread (`protocol_server.cpp:353`) and
  coordinator step 2 re-emits it, so a client would see it twice. **Pinned:** keep
  the in-dispatch emission and make `onDaemonShuttingDown` idempotent with an
  io-thread-only `bool shutdown_notice_emitted_{false}` guard (set and read only
  on the io thread — both call sites run there, so no lock). The in-dispatch
  emission is the effective one: it runs before `close_when_drained` is set
  (`:354`), whereas the coordinator's `post()` can lose the race against the
  async reply write completing and `maybe_close_after_drain` (`:620-627`)
  dropping the connection. Removing the in-dispatch emission (`:353`) is
  therefore rejected; dedupe preserves exactly-once delivery.
- If the queue cannot drain within `shutdown_grace`, the coordinator force-closes
  and still completes step 5 (04 D-F16).

---

## 12. Misc errata (D24, D25, D26)

### 12.1 `.ymh/` gitignore (D24)

- The daemon creates `<workspace>/.ymh/` (mode 0700) holding `sessions.db`,
  `sessions.db-wal`, `sessions.lock`, `host.sock`, `host.log` (02 §4.3; 04 §5.1).
  These must never be committed.
- **Pinned:** on first daemon startup (and in `registerWorkspace`), write
  `<workspace>/.ymh/.gitignore` containing exactly:

  ```gitignore
  *
  ```

  A self-ignoring directory keeps the user's root `.gitignore` untouched and
  covers every future file in `.ymh/`. This is additive to the repo's own
  `.gitignore` (which must list `build/` and `.ymh/` for the ymh repo itself).

### 12.2 `host.log` path and redaction (D24)

- Path: `<workspace>/.ymh/host.log`, mode 0600, opened under `umask(0o077)`
  (04 §4.1 `:464`, decision (k) `:1381-1384`). The daemon's stdio sink is the log
  (04 decision (c)).
- **Redaction:** the daemon never logs full prompts or raw tool output unless
  `config.logging.log_prompts` is set (AGENTS.md logging rule; 00 §40). All
  provider-derived strings pass through `redact_secrets`
  (`include/ymh/llm/redaction.hpp:14`). The session event log remains the
  authoritative trace.
- Log rotation is **not** in v1; `host.log` is truncated on daemon startup and
  the file descriptor is held for the daemon's life.

### 12.3 `agent.prompt` on an inactive session (D25)

- **Pinned:** `agent.prompt` on an existing but **Idle/Suspended** session
  triggers activation, then delivers the prompt. It does **not** return
  `SessionNotActive`. `SessionNotActive` is reserved for unknown, deleted, or
  disposed sessions.
- The activation runs on `TurnExecutor` (§3.2), never on the transport thread.

### 12.4 `activateSession` must not cancel in-flight turns (D25)

- `AgentRegistry::activateSession` (`src/agent/agent_registry.cpp:181-190`) calls
  `AgentLoop::activate()`, which already returns early when `running_` or
  `state_ == Error` (`agent_loop.cpp:149-152`) — so it does **not** cancel.
  Preserve this.
- **Pinned:** activation is gated on `AgentStatus::Idle` **or** a blocked
  `AgentState` (`WaitingForPermission`, `WaitingForInput`). If the session is
  `Running` (`Thinking`/`CallingTool`), activation is a no-op. Only an explicit
  `session.suspend` or `agent.cancel` cancels a turn (`"superseded"`/`"user"`).
- The single-active-session arbiter (04 §3.7 `:440-446`) starts the next session
  with pending work when the active one finishes or **blocks** — it never
  suspends a running turn to switch.

### 12.5 Reap TOCTOU re-probe (D26)

- `WorkspaceRegistry::reapHost` (`registry.hpp:225`) must **re-probe** the sidecar
  lock **after** acquiring the D22 write lock and immediately before clearing the
  claim. Between `ensureRunning`'s read and the write lock, the workspace may have
  become live; re-probing under the lock closes the TOCTOU window (03 R5/R11,
  04 §7.3).
- **Pinned:** `reapHost(WorkspaceId)` (M1 returns `bool`, `registry.hpp:225`)
  clears only when, under the lock,
  `probeWorkspaceLock(canonical_path).held == false` (M1 `WorkspaceLockProbe`,
  `liveness.hpp:20-31`) **and** the recorded `host_boot_id` differs from any live
  holder. A held lock aborts the reap and `reapHost` returns `false`; a cleared
  claim returns `true`. M1 has no `ReapResult` type
  (`HostLiveness::{Absent,Live,Stale}` is the read-only observation enum,
  `registry.hpp:97-101`); if a richer result is wanted, pin it explicitly rather
  than referencing a nonexistent `ReapResult::Live`. Never signal a process
  (04 decision (h), `:1373-1375`).

### 12.6 Bootstrap discovery must not block the TUI (D26)

- `WorkspaceRegistry::bootstrap` walks `config.workspace_roots` (03 §7.1) and can
  be slow on a large `$HOME/prjs`. The supervisor must not block first paint on it.
- **Pinned:** the TUI starts with the registry's current rows and shows a
  provisional/empty tree; bootstrap (and `importSessionsFromDisk`) runs on a
  worker and delivers results via a UI event (10 §3.3 threading model). Discovery
  failures surface as a banner, never a hang.

### 12.7 Supervisor multi-workspace model refactor (D26)

- `UiModel` must move from a single `WorkspaceModel` (M1, 10 §2.3 `:191-195`) to
  `std::map<WorkspaceId, WorkspaceModel> workspaces` (10 §4.1 `:336`) with one
  `HostConnection` per attached daemon (10 §1.4 `:120`).
- Browse uses `WorkspaceRegistry::openReadOnly`; attaching uses
  `HostLifecycle::ensureRunning`; each `WorkspaceModel` carries its own
  `daemonStatus` (10 U-F2 `:1344`). The aggregate status line sums across all
  attached workspaces (10 §6.1).
- The refactor is mechanical by design (10 §2.3 `:194-195`): the
  `WorkspaceModel`/`SessionUiState` split is already in place, so no projection
  logic changes — only the cardinality of the container and the event source.

---

## 13. Defect traceability (D1–D26 → section → wave)

Waves: **W0** interface freeze/prereqs · **W1A** transport hardening · **W1B**
daemon loop/lifecycle · **W1C** store seams + boot nonce · **W1D** `HostRuntime`
adapter · **W1E** permissions · **W1F** reconnect/claim/entry · **W2–W6**
follow-on (activation, reap, bootstrap, multi-workspace, verification).

| D | Summary | Section | Wave |
|---|---|---|---|
| D1 | `TransportServer::post()`/`io()` missing; fan-out thread race | §2.2 | W1A |
| D2 | `stop()` unlinks a successor's socket (no inode check) | §2.3 | W1A |
| D3 | `start()` clobbers socket without connect-probe | §2.3 | W1A |
| D4 | `ProtocolServerConfig::uid` defaults 0 | §2.2 | W1A |
| D5 | No ping/idle keepalive | §2.4 | W1A |
| D6 | Agent turn runs on the io loop (no bounded worker) | §3.2 | W1B |
| D7 | Global SIGCHLD reaper conflicts with `process.cpp` | §3.3 | W1B |
| D8 | `WorkspaceRuntimeErrorCode::WorkspaceBusy` missing; exit/error map | §3.4 | W0 |
| D9 | Store-opened-exactly-once + injection seam | §3.5 | W1C |
| D10 | Provider ordering before bind + null policy | §3.6 | W1B |
| D11 | `HostRuntime` adapter unspecified | §4.2–§4.3 | W1D |
| D12 | Cursor mapping (`resolveCursor`/`cursorFor`) unpinned | §4.3.1 | W1D |
| D13 | `readEvents` unbounded | §4.3.2 | W1D |
| D14 | Typed error mapping incomplete | §4.4 | W0 |
| D15 | `SessionStore::headSequence` + bounded `readAfter` missing | §5.2 | W1C |
| D16 | Junction writes + `session.list` order unpinned | §5.3 | W1C |
| D17 | Boot nonce not UUIDv4-once; not threaded to both sinks | §6 | W0 |
| D18 | Async `PermissionBroker` interface unpinned | §7.2 | W1E |
| D19 | Fail-closed timeout / first-wins / re-broadcast / thread boundary | §7.3 | W1E |
| D20 | Reconnect two-phase / cursor / identity cross-check unpinned | §8 | W1F |
| D21 | Claim protocol restatement + 03 §5.3 table clarification | §9 | W0 |
| D22 | M2 entry flow + read-only browse + flock collision | §10 | W1F |
| D23 | Destruction ordering + shutdown reply-before-stop | §11 | W1B |
| D24 | `.ymh/` gitignore + `host.log` path/redaction | §12.1–§12.2 | W2 |
| D25 | `agent.prompt` activation + `activateSession` no-cancel | §12.3–§12.4 | W2 |
| D26 | Reap TOCTOU + non-blocking bootstrap + multi-workspace refactor | §12.5–§12.7 | W2 |

Wave exit gates:

- **W0** — this errata verified; no open HIGH/MEDIUM. Blocks everything.
- **W1A–W1F** — each wave lands with its unit/integration tests green (05 §13,
  04 §13) and no warning suppression.
- **W2–W6** — behaviour waves (activation, reap, bootstrap, multi-workspace,
  live PTY/live-LLM verification) gated on W1 completion.

---

## 14. Invariants (E1–E20)

| # | Invariant | § |
|---|---|---|
| E1 | One transport runner thread; `post()` never starts one | §2.2 |
| E2 | `ProtocolServer` fan-out only on the transport io thread | §2.2 |
| E3 | `ProtocolServerConfig::uid == getuid()` | §2.2 |
| E4 | `stop()` unlinks only its own inode | §2.3 |
| E5 | Ping/pong never changes state or gates work | §2.4 |
| E6 | Agent turns run off the io loop, on the bounded executor | §3.2 |
| E7 | Executor workers bounded by `max_llm_concurrency` | §3.2 |
| E8 | One `waitpid` owner per child (`process.cpp`) | §3.3 |
| E9 | Store opened exactly once per daemon | §3.5 |
| E10 | Provider constructed before bind; null ⇒ startup failure | §3.6 |
| E11 | `CursorInvalid` ⇒ re-subscribe `beginning`, never `now` | §4.3.1 |
| E12 | Wire errors carry `data.kind`; never a raw errno/SQLite string | §4.4 |
| E13 | Boot nonce minted once | §6.2 |
| E14 | Three boot-id types; conversions only at the two adapters | §6.2 |
| E15 | Read-only browse never spawns a daemon | §10.2 |
| E16 | `ymh run`/TUI never collide with a live daemon's flock | §10.2 |
| E17 | Supervisor never `chdir()`s or resolves tool paths | §10.2 |
| E18 | Destruction order: runtime ⊃ adapter ⊃ protocol ⊃ transport | §11.2 |
| E19 | `host.shutdown` reply and notice flush before `stop()`; `stop()` off the io thread | §11.3 |
| E20 | `DaemonShuttingDown` precedes `SessionClosed`/`LeaseLost` notices | §11.3 |

## 15. Failure modes (M-F1–M-F12)

| # | Failure | Detection | Handling |
|---|---|---|---|
| M-F1 | Fan-out callback runs on the turn thread | TSan/assert executor id | `post()` marshal (E2) |
| M-F2 | `stop()` unlinks a successor socket | inode mismatch at `stop()` | skip unlink (E4) |
| M-F3 | `start()` clobbers a live socket | connect succeeds | throw `AlreadyRunning` |
| M-F4 | Turn blocks the io loop | heartbeat/accept latency | bounded executor (E6) |
| M-F5 | Global reaper steals a tool child's status | `waitpid` returns ECHILD | specific-pid only (E8) |
| M-F6 | Second daemon binds the same workspace | flock `EWOULDBLOCK` | `WorkspaceBusy`, attach to winner |
| M-F7 | Provider null but daemon bound | startup check | `StartupRejected`, no bind (E10) |
| M-F8 | Cursor from a deleted session | `resolveCursor` ⇒ `nullopt` | `CursorInvalid` ⇒ `beginning` (E11) |
| M-F9 | Reconnect loses/duplicates events | cursor gap/dup | per-session cursor + id-keyed apply (§8) |
| M-F10 | Permission wait blocks the loop | no subscriber | auto-deny, off-loop `resolve` (§7.3) |
| M-F11 | Shutdown reply lost to a closed socket | EOF before reply | reply-before-stop + drain (E19) |
| M-F12 | Bootstrap blocks first paint | slow `$HOME` scan | worker + provisional tree (§12.6) |

---

## 16. Test plan (additions)

- **Unit.** `TransportServer::post` ordering/after-stop drop; connect-probe
  (`AlreadyRunning`/stale/`SocketUnavailable`); inode-checked unlink; uid
  enforcement; `headSequence`/`readAfter` bounds; cursor round-trip and
  `CursorInvalid`; boot-nonce mint-once; error-mapping table (every row of §4.4);
  `PermissionBroker` first-wins + timeout + zero-subscriber auto-deny.
- **Integration (fake host / Fake LLM).** `foreground == true` daemon with an
  injected store; turn on the executor while accept/heartbeat stay live;
  reconnect supersede + per-session resume; `host.shutdown` reply-before-stop;
  claim provisional→authoritative; `session.list` junction order across a
  simulated crash window.
- **Golden/replay.** Multi-workspace `UiModel` render; browse with a read-only
  registry (no daemon); idempotent apply on a replayed overlap.
- **Failure-mode matrix.** One test per M-F1–M-F12.
- **Live (opt-in, `YMH_LIVE_LLM=1`).** Supervisor + daemon over the real socket
  under a PTY; permission round-trip; reconnect after a TUI restart.

---

## 17. Open questions

- **OQ-1 — `host.ping` cadence under load.** 10 s ping / 30 s idle is pinned;
  whether a busy stream should suppress the idle timer entirely is deferred.
- **OQ-2 — Cursor format stability.** `s1:` is a version prefix; a future format
  must resolve old prefixes to `CursorInvalid` (fail to `beginning`).
- **OQ-3 — `host.log` location.** `<workspace>/.ymh/host.log` is v1; a
  state-dir alternative (`<state>/ymh/workspaces/<id>/host.log`) remains open
  (04 OQ-5).
- **OQ-4 — Multi-supervisor fan-out.** Broadcast + first-wins is pinned for v1;
  per-supervisor policy is deferred (05 OQ-4).
- **OQ-5 — Bootstrap incrementality.** A resumed/partial discovery walk is
  deferred; v1 is one non-blocking full walk.

---

## 18. References

- `00-architecture.md` §8.2, §9.6, §9.7, §9.10, §57 Step 13, §58.
- `01-session.md` §4.6, §7, §9.8; `02-persistence.md` §2.1, §4.1, §4.3, §5.
- `03-workspace-registry.md` §2.1, §4.2, §4.3, §5.3, §6.1, §6.2, §7.1, R5/R11/R16.
- `04-workspace-host-daemon.md` §2.1, §2.2, §3.3, §3.5, §3.6, §3.7, §4.2, §5.2,
  §5.3, §6.2, §9, decisions (d)/(e)/(i)/(k)/(m).
- `05-transport.md` §2.2, §3.1–§3.3, §4.2, §4.4, §4.5, §5.3, §5.5, §7.1, §7.6,
  §7.7, §8.5, §9, T5/T8/T12/T23.
- `06-agent-loop.md` §4.1, A1/A2; `07-tools-execution.md` §5.4, X8.
- `09-permissions.md` §4.1–§4.4, §5.2, Q1/Q3/Q15.
- `10-supervisor-tui.md` §1.4, §2.3, §3.3, §4.1, §5.3, §6.1, §9.2, §11.1, U7.
- Code: `include/ymh/transport/*`, `include/ymh/agent/workspace_runtime.hpp`,
  `include/ymh/registry/registry.hpp`, `include/ymh/session/*`,
  `include/ymh/policy/permission_policy.hpp`, `src/cli/wiring.cpp`,
  `src/transport/transport_server.cpp`, `src/agent/agent_registry.cpp`,
  `src/execution/process.cpp`.

# 76 - Host Startup Residue Recovery Errata

```
Status: verified (Rev 3)
Revision: 3
Verification status: verified (Rev 3) -- adversarial reviewer (independent,
           Rev 3): GATE PASS (0 HIGH / 0 MEDIUM, 2 non-blocking LOW); Oracle
           anchor/architecture confirm PASS. DESIGN_STATUS.md row 76 states the
           same Status/Revision per AGENTS.md and the `design_status_drift` ctest.
Component: 76 (errata) -- bounds every blocking step on the bare-`ymh` startup
           path and reclaims a stale/foreign `host.sock` (a socket that does not
           authenticate as this workspace's daemon), so `ymh` never hangs before
           the TUI renders and a user never has to delete `<ws>/.ymh/`. Amends
           04-workspace-host-daemon.md (sec 5.2, sec 6.2, sec 7.2),
           05-transport.md (HostConnection.connect / request; a ProtocolServer
           workspace accessor), 11-m2-errata.md (sec 3.4 D8; sec 1 D8/D17
           cross-cutting freeze `:53`) and 16-daemon-ownership.md
           (sec 1.3.1 / sec 3.2) by reference.
Depends on: 04-workspace-host-daemon.md (verified), 05-transport.md (verified),
            11-m2-errata.md (verified; frozen M2 interfaces),
            16-daemon-ownership.md (verified; supervisor-owned daemons),
            69-host-startup-diagnosability-errata.md (verified; the
            failure-reporting channel this errata composes with)
Supersedes: (a) 04 sec 6.2 `ensureRunning` held-lock algorithm
            (`04-workspace-host-daemon.md:741-748`) -- the bare
            "HeldBy => try attach; on held lock never spawn, surface
            HostUnreachable" is replaced by a bounded, authenticated
            corroboration with two new specific outcomes (76-D4/76-D5);
            (b) 04 sec 5.2 / H7 stale-socket replace rule
            (`04-workspace-host-daemon.md:1018-1020`) -- the raw
            `connect(2)` probe (`src/transport/transport_server.cpp:219-234`) is
            replaced by an authenticated one (76-D6);
            (c) 16 sec 1.3.1 / sec 3.2 `OwnershipMark::Unreachable = live claim
            but handshake failed (stale socket)` (`16-daemon-ownership.md:701`) --
            made operational: `Unreachable` becomes a classified, actionable
            outcome (76-D5) instead of intended-but-unbuilt.
            It does NOT supersede 16's ownership model (O1..O22); it preserves
            04 H10/H11 and 16 O11 ("never kill a live daemon"). It does NOT
            supersede 69: 69 owns the failure-*reporting* channel, 76 only bounds
            the *time* before a report is possible, and reports through 69's
            channel.
Scope: supervisor attach liveness (bounded `connect`, bounded `send`), the
       daemon-side stale-socket reclaim probe, the classification of a held
       sidecar lock, and the per-artifact residue-reclamation decision. It does
       NOT design `/fork`, `/rewind`, `/handoff`, the dashboard, session-open
       latency (item C of the design brief), or any wire/persistence/schema
       change.
```

## 1. Purpose and root cause

The user report is: a bare `ymh` (no args) **hangs before the TUI appears**,
and the only reliable recovery is `rm -rf ./.ymh/`. Two coupled mechanisms
produce this. Both are reachable from the synchronous bare-`ymh` startup at
`src/cli/cli.cpp:522` (`lifecycle.ensureRunning(row->id, identity)`).

### 1.1 B1a -- the supervisor blocks forever

`HostLifecycle::ensureRunning` (`src/host/workspace_host.cpp:1613-1632`) takes
the `Live` branch whenever the sidecar `flock` on `<ws>/.ymh/sessions.lock` is
held by **any** process (`host_liveness` is `held ? Live : Stale`,
`src/registry/registry.cpp:918-926`; the lock itself at
`src/registry/liveness.cpp:16-53`). The `Live` branch goes straight to
`connect_checked` (`workspace_host.cpp:1626-1628`, function at `:382-397`) with
no corroboration and no reap.

`connect_checked` calls `HostConnection::connect`, whose `::connect(2)` at
`src/transport/host_connection.cpp:56` runs on a **blocking** `AF_UNIX`
`SOCK_STREAM` socket with **no timeout**: for a Unix stream socket, `connect`
stalls when the listen backlog is full; a foreign listener with a full backlog,
or a stopped peer, is exactly that. (The inherited-listener orphan path is
already closed by the baseline `CLOEXEC` guard at `transport_server.cpp:241-251`;
see sec 1.2 for the residue that remains.)
After connect, the mandatory `host.hello` is sent by
`HostConnection::request`, but `request` computes its deadline **after**
`send_message` (`host_connection.cpp:160` then `:162`), and `send_message`
itself loops on a blocking `::send` with no deadline (`:64-81`). A peer with a
full receive buffer therefore stalls the supervisor too. The 10 s readiness
deadline in `spawnAndAttach` (`workspace_host.cpp:1539-1543`) cannot fire
because a blocking `connect`/`send` never returns to the loop that checks it.

### 1.2 B1b -- the permanent "delete `.ymh`" state

A **replacement** daemon reaches `TransportServer::start`
(`src/transport/transport_server.cpp:208-234`) only after it already holds the
sidecar flock (sec 1.3). Its stale-socket probe does an `O_NONBLOCK` connect
(`:215-220`) and treats `connected == 0 || errno == EINPROGRESS || errno == EAGAIN`
as "another daemon is live" (`:223-226`) and **refuses to unlink**. In that state
the replacement daemon exits `AlreadyRunning` -> `WorkspaceBusy = 10`
(`include/ymh/host/workspace_host.hpp:60`); the supervisor's 5 s winner loop
(`workspace_host.cpp:1594-1608`) finds no claim and throws `HostUnreachable`
(`:1609-1610`). Nothing ever unlinks the socket, so every subsequent start
repeats this. This is the user's exact "must delete `./.ymh/`" signature.

The reachable residue is **not** a dead ymh daemon: a dead daemon's listener fd
is closed, and its fresh socket answers `ECONNREFUSED`, which the code already
unlinks (`:227-228`). The baseline `CLOEXEC` guard on the listener
(`transport_server.cpp:241-251`, added 2026-09-26) closes the inherited-listener
orphan path entirely. The reachable residue is a socket that stays connectable
because a **live non-ymh listener** holds it (or a socket left by a process that
never held the workspace flock -- 04 sec 5.2 `:623-627` calls this the
defense-in-depth case). Such a socket accepts `connect` but never answers a valid
`host.hello`; the raw probe cannot tell it apart from a ymh daemon.

On an `O_NONBLOCK` `AF_UNIX` connect, `EINPROGRESS` means the kernel has not
answered yet and `EAGAIN`/`EWOULDBLOCK` means the listen backlog is full -- i.e.
a listener **is** present, but nothing proves it is this workspace's daemon.
Treating either as "a live daemon" without a handshake is the drift: spec 16
already anticipated the correct semantics in its `OwnershipMark::Unreachable`
state (`16-daemon-ownership.md:701`, "live claim but handshake failed (stale
socket)").

### 1.3 The load-bearing ordering fact

The daemon acquires the sidecar `flock` **before** it binds the socket: the
startup order is `chdir` (`workspace_host.cpp:786`) -> state dir (`:797`) ->
`WorkspaceRuntime::create`, which opens the session store and takes
`LOCK_EX` on `sessions.lock` (`:853`; store open at
`src/session/session_persistence.cpp:764-779`) -> `transport_->start()`
(`workspace_host.cpp:921`) -> `claimHost` (`:938`). Therefore **at bind time
the process already holds the workspace flock**, so an existing connectable
socket cannot be a live daemon *for this workspace* (a live daemon would hold
the lock and this process's store-open would have failed `WorkspaceBusy`).
This makes an authenticated reclaim-at-bind safe and is the foundation of
76-D6.

### 1.4 The "live unrelated lock holder" case

The sidecar lock is an ordinary `flock`, so a **live, unrelated** process can
hold it (e.g. `flock .ymh/sessions.lock -c ...`). In that state a replacement
daemon cannot acquire the flock, and no amount of respawning can succeed. The
current code has no deterministic outcome for this: `ensureRunning` conflates
it with `Live` and blocks in `connect`. 76-D5 pins a specific, actionable
outcome that never kills and never loops.

## 2. Scope boundaries

In scope:

- bounding `HostConnection::connect` and `HostConnection::send_message`, and
  ordering `request`'s deadline before the first send (B1a);
- corroborating `Live` with a bounded authenticated `host.hello` and mapping a
  failure to a specific error (B1a, the live-unrelated-lock case);
- authenticating the daemon-side stale-socket reclaim probe (B1b);
- the per-artifact residue-reclamation decision (what is provably stale; what
  must be preserved);
- a wall-clock bound for the bare-`ymh` synchronous attach.

Out of scope (owned elsewhere): the text/presentation of startup failures (69);
session-open latency, `context.show` coalescing, cursor seeding (brief item C,
proposed spec 77); `/fork` (78), `/rewind` (79), file checkpoints (80),
dashboard (81), `/handoff` (82).

## 3. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 76-A1 | 05 `HostConnection::connect` | `src/transport/host_connection.cpp:41-62` | `connect` gains a `timeout` parameter; the socket is created `SOCK_NONBLOCK`; `connect(2)` is followed by `poll(POLLOUT)` (covering `EINPROGRESS`/`EAGAIN`/`EWOULDBLOCK`) to the deadline and a mandatory `getsockopt(SO_ERROR)` (76-D1). |
| 76-A2 | 05 `HostConnection::send_message` / `request` / receive path | `src/transport/host_connection.cpp:64-81`, `:83-134`, `:156-162` | `send_message` takes a `deadline`; `request` computes the deadline **before** `send_message`; `wait_readable`/`fill_incoming` handle `POLLERR`/`POLLHUP`/`POLLNVAL` and `read` `EAGAIN` (76-D2, 76-D12). |
| 76-A3 | 04 sec 6.2 `ensureRunning` held-lock algorithm | `src/host/workspace_host.cpp:1613-1632` | the `Live` branch becomes a bounded, authenticated `probeLive` with outcomes `Attached` / `IdentityMismatch` / `OurDaemonUnresponsive` / `ForeignLockHolder` / `NoHolder` (76-D4/76-D5). |
| 76-A4 | 04 sec 4.2 / sec 6.2 `HostLifecycle` interface | `include/ymh/host/host_launcher.hpp:114-142` | `ensureRunning` gains a defaulted `const AttachBudget&`; new `probeLive` private member; `spawnAndAttach` gains `const AttachBudget& budget` + a `deadline`, and the file-local `connect_checked` gains `const AttachBudget& budget` + a `deadline` (its `budget.connect`/`budget.handshake` fields are consumed there; 76-D3/76-D4/76-M5-fix). |
| 76-A5 | 04 sec 5.2 stale-socket replace rule | `src/transport/transport_server.cpp:208-234` | the raw connect-probe is replaced by `probe_existing_socket(socket_path_, server_.workspace(), deadline)`, which performs a bounded connect + authenticated `host.hello` (76-D6). |
| 76-A6 | 05 / 04 sec 2.2 error taxonomy | `include/ymh/transport/protocol.hpp:94-106` | add `HostErrorCode::HostUnresponsive` and `HostErrorCode::WorkspaceLockForeign` (76-D8). |
| 76-A7 | 11 sec 3.4 D8 exit/error codes; sec 1 cross-cutting D8/D17 freeze | `11-m2-errata.md:320-341` (D8 mapping table), `:53` (`D17`/`D8` cross-cutting) | record the two additive `HostErrorCode` members and the probe; no JSON-RPC wire code, exit code, or schema changes (76-D8, 76-D10). |
| 76-A8 | 16 sec 1.3.1 / sec 3.2 | `16-daemon-ownership.md:701` | `OwnershipMark::Unreachable` is operationalized by `ForeignLockHolder` / `OurDaemonUnresponsive`; 16's ownership model and O11 are retained unchanged. |
| 76-A9 | 05 `ProtocolServer` | `include/ymh/transport/protocol_server.hpp:55-88`, `:179` | add a read-only `workspace()` accessor (`return config_.workspace;`) so the daemon-side probe can name the workspace (76-D6; closes gate H1). |

## 4. The recovery model

```text
bare `ymh`  (src/cli/cli.cpp:465, :522)
  |
  v
ensureRunning(ws, identity, AttachBudget{total=25s})
  |  deadline := now + budget.total                        (76-I4)
  |  row := registry.findById(ws)
  |  no row                        -> throw WorkspaceMissing
  |  no claim                      -> spawnAndAttach(bounded)
  |  claim present:
  |     probeLive(row, identity, budget, deadline)          (76-D4)
  |       fresh probeWorkspaceLock(ws):
  |         flock free             -> NoHolder  -> reapIfStale + spawnAndAttach
  |         flock held:
  |           bounded connect_checked (budget.connect / budget.handshake)
  |             hello matches claim-> Attached  -> attach
  |             hello mismatch     -> IdentityMismatch -> throw AttachRejected
  |             timeout / no peer  -> classify by diagnostic + killHint:
  |                diag boot == claim boot and NOT Dead
  |                                -> OurDaemonUnresponsive -> throw HostUnresponsive
  |                otherwise       -> ForeignLockHolder      -> throw WorkspaceLockForeign
  |
  spawnAndAttach(bounded):
    spawn; readiness loop <= budget.spawn_readiness (reap each tick)
    on WorkspaceBusy: winner loop <= budget.winner; each connect_checked bounded
    exhausted -> throw HostUnreachable "startup exceeded <T> ms during <phase>"

replacement daemon bind  (src/transport/transport_server.cpp:202)
  |  sidecar flock already held (workspace_host.cpp:853 before :921)
  v
probe_existing_socket(socket_path_, server_.workspace(), deadline)   (76-D6)
  absent                         -> Free       -> bind
  connect ECONNREFUSED           -> Refused    -> unlink -> bind
  connected, hello names this ws -> LiveYmhDaemon -> AlreadyRunning
  connected, no valid hello      -> Residue    -> unlink -> bind
```

## 5. Decision register

**76-D1 -- `HostConnection::connect` is bounded and non-blocking.**
`connect` creates the socket `SOCK_CLOEXEC | SOCK_NONBLOCK`, calls `::connect`,
and on `EINPROGRESS`, `EAGAIN`, or `EWOULDBLOCK` waits with `poll(POLLOUT)`
until the deadline (on a non-blocking `AF_UNIX` connect a full accept backlog
returns `EAGAIN`, the exact B1a backlog case); on readiness it **must** read
`getsockopt(SO_ERROR)` and treat a nonzero value as failure (Oracle case 2). On
deadline it throws `connection_error("connect: timed out after <n> ms")`; on
refusal it throws `connection_error("connect: <strerror(SO_ERROR)>")`. The
`O_NONBLOCK` flag is kept for the connection's lifetime so `send_message` can
poll-wait (76-D2), and the receive path is likewise made `EAGAIN`-safe (76-D12);
it is not persisted. New constant `protocol::kHostConnectTimeout = 5000 ms`
(`include/ymh/transport/protocol.hpp`), used as the default and by the daemon
probe. Bounds B1a's connect.

**76-D2 -- Every send has a deadline set before the first byte.** `request`
computes `deadline = steady_clock::now() + timeout` **before** calling
`send_message`, and passes the deadline in; `send_message` becomes a
non-blocking loop that `poll(POLLOUT)`s the remaining time before each
`::send`, handling `EAGAIN`/`EINTR` and throwing on deadline or `POLLERR` /
`POLLHUP` (Oracle case 1). This covers the handshake `request` from
`handshake()` (`host_connection.cpp:150`) and every later request. The existing
`handshake` default timeout (5 s) is unchanged; `request`'s default (30 s) is
unchanged. Bounds B1a's send. The receive path on the same non-blocking fd is
covered by 76-D12.

**76-D12 -- The receive path is `EAGAIN`-safe on the non-blocking fd.** Because
76-D1 keeps `O_NONBLOCK` for the connection's lifetime, `wait_readable`
(`host_connection.cpp:83-98`) checks `revents`, and `fill_incoming` (`:100-134`)
treats a `read(2)` returning `EAGAIN` / `EWOULDBLOCK` as "retry within the
remaining deadline" rather than a hard `connection_error`.
**Precedence: readable before hangup.** A peer may half-close immediately after
writing a final frame, so one `poll` can report `POLLIN | POLLHUP`. `POLLIN` is
therefore serviced first: the pending bytes are drained and any complete frame
is delivered. `POLLHUP` / `POLLERR` / `POLLNVAL` is treated as terminal **only**
after the readable data has been drained -- i.e. a `read(2)` returning 0, or the
error surfacing when no frame is pending. Erroring on `POLLHUP` before reading
would lose the final frame; the old blocking path drains the socket before
treating EOF. Without this, a spurious `POLLIN` would abort a request with
`"read: Resource temporarily unavailable"` (`protocol.hpp` `HostUnreachable`
path) on a connection the old blocking code served. No new symbol; the two
functions gain an `errno` branch and the precedence ordering.

**76-D3 -- The bare-`ymh` synchronous attach has a wall-clock budget.**
New value type `AttachBudget` (defaulted) bounds the whole `ensureRunning`
call: `connect` 5 s, `handshake` 5 s, `spawn_readiness` 10 s (existing),
`winner` 5 s (existing), `total` 25 s. `ensureRunning` computes an overall
`deadline = steady_clock::now() + budget.total` and threads it into the
sub-steps, whose signatures change (76-A4) so the budget actually reaches them:

- `connect_checked(socket_path, workspace, boot_id, identity, budget, deadline)`
  -- the two `AttachBudget` per-step fields are consumed here (they would
  otherwise be dead): it calls `HostConnection::connect` with
  `min(budget.connect, remaining_until(deadline))` and then `handshake` with
  `min(budget.handshake, remaining_until(deadline))`
  (`workspace_host.cpp:382-397`). `probeLive` and `spawnAndAttach` both already
  hold `budget` and pass it through.
- `spawnAndAttach(record, identity, budget, deadline)` -- replaces the
  hardcoded `seconds{10}` readiness deadline (`workspace_host.cpp:1539-1540`)
  and `seconds{5}` winner deadline (`:1594-1595`) with
  `min(budget.spawn_readiness, remaining_until(deadline))` and
  `min(budget.winner, remaining_until(deadline))`.

On exhaustion it throws
`HostError(HostUnreachable, "startup exceeded <total> ms during <phase>: <detail>")`.
`cli.cpp:522` passes the budget explicitly (concrete caller);
`src/ui/supervisor.cpp`'s background `ensure_worker_loop` (`:1488-1550`, call
at `:1512`) and `run_via_daemon` (`src/cli/cli.cpp:566`) use the default.
Bounds every blocking step with an explicit deadline (Oracle case 1's "every
path that can block"). Without the `connect_checked`/`spawnAndAttach`
signature changes the `total` ceiling could be overshot by the sub-steps'
fixed defaults.

**76-D4 -- `Live` is corroborated by a bounded authenticated `host.hello`.**
New private member `HostLifecycle::probeLive(record, identity, budget,
deadline)` re-runs `probeWorkspaceLock` fresh, then, when the lock is held,
attempts `connect_checked(socket_path, workspace, boot_id, identity, budget,
deadline)` (per-step caps derived from `budget`). It
attaches (`Attached`) **only** when the returned `HelloResult` names the claim's
workspace and boot nonce (the check already at `workspace_host.cpp:391-395`).
A hello that answers but names a different workspace/boot is `IdentityMismatch`
and is rethrown as the existing `HostError(AttachRejected)` (04 D20.7
preserved). A connect/handshake timeout or failure is classified in 76-D5. A
free lock is `NoHolder` (the claim is stale; the existing `reapIfStale` ->
`spawnAndAttach` path runs). `probeLive` never signals, never reaps, never
spawns.

**76-D5 -- A held sidecar lock with no answering daemon has a deterministic
outcome; the fix never kills.** When `probeLive` cannot attach, it classifies
the holder using the self-reported lock diagnostic (`WorkspaceLockProbe::pid`,
`WorkspaceLockProbe::bootId`, `src/registry/liveness.hpp:20-24`) and
`killHint(pid)` (`src/registry/liveness.cpp:55-63`, which gains its first
production caller here):

- `diag bootId == record.host->bootId.value` **and** `killHint(pid) != Dead`
  => `OurDaemonUnresponsive` => throw
  `HostError(HostUnresponsive, ...)` naming the lock path, pid, boot, and the
  elapsed probe deadline. This is the SIGSTOP / wedged-daemon case; the claim
  is not cleared and no second daemon is spawned (04 H10, 16 O11).
- otherwise => `ForeignLockHolder` => throw
  `HostError(WorkspaceLockForeign, ...)` naming the lock path, the observed
  pid/boot (or "none"), the registry claim boot, and the fact that the lock is
  held by a live process that is not this workspace's daemon. This is Oracle
  case 3: respawning cannot flock, so the error is surfaced once, deterministically,
  with no infinite retry and no signal.

`killHint` returns `Alive | Dead | Unknown` (`liveness.hpp:35`); `Unknown`
covers `pid <= 0` and `EPERM` (`liveness.cpp:55-63`). `Unknown` is treated as
**not proven dead** (the `!= Dead` arm above): with a boot-id match it stays
`OurDaemonUnresponsive`, without a match it is `ForeignLockHolder`. `Unknown` is
never a reason to reap or kill; it is a hint, never an authorization (04 H10).

Both branches are terminal for this call: one actionable error, zero blocking,
zero killing. This operationalizes the `OwnershipMark::Unreachable` state
(`16-daemon-ownership.md:701`) without violating 04 H11 / 16 O11.

**76-D6 -- The daemon-side reclaim probe is authenticated.** `TransportServer::start`
replaces the raw `connect == 0 / EINPROGRESS / EAGAIN => AlreadyRunning`
misclassification (`transport_server.cpp:223-226`) with
`probe_existing_socket(path, workspace, deadline)`:

```cpp
enum class SocketProbeResult : std::uint8_t {
    Free,           // no inode at the path (or lstat failed); safe to bind
    Refused,        // connect(2) => ECONNREFUSED; safe to unlink + bind
    Residue,        // connected but no valid ymh hello for this workspace
    LiveYmhDaemon,  // connected and a hello named this workspace
};
```

It performs the bounded non-blocking connect + `SO_ERROR` of 76-D1, then a
bounded `host.hello`; it unlinks only on `Refused` or `Residue`, and never on
`LiveYmhDaemon`. The probe mints a throwaway `ClientInstanceId` and uses
`ServerProfile::Interactive` for its `HelloParams`
(`include/ymh/transport/protocol.hpp:286-292`), and compares the returned
`HelloResult.workspace` (`:294-303`) to the workspace it was given. The
bind-time flock ordering (sec 1.3) makes `Residue` safe: the caller already
holds the sidecar lock, so any connectable node that does not speak for this
workspace is provably residue, not a live daemon. `Residue` names exactly the
sec-1.2 case: a live non-ymh listener or a never-flocked socket that accepts
`connect` but never authenticates. Fixes B1b, the "delete `.ymh`" signature.

**The caller seam (gate H1).** `TransportServer` stores only `server_` and
`socket_path_` (`include/ymh/transport/transport_server.hpp:88-89`) and has no
workspace. `TransportServer::start` therefore calls
`probe_existing_socket(socket_path_, server_.workspace(), deadline)`, where
`ProtocolServer::workspace()` is a new read-only accessor returning
`config_.workspace` (`include/ymh/transport/protocol_server.hpp:179`; the config
already carries the field at `:45`). This accessor is registered in 76-A9 and
sec 6; it is the concrete way the daemon-side probe learns the workspace.

**76-D7 -- Residue reclamation touches exactly one artifact.** Reclamation may
`unlink` only `<ws>/.ymh/host.sock` and only through 76-D6. It never deletes or
truncates `sessions.lock`, `sessions.db`, `host.log`, `permissions.jsonc`, the
`workspaces` registry row, or any `supervisors` row. `sessions.lock` survives
because it is an `O_CREAT` sidecar whose kernel `flock` is already released; the
next daemon reuses the file and overwrites the diagnostic JSON
(`session_persistence.cpp:821-834`). The `workspaces.host_*` claim columns are
cleared only by the existing `reapHost`, whose own guard refuses when the flock
is held (`src/registry/registry.cpp:1187-1208`, guard at `:1193`);
"unreachable-but-claimed" is therefore **never** reaped. Full table in sec 7.

**76-D8 -- Two additive `HostErrorCode` members.** Add
`HostErrorCode::HostUnresponsive` and `HostErrorCode::WorkspaceLockForeign`
(`include/ymh/transport/protocol.hpp:94-106`), with the corresponding
`rpc_code_for_host_error` cases (`src/transport/protocol.cpp:115-137`; declared
`include/ymh/transport/protocol.hpp:207`). Both new members map to
`code_value(AppCode::NotServing)`, exactly as `HostErrorCode::HostUnreachable`
and `HostErrorCode::NotServing` already do (`src/transport/protocol.cpp:125-130`);
the switch is `-Wswitch`-checked under `-Werror` and has **no `default:`**, so
each new member needs its own explicit case. These are in-process error values; no
new JSON-RPC numeric code (they reuse the existing `AppCode::NotServing`), no
exit code, and no on-disk value changes (76-D10).
`HostUnresponsive` is distinct from `HostUnreachable` (which stays the
spawn/winner failure) so the TUI can label a wedged live daemon apart from a
failed spawn.

**76-D9 -- Diagnostics compose with 69; they never bypass it.** Every bounded
failure is thrown as a `HostError` whose `what()` is a single actionable ASCII
line naming the phase and the artifact path; the CLI prints it verbatim and the
TUI renders it through the channel 69 owns
(`docs/design/69-host-startup-diagnosability-errata.md` sec 4/6). 76 adds no
new message transport; it only ensures a message exists within finite time.

**76-D10 -- No new persistence, schema, or wire.** `AttachBudget`, the probe
results, and the per-step deadlines are call-scoped; the `O_NONBLOCK` flag is
connection-scoped (sec 12). No `sessions.db` / `registry.db` column is added; no
schema version changes; no exit code changes. The two enum members are additive.

**76-D11 -- Convergence (idempotence).** After one successful startup following
residue, the residue `host.sock` is gone, so a second bare `ymh` with no live
daemon attaches within `AttachBudget::total` without any manual delete. The
only state the fix mutates on disk is the removal of a provably-residue socket
node.

## 6. New symbols and their callers

Every symbol introduced or changed by this errata names at least one concrete
caller in the tree (AGENTS.md "new symbols are normative").

| Symbol | Declared | Concrete caller(s) |
|---|---|---|
| `protocol::kHostConnectTimeout` | `include/ymh/transport/protocol.hpp` | default arg of `HostConnection::connect` (`host_connection.cpp:41`); `probe_existing_socket` deadline (`transport_server.cpp:208`) |
| `HostConnection::connect(path, timeout)` | `include/ymh/transport/host_connection.hpp:34` | `connect_checked` (`workspace_host.cpp:387`), `connect_to` (`:371`), supervisor attach (`src/ui/supervisor_connection.cpp` attach path), tests |
| `HostConnection::send_message(message, deadline)` | `include/ymh/transport/host_connection.hpp:55` | `HostConnection::request` (`host_connection.cpp:160`) |
| `SocketProbeResult` | `include/ymh/transport/transport_server.hpp` | `probe_existing_socket` |
| `probe_existing_socket(path, ws, deadline)` | `include/ymh/transport/transport_server.hpp` | `TransportServer::start` (`transport_server.cpp:208`), called as `probe_existing_socket(socket_path_, server_.workspace(), deadline)` |
| `ProtocolServer::workspace()` | `include/ymh/transport/protocol_server.hpp:55-88` | `TransportServer::start` (the `server_.workspace()` argument; closes gate H1) |
| `connect_checked(..., budget, deadline)` (changed, file-local) | `src/host/workspace_host.cpp:382-397` | `HostLifecycle::probeLive`, `spawnAndAttach` (`:1569`), winner loop (`:1600`); consumes `budget.connect`/`budget.handshake` |
| `HostLifecycle::spawnAndAttach(record, identity, budget, deadline)` (changed, private) | `include/ymh/host/host_launcher.hpp:136` | `HostLifecycle::ensureRunning` (`workspace_host.cpp:1620`, `:1631`) |
| `AttachBudget` | `include/ymh/host/host_launcher.hpp` | default of `HostLifecycle::ensureRunning`; explicit at `src/cli/cli.cpp:522`; `connect`/`handshake` consumed by `connect_checked`, `spawn_readiness`/`winner`/`total` by `spawnAndAttach`/`ensureRunning` |
| `HostLifecycle::ensureRunning(ws, identity, budget)` | `include/ymh/host/host_launcher.hpp:124` | `src/cli/cli.cpp:522`, `src/ui/supervisor.cpp:1512`, `run_via_daemon` (`src/cli/cli.cpp:566`) |
| `LiveProbeOutcome`, `LiveProbe` | `include/ymh/host/host_launcher.hpp` | `HostLifecycle::probeLive`, consumed by `ensureRunning` |
| `HostLifecycle::probeLive(...)` (private) | `include/ymh/host/host_launcher.hpp` | `HostLifecycle::ensureRunning` (`workspace_host.cpp:1613`) |
| `killHint` (existing; first production caller) | `include/ymh/registry/liveness.hpp:37` | `HostLifecycle::probeLive` classification (76-D5) |
| `HostErrorCode::HostUnresponsive`, `WorkspaceLockForeign` | `include/ymh/transport/protocol.hpp:94-106` | thrown by `HostLifecycle::probeLive` / `ensureRunning`; mapped in `rpc_code_for_host_error` (`src/transport/protocol.cpp:115`) to `AppCode::NotServing` (76-D8) |

## 7. Per-artifact residue and reclamation

| Artifact | What is "stale" | Reclamation decision | Never reclaim when |
|---|---|---|---|
| `<ws>/.ymh/sessions.lock` | the kernel `flock` is free (`probeWorkspaceLock.held == false`); the file itself is not a lock | none. The file persists (`O_CREAT`, `liveness.cpp:19`); the next daemon re-takes `LOCK_EX` and overwrites the diagnostic JSON (`session_persistence.cpp:821-834`). Never unlink: unlinking a lock path is a classic `flock` race. | always preserved as the lock path |
| `<ws>/.ymh/host.sock` | connect succeeds but no valid `host.hello` names this workspace at bind time (76-D6) | `unlink` before bind (only under the bind-time flock held since `workspace_host.cpp:853`) | a hello names this workspace (`LiveYmhDaemon`); a path that does not exist (`Free`) |
| `<ws>/.ymh/host.log` | never reclaimed by liveness logic | preserve; it is 69's diagnosability sink (`workspace_host.cpp:1409-1430`, read at `:253-366`) | always preserved |
| `<ws>/.ymh/sessions.db` (+ leases) | stale in-DB `session_leases` expire on their own TTL (15 s) and are **not** a startup-liveness input (recon-daemon sec 2.7) | preserve; never touched by startup | always preserved |
| `workspaces.host_pid/host_boot_id/host_socket/host_heartbeat` | `probeLiveness == Stale` (flock free) -- the boot is provably dead | clear via existing `reapHost` (`registry.cpp:1187-1208`) | the flock is held (`:1193` guard): "unreachable-but-claimed" is preserved |
| `supervisors` rows | stale by 16's TTL (`registry.cpp:1171-1185`) | out of scope; owned by spec 16 | n/a |
| `registry.lock` | never | preserve | always |

The rule in one line: **only a socket that provably cannot be serving this
workspace is unlinked; a held lock is never seized, and no daemon is ever
signalled.** A dead boot is proven by the kernel-released `flock` (existing
`Stale` -> `reapHost`), never by a PID.

## 8. C++ interface sketches

```cpp
// include/ymh/transport/protocol.hpp (additive; 76-D1/76-D8)
inline constexpr std::chrono::milliseconds kHostConnectTimeout{5000};

enum class HostErrorCode : std::uint8_t {
    // ... existing members unchanged ...
    HostUnresponsive,     // live lock holder that is (probably) our daemon but
                          // did not answer host.hello within the budget (76-D5)
    WorkspaceLockForeign, // the sidecar lock is held by a process that is not
                          // this workspace's daemon (76-D5)
};

// include/ymh/transport/host_connection.hpp (changed; 76-D1/76-D2)
class HostConnection {
public:
    // CHANGED: bounded. SOCK_NONBLOCK + poll(POLLOUT) + getsockopt(SO_ERROR).
    // Throws on deadline ("connect: timed out after <n> ms") and on refusal.
    void connect(const std::string& socket_path,
                 std::chrono::milliseconds timeout = kHostConnectTimeout);

    [[nodiscard]] nlohmann::json request(
        std::string_view method, nlohmann::json params = nlohmann::json::object(),
        std::chrono::milliseconds timeout = std::chrono::seconds{30});
    // ... unchanged public members ...
private:
    // CHANGED: deadline is set by request() BEFORE the first send.
    void send_message(const nlohmann::json& message,
                      std::chrono::steady_clock::time_point deadline);
    // ... unchanged private members ...
};

// include/ymh/transport/transport_server.hpp (additive; 76-D6)
enum class SocketProbeResult : std::uint8_t {
    Free, Refused, Residue, LiveYmhDaemon,
};
// Bounded connect + authenticated host.hello within `deadline`. The probe mints
// a throwaway ClientInstanceId and uses ServerProfile::Interactive
// (HelloParams, protocol.hpp:286-292) and compares HelloResult.workspace
// (protocol.hpp:294-303). Concrete caller: TransportServer::start, via
// probe_existing_socket(socket_path_, server_.workspace(), deadline).
[[nodiscard]] SocketProbeResult probe_existing_socket(
    const std::string& socket_path,
    const protocol::WorkspaceId& workspace,
    std::chrono::steady_clock::time_point deadline);

// include/ymh/transport/protocol_server.hpp (additive; 76-A9, gate H1)
class ProtocolServer {
public:
    // ... existing members unchanged ...
    // Read-only accessor for the configured workspace; caller:
    // TransportServer::start (server_.workspace()).
    [[nodiscard]] const WorkspaceId& workspace() const noexcept { return config_.workspace; }
};

// include/ymh/host/host_launcher.hpp (additive/changed; 76-D3/76-D4)
inline constexpr std::chrono::milliseconds kAttachTotalBudget{25000};

struct AttachBudget {
    std::chrono::milliseconds connect{protocol::kHostConnectTimeout};  // 5 s (76-D1)
    std::chrono::milliseconds handshake{5000};        // matches 05 handshake default
    std::chrono::milliseconds spawn_readiness{10000}; // existing spawnAndAttach
    std::chrono::milliseconds winner{5000};           // existing winner loop
    std::chrono::milliseconds total{kAttachTotalBudget};  // hard ceiling (76-I4)
};

enum class LiveProbeOutcome : std::uint8_t {
    Attached,               // authenticated hello matched the claim
    NoHolder,               // the sidecar lock is free (claim is stale)
    IdentityMismatch,       // hello answered, named a different workspace/boot
    OurDaemonUnresponsive,  // lock diagnostic matches the claim, hello failed
    ForeignLockHolder,      // lock diagnostic missing or names a different boot
};

struct LiveProbe {
    LiveProbeOutcome                          outcome = LiveProbeOutcome::ForeignLockHolder;
    std::unique_ptr<protocol::HostConnection> connection;   // set iff Attached
    std::optional<std::int32_t>               holder_pid;   // diagnostic when known
};

class HostLifecycle {
public:
    // CHANGED: `budget` bounds the whole call (76-D3).
    AttachResult ensureRunning(WorkspaceId workspace, AttachIdentity identity,
                               const AttachBudget& budget = AttachBudget{});
private:
    // CHANGED: takes the budget + the overall deadline (76-D3); the hardcoded
    // 10 s / 5 s loop bounds now derive from `budget` and `remaining_until`.
    AttachResult spawnAndAttach(const WorkspaceRecord& record,
                                const AttachIdentity& identity,
                                const AttachBudget& budget,
                                std::chrono::steady_clock::time_point deadline);
    // 76-D4/76-D5. Never reaps, never signals, never spawns.
    [[nodiscard]] LiveProbe probeLive(const WorkspaceRecord& record,
                                      const AttachIdentity& identity,
                                      const AttachBudget& budget,
                                      std::chrono::steady_clock::time_point deadline);
};

// src/host/workspace_host.cpp (changed, file-local; 76-D3)
// CHANGED: consumes budget.connect for the connect and budget.handshake for
// the hello, each capped by remaining_until(deadline). Callers (probeLive,
// spawnAndAttach) already hold `budget`.
std::unique_ptr<HostConnection> connect_checked(
    const std::filesystem::path& socket_path, const WorkspaceId& workspace,
    const HostBootId& boot_id, const AttachIdentity& identity,
    const AttachBudget& budget,
    std::chrono::steady_clock::time_point deadline);
```

## 9. Invariants

| ID | Invariant |
|---|---|
| **76-I1** | `HostConnection::connect` returns (success or exception) within its `timeout`; it never blocks past the deadline. Success is reported only after `getsockopt(SO_ERROR) == 0`. |
| **76-I2** | On every path that writes to the wire, a request deadline exists **before** the first byte is sent, including the `host.hello` handshake. `send_message` never blocks past that deadline. |
| **76-I3** | After `poll(POLLOUT)` reports writable, the code reads `getsockopt(SO_ERROR)`; a nonzero value is a connect failure, never a success. |
| **76-I4** | Every blocking step reachable from the bare-`ymh` synchronous attach carries an explicit deadline, and `ensureRunning` returns or throws within `AttachBudget::total`. No input reaches the TUI loop with the supervisor blocked in `connect`/`send`/`waitpid`/`poll` without a deadline. |
| **76-I5** | The `Live` branch attaches only after a bounded authenticated `host.hello` naming the claim's workspace and boot. A failed or timed-out hello never spawns a second daemon and never clears the claim (04 H10, 16 O11). |
| **76-I6** | The fix never signals, never reaps, and never clears a claim whose sidecar flock is held. A held lock yields `OurDaemonUnresponsive` or `ForeignLockHolder`, never a kill and never a respawn loop. |
| **76-I7** | A held `sessions.lock` whose diagnostic does not match the claim's `host_boot_id` (or is absent) yields `HostError(WorkspaceLockForeign)`, with the observed pid when available, in finite time -- never an unbounded retry. |
| **76-I8** | At bind time the daemon already holds the sidecar flock; `probe_existing_socket` unlinks `host.sock` only for `Refused`/`Residue` and never for `LiveYmhDaemon`. |
| **76-I9** | Residue reclamation deletes only `host.sock`. It never deletes `sessions.lock`, `sessions.db`, `host.log`, `permissions.jsonc`, a `workspaces` row, or a `supervisors` row. |
| **76-I10** | After one successful startup following residue, a second bare `ymh` with no live daemon attaches within `AttachBudget::total`; no residue state requires a manual `rm -rf .ymh`. |
| **76-I11** | Every bounded-timeout failure names the phase and the artifact path in an ASCII, bounded `HostError::what()`; the text is surfaced through 69's channel, never dropped. |
| **76-I12** | The daemon-side probe is read-only: it issues only `host.hello` and closes its socket. It never sends a state-changing method and never mutates the workspace. |
| **76-I13** | Two daemons never reach bind for one workspace: the sidecar flock serializes them, so the probe can never unlink the socket a live daemon just bound. |
| **76-I14** | On the non-blocking connection fd, `fill_incoming` treats a `read` `EAGAIN`/`EWOULDBLOCK` as retry-within-deadline and services a pending `POLLIN` before treating `POLLHUP`/`POLLERR`/`POLLNVAL` as terminal, so a response-then-close peer's final frame is delivered; a spurious readable never aborts a request with `"read: Resource temporarily unavailable"` (76-D12). |

## 10. Failure modes (`76-F#`)

| 76-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **76-F1** | `connect(2)` stalls on a full accept backlog / foreign listener (B1a) | `poll(POLLOUT)` reaches the deadline; `EINPROGRESS`/`EAGAIN`/`EWOULDBLOCK` all enter the poll | throw `connection_error("connect: timed out after <n> ms")`; the supervisor surfaces a bounded `HostError` (76-I1, 76-I4) |
| **76-F2** | `send()` stalls on a peer with a full receive buffer during `host.hello` (B1a) | deadline set before send; `poll(POLLOUT)` reaches it | throw a bounded write timeout; no infinite block (76-I2) |
| **76-F3** | `poll(POLLOUT)` reports writable but the connect failed | `getsockopt(SO_ERROR) != 0` | treat as connect failure with `strerror(SO_ERROR)`, never as a live peer (76-I3) |
| **76-F4** | Connectable `host.sock` that does not authenticate as this workspace (B1b: a live non-ymh listener or a never-flocked socket) | `probe_existing_socket` returns `Residue` | unlink the socket, bind, publish claim; the user's "delete `.ymh`" is no longer required (76-I8, 76-I10). A dead ymh daemon is *not* this case: its listener is closed (`transport_server.cpp:241-251`) and answers `ECONNREFUSED`, already unlinked (`:227-228`) |
| **76-F5** | `sessions.lock` held by a live unrelated process | lock held, `connect_checked` fails, diagnostic absent/mismatched | throw `HostError(WorkspaceLockForeign)` naming the holder; never spawn, never kill (76-D5, 76-I7) |
| **76-F6** | Our daemon is live but wedged (`SIGSTOP`, stuck turn) | lock held, diagnostic boot matches claim, `killHint != Dead`, hello times out | throw `HostError(HostUnresponsive)`; never spawn a second daemon, never clear the claim, never signal (04 H10, 16 O11, 76-I5/76-I6) |
| **76-F7** | A daemon answers but names a different workspace/boot | `HelloResult.workspace`/`boot_id` mismatch (`workspace_host.cpp:391-395`) | rethrow the existing `HostError(AttachRejected)` (04 D20.7 preserved) |
| **76-F8** | Socket path too long, or the probe socket cannot be created | `socket_path_too_long()` / `socket(2) < 0` | existing `SocketPathTooLong` / `SocketUnavailable`; unchanged |
| **76-F9** | The whole budget is exhausted before a sub-step succeeds | `steady_clock::now() >= deadline` in `ensureRunning` | throw `HostError(HostUnreachable, "startup exceeded <total> ms during <phase>")` (76-D3, 76-I4) |
| **76-F10** | No workspace row / read-only registry | `findById` empty | existing `WorkspaceMissing`; unchanged |
| **76-F11** | Two supervisors race the spawn | child exits `WorkspaceBusy` | existing winner loop (`workspace_host.cpp:1594-1608`); each `connect_checked` bounded (76-D3); no kill |
| **76-F12** | The lock diagnostic JSON is corrupt or absent while the lock is held | `probeWorkspaceLock` parse fails (`liveness.cpp:46-49`) | classify `ForeignLockHolder` (cannot prove the holder is ours); surface the path and pid-when-known; never kill (recorded limitation, sec 14 OQ-1) |
| **76-F13** | An orphan ymh daemon answers `hello` for this workspace but does not hold the lock (invariant violation) | probe returns `LiveYmhDaemon` though the caller holds the lock | refuse to unlink; exit `AlreadyRunning`/`WorkspaceBusy`; bounded, actionable message names the socket path and the peer pid (`HelloResult.pid`, `protocol.hpp:298`) (76-I8, sec 14 OQ-2) |
| **76-F14** | A probe unlinks a socket a new daemon just bound (TOCTOU) | would require two daemons at bind for one workspace | impossible: the sidecar flock serializes bind (76-I13) |
| **76-F15** | A spurious `POLLIN` on the non-blocking fd leads to `read` `EAGAIN` | `wait_readable` reported readable but `read` returns `EAGAIN`/`EWOULDBLOCK` | loop back within the deadline; never abort with `"read: Resource temporarily unavailable"` (76-D12, 76-I14) |
| **76-F16** | A peer writes a final frame then half-closes, so `poll` reports `POLLIN | POLLHUP` | readable + hangup set together | service `POLLIN` first: drain the pending bytes and deliver the complete frame; treat `POLLHUP` as terminal only after that (never drop the final frame) (76-D12, 76-I14, `Err76.ResponseThenClose`) |

## 11. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| request/response timeouts on the control link | `HostConnection::connect`/`send_message` deadlines (76-D1/76-D2) | Mirror of the capability: dsh applies bounded timeouts to control-link calls; ymh previously applied a deadline only to the *receive* half (`host_connection.cpp:162`), leaving connect/send unbounded. Anchor: `04-workspace-host-daemon.md:1150` (service discovery via authenticated handshake). |
| service liveness by authenticated handshake, not by port reachability | `probe_existing_socket` + `probeLive` (76-D4/76-D6) | Mirror: dsh identifies a service by its handshake, never by a bare connect. ymh's own `OwnershipMark::Unreachable` state already says the same (`16-daemon-ownership.md:701`); 76 makes the code match it. |
| reclaim of on-disk socket residue after a crash | `probe_existing_socket` unlink-on-`Residue` (76-D6/76-D7) | Non-mirror: dsh owns an in-process runtime whose service endpoints do not outlive the harness, so it has no crashed-daemon socket node to reclaim. ymh persists a per-workspace Unix socket (04 sec 5.1/5.2, `04-workspace-host-daemon.md:592-627`) that a killed daemon leaves behind, so reclaim is required. Reason is an architectural difference, not an omission. |
| typed error propagation | two additive `HostErrorCode` members (76-D8) | Non-mirror refinement: dsh raises typed errors; ymh additionally distinguishes "our daemon unresponsive" from "foreign lock holder" because its liveness primitive is a kernel `flock` any local process can take (03 R5/R11, `16-daemon-ownership.md:130`). The refinement carries the reason for the divergence (a shared, non-ymh-addressable lock) and a concrete anchor. |

## 12. State-lifetime table

Every state this errata introduces is call-scoped; nothing new is persisted.

| State | Type / where | Created | Destroyed / cleared | Owner | Survives process restart? |
|---|---|---|---|---|---|
| connect deadline | `steady_clock::time_point`, local in `HostConnection::connect` | on call | at call return (stack) | call frame | no |
| `O_NONBLOCK` flag on the connection fd | fd status flag | set in `connect` | on `close()` / fd close | `HostConnection` | no |
| send/request deadline | `steady_clock::time_point`, local in `request` | before `send_message` | at `request` return | call frame | no |
| `AttachBudget` | value parameter | at the caller (cli/supervisor stack) | caller scope | caller | no |
| `LiveProbe` (outcome, holder_pid, connection) | local in `ensureRunning` | on call | moved into `AttachResult` (Attached) or discarded on throw | call frame / `AttachResult` | no |
| probe's transient socket fd | fd | in `probe_existing_socket` | closed before return | probe frame | no |
| daemon-side probe deadline | `steady_clock::time_point`, local in `TransportServer::start` | `now + kHostConnectTimeout` at `start()` | at `start()` return | `start()` frame | no |
| probe throwaway hello identity (`ClientInstanceId`, `ServerProfile`) | value temporaries in `probe_existing_socket` | on call | at probe return | probe frame | no |
| `SocketProbeResult` | local value in `TransportServer::start` | on call | at `start()` return | `start()` frame | no |
| `<ws>/.ymh/host.sock` node | filesystem inode | daemon bind | `unlink` on `Residue`/`Refused` (next start) or on clean `stop()` when the inode matches (`transport_server.cpp:306-312`) | filesystem / next daemon | yes (the node persists until reclaimed) |
| `workspaces.host_*` claim columns | registry rows | existing `claimHost` (`registry.cpp:1061-1087`) | existing `reapHost` only when the flock is free (`:1187-1208`) | spec 16/03 | yes (durable) |
| two `HostErrorCode` members | compile-time enum | n/a | n/a | n/a | n/a (no runtime state) |

## 13. Test plan

Hermetic (no real LLM, no network). New/added files:

- `tests/unit/errata76_connection_timeout_test.cpp` (extends the coverage of
  `tests/unit/transport_socket_test.cpp`);
- `tests/unit/errata76_startup_residue_test.cpp` (uses a fake launcher and a
  real Unix socket peer, no daemon);
- `tests/integration_errata76_residue_recovery_test.cpp` (real `ymh --host`
  daemon via the reusable harness `tests/support/host_harness.hpp`, used by
  `tests/integration_host_harness_test.cpp`; provider satisfaction comes from
  `tests/support/global_test_env.cpp`'s process-wide FakeLLM).

`tests/CMakeLists.txt` lists sources explicitly (no glob), so the three new
files must be added to that list in the same change set.

| Test | Setup | Assertion |
|---|---|---|
| `Err76.ConnectTimesOutOnFullBacklog` | listen on a Unix socket with backlog 1 and a blocked acceptor so the next `connect` stalls; call `HostConnection::connect` with a 200 ms timeout | throws within the timeout with `timed out`; the call does not exceed the deadline (76-I1, 76-F1) |
| `Err76.ConnectReadsSoError` | a socket that reports `POLLOUT` with a nonzero error condition | connect throws the `SO_ERROR` reason, never returns success (76-I3, 76-F3) |
| `Err76.SendTimesOutBeforeHello` | accept a connection but never read; call `handshake` with a small timeout | throws a bounded write timeout; the deadline was set before the send (76-I2, 76-F2) |
| `Err76.ReadEagainOnSpuriousWakeup` | a peer that triggers a readable wakeup with no bytes available, then answers within the deadline | the request completes; it never throws `read: Resource temporarily unavailable` (76-I14, 76-F15) |
| `Err76.ResponseThenClose` | a peer that writes one complete response then immediately `close()`s, so a single `poll` reports `POLLIN | POLLHUP` | the request returns that response; the final frame is not lost and no `peer closed` error is thrown (76-I14, 76-D12, 76-F16) |
| `Err76.EnsureRunningBoundedOnWedge` | fake launcher; a held `sessions.lock` diagnostic matching the claim; a socket peer that accepts but never answers | `ensureRunning` throws `HostUnresponsive` within `AttachBudget::total`; no second spawn; the claim is not cleared (76-I5/76-I6, 76-F6) |
| `Err76.ForeignLockHolderSpecific` | create `<ws>/.ymh/sessions.lock`, take `flock(LOCK_EX)` from the test process, leave the diagnostic JSON absent | `ensureRunning` throws `WorkspaceLockForeign`; no spawn; no signal (76-I7, 76-F5, Oracle case 3) |
| `Err76.StaleSocketReclaimed` | bind a Unix socket at `<ws>/.ymh/host.sock` that accepts but does not answer `host.hello`, with the sidecar lock free | a replacement daemon's `probe_existing_socket` returns `Residue` and unlinks the node, then binds (76-I8, 76-F4) |
| `Err76.LiveHelloNotReclaimed` | a peer that answers `host.hello` naming this workspace | probe returns `LiveYmhDaemon`; the socket is not unlinked (76-I8, 76-F13) |
| `Err76.LiveDaemonNeverKilled` | a live daemon (real harness) attached, then a second `ensureRunning` | the second call attaches; the daemon's pid is unchanged; no `SIGTERM` observed (76-I5/76-I6, spec 16 O11) |
| `Err76.NoDataDeleted` | a workspace with `sessions.db`, `host.log`, `permissions.jsonc`, and a residue `host.sock` | after recovery every file except the residue socket still exists with unchanged contents (76-I9) |
| `Err76.SecondStartConverges` | residue workspace; one successful start, then a second bare-start path | the second start attaches within the budget; `host.sock` is not residue (76-I10, 76-D11) |
| `Err76.BudgetNamesPhase` | fake launcher that never publishes a claim | the thrown `HostError::what()` names the phase and is bounded ASCII (76-I11, 76-F9) |

## 14. Open questions

- **76-OQ1 (LOW).** The foreign-vs-ours classification (76-D5) is heuristic when
  the lock diagnostic is corrupt/absent: 76-F12 classifies such a holder as
  `ForeignLockHolder`. A genuine ymh daemon whose diagnostic write failed, or
  whose `killHint` is `Unknown` (non-positive/`EPERM` pid, `liveness.cpp:55-63`),
  would be mislabelled. Accepted: 76-D5 pins `Unknown` as "not proven dead" (a
  boot-id match keeps it `OurDaemonUnresponsive`), and both outcomes preserve the
  safety property (no kill, no respawn). The diagnostic write happens on the same
  fd that acquired the lock (`session_persistence.cpp:821-834`), so a failure is
  rare.
- **76-OQ2 (LOW).** 76-F13 (an orphan ymh daemon answering `hello` without the
  lock) cannot happen under H6; the branch exists to fail safe. Its message can
  now name the peer pid: `HelloResult` carries `HostPid pid{0}` at
  `include/ymh/transport/protocol.hpp:298` (the struct is `:294-303`), populated
  from `config_.pid` at `workspace_host.cpp:900`. `ymh workspace stop <path>` is
  the suggested action.
- **76-OQ3 (LOW).** `AttachBudget::total` is set to 25 s (sum of the existing
  10 s readiness + 5 s winner + 5 s connect + 5 s handshake). If a machine's
  cold spawn routinely approaches 10 s the winner budget could be starved;
  measured margin is large (a healthy spawn is sub-second), so no change is
  proposed. Revisit only if telemetry shows exhaustion.

## 15. Revision log

| Date | Revision | Note |
|---|---|---|
| 2026-10-07 | Rev 1 (draft) | Initial: bounded `connect`/`send` (76-D1/76-D2), `AttachBudget` (76-D3), corroborated `Live` (76-D4), deterministic held-lock outcomes (76-D5), authenticated daemon reclaim probe (76-D6), residue policy (76-D7), two additive error codes (76-D8), convergence and no-data-loss invariants, test plan. |
| 2026-10-07 | Rev 2 (draft) | Independent gate fixes: H1 -- pin the `ProtocolServer::workspace()` accessor (76-A9, sec 6/8) so `TransportServer::start` can call `probe_existing_socket(socket_path_, server_.workspace(), deadline)`; M1 -- restate B1b as a non-authenticating/foreign listener and cite the `CLOEXEC` listener guard; M2 -- rename to `rpc_code_for_host_error`; M3 -- cite 11 sec 3.4 D8 (`:320-341`) / the `:53` freeze instead of D20.3; M4 -- correct the `HelloResult` pid claim (`:294-303`); M5 -- thread `AttachBudget`/deadline through `connect_checked`/`spawnAndAttach` (76-A4/D3 + sketch); M6 -- add 76-D12/76-I14 (non-blocking read path); M7 -- handle `EAGAIN`/`EWOULDBLOCK` in the connect poll; M8 -- add `Verification status:` + tracker instruction; M9 -- add daemon-side probe state rows; L1 -- pin `killHint == Unknown`; L2 -- qualify `protocol::kHostConnectTimeout`; L3 -- `OwnershipMark::Unreachable`; L4 -- `tests/support/host_harness.hpp` + CMake list; L5 -- probe mints a transient `ClientInstanceId`. |
| 2026-10-07 | Rev 3 (draft) | Second independent gate fixes: NEW-M1 -- `connect_checked`'s pinned signature now carries `const AttachBudget& budget` and consumes `budget.connect`/`budget.handshake` (defined in the sec 8 signature, 76-D3/D4, 76-A4, and the new-symbol table), so those fields are no longer dead; L3 -- the residual "`Unreachable` lease" at 76-D5 is now "the `OwnershipMark::Unreachable` state"; NEW-L1 -- 76-D8 pins both new members to `code_value(AppCode::NotServing)` (matching `HostUnreachable`; no `default:` in the `-Wswitch`/`-Werror` switch); NEW-L2 -- 76-D12/76-I14 pin `POLLIN`-before-`POLLHUP` precedence with new failure mode 76-F16 and test `Err76.ResponseThenClose`. |

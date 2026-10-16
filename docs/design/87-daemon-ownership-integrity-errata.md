# 87 - Daemon Ownership Integrity Errata

```
Status: verified (Rev 3)
Revision: 3
Verification status: adversarial reviewer (independent, Rev 3): GATE PASS (0 HIGH / 0 MEDIUM, 1 LOW); Oracle confirm PASS.
           Rev 2 resolved the Rev 1 gate; Rev 3 resolves the Rev 2 gate
           (0 HIGH / 1 MEDIUM / 3 LOW) -- see sec 12.
Component: 87 (errata) -- closes four daemon-ownership-integrity gaps that let a
           bare `ymh` start kill its own live daemon link, let a stale
           same-workspace socket survive reclaim, let the daemon silently drop a
           handshaked connection mid-request, and let a cross-workspace
           ownership claim go unflagged. It pins one boot-checked notion of "this
           workspace's live daemon" (composing with 76-D6), makes every
           liveness probe non-owning, pins the mid-session close protocol, and
           references the ownership-integrity check to spec 88.
Amends: 76-host-startup-residue-recovery-errata.md (76-D4/76-D6),
        16-daemon-ownership.md (16-D8, sec 7.5, O1..O22),
        05-transport.md (HostConnection connect/handshake/send; a HostNotice),
        04-workspace-host-daemon.md (sec 4.2, sec 5.2, sec 6.2),
        02-persistence.md (sec 5.7),
        69-host-startup-diagnosability-errata.md (the failure-reporting channel
        this errata routes through),
        10-supervisor-tui.md (sec 2.1 DaemonStatus).
Depends on: 04-workspace-host-daemon.md (verified), 05-transport.md (verified),
        16-daemon-ownership.md (verified), 69-host-startup-diagnosability-errata.md
        (verified), 76-host-startup-residue-recovery-errata.md (verified).
Supersedes: 76-D6's workspace-only reclaim-probe authentication
        (`src/transport/transport_server.cpp:43-77`, check `:69`) -- replaced by a
        boot-checked predicate shared with `connect_checked` and `attempt_attach`
        (87-D1); the predicate is workspace-only **only** when the expected boot
        nonce is genuinely unknown (no claim at read time), and in that case it
        refuses (never unlinks) a same-workspace answering daemon, so 76-D6's
        safety is preserved for the empty case;
        76-D4's use of the caller's `identity.client_instance` for the transient
        `probeLive` handshake (`src/host/workspace_host.cpp:1661-1697`) -- the
        transient probe is separated from the durable link identity (87-D2).
        It does NOT supersede 76's residue-reclamation model, 04 H10/H11, or
        16 O11 ("never kill a live daemon"): a same-workspace answering daemon is
        never unlinked when the expected nonce is unknown, and the only new
        reclaim is a boot-mismatched answer under a non-empty claim -- the exact
        signal `connect_checked`/`attempt_attach` already reject on.
Scope: the identity/authentication of a "live daemon", the identity of a
        transient liveness probe, the daemon-initiated close protocol for an
        established connection, and the routing of a mid-request link loss. It
        does NOT design the ownership-integrity check itself (spec 88), any /fork
        /rewind /handoff feature, session-open latency, or any persistence-schema
        or wire change beyond one additive `HostNotice` kind.
```

## 1. Purpose and scope

### 1.1 The user-visible defect

Observed in the shipped `tmp` tree (version 0.003): a bare `ymh` in a workspace
whose ownership metadata is corrupt prints

```
cannot create session: host connection: write: Broken pipe
```

and never opens a session. The trace (oracle, `/tmp/opencode/oracle-76.md`)
shows the failing frame is `session.create`, sent on a `SupervisorConnection`
that already completed `host.hello` and `session.list`. The link dies between
those frames because the daemon **superseded it**: the bare-`ymh` startup path
opened a second `host.hello` from the *same* `ClientInstanceId` as the live
link, and the daemon drops every same-instance connection on a duplicate hello
(`src/transport/protocol_server.cpp:320-332`). The client's next `send()` then
gets `EPIPE`, and the raw transport string is surfaced to the user
(`src/ui/supervisor.cpp:2662-2665`).

Three further gaps compound it. `probe_existing_socket`
(`src/transport/transport_server.cpp:43-77`) authenticates only
`hello.workspace` (check at `:69`), so a *stale same-workspace* daemon is
classified `LiveYmhDaemon` and blocks reclaim, while the supervisor's own
attach (`connect_checked`, `src/host/workspace_host.cpp:394-412`) would reject
it on `boot_id` -- two different notions of "live". No spec states what a daemon
must do when it closes an established connection, nor what a supervisor must do
when a request dies mid-flight. And nothing verifies that the pid recorded in
`workspaces.host_pid` / the sidecar lease actually belongs to this workspace
(which is how a `bb_tedi` daemon came to be recorded as `louvre`'s host).

### 1.2 The four gaps

- **G1 -- two notions of "live".** 76-D6's reclaim probe checks workspace only;
  `connect_checked` checks workspace and boot, and `attempt_attach`
  (`src/ui/supervisor_connection.cpp:235-244`) is a third variant (workspace,
  then boot only when a boot is known). Pin one predicate used by all three
  (87-D1). The predicate boot-checks when a nonce is known and falls back to
  workspace-only **refuse** (never unlink) when it is not.
- **G2 -- the probe shares the daemon's supersede key.** `ensureRunning`'s
  transient attach probe uses the process-wide `client_instance`
  (`src/ui/supervisor_presence.cpp:12-15`; used at `src/ui/supervisor.cpp:960`,
  `:1579-1581`), and the daemon drops any same-instance connection on a
  duplicate hello (`src/transport/protocol_server.cpp:320-332`). `ymh`'s own
  startup can therefore kill its own live link -- and, because the daemon counts
  `Supervisor`/`Automation` connections as owners
  (`src/transport/protocol_server.cpp:1189`), a transient probe also pollutes
  the ownership set (16 O1..O22). Pin a non-owning, distinct probe identity
  (87-D2).
- **G3 -- no mid-session close protocol.** Nothing pins what a daemon does when
  it closes a `hello_done` connection (supersede, drain), nor what the
  supervisor does when a request dies mid-flight. Pin the daemon close frames
  and the supervisor's link-loss classification (87-D3/87-D4).
- **G4 -- ownership metadata has no integrity check.** A claim/lease whose pid's
  root disagrees with the row's `canonical_path` is unflagged. Pin the
  flag-not-kill invariant and reference the check to spec 88 (87-D5).

### 1.3 Precedence and relationship to 76

87 is an errata on top of 76, not a replacement. 76 makes startup *bounded* and
reclaims a residue socket; 87 makes the *identity* tests consistent and pins the
*established-connection* lifecycle that 76 deliberately left out of scope
(76 sec 2, "Out of scope ... the text/presentation of startup failures"; and
76 sec 1.2-1.3 cover only the pre-handshake residue). Where 76-D6 and 87-D1
differ, 87-D1 wins; every other 76 decision stands. 87, like 76, adds no
message transport of its own: it routes diagnostics through 69's channel
(`docs/design/69-host-startup-diagnosability-errata.md` sec 4/6).

### 1.4 Scope boundaries

In scope:

- the shared "live daemon" predicate and the reclaim-probe signature/source of
  its expected boot nonce (G1);
- the identity and role of a transient liveness probe, and the daemon's
  supersede key (G2);
- the daemon's close frames before draining an established connection, and the
  supervisor's classification of a mid-request link loss (G3);
- the flag-not-kill invariant for ownership-integrity and its reference to
  spec 88 (G4).

Out of scope (owned elsewhere): the ownership-integrity check's own design,
entry point, and symbols (spec 88, authored in parallel); startup-failure text
presentation (69); residue reclamation itself (76); session-open latency (77);
`/fork` (78), `/rewind` (79), file checkpoints (80), dashboard (81),
`/handoff` (82).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 87-A1 | 76-D6 (and 76-A5, 76-A9), 10 sec 3.3 | `src/transport/transport_server.cpp:43-77` (check `:69`), `include/ymh/transport/transport_server.hpp:61-63`, `include/ymh/transport/protocol_server.hpp:123` (the `workspace()` accessor added by 76-A9; NOT `:179`, which is `compose_status`), `src/host/workspace_host.cpp:406-410`, `src/ui/supervisor_connection.cpp:235-244` | `probe_existing_socket` gains `const HostBootId& expected_boot_id`; `LiveYmhDaemon` iff `names_live_daemon(hello, workspace, expected_boot_id)`, else `Residue`. `connect_checked` and `attempt_attach` are routed through the same predicate (three sites, one notion; 87-I1). `TransportServer::start` gains the expected predecessor boot. Supersedes 76-D6's "compares `HelloResult.workspace`". |
| 87-A2 | 16-D8 / 16 sec 7.5, 05 sec 4.4, 76-D4 | `src/ui/supervisor_presence.cpp:12-15`, `src/cli/cli.cpp:519`, `src/ui/supervisor.cpp:960`, `:1579-1581`, `src/transport/protocol_server.cpp:320-332` | transient liveness probes use a fresh `ClientInstanceId` + `ClientRole::Observer`; the process instance is reserved for the durable `SupervisorConnection`; the daemon's supersede key becomes `(ClientInstanceId, ClientRole)`. |
| 87-A3 | 02 sec 5.7, 04 sec 4.2, 05 sec 5, 16 sec 3 | `src/transport/protocol_server.cpp:320-332`, `:763-789`, `:791-808` (session-scoped `end_subscriptions`), `include/ymh/transport/protocol.hpp:255-263`, `src/transport/protocol.cpp:53-60`, `src/ui/ui_event_adapter.cpp:431-482`, `src/ui/supervisor_connection.cpp:436-480`, `:502-525`, `src/ui/supervisor.cpp:2662-2665`, `src/transport/host_connection.cpp:240-243` | daemon-initiated close of a `hello_done` connection delivers `event.unsubscribed` (via a new all-subscriptions companion to the session-scoped `end_subscriptions`) + a `host.event` `LinkDropped` notice before drain; the supervisor classifies a mid-request `EPIPE`/peer-close as a reconnectable `Dead` link loss, never surfacing the raw transport string; `UiEventAdapter::onHostNotice` gains a `LinkDropped` case; `request()`'s read-timeout throw becomes `ConnectionError(Timeout)` (N1, non-link-loss). |
| 87-A4 | 03 sec 5, 04 sec 5, 16 sec 2 | `src/registry/registry.cpp:1061-1087` (claimHost), `src/session/session_persistence.cpp:821-834` (lease write) | ownership-integrity is a flag-not-kill invariant; the check itself (read-only `ymh doctor` + explicit `ymh workspace reap`) is `docs/design/88-doctor-ownership-integrity-errata.md`. No signal, no reap, no claim clear from an integrity mismatch without explicit user action. |
| 87-A5 | 04 sec 5.2, 76 sec 5 (76-A5) | `src/transport/transport_server.cpp:263` | `TransportServer::start` caller passes the predecessor boot nonce threaded by `HostLifecycle` (87-D1 source rule). |

## 3. Decision register

**87-D1 -- One notion of "this workspace's live daemon" (boot-checked when a
nonce is known, workspace-only refuse when it is not).**

A single predicate replaces the three divergent checks:

```cpp
// A hello names this workspace's live daemon iff it names the workspace AND
// either (a) a nonce is known and the hello's boot matches it, or (b) no nonce
// is known, in which case only the workspace is corroborated. Case (b) is the
// CONSERVATIVE fallback: it accepts (refuses-to-unlink) a same-workspace
// answering daemon rather than treating it as residue.
[[nodiscard]] bool names_live_daemon(const HelloResult& hello,
                                     const WorkspaceId& workspace,
                                     const HostBootId& expected_boot_id) noexcept {
    if (hello.workspace.value != workspace.value) {
        return false;
    }
    return expected_boot_id.value.empty() ||
           hello.boot_id.value == expected_boot_id.value;
}
```

Three callers, one notion (87-I1, resolves the L8 split):

- `connect_checked` (supervisor attach, `src/host/workspace_host.cpp:406-410`)
  uses it with `expected_boot_id = record.host->bootId` (always non-empty; the
  inline `||` is replaced by the predicate).
- `attempt_attach` (durable link, `src/ui/supervisor_connection.cpp:235-244`)
  uses it with `config_.expected_boot_id`; the predicate reproduces today's
  behavior exactly (empty expected => workspace-only).
- `probe_existing_socket` (daemon reclaim, `src/transport/transport_server.cpp:69`)
  uses it with the predecessor boot nonce. `LiveYmhDaemon` iff the predicate
  holds; otherwise `Residue`.

**When the expected nonce is set vs empty (pinned).** The nonce is "the boot
nonce currently associated with this workspace's claim":

- **Set (non-empty)** whenever the workspace had a claim at the `ensureRunning`
  read (`record.host.has_value()`, `src/host/workspace_host.cpp:1703`). This
  includes the reaped-replacement path: `ensureRunning` takes the
  `WorkspaceRecord` **copy before** `reapIfStale` (`:1718`), and `reapHost`
  mutates the DB, not the copy, so `configFor` (`:1538`) still sees
  `record.host->bootId`. `HostLifecycle::configFor` threads it into
  `HostConfig.predecessor_boot_id`, the launcher emits
  `--predecessor-boot-id`, and `WorkspaceHost::run` passes it to
  `TransportServer::start` (87-A1/87-A5).
- **Empty** only on the no-claim path: `ensureRunning` reaches
  `spawnAndAttach` with `record->host` absent at `:1708-1709`, so there is no
  predecessor nonce to carry; the defaulted
  `TransportServer::start(expected = {})` and any legacy caller also produce an
  empty nonce.

**Safety (M1).** The empty case **cannot unlink or close a live daemon**:
`names_live_daemon` returns `true` for any same-workspace hello when the nonce
is empty, so `probe_existing_socket` returns `LiveYmhDaemon` and
`TransportServer::start` refuses (`AlreadyRunning`). `TransportServer::start`
unlinks on **two** results (`src/transport/transport_server.cpp:266-269`):
`Refused` (a stale inode with nothing listening -- by definition not a live
daemon) and `Residue`. Of those, the only *reclaim* path that can remove a
**live** daemon is `Residue`, and `Residue` is reachable **only** with a
non-empty nonce and a differing boot. This is strictly more conservative than
76-D6 (which classified every same-workspace hello as `LiveYmhDaemon`) and is
safe under inode split: an empty nonce never authorizes a reclaim.

The non-empty reclaim (`Residue` on a boot mismatch) is the intended G1 change
and is the same signal `connect_checked`/`attempt_attach` already reject on. Its
residual -- a split-lock predecessor whose live boot disagrees with a
*corrupt* registry claim -- is recorded in RISK-87-1; that is the G4 corruption
case, flagged by spec 88, not a case 87 can prove locally (see OQ-87-1).

Rejected: comparing against the probing daemon's own fresh boot id. That would
unlink a genuinely live same-workspace daemon (split-inode case, 76-I13) whose
boot differs from the probe's, violating 04 H11 / 16 O11. Also rejected: the
Rev 1 rule "empty nonce => workspace-match is `Residue`", which unlinked a live
daemon whenever the registry had no claim (M1).

**87-D2 -- Liveness probes are non-owning and never share the durable link's
supersede key.**

Every transient liveness probe declares a **fresh `ClientInstanceId`** and
`ClientRole::Observer` ("owns nothing", 16 sec 4.6), so it can neither supersede
the durable link nor enter the owner set (`src/transport/protocol_server.cpp:1189`
counts only `Supervisor`/`Automation`).

- New `ui::probe_attach_identity()` returns
  `AttachIdentity{ClientInstanceId{generate_uuid_v4()}, ClientRole::Observer}`.
- The supervisor's `ensureRunning` call sites -- bare-`ymh`
  (`src/cli/cli.cpp:519-523`) and `ensure_worker_loop`
  (`src/ui/supervisor.cpp:1579-1581`) -- pass `probe_attach_identity()`. The
  process-wide `process_client_instance()` (`src/ui/supervisor_presence.cpp:12-15`,
  which equals `supervisors.id` per 16-D8/C-H2) is reserved for the durable
  `SupervisorConnection` (`src/ui/supervisor.cpp:960`).
- `ymh run` keeps its per-process `Automation` identity
  (`src/cli/cli.cpp:573-576`) because it *retains* the returned connection as
  its live link; only the returned-connection-is-discarded paths are probes.
- `probe_existing_socket`'s handshake (`src/transport/transport_server.cpp:64-67`)
  also declares `ClientRole::Observer` instead of the default `Supervisor`, so
  the daemon-side reclaim probe cannot pollute the owner set of a live peer.
- The daemon's supersede key becomes the pair `(ClientInstanceId, ClientRole)`
  (87-A2): a reconnect of the same role+instance still supersedes its own dead
  predecessor link, but two connections that share an instance and differ in
  role never cross-evict.

Rejected: "make supersede role-aware" *alone*. A probe reusing the process
instance would still carry `ClientRole::Supervisor` and collide on
`(instance, Supervisor)`; role-awareness does not fix the collision unless the
probe has a distinct role, which is exactly the fresh-`Observer` rule. The
`(instance, role)` key is therefore adopted as defense-in-depth **in addition
to** the fresh-`Observer` probe identity, not instead of it.

**87-D3 -- A daemon never silently closes a handshaked connection.**

Add `HostNoticeKind::LinkDropped` (wire name `"link_dropped"`, additive,
87-D6). Add `ProtocolServer::graceful_drop(Connection& conn, std::string reason)`:

- if `conn.dropped || !conn.hello_done || !conn.send`, fall back to
  `drop_client(conn, reason)` (nothing can be delivered);
- otherwise, in order: (1) `end_all_subscriptions(conn, reason)` so every active
  subscription receives `event.unsubscribed{reason}`; (2) enqueue `host.event`
  `HostNotice{LinkDropped, workspace, nullopt, reason}`; (3) set
  `conn.close_when_drained = true` and `maybe_close_after_drain(conn)` so the
  frames flush before the fd closes.

**L2 fix -- a new all-subscriptions companion.** The shipped
`ProtocolServer::end_subscriptions(Connection&, const SessionId& session,
std::string reason)` (`src/transport/protocol_server.cpp:791-808`) is
**session-scoped**. `graceful_drop` needs to end every subscription on the
connection, so 87 adds `ProtocolServer::end_all_subscriptions(Connection&,
std::string reason)`, which iterates `conn.subscriptions` and enqueues one
`event.unsubscribed{reason}` per entry, reusing the same `notification_json`/
`UnsubscribedNotice` builder the session-scoped method uses. Concrete caller:
`graceful_drop`. The session-scoped `end_subscriptions` is unchanged.

**L3 fix -- the receive-side consumer is named and normative.**
`UiEventAdapter::onHostNotice` (`src/ui/ui_event_adapter.cpp:431-482`) is an
exhaustive `switch` over `HostNoticeKind` with **no `default:`**, compiled under
`-Wswitch -Werror`, so the new member is a hard build break until a case is
added. 87 requires a `LinkDropped` case (a no-op or a 69-channel connection-loss
notice for the workspace); this consumer is listed in sec 4.

**L1 fix -- backward compatibility with an OLD client (`kProtocolVersion`
stays 1).** The two directions are asymmetric:

- **new daemon -> old client.** An old client's `from_json(HostNotice)`
  (`src/transport/protocol.cpp:441-445`) throws
  `std::invalid_argument("unknown host notice kind")` for `link_dropped`; in the
  pump (`src/ui/supervisor_connection.cpp:428-433`, dispatched from `:573-583`)
  that surfaces as `handle_disconnect("unknown host notice kind")`. Impact is
  benign: the daemon is closing the connection anyway, so the old client reaches
  the same terminal state (`Dead`/reconnect) it would have reached from the
  following EOF; only the diagnostic detail differs. No data is lost and no
  other method's semantics change.
- **old daemon -> new client.** Fine: an old daemon never emits `link_dropped`,
  and the new client's switch has a case for every old kind.

87 therefore keeps `kProtocolVersion == 1` (`include/ymh/transport/protocol.hpp:165`):
the new member is additive in the wire sense, and the one strict-parse
consequence falls on an already-terminal close. This is recorded as 87-F13.

Every daemon-initiated drop of a `hello_done` connection routes through
`graceful_drop`. The concrete shipped caller is the supersede path
(`src/transport/protocol_server.cpp:330`). The shutdown path is
`kHostShutdown` (`:364-377`) plus `onDaemonShuttingDown` (`:946-962`): it
already emits a `DaemonShuttingDown` notice to every `hello_done` Interactive
connection and sets `close_when_drained` only on the *requesting* connection
(`:376-377`), so it does not currently `drop_client` the other established
connections; for correctness, any drain-time close of an established connection
must use `graceful_drop(conn, "daemon_shutting_down")` (the `DaemonShuttingDown`
notice is retained, so a client that already saw it does not get a duplicate
reason). Admission rejections that already send a framed error before closing
(uid mismatch, unsupported protocol, not serving; `:303-318`) are unchanged --
they are not "silent". The outbound-overflow drops (`:713`, `:724`) keep
`drop_client` because the queue is already unsendable.

**87-D4 -- The supervisor treats a mid-request link loss as a reconnectable
`Dead` state, routed through 69.**

Add a typed transport error:

```cpp
enum class ConnectionErrorKind : std::uint8_t {
    Connect, Socket, Timeout, Epipe, PeerClosed, Other,
};
class ConnectionError : public std::runtime_error {
public:
    ConnectionError(ConnectionErrorKind kind, std::string message);
    [[nodiscard]] ConnectionErrorKind kind() const noexcept;
    [[nodiscard]] bool is_link_loss() const noexcept;   // Epipe || PeerClosed
};
```

All `HostConnection` **transport** faults become `ConnectionError` with a kind;
the message text is unchanged (76-D1/D2 preserved). This has two parts:

- Every fault currently built by the file-local `connection_error`
  (`src/transport/host_connection.cpp:22-24`) -- connect, send, and the read
  `0`/`errno` paths -- becomes `ConnectionError`. A `send(2) => EPIPE` is
  `Epipe`; a read `0` / `ECONNRESET` / `ECONNABORTED` is `PeerClosed`; **and the
  write-path peer-close `POLLERR|POLLHUP|POLLNVAL` in `send_message`
  (`src/transport/host_connection.cpp:138-141`, "write: peer closed the
  connection") is also `PeerClosed`** (L9 fix); `connect(2)`-time faults are
  `Connect`/`Socket`.
- **N1 fix -- the `request()` read wait is brought into the taxonomy.** Today the
  bounded read wait is *not* a `connection_error`: `wait_readable`
  (`src/transport/host_connection.cpp:154-174`) returns `bool` on a poll
  timeout (no throw), `fill_incoming` (`:176-210`) returns `bool`, and
  `request()` maps the exhausted wait to
  `throw RpcException(InternalError, "timed out waiting for a response")`
  (`:240-243`). 87 changes **exactly that throw** to
  `ConnectionError(Timeout, "timed out waiting for a response")`. Nothing else
  in the read path changes: `wait_readable` and `fill_incoming` keep returning
  `bool`; `nextNotification`'s `!fill_incoming(timeout)` still returns
  `nullopt` (no throw, `:271-273`); decode/framing failures
  (`:186-188`, `ProtocolError -> RpcException`) and server error responses
  (`:252-255`) stay `RpcException` because they are semantic, not transport.
  `Timeout` is by definition **non-link-loss** (`is_link_loss()` false, `:328`),
  so a read timeout does **not** reconnect: the link stays `Attached`, the
  in-flight request gets the classified token, and a later frame still flows.

`SupervisorConnection::process_requests` (`src/ui/supervisor_connection.cpp:436-480`):

- `RpcException` handling is unchanged (semantic per-op error, incl. decode and
  server-error responses, which stay `RpcException`).
- **Every** `ConnectionError` is answered with the classified token
  `describe_connection_loss(kind)` (e.g. `"connection lost"` for
  `Epipe`/`PeerClosed`, `"request timed out"` for `Timeout`), never the raw
  `what()`.
- A `ConnectionError` with `is_link_loss()` additionally answers the in-flight
  and every remaining batched request with that classified token, then calls
  `handle_disconnect(describe_connection_loss(kind))`. `handle_disconnect`
  closes the fd, clears subscriptions, drains pending requests, and sets
  `SupervisorLinkState::Dead`; the pump loop then re-attaches
  (`src/ui/supervisor_connection.cpp:527-584`). The `Dead` detail passed to
  `on_link_state` is likewise a classified token, not a raw `strerror` string.
- A `ConnectionError` with `Timeout` (link still alive, incl. the N1 read wait)
  surfaces the classified token and the link stays `Attached` -- **no
  reconnect**.

Consequently `apply_create_reply` (`src/ui/supervisor.cpp:2662-2665`) can never
render `"host connection: write: Broken pipe"`: the reply it receives is the
classified token. The `Dead` transition flows through `on_link_state`
(`src/ui/supervisor.cpp:2448`) into 69's channel (`surface_notice`), which owns
the user-visible text.

**87-D5 -- Ownership-integrity is flag-not-kill; the check is spec 88.**

No ownership-integrity check -- and no path introduced by 87 -- may signal,
reap, or clear a claim. A claim/lease whose pid's command-line root disagrees
with the row's `canonical_path` (`src/registry/registry.cpp:1061-1087`,
`src/session/session_persistence.cpp:821-834`) is **flagged** (surfaced through
69's channel / a status field), never acted on. The check's design, its entry
point, its symbols, and its state lifetimes are owned by
`docs/design/88-doctor-ownership-integrity-errata.md` (authored in parallel; a
read-only `ymh doctor` flag plus an explicit, user-invoked `ymh workspace reap`);
87 pins only this invariant and the interface contract 88 must satisfy (a
read-only classification that never signals on its own).
This preserves 04 H10/H11 and 16 O11.

**87-D6 -- One additive wire member; no schema, exit-code, or registry change.**

`HostNoticeKind::LinkDropped` and `ConnectionError` are additive; no
`registry.db` / `sessions.db` column, no schema version, and no process exit
code changes. `predecessor_boot_id` is a spawn-time value threaded in memory and
on the daemon argv; it is never persisted. `kProtocolVersion` stays 1: the only
cross-version consequence is 87-F13 (a pre-87 client's strict `HostNotice`
parse of `link_dropped` fails on an already-terminating close), analyzed in
87-D3; a new client is forward- and backward-compatible with all existing kinds.

**87-D7 -- Diagnostics compose with 69; they never bypass it.**

Every failure 87 introduces is reported as a bounded, ASCII single line through
69's channel. 87 adds no new message transport.

**87-D8 -- Convergence (idempotence).**

After the fixes: (a) a bare `ymh` start that attaches to a live daemon makes no
second `hello` on the durable link's identity, so the link survives and
`session.create` succeeds; (b) a replacement daemon reclaims a stale
same-workspace socket (boot mismatch => `Residue`); (c) a mid-request link loss
transitions `Dead` and reconnects without surfacing a raw transport string.

## 4. New symbols and their callers

Every symbol 87 introduces or changes names at least one concrete caller in the
tree (AGENTS.md "new symbols are normative").

| Symbol | Declared | Concrete caller(s) |
|---|---|---|
| `protocol::names_live_daemon` | `include/ymh/transport/protocol.hpp` | `connect_checked` (`src/host/workspace_host.cpp:406`), `attempt_attach` (`src/ui/supervisor_connection.cpp:235-244`), and `probe_existing_socket` (`src/transport/transport_server.cpp:69`) |
| `probe_existing_socket(path, ws, expected_boot_id, deadline)` | `include/ymh/transport/transport_server.hpp:61` | `TransportServer::start` (`src/transport/transport_server.cpp:263`) |
| `TransportServer::start(expected_predecessor_boot_id)` | `include/ymh/transport/transport_server.hpp:80` | `WorkspaceHost::run` (`src/host/workspace_host.cpp:950`) |
| `HostConfig::predecessor_boot_id` | `include/ymh/host/workspace_host.hpp:151` (new member after `boot_id` at `:150`) | `HostLifecycle::configFor` (`src/host/workspace_host.cpp:1538`); read by `WorkspaceHost::run`; emitted/parsed by `ForkExecLauncher::build_argv` / the `--host` entry |
| `ui::probe_attach_identity()` | `include/ymh/ui/supervisor_presence.hpp` | `src/cli/cli.cpp:519-523` (bare `ymh`), `src/ui/supervisor.cpp:1579` (`ensure_worker_loop`) |
| `HostNoticeKind::LinkDropped` | `include/ymh/transport/protocol.hpp:255` | producer: `ProtocolServer::graceful_drop` (`src/transport/protocol_server.cpp`) + wire table `kHostNoticeKinds` (`src/transport/protocol.cpp:53`); consumer: `UiEventAdapter::onHostNotice` `LinkDropped` case (`src/ui/ui_event_adapter.cpp:431-482`, exhaustive `-Wswitch` switch), reached via `SupervisorConnection` `on_notice` (`src/ui/supervisor_connection.cpp:428-433`) -> `handle_sink_notice` (`src/ui/supervisor.cpp:978-982`) |
| `ProtocolServer::graceful_drop` | `include/ymh/transport/protocol_server.hpp` | concrete caller: supersede path (`src/transport/protocol_server.cpp:330`); drain-time close uses it with reason `"daemon_shutting_down"` (`kHostShutdown` handler `:364-377` + `onDaemonShuttingDown` `:946-962`) |
| `ProtocolServer::end_all_subscriptions` | `include/ymh/transport/protocol_server.hpp` | `ProtocolServer::graceful_drop` (all-subscriptions companion to the session-scoped `end_subscriptions`, `src/transport/protocol_server.cpp:791-808`) |
| `protocol::ConnectionError` / `ConnectionErrorKind` | `include/ymh/transport/host_connection.hpp` | thrown by `HostConnection` (`src/transport/host_connection.cpp:22-243`, incl. the N1 read-wait conversion at `:240-243`); caught by `SupervisorConnection::process_requests` (`src/ui/supervisor_connection.cpp:436-480`) and `maybe_ping` (`:484-500`) |
| `protocol::describe_connection_loss` | `include/ymh/transport/host_connection.hpp` (same home as `ConnectionError`; the sketch banner matches) | `SupervisorConnection::process_requests` (reply + `handle_disconnect` detail) and `maybe_ping` (`handle_disconnect` detail) (`src/ui/supervisor_connection.cpp:436-500`) |

## 5. C++ interface sketches

```cpp
// include/ymh/transport/protocol.hpp (additive; 87-D1/87-D3)
enum class HostNoticeKind : std::uint8_t {
    // ... existing members unchanged ...
    LinkDropped,   // 87-D3: the daemon is closing a handshaked connection;
                   // `detail` is the bounded drop reason. Wire: "link_dropped".
};

// 87-D1: the single notion of "this workspace's live daemon". Boot-checked when
// `expected_boot_id` is non-empty; workspace-only (conservative accept) when it
// is empty -- see 87-D1 for the exact set/empty rule and the safety argument.
[[nodiscard]] bool names_live_daemon(const HelloResult& hello,
                                     const WorkspaceId& workspace,
                                     const HostBootId& expected_boot_id) noexcept;

// include/ymh/transport/host_connection.hpp (additive; 87-D4) -- declared next to
// HostConnection, which throws it and whose `connection_error` helper it replaces.
enum class ConnectionErrorKind : std::uint8_t {
    Connect,     // connect(2)/SO_ERROR before an established link
    Socket,      // socket(2)/fcntl/poll setup failure
    Timeout,     // a bounded wait expired on a live link: send-track POLLOUT
                 // (host_connection.cpp:122-123/:135-136) OR request()'s read
                 // wait (:240-243, converted by 87-D4 N1). NON-link-loss.
    Epipe,       // send(2) => EPIPE (peer read side closed)
    PeerClosed,  // read => 0 / ECONNRESET / ECONNABORTED, OR write-side
                 // POLLERR|POLLHUP|POLLNVAL ("write: peer closed the connection")
    Other,
};
class ConnectionError : public std::runtime_error {
public:
    ConnectionError(ConnectionErrorKind kind, std::string message);
    [[nodiscard]] ConnectionErrorKind kind() const noexcept;
    [[nodiscard]] bool is_link_loss() const noexcept;   // Epipe || PeerClosed
private:
    ConnectionErrorKind kind_;
};
[[nodiscard]] std::string_view describe_connection_loss(ConnectionErrorKind) noexcept;

// include/ymh/transport/transport_server.hpp (changed; 87-D1/87-A5)
[[nodiscard]] SocketProbeResult probe_existing_socket(
    const std::string& socket_path,
    const WorkspaceId& workspace,
    const HostBootId& expected_boot_id,      // "" => workspace-only accept
                                             // (LiveYmhDaemon/refuse); Residue
                                             // requires a non-empty mismatch
    std::chrono::steady_clock::time_point deadline);

class TransportServer {
public:
    // CHANGED (87-D1): the predecessor boot nonce bounds what counts as a live
    // peer at reclaim time. Defaulted so existing tests keep compiling.
    void start(const HostBootId& expected_predecessor_boot_id = {});
    // ... rest unchanged ...
};

// include/ymh/host/workspace_host.hpp (additive; 87-D1)
struct HostConfig {
    // ... existing members unchanged ...
    std::optional<HostBootId> boot_id;              // this daemon's own nonce
    std::optional<HostBootId> predecessor_boot_id;  // 87-D1: the claim being
                                                    // replaced (empty if none)
};

// include/ymh/ui/supervisor_presence.hpp (additive; 87-D2)
// A transient, non-owning liveness-probe identity: a fresh in-memory UUIDv4
// plus ClientRole::Observer (owns nothing, 16 sec 4.6). Distinct from
// process_client_instance() (the durable link identity == supervisors.id).
[[nodiscard]] AttachIdentity probe_attach_identity();

// include/ymh/transport/protocol_server.hpp (additive; 87-D3)
class ProtocolServer {
public:
    // Deliver the close reason frames for a handshaked connection, then drain
    // and close. Falls back to drop_client when nothing can be delivered.
    void graceful_drop(Connection& conn, std::string reason);
    // 87-D3/L2: end EVERY subscription on the connection (the shipped
    // `end_subscriptions` at `src/transport/protocol_server.cpp:791-808` is
    // session-scoped). Caller: graceful_drop.
    void end_all_subscriptions(Connection& conn, std::string reason);
    // ... rest unchanged ...
};
```

`graceful_drop` in context (the supersede path; `src/transport/protocol_server.cpp:320-332`):

```text
handle_hello(...):
  for each other conn with (instance, role) == params:
      graceful_drop(*other, "superseded")     # 87-D3 (was drop_client)
        end_all_subscriptions(other, "superseded") -> event.unsubscribed per sub
        enqueue host.event HostNotice{LinkDropped, ws, nullopt, "superseded"}
        close_when_drained = true; maybe_close_after_drain(other)
```

Supervisor link-loss classification (`src/ui/supervisor_connection.cpp:436-480`):

```text
process_requests():
  for each request:
    try   { reply = connection_->request(...) }
    catch (RpcException& e)          { reply.ok=false; reply.error_code=e.code(); reply.error=e.what(); }
    catch (ConnectionError& e) {
        reply.ok=false; reply.error=describe_connection_loss(e.kind());  # classified token
        if (e.is_link_loss()) link_lost = true;                          # -> Dead -> reconnect
    }
    catch (std::exception& e)        { reply.ok=false; reply.error=e.what(); }
    send reply;
  if (link_lost) handle_disconnect(describe_connection_loss(kind));  # -> Dead -> reconnect
```

## 6. State-lifetime table

Every state 87 introduces is call- or process-scoped; nothing new is persisted.

| State | Type / where | Created | Destroyed / cleared | Owner | Survives process restart? |
|---|---|---|---|---|---|
| `expected_predecessor_boot_id` (probe) | `HostBootId` value | read from the pre-reap `WorkspaceRecord` in `ensureRunning` (`workspace_host.cpp:1703`); **empty only on the no-claim path** where `record->host` is absent at `:1708` | at `spawnAndAttach` return | spawn call frame -> `HostConfig` -> daemon argv | no (re-derived per spawn) |
| `HostConfig::predecessor_boot_id` | `std::optional<HostBootId>` | `configFor` (`workspace_host.cpp:1538`) | daemon process exit | daemon process | no |
| transient probe `ClientInstanceId` | `ClientInstanceId` (UUIDv4) | `probe_attach_identity()` per call | probe connection close (RAII) | probe call frame / `AttachResult` | no |
| `ConnectionError::kind_` | enum, on the exception | thrown in `HostConnection` | stack unwind | `HostConnection` call frame | no |
| `conn.close_when_drained` | `bool`, per `ProtocolServer::Connection` | already exists | `drop_client` / fd close | `ProtocolServer` | no |
| `HostNoticeKind::LinkDropped` | enum member | static | static | binary | n/a (wire value) |
| `SupervisorLinkState::Dead` | existing enum value, per `SupervisorConnection` | set by `handle_disconnect` | overwritten by `Connecting` on re-attach | `SupervisorConnection` | no |

## 7. Invariants

| ID | Invariant |
|---|---|
| **87-I1** | Exactly one predicate defines "this workspace's live daemon": `names_live_daemon(hello, workspace, expected_boot_id)` -- a workspace match **and** (the expected boot is non-empty and matches **or** the expected boot is empty). All three attach sites use it: `connect_checked`, `attempt_attach`, and `probe_existing_socket`; no call site compares `hello.workspace` alone. |
| **87-I2** | `probe_existing_socket` returns `Residue` **only** for a hello that names this workspace with a **non-empty** expected boot that differs; it returns `LiveYmhDaemon` when `names_live_daemon` holds (workspace+boot match, **or** workspace-only when the expected boot is empty), and `Residue` for an unauthenticated/non-workspace hello. `TransportServer::start` unlinks only for `Refused`/`Residue` (76-D6 retained). An **empty** expected boot can never yield `Residue`, so it can never unlink a same-workspace answering daemon (M1). |
| **87-I3** | The reclaim probe's expected boot is the claim the daemon is replacing, captured before `reapIfStale`; it is non-empty whenever the workspace had a claim at `ensureRunning` read time (including the reaped path, since the record copy is pre-reap) and empty only on the no-claim path. It is never the probing daemon's own `boot_id`. |
| **87-I4** | A transient liveness probe never carries the durable link's `ClientInstanceId`, never declares `ClientRole::Supervisor`/`Automation`, and is therefore excluded from the owner set (`protocol_server.cpp:1189`). |
| **87-I5** | The daemon's supersede key is `(ClientInstanceId, ClientRole)`; a duplicate hello evicts only a connection with both equal. |
| **87-I6** | `ensureRunning`'s probe identity is a fresh `ClientInstanceId` + `Observer`; `process_client_instance()` is used only for a durable `SupervisorConnection` (whose id equals `supervisors.id`, 16-D8/C-H2). |
| **87-I7** | A daemon-initiated close of a `hello_done` connection that still has a writable send path delivers `event.unsubscribed` for each subscription and one `LinkDropped` `host.event` before the socket closes. |
| **87-I8** | Every `HostConnection` **transport** fault -- connect, send, read, and both bounded waits (write `poll(POLLOUT)` at `host_connection.cpp:122-123`/`:135-136` and `request()`'s read wait at `:240-243`) -- is a `ConnectionError` with a kind. Decode/framing failures and server error responses stay `RpcException` (semantic, not transport). A link-loss kind is `Epipe` or `PeerClosed`; `Timeout` is an explicit **non-link-loss** kind, so a read or write timeout never triggers a reconnect. |
| **87-I9** | `SupervisorConnection` maps every `ConnectionError` to the classified token `describe_connection_loss(kind)` in a `SupervisorReply.error`; a link-loss kind further transitions the link to `Dead` and reconnects. The raw transport `what()` never reaches a `SupervisorReply.error` or a `Dead` detail that is surfaced to a user. |
| **87-I10** | No ownership-integrity check signals, reaps, or clears a claim; a mismatch is flagged (87-D5). No path in 87 kills a process. |
| **87-I11** | 87 adds one wire value (`link_dropped`) and no schema/exit-code/registry change. |
| **87-I12** | Every diagnostic 87 produces is a bounded ASCII single line surfaced through 69's channel. |

## 8. Failure modes (`87-F#`)

| ID | Trigger | Observable | Required behavior |
|---|---|---|---|
| **87-F1** | `probe_existing_socket` reaches a socket that answers `hello.workspace` but a different boot than a **non-empty** expected predecessor | `ProbeResult` | `Residue` -> unlink + bind (was `LiveYmhDaemon` -> refuse; 87-D1) |
| **87-F2** | Reclaim probe has an **empty** expected predecessor boot (no-claim path) and a same-workspace daemon answers | `ProbeResult` | `LiveYmhDaemon` -> refuse; the socket is **not** unlinked (M1: empty never authorizes reclaim) |
| **87-F3** | A genuinely live same-workspace daemon answers with the expected predecessor boot (split-lock) | `ProbeResult` | `LiveYmhDaemon` -> `AlreadyRunning`; the socket is not unlinked (04 H11 / 16 O11) |
| **87-F4** | `ensureRunning` probe reuses the durable link's instance | live link is dropped by the daemon's supersede on the probe's hello | forbidden: probe uses a fresh instance + `Observer` (87-D2); the durable link is untouched |
| **87-F5** | A transient probe shares a process instance with a different-role connection | cross-eviction | forbidden: supersede keys on `(instance, role)` (87-I5) |
| **87-F6** | A probe (supervisor- or daemon-side) handshakes and closes | owner set / last-exit accounting | no effect: `Observer` is excluded from owner liveness and `other_fresh_owners` (87-I4) |
| **87-F7** | The daemon supersedes or drains a `hello_done` connection | client sees a bare EOF/`EPIPE` with no reason | forbidden: `event.unsubscribed` + `LinkDropped` are delivered before the close (87-D3) |
| **87-F8** | `send(2)` returns `EPIPE` mid-request | raw `"host connection: write: Broken pipe"` reaches a per-op error | forbidden: classified `Dead` link loss; reply error is `"connection lost"` (87-D4) |
| **87-F9** | A read returns `0` / `ECONNRESET` / `ECONNABORTED`, **or** a write-side `poll` reports `POLLERR|POLLHUP|POLLNVAL` (`host_connection.cpp:138-141`) on an established link | link stays `Attached` with a dead fd | forbidden: both are `PeerClosed` link loss -> `Dead` -> reconnect (87-I8/I9) |
| **87-F10** | A bounded wait expires with the link alive (write `POLLOUT` or `request()`'s read wait) | link marked `Dead` | forbidden: `Timeout` is not a link loss; the reply carries the classified token `describe_connection_loss(Timeout)` and the link stays `Attached` -- no reconnect (87-I8/I9, N1) |
| **87-F11** | Ownership claim/lease pid's root disagrees with `canonical_path` | unflagged corruption | forbidden (by 87-D5): flagged (read-only `ymh doctor`), never killed; check design in `docs/design/88-doctor-ownership-integrity-errata.md` |
| **87-F12** | `session.create` loses its link mid-flight | user sees a raw transport string | forbidden: `apply_create_reply` receives the classified token; the `Dead` transition is surfaced through 69 (87-I9/87-I12) |
| **87-F13** | A NEW (`link_dropped`-emitting) daemon serves an OLD client (`kProtocolVersion == 1`) | old client's strict `from_json(HostNotice)` throws `"unknown host notice kind"` -> `handle_disconnect` | accepted: the daemon is closing anyway, so the old client reaches the same terminal `Dead`/reconnect it would reach from the following EOF; only the diagnostic detail differs (87-D3/87-D6) |

## 9. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| Service identity by authenticated handshake, not by endpoint reachability | `names_live_daemon` used by `connect_checked`, `attempt_attach`, and `probe_existing_socket` (87-D1) | Mirror: dsh identifies a service by its handshake. ymh already says the same in `OwnershipMark::Unreachable` (`docs/design/16-daemon-ownership.md:701`); 87 makes all three attach sites agree, closing the workspace-only/boot-checked asymmetry 76-D6 left (76's own dsh row, `docs/design/76-host-startup-residue-recovery-errata.md:598`). |
| Connection/request lifecycle with a terminal close frame | `graceful_drop` + `HostNoticeKind::LinkDropped` (87-D3) | Mirror: dsh closes a control channel with a terminal status, not a bare EOF. Anchor: 05 sec 5 (framed JSON-RPC methods and notifications) and 02 sec 5.7 (subscription end on session close; the session-scoped `end_subscriptions` at `src/transport/protocol_server.cpp:791-808` is the template for 87's all-subscriptions companion). |
| Typed error propagation across a process boundary | `ConnectionError` + `is_link_loss()` + `Dead` routing (87-D4) | Non-mirror refinement: dsh is in-process, so a transport fault and a service error are the same C++ exception. ymh crosses a Unix-socket boundary, where an established link's death is a distinct, reconnectable condition from an RPC error, so it needs an explicit link-loss class. The divergence carries its reason (an out-of-process daemon per workspace; `docs/design/04-workspace-host-daemon.md:3` -- "WorkspaceHost is the per-workspace daemon process") and a concrete anchor: `src/ui/supervisor_connection.cpp:224-273` (two-phase attach/reconnect). |
| No cross-workspace ownership metadata to corrupt | ownership-integrity check deferred to `docs/design/88-doctor-ownership-integrity-errata.md` (87-D5) | Non-mirror: dsh owns an in-process runtime with no per-workspace registry row and no on-disk per-workspace lease, so a "claim whose pid's root is another workspace" cannot arise. ymh persists a shared `registry.db` (`src/registry/registry.cpp:1061-1087`) and a per-workspace sidecar lease (`src/session/session_persistence.cpp:821-834`), so the integrity check is required; 87 pins only its flag-not-kill invariant and routes the design (a read-only `ymh doctor` flag plus an explicit `ymh workspace reap`) to spec 88. Reason is an architectural absence in dsh, not an omission. |

## 10. Test plan

Hermetic unless marked live. New tests land in
`tests/unit/errata87_*` and `tests/integration_errata87_*`, wired into
`tests/CMakeLists.txt`; every test names a real seam (no private-internal
reach-around beyond the test seams the tree already exposes: injected
`HostLauncher`, in-process `foreground` daemon, `FakeLLM`).

| Test | Seam | Asserts |
|---|---|---|
| `Ui87.NamesLiveDaemonPredicate` | `protocol::names_live_daemon` (pure) | workspace+boot match => true; workspace match + non-empty boot mismatch => false; workspace match + empty expected => true (conservative accept); workspace mismatch => false (87-I1) |
| `Transport87.ProbeBootMismatchIsResidue` | `probe_existing_socket` against an in-process `ProtocolServer` with a scripted `HelloResult` | same-workspace/different-boot with non-empty expected => `Residue`; matching boot => `LiveYmhDaemon`; **empty expected + workspace match => `LiveYmhDaemon` (never `Residue`, M1)** (87-I2, 87-F1/F2) |
| `Transport87.ProbeRefusesLivePeer` | in-process live daemon claiming the expected boot | `LiveYmhDaemon` and no `unlink` of `host.sock` (87-F3) |
| `Host87.PredecessorBootThreaded` | `HostLifecycle` with a fake launcher capturing `HostConfig` / argv | `predecessor_boot_id` equals the pre-reap claim (including on the reaped path); empty only on the no-claim path (`record->host` absent) (87-I3, M1) |
| `Protocol87.SupersedeIsRoleAware` | two `HostConnection`s, same instance, different roles, to one in-process daemon | the same-role duplicate is dropped; the cross-role connection survives (87-I5, 87-F5) |
| `Presence87.ProbeIdentityIsObserver` | `ui::probe_attach_identity()` | role is `Observer`, instance differs from `process_client_instance()`, and two calls differ (87-I4/I6) |
| `Ui87.Err87ProbeDoesNotEvictLiveLink` | in-process daemon + a live `SupervisorConnection` + an `ensureRunning` probe with `probe_attach_identity()` | the live link completes `session.create` after the probe; the probe is not counted in `live_supervisors` (87-F4/F6) |
| `Protocol87.GracefulDropSendsFrames` | in-process daemon; a `hello_done` client with one subscription; trigger a supersede | the client reads `event.unsubscribed` then `host.event{link_dropped}` before EOF (87-I7, 87-F7) |
| `SupervisorConn87.EpipeBecomesDeadAndReconnects` | a fake socket server that accepts `hello`, answers one request, then closes; a healthy second listener | the in-flight reply error is `"connection lost"`, state reaches `Dead`, then re-attaches to the healthy listener (87-I8/I9, 87-F8/F9) |
| `SupervisorConn87.TimeoutIsNotLinkLoss` | a fake peer that accepts `hello` then stalls (triggers `request()`'s **read** wait, `host_connection.cpp:240-243`) | the reply is the classified `Timeout` token (not the raw `"timed out waiting for a response"`, not `"connection lost"`), the link stays `Attached`, and **no reconnect** occurs (87-I8, 87-F10, N1) |
| `HostConnection87.ReadWaitThrowsTimeoutKind` | a fake peer that accepts `hello` then stalls; a raw `HostConnection` | `request()` throws `ConnectionError` with `kind() == Timeout` and `is_link_loss() == false` (N1) |
| `Ui87.NoRawTransportStringInCreateNotice` | driver injects a link-loss reply for `session.create` (`install_method_error_reply`-style seam) | the surfaced notice contains neither `"host connection:"` nor `"Broken pipe"` (87-F12, 87-I9) |
| `Transport87.LinkDroppedWireRoundTrip` | `to_json`/`from_json` + `wire_name`/`parse_host_notice_kind` | `LinkDropped` <-> `"link_dropped"` (87-D6) |
| `UiEventAdapter87.LinkDroppedCase` | `UiEventAdapter::onHostNotice` with a `LinkDropped` notice | the exhaustive switch compiles under `-Wswitch -Werror` and the case is handled (no-op or 69-channel notice) (L3, 87-I7) |
| `Protocol87.EndAllSubscriptions` | in-process daemon, a connection with two subscriptions in two sessions | `end_all_subscriptions` enqueues one `event.unsubscribed` per subscription (vs the session-scoped `end_subscriptions`) (L2) |
| `HostConnection87.WritePeerCloseIsLinkLoss` | a fake peer that triggers a write-side `POLLHUP` after `hello` | the fault is `PeerClosed` with `is_link_loss() == true` (L9, 87-I8) |
| `Protocol87.LinkDroppedOldClientCompat` | a raw pre-87 client that parses `HostNotice` strictly | `from_json` throws on `link_dropped`; the documented, benign behavior of 87-F13 (L1) |
| `Transport76.ProbeSignatureUpdated` (updates an existing test) | `tests/unit/errata76_startup_residue_test.cpp:305`, `:331` | these 3-arg `probe_existing_socket(path, workspace, deadline)` calls are updated in the SAME change set to the 4-arg signature, compiled before the new `errata87_*` target (L6) |
| `E2E87.RestartStormKeepsLink` (integration, two-process harness) | real daemon + supervisor, fake LLM | a bare-`ymh` attach followed by a `session.create` succeeds; a second probe does not break the link; no `Broken pipe` (87-D8) |
| `Live87.RealDaemonReclaim` (opt-in: `YMH_LIVE_LLM=1` + `DEEPSEEK_API_KEY`) | real `ymh --host` | a stale same-workspace socket is reclaimed; a live link survives a probe; no ownership flag fires on a clean workspace (87-D8) |

## 11. Open questions and recorded risks

- **OQ-87-1.** The split-lock residual (76-I13): under an inode split the
  prober holds a different `sessions.lock` inode, so it cannot locally prove "no
  live owner". This matters **only** when the expected nonce is non-empty and a
  live predecessor answers a boot that disagrees with the (possibly corrupt)
  registry claim -- then `Residue` unlinks its socket. The empty-nonce case is
  fully closed by the conservative rule (87-D1/87-F2): it never authorizes a
  reclaim. A fully robust non-empty guard would need a same-inode proof; that is
  the same residual 76 already accepts, and the corrupt-claim trigger is the G4
  case `ymh doctor` flags. Recorded, not a `verified` blocker.
- **OQ-87-2.** `describe_connection_loss` wording is owned by 69's presentation
  rules; 87 pins only that it is bounded, ASCII, and never the raw `strerror`
  text.
- **RISK-87-1.** The boot-checked reclaim (non-empty nonce) means a same-workspace
  daemon whose boot differs from the predecessor claim is reclaimed rather than
  protected. This is the intended G1 change and the same signal
  `connect_checked`/`attempt_attach` reject on; it is safe under a single lock
  inode. Under the split-lock + corrupt-claim combination above it can unlink a
  live daemon's socket -- recorded as the OQ-87-1 residual, owned by the G4
  integrity check, not silently accepted. The **empty-nonce** path is excluded
  from this risk by construction (87-F2).

## 12. Revision log

| Rev | Change |
|---|---|
| 1 | Initial draft. G1 boot-checked reclaim probe (87-D1, supersedes 76-D6); G2 non-owning probe identity and role-aware supersede (87-D2); G3 close protocol and link-loss routing (87-D3/87-D4); G4 ownership-integrity flag-not-kill referenced to spec 88 (87-D5). |
| 2 | Rev 1 gate fixes. M1: `names_live_daemon` now treats an empty expected nonce as workspace-only **accept/refuse**, never `Residue`, so the empty case cannot unlink a live daemon; pinned exactly when the nonce is set (any claim, including reaped via the pre-reap copy) vs empty (no-claim path only); softened the O11 claim and rewrote OQ-87-1/RISK-87-1. L1: analyzed `LinkDropped` vs an old client (`kProtocolVersion` stays 1; 87-F13). L2: added `end_all_subscriptions` (the existing `end_subscriptions` is session-scoped). L3: named `UiEventAdapter::onHostNotice` as the consumer. L4: fixed the 87-A1 anchor (`protocol_server.hpp:123`, not `:179`). L5: replaced the invalid dsh anchor with `04-workspace-host-daemon.md:1`. L6: accounted for the existing 76 probe tests. L7: reconciled `ConnectionError`/`describe_connection_loss` home (host_connection.hpp) and callers (`process_requests`, `maybe_ping`). L8: folded `attempt_attach` into the predicate (three sites, one notion). L9: mapped the write-path `POLLHUP` to `PeerClosed`. L10: fixed the `HostConfig::predecessor_boot_id` anchor (`:151`). |
| 3 | Rev 2 gate fixes. N1: brought `request()`'s read wait into the taxonomy -- the `fill_incoming`-false throw at `host_connection.cpp:240-243` becomes `ConnectionError(Timeout)` (was `RpcException(InternalError)`); pinned `Timeout` as non-link-loss (no reconnect), that `wait_readable`/`fill_incoming` keep returning `bool`, `nextNotification` stays `nullopt`, and decode/server-error stay `RpcException`; restated 87-I8, 87-F10, and the timeout test seams. N2: dsh anchor `04-workspace-host-daemon.md:1` -> `:3`. N3: reconciled 87-D1's "only unlink path" with 87-I2 (`Refused` + `Residue` unlink; only a non-empty boot mismatch can reclaim a *live* daemon). N4: named the shutdown caller (`kHostShutdown` `:364-377`, `onDaemonShuttingDown` `:946-962`) for `graceful_drop`'s drain path. |

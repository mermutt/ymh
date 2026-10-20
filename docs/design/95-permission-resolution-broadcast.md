# 95 - Permission prompt resolution broadcast (multi-UI)

```
Status: verified (Rev 7)
Revision: 7
Verification status: adversarial reviewer (independent, Rev 7): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 95 (feature) -- team permission prompts are broadcast to every
           subscribed UI, but the resolution is not. Rev 5 carries the Rev 2-4
           fixes (two-phase publication with no virtual under the broker mutex,
           95-D3a; the session-keyed, state-gated dialog close, 95-D6; no
           transport calls from teardown, 95-D17; containment inside
           `State::resolve`, 95-D18) and fixes the last two gaps: the notice
           carries the winning answer *and* scope (95-D1/95-D3), and the
           unreachable "internal" site is removed (the notice post is
           `noexcept`, 95-D15).
Depends on: 05-transport.md (verified; sec. 7.6 notifications, OQ-4),
           09-permissions.md (verified; sec. 4 decision flow + fan-out),
           11-m2-errata.md (verified; sec. 7.2 PermissionTransport port),
           25-ui-ux-errata.md / 46-permissions-ui-errata.md (dialog),
           58 (sessions), 56-dsh-fidelity-gap-closure-errata.md (56-D6).
Amends: 05 sec. 7.6 (adds `permission.resolved`; closes the resolution half of
        OQ-4), 05 OQ-4 (`:1680-1682`), 09 sec. 4.4 (Automation half -- see
        95-D2b), 11 sec. 7.2 (third port method, with a default body).
Tracker: a `DESIGN_STATUS.md` row is required before commit.
```

## 1. Problem

A workspace daemon serves several attached supervisors (two `ymh` windows, or a
supervisor plus a reattached client). When a tool call hits `Ask`, the daemon
broadcasts `permission.request` to every subscribed Interactive client
(`src/transport/protocol_server.cpp:1026-1041`). Both windows open the dialog.

The user answers in window A. Window A closes its dialog and sends
`permission.decide` (`src/ui/supervisor.cpp:633-645`). The daemon resolves the
broker's future (`src/permission/permission_broker.cpp:335-348`) and the turn
resumes. Window B is told nothing: the notification namespace has no resolved
kind (`include/ymh/transport/protocol.hpp:636-642`), and the broker never calls
the transport after a resolution.

Consequences:

1. Stale modal: window B blocks its composer on a request that no longer
   exists.
2. A second answer is a dead end: the daemon returns `InvalidParams` for an
   unknown `request_id` (`src/transport/protocol_server.cpp:544-549`), and the
   UI drops it (the reply callback is `nullptr`, `supervisor.cpp:642-643`), so
   the choice silently does nothing.
3. "First decision wins" (`05-transport.md:998-999`) is implemented
   (`claim()` at `permission_broker.cpp:189-204` is a first-wins swap), but the
   losing window is never shown that it lost.

This is a missing fan-out for the resolution, not a missing request broadcast.

## 2. Decisions

### 95-D1 - New additive notification `permission.resolved`

```cpp
namespace ymh::protocol {

namespace notify {
inline constexpr std::string_view kPermissionResolved = "permission.resolved"; // 95-D1
}

// 95-D1. Sent to every client that received the matching `permission.request`.
// `request_id` + `session` are the correlation keys; `decision`/`scope` are the
// winning answer; `reason` is a bounded diagnostic token (95-D12), never
// rendered as an error.
struct PermissionResolvedNotice {
    std::string      request_id;
    SessionId        session;
    PermissionAnswer decision{PermissionAnswer::Deny};
    PermissionScope  scope{PermissionScope::Once};
    std::string      reason;
};

} // namespace ymh::protocol
```

Wire JSON (mirrors `PermissionDecisionParams`, `src/transport/protocol.cpp:555-570`):

```text
server -> client   permission.resolved  params: { request_id, session, decision,
                                                  scope, reason }
```

`decision`/`scope` reuse `wire_name(PermissionAnswer)` / `wire_name(PermissionScope)`
and the existing strict parsers; `reason` is lenient (default `""`).

### 95-D2 - One fan-out predicate, shared by request and resolution

`ProtocolServer::onPermissionRequest` inlines its filter (dropped? hello done?
Interactive? subscribed?). Extract a private predicate and use it from both
sites, so the halves cannot drift:

```cpp
bool ProtocolServer::is_subscribed_interactive(const Connection& conn,
                                               const SessionId& session) const;
```

Behavior-preserving; the existing test
`tests/unit/transport_server_test.cpp:730-761` pins the request half.

### 95-D2b - Supersession: Automation does not get permission notices

`docs/design/09-permissions.md:701-703` says every client "subscribed
(Interactive) **or attached (Automation)**" receives `permission.request`. The
shipped code sends only to Interactive subscribers
(`protocol_server.cpp:1029-1039`), and `transport_server_test.cpp:730-761`
pins that. 95 codifies the shipped behavior: Automation has no dialog (it is
the headless/automation profile; a prompt there would be an unattended block),
so it receives neither the request nor the resolution. This supersedes the
Automation half of 09 sec. 4.4 with that reason (56-D6).

### 95-D3 - Exactly one resolution notice per *delivered* request

A request is *delivered* only when the broker published a `Pending` entry **and**
`broadcast_permission_request` returned `true` (95-D3a). A notice is emitted at
every site that resolves such an entry, and at no other site:

| Site | Anchor | Reason token | Decision / scope |
|---|---|---|---|
| user decision | `permission_broker.cpp:335-348` | `"user"` | `params.decision` / `params.scope` verbatim |
| cancellation | `:258-265` | `"cancelled"` | `Deny` / `Once` |
| timeout | `:267-279` | `"timeout"` | `Deny` / `Once` |

There is no `"internal"` notice site (Rev 5 had one; it is unreachable: a
pre-post throw has `request_posted == false`, and a post-post throw can only
come from the notice post itself, which is now `noexcept` -- 95-D15). A throw
inside `State::resolve` is contained by 95-D18 and never notifies.

The notice carries the wire `answer` **and** `scope` separately, not a
`payload::PermissionDecisionKind`: that enum collapses `Allow+Session` to
`Allow` and all `Deny` scopes to `Deny` (`permission_broker.cpp:148-157`), so a
coarse kind would lose the winning scope that T3 asserts. A throw **before** the
post is an undelivered request and emits nothing (95-I3).

**No notice** for a request that was never delivered: policy `Allow`/`Deny`
(`:229-234`), pre-cancelled (`:235-237`), no subscribers (`:238-240`),
`shutting_down` (`:246-248`), a failed `schedule_after` (`:280-286`), or a
failed request post (`:289-294`). No dialog was ever created for those, so there
is nothing to dismiss.

**Emission point.** The notice is emitted at the resolution site **after**
`promise.set_value` and wrapped in `try/catch (...)`: the turn's resumption must
never be blocked or skipped by notice machinery (95-D15). Stream order is
therefore not guaranteed to put the notice before the turn's committed events;
the UI is intentionally order-insensitive (95-D6b requires both an open matching
dialog and a still-waiting session before it transitions state). Exactly-once is
guaranteed by `claim()` (first-wins): only the thread that claims resolves and
notifies.

### 95-D3a - Two-phase publication (request before notice; no deadlock)

The entry becomes claimable at `:253`, while the request is posted at `:289`; a
cancel (`:258-265`) or timeout (`:267-279`) can therefore claim it first, and a
naive "hold the mutex across the post" fix deadlocks: the cancel hook is invoked
inline when the token is already cancelled
(`include/ymh/core/cancellation.hpp:50-68`), and `claim()` takes the same
non-recursive mutex (`permission_broker.cpp:189-190`). Rev 3 uses a two-phase
publication instead, and never calls a virtual under `State::mutex`.

`Pending` gains `bool request_posted = false;` and
`std::optional<protocol::PermissionResolvedNotice> deferred_notice;`.

Phase 1 (under `State::mutex`): create the entry with `request_posted=false` and
insert it (`:242-254`). Nothing else changes.

Phase 2 (after `get_future`, `on_cancel`, and `schedule_after`, replacing
`:289-294`):

```cpp
bool already_resolved = false;
{
    std::lock_guard<std::mutex> lock(mutex);
    already_resolved = entry->resolved;
}
if (already_resolved) { return future; }                 // no post, no notice

const bool posted = transport.broadcast_permission_request(entry->wire); // no lock
std::optional<protocol::PermissionResolvedNotice> flush;
{
    std::lock_guard<std::mutex> lock(mutex);
    entry->request_posted = posted;
    if (posted && entry->deferred_notice.has_value()) {
        flush = std::move(entry->deferred_notice);
    }
    entry->deferred_notice.reset();
}
if (flush.has_value()) { transport.broadcast_permission_resolved(*flush); }
if (!posted) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!entry->resolved) { /* erase + resolve Deny "transport", no notice */ }
}
```

`notify_resolved` reads `request_posted` under the mutex: if true it posts the
notice immediately (the request post has already returned, so the notice post is
later on the same io queue); if false it stores `deferred_notice` and returns.
The phase-2 flush posts it after the request, preserving request-before-notice.
`notify_resolved` never throws: its transport call is wrapped in
`try/catch (...)`, so a subscription or notice failure cannot change the
resolution outcome (95-D15/95-I9).

Consequences:

* No virtual call is made under `State::mutex`; a synchronous or re-entrant test
  fake cannot deadlock (95-M1), and the frozen port contract
  (`11-m2-errata.md:913-918` D19.5) is unchanged.
* A client never receives a notice that overtakes the request (95-I10), and a
  request that was never delivered gets no notice (95-I3).
* A claim that wins the race defers its notice; a failed post drops it.
* `entry->resolved` is set by `claim()` under the mutex; the phase-2 pre-check
  and the claim are ordered by that mutex, and any claim after the check defers.

### 95-D4 - `PermissionTransport` gains a third method with a default body

```cpp
// Broadcast `permission.resolved` with the same predicate as
// `broadcast_permission_request` (95-D2). Best-effort: false means the
// transport is not running; the request is already resolved (95-D15).
// The default keeps the ten test fakes compiling (95-M1); production
// implementors MUST override it.
virtual bool broadcast_permission_resolved(protocol::PermissionResolvedNotice) {
    return false;
}
```

Production overrides:

* `TransportServerAdapter` (`src/host/workspace_host.cpp:68-105`) posts
  `server->onPermissionResolved(std::move(notice))` onto io, like the request
  path (`:75-84`).
* `HeadlessPermissionTransport` (`src/cli/headless.cpp:108-115`) returns
  `false` (explicit override kept for readability; the subscriber count is 0,
  so no `Pending` is ever created).

The broker calls the method only from the three 95-D3 resolution sites and the
phase-2 flush, never from `denyAll` or the destructor (95-D17).

### 95-D5 - UI sink and dispatch

`SupervisorSink` (`src/ui/supervisor_connection.hpp:86-99`) gains:

```cpp
std::function<void(const protocol::PermissionResolvedNotice&)> on_permission_resolved;
```

Dispatch in the notification if-chain (`supervisor_connection.cpp:417-429`)
before the `kHostEvent` branch; unknown names keep falling through (95-D9). The
supervisor wires it next to `on_permission` (`supervisor.cpp:973-977`), enqueues
on the UI loop, and calls

```cpp
adapter_.onPermissionResolved(notice.session,
                              PermissionRequestId{notice.request_id},
                              decision_kind(notice.decision, notice.scope));
```

`decision_kind` is the three-line mapping already used by the broker
(`permission_broker.cpp:148-157`), duplicated in the UI so the dependency stays
one-way (the UI never includes a broker-private helper).

### 95-D6 - Close only the matching, still-open dialog

`UiModel` (`src/ui/ui_model.cpp:1840-1858`) stores `dialog.session` on open but
the close guard compares only `dialog.request`. Two fixes:

**(a) Session key.** Request ids are minted per daemon as `perm-N`
(`permission_broker.cpp:249`), so two workspaces both have `perm-1` (05
sec. 7.6's "UUIDv4" comment is not what ships). The dialog is a single global
model, so a notice from workspace A would close workspace B's dialog. Guard:

```cpp
if (dialog.open && dialog.session == e.session && dialog.request == e.request) {
    const UiMode prev = dialog.prev_mode;
    dialog.open = false;
    mode = prev;
    state.attention.needsInput = false;
}
dirty.mark(e.session, UiDirtyFlag::Attention);
```

`state.attention.needsInput = false` moves **inside** the guard: a stale notice
must not clear a newer prompt's attention, and the `open` check also prevents a
duplicate from re-assigning `prev_mode` (so a modal opened in between survives).

**(b) No state transition without a close.** `UiEventAdapter::onPermissionResolved`
(`src/ui/ui_event_adapter.cpp:415-425`) unconditionally emits
`AgentStateChanged -> Thinking`, which would overwrite `Idle` when a duplicate
arrives after the turn ended. Pin:

```cpp
void UiEventAdapter::onPermissionResolved(const SessionId& session,
                                          const PermissionRequestId& id,
                                          payload::PermissionDecisionKind decision) {
    const bool was_open = model_.permission_dialog_open_for(session, id);
    const auto state = lastState_.find(session);
    const bool waiting = state != lastState_.end() &&
                         state->second == AgentState::WaitingForPermission;
    applyAndMark(UiEvent{PermissionResolved{session, id, decision}});
    if (was_open && waiting) {
        applyAndMark(UiEvent{AgentStateChanged{session, AgentState::WaitingForPermission,
                                               AgentState::Thinking}});
    }
}
```

New model symbol (with the caller above):

```cpp
// ui_model.hpp / ui_model.cpp
bool UiModel::permission_dialog_open_for(const SessionId& session,
                                         const PermissionRequestId& id) const {
    return dialog.open && dialog.session == session && dialog.request == id;
}
```

A duplicate or stale notice is then a pure no-op for agent state (95-I4). The
`waiting` clause makes the transition robust to stream order: if the turn's
committed events already moved the session to `Idle`, a late notice cannot
resurrect `Thinking` (95-F1).

### 95-D7 - A late subscriber never sees a resolved request

`HostRuntime::onSubscriberAttached` re-sends `broker_.pending(session)`
(`src/host/host_runtime.cpp:419-427`), and `pending()` returns only unresolved
entries (`permission_broker.cpp:350-360`). A client attaching after resolution
receives neither the request nor a notice (95-I7). No replay of the notice is
allowed: it is a live notice, not an event.

### 95-D8 - A losing answer is benign

Two concurrent answers: `claim()` is first-wins; the loser's `permission.decide`
gets `InvalidParams` (`protocol_server.cpp:544-549`). The UI treats it as
benign: the winner's notice already closed the dialog (95-D6), and the `nullptr`
callback already swallows the error (`supervisor.cpp:642-643`). No banner, no
re-send (95-F6).

### 95-D9 - Additive wire; `kProtocolVersion` stays 1

New notification name; clients dispatch by name and ignore unknown names
(`supervisor_connection.cpp:417-429`; `src/cli/cli.cpp:637-648` `else { continue; }`).
A pre-95 client keeps the old stale-dialog behavior but never mis-parses. Follows
the 87-D6 precedent (`protocol.hpp:263-266`).

### 95-D10 - Session and workspace keying

A notice carries `session`; the UI applies it to that session and closes only a
dialog whose `dialog.session` matches (95-D6a). `request_id` is the primary key;
`session` is the ownership key that prevents cross-workspace collisions.

### 95-D11 - The deciding UI keeps its local close

`resolvePermission` calls `adapter_.onPermissionResolved` immediately
(`supervisor.cpp:644`) and does not wait for the round-trip. The broadcast then
arrives as a duplicate and is a no-op (95-D6b). This keeps the deciding window
responsive on a slow link.

### 95-D12 - `reason` is diagnostic only, bounded to 64 bytes

The broker's existing tokens: `"user"`, `"cancelled"`, `"timeout"`, `"internal"`.
Bounding rule (single, testable): if longer than 61 bytes, keep the first 61 and
append `"..."`, so the result is **at most 64** bytes; otherwise unchanged.
Never rendered as an error; the durable `permission/decision` event and the
agent's `ToolOutcome` are authoritative.

### 95-D13 - Agent-state parity

The remote path applies the same conditional transition as the local path
(95-D6b): `WaitingForPermission -> Thinking` only when the dialog was actually
open. A remote resolution therefore cannot strand a session in
`WaitingForPermission`, and cannot resurrect `Thinking` after `Idle` (95-I8).

### 95-D14 - No new UI chrome in Rev 2

The remote window closes the dialog and lets the stream tell the story. A
transient "answered in another window" toast is not added: no generic
transient-notice surface exists that is safe from this path. Recorded as
possible future polish.

### 95-D15 - Best-effort delivery

The request is already resolved when the notice is posted; a failed post must
not change the outcome, block, or throw. The broker ignores the return value
(unlike the request post, whose failure resolves fail-closed).

### 95-D16 - Link loss and session close clear the matching dialog

Teardown resolves pending requests **without** notices (95-D17), and a dropped
socket cannot deliver them. The UI must not rely on the notice for those paths:

* `on_link_state` (`src/ui/supervisor.cpp:2451-2464`): for `Detached`/`Dead`,
  call `model_.closePermissionDialogForWorkspace(workspace)`.
* `SessionClosed` handling (`:1776-1787`): call
  `model_.closePermissionDialogForSession(*notice.session)`.

```cpp
// ui_model.cpp. Both restore prev_mode exactly like the 95-D6a close; they
// clear needsInput only when they actually close (95-I6).
void UiModel::closePermissionDialogForSession(const SessionId& session) {
    if (!dialog.open || dialog.session != session) { return; }
    const UiMode prev = dialog.prev_mode;
    dialog.open = false;
    mode = prev;
    const auto it = sessions.find(session);
    if (it != sessions.end()) { it->second.attention.needsInput = false; }
    dirty.mark(session, UiDirtyFlag::Attention);
}

void UiModel::closePermissionDialogForWorkspace(const WorkspaceId& workspace) {
    if (!dialog.open) { return; }
    const auto it = sessions.find(dialog.session);
    if (it == sessions.end() || it->second.workspace != workspace) { return; }
    closePermissionDialogForSession(dialog.session);
}
```

`SessionUiState::workspace` (`ui_model.hpp:405`) is the in-model ownership key;
the dialog is a single global model, so both checks are required. This is the backstop that makes the original bug impossible even when
delivery fails (95-F11).

### 95-D17 - Teardown never calls the transport

`~PermissionBroker` calls `denyAll("shutdown")` (`permission_broker.cpp:304`),
and `cleanupStartupFailure` destroys `transport_`/`protocol_` **before**
`broker_` (`workspace_host.cpp:1120-1126`; member order `:693-698`). A notice
from the destructor would call `transport_->post` on a destroyed object: UB.

Pin: `denyAll` (and therefore the destructor) resolves pending entries and
emits **no** notices. The coordinator's orderly shutdown calls `denyAll` after
`waitForDrain` (`workspace_host.cpp:1030-1038`), so a notice there would be
undeliverable by construction anyway. Teardown is covered in the UI by 95-D16.
Add the invariant: no transport call may occur on the destructor path.

### 95-D18 - Failures inside `State::resolve` must not orphan a pending entry

`PermissionBroker::resolve` (`:325-333`) wraps `State::resolve` in a `try/catch`
returning `Deny{"internal"}`. If anything after publication (`:253`) throws, the
entry stays in `pending` and the turn resumes while a later attach re-delivers a
request whose turn already ended (95-I7). Containment therefore moves **inside**
`State::resolve`, where `id` and `entry` are in scope:

```cpp
std::future<PermissionOutcome> PermissionBroker::State::resolve(...) {
    // ... up to publication (:242-254) ...
    std::future<PermissionOutcome> future = entry->promise.get_future();
    try {
        // on_cancel, schedule_after, phase 2 (95-D3a)
    } catch (...) {
        std::shared_ptr<Pending> claimed;
        { std::lock_guard<std::mutex> lock(mutex); claimed = claim_locked(id); }
        if (claimed) {
            claimed->promise.set_value({payload::PermissionDecisionKind::Deny, "internal"});
        }
        return future;   // no notice: a pre-post failure was never delivered
    }
}
```

Pin: (a) `id` and `entry` are function-scoped so the catch sees them;
`get_future` is hoisted before the `try`, so the catch always has a valid future
to return; (b) `claim_locked(id)` is a new private no-lock variant of `claim`
(the existing `claim` takes `mutex`; calling it under the lock would deadlock),
and `claim()` becomes `lock + claim_locked`; (c) the catch **does not notify**: a reachable throw precedes
`request_posted == true` (never delivered, T14 pins zero notices), and a
notice-post failure is swallowed by 95-D15 instead of reaching the catch;
(d) the outer
`PermissionBroker::resolve` catch stays as a last resort and is not the
mechanism.

## 3. Interfaces

```cpp
// include/ymh/transport/protocol.hpp
namespace notify {
inline constexpr std::string_view kEventUnsubscribed  = "event.unsubscribed";
inline constexpr std::string_view kPermissionRequest  = "permission.request";
inline constexpr std::string_view kPermissionResolved = "permission.resolved"; // 95-D1
}

struct PermissionResolvedNotice { std::string request_id; SessionId session;
                                  PermissionAnswer decision{PermissionAnswer::Deny};
                                  PermissionScope scope{PermissionScope::Once};
                                  std::string reason; };

// include/ymh/permission/permission_transport.hpp
virtual bool broadcast_permission_resolved(protocol::PermissionResolvedNotice); // default false

// include/ymh/transport/protocol_server.hpp
void onPermissionResolved(const PermissionResolvedNotice& notice);
bool is_subscribed_interactive(const Connection&, const SessionId&) const;

// include/ymh/ui/supervisor_connection.hpp
std::function<void(const protocol::PermissionResolvedNotice&)> on_permission_resolved;

// permission_broker.cpp (private)
std::shared_ptr<Pending> claim_locked(const std::string& id);   // caller holds mutex
void notify_resolved(const std::shared_ptr<Pending>& entry,
                     protocol::PermissionAnswer answer,
                     protocol::PermissionScope scope,
                     std::string_view reason) noexcept;

// include/ymh/ui/ui_model.hpp
bool UiModel::permission_dialog_open_for(const SessionId&, const PermissionRequestId&) const;
void UiModel::closePermissionDialogForWorkspace(const WorkspaceId&);
void UiModel::closePermissionDialogForSession(const SessionId&);
```

## 4. Implementation

### 4.1 Protocol

Add `kPermissionResolved` after `kPermissionRequest` (`protocol.hpp:640`), the
struct after `PermissionDecisionParams` (`:291-295`), and `to_json`/`from_json`:
`request_id`, `session.value`, `wire_name(decision)`, `wire_name(scope)`,
`reason` (default `""`); strict on decision/scope like
`PermissionDecisionParams` (`protocol.cpp:562-570`).

Out of scope: `SubscriberCount` is refreshed on attach (`host_runtime.cpp:419-427`)
but not decremented on detach in this path, so a request issued after the last
client left still waits for the timeout. That is pre-existing and orthogonal to
95-D7 (which only guarantees no stale re-delivery).

### 4.2 Transport port

Add the defaulted virtual (95-D4) with the existing threading contract.

### 4.3 Workspace adapter / headless

Post `server->onPermissionResolved(notice)` on io; headless returns false.

### 4.4 Broker

* `Pending::request_posted`.
* `State::resolve`: phase 1 insert (`:242-254`), phase 2 post + flush
  (95-D3a); a failed post resolves inline with no notice.
* `notify_resolved(entry, protocol::PermissionAnswer answer,
  protocol::PermissionScope scope, std::string_view reason)` helper: build the
  notice from the wire answer/scope (95-D3), cap `reason` (95-D12); read
  `request_posted` **under** the mutex, then post **without** it if true (the
  request post already returned), else store `deferred_notice`. It never throws
  (the post is `try/catch`-wrapped, 95-D15).
* Call it after `set_value`, inside `try/catch (...)`, at the decision, cancel,
  and timeout sites (95-D3).
* `denyAll`: resolve only; never notify (95-D17).
* `resolve` catch: claim + resolve, **no notice** (95-D18).

### 4.5 Server

Extract `is_subscribed_interactive`; implement `onPermissionResolved` with the
same loop and `notify::kPermissionResolved`.

### 4.6 Supervisor

Sink field, dispatch, wiring, enqueue.

### 4.7 UI

`permission_dialog_open_for`, the 95-D6a guard, the 95-D6b conditional
transition, and the two 95-D16 cleanup methods + call sites.

## 5. Invariants

* **95-I1 (exactly one).** One notice per delivered `request_id`; `claim`
  (via `claim_locked`) or `denyAll` is the only resolver of a delivered
  request; only the claimer (or the phase-2 flush for a deferred notice) posts
  it. Carve-out: a `denyAll` that wins between the phase-2 pre-check and the
  post resolves without a notice (teardown, 95-D17), backstopped by 95-D16. The failed-post branch resolves inline, by construction
  before any delivery, and emits no notice.
* **95-I2 (same predicate, non-teardown).** Every non-teardown resolution of a
  delivered request reaches exactly the Interactive subscribers that got the
  request (Automation never does, 95-D2b). Teardown
  (95-D17) is exempt and covered by link-loss cleanup (95-D16).
* **95-I3 (no orphan notices).** Undelivered requests (policy, pre-cancel, no
  subscribers, shutting down, failed schedule, failed post) emit none.
* **95-I4 (idempotent).** A duplicate/stale notice changes no dialog and no
  agent state (95-D6).
* **95-I5 (open + session + id).** A dialog closes only when open, same
  session, same request; a later modal is never clobbered, and a cross-workspace
  id collision is impossible.
* **95-I6 (attention).** Cleared only by the close that matches 95-I5.
* **95-I7 (no stale re-delivery).** `pending()` excludes resolved entries; a
  failed `resolve` never leaves an unresolved orphan (95-D18).
* **95-I8 (state).** A decided turn ends `Idle`, never `Thinking`: the remote
  transition requires an open matching dialog **and** a session still in
  `WaitingForPermission`, so a late notice cannot resurrect `Thinking`
  (95-D6b).
* **95-I9 (resolution authoritative).** A failed notice post never changes the
  outcome or blocks.
* **95-I10 (request before notice).** The request post precedes every notice for
  that id: a notice is posted only once `request_posted` is true, and a
  pre-post resolution defers to the phase-2 flush (95-D3a).
* **95-I11 (no persistent state).** Transient notice; no file, row, or event.
* **95-I12 (additive).** `kProtocolVersion` stays 1; unknown names ignored.
* **95-I13 (destructor safety).** No transport call on the broker destructor
  path (95-D17).

## 6. Failure modes

| ID | Tag | Scenario | Required behavior |
|---|---|---|---|
| 95-F1 | F3 | Duplicate/stale notice after another modal opened, or after the turn reached `Idle` | No-op: close requires open + session + id; state transition additionally requires `WaitingForPermission` (95-D6) |
| 95-F2 | F3 | Request/resolution predicate drift | Shared predicate (95-D2) + fan-out test |
| 95-F3 | F3 | Notice for an undelivered request | Forbidden (95-D3/95-I3); covered by T4 |
| 95-F4 | F9 | Timeout after decision | `claim()` null; one notice; one `set_value` |
| 95-F5 | F9 | Transport stopping at notice time | `false` ignored (95-D15) |
| 95-F6 | F9 | Two concurrent answers | First wins; loser gets `InvalidParams`, swallowed; both dialogs close |
| 95-F7 | F1 | Two workspaces, same `perm-N` | Wrong dialog untouched (95-D6a) |
| 95-F8 | F9 | Cancel fires before the request post | Pre-post claim defers the notice; the flush preserves request-before-notice (95-D3a) |
| 95-F9 | F8 | Long `reason` | `<= 64` bytes (95-D12) |
| 95-F10 | F3 | Pre-95 client receives the notice | Ignored by name (95-D9) |
| 95-F11 | F3 | Socket drops before the notice | Link-loss cleanup closes the dialog (95-D16) |
| 95-F12 | F9 | Throw inside `State::resolve` after publication | Catch claims + resolves the captured `id`; no orphan (95-D18) |
| 95-F13 | F9 | Destructor with a pending entry | No transport call (95-D17); no UB |
| 95-F14 | F4 | Stale notice while a newer prompt is pending | Attention is cleared only by the matching close (95-I6); no `Thinking` |

Tag key: F1 = workspace isolation (cross-workspace id), F3 = late/duplicate
event after close, F4 = edge-triggered attention, F8 = resource cap (`reason`
bound), F9 = cancellation/timeout/teardown scoping.

## 7. State lifetime

| State | Created | Destroyed | Owner | Survives restart? |
|---|---|---|---|---|
| `Pending` (+ `request_posted`) | `State::resolve` (`:242-254`) | first `claim()` (`:189-204`) / inline failure / `denyAll` (`:314-318`) | broker `State` | no |
| Notice | at a 95-D3 site | after the io post; briefly held as `deferred_notice` only in the pre-post race (95-D3a) | broker -> transport -> `ProtocolServer` | no |
| `lastState_` mirror | first event per session | session removal | `UiEventAdapter` | no |
| UI dialog (`open`, `session`, `request`) | `PermissionRequested` (`:1840-1850`) | matching close (95-D6) or link/session close (95-D16) | `UiModel` | no |
| `attention.needsInput` | `PermissionRequested` | matching close (95-I6) | `UiModel` | no |
| Durable `permission/decision` | agent loop on resume | never | session log | yes |

Ownership order (95-D17): `broker_` is destroyed after `transport_`/`protocol_`;
therefore the broker must not touch the transport during destruction. The
adapter outlives both only in declaration order, not in reverse destruction
order, so it must not be called on that path either.

## 8. Tests

1. **T1 round-trip** - all decision/scope pairs; `reason` absent -> `""`;
   unknown decision/scope throws; a 100-byte reason is capped at 64.
2. **T2 fan-out** - A/B subscribed, C unsubscribed, D Automation: request and
   resolution reach A and B only; exactly one each.
3. **T3 delivered sites** - decision, cancel, timeout: one notice each with
   the right decision/scope/reason; the notice is posted after `set_value` (the
   fake transport records the order).
4. **T4 undelivered sites** - policy allow/deny, pre-cancel, no subscribers,
   shutting down, failed schedule, failed post: zero notices.
5. **T5 first-wins** - decision+timeout in either order: one `set_value`, one
   notice.
3b. **T3 scope fidelity** - a decision `Allow`/`Session` produces a notice with
   `decision=allow, scope=session` (not collapsed to `Allow`/`Once`).
6. **T3a ordering** - cancel the turn immediately after the entry is created:
   the fake transport asserts the request post strictly precedes the notice
   post (deferred flush), or that neither is posted; never notice-first. Also a
   fake whose `broadcast_permission_request` re-enters `claim()` must not
   deadlock (the port is called without the mutex).
7. **T6 UI guard** - duplicate on a closed dialog: no-op; stale notice with a
   different session but the same id: no-op; matching open: closes and restores
   `prev_mode`.
8. **T6b state** - after a decided turn the session is `Idle`, not `Thinking`
   (final-state assertion); a notice arriving after the turn's committed events
   does not resurrect `Thinking` while the dialog is still open (the `waiting`
   clause); a stale notice does not clear a newer prompt's attention.
9. **T8 dispatch** - `permission.resolved` reaches the sink; unknown names
   ignored.
10. **T9 end-to-end** - two clients, A decides: B's dialog closes and B ends
    `Idle`.
11. **T10 losing answer** - B answers after A: `InvalidParams`, no error
    surface, both closed.
12. **T11 late attach** - no request, no notice.
13. **T12 headless** - no notice, auto-deny unchanged.
14. **T13 teardown** - destroying the broker with a pending entry makes no
    transport call (fake transport asserts zero calls) and does not crash; also
    exercise `cleanupStartupFailure` order (`workspace_host.cpp:1120-1126`).
15. **T14 resolve throw** - a throwing `schedule_after`: no orphan pending
    entry; one `"internal"` outcome; **zero** notices (the throw precedes the
    post). A throwing notice post is swallowed and leaves the outcome intact
    (95-D15).
16. **T15 link loss** - Detached/Dead closes the open dialog; `SessionClosed`
    closes that session's dialog.

## 9. Amendments and supersession

* **05 sec. 7.6 (`:955-999`)** - adds `permission.resolved`; "first decision wins
  and later ones are ignored" (`:998-999`) is completed: later decisions are
  ignored **and** every client learns the winner.
* **05 OQ-4 (`:1680-1682`)** - the resolution half is pinned here.
* **09 sec. 4.4 (`:701-703`)** - the Automation half is superseded (95-D2b):
  Automation has no dialog; the shipped Interactive-only fan-out and its test
  are authoritative.
* **11 sec. 7.2** - the port gains a defaulted third method; the two production
  implementors override it.
* **25/46 dialog** - close semantics tightened (95-D6); no visual or keybinding
  change. Link-loss and session-close cleanup are new (95-D16).

## 10. dsh mapping

| dsh behavior | ymh | Anchor / justification |
|---|---|---|
| A prompt is broadcast to every attached interactive client | Mirrored | `protocol_server.cpp:1026-1041`; 05 sec. 7.6 `:990-999` |
| Resolving a prompt clears it for every client | Mirrored (this spec) | 95-D1..95-D5; 05 sec. 7.6 `:998-999` |
| Resolution is not a durable event | Mirrored | 05 sec. 7.6 `:957-958`; 95-I11 |
| Idempotent duplicates | Mirrored | 95-D6/95-I4 |
| Per-connection FIFO ordering | Mirrored | 95-I10; 05 sec. 7.7 |
| Losing decision rejected without user-visible error | Mirrored | 95-D8; 05 sec. 7.6 `:995-997` |
| Automation profile receives permission prompts | **Not mirrored** | Automation is unattended and has no dialog; the prompt would block with no consumer (`95-D2b`; `09-permissions.md:701-703` superseded; `transport_server_test.cpp:730-761` pins Interactive-only; 56-D6) |
| New UI chrome for remote resolution | **Not mirrored** | No safe generic transient-notice surface on this path; the close + agent state is unambiguous (`95-D14`; 25 dialog contract) |

## 11. Checklist

* [ ] `permission.resolved` + struct + JSON pair (95-D1)
* [ ] Shared predicate (95-D2); Automation supersession recorded (95-D2b)
* [ ] Two-phase publication; no virtual under `State::mutex` (95-D3a)
* [ ] Three notice sites (+ deferred flush); none for undelivered requests (95-D3)
* [ ] Notice after `set_value`, exception-contained (95-D3)
* [ ] Defaulted port method; two production overrides (95-D4)
* [ ] Sink + dispatch + wiring (95-D5)
* [ ] Session-keyed guard; conditional state transition (95-D6)
* [ ] Link-loss and session-close cleanup (95-D16)
* [ ] No transport calls on teardown (95-D17)
* [ ] Catch claims + resolves (95-D18)
* [ ] T1-T15 registered and passing
* [ ] ASCII; anchors resolve; tracker row before commit

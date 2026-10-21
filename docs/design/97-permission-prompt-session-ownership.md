# 97 - Permission prompts follow their session

```
Status: draft (Rev 1)
Revision: 1
Verification status: not gated
Component: 97 (errata) -- two live defects. (a) A second `ymh` started in the
           same directory as a running one shows a permission dialog over its
           own empty session, because a fresh supervisor subscribes to EVERY
           live session in the workspace (`supervisor.cpp:2569-2575`) and the
           permission dialog is one global model shared by all sessions
           (`ui_model.hpp:914`, opened unconditionally at
           `ui_model.cpp:1869-1879`). (b) `permission.decide` is accepted from
           ANY connected client (`protocol_server.cpp:544-550`;
           `permission_broker.cpp:427-443`), so that unrelated dialog can
           resolve/deny a tool call in the first instance's session.
           Observed by the user: instance A ran a turn and prompted; instance B
           (same dir, empty screen) also prompted; pressing `A` in A resumed A;
           B's prompt remained; dismissing it in B produced `DENIED` on a later
           tool call in A. `remember()` is not the cause: it returns `{}` for
           every Deny and for scope Once (`permission_policy.cpp:545-555`).
Depends on: 09-permissions.md (fan-out, background policy F2), 46 (dialog),
           58 (session subscriptions), 90 (path grants), 95 (resolution
           broadcast, `closePermissionDialogFor*`).
Amends: 25/46 dialog ownership, 09 sec. 4.4 decision acceptance, 58 session
        subscription intent.
Tracker: a `DESIGN_STATUS.md` row is required before commit.
```

## 1. Problem

Sequence observed:

1. Instance A (session `S1`) runs a turn and a tool call hits `Ask`.
2. Instance B is started in the same directory. Same directory means the **same
   workspace daemon**: `cli.cpp:534-556` calls `ensureRunning` on the workspace
   row, and the registry resolves to one daemon per workspace. B is a second
   supervisor attached to that daemon, not a second daemon.
3. On startup B lists the workspace's sessions and **subscribes to every live
   one** (`supervisor.cpp:2569-2575`), including `S1`. It then creates its own
   fresh session `S2`, which is what the empty screen shows
   (`supervisor.cpp:2586-2607`).
4. `permission.request` for `S1` is broadcast to every Interactive subscriber
   of `S1` (`protocol_server.cpp:1026-1041`). B is one, so B receives it.
5. `UiModel` has a **single** `PermissionDialogModel` (`ui_model.hpp:914`) and
   `PermissionRequested` opens it unconditionally, with no check that the event
   belongs to the session currently displayed (`ui_model.cpp:1869-1879`). The
   renderer draws `model.dialog` whenever `mode == UiMode::Dialog`
   (`ui_render.cpp:1153`). So B's empty `S2` view is covered by a prompt for
   `S1`.
6. A's `A` resolves `S1`'s request and A proceeds. With a pre-95 daemon B's
   dialog does not close; with a 95 daemon it does (95-D3/95-D6). Either way B
   can still answer.
7. If B answers while a **later** request `R2` for `S1` is pending, B's
   `permission.decide` reaches the broker, `claim()` succeeds, and `S1`'s call
   is denied even though B never displayed `S1`. That is the observed `DENIED`
   without a local prompt.

Two independent defects:

* **D-A**: a prompt is rendered without regard to the session being viewed.
* **D-B**: any client may resolve any pending request id. Ids are sequential
  (`perm-N`, `permission_broker.cpp:249`), so a confused or stale client is not
  even required to guess.

Counter-hypothesis ruled out: a Deny is never remembered
(`permission_policy.cpp:553-555`), so the later `DENIED` was a real decision on
a pending request, not a sticky rule.

## 2. Decisions

### 97-D1 - A per-session pending prompt

`SessionUiState` (`include/ymh/ui/ui_model.hpp:403-425`) gains:

```cpp
struct PendingPermission {
    PermissionRequestId request;
    std::string         tool;
    std::string         summary;
    bool                force_ask = false;
};
std::optional<PendingPermission> pending_permission;
```

One per session. The broker already permits at most one pending request per
session (the agent blocks on the future), so at most one is ever stored; a
second `PermissionRequested` for the same session overwrites (last-wins) and the
overwritten id is still cleared by its own 95 notice.

### 97-D2 - The dialog opens only for the displayed session

`PermissionRequested` (`ui_model.cpp:1869-1879`) becomes:

```cpp
if (e.session == activeSessionId()) {
    dialog.open = true;                 // exactly the shipped open path
    dialog.session = e.session;
    // ... tool, summary, force_ask, selected, prev_mode = mode
    state.attention.needsInput = true;
    mode = UiMode::Dialog;
    state.pending_permission.reset();   // was empty for the viewed session
} else {
    state.pending_permission = PendingPermission{e.request, e.tool, e.summary, e.force_ask};
    state.attention.needsInput = true;  // the session bar shows it waiting
}
dirty.mark(e.session, UiDirtyFlag::Attention);
```

The dialog, `mode`, and `prev_mode` are untouched in the else branch. A prompt
for a non-viewed session can no longer overlay the empty screen (97-I1).

`activeSessionId()` is the model's single displayed session; it is empty on the
dashboard, so a prompt arriving there is stored, never drawn (97-I2).

### 97-D3 - Focusing a session opens its stored prompt

```cpp
// ui_model.hpp / ui_model.cpp
void UiModel::openPendingPermissionIfAny();
```

Body: let `state = activeSession()`; if `state == nullptr` or
`state->pending_permission` is empty, return; if `dialog.open` (another modal is
already up), return and leave it stored; otherwise move the stored value into
`dialog` (`request`, `tool`, `summary`, `force_ask`, `selected = force_ask ? 1 :
0`, `prev_mode = mode`), reset the optional, set `mode = UiMode::Dialog`, keep
`attention.needsInput = true`, and mark Attention.

Callers (both concrete): `SupervisorApp::focusSessionIn` after
`model_.focusSession(session)` (`supervisor.cpp:1937`), and
`SupervisorApp::activate_session` after the active session changes
(`supervisor.cpp:2531`). Opening a session is therefore the moment its prompt
becomes visible and answerable (97-I3).

### 97-D4 - Resolution clears the stored prompt

`PermissionResolved` (`ui_model.cpp:1881-1893`) additionally:

```cpp
if (state.pending_permission.has_value() &&
    state.pending_permission->request == e.request) {
    state.pending_permission.reset();
    state.attention.needsInput = false;
}
```

The existing open-dialog close is unchanged. A resolution for `S1` clears only
`S1`'s stored prompt (the `state` is the event's session), so it cannot clear
another session's attention (97-I4).

### 97-D5 - Session close and link loss clear it too

`closePermissionDialogForSession` / `closePermissionDialogForWorkspace`
(95-D16, `ui_model.cpp`) also reset `pending_permission` for the affected
session(s), so a closed or detached session leaves no stale prompt behind
(97-I5). `SupervisorApp::on_link_state` and the `SessionClosed` handler already
call them.

### 97-D6 - Only a subscriber may resolve a request

The daemon must not accept a decision from a connection that is not subscribed
to the request's session.

* `PermissionBroker::session_of(const std::string& request_id) const ->
  std::optional<SessionId>`: scan `pending` under `mutex` and return the
  matching entry's `core.session` (bounded by the pending list size).
* `HostRuntime::permissionSession(request_id)` forwards it.
* In `ProtocolServer`'s `kPermissionDecide` branch
  (`protocol_server.cpp:544-550`), after parsing the params:

```cpp
const std::optional<SessionId> session = host_.permissionSession(params.request_id);
if (!session.has_value()) {
    throw RpcException(code_value(RpcCode::InvalidParams),
                       "unknown or expired permission request_id");
}
if (!is_subscribed_interactive(conn, *session)) {
    throw RpcException(code_value(RpcCode::PermissionDenied),
                       "not subscribed to this session");
}
if (!host_.decidePermission(params.request_id, params.decision, params.scope)) {
    throw RpcException(code_value(RpcCode::InvalidParams),
                       "unknown or expired permission request_id");
}
```

The check is server-side and uses the broker's own session, never a
client-supplied one. `is_subscribed_interactive` is the spec-95 helper
(`protocol_server.cpp`), so the predicate matches the fan-out predicate exactly
(97-I6). No wire change.

If `RpcCode` has no `PermissionDenied`, add it (additive enum value) or use
`InvalidParams` with the distinct message; the spec pins `InvalidParams` with
the message if the enum would change the wire. **Decision: reuse
`InvalidParams`** with the message `"not subscribed to this session"`; no new
code.

### 97-D7 - The stored prompt is not a second queue

There is at most one stored prompt per session (97-D1). When the user switches
away while a dialog is open, the modal blocks switching, so no hand-off is
needed. When a stored prompt is opened and the user switches away again, the
open dialog stays (as today; modal input capture). Subagent children follow the
same rule; the broker fixes their permission scope, so they normally never
prompt.

### 97-D8 - Out of scope: startup subscription fan-out

A fresh supervisor still subscribes to every live session
(`supervisor.cpp:2569-2575`) to keep the dashboard's per-session status live,
and each subscription starts at `beginning`
(`supervisor_connection.cpp:303-317`), so a second instance also replays every
live session's history. That is a startup-cost concern, not this correctness
bug; the prompt is now stored per session (97-D2), so the fan-out is harmless.
Recorded as OQ-97-1 (a future "background sessions subscribe from `now` and
replay on activation" change).

## 3. Interfaces

```cpp
// include/ymh/ui/ui_model.hpp
struct PendingPermission { PermissionRequestId request; std::string tool;
                           std::string summary; bool force_ask = false; };
struct SessionUiState { /* ... */ std::optional<PendingPermission> pending_permission; };
class UiModel {
public:
    void openPendingPermissionIfAny();   // 97-D3
};

// include/ymh/permission/permission_broker.hpp
class PermissionBroker {
public:
    std::optional<SessionId> session_of(const std::string& request_id) const; // 97-D6
};

// include/ymh/host/host_runtime.hpp
std::optional<SessionId> permissionSession(const std::string& request_id);   // 97-D6
```

## 4. Implementation

### 4.1 Model

* Add `PendingPermission` + the `SessionUiState` field (97-D1).
* Split `PermissionRequested` into the viewed/non-viewed branches (97-D2).
* Add `openPendingPermissionIfAny` (97-D3).
* Extend `PermissionResolved` (97-D4).
* Extend the two `closePermissionDialogFor*` bodies (97-D5).

### 4.2 Supervisor

Call `model_.openPendingPermissionIfAny()` after the session switch in
`focusSessionIn` (`:1937`) and `activate_session` (`:2531`). No other call site.

### 4.3 Broker + host + server

Add `session_of`, the `HostRuntime` wrapper, and the server's two-step check
(97-D6). The broker's `onDecision` is unchanged.

### 4.4 Ordering with spec 95

The 95 resolution notice still arrives for every delivered request; 97-D4 makes
it clear the stored prompt. The deciding UI still closes locally (95-D11). No
protocol change, so `kProtocolVersion` stays 1.

## 5. Invariants

* **97-I1 (no cross-session overlay).** A `PermissionRequested` whose session is
  not the displayed session never changes `dialog`, `mode`, or `prev_mode`.
* **97-I2 (dashboard).** With no active session, a prompt is stored and only
  the session's attention indicator changes.
* **97-I3 (prompt follows focus).** Focusing a session with a stored prompt
  opens it exactly once, with the stored fields; the optional is cleared.
* **97-I4 (resolution is scoped).** A resolution clears only the matching
  session's stored prompt and attention.
* **97-I5 (no stale prompt).** Session close and link loss clear the stored
  prompt (95-D16 paths).
* **97-I6 (decision authorization).** A `permission.decide` for a request whose
  session the connection is not subscribed to is rejected and has no effect.
* **97-I7 (single pending).** At most one stored prompt per session; the broker
  guarantees at most one pending request per session.
* **97-I8 (no wire change).** `kProtocolVersion` stays 1; no DTO changes.
* **97-I9 (no grant change).** No change to `remember`, grant scopes, or
  `permissions.jsonc`.
* **97-I10 (spec-95 compatibility).** Every broadcast request still produces
  exactly one resolution notice, and the stored prompt is cleared by it.

## 6. Failure modes

| ID | Tag | Scenario | Required behavior |
|---|---|---|---|
| 97-F1 | F3 | Prompt for a non-viewed session | Stored, not drawn (97-I1) |
| 97-F2 | F9 | Second client answers another session's request | Rejected by 97-D6; no effect |
| 97-F3 | F3 | Focus a session with a stored prompt | Opens once, fields preserved |
| 97-F4 | F3 | Resolution for a stored (not open) prompt | Cleared, attention cleared |
| 97-F5 | F3 | Resolution for session A while B has a stored prompt | B untouched |
| 97-F6 | F3 | Session closed with a stored prompt | Cleared (97-D5) |
| 97-F7 | F3 | Link lost with a stored prompt | Cleared |
| 97-F8 | F4 | Prompt on the dashboard | Stored + indicator, no modal |
| 97-F9 | F9 | Two decides race | Subscription check passes for both; `claim` first-wins |
| 97-F10 | F3 | Stale prompt opened after the daemon resolved it elsewhere | Answering returns `InvalidParams`; UI treats it as benign (95-D8) |

## 7. State lifetime

| State | Created | Destroyed | Owner | Survives restart? |
|---|---|---|---|---|
| `SessionUiState::pending_permission` | `PermissionRequested` for a non-viewed session | open (97-D3), resolution (97-D4), close/link loss (97-D5) | `UiModel` | no |
| `dialog` | open path (97-D2/D3) | resolution (95-D6) / close (95-D16) | `UiModel` | no |
| Broker `Pending` | `State::resolve` | `claim`/`denyAll` | broker | no |
| `attention.needsInput` | request | matching clear | `UiModel` | no |

## 8. Tests

1. **T1 no overlay** - a `PermissionRequested` for a session that is not
   displayed leaves `mode`, `dialog`, and `prev_mode` unchanged, stores the
   prompt, and sets that session's attention.
2. **T2 prompt follows focus** - after `focusSession` on the stored session,
   the dialog opens with the stored tool/summary/force_ask and the optional is
   empty; a second focus does not reopen it.
3. **T3 resolution clears storage** - `PermissionResolved` for a stored prompt
   clears it and its attention; a different session's resolution does not.
4. **T4 dashboard** - with no active session the prompt is stored and no modal
   opens.
5. **T5 close/link** - `SessionClosed` and `Detached` clear the stored prompt.
6. **T6 authorization** - a fake connection not subscribed to the session gets
   `InvalidParams` and the request stays pending; a subscribed one succeeds.
   Two clients: only the subscribed one can resolve.
7. **T7 end-to-end** - two supervisors on one daemon: B (viewing a fresh
   session) receives A's request but shows no dialog and sends no decision; A
   decides; B's stored prompt is cleared by the 95 notice; A's turn continues.
8. **T8 no-regression** - a prompt for the viewed session still opens the
   dialog exactly as before (keyboard paths, force_ask, spec-94 hotkeys).

## 9. Amendments

* **25/46 dialog contract** - the dialog is owned by the **displayed** session;
  a request for any other session is a per-session pending state (97-D2/D3).
* **09 sec. 4.4** - only a client subscribed to the request's session may
  decide (97-D6); this is an authorization tightening, not a fan-out change.
* **58 session subscriptions** - the startup fan-out stays (97-D8) but no
  longer implies modal ownership.
* **95** - the resolution broadcast is unchanged; 97-D4 consumes it for stored
  prompts too.

## 10. dsh mapping

| dsh behavior | ymh | Anchor / justification |
|---|---|---|
| A prompt is shown to the client viewing that session | Mirrored (this spec) | 97-D2/D3; `ui_model.hpp:914` (single dialog) was the divergence |
| Session switching is explicit; prompts do not steal focus | Mirrored | 97-D3 opens on focus; no auto-switch |
| A background session's prompt does not block other sessions | Mirrored | 97-D2 stores it; 09 F2 timeout applies when nobody views it |
| Any connected client may resolve any request id | **Not mirrored** | Deliberate: ids are sequential (`permission_broker.cpp:249`); 97-D6 binds a decision to a subscription (`protocol_server.cpp:544-550`) |
| Dashboard live status via subscriptions | Mirrored | 97-D8 keeps the fan-out; only modal ownership changes |

## 11. Checklist

* [ ] `PendingPermission` + `SessionUiState` field (97-D1)
* [ ] Split `PermissionRequested` (97-D2)
* [ ] `openPendingPermissionIfAny` + the two supervisor callers (97-D3)
* [ ] `PermissionResolved` and the close paths (97-D4/D5)
* [ ] `session_of` + `permissionSession` + the server check (97-D6)
* [ ] T1-T8 registered and passing
* [ ] No wire change; spec-95 tests still pass
* [ ] ASCII; anchors resolve; tracker row before commit

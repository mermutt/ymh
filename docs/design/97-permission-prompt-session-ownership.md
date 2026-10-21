# 97 - A second UI joins the live session; prompts belong to the viewed session

```
Status: verified (Rev 26)
Revision: 26
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 26, 6 LOW) + Oracle PASS
Component: 97 (feature + errata) -- three defects observed with a second `ymh`
           in the same repository. (a) The second instance attaches to the live
           daemon but shows an EMPTY new session instead of the session being
           worked on (`supervisor.cpp:2586-2607`, 50-D2.1 "a fresh launch
           always creates"), so it looks unattached. (b) It nevertheless
           receives and renders the first instance's permission prompt,
           because the dialog is one global model opened for any session
           (`ui_model.hpp:914`, `ui_model.cpp:1869-1879`,
           `ui_render.cpp:1153`). (c) `permission.decide` is accepted from any
           client (`protocol_server.cpp:544-550`,
           `permission_broker.cpp:427-443`), so dismissing that prompt in the
           second UI resolved a request of the first session (the observed
           `DENIED`). One daemon per workspace stays (87/16); only the UI
           attachment and prompt ownership change.
Depends on: 09 (decision flow), 16/87 (single daemon per workspace -- unchanged),
            25/46 (dialog), 50 (launch/activation), 58 (subscriptions),
            90 (path grants), 95 (resolution broadcast, close helpers).
Amends: 50-D2.1 (fresh launch always creates) for the attach-with-live-sessions
        case; 25/46 dialog ownership; 09 sec. 4.4 decision acceptance.
Tracker: a `DESIGN_STATUS.md` row is required before commit.
```

## 1. Problem

Observed, in order:

1. Instance A runs a turn in session `S1` and prompts.
2. Instance B starts in the same directory. Same directory is the same
   workspace daemon (`cli.cpp:534-556` `ensureRunning`); B is a second
   supervisor, not a second daemon.
3. B shows an **empty composer** (a newly created `S2`), not `S1`
   (`supervisor.cpp:2586-2607`), while it has already subscribed to every live
   session including `S1` (`supervisor.cpp:2569-2575`). It looks unattached.
4. B receives `permission.request` for `S1` (broadcast to subscribers,
   `protocol_server.cpp:1026-1041`) and opens the **global** dialog over its
   empty `S2` view (`ui_model.cpp:1869-1879`, `ui_render.cpp:1153`).
5. Answering in B reaches the broker for `S1` (`permission_broker.cpp:427-443`)
   and denies or allows a call B was never showing. Denials are never
   remembered (`permission_policy.cpp:553-555`), so this was a real decision on
   a pending request.

Desired behavior:

* A second instance that attaches to a live daemon **joins the live session**
  (shows the conversation and its prompts) instead of inviting a new one.
* `--new` still starts a fresh session; `--resume` still resumes.
* A UI shows/answers a prompt only for the session it is **viewing**.
* When several UIs view the same session (the join case), all show the prompt
  and an answer in any of them closes it in all (spec 95 already broadcasts the
  resolution).
* One daemon per workspace remains.

## 2. Decisions

### 97-D1 - Per-session stored prompt

`SessionUiState` (`include/ymh/ui/ui_model.hpp:403-425`) gains:

```cpp
struct PendingPermission {
    PermissionRequestId request;
    std::string         tool;
    std::string         summary;
    bool                force_ask = false;
};
std::deque<PendingPermission> pending_permissions;
```

There is no broker guarantee of one pending per session (parallel tool calls),
so requests for one session are **queued FIFO**; nothing is overwritten. The
queue is drained by 97-D3/D4; a 95 notice for an id that is not in the dialog
still erases its queued entry (97-D4).

### 97-D2 - The dialog opens only for the **viewed** session

`PermissionRequested` (`ui_model.cpp:1869-1879`) becomes:

```cpp
const SessionUiState* viewed = viewedSession();
const bool showing = viewed != nullptr && viewed->id == e.session &&
                     !dialog.open &&                // never overwrite a live dialog
                     mode == UiMode::Conversation;   // not Dashboard/Switcher/etc.
if (showing) {
    dialog.open      = true;            // the shipped open path, plus the field
    dialog.session   = e.session;
    dialog.request   = e.request;
    dialog.tool      = e.tool;
    dialog.summary   = e.summary;
    dialog.force_ask = e.force_ask;
    dialog.selected  = e.force_ask ? 1 : 0;
    dialog.prev_mode = mode;
    state.attention.needsInput = true;
    mode = UiMode::Dialog;
} else {
    state.pending_permissions.push_back(PendingPermission{e.request, e.tool, e.summary, e.force_ask});
    state.attention.needsInput = true;  // the session bar shows it waiting
}
dirty.mark(e.session, UiDirtyFlag::Attention);
```

If a dialog is already open, the request is **stored** (the `else` branch); the
open dialog keeps its request and 97-D4 re-opens the stored one when it closes
(Rev 7 M-1).

A re-post of the **same** request (the late-subscriber path,
`host_runtime.cpp:419-427`, rebroadcasts pending requests) must not reset the
user's selection or `prev_mode` **and must not enqueue a duplicate**: when
`dialog.open && dialog.request == e.request`, leave `selected`/`prev_mode`
untouched and return; when the id is already queued, return (idempotent
apply).

`viewedSession()` (`ui_model.cpp:1421-1435`) is the view accessor, but it is
**not mode-aware**: `activeSessionId()` stays set while the dashboard is open
(`ui_model.cpp:2061-2067`), and the switcher/context overlays also keep a viewed
session. The gate is therefore `viewedSession() + mode == UiMode::Conversation`,
so a prompt arriving over the Dashboard, Switcher, Context, ExitConfirm, or
Notice is stored, never drawn (97-I2). `dialog.open` and
`dialog.session` are both required by the renderer/input
(`ui_render.cpp:1153`, `ui_render.cpp:2497`, `supervisor.cpp:3664`) and the
close path (`ui_model.cpp:1652`, `:1885`).

### 97-D3 - Focusing a session opens its stored prompt

Add `void UiModel::openPendingPermissionIfAny();`. It opens only when
`mode == UiMode::Conversation` (plus `viewedSession() != nullptr &&
!dialog.open`). It is **idempotent and cheap** (three checks), so it is called
from two **dispatch choke points**, not enumerated per path:

* `SupervisorApp::drain()` (`supervisor.cpp:2761`), after the batch of
  notifications is applied (covers every reply, `PermissionRequested`/`Resolved`,
  `SessionClosed`, link state, resume/fork/rewind success, child-close
  reconcile, subscribe error);
* `SupervisorApp::handle_event()` (`supervisor.cpp:4398`), after input dispatch
  (covers every key-driven mode change: switcher/EnterChild/Focus, dashboard,
  overlay closes, `cancel_exit`, `return_subagent`, workspace delete);

and by `PermissionResolved` after the dialog closes (97-D4). The model-level
`focusSessionIn` never calls it (Rev 5 M-1); the drain/event tails cover every
path that ends in `Conversation`, including ones added later (Rev 15 M-1/M-2).
A path that restores another mode (e.g. `cancel_exit` from the Dashboard
restores `Dashboard`) leaves the prompt queued; the next tail run in
`Conversation` opens it.

`UiModel::eraseSession` / `eraseWorkspace` (`ui_model.cpp:1509-1528`, `:1610`)
are the only other `activeSessionId` writers. **Before their workspace-missing
early return** (`:1510-1513`), when `dialog.open` and the dialog's session (or
its workspace) is erased, they restore `mode = dialog.prev_mode` (falling back
to `Conversation` if `prev_mode` is `Dialog`/not a valid restore target), clear
`dialog`, and drop that session's queue in the same transaction: there is no
session to demote to, and leaving `mode == Dialog` with `dialog.open == false`
would strand every later prompt (Rev 17 M-1). No other model path writes
`activeSessionId` directly in production (`apply(WorkspaceEvent)` and the test
seam call the same helpers; Rev 16 M-1).

Body:

* if `viewedSession() == nullptr` or `dialog.open` or
  `mode != UiMode::Conversation`, return;
* if the viewed session's `pending_permissions` is empty, return;
* take the **front** and `pop_front()`; move its values into `dialog`
  (including `open = true`, `session = viewed->id`,
  `selected = force_ask ? 1 : 0`, `prev_mode = mode`), set
  `mode = UiMode::Dialog`, keep `attention.needsInput = true`, mark Attention.

The model hook is shared; the call sites above plus `PermissionResolved` are the
only touch points. The model-level `focusSession` itself does not open a prompt;
every supervisor path that ends in `Conversation` must call the hook (Rev 14
M-1).

`UiModel::focusSessionIn` (`ui_model.cpp:2096`, also reached from
`beginOpening` `:1536`) must never leave a dialog open for a session other than
the one being viewed (the renderer has no session label, so it would show and
answer S1's prompt while S2 is viewed; Rev 10 M-1). Before it changes
`activeSessionId`, if `dialog.open` it **demotes** the open request back to the
stored form without resolving it:

```cpp
if (dialog.open) {
    auto it = sessions.find(dialog.session);   // never operator[] (no ghost row)
    if (it != sessions.end()) {
        it->second.pending_permissions.push_front(PendingPermission{dialog.request,
            dialog.tool, dialog.summary, dialog.force_ask});
        it->second.attention.needsInput = true;
    }
    dialog.open = false;        // broker entry stays pending; no decision sent
}
mode = UiMode::Conversation;
```

Nothing is lost: the broker still holds the request, the prompt is stored for
its session, and D3 re-opens it when the user returns. `dialog.open &&
mode == Conversation` cannot exist, and `dialog.session` is the viewed session
whenever `dialog.open` (97-I11). The supervisor tail then calls
`openPendingPermissionIfAny()` for the now-viewed session.

### 97-D4 - Resolution clears the stored prompt

`PermissionResolved` (`ui_model.cpp:1881-1893`) additionally clears
`state.pending_permissions` of **every** entry whose request matches **and**
clears that
session's `attention.needsInput` **only when the queue is empty after the erase
and the open dialog (if any) does not hold that session's request**, i.e.
`!(dialog.open && dialog.session == e.session) && queue.empty()`. A queued
sibling or this session's own open dialog keeps the bar Waiting; another
session's dialog does not block clearing this one (Rev 20 M-2/M-3, 95-D6a). After the
close, it calls `openPendingPermissionIfAny()` so the **next queued** request
for the viewed session (parallel tool calls, multiple broker entries --
`permission_broker.cpp:262-263`) opens immediately instead of waiting for the
timeout. The close restored `prev_mode == Conversation` (prompts only open from
Conversation), so the hook's caller contract holds. A resolution for `S1` cannot touch `S2`.

### 97-D5 - Close and link loss clear it even with no open dialog

`closePermissionDialogForSession` / `closePermissionDialogForWorkspace`
(95-D16, `ui_model.cpp`) currently early-return when `dialog.open` is false, so
they cannot clear a stored prompt. Restructure: always clear
`pending_permissions` **and that session's `attention.needsInput`** for the
affected session(s); restore `prev_mode`/`mode` only when a dialog was actually
open. Both are already called from `on_link_state` and `SessionClosed`.

### 97-D6 - Only a subscriber of the request's session may decide

* `PermissionBroker::session_of(id) -> std::optional<SessionId>`: scan `pending`
  under `mutex`, return `core.session`.
* `HostRuntime::permissionSession(id)` forwards it.
* Add `virtual std::optional<SessionId> permissionSession(const std::string&) = 0;`
  to `TransportHost` and implement it on `FakeTransportHost` (tests map the ids
  they mint).
* In `ProtocolServer`'s `kPermissionDecide` branch (`:544-550`): parse, look up
  the session; if `nullopt`, throw `InvalidParams` `"unknown or expired
  permission request_id"`; if not `is_subscribed_interactive(conn, *session)`,
  throw `InvalidParams` `"not subscribed to this session"`; then call
  `decidePermission`. (No new `RpcCode`.)

Every UI that received the prompt is subscribed, so any of them may answer
(97-I6). This check does **not** stop the second instance in the observed
scenario: it is subscribed (the startup fan-out), so 97-D2 is what prevents it
from showing/answering a session it is not viewing. 97-D6 stops a third client
(or a stale/buggy one) from answering by id.

### 97-D7 - A second instance joins the live session

`SupervisorRunOptions` gains `bool force_new_session{false}`, set from
`invocation.new_session` in `cli.cpp` (the `--new` flag, `:105`).

In `refresh_sessions` (`supervisor.cpp:2586-2607`), replace the
"no active session -> always create" branch with:

```text
if (activeSessionId is empty && active workspace && no resume in flight):
    if (options_.force_new_session || live.empty()):
        attach_decided_.insert(workspace);                // launch decided: create
        create_session(workspace, {})                     // --new or first launch
    else if (attach_decided_.insert(workspace).second):   // decide ONCE per launch
        activate_session(workspace, last_used(live))      // max updated_at_ms
    else:
        create_session(workspace, {})                     // active session was deleted
```

`attach_decided_` is a per-supervisor `std::set<WorkspaceId>`; without it a later
`session.list` refresh (e.g. after deleting the active session) would join a
pre-existing session instead of creating, surprising the user (M-3). It is
seeded on **every** way the workspace gets an active session: the join, the
create, `apply_resume_success` (`supervisor.cpp:1899-1939`), and (before the
branch) whenever `refresh_sessions` already observes a non-empty
`activeSessionId` (reconnect/reattach). A launch or reconnect that already has
a session must never join later; deleting that session creates a new one
(Rev 22 M-1, Rev 25 Oracle).

`live` is the non-subagent live list from `session.list`, ordered by
**ordinal ascending** (`registry.cpp:609-644`); the supervisor's local row
(`supervisor.cpp:2552`, `:2566`) must be widened to keep `updated_at_ms`
(N1, gate Rev 10). Ordinal is **creation** order
(`registry.cpp:1224` allocates `MAX(ordinal)+1`), **not** last use, so
`live.back()` is the newest-*created* session (Oracle Rev 8). Each row carries
`updated_at_ms` (`protocol.cpp:644`, from `session.cpp:649`), the 57-D5
last-use key; the join picks `last_used(live)` = the live session with the
greatest `updated_at_ms` (tie: highest ordinal). With two live sessions this
joins the one actually being worked on, not the most recently created one. The
join must also call `refresh_status_context` for the activated session (the 46
canonical activate path does). `--resume` still takes precedence
(`resume_in_flight_`); `--new` forces creation (97-I8).

This supersedes 50-D2.1, 50-D2.2, and 50-S6's "a fresh launch always creates"
for the attach-with-live-sessions case only. A first launch (no live sessions) is
unchanged. Rationale: the second instance attached to a live daemon is a
**second view** of work already in progress, not a new conversation; the user
asked for exactly this (2026-10-20) after seeing a blank screen.

### 97-D8 - Shared prompts across N UIs

When N UIs view the same session, all N receive the request (per-subscription
broadcast), all N satisfy 97-D2 (viewed), all N render the dialog, and spec 95
broadcasts the resolution to all N, whose 95-D6 guard closes it. This is the
requested "regardless of the number of UIs" behavior and needs no new wire
message: 97 changes **who may display/answer**, not how the answer is shared.

### 97-D9 - No wire change

`kProtocolVersion` stays 1; no DTO change; 95 remains the only resolution
channel.

### 97-D10 - Cross-workspace isolation

A supervisor attaches every live registered workspace
(`supervisor_presence.cpp:90-107`, `supervisor.cpp:2368-2391`), each connection
has its own `on_permission` sink (`:984-989`), and `refresh_sessions` tracks
that workspace's live sessions (`:2571-2575`); the daemon fans out to all
subscribers and re-posts pending requests to a late subscriber
(`protocol_server.cpp:1026-1043`, `host_runtime.cpp:419-427`). So a prompt from
a `ymh` in another directory **does** reach this supervisor.

D2's viewed-session gate is already workspace-scoped: a session id (UUIDv4,
`session.cpp:47`) belongs to exactly one workspace, and `viewedSession()` is
the focused workspace's active session (`ui_model.cpp:1328-1434`), so a
request for a session in a **non-focused** workspace queues on that session
and never opens a modal. No extra ingress filter is needed; the adapter's
`ensureSessionIn(workspace, ...)` (`ui_event_adapter.cpp:393-395`) materializes
the session in its own workspace first (Rev 24; reported 2026-10-21). The
remaining rule is negative: D2 must not be bypassed for these events, and the
adapter's synthetic `WaitingForPermission` (`:407-412`) must stay a per-session
status, not a dialog request.

## 3. Interfaces

```cpp
// include/ymh/ui/ui_model.hpp
struct PendingPermission { PermissionRequestId request; std::string tool;
                           std::string summary; bool force_ask = false; };
struct SessionUiState { /* ... */ std::deque<PendingPermission> pending_permissions; };
class UiModel {
public:
    void openPendingPermissionIfAny();  // 97-D3: drain()+handle_event() tails + PermissionResolved
};

// include/ymh/ui/supervisor.hpp
struct SupervisorRunOptions { /* ... */ bool force_new_session{false}; };  // 97-D7

// include/ymh/permission/permission_broker.hpp
std::optional<SessionId> session_of(const std::string& request_id) const;  // 97-D6

// include/ymh/host/host_runtime.hpp
std::optional<SessionId> permissionSession(const std::string& request_id); // 97-D6
```

## 4. Implementation

* Model: field, split `PermissionRequested`, `openPendingPermissionIfAny`
  called from the two supervisor dispatch tails and by `PermissionResolved`,
  extend `PermissionResolved`, restructure the two close helpers (97-D1..D5).
* Supervisor: `force_new_session` wiring in `cli.cpp`; the join branch in
  `refresh_sessions` (97-D7).
* Broker/host/server: `session_of` + wrapper + the subscription check (97-D6).

## 5. Invariants

* **97-I1.** A `PermissionRequested` for a non-viewed session never changes
  `dialog`, `mode`, or `prev_mode`.
* **97-I2.** The dashboard (no viewed session) stores a prompt and shows no
  modal, even though `activeSessionId` may still be set.
* **97-I3.** Focusing a session with a stored prompt opens it exactly once,
  with the stored fields; the front is popped.
* **97-I4.** A resolution clears only the matching session's stored prompt and
  attention.
* **97-I5.** Session close and link loss clear a stored prompt even when no
  dialog is open.
* **97-I6.** A decide is accepted only from a connection subscribed to the
  request's session; a UI that received the prompt may answer, any other may
  not.
* **97-I7.** N UIs viewing one session all show the prompt; one answer closes
  it in all (95).
* **97-I8.** A second instance attaching to a daemon with live sessions joins
  the **last-used** live session (max `updated_at_ms`, tie: highest ordinal);
  with none, or with `--new`, it creates one; `--resume` is unaffected.
* **97-I9.** One daemon per workspace is unchanged (87/16).
* **97-I10.** No wire change; 95 tests still pass.
* **97-I13.** No modal ever opens for a session outside the focused workspace:
  the supervisor may hold connections to several workspaces and receive their
  `PermissionRequested`, but only a request whose session is the viewed session
  opens a dialog (97-D2); the others queue on their own session.
* **97-I12.** After every `drain()` batch and every dispatched input event, if
  `mode == Conversation` and the viewed session has a queued prompt, the head is
  open (the two dispatch tails).
* **97-I11.** Whenever `dialog.open`, `dialog.session` is the viewed session;
  a view change demotes the open request to the queue front before the
  change (97-D3), never resolves it. The only exception is erasing that
  session/workspace, which closes the dialog and drops its queue in the same
  transaction.

## 6. Failure modes

| ID | Tag | Scenario | Required behavior |
|---|---|---|---|
| 97-F1 | F3 | Prompt for a non-viewed session | Stored, not drawn |
| 97-F2 | F9 | A client **not subscribed** to the session answers | Rejected, no effect |
| 97-F3 | F3 | Focus a session with a stored prompt | Opens once |
| 97-F4 | F3 | Resolution for a stored (closed) prompt | Cleared, attention cleared |
| 97-F5 | F3 | Resolution for A while B has a stored prompt | B untouched |
| 97-F6 | F3 | Session close / link loss with a stored prompt | Cleared with no open dialog |
| 97-F7 | F4 | Prompt on the dashboard | Stored + indicator, no modal |
| 97-F8 | F9 | Two decides race | Both pass the subscription check; `claim` first-wins |
| 97-F9 | F3 | Stale prompt opened after the daemon resolved it | Answering returns InvalidParams; benign (95-D8) |
| 97-F10 | F3 | Second instance joins while A is mid-turn | Joins A's live session; both show the same prompt |

## 7. State lifetime

| State | Created | Destroyed | Owner | Survives restart? |
|---|---|---|---|---|
| `pending_permissions` | any request not opened immediately (D2: non-viewed session, an open dialog, or `mode != Conversation`); FIFO | open (97-D3), resolution (97-D4), close/link (97-D5), erase (97-D3) | `UiModel` | no |
| `dialog` | open path (97-D2/D3) | resolution (95-D6) / close (95-D16) / erase (97-D3) | `UiModel` | no |
| `attention.needsInput` | any request that is queued or opened (D2) | resolution when that session's queue drains and its own dialog is not holding it (97-D4), close/link loss (97-D5, unconditional), erase | `UiModel` | no |
| broker `Pending` | `State::resolve` | `claim`/`denyAll` | broker | no |
| `force_new_session` | CLI parse | process exit | supervisor options | no |
| `attach_decided_` | first `refresh_sessions` after launch (create or join) | process exit | supervisor | no |
| `last_used(live)` row | each `session.list` reply | next reply | supervisor (local) | no |

## 8. Tests

1. **T1** a request for a non-viewed session leaves `mode`/`dialog`/`prev_mode`
   unchanged, queues the prompt, sets that session's attention.
2. **T2** `EnterChild` (`:3767-3771`) and a workspace `Focus` (`:3781-3782`)
   open the queued tool/summary/force_ask after the `handle_event` tail;
   `prev_mode` is `Conversation`; a second focus does not reopen. The
   model-level `focusSession` alone does **not** open it (97-D3).
3. **T3** resolution clears the matching queued entry and its attention only
   when the queue is empty **and** the open dialog (if any) does not hold that
   session's request; a dialog open on another session does not block the
   clear; another session's resolution does not touch this one (Rev 20 M-3).
4. **T4** dashboard (no viewed session) queues, no modal, despite
   `activeSessionId` being set.
5. **T5** `SessionClosed`/`Detached` clear a queued prompt with no open dialog.
   Erasing the dialog's session **and** erasing its workspace (including the
   workspace-missing early return) each close the dialog, drop the queue, and
   restore `mode == prev_mode` (fallback `Conversation`); assert `mode` and
   `prev_mode`, not merely `!dialog.open` (Rev 18 M-1).
6. **T6** two parallel requests for one session (R1 opens, R2 queued): a view
   change demotes R1 to the queue front, both survive in order, returning shows
   R1 then R2; erasing the session before the focus drops R1 without creating a
   ghost row (97-I11).
7. **T7** a connection not subscribed to the session gets `InvalidParams` and
   the request stays queued; a subscribed one succeeds.
8. **T8** end-to-end: A and B on one daemon viewing the same session both show
   the prompt; A answers; B's dialog closes (95); A continues.
9. **T9** second-instance launch: with a live session, the supervisor focuses
   the last-used one instead of creating; with none, it creates; `--new`
   creates; `--resume` resumes; deleting the resumed session later creates a
   new one (never joins another live session; Rev 22 M-1).
10. **T10** no-regression: a prompt for the viewed session still opens exactly
    as before (hotkeys, force_ask).
11. **T11** the `drain()` tail: an async join/reconcile/notification that makes
    a session with a queued prompt the viewed one opens it with **no key
    event** (97-I12).
12. **T12** a re-post of the open request neither resets `selected`/`prev_mode`
    nor enqueues a duplicate.
13. **T13 (existing tests updated, not merely added)**:
    `Errata95.T6UiCloseGuardIsSessionAndRequestKeyed` and
    `Errata95.T15LinkLossAndSessionCloseCleanup` currently open a dialog from
    the Dashboard; with 97-D2 that queues and opens no modal, so they are
    rewritten to enter `Conversation` first (or to assert the queue).
    `SupervisorHarnessTest.UI50_D2_FreshAttachDoesNotFocusLiveSession` and
    `UI50_D2_FreshAttachWithLiveSessionCreatesNew` encode the superseded
    50-D2.1 ("do not focus live"/"create new") and are rewritten to the 97-D7
    join (last-used) behavior; T9 covers the matrix.
    `Errata81.OverlayPrevModeRestores` (opens from the Dashboard) and
    `UiEventAdapter.PermissionOverWireOpensDialog` (request for session B while
    viewing A) are rewritten to enter `Conversation`/view the right session
    first (Rev 20 M-1).
14. **T14** cross-workspace isolation: with connections to two workspaces
    (two `ymh` instances in different directories, or one supervisor hosting
    both), a `PermissionRequested` for a session in the **non-focused**
    workspace opens **no** modal, queues on that session, and marks only it;
    the synthetic `WaitingForPermission` status stays per-session. Focusing
    that workspace/session later opens the queued prompt (97-D10/I13).

## 9. Amendments

* **50-D2.1 / 50-D2.2 / 50-S6** - superseded for the attach-with-live-sessions
  case (97-D7); a fresh workspace with no live session still creates.
* **25/46** - the dialog is owned by the **viewed** session; other sessions
  hold a per-session pending prompt.
* **09 sec. 4.4** - a decision requires an **Interactive** subscription to the
  request's session (97-D6); Automation has no dialog and never decides
  (`is_subscribed_interactive`, spec-95 helper).
* **87/16** - unchanged: one daemon per workspace; 97-D7 joins it, it does not
  create a second one.

## 10. dsh mapping

| dsh behavior | ymh | Anchor |
|---|---|---|
| A second client attaches to the running session | Mirrored (this spec) | 97-D7; `supervisor.cpp:2586-2607` |
| Prompts are shown to clients viewing that session | Mirrored | 97-D2/D3 |
| Any viewer may answer; all viewers see it close | Mirrored | 97-D8; 95 |
| A non-viewer may answer (previous behavior) | **Not mirrored** | Deliberate: a decision is bound to a subscription (97-D6) and a prompt is only shown to the viewed session (97-D2); ids are minted per daemon (`permission_broker.cpp:295`) |
| Independent daemons per workspace | **Not mirrored** | Deliberate: one daemon per workspace (87/16); the user confirmed the limitation (2026-10-20) |

## 11. Checklist

* [ ] `PendingPermission` + deque field (97-D1)
* [ ] Split `PermissionRequested` on `viewedSession()` (97-D2)
* [ ] `openPendingPermissionIfAny` in `drain()` and `handle_event()` tails + `PermissionResolved` (97-D3/D4)
* [ ] erase clears the dialog + restores the mode before the early return (97-D3)
* [ ] `PermissionResolved` + close helpers (97-D4/D5)
* [ ] `session_of` + `permissionSession` + the server check (97-D6)
* [ ] `force_new_session` + the join branch (97-D7)
* [ ] T1-T9 registered and passing
* [ ] No wire change; 95 tests still pass
* [ ] ASCII; anchors resolve; tracker row before commit

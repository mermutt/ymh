# 65 — Subagent Working-State Errata: The Activity Indicator Follows the Turn Subtree

Status: **implemented; MEDIUM-1 fixed; independent re-gate pending** (see
`DESIGN_STATUS.md`). This
errata is written design-first and implemented in the same change set as its code,
at the user's request; the owning specs 46 (verified) and 63 (implemented) are
unchanged in intent. It fixes one defect: during a task that delegates to a
**background** subagent, the bottom activity indicator (63-D1) and the spinner
clock disappear once the parent's own turn ends, even though the child keeps
working, so the parent's transcript looks frozen; submitting a prompt restores
the indicator only until that turn ends.

The user reported, verbatim:

> "During a long turn the UI loses the working state: the bottom working/activity
> indicator disappears and the transcript stops showing new output, while the
> agent (daemon) keeps working. ... Submitting a prompt restores the working
> state, but only briefly. ... with subagents involved; with no subagents it works
> fine."

## 1. Diagnosis

### 1.1 The refuted hypothesis (recorded for the reviewer)

The initial hypothesis was that a child/subagent session's event (its
`turn/ended`) clobbers the **parent's** turn state, or that the active-session
pointer moves to an idle child. Both are refuted by the shipped code:

- Agent state is already **per session**: `std::map<SessionId, SessionUiState>`
  (`include/ymh/ui/ui_model.hpp:649`), each with its own `agent_state` (`:338`).
  `UiEventAdapter::adapt` synthesizes `AgentStateChanged{session, …}` from the
  event's own `session_id` (`src/ui/ui_event_adapter.cpp:76-205`), and the only
  writer of `agent_state` is `UiModel::apply` at that same key
  (`src/ui/ui_model.cpp:1205`). A child's `turn/ended` reaches only the child's
  entry (pinned by `UI65_D2_ChildTurnEndedDoesNotIdleParent`).
- Transport routes strictly by session: `ProtocolServer::onEventCommitted`
  (`src/transport/protocol_server.cpp:799-816`) and `onLiveEvent` (`:818-840`)
  deliver an event only to subscribers of `record.event.session_id`; a child is
  subscribed only while viewed (`sync_subagent_subscriptions`,
  `src/ui/supervisor.cpp:1012-1039`).
- The active-session pointer is changed only by `focusSessionIn`/`focusSession`/
  `focusWorkspace`/`eraseSession` (`src/ui/ui_model.cpp:1370-1391`, `:946`).
  Entering a subagent never calls `focusSessionIn` (58-I1); the live session cells
  never contain a subagent state (58-D7), the catalog membership check and the
  History switcher both exclude `kind == "subagent"` (`ui_model.cpp:775`,
  `:1537`), and `select_history` refuses a child row (`supervisor.cpp:2910-2914`).
  The one
  remaining child-focus path is the explicitly permitted `--resume <child>`
  (58-D7, 55-D3), which is not the reported scenario.

### 1.2 The actual cause

`has_active_turn()` (`src/ui/ui_model.cpp:1795`) reads **only** the active
session's own `agent_state`. The background-delegation guidance tells the model
to use `subagent_continuable` in the background by default (text
`src/agent/subagent_types.cpp:8-12`; registered `src/agent/workspace_runtime.cpp:434-461`),
and a background child returns
immediately (`src/agent/subagent_service.cpp:667-686`), so the parent's
`turn/ended` arrives while the child still runs. At that instant:

- the indicator (gated on `has_active_turn()`, `src/ui/ui_render.cpp:1003`),
- the spinner clock (`UiModel::advance_spinner`, `src/ui/ui_model.cpp:1840`), and
- `animation_active_` (`src/ui/supervisor.cpp:2079-2081`)

all drop to idle, while the daemon is still executing the child. The parent has
no further output until the child settles, so its transcript looks frozen; the
indicator returns for the short parent turn that follows the next prompt. The
strip already shows the child as `>`/`running`, so the information exists — it is
simply not consulted by the indicator.

## 2. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 65-D1 | A session is **working** iff its own `agent_state` is `Thinking`/`CallingTool` **or** it has at least one direct subagent with `SubagentView::status == SubagentStatus::Running`. The derived predicate `UiModel::active_session_working` (`src/ui/ui_model.cpp:1807`, helper `session_working` above it) drives the bottom indicator (`src/ui/ui_render.cpp:1003`), `advance_spinner` (`src/ui/ui_model.cpp:1840`) and `animation_active_` (`src/ui/supervisor.cpp:2081`). | Amends 63-D3 / 46-D9.6 for the subagent case; 63-D1/D2 retained |
| 65-D2 | The parent's own `agent_state` is **never** written by a child's event. `SubagentSpawned` sets a **direct** child `Running` before it is activated and `SubagentFanIn` flips it terminal only after that child's activation settles. The predicate is scoped to direct children; a background grandchild whose own parent has already settled is a known gap (65-F5). | 46-D9.6 retained for own-turn state; pins per-session isolation |
| 65-D3 | The Esc-Esc interrupt keeps using `has_active_turn()` (the active session's own state), so a running child alone never arms or fires the parent's cancel. | 48-D2 / 58-D4 retained |
| 65-D4 | Every `SubagentSpawned` gets exactly one terminal `SubagentFanIn` on **every** spawn-path exit that abandons the child before it can settle. A rejected background activation (`SessionActivator::submit` returns false, 55-F13) settles the epoch `Cancelled` **before** disposing the child; a failed foreground await (timeout / disposed child, 55-F21) settles `Cancelled` and rolls the child back; and the live detached-worker activation (`SubagentService::runBackgroundActivation`, `src/agent/subagent_service.cpp:786-792`) settles an epoch whose activation it cannot start. `AgentLoop::dispose` drops the settle callbacks without firing them (`src/agent/agent_loop.cpp:383`), so the fan-in must be emitted by the spawn path, not the dispose path. | 55-D8/55-F13/55-F21; makes 65-I1 total (no dangling `Running`) |

## 3. Supersession

| ID | Superseded | Superseded by |
|---|---|---|
| 65-S1 | `63-bottom-activity-indicator-errata.md` 63-D3: "the comet renders only while `model.has_active_turn()`". | **65-D1**: the comet renders while the active session's turn **subtree** is working. 63-D1, 63-D2, 63-D4, 63-D5, 63-D6 are retained. |
| 65-S2 | `63-bottom-activity-indicator-errata.md` 63-I1/63-I2 (the indicator is a pure function of `has_active_turn()`) and 63-F2 ("the comet advances while idle"). | **65-D1**: the indicator is a pure function of `active_session_working()`; 63-I1's "no new state" and 63-I3 hold, and 63-F2's check becomes "no bottom frame when `active_session_working()` is false". |
| 65-S3 | `46-permissions-ui-errata.md` §11.2 items 5–6 (the spinner is on/advances only while the **active session's** turn is active) and 46-F13 ("a turn ends between repaints" → stale frame). | **65-D1**: the indicator and clock also cover the active session's direct running subagents; 46-F13's "`has_active_turn()` false → absent" becomes "`active_session_working()` false → absent". 46-I18/46-I19 are retained for the own-turn case. |
| 65-S4 | `55-multi-agent-delegation-errata.md` §6's pinned settlement notice ("Background subagent <id> **finished** and will do no further work…", also quoted in `26-dsh-alignment.md`) | **65-D4**: the shipped notice now reads "is **no longer running** and will do no further work…", so an activation that was rejected before it ran is not described as "finished". No invariant or test depends on the literal. |

## 4. Invariants

| ID | Invariant |
|---|---|
| 65-I1 | The bottom indicator is present iff `UiModel::active_session_working()` is true; with no running child it is exactly `has_active_turn()`, so main-agent behaviour is byte-identical. |
| 65-I2 | A child session's events never change the parent session's `agent_state`; only the parent's own turn events do. |
| 65-I3 | `advance_spinner` advances iff `active_session_working() || active_has_streaming_reasoning()`. |
| 65-I4 | `animation_active_` is true iff the aggregate flash is flashing, reasoning streams, or `active_session_working()`; the repaint timer keeps ticking while a background child runs. |
| 65-I5 | The Esc-Esc interrupt predicate is `has_active_turn()` (own state); `active_session_working()` is never used for cancel. |
| 65-I6 | A `SubagentFanIn` (terminal status) with an idle parent ends the derived work; the indicator returns when the parent is reactivated. |
| 65-I7 | For every `SubagentSpawned` appended to a session there is exactly one terminal `SubagentFanIn` for that child on every exit path — activation rejected (55-F13), foreground await failed (55-F21), or normal settlement — so no exit leaves the child's `SubagentView` `Running` (65-D4). |

## 5. Failure modes

| ID | F-… | Situation | Behaviour |
|---|---|---|---|
| 65-F1 | F1 | Background child runs, parent turn ends | Indicator/spinner stay active (65-I1); the transcript is unchanged until the parent is reactivated |
| 65-F2 | F2 | Child fan-in, parent idle, no other running child | Indicator absent (65-I6) |
| 65-F3 | F3 | Child `turn/ended` while the parent turn is active | Parent unaffected (65-I2) |
| 65-F4 | F4 | Child running, parent idle, user presses Esc-Esc | No arm, no cancel (65-I5) |
| 65-F5 | F5 | **Nested background grandchild**: the direct child has already settled (its `SubagentFanIn` is terminal) while its own background child still runs | The direct child is no longer `Running`, so the parent's indicator drops even though a grandchild works. Known gap: the UI observes only direct children; a full fix requires descendant tracking or a `send_message`/settlement-gate change (`src/agent/subagent_service.cpp:441-462`). |
| 65-F6 | F6 | **`send_message` revival**: a settled continuable child is revived by `send_message` | No new `SubagentSpawned` is emitted (the revival opens an epoch only, `src/agent/subagent_service.cpp:828`), so its `SubagentView::status` stays terminal and the indicator does not return. Known gap; the fix is a status→`Running` event on revival. |
| 65-F7 | F7 | **Dangling `Running`**: a child crashes without a `SubagentFanIn`, then its parent session is replayed | The replayed `SubagentSpawned` has no terminal counterpart, so the derived work sticks `Running` until the child settles or the workspace is evicted. Known gap. |
| 65-F8 | F8 | **Abandoned spawn**: a background activation is rejected (`submit` false) or a foreground await fails (timeout / disposed child) | The epoch settles `Cancelled` and a terminal `SubagentFanIn` is appended before the child is disposed/rolled back (65-D4), so the indicator never sticks (65-I7). |

## 6. Tests

- `UI65_D1_RunningSubagentKeepsIndicatorActive` (`tests/unit/errata46_ui_test.cpp`):
  an idle active session with one `Running` child renders a `kBottomActivityFrames`
  entry and `advance_spinner` advances. **Fails pre-fix** (no comet; the spinner
  is static) — the regression.
- `UI65_D1_SettledSubagentEndsDerivedWork`: a `Completed` child with an idle
  parent renders no comet and `advance_spinner` does not advance.
- `UI65_D1_TerminalStatusesEndDerivedWork`: `Completed`/`Failed`/`Cancelled` all
  end the derived work with an idle parent.
- `UI65_D1_OwnTurnStillDrivesIndicator`: an active own turn still renders the
  comet (no regression to 46-D9/63-D1).
- `UI65_D2_ChildTurnEndedDoesNotIdleParent` (`tests/unit/ui_event_adapter_test.cpp`):
  a child's `turn/ended` leaves the parent `Thinking` and working.
- `UI65_D3_RunningChildDoesNotArmParentInterrupt` (`tests/unit/errata58_ui_test.cpp`):
  Esc with an idle parent and a running child leaves `esc_arm` disarmed and
  submits no cancel.
- `UI65_D4_RejectedBackgroundActivationSettlesChildView`
  (`tests/unit/spec55_delegation_test.cpp`): a rejecting `SessionActivator` makes
  `startContinuable(run_in_background)` return `InboxFull`; the parent log carries
  exactly one terminal `SubagentFanIn` for the child, and projected through
  `UiEventAdapter` the `SubagentView` is not `Running` and
  `active_session_working()` is false. **Fails pre-fix** (no fan-in is appended;
  the view stays `Running` forever) — the MEDIUM-1 regression.
- `UI65_D4_AbandonedAsyncActivationSettlesChildView`: an activator that accepts
  without running the child, then the child is disposed before
  `runBackgroundActivation`; the abandoned epoch still emits one terminal
  `SubagentFanIn` and the projected view is not `Running`. **Fails without the
  detached-worker backstop** — the async residual regression.

**Owning specs amended:** `63` 63-D3 (superseded by 65-D1); references `46` §11
(46-D9.6) and `58` (subagent view scope, 58-I1); `55` 55-D8/55-F13/55-F21 (the
abandoned-spawn rollback gains the terminal fan-in, 65-D4).

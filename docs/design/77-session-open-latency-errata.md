# 77 - Session-Open Latency Errata: Bounded Replay and Replay-Stable UI State

```
Status: verified (Rev 7)
Revision: 7
Verification status: adversarial reviewer (independent, Rev 7): GATE PASS (0 HIGH / 0 MEDIUM, 2 LOW)
Component: 77 (errata) - bounds the supervisor-side *amplification* of opening a
           stored session from `/sessions` (or the Ctrl-S switcher), and pins a
           replay-stable UI state so the bottom activity comet does not sweep for
           the whole open. It is a presentation/derivation errata: it changes no
           durable data and no RPC request/response shape (one additive
           notification only).
Depends on: 00-architecture.md (verified) sec 9.2 / sec 20.24 / sec 44 / sec 54
             (F1-F12);
             05-transport.md (verified) sec 7.4 / sec 7.7 / sec 8.5 (T8, T16, T21,
             T23) - the cursor model and the `replay` flag;
             10-supervisor-tui.md (verified) sec 3.3 / sec 5.4 / sec 9.8 - the
             pump `SupervisorSink` and the drain loop;
             22-switcher-sessions-errata.md (verified) sec 4.3 / sec 5.2 / sec 5.3
             (S2-S4) - the `/sessions` -> resume path this errata bounds;
             24-agent-lifetime-errata.md (verified) - `session.resume` semantics;
             63-bottom-activity-indicator-errata.md (verified) and
             64-ui-polish-errata.md (verified) - the comet and its clock;
             53-eager-daemon-and-model-switching-errata.md (verified) - the eager
             cwd daemon; 76-host-startup-residue-recovery-errata.md (draft) - the
             cold-spawn/attach bound this errata explicitly does NOT re-design.
Scope:      the selected-session open path `select_history` ->
            `resume_from_history` -> `resume_after_attach` -> `session.resume` ->
            `event.subscribe` -> replay -> live. It pins: (A) a live-only gate on
            `context.show` (77-D1); (B) a history-preserving subscribe seed rule
            (77-D2); (C) an optimistic "opening <title>..." UI state that is
            decoupled from the resume reply and is not `working` (77-D3); (D) a
            replay-completion signal and the interrupted-turn reconciliation it
            enables (77-D4). C5 (catalog rebuild cost) is pinned but out of scope
            (sec 1.3).
Supersedes: (a) the implicit "replay fires one `context.show` per
            `AssistantMessage`" behavior - nothing in 18-context-errata.md pins
            it, but the shipped sink lambda at
            `src/ui/supervisor.cpp:938-940` implements it; replaced by 77-D1.
            (b) The implicit "visibility of the comet during an open" behavior of
            63-D3 / 63-I1 (`63-bottom-activity-indicator-errata.md:49`, 63-I1 at
            `:80`) - it is scoped so that a session in the `opening` state is not
            `working` (77-D3 / OL4). 63-D1/D2/D5/D6 and 64-D1/D2 (the glyph
            table, the 5-cell reserved slot, and the exact-elapsed clock) are
            RETAINED unchanged, matching sec 9.
Amends:     05-transport.md sec 7.7 (Interactive `event.subscribe`) - gains the
            additive `host.event{kind: replay_complete}` notice emitted after the
            catch-up batch (77-D4a); it is distinct from the read-only
            `event.unsubscribed{reason:"replay_complete"}` at
            `05-transport.md:913` and `:1187` (which is `session.replay` only);
            05-transport.md sec 7.4 :904-905 ("`session.resume` ... returns
            `AgentStatus::Idle`") - narrowed to "returns the daemon's current
            agent status (`Idle` | `Running`)" per the shipped
            `src/host/host_runtime.cpp:822-823`;
            22-switcher-sessions-errata.md sec 5.2 (`:1298-1397`) and sec 5.3
            (`:1398-1454`) - the success branch keeps SW25 (`:239`, never inject a
            workspace) and gains the optimistic `opening` cell/focus (77-D3).
Retained:   SW25 (the success branch never injects a workspace) - `opening` only
            touches an already-modeled workspace; SW-F3 / 45-D10.8 unknown-session
            recovery (the single dangling-focus authority); 16 O1-O22 ownership;
            05 D20.4/D20.5 (per-connection cursors; `CursorInvalid` -> beginning,
            never now); 64-D1/D2; 63-D6.
```

This document is the design gate for the user-requested session-open latency
fix. It is **additive**: it pins new text, interface names, invariants, failure
modes, and tests. It does not rewrite 05, 10, 18, 22, 24, 63, 64, 76; each
superseded/amended clause is quoted with `file:line` and its replacement is given
here. The convention matches specs 16, 17, 22, 57, 63, 64, 76.

**Naming note.** Invariants local to this spec are **`OL1`-`OL16`**; failure modes
are **`OL-F1`-`OL-F12`**; decisions are **`77-D1`-`77-D5`**. The prefix `OL` is
unused by every spec in `docs/design/`. The user's four fix items keep their brief
numbers 1-4 in prose; pinned decisions are always written `77-Dn`.

**No code is written by this document.** It is design-first per `AGENTS.md`.

**Revision 2 changes (post first gate).** M1: dropped the separate `seeds_` map;
`track(session, seed_from)` now initializes `cursors_` only when absent, so a
same-connection reconnect never regresses to a stale track-time seed (sec 3.3,
OL2/OL10). M2: added `expireOpenings` + `kOpeningTimeout` (30 s) called from
`drain()`, bounding the `opening` placeholder (sec 3.4, OL6/OL15, OL-F1/F5,
77-OQ3 resolved). M3: removed the unreachable `UiModel::opening(id)` accessor
(sec 3.4). M4: added sink-level harness seams `feed_sink_envelope`,
`deliver_sink_notice`, `set_daemon_turn_status` so the OL1/OL4/OL5 tests exercise
the real sink paths (sec 3.7, sec 10). LOWs: corrected stale anchors (sec 2.2,
3.2, 11), stated the `title` source (sec 3.4), made the C2 attribution honest
(sec 1.3, 2, 77-D2), noted the frozen spinner clock (sec 3.4, OL4), and recorded
the auto-tracked-live residual (sec 3.6, OL-F4, 77-OQ4).

**Revision 3 changes (post re-review).** N1: the `kOpeningTimeout` bound is now
driven by a real, `animation_active_`-independent timer path -
`opening_pending_` + `SupervisorApp::expire_openings` posted from the timer thread
at `kPresencePostInterval` (1 s) - instead of `drain()`, which does not run in the
idle-attached `opening` state (sec 3.4; OL15/OL-F1/OL-F5 rewritten; sec 7 gains an
`opening_pending_` row; sec 10.1 OL-F1/OL15 assert the production driver). N-L1: the
OL15 test backdates the public `opening.since` (no clock seam; stated at sec 10.1).
N-L2: the sink lambda bodies are extracted into `SupervisorApp::handle_sink_envelope`
/ `handle_sink_notice` so the seams can drive the real handlers (sec 3.2, 3.7).

**Revision 4 changes (post re-review 2).** N2: the timer body's expiry branch is
factored into `SupervisorApp::maybe_post_opening_expiry(now, post)` (the
*scheduling decision*, not the callback), the timer body calls it, and sec 10.1
drives it through a new `opening_expiry_tick(now)` seam (plus a
`resume_after_attach` seam for the `beginOpening` precondition). The false "fails
if `expire_openings` is not wired into the timer branch" claim is corrected: the
tests now assert the decision *posts* while an opening is set (return true) and
run the production-delivered `expire_openings` closure with no `drain_actions()`.
N-L3: the tick falls through to `tick_presence` when no expiry post is due.
N-L4: `expire_openings` is renamed "the timer's expiry callback" (the driver is
the timer branch / `maybe_post_opening_expiry`). (sec 3.4, 3.7, 10.1.)

**Revision 5 changes (post re-review 3).** N3: the OL-F1 first-live-envelope
backstop is pinned to its implementation site - `handle_sink_envelope` now calls
`model_.endOpening(envelope.session)` when `!replay` (sec 3.2) - and that site is
added to the `endOpening` caller list (sec 3.4), the clearing semantics (sec 3.4
struct comment and the 77-D3 row / sec 7 `opening` row), and the
`feed_sink_envelope` seam comment (sec 3.7); the §10.1 live case (sec 10) now
matches the pinned handler body. N-L5: `last_opening_post` gains its sec 7
state-lifetime row (created epoch, written only on a successful expiry post,
timer-thread-owned in production / UI-thread in the test seam, lost on restart so
the first tick is immediately due).

**Revision 6 changes (post adversarial-gate follow-up).** OL-X1 (MEDIUM): the
`reconcile_after_replay` body now early-returns when `daemon_turn_status_` has no
entry (an auto-tracked `live` session), matching 77-D4/OL-F4 ("do not hide a
possibly-live turn"); the false Idle-while-live is now unreachable by
construction (OL16), and sec 10.1 adds the auto-tracked-`live` + `ReplayComplete`
test. OL-X2 (MEDIUM): `UiModel::expireOpenings` now returns the ids it cleared and
the timer's `expire_openings` callback reconciles each via
`reconcile_after_replay` (sec 3.4), so a lost `ReplayComplete` that left a
dangling replay turn cannot re-arm the indicator; OL5/OL-F1/OL-F5 extended and a
timeout-reconcile test added (sec 5, 6, 10.1). LOWs: `protocol.hpp` anchors
corrected by +12 lines (sec 3.1/3.2/77-D1/sec 11); the header "Supersedes (b)"
no longer lists 64-D2 (retained unchanged, matching sec 9); the `DESIGN_STATUS.md`
77 row is bumped to `draft (Rev 6)`.

**Revision 7 changes (post adversarial re-review 5).** X-1 (MEDIUM): the
`kOpeningTimeout` escape was one-shot and anchored to resume *submit*, so it
could clear `opening` and reconcile *before* a slow catch-up delivered the
replay's final dangling turn; nothing reconciled that later turn and the comet
could re-arm. Fixed by making reconciliation **continuous**: `handle_sink_envelope`
now calls `reconcile_after_replay` after **every applied replay (`replay ==
true`) envelope** (OL-X3), so the derived state converges whenever it is derived,
independent of the escape's timing (sec 3.2, 3.6; OL5/OL-F1/OL-F5 rewritten; sec
10.1 adds the escape-before-dangling ordering test). X-2 (LOW):
`daemon_turn_status_`'s production erase site is now pinned (each `SupervisorApp`
method that calls `model_.eraseSession`; the `forget_session_state` harness
override is test-only) (sec 3.6, sec 7). X-3 (LOW): the supervisor-side
`handle_sink_notice` `ReplayComplete` branch body is now pinned, including the
`UiEventAdapter::onHostNotice` no-op case the new enum value requires for
`-Wswitch` cleanliness (sec 3.2). X-4 (LOW): `set_daemon_turn_status` placement
is pinned to the **success `enqueue`** (UI thread), and the sec 10.1 tests state
the required `drain_actions()` (sec 3.6, sec 10.1).

**Tracker note.** This spec is `draft`; its `DESIGN_STATUS.md` row is maintained
by the authoring change set and is bumped in lock-step with this header. This
spec file itself edits no other file.

---

## 1. Purpose, scope, and what this spec does NOT cover

### 1.1 The report

Selecting a stored session from `/sessions` (History source) or the Ctrl-S
switcher takes "tens of seconds" to reach a usable transcript, and during that
wait the bottom activity comet (the left-right sweep at
`src/ui/ui_render.cpp:1064-1074`, 63-D1) sweeps continuously, reading as if the
app were doing work the whole time.

### 1.2 The open path (pinned, shipped)

1. `/sessions` -> `open_sessions` (`src/ui/supervisor.cpp:1066-1075`).
2. Return on a session row -> `handle_switcher` (`:3060-3088`) ->
   `select_history` (`:2988-3015`).
3. `select_history` -> `resume_from_history` (`:1557-1565`).
4. Attached -> `resume_after_attach` (`:1567-1604`) submits `session.resume`;
   not attached -> `ensure_workspace_running` (`:1467-1486`) queues the worker,
   which resumes on `Attached` (`:1843-1889`).
5. Resume reply -> `apply_resume_success` (`:1641-1674`): `ensureSessionIn`,
   `track(session)` (`:1667`), `focusSession` (`:1672`).
6. `track` -> `subscribe_one` (`src/ui/supervisor_connection.cpp:286-297`) ->
   `event.subscribe`; the daemon replays the committed view in 256-event batches
   (`src/transport/protocol_server.cpp:623-635`, `kReplayBatch`
   `:20`) and then goes live (`:644`).

### 1.3 In scope and out of scope

**In scope (bounded, deterministic):** the five amplification/derivation
mechanisms C2-C5 below and the UI state during the open.

**Out of scope, explicitly:**

- **C1 (cold spawn/attach) may still take seconds.** `spawnAndAttach` polls up to
  10 s (`src/host/workspace_host.cpp:1539-1543`) then races a 5 s winner loop
  (`:1594-1608`), and the underlying blocking connect/send bound is
  `76-host-startup-residue-recovery-errata.md`'s remit; 76 states verbatim that it
  "does NOT design ... session-open latency (item C of the design brief)" and this
  errata reciprocally does not re-design the spawn bound. C1 is cited here only to
  set the latency budget and is otherwise covered by spec 76 and
  `53-eager-daemon-and-model-switching-errata.md` (eager cwd daemon so the common
  case does not spawn at all).
- **C5 (catalog rebuild cost).** `SessionCatalogReader::build` opens and reads
  every registered workspace DB per refresh (`src/ui/session_catalog.cpp:228-243`,
  `refreshNow` `:220-226`). This is the `/sessions` *listing* cost, not the
  open-to-ready cost, and it runs on the catalog worker thread, never the UI
  thread (`include/ymh/ui/session_catalog.hpp:5-9`). It is pinned as a residual
  in sec 7 but no fix is designed here (scope discipline; a separate listing-latency
  errata owns it).
- `/fork`, `/rewind`, `/handoff`, `/dashboard`: not designed here.
- Any durable-log mutation: this errata is presentation-only (77-D4).

**Outcome target.** With C1 removed by 53/76, opening a 1000+-event stored session
from an already-live daemon reaches ready in ~1-2 s. On a **first** (unmodeled)
open the visible history is still delivered by a single `Beginning` replay
(required; sec 2.3); the win is: exactly one `context.show` instead of one per
assistant message (77-D1), no comet-driven rebuild of the whole transcript
(77-D3 / 77-D4), and no per-replayed-message RPC. On a **re-open / reconnect** of
an already-modeled session, 77-D2 additionally removes the re-replay, fetching
only the delta.

---

## 2. Pinned current behavior and root causes

All anchors are the shipped `tmp` tree (main+dev merged).

| # | Mechanism | Anchor | Amplification |
|---|---|---|---|
| C1 | Cold spawn/attach off the UI thread, but the picker is closed and the target unfocusable until the resume reply | `src/host/workspace_host.cpp:1539-1543`, `:1594-1608`; `src/ui/supervisor.cpp:1467-1486`, `:1557-1565` | up to ~15 s (out of scope; 76/53) |
| C2 | `event.subscribe` seeds `Beginning` with no retained cursor; server replays in 256-event batches; the UI full-redraws the whole transcript per drain | `src/ui/supervisor_connection.cpp:286-297`; `src/transport/protocol_server.cpp:623-635`; `src/ui/supervisor.cpp:2121-2162`; `src/ui/ui_render.cpp:2085` | O(events) apply and per-drain full rebuild |
| C3 | Every replayed `AssistantMessage` fires `refresh_status_context`; one RPC per message, serial on the pump | `src/ui/supervisor.cpp:938-940` -> `:3569-3596`; serial at `src/ui/supervisor_connection.cpp:432-478` | one round-trip + one full render per assistant message (largest local session: 134) |
| C4 | `animation_active_` posts `Event::Custom` every 50 ms; a replay ending non-idle sweeps the comet indefinitely | `src/ui/supervisor.cpp:3933-3955` (`:3944-3945`), `:2159-2161`; `src/ui/ui_model.cpp:615-625`, `:597-599`; `src/ui/ui_event_adapter.cpp:44-74` | a full `build_ui` per 50 ms frame for the whole open |
| C5 | Catalog rebuild opens every registered workspace DB per refresh | `src/ui/session_catalog.cpp:228-243`, `:220-226` | listing cost; out of scope (sec 1.3) |

**C2 attribution (honest).** On the reported case - a *first* open of a **stored,
unmodeled** session - `applied_through` is empty, so 77-D2 correctly seeds
`Beginning` (OL7) and the O(events) replay itself is unavoidable: it is how the
visible history reaches the UI. The 77-D2 cursor seed removes the *re-replay*
only for an already-modeled session (a same-connection reconnect, or an attached
session whose `SupervisorConnection` object was replaced). The first-open win
therefore comes from 77-D1 (kill the per-message `context.show`, C3) and 77-D3 /
77-D4 (no comet rebuild, C4), not from the seed. The C2 row above is pinned for
completeness; its row-local win is the reconnect case.

### 2.1 C3 precisely

`SupervisorSink::on_envelope` is installed at `src/ui/supervisor.cpp:935-942`.
Inside the enqueued UI action, line `:938` tests
`envelope.event.type == EventType::AssistantMessage` and calls
`refresh_status_context(workspace_id, envelope.session)` (`:3569-3596`), which
submits `kContextShow` (`:3570-3571`). This runs for replayed *and* live
messages. On the pump, `process_requests` executes pending requests one at a time
(`src/ui/supervisor_connection.cpp:432-478`), and each reply enqueues a UI action
that marks the session `Status` dirty and therefore triggers a full `build_ui`.
For a 134-assistant-message session, this is 134 extra RPC round-trips and 134
extra full-tree rebuilds beyond the replay itself.

### 2.2 C4 precisely (the exact non-idle sequence)

`UiEventAdapter::project_state` (`src/ui/ui_event_adapter.cpp:44-74`) derives the
session state from the event stream (no `state_provider_` is installed in
production; `set_state_provider` at `:39-42` has no production caller). For a
`TurnStarted` it returns `AgentState::Thinking` (`:51-54`). If the process was
interrupted after `TurnStarted` and before any terminal
(`TurnEnded`/`TurnCancelled`/`TurnFailed`, `:59-68`), the replay ends with
`lastState_[session] == Thinking`. Then:

- `is_active_state(Thinking)` is true (`src/ui/ui_model.cpp:597-599`);
- `session_working(state)` is true (`src/ui/ui_model.cpp:615-618`);
- `active_session_working()` is true (`:1819-1829`);
- `animation_active_` is set true at the end of every drain
  (`src/ui/supervisor.cpp:2159-2161`);
- the timer thread posts `Event::Custom` every 50 ms (`:3933-3955`, `:3944-3945`);
- each frame re-runs `build_ui` (`:3917-3926`) and `render_status` paints the next
  comet frame (`src/ui/ui_render.cpp:1064-1074`).

The variant "a `SubagentSpawned` with no `SubagentFanIn`" keeps a direct child
`Running`: `SubagentSpawned` is adapted at
`src/ui/ui_event_adapter.cpp:150-160`, and the child is actually appended with
`SubagentStatus::Running` in `src/ui/ui_model.cpp:1227-1240`; the missing
`SubagentFanIn` (the terminal setter at
`src/ui/ui_event_adapter.cpp:154-160`) means the child is never flipped terminal.
This is the same false positive through `src/ui/ui_model.cpp:619-623`.

Note: an *interrupted* session is genuinely `Idle` in the daemon. On a fresh
daemon the `AgentLoop` is constructed with `state_{AgentState::Idle}`
(`include/ymh/agent/agent_loop.hpp:257`) and `status()` reports `Idle`
(`src/agent/agent_loop.cpp:249-266`); `HostRuntime::agentStatus` therefore
returns `"Idle"` (`src/host/host_runtime.cpp:1096-1108`), and `session.resume`
carries it (`src/host/host_runtime.cpp:822-823`). The UI's event-only projection
is the thing that is wrong.

### 2.3 C2 precisely

`subscribe_one` chooses `Beginning` when the connection's in-memory
`cursors_` map has no entry (`src/ui/supervisor_connection.cpp:286-297`). The
authoritative resume knowledge for an already-rendered session lives in the
supervisor's `UiModel`, not in the connection; a fresh `SupervisorConnection`
created by `attach_workspace` (`src/ui/supervisor.cpp:908-981`) starts with an
empty `cursors_` even though the model may still hold the full transcript for the
session. `untrack` erases the cursor unconditionally
(`src/ui/supervisor_connection.cpp:80-101`, `:93`), so a re-track after a
transient detach re-fetches from `Beginning`. `Now` is never used today (good),
but nothing pins that.

---

## 3. Interface changes (sketches)

All sketches are normative for the coding change set. New symbols carry the
concrete caller required by `AGENTS.md`.

### 3.1 Wire: one additive `HostNotice` kind (77-D4a)

`include/ymh/transport/protocol.hpp` (`HostNoticeKind`, `:252-258`):

```cpp
enum class HostNoticeKind : std::uint8_t {
    SessionClosed,
    SessionCreated,
    LeaseLost,
    DaemonShuttingDown,
    McpServerStatus,
    ReplayComplete,      // NEW: the Interactive event.subscribe catch-up ended
};
```

`src/transport/protocol.cpp` (`kHostNoticeKinds`, `:53-58`) gains
`{HostNoticeKind::ReplayComplete, "replay_complete"}`. The array size constant is
updated 5 -> 6. No field is added, renamed, or moved; no existing kind changes.
The notice reuses the existing `HostNotice` shape (`protocol.hpp:260-265`):
`{kind: ReplayComplete, workspace, session, detail: ""}`. It is distinct from
`UnsubscribedNotice{reason:"replay_complete"}`
(`protocol.hpp:367-370`), which remains `session.replay`-only
(`src/transport/protocol_server.cpp:637-642`).

**Emission caller:** `ProtocolServer::handle_subscribe`
(`src/transport/protocol_server.cpp:574-650`), immediately after the replay
`while` loop at `:635`, only when `!replay_only` and
`conn.profile == ServerProfile::Interactive` (mirroring `onSessionCreated`
`:845`):

```cpp
if (!replay_only && conn.profile == ServerProfile::Interactive) {
    enqueue(conn, notification_json(
                     notify::kHostEvent,
                     to_json_value(HostNotice{HostNoticeKind::ReplayComplete,
                                              config_.workspace,
                                              params.session,
                                              std::string{}})));
}
```

### 3.2 `SupervisorSink::on_envelope` (77-D1, 77-D2)

`include/ymh/ui/supervisor_connection.hpp:86-93`:

```cpp
struct SupervisorSink {
    // replay == true for a catch-up notification (event.stream replay batch,
    // StreamNotification.replay == true); false for a live committed event
    // (event.stream replay == false) or a live delta (event.live).
    // cursor is the post-event resume position (protocol.hpp T21, :354) for an
    // event.stream notification; nullopt for event.live (which carries no cursor,
    // protocol.hpp:357-365).
    std::function<void(const protocol::SessionEnvelope&, bool replay,
                       std::optional<protocol::EventCursor> cursor)> on_envelope;
    // ... other callbacks unchanged ...
};
```

**Pass-site caller:** `SupervisorConnection::dispatch`
(`src/ui/supervisor_connection.cpp:373-415`): the `kEventStream` branch passes
`stream.replay` and `stream.cursor`; the `kEventLive` branch passes `false` and
`std::nullopt`. The `envelope_skipped` early return (`:377-388`) is unchanged.

**Consume-site caller (N-L2):** the supervisor does **not** retain the sink
lambdas - they are locals in `attach_workspace` (`src/ui/supervisor.cpp:935-968`)
`std::move`d into the `SupervisorConnection` (`:977-980`). So the test seams of
sec 3.7 cannot call "the sink lambda" unless the bodies are reachable. The
lambda bodies are therefore extracted into `SupervisorApp` member functions; the
lambdas keep only the `enqueue` hop:

```cpp
// UI thread.
void handle_sink_envelope(const WorkspaceId& workspace,
                          const protocol::SessionEnvelope& envelope, bool replay,
                          std::optional<protocol::EventCursor> cursor);
// UI thread. The existing on_notice body (src/ui/supervisor.cpp:948-968) plus
// the 77-D4 ReplayComplete branch.
void handle_sink_notice(const WorkspaceId& workspace,
                        const protocol::HostNotice& notice);
```

```cpp
sink.on_envelope = [this, workspace_id](
        const protocol::SessionEnvelope& envelope, bool replay,
        std::optional<protocol::EventCursor> cursor) {
    enqueue([this, workspace_id, envelope, replay, cursor] {
        handle_sink_envelope(workspace_id, envelope, replay, cursor);
    });
};
sink.on_notice = [this, workspace_id](const protocol::HostNotice& notice) {
    enqueue([this, workspace_id, notice] {
        handle_sink_notice(workspace_id, notice);
    });
};
```

`handle_sink_envelope` is the handler body, in order: update `applied_through`
from `cursor`, call `adapter_.onSessionEnvelope`, for a replay envelope reconcile
the derived turn immediately
(`if (replay) reconcile_after_replay(envelope.session);`, OL-X3), apply the 77-D1
gate (`refresh_status_context` only when `!replay`), and run the **OL-F1 live
backstop** - `if (!replay) model_.endOpening(envelope.session);`. A live committed
event proves the catch-up ended, so the opening placeholder clears even when the
`ReplayComplete` notice is lost; running it after `adapter_.onSessionEnvelope`
applies the live event before the placeholder goes. The replay reconcile (OL-X3)
is what makes the derived-state guarantee ordering-independent: because it runs
*after every applied replay event*, a dangling `TurnStarted` delivered late (after
the `kOpeningTimeout` escape already cleared `opening`) is still forced Idle in
the same UI action that applied it, before any render - the escape's timing can
no longer strand it. It is
idempotent (a no-op when the derived state already matches, when the recorded
status is `"Running"`, or when no status is recorded, OL16/OL-F4), so a
per-replayed-event call costs one `std::map` lookup and never an RPC or a rebuild.
This is the pinned implementation site for OL-F1's first-live-envelope recovery
(N3); the `replay` flag now has three consumers: the 77-D1 gate (`!replay`), the
OL-F1 backstop (`!replay`), and the OL-X3 reconcile (`replay`). **Callers:** the
`on_envelope` sink lambda above and the harness seam `feed_sink_envelope`
(sec 3.7).

`handle_sink_notice` is the handler body (X-3), in order: the 77-D4
`ReplayComplete` branch, then the existing `on_notice` body verbatim
(`src/ui/supervisor.cpp:948-968`). The `ReplayComplete` branch is pinned:

```cpp
// UI thread.
void handle_sink_notice(const WorkspaceId& workspace,
                        const protocol::HostNotice& notice) {
    if (notice.kind == protocol::HostNoticeKind::ReplayComplete &&
        notice.session.has_value()) {
        model_.endOpening(*notice.session);        // 77-D4: placeholder gone
        reconcile_after_replay(*notice.session);   // 77-D4b: derived turn
        return;
    }
    // ... the existing on_notice body verbatim (src/ui/supervisor.cpp:948-968):
    //   SessionClosed -> connection->untrack + reconcile_subagent_path_for
    //   adapter_.onHostNotice(workspace, notice)
    //   SessionCreated -> refresh_sessions(workspace)
}
```

Because the branch returns before delegating, `UiEventAdapter::onHostNotice`
(`src/ui/ui_event_adapter.cpp:394`) never sees `ReplayComplete` at runtime; the
implementation nonetheless adds a no-op `case protocol::HostNoticeKind::
ReplayComplete: break;` to that `switch` so the new enum value keeps the
`-Wswitch -Werror` build clean. `handle_sink_notice`'s callers are the
`on_notice` sink lambda above and the harness seam `deliver_sink_notice`
(sec 3.7).

All in-tree `SupervisorSink::on_envelope` assignments are updated: the production
site above and the test sites at
`tests/unit/supervisor_connection_test.cpp:412-414`,
`tests/integration_two_process_test.cpp:657-658`, `:1020-1021`, `:1155-1156`
(there is no assignment at `:1419`; that line is only a `SupervisorSink`
declaration), and `tests/integration_host_harness_test.cpp:207-208`.

### 3.3 `SupervisorConnection::track` seed (77-D2)

`include/ymh/ui/supervisor_connection.hpp` (declaration near `:123`):

```cpp
// Begin tracking. `seed_from` is the caller-model retained applied-through
// cursor for this session (opaque; never parsed), or nullopt for a fresh
// (unmaterialized) open. It initializes the connection's per-session cursor
// ONLY when no cursor is already known (M1 fix): a same-connection reconnect
// always keeps the fresher `cursors_` value and never regresses to a stale
// track-time seed.
void track(const SessionId& session,
           std::optional<protocol::EventCursor> seed_from = std::nullopt);
```

`track` (`src/ui/supervisor_connection.cpp:68-78`) writes the seed straight into
`cursors_` and never overrides an existing entry:

```cpp
void SupervisorConnection::track(const SessionId& session,
                                 std::optional<protocol::EventCursor> seed_from) {
    {
        std::lock_guard lock(mutex_);
        if (std::find(tracked_.begin(), tracked_.end(), session) == tracked_.end()) {
            tracked_.push_back(session);
            ++subscribe_requests_;
        }
        if (seed_from.has_value() && cursors_.find(session) == cursors_.end()) {
            cursors_[session] = *seed_from;
        }
        subscribe_pending_ = true;
    }
    cv_.notify_all();
}
```

`subscribe_one` (`src/ui/supervisor_connection.cpp:286-297`) is **unchanged**: it
uses `cursors_[session]` when present, else `Beginning` (never `Now`, OL3).

**M1 (fixed).** There is no separate `seeds_` map. `attempt_attach` clears
`subscribed_`/`subscriptions_` but not `cursors_`
(`src/ui/supervisor_connection.cpp:241-246`), then calls `subscribe_tracked()`
synchronously before publishing `Attached` (`:256-267`). On a same-connection
reconnect, `subscribe_one` therefore finds the fresher `cursors_` entry (updated
on every delivered `event.stream`, `:399`) and can never see a stale track-time
seed. OL10 holds by construction; the only residual risk (re-applying already
seen events) would be masked by the adapter's id dedupe
(`src/ui/ui_event_adapter.cpp:306-318`) but is eliminated outright here.

The `CursorInvalid` retry (D20.5, `:328-345`) drops `cursors_[session]` and
re-subscribes from `Beginning`; history is preserved.

**Caller:** `apply_resume_success` (`src/ui/supervisor.cpp:1665-1668`) passes
`state.applied_through`:

```cpp
if (const auto connection = connections_.find(workspace);
    connection != connections_.end()) {
    const std::string& through = state.applied_through;
    connection->second->track(
        session, through.empty()
                     ? std::nullopt
                     : std::optional<protocol::EventCursor>{protocol::EventCursor{through}});
}
```

The reconnect resubscribe path (`subscribe_tracked`,
`src/ui/supervisor_connection.cpp:271-284`) is unchanged; the retained
`cursors_` entry is authoritative for a same-connection reconnect.

### 3.4 `UiModel`: opening state and applied-through cursor (77-D2, 77-D3)

`include/ymh/ui/ui_model.hpp`:

```cpp
// 77-D3: the optimistic open state. Set the instant a resume is submitted for a
// modeled workspace; cleared at replay completion (77-D4), on the first live
// (replay == false) envelope via handle_sink_envelope (OL-F1 backstop), on resume
// failure, on a link-state drop, on session eviction, or by the kOpeningTimeout
// escape (`expire_openings`, which reconciles the derived turn, OL-X2). The
// derived turn is reconciled independently of this placeholder on every replay
// envelope (OL-X3), so clearing it early cannot strand a dangling turn.
// Never persisted.
struct OpeningState {
    std::string                                    title;
    std::chrono::steady_clock::time_point          since;
};
```

`SessionUiState` (`include/ymh/ui/ui_model.hpp:312-352`) gains:

```cpp
    // 77-D3: present iff this session is actively being opened/resumed.
    std::optional<OpeningState> opening;
    // 77-D2: the opaque event.subscribe cursor (EventCursor.value, protocol
    // T5/T21) of the last event materialized into this state. Empty when no
    // event has been applied. Never parsed; dies with the state.
    std::string applied_through;
```

New members (declarations beside `ensureSessionIn` `:702` / `focusSession` `:737`):

```cpp
// 77-D3: if `workspace` is modeled, ensure the session cell/state and mark it
// opening with `title`, then focus it. No-op for an unmodeled workspace (no
// phantom workspace; SW25). Returns true iff the state was marked.
bool beginOpening(const WorkspaceId& workspace, const SessionId& session,
                  std::string title);
// 77-D3: clear `session`'s opening state. Returns true iff it was set.
bool endOpening(const SessionId& session);
// 77-D3: clear every opening state in `workspace` (link-state drop backstop).
void endOpeningsIn(const WorkspaceId& workspace);
// 77-D3 / M2 / OL-X2: clear every opening state older than `kOpeningTimeout` as
// of `now` and return the ids whose opening it cleared, so the caller can
// reconcile the derived state at the same escape (never leave a dangling
// replayed turn that would re-arm the comet). The bounded wall-clock backstop
// for a lost ReplayComplete (OL-F1/OL-F5). Defensive with OL-X3: the derived
// turn is already reconciled on every applied replay envelope.
[[nodiscard]] std::vector<SessionId> expireOpenings(
    std::chrono::steady_clock::time_point now);
// 77-D3 / N1: true iff any SessionUiState has an `opening`. Read by the timer's
// opening-expiry scheduling decision `maybe_post_opening_expiry` to decide
// whether to post the expiry task.
[[nodiscard]] bool hasAnyOpening() const;
```

There is deliberately **no** `UiModel::opening(id)` accessor (M3): the two
readers, `session_working` (`src/ui/ui_model.cpp:615`) and `render_conversation`
(`src/ui/ui_render.cpp:511`), already hold the `SessionUiState` and read
`state.opening` directly, so an id-keyed accessor would be dead surface.

Two supervisor members drive the bound (N1), because `drain()` alone cannot:
`expireOpenings` is only reachable from `drain()`, and `drain()` runs only on
`Event::Custom`, which the timer posts only while `animation_active_` is true
(`src/ui/supervisor.cpp:3944-3946`) - which is false while `opening` is set
(OL4). A separate, `animation_active_`-independent driver is therefore required:

```cpp
// 77-D3 / N1: true while any session is opening. Set when beginOpening marks
// one; recomputed on every expiry tick. Read by the timer thread (hence atomic,
// mirroring animation_active_ at src/ui/supervisor.cpp:4092).
std::atomic<bool> opening_pending_{false};
// 77-D3 / N1: the last time the timer posted an expiry task (timer thread only).
std::chrono::steady_clock::time_point last_opening_post;

// 77-D3 / N1 / N2: the timer loop's opening-expiry scheduling decision,
// factored out of the timer lambda (src/ui/supervisor.cpp:3933-3955) so the
// decision itself - not merely its callback - is hermetic-testable. When a
// session is opening it delivers the expiry closure to `post` (production:
// screen_->Post(Task{Closure{...}})) at most once per kPresencePostInterval,
// independent of animation_active_. Returns true iff it posted, so the timer
// body skips the presence heartbeat this tick; false falls through to the
// presence logic (N-L3).
bool maybe_post_opening_expiry(
    std::chrono::steady_clock::time_point now,
    const std::function<void(std::function<void()>)>& post);

// 77-D3 / N1: the timer's expiry *callback* (N-L4 - not "the driver"), run on
// the UI thread from the Task `maybe_post_opening_expiry` posts (NOT from
// drain()). Ages out any opening past kOpeningTimeout, reconciles each expired
// session's derived state (OL-X2), and recomputes opening_pending_.
void expire_openings();
```

`expire_openings` body (`now` is `std::chrono::steady_clock::now()`):

```cpp
void expire_openings() {
    for (const SessionId& session :
         model_.expireOpenings(std::chrono::steady_clock::now())) {
        reconcile_after_replay(session);   // OL-X2 (idempotent w/ OL-X3): no dangling turn
    }
    opening_pending_.store(model_.hasAnyOpening());
}
```

`maybe_post_opening_expiry` body (the `now` is injected by the caller; the
production timer passes `steady_clock::now()`):

```cpp
bool maybe_post_opening_expiry(
    std::chrono::steady_clock::time_point now,
    const std::function<void(std::function<void()>)>& post) {
    if (!opening_pending_.load()) {
        return false;
    }
    if (now - last_opening_post < kPresencePostInterval) {
        return false;
    }
    last_opening_post = now;
    post([this] { expire_openings(); });
    return true;
}
```

The timer thread (`src/ui/supervisor.cpp:3933-3955`) calls that decision
**after** the `animation_active_` test, so an active animation still repaints and
the expiry is scheduled whenever `animation_active_` is false - the target
idle-attached case:

```cpp
if (animation_active_.load()) {
    screen_->PostEvent(ftxui::Event::Custom);
    continue;
}
const auto now = std::chrono::steady_clock::now();
if (maybe_post_opening_expiry(now, [this](std::function<void()> expiry) {
        screen_->Post(ftxui::Task{ftxui::Closure{std::move(expiry)}});
    })) {
    continue;
}
if (now - last_presence_post < kPresencePostInterval) {
    continue;
}
last_presence_post = now;
screen_->Post(ftxui::Task{ftxui::Closure{[this] { tick_presence(); }}});
```

This polls at `kPresencePostInterval` (1 s, `src/ui/supervisor.cpp:61`), not
`kFrameInterval` (50 ms): no 20 fps full rebuild. Crucially the expiry branch
runs when `animation_active_` is **false**, so it does not *require* animation to
be active (the old bug: with `animation_active_` false the timer posted nothing
that reached `drain()`). `beginOpening` sets `opening_pending_.store(true)` on a
successful mark; `expire_openings` clears it via `hasAnyOpening` when the last
placeholder goes. `endOpening`/`endOpeningsIn`/`eraseSession` do not need to
touch the flag: the next 1 s tick recomputes it (at most one extra tick). When
the decision is rate-limited or nothing is opening it returns false and the tick
falls through to `tick_presence` (N-L3), so the idle presence heartbeat is not
suppressed while a placeholder is pending.

**Title source (L2).** `resume_after_attach` does not carry a title, so the
supervisor resolves it with a new private helper used at the `beginOpening` call
site:

```cpp
// 77-D3: the label for the opening placeholder. Prefers the modeled
// SessionCell title, then the `/sessions` catalog entry title (scanned exactly
// as `stored_session_model` does at `src/ui/supervisor.cpp:1624-1634`), then the
// raw SessionId. Always non-empty.
std::string opening_title(const WorkspaceId& workspace, const SessionId& session) const;
```

**Callers.** `beginOpening`: `SupervisorApp::resume_after_attach`
(`src/ui/supervisor.cpp:1567`) before submitting `session.resume`, with
`title = opening_title(workspace, session)`; on a successful mark it sets
`opening_pending_`. `opening_title`: `resume_after_attach` (the same site).
`endOpening`: `SupervisorApp::handle_sink_envelope` on the first live
(`replay == false`) envelope for the session (the OL-F1 backstop, sec 3.2), the
`ReplayComplete` notice handler (77-D4, `:948-968`), the resume failure branch
(`:1590-1595`), and `recover_unknown_session` (`:1676-1690`).
`endOpeningsIn`: `on_link_state` when the workspace link becomes
`Detached`/`Dead` (`src/ui/supervisor.cpp:1843-1889`).
`expireOpenings`/`hasAnyOpening`: `SupervisorApp::expire_openings` (the timer's
expiry callback; it consumes `expireOpenings`'s returned ids and reconciles each
via `reconcile_after_replay`, OL-X2) and `SupervisorApp::maybe_post_opening_expiry`
(the decision that schedules it); `kOpeningTimeout` (30 s) is declared with the
other frame constants at `src/ui/supervisor.cpp:54-61`.

`session_working` (`src/ui/ui_model.cpp:615-625`) gains its first line:

```cpp
bool session_working(const SessionUiState& state) {
    if (state.opening.has_value()) {
        return false;   // 77-D3 / OL4: an open is not work
    }
    if (is_active_state(state.agent_state)) {
        return true;
    }
    // ... subagent loop unchanged ...
}
```

**L4 (spinner clock).** Gating `session_working` on `opening` also freezes the
spinner clock, because `UiModel::advance_spinner` feeds `active_session_working()`
into `advance_spinner_clock` (`src/ui/ui_model.cpp:1852-1855`, invoked from
`UiEventAdapter::onTick` via `drain()`, `src/ui/supervisor.cpp:2137`). This is
intentional and harmless: while `opening` is set the only visible indicator is
the placeholder row; the comet is not rendered (`src/ui/ui_render.cpp:1064-1074`)
because `active_session_working()` is false, and the placeholder has no spinner,
so a frozen frame index paints nothing.

`render_conversation` (`src/ui/ui_render.cpp:511-520`) prepends a dim
`"opening " + title + "..."` row when `active->opening.has_value()`, before the
transcript rows; the transcript (already delivered) follows.

### 3.5 `UiEventAdapter::force_idle` (77-D4)

`include/ymh/ui/ui_event_adapter.hpp`:

```cpp
// 77-D4b: reconcile a replayed session whose daemon status is not Running.
// Resets the session's derived agent_state to Idle (clearing any dangling
// replayed Thinking a lost ReplayComplete left behind), emits an
// AgentStateChanged Idle when lastState_ != Idle, resets lastState_ to Idle,
// and flips any still-Running direct child in the model to SubagentStatus::
// Cancelled (a daemon reporting non-Running has no live direct child). Idempotent.
void force_idle(const SessionId& id);
```

**Caller:** `SupervisorApp::reconcile_after_replay`
(new private method, `src/ui/supervisor.cpp`, near `apply_resume_success`).

### 3.6 `SupervisorApp` private seams (77-D4)

```cpp
// 77-D4: authoritative per-session daemon turn status recorded from the
// session.resume reply ("Idle" | "Running"). Read after every applied replay
// envelope (OL-X3), at replay completion, and at the expiry escape.
std::map<SessionId, std::string> daemon_turn_status_;

// 77-D4: record the daemon's turn status for a session. The single writer is
// the session.resume reply callback; exposed so the test harness can drive the
// real reconciliation path (M4) without a daemon.
void set_daemon_turn_status(const SessionId& session, std::string status);

// 77-D4b: called after every applied replay envelope (handle_sink_envelope,
// OL-X3), from the ReplayComplete notice action (77-D4), and from the
// kOpeningTimeout escape (OL-X2/OL-F5). If a status is recorded and is not
// "Running", force the session Idle; a session with no recorded status (an
// auto-tracked `live` session, OL16/OL-F4) is a no-op - never hide a
// possibly-live turn. Idempotent, so the three call sites compose.
void reconcile_after_replay(const SessionId& session);
```

`set_daemon_turn_status` body (`daemon_turn_status_[session] = std::move(status);`).

`reconcile_after_replay` body:

```cpp
void reconcile_after_replay(const SessionId& session) {
    const auto it = daemon_turn_status_.find(session);
    if (it == daemon_turn_status_.end()) {
        return;   // OL16 / OL-F4: no status recorded -> not ours to hide
    }
    if (it->second == "Running") {
        return;   // a live turn: keep the derived active state
    }
    adapter_.force_idle(session);
}
```

**Callers.** `set_daemon_turn_status`: the resume reply callback
(`src/ui/supervisor.cpp:1576-1603`), on the **UI thread inside the success
`enqueue`** - the same enqueued action that calls `apply_resume_success` - from
`reply.result.value("status", "")` (X-4; the reply callback itself runs on the
pump thread, so the map write must not happen there; `apply_resume_success`
already runs in that enqueue, `:1599-1602`).
`reconcile_after_replay`: `SupervisorApp::handle_sink_envelope` after every
applied `replay == true` envelope (OL-X3, sec 3.2); the `ReplayComplete` branch of
`handle_sink_notice` (`src/ui/supervisor.cpp:948-968`), inside the enqueued UI
action; **and** the `kOpeningTimeout` escape - once per id returned by
`UiModel::expireOpenings` inside `expire_openings` (OL-X2 / OL-F5, idempotent
with OL-X3).
`daemon_turn_status_` is erased on the production session-eviction paths: each
`SupervisorApp` method that calls `model_.eraseSession` also erases the entry
immediately after - `sync_subagent_subscriptions` (`src/ui/supervisor.cpp:1104`),
`handle_subscribe_error` (`:1183`), `recover_unknown_session` (`:1681`), and
`apply_session_deleted` (`:3207`) (X-2). The `forget_session_state` harness
override (`:4232`) only erases the model cell (`app_.model_.sessions.erase`) and
is test-only - the file comment at `:624-627` states production never uses it - so
it is **not** a production erase site. **Unreachable by construction (OL16).** The
only writer of an entry is the `session.resume` reply callback, and
`reconcile_after_replay` reaches `adapter_.force_idle` only when an entry exists;
an auto-tracked `live` session (`refresh_sessions` -> `track`, no
`session.resume`) therefore can never be force-Idled at `ReplayComplete` or after
any applied replay envelope (OL-X3 reaches `force_idle` only through the same
entry check), so a genuinely live auto-tracked turn cannot be hidden and its
Running direct child cannot be flipped to `Cancelled`.

**L5 residual (auto-tracked live sessions).** `refresh_sessions` calls
`track(session)` directly for every daemon-reported `live` session
(`src/ui/supervisor.cpp:1931-1936`), with no `session.resume` and therefore no
`set_daemon_turn_status` call. For such a session, `reconcile_after_replay` is a
no-op (OL-F4): a replay that ends on `TurnStarted`/`UserMessage` (both map to
`Thinking`, `src/ui/ui_event_adapter.cpp:51-54`) can leave it `working`
indefinitely. This residual is scoped out (no authoritative status is fetched on
that path); a follow-up could source the status from `session.list`'s `live`
flag or an `agent.status` read. Recorded in 77-OQ4.

### 3.7 Test seams (M4)

The existing `SupervisorHarness` seams bypass the sink lambdas where 77-D1 and
77-D4 live: `feed_session_envelope` calls `adapter_.onSessionEnvelope` directly
(`src/ui/supervisor.cpp:4445-4448`) and `deliver_host_notice` calls
`adapter_.onHostNotice` (`:4450-4462`), so neither reaches the
`refresh_status_context` gate nor the `ReplayComplete` handler. Because the sink
lambdas are not retained (N-L2, sec 3.2), the seams call the **extracted member
functions** `handle_sink_envelope` / `handle_sink_notice` directly on the UI
thread (they are the same bodies the sink lambdas call), then `drain_actions()`
for any nested `enqueue`. Add five seams
(`include/ymh/ui/supervisor_harness.hpp`):

```cpp
// M4: invoke SupervisorApp::handle_sink_envelope (the real 77-D1 gate, the
// applied_through update, the OL-X3 per-replay-envelope reconcile, and the OL-F1
// first-live-envelope endOpening backstop), then drain.
virtual void feed_sink_envelope(const WorkspaceId& workspace,
                                const protocol::SessionEnvelope& envelope,
                                bool replay,
                                std::optional<protocol::EventCursor> cursor) = 0;
// M4: invoke SupervisorApp::handle_sink_notice, including the ReplayComplete
// handling (77-D4), then drain.
virtual void deliver_sink_notice(const WorkspaceId& workspace,
                                 const protocol::HostNotice& notice) = 0;
// M4: invoke SupervisorApp::set_daemon_turn_status (77-D4), then drain.
virtual void set_daemon_turn_status(const SessionId& session,
                                    std::string status) = 0;
// N2: drive the real SupervisorApp::resume_after_attach call site on the UI
// thread - the production path that marks the 77-D3 opening (beginOpening) and
// sets opening_pending_ (N1) before submitting session.resume. Pins the
// previously-unstated beginOpening precondition (N2): seed a modeled workspace,
// install a canned session.resume reply, then call this.
virtual void resume_after_attach(const WorkspaceId& workspace,
                                 const SessionId& session) = 0;
// N2: run one opening-expiry timer tick by calling the production scheduling
// decision SupervisorApp::maybe_post_opening_expiry(now, post) with a `post`
// that synchronously runs the production-delivered closure (the real
// SupervisorApp::expire_openings) - NOT the callback alone, and NOT
// drain_actions(). Returns true iff an expiry task was posted (i.e. a session
// was opening and the rate limit elapsed), the observable that replaces the
// unpinned raw `opening_pending_` read. `now` is injected so the rate limit and
// the backdated `opening.since` need no clock seam.
virtual bool opening_expiry_tick(std::chrono::steady_clock::time_point now) = 0;
```

**Callers:** the new tests `feed_sink_envelope`, `deliver_sink_notice`,
`set_daemon_turn_status`, `resume_after_attach`, `opening_expiry_tick` (sec 10).
The last two realize the N2 regression: `resume_after_attach` runs the real
`beginOpening` call site, and `opening_expiry_tick` calls the same
`maybe_post_opening_expiry` the timer body calls and runs the same
`expire_openings` closure it delivers, so removing or gutting the scheduling
decision fails the suite without the test calling `expire_openings` or
`drain_actions()` directly. `feed_session_envelope` and `deliver_host_notice` are
retained for the adapter-only and existing 58 tests.

---

## 4. Decisions

| ID | Decision | Rationale / cites |
|---|---|---|
| 77-D1 | **Gate `context.show` to live envelopes.** `refresh_status_context` is called only when `replay == false`. Chosen over "coalesce to one at replay completion" because (a) it needs no replay-window lifetime state, so a mid-replay disconnect cannot strand a suppression flag; (b) the single initial fetch already exists at `apply_resume_success` (`src/ui/supervisor.cpp:1599-1602`, `refresh_status_context` `:1601`); (c) every live `AssistantMessage` keeps the gauge current; (d) it is per-envelope precise (the connection knows `StreamNotification.replay` from the wire, `src/ui/supervisor_connection.cpp:375`, `protocol.hpp:352`). The wire is unchanged; only the sink signature grows. | `src/ui/supervisor.cpp:938-940`; `src/ui/supervisor_connection.cpp:432-478`; 18-context-errata (context gauge is advisory) |
| 77-D2 | **History-preserving subscribe seed.** Seed = `Cursor(applied_through)` iff the model retains the session transcript and `applied_through` is non-empty; otherwise `Beginning`. `Now` is never used. `track(session, seed_from)` writes the seed into `cursors_[session]` **only when no cursor is already present** (M1), so a same-connection reconnect keeps the fresher `cursors_` value (OL10) and a new `SupervisorConnection` object still bridges the caller's retained cursor. `untrack`/`CursorInvalid` drop it; on model-forget (`eraseSession`) `applied_through` dies with the state, so the next open re-fetches from `Beginning`. The seed removes a **re-replay** only for an already-modeled session; a first open of a stored session necessarily seeds `Beginning` (OL7), which is where the visible history comes from. `session_snapshots` is NOT used (unwired; no projection consumer; a dead requirement if pinned). | `src/ui/supervisor_connection.cpp:68-78`, `:241-246`, `:286-297`; `src/transport/protocol_server.cpp:597-612`; 05 sec 8.5 (T16/T21/T23) |
| 77-D3 | **Optimistic open state.** `beginOpening` marks the target session `opening` (title resolved by `opening_title`) and focuses it at resume submit (decoupled from the resume reply); the conversation pane renders `"opening <title>..."`; `session_working` returns false while opening, so neither the comet nor the spinner clock advances. The state is cleared by the first live (`replay == false`) envelope in `handle_sink_envelope` (the OL-F1 fast path, sec 3.2) and by `ReplayComplete` (77-D4), and is bounded by `kOpeningTimeout` (30 s) via the `animation_active_`-independent timer scheduling decision `maybe_post_opening_expiry` (N1/N2), which posts the `expire_openings` callback (which also reconciles the expired
session's derived turn, OL-X2) - not by `drain()`, which does not run while
`opening` with no animation. No phantom workspace (SW25): `beginOpening` is a no-op unless the workspace is modeled. Focus recovery on failure remains 45-D10.8 / `recover_unknown_session`. | `src/ui/supervisor.cpp:1567-1604`, `:1672`, `:3933-3955`, `:4092`; `src/ui/ui_model.cpp:615-625`, `:1852-1855`; `src/ui/ui_render.cpp:511`; 63-D3; 64-D2 |
| 77-D4 | **Replay-completion signal + interrupted-turn reconciliation.** (a) The daemon emits an additive `HostNotice{ReplayComplete}` after the Interactive catch-up replay. (b) On receipt, the supervisor clears `opening` and, if the recorded `session.resume` status is **present and** not `"Running"`, forces the session Idle (`adapter_.force_idle`); an absent entry is a no-op (OL16/OL-F4). This stops C4 for a replayed interrupted turn (the daemon knows the truth; the event-only projection does not). The same reconcile also runs after every applied replay envelope (OL-X3, `handle_sink_envelope`), so it is independent of the replay's last-event ordering, and it additionally runs at the `kOpeningTimeout` escape for each expired opening (OL-X2). A lost ReplayComplete, a slow replay, or an escape firing before the final dangling event therefore all leave the indicator disarmed. A `"Running"` status keeps the derived active state. **L5 residual:** an auto-tracked daemon-`live` session (`src/ui/supervisor.cpp:1931-1936`) has no recorded status, so the reconcile is a no-op there (77-OQ4). | `src/ui/supervisor.cpp:948-968`, `:2159-2161`, `:1931-1936`; `src/ui/ui_model.cpp:615-625`, `:597-599`; `src/ui/ui_event_adapter.cpp:44-74`; `src/host/host_runtime.cpp:822-823`, `:1096-1108`; `include/ymh/agent/agent_loop.hpp:257`; 05 sec 7.4/7.7 |
| 77-D5 | **C5 is out of scope.** The catalog rebuild cost (`src/ui/session_catalog.cpp:228-243`) affects the `/sessions` listing, not open-to-ready; it is off the UI thread. Pinned as a residual (sec 7); a separate listing-latency errata owns it. | `include/ymh/ui/session_catalog.hpp:5-9`; 22 S2 |

---

## 5. Invariants

- **OL1.** For a session opened via `select_history`, the supervisor issues at most
  one `context.show` per resume (`apply_resume_success`, `:1601`) plus one per
  subsequent *live* `AssistantMessage`. Zero `context.show` requests are issued
  for replayed `AssistantMessage` events. (77-D1)
- **OL2.** The `event.subscribe` seed is `Cursor(applied_through)` iff the
  supervisor model holds the session transcript and `applied_through` is
  non-empty; otherwise `Beginning`. The caller seed is written into
  `cursors_[session]` only when no cursor is already present, so it can never
  regress a fresher retained cursor. (77-D2)
- **OL3.** `StreamFrom::Kind::Now` is never the seed for a session whose visible
  history is not already materialized in the model. (77-D2; history-preservation)
- **OL4.** While `SessionUiState::opening` is set, `session_working(state)` is
  false, so `active_session_working()` is false and neither the comet
  (`src/ui/ui_render.cpp:1064`) nor the spinner clock
  (`src/ui/ui_model.cpp:1852-1855`) advances. (77-D3)
- **OL5.** A session whose `session.resume` reply status is not `"Running"` has
  derived `agent_state == Idle` after every applied replay envelope, so it cannot
  end its catch-up replay on a dangling `Thinking` even when the stream ends on a
  non-terminal `TurnStarted`/`ToolCall`/`ToolResult`: `handle_sink_envelope`
  reconciles after each replay event (**OL-X3**), which is independent of *when*
  that event is applied. The `ReplayComplete` notice and the `kOpeningTimeout`
  escape additionally reconcile (OL-X2), so a lost notice, a slow replay, or an
  escape that fires before the final dangling event all converge to the same end
  state. (77-D4; OL-X3)
- **OL6.** `endOpening`/`force_idle` are idempotent; `ReplayComplete` handling for
  an already-cleared opening is a no-op, and `opening` is bounded by
  `kOpeningTimeout` so no placeholder is unbounded (M2/N1). (77-D3/77-D4)
- **OL7.** History is never dropped: a fresh open (unmodeled session) still
  delivers the committed view from `Beginning` and renders it in full. (77-D2)
- **OL8.** No new UI-thread blocking call is introduced: every new supervisor
  action runs on the UI thread and returns immediately; all RPC/disk work remains
  on the pump/worker threads. (10 sec 3.3)
- **OL9.** The open path issues no wire message whose JSON-RPC request/response
  *shape* changes; the only wire addition is the additive
  `HostNoticeKind::ReplayComplete` value on the existing `HostNotice` envelope.
  (77-D4a)
- **OL10.** A same-connection reconnect re-subscribes from the retained
  `cursors_` cursor (never a stale track-time seed, because `track` does not
  override an existing cursor), and `CursorInvalid` falls back to `Beginning`,
  never `Now` (05 D20.4/D20.5). (77-D2; M1)
- **OL11.** `opening` and `applied_through` are supervisor-local in-memory state:
  neither is written to the event log, the registry, or any DB, and both are lost
  on process restart (a restarted supervisor re-opens from `Beginning`, which is
  correct: the model is empty too). (77-D2/77-D3)
- **OL12.** `beginOpening` never creates a `WorkspaceModel`; it is a no-op for an
  unmodeled workspace (SW25 retained). (77-D3)
- **OL13.** The `ReplayComplete` notice is emitted once per Interactive
  `event.subscribe` replay, after all replay notifications on that connection's
  byte stream, and never for `session.replay` (`replay_only == true`). (77-D4a/05
  sec 7.7)
- **OL14.** A session reported `"Running"` by `session.resume` retains its derived
  active state at replay completion (a genuinely running turn still shows the
  comet). (77-D4)
- **OL15.** No `opening` state outlives `kOpeningTimeout` (30 s). The bound is
  scheduled by the timer thread's `opening_pending_` branch
  (`src/ui/supervisor.cpp:3933-3955`), whose decision
  `SupervisorApp::maybe_post_opening_expiry` (N2) delivers a Task calling
  `SupervisorApp::expire_openings` -> `UiModel::expireOpenings(now)` at
  `kPresencePostInterval` (1 s) **independently of `animation_active_` and of
  `drain()`**.   `drain()` is not required (N1): while `opening` is set with no
  streaming/flash, `animation_active_` is false, the timer never posts
  `Event::Custom`, and `drain()` never runs. (77-D3)
- **OL16.** On the replay/escape paths, `adapter_.force_idle` is reachable only
  for a session that has a `daemon_turn_status_` entry written by a
  `session.resume` reply. A session with no such entry (an auto-tracked `live`
  session that was never resumed) can never be force-Idled at `ReplayComplete`,
  so a genuinely live turn on that path can neither be hidden nor have its
  Running direct child flipped to `Cancelled` (OL-X1 unreachable by construction;
  OL-F4/OL14). The `kOpeningTimeout` escape reconciles only sessions that entered
  `opening` via `beginOpening`; for such a session the resume reply records a
  (possibly empty) status, so the reconcile consults it (a reply that never
  applied leaves the entry absent and the reconcile is a conservative no-op,
  OL-F4). (77-D4)

---

## 6. Failure modes

- **OL-F1 (ReplayComplete never arrives).** A connection drops mid-replay, the
  subscribe reply is answered but the notice is lost, or the replayed session is
  idle on an attached link (no live envelope follows). *Effect:* `opening` could
  otherwise stick. *Detection:* none on the wire. *Recovery (bounded, M2/N1):*
  `endOpening` fires on the first `replay == false` envelope for the session (a
  live event proves the catch-up ended), on any
  `SupervisorLinkState::Detached/Dead` via `endOpeningsIn`, on `untrack`/eviction,
  and - covering the idle-attached case - unconditionally at `kOpeningTimeout`
  (30 s) via the timer's `opening_pending_` scheduling decision
  `maybe_post_opening_expiry`, which delivers the `expire_openings` callback
  (`src/ui/supervisor.cpp:3933-3955`). That callback clears `opening` and
  reconciles each expired session (OL-X2). The derived state does **not** depend
  on the escape's timing: `handle_sink_envelope` reconciles after every applied
  replay envelope (OL-X3), so a dangling turn delivered *after* the escape is
  forced Idle when applied, while a turn already present at expiry is forced Idle
  by the escape. A recorded non-`"Running"` status is forced Idle, an absent
  entry is a no-op (OL16), and a `"Running"` status is honored (OL14). Neither
  the scheduling nor the expiry depends on `animation_active_` or on `drain()`.
  No unbounded placeholder (OL15).
- **OL-F2 (seed cursor invalid).** The model's `applied_through` no longer
  resolves (session deleted/pruned). *Recovery:* existing D20.5 path
  (`src/ui/supervisor_connection.cpp:321-345`) drops the cursor and re-subscribes
  `Beginning`; history is re-fetched, OL7 holds.
- **OL-F3 (seed advanced past head).** `applied_through` is ahead of the daemon
  head (store replaced). *Effect:* the cursor resolves but yields no events, or
  `CursorInvalid`. *Recovery:* same as OL-F2; live events then follow.
- **OL-F4 (no recorded daemon turn status).** *Effect:*
  `daemon_turn_status_` has no entry (an auto-tracked `live` session, or a
  resume reply that never applied). *Recovery:* `reconcile_after_replay` is a
  no-op for `"Running"` and, per 77-D4b, also a no-op when the entry is absent
  (conservative: do not hide a possibly-live turn; OL16). This absent case is
  unreachable on the `beginOpening` path, where the resume reply records a
  (possibly empty) status, so the `kOpeningTimeout` reconcile (OL-X2) always has
  an entry to consult. Residual: a genuinely interrupted **auto-tracked** turn
  (`refresh_sessions` tracks daemon-`live` sessions with no `session.resume`,
  `src/ui/supervisor.cpp:1931-1936`) has no `opening` and no status, so it keeps
  its derived `Thinking` (L5, 77-OQ4).
- **OL-F5 (stale daemon predates ReplayComplete).** An older daemon that never
  emits the notice. *Recovery:* OL-F1's first-live-envelope backstop clears
  `opening` if a live event follows; for an idle session the `kOpeningTimeout`
  deadline clears it through the timer decision `maybe_post_opening_expiry` and
  its `expire_openings` callback (M2/N1). The derived state is reconciled
  continuously, not only at the deadline: `handle_sink_envelope` reconciles after
  every applied replay envelope (OL-X3), so a dangling replayed turn is forced
  Idle whether it is applied before the `kOpeningTimeout` escape or after it
  (the X-1 ordering). A recorded non-`"Running"` status is forced Idle, an absent
  entry is a no-op (OL16), and a `"Running"` status is honored (OL14) - so the
  indicator cannot re-arm. No crash; the wire is additive and the client tolerates its
  absence.
- **OL-F6 (replay suppressed a needed gauge refresh).** A replay-only session that
  never goes live (unusual for `event.subscribe`) would not refresh
  `context.show`. *Recovery:* `apply_resume_success`'s explicit refresh
  (`:1601`) always runs once per resume.
- **OL-F7 (optimistic focus on an unknown session).** Resume fails after
  `beginOpening` focused the target. *Recovery:* the failure branch calls
  `endOpening` and the existing 45-D10.8 `recover_unknown_session`
  (`src/ui/supervisor.cpp:1676-1690`) re-focuses a valid session; SW-F3 notice
  surfaces. No phantom workspace (SW25).
- **OL-F8 (duplicate ReplayComplete).** A reconnect/re-subscribe emits a second
  notice. *Recovery:* `endOpening`/`force_idle` are idempotent (OL6); the second
  reconcile with a `"Running"` recorded status is a no-op.
- **OL-F9 (opening stuck across a session switch).** The user opens another
  session while one is `opening`. *Effect:* the first session's placeholder
  remains until its notice/backstop. *Recovery:* `endOpeningsIn` on link drop,
  `eraseSession` on eviction, the first-live-envelope backstop, and the
  `kOpeningTimeout` deadline. The placeholder is per-session, so the new session
  is unaffected.
- **OL-F10 (context.show reply races a reopen).** A late `context.show` reply for
  a session being re-opened. *Recovery:* the existing generation/`enqueue`
  discipline (`:3575-3594`) is unchanged; replies apply to `model_.session`,
  which is keyed by id.
- **OL-F11 (ReplayComplete on a non-Interactive profile).** *Recovery:* the
  emission is gated on `ServerProfile::Interactive` (sec 3.1); Automation clients
  are unaffected.
- **OL-F12 (C5 slow catalog while opening).** The catalog worker may still be
  scanning DBs. *Effect:* the `/sessions` list updates late; the open itself is
  unaffected (the seeded subscribe does not consult the catalog). Residual,
  out of scope (77-D5).

---

## 7. State lifetime

Every state element this errata introduces, with create/update/destroy, owner,
and restart behavior (AGENTS.md "State lifetime is part of the spec").

| State | Created | Updated | Destroyed / evicted | Owner | Process restart |
|---|---|---|---|---|---|
| `SessionUiState::applied_through` (77-D2) | first `event.stream` applied to the state (sink lambda, cursor present) | every applied `event.stream` notification (replay and live committed); not `event.live` (no cursor) | with the `SessionUiState` on `eraseSession` / eviction / `forget_session_state` | supervisor UI thread (state) | lost; next open is a fresh `Beginning` (model empty too) |
| `SupervisorConnection::cursors_[session]` (77-D2; M1 - caller seed initializes it, never overwrites) | `track(session, seed_from)` when absent, else first `event.stream` | every `event.stream` (`:399`), and the skipped-event branch (`:386`); **never** regressed by a later `track` seed | `untrack` (`:93`); `CursorInvalid` (`:331`); connection destruction | pump thread, under `mutex_` | n/a (connection is process-local) |
| `SessionUiState::opening` (77-D3; bounded) | `beginOpening` at resume submit | `since` set once | `endOpening` on the first live envelope (`handle_sink_envelope`, sec 3.2) / ReplayComplete / resume failure / `endOpeningsIn`; timer decision `maybe_post_opening_expiry` -> `expire_openings` -> `expireOpenings` + `reconcile_after_replay` at `kOpeningTimeout` (OL-X2; the derived turn is already reconciled after every applied replay envelope, OL-X3); also with the state on `eraseSession` | supervisor UI thread | lost; no placeholder |
| `SupervisorApp::opening_pending_` (77-D3/N1) | `beginOpening` on a successful mark (`store(true)`) | read by `maybe_post_opening_expiry` (guards the post); `expire_openings` recomputes via `hasAnyOpening()` on each 1 s tick | `store(false)` when no opening remains; with the app on exit | UI thread writes; timer thread reads (atomic) | lost; timer posts no expiry task |
| `SupervisorApp::last_opening_post` (77-D3/N1) | default-constructed (epoch) with the app | set to `now` by `maybe_post_opening_expiry` only when it posts an expiry task; never on the rate-limited/absent return | with the app on exit; no explicit reset | timer thread in production (`:513-514`); the UI thread in the `opening_expiry_tick` test seam; not atomic because only `maybe_post_opening_expiry` reads/writes it | lost (epoch again), so the first tick after restart is immediately due |
| `SupervisorApp::daemon_turn_status_[session]` (77-D4) | `set_daemon_turn_status` from the resume reply callback, on the UI thread inside the success `enqueue` (X-4) | re-resume overwrites; read by `reconcile_after_replay` after every applied replay envelope (OL-X3), at `ReplayComplete`, and at the `kOpeningTimeout` expiry (OL-X2) | erased in the same `SupervisorApp` method as each production `model_.eraseSession` (`src/ui/supervisor.cpp:1104`, `:1183`, `:1681`, `:3207`); the `forget_session_state` harness override (`:4232`) is test-only (X-2) | supervisor UI thread | lost; reconcile becomes a no-op until the next resume (an absent entry is a no-op by construction, OL16) |
| `HostNoticeKind::ReplayComplete` (77-D4a) | daemon, per Interactive subscribe | not applicable | consumed by `on_notice` | daemon io thread; hand-off to supervisor UI thread | n/a (transient wire notice) |
| `EventCursor` seed value in `sink.on_envelope` (77-D1/77-D2) | pump `dispatch` from `StreamNotification.cursor` | per notification | at end of the sink call | pump thread; value copied into the enqueued action | n/a |
| C5 catalog snapshot (unchanged) | catalog worker `build()` | per refresh | replaced per generation | catalog worker | rebuilt on start |
| `adapter_` `lastState_`/`applied_event_ids_` (existing; 77-D4 resets `lastState_`) | first envelope | per event | `forget_session` on eviction | UI thread | lost |

Additional duty per `AGENTS.md`: none of the new state is durable; none is written
to the event log, `sessions.db`, `registry.db`, or config. No crash path can
corrupt durable state because no durable state is touched. `ReplayComplete` is
observationally idempotent (OL-F8).

---

## 8. dsh (DeepSeek Harness) mapping

dsh is a headless harness whose UI is a plugin (`00-architecture.md:69`, and sec
55 `:4860+`), so there is no dsh interactive session-open contract to mirror. Every
non-mirror cell carries its reason and a concrete anchor per `AGENTS.md`
(56-D6).

| dsh concept | ymh realization (77) | Anchor / non-mirror justification |
|---|---|---|
| Append-only-log resume by position | `event.subscribe` seeded by an opaque cursor; `CursorInvalid` -> `Beginning` | Mirrors 05 sec 8.5 (`05-transport.md:1177-1196`; T8/T16/T21/T23). No divergence. |
| Catch-up completion signal | additive `HostNotice{ReplayComplete}` after the Interactive replay batch | **Non-mirror, deliberate seam.** dsh has no interactive client to signal; ymh needs an explicit boundary so the open state and the derived state can be finalized (`src/transport/protocol_server.cpp:623-650`). The read-only `event.unsubscribed{replay_complete}` (`05-transport.md:913`) is `session.replay`-only and does not fit a live subscription. |
| Per-message context projection | one `context.show` per resume + per live message; none per replayed message | **Non-mirror, deliberate scope.** dsh exposes no `context.show` TUI gauge; the storm is a ymh UI artifact (`src/ui/supervisor.cpp:3569-3596`). Additive rule 77-D1. |
| Optimistic "opening..." UI | `OpeningState` rendered as a distinct non-working state | **Non-mirror.** dsh has no interactive open screen; ymh-only presentation (`src/ui/ui_render.cpp:511`; `00-architecture.md:69`). |
| Interrupted-turn truth | authoritative `session.resume` `status` reconciles the event-only projection to Idle | **Partially mirrors** dsh's "log is truth" only as far as the log is complete; ymh's `session.resume` appends nothing (`05-transport.md:904`), so the daemon's live `agentStatus` (`src/host/host_runtime.cpp:1096-1108`) is the authority for an interrupted turn. |
| Cursor retention across client objects | model-held `applied_through` bridges the `SupervisorConnection` replacement | **Non-mirror, deliberate.** dsh clients are stateless per request; ymh's supervisor owns a long-lived model that must survive an attach (`src/ui/supervisor.cpp:908-981`). |

---

## 9. Supersedes / amends (explicit)

- **Supersedes** the implicit per-`AssistantMessage` `context.show` behavior of
  the sink lambda at `src/ui/supervisor.cpp:938-940` (no spec pinned it; the code
  is the only carrier). Replacement: 77-D1.
- **Scopes / supersedes** the "comet visible whenever the session is working"
  clause of 63-D3 (`63-bottom-activity-indicator-errata.md:49`) and 63-I1 for the
  *opening* case: a session in `opening` is not `working` (77-D3 / OL4). 63-D1,
  63-D2, 63-D5, 63-D6 and 64-D1/D2 are **retained** unchanged.
- **Amends** `05-transport.md` sec 7.7 (Interactive `event.subscribe`): gains the
  additive `HostNotice{ReplayComplete}` after the catch-up batch (77-D4a); it does
  not replace the `session.replay`-only `event.unsubscribed{reason:
  "replay_complete"}` at `05-transport.md:913`, `:1187`, `:1477`, `:1645`.
- **Amends** `05-transport.md` sec 7.4 `:906` ("`session.resume` ... returns
  `AgentStatus::Idle`"): narrowed to "returns the daemon's current agent status
  (`Idle` | `Running`)", matching the shipped
  `src/host/host_runtime.cpp:822-823` and the `SessionResumed.status` contract
  (`include/ymh/transport/host.hpp:53-56`). 77-D4 reads this status; a `"Running"`
  result is honored (OL14).
- **Amends** 22 sec 5.2/5.3: the success branch retains SW25 (no workspace
  injection); the target is optimistically modeled and focused at submit
  (`beginOpening`), and the failure path retains 45-D10.8 recovery. No 22 clause
  is deleted.
- **Does not touch** 53, 63-D1/D2/D5/D6, 64, or 76, and changes no source/test
  file; this is a design document (its `DESIGN_STATUS.md` row is bumped by the
  authoring change set, sec "Tracker note").

**24 is referenced, not amended:** `session.resume` semantics (append nothing)
are unchanged; 77 adds only a UI projection reconciliation for a replay that ends
on a dangling turn. 76 is referenced for C1 and explicitly not re-designed.

---

## 10. Test plan

Hermetic; no network, no real daemon. Tests use the existing harness seams
(`include/ymh/ui/supervisor_harness.hpp`): `submitted_count("context.show")`
(`:75`), `drain_actions` (`:58`), `subscribe_request_count` (`:205`), plus the
**new** seams from sec 3.7: the sink-level `feed_sink_envelope`,
`deliver_sink_notice`, `set_daemon_turn_status` (which drive the real sink lambdas
where 77-D1 and 77-D4 live, M4), and the timer-level `resume_after_attach`,
`opening_expiry_tick` (which drive the real `beginOpening` call site and the real
`maybe_post_opening_expiry` scheduling decision, N2). `feed_session_envelope`
(`:186`) and `deliver_host_notice` (`:190`) remain for adapter-only tests. The
`SubscribeParams.from` capture lives in
`tests/unit/supervisor_connection_test.cpp`'s fake server.

### 10.1 `tests/unit/errata77_open_latency_test.cpp`

- **OL1 (O(1) context.show).** Build a 1000+-event session transcript
  (`TurnStarted`, N x {`AssistantMessage` + `ToolCall`/`ToolResult`},
  `TurnEnded`). Deliver each envelope through `feed_sink_envelope(ws, env,
  replay=true, cursor)` (real 77-D1 sink path), `drain_actions()` between batches;
  drive `apply_resume_success` so the one initial refresh runs (or
  `set_daemon_turn_status` + `deliver_sink_notice` for `ReplayComplete`). Assert
  `submitted_count("context.show") == 1` (the `apply_resume_success` refresh) and
  not `N`. Deliver one live (`replay=false`) `AssistantMessage` via
  `feed_sink_envelope`; assert the count is now 2.
- **OL4/OL5 (interrupted opens Idle, indicator not working during replay).**
  Replay `TurnStarted` only (no terminal) with `feed_sink_envelope`, then
  `set_daemon_turn_status(session, "Idle")`, assert `active_session_working()` is
  false while `opening` is set, then
  `deliver_sink_notice(ws, HostNotice{ReplayComplete, ws, session, ""})` and
  `drain_actions()`; assert `agent_state == Idle` and `active_session_working()`
  false. `animation_active_` has no harness getter, so assert its
  `drain()`-input proxy instead after `drain_actions()`:
  `!model().active_session_working()`, no aggregate flash, no streaming
  reasoning (`src/ui/supervisor.cpp:2159-2161`).
- **OL14 (running stays working).** Same sequence with
  `set_daemon_turn_status(session, "Running")`; assert the derived state remains
  active after `ReplayComplete`.
- **OL16 / OL-F4 (auto-tracked `live` + `ReplayComplete` never false-Idles).** Do
  **not** call `set_daemon_turn_status` (the auto-track case:
  `refresh_sessions` -> `track`, no `session.resume`). Replay a `TurnStarted` via
  `feed_sink_envelope(ws, env, replay=true, cursor)` so the derived state is
  `Thinking`, and record one `Running` direct child. Deliver
  `deliver_sink_notice(ws, HostNotice{ReplayComplete, ws, session, ""})` and
  `drain_actions()`; assert `active_session_working()` is still **true**, the
  direct child is still `Running`, and no `AgentStateChanged(Idle)` was applied.
  Removing the absent-entry early-return in `reconcile_after_replay` (or restoring
  the old `find`-and-`force_idle` body) fails this test. A second `ReplayComplete`
  is likewise a no-op (OL-F8).
- **OL-X2 / OL-F5 (timeout reconcile clears a dangling replay turn).** With the
  real `resume_after_attach` precondition (a canned `session.resume` reply whose
  `status` is `"Idle"`), `drain_actions()` (so the enqueued success action records
  the status via `set_daemon_turn_status` and runs `apply_resume_success`, X-4),
  replay a `TurnStarted` via `feed_sink_envelope(..., replay=true, ...)`, then
  **withhold** `ReplayComplete`. Backdate `opening.since` past `kOpeningTimeout`
  and drive `opening_expiry_tick(now)`; assert it returns **true**, that `opening`
  is cleared, **and** that `agent_state == Idle` / `active_session_working()` is
  false - the dangling turn is reconciled (OL-X3 at the replay apply, and
  idempotently by the escape, OL-X2), so the comet cannot re-arm. A companion case
  whose canned reply carries `status: "Running"` asserts the derived state is
  **retained** (OL14), proving the reconcile is not a blanket Idle.
- **X-1 / OL-X3 (escape *before* the dangling turn: still reconciled).** With the
  same real `resume_after_attach` precondition (`status: "Idle"`, then
  `drain_actions()`), replay only a non-dangling prefix (a completed
  `TurnStarted`+`TurnEnded`) via `feed_sink_envelope(..., replay=true, ...)`,
  backdate `opening.since`, and drive `opening_expiry_tick(now)` so the escape
  clears `opening` **before** the dangling event is applied. Then deliver the final
  `TurnStarted` via `feed_sink_envelope(..., replay=true, ...)` and assert
  `agent_state == Idle` / `active_session_working()` false **with no
  `ReplayComplete`** - the per-envelope reconcile (OL-X3) forced Idle the moment
  the late dangling turn was applied, which is exactly the ordering X-1 found
  unhandled. Removing the `if (replay) reconcile_after_replay(...)` call from
  `handle_sink_envelope` fails this test (the comet would re-arm after the escape).
  Companion: the same ordering with a `"Running"` canned reply retains the derived
  state (OL14).
- **OL7 (history preserved).** Assert the 1000 replayed entries are present in
  `SessionUiState::conversation.entries` after replay.
- **OL-F1 (backstop).**
  *Live mid-replay case:* with no `ReplayComplete`, deliver one live envelope via
  `feed_sink_envelope(..., replay=false, ...)`; assert `opening` is cleared.
  *Idle-attached case (real scheduling decision, N2):* seed a modeled workspace,
  install a canned `session.resume` reply, then `resume_after_attach(ws, session)`
  - the real call site that marks `opening` and sets `opening_pending_` (pins the
  previously-unstated `beginOpening` precondition), then `drain_actions()` (the
  success enqueue records the status via `set_daemon_turn_status` and runs
  `apply_resume_success`; X-4). Backdate the target
  `SessionUiState::opening.since` by more than `kOpeningTimeout` (N-L1:
  `opening`/`since` are public and `UiModel::expireOpenings` acts on the `now` it
  is handed, so no clock seam is needed - backdating `since` is the supported
  hermetic method). Call `opening_expiry_tick(now)` with `now` past the last post
  (`last_opening_post` is epoch before the first tick, so the first tick is due).
  Assert it returns **true** - the production decision posted an expiry Task while
  an opening was set, the fact a direct `expire_openings()` call could not
  establish (N2) - and that `!model().hasAnyOpening()` (`opening` cleared)
  **without any `drain_actions()`**. Removing `maybe_post_opening_expiry`'s
  `opening_pending_`/rate-limit decision fails this test; the test never calls
  `expire_openings()` directly.
- **OL15 (bounded opening, production scheduling decision).** Same precondition
  (`resume_after_attach` on a modeled workspace, no `ReplayComplete`). The flag is
  observed through the decision, not a raw read: `opening_expiry_tick(now)`
  returns **true** (⇒ `opening_pending_` set; no `opening_pending_` getter is
  pinned - `maybe_post_opening_expiry`'s return is the pinned observer and
  `model().hasAnyOpening()` its model half). `opening_expiry_tick` calls the
  **exact** `SupervisorApp::maybe_post_opening_expiry` the timer body calls, with
  an injected `now` and a `post` that synchronously runs the production-delivered
  closure (the real `SupervisorApp::expire_openings`), and **not**
  `drain_actions()`. Backdate `opening.since` past `kOpeningTimeout` before the
  first tick, so that same tick's delivered closure clears `opening`; then advance
  `now` past `kPresencePostInterval` and a second `opening_expiry_tick(now)`
  returns **false** (⇒ the flag was recomputed false, distinguishable from the
  rate limit because `now` was advanced). *Companion
  scheduling assertion (N1/N2):* the decision is `animation_active_`-independent
  by construction - `maybe_post_opening_expiry` takes no animation input and the
  timer reaches it only after the `animation_active_` branch is skipped - and with
  no opening the decision returns false so the tick falls through to the
  animation/presence logic (N-L3: the presence heartbeat is not suppressed while
  an opening pends).

### 10.2 `tests/unit/errata77_subscribe_seed_test.cpp`

- **OL2 (caller seed initializes the cursor).** `SupervisorConnection` with a fake
  host; `track(session, EventCursor{"S"})` with no prior cursor; assert the
  captured `SubscribeParams.from.kind == Cursor` and `from.cursor.value == "S"`.
- **OL7 (fresh open uses Beginning).** `track(session)` with no seed and no
  retained cursor; assert `from.kind == Beginning`.
- **OL3 (never Now).** Assert no captured subscribe ever carries
  `StreamFrom::Kind::Now` for a model-unmaterialized session.
- **OL10 (M1: reconnect uses the fresher cursor, never the stale seed).** Deliver
  a `StreamNotification` (advancing `cursors_` to `"A"`), then call
  `track(session, EventCursor{"S"})` again (a stale track-time seed), drop and
  re-attach the connection; assert the resubscribe uses `Cursor("A")`, **not**
  `Cursor("S")` and not `Beginning`.
- **OL-F2 (CursorInvalid).** The fake server returns `CursorInvalid`; assert the
  retry is `Beginning`.

### 10.3 `tests/unit/errata77_replay_complete_test.cpp`

- **OL13.** Drive `ProtocolServer::handle_subscribe` with an Interactive
  connection and a non-empty session: assert exactly one
  `HostNotice{ReplayComplete}` is enqueued, after all `event.stream` frames, with
  the subscribed session id.
- **OL13 (replay-only excluded).** `session.replay` (`replay_only == true`) emits
  `event.unsubscribed{reason:"replay_complete"}` and **no**
  `HostNotice{ReplayComplete}`.
- **OL9 (additive wire).** Assert `parse_host_notice_kind("replay_complete")`
  yields the new value and that every pre-existing kind string still parses.

### 10.4 Regression

`tests/unit/supervisor_connection_test.cpp`,
`tests/unit/ui_event_adapter_test.cpp`,
`tests/unit/errata58_ui_test.cpp`, `tests/unit/errata57_ui_test.cpp`,
`tests/unit/errata53_ui_test.cpp`, `tests/unit/session_catalog_test.cpp`, and
`tests/integration_two_process_test.cpp` (updated sink lambdas must be recompiled
against the new `on_envelope` signature; the assignment sites are
`:658`, `:1021`, `:1156`, plus
`tests/integration_host_harness_test.cpp:207-208`) must stay green.
`UiEventAdapter::onHostNotice`'s `switch` gains the no-op `ReplayComplete` case
(sec 3.2), exercised by `tests/unit/ui_event_adapter_test.cpp`. The golden
render tests (`tests/unit/ui_render_golden_test.cpp`) gain one snapshot for the
`"opening <title>..."` row. The new harness seams (sec 3.7) are added to
`include/ymh/ui/supervisor_harness.hpp` and defined in `src/ui/supervisor.cpp`;
the five seams call the extracted `handle_sink_envelope` / `handle_sink_notice` /
`set_daemon_turn_status` member functions (N-L2) and the real
`resume_after_attach` / `maybe_post_opening_expiry` production paths (N2).

---

## 11. Compatibility (ABI and wire) - Oracle-flagged implication

- **Wire (JSON-RPC):** the only change is the additive
  `HostNoticeKind::ReplayComplete` value on the existing `HostNotice` envelope.
  No method, request, response, or existing field changes; no field is added,
  renamed, reordered, or retyped. This follows the protocol's established
  additive pattern - an appended value with a preserving default (see the
  additive `HelloParams::role` field and its "additive; default preserves v1"
  comment at `include/ymh/transport/protocol.hpp:303`, and the pinned
  wire-enum-string rule T22 at `05-transport.md` sec 3). Per the task constraint
  this is the single explicitly justified wire addition.
- **`context.show` ABI:** the request/response shape is unchanged. Only the
  request *volume* changes (from O(replayed messages) to O(1) per resume plus
  O(live messages)). No server-side change is needed; `context.show` remains
  stateless and idempotent. A client that still sent per-message `context.show`
  would be served exactly as before, so mixed-version operations do not break.
- **C++ ABI (internal):** `SupervisorSink::on_envelope` and
  `SupervisorConnection::track` change signature; both are supervisor-internal
  (no shared-library boundary; the supervisor and daemon ship as one binary).
  All call sites are in-tree and are listed in sec 3.2/3.3. Tests are updated.
- **Durability:** no schema change, no migration, no durable write.

---

## 12. Open questions

- **77-OQ1.** Should the durable `session_snapshots.at_sequence` become the
  cross-process open seed (with a daemon-side messages projection consumer), so a
  restarted supervisor can render history from the snapshot and subscribe from the
  tail instead of replaying from `Beginning`? This is deferred: `checkpoint`/
  `loadSnapshot` are unwired (no production caller) and the projection consumer
  does not exist, so pinning it here would be a dead requirement (77-D2).
- **77-OQ2.** Should the dangling direct-subagent case (`SubagentSpawned` with no
  `SubagentFanIn`) be reconciled against an authoritative daemon subagent status
  rather than inferred from the turn status? 77-D4 conservatively cancels
  replay-derived `Running` children when the turn status is non-`Running`; a
  dedicated `subagent.status` read would be stronger but is out of scope.
- **77-OQ3.** [RESOLVED by 77-D3 / OL15 in Rev 2; driver corrected in Rev 3 by
  N1] The `opening` state is bounded by `kOpeningTimeout` (30 s) via
  `SupervisorApp::maybe_post_opening_expiry` (the scheduling decision, N2)
  delivering `SupervisorApp::expire_openings` -> `UiModel::expireOpenings` while
  `opening_pending_` is true (1 s cadence), **not** via `drain()`. The first-live-envelope backstop and the link-drop cleanup remain
  the fast paths; the wall clock is the guaranteed bound for an idle-attached
  session whose `ReplayComplete` was lost (OL-F1) or never emitted (OL-F5).
- **77-OQ4.** Should the auto-tracked daemon-`live` session path
  (`src/ui/supervisor.cpp:1931-1936`, which calls `track(session)` with no
  `session.resume` and therefore no `set_daemon_turn_status`) fetch an
  authoritative turn status so `reconcile_after_replay` can run for it (L5)? A
  candidate source is `session.list`'s `live` flag or an `agent.status` read.
  Deferred: these sessions are normally already `live` and driven by live events,
  so the false-active window is bounded in practice; no new RPC is introduced in
  this revision.

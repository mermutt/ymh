# 61 — Composer/Caret and Step-Limit Resilience Errata

Status: **Rev 3 — draft** (authored under the task's explicit diagnose-and-fix
directive; pending independent verification; see `DESIGN_STATUS.md`). Rev 3
reworks the step-limit design after the Rev 2 Oracle gate returned **DO NOT
APPROVE**:

- **HIGH-1** the no-progress guard was defeated by tools that report failure as
  `Ok` — `ShellTool` returned `Ok` for a non-zero exit, so a failing-`shell`
  loop scored productive every step.
- **MEDIUM-1** the guard stopped legitimate work early: three consecutive
  non-`Ok` steps aborted distinct informative failures (three absent paths,
  three failing builds, three timeouts) that pre-61 ran to 100.
- **MEDIUM-2** dropping auto-continuation re-opened the user's original
  complaint (defect #3): a productive job longer than `max_steps` still needed a
  user message.

Rev 3 resolves all three with **progress-gated continuation**: a turn
auto-continues while it is genuinely making progress and stops fast when it is
not, and tool outcomes are made honest so "progress" can be measured.

This errata covers three user-reported defects on the TUI surface:

1. **Composer overflow (defect fix).** A draft long enough to need the whole
   display was clipped: `render_input` (`src/ui/ui_render.cpp`) capped its height
   only by `kComposerMaxRows = 8`, never by the height the layout actually had
   left. On a short terminal (or with chrome rows present) FTXUI's box shrinker
   (`build/_deps/ftxui-src/src/ftxui/dom/box_helper.cpp` `ComputeShrinkHard`)
   clipped the composer's bottom rows, so the caret row left the screen.
2. **Cursor flashing during the spinner (design change).** The repaint timer
   (`src/ui/supervisor.cpp`) posts `Event::Custom` every `kFrameInterval = 50 ms`
   while `animation_active_` is set. Each `ScreenInteractive::Draw` emits
   `ResetCursorPosition`, rewrites the whole screen, then repositions the
   hardware cursor. A visible cursor therefore traverses the screen every frame.
3. **Step limit abandons the turn (design change).** `06-agent-loop.md` §5.8 pinned
   `maxSteps` → `TurnFailed{StepLimitExceeded}` → `AgentState::Error`. The UI
   adapts that to an `error:` transcript row (`ui_event_adapter.cpp`,
   `ui_model.cpp`), so a long but productive job looks like a crash and must be
   re-prompted. The user's report was *"a long running job seemed to stop
   execution after displaying an error. Once I prompted again it completed."*

## 1. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 61-D1 | `render_input` takes `max_rows` (the height left after the fixed chrome, already `<= kComposerMaxRows`) and uses it as its window budget. `build_ui` measures the header/breadcrumb/separators/scroll-hint/subagents/command-hints/status rows, reserves them, and passes the remainder. The transcript (the `flex` row) collapses first. | Extends 59-D1/59-I4; defect fix |
| 61-D2 | The composer caret's cursor shape is `Hidden` **iff the rendered composer's session is animating** (its turn is in an active state, or one of **its own** reasoning entries is streaming) **and** that session's composer was not edited within `kComposerCaretGrace` (700 ms); otherwise `Bar`. The edit instant is per-session (`SessionUiState::composer_input_at`), the same scope the renderer arms from, so an edit in workspace A cannot force `Bar` in workspace B and a background workspace animating never hides the focused caret. | Amends 48-D5.1 / 59-D4 |
| 61-D3 | `agent.max_steps` is the **per-segment step budget** (default 100, clamped to `>= 1`), not the per-turn ceiling and not multiplied by any segment factor. A segment that commits a successful tool call auto-continues into a new segment (61-D8); a segment with no success stops at the budget. The printed message names the enforced value. | Amends 06 §5.8, §5.7, §5.6, A-F4, decision (g) |
| 61-D4 | On a stop — a segment budget with no success, the no-progress guard (61-D6), or the optional segment cap (61-D8) — the turn stops with the durable `TurnFailed{StepLimitExceeded}` (no new `EventType`, per the 55-A13 precedent) whose message is actionable ("… task incomplete; send a message to continue"). The **`AgentLoop` itself returns to `Idle`** via `appendTurnFailed(..., recoverable=true)`; it is not left in `Error`. The adapter's `StepLimitExceeded → Idle` projection is retained only as a replay/resume backstop. | Amends 06 §5.8 / A-F4 |
| 61-D5 | The adapter maps a `StepLimitExceeded` `TurnFailed` to a new frontend-only `StepLimitReached` `UiEvent` (never on the wire) instead of `ErrorOccurred`; the model renders it as a `ConversationRole::Notice` row (bold yellow, not the dim `System`/`error:` style), records the message in the status note, and treats it as conversation content (`scroll.onNewContent`). | New; extends 10 §5.1 (sketch amended) |
| 61-D6 | **Progress and the no-progress guard.** A step's *action signature* digests the ordered tool calls (name + canonical JSON arguments); its *result signature* digests the ordered committed results (name + outcome + error + output); both exclude the per-call unique id. A step **makes progress** iff its action signature is novel in the turn, **or** it commits at least one `Ok` result whose result signature is novel. When `kNoProgressStreak = 3` consecutive steps make no progress the loop stops immediately (before the segment budget) with the same recoverable `TurnFailed{StepLimitExceeded}`. This catches a repeated identical failing call and an alternating A/B failing-call loop — neither of which the advisory `RepeatToolReminder` bounds — while distinct informative failures remain progress. | New; amends 06 §5.6 / §5.8 / A-F4 |
| 61-D7 | **Honest tool outcomes.** `ShellTool` sets `ToolOutcome::Error` on a non-zero exit (the combined output still carries the `exit_code: N` line, and `error` carries `exit_code: N`). A tool-layer failure must not be reported as an unqualified `Ok`, so every consumer — the UI (`✗`), the headless/CLI printer (`!!`), and the progress guard — sees the failure. This is what makes a repeated failing-`shell` loop count as no progress (61-D6). | New; amends 07 §14.1 |
| 61-D8 | **Progress-gated continuation.** At the segment budget (`max_steps`) a segment that committed a successful tool call auto-continues into a new segment with **no user message**; `turn_step`/`StepId` stay monotonic so `force_first_tool_call` cannot re-trigger. `agent.max_segments` (default **0 = unlimited**) optionally caps the number of segments per turn; a positive cap is a recoverable stop. A segment with no success stops (61-D3/D4). This is the resolution of defect #3: a legitimate long productive job is no longer re-prompted. | New; resolves defect #3 |

`kNoProgressStreak` is a compile-time safety constant. The configurable knobs are
`agent.max_steps` (per-segment budget, default 100) and `agent.max_segments`
(default 0 = unlimited).

### Worst-case bound (H1, MEDIUM-1, MEDIUM-2)

| Case | Steps | Wall-clock @ ~2 s/round-trip | Output tokens @ ~10 k/call |
|---|---|---|---|
| Runaway **repetitive** loop (identical, or periodic A/B) | `kNoProgressStreak + D`, D = distinct signatures before the cycle (`D <= 2`) → **<= 5** | ~10 s | ~50 k |
| **Novel but non-progressing** loop (distinct failures, no `Ok` — e.g. probing many distinct absent paths) | **`max_steps` = 100** (no `Ok` in the segment ⇒ no continuation) | ~200 s | ~1 M |
| **Legitimate long productive job** (> `max_steps`) | **auto-continues** while each segment has an `Ok`; no step-count bound by default — bounded by task completion, or by `max_segments` when an operator sets it | task-dependent | task-dependent |

The pre-61 bound was `max_steps` = 100 steps for every case. Rev 3 is **not
weaker for the runaway case**: a repetitive loop stops in `<= 5` steps (vs 100),
and a novel non-`Ok` loop stops at 100 (identical to pre-61). The productive case
is deliberately unbounded so that the user's complaint (a productive job stopping
and needing a re-prompt) is resolved; this is the explicit trade-off. Operators
who want a hard stop for a productive job set `agent.max_segments > 0`, at which
point a job exceeding `max_steps × max_segments` re-prompts — stated plainly
here. Rev 1's unconditional `max_steps × 10 = 1000` continuation remains removed:
continuation now requires progress.

## 2. Interface sketch

```cpp
// src/ui/ui_render.cpp (anonymous namespace)
constexpr int kComposerMaxRows = 8;
constexpr std::chrono::milliseconds kComposerCaretGrace{700};
Element render_input(const UiModel& model, const Theme& theme, int terminal_width,
                     int max_rows, bool hide_caret);
Element caret_anchor(Element element, ftxui::Screen::Cursor::Shape shape);

// src/agent/agent_loop.cpp (anonymous namespace)
constexpr std::size_t kNoProgressStreak = 3;
std::string action_signature(const std::vector<ToolCallAssembled>& calls);
std::string result_signature(const ToolScheduleOutcome& outcome);
bool        step_had_success(const ToolScheduleOutcome& outcome);

// include/ymh/agent/agent.hpp
std::size_t max_steps = 100;      // per-segment step budget (61-D3/D8)
std::size_t max_segments = 0;     // segments per turn; 0 = unlimited (61-D8)

// include/ymh/agent/agent_loop.hpp
void appendTurnFailed(TurnId turn, AgentErrorCode code, std::string message,
                      bool recoverable = false);   // recoverable => Idle

// include/ymh/ui/ui_event.hpp
struct StepLimitReached { SessionId session; std::string message; };

// include/ymh/ui/ui_model.hpp
enum class ConversationRole : std::uint8_t { …, Notice };
// SessionUiState (61-D2): per-session, so an edit in A cannot arm B.
std::optional<std::chrono::steady_clock::time_point> composer_input_at;
[[nodiscard]] static bool session_has_streaming_reasoning(const SessionUiState& state);
```

## 3. Invariants

| ID | Invariant |
|---|---|
| 61-I1 | The composer renders at most `min(kComposerMaxRows, max(1, inner_height - reserved))` rows, where `inner_height = size.height - 2` and `reserved` is the measured fixed chrome. The `max(1, …)` clamp means that when `inner_height - reserved <= 0` the composer still renders one row (best-effort, 61-F6); in that degenerate case the caret is **not** guaranteed on-screen. Whenever the clamp is not hit, the caret row is within the screen. |
| 61-I2 | When the composer needs the whole display the transcript row collapses to zero; it is never the composer that is clipped. |
| 61-I3 | A one-row draft renders identically to pre-61 (59-I5 preserved). |
| 61-I4 | The caret is `Cursor::Bar` unless the **rendered composer's** session is animating and that session's composer was not edited within `kComposerCaretGrace`; then it is `Cursor::Hidden`. "Animating" = `is_active_state(composer.agent_state) \|\| composer has a streaming `Reasoning` entry`, via the shared `session_has_streaming_reasoning` helper. A background session's animation does not affect the focused caret, and a background session's edit does not arm the focused caret. |
| 61-I5 | A segment executes at most `max(1, max_steps)` steps before it either auto-continues or stops; `turn_step`/`StepId` are monotonic for the whole turn. |
| 61-I6 | A segment that commits at least one successful (`Ok`) tool call auto-continues into a new segment with no user message; a segment that commits none stops. Both the stop paths and the optional segment cap are terminal but **recoverable** (`Idle`, not `Error`). |
| 61-I7 | Every stop terminal event is a durable `TurnFailed{StepLimitExceeded}` with a non-empty actionable message, and the **`AgentLoop` state is `Idle`** — set at the loop layer by `appendTurnFailed(..., recoverable=true)`, not merely projected by the adapter. |
| 61-I8 | `StepLimitExceeded` never renders as `error:` and never sets `ApiConnectivity::Error`. |
| 61-I9 | A step makes progress iff its action signature is novel in the turn, or it commits an `Ok` result whose result signature is novel. The no-progress guard stops the turn after `kNoProgressStreak` consecutive non-progress steps; this includes a repeated identical call and an alternating A/B failing-call loop, which the consecutive-identical repeat reminder never trips, while distinct informative failures are progress and do not trip it. |
| 61-I10 | A `StepLimitReached` is conversation content: it raises `scroll.unseen` when the user has paged up, exactly like `ErrorOccurred` and the other `conversation_event`s. |
| 61-I11 | A non-zero `shell` exit is `ToolOutcome::Error` (61-D7); the combined output and the `exit_code: N` line still reach the model unchanged. |
| 61-I12 | With `max_segments = 0` (the default) there is no step-count cap on a productive turn; the turn ends only when the model stops calling tools, on cancel/failure, or on the no-progress guard. |

## 4. Failure modes

F# tags per `00-architecture.md` §54.

| ID | Failure | Mitigation |
|---|---|---|
| 61-F1 | (F6) The hidden caret also hides a genuinely idle composer caret. | The predicate is scoped to the rendered session's animation and exempts a recent edit to that same session (61-D2/I4); `WaitingForPermission`/`WaitingForInput` are not active states. |
| 61-F2 | (F6) The composer height predicate diverges from the layout's actual reservation. | `build_ui` measures the same elements it later places (61-D1). |
| 61-F3 | (F8) Unbounded auto-continuation. | Removed: continuation requires a successful tool call in the segment (61-D8/I6); the no-progress guard stops repetitive loops at `kNoProgressStreak` (61-D6/I9); `max_segments` bounds a productive turn when set. |
| 61-F4 | (F8) A segment budget reset re-triggers `force_first_tool_call`. | `segment_step` resets but `turn_step`/`StepId` stay monotonic for the whole turn (61-D8/I5). |
| 61-F5 | A `StepLimitReached` event is replayed as an error. | The adapter routes only `TurnFailed{StepLimitExceeded}` to it; `project_state` returns `Idle` for that code (61-D4/D5). |
| 61-F6 | A short terminal cannot fit even one composer row. | `max_rows` clamps to `>= 1`; degenerate terminals remain best-effort (61-I1). |
| 61-F7 | (F8) An alternating A/B failing-call loop never trips the repeat reminder and runs to the ceiling. | The no-progress guard stops it at `kNoProgressStreak` non-progress steps (61-D6/I9); test 61-U7. |
| 61-F8 | A recoverable stop reuses the running→`Idle` edge, so `armOnEdge` arms the completion flash and `attention.completed` is set — the stop can read as a *completion*. | Accepted and documented (L4): the bold `Notice` and the status note are the authoritative signals; the flash is a brief visual. A distinct "stopped" edge is deferred. |
| 61-F9 | A **background** session's animation still sets the aggregate `animation_active_` (`supervisor.cpp`), so the 50 ms repaint timer runs while the focused caret is `Bar`, and the caret can briefly traverse the screen. | Accepted and documented (M2 residual): the caret no longer *hides* (61-D2 fixed the visible-caret case); only the redundant repaint remains. Fixing it would need a per-session repaint predicate and a check that the esc-arm/delete-arm timeouts still tick; deferred. |
| 61-F10 | (F8) A productive-but-pointless loop (distinct successful calls forever) is indistinguishable from real work and is bounded only by task completion. | Deliberate (MEDIUM-2 trade-off): the default imposes no re-prompt; operators set `agent.max_segments` to bound it (61-D8/I12). |

## 5. dsh mapping

| dsh behaviour | this spec | justification / anchor |
|---|---|---|
| Composer that never leaves the viewport | **mirrored** (61-D1) | Defect fix within 59-D1; the cap becomes the available height, not a constant. |
| Caret hidden while the agent runs | **mirrored, scoped** (61-D2) | dsh's input caret is not the terminal cursor during streaming; hiding avoids the per-frame repaint artefact. Scoped to the rendered session and suspended for a recent edit to that session so typing stays visible. |
| Long turn continues without user re-prompt | **mirrored, progress-gated** (61-D8) | dsh's graduated reminders + steering (`26 G29`, `26-dsh-alignment.md:1244`) keep a productive turn alive without a user message; ymh now mirrors that *outcome* through progress-gated continuation: a segment with a successful tool call auto-continues, a segment without one stops (61-D3/D4/D6/D8). This resolves defect #3; Rev 1's unconditional `max_steps × 10` was rejected as a safety regression (Oracle H1) and is not reinstated. Steering remains unbuilt (`55-multi-agent-delegation-errata.md`). |
| Hard stop with an actionable, recoverable message | **mirrored** (61-D4/D5) | 06 §5.8's `TurnFailed{StepLimitExceeded}` is retained as the durable record; only its UX and state are corrected. |
| A new durable step-budget `EventType` | **not mirrored** | Deliberate scope decision: the existing `TurnFailed` already carries a code+message and is durable; a new `EventType` adds wire/schema surface for no semantic gain (55-A13 precedent, `55-multi-agent-delegation-errata.md:1517`). |
| Tool outcome distinguishes success from failure | **mirrored** (61-D7) | A non-zero `shell` exit is `Error`, matching the tool-contract's success/failure taxonomy (07 §14.1); this is what lets the progress guard be sound (Oracle HIGH-1). |

## 6. Test plan

| ID | Test | Asserts |
|---|---|---|
| 61-U1 | `UiRenderGolden.ComposerFillsScreenKeepsCaretVisible` | 61-I1/I2: on a 40×10 terminal a 500-char draft keeps `END_MARKER` visible and `cursor().y < dimy()`, shape `Bar`. |
| 61-U2 | `UiRenderGolden.CursorHiddenWhileTurnActive` | 61-I4: focused `AgentState::Thinking` with no recent edit ⇒ `cursor().shape == Hidden`. |
| 61-U3 | existing `LongComposer*`/`UI51_D2_ComposerTintAndBarGolden` | 61-I3: short/normal composer rendering unchanged. |
| 61-U4 | `UiRenderGolden.CursorBarWhileEditingDuringTurn` | 61-I4: focused turn active **and** a composer edit within the grace window ⇒ `Bar`. |
| 61-U5 | `AgentLoop.StepCeilingStopsANonProductiveSegment` | 61-I5/I6/I7: `max_steps = 3` with 6 distinct non-`Ok` reads stops after exactly 3 `StepStarted` with the **step-limit** (not no-progress) message, one `TurnFailed{StepLimitExceeded}`, and `Idle`. |
| 61-U6 | `UiEventAdapter.StepLimitExceededIsARecoverableNotice` | 61-I8: `StepLimitExceeded` yields `StepLimitReached`, a `Notice` row (no `error:`), `ApiConnectivity != Error`, `AgentState::Idle`. |
| 61-U7 | `AgentLoop.NoProgressGuardStopsAlternatingErrorLoop` | 61-I9: `max_steps = 100` with an A/B/A/B failing-call script stops after 5 `StepStarted` (two distinct actions + three non-progress steps), one `TurnFailed` whose message names no progress, and `Idle`. |
| 61-U8 | `UiModel.StepLimitNoticeRaisesUnseenWhileScrolled` | 61-I10: a `StepLimitReached` applied while scrolled up raises `scroll.unseen` and appends a `Notice`. |
| 61-U11 | `AgentLoop.NoProgressGuardStopsRepeatedFailingShell` | 61-I9/I11, HIGH-1: a repeated `shell("exit 1")` loop is no progress and stops after 4 `StepStarted`, one `TurnFailed` naming no progress, and `Idle`. |
| 61-U12 | `AgentLoop.DistinctNonOkStepsDoNotTripGuard` | 61-I9, MEDIUM-1: three distinct absent-path reads followed by a text step end with `TurnEnded`, **zero** `TurnFailed`, and 4 `StepStarted`. |
| 61-U13 | `AgentLoop.ProductiveJobContinuesPastMaxSteps` | 61-I6/I12, MEDIUM-2: 9 distinct successful reads with `max_steps = 3` run all 9 tool steps plus the text step (10 `StepStarted`) with **zero** `TurnFailed` and `TurnEnded`. |
| 61-U14 | `BuiltinTools.ShellCapturesOutputAndExitCode` | 61-I11: `printf hi` ⇒ `Ok`; `exit 7` ⇒ `Error` with `error == "exit_code: 7"` and the output unchanged. |
| 61-U15 | `UiRenderGolden.CursorHiddenInOtherSessionAfterEditingFirst` | 61-I4, LOW-1: editing workspace A then switching to an animating workspace B yields `Hidden` in B (the grace is per-session). |

### Pre-fix evidence (honest)

Two classes of test:

- **Behavioural, pre-fix-compilable** (proven failing against the Rev 2 code by
  reverting only the production sources, keeping the new API so the tests still
  compile):
  - `61-U5`: pre-fix the 6 distinct reads stopped after 3 with the **no-progress**
    message, not the step-limit message — the guard conflated distinct non-`Ok`
    work with no progress (MEDIUM-1).
  - `61-U7`: pre-fix `StepStarted == 3` (the all-non-`Ok` streak), so the
    assertion `== 5` fails.
  - `61-U11`: pre-fix `TurnFailed == 0` — the shell returned `Ok`, so the guard
    never tripped and the script ran to exhaustion (HIGH-1).
  - `61-U12`: pre-fix `TurnFailed == 1`, `TurnEnded == 0`, `StepStarted == 3` —
    the three distinct failures were stopped as no progress (MEDIUM-1).
  - `61-U13`: pre-fix `TurnFailed == 1`, `TurnEnded == 0`, `StepStarted == 3` —
    the ceiling stopped the productive job and required a re-prompt (MEDIUM-2).
  - `61-U14`: pre-fix `exit 7` yielded `Ok` (and no `error`), so the tool-layer
    assertion fails (HIGH-1).
- **API-guard** (cannot compile pre-fix, so they prove the API was absent rather
  than a runtime failure): `61-U4`'s grace half (`composer_input_at`), `61-U6`
  (`ConversationRole::Notice`/`StepLimitReached`), `61-U8` (`StepLimitReached`),
  and `61-U15` (the per-session `SessionUiState::composer_input_at`). Their
  runtime behaviour is only exercised post-fix; the behavioural proof of the
  step-limit fix is `61-U5`/`61-U7`/`61-U11`/`61-U12`/`61-U13`/`61-U14`.

Pre-fix `61-U1` also fails (`END_MARKER` absent, caret off-screen) and `61-U2`
fails (`cursor().shape == Bar`).

## 7. Supersedes / amendments

- **Extends 59-D1/59-I4**: the composer cap is `min(kComposerMaxRows, available)`,
  not a fixed `kComposerMaxRows`.
- **Amends 48-D5.1** (`48:372-388` focus ownership, `48:390-398` caret shape) /
  **59-D4**: the caret remains the single focus owner and the glyph/box tracking
  is unchanged; only its **shape** is conditional (61-D2), scoped to the rendered
  composer's session with a per-session edit grace. The idle case still asserts
  `Bar` in the §5.5/§13.2 caret goldens (`CaretCursorLandsAtInputPosition`,
  `CaretCursorHandlesCjkLeadingCell`, `UI51_D2_ComposerTintAndBarGolden`).
- **Amends 06** (all sites, with anchors):
  - §5.7 transition table (`06:369-371`): a segment budget with ≥1 success
    auto-continues (`Thinking` → `Thinking`, no durable event); a segment budget
    with no success and the no-progress guard both → `Idle` with a recoverable
    `TurnFailed`.
  - §5.7 diagram (`06:353`): segment budget/no progress → `Idle`, not `Error`;
    a productive segment auto-continues.
  - §5.6 pseudocode (`06:645-652`): `noProgressStreak >= kNoProgressStreak` →
    `TurnFailed{StepLimitExceeded}` + `Idle`; `segmentStep >= maxSteps` with a
    success → reset and continue, without a success → `TurnFailed` + `Idle`.
  - §5.8 (`06:837`): the per-segment budget, progress-gated continuation,
    `max_segments`, the progress definition, and the honest-`shell`-outcome note.
  - A-F4 (`06:1134`): segment has no successful call at `max_steps` **OR**
    `noProgressStreak >= kNoProgressStreak`; flush + `TurnFailed` + `Idle`
    (recoverable); a segment with a successful call auto-continues.
  - Decision (g) (`06:1364-1371`): a progress-gated recoverable loop-policy stop,
    not a failure.
- **Amends 10 §5.1** (`10:661`): the pinned `UiEvent` variant gains
  `StepLimitReached` (61-D5); the sketch is updated in place, not only "extended".
- **Amends 48-D6.2** (`48:512`): `Notice` → `FinalAnswer` in the role→`Presentation`
  table and classifier (61-D5).
- **Amends 45-D7** (`45:905`): `StepLimitReached` is not `ErrorOccurred` and
  leaves `ApiConnectivity` unchanged (61-D5).
- **Amends 07 §14.1** (the tool-contract outcome taxonomy): `ShellTool` maps a
  non-zero exit to `ToolOutcome::Error` (61-D7).
- **Retires the Rev 2 `step_made_progress` empty-id skip (LOW-2).** The Rev 2
  helper ignored results with an empty `id` without explanation. The Rev 3
  signature helpers do not: `commitToolResult` guarantees every committed result
  carries its call's id, and the action/result signatures are computed over the
  whole step, so the skip was dead and is removed.
- **No amendment to 59's wrap/glyph rules** (59-D3/59-I6) or to the renderer's
  single-focus-owner guarantee (59-D4).

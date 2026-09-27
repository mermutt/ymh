# 61 — Composer/Caret and Step-Limit Resilience Errata

Status: **Rev 2 — draft** (authored under the task's explicit diagnose-and-fix
directive; pending independent verification; see `DESIGN_STATUS.md`). Rev 2
reworks the step-limit design after the Rev 1 Oracle gate returned
**DO NOT APPROVE** (H1 counter-only auto-continue weakened the safety bound 10×;
H2 the `Idle` claim was false at the layer it named; H3 the `06` amendment was
incomplete and self-contradictory; M1–M4; L1–L4).

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
3. **Step limit abandons the turn (design change).** `06-agent-loop.md` §5.8 pins
   `maxSteps` → `TurnFailed{StepLimitExceeded}` → `AgentState::Error`. The UI
   adapts that to an `error:` transcript row (`ui_event_adapter.cpp`,
   `ui_model.cpp`), so a long but productive job looks like a crash and must be
   re-prompted.

## 1. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 61-D1 | `render_input` takes `max_rows` (the height left after the fixed chrome, already `<= kComposerMaxRows`) and uses it as its window budget. `build_ui` measures the header/breadcrumb/separators/scroll-hint/subagents/command-hints/status rows, reserves them, and passes the remainder. The transcript (the `flex` row) collapses first. | Extends 59-D1/59-I4; defect fix |
| 61-D2 | The composer caret's cursor shape is `Hidden` **iff the focused session is animating** (its turn is in an active state, or one of **its own** reasoning entries is streaming) **and** the focused composer was not edited within `kComposerCaretGrace` (700 ms); otherwise `Bar`. The predicate is scoped to the focused session — a background workspace animating never hides the focused caret. | Amends 48-D5.1 / 59-D4 |
| 61-D3 | `agent.max_steps` is the **per-turn hard ceiling**, unchanged from pre-61 (default 100), **not multiplied** by any segment factor. The ceiling is clamped to `>= 1` (`max_steps = 0` → 1) and the printed message names the enforced value. | Amends 06 §5.8, §5.7, §5.6, A-F4, decision (g) |
| 61-D4 | On the hard ceiling — and on the no-progress guard (61-D6) — the turn stops with the durable `TurnFailed{StepLimitExceeded}` (no new `EventType`, per the 55-A13 precedent) whose message is actionable ("… task incomplete; send a message to continue"). The **`AgentLoop` itself returns to `Idle`** via `appendTurnFailed(..., recoverable=true)`; it is not left in `Error`. The adapter's `StepLimitExceeded → Idle` projection is retained only as a replay/resume backstop. | Amends 06 §5.8 / A-F4 |
| 61-D5 | The adapter maps a `StepLimitExceeded` `TurnFailed` to a new frontend-only `StepLimitReached` `UiEvent` (never on the wire) instead of `ErrorOccurred`; the model renders it as a `ConversationRole::Notice` row (bold yellow, not the dim `System`/`error:` style), records the message in the status note, and treats it as conversation content (`scroll.onNewContent`). | New; extends 10 §5.1 (sketch amended) |
| 61-D6 | **No-progress guard.** A step is *productive* iff at least one of its committed tool results has `ToolOutcome::Ok`. When `kNoProgressStreak = 3` consecutive steps are unproductive the loop stops immediately (before the ceiling) with the same recoverable `StepLimitExceeded` `TurnFailed`. This catches both a repeated identical failing call and an **alternating A/B failing-call** loop, neither of which the advisory `RepeatToolReminder` bounds. | New; amends 06 §5.6 / §5.8 / A-F4 |

`kNoProgressStreak` is a compile-time safety constant. The only configurable
ceiling remains `agent.max_steps` (default 100).

### Worst-case bound (H1)

| Case | Steps | Wall-clock @ ~2 s/round-trip | Output tokens @ ~10 k/call |
|---|---|---|---|
| Unproductive loop (error-only or alternating A/B) | **3** | ~6 s | ~30 k |
| Productive loop (≥1 successful call per step) | `max(1, max_steps)` = **100** | ~200 s | ~1 M |

The pre-61 bound was `max_steps` = 100 steps. The Rev 2 bound is **not weaker**:
the productive worst case is identical to pre-61, and the unproductive case is
**33× tighter** (3 vs 100) — which is the case that produced runaway cost. Rev 1's
`max_steps × 10 = 1000` is removed.

## 2. Interface sketch

```cpp
// src/ui/ui_render.cpp (anonymous namespace)
constexpr int kComposerMaxRows = 8;
constexpr std::chrono::milliseconds kComposerCaretGrace{700};
Element render_input(const UiModel& model, const Theme& theme, int terminal_width,
                     int max_rows, bool hide_caret);
Element caret_anchor(Element element, ftxui::Screen::Cursor::Shape shape);

// src/agent/agent_loop.cpp (anonymous namespace)
constexpr std::size_t kNoProgressStreak = 3;   // consecutive error-only steps
bool step_made_progress(const ToolScheduleOutcome& outcome);

// include/ymh/agent/agent_loop.hpp
void appendTurnFailed(TurnId turn, AgentErrorCode code, std::string message,
                      bool recoverable = false);   // recoverable => Idle

// include/ymh/ui/ui_event.hpp
struct StepLimitReached { SessionId session; std::string message; };

// include/ymh/ui/ui_model.hpp
enum class ConversationRole : std::uint8_t { …, Notice };
std::optional<std::chrono::steady_clock::time_point> composer_input_at;
```

## 3. Invariants

| ID | Invariant |
|---|---|
| 61-I1 | The composer renders at most `min(kComposerMaxRows, max(1, inner_height - reserved))` rows, where `inner_height = size.height - 2` and `reserved` is the measured fixed chrome. The `max(1, …)` clamp means that when `inner_height - reserved <= 0` the composer still renders one row (best-effort, 61-F6); in that degenerate case the caret is **not** guaranteed on-screen. Whenever the clamp is not hit, the caret row is within the screen. |
| 61-I2 | When the composer needs the whole display the transcript row collapses to zero; it is never the composer that is clipped. |
| 61-I3 | A one-row draft renders identically to pre-61 (59-I5 preserved). |
| 61-I4 | The caret is `Cursor::Bar` unless the **focused** session is animating and no composer edit occurred within `kComposerCaretGrace`; then it is `Cursor::Hidden`. "Animating" = `is_active_state(focused.agent_state) \|\| focused has a streaming `Reasoning` entry`. A background session's animation does not affect the focused caret. |
| 61-I5 | A turn executes at most `max(1, max_steps)` steps before its terminal event. |
| 61-I6 | Reaching `max_steps` is terminal (no auto-continuation past it) but **recoverable**: the turn ends in `Idle`, not `Error`. |
| 61-I7 | The hard-ceiling terminal event is a durable `TurnFailed{StepLimitExceeded}` with a non-empty actionable message, and the **`AgentLoop` state is `Idle`** — set at the loop layer by `appendTurnFailed(..., recoverable=true)`, not merely projected by the adapter. |
| 61-I8 | `StepLimitExceeded` never renders as `error:` and never sets `ApiConnectivity::Error`. |
| 61-I9 | The no-progress guard stops the turn after `kNoProgressStreak` consecutive steps that commit no successful tool call; this includes an alternating A/B failing-call loop, which the consecutive-identical repeat reminder never trips. |
| 61-I10 | A `StepLimitReached` is conversation content: it raises `scroll.unseen` when the user has paged up, exactly like `ErrorOccurred` and the other `conversation_event`s. |

## 4. Failure modes

F# tags per `00-architecture.md` §54.

| ID | Failure | Mitigation |
|---|---|---|
| 61-F1 | (F6) The hidden caret also hides a genuinely idle composer caret. | The predicate is scoped to the focused session's animation and exempts a recent edit (61-D2/I4); `WaitingForPermission`/`WaitingForInput` are not active states. |
| 61-F2 | (F6) The composer height predicate diverges from the layout's actual reservation. | `build_ui` measures the same elements it later places (61-D1). |
| 61-F3 | (F8) Unbounded auto-continuation. | Removed: the ceiling is `max_steps` (61-D3/I5); the no-progress guard stops error-only loops at 3 (61-D6/I9). |
| 61-F4 | (F8) A step-budget reset re-triggers `force_first_tool_call`. | No segments exist in Rev 2; `turn_step`/`stepNumber` stay monotonic for the whole turn (61-D3). |
| 61-F5 | A `StepLimitReached` event is replayed as an error. | The adapter routes only `TurnFailed{StepLimitExceeded}` to it; `project_state` returns `Idle` for that code (61-D4/D5). |
| 61-F6 | A short terminal cannot fit even one composer row. | `max_rows` clamps to `>= 1`; degenerate terminals remain best-effort (61-I1). |
| 61-F7 | (F8) An alternating A/B failing-call loop never trips the repeat reminder and runs to the ceiling. | The no-progress guard stops it after 3 error-only steps (61-D6/I9); test 61-U7. |
| 61-F8 | A recoverable stop reuses the running→`Idle` edge, so `armOnEdge` arms the completion flash and `attention.completed` is set — the stop can read as a *completion*. | Accepted and documented (L4): the bold `Notice` and the status note are the authoritative signals; the flash is a brief visual. A distinct "stopped" edge is deferred. |

## 5. dsh mapping

| dsh behaviour | this spec | justification / anchor |
|---|---|---|
| Composer that never leaves the viewport | **mirrored** (61-D1) | Defect fix within 59-D1; the cap becomes the available height, not a constant. |
| Caret hidden while the agent runs | **mirrored, scoped** (61-D2) | dsh's input caret is not the terminal cursor during streaming; hiding avoids the per-frame repaint artefact. Scoped to the focused session and suspended for a recent edit so typing stays visible. |
| Long turn continues without user re-prompt | **not mirrored** | Deliberate scope decision with a citation: Rev 1's `max_steps × 10` continuation was rejected as a safety regression (Oracle H1). The recoverable stop (61-D4) plus the no-progress guard (61-D6) keep the pre-61 100-step bound and make a stop resumable by one user message; dsh's graduated reminders + steering (`26 G29`, `26-dsh-alignment.md:1244`) are not a bound and steering is unbuilt (`55-multi-agent-delegation-errata.md`). |
| Hard stop with an actionable, recoverable message | **mirrored** (61-D4/D5) | 06 §5.8's `TurnFailed{StepLimitExceeded}` is retained as the durable record; only its UX and state are corrected. |
| A new durable step-budget `EventType` | **not mirrored** | Deliberate scope decision: the existing `TurnFailed` already carries a code+message and is durable; a new `EventType` adds wire/schema surface for no semantic gain (55-A13 precedent, `55-multi-agent-delegation-errata.md:1517`). |

## 6. Test plan

| ID | Test | Asserts |
|---|---|---|
| 61-U1 | `UiRenderGolden.ComposerFillsScreenKeepsCaretVisible` | 61-I1/I2: on a 40×10 terminal a 500-char draft keeps `END_MARKER` visible and `cursor().y < dimy()`, shape `Bar`. |
| 61-U2 | `UiRenderGolden.CursorHiddenWhileTurnActive` | 61-I4: focused `AgentState::Thinking` with no recent edit ⇒ `cursor().shape == Hidden`. |
| 61-U3 | existing `LongComposer*`/`UI51_D2_ComposerTintAndBarGolden` | 61-I3: short/normal composer rendering unchanged. |
| 61-U4 | `UiRenderGolden.CursorBarWhileEditingDuringTurn` | 61-I4: focused turn active **and** a composer edit within the grace window ⇒ `Bar`. |
| 61-U5 | `AgentLoop.StepLimitStopsAtCeilingRecoverably` | 61-I5/I6/I7: `max_steps = 3` with 40 productive steps stops after exactly 3 `StepStarted`, one `TurnFailed{StepLimitExceeded}` naming resuming, and `agent.state() == Idle`. |
| 61-U6 | `UiEventAdapter.StepLimitExceededIsARecoverableNotice` | 61-I8: `StepLimitExceeded` yields `StepLimitReached`, a `Notice` row (no `error:`), `ApiConnectivity != Error`, `AgentState::Idle`. |
| 61-U7 | `AgentLoop.NoProgressGuardStopsAlternatingErrorLoop` | 61-I9: `max_steps = 100` with an A/B/A/B failing-call script stops after 3 `StepStarted`, one `TurnFailed` whose message names no progress, and `Idle`. |
| 61-U8 | `UiModel.StepLimitNoticeRaisesUnseenWhileScrolled` | 61-I10: a `StepLimitReached` applied while scrolled up raises `scroll.unseen` and appends a `Notice`. |

### Pre-fix evidence (honest)

Two classes of test:

- **Behavioural, pre-fix-compilable** (proven failing against `aed3c25e9` by
  reverting only the production sources):
  - `61-U5`: pre-fix `StepStarted == 30` (the `×10` ceiling) and
    `agent.state() == Error` — the exact H1/H2 defects.
  - `61-U7`: pre-fix `TurnFailed == 0` — the alternating loop ran to script
    exhaustion with no guard (H1).
  - `61-U4`'s scoping half (`UiRenderGolden.CursorBarWhenOnlyBackgroundSessionAnimates`):
    pre-fix `cursor().shape == Hidden` because `has_streaming_reasoning()` scanned
    every session (M2).
- **API-guard** (cannot compile pre-fix, so they prove the API was absent rather
  than a runtime failure): `61-U4`'s grace half (`composer_input_at`), `61-U6`
  (`ConversationRole::Notice`/`StepLimitReached`), and `61-U8` (`StepLimitReached`).
  Their runtime behaviour is only exercised post-fix; the behavioural proof of the
  step-limit fix is `61-U5`/`61-U7`.

Pre-fix `61-U1` also fails (`END_MARKER` absent, caret off-screen) and `61-U2`
fails (`cursor().shape == Bar`).

## 7. Supersedes / amendments

- **Extends 59-D1/59-I4**: the composer cap is `min(kComposerMaxRows, available)`,
  not a fixed `kComposerMaxRows`.
- **Amends 48-D5.1** (`48:372-388`, `:1110-1118`) / **59-D4**: the caret remains
  the single focus owner and the glyph/box tracking is unchanged; only its
  **shape** is conditional (61-D2). The idle case still asserts `Bar` in the
  §5.5/§13.2 caret goldens (`CaretCursorLandsAtInputPosition`,
  `CaretCursorHandlesCjkLeadingCell`, `UI51_D2_ComposerTintAndBarGolden`).
- **Amends 06** (all sites, with anchors):
  - §5.7 transition table (`06:367`): `Thinking` + step ceiling → `Idle` with
    recoverable `TurnFailed`; a new no-progress row.
  - §5.7 diagram (`06:353`): step ceiling/no-progress → `Idle`, not `Error`.
  - §5.6 pseudocode (`06:641-642`): `max_steps` **or** `noProgressStreak >=
    kNoProgressStreak` → `TurnFailed{StepLimitExceeded}` + `Idle`, no live `Error`.
  - §5.8 (`06:829-831`): the ceiling is `max_steps` (unmultiplied); recoverable
    stop; no-progress guard.
  - A-F4 (`06:1104`): detection `step >= max_steps` **OR** `noProgressStreak >=
    kNoProgressStreak`; behaviour flush + `TurnFailed` + `Idle` (recoverable).
  - Decision (g) (`06:1334-1335`): a recoverable loop-policy stop, not a failure.
- **Amends 10 §5.1** (`10:660`): the pinned `UiEvent` variant gains
  `StepLimitReached` (61-D5); the sketch is updated in place, not only "extended".
- **Amends 48-D6.2** (`48:497-516`): `Notice` → `FinalAnswer` in the
  role→`Presentation` table and classifier (61-D5).
- **Amends 45-D7** (`45:891-902`): `StepLimitReached` is not `ErrorOccurred` and
  leaves `ApiConnectivity` unchanged (61-D5).
- **No amendment to 59's wrap/glyph rules** (59-D3/59-I6) or to the read-only
  subagent composer path (58-E33).

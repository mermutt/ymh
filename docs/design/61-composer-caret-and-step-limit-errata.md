# 61 — Composer/Caret and Step-Limit Resilience Errata

Status: **draft** (authored under the task's explicit diagnose-and-fix directive;
pending independent verification; see `DESIGN_STATUS.md`).

This errata covers three user-reported defects on the TUI surface:

1. **Composer overflow (defect fix).** A draft long enough to need the whole
   display was clipped: `render_input` (`src/ui/ui_render.cpp`) capped its height
   only by `kComposerMaxRows = 8`, never by the height the layout actually had
   left. On a short terminal (or with chrome rows present) FTXUI's box shrinker
   (`build/_deps/ftxui-src/src/ftxui/dom/box_helper.cpp` `ComputeShrinkHard`)
   clipped the composer's bottom rows, so the caret row left the screen.
2. **Cursor flashing during the spinner (design change).** The repaint timer
   (`src/ui/supervisor.cpp:3713-3735`) posts `Event::Custom` every
   `kFrameInterval = 50 ms` while `animation_active_` is set. Each
   `ScreenInteractive::Draw` (`screen_interactive.cpp:906-1013`) emits
   `ResetCursorPosition`, rewrites the whole screen, then repositions the
   hardware cursor. A visible cursor therefore traverses the screen every frame.
   Spec `59-D4`/`48-D5.1` pin the caret to `Cursor::Bar` unconditionally.
3. **Step limit abandons the turn (design change).** `06-agent-loop.md` §5.8
   pins `maxSteps` → `TurnFailed{StepLimitExceeded}` → `AgentState::Error`. The
   UI adapts that to an `error:` transcript row (`ui_event_adapter.cpp:132-135`,
   `ui_model.cpp:1219-1227`), so a long but productive job looks like a crash
   and must be re-prompted.

## 1. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 61-D1 | `render_input` takes `max_rows` (the height left after the fixed chrome, already `<= kComposerMaxRows`) and uses it as its window budget. `build_ui` measures the header/breadcrumb/separators/scroll-hint/subagents/command-hints/status rows, reserves them, and passes the remainder. The transcript (the `flex` row) collapses first. | Extends 59-D1/59-I4; defect fix |
| 61-D2 | The composer caret's cursor shape is `Hidden` while a turn animates and `Bar` otherwise. The animate predicate mirrors the supervisor's `animation_active_`: `has_active_turn() \|\| has_streaming_reasoning() \|\| aggregate.flash.isFlashing()`. | Amends 48-D5.1 / 59-D4 |
| 61-D3 | `agent.max_steps` is the **per-segment** step budget. When a segment reaches it the loop starts a fresh segment (the durable `StepId` stays monotonic; `turn_step` passed to `buildRequest` stays monotonic, so `force_first_tool_call` is not re-triggered). A turn is bounded by `kMaxStepSegments = 10` segments, i.e. a hard ceiling of `max_steps * 10` steps. | Amends 06 §5.8 |
| 61-D4 | On the hard ceiling the turn stops with the existing durable `TurnFailed{StepLimitExceeded}` (no new `EventType`, per the 55-A13 precedent) whose message is actionable ("… task incomplete; send a message to continue"). The agent returns to `Idle`, not `Error`. | Amends 06 §5.8 / 06 A-F4 |
| 61-D5 | The adapter maps a `StepLimitExceeded` `TurnFailed` to a new frontend-only `StepLimitReached` `UiEvent` (never on the wire, U4) instead of `ErrorOccurred`; the model renders it as a `ConversationRole::Notice` row (bold, not the dim `System`/`error:` style) and records the message in the status note. | New; extends 10 §5.1 |

`kMaxStepSegments` is a compile-time safety multiple, not a config key: the
config surface stays `agent.max_steps` (default 100) so the default hard ceiling
is 1000 steps per turn.

## 2. Interface sketch

```cpp
// src/ui/ui_render.cpp (anonymous namespace)
constexpr int kComposerMaxRows = 8;
Element render_input(const UiModel& model, const Theme& theme, int terminal_width,
                     int max_rows, bool hide_caret);
Element caret_anchor(Element element, ftxui::Screen::Cursor::Shape shape);

// src/agent/agent_loop.cpp (anonymous namespace)
constexpr std::size_t kMaxStepSegments = 10;   // per-turn hard ceiling multiple

// include/ymh/ui/ui_event.hpp
struct StepLimitReached { SessionId session; std::string message; };

// include/ymh/ui/ui_model.hpp
enum class ConversationRole : std::uint8_t { …, Notice };
```

## 3. Invariants

| ID | Invariant |
|---|---|
| 61-I1 | The composer renders at most `min(kComposerMaxRows, inner_height - reserved)` rows, where `inner_height = size.height - 2` and `reserved` is the measured fixed chrome. The caret row is always within the screen. |
| 61-I2 | When the composer needs the whole display the transcript row collapses to zero; it is never the composer that is clipped. |
| 61-I3 | A one-row draft renders identically to pre-61 (59-I5 preserved). |
| 61-I4 | The caret is `Cursor::Bar` iff no animation is active; it is `Cursor::Hidden` while `has_active_turn() \|\| has_streaming_reasoning() \|\| aggregate.flash.isFlashing()`. |
| 61-I5 | A turn executes at most `max_steps * kMaxStepSegments` steps before its terminal event. |
| 61-I6 | A turn that reaches `max_steps` mid-work continues automatically; it is not terminal until the hard ceiling. |
| 61-I7 | The hard-ceiling terminal event is a durable `TurnFailed{StepLimitExceeded}` with a non-empty actionable message, and the agent state is `Idle`. |
| 61-I8 | `StepLimitExceeded` never renders as `error:` and never sets `ApiConnectivity::Error`. |

## 4. Failure modes

F# tags per `00-architecture.md` §54.

| ID | Failure | Mitigation |
|---|---|---|
| 61-F1 | (F6) The hidden caret also hides a genuinely idle composer caret. | The predicate is exactly `animation_active_`; `WaitingForPermission`/`WaitingForInput` are not active states (61-I4). |
| 61-F2 | (F6) The composer height predicate diverges from the layout's actual reservation. | `build_ui` measures the same elements it later places (61-D1). |
| 61-F3 | (F8) Unbounded auto-continuation. | `kMaxStepSegments` hard ceiling (61-I5). |
| 61-F4 | (F8) The per-segment reset re-triggers `force_first_tool_call`. | `turn_step` stays monotonic (61-D3). |
| 61-F5 | A `StepLimitReached` event is replayed as an error. | The adapter routes only `TurnFailed{StepLimitExceeded}` to it; `project_state` returns `Idle` (61-D4). |
| 61-F6 | A short terminal cannot fit even one composer row. | `max_rows` clamps to `>= 1`; degenerate terminals remain best-effort. |

## 5. dsh mapping

| dsh behaviour | this spec | justification / anchor |
|---|---|---|
| Composer that never leaves the viewport | **mirrored** (61-D1) | Defect fix within 59-D1; the cap becomes the available height, not a constant. |
| Caret hidden while the agent runs | **mirrored** (61-D2) | dsh's input caret is not the terminal cursor during streaming; hiding avoids the per-frame repaint artefact. |
| Long turn continues without user re-prompt | **mirrored** (61-D3) | dsh uses graduated repeat-tool reminders plus steering rather than a hard per-turn abandon (26 G29, `26-dsh-alignment.md:1244`). |
| Hard stop with an actionable, recoverable message | **mirrored** (61-D4/D5) | 06 §5.8's `TurnFailed{StepLimitExceeded}` is retained as the durable record; only its UX and state are corrected. |
| A new durable step-budget `EventType` | **not mirrored** | Deliberate scope decision: the existing `TurnFailed` already carries a code+message and is durable; a new `EventType` adds wire/schema surface for no semantic gain (55-A13 precedent, `55-multi-agent-delegation-errata.md:1517`). |

## 6. Test plan

| ID | Test | Asserts |
|---|---|---|
| 61-U1 | `UiRenderGolden.ComposerFillsScreenKeepsCaretVisible` | 61-I1/I2: on a 40×10 terminal a 500-char draft keeps `END_MARKER` visible and `cursor().y < dimy()`, shape `Bar`. |
| 61-U2 | `UiRenderGolden.CursorHiddenWhileTurnActive` | 61-I4: `AgentState::Thinking` ⇒ `cursor().shape == Hidden`. |
| 61-U3 | existing `LongComposer*`/`UI51_D2_ComposerTintAndBarGolden` | 61-I3: short/normal composer rendering unchanged. |
| 61-U4 | `AgentLoop.StepLimitContinuesTurn` (renamed from `StepLimitFailsTurn`) | 61-I6: `max_steps = 2` with a 3-step script ends in `TurnEnded`, no `TurnFailed`. |
| 61-U5 | `AgentLoop.StepLimitHardCeilingStopsRecoverably` | 61-I5/I7: `max_steps = 1` with >10 tool steps ends in `TurnFailed{StepLimitExceeded}` whose message names resuming. |
| 61-U6 | `UiModel`/adapter test | 61-I8: `StepLimitExceeded` yields `StepLimitReached`, `Notice` row, `Idle`. |

Pre-fix evidence: 61-U1 fails (`END_MARKER` absent, caret off-screen); 61-U2
fails (`cursor().shape == Bar`); 61-U4 fails (`TurnFailed`, `TurnEnded == 0`).

## 7. Supersedes / amendments

- **Extends 59-D1/59-I4**: the composer cap is `min(kComposerMaxRows, available)`,
  not a fixed `kComposerMaxRows`.
- **Amends 48-D5.1 / 59-D4**: the caret is the single focus owner, but its
  cursor shape is conditional (61-D2); when idle it remains `Bar`.
- **Amends 06 §5.8 / A-F4**: `maxSteps` is per-segment; the terminal
  `StepLimitExceeded` occurs only at the hard ceiling, with a recoverable state.
- **No amendment to 59's wrap/glyph rules** (59-D3/59-I6) or to the read-only
  subagent composer path (58-E33).

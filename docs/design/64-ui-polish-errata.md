# 64 — UI Polish Errata: Animation Pace, Status Slot, Composer Padding

Status: **implemented; pending independent gate** (see `DESIGN_STATUS.md`). This
errata is written design-first and implemented in the same change set as its
code, at the user's request; the owning specs 46 (verified) and 63 (implemented)
are unchanged in intent. It fixes three independent UI defects:

1. the bottom-activity comet raced during streaming bursts (64-D1),
2. the status line's text shifted right when the comet appeared (64-D2),
3. the composer lacked the requested blank line above and below (64-D3).

The user asked, verbatim:

> "now the bottom line text starts from the very left position 'build -
> deepseek-flash - ...' And when prompt is sent all that shifts right to give
> space to the animation drawing by a few chars. I want the text initially drawn
> a few chars to the right. So that when animation appears it won't shift the
> text."

> "I want the prompt to have an empty line above the current line and one below
> as on ./screen.txt"

`./screen.txt` is not present in the repository or on disk; the requirement is
taken from the words: **exactly one** blank line above and one below the
composer's draft.

## 1. Diagnosis

### 1.1 The comet raced (64-D1)

`SupervisorApp::drain()` (`src/ui/supervisor.cpp:2050-2059` pre-change) ran for
**every** posted `Event::Custom`, not only the 50 ms timer: `enqueue()` posts one
per event (`src/ui/supervisor.cpp:818-826`), and the timer thread posts one per
`kFrameInterval` (`src/ui/supervisor.cpp:3740-3749`). The spinner delta was
`duration_cast<milliseconds>(now - last_tick_)`, which **truncates**; two events
less than 1 ms apart produced `delta == 0`, which was then credited as a full
`kFrameInterval` (50 ms). During a streaming burst (~200 events/s) the spinner
accumulated up to ~10 s of animation time per real second and raced; when quiet
only the timer fired and the pace was correct — the reported "stable for a
second, then rushed". The earlier 63-D7 step-size change (120 ms → 100 ms) could
not fix it: the defect was the per-drain credit, not the step.

### 1.2 The status text shifted (64-D2)

`render_status` (`src/ui/ui_render.cpp`) pushed the comet's 4-cell frame plus a
trailing space into `left_cells` **only** while `has_active_turn()`; the fit math
counted neither that slot nor its `" · "` separator. With the indicator absent
the mode text began at the border; with it present the text was pushed 8 cells
right. The same under-count also let the active line overflow and clip the
right-aligned aggregate.

### 1.3 The composer lacked padding (64-D3)

`render_input` returned exactly the draft's wrapped rows inside the gutter/tint
block; the user wants one blank row above and one below the draft.

## 2. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 64-D1 | The animation frame is a pure function of the **exact elapsed wall-clock time** since the animation started: `frame = floor(elapsed / step)` with `step = 120 ms`. `ReasoningSpinnerState` accumulates exact `steady_clock::duration` deltas and keeps the sub-step remainder; a single tick gap longer than 250 ms credits at most the clamp (`kSpinnerMaxStall`). `UiModel::advance_spinner` / `advance_reasoning_spinner` take the current `steady_clock::time_point`; `UiEventAdapter::onTick` forwards it alongside the flash `delta`. Drain frequency is therefore irrelevant by construction. `SupervisorApp::drain()` no longer credits a non-positive delta with `kFrameInterval`. | Supersedes 63-D2's per-drain `delta` clock and RB-17's `advance_spinner(delta)`; 63-D1/D4/D6 and the 120 ms visual pace retained; the 250 ms clamp retained (the spinner half now lives in the model) |
| 64-D2 | The status line always reserves the comet's 5-cell slot (4-cell frame + 1 space), emitting 5 blank cells when `has_active_turn()` is false, so the mode/model text starts at a constant column. The slot and its `" · "` separator (8 cells total) are subtracted from the fit budget up front, so the right-aligned aggregate is never clipped. Visibility is otherwise unchanged: no glyph when idle (63-D3). | Amends 63-D3/63-D5 and 46-D9.5–D9.7; 25-D1 segment order retained |
| 64-D3 | `render_input` renders exactly one blank row above and one below the wrapped draft, inside the `LeftBar` gutter and `user_block_background` tint. The draft area keeps its `kComposerMaxRows = 8` cap; the two padding rows are reserved out of the on-screen height budget (`kComposerPaddingRows = 2`). The wrap, slide window and caret rules (59-D1–D7, 62-D1/D2) are unchanged. | Amends 59-D1/59-I4; extends 51-D2.3/59-I7 |

## 3. Interface sketch

```cpp
// include/ymh/ui/ui_model.hpp — 64-D1
struct ReasoningSpinnerState {
    std::uint32_t             frame = 0;
    std::chrono::steady_clock::duration elapsed{};
    std::optional<std::chrono::steady_clock::time_point> last_tick;
};
bool advance_spinner(std::chrono::steady_clock::time_point now);
bool advance_reasoning_spinner(std::chrono::steady_clock::time_point now);

// include/ymh/ui/ui_event_adapter.hpp — 64-D1
void onTick(std::chrono::steady_clock::time_point now, std::chrono::milliseconds delta);

// src/ui/ui_render.cpp — 64-D2/64-D3
constexpr int kActivitySlotWidth = 5;   // 4-cell frame + 1 space
constexpr int kComposerPaddingRows = 2; // one row above + one below
```

## 4. Invariants

- **64-I1** The frame advanced over a real interval `T` is `floor(T / 120 ms)`
  regardless of how many times `advance_spinner` is called inside `T`.
- **64-I2** A non-positive tick gap (the same `now`) never advances the frame.
- **64-I3** The status line's mode/model text starts at the same display column
  with and without an active turn; when idle the line contains no
  `kBottomActivityFrames` entry.
- **64-I4** The composer renders exactly one blank row above and one below the
  draft; the draft area is 1..8 rows.
- **64-I5** The transcript thinking indicator and the caret rules are unchanged.

## 5. Failure modes

- **64-F1** The comet races again: `UI64_D1_SpinnerPaceIsDrainFrequencyIndependent`
  fails (N sub-ms drains over T ms advance by more than `T/step`).
- **64-F2** The status text shifts: `UI64_D2_StatusTextColumnStable` fails.
- **64-F3** The composer padding is missing or doubled:
  `UI64_D3_ComposerHasOneBlankRowAboveAndBelow` fails.

## 6. Tests

- `UI64_D1_SpinnerPaceIsDrainFrequencyIndependent` (`tests/unit/errata46_ui_test.cpp`):
  500 drains at 100 ms advance 0; 500 more at 120 ms advance exactly 1; 1000
  sub-millisecond drains over 120 ms advance exactly 1, not 1000.
- `UI64_D2_StatusTextColumnStable`: idle and active status lines place `build`
  at the same display column; the idle line carries the reserved 5-cell slot and
  no comet glyph.
- `UI64_D3_ComposerHasOneBlankRowAboveAndBelow`
  (`tests/unit/ui_render_golden_test.cpp`): the rows immediately above and below
  the draft are blank (gutter bars only) and the rows two away are not.
- Amended: `UI46_D9_SpinnerAdvancesOnTick` and
  `UiRenderGolden.ReasoningSpinnerOnlyAdvancesWhileStreaming` (absolute-time
  API); `UiRenderGolden.ConversationSnapshot`, `StatusPlanModeGolden`,
  `StatusNarrowDegradationDropsTpsBeforeNoteAndNotice`,
  `LongComposerDraftCapsHeightAndKeepsTailVisible` (new geometry).

**Owning specs amended:** `46` §11 (46-D9), `59` (59-D1/59-I4),
`63` (63-D2/63-D3/63-D5).

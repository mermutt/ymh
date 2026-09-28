# 63 — Bottom Activity Indicator Errata: Pendulum-Comet Status Sweep

Status: **verified** — independent gate PASS (0 HIGH / 0 MEDIUM / 3 LOW; see
`DESIGN_STATUS.md`). Two later specs supersede/amend parts of this errata and
carry the supersession entries: `64-ui-polish-errata.md` 64-D1 replaces 63-D2's
per-drain clock with an exact-elapsed-time pace, and
`65-subagent-working-state-errata.md` 65-D1 replaces 63-D3/63-I1/63-I2/63-F2's
`has_active_turn()` predicate with `active_session_working()`. This
errata is written design-first but implemented in the same change set as its code
(the user requested a single atomic UI change; the owning spec 46 is already
`verified`). It adds 63-D1: the **bottom** status indicator becomes a pendulum
comet (a solid head with a trailing tail that sweeps one way, then the opposite),
while the transcript **thinking** indicator keeps its existing braille spinner.
It supersedes 46-D9.4's "the status line uses the identical glyph set as Thinking".

The user asked, verbatim:

> "in the image there is a different type of activity indicator below the prompt.
> It uses a few chars. In static it just a few dots or similar. But during
> thinking periodically there is line a moving solid character with a tail to one
> direction and later into opposite. I like that more for the bottom indicator.
> Leave the existing one for all those 'thinking' cases."

## 1. The two indicators (pinned before the change)

There are exactly two distinct activity indicators. They are **not** swapped.

| # | Indicator | Location | Today's glyph set |
|---|---|---|---|
| 1 | **Bottom status indicator** (the one this errata changes) | `render_status`, `src/ui/ui_render.cpp:977-983` — first element of `left_cells`, rendered only when `model.has_active_turn()`; the resulting status-bar prefix is `│<glyph>  · build` | `kReasoningSpinnerFrames` (`src/ui/ui_render.cpp:360-362`), the same braille set as #2 |
| 2 | **Thinking indicator** (unchanged) | `reasoning_sign` / `render_reasoning_entry`, `src/ui/ui_render.cpp:364-375` — the transcript's `"⠋ Thinking"` header | `kReasoningSpinnerFrames` |

Confirmation that #1 is the bottom bar: `render_status` is the last row of the UI
(`build_ui`, `src/ui/ui_render.cpp:2050-2051`, `rows.push_back(std::move(status))`),
and the element at `:977-983` is pushed into `left_cells` **before** the `mode`
segment, producing the status-line prefix the shipped tests assert
(`tests/unit/errata46_ui_test.cpp:355-364`,
`tests/unit/ui_render_golden_test.cpp:2157-2169`). Confirmation that #2 is the
transcript indicator: it is emitted by `render_reasoning_entry` inside the
conversation pane, not by `render_status`, and the shipped test asserts the
separate string `"⠸ Thinking"` (`tests/unit/errata46_ui_test.cpp:366-382`).

## 2. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 63-D1 | The bottom status indicator uses a **new** frame table `kBottomActivityFrames` — a pendulum comet: a solid head `●` with a trailing tail `•` sweeping left→right (tail to the left) then right→left (tail to the right). It is 4 display cells wide and has 8 frames. The transcript thinking indicator keeps `kReasoningSpinnerFrames`. | Supersedes 46-D9.4 (shared glyph set); 46-D9.7 retained (still a prefix, not a segment) |
| 63-D2 | The comet reuses the **existing** frame machinery: the `model.spinner.frame` clock advanced by `UiModel::advance_spinner` at the 120 ms step (`src/ui/ui_model.cpp:1774-1788`; `ReasoningSpinnerState`, `include/ymh/ui/ui_model.hpp:634-637`). No second timer, no new model state. The frame index is `model.spinner.frame % kBottomActivityFrames.size()`. | 46-D9.1/46-D9.2 retained; RB-17/RB-18 retained |
| 63-D3 | **Visibility is unchanged**: the comet renders only while `model.has_active_turn()` (the active session is `Thinking`/`CallingTool`), exactly as before. When idle (or waiting for permission/error) the indicator is **absent** — no static idle glyph is added. | 46-D9.5/46-D9.6 retained |
| 63-D4 | The transcript **thinking** indicator (`reasoning_sign`) is byte-for-byte unchanged: streaming reasoning renders `kReasoningSpinnerFrames`, non-streaming renders the static `•`. | 46-D9 / RB-17 unchanged |
| 63-D5 | The status prefix format is unchanged: `"<4-cell frame>" + " "`, then `append_segment("build"/"plan")` inserts `" · "`, yielding `│<frame>  · build`. Only the glyph run's content/width changes. | 46-D9.4 (F-30 prefix rule retained) |
| 63-D6 | The frame table is exactly: `{"●···", "•●··", "·•●·", "··•●", "···●", "··●•", "·●•·", "●•··"}`. Frames 1–3 move the head right (tail trails left); frame 4 is the rightmost turnaround; frames 5–7 move the head left (tail trails right); frame 0 closes the loop. | New |

### 63.2.1 Frame table (63-D6)

```
idx   frame   motion
 0    ●···    head col0 (left turnaround, no tail)
 1    •●··    → head col1, tail col0
 2    ·•●·    → head col2, tail col1
 3    ··•●    → head col3, tail col2
 4    ···●    rightmost turnaround
 5    ··●•    ← head col2, tail col3
 6    ·●•·    ← head col1, tail col2
 7    ●•··    ← head col0, tail col1
```

The tail direction flips with the sweep: while moving right the tail is to the
left of the head; while moving left it is to the right — "a tail to one direction
and later into opposite".

## 3. Supersession

| ID | Superseded | Superseded by |
|---|---|---|
| 63-S1 | `46-permissions-ui-errata.md` 46-D9.4: "the status line use[s] the identical glyph set [as Thinking]" and the shipped tests `UI46_D9_SpinnerSharesFrameTable`, `UI46_D9_SpinnerBeforeModeSegment`, `UI46_G6_StatusSpinner`. | **63-D1**: the bottom indicator has its own `kBottomActivityFrames`; the thinking indicator retains `kReasoningSpinnerFrames`. 46-D9.1–D9.3, D9.5–D9.7 are retained. |

## 4. Invariants

- **63-I1** The bottom indicator's glyph is a pure function of
  `model.spinner.frame` (modulo the table size) and `has_active_turn()`; no new
  state, timer, or I/O.
- **63-I2** With `has_active_turn()` false the status line contains no
  `kBottomActivityFrames` entry (idle is absent, matching pre-63 behavior).
- **63-I3** The transcript thinking indicator is independent of
  `kBottomActivityFrames`: a streaming reasoning entry renders a
  `kReasoningSpinnerFrames` glyph at the same `model.spinner.frame`.

## 5. Failure modes

- **63-F1** The two indicators drift back to one table: `UI63_D1_BottomAndThinkingFramesDiffer`
  fails (it asserts distinct glyphs at the same frame index).
- **63-F2** The comet advances while idle: `UI63_D1_BottomStaticWhenIdle` fails
  (no bottom frame in the status line, `advance_spinner` returns false).

## 6. Tests

- `UI63_D1_BottomAndThinkingFramesDiffer` (`tests/unit/errata46_ui_test.cpp`):
  at frame 3 the status line shows `kBottomActivityFrames[3]` while the
  transcript shows `kReasoningSpinnerFrames[3] + " Thinking"` — the two tables are
  distinct.
- `UI63_D1_BottomStaticWhenIdle`: an idle model renders no bottom frame and
  `advance_spinner` does not advance the clock.
- Amended: `UI46_D9_SpinnerOnWhileTurnActive`, `UI46_D9_SpinnerOffOnIdle`,
  `UI46_D9_SpinnerOffOnWaitingForPermission`, `UI46_D9_SpinnerBeforeModeSegment`,
  `UI46_D9_SpinnerOnlyActiveSession` (assert the new bottom table),
  `UI46_D9_SpinnerSharesFrameTable` → repurposed as the distinct-tables assertion,
  and golden `UI46_G6_StatusSpinner` (new exact prefix).

**Owning specs amended:** `46` §11 (46-D9.4, superseded by 63-D1); references
`10` §6 and `25` D1 (the segment order is unchanged).

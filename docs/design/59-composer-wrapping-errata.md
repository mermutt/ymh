# 59 — Composer Wrapping Errata: Multi-Row Draft, Bounded Growth, Caret Tracking

Status: **verified (Rev 2)** (design-first gate; see `DESIGN_STATUS.md`).
Independent gate (gate-59, 2026-09-28) against the shipped code on `main`:
**PASS — 0 HIGH / 0 MEDIUM / 0 LOW**. The gate's initial 4 LOW documentation
drifts are all closed in this revision: (L1) §2 now states the authoritative
`terminal_width - kComposerTextInset` budget (`kComposerTextInset = 6`,
`ui_render.hpp:60`, `ui_render.cpp:689`, caller `:2084`) and no longer derives
`size.width - 8`; (L2) 59-I2 is qualified to the emitted window (59-I4) with the
golden that pins the scrolled-off head; (L3) 59-U5 names the actual height test
`UiRenderGolden.LongComposerDraftCapsHeightAndKeepsTailVisible`; (L4) the stale
`file:line` citations and the §1 pre-fix tense are corrected, and the §2
`render_input` 3-arg sketch is marked superseded by 62-D1/64-D3. Rev 2 adds
59-D7: plain `ArrowUp`/`ArrowDown` first move the caret across the wrapped draft
rows and only fall through to history on the first/last visual row. This amends
45-D1.2, `10` §9.2, `48` §5 and `00` §20.26; the rendering contract (59-D1–D6) is
unchanged.

This errata fixes the user-visible defect *"when I typed a long prompt it did
not wrap to the next line but just disappeared from screen."* Pre-59, the
composer (`render_input`) was a single `ftxui::hbox` of `> ` + before + caret +
after. `ftxui::hbox` never wraps: its `ComputeRequirement` sums `min_x` and
`SetBox` shrinks/clips children to the box
(`build/_deps/ftxui-src/src/ftxui/dom/hbox.cpp:20-57`; `Text::Render` stops at
`box_.x_max`, `build/_deps/ftxui-src/src/ftxui/dom/text.cpp:59-70`). Once the
draft exceeded the composer width the tail was clipped off-screen. Shipped,
`render_input` is the multi-row wrap (`src/ui/ui_render.cpp:655-774`). This is a
**layout-contract change** (the composer became multi-row), so it is specified
here rather than fixed as a pure defect.

## 1. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 59-D1 | `render_input` wraps the draft across visual rows instead of one clipped `hbox`. The composer grows to at most `kComposerMaxRows = 8` rows; beyond that a sliding window keeps the caret row visible (internal scroll, no key). | New contract; extends 51-D2.3 |
| 59-D2 | Row layout: the first visual row carries the `> ` marker; every continuation row carries a 2-cell `  ` alignment prefix. The `with_left_bar` gutter is unchanged and spans every composer row. | Extends 51-D2.3 (gutter/tint now multi-row) |
| 59-D3 | Wrapping is glyph-aware and measured in display **cells** (`ftxui::string_width`), reusing `glyph_len` (`include/ymh/ui/ui_model.hpp:229`). A glyph wider than the remaining cells moves wholly to the next row; it is never split. | New; uses 48/51 caret machinery |
| 59-D4 | The caret remains the **single** focus owner (48-D5.1, `00` F6). `CaretAnchor` is placed on the wrapped row/column that holds the caret glyph, so `screen.cursor()` tracks it. | Preserves 48-D5.1 |
| 59-D5 | The 58-E33 read-only subagent path is unchanged: with a non-empty `subagent_path`, `render_input` returns the hint row and no draft/caret. | Preserves 58-E33 |
| 59-D6 | `render_input` takes the composer's available width (`size.width`) as a parameter; `build_ui` computes it. `render_input` is in the anonymous namespace (`src/ui/ui_render.cpp:655-774`; the namespace closes at `:1851`) with one caller (`build_ui`, `:2084`). | New signature; internal |
| 59-D7 | Plain `ArrowUp`/`ArrowDown` first move the caret **one visual row** within the wrapped draft whenever an adjacent visual row exists, preserving the display-cell column where it fits and clamping to the target row's end; on the first row (`Up`) / last row (`Down`) they fall through to the existing `history_up`/`history_down` (45-D1.2). The draft is unchanged by the caret move and history is untouched. `Shift+Arrow*` (60) and the command-list branch (45-D2) keep their precedence; no new key is added. The slide window (59-D1/62-D1) follows the new caret row. | New; amends 45-D1.2, `10` §9.2, `48` §5, `00` §20.26; cites 59-D1/D3/D4, 62-D1/D2, 48-D5.1 |

## 2. Interface sketch

```cpp
// src/ui/ui_render.cpp (anonymous namespace)
constexpr int kComposerMaxRows = 8;

// 59-D1/59-D3: wraps `draft` (reconstructed as before+at+after with the caret
// at glyph index `caret_glyph`) into visual rows of at most `text_width` cells,
// then emits at most kComposerMaxRows rows: a window that always contains the
// caret row. First row prefix `"> "`, continuation prefix `"  "`.
//
// The Rev-2 3-arg sketch `(model, theme, width)` is SUPERSEDED by 62-D1 and
// 64-D3; the shipped signature is
//   Element render_input(const UiModel& model, const Theme& theme,
//                        int terminal_width, int max_rows, bool hide_caret,
//                        bool pad);
// (`terminal_width` is `size.width`; `max_rows` = 62-D1 height budget,
// `hide_caret` = 62-D2, `pad` = 64-D3.)
```

The caller (`build_ui`, `ui_render.cpp:2084`) passes `size.width` as
`terminal_width`. The draft's cell budget is the authoritative
`terminal_width - kComposerTextInset` (`include/ymh/ui/ui_render.hpp:60`,
`kComposerTextInset = 6`; `ui_render.cpp:689`), clamped to `>= 1`: the border
(2), the `LeftBar` gutter (2, `ui_render.cpp:50-87`) and the `> `/`  ` row
prefix (2). The inset is applied **once** to `size.width` — there is no
separate `width = size.width - 2` step, so no reader can derive
`size.width - 8`.

```cpp
// include/ymh/ui/ui_render.hpp — 59-D7
// The single glyph-aware wrap (59-D3) is shared by `render_input` and the
// vertical-caret mover, so the row/column mapping cannot drift (59-F7).
// `text_width` is the draft's cell budget (`size.width - kComposerTextInset`,
// 59-D3/59-D6).
[[nodiscard]] bool composer_move_cursor_vertical(std::string_view draft,
                                                 std::size_t& cursor, int direction,
                                                 int text_width);
```

`direction < 0` is `ArrowUp`, `direction > 0` is `ArrowDown`. The function is
pure (no I/O, no model mutation beyond the `cursor` out-param) and total: it
returns `false` — leaving `cursor` untouched — exactly when the caret is already
on the first (`Up`) or last (`Down`) visual row, which is the signal for the
caller to fall through to history. The `cursor` is glyph-aligned (`glyph_floor`,
48-D5.1) before use.

## 3. Invariants

| ID | Invariant |
|---|---|
| 59-I1 | With `!model.subagent_path.empty()`, `render_input` renders the 58-E33 hint row and **no** draft or caret. |
| 59-I2 | A draft whose cell width exceeds the text width renders on multiple rows; **within the emitted window** (59-I4) no draft glyph is partially clipped or dropped, and the final draft glyph is visible. (Head rows may scroll off per 59-I4 — `UiRenderGolden.ComposerVerticalCaretKeepsRowVisibleWhenWindowSlides` asserts the head `HSTART` is not rendered.) |
| 59-I3 | The caret renders on the visual row holding the caret glyph: `screen.cursor().y` equals that row and `screen.cursor().x` equals the caret glyph's leading cell. Exactly one focus owner exists (`00` F6). |
| 59-I4 | The composer's draft area renders at most `kComposerMaxRows` rows; when wrapping exceeds it, the emitted window contains the caret row. (64-D3 later amends this: the composer block additionally renders the two padding rows, `kComposerPaddingRows = 2`.) |
| 59-I5 | For a draft that fits one row, the draft's rendered row content is identical to the pre-59 single-line `hbox` (pinned by `CaretCursorLandsAtInputPosition`, `CaretCursorHandlesCjkLeadingCell` and `ConversationSnapshot`; 64-D3 later adds the composer padding rows around it). |
| 59-I6 | All width math is in display cells (`ftxui::string_width`), never UTF-8 bytes. |
| 59-I7 | The `with_left_bar` gutter and `user_block_background` tint span every rendered composer row (51-D2.3). |
| 59-I8 | Plain `ArrowUp`/`ArrowDown` with the caret **not** on the first/last visual row move the caret to the adjacent visual row: the display-cell column is preserved where the target row has a glyph boundary at or before it and clamped to the target row's last boundary otherwise; the draft is unchanged and history is not touched. On the first (`Up`) / last (`Down`) visual row the key recalls history instead (45-D1.2). The command-list branch (45-D2) still wins when a list is active. |

## 4. Failure modes

F# tags per `00-architecture.md` §54.

| ID | Failure | Mitigation |
|---|---|---|
| 59-F1 | (F6) Wrapping introduces a second focus owner / cursor. | Exactly one `CaretAnchor` per render (59-D4). |
| 59-F2 | (F6) The caret glyph is lost or duplicated at a wrap boundary. | The glyph stream is partitioned exactly once; the caret index is mapped to one row/column (59-I3). |
| 59-F3 | (F8) Unbounded composer growth crowds out the transcript. | Hard cap `kComposerMaxRows` (59-D1/59-I4). |
| 59-F4 | Terminal narrower than the chrome → non-positive text width → loop or zero-size element. | Clamp text width to `>= 1`; always emit at least one row. |
| 59-F5 | A wide (CJK) glyph straddles the row boundary and overflows. | Whole-glyph wrap on cell width (59-D3/59-I6). |
| 59-F6 | (F11/58-E33) The read-only subagent hint leaks the draft/caret. | Early return before wrapping (59-D5/59-I1). |
| 59-F7 | (F6) Vertical caret motion desyncs from the rendered caret row (a second wrap mapping drifts from `render_input`'s). | One shared glyph wrap feeds both `render_input` and `composer_move_cursor_vertical` (59-D7); `screen.cursor()` remains the sole focus owner (59-D4). |
| 59-F8 | (F6) A caret move is mistaken for a history recall (or vice versa) and silently replaces the draft. | The caret move never edits the draft; `history_up`/`history_down` run only when `composer_move_cursor_vertical` returns false (59-I8). Tests 59-U7/59-U8/59-U9 pin both halves. |

## 5. dsh mapping

| dsh behaviour | this spec | justification / anchor |
|---|---|---|
| Multi-line composer that grows with the draft | **mirrored** (59-D1) | dsh's composer grows; here growth is capped at `kComposerMaxRows` for transcript space. |
| Composer internal scroll past the cap | **mirrored** (59-D1) | Sliding window keyed on the caret row; no new keys. |
| `ArrowUp`/`ArrowDown` navigate a multi-line draft's rows before history | **mirrored** (59-D7) | dsh's composer is multi-line, so the arrows belong to the caret while an adjacent row exists; history is the first/last-row fall-through (45-D1.2 amendment). |
| Word-boundary wrap | **not mirrored** | Deliberate scope decision: the composer wraps on cell boundaries like a shell prompt; a wrap-point policy change would be a separate decision (compare `src/ui/render/markdown_renderer.cpp:264` word-wrap for prose). |

## 6. Test plan

| ID | Test | Asserts |
|---|---|---|
| 59-U1 | `UiRenderGolden.LongComposerDraftWrapsAndStaysVisible` | 59-I2: both the head (`START`) and the tail (`END_MARKER`) of a >width draft are visible. |
| 59-U2 | `UiRenderGolden.LongComposerCaretSitsOnLastWrappedRow` | 59-I3: the caret row equals the row holding the tail glyph; caret shape `Bar`. |
| 59-U3 | existing `CaretCursorLandsAtInputPosition`, `CaretCursorHandlesCjkLeadingCell`, `ConversationSnapshot` | 59-I5: short-draft rendering unchanged. |
| 59-U4 | existing `UI58_G6_*` read-only composer test | 59-I1: subagent hint unchanged. |
| 59-U5 | `UiRenderGolden.LongComposerDraftCapsHeightAndKeepsTailVisible` | 59-I4: the composer's draft rows `<= kComposerMaxRows` (the 64-D3 padding adds the two `kComposerPaddingRows` rows). |
| 59-U6 | `UiRenderGolden.ComposerVerticalCaretMovesWithinWrappedRows` | 59-I8: `composer_move_cursor_vertical` moves across rows, preserves/clamps the column, and returns `false` on the first/last row. |
| 59-U7 | `SupervisorHarnessTest.UI59_D7_UpOnLaterRowMovesCaretNotHistory` | 59-I8: a plain Up on a later visual row moves the caret; draft/history untouched. |
| 59-U8 | `SupervisorHarnessTest.UI59_D7_UpOnFirstRowRecallsHistory` | 59-I8/45-D1.2: a plain Up on the first visual row recalls history. |
| 59-U9 | `SupervisorHarnessTest.UI59_D7_DownOnLastRowRecallsHistory` | 59-I8/45-D1.2: a plain Down on the last visual row recalls history-next. |
| 59-U10 | `UiRenderGolden.ComposerVerticalCaretKeepsRowVisibleWhenWindowSlides` | 59-D7/59-I4: after a caret move up from a draft taller than `kComposerMaxRows`, the window follows the caret and `screen.cursor()` stays on a visible composer row. |

Pre-fix evidence: 59-U1 and 59-U2 fail on the pre-59 code — the composer renders
`│ >STARTxxxx…│` with `END_MARKER` clipped (`marker_row == -1`).

Rev 2 pre-fix evidence: 59-U7/59-U8/59-U9 fail on the pre-59-D7 handler — a plain
`Up`/`Down` always recalls history, so a later-row `Up` replaces the draft with
the recalled prompt instead of moving the caret, and a first-row `Up` is
indistinguishable from the later-row case. 59-U6/59-U10 cannot be built pre-fix
(`composer_move_cursor_vertical` does not exist).

## 7. Supersedes / amendments

- **Extends 51-D2.3**: the composer gutter + `user_block_background` tint now
  apply to every wrapped row (59-I7), not a single row.
- **Preserves 48-D5.1** (`ui_render.cpp:89-123`): the caret remains the single
  focus owner (`00` F6); it is now placed on the wrapped row/column.
- **Preserves 58-E33** (`58-subagent-navigation-errata.md:1539-1542`): the
  read-only subagent composer hint and its dispatch guard
  (`src/ui/supervisor.cpp:3266-3275`) are unchanged.
- **Amends 45-D1.2 / 45-I2** (`45-ui-interaction-errata.md:219-233`, `:1656`):
  the inactive-list branch of plain `ArrowUp`/`ArrowDown` gains a caret step
  (`composer_move_cursor_vertical`, 59-D7) *before* history recall. History
  storage and the command-list precedence are unchanged; history remains the
  fall-through on the first/last visual row.
- **Amends `10-supervisor-tui.md` §9.2** (`:1059-1061`): the input-editor
  binding becomes `Up`/`Down` = caret rows, else history.
- **Amends `48-ui-and-config-errata.md` §5** (`:474`): the `ArrowUp`/`ArrowDown`
  row now cites "45-D1 (as amended by 59-D7)".
- **Amends `00-architecture.md` §20.26** (`:3513`): the keybinding table reads
  `Up/Down` = caret rows, else history.
- No existing invariant pins the composer to exactly one row; this spec
  introduces that layout contract and keeps one-row output for short drafts
  (59-I5). For a one-row draft 59-D7 is a no-op: both arrows fall straight
  through to history (59-I5 + 59-I8 agree).

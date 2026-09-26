# 59 — Composer Wrapping Errata: Multi-Row Draft, Bounded Growth, Caret Tracking

Status: **draft** (design-first gate; see `DESIGN_STATUS.md`).

This errata fixes the user-visible defect *"when I typed a long prompt it did
not wrap to the next line but just disappeared from screen."* The composer
(`render_input`, `src/ui/ui_render.cpp:525-565`) is a single `ftxui::hbox` of
`> ` + before + caret + after (`:554-563`). `ftxui::hbox` never wraps: its
`ComputeRequirement` sums `min_x` and `SetBox` shrinks/clips children to the box
(`build/_deps/ftxui-src/src/ftxui/dom/hbox.cpp:20-57`; `Text::Render` stops at
`box_.x_max`, `build/_deps/ftxui-src/src/ftxui/dom/text.cpp:59-70`). Once the
draft exceeds the composer width the tail is clipped off-screen. This is a
**layout-contract change** (the composer becomes multi-row), so it is specified
here rather than fixed as a pure defect.

## 1. Decisions

| ID | Decision | Amends / cites |
|---|---|---|
| 59-D1 | `render_input` wraps the draft across visual rows instead of one clipped `hbox`. The composer grows to at most `kComposerMaxRows = 8` rows; beyond that a sliding window keeps the caret row visible (internal scroll, no key). | New contract; extends 51-D2.3 |
| 59-D2 | Row layout: the first visual row carries the `> ` marker; every continuation row carries a 2-cell `  ` alignment prefix. The `with_left_bar` gutter is unchanged and spans every composer row. | Extends 51-D2.3 (gutter/tint now multi-row) |
| 59-D3 | Wrapping is glyph-aware and measured in display **cells** (`ftxui::string_width`), reusing `glyph_len` (`include/ymh/ui/ui_model.hpp:205`). A glyph wider than the remaining cells moves wholly to the next row; it is never split. | New; uses 48/51 caret machinery |
| 59-D4 | The caret remains the **single** focus owner (48-D5.1, `00` F6). `CaretAnchor` is placed on the wrapped row/column that holds the caret glyph, so `screen.cursor()` tracks it. | Preserves 48-D5.1 |
| 59-D5 | The 58-E33 read-only subagent path is unchanged: with a non-empty `subagent_path`, `render_input` returns the hint row and no draft/caret. | Preserves 58-E33 |
| 59-D6 | `render_input` takes the composer's available width (`size.width`) as a parameter; `build_ui` computes it. `render_input` is in the anonymous namespace (`src/ui/ui_render.cpp:28-1632`) with one caller (`:1742`). | New signature; internal |

## 2. Interface sketch

```cpp
// src/ui/ui_render.cpp (anonymous namespace)
constexpr int kComposerMaxRows = 8;

// Wraps `draft` (already reconstructed as before+at+after with the caret at
// glyph index `caret_glyph`) into visual rows of at most `text_width` cells,
// then emits at most kComposerMaxRows rows: a window that always contains the
// caret row. First row prefix `"> "`, continuation prefix `"  "`.
Element render_input(const UiModel& model, const Theme& theme, int width);
```

`width` is the composer's outer width (`size.width - 2` for the border). The
text width is `width - 4` (the `LeftBar` reserves two columns, `ui_render.cpp:55-66`)
`- 2` (the `> `/`  ` prefix), clamped to `>= 1`.

## 3. Invariants

| ID | Invariant |
|---|---|
| 59-I1 | With `!model.subagent_path.empty()`, `render_input` renders the 58-E33 hint row and **no** draft or caret. |
| 59-I2 | A draft whose cell width exceeds the text width renders on multiple rows; **no** draft glyph is dropped by clipping. The final draft glyph is visible. |
| 59-I3 | The caret renders on the visual row holding the caret glyph: `screen.cursor().y` equals that row and `screen.cursor().x` equals the caret glyph's leading cell. Exactly one focus owner exists (`00` F6). |
| 59-I4 | The composer renders at most `kComposerMaxRows` rows; when wrapping exceeds it, the emitted window contains the caret row. |
| 59-I5 | For a draft that fits one row, the rendered row is identical to the pre-59 single-line `hbox` (existing caret goldens and `ConversationSnapshot` unchanged). |
| 59-I6 | All width math is in display cells (`ftxui::string_width`), never UTF-8 bytes. |
| 59-I7 | The `with_left_bar` gutter and `user_block_background` tint span every rendered composer row (51-D2.3). |

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

## 5. dsh mapping

| dsh behaviour | this spec | justification / anchor |
|---|---|---|
| Multi-line composer that grows with the draft | **mirrored** (59-D1) | dsh's composer grows; here growth is capped at `kComposerMaxRows` for transcript space. |
| Composer internal scroll past the cap | **mirrored** (59-D1) | Sliding window keyed on the caret row; no new keys. |
| Word-boundary wrap | **not mirrored** | Deliberate scope decision: the composer wraps on cell boundaries like a shell prompt; a wrap-point policy change would be a separate decision (compare `markdown_renderer.cpp:264` word-wrap for prose). |

## 6. Test plan

| ID | Test | Asserts |
|---|---|---|
| 59-U1 | `UiRenderGolden.LongComposerDraftWrapsAndStaysVisible` | 59-I2: both the head (`START`) and the tail (`END_MARKER`) of a >width draft are visible. |
| 59-U2 | `UiRenderGolden.LongComposerCaretSitsOnLastWrappedRow` | 59-I3: the caret row equals the row holding the tail glyph; caret shape `Bar`. |
| 59-U3 | existing `CaretCursorLandsAtInputPosition`, `CaretCursorHandlesCjkLeadingCell`, `ConversationSnapshot` | 59-I5: short-draft rendering unchanged. |
| 59-U4 | existing `UI58_G6_*` read-only composer test | 59-I1: subagent hint unchanged. |
| 59-U5 | `UiRenderGolden.LongComposerDraftWrapsAndStaysVisible` (height) | 59-I4: composer rows `<= kComposerMaxRows`. |

Pre-fix evidence: 59-U1 and 59-U2 fail on HEAD — the composer renders
`│ >STARTxxxx…│` with `END_MARKER` clipped (`marker_row == -1`).

## 7. Supersedes / amendments

- **Extends 51-D2.3**: the composer gutter + `user_block_background` tint now
  apply to every wrapped row (59-I7), not a single row.
- **Preserves 48-D5.1** (`ui_render.cpp:89-123`): the caret remains the single
  focus owner (`00` F6); it is now placed on the wrapped row/column.
- **Preserves 58-E33** (`58-subagent-navigation-errata.md:1539-1542`): the
  read-only subagent composer hint and its dispatch guard
  (`src/ui/supervisor.cpp:3257-3265`) are unchanged.
- **No amendment to 45** input handling: 45-I20/45-D9 (composer Tab/agent
  cycling) operate on `InputModel` and are independent of composer height.
- No existing invariant pins the composer to exactly one row; this spec
  introduces that layout contract and keeps one-row output for short drafts
  (59-I5).

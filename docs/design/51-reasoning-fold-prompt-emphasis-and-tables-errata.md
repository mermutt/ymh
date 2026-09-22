# 51 — Inline Reasoning Folding, User-Prompt Emphasis, and Markdown Table Rendering Errata

```
Status: **draft (Rev 1)** — awaiting the independent design gate. This spec amends
        the owning specs (10, 17, 47, 48); it introduces no new component and no
        new subsystem. Like 45/46/48, every "current state" claim is reproducible
        from the shipped tree (HEAD `fa193bd26`), and every claim that could not
        be verified is marked an open question rather than asserted.

        **Origin.** Three items from `requirements_draft.txt` (lines 1–9):
        (A) Muse emits reasoning inline and it is not folded; (B) emphasise the
        user's prompt with a subtle background tint and a bluish left bar; and
        (C) Markdown tables are dropped. Item C's diagnosis is supplied by the
        user and is independently verified here.

        **Verification status: DRAFT — not yet reviewed.** No code may be written
        from this spec until an independent gate marks it `verified`
        (AGENTS.md, the rule).
```

## 1. Purpose, scope, and supersession map

### 1.1 In-scope requirements (verbatim, from `requirements_draft.txt`)

1. "why Muse emit `<thinks>` and some text after that and it is not folded. I
   think there was a requirement to fold such things with displaying ctrl+o to
   unfold?"
2. "Whould it be easy to highlight slightly background of prompt entered by user?
   On top of making vertical bar on far left of prompt text bluish (same color
   opencode uses)?"
3. "Markdown tables are not rendered in sessions. Instead of tables there are
   only multiple empty lines." — with the user's own diagnosis:
   "This is a silent failure, and it's arguably a regression caused by enabling
   the extension: without the 'table' extension the pipe-rows would have rendered
   as visible (if ugly) paragraph text. Enabling the extension promoted them into
   nodes the renderer then drops." — and two suggested fixes: the **proper** fix
   (a `CMARK_NODE_TABLE` case routing cells through
   `spans_to_element(inline_spans(cell), context)`) and the **cheap mitigation**
   (drop `"table"` from the extensions array).

### 1.2 What this changes, in one sentence

Inline reasoning tags emitted inside assistant **content** are detected by a
conservative, complete-pair allowlist and rendered as folded reasoning blocks
that reuse the existing `expand_all_folds` / Ctrl+O mechanism (D1); the user's
prompt and transcript message gain a theme-token background tint plus a bluish
left bar, applied to **both** the composer and the transcript (D2); and
`CMARK_NODE_TABLE` is rendered as a box-drawing grid whose cells route through
the existing inline-span renderer, with a documented raw-text fallback (D3).

### 1.3 Amendment / supersession map

| ID | Decision | Amends / extends |
|---|---|---|
| 51-A1 | Inline reasoning-tag folding (51-D1) | **Extends** 47-D5's detection-only principle to a second leaked-syntax family; **does not** change 47's interfaces, profiles or scope. Composes with 48-D8 (blank line after reasoning) and 48-D6 (presentation levels). |
| 51-A2 | Markdown table rendering (51-D3) | **Amends** 10 §8.2 (`docs/design/10-supervisor-tui.md:937-972`), the renderer surface, which is silent on tables; **adds detail to** `00-architecture.md` §22 (`:3284-3297`). No prior spec covers markdown tables. |
| 51-A3 | User-prompt emphasis (51-D2) | **Amends** 48-D6.1 (the user background literal becomes a theme token; the left bar is recoloured bluish) and **extends** 17 §3/RB-01 (`user_bar`, `user_block`) to the composer. |

**51-A1 precision note (do not misattribute).** Spec 47's D5 is *native-ATEM
tool-call detection*, not generic leaked-syntax detection: it recognises a
complete `<atem:invoke>…</atem:invoke>` in an assistant tool channel and surfaces
a typed error, and it is **detection-only** (`47:113-114`, `47:53-66`,
`47:791-…`). The `<thinks>` reasoning tag is a *different* family — benign
presentational leakage, not an executable call — so it gets a different remedy
(**fold, not typed error**) and a different layer (**UI projection, not the
provider boundary**). This spec does not render or convert ATEM and does not
touch the tool-call path; 47's §1.4 out-of-scope note stands unchanged.

### 1.4 Scope boundaries

- **In scope.** `src/ui/` (the projection + renderers), `include/ymh/ui/`
  (theme, model, render headers), and `src/ui/render/markdown_renderer.cpp`.
- **Out of scope.** The LLM provider boundary (`src/llm/`) is **not** modified:
  the inline tag never becomes a `ReasoningDelta` and never changes the durable
  event log. ATEM/`tool_calls` handling is untouched. Config wiring for a theme
  *variant* (light/dark) is recorded as an open question, not implemented here.
- **No new component.** D1 and D2 are pure functions/theme tokens in the
  existing UI; D3 is one new case in the existing `render_block` dispatcher.

### 1.5 Terminology (pinned)

- **Reasoning block.** A `ConversationRole::Reasoning` entry, rendered by
  `render_reasoning_entry` (`src/ui/ui_render.cpp:308-323`) as a `• Thinking`
  header plus `ctrl+o to expand`, body hidden until `expand_all_folds`.
- **Inline reasoning tag.** A complete, allowlisted `<tag>…</tag>` pair found in
  an assistant message's **content text** (not in `reasoning_content`).
- **Complete pair.** An allowlisted opening tag followed, in order and without
  an intervening unmatched closer, by its matching closing tag.
- **Degradation.** Falling back to rendering the table's pipe text as an
  ordinary paragraph, i.e. the visible (if ugly) pre-extension behaviour.

---

## 2. Amendment register

| ID | Decision | Owning spec / site |
|---|---|---|
| 51-D1 | Inline reasoning tags fold; conservative complete-pair allowlist; render-time segmentation reusing `render_reasoning_entry` + `expand_all_folds` | 47-D5 (extend, not alter), 48-D6/D8 |
| 51-D2 | Theme tokens `user_bar` (bluish), `user_block_background`, `true_color`, `variant`; applied to the composer and the transcript | 48-D6.1, 17 §3/RB-01 |
| 51-D3 | `CMARK_NODE_TABLE` grid rendering (box-drawing, wrap-not-truncate, alignment, raw-text fallback) | 10 §8.2, 00 §22 |

---

## 3. D1 — Inline reasoning tags fold

### 3.1 Current state (verified)

**The fold mechanism already exists and Ctrl+O already works.**

- `SessionUiState::expand_all_folds` defaults `false`
  (`include/ymh/ui/ui_model.hpp:281-282`).
- The **only** writer is `toggle_folds()` (`src/ui/supervisor.cpp:2483-2490`),
  called from the Ctrl+O handler (`src/ui/supervisor.cpp:3005-3008`).
- Readers are all in `src/ui/ui_render.cpp`: tool entries (`:245`), the
  reasoning header hint (`:316`), the reasoning body (`:318`), the Context entry
  (`:379`), and the pass-through into `render_entry` (`:407`).
- `render_reasoning_entry` (`src/ui/ui_render.cpp:308-323`) emits
  `• Thinking` + `ctrl+o to expand` and, only when expanded, the markdown body.

**Reasoning is classified by the wire field, never by inline tags.**

- `openai_adapter.cpp:458-487`: `delta.content` → `TextDelta` (`:459-464`);
  `delta.reasoning_content` / `delta.reasoning` → `ReasoningDelta` (`:467-477`),
  gated on `capabilities_.reasoning`.
- The Muse profile sets `capabilities.reasoning = true`
  (`src/llm/model_profile.cpp:27`), so a well-behaved `reasoning_content` field
  **would** fold for Muse.
- The UI adapter maps the chunk kind to a boolean
  (`src/ui/ui_event_adapter.cpp:93-96`); the model coalesces reasoning deltas
  into a `ConversationRole::Reasoning` entry (`src/ui/ui_model.cpp:58-92`) and
  appends content deltas to a `ConversationRole::Assistant` entry
  (`:844-859`).
- **There is no inline tag detection anywhere.** A tree-wide search for
  `<think`, `<thinks>`, `<thinking>`, `<reasoning>` and `<analysis>` in `src/`,
  `include/` and `tests/` finds nothing except the unrelated leaked **tool-call**
  detector (`src/llm/leaked_call_detector.{hpp,cpp}`; `47-D4`).

**Hypothesis (structurally confirmed):** Muse emits `<thinks>` inside
`delta.content`. Content is a `TextDelta`, so it lands in an ordinary
`Assistant` entry and renders as answer text; the renderer never classifies it
as reasoning, so it is never folded. The `<thinks>` text is visible because the
markdown renderer treats `<thinks>` as an inline HTML node and emits it dimmed
(`src/ui/render/markdown_renderer.cpp:115-118`).

### 3.2 Diagnosis — what the logs do and do not show (honest status)

The per-workspace session DBs on this machine are
`/home/yury/prjs/github/txtcoder/.ymh/sessions.db` and
`/home/yury/prjs/github/txtcoder/build/tests/.ymh/sessions.db` (no others under
`/home/yury`). The `events` table stores `type` + `payload` JSON
(`.schema`: `events(sequence, session_id, event_id, timestamp, type, payload)`).

- `SELECT COUNT(*) FROM events WHERE payload LIKE '%<think%'` → **0**; the same
  for `<thinks>`, `&lt;think`, `&lt;thinks`, `thinks`, `<analysis` → **0**.
- `SELECT DISTINCT model FROM sessions` includes `Muse-Glimmer-30B`, but the two
  Muse sessions (`40ed5e0c…`, `84fb2b85…`) contain **no assistant output**: each
  ends in `turn/fail` with `ProviderFailed` ("host resolution failed" /
  "invalid_request_error"). The third (`a625656d…`) has only `session/start`.
- `session_snapshots.messages_json LIKE '%think%'` → **0**.

**Conclusion: the actual Muse emission could not be verified from any log on
this machine.** The `reasoning_content`-vs-`content` question is therefore
**OQ-51-1** (below), not an assertion. The design is nonetheless safe under both
readings: if Muse used `reasoning_content`, D1 changes nothing (already folded);
if it emitted inline tags in `content`, D1 folds them. The design never folds
untagged prose, so a misdiagnosis cannot hide real answer text.

### 3.3 Decision (51-D1)

1. **Recognised forms — a conservative allowlist.** A reasoning span is a
   complete pair whose tag name is exactly one of:

   ```text
   <think>…</think>
   <thinks>…</thinks>
   <thinking>…</thinking>
   <reasoning>…</reasoning>
   <analysis>…</analysis>
   ```

   Tags are **lowercase, exact, and attribute-free**. An opening tag must be
   immediately preceded by start-of-text or whitespace and be exactly `<name>`;
   a closing tag must be exactly `</name>` and be immediately followed by
   end-of-text or whitespace. Unknown tags, mixed case, attributes, and
   self-closing forms are **not** recognised.
2. **Complete-pair rule.** A span folds **only** when a recognised opener is
   followed, in order, by its matching closer. Empty bodies fold (header only).
   Nested or overlapping tags, a closer with no opener, and a mismatched closer
   are **not** folded; the ambiguous text renders as ordinary markdown.
3. **Unterminated tag → literal text.** An opener with no closer stays visible as
   ordinary assistant markdown and is **not** folded. During streaming this is
   the transient state; when the closer arrives, the same accumulated text
   re-segments and the span folds. This is the single streaming rule: **fold on
   completeness, never on the opener alone.**
4. **Code contexts are excluded.** Tags inside fenced code blocks (` ``` ` /
   `~~~`) or inline code spans (`` `…` ``) are never folded. This prevents a
   documentation example from being hidden.
5. **Untagged prose is never folded** (explicit invariant `51-I3`).
6. **Reuse the existing mechanism; do not invent a second fold path.** Each
   folded span renders through the existing `render_reasoning_entry` with the
   session's `expand_all_folds`, so Ctrl+O, the `ctrl+o to expand` hint, the
   reasoning spinner, and the `• Thinking` header are unchanged. No new key, no
   new state flag, no new fold primitive.
7. **Segmentation is a render-time pure projection.** The split happens in the
   renderer over the accumulated assistant text; the **model and the durable
   event log are untouched**. The raw `assistant/chunk` / `assistant/message`
   payloads (tags included) remain byte-identical, preserving provenance and the
   "event log is the source of truth" invariant (D2). The tag delimiters are not
   shown in the UI; only the inner text is.
8. **Ordering.** Segments render in document order: prose, reasoning, prose, …,
   matching the order in the source text. (This intentionally differs from
   `reasoning_content`, whose reasoning is coalesced *before* the assistant
   entry by `17 §4/RB-02`; the inline form has no separate stream.)
9. **Blank line after inline reasoning (48-D8).** Each synthetic reasoning
   segment is followed by exactly one blank row, exactly as
   `render_conversation` does for a real `Reasoning` entry
   (`src/ui/ui_render.cpp:410-412`), so `48-I19` holds for inline reasoning too.
10. **Scope of application.** Only `ConversationRole::Assistant` entries are
    segmented. `User`, `Tool`, `System`, `Context` and already-`Reasoning`
    entries are never segmented.

**Rejected alternative (recorded).** Segmenting at the *model* layer into real
`ConversationRole::Reasoning` entries would make every entry-level mechanism
(blank line, spinner, presentation) apply with zero special-casing, but it
requires re-segmenting on every streaming delta with per-message entry-index
bookkeeping (insert/erase shifts `by_message` / `by_reasoning_message` for all
later entries) and adds mutable model state that must stay replay-stable. The
render-time split is chosen because it is pure (D16), stateless, needs no index
maintenance, and is recomputed from the same accumulated text the renderer
already reads each frame. The one consequence — synthetic reasoning does not
participate in `entry_presentation`'s forward scan — is bounded by D1.11.

11. **Presentation of synthetic segments.** A synthetic reasoning segment is
    rendered with `Intermediate` presentation (dim), matching a real
    `Reasoning` entry (`render_entry` Reasoning case,
    `src/ui/ui_render.cpp:360-363`); the surrounding prose keeps the entry's own
    `Presentation`.

### 3.4 Interfaces (pinned)

```cpp
// ── include/ymh/ui/ui_render.hpp ───────────────────────────────────────────
#include <string>
#include <string_view>
#include <vector>
namespace ymh::ui {

// 51-D1.7: one segment of an assistant message after inline-reasoning
// splitting. `reasoning == true` means the segment came from a complete
// allowlisted tag pair and must render as a folded reasoning block.
struct InlineReasoningSegment {
    bool        reasoning = false;
    std::string text;
};

// 51-D1.1–D1.5: pure, total, allocation-only. Splits `text` at COMPLETE
// allowlisted reasoning-tag pairs. Fenced code blocks and inline code spans are
// ignored. An unterminated opener, a stray closer, or nested/overlapping tags
// leave the affected text literal. Never throws.
[[nodiscard]] std::vector<InlineReasoningSegment>
segment_inline_reasoning(std::string_view text);

} // namespace ymh::ui
```

```cpp
// ── src/ui/ui_render.cpp (file-local, anonymous namespace) ─────────────────
// 51-D1.6/D1.9/D1.11: render an Assistant entry, splitting inline reasoning
// tags into folded blocks that reuse render_reasoning_entry and the session's
// expand_all_folds, and emitting the 48-D8 blank row after each synthetic
// reasoning segment.
ftxui::Element render_assistant_with_inline_reasoning(
    const ConversationEntry& entry, bool expand_all_folds,
    const RenderContext& context);
```

`render_entry`'s `ConversationRole::Assistant` case
(`src/ui/ui_render.cpp:357-359`) calls
`render_assistant_with_inline_reasoning` instead of a bare
`markdown.render(...)`. The synthetic segment is materialised as a
`ConversationEntry{ .role = ConversationRole::Reasoning, .text = segment.text,
.streaming = entry.streaming }` and handed to `render_reasoning_entry`; no other
entry is fabricated.

### 3.5 Invariants

| ID | Gate | Invariant |
|---|---|---|
| 51-I1 | Y | A complete allowlisted pair is rendered as a folded `Reasoning` block whose body is hidden while `expand_all_folds == false` and shown when it is true. |
| 51-I2 | Y | An unterminated opener is rendered as literal markdown text and is never hidden. |
| 51-I3 | Y | Text containing no complete allowlisted pair is rendered byte-identically to today's assistant rendering (untagged prose is never folded). |
| 51-I4 | Y | Tags inside fenced code blocks or inline code spans are never folded. |
| 51-I5 | Y | `segment_inline_reasoning` is pure, total, and never throws; `concat(segments) == text` after removing only the recognised tag delimiters. |
| 51-I6 | Y | The durable event log and the `UiModel` entries are unchanged by D1; no `reasoning_content` is synthesised and no `ReasoningDelta` is emitted. |
| 51-I7 | Y | Each synthetic reasoning segment is followed by exactly one blank row (48-I19 preserved). |
| 51-I8 | Y | Ctrl+O toggles all inline reasoning blocks in the active session together with real reasoning blocks (one `expand_all_folds`). |
| 51-I9 | pin | Rendering never mutates the model (D16 / 48-I9). |

### 3.6 Failure modes

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 51-F1 | F6 | Opener arrives, closer delayed across streaming deltas | Not folded until complete; literal text in the interim; folds on completion (51-D1.3). |
| 51-F2 | F6 | Mismatched closers (`<think>…</thinks>`) | No fold; literal text (51-D1.2). |
| 51-F3 | F6 | Tag-like text inside a code fence | Excluded by 51-D1.4; renders as code (51-I4). |
| 51-F4 | F7 | Background session's inline reasoning arrives while scrolled up | Same repaint as any other transcript change; `scroll.following` untouched. |
| 51-F5 | — | A tag pair spanning the assistant-message boundary | Not folded: segmentation is per entry; each entry is independent. |

---

## 4. D2 — User-prompt emphasis (background tint + bluish left bar)

### 4.1 Current state (verified)

- `Theme` (`include/ymh/ui/theme.hpp:7-25`) has exactly: `color` (`:8`),
  `user_block` (`:13`), `completion_selected` (`:18`), `user_foreground` = White
  (`:21`), `user_bar` = Green (`:22`), `tool_name` (`:23`), `tool_args` (`:24`).
  **There is no background token and no theme variant.**
- `paint` no-ops when `!color` (`src/ui/ui_render.cpp:33-38`); `paint_bg` no-ops
  when `!color || !user_block` (`:40-45`); `with_left_bar` draws `│` in
  `theme.user_bar` on every rendered line (`:47-87`; `color_enabled = theme.color`).
- **Transcript user message** (`render_entry` User case,
  `src/ui/ui_render.cpp:350-356`): `with_left_bar(markdown.render(...))` painted
  with the **hardcoded** `ftxui::Color::RGB(40, 42, 54)` (`:354`).
- **Composer** (`render_input`, `src/ui/ui_render.cpp:477-509`): `> ` is Green +
  bold (`:501`), the draft/caret/after cells are uncoloured (`:502-504`). There
  is **no background and no left bar** on the composer today.
- Theme construction: the sole site is
  `src/ui/supervisor.cpp:3041` `const Theme theme{terminal.capabilities().trueColor};`.
  Config has `ui.theme` (`include/ymh/config/config.hpp:48`;
  `src/config/config.cpp:237-238`) but it is **not wired** to rendering.
- Terminal capabilities: `trueColor` = `COLORTERM` `truecolor`/`24bit`;
  `color256` = `trueColor || TERM` contains `256color`
  (`src/ui/terminal_layer.cpp:86-93`).
- **Existing user emphasis (48-D6.1).** User entries are `Color::White` +
  `bold`, "left bar retained, background `RGB(40,42,54)` retained"
  (`docs/design/48-ui-and-config-errata.md:497-503`; 17 §3/RB-01,
  `docs/design/17-ui-transcript-errata.md:55-115`). D2 **amends** the two
  "retained" clauses (token + recolour) and leaves the foreground/bold rule
  untouched.

### 4.2 Decision (51-D2)

1. **Extend the theme rather than hardcode.** Add to `Theme`:
   - `ftxui::Color user_block_background` — replaces the literal
     `RGB(40,42,54)` at `src/ui/ui_render.cpp:354`. Default (dark)
     `RGB(40, 42, 54)` — the current value, retained as the dark tint.
   - `ftxui::Color user_bar` default **changes** from `Color::Green` to the
     opencode blue. Pinned default `RGB(92, 156, 245)` (`#5c9cf5`, opencode's
     dark secondary blue).
   - `bool true_color` — mirrors the terminal capability; gates the exact-RGB
     background tint (see 4.4).
   - `enum class ThemeVariant : std::uint8_t { Dark, Light }` plus a
     `ThemeVariant variant` field, and a factory
     `Theme make_theme(bool color, bool true_color, ThemeVariant variant)`.
     Light variant values are pinned (`user_bar = RGB(59, 125, 216)` (`#3b7dd8`),
     `user_block_background = RGB(238, 240, 244)`) but variant **selection** is
     an open question (OQ-51-2); the supervisor constructs `Dark` for now.
2. **Apply to BOTH scopes** (user decision):
   - **Transcript user message** — `with_left_bar(...)` (now bluish) +
     `paint_bg(theme.user_block_background)` (token replaces the literal).
   - **Composer** — wrap the `render_input` `hbox` in `with_left_bar(...)` and
     `paint_bg(theme.user_block_background)`, so the prompt line gets the same
     bluish gutter and tint. The `> ` marker keeps its Green + bold styling; the
     **new** bluish element is the left bar, so the marker and the gutter stay
     visually distinct.
   - **Justification for both scopes.** The user's words ("background of prompt
     entered by user" + "vertical bar on far left of prompt text") name the
     composer, but the transcript message is the same authored text rendered
     again; tinting only one would make the prompt visually jump between
     composing and sending. One rule, two render sites.
3. **Composition with 48-D6, not duplication.** `user_foreground = White` and
   `bold` are unchanged; the left bar remains unconditional (17 §3/RB-01); only
   the bar colour and the background source change. `user_block` keeps its exact
   meaning (background opt-out on a colour terminal); it now also suppresses the
   composer tint.
4. **Degradation.**
   - `color == false` (monochrome): no bar colour, no tint; the `│` glyph is
     still drawn (RB-01's unconditional bar), matching the transcript today.
   - `color == true && !true_color` (256-colour, no truecolor): the bar renders
     (FTXUI maps the RGB to the nearest palette entry); the **background tint is
     omitted** — an approximated tint is noisy at 256 colours and the user asked
     for a *subtle* highlight.
   - `user_block == false`: the tint is omitted on both scopes; the bar stays.
5. **No layout regression.** `LeftBar` reserves two columns and shifts the
   focused box by 2 (`src/ui/ui_render.cpp:55-66`); the composer's caret is a
   `CaretAnchor` inside that shifted box (`:96-119`). The caret must still land
   on the glyph's leading cell after the gutter shift.
6. **Theme tokens are the only colour source.** No new hardcoded colour is
   introduced; `paint`/`paint_bg` remain the single suppression seam.

### 4.3 Interfaces (pinned)

```cpp
// ── include/ymh/ui/theme.hpp ───────────────────────────────────────────────
#include <cstdint>
#include <ftxui/screen/color.hpp>
namespace ymh::ui {

enum class ThemeVariant : std::uint8_t { Dark, Light };

struct Theme {
    bool          color = true;
    bool          true_color = false;                 // 51-D2.1
    ThemeVariant  variant = ThemeVariant::Dark;       // 51-D2.1
    bool          user_block = true;
    ftxui::Color  completion_selected = ftxui::Color::CyanLight;
    ftxui::Color  user_foreground = ftxui::Color::White;
    ftxui::Color  user_bar = ftxui::Color::RGB(92, 156, 245);        // 51-D2.1
    ftxui::Color  user_block_background = ftxui::Color::RGB(40, 42, 54); // 51-D2.1
    ftxui::Color  tool_name = ftxui::Color::CyanLight;
    ftxui::Color  tool_args = ftxui::Color::GrayLight;
};

// 51-D2.1: variant-resolved construction. `Dark` reproduces the defaults above;
// `Light` substitutes user_bar = RGB(59,125,216) and
// user_block_background = RGB(238,240,244).
[[nodiscard]] Theme make_theme(bool color, bool true_color, ThemeVariant variant);

} // namespace ymh::ui
```

```cpp
// ── src/ui/ui_render.cpp ───────────────────────────────────────────────────
// 51-D2.2: the composer carries the same gutter + tint as the transcript.
Element render_input(const UiModel& model, const Theme& theme);
// 51-D2.4: paint_bg gains the true_color gate for the tint only.
Element paint_bg(Element element, ftxui::Color color, const Theme& theme);
```

### 4.4 Invariants

| ID | Gate | Invariant |
|---|---|---|
| 51-I10 | Y | The transcript user message and the composer both carry the left bar and the tint; neither hardcodes a colour. |
| 51-I11 | Y | `Theme::user_bar` default is the pinned opencode blue; no `Color::Green` remains as the user gutter. |
| 51-I12 | Y | `user_foreground == White` and `bold` for user text are unchanged (48-D6.1 not contradicted). |
| 51-I13 | Y | `color == false` ⇒ no foreground/background colour is emitted, but the `│` glyph is still drawn. |
| 51-I14 | Y | `color && !true_color` ⇒ the bar is drawn, the tint is omitted. |
| 51-I15 | Y | `user_block == false` ⇒ the tint is omitted in both scopes; the bar is retained. |
| 51-I16 | Y | The composer caret remains on the leading cell of the cursor glyph after the 2-column gutter shift. |
| 51-I17 | pin | `paint`/`paint_bg` remain the only colour-suppression seam; rendering never mutates the model. |

### 4.5 Failure modes

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 51-F6 | — | 256-colour terminal, no truecolor | Bar drawn from the palette; tint omitted (51-D2.4). |
| 51-F7 | F6 | Monochrome terminal | Glyph-only gutter, no colour (51-I13). |
| 51-F8 | — | `user_block == false` but the user wants the bar | Bar retained (51-I15). |
| 51-F9 | — | Narrow pane; the extra 2 composer columns clip the draft | The composer is inside the existing `content_width` box; the draft scrolls within it exactly as today; a golden test pins the caret column. |

---

## 5. D3 — Markdown table rendering

### 5.1 Current state (verified)

- **The `table` extension is enabled.** `parse_document` attaches
  `{"table", "strikethrough", "autolink", "tasklist"}`
  (`src/ui/render/markdown_renderer.cpp:304`), via
  `cmark_gfm_core_extensions_ensure_registered()` (`:299`) +
  `cmark_find_syntax_extension` + `cmark_parser_attach_syntax_extension`
  (`:305-310`).
- **The block dispatcher has no table case.** `render_block`
  (`src/ui/render/markdown_renderer.cpp:256-296`) handles `HEADING` (`:259`),
  `PARAGRAPH` (`:271`), `CODE_BLOCK` (`:274`), `BLOCK_QUOTE` (`:277`), `LIST`
  (`:285`), `THEMATIC_BREAK` (`:288`), `HTML_BLOCK` (`:291`) and otherwise falls
  through to `return render_children(node, context);` (`:295`). There is **no**
  `CMARK_NODE_TABLE`, `CMARK_NODE_TABLE_ROW` or `CMARK_NODE_TABLE_CELL` case.
- **Silent drop, confirmed.** `render_children` (`:205-218`) iterates children
  and returns `ftxui::text("")` for a childless node (`:211-213`). For a table:
  `render_block(table)` → default → `render_children(table)` → each
  `TABLE_ROW` → default → `render_children(row)` → each `TABLE_CELL` → default →
  `render_children(cell)` → the inline children → `render_block(text)` → default
  → `render_children(text)` → no children → `ftxui::text("")`. **Every cell
  becomes an empty element**, i.e. the "multiple empty lines" the user reported.
- **The regression claim is confirmed.** Without the `table` extension the pipe
  rows are not table nodes; they parse as `CMARK_NODE_PARAGRAPH` and render
  through `spans_to_element(inline_spans(node), context)` (`:271-273`), i.e.
  visible (ugly) text. Enabling the extension promotes them into unhandled nodes.
- **Reusable inline plumbing.** `inline_spans(cmark_node*)` (`:124-129`) and
  `spans_to_element(const std::vector<Span>&, const RenderContext&)`
  (`:167-201`) exist; `Span` is file-local (anonymous namespace, `:28-31`), so a
  table renderer must live in this `.cpp`. `collect_inline` (`:61-122`) already
  preserves bold/italic/underline/code/dim and inline HTML.
- **Width.** `RenderContext::content_width` (`include/ymh/ui/render/render_context.hpp:11-14`)
  is the pane's content-box width (`build_ui`: `max(1, width - 2 - 1)`).
- **Alignment is unused.** No `cmark_gfm_extensions_get_table_*` call exists in
  the tree (grep: none). The API is declared in
  `<cmark-gfm-core-extensions.h>` — **already included** at
  `markdown_renderer.cpp:10`: `cmark_gfm_extensions_get_table_columns`
  (`:17`), `cmark_gfm_extensions_get_table_alignments` (`:25`, returns
  `uint8_t*`), `cmark_gfm_extensions_get_table_row_is_header` (`:33`). The
  alignment bytes are ASCII `'l'`/`'c'`/`'r'`/`0` (cmark-gfm 0.29.0.gfm.13
  `extensions/table.c:378-393`, `:588-592`).
- **Tests.** `tests/unit/markdown_renderer_test.cpp` has 11 tests
  (`RendersHeading`, `RendersBoldAndItalic`, `RendersInlineCode`,
  `RendersFencedCodeBlock`, `RendersList`, `RendersOrderedList`,
  `RendersBlockQuote`, `RendersLinkWithUrl`, `RendersThematicBreak`,
  `StreamingPartialDoesNotThrow`, `BlockOverloadMatchesStringOverload`) and
  **zero table tests**. The helper `render_text(markdown)` (`:32-39`) renders at
  width 80 / content 80 with `Theme{false}` into an 80×40 screen and
  `strip_ansi`s the result.
- **No spec covers tables.** Spec 10 §8.2 (`docs/design/10-supervisor-tui.md:937-972`)
  names `MarkdownRenderer` but says nothing about tables; `00-architecture.md`
  §22 (`:3284-3297`) likewise. This is an unspecified gap (51-A2).

**Portability constraint (pinned, must not be glossed).** The table node-type
symbols `CMARK_NODE_TABLE` / `CMARK_NODE_TABLE_ROW` / `CMARK_NODE_TABLE_CELL`
are `extern cmark_node_type` **variables** declared in upstream
`extensions/table.h`, not in `cmark-gfm.h`. The Debian/Ubuntu
`libcmark-gfm-extensions-dev` package installs that header; **Arch's `cmark-gfm`
package does not** (it installs only `cmark-gfm.h`,
`cmark-gfm-core-extensions.h`, `cmark-gfm-extension_api.h`,
`cmark-gfm_export.h`, `cmark-gfm_version.h`). CMake already optionally links the
extensions module (`CMakeLists.txt:150-152`). The implementation must pin a
portable include/forward-declaration strategy; this is recorded as **OQ-51-3**
and a build failure mode (`51-F16`), not silently assumed.

### 5.2 Decision (51-D3)

1. **Proper fix, not mitigation.** Add a `CMARK_NODE_TABLE` case to
   `render_block` (before the `:295` fall-through) that lays the table out as a
   grid. Keep `"table"` in the extensions array.
2. **Cell routing (information-preserving).** Every cell is rendered as
   `spans_to_element(inline_spans(cell), context)` (`:124`, `:167`), so bold,
   italic, underline, inline code, links and dim survive exactly as in a
   paragraph. `CMARK_NODE_TABLE_ROW` / `CMARK_NODE_TABLE_CELL` are never passed
   to `render_block` individually; `render_table` walks them directly.
3. **Column-width policy (wrap, never truncate).**
   - Measure each cell's plain display width with `ftxui::string_width` over the
     concatenation of its `Span::text` (the same measure `span_element` uses).
   - `natural[j] = min(kTableCellMaxWidth, max over rows of cell width)` with
     `kTableCellMaxWidth = 32`.
   - `kTableMinColumnWidth = 8`.
   - If `Σ natural[j] + separators <= content_width`, use `natural` (each ≥ min).
   - Else shrink the currently widest column by one repeatedly until the table
     fits or every column is at `kTableMinColumnWidth`.
   - If even the minimum grid does not fit (`ncols * 8 + separators >
     content_width`), use the fallback in 5.2.5.
   - A cell whose content exceeds its column **wraps** inside the column (an
     FTXUI paragraph in a fixed-width box); it is **never truncated**.
     Justification: truncation discards information silently — the exact failure
     class this item fixes — whereas wrapping keeps every glyph.
4. **Alignment.** Read `cmark_gfm_extensions_get_table_alignments(table)`; for
   each column map `'l'`/`0` → left, `'c'` → center, `'r'` → right, and apply
   `ftxui::align_left` / `align_center` / `align_right` to the cell element
   within its fixed width. The header row is bold and uses the same per-column
   alignment.
5. **Overflow when the table is wider than the content box → raw pipe-text
   fallback.** If the minimum grid cannot fit, render the table as a wrapped
   paragraph of its pipe text (`| cell | cell |`, one row per line, via
   `spans_to_element` over the concatenated cell spans with literal `" | "`
   separators). Chosen over horizontal overflow because the conversation pane has
   no horizontal scroll (it is `yframe` + `vscroll_indicator`,
   `src/ui/ui_render.cpp:418-420`): an overflowing grid would silently clip the
   trailing columns — the same silent-loss failure the item is fixing — whereas
   the fallback is exactly the visible (if ugly) pre-extension behaviour the user
   already accepted as the mitigation.
6. **Border style — box-drawing.** Pinned glyphs: top rule `╭─┬─╮`, header
   separator `├─┼─┤`, bottom rule `╰─┴─╯`, column separator `│`; a space pads
   each cell. Justification: it matches the app's existing box-drawing chrome
   (the root frame uses `╭─…─╮`), it makes the grid unambiguous in monochrome
   (`Theme{false}`, the test theme), and it visually separates tabular data from
   surrounding prose. Cell contents are never rendered with their own borders.
7. **Header detection.** Use `cmark_gfm_extensions_get_table_row_is_header(row)`;
   the first row is normally the header. A table with no header flag renders
   without the header rule.
8. **Empty / ragged cells.** An empty cell renders as its column width of spaces;
   cmark-gfm autocompletes short rows (`extensions/table.c:174-181`, the
   `MAX_AUTOCOMPLETED_CELLS` guard) and any extra cells beyond `n_columns` are
   ignored. Rendering is best-effort and never throws; `MarkdownRenderer::render`
   already catches all exceptions and falls back to a plain paragraph
   (`src/ui/render/markdown_renderer.cpp:325-329`), which D3 preserves.
9. **Cheap mitigation (documented fallback, superseded).** Drop `"table"` from
   the extensions array (`:304`). Trade-off: tables then degrade to visible pipe
   text through the ordinary paragraph path (`:271-273`) — zero risk, but no
   grid, no alignment, no inline-aware layout, and ugly. It is the correct
   stopgap **only** if D3 is not implemented; once D3 lands, `"table"` stays
   enabled and the mitigation is retired. The two must not be enabled together.
10. **No new dependency beyond the existing cmark-gfm extensions library**, whose
    link is already conditional in CMake (`CMakeLists.txt:150-152`); the header
    portability strategy is OQ-51-3.

### 5.3 Interfaces (pinned)

```cpp
// ── src/ui/render/markdown_renderer.cpp (file-local; Span is anonymous) ────
#include <cstdint>
#include <vector>
namespace ymh::ui {
namespace {

// 51-D3.2: one table cell's inline content, already measured.
struct TableCell {
    ftxui::Element element;   // spans_to_element(inline_spans(cell), context)
    int            width = 0; // display width of the plain text
};

// 51-D3.3/D3.4: the resolved geometry for a table.
struct TableLayout {
    std::vector<int>    column_width;   // one entry per column, >= kTableMinColumnWidth
    std::vector<uint8_t> alignment;     // 'l' | 'c' | 'r' | 0, one per column
    bool                fits = false;   // false => use the 51-D3.5 fallback
};

[[nodiscard]] TableLayout compute_table_layout(cmark_node* table,
                                               int content_width);

// 51-D3.1/D3.2/D3.6: the box-drawing grid.
ftxui::Element render_table(cmark_node* table, const RenderContext& context);

// 51-D3.5: the raw pipe-text fallback when the grid cannot fit.
ftxui::Element render_table_as_text(cmark_node* table,
                                    const RenderContext& context);

} // namespace
} // namespace ymh::ui
```

`render_block` gains, before the `:295` fall-through:

```cpp
if (type == CMARK_NODE_TABLE) {
    return render_table(node, context);
}
```

Constants pinned: `kTableCellMaxWidth = 32`, `kTableMinColumnWidth = 8`,
separator `" │ "` (3 columns), padding 1 space each side.

### 5.4 Invariants

| ID | Gate | Invariant |
|---|---|---|
| 51-I18 | Y | A `CMARK_NODE_TABLE` renders its header and rows as a grid; no cell renders as an empty element. |
| 51-I19 | Y | Every cell routes through `spans_to_element(inline_spans(cell), context)`; inline bold/italic/code/link survive. |
| 51-I20 | Y | No cell content is truncated. Overflowing content wraps within its column, or the table degrades to pipe text. |
| 51-I21 | Y | A rendered grid's total width is ≤ `content_width`, or the pipe-text fallback is used. |
| 51-I22 | Y | Alignment is applied per column from `cmark_gfm_extensions_get_table_alignments`; a null/absent alignment array means left. |
| 51-I23 | Y | Empty cells and ragged/short rows render without a crash; the outer `catch (...)` fallback is preserved. |
| 51-I24 | Y | `"table"` remains in the extensions array; the cheap mitigation (5.2.9) is not active when D3 is implemented. |
| 51-I25 | pin | `MarkdownRenderer::render` remains pure and never throws (D16). |
| 51-I26 | Y | A table that does not parse as a table (no extension / malformed) still renders its pipe text visibly (the `PARAGRAPH` path). |

### 5.5 Failure modes

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 51-F10 | — | Table with zero columns | `compute_table_layout` yields `fits = false`; pipe-text fallback (51-D3.5). |
| 51-F11 | F8 | `content_width` smaller than one column | Pipe-text fallback (51-D3.5). |
| 51-F12 | — | `cmark_gfm_extensions_get_table_alignments` returns `nullptr` | All columns left (51-I22). |
| 51-F13 | — | Ragged row (fewer/more cells than the header) | cmark autocompletes/ignores; best-effort grid (51-D3.8). |
| 51-F14 | F5/F8 | Very large table (many rows) | Bounded by cmark's `MAX_AUTOCOMPLETED_CELLS`; the renderer adds no unbounded buffer. Renderer-level row cap is OQ-51-4. |
| 51-F15 | — | Table inside a block quote / list item | `render_block` recursion reaches `render_table`; the grid is nested in the parent element as today. |
| 51-F16 | — | Table node symbols unavailable at build (Arch header gap) | Build-time failure; resolved by OQ-51-3's pinned include strategy, never by silencing the compiler. |
| 51-F17 | F6 | Streaming a partial table | cmark parses what is complete; the last row may be incomplete; never throws (51-I23). |

---

## 6. Invariants (consolidated)

Numbered `51-I#`; contiguous with §3.5/§4.4/§5.4. Each is testable or an
explicit **pin** (retained behaviour, excluded from the gate). The "Gate" column
is `Y` or `pin`.

| ID | Gate | Invariant | Section |
|---|---|---|---|
| 51-I1–I9 | see §3.5 | Inline reasoning folding | D1 |
| 51-I10–I17 | see §4.4 | User-prompt emphasis | D2 |
| 51-I18–I26 | see §5.4 | Markdown tables | D3 |

No numbering gaps: `I1…I26` are defined exactly once, above.

---

## 7. Failure modes (consolidated)

Component-local tags `51-F#`, contiguous `F1…F17`; each maps to the shared
`F1–F12` finding where applicable (`00-architecture.md:4836-4856`).

| ID | Shared | Section |
|---|---|---|
| 51-F1–F5 | F6/F7 | D1 (§3.6) |
| 51-F6–F9 | F6 | D2 (§4.5) |
| 51-F10–F17 | F8/F5/F6 | D3 (§5.5) |

---

## 8. dsh (DeepSeek Harness) mapping

- **D1 — provider-leaked syntax is repaired at the presentation seam, not the
  provider seam.** dsh treats a model's wire format as the provider's
  responsibility and the message/tool model as provider-agnostic. ymh already
  follows this for leaked tool calls (47-D4) and native ATEM (47-D5). Reasoning
  tags are presentational leakage, so the repair is a **pure UI projection** that
  leaves the event log and the provider boundary untouched — the same
  separation, one layer later.
- **D2 — theme is presentation, never model.** The tint and bar live in `Theme`
  and the renderers, consistent with D13 (the UI renders a pure `UiModel`) and
  D14 (core never depends on FTXUI). No application state is added.
- **D3 — Markdown is a pure function of text.** Table rendering extends the
  existing `MarkdownRenderer` pure function; it adds no I/O, no state and no new
  dependency surface, consistent with D16 (rendering is side-effect free).

---

## 9. Test plan

### 9.1 Unit (hermetic, no LLM, no daemon)

| Test | Covers | Non-vacuity |
|---|---|---|
| `UI51_D1_SegmentsCompletePair` | 51-I1 | new |
| `UI51_D1_UnterminatedIsLiteral` | 51-I2, 51-F1 | new |
| `UI51_D1_UntaggedProseUnchanged` | 51-I3 | new (negative: proves no over-folding) |
| `UI51_D1_TagInCodeFenceIgnored` | 51-I4, 51-F3 | new |
| `UI51_D1_TagInInlineCodeIgnored` | 51-I4 | new |
| `UI51_D1_MismatchedCloserIsLiteral` | 51-F2 | new |
| `UI51_D1_SegmentsConcatenateToText` | 51-I5 | new |
| `UI51_D1_UnknownTagNotFolded` | 51-D1.1 | new |
| `UI51_D1_TagSplitAcrossDeltas` | 51-F1 | new (streaming) |
| `UI51_D2_ThemeDefaultsPinned` | 51-I11 | new (theme-token test: `user_bar`, `user_block_background`, light variant via `make_theme`) |
| `UI51_D2_PaintBgSuppressedByUserBlock` | 51-I15 | new |
| `UI51_D2_PaintBgSuppressedByMonochrome` | 51-I13 | new |
| `UI51_D2_TintOmittedWithoutTrueColor` | 51-I14 | new |
| `UI51_D3_SimpleTable` | 51-I18, 51-I19 | new |
| `UI51_D3_AlignmentVariants` | 51-I22 | new |
| `UI51_D3_InlineFormattingInCells` | 51-I19 | new |
| `UI51_D3_WiderThanPaneFallsBack` | 51-I20, 51-I21, 51-F11 | new |
| `UI51_D3_EmptyCell` | 51-I23 | new |
| `UI51_D3_RaggedTable` | 51-I23, 51-F13 | new |
| `UI51_D3_ZeroColumnsFallsBack` | 51-F10 | new |
| `UI51_D3_NullAlignmentsMeansLeft` | 51-I22, 51-F12 | new |

Table tests use the existing `render_text` helper style
(`tests/unit/markdown_renderer_test.cpp:32-39`): `Theme{false}`, 80×40 screen,
`strip_ansi`, substring assertions. `UI51_D1_*` tests call
`segment_inline_reasoning` directly (pure) **and** one golden that exercises the
render path.

### 9.2 Golden TUI render (`tests/unit/ui_render_golden_test.cpp`)

| Test | Covers |
|---|---|
| `UI51_D1_InlineReasoningCollapsedGolden` | A transcript whose assistant text contains a complete `<thinks>…</thinks>` pair shows `• Thinking` + `ctrl+o to expand`, and the inner text is **absent** (51-I1). |
| `UI51_D1_InlineReasoningExpandedGolden` | Same model with `state->expand_all_folds = true` shows the inner text and `expanded` (51-I1, 51-I8). Mirrors `ReasoningFoldSummaryAndExpandAll` (`:561-588`). |
| `UI51_D1_InlineReasoningBlankRowGolden` | Exactly one blank row after the inline reasoning block (51-I7). |
| `UI51_D1_UntaggedProseGolden` | A transcript with no tags is byte-identical to the pre-change rendering (51-I3). |
| `UI51_D2_UserMessageTintAndBarGolden` | The transcript user message carries the bluish bar + tint (51-I10, 51-I11). |
| `UI51_D2_ComposerTintAndBarGolden` | The composer row carries the bluish bar + tint, and the caret is on the leading glyph cell (51-I10, 51-I16). |
| `UI51_D2_MonochromeUserGolden` | `Theme{false}` keeps the `│` glyph and emits no colour (51-I13). |
| `UI51_D3_TableGolden` | A simple table's box-drawing grid, header rule and cell text (51-I18). |
| `UI51_D3_TableAlignmentGolden` | `:---:`, `---:` and `:---` columns render centred/right/left (51-I22). |
| `UI51_D3_TableFallbackGolden` | A table wider than the pane renders its pipe text (51-I20, 51-I21). |

### 9.3 Integration (FakeLLM)

- A scripted assistant turn whose content contains `<thinks>…</thinks>` followed
  by an answer produces an `Assistant` entry whose **raw** text still contains the
  tags, and the rendered transcript folds the span (51-I6). This is the
  provenance test: the log is unchanged, the view is folded.

### 9.4 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- A live turn against a model that leaks `<thinks>` (if one is available) shows
  the folded block and Ctrl+O expands it. Skipped by default. The Muse emission
  question (OQ-51-1) is what this test would settle.

### 9.5 Invariant / failure-mode coverage

Every `51-I#` is exercised by at least one test in §9.1–§9.3; every `51-F#` has a
negative or boundary test except `51-F4` (a scroll/repaint property already
covered by the existing scroll goldens) and `51-F16` (a build-time concern, not
a runtime test).

---

## 10. Open questions / interpretations

- **OQ-51-1 (what Muse actually emits).** No log on this machine contains a
  Muse assistant message (both Muse sessions failed at provider resolution;
  zero `<think`/`<thinks>` occurrences in any `events` payload). Whether Muse
  leaks `<thinks>` in `content` or uses `reasoning_content` is **unverified**. The
  design is correct under both readings and never folds untagged prose; the live
  PTY test (§9.4) settles it. **Recorded, not solved.**
- **OQ-51-2 (theme variant selection).** `ui.theme` exists in config
  (`include/ymh/config/config.hpp:48`) but is unwired; there is no light/dark
  detection. D2 pins both variants' tokens and constructs `Dark`. Choosing the
  variant (config key, terminal query, or `COLORFGBG`) is deferred. **Recorded,
  not solved.**
- **OQ-51-3 (cmark-gfm table-node header portability).** `CMARK_NODE_TABLE` /
  `_ROW` / `_CELL` are declared only in upstream `extensions/table.h`, which
  Arch's package omits while Debian ships it. The implementation must pin an
  include/forward-declaration strategy that builds on both without suppressing
  warnings. **Recorded; must be resolved before coding.**
- **OQ-51-4 (table row cap).** Whether the renderer should cap the number of
  rendered rows for very large tables is undecided; cmark's
  `MAX_AUTOCOMPLETED_CELLS` bounds autocompletion, not row count.
  **Recorded, not solved.**
- **OQ-51-5 (allowlist breadth).** The D1 allowlist is the union of the common
  reasoning-tag spellings. If the real Muse form is not in the set, add it (a
  one-line change) — but the set must stay small so that ordinary prose
  containing angle brackets is never folded. **Recorded.**

---

## 11. Revision log

- **Rev 1 (initial draft).** Covers the three `requirements_draft.txt` items.
  Decisions `51-D1 … 51-D3`. Current-state claims verified against the shipped
  tree (HEAD `fa193bd26`):
  `include/ymh/ui/ui_model.hpp:82-99`, `:281-282`;
  `include/ymh/ui/theme.hpp:7-25`;
  `include/ymh/ui/render/render_context.hpp:9-18`;
  `include/ymh/ui/render/markdown_renderer.hpp`;
  `include/ymh/llm/model_profile.hpp:29-40`;
  `src/ui/ui_render.cpp:33-45`, `:47-87`, `:96-119`, `:205-323`, `:343-421`,
  `:477-509`;
  `src/ui/ui_model.cpp:58-92`, `:820-866`;
  `src/ui/ui_event_adapter.cpp:84-105`;
  `src/ui/supervisor.cpp:2483-2490`, `:3005-3008`, `:3041`;
  `src/ui/terminal_layer.cpp:86-93`;
  `src/llm/openai_adapter.cpp:458-487`;
  `src/llm/model_profile.cpp:13-34`;
  `src/ui/render/markdown_renderer.cpp:61-129`, `:167-201`, `:205-337`;
  `tests/unit/markdown_renderer_test.cpp:1-118`;
  `tests/unit/ui_render_golden_test.cpp:37-70`, `:548-588`;
  `CMakeLists.txt:145-152`;
  `/usr/include/cmark-gfm-core-extensions.h:13-38`; cmark-gfm 0.29.0.gfm.13
  `extensions/table.c:378-393`, `:588-592`;
  `docs/design/47-muse-glimmer-support.md:53-66`, `:113-114`, `:791-…`;
  `docs/design/48-ui-and-config-errata.md:473-520`, `:747-788`;
  `docs/design/17-ui-transcript-errata.md:47-115`;
  `docs/design/10-supervisor-tui.md:906-972`;
  `docs/design/00-architecture.md:3284-3297`, `:4836-4856`.
  Session-DB scan: `.ymh/sessions.db` `events`/`session_snapshots` (zero
  `<think`/`<thinks>` matches; Muse sessions `40ed5e0c…`, `84fb2b85…` failed at
  provider resolution). **Verification status: DRAFT — not yet reviewed.**

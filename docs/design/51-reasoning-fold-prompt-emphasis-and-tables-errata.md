# 51 — User-Prompt Emphasis and Markdown Table Rendering Errata

```
Status: **verified (Rev 3)** — the independent design gate PASSED (0 HIGH / 0
        MEDIUM open). This spec amends
        the owning specs 10, 22 and 45 (no longer 47; the theme-token clause also
        refines 17 §3/RB-01 and 48-D6.1 by reference, and **supersedes 48-D6.5's
        green-bar pin** and **45-I25's Ctrl+D exit pin**); it introduces no new
        component and no new subsystem. Like 45/46/48, every "current state" claim
        is reproducible from the shipped tree (HEAD `e9a644a30`), and every claim
        that could not be verified is marked an open question rather than
        asserted.

        **Origin.** Two live items from `requirements_draft.txt` (lines 1–9):
        (B) emphasise the user's prompt with a subtle background tint and a
        bluish left bar; and (C) Markdown tables are dropped. Item C's diagnosis
        is supplied by the user and is independently verified here. A third item,
        (D) move exit to Ctrl+Q and repurpose Ctrl+D to delete in the switcher,
        was supplied by the user directly (not in `requirements_draft.txt`).
        Item (A) — folding inline reasoning tags in assistant content — was
        **WITHDRAWN by the user** (see §3); Rev 2 removes it entirely.

        **Verification status: VERIFIED (Rev 3).** Gate history: an Oracle gate
        and an adversarial critic both reviewed Rev 2 and returned **DO NOT
        APPROVE** (2 HIGH / 10 MEDIUM / 8 LOW) → Rev 3 fixes → independent
        re-check **PASS (0 HIGH / 0 MEDIUM)**. Implementation may proceed
        (AGENTS.md, the rule).
```

## 1. Purpose, scope, and supersession map

### 1.1 In-scope requirements (verbatim)

From `requirements_draft.txt`:

1. **[WITHDRAWN — not actioned, see §3]** "why Muse emit `<thinks>` and some text
   after that and it is not folded. I think there was a requirement to fold such
   things with displaying ctrl+o to unfold?"
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

Supplied directly by the user (2026-09-22; not in `requirements_draft.txt`):

4. "Change ctrl+d to ctrl+q for exit. And use ctrl+d in workspaces and sessions
   popup windows for deleting listed workspaces/sessions. I think twice hit
   ctrl+d should delete a highlighted item."

### 1.2 What this changes, in one sentence

The user's prompt gains a theme-token background tint plus a bluish left bar,
applied to **both** the composer and the transcript (D2); the `table` node is
rendered as a box-drawing grid whose cells route through the existing
inline-span renderer, with a documented raw-text fallback (D3); and exit moves
from Ctrl+D to Ctrl+Q while Ctrl+D in the Ctrl+S / `/sessions` overlay deletes
the highlighted session or workspace behind a double-press arm (D4).

### 1.3 Amendment / supersession map

| ID | Decision | Amends / extends |
|---|---|---|
| 51-A2 | Markdown table rendering (51-D3) | **Amends** 10 §8.2 (`docs/design/10-supervisor-tui.md:937-972`), the renderer surface, which is silent on tables; **adds detail to** `00-architecture.md` §22 (`:3284-3297`). No prior spec covers markdown tables. |
| 51-A3 | User-prompt emphasis (51-D2) | **Amends** 48-D6.1 (the user background literal becomes a theme token; the left bar is recoloured bluish) and **supersedes 48-D6.5's green-bar pin** (`docs/design/48-ui-and-config-errata.md:544-547`, the `Color::Green` bar) and its test-plan restatement (`:1027`, "user bold/white + green bar"); **extends** 17 §3/RB-01 (`user_bar`, `user_block`) to the composer. |
| 51-A4 | Exit rebind + switcher delete (51-D4) | **Amends** 45 §1.1's authoritative clarification (`docs/design/45-ui-interaction-errata.md:71-74`, "Ctrl+D stays EXIT"), 45-D8's keybinding table, and **supersedes 45-I25** (`:1666`, "Ctrl+D keeps the exit confirmation") — exit-with-confirmation moves to Ctrl+Q; **amends** 22 §3.6/§4.3 (the switcher overlay gains a delete action and a hierarchical delete target); **interacts with** 16 §7.6 (the last-supervisor exit confirmation that Ctrl+Q must still raise) and **supersedes 16's Ctrl+D thin-caller references** (`docs/design/16-daemon-ownership.md:688`, `:717`, `:801`, `:2104`) with Ctrl+Q. |
| 51-A5 | Session delete via the daemon `session.delete` RPC (51-D4.4) | **Uses** the shipped transport seam (`protocol::method::kSessionDelete`, `include/ymh/transport/protocol.hpp:523`; served at `src/transport/protocol_server.cpp:444`; implemented by `HostRuntime::deleteSession`, `src/host/host_runtime.cpp:805`; client seam `include/ymh/transport/host.hpp:102`). **Supersedes** the Rev-2 draft's direct `SessionManager::deleteSession` / `WorkspaceRegistry::removeSession` calls, which are impossible from the supervisor process. |

### 1.4 Scope boundaries

- **In scope.** `src/ui/` (the projection + renderers + the supervisor event
  router), `include/ymh/ui/` (theme, model, render headers),
  `src/ui/render/markdown_renderer.cpp`, and the already-shipped registry
  mutation primitives that D4's **workspace** delete calls
  (`WorkspaceRegistry::removeWorkspace` / `removeSession` — the supervisor holds a
  read-write registry handle, `src/cli/cli.cpp:510`). **Session** deletes are
  **not** performed in-process: they are issued as a `session.delete` JSON-RPC to
  the owning daemon (51-A5). D4 adds no new registry or session-manager method.
- **Out of scope.** The LLM provider boundary (`src/llm/`) is **not** modified:
  no reasoning classification changes, no `ReasoningDelta` synthesis, no change
  to the LLM event stream. (D4's session delete removes a session's `sessions.db`
  log through the **daemon's** shipped `HostRuntime::deleteSession`; that is a
  user-initiated delete, not an LLM-path change.) ATEM/`tool_calls` handling is
  untouched. Config wiring for a theme *variant* (light/dark) is recorded as an
  open question, not implemented here. Deleting a workspace does **not** touch the
  filesystem (§6.2.8). Remote/SSH transport is out of scope for the project.
- **No new component.** D2 is theme tokens in the existing UI; D3 is one new case
  in the existing `render_block` dispatcher; D4 is one keybinding rebind plus one
  new action in the existing switcher overlay.

### 1.5 Terminology (pinned)

- **Reasoning block.** A `ConversationRole::Reasoning` entry, rendered by
  `render_reasoning_entry` (`src/ui/ui_render.cpp:308-323`) as a `• Thinking`
  header plus `ctrl+o to expand`, body hidden until `expand_all_folds`. Retained
  here only as background for the withdrawn D1 (§3); no live decision uses it.
- **Degradation.** Falling back to rendering the table's pipe text as an
  ordinary paragraph, i.e. the visible (if ugly) pre-extension behaviour.
- **Switcher overlay.** The single overlay model `SwitcherOverlayModel`
  (`include/ymh/ui/ui_model.hpp:403`) backing **both** the Ctrl+S Live switcher
  (`SwitcherSource::Live`) and the `/sessions` list (`SwitcherSource::History`).
- **Delete target.** The node the switcher cursor highlights: the **session**
  when `SwitcherCursor::session` (`ui_model.hpp:396-401`) has a value, otherwise
  the **workspace**.
- **Delete arm.** The UI-only two-press confirmation state for Ctrl+D, reusing
  the Esc-Esc arm/disarm pattern of 48-D2 (`EscArm`, `kEscArmTimeout`). It lives
  on `SwitcherOverlayModel` (`delete_arm`/`delete_armed_at`) and is **bound to a
  specific target** captured at arm time (`delete_target`), so a cursor move
  cannot redirect a confirmed delete (51-I21). It resets on `open()`/
  `openHistory()`/`close()` and on the periodic tick.
- **Owning daemon.** The live `WorkspaceHost` daemon for the workspace that owns
  the delete target. Session deletes require it (51-A5); a workspace with no live
  daemon has no owning daemon.

---

## 2. Amendment register

| ID | Decision | Owning spec / site |
|---|---|---|
| 51-D1 | **WITHDRAWN** — no inline reasoning folding; Muse inline tags, if any, are the model's defect (§3) | 47-D5 (unchanged), 48-D6/D8 (unchanged) |
| 51-D2 | Theme tokens `user_bar` (bluish) and `user_block_background` (new, appended); applied to the composer and the transcript | 48-D6.1, **supersedes 48-D6.5** (green bar), 17 §3/RB-01 |
| 51-D3 | `table` node grid rendering (box-drawing, wrap-with-hard-break, alignment, synthesized raw-text fallback; layout from the width available at the nesting point) | 10 §8.2, 00 §22 |
| 51-D4 | Ctrl+Q exits (via `begin_exit(true)`); Ctrl+D in the switcher deletes the highlighted session/workspace behind a double-press arm; session delete is a `session.delete` RPC to the owning daemon | 45 §1.1/D8, **supersedes 45-I25**, **supersedes 16's Ctrl+D refs**, 22 §3.6/§4.3, 16 §7.6 |

---

## 3. D1 — WITHDRAWN (not actioned)

### 3.1 The user's decision (verbatim)

> "If the issue is with Muse model sending `<thinking>` within normal text, which
> is Muse's bug. Let's drop requirement to fold it. I do not want one more
> workaround specifically for Muse. Just fold when it correctly sends
> `<thinking>` tokens."

D1 is **withdrawn**: Rev 2 removes the inline-reasoning-fold requirement and all
of its interfaces, invariants, failure modes, tests and open questions. Nothing
in this spec builds an inline-tag fold path.

### 3.2 Why the premise was never verified (background; not actioned)

- **No Muse assistant message exists in any log on this machine.** The two Muse
  sessions in the per-workspace session DBs (`40ed5e0c…`, `84fb2b85…`) end in
  `turn/fail` with `ProviderFailed` (host resolution / `invalid_request_error`)
  and contain **no assistant output**; the third (`a625656d…`) has only
  `session/start`.
- **A tree-wide grep finds nothing.** A search for `<think`, `<thinks>`,
  `<thinking>`, `<reasoning>` and `<analysis>` over `src/`, `include/` and
  `tests/` matches **nothing** except the unrelated leaked **tool-call** detector
  (`src/llm/leaked_call_detector.{hpp,cpp}`, 47-D4). `SELECT COUNT(*) FROM events
  WHERE payload LIKE '%<think%'` → **0**; `session_snapshots.messages_json LIKE
  '%think%'` → **0**.
- So the "Muse emits `<thinks>` inside `content`" premise was a **hypothesis,
  never an observation**. The withdrawn D1 was designed to be safe under both
  readings, but no evidence shows it was ever needed.

### 3.3 The correct channel already folds (background; not actioned)

- `src/llm/model_profile.cpp:27` sets `profile.capabilities.reasoning = true` for
  the `muse-glimmer` profile (spec 47). `src/llm/openai_adapter.cpp:467-477`
  therefore maps `delta.reasoning_content` / `delta.reasoning` to
  `ReasoningDelta`, which the model coalesces into a
  `ConversationRole::Reasoning` entry and the renderer folds
  (`render_reasoning_entry`, `src/ui/ui_render.cpp:308-323`) behind
  `expand_all_folds` / Ctrl+O.
- **Consequence: when Muse sends its reasoning in the correct `reasoning_content`
  field, it is already folded — there is nothing to build.** D1 adds no code, no
  invariant and no test.

### 3.4 Honest consequence (accepted)

- If Muse instead emits inline `<thinking>`-style tags **inside** `content`, those
  characters render as ordinary assistant text (the markdown renderer treats
  `<thinks>` as an inline-HTML node and emits it dimmed,
  `src/ui/render/markdown_renderer.cpp:115-118`). Rev 2 accepts this as **the
  model's defect, not a harness gap** — the user explicitly declined a
  Muse-specific workaround.
- The durable event log and the provider boundary are untouched by D1 (there is
  no D1).
---

## 4. D2 — User-prompt emphasis (background tint + bluish left bar)

### 4.1 Current state (verified)

- `Theme` (`include/ymh/ui/theme.hpp:7-25`) has exactly: `color` (`:8`),
  `user_block` (`:13`), `completion_selected` (`:18`), `user_foreground` = White
  (`:21`), `user_bar` = Green (`:22`), `tool_name` (`:23`), `tool_args` (`:24`).
  **There is no background token and no light/dark variant today.**
- `paint` no-ops when `!color` (`src/ui/ui_render.cpp:33-38`); `paint_bg` no-ops
  when `!color || !user_block` (`:40-45`); `with_left_bar` (`:85`) builds a
  file-local `LeftBar` (`:50-83`) that draws `│` in `theme.user_bar` on every
  rendered line, with `color_enabled = theme.color` (`:86`).
- **Transcript user message** (`render_entry` User case,
  `src/ui/ui_render.cpp:350-356`): `with_left_bar(markdown.render(...))` painted
  with the **hardcoded** `ftxui::Color::RGB(40, 42, 54)` (`:354`).
- **Composer** (`render_input`, `src/ui/ui_render.cpp:477-509`): `> ` is Green +
  bold (`:501`), the draft/caret/after cells are uncoloured (`:502-504`). There
  is **no background and no left bar** on the composer today.
- Theme construction: the sole site is
  `src/ui/supervisor.cpp:3041` `const Theme theme{terminal.capabilities().trueColor};`.
  This aggregate-initializes the **first** field, `color`, from the terminal's
  truecolor capability — there is no separate `true_color` field to gate on.
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

1. **Extend the theme — the minimal change.**
   - Add `ftxui::Color user_block_background` (default `RGB(40, 42, 54)`, the
     current literal) and use it at `src/ui/ui_render.cpp:354` in place of the
     hardcoded colour.
   - Change `user_bar`'s default from `Color::Green` to the opencode blue.
     Pinned default `RGB(92, 156, 245)` (`#5c9cf5`, opencode's dark secondary
     blue).
   - **No `true_color` gate.** `color` (already `== trueColor` at the sole
     construction site, `src/ui/supervisor.cpp:3041`) is the **only** colour
     gate; there is no 256-colour-specific branch and this spec does **not**
     widen to `color256`.
2. **Field-order rule (pinned).** Any new `Theme` field must be **appended after
   the shipped fields** (or set via designated initializers), never inserted
   between existing fields. Reason: `tests/unit/ui_render_golden_test.cpp`
   contains 2-field aggregate initializations (`Theme{true, true}` /
   `Theme{true, false}`) that mean `{color, user_block}` and must keep that
   meaning. Inserting a field before `user_block` silently rebinds the second
   argument and breaks those tests.
3. **Apply to BOTH scopes** (user decision, preserved from Rev 1):
   - **Transcript user message** — `with_left_bar(...)` (now bluish) +
     `paint_bg(theme.user_block_background)` (token replaces the literal).
   - **Composer** — wrap the `render_input` `hbox` in `with_left_bar(...)` and
     `paint_bg(theme.user_block_background)`, so the prompt line gets the same
     bluish gutter and tint. The `> ` marker keeps its Green + bold styling; the
     **new** bluish element is the left bar, so the marker and the gutter stay
     visually distinct.
   - **Composer left bar — explicit decision and its consequence.** The composer
     **does** get the left bar. Consequences (accepted, pinned costs; 51-I6):
     - `LeftBar` reserves two columns and shifts the focused box by 2
       (`src/ui/ui_render.cpp:55-66`), so the composer caret's box moves by 2
       columns and the two caret goldens —
       `CaretCursorLandsAtInputPosition` (`tests/unit/ui_render_golden_test.cpp:2029`)
       and `CaretCursorHandlesCjkLeadingCell` (`:2044`) — change their expected
       column.
     - The composer row in the **`ConversationSnapshot`** golden
       (`tests/unit/ui_render_golden_test.cpp:200`, the `kGolden` literal at `:179`)
       changes from `│>` to `││ >`: outer border `│` + `LeftBar` `│` + its
       reserved gap column + `> `. `LeftBar` draws only the bar column and
       reserves two (`SetBox` shifts the child by 2, `src/ui/ui_render.cpp:55-66`),
       so a space sits between the bar and the child — exactly as the existing
       transcript row `││ hello there` (`:182`) shows. In the 72-wide,
       `Theme{false}` snapshot the row **must** become exactly
       `││ > ` followed by 66 spaces followed by `│` (72 display columns total);
       the `kGolden` literal must be updated in the same change. This is a
       **monochrome** golden, so the glyph change is independent of the recolour.
   - **`user_bar` recolour — explicit consequence.** Recolouring `user_bar` from
     `Color::Green` (`include/ymh/ui/theme.hpp:22`) to `RGB(92,156,245)` removes
     the green foreground escape from the transcript. The existing test
     `UserBrightAndIntermediateDimmedInTranscript`
     (`tests/unit/ui_render_golden_test.cpp:2090`) asserts
     `raw.rfind("\x1b[32m", user_text) != npos` (the 48-D6.5 green bar). That
     assertion is **wrong after D2** and must be replaced with the truecolor
     foreground escape for the new bar,
     `raw.rfind("\x1b[38;2;92;156;245m", user_text) != npos`; the
     `\x1b[1m` bold assertion and the dimmed-assistant assertion are unchanged.
     This is a mandatory edit in the same change (51-I2, 51-A3).
   - **Justification for both scopes.** The user's words ("background of prompt
     entered by user" + "vertical bar on far left of prompt text") name the
     prompt being authored; the transcript message is the same authored text
     rendered again, and tinting only one would make the prompt visually jump
     between composing and sending. One rule, two render sites.
4. **Composition with 48-D6, not duplication.** `user_foreground = White` and
   `bold` are unchanged; the left bar remains unconditional (17 §3/RB-01); only
   the bar colour and the background source change. `user_block` keeps its exact
   meaning (background opt-out on a colour terminal); it now also suppresses the
   composer tint.
5. **Degradation.**
   - `color == false` (monochrome): no bar colour, no tint; the `│` glyph is
     still drawn (RB-01's unconditional bar), matching the transcript today.
   - `user_block == false`: the tint is omitted on both scopes; the bar stays.
6. **No layout regression.** `LeftBar` reserves two columns and shifts the
   focused box by 2 (`src/ui/ui_render.cpp:55-66`); the composer's caret is a
   `CaretAnchor` inside that shifted box (`:96-119`). The caret must still land
   on the glyph's leading cell after the gutter shift (51-I6).
7. **Theme tokens are the only colour source.** No new hardcoded colour is
   introduced; `paint`/`paint_bg` remain the single suppression seam.
8. **Variant scaffold (pinned interface; selection deferred).** Add
   `enum class ThemeVariant : std::uint8_t { Dark, Light }` and a factory
   `Theme make_theme(bool color, ThemeVariant variant)`. Light values are pinned
   (`user_bar = RGB(59, 125, 216)` (`#3b7dd8`),
   `user_block_background = RGB(238, 240, 244)`) but variant **selection** is an
   open question (OQ-51-2); the supervisor constructs `Dark` for now. The
   variant field is appended last (field-order rule, D2.2).
   **Definition site (pinned).** `make_theme` is defined **`inline` in
   `include/ymh/ui/theme.hpp`** (the header that already declares `Theme`); it
   introduces **no new translation unit and no CMake change** — there is no
   `src/ui/theme.cpp` today and none is added. `Theme`/`ThemeVariant`/
   `make_theme` are consumed through the existing `ymh_ui` target
   (`CMakeLists.txt:710-720`); no target is added or relinked.

### 4.3 Interfaces (pinned)

```cpp
// ── include/ymh/ui/theme.hpp ───────────────────────────────────────────────
#include <cstdint>
#include <ftxui/screen/color.hpp>
namespace ymh::ui {

enum class ThemeVariant : std::uint8_t { Dark, Light };

struct Theme {
    // ---- shipped fields, unchanged in position ----
    bool          color = true;
    bool          user_block = true;
    ftxui::Color  completion_selected = ftxui::Color::CyanLight;
    ftxui::Color  user_foreground = ftxui::Color::White;
    ftxui::Color  user_bar = ftxui::Color::RGB(92, 156, 245);  // 51-D2.1 recolour
    ftxui::Color  tool_name = ftxui::Color::CyanLight;
    ftxui::Color  tool_args = ftxui::Color::GrayLight;
    // ---- 51-D2.2: new fields APPENDED after the shipped fields ----
    ftxui::Color  user_block_background = ftxui::Color::RGB(40, 42, 54);
    ThemeVariant  variant = ThemeVariant::Dark;               // 51-D2.8
};

// 51-D2.8: variant-resolved construction. `Dark` reproduces the defaults above;
// `Light` substitutes user_bar = RGB(59,125,216) and
// user_block_background = RGB(238,240,244). There is no `true_color` parameter:
// `color` is the sole colour gate (51-D2.1).
[[nodiscard]] Theme make_theme(bool color, ThemeVariant variant);

} // namespace ymh::ui
```

```cpp
// ── src/ui/ui_render.cpp — FILE-LOCAL helpers (anonymous namespace, §:28-1262).
// These are NOT part of the public pinned interface; they are implementation
// detail of the single public seam `render_to_ansi(model, size, theme)`
// (`include/ymh/ui/ui_render.hpp:45`). Listed here only to pin their signatures
// and suppression behaviour (51-I9, 51-L10).
// 51-D2.3: the composer carries the same gutter + tint as the transcript.
Element render_input(const UiModel& model, const Theme& theme);
// 51-D2.1: file-local (anonymous namespace). Its suppression behaviour is
// exercised only through render-level ANSI goldens, never a direct unit test
// (a file-local symbol cannot be linked from a test translation unit).
Element paint_bg(Element element, ftxui::Color color, const Theme& theme);
```

The **public** interface D2 pins is `Theme` + `ThemeVariant` + `make_theme`
(above) and the existing `render_to_ansi`; `render_input`/`paint_bg`/`paint`/
`with_left_bar`/`LeftBar`/`CaretAnchor` stay file-local.

### 4.4 Invariants

| ID | Gate | Invariant |
|---|---|---|
| 51-I1 | Y | The transcript user message and the composer both carry the left bar (theme token `user_bar`) and the tint (theme token `user_block_background`); those two colours are never hardcoded. **Scope note (51-M10):** the composer's pre-existing `> ` prompt marker keeps its hardcoded `Color::Green` + bold (D2.3) — that is an existing, out-of-D2-scope marker, not the bar/tint; I1 does not require tokenising it. |
| 51-I2 | Y | `Theme::user_bar` default is the pinned opencode blue (`RGB(92,156,245)`); no `Color::Green` remains as the user gutter. `UserBrightAndIntermediateDimmedInTranscript` (`tests/unit/ui_render_golden_test.cpp:2090`) is updated to assert `\x1b[38;2;92;156;245m` (not `\x1b[32m`) before the user text. |
| 51-I3 | Y | `user_foreground == White` and `bold` for user text are unchanged (48-D6.1 not contradicted). |
| 51-I4 | Y | `color == false` ⇒ no foreground/background colour is emitted, but the `│` glyph is still drawn. |
| 51-I5 | Y | `user_block == false` ⇒ the tint is omitted in both scopes; the bar is retained. |
| 51-I6 | Y | The composer caret remains on the leading cell of the cursor glyph after the 2-column gutter shift; the two caret goldens (`:2029`, `:2044`) **and** the `ConversationSnapshot` composer row (`:200`, `kGolden` at `:179`, `│>` → `││ >`) are updated in the same change. |
| 51-I7 | Y | Every new `Theme` field is appended after the shipped fields (or set via designated initializers); `Theme{true, true}` / `Theme{true, false}` keep their `{color, user_block}` meaning. |
| 51-I8 | pin | `paint`/`paint_bg` remain the only colour-suppression seam; rendering never mutates the model. |
| 51-I9 | pin | `paint_bg` stays file-local (anonymous namespace) and is tested through render-level ANSI goldens, not linked directly. |

### 4.5 Failure modes

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 51-F1 | F6 | Monochrome terminal | Glyph-only gutter, no colour (51-I4). |
| 51-F2 | — | `user_block == false` but the user wants the bar | Bar retained (51-I5). |
| 51-F3 | F8 | Narrow pane; the extra 2 composer columns clip the draft | The composer is inside the existing `content_width` box; the draft scrolls within it exactly as today; a golden test pins the caret column. |

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
  case for the table node type. Dispatch **must** use
  `cmark_node_get_type_string(node)`; it must **not** reference the
  `CMARK_NODE_TABLE` / `_ROW` / `_CELL` symbols nor include `table.h` (see the
  portability constraint below).
- **Node type strings (cmark-gfm 0.29.0.gfm.13).**
  `cmark_node_get_type_string` returns `"table"`, `"table_header"`,
  `"table_row"` and `"table_cell"`. **CRITICAL:** the header row's type string is
  `"table_header"`, **not** `"table_row"`, and header and body rows share the
  **same numeric type (`32781`)**. Filtering rows on `"table_row"` therefore
  **silently drops the header**. Iterate the table's children and classify each
  row with `cmark_gfm_extensions_get_table_row_is_header(row)`; handle the
  `is_header == 0` case defensively (render no header rule, still render the
  row).
- **Silent drop, confirmed.** `render_children` (`:205-218`) iterates children
  and returns `ftxui::text("")` for a childless node (`:211-213`). For a table:
  `render_block(table)` → default → `render_children(table)` → each row →
  default → `render_children(row)` → each cell → default → `render_children(cell)`
  → the inline children → `render_block(text)` → default → `render_children(text)`
  → no children → `ftxui::text("")`. **Every cell becomes an empty element**, i.e.
  the "multiple empty lines" the user reported.
- **The regression claim is confirmed.** Without the `table` extension the pipe
  rows are not table nodes; they parse as `CMARK_NODE_PARAGRAPH` and render
  through `spans_to_element(inline_spans(node), context)` (`:271-273`), i.e.
  visible (ugly) text. Enabling the extension promotes them into unhandled nodes.
- **Reusable inline plumbing.** `inline_spans(cmark_node*)` (`:124-129`) and
  `spans_to_element(const std::vector<Span>&, const RenderContext&)`
  (`:167-201`) exist; `Span` is file-local (anonymous namespace, `:28-31`), so a
  table renderer must live in this `.cpp`. `collect_inline` (`:61-122`) already
  preserves bold/italic/underline/code/dim and inline HTML.
- **Width.** `RenderContext::content_width`
  (`include/ymh/ui/render/render_context.hpp:11-14`) is the pane's content-box
  width, computed as `content_width = size.width - 3`
  (`src/ui/ui_render.cpp:1327`). This is small: **two or three columns already
  exceed a typical pane**, which is why the shrink loop (D3.4) is load-bearing.
- **Nesting reduces the available width (51-H5).** `content_width` is the width
  at the **top** of the markdown document. A table can be nested inside a
  prefix that consumes columns before the table starts:
  - a block quote — `hbox({text("> "), render_children(node, context)})`
    (`src/ui/render/markdown_renderer.cpp:277-279`) consumes **2** columns;
  - a list item — `hbox({text(marker), render_children(child, context)})`
    (`:235`) consumes the marker width (`"• "` = 2; `"N. "` = 3+);
  - the transcript **user block** — `render_entry` wraps the whole rendered
    markdown in `with_left_bar(...)` (`src/ui/ui_render.cpp:352-354`), whose
    `LeftBar` reserves **2** columns (`:55-66`).
  The nested `render_children`/`render_children(child, context)` calls pass the
  **same** `context`, so a table nested under a prefix is currently laid out for
  the full `content_width` and then silently clipped by the enclosing `hbox` /
  `LeftBar`. This is the same silent-loss class the item fixes, and D3.4 must
  compute the layout from the width **actually available at the table's nesting
  point** (51-I13).
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

**Portability constraint (pinned; replaces OQ-51-3).** The `CMARK_NODE_TABLE` /
`_ROW` / `_CELL` symbols are `extern cmark_node_type` **variables** declared only
in upstream `extensions/table.h`, which Arch's `cmark-gfm` package does **not**
install (it installs only `cmark-gfm.h`, `cmark-gfm-core-extensions.h`,
`cmark-gfm-extension_api.h`, `cmark-gfm_export.h`, `cmark-gfm_version.h`;
Debian/Ubuntu's `libcmark-gfm-extensions-dev` does ship `table.h`). D3 therefore
**does not reference those symbols and does not include `table.h`**; it
dispatches on `cmark_node_get_type_string(node)` and uses only the
`<cmark-gfm-core-extensions.h>` functions. `/usr/lib/pkgconfig/libcmark-gfm.pc`
already emits `-lcmark-gfm-extensions` (load-bearing; do not remove), and CMake's
conditional link (`CMakeLists.txt:150-152`) stays.

### 5.2 Decision (51-D3)

1. **Proper fix, not mitigation.** Add a table case to `render_block` (before the
   `:295` fall-through) dispatching on
   `cmark_node_get_type_string(node) == "table"`, laying the table out as a grid.
   Keep `"table"` in the extensions array.
2. **Dispatch and row classification.** Use `cmark_node_get_type_string` for
   `"table"`; iterate the table's children (rows) and classify each row with
   `cmark_gfm_extensions_get_table_row_is_header(row)` rather than by type string
   (header rows are `"table_header"`, body rows are `"table_row"`, both numeric
   type `32781`). The `is_header == 0` case is handled defensively.
3. **Cell routing (information-preserving).** Each cell's inline spans are
   `inline_spans(cell)` (`:124`); they are then pre-wrapped by `wrap_cell` (D3.5)
   and each wrapped line is rendered through the same inline-span path
   (`spans_to_element`-equivalent, `:167`), so bold, italic, underline, inline
   code, links and dim survive exactly as in a paragraph. Cells are never passed
   to `render_block` individually; `render_table` walks rows and cells directly.
4. **Column-width policy and the shrink loop (kept).**
   - **Available width at the nesting point (51-H5).** The layout budget is
     `available_width = max(1, context.content_width - context.indent)`, where
     `RenderContext` gains `int indent = 0` (the display columns consumed by
     enclosing prefixes). `render_block` increments `indent` when it descends:
     block quote `+2` (`"> "`), list item `+ string_width(marker)`; and
     `render_entry` passes `indent + 2` for the transcript user block's
     `with_left_bar` gutter. `render_table` therefore never lays out for more
     columns than are actually available where it sits, so the enclosing `hbox`/
     `LeftBar` cannot clip it. Non-table renderers ignore `indent` (their
     wrapping is unchanged). This is the "compute from the width actually
     available" branch of 51-H5; scoping tables to the top level is explicitly
     **rejected** because it would degrade every table inside a user prompt.
   - Measure each cell's plain display width with `ftxui::string_width` over the
     concatenation of its `Span::text` (the same measure `span_element` uses).
   - `natural[j] = clamp(max over rows of cell width, kTableMinColumnWidth,
     kTableCellMaxWidth)`, with `kTableCellMaxWidth = 32` and
     `kTableMinColumnWidth = 8`.
   - `total = Σ column_width[j] + 3·ncols + 1` (separator `" │ "` is 3 columns,
     plus the two outer border columns).
   - If `total <= available_width`, use `natural`.
   - Else **iteratively shrink the currently widest column by one** until
     `total <= available_width` or every column is at `kTableMinColumnWidth`. The
     min-8 floor is the loop's termination guard.
   - If even the minimum grid does not fit, use the fallback (D3.7).
   - **The shrink loop must be kept.** `available_width` is at most
     `content_width = size.width - 3` (`src/ui/ui_render.cpp:1327`) and is smaller
     under any prefix, so two or three columns already exceed the pane; cutting
     the loop makes the fallback the common case instead of the exception.
   - **Over-wide unbreakable tokens.** FTXUI `size(WIDTH, EQUAL, n)` cannot
     shrink below a word's minimum width, so a single token longer than its
     column would be **clipped**, not wrapped — the "never truncate" claim is
     false without this rule. The cell renderer **hard-breaks** such a token at
     the column boundary (a break every `column_width` display columns) using the
     same wrap helper as D3.5; if even that cannot fit (a wide glyph wider than
     the column), the table routes to the pipe-text fallback (D3.7). No content is
     silently discarded (51-I12).
5. **Variable row heights (pinned).** A wrapped cell makes its row taller than
   its siblings, and `TableLayout` has no row-height field. **`ceil(cell_width /
   column_width)` is NOT a safe upper bound** (51-M7): FTXUI's greedy word wrap
   can emit more lines than that ratio whenever a line ends with wasted slack
   (e.g. a 1-column word followed by a word that does not fit wastes
   `column_width - 1` columns). Pin instead that the cell content is
   **pre-wrapped once** by a single helper
   `std::vector<std::vector<Span>> wrap_cell(spans, column_width)` that greedily
   packs whole words and **hard-breaks** any token wider than `column_width`; the
   cell's element is built as a `vbox` of those exact lines (one `text`/`hbox`
   per line) and **`row_height = wrap_cell(...).size()`** (≥ 1). Because the
   measured height and the rendered content come from the *same* helper, the
   height is exact — no reliance on FTXUI's internal wrap, and no silent clip.
   Sibling cells in the row are padded to the row height (e.g.
   `vbox(cell, filler())`). `ftxui::gridbox` computes a per-row height but has
   **no width cap, no overflow fallback and emits no separator glyphs**, so it is
   not a drop-in.
6. **Alignment (span-safe; 51-M8).** Read
   `cmark_gfm_extensions_get_table_alignments(table)`; for each column map
   `'l'`/`0` → left, `'c'` → center, `'r'` → right. Apply the alignment **inside
   a `size(WIDTH, EQUAL, w)` box** by padding each pre-wrapped line with
   `ftxui::filler()` in an `hbox`: left = `hbox({line, filler()})`, right =
   `hbox({filler(), line})`, center = `hbox({filler(), line, filler()})`. This is
   the only span-safe choice: the `ftxui::paragraphAlign*` helpers take a
   **`std::string`**, so they cannot align a rich `Element` built from `Span`s
   (the Rev-2 sketch was unimplementable); `ftxui::align_right` + `ftxui::hcenter`
   are equivalent alternatives for a single line but the `hbox`/`filler` form is
   pinned. **`ftxui::align_left` and `ftxui::align_center` do not exist in FTXUI
   6.1.9** — `elements.hpp:185-188` offers only `align_right` plus
   `hcenter`/`vcenter`/`center`. The header row is bold and uses the same
   per-column alignment.
7. **Overflow when the table is wider than the available box → synthesized
   raw pipe-text fallback.** If the minimum grid cannot fit, render the table as
   a wrapped paragraph of **synthesized** pipe text (`| cell | cell |`, one row
   per line), built from the cell spans with literal `" | "` separators — cmark
   retains **no** source text for a table node, so the raw text cannot be
   recovered from the tree. `render_table_as_text` is therefore **kept**, not
   dropped. The fallback routes through the **same** `wrap_cell` helper
   (51-M7) with `available_width` as the width, so an over-wide unbreakable token
   in the fallback is **hard-broken** rather than clipped (51-L12); it does not
   hand a raw over-wide token to `ftxui::paragraph`, whose own wrapping would
   clip it. This path is meaningful because malformed or delimiter-less pipe text
   parses as a `PARAGRAPH` (the live path, D3.11). Chosen over horizontal overflow
   because the conversation pane has no horizontal scroll (it is `yframe` +
   `vscroll_indicator`, `src/ui/ui_render.cpp:418-420`): an overflowing grid would
   silently clip the trailing columns — the same silent-loss failure the item is
   fixing.
8. **Border style — box-drawing.** Pinned glyphs: top rule `╭─┬─╮`, header
   separator `├─┼─┤`, bottom rule `╰─┴─╯`, column separator `│`; a space pads
   each cell. Justification: it matches the app's existing box-drawing chrome
   (the root frame uses `╭─…─╮`), it makes the grid unambiguous in monochrome
   (`Theme{false}`, the test theme), and it visually separates tabular data from
   surrounding prose. Cell contents are never rendered with their own borders.
9. **Header rule.** A header row (per D3.2) is bold and separated by the header
   rule; a table whose rows all report `is_header == 0` renders without the
   header rule.
10. **Empty / ragged cells.** An empty cell renders as its column width of
    spaces; cmark-gfm autocompletes short rows (`extensions/table.c:174-181`, the
    `MAX_AUTOCOMPLETED_CELLS` guard) and any extra cells beyond `n_columns` are
    ignored. Rendering is best-effort and never throws; `MarkdownRenderer::render`
    already catches all exceptions and falls back to a plain paragraph
    (`src/ui/render/markdown_renderer.cpp:325-329`), which D3 preserves.
11. **Cheap mitigation (documented fallback, superseded).** Drop `"table"` from
    the extensions array (`:304`). Trade-off: tables then degrade to visible pipe
    text through the ordinary paragraph path (`:271-273`) — zero risk, but no
    grid, no alignment, no inline-aware layout, and ugly. It is the correct
    stopgap **only** if D3 is not implemented; once D3 lands, `"table"` stays
    enabled and the mitigation is retired. The two must not be enabled together.
12. **No new dependency beyond the existing cmark-gfm extensions library**, whose
    link is already conditional in CMake (`CMakeLists.txt:150-152`); the header
    portability strategy is pinned in §5.1, not deferred.

### 5.3 Interfaces (pinned)

```cpp
// ── include/ymh/ui/render/render_context.hpp ───────────────────────────────
struct RenderContext {
    // ... existing fields (width, content_width, theme, compact, spinner_frame)
    // 51-D3.4/51-H5: display columns consumed by enclosing prefixes (block quote
    // `"> "` = 2, list markers, the user-block LeftBar gutter = 2). Default 0.
    // Only `render_table` reads it; other renderers' wrapping is unchanged.
    int indent = 0;
};
```

```cpp
// ── src/ui/render/markdown_renderer.cpp (file-local; Span is anonymous) ────
#include <cstdint>
#include <string_view>
#include <vector>
namespace ymh::ui {
namespace {

// 51-D3.5/51-M7: pre-wrap one cell's spans to `column_width` display columns.
// Greedy word packing; hard-breaks any token wider than the column. The SAME
// helper measures and renders, so height == lines.size() exactly (no reliance
// on FTXUI's internal wrap). Each inner vector is one display line of spans.
[[nodiscard]] std::vector<std::vector<Span>> wrap_cell(const std::vector<Span>& spans,
                                                       int column_width);

// 51-D3.3: one table cell, already measured AND pre-wrapped.
struct TableCell {
    std::vector<std::vector<Span>> lines;   // wrap_cell(inline_spans(cell), col_width)
    int            width = 0;               // natural display width of the plain text
    // row_height is `lines.size()` (>= 1); it is NOT ceil(width/col_width) (51-M7).
};

// 51-D3.4/D3.5/D3.6: the resolved geometry for a table.
struct TableLayout {
    std::vector<int>     column_width;  // one entry per column, >= kTableMinColumnWidth
    std::vector<uint8_t> alignment;     // 'l' | 'c' | 'r' | 0, one per column
    std::vector<int>     row_height;    // 51-D3.5: lines.size() per row, >= 1
    bool                 fits = false;  // false => use the 51-D3.7 fallback
};

// 51-D3.4/51-H5: `available_width` is the width ACTUALLY available at the
// table's nesting point (== max(1, context.content_width - context.indent)),
// never the bare pane content width.
[[nodiscard]] TableLayout compute_table_layout(cmark_node* table,
                                               int available_width);

// 51-D3.1/D3.2/D3.3/D3.8: the box-drawing grid. Reads context.indent for width.
ftxui::Element render_table(cmark_node* table, const RenderContext& context);

// 51-D3.7: the SYNTHESIZED raw pipe-text fallback when the grid cannot fit.
// cmark retains no source text for a table, so the text is built from the cell
// spans. KEPT (not dropped): malformed/delimiter-less pipe text parses as a
// PARAGRAPH, which is the live path that makes the fallback meaningful.
ftxui::Element render_table_as_text(cmark_node* table,
                                    const RenderContext& context);

} // namespace
} // namespace ymh::ui
```

`render_block` gains, before the `:295` fall-through:

```cpp
if (std::string_view{cmark_node_get_type_string(node)} == "table") {
    return render_table(node, context);
}
```

and the three nesting sites thread `indent` (51-H5): the `BLOCK_QUOTE` branch
recurse with `RenderContext{.indent = context.indent + 2, ...}`; `render_list`
recurses each item with `indent + ftxui::string_width(marker)`; `render_entry`'s
user case passes `indent + 2` to `markdown.render` for the `with_left_bar`
gutter.

Constants pinned: `kTableCellMaxWidth = 32`, `kTableMinColumnWidth = 8`,
separator `" │ "` (3 columns), padding 1 space each side.

### 5.4 Invariants

| ID | Gate | Invariant |
|---|---|---|
| 51-I10 | Y | A table node renders its header and rows as a grid; no cell renders as an empty element. The header row is detected with `cmark_gfm_extensions_get_table_row_is_header`, never by the `"table_row"` type string. |
| 51-I11 | Y | Every cell's inline spans route through the same inline-span rendering as a paragraph (`inline_spans` + `wrap_cell` + the span-element path); inline bold/italic/code/link survive. |
| 51-I12 | Y | No cell content is silently discarded. Over-wide **unbreakable tokens are hard-broken at the column boundary** by `wrap_cell`; content that still cannot fit routes to the pipe-text fallback, whose over-wide tokens are **also hard-broken** (51-L12). |
| 51-I13 | Y | A rendered grid's total width is ≤ the width **actually available at the table's nesting point** (`max(1, content_width - indent)`, 51-H5), or the pipe-text fallback is used. |
| 51-I14 | Y | Alignment is applied per column from `cmark_gfm_extensions_get_table_alignments`; a null/absent alignment array means left; alignment is span-safe (`hbox`+`filler`, or `align_right`/`hcenter`) and does **not** use the string-only `paragraphAlign*` helpers (51-M8). |
| 51-I15 | Y | Empty cells and ragged/short rows render without a crash; the outer `catch (...)` fallback is preserved. |
| 51-I16 | Y | `"table"` remains in the extensions array; the cheap mitigation (5.2.11) is not active when D3 is implemented. |
| 51-I17 | pin | `MarkdownRenderer::render` remains pure and never throws (D16). |
| 51-I18 | Y | A table that does not parse as a table (no extension / malformed) still renders its pipe text visibly (the `PARAGRAPH` path). |

### 5.5 Failure modes

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 51-F4 | — | Table with zero columns | `compute_table_layout` yields `fits = false`; pipe-text fallback (51-D3.7). |
| 51-F5 | F8 | `available_width` smaller than one column (tiny pane or deep prefix indent) | Pipe-text fallback (51-D3.7, 51-H5). |
| 51-F6 | — | `cmark_gfm_extensions_get_table_alignments` returns `nullptr` | All columns left (51-I14). |
| 51-F7 | — | Ragged row (fewer/more cells than the header) | cmark autocompletes/ignores; best-effort grid (51-D3.10). |
| 51-F8 | F5/F8 | Very large table (many rows) | Bounded by cmark's `MAX_AUTOCOMPLETED_CELLS`; the renderer adds no unbounded buffer. Renderer-level row cap is OQ-51-4. |
| 51-F9 | — | Table nested inside a block quote / list item / the user-block gutter | `render_block` recursion reaches `render_table` with `context.indent` incremented (51-H5); the grid is laid out for `available_width` and nested in the parent element. If it cannot fit `available_width`, the pipe-text fallback fires — the grid is never clipped by the enclosing prefix. |
| 51-F10 | — | Header row misclassified (type string `"table_header"` vs shared numeric type `32781`) | Rows classified with `is_header`, never by type string; `is_header == 0` handled defensively (51-I10). |
| 51-F11 | F6 | Streaming a partial table | cmark parses what is complete; the last row may be incomplete; never throws (51-I15). |

---

## 6. D4 — Ctrl+Q exits; Ctrl+D deletes in the switcher

### 6.1 Current state (verified)

- **The exit binding to move.** `src/ui/supervisor.cpp:2993` is
  `if (event == ftxui::Event::CtrlD) { begin_exit(/*allow_prompt=*/true); return true; }`
  — the global Ctrl+D handler in `handle_event_inner`.
- **Ctrl+Q is unbound.** A tree-wide search for `CtrlQ` in `src/` and `include/`
  finds nothing.
- **The exit path.** `begin_exit(bool allow_prompt)` (`src/ui/supervisor.cpp:539`)
  guards on `quit_.load() || model_.exitConfirm.open` (`:540`), disarms Esc,
  computes the orphaning set, and either confirms directly or raises the existing
  last-supervisor exit confirmation (`ExitConfirmState`, handled by
  `handle_exit_confirm` at `:767`; the modal is dispatched first at `:2969`).
  Ctrl+Q therefore becomes `begin_exit(true)`. **Precise behaviour (51-L1):** the
  confirmation is raised **iff** the orphaning set is non-empty
  (`begin_exit`: `orphaning.empty() || !allow_prompt || options_.no_prompt` ⇒
  exit directly); Ctrl+Q preserves that, it does not unconditionally prompt.
- **Ctrl+Q is safe in raw mode (51-L4).** `Ctrl+Q` is ASCII `DC1`/XON, which a
  cooked terminal would consume as flow control. `TerminalLayer::enterRawMode()`
  clears `IXON` (`src/ui/terminal_layer.cpp:47`) precisely so `Ctrl+S`/`Ctrl+Q`
  reach the application (`include/ymh/ui/terminal_layer.hpp:5-6`, U17/§7.4), and
  FTXUI exposes `Event::CtrlQ` (`ftxui/component/event.hpp:83`). The TUI always
  enters raw mode; if raw mode were not active the key would be swallowed by the
  terminal (a documented assumption, not a code path D4 adds). `ymh run`
  (headless) has no key handling and is unaffected.
- **Overlay-first dispatch.** `handle_event_inner` (`:2964`) routes, in order:
  `handle_exit_confirm` (`:2969-2971`) → `handle_dialog` (`:2972-2975`) →
  `handle_context` (`:2976-2978`) → `handle_notice` (`:2979-2981`) →
  `handle_switcher` (`:2982-2984`), then the global keys CtrlS/CtrlP (`:2985`),
  CtrlD (`:2993`), CtrlC (`:2997`), CtrlN (`:3001`), CtrlO (`:3005`), … and
  finally `handle_input`.
- **KEY FINDING — one overlay, not two.** The Ctrl+S workspace switcher and the
  `/sessions` list are the **same** overlay: `SwitcherOverlayModel`
  (`include/ymh/ui/ui_model.hpp:403`), distinguished by `SwitcherSource::Live` vs
  `SwitcherSource::History` (`:369`) and opened by `open()` / `openHistory()`
  (`:411`/`:414`). Its keys are handled by `handle_switcher`
  (`src/ui/supervisor.cpp:2583`): Esc/CtrlC close (`:2584`), ArrowUp/Down or j/k
  move (`:2590`/`:2594`), Tab toggles expand (`:2598`), `r` refreshes in History
  (`:2602`), Return selects (`:2609`), and the default branch returns `true`
  (`:2623`) — so adding Ctrl+D there is clean. **One D-item covers both popups.**
- **The cursor is hierarchical.** `SwitcherCursor { WorkspaceId workspace;
  std::optional<SessionId> session; }` (`ui_model.hpp:396-401`). "The highlighted
  item" is therefore the **session** when `cursor.session` has a value and the
  **workspace** otherwise.
- **The confirmation precedent already exists.** The Esc-Esc interrupt
  confirmation from 48-D2 is an arm/disarm mechanism: `EscArm`
  (`ui_model.hpp:202`), `esc_armed_at`, `kEscArmTimeout{3000ms}`
  (`src/ui/supervisor.cpp:54`), `disarm_esc` (`:497`), arm-on-first-Esc /
  confirm-on-second (`:2657-2679`), and any other key disarms (`:2649`). D4
  **reuses that pattern** for the double Ctrl+D; it does not invent a second
  mechanism.
- **Existing storage primitives (do not invent new ones).**
  - `WorkspaceRegistry::removeWorkspace(WorkspaceId)` —
    `include/ymh/registry/registry.hpp:255`, documented "refuses if
    non-empty/hosted"; impl `src/registry/registry.cpp:1017-1043` throws
    `WorkspaceNotEmpty` when `workspace_sessions` junction rows remain
    (`:1028-1031`) and `HostClaimed` when a live host claim is held
    (`:1033-1036`).
  - `WorkspaceRegistry::removeSession(WorkspaceId, SessionId)` —
    `registry.hpp:282`, impl `registry.cpp:1261-1282` — removes the junction row
    under a Delete mutation marker. **Registry layer only** (no store write).
  - `WorkspaceRegistry::listSessions(WorkspaceId)` (`registry.hpp:220`) — the
    `workspace_sessions` junction rows; the source of the workspace-delete
    session count (51-L3).
  - `WorkspaceRegistry::archiveSession(WorkspaceId, SessionId, bool)` —
    `registry.hpp:283` — the soft alternative (rejected; 6.2.7).
  - **The CLI delete precedent is two-layered, and has two paths (51-H1).**
    - **Live path** — `apply_live` (`src/cli/session_cli.cpp:420-461`): connects
      to the workspace's daemon (`protocol::HostConnection`) and issues
      `protocol::method::kSessionDelete` with
      `{"session", id}, {"confirm", true}, {"only_if_empty", true}, {"force", force}`
      (`:431-435`); it maps the reply's `AppCode::DependentSession` /
      `RpcCode::InvalidParams` / `AppCode::UnknownSession` to per-entry status
      (`:440-454`). This is the daemon RPC path D4 reuses.
    - **Offline path** — `apply_stopped` (`:467-515`): opens the store directly,
      skips when `store->hasDependents(id)` (`:496-500`), then calls
      `writer->removeSession(workspace, id)` (the registry junction, `:501`)
      **and** `manager.deleteSession(id, /*only_if_empty=*/true)` (the sessions.db
      event log, `:503`). Both layers are required for a **complete** delete.
- **KEY ARCHITECTURAL FACT (51-H1).** The **supervisor** cannot perform a
  complete session delete in-process. It opens sessions.db **read-only**
  (`SessionPersistence::openReadOnly`, `src/ui/supervisor.cpp:1893`; `db_path =
  root/".ymh"/"sessions.db"`, `:1886`) and has **no `SessionManager`**. The
  **daemon owns writes** and already exposes the operation end-to-end:
  - `protocol::method::kSessionDelete = "session.delete"`
    (`include/ymh/transport/protocol.hpp:523`);
  - served at `src/transport/protocol_server.cpp:444` (requires
    `confirm: true`, accepts `only_if_empty`/`force`);
  - implemented by `HostRuntime::deleteSession`
    (`include/ymh/host/host_runtime.hpp:171`,
    `src/host/host_runtime.cpp:805`) — it performs **both** layers
    (`registry_.removeSession` then `runtime_.sessions().deleteSession`) and all
    refusals;
  - client seam `include/ymh/transport/host.hpp:102`
    `virtual void deleteSession(const SessionId& id, bool only_if_empty, bool force) = 0;`.
  The supervisor reaches it through the existing per-workspace
  `SupervisorConnection` seam (`submit_to`, `src/ui/supervisor.cpp:1683`;
  `SupervisorReply{ok, result, error_code, error}`,
  `include/ymh/ui/supervisor_connection.hpp:94-99`), not by opening the store.
  (The supervisor **does** hold a read-write **registry** handle —
  `WorkspaceRegistry::open(default_registry_config())`, `src/cli/cli.cpp:510` —
  which the workspace-delete cascade uses.)

### 6.2 Decision (51-D4)

1. **Exit moves to Ctrl+Q.** Rebind the global exit handler from Ctrl+D to
   Ctrl+Q: `if (event == ftxui::Event::CtrlQ) { begin_exit(/*allow_prompt=*/true);
   return true; }`. This still routes through `begin_exit`, preserving the 16 §7.6
   confirmation semantics — it prompts **iff** the orphaning set is non-empty and
   otherwise exits directly (51-L1); Ctrl+D in Conversation mode is **no longer**
   exit. Ctrl+Q reaches the app because raw mode clears `IXON` (6.1, 51-L4).
2. **Ctrl+D in the switcher deletes.** Add a Ctrl+D branch to `handle_switcher`
   (before its default `return true`). It acts on the **delete target** (§1.5):
   the session when `cursor.session` has a value, else the workspace.
3. **Double-press arm (reuse 48-D2's pattern), bound to a specific target
   (51-M1/M2).** The first Ctrl+D **arms** the delete (UI-only state on
   `SwitcherOverlayModel`; 6.3) and shows a hint (`- one more Ctrl+D to delete`);
   a second Ctrl+D **within the same window** (`kEscArmTimeout`, 3000 ms)
   confirms. The arm records the exact target (`delete_target = cursor`) at arm
   time; on confirm the live cursor must still equal the armed target, else the
   arm is ignored and disarmed — a cursor move can never redirect a confirmed
   delete. Any other key, or the timeout, disarms. A single Ctrl+D never deletes.
   The arm is evaluated on the switcher's key events and on the same periodic
   tick that disarms Esc (`src/ui/supervisor.cpp:1734-1746`); it resets to
   `Disarmed`/`nullopt` on `open()`, `openHistory()` and `close()` (51-I21).
4. **Session delete — via the owning daemon's `session.delete` RPC
   (51-H1/H2).**
   - Resolve the owning workspace of `cursor.session`. If it has **no live
     daemon**, **refuse** with a notice ("workspace not running; use `ymh session
     prune`") and no mutation: the supervisor cannot write the store (6.1).
     Auto-spawn-then-delete is OQ-51-9.
   - Otherwise issue, through the existing `submit_to(workspace, ...)` seam:
     `protocol::method::kSessionDelete` with params
     `{"session", id}, {"confirm", true}, {"only_if_empty", false}, {"force", false}`.
     - `only_if_empty = false`: the user asked to *delete* a (possibly non-empty)
       session. The second `SessionManager::deleteSession` parameter is
       `only_if_empty`, **not** `force` (`include/ymh/session/session_manager.hpp:75`);
       passing `true` would **refuse** a non-empty session. The Rev-2
       `deleteSession(id, /*force=*/true)` call was both mislabelled and
       self-defeating (51-H2).
     - `force = false`: the daemon refuses the **active** session when `!force`
       (`src/host/host_runtime.cpp:834-836`); D4 **wants** that refusal, so the
       active session stays protected (51-I22, 51-F15).
   - The daemon performs **both** layers (`registry_.removeSession` then
     `runtime_.sessions().deleteSession`, `host_runtime.cpp:855-860`) and all
     refusals; the supervisor performs **none**.
   - Map the `SupervisorReply` to a status-bar notice (no mutation on any
     refusal):
     - `RpcCode::InvalidParams` / "turn in progress" → 51-F19;
     - `RpcCode::InvalidParams` / "active session" → 51-F15;
     - `RpcCode::InvalidParams` / "session is not empty" → unreachable under D4's
       `only_if_empty=false`;
     - `AppCode::DependentSession` (`-32009`) → "has dependent sessions" (51-F14);
     - `AppCode::UnknownSession` (`-32006`) → "already gone" (51-F21);
     - any other `!ok` reply → generic notice (51-I25).
   - This mirrors the live CLI path `apply_live`
     (`src/cli/session_cli.cpp:420-461`) exactly.
5. **Workspace delete — registry-only cascade, non-running workspaces only.**
   - A workspace delete is **always refused** whenever the workspace has a live
     daemon (the Ctrl+S **Live** source, or a live workspace shown in History):
     Live lists only live daemons and `removeWorkspace` refuses a live claim
     (`HostClaimed`, `registry.cpp:1033-1036`) (51-M5). The only reachable case is
     a **non-running** workspace from `/sessions` (History); "stop-then-delete" is
     OQ-51-6.
   - A non-running workspace has no daemon, so there is no `session.delete` RPC.
     The supervisor uses its **read-write registry handle** (6.1):
     1. Read the junction set `options_.registry->listSessions(workspace)`; the
        **confirmation states its size** (`delete workspace "alpha" and its 3
        sessions?`). The count comes from the registry `workspace_sessions`
        junction rows, **not** the Live model's session list (which holds only
        live sessions) — 51-L3.
     2. For each junction row, `removeSession(workspace, id)` (registry junction
        only; the store is untouched, D4.8).
     3. `removeWorkspace(workspace)`.
   - **Do not** call `SessionManager` / touch sessions.db: D4.8 leaves
     `<workspace>/.ymh/` intact, so the cascade is registry-only. This is why the
     workspace delete is possible without a daemon while the session delete is
     not.
   - Refusals → notice, no partial delete: `HostClaimed` (live claim, 51-F16),
     `WorkspaceNotEmpty` (a junction reappeared mid-cascade, 51-F20),
     `MutationInProgress` (pending marker, 51-F17). A daemon starting mid-cascade
     is a race: the cascade re-checks the live claim before each mutation and
     stops with a notice (already-removed junctions are durable and documented,
     not rolled back) — OQ-51-9.
6. **Live workspace delete from the Live source is ALWAYS a refusal (51-M5).**
   Explicit, not implied: a workspace delete issued from the Ctrl+S `Live` source
   is **always** refused with a notice naming the reason ("stop the workspace
   first") and is never attempted. "Stop-then-delete" is OQ-51-6, not implemented
   here.
7. **`archiveSession` is the rejected alternative.** `archiveSession(..., true)`
   would soft-hide a session without destroying its event log. D4 rejects it
   because the user asked to *delete*, and because the shipped CLI delete
   (`session_cli.cpp:420-515`, both paths) is a hard two-layer delete; a soft path
   would diverge from it. Recorded, not used.
8. **On-disk boundary (pinned).** Deleting a workspace removes its `registry.db`
   row and its session junctions **only**. The workspace's `<workspace>/.ymh/`
   directory (including `sessions.db`) is **not** deleted. This matches the
   shipped `removeWorkspace` contract and the CLI, and leaves an orphan
   `sessions.db` (spec 22 §11.1). Removing the on-disk directory is recorded as
   an open question (OQ-51-7), not implemented here.
9. **No new key outside the switcher.** Ctrl+D remains unbound in Conversation
   mode (it no longer exits); the delete action exists **only** while the
   switcher overlay is open. This keeps F6 (exactly one focus owner) intact.
   - **Stale Ctrl+D hint/comment text must be updated (51-M6).** Two shipped
     sites still say Ctrl+D:
     - `src/ui/ui_render.cpp:415` — the empty-transcript hint
       `"Type a message and press Enter. Ctrl+D or /exit quits."` becomes
       `"… Ctrl+Q or /exit quits."`;
     - `src/ui/supervisor.cpp:538` — the `begin_exit` comment "Ctrl+D and `/exit`
       stay thin callers" becomes "Ctrl+Q and `/exit` …".
     Both are part of the D4 change; a grep for `Ctrl+D` in `src/ui/` must find
     only the switcher delete handler after D4.
10. **After a SUCCESSFUL delete (51-M4).** The overlay re-clamps `cursor` to a
    still-valid node and refreshes the list, so no stale node is ever rendered:
    - a deleted **session** → `cursor.session` moves to the previous session in
      that workspace, or resets to the workspace when none remain;
    - a deleted **workspace** → the cursor moves to a neighbouring workspace
      node, and the overlay closes if no nodes remain;
    - the list is re-read from its source — History re-runs
      `catalog_->refreshNow()` (the `r` path, `src/ui/supervisor.cpp:2602-2607`);
      Live re-requests `session.list`.
    Pinned by 51-I27.

### 6.3 Interfaces (pinned)

```cpp
// ── include/ymh/ui/ui_model.hpp ────────────────────────────────────────────
// 51-D4.3: the switcher's double-Ctrl+D arm. Reuses the 48-D2 EscArm enum and
// the kEscArmTimeout window; UI-only, never persisted.
struct SwitcherOverlayModel {
    // ... existing fields (workspaces, cursor, filter, collapsed, source) ...
    EscArm                                     delete_arm = EscArm::Disarmed;  // 51-D4.3
    std::optional<std::chrono::steady_clock::time_point> delete_armed_at;      // 51-D4.3
    // 51-D4.3 (51-M1/M2): the exact target the arm is bound to. The confirm
    // requires the live cursor == *delete_target; a mismatch disarms, so a
    // cursor move can never redirect a confirmed delete.
    std::optional<SwitcherCursor>              delete_target;                  // 51-D4.3

    // 51-D4.3/51-M1/M2: reset delete_arm/delete_armed_at/delete_target. Called
    // by open(), openHistory() and close() (none of them reset the arm today).
    void disarm_delete();
    // 51-D4.10 (51-M4): re-clamp `cursor` to a still-valid node after a delete.
    void clamp_cursor();
};
```

```cpp
// ── src/ui/supervisor.cpp (file-local) ─────────────────────────────────────
// 51-D4.1: exit is now Ctrl+Q; Ctrl+D is not a Conversation-mode key.
// if (event == ftxui::Event::CtrlQ) { begin_exit(/*allow_prompt=*/true); return true; }

// 51-D4.2/D4.3: the switcher's Ctrl+D arm/confirm handler. On the first press it
// arms (capturing delete_target) and pushes the hint; on the second, within
// kEscArmTimeout and with cursor == *delete_target, it dispatches to the
// session/workspace delete below. Any other key disarms.
bool handle_switcher_delete(const ftxui::Event& event);

// 51-D4.4: session delete via the owning daemon's `session.delete` RPC
// (only_if_empty=false, force=false). Surfaces a notice on refusal and performs
// NO in-process store write.
void delete_highlighted_session(const SwitcherCursor& cursor);
// 51-D4.5/D4.6: the registry-only workspace cascade (non-running workspaces
// only). Surfaces a notice on refusal.
void delete_highlighted_workspace(const SwitcherCursor& cursor);
// 51-D4.10 (51-M4): after a successful delete, clamp_cursor() + refresh the
// list (History: catalog_->refreshNow(); Live: re-request session.list).
void refresh_after_delete();
```

`handle_switcher` gains, before its `:2623` default:

```cpp
if (event == ftxui::Event::CtrlD) {
    return handle_switcher_delete(event);
}
```

and, for any non-Ctrl+D key, the existing arm is disarmed (mirroring
`src/ui/supervisor.cpp:2649-2653`).

### 6.4 Invariants

| ID | Gate | Invariant |
|---|---|---|
| 51-I19 | Y | Ctrl+Q calls `begin_exit(/*allow_prompt=*/true)`, preserving 16 §7.6: it raises the exit confirmation **iff** the orphaning set is non-empty (otherwise it exits directly, 51-L1); Ctrl+D no longer exits in Conversation mode. |
| 51-I20 | Y | In the switcher overlay (both `Live` and `History`), Ctrl+D targets the session when `cursor.session` has a value, else the workspace. |
| 51-I21 | Y | A first Ctrl+D arms the delete and records `delete_target = cursor`; a second Ctrl+D within `kEscArmTimeout` and with the live cursor still equal to `*delete_target` confirms; any other key or the timeout disarms; `open()`/`openHistory()`/`close()` reset the arm. A single Ctrl+D never deletes, and a cursor move can never redirect a confirmed delete. |
| 51-I22 | Y | A session delete is issued as a `session.delete` RPC to the **owning daemon** with `only_if_empty=false`, `force=false`; the daemon performs both layers and refuses the active session (because `force=false`) and dependent sessions. The supervisor performs no store write. If the owning workspace has no live daemon the delete is refused with a notice. |
| 51-I23 | Y | A workspace delete is registry-only and reachable only for a **non-running** workspace: it states the junction-row count from `WorkspaceRegistry::listSessions` (not the Live session list, 51-L3), removes each junction via `removeSession`, then calls `removeWorkspace`; it refuses with a notice when a live host claim is held (`HostClaimed`) and is **always** refused from the Ctrl+S Live source (51-M5). |
| 51-I24 | Y | Deleting a workspace removes its registry row and session junctions only; `<workspace>/.ymh/` (including `sessions.db`) is not deleted. |
| 51-I25 | Y | Every refusal path surfaces a status-bar notice and leaves the model, the registry and the overlay consistent (no phantom workspace, no partial delete on refusal). Any `SupervisorReply` with `ok == false` maps to a notice; no refusal mutates durable state. |
| 51-I26 | pin | The delete arm is UI-only and never persisted; a refused delete mutates nothing durable. |
| 51-I27 | Y | After a **successful** delete the switcher re-clamps `cursor` to a valid node (previous session, or the workspace when its sessions are gone, or a neighbouring workspace when the workspace node is gone; overlay closes when no nodes remain) and refreshes the list from its source; no stale node is rendered (51-M4). |

### 6.5 Failure modes

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 51-F12 | F6 | Ctrl+D pressed once, then the window expires | Disarmed on the next tick/key; nothing deleted (51-I21). |
| 51-F13 | F6 | Ctrl+D pressed, then any other key | Disarmed; nothing deleted (51-I21). |
| 51-F14 | — | Session delete refused: has dependents | Daemon reply `AppCode::DependentSession`; notice "has dependent sessions"; no mutation (51-I22). |
| 51-F15 | — | Session delete refused: active session of a live daemon | Daemon reply `RpcCode::InvalidParams` / "active session" (because `force=false`); notice; no mutation (51-I22, 51-H2). |
| 51-F16 | — | Workspace delete refused: live host claim | Notice "stop the workspace first" (51-I23); always refused from the Live source (51-M5); "stop-then-delete" is OQ-51-6. |
| 51-F17 | — | Workspace delete: a registry mutation marker is pending (`MutationInProgress`) | Notice; no mutation (51-I25). |
| 51-F18 | — | Ctrl+Q while the exit confirmation is already open | `begin_exit`'s quit/exitConfirm-open guard no-ops (6.1); the modal is unaffected. |
| 51-F19 | — | Session delete refused: a turn is in progress | Daemon reply `RpcCode::InvalidParams` / "turn in progress" (`host_runtime.cpp:807-831`); notice; no mutation (51-I22). |
| 51-F20 | — | Workspace delete refused: the workspace is not empty (`WorkspaceNotEmpty`) | Notice; no mutation. Arises when a junction reappears mid-cascade; the cascade pre-removes all junctions (51-I23, 51-M3). |
| 51-F21 | — | Session delete: the session is already gone (`UnknownSession`, `-32006`) | Daemon reply `AppCode::UnknownSession`; notice "already gone"; no mutation (51-I22). |
| 51-F22 | — | Session delete on a workspace with no live daemon (History source) | Refused with a notice ("workspace not running; use `ymh session prune`"); no mutation; auto-spawn-then-delete is OQ-51-9 (51-I22). |

---

## 7. Invariants (consolidated)

Numbered `51-I#`; contiguous with §4.4/§5.4/§6.4. Each is testable or an explicit
**pin** (retained behaviour, excluded from the gate). The "Gate" column is `Y` or
`pin`.

| ID | Gate | Invariant | Section |
|---|---|---|---|
| 51-I1–I9 | see §4.4 | User-prompt emphasis | D2 |
| 51-I10–I18 | see §5.4 | Markdown tables | D3 |
| 51-I19–I27 | see §6.4 | Ctrl+Q exit / Ctrl+D delete | D4 |

No numbering gaps: `I1…I27` are defined exactly once, above. (D1, withdrawn,
defines none.)

---

## 8. Failure modes (consolidated)

Component-local tags `51-F#`, contiguous `F1…F22`; each maps to the shared
`F1–F12` finding where applicable (`00-architecture.md:4836-4856`).

| ID | Shared | Section |
|---|---|---|
| 51-F1–F3 | F6/F8 | D2 (§4.5) |
| 51-F4–F11 | F8/F5/F6 | D3 (§5.5) |
| 51-F12–F22 | F6 | D4 (§6.5) |

---

## 9. dsh (DeepSeek Harness) mapping

- **D2 — theme is presentation, never model.** The tint and bar live in `Theme`
  and the renderers, consistent with D13 (the UI renders a pure `UiModel`) and
  D14 (core never depends on FTXUI). No application state is added.
- **D3 — Markdown is a pure function of text.** Table rendering extends the
  existing `MarkdownRenderer` pure function; it adds no I/O, no state and no new
  dependency surface, consistent with D16 (rendering is side-effect free).
- **D4 — destructive actions go through the daemon/registry seam, and the UI only
  requests.** A **session** delete is a `session.delete` JSON-RPC to the owning
  daemon (which owns the store write and enforces the same refusals the CLI's
  `apply_live` path uses, `src/cli/session_cli.cpp:420-461`); a **workspace**
  delete (non-running workspaces only) is a registry-only cascade on the
  supervisor's read-write registry handle. The UI holds no durable state (the arm
  is UI-only), consistent with D13 and with dsh's "daemon owns durability, UI is a
  consumer" split. Reusing 48-D2's arm/disarm pattern keeps a single confirmation
  mechanism rather than inventing a second.

---

## 10. Test plan

### 10.1 Unit (hermetic, no LLM, no daemon)

| Test | Covers | Non-vacuity |
|---|---|---|
| `UI51_D2_ThemeDefaultsPinned` | 51-I2 | new (theme-token test: `user_bar` blue, `user_block_background`, light variant via `make_theme`) |
| `UI51_D2_ThemeFieldOrderPreserved` | 51-I7 | new (regression: `Theme{true, true}` / `Theme{true, false}` still mean `{color, user_block}`) |
| `UI51_D2_UserBoldUnchanged` | 51-I3 | new (user text keeps `\x1b[1m` + White; complements the existing golden) |
| `UI51_D3_SimpleTable` | 51-I10, 51-I11 | new |
| `UI51_D3_HeaderRowNotDropped` | 51-I10, 51-F10 | new (regression: header classified via `is_header`, not the `"table_row"` type string) |
| `UI51_D3_AlignmentVariants` | 51-I14 | new (span-rich cell; `hbox`+`filler`, not `paragraphAlign*`, 51-M8) |
| `UI51_D3_InlineFormattingInCells` | 51-I11 | new |
| `UI51_D3_WiderThanPaneFallsBack` | 51-I13, 51-F5 | new |
| `UI51_D3_OverWideTokenHardBreak` | 51-I12 | new (negative: unbreakable token longer than its column) |
| `UI51_D3_RowHeightMatchesWrap` | 51-I12 | new (51-M7: a cell that greedy-wraps to more lines than `ceil(width/col)` is not clipped; height == `wrap_cell` lines) |
| `UI51_D3_EmptyCell` | 51-I15 | new |
| `UI51_D3_RaggedTable` | 51-I15, 51-F7 | new |
| `UI51_D3_ZeroColumnsFallsBack` | 51-F4 | new |
| `UI51_D3_NullAlignmentsMeansLeft` | 51-I14, 51-F6 | new |
| `UI51_D3_ExtensionStillEnabled` | 51-I16 | new (a pipe table parses as a grid, not a paragraph; `"table"` still attached) |
| `UI51_D3_MalformedPipeTextStillVisible` | 51-I18 | new (a delimiter-less pipe line renders visibly as a paragraph) |
| `UI51_D3_LargeTableBounded` | 51-F8 | new (many rows; renders without unbounded growth and never throws) |
| `UI51_D3_TableInBlockquoteUsesNestedWidth` | 51-I13, 51-F9 | new (51-H5: a table under `"> "` is laid out for `content_width - 2`, not the pane) |
| `UI51_D3_TableInUserBlockUsesNestedWidth` | 51-I13, 51-F9 | new (51-H5: the `with_left_bar` gutter reduces the table width) |
| `UI51_D3_PartialTableDoesNotThrow` | 51-F11 | new (a streaming prefix of a table; `MarkdownRenderer::render` never throws) |
| `UI51_D3_FallbackHardBreaksToken` | 51-I12, 51-F5 | new (51-L12: an over-wide token in the pipe-text fallback is hard-broken, not clipped) |
| `UI51_D4_DeleteTargetSelection` | 51-I20 | new (cursor.session set ⇒ session; unset ⇒ workspace) |
| `UI51_D4_ArmThenConfirm` | 51-I21, 51-F12 | new |
| `UI51_D4_SinglePressDoesNotDelete` | 51-I21 | new (negative) |
| `UI51_D4_DisarmOnOtherKey` | 51-I21, 51-F13 | new |
| `UI51_D4_ArmBoundToTarget` | 51-I21 | new (51-M1/M2: arm on target A, move to B, second press does not delete B) |
| `UI51_D4_ArmResetOnOpenClose` | 51-I21 | new (51-M1/M2: `open`/`openHistory`/`close` reset the arm) |
| `UI51_D4_SessionDeleteViaRpc` | 51-I22 | new (issues `session.delete` with `only_if_empty=false, force=false` to the owning daemon; no in-process store write) |
| `UI51_D4_RefuseDependentSession` | 51-I22, 51-F14 | new (daemon reply `DependentSession`) |
| `UI51_D4_RefuseLiveSession` | 51-I22, 51-F15 | new (daemon reply `InvalidParams`/"active session"; `force=false`) |
| `UI51_D4_RefuseTurnInProgress` | 51-I22, 51-F19 | new (daemon reply `InvalidParams`/"turn in progress") |
| `UI51_D4_RefuseUnknownSession` | 51-I22, 51-F21 | new (daemon reply `UnknownSession`) |
| `UI51_D4_RefuseNoDaemon` | 51-I22, 51-F22 | new (a History session in a non-running workspace is refused with a notice) |
| `UI51_D4_WorkspaceCascadeCount` | 51-I23, 51-L3 | new (confirmation states the `listSessions` junction count, not the Live list) |
| `UI51_D4_RefuseLiveWorkspace` | 51-I23, 51-F16 | new (Live-source workspace delete is always refused) |
| `UI51_D4_RefuseNonEmptyWorkspace` | 51-I23, 51-F20 | new (`WorkspaceNotEmpty` mid-cascade → notice, no partial) |
| `UI51_D4_WorkspaceOnDiskUntouched` | 51-I24 | new (`.ymh/sessions.db` survives) |
| `UI51_D4_PostDeleteCursorClamped` | 51-I27 | new (51-M4: cursor re-clamped + list refreshed after a successful delete) |
| `UI51_D4_EmptyHintSaysCtrlQ` | 51-I19 | new (51-M6: the empty-transcript hint says `Ctrl+Q or /exit`, not `Ctrl+D`) |

Table tests use the existing `render_text` helper style
(`tests/unit/markdown_renderer_test.cpp:32-39`): `Theme{false}`, 80×40 screen,
`strip_ansi`, substring assertions; the nested-width tests additionally build a
`RenderContext` with a non-zero `indent` (or render a block quote / user entry)
and assert the grid fits. The two D2 suppression tests live **only** in §10.2
(render-level ANSI goldens) — they are not duplicated here (51-L11).
`UI51_D4_*` tests drive the supervisor event router / switcher model, a **fake
`SupervisorConnection` reply** carrying the daemon's `error_code`/message for the
RPC refusals, and the registry directly.

### 10.2 Golden TUI render (`tests/unit/ui_render_golden_test.cpp`)

| Test | Covers |
|---|---|
| `UI51_D2_UserMessageTintAndBarGolden` | The transcript user message carries the bluish bar + tint (51-I1, 51-I2). |
| `UI51_D2_ComposerTintAndBarGolden` | The composer row carries the bluish bar + tint, and the caret is on the leading glyph cell after the 2-column shift (51-I1, 51-I6). |
| `UI51_D2_MonochromeUserGolden` | `Theme{false}` keeps the `│` glyph and emits no colour (51-I4). **This is the sole monochrome-suppression golden** (51-L11). |
| `UI51_D2_UserBlockGatedGolden` | Mirrors the existing `UserBlockBackgroundGatedByTheme` (`:841`): the raw `\x1b[48;2;` sequence is present for `Theme{true, true}` and absent for `Theme{true, false}` (51-I5). **Sole flag-suppression golden** (51-L11). |
| `UI51_D3_TableGolden` | A simple table's box-drawing grid, header rule and cell text (51-I10). |
| `UI51_D3_TableAlignmentGolden` | `:---:`, `---:` and `:---` columns render centred/right/left (51-I14). |
| `UI51_D3_TableFallbackGolden` | A table wider than the pane renders its synthesized pipe text (51-I13). |
| `UI51_D4_SwitcherDeleteHintGolden` | The switcher shows `- one more Ctrl+D to delete` while armed (51-I21). |
| **`ConversationSnapshot` (mandatory edit)** | The composer row changes `│>` → `││ >` (51-H3, 51-I6). Update the `kGolden` literal (`:179`): the row becomes `││ > ` + 66 spaces + `│` (72 columns). |
| **`CaretCursorLandsAtInputPosition` / `CaretCursorHandlesCjkLeadingCell` (mandatory edits)** | The 2-column gutter shift moves the expected caret column (`:2029`, `:2044`) (51-I6). |
| **`UserBrightAndIntermediateDimmedInTranscript` (mandatory edit)** | Replace the `\x1b[32m` green-bar assertion (`:2090`) with `\x1b[38;2;92;156;245m` for the recoloured `user_bar` (51-H4, 51-I2, 51-A3). |

### 10.3 Integration (FakeLLM)

- D4's delete paths are registry/session-layer integration tests (real
  `registry.db` + sessions.db under a temp state dir, plus a fake daemon reply),
  not LLM turns; the FakeLLM path is unaffected by D4. No D1 integration test
  remains (D1 withdrawn).

### 10.4 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- A live turn with a model that leaks `<thinking>` in `content` renders the tags
  as literal text — the accepted behaviour (§3.4). Skipped by default.

### 10.5 Invariant / failure-mode coverage

Every `51-I#` is exercised by at least one test in §10.1–§10.3 (the mandatory
golden edits in §10.2 cover 51-I2/I3/I6; `UI51_D3_*` cover I10–I18 and
`UI51_D4_*` cover I19–I27). Every `51-F#` has a negative or boundary test except:
- `51-F3` — a narrow-pane layout property already covered by the existing
  scroll/layout goldens;
- `51-F18` — a guard backstop on an already-open modal.
The Rev-2 claim that I3/I16/I18 and F8/F9/F11 were covered was **false** (51-M9);
this revision adds `UI51_D2_UserBoldUnchanged` (I3),
`UI51_D3_ExtensionStillEnabled` (I16), `UI51_D3_MalformedPipeTextStillVisible`
(I18), `UI51_D3_LargeTableBounded` (F8),
`UI51_D3_TableInBlockquoteUsesNestedWidth` / `UI51_D3_TableInUserBlockUsesNestedWidth`
(F9), and `UI51_D3_PartialTableDoesNotThrow` (F11). New failure modes F19–F22 are
covered by `UI51_D4_RefuseTurnInProgress`, `UI51_D4_RefuseNonEmptyWorkspace`,
`UI51_D4_RefuseUnknownSession`, and `UI51_D4_RefuseNoDaemon` respectively.

---

## 11. Open questions / interpretations

- **OQ-51-2 (theme variant selection).** `ui.theme` exists in config
  (`include/ymh/config/config.hpp:48`) but is unwired; there is no light/dark
  detection. D2 pins both variants' tokens (`make_theme`) and constructs `Dark`.
  Choosing the variant (config key, terminal query, or `COLORFGBG`) is deferred.
  **Recorded, not solved.**
- **OQ-51-4 (table row cap).** Whether the renderer should cap the number of
  rendered rows for very large tables is undecided; cmark's
  `MAX_AUTOCOMPLETED_CELLS` bounds autocompletion, not row count.
  **Recorded, not solved.**
- **OQ-51-6 (live-workspace stop-then-delete).** The Ctrl+S Live switcher lists
  only live workspaces, and `removeWorkspace` refuses a live host claim, so a
  workspace delete from the Live source is **always** refused with "stop the
  workspace first" (51-M5). Whether Ctrl+D should instead stop the daemon and then
  delete is undecided; D4 pins the conservative refusal.
  **Recorded; must be resolved before any stop-then-delete behaviour is added.**
- **OQ-51-7 (workspace on-disk `.ymh/` removal).** D4 deletes the registry row
  and session junctions only; the `<workspace>/.ymh/` directory (including
  `sessions.db`) survives as an orphan (spec 22 §11.1). Whether a workspace delete
  should also remove the on-disk directory is undecided. **Recorded, not solved.**
- **OQ-51-8 (composer left bar).** D2 pins that the composer **does** get the
  left bar, which shifts the caret box by 2 columns and changes the two caret
  goldens (§4.2.3). If the caret churn is judged too costly, the alternative is
  transcript-only emphasis (tint + bar on the transcript, tint-only on the
  composer). **Recorded; the D2.3 decision is the pinned default.**
- **OQ-51-9 (session delete in a non-running workspace).** D4's session delete is
  a `session.delete` RPC to the owning daemon (51-H1); a History session whose
  workspace has no live daemon is therefore **refused** with a notice pointing at
  the offline CLI (`ymh session prune`), because the supervisor has no store
  writer. Whether Ctrl+D should instead spawn/attach the daemon and then delete
  (mirroring `/sessions` resume) is undecided; D4 pins the conservative refusal.
  The same OQ covers the workspace-cascade race where a daemon starts mid-cascade.
  **Recorded, not solved.**

Retired: **OQ-51-1** (what Muse emits) and **OQ-51-5** (D1 allowlist breadth)
are removed with D1; **OQ-51-3** (table-node header portability) is resolved by
the §5.1 portability pin.

---

## 12. Revision log

- **Rev 3 (this revision).** Independent-review fixes. Design only; no code. All
  anchors re-verified against HEAD `e9a644a30`. Throughout the body, `51-H#`/
  `51-M#`/`51-L#` are the **review-finding** identifiers (not spec IDs, which are
  `51-I#`/`51-F#`/`51-D#`/`51-A#`/`OQ-51-#`); they trace to the list below.
  - **H1 (session delete must go through the daemon).** §6.1 now states the
    architectural fact: the supervisor opens sessions.db read-only
    (`src/ui/supervisor.cpp:1893`) and has no `SessionManager`, so §6.2.4 mandates
    issuing `session.delete` to the owning daemon via `submit_to` (protocol
    `:523`, server `:444`, `HostRuntime::deleteSession` `host_runtime.cpp:805`,
    client seam `host.hpp:102`), mapping the reply's error codes to notices. §6.1
    now cites **both** CLI paths (`apply_live` `session_cli.cpp:420-461` and
    `apply_stopped` `:467-515`). New 51-A5.
  - **H2 (`only_if_empty`/`force`).** The Rev-2 `deleteSession(id, /*force=*/true)`
    is dropped; the RPC passes `only_if_empty=false, force=false`
    (`session_manager.hpp:75` is `only_if_empty`; `host_runtime.cpp:834-836`
    refuses the active session only when `!force`). I22/F15 rewritten so the
    active-session refusal is preserved, not defeated.
  - **H3 (composer bar breaks `ConversationSnapshot`).** §4.2.3 and I6 now name
    `ConversationSnapshot` (`tests/unit/ui_render_golden_test.cpp:200`, `kGolden`
    `:179`) and pin the new composer row `││ > ` + 66 spaces + `│`.
  - **H4 (`user_bar` recolour breaks the green-bar test).** §4.2.3 and I2 now name
    `UserBrightAndIntermediateDimmedInTranscript` (`:2090`) as a mandatory edit
    and pin the new `\x1b[38;2;92;156;245m` sequence; §1.3/§2 supersede 48-D6.5
    (`docs/design/48-ui-and-config-errata.md:544-547`, `:1027`).
  - **H5 (table width ignores nesting).** §5.1/§5.2 add `RenderContext::indent`
    and lay the table out for `max(1, content_width - indent)`; block quote, list
    markers and the user-block gutter increment it. I13/F5/F9 reconciled.
  - **M1/M2 (delete-arm state).** §1.5/§6.2.3/§6.3 pin the arm's location
    (`SwitcherOverlayModel`), its target binding (`delete_target`), and its reset
    on `open`/`openHistory`/`close` and the periodic tick; new I21 text.
  - **M3 (missing refusals).** New F19 (turn in progress), F20 (`WorkspaceNotEmpty`)
    and the active-session F15 rewrite.
  - **M4 (post-delete consistency).** §6.2.10 pins cursor re-clamp + list refresh;
    new I27 and `clamp_cursor()`/`refresh_after_delete()`.
  - **M5 (Live workspace delete always refused).** §6.2.5/§6.2.6 and I23 state it
    explicitly.
  - **M6 (stale Ctrl+D hints).** The change list now includes
    `src/ui/ui_render.cpp:415` (`Ctrl+D or /exit` → `Ctrl+Q or /exit`) and
    `src/ui/supervisor.cpp:538` (comment).
  - **M7 (`ceil` is not a wrap upper bound).** §5.2.5 pins `wrap_cell` and
    `row_height = wrap_cell(...).size()`; `UI51_D3_RowHeightMatchesWrap`.
  - **M8 (unimplementable alignment/hard-break).** §5.2.6/§5.3 replace the
    `paragraphAlign*`-based sketch with span-safe `hbox`+`filler` and the
    pre-wrapped `TableCell::lines`.
  - **M9 (false coverage claim).** §10.1/§10.5 add I3/I16/I18 and F8/F9/F11 tests
    and correct the claim.
  - **M10 (I1 self-contradiction).** I1 now scopes "no hardcoded colour" to the
    bar/tint and explicitly exempts the pre-existing `> ` green marker.
  - **L1 (I19 overstatement).** I19/§6.2.1 now say the confirmation is raised iff
    the orphaning set is non-empty.
  - **L2 (invariant-level supersession).** §1.3/§2/51-A4 supersede 45-I25
    (`docs/design/45-ui-interaction-errata.md:1666`) and 16's Ctrl+D thin-caller
    references (`docs/design/16-daemon-ownership.md:688`, `:717`, `:801`, `:2104`).
  - **L3 (session-count source).** §6.2.5/I23 pin the count to
    `WorkspaceRegistry::listSessions` junction rows, not the Live session list.
  - **L4 (Ctrl+Q raw mode).** §6.1 pins that `enterRawMode()` clears `IXON`
    (`terminal_layer.cpp:47`, `terminal_layer.hpp:5-6`).
  - **L9 (`make_theme` site).** §4.2.8 pins it `inline` in `theme.hpp` (no new
    translation unit, no CMake change; `ymh_ui`, `CMakeLists.txt:710-720`).
  - **L10 (file-local "interfaces").** §4.3 marks `render_input`/`paint_bg` as
    file-local helpers, not the public seam.
  - **L11 (duplicate suppression tests).** §10.1 duplicates removed; they live
    only in §10.2.
  - **L12 (fallback clipping).** §5.2.7/I12 pin that the fallback routes through
    `wrap_cell` so over-wide tokens are hard-broken; new
    `UI51_D3_FallbackHardBreaksToken`.
- **Rev 2.** Targeted fixes after the M1 drift to HEAD
  `e9a644a30` (suite 1895/1895 green). All anchors below are pinned to
  `e9a644a30`.
  - **D1 withdrawn** (user decision, §3): the inline-reasoning-fold requirement
    and all its interfaces, invariants, failure modes, tests and open questions
    (OQ-51-1, OQ-51-5) are removed; the spec no longer amends spec 47. The correct
    `reasoning_content` channel already folds (47 set `capabilities.reasoning =
    true`, `src/llm/model_profile.cpp:27`).
  - **D2 corrected**: the dead `true_color` gate is deleted (`Theme` has no such
    field; `src/ui/supervisor.cpp:3041` aggregate-initializes `color`). The
    minimal change is pinned (`user_block_background` appended, `user_bar`
    recoloured), a field-order rule is added for the 2-field golden aggregate
    inits (`tests/unit/ui_render_golden_test.cpp:841`), the composer-left-bar
    decision and its caret-golden consequence are pinned, and the D2 tests move
    from direct `paint_bg` linking (impossible: file-local) to render-level ANSI
    goldens.
  - **D3 corrected** (six blocking errors): `align_left`/`align_center` do not
    exist in FTXUI 6.1.9, so alignment uses `align_right`/`hcenter` and
    `paragraphAlign*`; dispatch uses `cmark_node_get_type_string` (no
    `table.h`/`CMARK_NODE_TABLE*` symbols), with the `"table_header"` vs shared
    numeric type `32781` header trap pinned; variable row heights are modeled;
    I12's "never truncate" is corrected with a hard-break rule; the fallback
    synthesizes pipe text from cell spans and is kept; the shrink loop and width
    policy are retained.
  - **D4 added**: exit moves to Ctrl+Q (`begin_exit(true)`, still raising the
    16 §7.6 confirmation); Ctrl+D in the Ctrl+S / `/sessions` overlay deletes the
    highlighted session or workspace behind a reused 48-D2 double-press arm; the
    two-layer session delete, the session-count workspace cascade, the
    live-workspace refusal, and the registry-only on-disk boundary are pinned;
    specs 45, 22 and 16 §7.6 are amended/interacted with.
  - **Anchors refreshed** to HEAD `e9a644a30`: the golden-test citations
    (`:2029`, `:2044`, `:2090`, `:841`) replace their stale Rev-1 values; the
    other cited anchors were re-verified unchanged.
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

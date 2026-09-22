# 48 — TUI & Input Errata: the Esc-Esc Interrupt, Word-Wise Cursor Movement, a Visible Glyph-Safe Caret, the Text-Hierarchy Styling Policy, Rich Tool Lines, and Reasoning Spacing

```
Status: **draft (Rev 8)** — gate findings resolved; implemented and green. This
        spec amends the owning specs (10, 45, 46); it introduces no new component
        and no new subsystem. Like 45 and 46, every "current state" claim is
        reproducible from the shipped tree (HEAD `bbf96aeaa`).

        **Rev 8 (gate resolution).** An adversarial gate returned 4 HIGH / 11
        MEDIUM / 9 LOW. Rev 8 resolves every HIGH and the MEDIUMs that touch the
        pinned surface: (HIGH-1) the caret is now the single focus owner via a
        `component_active` anchor instead of an overridden `focusCursorBar`;
        (HIGH-2) the out-of-scope `50-D2` live test is withdrawn from §13.4;
        (HIGH-3) the colliding `summarize_tool_arguments` is unified and the
        adapter keeps the raw JSON field; (HIGH-4) the tool-entry `Notice` claim
        is withdrawn. MEDIUMs: word-boundary methods pinned as `InputModel`
        members; `glyph_len`/`glyph_at` pinned; the "first cell" claim corrected
        and a leading-cell anchor + CJK test added; `RenderContext` call sites
        use designated initializers; `truncate_spans` pins `ftxui::string_width`
        and has a CJK test. The implementation follows this revision.

        **Rev 7 (split).** The original 48 covered nine items and each gate
        round re-opened the whole surface. By user decision the spec is split by
        risk: **48 keeps the implementation-ready TUI/rendering slice** (items 2,
        4, 5, 6, 7, 8) and is self-contained; the higher-risk items moved to
        `docs/design/50-skills-mcp-and-session-lifecycle-errata.md` (skill/
        command discovery, the fresh-launch session guarantee, the MCP child
        environment, and the MCP global-layer-only guard). Decision **48-D10**
        (MCP 401/403 hints) was **dropped by user decision** (the underlying
        problem was an expired credential, not a harness defect). Changes are in
        §15. **No code may be written from this spec until an independent gate
        marks it `verified`.**

        Scope of this spec: the composer and transcript rendering only. It does
        not touch skills, session lifecycle, or MCP configuration.
```

## 1. Purpose, scope, and supersession map

### 1.1 The six in-scope requirements (verbatim, from `requirements_draft.txt`)

2. "Very important: `<Esc><Esc>` during work shall interrupt current work. One
   `<Esc>` shall display - one more `<Esc>` to interrupt. Twice is to prevent
   accidental interruptions."
4. "When editing prompt `<Ctrl + arrow [left|right]>` shall move cursor by
   words."
5. "When moving cursor within typed prompt to edit some word for example, the
   cursor position is not shown"
6. "Highlight more the text that user typed - make it brighter or deem slightly
   all LLM's intermediate text except the final answer."
7. "When a tool is called instead of shoing 'tool: grep' or 'tool: shell' show me
   more … Notice how longer lines are truncated."
8. "After each '<think>Some text..' and an empty line to separate from the next
   piece."

### 1.2 What this changes, in one sentence

A single `Esc` arms and a second `Esc` interrupts the active turn; the composer
gains `Ctrl+←`/`Ctrl+→` word motion and a real, glyph-safe caret at
`InputModel::cursor`; the transcript gains a three-level text hierarchy (user
bright / intermediate dim / final normal), one-line rich tool invocations with
span-aware, content-box-width truncation, and a blank line after each reasoning
block.

### 1.3 Supersession map

#### 1.3.1 Superseded

| ID | Prior text | Change |
|---|---|---|
| 48-S1 | `45-ui-interaction-errata.md` 45-D5 (`:533-568`): a single `Esc` in Conversation mode dismisses the command list and is otherwise a no-op (`return false`). | **Extended by 48-D2**, not superseded: when there is **no** visible command list, the first `Esc` now arms the interrupt and the second interrupts. The list-dismissal path (45-D5) is unchanged and has priority. |
| 48-S2 | `10-supervisor-tui.md` §8.2 and `src/ui/ui_render.cpp:385-400`: the composer renders `draft` followed by a literal `"_"`. | **Superseded by 48-D5.** The literal underscore is removed; a real caret is rendered at `InputModel::cursor`. |

#### 1.3.2 Not superseded (explicitly retained)

- 45-D1 history precedence, 45-D2 command list navigation, 46-D5 alias
  matching, and 46-D6 two-press Enter are **retained**; the key-binding matrix
  in §5.6 pins that 48 adds no conflict.
- 17-D1 tool folds / Ctrl+O expansion (`render_tool_entry`'s expanded branch) is
  retained; 48-D7 only changes the collapsed header line.

### 1.4 Scope boundaries

**In scope.** The Esc-Esc interrupt state machine; `Ctrl+←`/`Ctrl+→`; the
composer caret; the transcript styling policy; the one-line tool-invocation
renderer; the reasoning trailing blank line.

**Out of scope (owned by `docs/design/50-...`).** Skill/command discovery and
the `~/.ymh`/`$HOME/.claude` roots; the fresh-launch session guarantee and the
Ctrl+C re-entry diagnosis; the MCP child environment; the MCP global-layer-only
guard; the MCP credential diagnostic procedure.

**Out of scope (recorded, not pinned).**
- A `/mcp test` connectivity probe.
- Remote/SSH transport.

### 1.5 Terminology (pinned)

- **Interruptible turn** = the active session's `agent_state` is
  `Thinking` or `CallingTool` (`is_active_state`, `src/ui/ui_model.cpp:326-328`),
  i.e. exactly `UiModel::has_active_turn()` (`src/ui/ui_model.cpp:1292-1301`).
  `WaitingForPermission`/`WaitingForInput` are **not** interruptible by Esc-Esc
  (the modal owns the keys); `Cancelling` is already terminal for the turn.
- **Armed** = the Esc-Esc state in which one `Esc` has been seen and the hint is
  shown. **Disarmed** = the initial state.
- **Caret** = the terminal cursor placed at `InputModel::cursor`, rendered via
  FTXUI `focusCursorBar` (`build/_deps/ftxui-src/include/ftxui/dom/elements.hpp:171`).
- **User text** = a `ConversationRole::User` entry. **Intermediate text** = a
  `ConversationRole::Reasoning` or `ConversationRole::Tool` entry, or an
  `Assistant` entry that is followed by a `Tool`/`Reasoning` entry before the
  next `User` entry. **Final answer** = an `Assistant` entry that is not
  intermediate. (48-D6.)
- **Tool line** = the one-line `▸ name  args…` rendering of a
  `ConversationRole::Tool` entry (48-D7).

## 2. Amendment register

| Owning spec / file | Section | Amendment |
|---|---|---|
| `10-supervisor-tui.md` | §9.2/§9.3 (input) | 48-D2/D4/D5: Esc-Esc, `Ctrl+←/→`, glyph-safe caret. |
| `45-ui-interaction-errata.md` | 45-D5 | 48-D2: the no-list Esc path is no longer a pure no-op. |
| `10-supervisor-tui.md` | §8.2 (rendering) | 48-D6/D7/D8: styling hierarchy, tool line, reasoning spacing. |
| `include/ymh/ui/ui_model.hpp` | `InputModel`, `Presentation`, `SessionUiState` | 48-D2/D4/D5/D6: `cursor_left`/`cursor_right`/`glyph_floor`; `esc_arm`; `entry_presentation(…, bool turn_active)`. |
| `include/ymh/ui/render/render_context.hpp` | `RenderContext` | 48-D7: `content_width` (pane content box, distinct from `width`). |
| `include/ymh/ui/theme.hpp` | `Theme` | 48-D6: user/tool color seams. |

---
## 3. D2 — `<Esc><Esc>` interrupts the current turn

### 3.1 Current state (verified)

- `handle_input` has one `Event::Escape` case (`src/ui/supervisor.cpp:2495-2504`):
  if a command list is visible it sets `hints_dismissed = true`, clears the
  list, and returns true; otherwise it `return false` (a no-op). This is 45-D5.
- `Ctrl+C` is handled one level up, in `handle_event_inner`
  (`src/ui/supervisor.cpp:2809-2811`): it calls `cancelActive()` and returns
  true. `cancelActive()` (`:459-467`) submits `agent.cancel` for the active
  session. It does **not** quit and does **not** check `agent_state`. FTXUI
  consumes Ctrl+C because the handler returns true
  (`build/_deps/ftxui-src/src/ftxui/component/screen_interactive.cpp:809-811`),
  so Ctrl+C is **not** a quit key today.
- Modals consume Esc before `handle_input`: exit-confirm (`:730`), dialog
  (`:2345`), switcher (`:2436`), context overlay (`:2724`), notice (`:972`).
  `handle_event_inner` dispatches them first (`:2782-2796`), so Esc-Esc must
  apply only in Conversation mode with no modal.
- `has_active_turn()` is `is_active_state(agent_state)` =
  `Thinking || CallingTool` (`src/ui/ui_model.cpp:326-328`, `:1292-1301`).

### 3.2 Decision (48-D2) — USER REQUIREMENT

1. **States.** A per-session `EscArm` value:

   ```text
   Disarmed --Esc--> Armed --Esc--> (cancel) --> Disarmed
   Armed --timeout(3s)--> Disarmed
   Armed --any other handled key--> Disarmed
   Armed --modal opens--> Disarmed
   ```

2. **First Esc (Armed).** In `handle_input`, when the command list is **not**
   visible and the draft is **not** command-shaped (so 45-D5's list path keeps
   priority), a single Esc:
   - if the active session has an interruptible turn
     (`model_.has_active_turn()`), sets `esc_armed = true`,
     `esc_armed_at = now`, marks the input dirty, and returns true;
   - otherwise (no active turn) returns true **without** arming and **without**
     a hint (Esc remains a harmless no-op, so the user cannot arm a cancel that
     does nothing).
   The exact hint string is **`- one more <Esc> to interrupt`**, rendered dim
   immediately after the composer caret (48-D5) while `esc_armed`.

3. **Second Esc (interrupt).** A second Esc while `esc_armed` calls the **same**
   `cancelActive()` path as Ctrl+C, clears the arm, clears the hint, and returns
   true. This preserves F9 ("Ctrl+C cancels only the active session",
   `00-architecture.md:4853`) because `cancelActive()` is unchanged and
   session-scoped.

4. **Reset semantics.** The arm is cleared by: the second Esc; any other event
   that `handle_input` handles (`Return`, `Tab`, arrows, Backspace, Delete,
   Ctrl+U/W/E, a character insert); any modal opening; a session switch; or the
   `esc_arm_timeout` (default `3000 ms`) elapsing, evaluated on the next tick or
   the next key event. The timeout is a UI-only value and is never persisted.
   The hint is rendered **only** while `esc_armed` and the session is the active
   session.

5. **No-turn case.** If the turn ends between the two Escs (e.g. the model
   finished), the second Esc finds `has_active_turn()` false; it clears the arm
   and returns true **without** submitting a cancel. (Never cancel a turn that
   is not active; never cancel a background session.)

6. **Command-list priority.** If a command list is visible, Esc follows 45-D5
   (dismiss the list) and does **not** arm. A user who wants to interrupt while
   the list is up presses Esc twice: the first dismisses the list, the second
   arms, the third interrupts. This is documented in the hint flow; the spec
   pins it because 45-D5's "Esc is a no-op when there is no list" is superseded
   for the no-list case only.

7. **Composition with 46-D6.** The two-press-Enter rule is untouched: Esc is
   never part of the Enter path, and arming never mutates `input.draft` or
   `input.cursor`.

### 3.3 Invariants

- **48-I1.** Esc-Esc cancels **only** the active session's turn, via
  `cancelActive()`; it never quits, never switches session, never touches
  another workspace.
- **48-I2.** The hint string is exactly `- one more <Esc> to interrupt` and is
  visible iff `esc_armed`.
- **48-I3.** `esc_armed` is cleared by any handled key other than the arming
  Esc, by a modal, by a session switch, and by the timeout; it is per-session
  and never persisted.
- **48-I4.** With no active turn, Esc never arms and never cancels.

### 3.4 Failure modes

- **48-F1** (`F6`, input focus). A key event routed to a modal never reaches
  `handle_input`, so a modal Esc does not arm the interrupt.
- **48-F2** (`F9`). A late turn-completion between the two Escs is handled by
  48-D2.5: no cancel is submitted.
- **48-F3.** A terminal that emits two Esc bytes for a single physical key
  (some multiplexers): the second byte would interrupt. Mitigation: the arm
  requires the two Escs to be **separate** FTXUI events and the first must have
  a visible repaint frame in between; this is the same exposure Ctrl+C already
  has and is recorded, not solved (OQ-48-4).

### 3.5 Test plan

- **Unit.** `EscArm` transition table: no-list+turn → armed; second → cancel
  submitted once; timeout → disarmed; any handled key → disarmed; no-turn →
  no arm; list visible → dismiss (45-D5) and not armed.
- **Integration (fake daemon).** First Esc emits no `agent.cancel`; second Esc
  emits exactly one `agent.cancel` with the active session id.
- **Golden TUI.** Composer row while armed shows `- one more <Esc> to interrupt`
  in the dim style; gone after timeout / second Esc.

---

## 4. D4 — `Ctrl+←` / `Ctrl+→` move the cursor by words

### 4.1 Current state (verified)

- `handle_input` handles only plain arrows: `ArrowLeft` decrements `cursor` by
  one byte and `ArrowRight` increments it by one byte
  (`src/ui/supervisor.cpp:2577-2588`). There is no `Ctrl+←`/`Ctrl+→` case
  anywhere; FTXUI exposes the events as `Event::ArrowLeftCtrl` /
  `Event::ArrowRightCtrl` (`build/_deps/ftxui-src/include/ftxui/component/event.hpp:43-44`).
- `InputModel` has `delete_word()` (`src/ui/ui_model.cpp:243-254`) but **no**
  `move_word_left`/`move_word_right`. Its word boundary is space-only
  (`draft[cursor-1] != ' '`), not a character-class boundary.
- The existing Ctrl shortcuts are Ctrl+U (`clear_line`), Ctrl+W (`delete_word`),
  Ctrl+E (`edit_prompt`) (`src/ui/supervisor.cpp:2614-2632`). Ctrl+←/→ are free.

### 4.2 Decision (48-D4)

1. **Add two `InputModel` methods** (pure, no I/O), using a pinned
   **character-class** word model (the free-function sketch of Rev 7 is
   superseded by these members so §4.2/§4.5/§5.6/§9 agree — gate MEDIUM):

   ```cpp
   // A byte belongs to one of three classes (ASCII; UTF-8 continuation bytes
   // inherit the class of their lead byte):
   //   Word  : [A-Za-z0-9_]           (identifier characters)
   //   Space : ' ' and '\t'           (whitespace)
   //   Punct : everything else        (punctuation/symbols)
   enum class WordClass : std::uint8_t { Word, Space, Punct };
   // InputModel members; both are pure, total, and clamp to [0, draft.size()].
   [[nodiscard]] std::size_t word_left_boundary(std::size_t cursor) const;
   [[nodiscard]] std::size_t word_right_boundary(std::size_t cursor) const;
   ```

   - `word_left_boundary`: if `cursor == 0` return 0. First skip a run of
     `Space`; then, if the byte before is `Word`, skip the whole `Word` run; if
     it is `Punct`, skip the whole `Punct` run. Return the resulting index.
   - `word_right_boundary`: if `cursor >= draft.size()` return `draft.size()`.
     First skip a run of `Space`; then skip one run of the class at the cursor
     (`Word` or `Punct`). Return the resulting index.
   - This is the readline/Emacs `backward-word`/`forward-word` behaviour:
     `foo|Bar` → `|fooBar` is one word (both `Word`), `foo.bar|` → left stops
     after `.` (the `.` is `Punct`), and `a  |  b` → left lands before `b`.

2. **Wire the events.** In `handle_input`, before the plain-arrow cases:

   ```cpp
   if (event == ftxui::Event::ArrowLeftCtrl)  { input.cursor = input.word_left_boundary(input.cursor); ...; return true; }
   if (event == ftxui::Event::ArrowRightCtrl) { input.cursor = input.word_right_boundary(input.cursor); ...; return true; }
   ```

   Word motion never edits the draft, never changes `hints_dismissed`, and does
   not call `refresh_hints` (the draft is unchanged). It marks `UiDirtyFlag::Input`.

3. **Edge cases (pinned).** Empty draft → both are no-ops (return true, no
   change). Cursor at 0 → left is a no-op. Cursor at end → right is a no-op.
   Leading/trailing whitespace is skipped in the direction of travel. A single
   `Punct` byte is its own run.

4. **Composition with 46-D6.** Word motion is a composer edit only; `Return`
   classification is untouched. `hints_dismissed` is **not** cleared by motion
   (only a prefix edit clears it, 45-D5.2).

5. **`delete_word` (Ctrl+W) is upgraded** to the same class model for
   consistency: skip trailing `Space`, then delete one `Word` or `Punct` run.
   This is a behaviour change for `a.b|c`: today it deletes `a.b` (space-only);
   with 48-D4 it deletes `b`. Recorded as an intentional alignment.

### 4.3 Invariants

- **48-I5.** Word motion is pure and total: it never reads past the draft, never
  edits it, and terminates for every input.
- **48-I6.** `word_left_boundary(text, n) <= n <= word_right_boundary(text, n)`
  and both results are valid byte indices.
- **48-I7.** A `Punct` run is atomic; a `Word` run is atomic; whitespace is
  never part of a word.

### 4.4 Failure modes

- **48-F4.** A multi-byte UTF-8 glyph: the word boundary scan operates on bytes
  but only classifies lead bytes, so a word boundary is byte-aligned. **Glyph
  alignment is a separate concern owned by 48-D5**: the plain-arrow path is
  byte-wise today (`src/ui/supervisor.cpp:2577-2588`), and 48-D5 makes
  `ArrowLeft`/`ArrowRight` call `InputModel::cursor_left`/`cursor_right`
  (glyph-wise) with a defensive `glyph_floor` snap at render time. A malformed
  sequence is treated as `Punct` (one byte). (Recorded; no crash path.) This is
  the same definition as §11's 48-F4 row.
- **48-F5** (`F6`). A terminal that does not emit `CSI 1;5D/C` for Ctrl+Arrow:
  the key is inert; plain arrows still work. Documented, not solved.

### 4.5 Test plan

- **Unit.** Table-driven `word_left_boundary`/`word_right_boundary`:
  `""`, `"a"`, `"ab cd"`, `"a.b"`, `"a  b"`, `"  ab"`, `"ab  "`, `"a_b-c"`,
  cursor at 0/mid/end; multi-byte glyph; idempotence.
- **Integration (event).** Dispatch `ArrowLeftCtrl`/`ArrowRightCtrl` through the
  harness and assert `input.cursor`.
- **Golden TUI.** Caret position after two Ctrl+→ on `"alpha beta gamma"`.

---

## 5. D5 — The caret is rendered at `InputModel::cursor`

### 5.1 Current state (verified)

- `render_input` (`src/ui/ui_render.cpp:385-400`) builds
  `hbox(text("> "), text(draft), text("_"))`. The `"_"` is a **literal
  underscore** appended at the end, independent of `input.cursor`. So the user
  sees no caret at the edit position (item 5), and the underscore is always at
  the end even when the cursor is in the middle.
- `InputModel::cursor` is a byte index maintained by insert/Backspace/Delete/
  arrows/history (`src/ui/ui_model.cpp:194-254`, `src/ui/supervisor.cpp:2559-2639`).

### 5.2 Decision (48-D5)

1. **Remove the literal `"_"`.** `render_input` splits the draft at
   `input.cursor` and renders the caret as a single-cell **focus anchor**:

   ```cpp
   const std::size_t cursor = glyph_floor(draft, input.cursor);
   const std::string before = draft.substr(0, cursor);
   const std::string at     = cursor < draft.size() ? std::string(glyph_at(draft, cursor)) : " ";
   const std::string after  = cursor < draft.size()
                                  ? draft.substr(cursor + glyph_len(draft, cursor)) : "";
   return ftxui::hbox({
       paint(ftxui::text("> "), ftxui::Color::Green, theme) | ftxui::bold,
       ftxui::text(before),
       caret_anchor(ftxui::text(at)),   // owns the terminal cursor (see below)
       ftxui::text(after),
   });
   ```

   `glyph_len`/`glyph_at` are pinned in §9. The screen is Fullscreen and
   repainted by the existing `Renderer` (`src/ui/supervisor.cpp:2855-2860`).

   **Focus ownership (Rev 8, gate HIGH-1).** FTXUI has exactly one focus slot:
   `Render()` places the terminal cursor from the root node's
   `requirement().focused`, and `VBox::ComputeRequirement` keeps the first
   enabled focus it sees (`Focused::Prefer`,
   `build/_deps/ftxui-src/include/ftxui/dom/requirement.hpp:35-44`). The
   conversation's `focusPositionRelative` sets that slot first, so a bare
   `focusCursorBar` in the composer is overridden and the cursor stays
   `Hidden`. Rev 8 therefore makes the composer caret the **single focus
   owner**: `caret_anchor` is a decorator whose `ComputeRequirement` sets
   `focused.enabled`, `focused.node = this`, and — crucially —
   `focused.component_active = true`, which `Focused::Prefer` ranks above the
   inactive `focusPositionRelative`. `focusPositionRelative` is retained for
   conversation scrolling; it no longer owns the cursor. The anchor also pins
   `box_` to the glyph's **leading** cell in `SetBox`, because FTXUI reads the
   cursor position from `box_.x_max/y_max` (`node.cpp:151-155`); a wide (CJK)
   glyph would otherwise push the caret onto its trailing cell. The pinned
   signature is `ftxui::Element caret_anchor(ftxui::Element)` (file-local).

   **Glyph alignment (Rev 2).** Rev 1 assumed `cursor` is always on a glyph
   boundary, but plain `ArrowLeft`/`ArrowRight` move it by one **byte**
   (`--input.cursor` / `++input.cursor`, `src/ui/supervisor.cpp:2577-2588`), so
   `draft.substr(cursor, glyph_len)` can slice a multi-byte character when the
   cursor lands inside it. Rev 2 pins **both** defenses:
   - **Movement is glyph-wise.** Add `InputModel::cursor_left()` /
     `cursor_right()` that step to the previous/next UTF-8 boundary (lead-byte
     scan, `(byte & 0xC0) != 0x80`) and make `ArrowLeft`/`ArrowRight` call them.
     `Ctrl+←`/`Ctrl+→` (48-D4) keep word motion.
   - **Render snaps defensively.** `render_input` first applies
     `glyph_floor(draft, cursor)` — advances `cursor` off any continuation byte
     (`(byte & 0xC0) == 0x80`) to the next lead byte, clamped to `draft.size()`
     — then slices. `glyph_floor` is a free helper pinned in §9.

   The two together make 48-I8 hold for a byte-index cursor (48-F6).

2. **Shape.** Use `focusCursorBar` (a thin bar). On terminals without a
   configurable cursor, FTXUI falls back to its default; the caret is still
   positioned correctly because the cursor cell is derived from the element box.
   A blinking variant (`focusCursorBarBlinking`) is **not** used, to avoid a
   forced repaint every frame (the timer thread only repaints when an animation
   is active, `src/ui/supervisor.cpp:2876-2879`).

3. **Hint suffix.** While `esc_armed` (48-D2), append
   `ftxui::text("- one more <Esc> to interrupt") | ftxui::dim` after the caret.

4. **Command hints.** `render_command_hints` is unchanged; it renders above the
   input row (`src/ui/ui_render.cpp:1213-1215`).

### 5.3 Invariants

- **48-I8.** The caret is at `input.cursor` for every value in
  `[0, draft.size()]`; `cursor` is glyph-aligned (or snapped at render by
  `glyph_floor`); there is no other cursor glyph.
- **48-I9.** Rendering never mutates the model (the existing pure-render rule);
  the cursor is read from `InputModel::cursor` only.
- **48-I10.** The caret and the armed hint coexist; the hint is dim and never
  displaces the caret.

### 5.4 Failure modes

- **48-F6.** `cursor > draft.size()` **or** `cursor` inside a UTF-8
  continuation byte (a model invariant violation): clamp / `glyph_floor` at
  render time and log once. (Defensive; `cursor_left`/`cursor_right` maintain
  the invariant.)
- **48-F7** (`F6`). A full-screen repaint while a permission dialog is open: the
  dialog is composited on top (`render_dialog`, `src/ui/ui_render.cpp:1227-1234`),
  so the caret is not visible; this is correct.

### 5.5 Test plan

- **Unit.** `render_input`-level helper: caret index for cursor 0/mid/end; UTF-8
  glyph; empty draft.
- **Golden TUI.** Three renders with cursor 0 / mid / end of `"hello"`; assert
  `Screen::cursor().shape == Bar` and `Screen::cursor().x` equals
  `1 /*border*/ + 2 /*prompt*/ + ftxui::string_width(before)` (gate MEDIUM: the
  Rev 7 `2 + cursor` ignored the border and is wrong for non-ASCII).
- **Golden TUI.** A CJK draft (`"日本語"`) at cursor 0/3/6/9: the caret sits on
  the leading cell of each glyph, never its trailing cell (gate MEDIUM).
- **Golden TUI.** Armed hint row.

### 5.6 Input-layer key-binding conflict matrix (48-D2/D4/D5 vs 45/46)

48 edits the same input layer as specs 45 and 46. This matrix pins, per key, its
owning spec, what 48 changes, and the regression assertion. **No row changes
ownership**; 48 only *adds* bindings or makes an existing one glyph-safe.

| Key / event | Owning spec | 48's change | Regression assertion |
|---|---|---|---|
| `ArrowLeft` / `ArrowRight` | 10 §9.2 (caret), extended by 48-D5 | Byte-wise → glyph-wise via `InputModel::cursor_left`/`cursor_right`, plus a render-time `glyph_floor` snap (48-I8) | Caret never lands inside a multi-byte glyph; crossing `"é"`/`"😀"` is one step, not 2/4 (`src/ui/supervisor.cpp:2577-2588` updated). |
| `ArrowLeftCtrl` / `ArrowRightCtrl` | **48-D4 (new)** | Word-wise motion (`word_left_boundary`/`word_right_boundary`) | Word-boundary table (48-I5–I17); inert when the terminal emits no `CSI 1;5D/C` (48-F5). |
| `ArrowUp` / `ArrowDown` | **45-D1** | **Unchanged by 48.** Command list first (`move_hint_selection`), history otherwise (`history_up`/`history_down`) | `UI45_D1_ArrowPrecedenceListVsHistory`; with the list active, history is untouched; with it inactive, history recalls. |
| `Enter` | **46-D6** | **Unchanged by 48.** First Enter completes a partial command in place; a second Enter executes; an exact name/alias dispatches on the first Enter | Two-press Enter tests: `/e` + Enter completes to `/exit` **without** exiting; second Enter exits; exact `/exit` exits on the first. |
| `Esc` (single) | 45-D5, extended by 48-D2 | Visible command list → dismiss (45-D5 priority retained); otherwise arm the interrupt, second `Esc` cancels | Command-list dismissal still wins (48-I1); `esc_armed` cleared by any handled key / modal / session switch / 3 s (48-I3). |
| `Ctrl+O` | 48-D7 | Toggle fold expansion (retained) | Expanded tool entry shows the full `args:` line and output (48-D7.4). |

**No conflict is introduced:** 48-D4/D5 use keys 45/46 leave free
(`ArrowLeftCtrl`/`ArrowRightCtrl`) or already own as caret movement
(`ArrowLeft`/`ArrowRight`); 45-D1's `ArrowUp`/`ArrowDown` and 46-D6's `Enter` are
untouched. The suite must keep the 45-D1 and 46-D6 assertions green.

---

## 6. D6 — The text-hierarchy styling policy

### 6.1 Current state (verified)

- `render_entry` (`src/ui/ui_render.cpp:263-309`):
  - **User**: `with_left_bar(markdown.render(...))` painted with
    `bgcolor(RGB(40,42,54))` (`:269-274`). No foreground brightening, no bold.
  - **Assistant**: `markdown.render(...)` plain (`:276-278`).
  - **Reasoning**: `render_reasoning_entry` — Magenta header
    (`sign + " Thinking"`) plus `"  ctrl+o to expand"` dim; body only when
    `expand_all_folds` (`:228-243`).
  - **Tool**: `render_tool_entry` — Yellow header; a `notice:` row exists
    (`:193-195`) but is unreachable for a tool entry (no tool result carries a
    `ContextForm::Notice`; see 48-I15); (only when expanded) `"args: " +
    arguments` dim and the output (`:176-212`).
  - **System/Context**: `dim` (`:284`, `:293`).
- There is no notion of "final answer" vs "intermediate assistant text"; all
  assistant entries render identically.

### 6.2 Decision (48-D6) — USER REQUIREMENT

1. **Three presentation levels** (computed per entry, pure function of the
   transcript and the turn-active predicate):

   | Level | Entries | Style |
   |---|---|---|
   | **UserAuthored** | `ConversationRole::User` | foreground `Color::White`, `ftxui::bold`, left bar retained, background `RGB(40,42,54)` retained |
   | **Intermediate** | `Reasoning`, `Tool`, and `Assistant` entries that are **streaming** or followed by a `Tool`/`Reasoning` entry before the next `User` entry | `ftxui::dim` on the whole entry (header and body) |
   | **FinalAnswer** | every other `Assistant` entry | no dim, no bold — the terminal default (normal) |
   | **Chrome** | `System`, `Context` | `dim` (unchanged) |

2. **Classifier (Rev 2 — streaming folded into the signature).**
   `entry_presentation(entries, index, turn_active)`:
   - `User` → `UserAuthored`.
   - `Reasoning`/`Tool` → `Intermediate`.
   - `Assistant`:
     - **streaming:** if `turn_active && index + 1 == entries.size()` (the entry
       is the transcript tail and a turn is running) → `Intermediate`. This is
       the "dim while streaming" rule; when the turn ends, `turn_active` becomes
       false and the same entry reclassifies to `FinalAnswer` (48-F9).
     - otherwise scan forward from `index+1` until a `User` entry; if any
       `Tool` or `Reasoning` entry is seen, → `Intermediate`; else →
       `FinalAnswer`.
   - `System`/`Context` → `Chrome`.

   `turn_active` is the per-session turn predicate:
   `is_active_state(session.agent_state)` (`src/ui/ui_model.cpp:326-328`) — for
   the rendered session this is exactly `UiModel::has_active_turn()`
   (`src/ui/ui_model.cpp:1292-1301`); a background session uses its own
   `agent_state`. **Rev 1 took `agent_state` but never used it, so a streaming
   final assistant classified as `FinalAnswer` (normal), contradicting the dim
   rule; Rev 2 passes the predicate explicitly and uses it.** The scan is O(n)
   amortized if the caller passes a precomputed `std::vector<Presentation>`; the
   renderer computes it once per `render_conversation` call.

3. **Where the style is applied.** A single `apply_presentation(Element, Presentation, theme)`
   helper wraps the entry element, so the policy is in one place and the
   markdown renderer's intra-text styles (bold/italic/code) are preserved
   (`src/ui/render/markdown_renderer.cpp:134-147`). `dim` is a decorator, so it
   composes with markdown styles rather than replacing them.

4. **Streaming (now an input to the classifier, not a post-hoc rule).** While
   an assistant entry is the transcript tail and `turn_active`, it is
   `Intermediate` (dim); when the turn completes it becomes `FinalAnswer`
   (normal) unless a tool/reasoning entry follows it. This gives the user the
   "intermediate dim, final bright" contrast during and after the turn. (The
   user's words: "make it brighter or deem slightly all LLM's intermediate text
   except the final answer.") The predicate is part of the classifier signature
   (item 2), so a streaming final assistant is dim, not normal — the Rev 1
   contradiction is removed.

5. **User brightening.** Add `bold` + `Color::White` to the user block. The
   left bar is painted `Color::Green` (currently the `LeftBar` node draws `│`
   with no color, `src/ui/ui_render.cpp:65-72`); the bar becomes a second visual
   cue. The background is retained (RB-01).

6. **Theme seam.** The three colors (`user_foreground`, `intermediate_dim` is a
   decorator not a color, `bar`) live in `Theme` (`include/ymh/ui/theme.hpp:7-19`)
   so a monochrome terminal (`theme.color == false`) degrades to plain text:
   `paint()`/`paint_bg()` already no-op when `color` is false
   (`src/ui/ui_render.cpp:31-43`), and `dim` is retained in monochrome (it is a
   weight, not a color).

### 6.3 Invariants

- **48-I11.** The presentation level is a pure function of the transcript and
  the turn-active predicate (`turn_active`); it is never stored on
  `ConversationEntry`.
- **48-I12.** Every `Assistant` entry is exactly one of `Intermediate` /
  `FinalAnswer`; the classification is deterministic and replay-stable. A
  streaming tail assistant is `Intermediate`; the same entry after the turn
  ends is `FinalAnswer` unless a tool/reasoning entry follows it.
- **48-I13.** `theme.color == false` disables all color but keeps `dim` and
  `bold`; the hierarchy is still visible in monochrome.
- **48-I14.** User text is never dimmed; intermediate text is never bold.

### 6.4 Failure modes

- **48-F8** (`F7`). A background session's transcript is classified but not
  rendered until focused; the classification is cached per session and
  invalidated by the existing dirty flags.
- **48-F9.** A very long assistant entry with a trailing tool call is dimmed
  while streaming and brightened when the turn ends; this is a repaint, not a
  re-render of content.

### 6.5 Test plan

- **Unit.** `entry_presentation` table (now including `turn_active`):
  user→UserAuthored; reasoning→Intermediate; tool→Intermediate;
  assistant followed by tool→Intermediate; assistant before user→FinalAnswer;
  **tail assistant with `turn_active == true`→Intermediate**; **the same tail
  assistant with `turn_active == false`→FinalAnswer**; system/context→Chrome;
  non-tail assistant with `turn_active == true` but no following tool→FinalAnswer.
- **Golden TUI.** Four entries (user, assistant+tool, final assistant, reasoning)
  and assert the per-entry decorators via the golden renderer.

---

## 7. D7 — Rich one-line tool invocations with truncation

### 7.1 Current state (verified)

- `render_tool_entry` (`src/ui/ui_render.cpp:176-212`) renders
  `"tool: " + entry.tool_name` in Yellow (`:188`, `:192`). When not expanded,
  that single line is the entire output (`:196-198`). The arguments are only
  shown when expanded, as `"args: " + call->arguments` dim (`:199-201`), and the
  raw JSON string is dumped verbatim.
- `ToolCallView` already carries `name`, `arguments`, `output`, `expanded`
  (`include/ymh/ui/ui_model.hpp:135-145`); `ConversationEntry` carries
  `tool_name`/`tool_call_id` (`:90-98`).
- `ellipsize_text(text, max_width)` already exists and appends `…`
  (`src/ui/ui_render.cpp:404-439`), but is only used by the status line.
- The user's target look (from the screenshot): `▸ tool_name  arguments…`, two
  colors, one line, truncated with `…`.

### 7.2 Decision (48-D7) — USER REQUIREMENT

1. **One-line format (pinned):**

   ```text
   ▸ <tool_name>  <argument_summary>            (truncated to the pane width)
   ```

   - Marker `▸` (U+25B8), painted `Color::Yellow`.
   - `<tool_name>` painted `Color::CyanLight` + `bold`.
   - Two spaces, then `<argument_summary>` painted `Color::GrayLight`
     (`ftxui::Color::GrayLight`); when `theme.color == false`, all three are
     plain text.
   - The line is composed as **styled spans** and truncated by
     `truncate_spans(spans, context.content_width)` — a span-aware, width-aware
     truncator (48-D7.3) — so the per-span colors survive truncation and the
     trailing `…` lands on the last visible column of the **content box**.

2. **Argument summary (Rev 8, gate HIGH-3).** A **single** symbol
   `summarize_tool_arguments(std::string_view tool_name,
   std::string_view arguments_json)` lives in `ui_render.hpp`/`.cpp`. The
   adapter's former same-named helper
   (`ui_event_adapter.hpp:96`, a 512-byte bounded `json.dump()` preview) is
   **removed**; it is not overloaded. The composition is pinned:
   - The adapter stores the **raw JSON** in `ToolCallView::arguments` as
     `payload.arguments.dump()` (no pre-summarization, no 512-byte bound). Rev 7
     fed the renderer the *already-summarized* preview, so long commands
     parse-failed and rendered raw truncated JSON.
   - The renderer parses `arguments_json`. On parse failure, return the raw
     string.
   - Prefer, in order: `command` (shell/bash), `path` (read_file/write_file/
     edit_file/glob), `pattern` (grep/glob), `query`, `url`, `name`.
   - For a `shell`/`bash` command, collapse whitespace to single spaces and
     render as-is (the screenshot shows the raw command).
   - Otherwise, if no preferred key matches, render the compact JSON of the
     object.
   - Values are `json.dump()`-unescaped (a string value is rendered without
     surrounding quotes). This is a **display** summary only; the model-facing
     arguments are never altered. The adapter uses the same symbol for a
     permission-request summary by passing `request.arguments.dump()`.

3. **Truncation (Rev 2 — span-aware and width-aware).** Rev 1 applied the
   existing `ellipsize_text` to the composed line. That cannot produce per-span
   colors: `ellipsize_text` is `std::string -> std::string`
   (`src/ui/ui_render.cpp:404-439`), so the Yellow/CyanLight/GrayLight spans
   are lost before truncation and no span-aware truncator was pinned. Rev 2
   pins a span model and a span-aware truncator:

   ```cpp
   struct StyledSpan {
       std::string  text;                 // one or more UTF-8 glyphs
       ftxui::Color color;                // ignored when theme.color == false
       bool         bold = false;
   };
   using StyledLine = std::vector<StyledSpan>;

   // Truncates to `max_width` terminal columns, walking whole UTF-8 glyphs and
   // never splitting one. Per-glyph width is `ftxui::string_width` (pinned), so
   // a double-width CJK glyph counts as two columns. Appends "…" inside the
   // last kept span (or a new GrayLight span when nothing was kept). Returns the
   // spans unchanged when the line already fits. Pure; never throws.
   [[nodiscard]] StyledLine truncate_spans(const StyledLine& line, int max_width);
   ```

   The tool line is built as `StyledLine{{"▸ ", Yellow}, {name, CyanLight,
   bold}, {"  ", default}, {summary, GrayLight}}` and rendered with
   `truncate_spans`; `render_tool_entry` maps the kept spans to FTXUI elements.
   `ellipsize_text` is retained for the status line only.

   **Width is the content box, not the terminal.** Rev 1 passed
   `context.width`, which `render_supervisor` sets to the full terminal width
   (`RenderContext{size.width, …}`, `src/ui/ui_render.cpp:1202-1203`). The
   conversation pane sits inside a `border` and carries a `vscroll_indicator`
   (`src/ui/ui_render.cpp:312-329`), so its content box is narrower. Rev 2 adds
   `RenderContext::content_width` and sets it at the call site to
   `max(1, size.width - 2 /*border*/ - 1 /*vscroll_indicator*/)`; the tool line
   uses `context.content_width`, not `context.width`. A golden test pins that
   the trailing `…` is on the last visible column of the pane (48-I15). The
   screenshot's mid-token truncation (`… c…`) is this behaviour.

4. **Expansion retained.** Ctrl+O (`toggle_folds`) still expands the entry; the
   expanded view shows the full `args:` line and the output/diff (`:193-210`).
   Rev 7 claimed the expanded view "keeps the notice"; that is withdrawn
   (gate HIGH-4): no runtime producer emits a `ContextForm::Notice` on a tool
   result (`src/jobs/job_wakeup.cpp:32,41`;
   `src/agent/repeat_tool_reminder.cpp:67`), so the renderer's `notice:` row
   (`src/ui/ui_render.cpp:193-195`) is a **defensive, unreachable-in-production**
   path (OQ-48-8). The one-line header is always shown, expanded or not; when
   expanded the header is not truncated (the user is inspecting it).

5. **Outcome affordance (optional, pinned shape).** A failed tool entry prefixes
   the marker with `✗` (`Color::Red`) and a truncated-output entry appends a dim
   `" [truncated]"`; these are display-only and derive from
   `ToolCallView::outcome` / `::truncated` (`include/ymh/ui/ui_model.hpp:140-141`).
   Not required by the user; recorded so the renderer has a single owner.

### 7.3 Invariants

- **48-I15.** A collapsed tool entry renders exactly one row (the `▸ …`
  header) and its width never exceeds `context.content_width` (the pane content
  box, not the terminal width). `render_tool_entry`'s `notice:` row
  (`src/ui/ui_render.cpp:193-195`) is unreachable for a tool entry because no
  tool result carries a `ContextForm::Notice` — `ContextForm::Notice` is
  produced only for messages (`src/jobs/job_wakeup.cpp:32,41`;
  `src/agent/repeat_tool_reminder.cpp:67`). If a tool ever emits one, this
  invariant and the collapsed renderer must be revisited (OQ-48-8).
- **48-I16.** Truncation is span-aware: it never splits a UTF-8 glyph, always
  terminates with `…` when it truncates, and preserves each kept span's color
  and bold attribute.
- **48-I17.** The summary is derived from `ToolCallView::arguments` only; the
  model-facing arguments and the durable event log are untouched.
- **48-I18.** The tool name and the argument summary use different colors in a
  color terminal.

### 7.4 Failure modes

- **48-F10.** Malformed `arguments` JSON: render the raw string, truncated. Never
  throw from `Render()` (the pure-render rule).
- **48-F11.** A tool entry with no `ToolCallView` (e.g. a replayed event without
  arguments): render `▸ <name>` with an empty summary.
- **48-F12** (`F5`). A very long argument string: truncated; the full text
  remains available via Ctrl+O and in the event log.

### 7.5 Test plan

- **Unit.** `summarize_tool_arguments`: shell command; `path`; `pattern`; no
  preferred key; malformed JSON; non-string values; whitespace collapse.
- **Unit.** `truncate_spans`: ASCII; multi-byte glyph (a split would be a
  failure); a CJK glyph counted as two columns (gate MEDIUM); exact width;
  width 0/1; a truncation point that falls inside a span (the kept prefix
  retains its color); the `…` inherits the last kept span's color; a line that
  already fits is returned unchanged.
- **Golden TUI.** The three screenshot-like rows (`read_file cfg/debug.py`,
  `bash git diff-tree …`, a truncated `bash for f in …`), asserting the marker,
  the per-span colors, and the trailing `…` on the last visible column of the
  pane content box (not the terminal width).

---

## 8. D8 — A blank line after each reasoning block

### 8.1 Current state (verified)

- `render_conversation` pushes each entry's element with **no separator**
  (`src/ui/ui_render.cpp:318-321`), so a collapsed reasoning header is
  immediately followed by the next entry on the next line.
- `render_reasoning_entry` emits one header row plus, when expanded, the
  markdown body (`:228-243`). There is no trailing blank row.

### 8.2 Decision (48-D8) — USER REQUIREMENT

1. **Trailing separator.** After every `ConversationRole::Reasoning` entry,
   `render_conversation` appends one empty element (`ftxui::text("")`), so the
   next piece is separated by exactly one blank line — whether the reasoning is
   collapsed or expanded.
2. **Idempotent.** If the next entry is itself a reasoning block, the blank line
   still appears (one per block, not deduplicated), matching the user's
   "after each `<think>`".
3. **No separator elsewhere.** User/assistant/tool entries are unchanged; the
   user did not ask for blank lines between them (and tool lines already read as
   a compact block).

### 8.3 Invariants

- **48-I19.** Every `Reasoning` entry is followed by exactly one blank row in the
  rendered transcript.
- **48-I20.** The separator is display-only; it is not a `ConversationEntry` and
  never enters the model or the log.

### 8.4 Failure modes

- **48-F13.** A reasoning entry as the last transcript element: the trailing
  blank row is still emitted (harmless; the viewport is `flex`).
- **48-F14** (`F7`). Background reasoning arriving while scrolled up: the blank
  row is part of the same repaint and does not change `scroll.following`.

### 8.5 Test plan

- **Golden TUI.** Transcript `[reasoning, assistant]` shows one blank line
  between; `[reasoning, reasoning]` shows one after each; `[assistant, tool]`
  shows none.

---

## 9. C++ interface sketches (pinned)

Only new/changed symbols are shown; unchanged members are elided with `…`.
Every sketch pins the headers it needs. All types live in `namespace ymh` unless
noted.

```cpp
// ── include/ymh/ui/ui_model.hpp ────────────────────────────────────────────
#include <chrono>
#include <cstdint>
#include <string>
namespace ymh::ui {

struct InputModel {
    std::string              draft;
    std::size_t              cursor = 0;
    std::vector<std::string> history;
    std::size_t              history_pos = 0;
    std::string              saved_draft;

    void push_history(std::string line);
    bool history_up();
    bool history_down();
    bool delete_forward();
    void clear_line();
    bool delete_word();                              // 48-D4.5: class model

    // 48-D4.1: pure word motion. Never edits the draft.
    [[nodiscard]] std::size_t word_left_boundary(std::size_t cursor) const;
    [[nodiscard]] std::size_t word_right_boundary(std::size_t cursor) const;

    // 48-D5.1 (Rev 2): glyph-wise motion for the plain arrows. Both are pure,
    // total, and clamp to [0, draft.size()]; `cursor_left` never lands on a
    // UTF-8 continuation byte.
    [[nodiscard]] std::size_t cursor_left(std::size_t cursor) const;
    [[nodiscard]] std::size_t cursor_right(std::size_t cursor) const;
};

// 48-D5.1 (Rev 2): render-time glyph snap. Advances `cursor` off any UTF-8
// continuation byte to the next lead byte, clamped to `draft.size()`.
[[nodiscard]] std::size_t glyph_floor(std::string_view draft,
                                      std::size_t cursor) noexcept;

// 48-D5.1 (Rev 8, gate MEDIUM): `glyph_len`/`glyph_at` are pinned because the
// caret sketch uses them. `glyph_len` is the byte length of the UTF-8 glyph at
// `cursor` (1..4), clamped to the end of `text`, 0 when `cursor >= text.size()`.
[[nodiscard]] std::size_t glyph_len(std::string_view text,
                                    std::size_t cursor) noexcept;
[[nodiscard]] std::string_view glyph_at(std::string_view text,
                                        std::size_t cursor) noexcept;

// 48-D2.1: the Esc-Esc arm. Per-session, never persisted.
enum class EscArm : std::uint8_t { Disarmed, Armed };

struct SessionUiState {
    …
    EscArm                                   esc_arm = EscArm::Disarmed;   // NEW
    std::optional<std::chrono::steady_clock::time_point> esc_armed_at;    // NEW
    …
};

// 48-D6.2: the derived styling level. Never stored on ConversationEntry.
enum class Presentation : std::uint8_t {
    UserAuthored,   // bright: White + bold
    Intermediate,   // dim
    FinalAnswer,    // normal
    Chrome,         // dim (System/Context)
};

// 48-D6.2 (Rev 2): `turn_active` is folded into the classifier so a streaming
// tail assistant is Intermediate. `turn_active` =
// `is_active_state(session.agent_state)` for the rendered session.
[[nodiscard]] Presentation entry_presentation(
    const std::vector<ConversationEntry>& entries, std::size_t index,
    bool turn_active) noexcept;                                              // NEW

} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/ui_render.hpp / src/ui/ui_render.cpp ────────────────────
namespace ymh::ui {

// 48-D5.1: the composer with a real caret. `input` is read-only.
[[nodiscard]] ftxui::Element render_input(const UiModel& model,
                                          const Theme& theme);

// 48-D7.2: the one-line argument summary. Pure; never throws.
[[nodiscard]] std::string summarize_tool_arguments(std::string_view tool_name,
                                                   std::string_view arguments_json);

// 48-D6.3: the single styling point.
[[nodiscard]] ftxui::Element apply_presentation(ftxui::Element element,
                                                Presentation presentation,
                                                const Theme& theme);

// 48-D7.3 (Rev 2): the span-aware, width-aware truncator. Pure; never throws.
struct StyledSpan {
    std::string  text;                 // one or more UTF-8 glyphs
    ftxui::Color color;                // ignored when theme.color == false
    bool         bold = false;
};
using StyledLine = std::vector<StyledSpan>;

[[nodiscard]] StyledLine truncate_spans(const StyledLine& line, int max_width);

} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/render/render_context.hpp ───────────────────────────────
namespace ymh::ui {
struct RenderContext {
    int         width = 80;         // full terminal width (unchanged)
    // 48-D7.3 (Rev 8): the conversation pane's content-box width. Set by
    // `build_ui` to `max(1, size.width - 2 /*border*/ - 1 /*vscroll_indicator*/)`;
    // the tool line truncates to THIS, not `width`.
    int         content_width = 80; // NEW
    Theme       theme{};
    bool        compact = false;
    std::size_t spinner_frame = 0;
};
// Gate MEDIUM: inserting `content_width` second breaks every positional
// aggregate initializer (`RenderContext{width, theme, compact, frame}` is a hard
// `Theme`→`int` build error under `-Werror`). All call sites (production
// `build_ui` and the renderer unit tests) use designated initializers.
// The old field order is therefore superseded, not merely extended.
} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/theme.hpp ───────────────────────────────────────────────
namespace ymh::ui {
struct Theme {
    bool          color = true;
    bool          user_block = true;
    ftxui::Color  completion_selected = ftxui::Color::CyanLight;
    // 48-D6.6
    ftxui::Color  user_foreground = ftxui::Color::White;        // NEW
    ftxui::Color  user_bar        = ftxui::Color::Green;        // NEW
    ftxui::Color  tool_name       = ftxui::Color::CyanLight;    // NEW
    ftxui::Color  tool_args       = ftxui::Color::GrayLight;    // NEW
};
} // namespace ymh::ui
```

## 10. Invariants

Numbered `48-I#`. Each is testable or explicitly labelled a **retained/regression
pin**. The "Gate" column is `Y` (gates this change) or `pin` (retained).

| ID | Gate | Invariant |
|---|---|---|
| 48-I1 | Y | Esc-Esc cancels only the active session's turn via `cancelActive()`; it never quits or switches. |
| 48-I2 | Y | The hint string is exactly `- one more <Esc> to interrupt`, visible iff `esc_armed`. |
| 48-I3 | Y | `esc_armed` is per-session, never persisted, and cleared by any handled key, a modal, a session switch, or the 3 s timeout. |
| 48-I4 | Y | With no active turn, Esc never arms and never cancels. |
| 48-I5 | Y | Word motion is pure and total. |
| 48-I6 | Y | `word_left_boundary <= cursor <= word_right_boundary`; both are valid indices. |
| 48-I7 | Y | `Word` and `Punct` runs are atomic; whitespace is never part of a word. |
| 48-I8 | Y | The caret is at `input.cursor` for every valid cursor; the cursor is glyph-aligned (or snapped by `glyph_floor`); no other cursor glyph exists. |
| 48-I9 | pin | Rendering never mutates the model. |
| 48-I10 | Y | The armed hint coexists with the caret and never displaces it. |
| 48-I11 | Y | `Presentation` is a pure function of the transcript + the turn-active predicate; never stored. |
| 48-I12 | Y | Every `Assistant` entry is exactly one of Intermediate/FinalAnswer; replay-stable; a streaming tail assistant is Intermediate. |
| 48-I13 | Y | Monochrome keeps `dim`/`bold`; the hierarchy survives. |
| 48-I14 | Y | User text is never dimmed; intermediate text is never bold. |
| 48-I15 | Y | A collapsed tool entry renders exactly one row (the `▸ …` header), width ≤ `context.content_width`; the `notice:` row is unreachable for a tool entry (no tool result carries a `ContextForm::Notice`). |
| 48-I16 | Y | Truncation is span-aware: never splits a UTF-8 glyph, terminates with `…`, and preserves kept spans' color/bold. |
| 48-I17 | Y | The tool summary derives only from `ToolCallView::arguments`; log/args untouched. |
| 48-I18 | Y | In a color terminal the tool name and argument summary use different colors. |
| 48-I19 | Y | Every reasoning entry is followed by exactly one blank row. |
| 48-I20 | pin | The separator is display-only; never a `ConversationEntry`. |

---

## 11. Failure modes

Component-local tags `48-F#`; each maps to the shared F1–F12 finding where
applicable (`00-architecture.md:4836-4856`).

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 48-F1 | F6 | Esc routed to a modal | Modal handles it; interrupt not armed. |
| 48-F2 | F9 | Turn ends between the two Escs | Second Esc clears the arm; no cancel submitted. |
| 48-F3 | F6 | Terminal emits two Esc bytes for one key | Recorded exposure (OQ-48-4); same as Ctrl+C. |
| 48-F4 | — | Multi-byte/malformed UTF-8 in word scan; cursor glyph alignment | Word scan is byte-aligned/lead-byte classification; malformed → `Punct`. Glyph alignment is 48-D5: arrows use `cursor_left`/`cursor_right` (plain arrows are byte-wise today, `src/ui/supervisor.cpp:2577-2588`) + `glyph_floor` at render. Same as §4.4. |
| 48-F5 | F6 | Terminal does not emit `CSI 1;5D/C` | Ctrl+Arrow inert; plain arrows work. |
| 48-F6 | — | `cursor > draft.size()` or inside a UTF-8 continuation byte | Clamp / `glyph_floor` at render + log once; arrows move glyph-wise. |
| 48-F7 | F6 | Repaint while a dialog is open | Dialog composited on top; caret hidden (correct). |
| 48-F8 | F7 | Background transcript classified but not rendered | Cached per session; invalidated by dirty flags. |
| 48-F9 | — | Streaming tail assistant reclassified at turn end | Classifier input `turn_active` flips; repaint only. |
| 48-F10 | — | Malformed tool `arguments` JSON | Raw string, truncated; never throw in Render. |
| 48-F11 | — | Tool entry without a `ToolCallView` | `▸ <name>` with empty summary. |
| 48-F12 | F5 | Very long tool arguments | Truncated; full text via Ctrl+O and the log. |
| 48-F13 | — | Reasoning entry last in transcript | Trailing blank row still emitted. |
| 48-F14 | F7 | Background reasoning while scrolled up | Same repaint; `scroll.following` unchanged. |

---

## 12. dsh (DeepSeek Harness) mapping

| dsh concept | ymh realization (48) |
|---|---|
| Interrupt / cancel | A two-key confirm over the existing session-scoped `agent.cancel`; the model never sees an "armed" state. |
| Input editing | A caret + word motion on a byte-index cursor; the composer remains the single input owner (F6). |
| Transcript projection | A pure presentation classifier over the event projection; the log is untouched. |
| Tool-call display | A one-line projection of the assembled tool call; full fidelity stays in the log/expanded view. |

---

## 13. Test plan

### 13.1 Unit (hermetic, no LLM, no daemon)

- **48-D2:** `EscArm` transition table; cancel submitted exactly once; timeout;
  no-turn; command-list priority.
- **48-D4:** `word_left_boundary`/`word_right_boundary` table; `delete_word`
  class model.
- **48-D5:** `glyph_floor` + `glyph_len`/`glyph_at` + `cursor_left`/
  `cursor_right` table (ASCII, 2/3/4-byte glyphs, cursor inside a continuation
  byte, end-of-line); `render_input` caret at cursor 0/mid/end.
- **48-D6:** `entry_presentation` table including `turn_active` true/false for the
  tail assistant.
- **48-D7:** `summarize_tool_arguments` table; `truncate_spans` table (span-aware).

### 13.2 Golden TUI render

- Composer caret at cursor 0/mid/end; armed hint row. The caret golden asserts
  `Screen::cursor().shape == Bar` and the exact x column, and a CJK draft asserts
  the leading-cell caret (gate HIGH-1 / MEDIUM).
- Tool lines: `read_file cfg/debug.py`, `bash git diff-tree …`, and a truncated
  `bash for f in …` with the trailing `…` on the last visible column of the
  pane content box (not the terminal width); the per-span colors survive the
  truncation. A collapsed tool entry renders **exactly one row** (48-I15).
- Styling hierarchy: user bold/white + green bar; intermediate dim; final
  normal; a streaming tail assistant is dim and brightens when the turn ends.
- Reasoning trailing blank line (`[reasoning, assistant]`).

Golden tests follow §44 and the existing golden harness (spec 45 §17.2).

### 13.3 Integration (FakeLLM / fake daemon)

- First Esc emits no `agent.cancel`; second Esc emits exactly one for the active
  session.

### 13.4 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- **Withdrawn (Rev 8, gate HIGH-2).** Rev 7 pinned a Ctrl+C relaunch /
  empty-transcript live test here, but that is the **fresh-launch session
  guarantee owned by `50-D2`** and is out of scope for 48; it is removed. The
  Esc-Esc path is covered hermetically by §13.3 (the real `handle_event` path
  through the supervisor harness), and a PTY-level Esc-Esc live test is optional
  and not required by this spec.

### 13.5 Invariant / failure-mode coverage

Every `48-I#` has at least one unit or integration assertion; every `48-F#` is
exercised by at least one negative test. The matrix is completed in the
implementation plan, not this spec.

---

## 14. Open questions / interpretations

- **OQ-48-4 (double-Esc terminal bytes).** Some multiplexers coalesce a single
  Esc into two bytes. The arm requires separate FTXUI events; if this proves
  noisy, a minimum inter-Esc interval can be added. Recorded, not solved.
- **OQ-48-6 (item 6 "final answer").** The classifier treats an assistant entry
  followed by a tool/reasoning entry as intermediate. A model that emits a
  summary, then a tool call, then another summary would dim the first summary.
  This is the intended reading of "intermediate vs final". If the user prefers
  "only the last assistant entry is final", that is a one-line change in
  `entry_presentation`. **Interpretation chosen: tool/reasoning-followed.**
- **OQ-48-8 (collapsed tool entry and the `notice:` row).** 48-I15 pins a
  collapsed tool entry to exactly one row; `render_tool_entry` nonetheless has a
  `notice:` row (`src/ui/ui_render.cpp:193-195`) that would add a second row if a
  tool result ever carried a `ContextForm::Notice`. No producer exists today
  (Notices are message-only: `src/jobs/job_wakeup.cpp:32,41`;
  `src/agent/repeat_tool_reminder.cpp:67`). If a future tool emits one, revisit
  48-I15 and the collapsed renderer. **Recorded, not solved.**

---

## 15. Revision log

- **Rev 1–Rev 6 (superseded history).** The original 48 covered nine items
  (`48-D1 … 48-D10`) and went through six revisions: D3's Ctrl+C re-entry
  diagnosis was demoted to a hypothesis; item 9's MCP 401 was investigated,
  refuted arm-by-arm, and finally **resolved as an expired credential**; D10
  (MCP 401/403 hints) was designed; D9.7 (MCP global-layer-only) and the
  glyph-safe caret / span-aware truncator / turn-active classifier were pinned.
  That history now lives in `docs/design/50-skills-mcp-and-session-lifecycle-errata.md`
  (skills, session lifecycle, MCP) and in the git history of this file.
- **Rev 7 (split; D10 dropped).** By user decision:
  - **48-D10 (MCP 401/403 hints) is DROPPED ENTIRELY.** Rationale: the
    underlying problem turned out to be an **expired credential, not a harness
    defect**, so the feature is not justified. The durable value was the
    **diagnostic procedure** (the `jq`/`curl` verification block), which is
    **preserved in spec 50** with the MCP content. Removing D10 also removes the
    four gate findings it caused: (i) the unreadable `error.what()` / wrong
    pinned site in `src/mcp/mcp_tool.cpp`; (ii) the "collapsed entry is exactly one line" invariant's contradiction with the `notice:` row — now reconciled (48-I15); (iii) the "classification never alters the durable event log" claim, now vacuously true because no hint is persisted; and (iv) an unreachable failure-mode row.
  - **The spec is SPLIT by risk.** 48 now keeps only the implementation-ready
    TUI/rendering slice — items 2, 4, 5, 6, 7, 8 — and is self-contained
    (invariants 48-I1–I20, failure modes 48-F1–F14). Everything else moved to
    `docs/design/50-skills-mcp-and-session-lifecycle-errata.md`: item 1
    (skills/commands discovery), item 3 (the fresh-launch session guarantee,
    reconciled with spec 49's lazy spawn), the MCP child environment fix
    (48-D9.1), and the MCP global-layer-only guard (48-D9.7).
  - **Section numbering** was rebased for the kept decisions (D2→§3, D4→§4,
    D5→§5, D6→§6, D7→§7, D8→§8); invariants and failure modes were renumbered
    contiguously; no dangling cross-references remain.
  - **The `notice:` wording** in §6.1 (D6 current state) and §13.2 was reconciled
    with the actual renderer (48-I15/OQ-48-8).

  **Verification status: DRAFT — not yet reviewed.** No implementation may begin
  until an independent gate marks this spec `verified` (AGENTS.md, the rule).

- **Rev 8 (gate resolution + implementation).** An adversarial gate returned
  4 HIGH / 11 MEDIUM / 9 LOW. Resolutions:
  - **HIGH-1 (caret).** The `focusCursorBar` sketch could not work: the
    conversation's `focusPositionRelative` owns FTXUI's single focus slot
    (`VBox::ComputeRequirement` keeps the first enabled focus), so the composer
    cursor stayed `Hidden`. §5.2 now pins a `caret_anchor` decorator that sets
    `focused.component_active = true` (winning `Focused::Prefer`) and pins
    `box_` to the glyph's leading cell. A golden test asserts
    `Screen::cursor().shape == Bar` and the exact x column.
  - **HIGH-2 (out-of-scope live test).** §13.4's Ctrl+C relaunch test is the
    `50-D2` fresh-launch guarantee; it is withdrawn and the Esc-Esc path is
    covered hermetically.
  - **HIGH-3 (symbol collision / wrong input).** The renderer's
    `summarize_tool_arguments(std::string_view, std::string_view)` is the single
    symbol; the adapter's same-named 512-byte preview helper is removed and
    `ToolCallView::arguments` holds the raw `arguments.dump()` JSON. §7.2.2 pins
    the composition.
  - **HIGH-4 (Notice claim).** §7.2.4 no longer claims a tool entry carries a
    `ContextForm::Notice`; the row is documented as a defensive,
    unreachable-in-production path (OQ-48-8).
  - **MEDIUMs.** Word motion is pinned as `InputModel` members in §4.2/§4.5/§9;
    `glyph_len`/`glyph_at` are pinned in §9; the false "first cell" claim is
    corrected and a CJK caret test added; `RenderContext` call sites use
    designated initializers; `truncate_spans` pins `ftxui::string_width` and has
    a CJK truncation test.
  - **Implemented and verified.** The full suite is green; new tests cover the
    Esc arm/cancel/timeout/command-list priority, word and glyph motion, the
    caret cursor and CJK leading cell, the presentation table and styles,
    `truncate_spans`, `summarize_tool_arguments`, the rich tool line and its
    content-box truncation, and the reasoning blank-line separator. An
    independent re-gate is still required before the spec is marked `verified`.

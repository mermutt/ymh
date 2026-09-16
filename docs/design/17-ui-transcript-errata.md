# 17 — UI Transcript & Session-Chrome Errata (RB-01/02/08/10/11)

```
Status: written · verified: — · reviewer: —
Component: 17 (errata) — amends 10-supervisor-tui.md by reference only
Depends on: 10-supervisor-tui.md (verified), 11-m2-errata.md,
            16-daemon-ownership.md §3.3/§3.6/§7.6, UI_SURFACE_INVENTORY.md (C1–C5)
Scope: five P0 supervisor-UI changes (RB-01, RB-02, RB-08, RB-10, RB-11)
       on the live `ui::run_supervisor` path
```

## 1. Purpose, scope, and precedence

This errata amends the **verified** spec `10-supervisor-tui.md` for five P0 UI
changes. It is **additive**: it pins new text, interface names, invariants, and
tests, and introduces **no new semantics beyond the five items below**. It does
not rewrite spec 10; each item names the exact §/line it supersedes.

**Live vs. dead path (pinned).** `ui::run_supervisor` (`src/ui/supervisor.cpp:806`,
called from `src/cli/cli.cpp:397`) is live. `ui::run_tui`
(`src/ui/ui_application.cpp:444`) is **dead code** — no caller in `src/`. All five
items change the live path only. `src/ui/ui_render.cpp` is **shared** by both paths
and by the golden/PTY tests, so every renderer change is also what the tests see.

**Precedence (pinned).** This errata wins over spec 10 for the five items. Where it
meets spec 16 on cross-supervisor visibility, **16 wins** (§8). Spec 11 is
untouched.

**Gate (per `AGENTS.md`).** Independent Oracle review must mark this `verified`
with no open HIGH/MEDIUM findings **before any code**. Nothing here is verified.

**Honesty note — recon vs. code.** Every `file:line` was re-read. Two recon claims
were corrected; the code is authoritative:

- RB-11: the recon implied the bottom line already renders counts-only. In fact
  spec 10 §6.1 (`10-supervisor-tui.md:762-773`) **still diagrams a per-session list
  row** (`:768`), and `render_session_bar` (`src/ui/ui_render.cpp:212-236`) renders
  it. RB-11 therefore amends §6.1 and the §8.1 hierarchy (`:921`), not only code.
- RB-08: spec 10 §9.2 (`10-supervisor-tui.md:1060`) **already lists "Tab
  completion"**; RB-08 implements an existing binding and pins only the cycle
  state/reset. All other cited lines matched as stated.

## 2. Amendment register

| ID | Amended clause | Verified code anchors | New behaviour |
|---|---|---|---|
| RB-01 | 10 §8.2 (`:937-972`); §8.1 (`:908-932`) | `ui_render.cpp:132,136-138,19-24`; `theme.hpp:5-7` | delete role labels; user body = bg + left bar |
| RB-02 | 10 §8.2 (`:967-972`); §5.1 (`:632-673`) | `ui_event_adapter.cpp:90-101`; `ui_event.hpp:84`; `events.hpp:91-94`; `ui_model.hpp:126`; `ui_model.cpp:398-405`; `ui_render.cpp:89,99-121,125`; `supervisor.cpp:517-525,725-728` | capture reasoning; collapsed = one line; Ctrl+O = expand-all |
| RB-08 | 10 §9.2 (`:1044-1062`); §9.3 (`:1064-1070`) | `supervisor.cpp:591-594,609-696,619-631,188-211,467-480`; `command_registry.hpp:37`; `command_registry.cpp:60-69`; `ui_model.hpp:136-149,196`; `ui_render.cpp:198-210` | Tab completes a bare `/prefix` |
| RB-10 | 10 §8.1 (`:913`); §4.4 (`:418-444`) | `ui_render.cpp:364-372,79-81,219`; `ui_model.hpp:203,319`; `ui_model.cpp:294-296,328-346`; `supervisor.cpp:280-329` (`:295`,`:316`); `supervisor_connection.hpp:123-124`; `host_runtime.cpp:142`; `protocol_server.cpp:374-379`; `supervisor.cpp:347` | session name right-aligned in header |
| RB-11 | 10 §6.1 (`:758-785`); §8.1 (`:921`); **§3.5 (`:275-320`); §11.3 (`:1159-1170`); F2 (`:1319`); U-F13 (`:1355`); §4.4 comment (`:425`)** | `ui_render.cpp:212-236,405,255-284,413,344,352`; `ui_model.hpp:257-262`; `ui_model.cpp:225-241` | bottom line drops the per-session list; badge becomes switcher-only |

---

## 3. RB-01 — Transcript styling (drop role labels; user block)

**Amended clause.** `10-supervisor-tui.md` §8.2 (`:937-972`) and §8.1 `MessageView`
(`:908-932`). Spec 10 never pins role labels; the "old text" is code behaviour the
spec silently allows.

**Old text (code, `src/ui/ui_render.cpp`).** `:132` emits a `"you:"` row
(`paint(text("you:"), Cyan, theme) | bold`); `:136-138` emits `"assistant"` /
`"assistant (streaming)"`. `:19-24` is the only helper, `paint(Element, Color,
const Theme&)` — **fg-only**, gated on `theme.color`. `include/ymh/ui/theme.hpp:5-7`
has exactly `bool color`. There is **no** `bgcolor` call and **no** vertical-bar
helper anywhere in `src/ui/` or `include/ymh/ui/` (verified by search).

**New text (pinned).**

1. Delete both label rows; `render_entry` emits no `"you:"` and no `"assistant"`.
2. A **user** body gets (a) a subtle background and (b) a left vertical bar (`│ `)
   on **every line**. The body is the existing `MarkdownRenderer` output, so the
   gutter wraps rendered markdown, not raw text.
3. **Assistant** output stays on the normal background, no gutter.
4. Add `Theme::user_block` (`bool`, default `true`). Its purpose is an
   **independent user/theme opt-out of the background on a color terminal**; the
   left bar is unconditional and is not affected by it. `color == false` already
   suppresses all color, so the background is absent on monochrome regardless of
   `user_block` (see F-14 in §11).
5. Pin two new **file-local** helpers beside `paint()` (`ui_render.cpp:19-24`,
   anonymous namespace, not public API):
   - `paint_bg(Element, ftxui::Color, const Theme&)` — applies `ftxui::bgcolor`
     only when `theme.color && theme.user_block`.
   - `with_left_bar(Element)` — draws the `│ ` gutter on **every rendered line**.
     FTXUI exposes only all-sides `border*` / uniform `borderWith(const Pixel&)`
     (`build/_deps/ftxui-src/include/ftxui/dom/elements.hpp:71-81`); there is **no
     per-line/left-only primitive**. Pin the mechanism as a small custom
     `ftxui::Node` (subclass) that adds 2 columns of width in
     `ComputeRequirement()` and writes the bar in the first column of every row in
     `Render()`. No fallback that loses the per-line bar is acceptable (it would
     break U-RB01-2).

**Rationale.** Labels cost a row per message and duplicate block-shape
information; bg + gutter is the standard transcript affordance and degrades to a
glyph. Assistant stays unadorned so markdown/diff rendering is unchanged.

**Interfaces touched.** `Theme` (+`user_block`); `render_entry`; file-local
`paint_bg`, `with_left_bar` (custom `Node`). `render_to_ansi`/`build_ui`
signatures unchanged.

**Invariants.**
- **U-RB01-1** — No entry renders a role label; the role distinction is styling-only.
- **U-RB01-2** — The user body always carries the left bar; with `user_block ==
  false` (or `color == false`) it has no background but remains distinguishable
  from assistant output.
- **U-RB01-3** — `Render()` stays pure: styling is a function of model + `Theme`
  (U3, `§20.12`).

**Failure modes.** No new `F#`. Degradation is the §6.5 capability path; the bar
still renders when `bgcolor` is unsupported (U-RB01-2).

**Test plan.** Unit: each role emits no label row. Golden: new snapshot asserting
the `│ ` gutter on every user line and none on assistant lines. Monochrome golden:
`Theme{color=false, user_block=false}` keeps the bar, no background. Color golden:
`Theme{color=true, user_block=false}` keeps the bar, no background. PTY: `/help`
output shows no `you:`/`assistant` labels.

**Out of scope.** Gutter glyph customization, per-role palette, markdown/diff
renderers.

---

## 4. RB-02 — Fold reasoning + tool output; Ctrl+O expands

**Amended clause.** `10-supervisor-tui.md` §8.2 (`:967-972`, "Tool output is
collapsed by default and expandable") and §5.1 `UiEvent` (`:632-673`).

**Old text (code).**

1. `src/ui/ui_event_adapter.cpp:90-101` — `if (payload.kind ==
   payload::AssistantChunkKind::Reasoning || payload.text.empty()) break;`.
   Reasoning is **dropped**. `AssistantTextDelta.reasoning` (`ui_event.hpp:84`)
   exists but is always `false` (adapter passes `false` at `:100`). Core kind enum
   `payload::AssistantChunkKind{Text, Reasoning}` (`include/ymh/session/events.hpp:91-94`);
   the stream is produced by `ChunkCoalescer::onReasoning`
   (`src/agent/chunk_coalescer.cpp:24-26`).
2. `src/ui/ui_render.cpp:99-121` — `ToolCallView.expanded` (`ui_model.hpp:126`)
   only toggles a `" (expanded)"` suffix (`:102-104`), args (`:106-108`) and `dim`
   (`:116-118`); a **collapsed tool still renders its body** (`:109-121`). Header is
   `"tool: " + name` at `:101`.
3. `src/ui/supervisor.cpp:517-525` — `toggle_last_tool()` flips only
   `tools.calls.back().expanded`; dispatched from Ctrl+O at `:725-728`.
4. `src/ui/ui_model.cpp:398-405` — `AssistantMessageStarted` **creates** the
   Assistant entry immediately, so a reasoning-first stream would order
   `[Assistant(empty), Reasoning]`.

**New text (pinned).**

1. **Capture reasoning.** Stop dropping `AssistantChunkKind::Reasoning`: emit
   `AssistantTextDelta{session, message, text, reasoning=true}`, and emit
   `AssistantMessageStarted` first when the message has not started (reasoning can
   precede text). Route a reasoning delta into a **folded `Reasoning` entry**: add
   `ConversationRole::Reasoning` (`ui_model.hpp:72-77`); add a parallel per-message
   index to `ConversationModel` (type name: `by_reasoning_message`) so all
   reasoning deltas of one message coalesce into one entry; handle
   `reasoning == true` in the `AssistantTextDelta` case of `UiModel::apply`.
   `flatten_content` (`ui_event_adapter.cpp:14-26`) already filters to `Text`, so
   `AssistantMessageFinished` cannot leak reasoning.
2. **Reasoning-before-answer ordering (pinned).** On the **first** reasoning delta
   of a message, if an Assistant entry for that message already exists — **empty
   or non-empty** — insert the Reasoning entry **immediately before** it and
   reindex `by_message` (the shifted Assistant entry). If no Assistant entry
   exists yet, append the Reasoning entry (it will precede the Assistant entry
   created by the later first text delta). Do not change
   `AssistantMessageStarted`'s create-on-start behaviour (verified clean). The
   non-empty case is **reachable**: `handle_delta`
   (`src/llm/openai_adapter.cpp:432-450`) emits content (`:433-438`) before
   reasoning (`:440-450`) within one delta, and `ui_model.cpp:417`
   (`text += e.text`) then makes the Assistant entry non-empty. Result is always
   `[…, Reasoning, Assistant, …]`, for **both** arrival orders.
3. **Finish clears both.** On `AssistantMessageFinished`, set `streaming = false`
   on the Assistant entry **and** on the message's Reasoning entry (found via
   `by_reasoning_message`), if present.
4. **Collapsed = one line, no body; exact strings (pinned).** A collapsed foldable
   entry renders exactly one line:
   - tool, collapsed: `tool: <name>` (no marker, no args, no body);
   - tool, expanded: `tool: <name> (expanded)` (unchanged);
   - reasoning, collapsed, not streaming: `reasoning (Ctrl+O to expand)`;
   - reasoning, collapsed, streaming: `reasoning (streaming · Ctrl+O to expand)`;
   - reasoning, expanded, not streaming: `reasoning (expanded)`;
   - reasoning, expanded, streaming: `reasoning (expanded · streaming)`.
   Expanded reasoning renders its body after the header. (The previous
   "outcome/truncated marker" is **dropped** — it was new content not required by
   RB-02 and its glyph was unpinned; see F-10 in §11.)
5. **Ctrl+O semantics (pinned decision): global expand-all / collapse-all.** Add
   `bool expand_all_folds = false` to `SessionUiState` (`ui_model.hpp:185-199`);
   replace `toggle_last_tool()` (`supervisor.cpp:517-525`) with `toggle_folds()`
   flipping it; the Ctrl+O dispatch (`:725-728`) calls it. **Effective expansion =
   `session.expand_all_folds || entry-specific expanded`**; the existing
   `ToolCallView.expanded` is retained as the per-entry flag and is never cleared by
   the global toggle.

   **Justification.** With one key and no mouse, only expand-all lets a user reach
   an **older** entry; a "last foldable" toggle leaves older entries permanently
   collapsed unless a fold cursor/selection model is invented — **new semantics
   beyond this errata**. Expand-all is a pure presentation boolean, per-session
   (respects U9/U11), and bounded by the existing caps (U13/F5; only the active
   session renders). **Expanding an older entry = Ctrl+O once, then scroll.**
6. **How the flag reaches the renderer (pinned).** `render_entry` and
   `render_tool_entry` currently take no `SessionUiState` (`ui_render.cpp:89,125`)
   and `RenderContext` (`render_context.hpp:7-11`) has no such field. Pin the
   **parameter** option (not a `RenderContext` widening, so markdown/diff renderers
   are untouched):
   - `render_entry(const ConversationEntry&, const ToolModel*, bool expand_all_folds, const RenderContext&)`;
   - `render_tool_entry(const ConversationEntry&, const ToolModel*, bool expand_all_folds, const RenderContext&)`;
   - `render_conversation` (`ui_render.cpp:150`) reads `active->expand_all_folds`
     and passes it down.
7. **Mouse out of scope.** `TerminalCapabilities::mouse` defaults `false`
   (`include/ymh/ui/terminal_layer.hpp:16`) and there is **no `Event::Mouse`
   routing** in `src/ui/` (verified). Click-to-expand is out of scope; if added it
   must define its own routing seam.

**Rationale.** Reasoning is a first-class stream (headless prints it) but invisible
in the TUI; folding it by default matches tool output. A session-wide Ctrl+O is the
minimal change usable with a single key.

**Interfaces touched.** `ConversationRole` (+`Reasoning`); `ConversationModel`
(+`by_reasoning_message`); `SessionUiState` (+`expand_all_folds`);
`UiEventAdapter::adapt`; `UiModel::apply`; `render_entry`/`render_tool_entry`
(+`bool expand_all_folds`); `render_conversation`;
`SupervisorApp::toggle_folds` (replaces `toggle_last_tool`).

**Invariants.**
- **U-RB02-1** — No reasoning chunk is dropped; each reaches the model as a folded
  `Reasoning` entry ordered before the Assistant entry of the same message,
  **regardless of whether reasoning or text arrived first** (the first reasoning
  delta inserts before the Assistant entry even when that entry is non-empty).
- **U-RB02-2** — A collapsed foldable entry renders exactly one line and no body.
- **U-RB02-3** — Effective expansion = `expand_all_folds || per-entry expanded`;
  the global toggle never mutates the per-entry flag.
- **U-RB02-4** — Folding is presentation-only: it never gates/starts/suspends/
  cancels work and never touches the event log (binds U2, U3, U7).

**Failure modes.** **F5/U13** — bounded buffers still apply; expand-all must not
retain unbounded output. **F12/U6** — the toggle is not a clock edge; it must not
arm flash or run in `Render()`.

**Test plan.** Unit: adapter maps a `Reasoning` chunk to
`AssistantTextDelta{reasoning=true}` (not dropped); `UiModel::apply` coalesces two
reasoning deltas into one `Reasoning` entry ordered before the Assistant entry for
**both** arrival orders — (i) reasoning-then-text and (ii) **text-then-reasoning**
(emit `AssistantMessageStarted`, a text delta, then a reasoning delta; assert
`[Reasoning, Assistant]` and the assistant text intact); `AssistantMessageFinished`
clears both `streaming` flags. Golden: collapsed tool shows only `tool: <name>`;
collapsed reasoning only its exact summary string; `expand_all_folds = true` shows
all bodies. PTY: prompt producing reasoning shows the summary, Ctrl+O shows the
body.

**Out of scope.** Mouse click-to-expand; fold cursor/selection; per-entry expansion
persistence across session switches.

---

## 5. RB-08 — `<Tab>` completes slash commands in the input

**Amended clause.** `10-supervisor-tui.md` §9.2 (`:1044-1062`; "Tab completion"
already listed at `:1060`) and §9.3 (`:1064-1070`).

**Old text (code).** Tab is bound **only in the switcher** (`supervisor.cpp:591-594`,
`toggleExpand`). `handle_input` (`supervisor.cpp:609-696`) has **no** Tab case, so
Tab falls through to `return false`. Machinery exists: `CommandRegistry::complete`
(`command_registry.hpp:37`, impl `command_registry.cpp:60-69`), live hints via
`refresh_hints` (`supervisor.cpp:467-480`) → `SessionUiState::command_hints`
(`ui_model.hpp:196`) → `render_command_hints` (`ui_render.cpp:198-210`).

**New text (pinned).**

1. Add an `Event::Tab` case to `handle_input`. Over a **bare `/prefix`** (leading
   `/`, no whitespace after), with `matches = complete(prefix)`:
   - **0 matches** → no-op (draft unchanged), return `false`.
   - **1 match** → `draft = "/" + name + " "` (**trailing space**),
     `cursor = draft.size()`, clear the cycle, clear `command_hints`, return `true`.
   - **N > 1, no active cycle for the current draft** → `draft = "/" +
     longest_common_prefix(matches)`, `cursor = draft.size()`, store
     `cycle = {draft, names, index = 0}`, `command_hints = matches`, return `true`.
     (If the draft is already the common prefix, this leaves it unchanged and only
     arms the cycle; the next Tab steps.)
   - **N > 1, active cycle whose stored `draft` equals the current draft** →
     `draft = "/" + names[index]` (**NO trailing space**), `cursor = draft.size()`,
     `index = (index + 1) % N`, update `cycle.draft = draft`,
     `command_hints = matches`, return `true`.
   A trailing space on a cycle step would make the next Tab a no-op and kill
   cycling; it is therefore forbidden on multi-match steps and used only for the
   unique-match terminal completion.
2. **Cycle state (pinned).** Add to `InputModel` (`ui_model.hpp:136-149`) a field
   (type name: `CompletionCycle`) with exactly these members:
   `std::string draft;` (the draft the cycle was computed for),
   `std::vector<std::string> names;` (match names, in registry order), and
   `std::size_t index = 0;`. Names (not `Command*`) are stored so the model stays
   pure and pointer-free. The field is per-session (binds U9).
3. **Reset condition (pinned).** Any draft mutation clears `CompletionCycle`:
   character insert (`:688-693`), Backspace (`:633-640`), Delete (`:642-647`),
   Ctrl+U (`:675-679`), Ctrl+W (`:681-686`), history recall (`:661-673`), **and
   submit/Return** — both the `Return` command/submit branch (`:619-631`) and
   `SupervisorApp::submit` (`:188-211`, which clears the draft). `refresh_hints`
   stays the single source for the visible hint list.
4. **Generic over the registry (pinned).** Uses only `CommandRegistry::complete`;
   no hard-coded command names, so spec-16 commands registered in `builtin()`
   appear automatically (inventory C5/Surface 5).
5. **Longest-common-prefix helper (pinned).** Add to `CommandRegistry`:
   `static std::string longest_common_prefix(const std::vector<const Command*>&);`
   returning `""` for an empty list. No other registry signature changes.
6. When the draft is not a bare `/prefix` (empty, not `/`-leading, or with
   whitespace), Tab is a **no-op** and returns `false` (preserves U8 focus rules;
   the switcher Tab is unaffected; mode precedence is `handle_event:703-708`).

**Rationale.** The binding is already specified (`:1060`); the gap is wiring.
Pinning cycle state/reset and the exact step output makes it deterministic; the
registry-only dependency keeps it extensible for spec 16.

**Interfaces touched.** `InputModel` (+`CompletionCycle`); `CommandRegistry`
(+static `longest_common_prefix`); `SupervisorApp::handle_input` (+Tab case);
`refresh_hints` reused.

**Invariants.**
- **U-RB08-1** — Tab completes **only** a bare `/prefix`; no-op otherwise.
- **U-RB08-2** — The cycle resets on any draft mutation (including submit); a
  unique match terminates with a trailing space, a multi-match step does not.
- **U-RB08-3** — Completion depends only on `CommandRegistry::complete`; no
  hard-coded names.
- **U-RB08-4** — Tab respects focus ownership: with an overlay open the input
  receives nothing (U8).

**Failure modes.** **F6/U8** — the existing mode checks (`supervisor.cpp:703-708`)
keep Tab from reaching the input while an overlay owns focus. No new `F#`: zero
matches leaves the draft unchanged.

**Test plan.** Unit: `InputModel` cycle reset on edit and on submit;
`longest_common_prefix` over 0/1/N matches. Model: `complete("he")` → `/help`;
a multi-match prefix: first Tab = common prefix (index 0), second Tab = `names[0]`
with **no** trailing space, third Tab = `names[1]`. Golden: `command_hints` still
render after Tab. PTY (extends `HelpListAndHistoryRecall`,
`ui_supervisor_pty_test.cpp:344-389`): type `/he`, Tab, assert `/help `; type `/`,
Tab twice, assert cycling with no trailing space.

**Out of scope.** Non-command completion, argument/flag completion, fuzzy
matching, a popup menu.

---

## 6. RB-10 — Current session name in the header, right-aligned

**Amended clause.** `10-supervisor-tui.md` §8.1 header (`:913`) and §4.4
`SessionCell` (`:418-444`, `title` at `:429`).

**Old text (code).** `render_header` (`ui_render.cpp:364-372`) renders
`"ymh · <cwd>"` then `ftxui::filler()`; the right side is **empty**. The active
session uses the same lookup as elsewhere (`:387-394`, mirrored at `:239-246`).
`SessionCell.title` exists (`ui_model.hpp:203`) but is **never populated**:
`refresh_sessions` (`supervisor.cpp:280-329`) reads `entry.value("title", …)` at
`:295` then discards it — `(void)title` at `:301` and `:311`. The title source is
`SessionHeader.title` → `SessionSummary.title` (`src/host/host_runtime.cpp:142`,
served by `session.list`, `src/transport/protocol_server.cpp:374-379`).
Placeholder titles are `"main"` (`ui_application.cpp:476`, dead path) and `"tui"`
(`supervisor.cpp:347`, live). `short_id` (`ui_render.cpp:79-81`) already exists,
used at `:219`.

**New text (pinned).**

1. `render_header` renders the active session's title **after the filler**,
   right-aligned, using the same lookup as `build_ui` (`:387-394`).
2. Fallback: when the cell title is empty, render `short_id(id)` (mirroring
   `:219`). The right slot is **never empty** while a session is active.
3. **Populate `SessionCell.title` (critical gap), thread-correct (F-01/F-11).**
   The title is captured at `:295` (already present in the `sessions` vector). The
   assignment must happen **on the UI thread inside the `enqueue` lambda
   (`:305-327`), after `model_.ensureCellIn(workspace, session)` at `:316`** — the
   cell does not exist before then. Delete the `(void)title` at `:301` and the
   discard at `:311`; the `:301` loop only calls `track(session)` (no model write).
   The submit reply lambda runs on the pump thread
   (`supervisor_connection.hpp:123-124`), so **no `UiModel` mutation may occur
   there** (U14, spec 10 §3.3 `:256-257`). Pin the mechanism as an additive model
   setter:
   - `UiModel::setCellTitle(const WorkspaceId&, const SessionId&, std::string)`
     (`ui_model.hpp`; there is currently **no** title setter — `refreshCellIn`,
     `ui_model.cpp:328-346`, sets `state`/`attention`/`unread` only).
   Order inside `enqueue`: `ensureSessionIn` → `ensureCellIn` → `setCellTitle`.
4. Empty/placeholder (pinned, F-13). `SessionCell` is constructed with an **empty**
   title (`ui_model.cpp:294-296`). If `session.list` returns an empty title, the
   cell stays empty and the header falls back to `short_id`. The live create path
   sends `"tui"` (`supervisor.cpp:347`), which the daemon stores and returns via
   `session.list`; until a rename, the header therefore normally shows `tui`. The
   create parameter is not itself a cell default.
5. Independent of spec 16: the right slot belongs to RB-10 (inventory C1). A
   spec-16 peer/ownership indicator composes in the **left** segment or switcher
   (`16 §3.6`, C1).

**Rationale.** The session name is the primary orientation cue once multiple
sessions exist, and populating `SessionCell.title` also fixes the dead field the
switcher reads.

**Interfaces touched.** `render_header`; `SupervisorApp::refresh_sessions` (title
captured at `:295`, assigned in `enqueue`); `UiModel` (+`setCellTitle`).

**Invariants.**
- **U-RB10-1** — With an active session the header right slot shows the cell title,
  falling back to `short_id` when empty; never blank.
- **U-RB10-2** — `SessionCell.title` is populated from `session.list` and is the
  single source for the header and the switcher leaf.
- **U-RB10-3** — The title is written to the model **only** inside the UI-thread
  `enqueue` lambda, after `ensureCellIn`; the submit reply lambda performs no model
  mutation (U14, §3.3).

**Failure modes.** No new `F#`. If the active session has no cell (transient), the
header falls back to the short id; renderer stays pure (U3).

**Test plan.** Unit: `refresh_sessions` populates the cell title on the UI thread
(fake host reply with a title), and the reply lambda itself leaves the model
untouched. Golden: header shows the title right-aligned; empty-title shows the
short id. PTY: after auto-create, the header shows the session name (`tui`), no
empty right side.

**Out of scope.** Rename UI; model/context % (already in the status line); spec-16
badges.

---

## 7. RB-11 — Bottom line: counts only

**Amended clause.** `10-supervisor-tui.md` §6.1 (`:758-785`) — including the ASCII
diagram at `:765-773`, whose row `[main ●] [api ✓] …` (`:768`) is a per-session
list — the §8.1 `SessionBar` entry (`:921`), **§3.5 (`:275-320`, the `SessionBar`
dirty bit and the strip's collapsed-summary obligation), §11.3 (`:1159-1170`, the
attention badge), F2 (`:1319`), U-F13 (`:1355`), and the §4.4 `SessionCell`
comment (`:425`)**.

**Old text (code).** `render_session_bar` (`ui_render.cpp:212-236`) renders one cell
per session of the **active workspace only** (`:213` looks up `activeWorkspaceId`),
label `"[" + title + " " + state_glyph(state)` at `:220`, placed by `build_ui` at
`:405`. Counts already render as `"N active · M waiting"` in `render_status`
(`:255-284`, at `:273-274`), placed at `:413`. Model `AggregateStatus{activeCount,
waitingCount}` (`ui_model.hpp:257-262`); `recompute` (`ui_model.cpp:225-241`)
iterates **all sessions across all attached workspaces**. The attention badge
(`cell.attention`) renders in **both** `render_session_bar` (`:221`,`:229`) and
`render_switcher` (`:344`,`:352`).

**New text (pinned).**

1. Drop the per-session list: remove the `render_session_bar` row from `build_ui`
   (`:405`). `render_session_bar` is an anonymous-namespace function; leaving it
   unused is a **`-Wunused-function`** error under `-Werror`, so it **must be
   deleted** (not "may be").
2. Keep the counts. The bottom region composes, in order: `render_input` (`:412`),
   then `render_status` (`:413`) — active-session status left, `"N active · M
   waiting"` right. No other widget is added.
3. **No counting change.** `recompute` already spans all attached workspaces
   (`ui_model.cpp:225-241`); consistent with 16 §3.6 C5 (spec 16 may only **extend**
   "attached" to its owned daemon set, never narrow it).
4. **Spec 10 §3.5 amendment (pinned).** The `SessionBar` strip is **removed**. The
   `UiDirtyFlag::SessionBar` value (`:292`) is **retained-unused** — no live code
   **consumes** it (it is still *marked* at `ui_model.cpp:361` and `:521`) — so the
   verified enum layout is not renumbered; the `§3.5:317-320` sentence about a
   per-session collapsed summary in the strip is superseded: non-active sessions
   now render **nowhere** in the always-on chrome; their state is visible via the
   switcher and the aggregate counts.
5. **Spec 10 §11.3 / F2 / U-F13 amendment (pinned).** The attention **badge**
   (`SessionCell.attention`) is now **switcher-only** (`render_switcher`
   `:344`,`:352`). The always-visible background-permission signal is
   `waitingCount` (`render_status` `:273-274`), which already includes
   `WaitingForPermission`; the dialog is still shown for any session. The badge is
   not cleared by focusing (U7).
6. **Spec 10 §4.4 comment amendment (pinned).** The `SessionCell` comment at
   `:425` ("for the SessionBar strip and the switcher leaf") becomes "for the
   switcher leaf and the header"; the type itself is unchanged.
7. Consistent with spec 16: cross-supervisor visibility uses **counts and the
   switcher**, not a list (`16 §3.6` C2/C4, `16 §3.3`). A spec-16 `"N supervisors"`
   badge composes left or in the switcher, never as a reintroduced bottom list (C2).
8. The switcher (`ui_render.cpp:314-360`) is the surviving per-session UI and is
   otherwise **untouched** (inventory Surface 3).

**Rationale.** The bottom list is redundant with the switcher, duplicates the
aggregate counts, and collides with spec 16's model (C2). Counts-only is the seam
spec 16 already binds to, and the waiting count already carries the
background-permission signal.

**Interfaces touched.** `build_ui` (drop the `render_session_bar` row);
`render_session_bar` (delete). No model changes.

**Invariants.**
- **U-RB11-1** — The bottom region has no per-session list; only the active-session
  status and the aggregate counts render there.
- **U-RB11-2** — The counts scope is unchanged (all attached workspaces,
  `ui_model.cpp:225-241`), consistent with 16 C5.
- **U-RB11-3** — The attention badge survives in the switcher; the always-visible
  background-permission signal is `waitingCount` (F2/U-F13 preserved).

**Failure modes.** **U5/F4** — counts stay a level snapshot recomputed on state
edges, never in `Render()`; removing the list must not change recompute cadence.

**Test plan.** Unit: `recompute` unchanged across workspaces. Golden: the bottom
line no longer contains `[<title> <glyph>]` cells but still contains `"N active ·
M waiting"`; a switcher golden asserts the attention badge still renders. PTY: the
auto-created session's bottom line shows only counts; the switcher still lists
sessions and shows the badge.

**Out of scope.** Redefining "active"/"waiting" (16 C5); the spec-16 supervisor
badge; ownership badges in the bottom row.

---

## 8. Cross-spec consistency with spec 16

- **C1** — header right slot → RB-10; spec-16 indicators go left or in the
  switcher (`16 §3.6`).
- **C2** — "display that there is one more" is a **count/badge**, never a
  per-session list; RB-11's counts-only bottom line is consistent.
- **C4** — visibility reuses the switcher; RB-11 does not touch it.
- **C5** — `AggregateStatus` already spans all attached workspaces
  (`ui_model.cpp:225-241`); spec 16 only extends "attached", never narrows it.
- **O14** — no duplicate visibility UI; this errata adds no always-visible panel.

`docs/design/UI_SURFACE_INVENTORY.md` (C1–C5) is authoritative for current pixels;
this errata changes the bottom line (Surface 2) and the header right slot
(Surface 1) exactly as assigned to RB-11/RB-10.

---

## 9. Test-impact register (explicit)

| Test | Lines | New expectation |
|---|---|---|
| `ui_render_golden_test.cpp` `ConversationSnapshot` | `147-166` | Golden drops `│you:` (`:150`) and `│assistant` (`:152`); user body gains `│ ` gutter; assistant lines unchanged; bottom drops `│[golden-s o]` (`:163`); collapsed tool (`:154-155`) is exactly one line `tool: read_file` with **no** `file-body`. |
| `ui_render_golden_test.cpp` `ExpandedToolCallShowsArguments` | `286-298` | Set `expanded = true` and keep `(expanded)` (`:296`) + `hello.txt` (`:297`); add a collapsed counterpart asserting no body. |
| `render_golden_test.cpp` `TuiRoutesAssistantMarkdownAndToolDiff` | `170,176-177` | Currently asserts a **collapsed** tool's diff body (`src/foo.cpp`, `-    return 1;`). Under fold-by-default these fail. Re-pin: add a `ToolCallView{id="call", name="git_diff", output=kSampleDiff, expanded=true}` to `session->tools` so the diff body renders; alternatively drop `:176-177` and assert the one-line collapsed header `tool: git_diff`. |
| `ui_model_test.cpp` `AppliesUserAndStreamingAssistant` | `58-74` | Text entry count/roles unchanged; add a reasoning variant asserting a `ConversationRole::Reasoning` entry **before** the Assistant entry, `expand_all_folds == false`, and both `streaming` flags cleared on finish. |
| `ui_model_test.cpp` `CommandRegistryDispatchesBuiltins` | `440-470` | `complete("he")` (`:467-469`) still `/help`; add `longest_common_prefix` assertions. |
| `ui_model_test.cpp` `CommandRegistryDispatchesCompact` | `472-487` | `complete("co")` (`:483-485`) still `/compact` (no change to its expectation). |
| `ui_supervisor_pty_test.cpp` `AttachesSpawnsAndSwitches` | `323-333` | Removed bottom session-bar cells no longer exist; `alpha`/`beta` still match via header cwd/switcher. Add a counts-only bottom-line assertion. |
| `ui_supervisor_pty_test.cpp` `HelpListAndHistoryRecall` | `344-389` | Extend with Tab: `/he` + Tab → `/help `; `/` + Tab twice → cycle with **no** trailing space. `/help` (`:373-374`), `/clear` (`:376-379`), history recall (`:381-383`) stay. |

**New tests required.** RB-02 reasoning-fold golden (exact summary strings); RB-02
Ctrl+O expand-all golden; RB-02 reasoning-before-answer ordering unit; RB-08
`InputModel` cycle-reset-on-submit unit; RB-10 `refresh_sessions` UI-thread
title-population unit; RB-11 bottom-line golden and switcher attention-badge golden.

---

## 10. Non-goals and open items

- No implementation code; no edits to `10-supervisor-tui.md` or any other spec.
- Mouse/click expansion, fold cursor, and per-entry expansion persistence are out
  of scope.
- `Theme::user_block` name is pinned; a different spelling needs a new review.
- Open for the Oracle gate: (a) confirm the custom-`Node` left-bar approach is
  acceptable; (b) confirm the global Ctrl+O choice versus a per-entry model.

---

## 11. Revision log (Oracle gate findings F-01..F-14, N-01..N-04)

| ID | Sev | Disposition | Sections changed |
|---|---|---|---|
| **F-01** | HIGH | Fixed. Deleted the `supervisor.cpp:301` assignment; title captured at `:295` and assigned in the UI-thread `enqueue` lambda **after** `ensureCellIn` (`:316`), via new `UiModel::setCellTitle`. New invariant U-RB10-3. | §2 (RB-10 anchors), §6 (items 3–4), §6 invariants/test plan |
| **F-02** | MED | Fixed (refined by N-01). Reasoning-before-answer ordering: insert-before an existing Assistant entry + reindex `by_message`; finish clears `streaming` on both entries. | §2 (RB-02 anchors), §4 (items 1–3), §4 invariants/test plan, §9 |
| **F-03** | MED | Fixed. Pinned the parameter option: `render_entry`/`render_tool_entry` gain `bool expand_all_folds`, passed by `render_conversation` from `active->expand_all_folds`; `RenderContext` unchanged. | §4 (item 6), §4 interfaces |
| **F-04** | MED | Fixed. Pinned exact collapsed/expanded strings incl. streaming variants; dropped the unpinned outcome/truncated marker. | §4 (item 4) |
| **F-05** | MED | Fixed. Pinned cycle-step output `"/" + name` (no trailing space), `cursor = draft.size()`, initial `index = 0`, and per-step stored-draft update; trailing space only for the unique-match terminal completion. | §5 (items 1–2), §5 invariants/test plan |
| **F-06** | MED | Fixed. Pinned `CompletionCycle{ std::string draft; std::vector<std::string> names; std::size_t index = 0; }`; added submit/Return to the reset set. | §5 (items 2–3) |
| **F-07** | MED | Fixed. RB-11 now amends §3.5 (strip removed, `SessionBar` bit retained-unused), §11.3/F2/U-F13 (badge switcher-only; signal = `waitingCount`), §4.4 comment; added U-RB11-3 and switcher-badge golden. | §2 (RB-11 clause), §7 (items 1,4–6), §7 invariants/test plan, §9 |
| **F-08** | MED | Fixed. Added `render_golden_test.cpp:170,176-177` to the register with a precise re-pin. | §9 |
| **F-09** | LOW | Fixed. Pinned `with_left_bar` as a custom `ftxui::Node` (no per-line/left-only primitive exists); no lossy fallback. | §3 (item 5) |
| **F-10** | LOW | Fixed. Dropped the "(+ outcome/truncated marker)" text entirely. | §4 (item 4) |
| **F-11** | LOW | Fixed. Pinned `UiModel::setCellTitle` (no setter exists; `refreshCellIn` omits title) and the order `ensureCellIn` → `setCellTitle`. | §2 (RB-10 anchors), §6 (item 3) |
| **F-12** | LOW | Fixed. `render_session_bar` "must be deleted" (`-Wunused-function`/`-Werror`); stale §4.4 `:425` comment amended by reference. | §7 (items 1,6) |
| **F-13** | LOW | Fixed. Reworded: `SessionCell` default title is empty (`ui_model.cpp:294-296`); `"tui"` is the create parameter, not a cell default; header falls back to `short_id`. | §6 (item 4) |
| **F-14** | LOW | Fixed. Clarified `Theme::user_block` as an independent color-terminal background opt-out; bar is unconditional; `color == false` suppresses bg regardless. | §3 (item 4), §3 test plan |

### Round 3 (N-01..N-04)

| ID | Sev | Disposition | Sections changed |
|---|---|---|---|
| **N-01** | MED | Fixed — **option (a)** chosen. The reasoning-order rule now inserts before an existing Assistant entry **empty or non-empty** (single unconditional branch), reindexing `by_message`; the reachable text-then-reasoning path is cited (`openai_adapter.cpp:432-450` content-before-reasoning; `ui_model.cpp:417`). U-RB02-1 and the prose/test expectations all now say "both arrival orders". | §4 (item 2), §4 U-RB02-1, §4 test plan, §11 (F-02 row) |
| **N-02** | LOW | Fixed. Reworded §7 item 4 to "no live code **consumes** it (still *marked* at `ui_model.cpp:361` and `:521`)"; the do-not-renumber conclusion is unchanged. | §7 (item 4) |
| **N-03** | LOW | Fixed. Corrected the `U-F13` citation from `10-supervisor-tui.md:1356` (which is U-F14) to `:1355` in both places. | §2 (RB-11 clause), §7 (amended clause) |
| **N-04** | LOW | Fixed. Split the register row: `CommandRegistryDispatchesBuiltins` `:440-470` (holds `complete("he")` at `:467-469`) and `CommandRegistryDispatchesCompact` `:472-487` (holds `complete("co")` at `:483-485`). | §9 |

No finding is disputed.

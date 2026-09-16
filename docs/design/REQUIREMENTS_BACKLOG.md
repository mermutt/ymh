# ymh — Requirements Backlog

Triaged from the user's raw notes in `requirements_draft.txt` (repo root,
untracked; read-only). This document is the *engineering* backlog: every raw
note is restated as a requirement, deduplicated against the **real shipped
code**, scoped into what remains, and given effort / risk / priority plus a
recommended order.

Method: each item was verified against `src/` + `include/` (not just specs).
Where Wave 7 already shipped part of an item, the exact `file:line` is cited and
the item is marked **PARTIALLY DONE**. Docs-only claims were not trusted.

Legend for **Current state**:
- **DONE** — requirement already satisfied by shipped code.
- **PARTIAL** — infrastructure exists; a specific delta remains (cited).
- **NEW** — no implementation exists.
- **ARCH** — architectural change; requires a verified design spec *before* code
  (design-first rule, `AGENTS.md`).

Priority: **P0** = do next (cheap, high user-visible value, or unblocks others);
**P1** = valuable, needs design/decision or medium effort; **P2** = polish /
optional. **GATE** = component gate applies (spec must be `verified` first).

---

## Summary

| ID | Title | Raw | State | Priority | Effort | Risk | Spec gate |
|----|-------|-----|-------|----------|--------|------|-----------|
| RB-01 | Message styling: drop role labels, highlight user input | 1 | PARTIAL | P0 | S | Low | No |
| RB-02 | Fold reasoning + tool output by default, Ctrl+O expand | 2 | PARTIAL | P0 | M | Med | Additive errata (10) |
| RB-03 | Auto-name + rename sessions | 3 | PARTIAL | P1 | M | Med | Additive errata (01/05) |
| RB-04 | Supervisor-owned daemons (no unsupervised daemons) | 4 | ARCH | P0 (design) | L | High | **Yes — 16 in progress** |
| RB-05 | `/skills` command | 5 | NEW | P1 | L | Med | **Yes — new spec** |
| RB-06 | `/context` visualizer (grid + MCP/tools) | 6 | PARTIAL | P1 | M | Low | Additive errata (10/13) |
| RB-07 | `/export` session to file (+ edit in `$EDITOR`) | 7 | NEW | P1 | S | Low | No (UI-local) |
| RB-08 | `<Tab>` completes slash commands | 8 | PARTIAL | P0 | S | Low | No |
| RB-09 | Config in `~/.config/ymh/config.jsonc` (JSONC) | 9 | NEW | P1 | M | Med | **Yes — format decision** |
| RB-10 | Current session name in top line, right-aligned | 10 | NEW | P0 | XS | Low | No |
| RB-11 | Bottom line: counts only, not the full session list | 11 | PARTIAL | P0 | XS | Low | No |

---

## Item records

### RB-01 — Message styling: drop role labels, highlight user input
- **Requirement (raw 1):** Remove the words `you` and `assistant` from printing.
  Instead, slightly highlight the background of user input and draw a vertical
  bar on the far left of each user-input line. Assistant output stays on the
  normal background.
- **Current state: PARTIAL.** Role labels are printed today:
  - `src/ui/ui_render.cpp:132` — `paint(ftxui::text("you:"), ftxui::Color::Cyan, theme) | ftxui::bold`
  - `src/ui/ui_render.cpp:136-138` — `"assistant"` / `"assistant (streaming)"` in green+bold.
  - User block is `markdown.render(...)` on the normal background; the only
    per-line affordance elsewhere is the input prompt `"> "`
    (`src/ui/ui_render.cpp:249`).
  - Styling is theme-gated via `paint()` (`src/ui/ui_render.cpp:19-24`) and
    `Theme::color` (`include/ymh/ui/theme.hpp`).
- **What remains:** delete the two label rows; wrap the *user* entry body in a
  subtle background (FTXUI `bgcolor`) and prepend a left vertical bar per line
  (e.g. `│ ` gutter, or a `borderLeft`). Add a theme flag (e.g. `user_block`) so
  monochrome terminals degrade gracefully. Update the golden render test
  expectations for user/assistant entries.
- **Effort:** S (≤0.5d). **Risk:** Low. **Deps:** none. **Priority:** P0.

### RB-02 — Fold reasoning + tool output by default; Ctrl+O to expand
- **Requirement (raw 2):** Fold all thinking / tool-use output by default.
  Provide Ctrl+O (and mouse click if easy) to expand.
- **Current state: PARTIAL.**
  - Ctrl+O already toggles **the last tool call only**:
    `src/ui/supervisor.cpp:725-727` → `toggle_last_tool()`
    (`src/ui/supervisor.cpp:517-525`, flips `tools.calls.back().expanded`).
  - `ToolCallView.expanded` exists (`include/ymh/ui/ui_model.hpp:126`), but
    "collapsed" is **not folded** — the body is still rendered, just `dim`:
    `src/ui/ui_render.cpp:109-120` (header `"tool: <name>"` at `:101`, body
    dimmed when `!expanded`). So output is always visible.
  - **Reasoning/thinking is dropped entirely**: `src/ui/ui_event_adapter.cpp:90-94`
    (`if (payload.kind == AssistantChunkKind::Reasoning || text.empty()) break;`).
    The UI event type has a `reasoning` flag (`include/ymh/ui/ui_event.hpp:84`)
    but no reasoning content is ever surfaced.
  - **No mouse handling**: only `bool mouse = false;` in
    `include/ymh/ui/terminal_layer.hpp:16`; no event routing anywhere.
- **What remains:** (a) capture reasoning chunks into the model (new
  `ConversationEntry` role or `ToolCallView`-like reasoning view); (b) make
  collapsed tool/reasoning entries truly collapsed (single summary line, no
  body); (c) decide expansion scope — last entry (current) vs. per-entry
  selection vs. expand-all; (d) optional mouse click (FTXUI `Event::Mouse`).
  Requires an **additive errata to spec 10** (new model fields + reasoning
  capture), then golden-render tests.
- **Effort:** M (1–2d). **Risk:** Med (reasoning capture touches the event
  adapter; expansion UX is a design choice). **Deps:** RB-01 shares the
  `render_entry` path. **Priority:** P0.

### RB-03 — Auto-name + rename sessions
- **Requirement (raw 3):** Rename sessions after they start to something
  meaningful and short.
- **Current state: PARTIAL.** A `title` field already exists end-to-end:
  - `SessionHeader.title` (`include/ymh/session/session.hpp:49`), persisted
    (`src/session/session_persistence.cpp:41,298,719`), carried on the wire.
  - Heuristic auto-name exists for the **headless** path:
    `src/cli/headless.cpp:159` sets `session_options.title = first_line(options.task, 60)`
    (helper `src/cli/headless.cpp:45`). The **TUI does not** auto-name:
    `src/ui/ui_application.cpp:476` sets `"main"`, and
    `src/ui/supervisor.cpp:347` creates with `{"title","tui"}`.
  - The title is rendered in the session bar (`src/ui/ui_render.cpp:219`), but
    there is **no rename event or wire method** (grep for
    `SessionRenamed`/`rename`/`set_title` finds none).
- **What remains:** (a) TUI auto-naming — either LLM-derived (reuse the
  summarizer/compaction model) or heuristic (first user message, like headless);
  (b) a rename path: an additive session event (e.g. `SessionRenamed`) +
  wire method + a `/rename` command (or inline edit) + persistence update. The
  event log is append-only and typed (`include/ymh/session/events.hpp:365+`), so
  this needs an **additive errata to 01 (session) and 05 (transport)**.
- **Effort:** M (1–2d). **Risk:** Med (touches event log + wire; naming quality
  is subjective). **Deps:** benefits from RB-10 (name display). **Priority:** P1.

### RB-04 — Supervisor-owned daemons (no unsupervised daemons)
- **Requirement (raw 4):** Daemons must not outlive their owning supervisors in
  an unsupervised way. Two supervisors launched in different repos must each see
  the other's sessions and be able to switch to and continue them. Exiting one
  supervisor ends *that* supervisor (the other keeps managing the remaining
  daemons). Exiting the **last** supervisor must prompt that the remaining
  daemons will terminate.
- **Current state: ARCH.** Today a `WorkspaceHost` daemon owns its cwd and
  **survives TUI/supervisor exit** — the "unsupervised daemon" the user is
  rejecting. Code evidence:
  - Spawn is a **detached fork**: `ForkExecLauncher::spawn`
    (`src/host/workspace_host.cpp:773`) forks at `:796` and calls **`::setsid()`**
    at `:801`, detaching the daemon from the supervisor's session/terminal.
  - Attach/spawn orchestration is `HostLifecycle::spawnAndAttach`
    (`src/host/workspace_host.cpp:874`); `HostLifecycle::detach` is a **no-op**
    (`src/host/workspace_host.cpp:937`).
  - Registry tracks a **daemon** liveness claim only — `HostClaim{pid, bootId,
    socketPath}` (`include/ymh/registry/registry.hpp:56-69`), `claimHost` /
    `heartbeat` (`:243-244`). There is **no notion of which supervisor owns or is
    attached to a daemon**.
  - Multi-workspace discovery already exists but is **read-only**:
    `src/cli/cli.cpp:380-381` — "Multi-workspace attach: every other registered
    workspace with a live daemon is attached without spawning one (read-only
    discovery)." So another supervisor's sessions can be *seen*, not *driven*.
  - Exit has **no confirmation**: `requestExit()` (`src/ui/supervisor.cpp:237`)
    bound to Ctrl+D (`:713-715`); `workspace stop` sends `kHostShutdown`
    (`src/cli/cli.cpp:286`). Host-side shutdown plumbing exists
    (`src/host/workspace_host.cpp:196-207`, `src/host/host_runtime.cpp:347-348`).
  Cross-supervisor *switch/continue* and last-supervisor terminate-with-prompt
  semantics do not exist.
- **What remains:** the entire ownership/lifetime model: supervisor→daemon
  ownership, multi-supervisor attach to shared daemons, cross-supervisor session
  visibility + switch, per-supervisor detach, and last-supervisor
  terminate-with-confirmation. This is a **top-level architectural change** and
  is being designed separately as **`docs/design/16-daemon-ownership.md`**
  (in progress by another team member).
- **Effort:** L (multi-day). **Risk:** High (process lifetime, registry
  ownership, transport, exit UX). **Deps:** blocks any daemon-lifecycle UI
  (RB-11 aggregates, switcher). **Priority:** P0 *for design*; code is gated.
- **GATE:** do **not** design here and do **not** write code until
  `16-daemon-ownership.md` is `verified` in `DESIGN_STATUS.md`.

### RB-05 — `/skills` command
- **Requirement (raw 5):** Develop a `/skills` command.
- **Current state: NEW.** There is **no "skill" concept anywhere** in `src/` or
  `include/` (grep returns nothing); the only mention is in
  `docs/design/00-architecture.md:4864`, describing the *dsh* reference design's
  plugin taxonomy — not an ymh feature. The slash-command registry exists
  (`include/ymh/ui/command_registry.hpp`, `src/ui/command_registry.cpp`) so a
  command can be registered cheaply, but there is no skill model, discovery,
  storage, invocation, or context injection.
- **What remains:** define what a "skill" *is* for ymh (discoverable
  prompt/spec bundles? directories with frontmatter? built-ins?), how skills are
  discovered (workspace `.ymh/skills/`, `~/.config/ymh/skills/`), how they are
  listed by `/skills`, how one is invoked, and how skill content enters the
  model context (interaction with RB-06 `/context`). This is a new subsystem.
- **Effort:** L (3–5d). **Risk:** Med. **Deps:** RB-06 (context visibility),
  RB-08 (completion should include the new command). **Priority:** P1.
- **GATE:** needs a new component spec (e.g. `17-skills.md`) verified before code.

### RB-06 — `/context` visualizer (color grid + MCP/tools the model sees)
- **Requirement (raw 6):** Develop `/context` to visualize the context as a
  color grid, plus display the MCPs etc. that the model sees (similar to Claude
  Code).
- **Current state: PARTIAL.** The *data* is largely available:
  - Token usage already flows to the model: `TokenUsageUpdated`
    (`src/ui/ui_event_adapter.cpp:125`), `StatusModel` token fields
    (`include/ymh/ui/ui_model.hpp:158-166`), rendered in the status line
    (`src/ui/ui_render.cpp:262-269`).
  - Compaction emits a token estimate / boundary:
    `src/ui/ui_event_adapter.cpp:147,211` (spec 13 implemented).
  - MCP server status + tools are enumerable:
    `include/ymh/mcp/mcp_manager.hpp:56` `statuses()`, `McpServerStatus`,
    `listTools()` / `McpToolInfo` (`include/ymh/mcp/mcp_client.hpp:43`).
  - But there is **no `/context` command** and no assembled-context snapshot
    (system prompt + tool schemas + MCP tools + message list + per-segment
    token accounting) exposed to the UI.
- **What remains:** expose an assembled-context snapshot (segments + token
  estimates + tool/MCP inventory) to the supervisor; render a color grid and a
  list of MCP servers/tools. Likely an **additive errata to 10** (UI) and a
  read-only view over 06/13/15 internals.
- **Effort:** M (1–3d). **Risk:** Low (read-only). **Deps:** benefits from
  RB-08 (completion). **Priority:** P1.

### RB-07 — `/export` session to a file in cwd (+ edit in editor)
- **Requirement (raw 7):** Develop `/export` to write the current session to a
  file in cwd. Nice-to-have: launch vim with the content so the user can edit it
  or write it as-is.
- **Current state: NEW.** No export command or session serializer exists.
  Building blocks are present: every event payload has `to_json`
  (`include/ymh/session/events.hpp:365+`) and persistence already serializes
  sessions to JSON (`src/session/session_persistence.cpp:959-960`); the
  workspace cwd is available to the supervisor. No editor integration exists.
- **What remains:** a `/export [path]` command that renders the active session
  (markdown or JSON) to a file under cwd; optional `$EDITOR`/`vim` launch via
  PTY (spec 14 PTY capability is implemented and could host the editor). Needs a
  format decision (markdown vs JSON vs both) and path-safety via
  `ExecutionEnvironment::resolve()`.
- **Effort:** S–M (0.5–1.5d). **Risk:** Low. **Deps:** RB-08; PTY (spec 14) for
  the editor launch. **Priority:** P1.

### RB-08 — `<Tab>` completes slash commands
- **Requirement (raw 8):** Develop `<Tab>` to complete slash commands.
- **Current state: PARTIAL.** The completion machinery exists but Tab is not
  wired in the input:
  - `CommandRegistry::complete(prefix)` (`include/ymh/ui/command_registry.hpp:37`,
    `src/ui/command_registry.cpp:60-69`).
  - Live hints are already computed and rendered as the user types:
    `refresh_hints()` (`src/ui/supervisor.cpp:467-480`) → `command_hints`
    (`include/ymh/ui/ui_model.hpp:196`) → `render_command_hints()`
    (`src/ui/ui_render.cpp:198-210`).
  - `Tab` is bound **only in the switcher** (`src/ui/supervisor.cpp:591-594`);
    `handle_input()` (`src/ui/supervisor.cpp:609-696`) has **no `Tab` case**, so
    Tab never inserts/completes a command.
- **What remains:** handle `Event::Tab` in `handle_input`: when the draft is a
  bare `/prefix`, complete to the unique match (or longest common prefix), cycle
  through multiple matches, and insert a trailing space. Needs a common-prefix
  helper on the registry.
- **Effort:** S (≤0.5d). **Risk:** Low. **Deps:** none (but every new command
  from RB-05/06/07 should appear automatically via the registry). **Priority:** P0.

### RB-09 — Config in `~/.config/ymh/config.jsonc` (JSONC)
- **Requirement (raw 9):** Make `ymh` read config using the file format of
  `~/.config/ymh/config.jsonc` (JSONC).
- **Current state: NEW (format is TOML today).**
  - Global config file is **`config.toml`**: `src/config/config.cpp:21`
    (`kConfigFile = "config.toml"`), documented at `include/ymh/config/config.hpp:8`.
  - Default path resolution: `src/config/config.cpp:579-587`
    (`$XDG_CONFIG_HOME/ymh/config.toml`, else `~/.config/ymh/config.toml`).
  - Parser is **toml++** (`src/config/config.cpp:13`, `apply_toml_file` `:619-640`);
    unknown keys are hard errors (`reject_unknown`, `:27`).
  - Layering: defaults → global TOML → `<workspace>/.ymh/config.toml` → `YMH_*`
    env → CLI (`include/ymh/config/config.hpp:5-16`). Full schema:
    `[ui] [agent] [workspace] [permissions] [logging] [llm] [mcp]`
    (`config.hpp:36-…`, `config.cpp:400-437`).
  - `nlohmann_json` is already a dependency, but JSON has **no comment support**
    natively; JSONC needs a comment-stripping pre-pass or a dedicated parser.
- **What remains:** a **format decision** — replace TOML with JSONC, support
  both, or accept `.jsonc` alongside `.toml`. If JSONC: comment-tolerant parse,
  same strict unknown-key behavior, same layer order, migrate
  `write_default_config`/scaffold and any docs. This changes a frozen config
  interface, so it needs an explicit decision + spec errata before code.
- **Effort:** M (1–2d, mostly tests + migration). **Risk:** Med (breaking change
  to an existing config surface; strictness parity). **Deps:** none. **Priority:** P1.
- **GATE:** needs a format decision + spec errata (00 §37 / 08 §5.3) verified
  before code.

### RB-10 — Current session name in top line, right-aligned
- **Requirement (raw 10):** Display the name of the current session in the top
  line, on the right.
- **Current state: NEW.** `render_header()` (`src/ui/ui_render.cpp:364-372`)
  renders `"ymh · <cwd>"` on the left followed by `ftxui::filler()` — the right
  side is intentionally empty. The header is placed by `build_ui()`
  (`src/ui/ui_render.cpp:397`). The session title *is* available
  (`SessionCell.title`, `include/ymh/ui/ui_model.hpp:203`; active session via
  `model.workspaces` / `model.sessions`).
- **What remains:** look up the active session's title in `render_header` and
  render it after the filler (right-aligned), falling back to a short id when
  empty (mirror `src/ui/ui_render.cpp:219`).
- **Effort:** XS (≤1h). **Risk:** Low. **Deps:** RB-03 makes titles meaningful;
  works today with the placeholder titles. **Priority:** P0.

### RB-11 — Bottom line: counts only, not the full session list
- **Requirement (raw 11):** Do not list all sessions in the bottom line. Just
  display the number of them, with how many are active/waiting.
- **Current state: PARTIAL.** Both halves already exist, just not in the desired
  form:
  - The **full session list** is rendered by `render_session_bar()`
    (`src/ui/ui_render.cpp:212-236`), one `[title glyph]` cell per session,
    placed by `build_ui()` (`src/ui/ui_render.cpp:405`).
  - The **aggregate counts** are already computed and rendered:
    `AggregateStatus{activeCount, waitingCount}`
    (`include/ymh/ui/ui_model.hpp:257-262`), rendered as
    `"N active · M waiting"` in `render_status()` (`src/ui/ui_render.cpp:273-274`).
- **What remains:** replace the per-session bar (`render_session_bar`) with a
  compact count summary (or fold the existing aggregate into that row) and keep
  full session navigation in the switcher overlay
  (`render_switcher`, `src/ui/ui_render.cpp:314-360`). Decide whether the
  aggregate moves from the status row to the session-bar row. Golden-render test
  updates.
- **Effort:** XS (≤1h). **Risk:** Low. **Deps:** none; interacts with RB-04
  (cross-supervisor session counts) and RB-10 (header name). **Priority:** P0.

---

## Recommended ordering

Phase A — **cheap TUI chrome** (no spec gate, small, high visible value):
1. **RB-10** session name top-right (XS)
2. **RB-11** bottom line counts (XS)
3. **RB-08** Tab completion (S)
4. **RB-01** message styling / drop labels (S)

Phase B — **presentation depth** (additive errata to 10):
5. **RB-02** fold reasoning + tool output (M)
6. **RB-06** `/context` visualizer (M)

Phase C — **commands & config** (small-to-medium, some need a decision):
7. **RB-07** `/export` (S–M)
8. **RB-03** auto-name + rename (M; errata 01/05)
9. **RB-09** JSONC config (M; format decision)

Phase D — **new subsystem & architecture** (spec-gated):
10. **RB-05** `/skills` (L; new spec)
11. **RB-04** supervisor-owned daemons (L; spec 16 in progress) — **design runs in
    parallel from the start**, code only after the gate.

Rationale: A items are independent, reversible, and test-only risk. B reuses the
`render_entry`/status plumbing established by A. C introduces wire/persistence
changes (RB-03) and a breaking config decision (RB-09), so they want their own
errata. D are the only items that need a new spec or a top-level architectural
change.

## Groupings

- **Group 1 — TUI chrome:** RB-01, RB-10, RB-11, RB-08. One rendering/event
  pass, shared golden-render test updates.
- **Group 2 — Output folding:** RB-02. Event-adapter + render + input-model
  changes; touches the reasoning path.
- **Group 3 — Session lifecycle & naming:** RB-03. Additive session event +
  wire method + persistence + `/rename`.
- **Group 4 — New slash commands:** RB-05, RB-06, RB-07. All extend the existing
  `CommandRegistry`; RB-08 completion picks them up automatically.
- **Group 5 — Config format:** RB-09. Isolated from the UI; a parser + schema
  decision.
- **Group 6 — Architecture:** RB-04. Cross-cutting; owns the daemon lifetime
  model and the switcher/aggregate semantics used by Groups 1/3.

## Needs a design spec (or explicit decision) *before code*

1. **RB-04 — daemon ownership.** Top-level architectural change.
   `docs/design/16-daemon-ownership.md` is in progress; mark code-gated until
   `DESIGN_STATUS.md` shows it verified. **Do not design here.**
2. **RB-05 — `/skills`.** New subsystem with no existing concept; needs a new
   component spec (e.g. `17-skills.md`): skill model, discovery, storage,
   invocation, context injection.
3. **RB-09 — JSONC config.** Breaking change to a frozen config interface; needs
   an explicit format decision (replace TOML / dual-read / new file) plus errata
   to the config spec (00 §37 / 08 §5.3).
4. **RB-03 — session rename.** Additive but wire- and log-visible; needs an
   errata to `01-session.md` + `05-transport.md` (new event + method) before code.
5. **RB-02 — reasoning capture.** Additive errata to `10-supervisor-tui.md`
   (new model fields + reasoning ingestion) before code.
6. **RB-06 — `/context`.** Additive errata to `10-supervisor-tui.md` (assembled-
   context snapshot interface) before code.

Items **RB-01, RB-07, RB-08, RB-10, RB-11** are UI-local, reversible, and do not
require a spec gate.

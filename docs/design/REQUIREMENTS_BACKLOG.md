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

## Scope decisions

- **DECISION (user, 2026-09-17): remote SSH/TCP transport is OUT of scope for
  this project.** It is not deferred and not a candidate: it will not be planned
  or built. The JSON-RPC protocol stays transport-agnostic, so this is a scope
  decision, not a design blocker. The old "deferred" text lives in
  `00-architecture.md` §47 Mode B, a historical superseded-by-errata record that
  is deliberately not edited here.

---

## Summary

| ID | Title | Raw | State | Priority | Effort | Risk | Spec gate |
|----|-------|-----|-------|----------|--------|------|-----------|
| RB-01 | Message styling: drop role labels, highlight user input | 1 | **DONE** | P0 | S | Low | No |
| RB-02 | Fold reasoning + tool output by default, Ctrl+O expand | 2 | **DONE** | P0 | M | Med | errata 17 (verified) |
| RB-03 | Auto-name + rename sessions | 3 | **DONE** | P1 | M | Med | `19-session-rename-errata.md` (verified + implemented) |
| RB-04 | Supervisor-owned daemons (no unsupervised daemons) | 4 | **DONE** | P0 | L | High | **16 verified + implemented** |
| RB-05 | `/skills` command | 5 | **DONE** | P1 | L | Med | `20-skills.md` (verified + implemented) |
| RB-06 | `/context` visualizer (grid + MCP/tools) | 6 | **DONE** | P1 | M | Low | `18-context-errata.md` (verified + implemented) |
| RB-07 | `/export` session to file (+ edit in `$EDITOR`) | 7 | **DONE** | P1 | S | Low | No (UI-local) |
| RB-08 | `<Tab>` completes slash commands | 8 | **DONE** | P0 | S | Low | No |
| RB-09 | Config in `~/.config/ymh/config.jsonc` (JSONC) | 9 | **DONE** | P1 | M | Med | `21-config-jsonc-errata.md` (verified + implemented) |
| RB-10 | Current session name in top line, right-aligned | 10 | **DONE** | P0 | XS | Low | No |
| RB-11 | Bottom line: counts only, not the full session list | 11 | **DONE** | P0 | XS | Low | No |
| RB-12 | Modal input focus: keystrokes must not split between dialog and composer | — (live testing) | NEW | P1 | S | Low | No |
| RB-13 | Live-only Ctrl-S switcher + `/sessions` disk catalog (workspace vs session) | — (reported confusion) | **DONE** | P0 | L | Med | `22-switcher-sessions-errata.md` (verified + implemented) |
| RB-14 | Test isolation: pin `XDG_STATE_HOME` in a process-wide fixture | — (spec 22 §11.2) | NEW | P2 | S | Low | No (test infra) |
| RB-15 | Guard the `session.create` reply path against an evicted workspace | — (spec 22 §11.7) | NEW | P2 | S | Low | No |
| RB-16 | Slash-command completion list with highlighted selection | — (user, 2026-09-17) | NEW | P1 | S | Low | No (UI-local) |
| RB-17 | Reasoning indicator: animated glyph + dimmed hint | — (user, 2026-09-17) | NEW | P1 | XS | Low | No (UI-local) |
| RB-18 | Cursor flicker when the tmux pane is unfocused | — (live testing, 2026-09-17) | NEW | P1 | S | Med | No (UI-local) |

**Shipped:** the five P0 UI items (RB-01/02/08/10/11) landed in commit `71dda4f16`
per the verified errata `17-ui-transcript-errata.md`; verified live in a PTY
against real DeepSeek. RB-04 landed in 8 waves (`036e4e4e1`…`065e221d9`) per the
verified spec `16-daemon-ownership.md`, also verified live with two supervisors.
RB-07 landed in `38c8a6214` (gate-free UI-local item). **RB-13** landed per the
verified spec `22-switcher-sessions-errata.md` (S1–S4: live-only Ctrl-S switcher
+ `/sessions` disk catalog), also verified live. RB-09 is implemented (JSONC only;
spec `21-config-jsonc-errata.md`, `9db17cd54`). RB-03 landed per the verified spec
`19-session-rename-errata.md` (`162fa8641`). RB-05 landed per the verified spec
`20-skills.md` (`6761e62ef`). RB-06 landed per the verified spec
`18-context-errata.md` (`edd696c6e`).

**No item is currently in a design gate.** Every item with a spec gate has passed
and shipped. The open items are RB-12 (UI-local, P1), RB-14 and RB-15 (test
infra / guard, P2), and the new RB-16 to RB-18 below.

---

## Item records

### RB-01 — Message styling: drop role labels, highlight user input
- **Requirement (raw 1):** Remove the words `you` and `assistant` from printing.
  Instead, slightly highlight the background of user input and draw a vertical
  bar on the far left of each user-input line. Assistant output stays on the
  normal background.
- **Current state: DONE** (errata `17-ui-transcript-errata.md`, `71dda4f16`).
  Pre-implementation snapshot: role labels were printed today:
  - `src/ui/ui_render.cpp:132` — `paint(ftxui::text("you:"), ftxui::Color::Cyan, theme) | ftxui::bold`
  - `src/ui/ui_render.cpp:136-138` — `"assistant"` / `"assistant (streaming)"` in green+bold.
  - User block is `markdown.render(...)` on the normal background; the only
    per-line affordance elsewhere is the input prompt `"> "`
    (`src/ui/ui_render.cpp:249`).
  - Styling is theme-gated via `paint()` (`src/ui/ui_render.cpp:19-24`) and
    `Theme::color` (`include/ymh/ui/theme.hpp`).
- **What shipped:** delete the two label rows; wrap the *user* entry body in a
  subtle background (FTXUI `bgcolor`) and prepend a left vertical bar per line
  (e.g. `│ ` gutter, or a `borderLeft`). Add a theme flag (e.g. `user_block`) so
  monochrome terminals degrade gracefully. Update the golden render test
  expectations for user/assistant entries.
- **Effort:** S (≤0.5d). **Risk:** Low. **Deps:** none. **Priority:** P0.

### RB-02 — Fold reasoning + tool output by default; Ctrl+O to expand
- **Requirement (raw 2):** Fold all thinking / tool-use output by default.
  Provide Ctrl+O (and mouse click if easy) to expand.
- **Current state: DONE** (errata `17-ui-transcript-errata.md`, `71dda4f16`).
  Pre-implementation snapshot:
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
- **What shipped:** (a) capture reasoning chunks into the model (new
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
- **Current state: DONE** (spec `19-session-rename-errata.md`, `162fa8641`).
  Pre-implementation snapshot: a `title` field already existed end-to-end:
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
- **What shipped:** (a) TUI auto-naming — either LLM-derived (reuse the
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
- **Current state: DONE** (spec `16-daemon-ownership.md`, 8 waves
  `036e4e4e1`…`065e221d9`). Pre-implementation snapshot: a `WorkspaceHost` daemon
  owned its cwd and
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
- **What shipped:** the entire ownership/lifetime model: supervisor→daemon
  ownership, multi-supervisor attach to shared daemons, cross-supervisor session
  visibility + switch, per-supervisor detach, and last-supervisor
  terminate-with-confirmation. This was a **top-level architectural change**,
  designed separately as **`docs/design/16-daemon-ownership.md`**.
- **Effort:** L (multi-day). **Risk:** High (process lifetime, registry
  ownership, transport, exit UX). **Deps:** blocks any daemon-lifecycle UI
  (RB-11 aggregates, switcher). **Priority:** P0; shipped.
- **GATE:** satisfied — `16-daemon-ownership.md` is `verified` + implemented in
  `DESIGN_STATUS.md`.

### RB-05 — `/skills` command
- **Requirement (raw 5):** Develop a `/skills` command.
- **Current state: DONE** (spec `20-skills.md`, `6761e62ef`). Pre-implementation
  snapshot: there was **no "skill" concept anywhere** in `src/` or
  `include/` (grep returns nothing); the only mention is in
  `docs/design/00-architecture.md:4864`, describing the *dsh* reference design's
  plugin taxonomy — not an ymh feature. The slash-command registry exists
  (`include/ymh/ui/command_registry.hpp`, `src/ui/command_registry.cpp`) so a
  command can be registered cheaply, but there is no skill model, discovery,
  storage, invocation, or context injection.
- **What shipped:** define what a "skill" *is* for ymh (discoverable
  prompt/spec bundles? directories with frontmatter? built-ins?), how skills are
  discovered (workspace `.ymh/skills/`, `~/.config/ymh/skills/`), how they are
  listed by `/skills`, how one is invoked, and how skill content enters the
  model context (interaction with RB-06 `/context`). This is a new subsystem.
- **Effort:** L (3–5d). **Risk:** Med. **Deps:** RB-06 (context visibility),
  RB-08 (completion should include the new command). **Priority:** P1.
- **GATE:** satisfied — spec `20-skills.md` is `verified` + implemented
  (`6761e62ef`).

### RB-06 — `/context` visualizer (color grid + MCP/tools the model sees)
- **Requirement (raw 6):** Develop `/context` to visualize the context as a
  color grid, plus display the MCPs etc. that the model sees (similar to Claude
  Code).
- **Current state: DONE** (spec `18-context-errata.md`, `edd696c6e`).
  Pre-implementation snapshot: the *data* was largely available:
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
- **What shipped:** expose an assembled-context snapshot (segments + token
  estimates + tool/MCP inventory) to the supervisor; render a color grid and a
  list of MCP servers/tools. Likely an **additive errata to 10** (UI) and a
  read-only view over 06/13/15 internals.
- **Effort:** M (1–3d). **Risk:** Low (read-only). **Deps:** benefits from
  RB-08 (completion). **Priority:** P1.

### RB-07 — `/export` session to a file in cwd (+ edit in editor)
- **Requirement (raw 7):** Develop `/export` to write the current session to a
  file in cwd. Nice-to-have: launch vim with the content so the user can edit it
  or write it as-is.
- **Current state: DONE** (`38c8a6214`). `/export [path]` renders the active
  session to markdown under the workspace root; `/export --edit` (`-e`) writes the
  file then hands the terminal to the editor via FTXUI `WithRestoredIO` ($VISUAL →
  $EDITOR → vi). The transcript is reconstructed from the durable event log
  (`session/session_persistence.hpp`), not the trimmed TUI model. Path safety goes
  through `ExecutionEnvironment::resolve()`; the filename stem is sanitized and
  capped so a title cannot traverse. Markdown was chosen as the format (readable,
  diffable, editor-friendly); the JSON view remains available via persistence.
  Tests: +7 (`session_export_test.cpp`).

### RB-08 — `<Tab>` completes slash commands
- **Requirement (raw 8):** Develop `<Tab>` to complete slash commands.
- **Current state: DONE** (errata `17-ui-transcript-errata.md`, `71dda4f16`).
  Pre-implementation snapshot: the completion machinery existed but Tab was not
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
- **What shipped:** handle `Event::Tab` in `handle_input`: when the draft is a
  bare `/prefix`, complete to the unique match (or longest common prefix), cycle
  through multiple matches, and insert a trailing space. Needs a common-prefix
  helper on the registry.
- **Effort:** S (≤0.5d). **Risk:** Low. **Deps:** none (but every new command
  from RB-05/06/07 should appear automatically via the registry). **Priority:** P0.

### RB-09 — Config in `~/.config/ymh/config.jsonc` (JSONC)
- **Requirement (raw 9):** Make `ymh` read config using the file format of
  `~/.config/ymh/config.jsonc` (JSONC).
- **DECISION (user, 2026-09-16): JSONC only; TOML is retired.** `config.jsonc` is
  the sole config file in both slots. `#` is not a comment and trailing commas
  are not allowed. ymh has **no TOML awareness**: it never stats, opens, parses,
  mentions, or warns about `config.toml`, so a leftover `config.toml` is
  invisible and contributes nothing (21-D11). No dual-read, no precedence rule.
  `toml++` is no longer a dependency.
- **Current state: DONE** (`9db17cd54`, spec `21-config-jsonc-errata.md`).
  - Global config file is **`config.jsonc`**: `src/config/config.cpp:29`
    (`kConfigFile = "config.jsonc"`); the workspace file is
    `<workspace>/.ymh/config.jsonc` (`workspace_config_path`, `config.cpp:734`).
  - Default path resolution: `default_global_config_path()` (`config.cpp:724`)
    returns `$XDG_CONFIG_HOME/ymh/config.jsonc`, else
    `~/.config/ymh/config.jsonc` (unchanged branch order).
  - Parser is **nlohmann/json** with `ignore_comments = true`
    (`apply_jsonc_file`); unknown keys are hard errors (`reject_unknown`), and the
    top-level message has no leading dot (`unknown key 'auto_compact_enabled'`;
    nested `unknown key 'agent.compaction.foo'`, 21-D14). A malformed document is
    a `ConfigError` naming file + line.
  - Layering is unchanged: defaults → global → workspace → `YMH_*` env → CLI
    (`include/ymh/config/config.hpp`). Full schema: `ui`, `agent`, `workspace`,
    `permissions`, `logging`, `llm`, `mcp`, `skills`.
- **Final contract (Rev 5–8):**
  - **Global layer required.** An empty or absent global path is a `ConfigError`
    (`config: required global config path is empty` / `config <path>: required
    global config not found`, exit 2); the workspace layer stays optional and
    contributes nothing when absent (21-D12).
  - **First run scaffolds** the conventional global `config.jsonc` (and
    `<workspace>/.ymh/`). An explicit `--config <path>` is **never**
    auto-created: a missing explicit path is the hard error above (21-D13).
  - **A present path must be a regular file** for either layer; a directory /
    FIFO / socket / device fails with `config path is not a regular file`
    (21-D16).
  - **Config loads only for** `ymh`, `ymh run`, `ymh list`, `ymh show`,
    `ymh replay`, `ymh fork`; `ymh workspace …`, `ymh config …`, and
    `ymh version` never load config (nor config-derived logging) (21-D17).
  - **The supervisor passes its effective global path to the daemon**
    (`ymh --host … --config <path>`), so the daemon loads exactly the same file
    as its supervisor (21-D15).
- **Effort:** M (1–2d, mostly tests + migration). **Risk:** Med (breaking change
  to an existing config surface; strictness parity). **Deps:** none. **Priority:** P1.
- **GATE:** satisfied — spec `21-config-jsonc-errata.md` is verified
  (Rev 5–8, 21-D11–21-D17) and implemented.

### RB-10 — Current session name in top line, right-aligned
- **Requirement (raw 10):** Display the name of the current session in the top
  line, on the right.
- **Current state: DONE** (errata `17-ui-transcript-errata.md`, `71dda4f16`).
  Pre-implementation snapshot: `render_header()` (`src/ui/ui_render.cpp:364-372`)
  renders `"ymh · <cwd>"` on the left followed by `ftxui::filler()` — the right
  side is intentionally empty. The header is placed by `build_ui()`
  (`src/ui/ui_render.cpp:397`). The session title *is* available
  (`SessionCell.title`, `include/ymh/ui/ui_model.hpp:203`; active session via
  `model.workspaces` / `model.sessions`).
- **What shipped:** look up the active session's title in `render_header` and
  render it after the filler (right-aligned), falling back to a short id when
  empty (mirror `src/ui/ui_render.cpp:219`).
- **Effort:** XS (≤1h). **Risk:** Low. **Deps:** RB-03 makes titles meaningful;
  works today with the placeholder titles. **Priority:** P0.

### RB-11 — Bottom line: counts only, not the full session list
- **Requirement (raw 11):** Do not list all sessions in the bottom line. Just
  display the number of them, with how many are active/waiting.
- **Current state: DONE** (errata `17-ui-transcript-errata.md`, `71dda4f16`).
  Pre-implementation snapshot: both halves already existed, just not in the
  desired form:
  - The **full session list** is rendered by `render_session_bar()`
    (`src/ui/ui_render.cpp:212-236`), one `[title glyph]` cell per session,
    placed by `build_ui()` (`src/ui/ui_render.cpp:405`).
  - The **aggregate counts** are already computed and rendered:
    `AggregateStatus{activeCount, waitingCount}`
    (`include/ymh/ui/ui_model.hpp:257-262`), rendered as
    `"N active · M waiting"` in `render_status()` (`src/ui/ui_render.cpp:273-274`).
- **What shipped:** replace the per-session bar (`render_session_bar`) with a
  compact count summary (or fold the existing aggregate into that row) and keep
  full session navigation in the switcher overlay
  (`render_switcher`, `src/ui/ui_render.cpp:314-360`). Decide whether the
  aggregate moves from the status row to the session-bar row. Golden-render test
  updates.
- **Effort:** XS (≤1h). **Risk:** Low. **Deps:** none; interacts with RB-04
  (cross-supervisor session counts) and RB-10 (header name). **Priority:** P0.

### RB-12 — Modal input focus: keystrokes must not split between dialog and composer
- **Requirement (discovered by live PTY verification, 2026-09-16):** while a
  modal (the permission dialog, the exit-confirm prompt, the switcher) has focus,
  typed characters must go to the modal only. Today a printable keystroke that the
  modal does not bind falls through into the composer, so a command typed during a
  modal is split between the two: typing `/rename repo overview` with the
  permission dialog open consumed the `r` as a dialog key and left
  `ame repo overview` in the composer — which, once the dialog closed, was
  submitted as a chat message.
- **Current state: DONE (2026-09-17).** Fixed in `SupervisorApp`
  (`src/ui/supervisor.cpp`): the composer/input state is snapshotted when a modal
  opens and restored when it closes, and printable input is dropped for a short
  wall-clock window (`kModalTailWindow`, 100 ms) after the close so the tail of a
  paste/burst whose resolving key closed the modal cannot reach the composer. The
  window self-expires, so later deliberate typing is never swallowed. Covered by
  the PTY test `UiSupervisorPty.ModalKeystrokesDoNotReachComposer`.
- **What remains:** none for the reported defect. The guard covers the permission
  dialog, the exit prompt, the switcher, and the context overlay.
- **Effort:** S (≤0.5d). **Risk:** Low (input routing only). **Deps:** none.
  **Priority:** P1 (user-visible, but only when a modal is open).
- **Note:** this is exactly the class of defect the hermetic suite structurally
  cannot catch — it needs a real terminal driving real keystrokes.
- **Decision (2026-09-17, user-approved) — implemented:** typing a normal slash
  command while the permission dialog was open could **silently answer** the
  request when the string contained `n`/`y`/a digit (e.g. `/rename …` denied it
  via the `n`). The permission dialog now resolves **only on `Enter`** against the
  highlighted option: the bare-letter/number decision branches (`1`/`y`/`Y`,
  `2`, `3`, `0`/`n`/`N`) were removed and every other key is swallowed by the
  dialog (the RB-12 contract, unchanged). `Escape`/`Ctrl-C` still deny (Once) and
  `ArrowUp`/`ArrowDown` still move the selection; the option labels dropped their
  now-dead numeric prefixes and the footer reads
  `↑/↓ select · Enter confirm · Esc cancel`. The **exit-confirm prompt's keys are
  unchanged** (`y`/`n` confirm/cancel) — this decision is scoped to the permission
  dialog only. Covered by the harness test
  `SupervisorHarnessTest.RB12_PermissionDialogResolvesOnlyOnEnter`.

### RB-13 — Live-only Ctrl-S switcher + `/sessions` disk catalog
- **Requirement (reported confusion):** the Ctrl-S switcher listed every
  workspace ever seen live (the reporter saw 42 entries while exactly one daemon
  was live), and there was no way to see a stopped workspace's stored sessions.
  The user asked for a clear split between *running workspaces* and *recorded
  sessions*.
- **Current state: DONE.** Implemented per the verified spec
  `22-switcher-sessions-errata.md` (S1–S4; Oracle PASS after 5 rounds):
  - **S1** — the Ctrl-S switcher is **live-only** (`live && daemonStatus ∈
    {Attached, Stopping}`) and entries are **evicted** when their daemon dies; it
    no longer shows detached/background workspaces (§3).
  - **S2** — `/sessions` lists stored sessions read **directly from disk**
    (`<workspace>/.ymh/sessions.db`) for every **registered** workspace, live or
    not, degrading per workspace; it reuses the switcher overlay rather than
    adding a second one (§4).
  - **S3** — selecting a session in a non-running workspace spawns/attaches its
    daemon then resumes it; failures surface a status-bar notice, never a phantom
    workspace (§5).
  - **S4** — `ymh --resume <id>` works in TUI mode (unknown id → exit 1) (§6).
- **Effort:** L. **Risk:** Med. **Deps:** RB-04 (daemon ownership).
  **Priority:** P0.
- **GATE:** `22-switcher-sessions-errata.md` verified + implemented.

### RB-14 — Test isolation: pin `XDG_STATE_HOME` in a process-wide fixture
- **Requirement (spec 22 §11.2, recorded):** some live-test paths do not pin
  `XDG_STATE_HOME`, so they can leak real workspace rows into the developer's
  `~/.local/state/ymh/registry.db`. A leaked row is exactly the kind of stale
  workspace the old switcher retained forever, and `/sessions` now enumerates
  leaked rows too.
- **Current state: NEW.** Out of scope for spec 22; needs a separate test-infra
  fix that pins `XDG_STATE_HOME` in a process-wide fixture. Pointers:
  `tests/support/host_harness.hpp`, `tests/unit/ui_supervisor_pty_test.cpp`,
  `tests/integration_live_e2e_test.cpp`.
- **Effort:** S. **Risk:** Low. **Deps:** none. **Priority:** P2.

### RB-15 — Guard the `session.create` reply path against an evicted workspace
- **Requirement (spec 22 §11.7, recorded):** `SupervisorApp::create_session`'s
  reply handler calls `model_.ensureSessionIn`/`ensureCellIn` with **no**
  workspace guard (`src/ui/supervisor.cpp:854-859`). If the workspace were evicted
  between the `session.create` submit and its reply, that path could inject a
  phantom `WorkspaceModel` (the same defect class as spec 22's MEDIUM-1, but
  pre-existing and on the `create_session` path).
- **Current state: NEW** (pre-existing, out of scope for spec 22; LOW-6).
- **What remains:** apply the same `model_.workspaces.count(workspace)` guard
  that SW25 uses on the resume path to the `create_session` handler.
- **Effort:** XS. **Risk:** Low. **Deps:** none. **Priority:** P2.

### RB-16 — Slash-command completion list with a highlighted selection
- **Requirement (user, 2026-09-17):** when the user types `/`, show a **list** of
  the possible completions. Pressing `<tab>` types the selected completion after
  the `/`. The **selected command in the list must be highlighted in a brighter
  colour** than the rest.
- **Current state: NEW (extends shipped RB-08 / spec 17).** Today there is only a
  single completion hint line and Tab completes:
  - hints: `refresh_hints()` (`src/ui/supervisor.cpp:467-480`) →
    `command_hints` (`include/ymh/ui/ui_model.hpp:196`) →
    `render_command_hints()` (`src/ui/ui_render.cpp:198-210`).
  - Tab completion landed with RB-08 per `17-ui-transcript-errata.md`.
  There is no multi-row candidate list and no selection highlight, so this is an
  extension of the existing completion surface, not a new one.
- **What remains:** render the full candidate list when the draft is a bare
  `/prefix`; track a selected index and paint it in a brighter colour than the
  unselected rows; `<tab>` inserts the selected completion after the `/`. The
  selection-navigation interaction (e.g. repeated `<tab>` cycling vs. arrow keys)
  must be decided during implementation and kept consistent with the existing key
  handling (`handle_input()`, `src/ui/supervisor.cpp:609-696`).
- **Effort:** S (≤0.5d). **Risk:** Low (render + input routing only). **Deps:**
  RB-08 (shipped); candidates come from the existing `CommandRegistry`.
  **Priority:** P1.
- **GATE:** none. UI-local, reversible, user-visible; same class as
  RB-01/RB-07/RB-08/RB-10/RB-11, which do not require a spec gate.

### RB-17 — Reasoning indicator: animated glyph + dimmed hint
- **Requirement (user, 2026-09-17):** replace the current wording
  `reasoning (Ctrl+O to expand)` with `<sign> Thinking`, where `<sign>` is a
  **single-character animation**, and render `ctrl+o to expand` in a **more
  greyish / less visible colour** than the label.
- **Current state: NEW (extends shipped RB-02 / spec 17).** Reasoning folding and
  the `Ctrl+O` expand affordance shipped with RB-02 per
  `17-ui-transcript-errata.md`; the indicator text is currently static.
- **What remains:** swap the wording, drive `<sign>` from a single-character
  animation, and dim the `ctrl+o to expand` hint. **Constraint:** the animation
  must only tick while a reasoning block is actually streaming, so an idle TUI
  burns no extra CPU (no free-running timer when nothing is streaming).
- **Effort:** XS (≤1h). **Risk:** Low. **Deps:** RB-02 (shipped).
  **Priority:** P1.
- **GATE:** none. UI-local, reversible, user-visible; no spec gate.

### RB-18 — Cursor flicker when the tmux pane is unfocused
- **Requirement (live testing, 2026-09-17):** when ymh runs in tmux and the
  cursor/focus is in **another pane**, ymh's cursor flickers in different
  positions.
- **Current state: NEW.** Observed live in tmux; not covered by any test.
- **What remains:** reproduce live before fixing. Root cause unknown; candidates:
  (a) no focus-in/out handling, (b) a cursor position being re-asserted every
  render, (c) FTXUI cursor behaviour while the pane is unfocused. Reproduce under
  tmux with a second pane focused, then fix only once the cause is confirmed.
- **Effort:** S (≤0.5d, diagnosis-dominated). **Risk:** Med (root cause unknown;
  may be FTXUI-internal). **Deps:** none. **Priority:** P1.
- **GATE:** none. UI-local, reversible, user-visible; no spec gate.

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
11. **RB-04** supervisor-owned daemons (L; spec 16 implemented).

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

1. **RB-04 — daemon ownership.** **Resolved** by `16-daemon-ownership.md`
   (verified + implemented, 8 waves `036e4e4e1`…`065e221d9`); top-level
   architectural change.
2. **RB-05 — `/skills`.** **Resolved** by `20-skills.md` (verified; implemented
   `6761e62ef`): skill model, discovery, storage, invocation, context injection.
3. **RB-09 — JSONC config.** **Resolved** by `21-config-jsonc-errata.md`
   (verified Rev 5–8; JSONC only, TOML retired); implemented in `9db17cd54`.
4. **RB-03 — session rename.** **Resolved** by `19-session-rename-errata.md`
   (verified + implemented `162fa8641`): additive event + wire method.
5. **RB-02 — reasoning capture.** **Resolved** by `17-ui-transcript-errata.md`
   (verified; shipped `71dda4f16`).
6. **RB-06 — `/context`.** **Resolved** by `18-context-errata.md` (verified +
   implemented `edd696c6e`): assembled-context snapshot interface.

Items **RB-01, RB-07, RB-08, RB-10, RB-11** are UI-local, reversible, and do not
require a spec gate.

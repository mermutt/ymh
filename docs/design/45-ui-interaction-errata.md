# 45 — UI Interaction Errata: Input History, Command List, Switcher/Sessions Subset, `/mcp`, `/status`, Agent Cycling, and the Unmodeled-Session Lockout

```
Status: **verified (Rev 6)** — independent gate PASS (0 HIGH / 0 MEDIUM; 1 LOW
        recorded). This spec amends the owning specs; it introduces no new
        component. Rev 6 closes the re-gate's 1 MEDIUM + 2 LOW: the
        shared-function dedupe is WITHDRAWN and replaced by a shared JSON SCHEMA
        implemented by two serializers (the rationale given in early Rev 6 drafts
        was inaccurate — `ymh_agent` DOES reach `ymh_mcp` transitively via
        `ymh::config`, so a shared builder *could* compile; the host-side
        serializer is kept local for cohesion, not necessity); `ContextServerEntry` is unchanged
        (no `connected` field) and `/status` derives `connected` from
        `state == "ready"`; D9.3 no longer claims an event-driven agent projection;
        and D10.7's migration list covers the renderer reads and the test
        assignments. Rev 5 closed 3 MEDIUM; Rev 4 closed 4 HIGH + 6 MEDIUM; Rev 3
        closed the re-gate MEDIUM; Rev 2 closed the independent gate's 2 HIGH + 10
        MEDIUM + LOW. No decision number changed meaning.
Component: 45 (errata) — amends 10-supervisor-tui.md, 15-mcp-adapter.md,
           17-ui-transcript-errata.md, 18-context-errata.md,
           22-switcher-sessions-errata.md, 25-ui-ux-errata.md, 42-agent-presets.md
Depends on: 00-architecture.md (verified), 05-transport.md (verified),
            10-supervisor-tui.md (verified), 15-mcp-adapter.md (verified),
            17-ui-transcript-errata.md (verified),
            18-context-errata.md (verified),
            22-switcher-sessions-errata.md (verified),
            25-ui-ux-errata.md (verified), 42-agent-presets.md (verified)
Related:    18-context-errata.md (§4 `/context`; M8 no-`last_error`-on-wire),
            20-skills.md (skills.list wire template), 24-agent-lifetime-errata.md
Method:     every "current state" claim below is verified against the shipped tree
            with `file:line`; spec sections are cited only for pinned decisions.
            No implementation is written here. The spec author does not touch git.
```

---

## 1. Purpose, scope, and supersession map

### 1.1 The ten requirements (verbatim, from `requirements_draft.txt`)

1. Arrow up/down cycles through the list of previous commands/prompts (input
   history — everything entered, in order).
2. Pressing `/` shows the command list; ArrowUp/ArrowDown move through it. Tab
   must NOT be used for navigation; Tab must COMPLETE the currently-selected
   command.
3. The Ctrl+S switcher on a fresh start shows a non-empty list that is currently
   LONGER than `/sessions`. It must be strictly a SUBSET of `/sessions` (it shows
   only live daemons; `/sessions` shows all stored sessions regardless of a live
   daemon).
4. Remove/hide the current active session from BOTH the Ctrl+S switcher list and
   the `/sessions` list.
5. Esc does not dismiss the command list — only deleting `/` does. Required:
   press `/` → list appears; press Esc → it disappears. Both ways must work.
6. Implement a `/mcp` command to "manage mcp servers" — connected, not
   connected, how many tools available (like Claude Code's `/mcp`).
7. Implement a `/status` command to "Show ymh status, including version, model,
   API connectivity and tool statuses".
8. `/quit` exits (it is an alias of `/exit`) but is not shown in the command
   list. It must appear exactly as `/exit(quit) - quit the supervisor` (i.e.
   render aliases in the displayed command name, matching the user's literal
   string).
9. Tab switches to the next available agent; Shift+Tab the previous. This
   applies ONLY when `/` was NOT pressed (i.e. when the command list is not
   active).
10. Bug: after switching to a session (via Ctrl+S or `/sessions`) and pressing
    Enter to activate it, typing a prompt yields "no active session". It must
    ALWAYS be possible: whatever session the user switched to and then typed into
    must be made alive and continue that conversation. Related bug: after that
    "no active session" error, typing `/` does nothing — an unrecoverable UI
    state; the UI must never become stuck.

**Authoritative clarification (user).** Ctrl+D stays EXIT (with the confirmation
prompt). The session switcher is Ctrl+S / Ctrl+P. Items 3, 4, and 10 refer to the
Ctrl+S switcher, NOT Ctrl+D. This spec honors that: `begin_exit(/*allow_prompt=*/true)`
on Ctrl+D (`src/ui/supervisor.cpp:2212-2215`) is untouched, and 25-D10 is retained.

### 1.2 What this changes, in one sentence

Input history keeps covering both prompts and commands and gains a
list-vs-history arrow precedence; `/` opens a command list that ArrowUp/Down
navigate and Tab completes (Tab no longer cycles); the Ctrl+S Live switcher
becomes a strict subset of `/sessions` and both hide the focused session; Esc
hides the command list without clearing the draft; two new informational commands
`/mcp` and `/status` land with one new read-only daemon RPC (`mcp.status`); `/quit`
renders as `/exit(quit) - quit the supervisor`; Tab/Shift+Tab cycle the agent when
the command list is inactive (with two new agent RPCs); and the unmodeled-session
lockout that produces "no active session" and then swallows every keystroke is
fixed at the model and input-router level.

### 1.3 Supersession map

#### 1.3.1 Superseded

| ID | Prior text | Change |
|---|---|---|
| 45-S1 | `17-ui-transcript-errata.md` §5 RB-08 items 1–4 (`:266-284`): Tab freezes a `CompletionCycle`, types `names[index]`, and advances on repeated Tab/Shift+Tab; a first multi-match Tab inserts the longest common prefix. Shipped at `src/ui/supervisor.cpp:1695-1737` (`input.draft = "/" + cycle->names[applied]` at `:1729`). | **Superseded by 45-D2.** Tab is no longer a navigation/cycle key. Tab completes the currently-selected command. ArrowUp/ArrowDown move the selection. `CompletionCycle` and `InputModel::completion` are retired. |
| 45-S2 | `25-ui-ux-errata.md` 25-D8 (`:1326-1337`): "highlight tracks the inserted candidate" (`state.command_hint_selected = applied` at `src/ui/supervisor.cpp:1734`). | **Superseded by 45-D2.** There is no inserted candidate to track: the highlight is moved explicitly by ArrowUp/ArrowDown and consumed by Tab. The 25-D8 arithmetic is dead once the cycle is gone. |
| 45-S3 | `25-ui-ux-errata.md` 25-D12 (`:1475-1505`): `/help` renders aliases as `" (alias: /" + alias + ")"`, producing `  /exit  quit the supervisor (alias: /quit)`; `complete` matches canonical names only. | **Superseded by 45-D8.** The displayed command name is `name(alias1,alias2)`, so the row is exactly `/exit(quit) - quit the supervisor`. `complete` still matches canonical names only (retained). |
| 45-S4 | `22-switcher-sessions-errata.md` 22-S1/22-S2 (`:132-133`) as refined by 22 §3.1 (`:279-310`): the Live source renders a workspace iff `live && daemonStatus ∈ {Attached, Stopping}`; its session leaves are the live `SessionCell`s. | **Amended by 45-D3.** The Live source additionally requires each session leaf to be present in the latest `/sessions` catalog snapshot. Live stays a subset of History by construction. The workspace predicate is retained. |
| 45-S5 | `10-supervisor-tui.md` §7.2 (`:877-880`): `toggleExpand()` is bound to Tab in the switcher. | **Retained** (the switcher overlay is a different focus owner, U8; 45-D2/45-D9 apply to the composer only). Recorded here so the two Tab meanings are not conflated. |

#### 1.3.2 Amended

| ID | Amended clause | Change |
|---|---|---|
| 45-A1 | `10-supervisor-tui.md` §9.2 (`:1058-1060`): `Up`/`Down` history, `Tab` completion, `Esc` cancel popup. | Arrow precedence pinned (45-D1); Tab completion redefined (45-D2); Esc pinned (45-D5); Tab/Shift+Tab agent cycling added (45-D9). |
| 45-A2 | `10-supervisor-tui.md` §9.3 (`:1064-1070`): per-session `InputModel` draft/cursor/history. | `InputModel` loses `completion`/`CompletionCycle` (45-D2); the per-session `SessionUiState` gains `hints_dismissed` (45-D5). |
| 45-A3 | `10-supervisor-tui.md` §7.1 (`:849-873`) and `22` §3: Live-source session membership. | Focused-session exclusion (both sources) + Live ⊆ History (45-D3). **No node suppression** (Rev 4): every eligible workspace node renders (live nodes are focusable; non-live History nodes are read-only, surfacing the existing `workspace not running` notice on Enter); empty leaf lists render an explicit placeholder (`(current session hidden)` / `(no live sessions)` / `(no stored sessions)` / `note` leaf) (45-D4). **`22 §4.3`/`§4.4` are retained, not superseded**: the History source keeps the `(no stored sessions)` empty-state leaf and the degradation note leaf (`src/ui/ui_render.cpp:773-778`; `src/ui/ui_model.cpp:1052-1069`). |
| 45-A4 | `10-supervisor-tui.md` §9.2 keybinding namespace (`:1044-1062`) and `25-ui-ux-errata.md` §7 (`:1466-1506`, the command-registration/alias decision). | `/mcp` and `/status` registered (45-D6, 45-D7); `/exit` display name (45-D8). |
| 45-A5 | `15-mcp-adapter.md` §4.7 (`:857-912`) and §12.3 AM-1 (`:919-945`, `:2010-2032`): MCP status is a bounded `HostNotice` string only. | Additive: a read-only `mcp.status` RPC exposes the structured `McpManager::statuses()` snapshot (45-D6). The bounded `HostNotice` path is retained. |
| 45-A6 | `05-transport.md` §7.3 method catalog (as extended by 20/18) and the profile gate (`src/transport/protocol.cpp:664-670`). | Additive methods `mcp.status`, `agent.list`, `agent.select` (45-D6, 45-D9); `protocol.hpp:495-530` catalog grows. `agent.select` is added to the Automation deny list (`MethodNotAllowedForProfile`); the read-only two stay allowed in both profiles (45-I29). |
| 45-A7 | `42-agent-presets.md` §3.2 (`:525-585`) and §2.3 (`:349-393`): `select` is an in-process call; the spec pins no RPC and never mentions the supervisor. | Additive: the roster is wired into the daemon and exposed via `agent.list`/`agent.select` (45-D9); `agent.list` surfaces the daemon-owned `blank`/`can_select` (45-D9.3). The blank-session predicate 42-I3/42-D4 is retained verbatim and never re-derived in the UI. |
| 45-A8 | `10-supervisor-tui.md` §4.1/§4.3: `UiModel`/`SessionUiState`. | Additive: `UiModel::ensureActiveSession`, `UiModel::focusSessionIn` (the single `activeSessionId` mutator), `SessionUiState::hints_dismissed`, `StatusModel::api_state`/`agent`/`pending_agent`, `CommandHint::display` (45 §9). |
| 45-A9 | `10-supervisor-tui.md` §6 (status line) and §9.3: no runtime connectivity state exists. | A derived `ApiConnectivity` is pinned (45-D7); no new LLM call is introduced and spec 08's provider interface is unchanged. |
| 45-A10 | `22-switcher-sessions-errata.md` §5.2 (`:1298-1397`): the resume/focus success path models the session in `apply_resume_success` (`src/ui/supervisor.cpp:1014-1026`) but `SupervisorApp::activate_session` (`src/ui/supervisor.cpp:1175-1184`) sets `activeSessionId` without modeling. | `activate_session` delegates to `focusSessionIn` (which models via `ensureSessionIn`); `handle_input`/`dispatch_command` self-heal and `UnknownSession` is recovered (45-D10). The 22 §5.2 lazy-resume semantics themselves are retained. |
| 45-A11 | `18-context-errata.md` §4 (`context.show`'s `mcp_servers`; `ContextServerEntry`). | Additive: `mcp.status` and `context.show` share a **pinned JSON schema** (not a shared function — `ymh_agent` does not link `ymh_mcp`, so no MCP-type builder may live there): the base object `{id,state,tool_count,skipped:<int>,has_error}`, which `mcp.status` extends with `connected` and `skipped_tools`. `context.show`'s serializer and `ContextServerEntry` are **unchanged** (no `connected` field); `/status` derives `connected` from `state == "ready"` (45-D6.9, 45-D7.5, 45-I30). |

#### 1.3.3 Retained (explicitly not changed)

- Ctrl+D → exit-with-confirmation (25-D10, `src/ui/supervisor.cpp:2212-2215`).
- Ctrl+S = Ctrl+P = switcher (`src/ui/supervisor.cpp:2207-2211`); Ctrl+P opens the
  same Live overlay (it does not open a separate "command palette").
- The switcher overlay's own keybindings (Tab expands, j/k navigate, Esc closes)
  (`src/ui/supervisor.cpp:1901-1941`; `10 §7.2`).
- The `/sessions` History source semantics: every registered workspace, read from
  disk, live or not (22 §4), **including** its empty-state and degradation
  rendering: `(no stored sessions)` and the `note` leaf (`22 §4.3`/`§4.4`,
  `src/ui/ui_render.cpp:773-778`). 45-D4 does **not** suppress those; it only
  removes the focused session leaf. When the exclusion empties a node that the
  catalog read did populate, the renderer draws the distinct
  `(current session hidden)` label instead of the false `(no stored sessions)`
  (45-D4.3). Neither source suppresses a node, and both render an explicit
  placeholder when a leaf list is empty (`src/ui/ui_render.cpp:740-747`,
  `:773-778`).
- `McpServerStatus::last_error` is never shipped on the wire (18-M8,
  `include/ymh/agent/context_snapshot.hpp:59-61`).
- Spec 42's blank-session-only switch (42-I3/42-D4, `docs/design/42-agent-presets.md:349-393`).

### 1.4 Scope boundaries

**In scope.** Composer key routing, the command-list model/renderer, the
switcher/session-catalog membership predicates, two read-only informational
commands, one read-only MCP RPC, two agent RPCs, the alias display string, and the
unmodeled-session repair path.

**Out of scope (recorded, not pinned).**
- MCP **mutations** (enable/disable/reconnect/auth). `/mcp` here is read-only,
  matching Claude Code's informational `/mcp`. A mutation surface needs its own
  spec and RPCs.
- A live LLM **probe**. Connectivity is derived from observed outcomes (45-D7);
  a real probe would add a billed call and is deferred.
- Agent switching for a non-blank session. Spec 42 forbids it (42-D4); 45-D9
  pins the next-session path instead.
- Remote transport (out of scope project-wide, `AGENTS.md`).

### 1.5 Terminology (pinned)

- **Command list** = the multi-row `SessionUiState::command_hints` block rendered
  by `render_command_hints` (`src/ui/ui_render.cpp:361-383`). It is shown iff
  `!command_hints.empty()`. "Active" means non-empty and not dismissed (45-D5).
- **Bare command prefix** = `draft.front() == '/'` and `draft` contains no space
  or tab (`src/ui/supervisor.cpp:1679-1682`, `:1698-1699`).
- **Focused session** = `UiModel::activeWorkspace()->activeSessionId`
  (`src/ui/ui_model.cpp:426-432`). It is supervisor-local, never registry state
  (03 R12, 22 §1.5).
- **Modeled session** = a `SessionId` present in `UiModel::sessions`
  (`src/ui/ui_model.cpp:442-445`). A session can have a `SessionCell` (switcher
  leaf) without being modeled — see 45-D10.
- **Live source** = `SwitcherSource::Live` (Ctrl+S/Ctrl+P). **History source** =
  `SwitcherSource::History` (`/sessions`) (`include/ymh/ui/ui_model.hpp:300-305`).
- **Connectivity** = the derived `ApiConnectivity` of 45-D7, never a live probe.

---

## 2. Amendment register

| ID | Decision | Amends |
|---|---|---|
| 45-D1 | History holds prompts + commands; Arrow precedence: command list first, history otherwise | 10, 17 |
| 45-D2 | `/` list; ArrowUp/Down navigate; Tab completes the selected; `CompletionCycle` retired | 10, 17, 25 |
| 45-D3 | Live ⊆ History: a Live session leaf requires catalog membership | 10, 22 |
| 45-D4 | Focused session excluded from both sources; **no node suppression** — explicit placeholders (22 §4.3/§4.4 retained) | 10, 22 |
| 45-D5 | Esc hides the command list without clearing the draft; `hints_dismissed` | 10, 17 |
| 45-D6 | `/mcp` read-only + `mcp.status` RPC (shared JSON schema, two serializers; `skipped` count + `skipped_tools`; both profiles; MethodNotFound degradation) | 10, 15, 05, 18 |
| 45-D7 | `/status` command; version seam; effective model; derived connectivity; per-tool statuses | 10, 18 |
| 45-D8 | Alias display name `name(alias)`; `/exit(quit) - quit the supervisor` | 10, 25 |
| 45-D9 | Tab (empty draft) agent cycle + `agent.list` (blank/can_select) / `agent.select` (Interactive-only) + roster wiring | 10, 42, 05 |
| 45-D10 | Model-on-focus + `handle_input` self-heal + `ensureActiveSession` + single-mutator `focusSessionIn` + `UnknownSession` recovery (the lockout fix) | 10, 22 |

---

## 3. D1 — Input history holds prompts and commands (item 1)

### 3.1 Current state (verified)

- `InputModel` carries `history`/`history_pos`/`saved_draft`
  (`include/ymh/ui/ui_model.hpp:161-175`). `push_history` de-duplicates only
  against the immediately previous entry (`src/ui/ui_model.cpp:192-203`);
  `history_up`/`history_down` walk the vector (`:205-228`).
- `submit()` pushes **every** non-empty submitted prompt:
  `state->input.push_history(text)` at `src/ui/supervisor.cpp:386`, inside
  `submit` (`:376-401`). It is reached from the `Return` handler at `:1973`.
- The command path pushes **separately**: `input.push_history(text)` at
  `src/ui/supervisor.cpp:1963`, inside the `dispatch_command(text)` branch
  (`:1962-1972`).
- Therefore history already contains **both** prompts and commands, in order.
  The task's suspicion ("only the command path calls `push_history`") is
  **false**: both call it. No storage change is needed for item 1.

The only real gap is **precedence**: today ArrowUp/ArrowDown *always* recall
history (`src/ui/supervisor.cpp:2019-2032`) with no `command_hints` branch, so
the list cannot be navigated.

### 3.2 Decision (45-D1)

1. **Storage (retained).** History contains both prompts and commands in
   submission order, de-duplicated only against the immediately previous entry.
   `submit()` keeps pushing prompts (`:386`); the command branch keeps pushing
   commands (`:1963`). No new field.
2. **Precedence (new).** In `handle_input`:

```text
ArrowUp / ArrowDown:
    if command_list_active(state)        -> move_hint_selection(state, -1/+1); return true
    else                                 -> history_up()/history_down(); refresh_hints(state)
```

   `command_list_active(state)` is `!state.command_hints.empty() &&
   !state.hints_dismissed` (45-D5). Moving the selection wraps modulo the list
   size; it never mutates `input.draft` and never touches history.
3. **History recall rebuilds hints.** When the list is inactive, history recall
   keeps calling `refresh_hints` (as today, `:2021`, `:2028`) so recalling a
   command repopulates its list; recall does **not** clear `hints_dismissed`
   (45-D5.2).

**Owning spec amended:** `10-supervisor-tui.md` §9.2/§9.3 (`:1058-1070`).
**Test:** `UI45_D1_HistoryHoldsPromptsAndCommands`,
`UI45_D1_ArrowPrecedenceListVsHistory` (45 §10).

---

## 4. D2 — Command list: `/` opens it, arrows navigate, Tab completes (item 2)

### 4.1 Current state (verified)

- There is **no `/` keybinding**. `/` is detected by content: `event.is_character()`
  inserts and calls `refresh_hints` (`src/ui/supervisor.cpp:2046-2052`).
- `refresh_hints` (`:1672-1686`) clears `command_hints`, and if the draft is a
  bare `/prefix` fills it from `registry_.complete(prefix)` (`:1683-1685`).
  `complete` matches canonical names by prefix only (`src/ui/command_registry.cpp:73-82`).
- `command_hints`/`command_hint_selected` live on `SessionUiState`
  (`include/ymh/ui/ui_model.hpp:227-231`); the renderer prefixes `"/"` and dims
  the description (`src/ui/ui_render.cpp:361-383`).
- ArrowUp/Down do **not** move the list (`src/ui/supervisor.cpp:2019-2032`).
- Tab is bound to `complete_command` (`:1976-1989`), which **types** the selected
  candidate into the draft and advances the cycle (`:1729-1732`), i.e. Tab is the
  navigation key today. `CompletionCycle` is `include/ymh/ui/ui_model.hpp:153-159`.

### 4.2 Decision (45-D2)

1. **Opening (retained).** Typing `/` at the start of an empty draft, or editing a
   bare `/prefix`, populates `command_hints` via `refresh_hints` and resets
   `command_hint_selected = 0`. Pressing `/` when the draft is already a bare
   prefix (producing `//`) yields zero matches and an empty list — that is the
   pinned "no match" behavior, unchanged.
2. **Navigation (new).** ArrowUp/ArrowDown move `command_hint_selected` when the
   list is active (45-D1.2). Navigation wraps and never edits the draft.
3. **Completion (new).** Tab, when the list is active, completes the selected
   command:

```cpp
// src/ui/supervisor.cpp — replaces complete_command's cycle branch
bool complete_selected_command(SessionUiState& state) {
    const std::string& draft = state.input.draft;
    if (state.command_hints.empty()) {
        // 45-D2.3 / 45-D5.5: after Esc the list is hidden; recompute the matches
        // from the bare `/prefix` and complete index 0.
        if (draft.empty() || draft.front() != '/' ||
            draft.find_first_of(" \t") != std::string::npos) {
            return false;
        }
        const std::vector<const Command*> matches = registry_.complete(draft.substr(1));
        if (matches.empty()) return false;
        state.input.draft  = "/" + matches.front()->name + " ";   // terminal space
        state.input.cursor = state.input.draft.size();
        state.command_hint_selected = 0;
        return true;
    }
    const std::size_t count = state.command_hints.size();
    const std::size_t index = std::min(state.command_hint_selected, count - 1);
    state.input.draft  = "/" + state.command_hints[index].name + " ";  // terminal space
    state.input.cursor = state.input.draft.size();
    state.command_hints.clear();
    state.command_hint_selected = 0;
    return true;
}
```

   - Tab completes with a **trailing space** (the pinned terminal form of the
     original RB-08 unique-match case, `17 §5` item 1).
   - If the list is **hidden** (Esc'd, 45-D5) but the draft is a bare `/prefix`
     with matches, the sketch's first branch recomputes the matches and completes
     index 0. This keeps Tab useful after Esc.
   - If the draft is not a bare `/prefix`, Tab is a no-op; agent cycling is
     **empty-draft-only** (45-D9.1), so a non-empty draft never cycles the agent.
   - **Shift+Tab while the list is active is a no-op** (returns `false`). The
     user pinned only Arrow keys for list navigation; Shift+Tab belongs to agent
     cycling (45-D9).
4. **Retirement.** `InputModel::completion` (`std::optional<CompletionCycle>`) and
   the `CompletionCycle` type are **removed**. `accept_highlight` (25-D9, Enter
   accepts the highlight) is retained; the highlight is now set only by
   ArrowUp/Down and reset by `refresh_hints`.
5. **Both `CommandHint` construction sites are pinned.** `CommandHint` gains
   `display` (45-D8), so the two sites that build it MUST pass
   `{command->name, command_display_name(*command), command->description}`:
   - `refresh_hints` (`src/ui/supervisor.cpp:1683-1685`, currently
     `CommandHint{command->name, command->description}`), and
   - `set_command_hints` (`src/ui/supervisor.cpp:1744-1745`, currently
     `CommandHint{command->name, command->description}`).
   Both are pinned; a missed site would silently assign the description into
   `display` and shift every field.

**Explicit supersession.** This supersedes `17 §5` RB-08 items 1–4
(`docs/design/17-ui-transcript-errata.md:266-284`) and 25-D8
(`docs/design/25-ui-ux-errata.md:1326-1337`) by reference; see 45-S1/45-S2.

**Owning specs amended:** `10` §9.2, `17` §5, `25` D8/D9.
**Tests:** `UI45_D2_ArrowMovesSelection`, `UI45_D2_TabCompletesSelected`,
`UI45_D2_TabIsNotNavigation`, `UI45_D2_TabAfterEscRecomputes`,
`UI45_D2_BothCommandHintSitesPinned`, `UI45_D2_CompletionCycleRetired`.

---

## 5. D3 — The Ctrl+S switcher is a strict subset of `/sessions` (item 3)

### 5.1 Current state (verified)

- Ctrl+S/Ctrl+P call `model_.openSwitcher()` (`src/ui/supervisor.cpp:2207-2211`),
  which sets `source = Live` and builds nodes from `UiModel::workspaces`
  (`src/ui/ui_model.cpp:932-937`). `SwitcherOverlayModel::open` emits one
  `SessionNode` per `WorkspaceModel::sessions` `SessionCell`
  (`src/ui/ui_model.cpp:978-989`).
- The Live renderable predicate is `live && daemonStatus ∈ {Attached, Stopping}`,
  shipped as `live_switcher_renderable` (`src/ui/ui_model.cpp:357-360`; `22 §3.1`).
- `/sessions` sets `source = History` and calls `openHistory`
  (`src/ui/supervisor.cpp:861-870`), which builds nodes from
  `UiModel::catalog` — the disk-read `SessionCatalogModel`
  (`include/ymh/ui/ui_model.hpp:421-432`, `session_catalog.hpp:33-64`).
- The Live leaves therefore come from the **daemon's live cell set**, while the
  History leaves come from the **disk catalog**. They are independent; a session
  created/prompted in the daemon can be a Live leaf before (or without) the
  catalog snapshot containing it. Hence the observed "Live longer than History".

### 5.2 Decision (45-D3)

**Pinned membership rule (Live ⊆ History).**

```text
Live source: render SessionNode(s) for workspace W iff
    live(W) && daemonStatus(W) ∈ {Attached, Stopping}
    AND the session id is present in
        catalog.workspaces[W].sessions  (the latest SessionCatalogModel snapshot)
    AND the session id is not the focused session (45-D4).

History source: render SessionNode(s) from catalog.workspaces[W].sessions
    (minus the focused session), for every registered workspace. The 22 §4.3
    empty-state leaf and §4.4 note leaf are RETAINED when no leaf survives.
```

Consequences, pinned:

1. `UiModel::catalog` is the single membership authority for the Live source. A
   helper `bool UiModel::catalog_has_session(const WorkspaceId&, const SessionId&) const`
   is added (45 §9).
2. On `Ctrl+S`/`Ctrl+P`, the supervisor calls `catalog_->refreshNow()` (the same
   call `/sessions` makes at `src/ui/supervisor.cpp:868`) and re-snapshots the
   Live switcher when the next catalog generation lands. The switcher is a
   snapshot (`22 §3.3`); the resnapshot path is `resnapshot_switcher`
   (`src/ui/supervisor.cpp:877-882`), extended to also fire on a catalog
   generation change while `source == Live`.
3. If no catalog snapshot has been delivered yet or the workspace's catalog read
   failed, the Live source renders the workspace node with **zero session
   leaves** (and the 45-D4.3 placeholder leaf). The "not yet delivered" signal is
   `SessionCatalogModel::loaded == false` (`include/ymh/ui/ui_model.hpp:427`, the
   field the shipped renderer already tests at `src/ui/ui_render.cpp:741`) or,
   equivalently, `SessionCatalogModel::generation == 0`
   (`include/ymh/ui/ui_model.hpp:430`). A catalog read failure is
   `WorkspaceHistory::note.has_value()` (`include/ymh/ui/session_catalog.hpp:52`).
   `SwitcherOverlayModel::open` propagates both signals onto the Live node
   (`WorkspaceNode::catalog_pending`, `WorkspaceNode::note`) so 45-D4.3 can render
   `(loading live sessions…)` / the `note` leaf instead of a false
   `(no live sessions)`. No node is suppressed; the History source is unaffected.
4. A session present in the daemon but absent from the snapshot is **hidden**
   until the next snapshot — never rendered in Live. This is the subset guarantee.

**Why this is safe.** A live workspace has a registry row (it is in
`UiModel::workspaces` only after `attach_workspace`, `src/ui/supervisor.cpp:713-728`,
which is fed by the registry-backed scan). `/sessions` enumerates every registered
workspace (`22 SW9`), so every Live workspace is in the catalog domain. A
successful daemon session is persisted (session row + event log), so the catalog
read will contain it once refreshed. The residual "not yet refreshed" window
hides the leaf rather than violating the subset rule.

**Owning specs amended:** `22` §3 (S1), `10` §7.1.
**Tests:** `UI45_D3_LiveIsSubsetOfHistory`,
`UI45_D3_LiveHidesUncataloguedSession`, `UI45_D3_CatalogRefreshOnOpen`.

---

## 6. D4 — Hide the focused session from both lists (item 4)

### 6.1 Current state (verified)

- The focused session is `UiModel::activeWorkspace()->activeSessionId`
  (`src/ui/ui_model.cpp:426-432`); `focusSession` sets it
  (`src/ui/ui_model.cpp:947-962`); `activate_session` sets it from a daemon reply
  (`src/ui/supervisor.cpp:1175-1184`).
- `SwitcherOverlayModel::open`/`openHistory` render **every** `SessionCell` /
  catalog entry (`src/ui/ui_model.cpp:978-989`, `:1052-1069`), including the
  focused one.
- Empty Live nodes are not currently suppressed; the History source renders a
  `(no stored sessions)` leaf or its `note` leaf when a workspace has no sessions
  (`src/ui/ui_render.cpp:773-778`).

### 6.2 Decision (45-D4)

1. **Which session.** The excluded id is exactly
   `model.activeWorkspace()->activeSessionId` (the focused session). It is read
   once when the switcher snapshot is built (both sources). If no workspace is
   active or `activeSessionId` is empty, nothing is excluded.
2. **Both sources.** The exclusion is applied inside `SwitcherOverlayModel::open`
   (Live) and `openHistory` (History), so Ctrl+S and `/sessions` cannot diverge.
   It is a render-time filter; the underlying `SessionCell`/catalog data is
   untouched.
3. **No suppression; explicit placeholders instead (HIGH fix).** Neither source
   suppresses a rendered workspace node. Every eligible workspace node is
   rendered; a live/`Attached` node is focusable and a non-live History node
   is read-only (45-D4.5). The focused session is removed
   from the leaf list; an empty leaf list renders an explicit placeholder, so the
   switcher and `/sessions` are never silently empty:
   - **Live source** (eligible = `live && daemonStatus ∈ {Attached, Stopping}`):
     - `catalog_pending` (no snapshot yet: `!catalog.loaded || generation == 0`)
       → `(loading live sessions…)`.
     - `note.has_value()` (the workspace's catalog read failed, so membership is
       unknown) → the 22 §4.4 degradation note leaf.
     - `sessions_hidden_by_focus == true` → `(current session hidden)`.
     - else → `(no live sessions)`.
   - **History source** (eligible = every registered workspace):
     - `note.has_value()` → the 22 §4.4 degradation note leaf (precedence: a
       `note` means the read produced no sessions, so the all-hidden case cannot
       co-occur with it).
     - else `sessions_hidden_by_focus == true` → `(current session hidden)`.
     - else → `(no stored sessions)` (22 §4.3).

   **Live truthfulness (MEDIUM fix).** `(no live sessions)` is emitted **only**
   when the catalog is loaded, the workspace read succeeded, and the source
   genuinely has no (non-focused) live session. It is never used while the
   snapshot is pending or the read failed — the Live node's `note`/`catalog_pending`
   carry that state (45-D3.3). Mechanism (pinned): `WorkspaceNode` gains
   `bool sessions_hidden_by_focus = false;` and `bool catalog_pending = false;`
   (`include/ymh/ui/ui_model.hpp:307-319`). `open`/`openHistory` set
   `sessions_hidden_by_focus` iff they removed ≥ 1 leaf by 45-D4 **and** the
   result is empty; `open` also sets `catalog_pending` from
   `!model.catalog.loaded || model.catalog.generation == 0` and copies the
   workspace's `WorkspaceHistory::note` (`include/ymh/ui/session_catalog.hpp:52`)
   onto the node so Live can render the degradation leaf. The renderer branch
   replaces the shipped history-only branch (`src/ui/ui_render.cpp:773-778`):

   ```text
   if (workspace.sessions.empty())
       if (!history && workspace.catalog_pending)   -> "(loading live sessions…)"
       else if (workspace.note.has_value())         -> note leaf  (22 §4.4)
       else if (workspace.sessions_hidden_by_focus) -> "(current session hidden)"
       else if (history)                            -> "(no stored sessions)"  (22 §4.3)
       else                                         -> "(no live sessions)"
   ```

   **Whole-list empty placeholder (pinned).** The shipped placeholder
   (`src/ui/ui_render.cpp:740-747`) is retained: `(no workspaces)` for Live;
   `loading stored sessions…` before the first History snapshot, else
   `(no stored sessions)`. The switcher is therefore never an empty screen with
   no explanation. **The Live-only suppression from Rev 2 is WITHDRAWN**: a Live
   workspace whose only session is the focused one still renders its node (with
   the placeholder leaf), so D4.3 no longer conflicts with D4.4. 45-D4 does
   **not** supersede 22 §4.3/§4.4; it adds the distinct `(current session hidden)`
   label and the Live `(no live sessions)` placeholder.
4. **Fresh-start consequence (recorded).** Item 3's observation that the Ctrl+S
   list was "non-empty and longer than `/sessions`" is the bug being fixed. After
   45-D3/45-D4, a fresh start with a single focused session shows the live
   workspace node with the `(current session hidden)` placeholder — **not** an
   empty list, and never the false `(no stored sessions)`; `/sessions` shows the
   same workspace with the same distinct label. This is the intended, honest
   result of items 3 and 4 together, not a regression.
5. **No node is suppressed; focusability is per-node, not universal (HIGH fix
   corrected).** Enter on a **Live** workspace node focuses it
   (`src/ui/supervisor.cpp:1933-1935`). Enter on a **History** node goes through
   `select_history` (`src/ui/supervisor.cpp:1886-1899`), which focuses the
   workspace **only when its connection is `Attached`**; otherwise it pushes the
   existing notice `"workspace not running: <label>"` (`:1898`). Since `/sessions`
   renders non-live nodes (22 §4.3), the correct pinned statement is: **no
   rendered node is suppressed or inert; a live/Attached node is focusable, and a
   non-live History node is rendered read-only** with that truthful notice on
   Enter (the pre-existing 22 behavior, retained). The earlier blanket "every
   rendered node is focusable" claim was false for non-live History nodes and is
   withdrawn.

**Owning specs amended:** `22` §3 (Live membership), `10` §7.1. **`22 §4.3`/`§4.4`
retained**; the all-hidden case gets a distinct label, and both sources get an
explicit empty-state placeholder.
**Tests:** `UI45_D4_FocusedSessionExcludedLive`,
`UI45_D4_FocusedSessionExcludedHistory`, `UI45_D4_LiveEmptyNodeRendered`,
`UI45_D4_HistoryGenuinelyEmptyKeepsEmptyState`,
`UI45_D4_HistoryAllHiddenDistinctLabel`,
`UI45_D4_HistoryNoteLeafRetained`, `UI45_D4_LivePlaceholderLeaf`,
`UI45_D4_LiveNodeFocusableHistoryReadOnly`.

---

## 7. D5 — Esc hides the command list (item 5)

### 7.1 Current state (verified)

- `handle_input` (`src/ui/supervisor.cpp:1944-2055`) has **no** `Event::Escape`
  case. In Conversation mode Esc falls through to `return false` and is ignored.
  Esc is handled only by modals: exit-confirm (`:674`), dialog (`:1811`),
  switcher (`:1902`), context overlay (`:2137`).
- The list is rebuilt by `refresh_hints`, which is called from every edit/recall
  key: insert (`:2050`), Backspace (`:1995`), Delete (`:2002`), history
  (`:2021`, `:2028`), Ctrl+U (`:2035`), Ctrl+W (`:2042`).
- The RB-12 modal-tail suppression (`:1433-1438`, `kModalTailWindow = 25 ms` at
  `:63`) can swallow a printable character for 25 ms after a modal closes, but it
  is time-expiring and not the permanent lockout (that is 45-D10).

### 7.2 Decision (45-D5)

The user's two statements — "Esc does not dismiss the command list — only
deleting `/` does" and "press Esc → it disappears. Both ways must work" — are
reconciled as: **both dismissal paths work; Esc hides the list without clearing
the composer line.**

1. **Esc (new).** Add an `Event::Escape` case to `handle_input`, before the
   character-insert branch:

```cpp
if (event == ftxui::Event::Escape) {
    if (!state->command_hints.empty()) {
        state->hints_dismissed = true;   // 45-D5.2; no draft snapshot is needed
        state->command_hints.clear();
        state->command_hint_selected = 0;
        model_.dirty.mark(state->id, UiDirtyFlag::Input);
        return true;
    }
    return false;                                            // no list: Esc is a no-op
}
```

   Esc **never** mutates `input.draft` or `input.cursor`. It only hides the list.
2. **`hints_dismissed` (new).** `refresh_hints` returns early (leaving the list
   empty) while `state.hints_dismissed` is true. The flag is cleared only by an
   **edit to the command name**: character insert, Backspace, Delete, Ctrl+U,
   Ctrl+W. History recall (ArrowUp/Down) does **not** clear it. This is why the
   list cannot "simply reappear on the next keystroke": after Esc, pressing
   ArrowUp to browse history no longer rebuilds the list; only editing the prefix
   does. This is the concrete defect the flag fixes — without it, `refresh_hints`
   on any recall key would immediately resurrect the list.
3. **Deleting `/` (retained).** Backspace/Delete/Ctrl+U that removes the leading
   `/` makes `draft.front() != '/'`, so `refresh_hints` leaves the list empty
   (`:1676-1678`). This path is unchanged and also clears `hints_dismissed`
   (it is a prefix edit).
4. **Re-showing.** Typing `/` on an empty draft is a character insert, so it
   clears `hints_dismissed` and repopulates the list. Thus "press `/` → list
   appears" holds after any Esc.
5. **Tab after Esc.** Because Tab is not an edit, it does not clear the flag;
   `complete_selected_command` recomputes the matches from the draft when the
   list is hidden (45-D2.3), so Tab still completes. The list stays hidden.

**Owning specs amended:** `10` §9.2 (the `Esc cancel popup` binding is now
realized), `17` §5.
**Tests:** `UI45_D5_EscHidesList`, `UI45_D5_EscKeepsDraft`,
`UI45_D5_EscSurvivesHistoryRecall`, `UI45_D5_DeleteSlashHidesList`,
`UI45_D5_SlashReopensAfterEsc`.

---

## 8. D6 — `/mcp` and the `mcp.status` RPC (item 6)

### 8.1 Current state (verified)

- MCP state is **daemon-owned**. `McpManager` is constructed only in
  `WorkspaceRuntime::Impl` (`src/agent/workspace_runtime.cpp:143-149`, member
  `:226`), reachable via `WorkspaceRuntime::mcp_statuses()` (`:373-376`). The
  supervisor (`src/ui/`) contains no MCP include (verified by grep); spec 15 §5.1
  pins "the `WorkspaceHost` daemon owns every `McpClient`"
  (`docs/design/15-mcp-adapter.md:1031-1046`).
- `McpManager::statuses()` returns `std::vector<McpServerStatus>` in stable id
  order (`include/ymh/mcp/mcp_manager.hpp:67`; impl `src/mcp/mcp_manager.cpp:473-494`).
  `McpServerStatus` = `{id, state, server_name, server_version, protocol_version,
  tool_count, skipped_tools, last_error}` (`include/ymh/mcp/mcp_types.hpp:139-148`).
  `McpServerState` = `{Disabled, Starting, Ready, Degraded, Disconnected, Failed,
  Stopped}` (`:41-49`); `mcp_state_token` gives the wire token (`:56`).
- The **only** structured MCP inventory currently reaching the supervisor is
  `context.show`'s `ContextSnapshot.mcp_servers`
  (`src/host/host_runtime.cpp:526-529`, `include/ymh/agent/context_snapshot.hpp:86-96`),
  serialized as `{id, state, tool_count, skipped, has_error}`
  (`src/agent/context_snapshot.cpp:219-237`). It is rendered only by the
  `/context` overlay. The live `HostNotice` path stores `UiModel::mcp_status`
  (`include/ymh/ui/ui_model.hpp:455`) but **no renderer reads it**.
- The JSON-RPC catalog (`include/ymh/transport/protocol.hpp:495-530`) has **no**
  `mcp.*` method. The closest template is `skills.list`/`skills.show`
  (`:527-528`; dispatch `src/transport/protocol_server.cpp:535-543`; host virtuals
  `include/ymh/transport/host.hpp:114-117`; impl `src/host/host_runtime.cpp:980-993`).

### 8.2 Decision (45-D6)

**`/mcp` is read-only and informational** (Claude Code's `/mcp` is a listing).
Mutations are explicitly out of scope (§1.4).

**New RPC `mcp.status`** (workspace-scoped, read-only, allowed in both profiles):

```text
mcp.status   params: {}                       // no parameters
             result: {
               "servers": [
                 { "id":            "<server-id>",      // McpServerId.value
                   "state":         "<token>",          // mcp_state_token(state)
                   "connected":     <bool>,             // state == Ready
                   "tool_count":    <int>,              // >= 0
                   "skipped":       <int>,              // COUNT, not an array
                   "skipped_tools": [ "<tool-name>", ... ], // names (mcp.status only)
                   "has_error":     <bool>              // last_error non-empty
                 }, ...
               ],
               "tool_total": <int>                      // sum of tool_count
             }
```

**`skipped` is a COUNT; `skipped_tools` carries the names (MEDIUM fix).** The
common `context.show`/`mcp.status` shape uses `"skipped": <int>` — the existing
`ContextServerEntry::skipped` is `std::size_t`
(`include/ymh/agent/context_snapshot.hpp:57`), serialized as a number
(`src/agent/context_snapshot.cpp:224`), deserialized with `get<std::size_t>()`
(`:278`), and rendered as `skipped=N` (`src/ui/ui_render.cpp:1042-1043`; pinned by
`docs/design/18-context-errata.md:1406-1407`). Only `mcp.status` adds the
`skipped_tools` string array. `context.show` MUST NOT gain `skipped_tools`; its
`skipped` stays a count, so `/context`'s `server.skipped > 0` render is
unchanged.

Pinned details:

1. **Params.** `params` MUST be absent or an empty object. Any other shape →
   `RpcException(RpcCode::InvalidParams, "mcp.status takes no parameters")`.
   (Compare `skills.list`, which ignores params; `mcp.status` is strict to keep
   the seam unambiguous.)
2. **No session/workspace param.** The method is dispatched on the caller's
   connection, which is already workspace-scoped (`protocol::HostConnection`).
3. **`state` tokens** are exactly `mcp_state_token()` outputs
   (`include/ymh/mcp/mcp_types.hpp:56`): `disabled`, `starting`, `ready`,
   `degraded`, `disconnected`, `failed`, `stopped`.
4. **`connected`** is `state == Ready`. `Degraded` is **not connected** (spec 15
   pins Degraded as non-callable: `docs/design/15-mcp-adapter.md:193-195`).
5. **`last_error` is never shipped.** Only `has_error` (18-M8,
   `include/ymh/agent/context_snapshot.hpp:59-61`). `server_name`/`server_version`
   are deliberately **not** shipped either, to avoid widening the disclosure
   surface beyond what 18 already ships; the human-readable identity is the
   configured `id` (human-authored by grammar, spec 15 §2.1).
6. **No MCP configured / no manager** is not an error: the result is
   `{"servers": [], "tool_total": 0}`. `WorkspaceRuntime::mcp_statuses()`
   already returns an empty vector when `mcp_ == nullptr`
   (`src/agent/workspace_runtime.cpp:373-376`).
7. **There is no `note` field.** An earlier draft proposed one, but `McpManager`
   has no such concept (`src/mcp/mcp_manager.cpp:473-494`) and no trigger could be
   pinned, so it is removed rather than left untestable. A future "MCP disabled by
   config" notice, if needed, is a separate additive field with its own spec.
8. **Ordering** is stable by `id` (`McpManager::statuses()` guarantee,
   spec 15 §4.7 `:891`).
9. **Deduplication with `context.show` — a shared SCHEMA, not a shared function
   (MEDIUM fix, corrected).** `context.show` already embeds MCP servers under
   `mcp_servers` (`src/host/host_runtime.cpp:526-529`; `ContextServerEntry`,
   `include/ymh/agent/context_snapshot.hpp:54-62`). A single shared builder is
   **not buildable and is withdrawn**: `ymh_agent` (which compiles
   `context_snapshot.cpp`, `CMakeLists.txt:441`) does **not** link `ymh_mcp`
   (`CMakeLists.txt:463-471` links only `core, session, llm, execution, policy,
   prompt, tools`), and `McpServerStatus` is defined in
   `include/ymh/mcp/mcp_types.hpp:139` (compiled into `ymh_mcp`,
   `CMakeLists.txt:347-348`). A function in `ymh_agent` taking
   `const std::vector<McpServerStatus>&` would couple the agent layer to a library
   it does not link. (It happens to compile today only because
   `ContextServerEntry` copies plain fields; a builder over the MCP type is the
   step that crosses the boundary.)

   Instead, **two serializers share one pinned JSON schema** (a documented shape,
   not a function):

   ```text
   # Shared MCP server object schema (45-D6.9)
   {
     "id":         <string>,   # McpServerId.value
     "state":      <string>,   # mcp_state_token(state)
     "tool_count": <int>,      # >= 0
     "skipped":    <int>,      # COUNT (never an array)
     "has_error":  <bool>      # last_error non-empty
   }
   # mcp.status EXTENDS the base object with:
   #   "connected":     <bool>   # state == Ready
   #   "skipped_tools": [<string>]  # names
   ```

   - **`context.show` serializer** stays in `ymh_agent`
     (`src/agent/context_snapshot.cpp:219-226`) and emits the **base** object
     (`{id, state, tool_count, skipped:<count>, has_error}`). It is **unchanged**:
     `ContextServerEntry` keeps its exact field set (`skipped` stays
     `std::size_t`), the `from_json` `get<std::size_t>()` (`:278`) is unchanged,
     and `/context`'s `server.skipped > 0` render (`src/ui/ui_render.cpp:1042-1043`)
     is unaffected. **No `connected` field is added to `ContextServerEntry`** — an
     unread wire field is the same smell that removed `mcp.status.note`.
   - **`mcp.status` serializer** stays where MCP is already reachable (the
     host/daemon side): `HostRuntime::mcpStatus()` (`src/host/host_runtime.cpp`,
     which links `ymh::mcp` transitively via `ymh::agent`'s consumers / the daemon
     wiring and directly holds `WorkspaceRuntime::mcp_statuses()`) emits the
     **extended** object (base + `connected` + `skipped_tools`). It must not read
     `last_error` into the JSON.
   - **`/status` derives `connected` from `state == "ready"`** (45-D7.5), not from
     a wire field — so the `context.show` path needs no schema extension.

   They are **not** redundant surfaces: `context.show` is session-scoped and
   budget-bound (it assembles the model-visible context and embeds the MCP
   summary as one segment), while `mcp.status` is **session-less and
   workspace-scoped** and returns the full configured-server set including
   `disabled`/`stopped` servers. `/mcp` uses `mcp.status`; `/context` keeps
   `context.show`. The shared thing is the schema (the field names and `skipped`
   being a count), which both serializers honor; there is no third shape.
10. **Profiles (MEDIUM fix).** `mcp.status` is read-only and allowed in **both**
    profiles (`is_method_allowed`, `src/transport/protocol.cpp:664-670`).
11. **Degradation (MEDIUM/HIGH fix).** If the daemon returns `MethodNotFound`
    (`-32601`) or `MethodNotAllowedForProfile` (`-32012`), `/mcp` renders
    `mcp: unavailable (daemon does not support mcp.status)` and disables the
    surface for the rest of the process (no retry loop); it never crashes
    (45-D9.10, 45-F19).

**Wiring (pinned seams).**

- `include/ymh/transport/protocol.hpp`: add
  `inline constexpr std::string_view kMcpStatus = "mcp.status";` after
  `kSkillsShow` (`:528`), and register it in `kMethodCatalog`
  (`src/transport/protocol.cpp:633-647`).
- `include/ymh/transport/host.hpp`: add a `TransportHost` virtual
  `[[nodiscard]] nlohmann::json mcpStatus();` beside `listSkills`/`showSkill`
  (`:114-117`).
- `src/agent/context_snapshot.cpp` (`ymh_agent`): **unchanged**. Its `mcp_servers`
  serializer (`:219-226`) already emits the shared base schema
  (`{id, state, tool_count, skipped:<count>, has_error}`); `ContextServerEntry` is
  not extended (no `connected` field). This is deliberate: `ymh_agent` does not
  link `ymh_mcp`, so no MCP-type builder belongs here (45-D6.9).
- `src/host/host_runtime.cpp` (MCP reachable on the daemon side): implement
  `HostRuntime::mcpStatus()` as a `translate(...)` that serializes
  `runtime_.mcp_statuses()` into the shared base schema **plus** `connected`
  (`state == Ready`) and `skipped_tools`, with a local
  `mcp_status_json(const std::vector<McpServerStatus>&)` helper (45-D6.9). It must
  not read `last_error` into the JSON.
- `src/transport/protocol_server.cpp`: add the dispatch branch after
  `kSkillsShow` (`:535-543`).
- Supervisor: `submit_to(workspace, method::kMcpStatus, {}, reply)` (the existing
  wrapper, `src/ui/supervisor.cpp:1324-1334`), then append the rendered block
  (45-D6 §8.2).

**`/mcp` rendering (pinned).** `/mcp` is a System-entry command
(`append_system_entry`, `include/ymh/ui/command_registry.hpp:43`), not a new
overlay/mode:

```text
mcp servers: 3 configured, 2 connected, 7 tools
  alpha   connected      3 tools
  beta    not connected  failed
  gamma   disabled
```

- The header line is `mcp servers: <N> configured, <C> connected, <T> tools`.
- One row per server: `  <id>  <connected|not connected>  <tool_count> tools`
  plus `  <state-token>` when not connected. The `skipped_tools` names, when
  non-empty, render on a continuation row `    skipped: a, b, …` (bounded; the
  `skipped` count is shown as `skipped=N`).
- **The RPC is session-less and issued unconditionally (MEDIUM fix).** `/mcp`
  calls `mcp.status` whether or not a session exists; the RPC never depends on a
  session. Only the *rendering target* differs: if a modeled active session
  exists (`UiModel::ensureActiveSession()`, 45-D10) the block is appended to it;
  otherwise the same block text goes to the notice ring
  (`UiModel::pushNotice`, `include/ymh/ui/ui_model.hpp:481`). `/mcp` never blocks
  on or requires a session.

**Owning specs amended:** `15` §4.7/§12.3 (additive), `05` §7.3 (method
catalog) and §7.3 profile gating, `10` §9.2, `18` §4 (shared MCP schema).
**Tests:** `UI45_D6_McpStatusWireShape`, `UI45_D6_McpStatusStrictParams`,
`UI45_D6_McpCommandRender`, `UI45_D6_NoMcpIsEmptyNotError`,
`UI45_D6_McpWorksWithoutSession`, `UI45_D6_McpSharedSchema`,
`UI45_D6_McpMethodNotFoundDegradation`.

---

## 9. D7 — `/status` (item 7)

### 9.1 Current state (verified)

- **Version.** `project(ymh VERSION 0.1.0)` (`CMakeLists.txt:3-5`).
  `YMH_VERSION` is defined **PRIVATE** to `ymh_cli` and `ymh` only
  (`CMakeLists.txt:772`, `:784`); `ymh_ui` (which compiles `supervisor.cpp`,
  `CMakeLists.txt:689-729`) does **not** receive it. `src/cli/cli.cpp:53-54`
  provides a `"0.0.0"` fallback and prints it at `:1154-1156`. There is no
  `version.hpp`. **The supervisor cannot read the version today.**
- **Model.** `effective_model()` = `agent.model` if non-empty else `llm.model`
  (`include/ymh/config/config.hpp:314-315`, `src/config/config.cpp:1525-1530`);
  defaults `llm.model = "deepseek-flash"`, `agent.model = ""`
  (`include/ymh/config/config.hpp:104-115`). Only CLI/daemon wiring computes it
  (`src/cli/wiring.cpp:65`, `:177`). The supervisor instead uses the raw
  `options_.config.agent.model` (`src/ui/supervisor.cpp:1217-1218`, `:1304-1305`),
  which is empty by default. `SessionHeader.model` (`include/ymh/session/session.hpp:50`)
  exists on the daemon side but the supervisor never fetches it: **no client
  calls the `session.show` RPC** (grep `src/`: the only mentions are the server
  dispatch `protocol_server.cpp:393`, the catalog `protocol.cpp:638`, and the
  CLI's own `session_show` helper `src/cli/session_cli.cpp:235`), and the only
  supervisor-side header read is `/export`'s direct durable-store read
  (`src/ui/supervisor.cpp:1479`), not an RPC.
- **Connectivity.** **No health/probe API exists** (no `health|probe|connectivity`
  in `src/llm`; `LLMProvider::stream` is failure-as-value,
  `include/ymh/llm/llm_provider.hpp:68-73`). The only signals are
  `UiEvent::ErrorOccurred` → `StatusModel::last_error`
  (`src/ui/ui_model.cpp:830-837`) and `UiEvent::StatusChanged` →
  `StatusModel::note` (`:843-845`), fields at
  `include/ymh/ui/ui_model.hpp:190-191`.
- **Tools/MCP.** `context.show` returns a `ContextSnapshot` with `tools`
  (`ContextToolEntry`) and `mcp_servers` (`ContextServerEntry`)
  (`include/ymh/agent/context_snapshot.hpp:86-96`); the supervisor already
  deserializes it (`parse_context_snapshot`, `src/ui/supervisor.cpp:130-137`) but
  `refresh_status_context` keeps only token counts (`:2057-2084`).

### 9.2 Decision (45-D7)

**`/status` is a System-entry command** (no new overlay/mode) that appends one
block to the active session (or a notice if none):

```text
ymh status
  version:  0.1.0
  model:    deepseek-flash
  api:      ok | error: <bounded last_error> | unknown
  daemon:   attached (workspace: <title>) | connecting | detached | ...
  tools:    12 available (8 builtin, 4 mcp)
    read_file (builtin) ~210 tok
    write_file (builtin) ~260 tok
    mcp.alpha.search (mcp) ~180 tok
    … and 9 more
  mcp:      2 connected / 3 configured
    alpha  connected
    beta   not connected (failed)
```

**Tool statuses, not just a count (MEDIUM fix).** `/status` lists each tool from
`ContextSnapshot.tools` (`ContextToolEntry`: `name`, `provenance`
`"builtin"|"mcp"`, `schema_tokens` — `include/ymh/agent/context_snapshot.hpp:48-52`)
as `name (provenance) ~N tok`, plus the summary `tools: N available (B builtin, M
mcp)`. The list is bounded to the first 20 rows followed by `… and K more`, so a
large MCP catalog cannot flood the entry. This surfaces per-tool availability and
origin (the "statuses"), not merely a number. MCP server states are listed from
`snapshot.mcp_servers` (`id` + state token, with `has_error` rendered as
`(failed)`), not just a connected count.

**Data sources, pinned.**

1. **Version seam (new).** Add `std::string version;` to `SupervisorRunOptions`
   (`include/ymh/ui/supervisor.hpp:30-67`), populated from `src/cli/cli.cpp`
   where `YMH_VERSION` is visible (`:53-54`, `:1154-1156`). The supervisor renders
   `options_.version`, or `"unknown"` when empty. This avoids changing `ymh_ui`'s
   CMake compile definitions (the alternative — adding `YMH_VERSION` to `ymh_ui`
   — is recorded as rejected because it couples a UI library to the CLI build
   identity).
2. **Model (fixed).** The supervisor MUST use `effective_model(options_.config)`
   instead of `options_.config.agent.model` at `src/ui/supervisor.cpp:1218` and
   `:1305`. `/status` renders `StatusModel::model`, which is now the effective
   model. **HIGH fix:** an earlier draft claimed `SessionHeader.model` is
   "available from `session.show`, already used for other fields". That is false:
   no client calls the `session.show` RPC (grep `src/`: only the server dispatch
   `protocol_server.cpp:393`, the catalog `protocol.cpp:638`, and the CLI helper
   `src/cli/session_cli.cpp:235`), and the supervisor reads a session header
   directly from the durable store only for `/export`
   (`src/ui/supervisor.cpp:1479`), never over RPC. The "daemon truth wins"
   sub-behavior is therefore **dropped**. `/status` has exactly one model source:
   `effective_model(options_.config)`. (A daemon-authoritative model would need a
   real async `session.show` call with reply/error handling; that is not pinned
   here and is recorded as a follow-up in §18 Q4a.)
3. **Connectivity (new, derived).** Define:

```cpp
enum class ApiConnectivity : std::uint8_t { Unknown, Ok, Error };
```

   `StatusModel` gains `ApiConnectivity api_state = ApiConnectivity::Unknown;`.
   `UiEventAdapter` updates it: any `ErrorOccurred` → `Error`; a successful
   `AssistantMessageFinished` (`include/ymh/ui/ui_event.hpp:96`) or
   `TokenUsageUpdated` (`:179`) for the session → `Ok`. (`AssistantMessageStarted`
   at `:84` does **not** set `Ok`: the message can still fail.) `/status` renders:
   `ok` for `Ok`; `error: <last_error>` (bounded to 120 chars, redacted) for
   `Error`; `unknown` before any observation. This is explicitly a
   **last-outcome** definition, not a probe; a non-LLM `ErrorOccurred` may set
   `Error` until the next success, which is accepted and documented (45 §12
   `45-F8`). A real probe is out of scope (§1.4).
4. **Daemon link.** Rendered from `WorkspaceModel::daemonStatus`
   (`include/ymh/ui/ui_model.hpp:254`) with the workspace title.
5. **Tools + MCP (statuses, MEDIUM fix).** `/status` triggers `context.show` for
   the active session (same call `/context` makes, `src/ui/supervisor.cpp:2058`,
   `:2098`). On reply it renders the per-tool rows and the per-server states
   (45-D7 block above), not just counts: the summary line
   `tools: N available (B builtin, M mcp)` plus up to 20 `name (provenance) ~N tok`
   rows and `… and K more`; the MCP lines list each server `id` + state token
   (`has_error` → `(failed)`) plus the `connected/configured` summary.
   **`connected` is derived from `state == "ready"`** — `ContextServerEntry` has
   no `connected` field (45-D6.9), so `/status` compares the wire `state` string
   to `"ready"` (the `mcp_state_token` value, `include/ymh/mcp/mcp_types.hpp:56`).
   The full inventory is **not** persisted into `StatusModel`; the block is
   appended when the reply lands, so `refresh_status_context` keeps discarding
   them (no change needed there). If no active session, `/status` renders the
   local lines (version/model/api/daemon) to the notice ring and omits the
   tool/MCP block with a `(no session)` note.

**Owning specs amended:** `10` §6 (status line) / §9.2 (keybindings), `18` §4
(read-only `context.show` consumer).
**Tests:** `UI45_D7_StatusRendersVersionModelApi`,
`UI45_D7_StatusUsesEffectiveModel`, `UI45_D7_StatusToolStatuses`,
`UI45_D7_StatusToolListBounded`, `UI45_D7_StatusConnectedFromState`,
`UI45_D7_ConnectivityTransitions`, `UI45_D7_StatusNoSessionNotice`.

---

## 10. D8 — `/quit` alias display (item 8)

### 10.1 Current state (verified)

- `Command` already has `aliases` (`include/ymh/ui/command_registry.hpp:45-50`).
- `exit` is registered with alias `{"quit"}` and description `"quit the
  supervisor"` (`src/ui/command_registry.cpp:243-250`). `find` resolves aliases
  (`:64-70`). So `/quit` already exits and already works.
- The command **list** renders `"/" + hint.name` plus the description
  (`src/ui/ui_render.cpp:373-379`), with `CommandHint::name = command->name`
  (`src/ui/supervisor.cpp:1684`). Aliases never appear in the list.
- `/help` appends `" (alias: /quit)"` (`src/ui/command_registry.cpp:251-266`),
  per 25-D12.

### 10.2 Decision (45-D8)

1. **Display name helper (new).** Add a pure function
   `std::string command_display_name(const Command&)` returning
   `name + "(" + join(aliases, ",") + ")"` when `aliases` is non-empty, else
   `name`. For `/exit` it returns `exit(quit)`.
2. **`CommandHint` gains `display`.** `CommandHint` becomes
   `{ name, display, description }` (`include/ymh/ui/ui_model.hpp:179-182`);
   `refresh_hints` sets `name = command->name` (completion) and
   `display = command_display_name(*command)` (rendering).
3. **Renderer.** `render_command_hints` renders `"/" + hint.display + " - " +
   hint.description` (`src/ui/ui_render.cpp:373-379`). The `/exit` row is
   therefore exactly:

   ```text
   > /exit(quit) - quit the supervisor
   ```

   (the leading `> `/`  ` selection marker and colours are retained). The
   separator changes from the current two-space gap to `" - "` to match the
   user's literal string.
4. **`/help`** renders the same `command_display_name` + `" - "` + description,
   superseding 25-D12's `(alias: /quit)` suffix. `/quit` is **not** a separate
   row in either surface (it is an alias, not a command).
5. **Completion is unchanged.** `complete` still matches canonical names only
   (`src/ui/command_registry.cpp:73-82`); `CommandHint::name` is the canonical
   name, so Tab completes `/exit`, never `/exit(quit)`.

**Owning specs amended:** `25` D12 (superseded, 45-S3), `10` §9.2.
**Tests:** `UI45_D8_ExitRowLiteral`, `UI45_D8_QuitNotSeparateRow`,
`UI45_D8_TabCompletesCanonicalName`, `UI45_D8_HelpUsesDisplayName`.

---

## 11. D9 — Tab/Shift+Tab cycle agents when the command list is inactive (item 9)

### 11.1 Current state (verified)

- Spec 42 is implemented as a **standalone library only**. `AgentPresetRoster`
  (`include/ymh/agent/preset.hpp:135`) provides `list()` (`:145`),
  `resolve()` (`:146`), and `select(Agent&, const std::string&)` (`:167`, impl
  `src/agent/preset.cpp:584-600`). It is constructed **nowhere in `src/`** except
  its own `preset.cpp`; `PresetConfig` is never built from `PresetsSettings`
  (`include/ymh/config/config.hpp:190-196`); `AgentServices::presets`
  (`include/ymh/agent/agent_loop.hpp:84`) is never assigned in
  `src/agent/workspace_runtime.cpp` (services at `:178-208`). There are **no
  builtin presets**: `default_presets_shipped_root()` returns `{}`
  (`src/agent/preset.cpp:307-309`); the roster is empty unless the user creates
  `$XDG_CONFIG_HOME/ymh/presets/<id>/preset.jsonc`.
- The TUI has **no agent-selection code**: `src/ui/` contains no
  `preset`/`roster`/agent-selection logic (the only `agent` tokens are
  `AgentState`/`agent_state` and the subagent display); no `/agent` command
  (`src/ui/command_registry.cpp:126-268`).
  `session.create` sends only `{title, model}` (`src/ui/supervisor.cpp:1254-1259`).
- `SessionHeader.agent_preset` is the **start** preset, copied at creation
  (`include/ymh/session/session.hpp:56-59`;
  `src/session/session_manager.cpp:65-66`); nothing reads it to drive a turn, and
  `HostRuntime::createSession` never sets `SessionOptions.agent_preset`
  (`src/host/host_runtime.cpp:567-589`). The field is inert in production.
- JSON-RPC has **no** agent selection method: only `agent.prompt`, `agent.followup`,
  `agent.steer`, `agent.inject`, `agent.cancel`, `agent.status`
  (`include/ymh/transport/protocol.hpp:516-521`; dispatch
  `src/transport/protocol_server.cpp:480-508`).
- Tab/Shift+Tab today are command completion only (`src/ui/supervisor.cpp:1976-1989`)
  and return `false` outside a bare `/prefix` — there is no fallback binding.
- Spec 42 pins the switch as **blank-session-only** (42-I3/42-D4,
  `docs/design/42-agent-presets.md:349-393`); `select` appends
  `agent_preset/selected` after the swap (`:385-388`). Spec 42 pins **no RPC**
  and never mentions the supervisor.

### 11.2 Decision (45-D9)

1. **Key routing (HIGH fix — restricted).** In `handle_input`, after the
   command-list branches (45-D2):

```text
Tab / TabReverse:
    if command_list_active(state)        -> Tab completes; Shift+Tab no-op     // 45-D2
    else if bare /prefix (any)           -> Tab completes first match if any,
                                            else no-op (NEVER agent cycling)    // 45-D2.3
    else if input.draft.empty()          -> cycle_agent(±1)                     // 45-D9
    else                                 -> no-op (return false)
```

   Agent cycling is gated to an **empty draft only**. A non-empty draft that is
   not a completable `/prefix` (e.g. `/zzz`, or plain text) MUST NOT cycle the
   agent: `Tab` returns `false`/no-ops. This fixes the defect where a bare
   `/prefix` with zero matches fell through to `cycle_agent` and silently
   changed the agent selection on a draft like `/zzz`. This satisfies
   "applies ONLY when `/` was NOT pressed" and is stricter than "non-`/` draft":
   only the empty composer cycles.
2. **Where the active agent lives.** The daemon owns the live composition: one
   `AgentLoop` per session (`AgentRegistry::bySession_`,
   `include/ymh/agent/agent_registry.hpp:103-104`), resolved by session id
   (`HostRuntime::ensureAgent`, `src/host/host_runtime.cpp:383-393`). The
   supervisor cannot switch locally (no `SystemPrompt`, no roster, no
   `SessionManager`), so cycling is a **daemon round-trip**.
3. **New RPC `agent.list`** (workspace-scoped, read-only):

```text
agent.list   params: { "session": "<session-id>" }     // optional
             result: {
               "agents": [ { "id": "<preset-id>",
                             "display_name": "<string>",
                             "blank": <bool>,        // daemon 42-I3 for `session`
                             "can_select": <bool> } ], // = blank && id != active
               "active":  "<preset-id>",   // last agent_preset/selected, else header
               "default": "<preset-id>"    // PresetConfig default, else ""
             }
```

   `agents` comes from `AgentPresetRoster::list()` (`preset.hpp:145`). When
   `session` is given, an unknown session → `UnknownSession`. `params: {}` returns
   the roster with `active = ""`.

   **`active` is the last `agent_preset/selected` event, not the header (MEDIUM
   fix).** `SessionHeader.agent_preset` is the **creation-time** preset
   (`src/session/session_manager.cpp:66`) and is never updated after a
   blank-session switch: the switch appends an `agent_preset/selected` event
   (`src/agent/preset.cpp:599`) but the message projection ignores it
   (`src/session/session.cpp:391`) and the header is not rewritten. Reading the
   header would therefore report a **stale** `active` (and a stale `can_select`)
   immediately after a successful `agent.select`. Pin: the daemon derives `active`
   by folding the session's own event log to the **last**
   `AgentPresetSelected` payload (`payload::AgentPresetSelected::agent_preset`,
   `include/ymh/session/events.hpp:276-277`), falling back to
   `SessionHeader.agent_preset` only when the log has none. `can_select` is
   computed against that folded `active`. The supervisor's `StatusModel::agent` is
   updated from the `agent.select` **reply** on success (and refreshable via
   `agent.list`) — there is **no** `UiEvent` for `AgentPresetSelected`
   (`include/ymh/ui/ui_event.hpp` has no such variant; the adapter has no
   `case EventType::AgentPresetSelected` and falls through its default
   `break;`, `src/ui/ui_event_adapter.cpp:182-183`), so the display must not
   claim an event-driven projection. The status line and `agent.list` therefore
   agree because both are fed by the daemon's folded `active`.

   **`blank`/`can_select` are daemon-owned (HIGH fix).** The supervisor MUST NOT
   reimplement 42-I3: that predicate scans the durable log for `UserMessage`,
   `AssistantMessage`, `ToolCall`, `ToolResult`, `ContextInjected`,
   `ContextCompaction`, `SubagentSpawned`, `SubagentFanIn`, and `LlmRequestHeader`
   (`docs/design/42-agent-presets.md:356-363`), several of which the UI model does
   not carry. The daemon computes `blank` per requested `session` (via
   `AgentPresetRoster::is_blank`, `preset.hpp:176`) and returns it on the wire;
   `can_select = blank && id != active` with the folded `active` above. With no
   `session`, `blank`/`can_select` are `false` (no session to test). The supervisor
   uses `can_select` only to choose the affordance/label and to avoid a doomed
   `agent.select`; the daemon's `CompositionFixed` remains the authority (defense
   in depth, 45-D9.5).

   **Param validation (strict, matching `mcp.status`).** `params` MUST be absent
   or a JSON object. If present: every key MUST be one of the declared keys
   (`session`); an unknown key → `InvalidParams`; `session` when present MUST be a
   string, else `InvalidParams`; an empty-string `session` is `InvalidParams`
   (not "absent"). A non-object `params` (array/string/number/bool) →
   `InvalidParams`. This is stricter than the lenient `skills.list` and is pinned
   so the seam is unambiguous.
4. **New RPC `agent.select`** (workspace-scoped, **Interactive profile only**):

```text
agent.select params: { "session": "<session-id>", "agent": "<preset-id>" }
             result: { "agent": "<preset-id>" }
```

   **Param validation (strict).** `params` MUST be a JSON object (absent/null →
   `InvalidParams`). Both `session` and `agent` are REQUIRED and MUST be non-empty
   strings (`InvalidParams` otherwise, including wrong JSON type). An unknown key
   → `InvalidParams`.

   The daemon resolves the session, resolves the agent's live leaf, and calls
   `AgentPresetRoster::select(Agent&, preset)` (`preset.hpp:167`). Errors, mapped
   to existing codes:
   - unknown session → `AppCode::UnknownSession`;
   - unknown preset → `RpcCode::InvalidParams` (spec 42 `42-F1`);
   - non-blank session → a new app code `AppCode::CompositionFixed = -32020`
     (spec 42 `42-F4`); `select` appends nothing and the composition is
     unchanged (`42 §2.3` step 2);
   - unwired daemon → `RpcCode::MethodNotFound` (the method is simply absent
     from the catalog, as with any unregistered method).
   On success the daemon appends `agent_preset/selected` (`42 §2.3` step 4); the
   next `agent.prompt` runs under the new composition (the roster swaps the live
   leaf; 42 §2.3 step 5).
5. **Blank predicate: the daemon is the only authority (HIGH/MEDIUM fix).**
   The supervisor does **not** implement 42-I3 locally; it reads the daemon's
   `can_select` from `agent.list` (45-D9.3). `cycle_agent` behavior:
   - `can_select == true` (blank, different preset) → issue `agent.select` and let
     the daemon answer; on `ok` update the display.
   - `can_select == false` and the session is **non-blank** → do **not** issue
     `agent.select`; set `preferred_agent_` for the next `session.create` and show
     the pending label (45-D9.6). This avoids the doomed round-trip and is not a
     reimplementation: the boolean came from the daemon.
   - `can_select == false` while the session is **blank** (the roster has no
     *other* selectable preset — e.g. a single-preset roster, so every entry is
     already `active`) → `cycle_agent` is a **no-op**: it does not issue
     `agent.select`, does not set `preferred_agent_` (there is nothing to prefer),
     and pushes at most one notice `"no other agents available"` (MEDIUM fix).
     This is distinct from the non-blank case, where a different preset exists and
     `preferred_agent_` is meaningful.
   - `CompositionFixed` from the daemon (a race between `agent.list` and the
     switch, e.g. a message appended in between) → the authority wins: show
     `"agent composition is fixed for this session"`, set `preferred_agent_`, and
     do **not** change the active display.
   - `UnknownSession` → notice; clear the stale focus via 45-D10 recovery; no
     display change.
   - `MethodNotFound` / `MethodNotAllowedForProfile` → see 45-D9.10.
   When there is no active session, `cycle_agent` only sets `preferred_agent_`.
6. **TUI display (HIGH fix).** `StatusModel::agent` shows the **daemon-active**
   preset **only** (from `agent.list`'s `active`, kept current by the
   `agent_preset/selected` projection). `preferred_agent_` is a **pending
   preference**, never the active composition; it MUST NOT be written into
   `StatusModel::agent` and MUST NOT overwrite the active display. When set, it
   renders only as a distinct trailing segment `agent(next):<id>` (a separate
   field, e.g. `StatusModel::pending_agent`). A non-blank session's active
   display therefore never changes on Tab; only the pending segment does.
7. **`preferred_agent_` on create.** `session.create` gains
   `create_params["agent_preset"] = preferred_agent_` when non-empty
   (`src/ui/supervisor.cpp:1254-1259`), and `HostRuntime::createSession` copies it
   into `SessionOptions.agent_preset` (currently absent,
   `src/host/host_runtime.cpp:567-589`).
8. **Wiring prerequisite (pinned).** Spec 42 is library-only; 45-D9 cannot work
   without: (a) building `PresetConfig` from `config.presets`
   (`include/ymh/config/config.hpp:190-196`) and constructing
   `AgentPresetRoster` in `WorkspaceRuntime` (beside the MCP manager,
   `src/agent/workspace_runtime.cpp:143-149`); (b) assigning
   `AgentServices::presets` (`include/ymh/agent/agent_loop.hpp:84`); (c) adding
   the two methods to `protocol.hpp:495-530`, `protocol.cpp:633-647`,
   `protocol_server.cpp:480-508`, and `host.hpp:114-117`. The wiring is a
   **prerequisite, not optional**: without it the two methods are simply absent
   from the catalog and 45-D9.10's degradation applies.
9. **Empty roster (MEDIUM fix).** With no user presets (the default),
   `agent.list` returns an empty `agents` list. `cycle_agent` MUST NOT silently
   no-op: on the first Tab with an empty roster the supervisor pushes the notice
   `"no agents configured (add <config>/presets/<id>/preset.jsonc)"` and renders
   no agent segment. Repeated Tabs do not spam the ring (the notice is pushed at
   most once per empty-roster state). This makes the default-install behavior
   discoverable rather than a dead key.
10. **Protocol version / compatibility (MEDIUM/HIGH fix).** `kProtocolVersion`
    stays **1** (`include/ymh/transport/protocol.hpp:153`;
    `docs/design/05-transport.md:167`). The three new methods are additive, and
    `AppCode::CompositionFixed = -32020` is the next free server-range code after
    `NotLastOwner = -32019` (`protocol.hpp:169-190`). No version bump is needed
    because an old supervisor never calls the new methods, and a new supervisor
    talking to an old daemon gets `MethodNotFound` (`RpcCode::MethodNotFound =
    -32601`, `protocol.hpp:163`). **Graceful degradation is pinned for all three
    methods** (not just `agent.select`): on `MethodNotFound` — or
    `MethodNotAllowedForProfile` (`-32012`) — the supervisor surfaces a one-line
    notice, disables the corresponding surface for the rest of the process (no
    retry loop), and never crashes:
    - `mcp.status` → `/mcp` renders `mcp: unavailable (daemon does not support mcp.status)`.
    - `agent.list` → no agent segment; `cycle_agent` no-ops with the 45-D9.9 notice.
    - `agent.select` → notice; `preferred_agent_` is still set for the next
      `session.create` (the `agent_preset` param is ignored by an old daemon's
      `createSession`, which reads only known keys — `src/host/host_runtime.cpp:567-589`).
    An unknown negative code from a future daemon is surfaced as a generic error,
    never parsed by name.

    **RPC profiles (MEDIUM fix, pinned).** `mcp.status` and `agent.list` are
    read-only and allowed in **both** profiles. `agent.select` mutates a live
    session's composition and is **Interactive only**: it is added to
    `is_method_allowed`'s deny list (`src/transport/protocol.cpp:664-670`), so an
    Automation client receives `MethodNotAllowedForProfile`. This matches the
    existing `session.activate`/`session.suspend`/`session.compact` treatment.

**Owning specs amended:** `42` §3.2/§2.3/§3.4 (wiring + RPC), `10` §9.2/§6
(Tab binding + status segment), `05` §7.3 (methods), §2.1 (`kProtocolVersion`
retained at 1) and §7.3 profile gating (`is_method_allowed`).
**Tests:** `UI45_D9_TabCyclesAgentWhenNoList`,
`UI45_D9_TabNoAgentCycleOnNonEmptyDraft`, `UI45_D9_TabCompletesWhenListActive`,
`UI45_D9_AgentListWireShape`, `UI45_D9_AgentListStrictParams`,
`UI45_D9_AgentListBlankCanSelect`, `UI45_D9_AgentListActiveFoldsLog`,
`UI45_D9_AgentSelectStrictParams`,
`UI45_D9_AgentSelectBlankSucceeds`, `UI45_D9_AgentSelectNonBlankRejected`,
`UI45_D9_PendingAgentNeverActive`, `UI45_D9_PreferredAgentOnCreate`,
`UI45_D9_SinglePresetNoop`, `UI45_D9_EmptyRosterNotice`,
`UI45_D9_ProtocolVersionRetained`,
`UI45_D9_MethodNotFoundDegradation`, `UI45_D9_AgentSelectProfileDenied`.

---

## 12. D10 — The unmodeled-session lockout (item 10)

### 12.1 Root cause (verified)

**Bug (a): "no active session" after switch+Enter.**

1. `CommandContext.session` is set from `active()`
   (`src/ui/supervisor.cpp:1593`).
2. `active()` is `model_.activeWorkspace()` then
   `model_.session(workspace->activeSessionId)` (`src/ui/supervisor.cpp:1368-1374`).
3. `UiModel::activeWorkspace()` returns the map entry (`src/ui/ui_model.cpp:426-432`);
   `UiModel::session(id)` returns `nullptr` when `id ∉ UiModel::sessions`
   (`src/ui/ui_model.cpp:442-445`).
4. **`UiModel::focusSession` silently no-ops when the target is unmodeled**:
   `if (state == sessions.end()) return;` (`src/ui/ui_model.cpp:948-951`). This is
   the Live-switcher activation path (`src/ui/supervisor.cpp:1932`). **Corrected
   attribution (MEDIUM fix):** because `focusSession` no-ops, it does **not**
   itself leave a dangling `activeSessionId`; it silently fails to switch. The
   dangling-id risk lives in the other setter, `activate_session`
   (`src/ui/supervisor.cpp:1180`), which assigns `it->second.activeSessionId =
   session` **unconditionally** and relies on its callers to have modeled the
   session first (`refresh_sessions` at `:1216-1225` and `apply_create_reply` at
   `:1303-1308` both do). The invariant "focus is always modeled" is therefore a
   caller convention, not enforced by the setter — a missed caller, or an
   `eraseSession` (`src/ui/ui_model.cpp:552-567`) racing between the ensure and
   the activate, leaves a non-empty but unmodeled `activeSessionId`. 45-D10.7
   makes `activate_session` model structurally (45-I10).
5. **The History path is asynchronous.** `select_history`
   (`src/ui/supervisor.cpp:1886-1899`) → `resume_from_history` (`:975-983`) →
   `resume_after_attach` submits `session.resume` and returns (`:985-1009`). The
   session is modeled only when the reply lands in `apply_resume_success`
   (`:1014-1026`, `ensureSessionIn` + `focusSession`). Between Enter and the
   reply the selection has not taken effect (the UI still shows the previous
   session); if the reply fails, `surface_notice` reports it (`:887-895`) and the
   focus stays on the previous, modeled session. The async gap alone does not
   create a dangling id, but combined with the unenforced setter (item 4) the
   supervisor can observe a non-empty, unmodeled focus.
6. Handlers that require a session then emit `"no active session"`
   (`src/ui/command_registry.cpp:180-181`, `:228-229`; `/export` at
   `src/ui/supervisor.cpp:1462-1463`), and the conversation renderer shows
   `"(no active session)"` (`src/ui/ui_render.cpp:315`).
7. A `SessionCell` can exist without a `SessionUiState`: `ensureCellIn` creates a
   cell alone (`src/ui/ui_model.cpp:454-467`), whereas `ensureSessionIn` creates
   the state and the cell (`:473-488`). Nothing in `focusSession` repairs this.

**Bug (b): after the error, `/` does nothing (unrecoverable).**

`handle_input` begins:

```cpp
SessionUiState* state = active();
if (state == nullptr) {
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace != nullptr && workspace->activeSessionId.value.empty()) {
        create_session(workspace->id, std::string{});
    }
    return false;                       // <-- every key dropped
}
```

(`src/ui/supervisor.cpp:1945-1952`). When `active()` is `nullptr` because
`activeSessionId` is **non-empty but unmodeled**, the function returns `false`
without handling the event. In Conversation mode this is the final fallback
(`handle_event_inner`, `:2190-2252`), so **every** keystroke — including `/` — is
dropped. The only branch that could repair the state (`create_session`) is gated
on `activeSessionId.value.empty()`, which is false. The composer is stuck. This
is the precise mechanism of "typing `/` does nothing".

### 12.2 Decision (45-D10)

1. **Model-on-focus (new).** `UiModel::focusSession` MUST model the target
   instead of returning when it is absent:

```cpp
void UiModel::focusSession(const SessionId& id) {
    if (id.value.empty()) return;
    const WorkspaceId workspace = /* existing state's workspace, else activeWorkspaceId */;
    if (workspaces.find(workspace) == workspaces.end()) return;
    ensureSessionIn(workspace, id);          // 45-D10.1: never leaves an unmodeled focus
    activeWorkspaceId = workspace;
    workspaces.at(workspace).activeSessionId = id;
    mode = UiMode::Conversation;
    dirty.mark(id, UiDirtyFlag::Conversation | UiDirtyFlag::Layout | UiDirtyFlag::Input);
    dirty.markAggregate();
}
```

   When the id is unknown to `sessions`, the target workspace is
   `activeWorkspaceId` (the switcher leaf already belongs to a workspace the
   caller knows; `open`/`openHistory` carry `WorkspaceNode::id`). The
   `SwitcherOverlayModel` selection paths pass the workspace explicitly via a new
   `focusSessionIn(const WorkspaceId&, const SessionId&)` overload, so the
   repair is unambiguous.
2. **`UiModel::ensureActiveSession()` (new).** A single repair point:

```cpp
// Reconciles the focused workspace's activeSessionId into `sessions`.
// Returns the state, or nullptr when there is no workspace / no active id.
[[nodiscard]] SessionUiState* UiModel::ensureActiveSession();
```

   It returns `activeSession()` when modeled; otherwise, if
   `activeWorkspace() != nullptr && !activeSessionId.value.empty()`, it calls
   `ensureSessionIn(workspace->id, workspace->activeSessionId)` and returns the
   state; otherwise `nullptr`.
3. **`handle_input` self-heals.** Replace the null branch with:

```cpp
SessionUiState* state = active();
if (state == nullptr) state = model_.ensureActiveSession();
if (state == nullptr) {
    WorkspaceModel* workspace = model_.activeWorkspace();
    if (workspace != nullptr && workspace->activeSessionId.value.empty()) {
        create_session(workspace->id, std::string{});
    }
    return false;   // only when there is genuinely no workspace/session to bind
}
```

   With this, a non-empty-but-unmodeled `activeSessionId` is modeled on the next
   keystroke, and `/` is processed normally. The UI can no longer get stuck.
4. **Command dispatch self-heals.** `dispatch_command` sets
   `context.session = model_.ensureActiveSession()` instead of `active()`
   (`src/ui/supervisor.cpp:1593`), so `/rename`, `/plan`, and `/export` never see
   a spurious `nullptr` for a session the user actually selected. A genuinely
   absent session (no workspace, empty id) still yields the existing
   `"no active session"` message — that message is retained for the real case.
5. **Selection models eagerly.** `handle_switcher` Return for the Live source
   calls `focusSessionIn(cursor.workspace, *cursor.session)` (which models,
   45-D10.1) before closing; the History path's `apply_resume_success` already
   calls `ensureSessionIn` (`src/ui/supervisor.cpp:1020`) and is unchanged. If the
   `session.resume` reply fails, `surface_notice` (`:887-895`) reports it and the
   focus is left on the previous, modeled session — never on a dangling id.
6. **Daemon makes it alive.** `agent.prompt` already calls `ensureAgent(id)`,
   which resumes the agent when absent (`src/host/host_runtime.cpp:839-845`,
   `:383-393`). This is the pinned "made alive" guarantee: whatever session the
   supervisor focuses, the first prompt resumes its agent daemon-side and
   continues the conversation. No new RPC is needed for item 10.
7. **`activate_session` delegates to the single mutator; the field is PRIVATE
   (MEDIUM fix).** `SupervisorApp::activate_session`
   (`src/ui/supervisor.cpp:1175-1184`) MUST delegate to
   `model_.focusSessionIn(workspace, session)` instead of assigning
   `it->second.activeSessionId` itself. **Compile-time mechanical enforcement:**
   `WorkspaceModel`'s `activeSessionId_` becomes **private** with a public getter
   `activeSessionId()` and a private `setActiveSessionId()`; `UiModel` is a
   `friend`, so the **only** writers are `UiModel::focusSessionIn`/`focusSession`
   (which call `ensureSessionIn` first) and `UiModel::eraseSession` (which clears
   it, `src/ui/ui_model.cpp:552-567`). Any other translation unit that tried to
   assign the focus fails to compile — 45-I10 is no longer enforced by review or
   a test guard. **Migration list (all read sites use the public getter):**
   - `src/ui/supervisor.cpp:384`, `:1159`, `:1223`, `:1368-1374`, `:1523-1528`,
     `:1599`, `:1608`, `:1633`, `:1948`;
   - `src/ui/ui_render.cpp:389`, `:834`, `:1151` (the renderer reads
     `workspace->second.activeSessionId` via the getter);
   - tests that currently assign the field directly must set focus via
     `focusSessionIn`: `tests/unit/ui_model_test.cpp:55`, `:384`, `:859`;
     `tests/unit/ui_driver_test.cpp:56`; `tests/unit/ui_event_adapter_test.cpp:33`.
   Its two current callers already model first (`refresh_sessions`
   `:1216-1225`; `apply_create_reply` `:1303-1308`); delegation makes the
   invariant structural rather than conventional.
8. **UnknownSession recovery (MEDIUM fix).** A session can disappear between the
   supervisor's model and the daemon (deleted/pruned, or a stale catalog row). Any
   `UnknownSession` (`AppCode::UnknownSession = -32006`,
   `include/ymh/transport/protocol.hpp:176`) returned by `agent.prompt`,
   `session.resume`, or `agent.select` triggers
   `SupervisorApp::recover_unknown_session(workspace, session)`:
   1. `model_.eraseSession(workspace, session)` (clears `activeSessionId` if it
      matched; `src/ui/ui_model.cpp:552-567`);
   2. push the notice `"session no longer exists"`;
   3. select the workspace's first remaining modeled session, or `create_session`
      when none remain.
   This guarantees the focus never dangles on a session the daemon rejects, and
   complements `ensureActiveSession` (which repairs the opposite direction: a
   modeled-but-not-focused state). Added tests:
   `UI45_D10_UnknownSessionRecovery`, `UI45_D10_UnknownSessionCreatesFallback`.
9. **Invariant.** After any successful selection (Ctrl+S, `/sessions`, or
   `--resume`), `activeSessionId` is always modeled: `active() != nullptr` and
   the composer accepts input. Every writer of `activeSessionId` is
   `focusSessionIn`/`focusSession` (models) or `eraseSession` (clears) — 45-I10;
   and a daemon `UnknownSession` is recovered, not left dangling (45-D10.8).

**Owning specs amended:** `10` §9.3, `22` §5.2.
**Tests:** `UI45_D10_NoActiveSessionAfterSwitcher`,
`UI45_D10_NoActiveSessionAfterSessions`, `UI45_D10_SlashWorksAfterRepair`,
`UI45_D10_FocusModelsTarget`, `UI45_D10_EnsureActiveSessionRepairs`,
`UI45_D10_ActivateSessionModels`, `UI45_D10_SingleMutator`,
`UI45_D10_UnknownSessionRecovery`, `UI45_D10_UnknownSessionCreatesFallback`,
`UI45_D10_AgentResumedOnPrompt`.

---

## 13. C++ interface sketches (pinned)

Only new/changed symbols are shown; unchanged members are elided with `…`.

```cpp
// ── include/ymh/ui/ui_model.hpp ────────────────────────────────────────────
namespace ymh::ui {

// 45-D2: CompletionCycle is REMOVED. InputModel keeps history unchanged; the
// 45-D5 dismissal flag lives on SessionUiState beside `command_hints`.
struct InputModel {
    std::string              draft;
    std::size_t              cursor = 0;
    std::vector<std::string> history;
    std::size_t              history_pos = 0;
    std::string              saved_draft;

    void push_history(std::string line);              // retained
    bool history_up();                                // retained
    bool history_down();                              // retained
    bool delete_forward();
    void clear_line();
    bool delete_word();
};

// 45-D8: `display` is the rendered label (name + aliases); `name` stays the
// canonical completion name.
struct CommandHint {
    std::string name;
    std::string display;      // 45-D8: e.g. "exit(quit)"
    std::string description;
};

// 45-D7: derived last-outcome connectivity (never a probe).
enum class ApiConnectivity : std::uint8_t { Unknown, Ok, Error };

struct StatusModel {
    std::string     model;
    …
    std::string     last_error;      // retained
    std::string     note;            // retained
    // 45-D7: derived connectivity.
    ApiConnectivity api_state = ApiConnectivity::Unknown;
    // 45-D9: the DAEMON-ACTIVE preset only; never the pending preference.
    std::string     agent;
    // 45-D9.6: the pending preference for the next session.create, rendered
    // distinctly (e.g. "agent(next):<id>"); never written into `agent`.
    std::string     pending_agent;
};

struct SessionUiState {
    SessionId          id;
    WorkspaceId        workspace;
    …
    std::vector<CommandHint> command_hints;
    std::size_t              command_hint_selected = 0;
    // 45-D5: true while the list is hidden by Esc; any command-prefix edit
    // clears it.
    bool                     hints_dismissed = false;
    …
};

struct UiModel {
    …
    // 45-D10: single repair point for a non-empty but unmodeled focus.
    [[nodiscard]] SessionUiState* ensureActiveSession();
    // 45-D10: THE single mutator of WorkspaceModel's private activeSessionId_
    // (models the target via ensureSessionIn). activate_session delegates here.
    void focusSessionIn(const WorkspaceId& workspace, const SessionId& id);
    // 45-D3: Live-source membership authority.
    [[nodiscard]] bool catalog_has_session(const WorkspaceId& workspace,
                                           const SessionId& session) const;
    …
};

// 45-D10.7 (MEDIUM fix): the focus is PRIVATE and has exactly one public writer,
// `UiModel::focusSessionIn`. `UiModel` is a friend so it can call the private
// setter; `eraseSession` clears through the same setter. No other translation
// unit can assign the focus, so 45-I10 is enforced by the compiler, not by
// convention. (Public read is via the getter, preserving the existing callers.)
class WorkspaceModel {
public:
    …
    [[nodiscard]] const SessionId& activeSessionId() const noexcept { return activeSessionId_; }
private:
    friend class UiModel;
    void setActiveSessionId(SessionId id) noexcept { activeSessionId_ = std::move(id); }
    SessionId activeSessionId_;   // written only by focusSessionIn / eraseSession
};

// 45-D4.3: WorkspaceNode distinguishes "source had zero sessions" from "all
// leaves were hidden by the focused-session exclusion", so the renderer never
// shows a false empty-state. `sessions_hidden_by_focus` is set by BOTH open()
// (Live) and openHistory() (History) when >= 1 leaf was removed and the result
// is empty. `open()` also sets `catalog_pending` and copies the catalog read's
// `note`, so Live never claims "(no live sessions)" while the snapshot is
// pending or the read failed. No node is suppressed.
struct WorkspaceNode {
    …
    bool                       historyOnly = false;
    std::optional<std::string> note;
    bool                       catalog_pending = false;            // 45-D4.3
    bool                       sessions_hidden_by_focus = false;   // 45-D4.3
    std::vector<SessionNode>   sessions;
};

// 45-D4: SwitcherOverlayModel applies the focused-session exclusion in both
// open() (Live) and openHistory() (History). NO node is suppressed: an empty
// leaf list renders a placeholder, and `sessions_hidden_by_focus` selects the
// distinct `(current session hidden)` label vs the source's own empty-state.
class SwitcherOverlayModel {
public:
    …
    void open(const UiModel& model);         // applies 45-D3 + 45-D4
    void openHistory(const UiModel& model);  // applies 45-D4 + 45-D4.3
    …
};

// 45-D6: the `/mcp` overlay is NOT added; the command appends a System entry.

} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/command_registry.hpp ────────────────────────────────────
namespace ymh::ui {

struct CommandContext {
    …
    // 45-D6/45-D7: read-only informational surfaces.
    std::function<void()> mcp;
    std::function<void()> status;
};

// 45-D8: pure display-name helper.
[[nodiscard]] std::string command_display_name(const Command& command);

} // namespace ymh::ui
```

```cpp
// ── src/ui/supervisor.cpp (new/changed private methods) ────────────────────
namespace {
// 45-D5.2: refresh_hints returns early while hints_dismissed is set.
void refresh_hints(SessionUiState& state);
// 45-D2: Tab completes the selected command (replaces complete_command's cycle).
bool complete_selected_command(SessionUiState& state);
// 45-D1: ArrowUp/Down; returns true when it moved the list selection.
bool move_hint_selection(SessionUiState& state, int delta);
// 45-D9: cycles preferred_agent_; issues agent.select ONLY when the daemon's
// `can_select` is true (45-D9.5), never a local blank predicate.
void cycle_agent(int delta);
// 45-D10.3: null branch self-heals via ensureActiveSession().
bool handle_input(const ftxui::Event& event);
} // namespace
```

```cpp
// ── include/ymh/transport/protocol.hpp ─────────────────────────────────────
namespace ymh::protocol::method {
…
inline constexpr std::string_view kMcpStatus = "mcp.status";     // 45-D6
inline constexpr std::string_view kAgentList = "agent.list";     // 45-D9
inline constexpr std::string_view kAgentSelect = "agent.select"; // 45-D9
} // namespace ymh::protocol::method

namespace ymh::protocol {
// 45-D9: the blank-session-only switch rejection. Next free server-range code
// after NotLastOwner = -32019 (protocol.hpp:169-190). Additive; kProtocolVersion
// stays 1 (45-D9.10).
enum class AppCode : int {
    …
    NotLastOwner     = -32019,
    CompositionFixed = -32020,   // 45-D9: agent.select on a non-blank session (42-F4)
};
} // namespace ymh::protocol
```

```cpp
// ── include/ymh/transport/host.hpp ─────────────────────────────────────────
class TransportHost {
public:
    …
    // 45-D6: structured, read-only MCP snapshot (both profiles). Backed by the
    // shared MCP JSON schema (two serializers; no ymh_agent MCP-type
    // builder, since ymh_agent does not link ymh_mcp) (45-D6.9).
    [[nodiscard]] virtual nlohmann::json mcpStatus() = 0;
    // 45-D9: roster + active preset (both profiles); and the blank-session-only
    // switch (Interactive only; MethodNotAllowedForProfile in Automation).
    [[nodiscard]] virtual nlohmann::json listAgents(const nlohmann::json& params) = 0;
    [[nodiscard]] virtual nlohmann::json selectAgent(const nlohmann::json& params) = 0;
};
```

```cpp
// ── include/ymh/agent/context_snapshot.hpp (ymh_agent) ─────────────────────
// 45-D6.9: NO shared builder lives here. `ymh_agent` does not link `ymh_mcp`
// (CMakeLists.txt:463-471), so a function taking `McpServerStatus` would cross a
// non-linked boundary. `context.show`'s serializer (context_snapshot.cpp:219-226)
// is UNCHANGED and emits the shared base schema:
//   { id, state, tool_count, skipped:<int>, has_error }
// `ContextServerEntry` is NOT extended (no `connected`).
```

```cpp
// ── src/host/host_runtime.cpp (MCP reachable on the daemon side) ───────────
// 45-D6.9: the mcp.status serializer (base schema + connected + skipped_tools).
[[nodiscard]] nlohmann::json mcp_status_json(const std::vector<McpServerStatus>& statuses);
```

```cpp
// ── include/ymh/ui/supervisor.hpp ──────────────────────────────────────────
struct SupervisorRunOptions {
    …
    // 45-D7: the build version, threaded from the CLI (YMH_VERSION is not
    // visible to ymh_ui). Empty means "unknown".
    std::string version;
};
```

---

## 14. Invariants

Numbered `45-I#`; testable and cited.

| ID | Invariant |
|---|---|
| 45-I1 | History contains both prompts and commands, in submission order, de-duplicated only against the immediately previous entry (`src/ui/supervisor.cpp:386`, `:1963`; `src/ui/ui_model.cpp:192-203`). |
| 45-I2 | When the command list is active, ArrowUp/ArrowDown move `command_hint_selected` and never mutate `input.draft` or history; otherwise they recall history (45-D1). |
| 45-I3 | Tab never navigates the command list. With the list active (or a bare `/prefix` with matches) Tab completes the selected/first command with a trailing space (45-D2). |
| 45-I4 | `CompletionCycle` and `InputModel::completion` do not exist after 45-D2. |
| 45-I5 | A session leaf renders in the Live source iff the workspace is renderable (22 SW1), the session id is in the latest catalog snapshot, and it is not the focused session (45-D3, 45-D4). |
| 45-I6 | Every session id rendered by the Live source is also rendered by the History source for the same workspace, minus the focused session (Live ⊆ History, 45-D3). |
| 45-I7 | **No workspace node is suppressed** in either source: every eligible node renders (a live/`Attached` node is focusable; a non-live History node is read-only with the existing `workspace not running` notice). An empty leaf list renders an explicit placeholder — Live: `(loading live sessions…)` while the catalog snapshot is pending, else the `note` leaf when the read failed, else `(current session hidden)` when the 45-D4 exclusion emptied it, else `(no live sessions)`; History: the `note` leaf (22 §4.4) when set, else `(current session hidden)`, else `(no stored sessions)` (22 §4.3). The whole-list placeholder (`(no workspaces)` / `loading stored sessions…` / `(no stored sessions)`) is retained. (45-D4.3; `WorkspaceNode::sessions_hidden_by_focus`.) |
| 45-I8 | Esc in Conversation mode hides a visible command list without changing `input.draft`/`input.cursor`; it is a no-op when no list is visible (45-D5). |
| 45-I9 | `hints_dismissed` is cleared only by a command-prefix edit; history recall does not clear it (45-D5.2). |
| 45-I10 | After any successful session selection (Ctrl+S, `/sessions`, `--resume`), `activeSessionId` is modeled: `active() != nullptr`. **Compile-time enforced:** `WorkspaceModel::activeSessionId_` is private with a public getter; `UiModel` (friend) is the only writer, via `focusSessionIn`/`focusSession` (which call `ensureSessionIn`) and `eraseSession` (clears); `activate_session` delegates to `focusSessionIn`. A daemon `UnknownSession` is recovered (45-D10.7/8). |
| 45-I11 | `handle_input` never returns `false` while an active workspace has a non-empty `activeSessionId`; it repairs the model first (45-D10.3). |
| 45-I12 | `CommandContext.session` is `ensureActiveSession()`, never a raw `active()` that can be `nullptr` for a selected session (45-D10.4). |
| 45-I13 | `mcp.status` params are absent or `{}`; any other shape is `InvalidParams` (45-D6.1). |
| 45-I14 | `mcp.status` never ships `last_error`, `server_name`, or `server_version` (45-D6.5; 18-M8). |
| 45-I15 | `mcp.status` with no configured servers returns `{"servers": [], "tool_total": 0}`, never an error (45-D6.6). |
| 45-I16 | `/status` renders `options_.version` (or `unknown`), `effective_model(config)`, the derived `api_state`, the daemon status, the per-tool **statuses** (`name (provenance) ~N tok`, bounded to 20 rows + `… and K more`, with a `N available (B builtin, M mcp)` summary), and the per-server MCP states (45-D7.5). |
| 45-I17 | The status line's model segment is never empty on a default install: it uses `effective_model` (`src/ui/supervisor.cpp:1218`, `:1305`; 45-D7.2). |
| 45-I18 | Connectivity is derived from observed outcomes only; no LLM probe call is issued (45-D7.3). |
| 45-I19 | The `/exit` command list row is exactly `/exit(quit) - quit the supervisor`; `/quit` is not a separate row (45-D8). |
| 45-I20 | Agent cycling happens **only when the composer draft is empty**. With the command list active Tab completes and Shift+Tab no-ops; with any non-empty draft (including a bare `/prefix` with zero matches, e.g. `/zzz`) Tab is a no-op and MUST NOT cycle the agent (45-D9.1). |
| 45-I21 | `agent.select` is issued only when the daemon's `can_select` is true; a non-blank session returns `CompositionFixed`, appends no event, and sets `preferred_agent_` for the next `session.create`. The supervisor runs no local 42-I3 predicate (45-D9.3/5; 42-D4). |
| 45-I22 | `agent.list` returns per-agent `blank`/`can_select` from the daemon; the supervisor never derives them (45-D9.3). |
| 45-I23 | With no user presets, `agent.list` returns an empty roster and `cycle_agent` pushes the 45-D9.9 notice at most once; it is never a silent no-op. |
| 45-I24 | The switcher overlay's Tab (expand) is unchanged and is not the composer's Tab (45-S5). |
| 45-I25 | Ctrl+D keeps the exit confirmation; `/exit` does not prompt (25-D10 retained; user clarification). |
| 45-I26 | `agent.list`/`agent.select` are strict: a non-object `params`, an unknown key, a missing/non-string/empty required field is `InvalidParams` (45-D9.3/4). |
| 45-I27 | `kProtocolVersion` remains 1; the three new methods and `AppCode::CompositionFixed = -32020` are additive. All three degrade gracefully on `MethodNotFound`/`MethodNotAllowedForProfile` (notice + disable, no crash, no retry loop) (45-D6.11, 45-D9.10). |
| 45-I28 | `StatusModel::agent` is the daemon-active preset only; `preferred_agent_` is rendered as a distinct `pending_agent` segment and never overwrites the active display (45-D9.6). |
| 45-I29 | `mcp.status` and `agent.list` are allowed in both profiles; `agent.select` is Interactive-only (`is_method_allowed`, `src/transport/protocol.cpp:664-670`) (45-D6.10, 45-D9.10). |
| 45-I30 | `mcp.status` and `context.show`'s `mcp_servers` share a pinned **JSON schema** (two serializers, no shared function — `ymh_agent` does not link `ymh_mcp`); `skipped` is a COUNT in both, `skipped_tools` (names) is `mcp.status`-only, and `context.show`'s `ContextServerEntry` is unchanged (45-D6.9). |

---

## 15. Failure modes

Prefix `45-F#` (trigger / symptom / recovery).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 45-F1 | User presses ArrowUp while the command list is visible | History would replace the draft instead of moving the selection | `move_hint_selection` runs first; history is untouched (45-I2) |
| 45-F2 | Tab pressed with the list visible | The old cycle would type a candidate and advance | `complete_selected_command` completes once and clears the list; no cycle exists (45-I3/I4) |
| 45-F3 | Catalog snapshot not yet loaded when Ctrl+S opens | Live would show a session not in History | Live renders zero leaves until the snapshot lands; `refreshNow` is requested on open (45-D3.2/3) |
| 45-F4 | Catalog read fails for a workspace (`note` set) or the snapshot is pending | Its live sessions would violate Live ⊆ History, and `(no live sessions)` would be a false claim | The Live node renders the `note` degradation leaf, or `(loading live sessions…)` while pending — never `(no live sessions)`; no node is suppressed (45-D3.3, 45-D4.3) |
| 45-F5 | Focused session is the only session in a workspace | The node would render an empty/inert group | **No suppression:** the node renders with `(current session hidden)` (Live or History) — never the false `(no stored sessions)`; a live node stays focusable, a non-live History node is read-only (45-I7, 45-D4.3/5) |
| 45-F6 | Esc pressed with an empty list | A stray key would be swallowed | `handle_input` returns `false`; Esc is a no-op (45-I8) |
| 45-F7 | ArrowUp after Esc (history recall) | `refresh_hints` would resurrect the list | `hints_dismissed` suppresses the rebuild (45-I9) |
| 45-F8 | A non-LLM `ErrorOccurred` fires | `/status` shows `error` although the provider is fine | Accepted last-outcome semantics; the next successful `AssistantMessageFinished` (`include/ymh/ui/ui_event.hpp:96`) sets `Ok`; a probe is out of scope (45-D7.3, §1.4) |
| 45-F9 | `context.show` reply is malformed / the session is gone | `/status` tool/MCP block would be wrong | Keep the local lines; omit the tool/MCP block with `(unavailable)`; never throw into the UI (45-D7.5) |
| 45-F10 | `/mcp` with no modeled active session | The System entry would be dropped silently | Push the block to the notice ring instead; `mcp.status` is session-less and still runs (45-D6 §8.2) |
| 45-F11 | `agent.select` on a non-blank session | The composition would change mid-conversation | Daemon returns `CompositionFixed`, appends nothing; UI notices and sets `preferred_agent_` (45-I21; 42-F4) |
| 45-F12 | `agent.list` on a daemon that has not wired the roster | `MethodNotFound` | No agent segment; `cycle_agent` shows the 45-D9.9/45-D9.10 notice; the surface is disabled, no retry loop (45-I27) |
| 45-F13 | A `session.resume` reply fails after a History selection | The selection would never take effect | `surface_notice` reports; focus stays on the previous modeled session; `ensureActiveSession` repairs any residual (45-D10.5/9) |
| 45-F14 | A stale `activeSessionId` (non-empty, unmodeled) is already set | `handle_input` would drop every keystroke (the lockout) | `ensureActiveSession` models it on the next event; input resumes (45-I11) |
| 45-F15 | `agent.select` succeeds but the daemon's live leaf swap fails after the append | UI and daemon disagree | The append happens only after the swap commits (42 §2.3 step 4); a failure leaves both unchanged and surfaces the RPC error |
| 45-F16 | `mcp.status` reply arrives for an evicted workspace | A phantom workspace would be injected | The reply is dropped when `model_.workspaces.count(workspace) == 0` (the RB-15/22-SW25 rule, `src/ui/supervisor.cpp:1289-1296` generalized) |
| 45-F17 | `agent.list`/`agent.select` receives a non-object `params`, an unknown key, or a missing/non-string/empty required field | A malformed call would be silently accepted | Strict `InvalidParams` (45-I26); the UI surfaces a notice and does not change the display |
| 45-F18 | `activate_session` is called for a session not yet modeled | A dangling `activeSessionId` (the lockout precondition) | It delegates to `focusSessionIn`, which calls `ensureSessionIn` first, so the id is always modeled (45-I10, 45-D10.7) |
| 45-F19 | `mcp.status` returns `MethodNotFound`/`MethodNotAllowedForProfile` (old daemon) | `/mcp` would crash or retry forever | `/mcp` renders `mcp: unavailable (daemon does not support mcp.status)` and disables the surface; no retry loop (45-D6.11, 45-I27) |
| 45-F20 | Every workspace is ineligible (e.g. all Live daemons down) | The switcher would render a blank screen | The retained whole-list placeholder (`(no workspaces)` / `loading stored sessions…` / `(no stored sessions)`, `src/ui/ui_render.cpp:740-747`) renders (45-D4.3, 45-I7) |
| 45-F21 | Tab pressed on a non-empty draft (e.g. `/zzz`, plain text) | The agent would silently cycle | Agent cycling is gated to an empty draft; Tab no-ops (45-I20, 45-D9.1) |
| 45-F22 | `preferred_agent_` is set on a non-blank session | The status line could show the pending preset as active | `preferred_agent_` renders only as `pending_agent`; `StatusModel::agent` stays the daemon-active preset (45-I28, 45-D9.6) |
| 45-F23 | The daemon returns `UnknownSession` for `agent.prompt`/`session.resume`/`agent.select` | The focus would dangle on a session the daemon rejects | `recover_unknown_session`: erase the session (clears focus), notice, select the first remaining session or create one (45-D10.8) |

---

## 16. dsh mapping

- **Input history is per-session and presentation-only.** Like dsh, the composer
  is a view over session state; `InputModel` stays keyed by `SessionId` and
  survives switches (10 U9). 45-D1 changes only the arrow routing, not the
  storage.
- **The command list is a UI affordance, not a protocol surface.** Commands
  mutate the presentation model through `CommandContext`
  (`include/ymh/ui/command_registry.hpp:15-39`); no command writes the wire or
  the event log directly. `/mcp` and `/status` are read-only and use the existing
  RPC path (`submit_to`), never a direct daemon call.
- **Membership is derived, never duplicated.** 45-D3 makes the Live switcher a
  projection of the History catalog for its session leaves. This mirrors dsh's
  "one source of truth per projection" and avoids a second session registry on
  the UI side.
- **Agent switching is daemon-owned and blank-only.** 45-D9 preserves dsh's rule
  that a composition is fixed for a session's life (42-D4); the TUI never swaps a
  scope locally. `preferred_agent_` is the UI-local analogue of
  `preferred_model_`, applied at `session.create`.
- **MCP stays daemon-owned.** 45-D6 adds a read-only projection of
  `McpManager::statuses()` over the existing JSON-RPC transport; it does not move
  MCP state into the supervisor (15 §5.1).
- **The lockout fix is a model invariant, not a special case.** 45-D10 pins that
  focus and modeling are atomic (`ensureSessionIn` inside `focusSession`) and
  that the input router is total over the focused-session domain. This matches
  dsh's "the composer never observes a half-bound session" property.

---

## 17. Test plan

### 17.1 Unit

| Test | Covers |
|---|---|
| `UI45_D1_HistoryHoldsPromptsAndCommands` | 45-I1 |
| `UI45_D1_ArrowPrecedenceListVsHistory` | 45-I2 |
| `UI45_D2_ArrowMovesSelection` | 45-I2 |
| `UI45_D2_TabCompletesSelected` | 45-I3 |
| `UI45_D2_TabIsNotNavigation` | 45-I3 |
| `UI45_D2_TabAfterEscRecomputes` | 45-D2.3 / 45-D5.5 |
| `UI45_D2_BothCommandHintSitesPinned` | 45-D2.5 |
| `UI45_D2_CompletionCycleRetired` | 45-I4 (compile-level: the type is gone) |
| `UI45_D3_LiveIsSubsetOfHistory` | 45-I5/I6 |
| `UI45_D3_LiveHidesUncataloguedSession` | 45-I5 |
| `UI45_D3_CatalogRefreshOnOpen` | 45-D3.2 |
| `UI45_D4_FocusedSessionExcludedLive` / `…History` | 45-I5/I6 |
| `UI45_D4_LiveEmptyNodeRendered` | 45-I7 (Live; node + placeholder, no suppression) |
| `UI45_D4_LivePlaceholderLeaf` | 45-I7 (Live `(no live sessions)` only when loaded+read-ok+genuinely-empty) |
| `UI45_D4_LiveCatalogPendingPlaceholder` | 45-I7, 45-D4.3 (`(loading live sessions…)`) |
| `UI45_D4_LiveCatalogNotePlaceholder` | 45-I7, 45-D4.3 (read failed → `note` leaf, not `(no live sessions)`) |
| `UI45_D4_HistoryGenuinelyEmptyKeepsEmptyState` | 45-I7 (History, 22 §4.3; catalog had zero sessions) |
| `UI45_D4_HistoryAllHiddenDistinctLabel` | 45-I7 (History; all leaves excluded → `(current session hidden)`) |
| `UI45_D4_HistoryNoteLeafRetained` | 45-I7 (History, 22 §4.4) |
| `UI45_D4_LiveNodeFocusableHistoryReadOnly` | 45-I7, 45-D4.5 (non-live History Enter → `workspace not running` notice) |
| `UI45_D4_WholeListPlaceholder` | 45-I7, 45-F20 |
| `UI45_D5_EscHidesList` / `_EscKeepsDraft` | 45-I8 |
| `UI45_D5_EscSurvivesHistoryRecall` | 45-I9 |
| `UI45_D5_DeleteSlashHidesList` / `_SlashReopensAfterEsc` | 45-D5.3/4 |
| `UI45_D6_McpStatusWireShape` | 45-I13/I14/I15 (`skipped` is an int; `skipped_tools` array) |
| `UI45_D6_McpStatusStrictParams` | 45-I13 |
| `UI45_D6_NoMcpIsEmptyNotError` | 45-I15 |
| `UI45_D6_McpWorksWithoutSession` | 45-D6 §8.2, 45-I30 |
| `UI45_D6_McpSharedSchema` | 45-I30 (two serializers, one schema; `context.show.skipped` stays a count; `ContextServerEntry` unchanged) |
| `UI45_D6_McpMethodNotFoundDegradation` | 45-D6.11, 45-I27, 45-F19 |
| `UI45_D7_StatusRendersVersionModelApi` | 45-I16 |
| `UI45_D7_StatusUsesEffectiveModel` | 45-I17 |
| `UI45_D7_ConnectivityTransitions` | 45-I18, 45-F8 |
| `UI45_D7_StatusToolStatuses` | 45-I16, 45-F9 |
| `UI45_D7_StatusToolListBounded` | 45-I16 (20-row bound) |
| `UI45_D7_StatusNoSessionNotice` | 45-F10 analogue |
| `UI45_D8_ExitRowLiteral` / `_QuitNotSeparateRow` | 45-I19 |
| `UI45_D8_TabCompletesCanonicalName` | 45-D8.5 |
| `UI45_D8_HelpUsesDisplayName` | 45-D8.4 |
| `UI45_D9_TabCyclesAgentWhenNoList` | 45-I20 (empty draft) |
| `UI45_D9_TabNoAgentCycleOnNonEmptyDraft` | 45-I20, 45-F21 |
| `UI45_D9_TabCompletesWhenListActive` | 45-I20 |
| `UI45_D9_AgentListStrictParams` | 45-I26 |
| `UI45_D9_AgentSelectStrictParams` | 45-I26 |
| `UI45_D9_AgentListBlankCanSelect` | 45-I22, 45-D9.3 |
| `UI45_D9_AgentListActiveFoldsLog` | 45-D9.3 (`active` from last `agent_preset/selected`, not the header) |
| `UI45_D9_AgentSelectBlankSucceeds` / `_NonBlankRejected` | 45-I21 |
| `UI45_D9_PendingAgentNeverActive` | 45-I28, 45-F22 |
| `UI45_D9_PreferredAgentOnCreate` | 45-D9.7 |
| `UI45_D9_SinglePresetNoop` | 45-D9.5 (blank + no other selectable → no-op, no pending) |
| `UI45_D9_EmptyRosterNotice` | 45-I23, 45-D9.9 |
| `UI45_D9_ProtocolVersionRetained` | 45-I27 |
| `UI45_D9_MethodNotFoundDegradation` | 45-I27, 45-D9.10 |
| `UI45_D9_AgentSelectProfileDenied` | 45-I29, 45-D9.10 |
| `UI45_D10_NoActiveSessionAfterSwitcher` / `…AfterSessions` | 45-I10 |
| `UI45_D10_SlashWorksAfterRepair` | 45-I11 |
| `UI45_D10_FocusModelsTarget` / `_EnsureActiveSessionRepairs` | 45-I10/I11 |
| `UI45_D10_ActivateSessionModels` | 45-I10, 45-D10.7 |
| `UI45_D10_SingleMutator` | 45-I10 (the field is private; only `UiModel` friends write it — compile-time) |
| `UI45_D10_UnknownSessionRecovery` | 45-D10.8, 45-F23 |
| `UI45_D10_UnknownSessionCreatesFallback` | 45-D10.8 |
| `UI45_D10_AgentResumedOnPrompt` | 45-D10.6 |

### 17.2 Golden TUI render

- `UI45_G1_ExitRowRendersAlias`: the command list over `/ex` renders exactly
  `> /exit(quit) - quit the supervisor`.
- `UI45_G2_StatusBlock`: `/status` renders the pinned block including the
  per-tool rows (`name (provenance) ~N tok`), the `N available (B builtin, M mcp)`
  summary, and the per-server MCP states.
- `UI45_G3_McpBlock`: `/mcp` renders the pinned header + per-server rows.
- `UI45_G4_LiveNoSuppression`: a **Live** snapshot with the focused session
  excluded renders the remaining leaves; a Live workspace whose only session is
  focused still renders its node with the `(current session hidden)` placeholder
  (no suppression); a pending catalog renders `(loading live sessions…)` and a
  failed read renders the `note` leaf — never a false `(no live sessions)`.
- `UI45_G5_HistoryDistinctHiddenLabel`: a **History** snapshot whose workspace
  genuinely has zero stored sessions renders `(no stored sessions)` (22 §4.3); a
  History snapshot whose only stored session is the focused one renders the
  distinct `(current session hidden)` label, **not** `(no stored sessions)`; a
  workspace with a `note` renders the note leaf (22 §4.4). A non-live History node
  is rendered read-only (Enter surfaces the `workspace not running` notice).
- `UI45_G6_EmptySwitcherPlaceholder`: a snapshot with zero eligible workspaces
  renders the whole-list placeholder (`(no workspaces)` / `(no stored sessions)` /
  `loading stored sessions…`), never a blank screen.

### 17.3 Integration (FakeLLM / fake daemon)

- `UI45_I1_McpStatusEndToEnd`: a fake `TransportHost` returning two
  `McpServerStatus` entries produces the pinned JSON (`skipped` is an int,
  `skipped_tools` an array) and the pinned block; `context.show`'s `mcp_servers`
  carries `skipped` as an int and no `skipped_tools` (45-I30).
- `UI45_I2_AgentSelectEndToEnd`: a fake roster rejects a non-blank session with
  `CompositionFixed` and accepts a blank one, appending
  `agent_preset/selected`.
- `UI45_I3_LiveSubset`: a fake catalog missing a live cell hides that leaf.
- `UI45_I4_ResumeFailureLeavesFocus`: a failed `session.resume` leaves the
  previous modeled focus and surfaces a notice.

### 17.4 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- `UI45_P1_HistoryArrows` (extends `UiSupervisorPty.HelpListAndHistoryRecall`).
- `UI45_P2_EscDismissesList` — type `/`, Esc, assert the list is gone and `/`
  reopens it.
- `UI45_P3_StatusAndMcpCommands` — run `/status` and `/mcp` against a real
  daemon and assert the blocks.

---

## 18. Open questions / interpretations

- **Q1 (resolved by decision).** The item-5 text is self-contradictory ("Esc does
  not dismiss" vs "Esc makes it disappear"). 45-D5 pins both paths (Esc and
  slash-deletion) as working; see §7.2.
- **Q2 (resolved by decision).** "Current active session" for item 4 is the
  supervisor's focused session (`activeWorkspace()->activeSessionId`). **No node
  is suppressed** (Rev 4): both sources render every eligible node and use an
  explicit placeholder, distinguishing a genuinely empty source
  (`(no stored sessions)` / `(no live sessions)`) from one emptied only by the
  exclusion (`(current session hidden)`) (45-D4.3).
- **Q3 (resolved by decision).** Item 6's "manage" is interpreted as the
  read-only informational listing (like Claude Code's `/mcp`); mutations are out
  of scope (§1.4).
- **Q4 (recorded).** Item 7's "API connectivity" is a last-outcome derivation,
  not a probe (45-D7.3). If the gate requires a truthful live probe, a new spec-08
  errata is needed; the derivation is the honest minimum.
- **Q4a (recorded, HIGH-fix follow-up).** `/status` has one model source
  (`effective_model`). A daemon-authoritative model would require a real async
  `session.show` call with reply/error handling (no client calls it today); it is
  **not** pinned here. If desired, it is a separate additive change.
- **Q5 (resolved by decision).** Item 9 conflicts with 42-D4 (blank-session-only).
  The supervisor runs no local blank predicate; it reads the daemon's
  `blank`/`can_select` from `agent.list` (45-D9.3) and issues `agent.select` only
  when `can_select` is true, setting `preferred_agent_` for the next
  `session.create` otherwise (45-D9.5).
- **Q6 (recorded).** The Live ⊆ History rule depends on the catalog refresh
  cadence; a session created immediately before Ctrl+S may be hidden for one
  snapshot. This is the correct side of the subset guarantee.
- **Q7 (recorded).** `docs/design/REQUIREMENTS_BACKLOG.md:378` states `kModalTailWindow` is
  100 ms; the shipped value is 25 ms (`src/ui/supervisor.cpp:63`). The backlog is
  stale; 45-D5 relies on the 25 ms value and does not change it.
- **Q8 (factual correction).** Oracle claimed `catalog.loaded` does not exist and
  that `generation == 0` should be used. That is **false**: `SessionCatalogModel`
  declares `bool loaded = false;` (`include/ymh/ui/ui_model.hpp:427`) and the
  shipped renderer already tests it (`src/ui/ui_render.cpp:741`). 45-D3.3 pins
  `loaded` as the primary signal and `generation == 0`
  (`include/ymh/ui/ui_model.hpp:430`) as the equivalent change-detection signal.
- **Q9 (resolved by decision).** `mcp.status` vs `context.show`: not merged,
  because they differ in scope (session-less/workspace vs session/budget-bound)
  and in completeness (all configured servers incl. disabled vs the model-visible
  subset). They are deduplicated by a shared **JSON schema** implemented by **two
  serializers** (not a shared function): a function taking `McpServerStatus`
  cannot live in `ymh_agent` because `ymh_agent` does not link `ymh_mcp`
  (`CMakeLists.txt:463-471` vs the `ymh_mcp` target at `:343-350`; the type is
  `include/ymh/mcp/mcp_types.hpp:139`). The base object is
  `{id,state,tool_count,skipped:<int>,has_error}`; `mcp.status` extends it with
  `connected` and `skipped_tools`, while `/status` derives `connected` from
  `state == "ready"` (45-D6.9, 45-D7.5, 45-I30).

---

## 19. Revision log

- **Rev 6 (re-gate-fix round).** Closes the re-gate's 1 MEDIUM + 2 LOW:
  - **(1) The shared-function dedupe is WITHDRAWN.** `ymh_agent` does not link
    `ymh_mcp` (`CMakeLists.txt:463-471`; `McpServerStatus` is
    `include/ymh/mcp/mcp_types.hpp:139`, compiled into `ymh_mcp` at
    `CMakeLists.txt:347-348`), so no builder taking `McpServerStatus` may live in
    `ymh_agent`. Dedup is now a **shared JSON schema** implemented by **two
    serializers** (45-D6.9, Q9): the `context.show` serializer stays in
    `ymh_agent` (`context_snapshot.cpp:219-226`, unchanged), the `mcp.status`
    serializer stays on the host/daemon side. `ContextServerEntry` is **not**
    extended (no `connected`); `/status` derives `connected` from
    `state == "ready"` (45-D7.5).
  - **(2)** D9.3 no longer claims `StatusModel::agent` is "kept current from the
    same event projection" (there is no `AgentPresetSelected` `UiEvent`; the
    adapter has no such case and falls through its default `break;`,
    `src/ui/ui_event_adapter.cpp:182-183`).
    It is updated from the `agent.select` reply / `agent.list`.
  - **(3)** D10.7's migration list now includes `src/ui/ui_render.cpp:389,834,1151`
    and the test assignment sites (`tests/unit/ui_model_test.cpp:55,384,859`,
    `tests/unit/ui_driver_test.cpp:56`, `tests/unit/ui_event_adapter_test.cpp:33`),
    which move to `focusSessionIn`.
- **Rev 5 (re-review-fix round).** Closes the critic's 3 MEDIUM + Oracle's 3
  MEDIUM: (1/7) `skipped` is pinned as a **count** in both `mcp.status` and
  `context.show`, with a `mcp.status`-only `skipped_tools` array, and the shared
  shared function is WITHDRAWN (`ymh_agent` does not link `ymh_mcp`); the dedupe
  is a shared JSON schema implemented by two serializers, and `/status` derives
  `connected` from `state == "ready"` — so `context_snapshot.cpp`'s
  `get<std::size_t>()` and `/context`'s `skipped > 0` render are unchanged
  (45-D6.9/§8.2, 45-A11, 45-I30); (2) the false "every rendered node is
  focusable" claim is withdrawn — a live/`Attached` node is focusable, a non-live
  History node is read-only with the existing `workspace not running` notice
  (45-D4.5, 45-I7, 45-F5, G4/G5); (3) the Live `(no live sessions)` label is
  emitted only when the catalog is loaded and the read succeeded — a pending
  snapshot renders `(loading live sessions…)` and a failed read renders the `note`
  leaf (45-D3.3, 45-D4.3, 45-I7, 45-F4, `WorkspaceNode::catalog_pending`); (4/5)
  the stale §13 `cycle_agent` comment and the D2.3 "falls through to agent
  cycling" text are corrected to the empty-draft-only rule; (6) the blank +
  `can_select == false` (single-preset) case is pinned to a no-op with a one-time
  `"no other agents available"` notice (45-D9.5); (8) `activeSessionId` becomes
  **private** with a public getter and a private setter, `UiModel` a friend — the
  single-writer rule is now compile-time (45-D10.7, 45-I10, §13 `WorkspaceModel`);
  (9) `agent.list.active` folds the session's last `agent_preset/selected` event
  (falling back to the creation-time header), because the header is never updated
  after a switch (45-D9.3, `UI45_D9_AgentListActiveFoldsLog`).
- **Rev 4 (Oracle-fix round).** Closes the second reviewer's 4 HIGH + 6 MEDIUM
  design findings: (H1) `agent.list` gains daemon-owned `blank`/`can_select`
  (45-D9.3) so the supervisor never reimplements 42-I3; (H2) no workspace node is
  suppressed — both sources render every eligible node and use explicit
  placeholders (`(current session hidden)` / `(no live sessions)` /
  `(no stored sessions)` / `note`) plus the retained whole-list placeholder
  (45-D4.3/4.5, 45-I7); (H3) Tab agent-cycling is gated to an **empty** draft
  (45-D9.1) and `preferred_agent_` renders only as `pending_agent`, never as the
  active agent (45-D9.6, 45-I28); (H4) protocol-version neutrality + a
  `MethodNotFound`/`MethodNotAllowedForProfile` degradation path for all three new
  RPCs (45-D6.11, 45-D9.10, 45-I27). MEDIUMs: `mcp.status` and `context.show` share
  one builder and `/mcp` is session-less (45-D6.9/§8.2); 45-I10 is mechanically
  enforced through a single `focusSessionIn` mutator plus `UnknownSession`
  recovery (45-D10.7/8); the `catalog.loaded` claim is corrected with
  `generation == 0` added (45-D3.3, Q8); the roster wiring and empty-roster
  notice are pinned (45-D9.8/9); `/status` shows per-tool statuses, not a count
  (45-D7.5); and the RPC profiles are pinned (45-D6.10, 45-D9.10, 45-I29).
- **Rev 3 (re-gate-fix round).** Closes the single remaining MEDIUM: the History
  source no longer renders a false `(no stored sessions)` label when the only
  stored session is the hidden focused one. `45-D4.3` now distinguishes a
  genuinely empty catalog read from an all-leaves-excluded node, pins
  `WorkspaceNode::sessions_hidden_by_focus`, and the distinct truthful label
  `(current session hidden)`; `45-I7`, `45-F5`, the §13 sketch, §18 Q2, and the
  golden test (renamed `UI45_G5_HistoryDistinctHiddenLabel`) were updated, and
  two unit tests (`UI45_D4_HistoryGenuinelyEmptyKeepsEmptyState`,
  `UI45_D4_HistoryAllHiddenDistinctLabel`) separately pin the two cases.
- **Rev 2 (gate-fix round).** Closes the independent gate's 2 HIGH + 10 MEDIUM +
  LOW findings: (H1) D4.3 scoped to the Live source, 22 §4.3/§4.4 retained, G4/G5
  split; (H2) the false `session.show` claim dropped, `/status` uses
  `effective_model` only, Q4a records the follow-up; (M1) the
  `complete_selected_command` sketch recomputes after Esc; (M2)
  `AppCode::CompositionFixed = -32020` added to §13 and the `kProtocolVersion`
  decision pinned (45-D9.10); (M3) both `CommandHint` construction sites pinned
  (45-D2.5); (M4) strict `agent.list`/`agent.select` params + 45-I26/F17; (M5)
  the untestable `mcp.status.note` removed; (M6) `AssistantMessageFinished`/
  `TokenUsageUpdated`; (M7) amendment claims for specs 04/21 dropped and
  `10 §8.2` retargeted to `10 §9.2`/`25 §7`; (M8) `activate_session` now models
  and the root-cause attribution is corrected; (M9) the supervisor-local blank
  predicate removed (always defer to the daemon). LOWs: `live_switcher_renderable`
  citation, dead `hints_dismissed_draft` removed, D5.2 cross-ref, `:1945-1952`,
  `src/ui` agent-reference wording, session-less `/mcp`, daemon-active-only agent
  display, and the fresh-start-empty reconciliation.
- **Rev 1** — initial errata: pins 45-D1…45-D10 for the ten requirements,
  supersedes 17 §5 RB-08 Tab-cycling and 25-D8/D12, and adds the `mcp.status`,
  `agent.list`, and `agent.select` RPCs. No implementation; awaiting the gate.

# 81 - Session Dashboard (Arrow-Left Agent View) Errata

```
Status: verified (Rev 6)
Revision: 6
Verification status: adversarial reviewer (independent, Rev 6): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 81 (errata) -- adds a full-screen session dashboard on the
           `ui::run_supervisor` path: a new `UiMode::Dashboard` whose renderer
           takes over the terminal (no `clear_under` window), opened by
           ArrowLeft on an empty composer (Claude Code "agent view") and by
           Ctrl+S / Ctrl+P. It supersedes the Live popup half of 22 and 57 and
           amends 10 sec 7.1/7.2. `/sessions` (History) and the Ctrl+T Subagents
           popup are unchanged.
Depends on: 00-architecture.md (verified; sec 9.2/9.9/sec 20.24/sec 54 D14/D16),
           06-agent-loop.md (verified; `AgentState`),
           10-supervisor-tui.md (verified; sec 4.3/4.4/4.6/sec 7/sec 9),
           16-daemon-ownership.md (verified; sec 4 exit prompt),
           22-switcher-sessions-errata.md (verified; S1/S2 split),
           45-ui-interaction-errata.md (verified; key precedence, command list),
           48-ui-and-config-errata.md (verified; `EscArm`, kEscArmTimeout),
           51-reasoning-fold-prompt-emphasis-and-tables-errata.md (verified;
           reused two-press arm),
           57-switcher-sessions-popup-errata.md (verified; ordering, key
           ownership), 60-scroll-keybindings-errata.md (verified; arrow keys),
           63-bottom-activity-indicator-errata.md, 64-ui-polish-errata.md,
           UI_SURFACE_INVENTORY.md (Surface 3)
Supersedes: (a) `22` sec 3 (S1, the Live-only Ctrl-S switcher) as the surface
            Ctrl-S opens, `22-switcher-sessions-errata.md:267-679`; (b) `22`
            sec 7 SW1-SW16 as they bind the Live popup surface, `:1567-1603`
            (the eviction and live-predicate half is RETAINED, 81-D10/81-D11);
            (c) the Live half of `57` D2/D3/D4's popup: the Live popup footer,
            inline delete badge and mode-only key ownership move to the
            dashboard surface, `57-switcher-sessions-popup-errata.md:358-572`;
            (d) `10` sec 7.1 `:852-854` ("`Ctrl+S` (fallback `Ctrl+P`) opens ONE
            tree overlay ... It shows ALL workspaces") and sec 7.2 `:876-881`
            (the Live keybinding table); (e) `46-D3` / `49-D6` and the `UiMode::Notice` surface it built: the
            single-OK `UiMode::Notice` for "No other sessions available"
            (`ui_event.hpp:52`, `src/ui/supervisor.cpp:1259-1271`) is replaced by
            an inline dashboard empty state (81-D9); because `openSwitcher()` was
            `UiMode::Notice`'s only producer, the mode/handler/renderer/field are
            removed, not retained (81-D4); (f) the doc summaries that describe
            Ctrl-S as the Live popup: `AGENTS.md:141-142` (the exact phrase
            "**Ctrl-S switcher is live-only** (live daemon with
            `Attached`/`Stopping`; entries are evicted when the daemon dies)")
            and `UI_SURFACE_INVENTORY.md:58-105` (Surface 3, esp. "Current
            (Live)" `:60-77`). Their replacement text is pinned in sec 1.3.1;
            editing them is orchestrator-owned. The History popup (`/sessions`)
            and the Subagents popup (`Ctrl+T`) are NOT superseded.
Amends:     `10` sec 7.3 `:883-889` ("While the overlay is open, the underlying
            `InputView` must not receive keys"): the dashboard is a third
            focus owner alongside the popups (81-D1, 81-D8); `10` sec 8.1
            `:909-937` (component hierarchy gains a Dashboard root variant);
            `10` sec 9.1 `:1038-1044` (focus rule).
Scope: exact file:line anchors for the current surfaces; one new `UiMode`
       value; one new full-screen renderer; one dashboard model with a bounded
       field set; the ArrowLeft-on-empty-composer open gesture and its
       precedence against the composer cursor binding; the status vocabulary and
       grouping mapped onto ymh's `AgentState`/`AttentionState` (and what is NOT
       derivable); list columns; keys including double-Ctrl+C-to-quit guarded by
       the existing exit confirm; empty/scroll/narrow behaviour; a state-lifetime
       table; invariants; failure modes; a dsh mapping; a test plan with golden
       renders. Design only -- no implementation is written here.
Related:   76-host-startup-residue-recovery-errata.md (startup), 78/79/80
           (`/fork`, `/rewind`, file checkpoints), 82 (`/handoff`).
```

This document is the design gate for the dashboard. It is **additive**: it pins
new text, interface names, invariants, failure modes, and tests. It does not
rewrite `00-architecture.md`, `10-supervisor-tui.md`, `22-...`, `57-...`, or
`UI_SURFACE_INVENTORY.md`; each superseded/amended clause is quoted with
`file:line` and its replacement is given here. The convention matches specs 16,
17, 21, 22, 57, and 76.

**Naming note.** Invariants local to this spec are **`81-I1`-`81-I21`**; failure
modes are **`81-F1`-`81-F13`**; decisions are **`81-D1`-`81-D13`**. The prefixes
`S` (01), `P` (02), `R`/`H` (03/04), `T` (05), `A` (06), `X` (07), `Q` (09),
`U` (10), `M` (11), `C` (13), `O` (16), `U-RB*` (17), `CTX` (18), `SK` (20),
`J` (21), `SW` (22), `L-`/`UI25`/`UI45` (25/45), `48-`/`49-`/`50-`/`51-`/`53-`/
`56-`/`57-`/`58-`/`60-`/`64-`/`65-`/`67-`/`76-` are all taken. **`81-` is
unused.** New tests are `Errata81.*` (hermetic) and `UiRenderGolden.Dashboard*`
(render). ASCII only.

**No new RPC (pinned).** The dashboard adds **zero** wire methods. It is a pure
projection of state the supervisor already holds: `WorkspaceModel::sessions`
(`SessionCell`, populated by `session.list`) and the existing daemon-reported
`live` flag. `/sessions` still reads disk directly. No schema change.

---

## 1. Purpose, scope, and supersession

### 1.1 The problem

Ctrl-S today opens a **centered popup** (`render_switcher`,
`src/ui/ui_render.cpp:1496-1532`) over the conversation: a `ftxui::window(...) |
ftxui::clear_under | ftxui::center` (`:1530-1531`). It lists live workspaces and
their sessions in a tree, with the focused session hidden
(`src/ui/ui_model.cpp:1466-1471`). `/sessions` reuses the **same** popup and
renderer with `source = History` (`src/ui/supervisor.cpp:1066-1075`).

The user asked for a Claude-Code-style **agent view**: pressing ArrowLeft on an
**empty** composer opens a **full-screen** session list that takes over the
terminal, groups sessions by what they need from the user, and shows counts. A
popup cannot be that surface: it is a `clear_under` window, so the conversation
stays visible behind it and the list can never use the full grid. The dashboard
must be a **new mode with its own full-screen renderer**, not a restyled popup
(`/tmp/opencode/design-brief.md` sec D, line 100-103).

### 1.2 What this changes, in one sentence each

- A new `UiMode::Dashboard` (`81-D1`) whose renderer replaces the base view
  entirely and does not use `clear_under`; overlays (exit confirm, permission
  dialog, context, notice) still compose over it.
- A new `DashboardModel` (`81-D2`) built from the existing **Live** projection;
  it is a pure snapshot rebuilt on open and on every catalog delivery, exactly
  like the switcher (`81-D12`).
- The **open gesture**: ArrowLeft on an empty composer (`81-D3`), plus Ctrl+S /
  Ctrl+P as an alias (`81-D4`). Opening is a pure view switch; it does NOT
  background, detach, stop, or activate anything.
- The **status vocabulary and grouping** (`81-D5`, `81-D6`): Working / Needs
  input / Idle / Completed / Failed mapped from `AgentState` + `AttentionState`;
  groups Needs input > Working > Completed. Stopped, Pinned, and Ready for
  review are recorded as **not derivable** with the reason.
- **Columns and counts** (`81-D7`): status, title, workspace, relative age, and
  a `N awaiting input - M working - K completed` summary.
- **Keys** (`81-D8`): Up/Down or j/k, PgUp/PgDn, Home/End, Enter or Right to
  attach (focus only), Tab to collapse a group, Esc to return restoring prior
  focus/scroll, and double Ctrl+C to quit guarded by the existing exit confirm.
- **Empty/scroll/narrow** (`81-D9`): inline empty state, cursor-anchored window,
  column degradation.
- Ctrl-S becomes a dashboard alias; `/sessions` (History) and Ctrl+T (Subagents)
  are untouched (`81-D10`, `81-D11`).

### 1.3 Supersession / amendment map

| Clause superseded/amended | Anchor | Replacement |
|---|---|---|
| 22 S1 "Live-only Ctrl-S switcher" as the Ctrl-S surface | `22-switcher-sessions-errata.md:267-679` | Dashboard (81-D1/81-D4); the live predicate/eviction is retained (81-D10) |
| 22 SW1-SW16 bound to the Live popup | `22-switcher-sessions-errata.md:1567-1603` | 81-I1..81-I21; live-filter half retained |
| 57 Live popup footer / delete badge / key ownership | `57-switcher-sessions-popup-errata.md:358-572` | The dashboard owns Ctrl-S keys (81-D8); 57's History half retained (81-D11). The Live-popup *models* (`SwitcherOverlayModel::open`, `render_switcher`'s Live branch, the `kSwitcherPolicy[Live]` values except the three repurposed fields) stay reachable for History/Subagents and for the latent-Live goldens (81-I20, sec 10.5) |
| 10 sec 7.1 Ctrl+S opens ONE tree overlay | `10-supervisor-tui.md:852-854` | 81-D4 |
| 10 sec 7.2 Live keybinding table | `10-supervisor-tui.md:876-881` | 81-D8 |
| 46-D3 / 49-D6 empty-Live `Notice` | `ui_event.hpp:52`, `src/ui/supervisor.cpp:1259-1271` | Inline dashboard empty state (81-D9); the now-dead `UiMode::Notice`/`handle_notice`/`render_notice`/`UiModel::message` are removed (81-D4) |
| `AGENTS.md` switcher summary | `AGENTS.md:141-142` | 81-D4/81-D10; replacement text pinned in sec 1.3.1 (doc-sync; header edit is orchestrator-owned) |
| UI_SURFACE_INVENTORY Surface 3 | `UI_SURFACE_INVENTORY.md:58-105` | 81-D4/81-D10; replacement text pinned in sec 1.3.1 (doc-sync; orchestrator-owned) |

### 1.3.1 Doc-summary reconciliation (orchestrator-owned; non-contradictory)

This spec is additive and does **not** edit either summary. It pins the exact
replacement each needs so the merge tree cannot carry two contradictory
descriptions of Ctrl-S. Until those edits land, the summaries are **stale but
not authoritative**: this spec supersedes them where they describe Ctrl-S.

| Doc | Stale clause (quoted) | Pinned replacement |
|---|---|---|
| `AGENTS.md:141-142` | "The **Ctrl-S switcher is live-only** (live daemon with `Attached`/`Stopping`; entries are evicted when the daemon dies; `NotRunning`/`Unreachable` never render), ordered effective-root-first, then last-usage descending ..." | "The **Ctrl-S dashboard** is live-only (live daemon with `Attached`/`Stopping`; entries are evicted when the daemon dies; `NotRunning`/`Unreachable` never render), ordered effective-root-first, then last-usage descending ...; it supersedes the Live popup half of 22 and 57 (`docs/design/81-session-dashboard-errata.md`). `/sessions` (History) and Ctrl+T (Subagents) still use the popup." |
| `AGENTS.md:144-152` | the `/sessions` / `ymh --resume` paragraph | unchanged (History is retained) |
| `UI_SURFACE_INVENTORY.md:58-77` | Surface 3 "Current (Live)" describing `SwitcherOverlayModel`/`render_switcher` as the Ctrl-S surface | split Surface 3 into "Surface 3a -- Dashboard (Live, Ctrl-S)" (this spec: `UiMode::Dashboard`, `render_dashboard`, `DashboardModel`) and "Surface 3b -- History/Subagents popup" (retained: `render_switcher`, `SwitcherOverlayModel`, `/sessions`, Ctrl+T, 57-D5 ordering). Surface 3b keeps the existing text verbatim; Surface 3a records the live-only predicate (81-D10) as retained. |
| `UI_SURFACE_INVENTORY.md:78-105` | Surface 3 "Current (History)" | unchanged (History is retained; becomes Surface 3b) |

### 1.4 Scope boundaries

In scope: the dashboard mode, model, renderer, open/close/keys, vocabulary,
grouping, columns, counts, empty/scroll/narrow, and the Ctrl-S re-point.

Out of scope: `/fork`, `/rewind`, file checkpoints, `/handoff` (specs 78-82);
any change to `/sessions` beyond sharing `render_switcher`; any new RPC or
schema; pinning, PR linkage, or dispatch of new sessions from the dashboard
(81-D5 records these as non-derivable); remote transport.

### 1.5 Terminology (pinned)

- **Live session** = a session in a `WorkspaceModel` whose daemon is
  `Attached`/`Stopping` (the `live_switcher_renderable` predicate, used at
  `src/ui/ui_model.cpp:1444`), carrying a `SessionCell` from the daemon's
  `session.list`. This is the dashboard's universe (81-D10).
- **Stored session** = a row in `<workspace>/.ymh/sessions.db`, surfaced by
  `/sessions` (`src/ui/session_catalog.cpp:96-243`). Never the dashboard's
  universe.
- **Focused session** = the supervisor-local `activeSessionId` of the active
  workspace (`UiModel::focusSession`); distinct from the daemon's
  `active_session_`. The dashboard shows the focused session too (81-D7), unlike
  the popup.
- **Attach** = set the focused session and switch the active workspace; NEVER
  `session.activate` (retained from 22 sec 5.2, `src/ui/supervisor.cpp:1669-1672`).

---

## 2. Verified current behaviour (not trusted from the brief)

Every claim below was re-verified against the shipped tree at the cited line.

### 2.1 UiMode and the overlay composition

- `UiMode` is `{Conversation, Switcher, Dialog, ExitConfirm, Context, Notice,
  ModelPicker}` (`include/ymh/ui/ui_event.hpp:45-55`).
- Overlay composition in `build_ui` is a fixed precedence `if`-chain. It
  computes a `base` element first -- today `main` (a bordered vbox of
  header/transcript/composer/status, root at `src/ui/ui_render.cpp:2196`); this
  spec makes it `dashboard.open ? render_dashboard(...) : main` (81-D1) -- then
  returns `dbox({base, overlay})`: `exitConfirm` (`:2197-2199`), `dialog`
  (`:2200-2202`), `Context && open` (`:2203-2205`), `Notice && message.open`
  (`:2206-2207`), `Switcher` (`:2209-2211`), `ModelPicker && visible`
  (`:2212-2214`), else `base` (`:2215`). Chain R in sec 2.2 pins this order and
  the `dashboard.open` selector.
- The switcher overlay is a `clear_under` window (`src/ui/ui_render.cpp:1530-1531`);
  so are the dialog (`:1162-1163`) and exit confirm (`:1202-1203`).
- `render_to_ansi` (`:2218-2224`) is the render entry used by golden tests.

### 2.2 Key precedence: three ordered chains (complete)

Every keystroke reaches the model through exactly one of two dispatch chains,
**chain A** (`SupervisorApp::handle_event_inner`) then, only if A did not
consume it, **chain B** (`SupervisorApp::handle_input`). The dashboard adds one
guard to each. Separately, **chain R** (`build_ui`, `ui_render.cpp:2196-2215`)
is the *render* precedence: it decides which element is the base under an
overlay. R is not a key chain, but it is pinned here because selecting the base
by `dashboard.open` (81-D1) is what makes the exit-confirm handoff correct
(81-D13, MEDIUM-1).

**Chain A -- supervisor dispatch (`handle_event_inner`,
`src/ui/supervisor.cpp:3810-3904`).** Exact order, each entry the guarding
predicate:

| # | Predicate / branch | Lines | Consumes |
|---|---|---|---|
| A1 | `handle_paste_event(event)` | `:3811-3813` | bracketed paste |
| A2 | `event == Custom` -> `drain()` | `:3814-3817` | repaint tick |
| A3 | `model_.exitConfirm.open` | `:3818-3820` | all keys -> `handle_exit_confirm` |
| A4 | `model_.dialog.open` | `:3821-3824` | all keys -> `handle_dialog` |
| A5 | `mode == Context && context.open` | `:3825-3827` | all keys -> `handle_context` |
| A6 | `mode == Notice && message.open` | `:3828-3829` | **removed by 81-D4** |
| A7 | `mode == Switcher` | `:3831-3833` | all keys -> `handle_switcher` |
| A8 | `mode == ModelPicker && visible` | `:3834-3836` | all keys -> `handle_model_picker` |
| A9 | **`mode == Dashboard` -> `handle_dashboard`** | new, inserted after `:3836` | all keys (81-D8); see the key map below |
| A10 | subagent child Ctrl+C suppression | `:3839-3841` | Ctrl+C while `!subagent_path.empty()` |
| A11 | Ctrl+T -> `open_subagents()` | `:3843-3845` | Ctrl+T |
| A12 | Ctrl+S / Ctrl+P -> `open_dashboard()` | `:3847-3853` | Ctrl+S, Ctrl+P |
| A13 | Ctrl+Q -> `begin_exit(true)` | `:3855-3858` | Ctrl+Q |
| A14 | Ctrl+C -> `cancelActive()` | `:3859-3862` | Ctrl+C |
| A15 | Ctrl+N -> `new_session()` | `:3863-3866` | Ctrl+N |
| A16 | Ctrl+O -> `toggle_folds()` | `:3867-3869` | Ctrl+O |
| A17 | spec-60 scroll block | `:3871-3902` | see the non-conflict proof |
| A18 | `return handle_input(event)` | `:3903` | everything else -> chain B |

**Chain B -- composer input (`handle_input`, `:3333-3495`).** Exact order:

| # | Predicate / branch | Lines | Note |
|---|---|---|---|
| B1 | `active()` / `ensureActiveSession()` / zero-workspace `pendingComposer` | `:3334-3359` | resolves the target state |
| B2 | `!subagent_path.empty()` guard | `:3354-3359` | Esc pops one level; **every other key consumed**, so a viewed child never opens the dashboard (81-I4) |
| B3 | **`event == ArrowLeft && input.draft.empty()` -> `open_dashboard(); return true;`** | new, inserted after `:3359`, before `:3360` | the open gesture (81-D3) |
| B4 | `event.is_character() && modal_tail_suppression_active()` | `:3361-3363` | characters only; ArrowLeft is not a character |
| B5 | caret-grace arm + Esc disarm block | `:3366-3379` | arms `composer_input_at`; disarms `esc_arm` |
| B6 | command-list / Esc handling | `:3380-3406` | Esc |
| B7 | `ArrowLeftCtrl` -> `word_left_boundary` | `:3481-3485` | Ctrl+Left (untouched by B3) |
| B8 | `ArrowRightCtrl` -> `word_right_boundary` | `:3486-3490` | Ctrl+Right |
| B9 | `ArrowLeft` -> `cursor_left` | `:3491-3495` | reached only when B3 did not fire |
| B10 | remaining edit/submit keys | `:3496-3611` | ... |

**Dashboard key map (`handle_dashboard`, 81-D8), all inside A9:** ArrowUp/`k`
-> `moveUp`; ArrowDown/`j` -> `moveDown`; PageUp -> `pageUp`; PageDown ->
`pageDown`; Home -> `moveHome`; End -> `moveEnd`; Tab -> `toggle_collapse`;
Enter/ArrowRight -> attach (focus-only, 81-D8.1); Escape -> close/restore
(81-D13); Ctrl+C -> two-press exit arm (81-D8.2); Ctrl+Q -> consumed no-op; any
other key -> consumed no-op. There is no ArrowLeft binding inside
`handle_dashboard`, so A9 consumes it before chain B's B3 and before the summary
"any other key" clause.

**Spec-60 non-conflict proof.** Spec 60 binds **exactly** these keys globally in
chain A's A17 block (`60-D5`, `60-scroll-keybindings-errata.md:108-110`):
`PageUp`/`PageDown` (`scroll_by(_, true)`), `Ctrl+Home`/`Ctrl+End`
(`scroll_to_top`/`scroll_to_bottom`), `Shift+Up`/`Shift+Down`
(`scroll_by(_, false)`), `Shift+PageUp`/`Shift+PageDown`
(`scroll_to_top`/`scroll_to_bottom`). Properties:

1. **ArrowLeft is not in the spec-60 set.** B3 binds ArrowLeft only through
   chain B, which is reached only after A17. When the dashboard is closed,
   ArrowLeft falls through A17 to B3 exactly as before; when the dashboard is
   open, A9 consumes it. No spec-60 key is ArrowLeft, so spec 60 and B3 share no
   key.
2. **The dashboard guard A9 precedes A17.** While `mode == Dashboard`, every
   spec-60 key reaches `handle_dashboard` and is consumed by the 81-D8 "any
   other key" clause (the dashboard's own PageUp/PageDown/Home/End are its *own*
   bindings and are handled explicitly, never routed to `scroll_by`). Thus the
   transcript is frozen while the dashboard is up (81-I19).
3. **Spec 60 is intact when the dashboard is closed.** A9's predicate is
   `mode == Dashboard`; on every other mode the chain is byte-for-byte the
   shipped order, so A17 fires exactly as before.

**Chain R -- render base precedence (`build_ui`,
`src/ui/ui_render.cpp:2196-2215`).** Order: compute `base` from
`dashboard.open ? render_dashboard(...) : (vbox(...) | border)` (`:2196`), then
`exitConfirm.open` -> `dbox({base, exit_confirm})` (`:2197-2199`), `dialog.open`
-> `dbox({base, dialog})` (`:2200-2202`), `Context && open` (`:2203-2205`),
`Notice && message.open` (`:2206-2207`; removed by 81-D4), `Switcher`
(`:2209-2211`), `ModelPicker && visible` (`:2212-2214`), else `base` (`:2215`).
R uses `dashboard.open`, not `mode == Dashboard`, so an overlay raised from the
dashboard composes over the dashboard base; see 81-D13 for the matching
mode save/restore on chains A3/A4/A5.

### 2.3 The Ctrl-S Live switcher

- Ctrl-S/Ctrl-P call `openSwitcher()` (`supervisor.cpp:3847-3854`).
- `SupervisorApp::openSwitcher()` (`:1259-1271`) opens a `UiMode::Notice` when
  `switcher_has_targets()` is false (`:1230-1254`), else `UiModel::openSwitcher()`
  (`src/ui/ui_model.cpp:1375-1380`) sets `source = Live`, builds the tree via
  `SwitcherOverlayModel::open()` and sets `mode = UiMode::Switcher`.
- `SwitcherOverlayModel::open()` (`src/ui/ui_model.cpp:1430-1510`) builds
  workspace nodes only for `live_switcher_renderable` workspaces (`:1444`) and
  **hides the focused session** (`:1466-1471`).
- Live/predicate retention: `evict_dead_workspaces` (`:1776`) feeds
  `resnapshot_switcher()` (`:1443-1448`) on live-set changes; catalog deliveries
  rebuild the Live/History snapshot in `on_catalog_snapshot` (`:1045-1062`).

### 2.4 `/sessions` (History) is a different surface

`/sessions` calls `open_sessions()` (`supervisor.cpp:1066-1075`): History
source + `openHistory` + `mode = Switcher` + catalog refresh. It reads
`<workspace>/.ymh/sessions.db` for every registered workspace, live or not
(`src/ui/session_catalog.cpp:96-243`), and reuses the same popup renderer
(`render_switcher`). This split (Ctrl-S = live; `/sessions` = stored) is the
shared contract and is **preserved** (81-D10).

### 2.5 Selection is focus-only

The Live Enter path (`supervisor.cpp:3060-3089`) ends in
`UiModel::focusSessionIn`/`focusWorkspace` (`:3078-3080`). The History path
(`apply_resume_success`) calls `focusSession` at `:1669-1672` with the pinned
comment `"focus only; no session.activate"`. The dashboard retains this exactly
(81-D8).

### 2.6 Status data available to the UI

- `AgentState` = `{Idle, Thinking, CallingTool, WaitingForPermission,
  WaitingForInput, Cancelling, Error}` (`include/ymh/agent/agent.hpp:31-39`).
- `is_active_state` = `Thinking || CallingTool` (`src/ui/ui_model.cpp:597-599`);
  `is_waiting_state` = `WaitingForInput || WaitingForPermission || Error`
  (`:601-605`).
- `AttentionState { lastState, needsInput, completed }`
  (`include/ymh/ui/ui_model.hpp:295-299`); set on each state transition:
  `lastState = newState`, `needsInput = is_waiting_state(newState)`,
  `completed = (newState == Idle)` (`src/ui/ui_model.cpp:1219-1221`).
- `session_working` = `is_active_state(agent_state)` OR any **direct** subagent
  `Running` (`src/ui/ui_model.cpp:615-625`).
- `SessionCell { id, title, state, attention, unread, readOnly }`
  (`ui_model.hpp:354-361`), where `state = agent_state`, `attention =
  attention.needsInput`, `unread = attention.completed && not focused`
  (`src/ui/ui_model.cpp:914-916`, the block; `attention` `:915`).
- `SessionUiState` carries `agent_state` (`ui_model.hpp:351`), `attention`
  (`:320`), `subagents` (`:321`); `SessionCell` does not carry `updatedAt`.
- Per-session `updatedAt` lives only in the catalog snapshot
  (`SessionHistoryEntry::updatedAt`, `include/ymh/ui/session_catalog.hpp:33-39`);
  `model.catalog_has_session` (`ui_model.cpp:1475`) is the existence predicate
  only, so the dashboard resolves the value by iterating
  `model.catalog.workspaces[ws].sessions` and matching the `SessionId` (the join
  `SessionCell` cannot carry, 81-D5/81-D7). Workspace `lastUsedAt` is
  `WorkspaceHistory::lastUsedAt` = `max(session.updatedAt)` (57-D6),
  copied into the row as `workspace_last_used` (81-D2/81-D6).
- A `SessionUiState` (the only carrier of `attention.completed`,
  `ui_model.hpp:320`, and `subagents`, `:321`) is reachable per id via
  `UiModel::session(id)` (`ui_model.hpp:692-693`, `ui_model.cpp:791-799`); the
  matching `SessionCell` is in `WorkspaceModel::sessions` (`ui_model.hpp:370`).
  Together they are the dashboard's status inputs (81-D2/81-D5).
- `daemonStatus` is per workspace (`DaemonStatus`), and `UiMode`/renderer read
  `state_glyph` (`src/ui/ui_render.cpp:233-250`) for the existing state marks.

---

## 3. Decisions

### 81-D1 -- A new `UiMode::Dashboard` with a full-screen renderer

Add `Dashboard` to `UiMode` (`include/ymh/ui/ui_event.hpp:45-55`). Its renderer
`render_dashboard(model, size, theme)` returns the **entire** terminal grid: a
top title+counts line, the grouped list, and a key-hint footer, sized to
`size.height`. It MUST NOT call `ftxui::clear_under` and MUST NOT be wrapped in
`ftxui::center`/`ftxui::window`.

`build_ui` (`src/ui/ui_render.cpp:2196`) builds a `base` element. The selector
is **`dashboard.open`, not `mode == Dashboard`** -- this is what lets an overlay
raised from the dashboard (the exit confirm, a permission dialog) compose over
the dashboard rather than over the conversation:

```cpp
Element base = model.dashboard.open
                   ? render_dashboard(model, size, theme)
                   : (vbox(...) | border);   // the existing conversation root
```

Every existing overlay branch then returns `dbox({base, overlay})`, and the
final fallthrough returns `base`. Because the Switcher/ModelPicker branches are
mutually exclusive with a raised dashboard (`open_dashboard` is only reached
when no overlay owns input, sec 2.2 A7/A8/A12), they still see `base == main`;
no overlay precedence changes. Result: exit confirm, permission dialog, and
`/context` compose **over** the dashboard (81-I1, 81-I2), while the conversation
never shows through it. (The `Notice` branch is gone; 81-D4.) Chain R in sec 2.2
pins this order. The matching mode save/restore that keeps `dashboard.open`
consistent across an overlay is 81-D13; without it `dashboard.open` and `mode`
would diverge (the MEDIUM-1 defect).

Dispatch: add, after the ModelPicker guard (`supervisor.cpp:3834-3836`) and
before the subagent Ctrl+C suppression (`:3839`):

```cpp
if (model_.mode == UiMode::Dashboard) {
    return handle_dashboard(event);
}
```

The predicate is `mode == Dashboard` (not `dashboard.open`): A3/A4/A5 precede
A9, so while an overlay is up over the dashboard that overlay owns the keys; A9
fires only when the dashboard is both open and the top focus owner. This makes
the dashboard consume every key before the globals, exactly like the popups
(57-I10/57-I15).

### 81-D2 -- `DashboardModel` (bounded state) and its lifecycle

Add `DashboardModel dashboard;` to `UiModel` (`include/ymh/ui/ui_model.hpp`).
The model is a pure, renderer-independent snapshot. It reuses the **Live
projection** rather than duplicating it: `UiModel::openDashboard()` sets
`switcher.source = SwitcherSource::Live` and calls a new
`SwitcherOverlayModel::openLive(model, /*include_focused=*/true)`, then projects
`switcher.workspaces` into `dashboard.rows` (81-D5..81-D7).

For each projected `SessionNode`, `open`/`rebuild` resolve the **live cell** by
id from the owning `WorkspaceModel::sessions` (`include/ymh/ui/ui_model.hpp:370`)
and the model state via `UiModel::session(id)` (`ui_model.hpp:692`,
`ui_model.cpp:791`), then set `status = dashboard_status(cell, ui_state)` and
`group = dashboard_group(status)`. This bridge is required because neither
`SessionNode` (`ui_model.hpp:413-425`) nor `SessionCell` (`:354-361`) carries
`attention.completed` or `subagents` (81-D5); `SessionUiState` does
(`:320-321`). The workspace `lastUsedAt` copied from the `WorkspaceNode`
(`ui_model.cpp:1457`) is stored on the row as `workspace_last_used` so ordering
rule 2 (81-D6) is evaluable from the row alone.

`openLive` is the ONLY new projection entry point. The existing
`SwitcherOverlayModel::open(model)` is retained and becomes
`return openLive(model, /*include_focused=*/false)` so the popup's focused-
session hiding (`ui_model.cpp:1466-1471`) is unchanged for History/Subagents
usage; the symbol keeps a compile-time caller (`resnapshot_switcher`,
`supervisor.cpp:1443-1448`) but that caller is guarded by `mode == Switcher &&
source == Live`, a state no production path reaches after 81-D4, so `open(model)`
is dynamically dead yet retained (latent, 81-I20). The dashboard is the only
caller of `openLive(..., true)`.

Fields (all UI-only; nothing persisted):

```cpp
struct DashboardCounts { std::size_t needs_input=0, working=0, completed=0,
                         idle=0, failed=0; };

struct DashboardModel {
    bool                        open = false;          // mirrors mode; see 81-I3
    std::vector<DashboardRow>   rows;                  // grouped + ordered (81-D6)
    std::size_t                 cursor = 0;            // index into rows
    UiMode                      prev_mode = UiMode::Conversation; // 81-D3
    EscArm                      exit_arm = EscArm::Disarmed;      // 81-D8
    std::optional<std::chrono::steady_clock::time_point> exit_armed_at;
    std::set<DashboardGroup>    collapsed;             // Tab (81-D8)

    void open(const UiModel& model);      // rows, prev_mode, cursor, disarm
    void rebuild(const UiModel& model);   // re-project in place, preserve cursor
    void close();                         // rows.clear(); cursor=0; disarm
    void moveDown();  void moveUp();
    void pageDown();  void pageUp();
    void moveHome();  void moveEnd();
    void toggle_collapse();
    [[nodiscard]] DashboardCounts counts() const;
};
```

`open()` **first saves `prev_mode` from `model.mode` -- before anything sets
`mode`** -- then projects rows, sets `cursor` to the row of the focused session
if present else 0, disarms `exit_arm`, and sets `open = true`. The order is
normative: `open()` takes `const UiModel& model` and captures `prev_mode` while
`mode` is still the outgoing mode, so a caller can never accidentally store
`Dashboard` as its own predecessor. `close()` clears rows, resets `cursor`,
disarms, and sets `open = false`.
`rebuild()` preserves the cursor across the re-projection by remembering the
`(workspace, session)` under the cursor and re-finding it; if it vanished, the
cursor is clamped to `min(cursor, rows.size()-1)` (81-F6).

**One projection, one comparator (LOW-1).** `open()` and `rebuild()` do **not**
own separate projections. Both call a private `project(model)` that (a) builds
`rows` from the Live tree, (b) computes each row's `status`/`group` via
`dashboard_status`/`dashboard_group`, and (c) sorts `rows` with
`dashboard_row_less` (81-D6). `dashboard_row_less`'s callers are therefore
`DashboardModel::open` **and** `DashboardModel::rebuild`, both through
`project()`; there is no ordering path that bypasses the comparator (81-I7).

`UiModel::openDashboard()` calls `dashboard.open(*this)` (which saves the
outgoing `prev_mode` before `mode` is touched) and **only then** records
`mode = UiMode::Dashboard`; `UiModel::closeDashboard()` restores
`mode = dashboard.prev_mode` and calls `dashboard.close()`. The
existing `UiModel::openSwitcher()` (`ui_model.cpp:1375-1380`) is **deleted**;
its callers migrate (81-D4, sec 4).

### 81-D3 -- Open gesture: ArrowLeft on an empty composer

Add, inside `handle_input`, immediately after the subagent-view guard
(`supervisor.cpp:3354-3359`) and before the caret-edit block (`:3366`):

```cpp
if (event == ftxui::Event::ArrowLeft && state->input.draft.empty()) {
    open_dashboard();
    return true;
}
```

Rules (pinned):

1. **Exactly ArrowLeft, unmodified.** `ArrowLeftCtrl` (`:3481-3485`) is
   untouched: with a non-empty draft Ctrl+Left still moves by words.
2. **Empty-draft predicate** is `state->input.draft.empty()`. An empty draft
   makes the existing `input.cursor_left` (`:3491-3495`) a no-op, so
   intercepting never changes any non-empty-draft behaviour (81-I4, 81-I5).
3. **Precedence (both chains, sec 2.2).** Chain A (`handle_event_inner`): the
   guard is only reached after paste/overlay/global/scroll branches
   (`supervisor.cpp:3810-3902`); a modal or overlay that owns input
   (`exitConfirm`, `dialog`, `Context`, `Switcher`, `ModelPicker`) -- and the
   `Dashboard` guard itself (81-D1) -- never opens the dashboard, and the 60
   scroll branches (`:3871-3902`) never fire the guard because ArrowLeft is not
   a 60 scroll key. Chain B (`handle_input`): the guard is inserted after the
   subagent-view guard (`:3354-3359`) and before the caret-edit block (`:3366`),
   so a viewed child consumes ArrowLeft and never opens the dashboard (81-I4),
   while a non-empty draft falls through to `cursor_left` (`:3491-3495`).
   `ExitConfirm` binds ArrowLeft itself (`:873-878`) and is checked earlier in
   chain A, so it is unaffected.
4. **Single press.** One ArrowLeft on an empty composer opens immediately. The
   Claude Code "press Left again to open agents" hint (double-press arm, after
   deleting the last prompt text or moving through history) is **NOT** mirrored
   (recorded non-mirror, sec 8): ymh's open is a pure view switch with no
   backgrounding side effect, so an accidental open is cheap (Esc returns), and
   a hidden arm would add unobservable state with no user benefit.
5. **No open when viewing a child**; no open while a modal owns input (rule 3).
6. **Zero-workspace case.** With `workspaces.empty()` the `pendingComposer`
   state (`:3336-3342`) is used; ArrowLeft on its empty draft opens the
   dashboard's inline empty state (81-D9). Allowed.
7. **`hints_dismissed`/`command_hints`** are irrelevant: an empty draft has no
   hints (`command_hints` is rebuilt on edit, `:3384-3389`).

### 81-D4 -- Ctrl+S / Ctrl+P become the dashboard alias

Change the global binding (`supervisor.cpp:3847-3854`) to call
`open_dashboard()` (the renamed `SupervisorApp::openSwitcher()`, sec 4). The
dashboard uses the same Live projection and the same catalog refresh:

```cpp
if (event == ftxui::Event::CtrlS || event == ftxui::Event::CtrlP) {
    open_dashboard();
    catalog_visible_.store(false);
    if (catalog_ != nullptr) { catalog_->refreshNow(); }
    return true;
}
```

Consequences (pinned):

- The **empty-target Notice** (`SupervisorApp::openSwitcher()`, `:1259-1271`,
  using `switcher_has_targets()`/`other_live_workspace_exists()`) is **removed
  for the dashboard**; the dashboard opens unconditionally and renders its own
  empty state (81-D9). `46-D3`/`49-D6` are superseded for the Live surface
  (sec 1.3).
- The `Notice` mode is **dead, so it is removed, not retained**. `openSwitcher()`
  (`:1259-1271`) is its only producer: `grep 'UiMode::Notice|message\.open' src/`
  matches only that branch (`:1262-1266`), `handle_notice` (`:1274-1282`), the
  `message.open` clause of `modal_owns_input` (`:2179`), the dispatch guard
  (`:3828-3829`), and the renderer branch (`ui_render.cpp:2206-2207`). With the
  branch gone there is no production producer, so `UiMode::Notice`
  (`ui_event.hpp:52`), `handle_notice`, the `build_ui` branch, `render_notice`
  (`ui_render.cpp:1577-1585`), the then-unused `UiModel::message` field
  (`ui_model.hpp:675`, `MessageDialogModel` `:564-567`) are all removed; the tests
  that construct or assert them migrate or are deleted (sec 10.5). The removed
  `message.open` clause of `modal_owns_input` is **replaced, not merely dropped**:
  `modal_owns_input()` (`supervisor.cpp:2176-2181`) gains
  `|| model_.mode == UiMode::Dashboard` so RB-12 captures and restores the
  composer draft while the dashboard is open (81-I16, 81-F12); without it
  `Errata81.ComposerDraftSurvives` (sec 10.2) fails. This is the honest
  disposition -- contrast 81-I20, where History/Subagents keep the Live popup's
  symbols reachable so it is latent, not dead.
- The **Live popup is retired**: no production path sets
  `mode == UiMode::Switcher` with `source == Live`. The Live branch of
  `render_switcher` and `handle_switcher` becomes latent (mode-only), exactly as
  57-I10 records for its own latent mode-only guard. `render_switcher` and
  `handle_switcher` remain reachable through History/Subagents, so 81-I20 holds
  and no symbol is dead. The Live `SwitcherSourcePolicy` row
  (`kSwitcherPolicy[Live]`, `src/ui/ui_model.cpp:30-31`) is **repurposed** as the
  dashboard's heading/empty-state/footer source (81-D9), so it too stays
  reachable.
- `/sessions` still opens History with `mode == Switcher`; Ctrl+T still opens
  Subagents. Both unchanged (81-D11).
- The `UiController::open_switcher()` harness hook
  (`include/ymh/ui/supervisor_harness.hpp:63`; impl
  `src/ui/supervisor.cpp:4164`) keeps its name but now opens the dashboard; the
  name is retained to avoid touching the single-process TUI and both harnesses
  (recorded; the semantic change is 81-D4).
- **Child view.** Ctrl+S/Ctrl+P stay global and are reached even while a subagent
  child is viewed: the binding (`:3847-3854`) sits after only the Ctrl+C
  suppression (`:3839`), so the dashboard opens over the child view without
  cancelling it. This is the current behavior, retained unchanged. ArrowLeft, by
  contrast, is consumed by the child guard (`:3354-3359`) and never opens the
  dashboard (81-D3 rule 5, 81-I4).

### 81-D5 -- Status vocabulary mapped onto ymh state (exhaustive)

The dashboard's per-row status is **not persisted; it is recomputed on every
`open`/`rebuild`** (the derived value is stored on `DashboardRow` so the
renderer never derives it) and is never advanced in `Render()` (10 sec 3.4/D16).
The one function is total over `(SessionCell, SessionUiState*)` and is the only
place a status is derived (81-I6):

```cpp
// src/ui/ui_model.cpp  (new free function; 81-D5)
DashboardStatus dashboard_status(const SessionCell& cell,
                                 const SessionUiState* state) noexcept {
    if (is_active_state(cell.state) || cell.state == AgentState::Cancelling ||
        (state != nullptr && session_working(*state))) {
        return DashboardStatus::Working;                 // (1)
    }
    if (cell.state == AgentState::Error) {
        return DashboardStatus::Failed;                  // (2) before (3)
    }
    if (cell.attention) {
        return DashboardStatus::NeedsInput;              // (3)
    }
    if (state != nullptr && state->attention.completed) {
        return DashboardStatus::Completed;               // (4)
    }
    return DashboardStatus::Idle;                        // (5)
}
```

Every input is a real field, verified at the cited line:

| Field | Type / where | Established at |
|---|---|---|
| `cell.state` | `SessionCell::state`, the daemon-reported `AgentState` | `include/ymh/ui/ui_model.hpp:354-361` (`state` `:357`) |
| `cell.attention` | `SessionCell::attention` = the session's `attention.needsInput` | `ui_model.hpp:358`; projected at `src/ui/ui_model.cpp:914-916` |
| `state->attention.completed` | `AttentionState::completed` | `ui_model.hpp:295-299`; set on each transition at `ui_model.cpp:1219-1221` (`:1221` is `completed = newState == Idle`) |
| `session_working(*state)` | `is_active_state(agent_state)` OR any **direct** subagent `Running` | `ui_model.cpp:615-625`; `is_active_state` `:597-599` |
| `AgentState::Cancelling` / `::Error` | `AgentState` members | `include/ymh/agent/agent.hpp:31-39` (`Cancelling` `:37`, `Error` `:38`) |

**Every dashboard status, with its exact derivation:**

| Dashboard status | Derivation | Reachable? |
|---|---|---|
| `Working` | cascade (1): `is_active_state(cell.state)` (`cell.state in {Thinking, CallingTool}`) OR `cell.state == Cancelling` OR `state && session_working(*state)` (a direct subagent `Running`) | yes |
| `Failed` | cascade (2): `cell.state == AgentState::Error` (`agent.hpp:38`). Ordered **before** `NeedsInput` because `Error` also sets `attention.needsInput` (`is_waiting_state` includes `Error`, `ui_model.cpp:601-605`) | yes |
| `NeedsInput` | cascade (3): `cell.attention == true`, i.e. the last transition was `WaitingForPermission` or `WaitingForInput` (the only non-Error waiting states) | yes |
| `Completed` | cascade (4): `state->attention.completed == true`, set when the last transition was to `Idle` (`ui_model.cpp:1221`) | yes |
| `Idle` | cascade (5): `cell.state == AgentState::Idle`, `cell.attention == false`, and `completed == false` (a session that has not finished a turn, or whose `SessionUiState*` is null) | yes |
| `Stopped` | **not derivable per session.** No field produces it | **no** |

**Exhaustive `AgentState` -> status table** (so no state is silently lost; the
`Cell state` column is `SessionCell::state`):

| `AgentState` | `cell.attention` | `session_working` | `dashboard_status` |
|---|---|---|---|
| `Idle` | false | false | `Completed` if `attention.completed`, else `Idle` |
| `Idle` + a direct subagent `Running` | false | true | `Working` (cascade 1 wins) |
| `Thinking` | false | true | `Working` |
| `CallingTool` | false | true | `Working` |
| `Cancelling` | false | false | `Working` -- **closest mapping**: the cancel is in flight (a turn is being torn down), so the row stays in the in-progress group. `session_working` alone omits it, so cascade (1) names `Cancelling` explicitly. It is *not* mapped to `Stopped` (that would make `Stopped` transiently produced, contradicting the row below). |
| `WaitingForPermission` | true | false | `NeedsInput` |
| `WaitingForInput` | true | false | `NeedsInput` |
| `Error` | true | false | `Failed` (cascade 2 precedes 3) |

`Stopped` is a produced-less vocabulary member: ymh has no per-session persisted
"stopped/cancelled" flag. `AgentState::Cancelling` is transient and maps to
`Working` (above); the daemon's `Stopping` is a workspace-level `DaemonStatus`.
A workspace whose `daemonStatus == DaemonStatus::Stopping` renders a
workspace-level `[stopping]` badge, but its sessions keep their derived status;
`Stopped` rows are never produced until a durable stop flag exists. This is an
intentional, reasoned non-mirror (sec 8, 56-D6); its seam is an additive
`sessions` column or a `SessionCell` bit (81-OQ2). `dashboard_status` never
returns it, and `dashboard_group(Stopped)` is pinned to `Completed` only so the
function stays total over its enum.

**Glyphs are the dashboard's own, not `state_glyph`.** `dashboard_status_glyph`
(81-D5) is a status table: `Working` `*`, `NeedsInput` `!`, `Idle` `o`,
`Completed` `+`, `Failed` `x`, `Stopped` `-`. It is deliberately **not**
`state_glyph` (`ui_render.cpp:233-250`), which would render `CallingTool` as
`>` and `Cancelling` as `-`; the dashboard renders the derived *status*, so a
`Working` row is always `*` regardless of the underlying `AgentState` (this
closes the LOW-3 fidelity note). `render_dashboard` reads
`DashboardRow::status`/`group` and never re-derives, so it calls
`dashboard_status_glyph` only.

`dashboard_status` is a free function (not a member) so `open` and `rebuild`
share one mapping and cannot drift (81-I6). Callers: `DashboardModel::open` and
`DashboardModel::rebuild` (`src/ui/ui_model.cpp`, via the shared `project()`,
81-D2), which resolve the `SessionCell` from `WorkspaceModel::sessions` and the
`SessionUiState*` from `UiModel::session(id)` (81-D2) and store the result on
the row.

### 81-D6 -- Grouping and ordering

`DashboardGroup = {NeedsInput, Working, Completed}` in render order. Mapping:

| Dashboard group | Member statuses | Claude Code group it mirrors | Omitted? |
|---|---|---|---|
| `NeedsInput` | `NeedsInput` | "Needs input" | no |
| `Working` | `Working` | "Working" | no |
| `Completed` | `Completed`, `Failed`, `Idle` | "Completed" (which "collects finished, failed, and stopped together") | no |
| -- | -- | "Pinned" | **omitted, not derivable** |
| -- | -- | "Ready for review" | **omitted, not derivable** |

`dashboard_group` is total over `DashboardStatus`: `NeedsInput` ->
`NeedsInput`, `Working` -> `Working`, and `Completed`/`Failed`/`Idle`/`Stopped`
-> `Completed`. The `Stopped` arm is unreachable (81-D5) and exists only so the
function has no UB/gap; it is not a produced bucket.

`Pinned` has no store: no pin column in `sessions` (`session_persistence.cpp:36-51`),
no pin field on `SessionCell`/`SessionNode` (`ui_model.hpp:354-361`, `:413-425`),
and no pin command in the registry (`src/ui/command_registry.cpp:147-298`).
`Ready for review` in Claude Code means "has an open pull request"; ymh has no
PR linkage (no Git-host integration; `libgit2` is local-only). Both are recorded
as non-mirror with the reason (sec 8). Their seam is named: a future pin store
would add a `SessionCell::pinned` bit and a `DashboardGroup::Pinned` bucket.

Within a group, rows are ordered by the retained 57-D5 workspace rule, then by
session recency:

1. **Effective-root workspace first** (`UiModel::cwdWorkspacePath`, 57-D6),
2. then `DashboardRow::workspace_last_used` descending (0 last); the field is
   copied from `WorkspaceNode::lastUsedAt` at projection (81-D2), so the
   comparator can evaluate the rule from the two rows alone,
3. then workspace title ascending (case-insensitive),
4. then session `updatedAt` descending (from the catalog join; unknown last),
5. then session title ascending (case-insensitive), then `SessionId` ascending.

The comparator is `dashboard_row_less(...)` (new free function, callers
`DashboardModel::open` **and** `DashboardModel::rebuild`, both through the
shared private `project()`; 81-D2, LOW-1); it reads only the two `DashboardRow`s -- including
`workspace_last_used`, `updatedAt`, `title`, `workspace_title`, `session` -- and
the two workspace paths (`cwd_path`, `left_ws_path`, `right_ws_path`), so every
rule above is evaluable from its arguments. Ordering is applied in the **model
only**; the renderer never sorts (81-I7), matching 57-I13. The focused session is **not**
excluded (unlike the popup, `ui_model.cpp:1466-1471`); it appears in its group
(81-I8).

### 81-D7 -- List content, columns, and counts

Each session row renders, left to right:

```
<glyph> <status label>  <title>            <workspace>   <relative age>
```

- `glyph`/`status label`: from 81-D5 (`dashboard_status_glyph`/
  `dashboard_status_label`).
- `title`: `SessionCell::title`; on empty/placeholder, the short id via the
  popup's fallback precedent `session_row_title` (`ui_render.cpp:320-321`), which
  calls `short_id` (`ui_render.cpp:304`).
- `workspace`: `WorkspaceModel::title` (fallback `cwd`), as in
  `SwitcherOverlayModel::open` (`ui_model.cpp:1449`).
- `relative age`: derived **purely** from the joined catalog
  `SessionHistoryEntry::updatedAt` and `SessionCatalogModel::nowMs`
  (57-I2 precedent, `ui_render.cpp:1241-1242`); `-` when the session is not in
  the snapshot or `nowMs == 0`. No clock read in `Render()`.
- A workspace-level `[stopping]` badge when `daemonStatus == Stopping` (81-D5).

Group headers: `Needs input (N)`, `Working (N)`, `Completed (N)`, skipped when
empty. Top summary line: the counts `N awaiting input - M working - K completed`
(from `DashboardModel::counts()`); `Completed` counts the Completed+Failed+Idle
bucket to match 81-D6. Footer: the key hints (81-D8). Separators are ASCII
`" - "` and `" | "` (no Unicode).

### 81-D8 -- Keys

`handle_dashboard(event)` owns every key while `mode == Dashboard`:

| Key | Action |
|---|---|
| `ArrowUp` / `k` | `moveUp()` |
| `ArrowDown` / `j` | `moveDown()` |
| `PageUp` | `pageUp()` |
| `PageDown` | `pageDown()` |
| `Home` | `moveHome()` |
| `End` | `moveEnd()` |
| `Tab` | `toggle_collapse()` for the current row's group |
| `Enter` / `ArrowRight` | attach: focus the row's session, close, return (81-D8.1) |
| `Escape` | close, restore `prev_mode` and prior focus/scroll (81-D3) |
| `Ctrl+C` | two-press exit arm (81-D8.2) |
| `Ctrl+Q` | consumed no-op (57-I10 parity) |
| any other key | consumed no-op (protects the globals, 81-I19) |

**81-D8.1 Attach is focus-only, and is a no-op on a non-session row.** Enter/Right
compute `(workspace, session)` from the row and call
`UiModel::focusSessionIn(workspace, session)` then `focusWorkspace(workspace)`
(the Live Enter path, `supervisor.cpp:3078-3080`), close the dashboard, and set
`mode = Conversation`. It NEVER calls `session.activate` (retained, 22 sec 5.2,
`supervisor.cpp:1669-1672`). If the row is already focused, it just closes.
Group headers are not rows and are never the cursor target (the cursor indexes
`rows`, 81-D2), so Enter can never fire on a header.

**81-D8.2 Double Ctrl+C to quit is guarded by the exit confirm.** The first
Ctrl+C sets `exit_arm = Armed`, records `exit_armed_at = now`, renders the
footer hint `press Ctrl+C again to quit`, and **does not** call `cancelActive`
(the existing Ctrl+C global, `supervisor.cpp:3859-3862`, is unreachable while
the dashboard owns keys). A second Ctrl+C within `kEscArmTimeout`
(`supervisor.cpp:57`, 3000 ms) **disarms as it fires** -- the triggering press
resets `exit_arm = Disarmed` and clears `exit_armed_at` before acting, mirroring
48-D2, so a later Ctrl+C after a cancelled confirm needs a fresh arm -- and then
calls `begin_exit(/*allow_prompt=*/true)`
(`:640-651`), which routes through the existing `ExitConfirmState` (spec 16
sec 4, `ui_model.hpp:606-612`, `handle_exit_confirm` `:868-896`). Any other key,
a cursor move, close, reopen, or the timeout disarms (81-I13). This reuses the
same `EscArm` + `kEscArmTimeout` two-press mechanism as 48-D2 and 51-D4.3
(`ui_model.hpp:251`, `:498-501`). While the exit confirm is open it is the first
guard in `handle_event_inner` (`:3818`), so its own Esc/Ctrl+C cancel path
(`:869-871`) is unaffected.

### 81-D9 -- Empty state, long-list scrolling, narrow terminal

- **Empty state.** `render_dashboard` with `rows.empty()` renders the heading,
  a dim `switcher_policy(SwitcherSource::Live).empty_state`
  (`"No live sessions."`), and a hint line. The dashboard does **not** open a
  `Notice` (supersedes 46-D3/49-D6, sec 1.3).
- **Long lists.** The renderer computes the visible window purely from `cursor`,
  `size.height`, and the group headers; it never mutates the model. If the
  cursor is above the window, the window starts at the cursor; if below, the
  window ends at the cursor. The list is clipped with a trailing
  `... N more` marker when rows fall outside (81-I9). `pageUp`/`pageDown` step by
  the last rendered viewport (stored transiently by the handler, not persisted).
- **Narrow terminal.** Below `kDashboardNarrowWidth` (60) the renderer drops the
  workspace and age columns, strips the group counts to `(N)`, truncates the
  title, and shortens the footer to `Enter attach | Esc close`; the summary line
  degrades to the two non-zero counts (81-I10). Nothing is clipped mid-glyph
  (the existing width helpers, e.g. `truncate_spans`, are reused).

### 81-D10 -- Live-only universe; `/sessions` stays the stored catalog

The dashboard lists only `live_switcher_renderable` workspaces and their
`SessionCell`s, i.e. attached daemons (81 sec 1.5). It does **not** read disk,
does **not** scan orphan DBs, and does **not** fake a workspace (retains 22
SW7/SW12 and the "never inject a phantom workspace" rule,
`supervisor.cpp:1453-1462`). `/sessions` remains the disk catalog
(`open_sessions`, `:1066-1075`). Selecting a stored session in a non-running
workspace still spawns/attaches then resumes (22 S3), unchanged -- that path is
reached only from `/sessions`, never from the dashboard.

### 81-D11 -- History and Subagents popups are unchanged

`render_switcher` (`ui_render.cpp:1496-1532`), `handle_switcher`
(`supervisor.cpp:3017-3091`), `open_sessions`, `open_subagents`, the History
footer (`57-D1`), the inline delete confirmation (`57-D3`), and the ordering
rule (`57-D5`) are unchanged. The dashboard is a **new** surface; it does not
alter the popup's pixels.

### 81-D12 -- Snapshot semantics: rebuild on open and on every delivery

The dashboard is a snapshot, like the switcher (22 sec 3.3). While
`mode == Dashboard`:

- `on_catalog_snapshot` (`supervisor.cpp:1045-1062`) gains a branch
  `else if (model_.dashboard.open) dashboard.rebuild(model_);` beside the
  existing Live/History rebuilds. The predicate is `dashboard.open` (not
  `mode == Dashboard`) so the snapshot also refreshes while an overlay is up
  over the dashboard (81-D13).
- `resnapshot_switcher()` (`:1443-1448`), called on live-set changes (`:1772`,
  `:1887`, `:3297`), gains: `if (model_.dashboard.open) dashboard.rebuild(model_);`.
  The `open()` projection is not re-run with cursor reset; `rebuild` preserves
  the cursor (81-D2). This is bounded and never loops (81-F11).

### 81-D13 -- Overlay mode save/restore (prior-mode lifetime)

Because chain R (sec 2.2) selects the base from `dashboard.open`, a mode raised
while the dashboard is open must (a) leave `dashboard.open` true and (b) restore
the interrupted mode when it closes. Otherwise `mode` and `dashboard.open`
diverge (81-I3) and the dashboard is dropped when its own overlay is cancelled
(the MEDIUM-1 defect: the shipped `cancel_exit` hard-codes
`mode = UiMode::Conversation`, `supervisor.cpp:787-791`, and
`PermissionResolved` hard-codes the same, `ui_model.cpp:1212`). Three overlays
can be raised while `mode == Dashboard`:

1. **The exit confirm** -- raised by the dashboard's own double Ctrl+C
   (81-D8.2 -> `begin_exit(true)` -> `open_exit_prompt`). `ExitConfirmState`
   (`ui_model.hpp:606-612`) gains `UiMode prev_mode = UiMode::Conversation;`.
   `open_exit_prompt` (`supervisor.cpp:749-757`) records
   `model_.exitConfirm.prev_mode = model_.mode;` **before** it sets
   `model_.mode = UiMode::ExitConfirm;` (`:755`). `cancel_exit` (`:787-791`)
   must capture `const UiMode prev = model_.exitConfirm.prev_mode;` **before**
   the `model_.exitConfirm = ExitConfirmState{}` reset at `:788` (which would
   otherwise clobber `prev_mode` to its `Conversation` default), then set
   `model_.mode = prev;` at `:789` in place of the hard-coded
   `UiMode::Conversation` -- so Esc/Ctrl+C/`n`/Cancel return to the
   dashboard, not the conversation. `confirm_exit` (`:795`) needs no restore: it
   deregisters and quits. `handle_exit_confirm`'s own bindings (`:868-896`) are
   unchanged, and its ArrowLeft/ArrowRight arm toggle stays.
2. **The permission dialog** -- raised asynchronously by a daemon's
   `PermissionRequested` while the dashboard is open. `PermissionDialogModel`
   (`ui_model.hpp:588-597`) gains `UiMode prev_mode = UiMode::Conversation;`.
   The reducer saves `dialog.prev_mode = mode;` before
   `mode = UiMode::Dialog;` (`ui_model.cpp:1198-1208`) and restores
   `mode = dialog.prev_mode;` when the matching `PermissionResolved` clears it
   (`ui_model.cpp:1209-1212`). The harness path `open_permission_dialog`
   (`supervisor.cpp:4299-4309`) does the same. On the conversation path this is
   byte-for-byte the current behaviour: `prev_mode` is `Conversation`.
3. **`/context`** -- **needs no `prev_mode`**: it is opened only from the
   composer command (`command_registry.cpp:227-228`) or the overlay's own `r`
   refresh (`supervisor.cpp:3655-3657`), both reachable only while the composer
   owns input, so `dashboard.open` is false whenever Context is open.
   `close_context` (`:3642-3646`) keeps its `mode = Conversation`.

No other overlay can be raised while the dashboard is open: the Switcher and
ModelPicker guards precede the Ctrl+S binding, and the dashboard's own Esc/"any
other key" handling (81-D8) consumes everything else, so those modes cannot be
entered from the dashboard. `begin_exit(false)` (the `no_prompt` path) calls
`confirm_exit` directly and never raises the overlay.

With (1) and (2), `dashboard.open` stays true under an overlay, chain R keeps
the dashboard as the base, and cancelling the overlay restores
`mode == Dashboard`, so the dashboard reappears with `rows`/`cursor`/`collapsed`
intact. The dashboard's `exit_arm` was disarmed when the second Ctrl+C fired
(81-D8.2), so returning from a cancelled confirm shows the normal footer.

---

## 4. New symbols and their callers

| New symbol | Signature (pinned) | Production caller(s) |
|---|---|---|
| `UiMode::Dashboard` | enum member (`include/ymh/ui/ui_event.hpp`) | `UiModel::openDashboard`; `handle_event_inner` guard; `build_ui` |
| `DashboardStatus` | `enum class DashboardStatus : std::uint8_t { Working, NeedsInput, Idle, Completed, Failed, Stopped };` | `dashboard_status`; `DashboardRow` |
| `DashboardGroup` | `enum class DashboardGroup : std::uint8_t { NeedsInput, Working, Completed };` | `dashboard_group`; `DashboardModel::collapsed` |
| `DashboardRow` | struct (81-D2 fields) | `DashboardModel::rows` |
| `DashboardCounts` | struct (81-D2) | `DashboardModel::counts`; `render_dashboard` |
| `DashboardModel` | struct/methods (81-D2) | `UiModel::dashboard`; `handle_dashboard` |
| `dashboard_status` | `[[nodiscard]] DashboardStatus dashboard_status(const SessionCell&, const SessionUiState*) noexcept;` | `DashboardModel::open`/`rebuild` (resolve the cell and `session(id)`, 81-D2) |
| `dashboard_group` | `[[nodiscard]] DashboardGroup dashboard_group(DashboardStatus) noexcept;` | `DashboardModel::open`/`rebuild` (set `group` after `dashboard_status`, 81-D2) |
| `dashboard_status_glyph` | `[[nodiscard]] const char* dashboard_status_glyph(DashboardStatus) noexcept;` | `render_dashboard` |
| `dashboard_status_label` | `[[nodiscard]] const char* dashboard_status_label(DashboardStatus) noexcept;` | `render_dashboard` |
| `dashboard_row_less` | `[[nodiscard]] bool dashboard_row_less(const DashboardRow&, const DashboardRow&, const std::string& cwd_path, const std::string& left_ws_path, const std::string& right_ws_path);` | `DashboardModel::open`/`rebuild` (both via the shared `project()`, 81-D2/81-D6) |
| `kDashboardNarrowWidth` | `constexpr int kDashboardNarrowWidth = 60;` (`src/ui/ui_render.cpp`) | `render_dashboard` (81-D9, 81-I10) |
| `SwitcherOverlayModel::openLive` | `void openLive(const UiModel&, bool include_focused);` | `SwitcherOverlayModel::open` (`openLive(model,false)`); `UiModel::openDashboard` (`openLive(model,true)`) |
| `UiModel::openDashboard` | `void openDashboard();` | `SupervisorApp::open_dashboard`; tests |
| `UiModel::closeDashboard` | `void closeDashboard();` | `SupervisorApp::handle_dashboard`; tests |
| `SupervisorApp::open_dashboard` | `void open_dashboard();` (renamed from `openSwitcher`) | Ctrl-S handler; `open_switcher` override |
| `SupervisorApp::handle_dashboard` | `bool handle_dashboard(const ftxui::Event&);` | `handle_event_inner` |
| `render_dashboard` | `ftxui::Element render_dashboard(const UiModel&, TerminalSize, const Theme&);` | `build_ui` |
| `ExitConfirmState::prev_mode` | `UiMode prev_mode = UiMode::Conversation;` (new field, `ui_model.hpp:606-612`) | written by `open_exit_prompt` (`supervisor.cpp:749-757`); read by `cancel_exit` (`:787-791`) -- 81-D13 |
| `PermissionDialogModel::prev_mode` | `UiMode prev_mode = UiMode::Conversation;` (new field, `ui_model.hpp:588-597`) | written by the `PermissionRequested` reducer (`ui_model.cpp:1198-1208`) and `open_permission_dialog` (`supervisor.cpp:4299-4309`); read by the `PermissionResolved` reducer (`ui_model.cpp:1209-1212`) -- 81-D13 |

Removed: `UiModel::openSwitcher()` (callers become `openDashboard()`); the
empty-target Notice branch of `SupervisorApp::openSwitcher()`; and -- because
that branch was `UiMode::Notice`'s only producer -- `UiMode::Notice`
(`ui_event.hpp:52`), `handle_notice`, the `build_ui` Notice branch,
`render_notice`, and `UiModel::message`/`MessageDialogModel` (81-D4; tests
migrate, sec 10.5). `render_notice` here is only the `src/ui/ui_render.cpp` free
function; the same-named `src/agent/subagent_service.cpp:129,560,916` overload is
unrelated and retained. The branch deletion also orphans its two helpers,
`SupervisorApp::switcher_has_targets()` (`supervisor.cpp:1230`) and
`SupervisorApp::other_live_workspace_exists()` (`:1218`) -- their only callers are
`:1261`/`:1262` inside the deleted branch -- so both are removed (sec 10.5).
Replaced: the `message.open` clause of `modal_owns_input`
(`supervisor.cpp:2179`) becomes `|| model_.mode == UiMode::Dashboard` so the
dashboard captures/releases the RB-12 composer draft (81-I16, 81-F12). Added
(81-D13): `ExitConfirmState::prev_mode` and `PermissionDialogModel::prev_mode`
so an overlay raised from the dashboard restores `mode == Dashboard` on close;
the chain-R base selector changes from `mode == Dashboard` to
`dashboard.open` (81-D1).

---

## 5. C++ interface sketches (signatures only)

```cpp
// include/ymh/ui/ui_event.hpp
enum class UiMode : std::uint8_t {
    Conversation,
    Switcher,
    Dialog,
    ExitConfirm,
    Context,
    ModelPicker,
    Dashboard,   // 81-D1: full-screen session list; the Ctrl-S surface
    // Notice is REMOVED (81-D4): openSwitcher() was its only producer.
};

// include/ymh/ui/ui_model.hpp
enum class DashboardStatus : std::uint8_t {
    Working, NeedsInput, Idle, Completed, Failed, Stopped,  // 81-D5
};
enum class DashboardGroup : std::uint8_t {  // 81-D6 render order
    NeedsInput, Working, Completed,
};
struct DashboardRow {
    WorkspaceId workspace;
    SessionId   session;
    std::string title;
    std::string workspace_title;
    std::int64_t workspace_last_used = 0;   // WorkspaceNode::lastUsedAt; rule 2 (81-D6)
    DashboardStatus status = DashboardStatus::Idle;
    DashboardGroup  group  = DashboardGroup::Completed;
    std::optional<std::int64_t> updatedAt;  // catalog join; nullopt == unknown
    bool workspace_stopping = false;        // 81-D5 [stopping] badge
};
struct DashboardCounts {
    std::size_t needs_input = 0, working = 0, completed = 0, idle = 0, failed = 0;
};
struct DashboardModel {
    bool                       open = false;
    std::vector<DashboardRow>  rows;
    std::size_t                cursor = 0;
    UiMode                     prev_mode = UiMode::Conversation;
    EscArm                     exit_arm = EscArm::Disarmed;
    std::optional<std::chrono::steady_clock::time_point> exit_armed_at;
    std::set<DashboardGroup>   collapsed;

    void open(const UiModel& model);
    void rebuild(const UiModel& model);
    void project(const UiModel& model);   // shared by open/rebuild (81-D2, LOW-1)
    void close();
    void moveDown();  void moveUp();
    void pageDown();  void pageUp();
    void moveHome();  void moveEnd();
    void toggle_collapse();
    [[nodiscard]] DashboardCounts counts() const;
};

// UiModel additions (81-D2)
class UiModel {
    // ... existing ...
    DashboardModel dashboard;
    void openDashboard();
    void closeDashboard();
    // UiModel::openSwitcher() is REMOVED.
};

// SwitcherOverlayModel addition (81-D2)
class SwitcherOverlayModel {
    // ... existing ...
    void openLive(const UiModel& model, bool include_focused);
};

// Existing overlay structs gain one field each (81-D13, the prior-mode
// save/restore that keeps `dashboard.open` consistent under an overlay):
struct PermissionDialogModel {
    // ... existing fields ...
    UiMode prev_mode = UiMode::Conversation;   // written before mode = Dialog
};
struct ExitConfirmState {
    // ... existing fields ...
    UiMode prev_mode = UiMode::Conversation;   // written before mode = ExitConfirm
};

// src/ui/ui_model.cpp free functions (81-D5/81-D6)
[[nodiscard]] DashboardStatus dashboard_status(const SessionCell& cell,
                                               const SessionUiState* state) noexcept;
[[nodiscard]] DashboardGroup  dashboard_group(DashboardStatus status) noexcept;
[[nodiscard]] const char*     dashboard_status_glyph(DashboardStatus status) noexcept;
[[nodiscard]] const char*     dashboard_status_label(DashboardStatus status) noexcept;
[[nodiscard]] bool dashboard_row_less(const DashboardRow& left,
                                      const DashboardRow& right,
                                      const std::string& cwd_path,
                                      const std::string& left_ws_path,
                                      const std::string& right_ws_path);

// src/ui/ui_render.cpp (81-D1)
constexpr int kDashboardNarrowWidth = 60;   // 81-D9/81-I10: narrow threshold
[[nodiscard]] ftxui::Element render_dashboard(const UiModel& model,
                                              TerminalSize size,
                                              const Theme& theme);

// src/ui/supervisor.cpp -- SupervisorApp member definitions (81-D1/81-D4),
// matching sec 4's `SupervisorApp::handle_dashboard`/`SupervisorApp::open_dashboard`.
bool SupervisorApp::handle_dashboard(const ftxui::Event& event);
void SupervisorApp::open_dashboard();
```

`switcher_policy(SwitcherSource::Live)` (`src/ui/ui_model.cpp:30-31`) is
**repurposed** for the dashboard: `heading = "Sessions"`,
`empty_state = "No live sessions."`,
`footer = "Up/Down move | Enter attach | Esc close | Ctrl+C twice quit"`.
`render_dashboard` reads those three fields only; the popup-only fields
(`enter`, `ctrl_d_enabled`, `tab_expands`, `ctrl_t_closes`, `r_refreshes`) are
read by `handle_switcher` for History/Subagents and are documented as
unused-by-the-dashboard (the Live popup is latent, 81-D4).

---

## 6. Invariants

| ID | Invariant |
|---|---|
| **81-I1** | When `dashboard.open` is true, `build_ui` returns a `render_dashboard` base and never the conversation/composer root; the dashboard does not use `clear_under`, `center`, or `window`. `dashboard.open` (not `mode == Dashboard`) is the base selector, so an overlay raised from the dashboard composes over the dashboard. |
| **81-I2** | Overlay precedence is unchanged: `exitConfirm`, `dialog`, `Context` compose `dbox` over the dashboard base in the existing order (`ui_render.cpp:2197-2214`); the `Notice` branch is removed (81-D4). |
| **81-I3** | `dashboard.open` is set true by `openDashboard()` and false by `closeDashboard()`/attach; while no overlay is raised, `mode == UiMode::Dashboard` iff `dashboard.open`. While an overlay raised from the dashboard is up (exit confirm, permission dialog), `dashboard.open` stays true and `mode` is the overlay; on overlay close, `mode` is restored from the overlay's recorded `prev_mode` (81-D13), so `mode == Dashboard` again. `dashboard.open` and `mode` therefore never diverge on a reachable state. |
| **81-I4** | ArrowLeft opens the dashboard iff `mode == Conversation`, no modal/overlay owns input, the subagent view is not active, and `input.draft.empty()`. |
| **81-I5** | With a non-empty draft, ArrowLeft still executes `cursor_left`; with an empty draft it opens the dashboard and never mutates the draft. |
| **81-I6** | The status of a row is computed only by `dashboard_status`; the renderer and the model use the same function, so they cannot disagree. |
| **81-I7** | Ordering is applied in the model (`open`/`rebuild`); `render_dashboard` never sorts and reads no clock. |
| **81-I8** | The focused session appears in the dashboard (it is not excluded as in the popup). |
| **81-I9** | The dashboard window is a pure function of `cursor`, `size`, and `rows`; it is recomputed each render and no scroll offset is persisted. |
| **81-I10** | Below `kDashboardNarrowWidth` (60) the workspace and age columns are dropped; no row is clipped mid-glyph. |
| **81-I11** | Enter/Right attach is focus-only: `focusSessionIn`/`focusWorkspace` run, `session.activate` never runs, and the dashboard closes on success. |
| **81-I12** | A row whose `(workspace, session)` equals the current focus attaches by closing only; no RPC is sent. |
| **81-I13** | The dashboard exit arm follows 48-D2/51-D4.3: first Ctrl+C arms and does not exit or cancel; the second within `kEscArmTimeout` disarms the arm as it fires and calls `begin_exit(true)`; any other key/cursor move/close/timeout disarms. |
| **81-I14** | A single Ctrl+C while the dashboard is open never calls `cancelActive` and never exits. |
| **81-I15** | Esc closes the dashboard and restores `prev_mode`; `activeWorkspaceId`, `activeSessionId`, and every session's `ConversationScroll` are unchanged by an open-then-Esc with no attach. |
| **81-I16** | While `mode == Dashboard`, `modal_owns_input()` is true, so the RB-12 composer snapshot is captured on open and released on close; a draft present at open survives (Ctrl-S from a non-empty composer). |
| **81-I17** | The dashboard lists only `live_switcher_renderable` workspaces and their `SessionCell`s; it never reads `sessions.db` and never scans orphan DBs. |
| **81-I18** | The dashboard adds no RPC and no persisted state; every field in `DashboardModel` is UI-only. |
| **81-I19** | While `mode == Dashboard`, the child-view Ctrl+C suppression branch (`:3839`), Ctrl+T, Ctrl+Q, Ctrl+S/P, Ctrl+N, Ctrl+O, and the spec-60 scroll keys (`PageUp`/`PageDown`, `Ctrl+Home`/`Ctrl+End`, `Shift+Arrow`, `Shift+PageUp`/`Shift+PageDown`) are unreachable (the dashboard guard precedes the globals and the `:3871-3902` scroll branches, sec 2.2); only Esc and double-Ctrl+C leave. |
| **81-I20** | The Live popup is latent, not dead: no production path sets `mode == Switcher` with `source == Live`, yet `render_switcher`/`handle_switcher`/`switcher_policy(Live)`/`SwitcherOverlayModel::open(model)` retain compile-time callers (History/Subagents/dashboard; `open(model)` via `resnapshot_switcher`, `supervisor.cpp:1443-1448`). `open(model)` and that one call site run only under the unreachable `Switcher && Live` state, so they are dynamically dead but retained, not removed; `open(model)` delegates to `openLive(model, /*include_focused=*/false)` (81-D2). |
| **81-I21** | An overlay raised while `mode == Dashboard` records the mode it interrupted (`ExitConfirmState::prev_mode`, `PermissionDialogModel::prev_mode`; 81-D13) and restores it when that overlay closes, so `dashboard.open` stays true and `mode` returns to `Dashboard`; cancelling the dashboard's own exit confirm (Esc/Ctrl+C/`n`/Cancel) never exits the dashboard, and `dashboard.rows`/`cursor`/`collapsed`/`exit_arm` are untouched by the overlay. |

---

## 7. Failure modes (`81-F#`)

| ID | F# class | Failure | Guard / disposition |
|---|---|---|---|
| **81-F1** | F6 | ArrowLeft opens the dashboard while a permission dialog is up. | `handle_event_inner` checks `dialog.open` (`:3821`) before `handle_input`; 81-I4. |
| **81-F2** | F6 | ArrowLeft opens the dashboard while viewing a subagent child, cancelling the parent. | The subagent guard (`:3354-3359`) consumes ArrowLeft; no open. |
| **81-F3** | F9 | Double Ctrl+C exits while daemons would be orphaned. | Routed through `begin_exit(true)` -> `ExitConfirmState` (spec 16 sec 4); 81-I13. |
| **81-F4** | F6 | A stale `mode == Dashboard` renders with `dashboard.open == false`. | 81-I3; `open`/`close` pair mode and flag. |
| **81-F5** | F4 | A catalog delivery re-sorts rows and moves the cursor under the user. | `rebuild()` restores the cursor by `(workspace, session)`; only a vanished row clamps. |
| **81-F6** | F8 | The focused session is deleted while the dashboard is open; the cursor points at nothing. | `rebuild()` clamps `cursor` to `min(cursor, rows.size()-1)`; empty list -> cursor 0. |
| **81-F7** | F9 | Enter attaches a session in a workspace whose daemon died mid-open. | The dashboard is live-only; `evict_dead_workspaces` (`:1776`) drops the node on the next snapshot, and `focusSessionIn` is a no-op for an absent workspace (`ui_model.cpp:1407-1410`). |
| **81-F8** | F7 | The dashboard render reads a clock and the golden test flakes. | Age uses `catalog.nowMs` only; `Render()` reads no clock (81-I7). |
| **81-F9** | F6 | Ctrl+Q inside the dashboard exits the app. | The dashboard guard consumes Ctrl+Q (57-I10 parity); 81-I19. |
| **81-F10** | F4 | A live-set change while the dashboard is open rebuilds and loses the arm state. | `rebuild()` does not touch `exit_arm`/`exit_armed_at`; a rebuild is not a cursor move, so the arm survives (81-D12). |
| **81-F11** | F12 | Snapshot rebuild loops. | `rebuild` never triggers a reader pass; bounded, like 57-I19. |
| **81-F12** | F6 | A draft typed before Ctrl-S is lost after Esc. | `modal_owns_input()` includes Dashboard, so RB-12 capture/release preserves it (81-I16). |
| **81-F13** | F4 | An overlay raised from the dashboard (exit confirm / permission dialog) closes to `Conversation`, dropping the dashboard while `dashboard.open == true`. | 81-D13: the overlay records `prev_mode` at open and restores it on close; `cancel_exit`/`PermissionResolved` are amended; 81-I21. Covered by `Errata81.ExitConfirmCancelReturnsToDashboard` and `Errata81.PermissionDialogOverDashboardRestores`. |

---

## 8. dsh (DeepSeek Harness) mapping

dsh is a headless harness whose UI is a plugin (`00-architecture.md:4864`), so
there is no dsh dashboard/agent-view contract to mirror. Every non-mirror cell
carries its reason and a concrete anchor (56-D6).

| dsh concept | ymh realization (81) | Justification |
|---|---|---|
| Full-screen "agent view" over live sessions | `UiMode::Dashboard` + `render_dashboard` (81-D1) | **Non-mirror, deliberate scope.** dsh exposes no interactive session dashboard; ymh's is a UI-only projection of `WorkspaceModel::sessions` (`22-D3`, `docs/design/22-switcher-sessions-errata.md:896`). The full-screen form follows Claude Code, not dsh. |
| Keybinding focus (F6) | The dashboard is a focus owner that consumes keys before the globals (81-D1/81-D8) | Mirrors the F6 principle (`docs/design/00-architecture.md:4850`); no divergence. |
| Status derivation from agent state | `dashboard_status` over `AgentState` + `AttentionState` (81-D5) | Mirrors the capability: dsh's agent exposes a state machine; ymh projects the same states. Anchors: `include/ymh/agent/agent.hpp:31-39`, `src/ui/ui_model.cpp:597-625`, `:1219-1221`. |
| "Pinned" sessions | Omitted (81-D6) | **Non-mirror.** No pin store exists in dsh or ymh; no `sessions` column (`src/session/session_persistence.cpp:36-51`) and no `SessionCell::pinned` (`include/ymh/ui/ui_model.hpp:354-361`). Seam named: a future pin store. |
| "Ready for review" (open PR) | Omitted (81-D6) | **Non-mirror.** dsh has no PR linkage; ymh links no Git host (local `libgit2` only, `AGENTS.md`). The Claude Code input is a PR number with no ymh analogue. |
| "Stopped" row state | Vocabulary member, never produced (81-D5) | **Non-mirror, architectural absence.** ymh has no durable per-session stop flag; `Cancelling` is transient (`agent.hpp:37`) and daemon `Stopping` is workspace-level. Reason recorded rather than fabricating a mapping. |
| Two-press destructive-action arm | `EscArm` + `kEscArmTimeout` reused for exit (81-D8.2) | Mirrors the existing ymh mechanism (48-D2, 51-D4.3); not a dsh contract. Anchors: `ui_model.hpp:251`, `src/ui/supervisor.cpp:57`. |

---

## 9. State-lifetime table

Every field is UI-only and process-local; nothing is persisted and nothing new
survives a restart. The table names each new state's creation, destruction,
owner, and crash/restart/resize/session-switch behaviour. "Survives a resize"
means the value is unchanged across a `SIGWINCH`/next-frame at a new size.

| State | Type / where | Created | Destroyed / cleared | Owner | Survives restart? | Survives resize? | Survives session switch? |
|---|---|---|---|---|---|---|---|
| `UiMode::Dashboard` | value held in `UiModel::mode` | `openDashboard()` | `closeDashboard()` / attach / overlay `prev_mode` restore | `UiModel` | no | yes (unchanged) | no (attach closes it) |
| `dashboard.open` | bool, `DashboardModel` | `open()` | `close()` | `DashboardModel` | no | yes | no |
| `dashboard.rows` | `std::vector<DashboardRow>` | `open()` / `rebuild()` | `close()` (`clear`); replaced each `rebuild` | `DashboardModel` | no | yes (not re-projected on resize) | re-projected by `rebuild` (content), pointer-safe |
| `dashboard.cursor` | `std::size_t` (index into `rows`) | `open()` (focused row or 0) | `close()` (0); updated by move keys | `DashboardModel` | no | **yes -- an index, size-independent**; never recomputed from geometry | **preserved across a `rebuild`** by re-finding `(workspace, session)` (`open`/`rebuild`, 81-D2); reset to 0 only by `close()` (attach) |
| `dashboard.prev_mode` | `UiMode` | `open()` (saves `model.mode` **before** `openDashboard` sets `mode`; 81-D2) | `close()` (read by `closeDashboard`) | `DashboardModel` | no | yes | no (cleared on attach via `close`) |
| `dashboard.exit_arm` | `EscArm` | `open()` (Disarmed) | `close()`, timeout, any other key | `DashboardModel` | no | yes | no |
| `dashboard.exit_armed_at` | `optional<steady_clock::time_point>` | first Ctrl+C | `close()`, timeout, disarm | `DashboardModel` | no | yes (monotonic, size-independent) | no |
| `dashboard.collapsed` | `std::set<DashboardGroup>` | first Tab | `close()` (clear) | `DashboardModel` | no | yes | no |
| page step | local `int` in `handle_dashboard` | `PageUp/Down` | end of the handler call | call frame | no | no (recomputed from the new viewport next press) | no |
| RB-12 composer snapshot | `ModalComposerSnapshot` (existing) | `modal_owns_input()` on open | `release` on close (`sync_modal_composer`, `supervisor.cpp:2220-2226`) | `SupervisorApp` | no | yes | released/recaptured like any modal |
| catalog join (`updatedAt`) | read from `model.catalog` | existing delivery (`on_catalog_snapshot`, `:1045-1062`) | next delivery | `SessionCatalogModel` | no (re-read on resume) | yes | re-read on the next delivery |
| `ExitConfirmState::prev_mode` (81-D13) | `UiMode` field | `open_exit_prompt` (`:749-757`), before `mode = ExitConfirm` | `cancel_exit` reads it then resets `exitConfirm = {}` (`:787-791`); `confirm_exit` resets on quit | `UiModel::exitConfirm` | no | yes | n/a -- the exit confirm owns input, so no attach can occur while it is up |
| `PermissionDialogModel::prev_mode` (81-D13) | `UiMode` field | `PermissionRequested` reducer (`ui_model.cpp:1198-1208`) / `open_permission_dialog` (`:4299-4309`), before `mode = Dialog` | `PermissionResolved` reducer reads it and clears `dialog.open` (`ui_model.cpp:1209-1212`) | `UiModel::dialog` | no | yes | n/a -- the dialog owns input, so no attach can occur while it is up |

**Resize.** A terminal resize destroys no dashboard state and re-projects
nothing: `render_dashboard`'s visible window is a pure function of `cursor`,
`size`, and `rows` recomputed each frame (81-I9), so `dashboard.open`, `rows`,
`cursor`, `prev_mode`, `exit_arm`, `exit_armed_at`, `collapsed`, and both
overlay `prev_mode` fields are unchanged. The only geometry-dependent value is
the transient `pageUp`/`pageDown` step (a local recomputed on the next press).
No field needs a resize-specific disposition.

**Session switch.** Two distinct events are named explicitly:

- **A `rebuild`** (catalog delivery `on_catalog_snapshot:1045-1062` or live-set
  change `resnapshot_switcher:1443-1448`, 81-D12) is **not** a session switch:
  `project()` re-derives `rows`, and `rebuild` restores `cursor` by re-finding
  the `(workspace, session)` under it (clamped only if the row vanished, 81-F6).
  `collapsed`, `exit_arm`/`exit_armed_at` survive a `rebuild` (81-F10).
- **An attach / `closeDashboard()`** is a session switch in the UI sense: it
  calls `close()`, which clears `rows`, resets `cursor` to 0, disarms, and sets
  `open = false`; `mode` is restored to `dashboard.prev_mode`. No cross-session
  dashboard state survives because there is only ever one `DashboardModel` on
  `UiModel` (the active workspace's supervisor), not one per session.

Deleted state: `UiModel::openSwitcher()` is removed (its callers become
`openDashboard()`), and the empty-target `Notice` surface it produced --
`UiMode::Notice`, `handle_notice`, `render_notice`, the `message.open` clause of
`modal_owns_input`, `UiModel::message`/`MessageDialogModel` -- is removed as dead
(81-D4, sec 4). The existing `SwitcherOverlayModel` state is unchanged (the
dashboard reuses it read-only for the Live projection).

---

## 10. Test plan

### 10.1 Unit -- model (`tests/unit/errata81_ui_test.cpp`, new; `tests/unit/ui_model_test.cpp`)

Pure-model tests (no FTXUI, no daemon, no LLM):

- `Errata81.StatusMappingTable` -- the exhaustive `AgentState` table of 81-D5:
  `Error` -> `Failed` (not `NeedsInput`), `WaitingForPermission`/
  `WaitingForInput` -> `NeedsInput`, `Thinking`/`CallingTool`/`Cancelling` ->
  `Working`, a direct subagent `Running` -> `Working`, `Idle` with
  `attention.completed` -> `Completed`, `Idle` without -> `Idle`.
- `Errata81.GroupBuckets` -- `Failed`/`Idle` fold into `Completed`; `Stopped` is
  never produced; `dashboard_group` is total (covers `Stopped`).
- `Errata81.OverlayPrevModeRestores` -- model-level: `open_exit_prompt` saves
  `exitConfirm.prev_mode` before `mode = ExitConfirm`; `PermissionRequested`
  saves `dialog.prev_mode` before `mode = Dialog`; both restore the saved mode
  (81-D13).
- `Errata81.OrderWithinGroup` -- effective-root first, then `lastUsedAt` desc,
  then title/path/session tie-breaks.
- `Errata81.OpenSavesPrevModeAndCursor` -- `openDashboard()` sets
  `mode == Dashboard`, `prev_mode == Conversation`, cursor on the focused row.
- `Errata81.CloseRestoresModeAndClears` -- `closeDashboard()` restores mode and
  clears rows/cursor/arm.
- `Errata81.RebuildPreservesCursor` -- a delivery that keeps the row keeps the
  cursor; a delivery that drops it clamps.
- `Errata81.CollapseToggle` -- Tab collapses/expands a group.
- `Errata81.LiveOnlyUniverse` -- a non-`live_switcher_renderable` workspace is
  excluded; no disk access.
- `Errata81.FocusedSessionIncluded` -- unlike the popup, the focused session is a
  row.
- `Errata81.Counts` -- `counts()` matches the summary string.
- Existing `ui_model_test.cpp` migrations: every `openSwitcher()` call site is
  dispositioned in the complete table of sec 10.5 (no "e.g."), with each
  `mode == Switcher`/`source == Live` assertion retargeted to
  `openDashboard()`/`Dashboard` or preserved via the latent-Live idiom.

### 10.2 Hermetic interaction -- key sequences (`tests/unit/errata81_ui_test.cpp`)

Drive the supervisor harness (`tests/unit/errata57_ui_test.cpp:150-260` is the
template) with synthetic `ftxui::Event`s and assert model state after each:

- `Errata81.ArrowLeftEmptyOpensDashboard` -- `{ArrowLeft}` with empty draft ->
  `mode == Dashboard`.
- `Errata81.ArrowLeftNonEmptyMovesCursor` -- `{a, ArrowLeft}` -> draft cursor
  moved, `mode == Conversation`.
- `Errata81.ArrowLeftInSubagentViewNoOpen` -- pushed child + `{ArrowLeft}` ->
  still the child view.
- `Errata81.ArrowLeftWithDialogNoOpen` -- `dialog.open` + `{ArrowLeft}` ->
  dialog still open.
- `Errata81.CtrlSOpensDashboard` -- `{CtrlS}` -> `mode == Dashboard`.
- `Errata81.CtrlPOpensDashboard` -- `{CtrlP}` -> `mode == Dashboard`.
- `Errata81.CtrlSNotPopup` -- after `{CtrlS}`, `mode != Switcher` (81-I20).
- `Errata81.CursorKeysMove` -- `{Down, j, Up, k}` change `cursor` monotonically.
- `Errata81.PageHomeEndClamp` -- `{PageDown, End, PageUp, Home}` clamp to
  `[0, rows-1]`.
- `Errata81.EnterAttachesFocusOnly` -- select another row, `{Return}` ->
  `mode == Conversation`, active session switched, no `session.activate` sent
  (assert the fake daemon received none).
- `Errata81.RightAttachesLikeEnter` -- `{ArrowRight}` equivalent.
- `Errata81.EscRestoresWithoutAttach` -- move cursor, `{Escape}` -> mode,
  focus, and scroll unchanged.
- `Errata81.DoubleCtrlCArmsThenExits` -- `{CtrlC}` -> armed, not exited; `{CtrlC}`
  within the window -> `begin_exit` (exit confirm or clean exit).
- `Errata81.SingleCtrlCNoExitNoCancel` -- one `{CtrlC}` neither exits nor calls
  `cancelActive`.
- `Errata81.CtrlQConsumedInDashboard` -- `{CtrlQ}` -> still `Dashboard`.
- `Errata81.ArrowLeftInDashboardConsumed` -- with `mode == Dashboard`,
  `{ArrowLeft}` leaves `mode == Dashboard` and does not re-enter/duplicate the
  open (the chain-A Dashboard guard consumes it before the chain-B
  `ArrowLeft` open guard, sec 2.2; 81-I19).
- `Errata81.ExitArmDisarmedByMove` -- arm, then `{Down, CtrlC}` -> no exit.
- `Errata81.ComposerDraftSurvives` -- type text, Ctrl+S, Esc -> draft intact
  (81-I16).
- `Errata81.EmptyDashboardInline` -- zero live sessions -> `mode == Dashboard`
  with `rows.empty()` and **no** `Notice`.
- `Errata81.RebuildOnCatalogDelivery` -- deliver a snapshot while open -> rows
  updated, cursor preserved.
- `Errata81.CursorSurvivesResize` -- open with the cursor on a middle row,
  change the harness `TerminalSize` (resize) -> `cursor` and `rows` unchanged
  (the visible window is recomputed only at render, 81-I9/81-D9).
- `Errata81.ExitConfirmCancelReturnsToDashboard` -- open the dashboard, then
  `{CtrlC, CtrlC}` with a non-empty orphaning set -> `exitConfirm.open`,
  `mode == ExitConfirm`, and `dashboard.open` still true; `{Escape}` ->
  `mode == Dashboard`, `dashboard.open` true, `rows`/`cursor`/`collapsed`
  intact; a further `{Escape}` closes to `Conversation` (81-D13, 81-I21,
  81-F13).
- `Errata81.ExitConfirmConfirmQuits` -- `{CtrlC, CtrlC}` with an empty orphaning
  set -> clean exit (`begin_exit` -> `confirm_exit`), no stuck overlay.
- `Errata81.PermissionDialogOverDashboardRestores` -- dashboard open, inject
  `PermissionRequested` -> `mode == Dialog`, `dialog.prev_mode == Dashboard`,
  `dashboard.open` true; inject the matching `PermissionResolved` ->
  `mode == Dashboard` (81-D13).
- `Errata81.HarnessOpenSwitcherOpensDashboard` -- `open_switcher()` (the harness
  hook, `supervisor.cpp:4164`) -> `mode == Dashboard`, not `Switcher` (81-D4).

### 10.3 Golden render (`tests/unit/ui_render_golden_test.cpp`)

`render_to_ansi` snapshots; assert exact strings (the file already uses this
pattern, e.g. `MultiWorkspaceSwitcherTree` `:355`):

- `UiRenderGolden.DashboardGroupedList` -- Needs input/Working/Completed headers,
  rows with glyph+label+title+workspace+age.
- `UiRenderGolden.DashboardCountsLine` -- the summary literal
  `N awaiting input - M working - K completed`.
- `UiRenderGolden.DashboardEmptyState` -- `No live sessions.` and no Notice.
- `UiRenderGolden.DashboardNarrowDegrades` -- at width 50, workspace/age columns
  gone, footer shortened.
- `UiRenderGolden.DashboardLongListWindow` -- many rows, cursor near the end,
  window scrolls and renders `... N more`.
- `UiRenderGolden.DashboardNoClearUnder` -- the grid at the top-left is the
  dashboard's own (the conversation is not visible through it).
- `UiRenderGolden.DashboardOverlayPrecedence` -- `exitConfirm.open` over the
  dashboard composes as `dbox` and the dashboard is the base.
- `UiRenderGolden.DashboardUnderExitConfirm` -- with `dashboard.open == true`
  and `exitConfirm.open == true`, the base is the dashboard: the conversation
  header/transcript are **absent** from the grid and the exit-confirm window is
  drawn over the dashboard (chain R, 81-I1/81-I2, 81-D13).
- `UiRenderGolden.DashboardUnderPermissionDialog` -- same for `dialog.open`
  (the permission dialog over the dashboard, not over the conversation).
- `UiRenderGolden.DashboardCancellingIsWorking` -- a row whose `SessionCell.state
  == AgentState::Cancelling` renders the `Working` glyph `*` and label, not the
  `state_glyph(Cancelling)` `-` (81-D5).
- `UiRenderGolden.DashboardStatusGlyphs` -- `*`/`!`/`o`/`+`/`x` per status.
- `UiRenderGolden.DashboardStatusGlyphNotStateGlyph` -- a `CallingTool` row
  renders `*` (Working status glyph), never `state_glyph`'s `>` (closes LOW-3;
  81-I6).
- `UiRenderGolden.DashboardStoppingBadge` -- `DaemonStatus::Stopping` workspace
  shows `[stopping]`.

### 10.4 PTY / live (opt-in)

`tests/unit/ui_supervisor_pty_test.cpp` (PTY driver, no LLM needed for the
gesture) gains: launch `ymh`, wait for the composer, send `\x1b[D` (ArrowLeft) on
an empty composer, assert the dashboard header appears and a listed live session
renders; send `Esc`, assert the conversation returns with the same focused
session. A live-LLM variant (`YMH_LIVE_LLM=1`) may select and attach to assert
focus-only (no new turn).

### 10.5 Mandatory updates to existing tests

Following 81-D4 (Ctrl-S now opens the dashboard) and 81-D4's `Notice` removal, the
suite neither compiles nor stays green until **every** occurrence below moves in
the same change set. This is ONE exhaustive table: every `file:line` occurrence of
every symbol 81 deletes or renames, plus every retained-hook site whose semantics
change, appears exactly once. There is no catch-all.

**Symbol inventory (raw grep over `src/`, `include/`, `tests/`; this revision).**
The counts are the completeness check for the table:

- `UiMode::Notice` -- 23 (3 `src/` + 20 `tests/`: errata57 1, errata46 9, errata49
  8, ui_render_golden 2).
- `handle_notice` -- 2 (both `src/`: definition + call).
- `render_notice` (the `src/ui/ui_render.cpp` free function only) -- 2 (both
  `src/`: definition + call). Distinct and retained: the same-named
  `src/agent/subagent_service.cpp:129,560,916` overload and `render_notice_block`
  (`ui_render.cpp:902,963,1114`).
- `MessageDialogModel` -- 2 (both `include/`: struct + field).
- `UiModel::message` -- 30 (21 `.message.open` + 9 `.message.text`).
- `UiModel::openSwitcher` / `SupervisorApp::openSwitcher` (renamed) -- 36 (7
  `src/`+`include/`, 28 test calls, 1 test comment).
- `switcher_has_targets` -- 2 (both `src/`: definition + call; orphaned).
- `other_live_workspace_exists` -- 2 (both `src/`: definition + call; orphaned).
- `open_switcher()` harness hook (name retained, body changes) -- 28 (2
  `src/`+`include/`, 26 test calls).
- `open_live_switcher()` test helper (name retained, body changes) -- 16 (1
  definition + 15 calls).

Dispositions: **REMOVE** / **RENAME** / **REPLACE** are production edits;
**DELETE** drops the test case or line; **RETARGET** rewrites it to
`openDashboard()` / `UiMode::Dashboard`; **LATENT-LIVE** keeps Live-popup coverage
via `model.switcher.open(model); model.mode = UiMode::Switcher;` (`SwitcherSource`
defaults to `Live`, `ui_model.hpp:496`; `SwitcherOverlayModel::open` is retained,
81-I20) and keeps its Live-branch assertions; **MIGRATE** moves the invariant to
the History/Subagents surface. The delete-badge and ownership-mark goldens require
LATENT-LIVE because the dashboard does not reproduce them (81-D11).

#### 10.5.1 Every occurrence (exhaustive)

| Occurrence (file:line) | Context (production site / test or helper) | Disposition | Concrete edit / kept assertion |
|---|---|---|---|
| `src/ui/supervisor.cpp:1266`, `:3828`; `src/ui/ui_render.cpp:2206` | `UiMode::Notice` (member, dispatch guard, render branch) | REMOVE | delete the member (`ui_event.hpp:52`), the guard (`:3828-3830`) and the branch (`ui_render.cpp:2206-2207`) |
| `src/ui/supervisor.cpp:1274` (def), `:3829` (call) | `handle_notice` | REMOVE | delete the member and its call (goes with the guard above) |
| `src/ui/ui_render.cpp:1577` (def), `:2207` (call) | `render_notice` (ui_render only) | REMOVE | delete; the call goes with `:2206-2207`. The `subagent_service.cpp` overload and `render_notice_block` are distinct and retained |
| `include/ymh/ui/ui_model.hpp:564` (struct), `:675` (field); `src/ui/supervisor.cpp:1262`, `:1265`, `:1277`; `src/ui/ui_render.cpp:1579` | `MessageDialogModel` / `UiModel::message` (dead field + Notice producer/renderer) | REMOVE | delete the struct, the field, and these reads/writes |
| `src/ui/supervisor.cpp:2179` | `modal_owns_input` `message.open` clause | REPLACE | drop `model_.message.open \|\|`; add `\|\| model_.mode == UiMode::Dashboard` (81-I16, 81-F12) |
| `include/ymh/ui/ui_model.hpp:735`, `src/ui/ui_model.cpp:1375`, `src/ui/supervisor.cpp:1270` | `UiModel::openSwitcher()` | RENAME | -> `openDashboard()` (declaration, definition, self-call) |
| `src/ui/supervisor.cpp:1259` (def), `:1441` (comment), `:3848` (call), `:4164` (harness body) | `SupervisorApp::openSwitcher()` | RENAME | -> `open_dashboard()`; body keeps `disarm_esc()` + `model_.openDashboard()`, drops the Notice branch |
| `src/ui/supervisor.cpp:1230` (def), `:1261` (call) | `switcher_has_targets` | REMOVE | orphaned by the branch deletion; delete |
| `src/ui/supervisor.cpp:1218` (def), `:1262` (call) | `other_live_workspace_exists` | REMOVE | orphaned by the branch deletion; delete |
| `include/ymh/ui/supervisor_harness.hpp:63` (decl); body at `src/ui/supervisor.cpp:4164` (renamed above) | `open_switcher()` harness hook | RETAIN | name kept (81-D4); override body calls `app_.open_dashboard()` |
| `ui_model_test.cpp:425` | `UiModel.SwitcherNavigatesAcrossWorkspaces` | RETARGET | `openDashboard()`; assert `mode == Dashboard` |
| `ui_model_test.cpp:450` | `UiModel.SwitcherCollapseHidesSessions` | RETARGET | same |
| `ui_model_test.cpp:1103` | `UiModel.SwitcherNodesCarryOwnershipMark` | RETARGET | same |
| `ui_model_test.cpp:1131` | `UiModel.SwitcherOpenIsLiveOnly` | RETARGET | same; live-only predicate kept via `dashboard.rows` |
| `ui_model_test.cpp:1216` | `UiModel.DaemonDeathHidesWorkspace` | RETARGET | same |
| `ui_model_test.cpp:1277` | `UiModel.SwitcherOrderingAndSessionOrder` | RETARGET | same; assert ordering on `dashboard.rows` |
| `ui_model_test.cpp:1516`, `:1521` | `UiModel.SwitcherSourceResetsToLive` | RETARGET | `openDashboard()`; assert `mode == Dashboard` |
| `ui_model_test.cpp:1768` | `UiModel.UI45_D4_LiveEmptyNodeRendered` | LATENT-LIVE | keep `sessions.empty()`, `sessions_hidden_by_focus`, and `(current session hidden)` `:1775` (`open(model)` still hides the focused session, 81-D2) |
| `ui_model_test.cpp:1789` | `UiModel.UI45_D4_LivePlaceholderLeaf` | LATENT-LIVE | keep `(no live sessions)` `:1796` -- the hardcoded live leaf (`ui_render.cpp:1359`), not `policy.empty_state` |
| `ui_model_test.cpp:1801` | `UiModel.UI45_D4_LiveCatalogPendingPlaceholder` | LATENT-LIVE | keep the `:1806` leaf. The shipped literal ends with a **U+2026 ellipsis** (`"(loading live sessions"` + U+2026 + `")"`, `ui_render.cpp:1347`); it is written here with ASCII `...` only because this spec is ASCII-only |
| `ui_model_test.cpp:1822` | `UiModel.UI45_D4_LiveCatalogNotePlaceholder` | LATENT-LIVE | keep `(corrupt)` `:1828` |
| `ui_render_golden_test.cpp:946` | `UiRenderGolden.SwitcherShowsShortIdForPlaceholderTitle` | LATENT-LIVE | keep the short-id live leaf |
| `ui_render_golden_test.cpp:977` | `UiRenderGolden.SwitcherShowsRenamedSessionTitle` | LATENT-LIVE | keep the renamed-title live leaf |
| `ui_render_golden_test.cpp:1011` | `UiRenderGolden.SwitcherAttentionBadgeStillRenders` | LATENT-LIVE | keep the attention badge |
| `ui_render_golden_test.cpp:1258` | `UiRenderGolden.SwitcherShowsOwnershipMarks` | LATENT-LIVE | keep `[owned]`; `:1263` `"Switcher"` -> `"Sessions"` (repurposed Live heading, sec 5) |
| `ui_render_golden_test.cpp:1555` | `armed_session_model()` helper (used by `SwitcherDeleteConfirmInline` `:3125`/`:3126`, `SwitcherDeleteConfirmColourGate` `:3225`/`:3226`, `SwitcherDeleteConfirmInvariants` `:3151`) | LATENT-LIVE | replace the helper body's `openSwitcher()` with the latent-Live idiom once; all three consumers keep their Live delete-badge/window assertions |
| `ui_render_golden_test.cpp:1838`, `:1871`, `:1880`, `:1897` | `UiRenderGolden.UI45_G4_LiveNoSuppression` | LATENT-LIVE | keep `(current session hidden)` `:1877`, the `:1884` pending leaf (U+2026 as shipped), `(corrupt)` `:1901` |
| `ui_render_golden_test.cpp:1960` | `UiRenderGolden.UI45_D4_WholeListPlaceholder` | LATENT-LIVE | `(no workspaces)` `:1964` -> `"No live sessions."` (repurposed `policy.empty_state`, sec 5) |
| `ui_render_golden_test.cpp:3166` | `UiRenderGolden.SwitcherDeleteConfirmInvariants` | LATENT-LIVE | direct call -> latent-Live idiom; keep window-border/width/row-count invariants and the armed-badge assertion |
| `ui_render_golden_test.cpp:3292` | `UiRenderGolden.SwitcherRowStyleAlignment` | LATENT-LIVE | direct call -> latent-Live idiom; keep `- Alpha`, `    [one`, `    [two` row styling |
| `ui_render_golden_test.cpp:373` | `UiRenderGolden.MultiWorkspaceSwitcherTree` | RETARGET | `:377` `"Switcher"` -> `"Sessions"` |
| `ui_render_golden_test.cpp:2080` | `UiRenderGolden.UI49_G2_MultiWorkspaceSwitcherUnchanged` | RETARGET | `:2085` `"Switcher"` -> `"Sessions"` |
| `ui_render_golden_test.cpp:2238` | `UiRenderGolden.UI46_G4_SwitcherOpaque` | RETARGET | `:2244` `expect_opaque_row(rendered, "j/k move", "BLEEDMARK")` -> `expect_opaque_row(rendered, "Up/Down move", "BLEEDMARK")` (repurposed dashboard footer, sec 5) |
| `render_golden_test.cpp:233` | `RenderGolden.TuiExitConfirmOverlayCountsAndOwnershipMark` | LATENT-LIVE | keep the `[owned]` ownership-mark golden |
| `ui_model_test.cpp:1513` | comment above `UiModel.SwitcherSourceResetsToLive` | RENAME | `openSwitcher` -> `openDashboard` in the comment |
| `ui_model_test.cpp:2213-2224` | `UiModel.UI58_U21_SwitcherPolicyTable` | RETARGET | `live.heading` -> `"Sessions"`, `live.empty_state` -> `"No live sessions."`, `live.footer` -> the ASCII dashboard footer; keep `window_title`, `enter`, `ctrl_d_enabled`, `tab_expands`, `ctrl_t_closes`, `r_refreshes` at the latent Live row (sec 10.5.2 note) |
| `errata57_ui_test.cpp:227`,`:228`,`:229`,`:234` | `Errata57.CtrlQSwallowedByEveryPopup` (`Notice` segment) | DELETE | delete `:227`-`:234` (the whole segment: `message.open`/`message.text`/`UiMode::Notice` + its Ctrl+Q asserts + the `:234` `message.open` reset). Deleting only `:227-229` leaves `:234` uncompilable (HIGH-1). `:235`+ Context/ModelPicker/ExitConfirm/Dialog still prove Ctrl+Q is swallowed |
| `errata57_ui_test.cpp:152` (assertions `:154`,`:160`,`:161`) | `Errata57.CtrlQInSwitcherDoesNotExit` | RETARGET | `open_switcher()` now opens the dashboard; assert `mode == Dashboard` at `:154`/`:160`/`:161`; `:167`/`:173` stay `Switcher` (History via `/sessions`, `:165`) |
| `errata57_ui_test.cpp:191` | `Errata57.ArmInlineDisarmTable` | LATENT-LIVE | the dashboard consumes Ctrl+D as "any other key" (81-D8) and never arms the Live delete; replace the call with `model.switcher.open(model); model.mode = UiMode::Switcher;` (81-I20); keep `:196-:218` delete-arm coverage |
| `errata46_ui_test.cpp:161`,`:163`,`:164`,`:165`,`:176`,`:177`,`:182`,`:192`,`:193`,`:198`,`:208`,`:209`,`:213`,`:214`,`:227`,`:229`,`:230`,`:264`,`:266`,`:267`,`:280`,`:281`,`:287`,`:297`,`:298`,`:302`,`:303` | `Errata46D3` suite (`:151`-`:307`) | DELETE | delete the whole `Errata46D3` block `:151`-`:307` (the `Notice` cases plus the two switcher cases). Empty-target coverage -> `Errata81.EmptyDashboardInline` (sec 10.1); switcher-predicate coverage -> `Errata81`/`UiRenderGolden.Dashboard*` (sec 10.1-10.3). The old `:151-302` range missed `:303`-`:307` (LOW-2) |
| `errata46_ui_test.cpp:836` | `Errata46D13.UI46_D13_EmptyDraft` | DELETE | delete the lone `message.open` assertion; keep `:835` empty-draft assertion |
| `errata49_ui_test.cpp:136`,`:138`,`:139`,`:140` | `UI49_D5_OtherLiveWorkspaceWithoutSessionsShowsNotice` (`:124`-`:142`) | DELETE | delete the case; replaced by the dashboard inline empty state (81-D9) |
| `errata49_ui_test.cpp:193`,`:195`,`:196` | `UI49_D6_OnlyLiveWorkspaceNoticeText` (`:184`-`:197`) | DELETE | same |
| `errata49_ui_test.cpp:258`,`:259`,`:263` | `UI49_D8_NoticeDismissal` (`:249`-`:265`) | DELETE | same |
| `errata49_ui_test.cpp:274`,`:275`,`:279`,`:280` | `UI49_D8_NoticeSwallowsCtrlS` (`:267`-`:282`) | DELETE | same |
| `errata49_ui_test.cpp:294`,`:295`,`:301` | `UI49_D8_NoticeBlocksComposer` (`:284`-`:302`) | DELETE | same |
| `errata49_ui_test.cpp:336`,`:337` | `UI53_F1_DegradedStartCtrlSNotice` (`:329`-`:338`) | DELETE | replaced by the dashboard inline empty state (81-D9) |
| `errata49_ui_test.cpp:159`,`:161`,`:162` | `UI49_D5_OtherLiveWorkspaceWithSessionShowsSwitcher` (`:144`-`:163`) | RETARGET | `:159` `openDashboard()`; `:161` assert `Dashboard`; delete `:162` `message.open` assertion |
| `errata49_ui_test.cpp:179`,`:181` | `UI49_D5_ActiveWorkspaceSecondSessionShowsSwitcher` (`:165`-`:182`) | RETARGET | `openDashboard()`; assert `Dashboard` |
| `errata49_ui_test.cpp:209`,`:211` | `UI49_D5_CatalogPendingFallsBackToSwitcher` (`:199`-`:212`) | RETARGET | same |
| `errata49_ui_test.cpp:227`,`:229` | `UI49_D5_NoteWorkspaceFallsBackToSwitcher` (`:214`-`:230`) | RETARGET | same |
| `errata49_ui_test.cpp:244`,`:246` | `UI49_D5_ActiveWorkspaceUnknownMembershipFallsBackToSwitcher` (`:232`-`:247`) | RETARGET | same |
| `ui_render_golden_test.cpp:2032`,`:2033`,`:2034` | `UiRenderGolden.UI46_G1_NoticePopup` (`:2030`-`:2043`) | DELETE | delete the case; `render_notice` is removed |
| `ui_render_golden_test.cpp:2049`,`:2050`,`:2051` | `UiRenderGolden.UI49_G1_NoticeNoOtherSessions` (`:2047`-`:2060`) | DELETE | same |
| `errata51_ui_test.cpp:179` (helper `:178`-`:181`) + calls `:238`,`:257`,`:275`,`:290`,`:307`,`:326`,`:349`,`:358`,`:376`,`:401`,`:417`,`:432`,`:447`,`:507`,`:591` | `open_live_switcher()` Live delete/arm cases | LATENT-LIVE | rewrite the helper body to `harness->mutable_model().switcher.open(harness->model()); harness->mutable_model().mode = UiMode::Switcher; drain_fully(*harness);` (81-I20); the 15 call sites are unchanged |
| `errata58_ui_test.cpp:292`,`:294` | `Errata58.UI60_ShiftPageConsumedByPopup` (`:290`-`:303`) | MIGRATE | open the History popup via `dispatch_command_line("/sessions")` (`switcher.source == History`); keep the spec-60 consumption proof |
| `errata58_ui_test.cpp:341`,`:343`,`:344` | `Errata58.UI58_H7_ChildNotInLiveSwitcher` (`:332`-...) | MIGRATE | retarget to `dispatch_key("ctrl-t")` (`source == Subagents`) or assert `mode == Dashboard` and the child absent from `dashboard.rows` |
| `errata58_ui_test.cpp:572`,`:574`,`:576`,`:577` | `Errata58.UI58_H19_CtrlTDoesNotCloseCatalogSwitchers` (`:568`-`:592`) | MIGRATE | the dashboard consumes Ctrl+T (81-D8); replace `:572` with `dispatch_command_line("/sessions")` and drop the Live `:574`/`:576`/`:577` assertions; `:582`-`:591` unchanged |
| `errata58_ui_test.cpp:672`,`:674` | `Errata58.UI58_H23` live Ctrl+D refusal (`:670`-`:681`) | LATENT-LIVE | replace `:672` with the latent Live idiom; keep the "stop the workspace first" refusal `:678`-`:680` (81-I20) |
| `errata66_ui_test.cpp:178` | `Errata66Paste.PasteSurvivesModalClose` (`:171`-`:188`) | RETARGET | `open_switcher()` keeps its name and now opens the dashboard, so `:178` is unchanged; keep the `:182`/`:187` paste assertions |

**Completeness.** The occurrence groups above sum to the inventory counts
(23 + 2 + 2 + 2 + 30 + 36 + 2 + 2 + 28 + 16 = 143 raw hits), so every
`file:line` occurrence of a removed/renamed symbol is dispositioned. After these
edits no test TU references `UiMode::Notice`, `UiModel::message`,
`MessageDialogModel`, `handle_notice`, or the `ui_render.cpp` `render_notice`,
and every renamed call site resolves -- i.e. the suite compiles.

#### 10.5.2 `switcher_policy(Live)` pin

`UI58_U21_SwitcherPolicyTable` (`ui_model_test.cpp:2213-2224`) pins the old Live
policy row. Sec 5 repurposes it, so retarget `live.heading` -> `"Sessions"`,
`live.empty_state` -> `"No live sessions."`, and `live.footer` -> the ASCII
dashboard footer (`"Up/Down move | Enter attach | Esc close | Ctrl+C twice
quit"`), keeping `window_title`, `enter`, `ctrl_d_enabled`, `tab_expands`,
`ctrl_t_closes`, `r_refreshes` at the latent Live row's values (sec 5).

#### 10.5.3 PTY held strings (`ui_supervisor_pty_test.cpp`)

The Ctrl-S PTY expectation moves to the dashboard heading/empty state (the new
ArrowLeft expectation is sec 10.4):

- `SwP4_LiveSwitcherHidesStoppedWorkspaceHistoryShowsIt` (`:1358`, frame-absent
  `:1363`) and `SwLive_HidesStoredClosedSessionsOnLiveWorkspace` (`:1403`,
  frame-absent `:1412`) wait `"No other workspaces available"` -> the dashboard
  empty state `"No live sessions."` (both list zero rows: the focused session is
  uncatalogued / the workspace opens no session).
- `UI53_P1_EagerCwdThenCtrlSShowsSwitcher` (`:1921`) waits `"Switcher"` ->
  `"Sessions"`.
- `UI49_P2_TwoWorkspacesWithSessionsShowsSwitcher` waits `"Switcher"` (`:2034`)
  -> `"Sessions"`; its `"No other sessions available"` fallback (`:2038-2040`)
  is deleted -- the dashboard always opens, so Ctrl-S never yields a `Notice`.
- `SwLive_ShowsOtherWorkspaceLiveSession` (`:1426`) sends `\x13` at `:1467` and
  asserts `(current session hidden)` present (`:1470`); the dashboard includes
  the focused session (81-I8) and emits no such leaf, so keep the `zzbetalive`
  assertion (`:1468`), retarget the frame check to the dashboard (assert the
  `"Sessions"` heading), and change the `:1470` assertion to
  `EXPECT_EQ(child.last_frame().find("(current session hidden)"), std::string::npos)`.

---

## 11. Open questions and recorded risks

- **81-OQ1 (low).** Should the dashboard reserve a filter (`/`) and a dispatch
  input like Claude Code's agent view? Default: no (out of scope). The key space
  is reserved by 81-D8's "any other key is consumed".
- **81-OQ2 (low).** A "Stopped" row needs a durable per-session stop flag; none
  exists. The transient `AgentState::Cancelling` is mapped to `Working` (81-D5),
  not to `Stopped`, precisely so `Stopped` stays a durable-only vocabulary
  member. The seam is an additive `sessions` column or a `SessionCell` bit; not
  in this spec (81-D5 non-mirror).
- **81-OQ3 (low).** `Pinned` needs a pin store; seam named in 81-D6. Until then
  the group is absent.
- **81-R1 (medium, tracked).** Retiring `UiModel::openSwitcher()` and changing
  the `open_switcher()` harness semantic touches the single-process TUI and both
  harnesses; the implementer must update every call site in the same change set
  (AGENTS.md "new symbols are normative"; a symbol with no caller blocks
  `verified`).
- **81-R2 (low).** The Live popup becomes latent (mode-only). A future cleanup
  may delete `SwitcherSource::Live`'s popup path entirely; 81-I20 pins the
  current latent-not-dead invariant.

---

## 12. Revision log

| Revision | Change |
|---|---|
| 1 | Initial draft: `UiMode::Dashboard`, `DashboardModel`, ArrowLeft open gesture, Ctrl-S alias, status vocabulary/grouping/two non-derivable groups, columns/counts, keys incl. double-Ctrl+C, empty/scroll/narrow, state-lifetime table, invariants 81-I1..81-I20, failure modes 81-F1..81-F12, dsh mapping, golden + hermetic + PTY test plan. |
| 2 | Gate fixes: record the `UiMode::Notice` surface as dead and removed (81-D4, sec 1.3/4/9), pin the `SessionCell`+`UiModel::session(id)` status bridge and `DashboardRow::workspace_last_used` (81-D2/5/6), fix the `short_id`/relative-age anchors (81-D7), add `kDashboardNarrowWidth` and real comparator/glyph signatures (sec 4/5), define ArrowLeft precedence in both dispatch chains without shadowing spec-60 (sec 2.2, 81-D3), add the resize disposition (sec 9), reword "derived, never stored" (81-D5), pin Ctrl+S in a child view (81-D4), disarm on the firing Ctrl+C (81-D8.2, 81-I13), and enumerate every mandatory test migration (sec 10.5). |
| 3 | Gate fixes: pin the `modal_owns_input` Dashboard clause that replaces the removed `message.open` (81-D4, sec 4); complete sec 10.5 with every remaining breaking site (`UI58_U21_SwitcherPolicyTable` policy row, `errata58:290-303`/`:332-349`, `errata46:836`, the four PTY held strings, the golden `"Switcher"` assertions and the preserved Live-popup goldens); record `SwitcherOverlayModel::open(model)` as latent (81-I20, 81-D2); add Ctrl+T and the child-view Ctrl+C branch to 81-I19; fix the `Notice`/dispatch/renderer/`MessageDialogModel` anchor off-by-ones. |
| 4 | Gate fixes (re-review): complete sec 10.5 with the nine previously unlisted blocking sites and their concrete dispositions -- the four `UI45_D4_*` Live-placeholder model tests (latent-Live idiom, assertions kept; the `(no live sessions)` leaf is the hardcoded `ui_render.cpp:1359`, not the repurposed `policy.empty_state`), `UI45_G4_LiveNoSuppression`, `UI45_D4_WholeListPlaceholder` (latent Live; `(no workspaces)` -> `"No live sessions."`), `UI49_G2_MultiWorkspaceSwitcherUnchanged` (`"Switcher"` -> `"Sessions"`), `UI46_G4_SwitcherOpaque` (`j/k move` -> `Up/Down move`), and PTY `SwLive_ShowsOtherWorkspaceLiveSession` (no `(current session hidden)`); replace the golden catch-all with explicit per-site dispositions. Pin `DashboardModel::open()` to save `prev_mode` before `mode` is set (81-D2, sec 9). Reconcile `dashboard_group`'s callers and present `handle_dashboard`/`open_dashboard` as `SupervisorApp` members in sec 5. Fix the `supervisor.cpp:1256-1271` -> `:1259-1271` and `errata58:574-577` -> call `:572`/assertions `:574-577` anchor drift. Add `Errata81.ArrowLeftInDashboardConsumed` for chain-A precedence completeness. |
| 5 | Gate fixes (HIGH-1/MEDIUM-1/LOW-1/LOW-2/LOW-3) and structural de/re-currence: **(HIGH-1)** sec 10.5 rewritten as an exhaustive, tabulated disposition of all 28 `openSwitcher()` call sites plus the `armed_session_model()` helper and its three delete-badge consumers (`:1555`, `:3166`, `:3292` now LATENT-LIVE); the false "every site dispositioned" catch-all claim is replaced by the table. **(MEDIUM-1)** new `81-D13` + invariant `81-I21` + failure `81-F13`: chain R selects the base from `dashboard.open`, and overlays that can be raised from the dashboard (`ExitConfirmState::prev_mode`, `PermissionDialogModel::prev_mode`) save/restore the interrupted mode so cancelling the dashboard's own exit confirm returns to it; state-lifetime rows added. **(LOW-1)** `dashboard_row_less` callers pinned to `open`/`rebuild` via one shared `project()`. **(LOW-2)** the `(loading live sessions...)` literal marked as a shipped U+2026 ellipsis. **(LOW-3)** `cell.state`/`cell.attention` anchor corrected to `:914-916`; `UI58_U21_SwitcherPolicyTable` anchor to `:2213-2224`; glyph section states the dashboard uses `dashboard_status_glyph`, not `state_glyph`. Also: exhaustive status derivation incl. `AgentState::Cancelling` -> `Working`, complete three-chain key precedence table (sec 2.2), BOTH overlay precedence chains, explicit 22/57 + `AGENTS.md:141-142`/`UI_SURFACE_INVENTORY.md` doc-summary reconciliation (sec 1.3.1), named golden/interaction tests, and the `Revision: 5` bump. |
| 6 | Gate fixes (HIGH-1, LOW-1, LOW-2) and the end of incremental dispositioning: **(HIGH-1)** sec 10.5 collapsed into ONE exhaustive disposition table with a symbol inventory and stated grep counts; every occurrence of every symbol 81 deletes or renames is listed, including the previously missed `errata57_ui_test.cpp:234` (`model.message.open = false`), whose `Notice` segment is now deleted as `:227`-`:234` (not `:227-229`); all `errata46`/`errata49`/`errata58`/`errata51`/`errata66`/golden sites are line-exhaustive (old `errata46:151-302` -> `:151-307`). **(LOW-1)** `cancel_exit` must capture `prev_mode` before the `ExitConfirmState{}` reset (81-D13, `:886-891`). **(LOW-2)** the `errata49` `message.open` lines `:263`/`:280` and the `errata46` suite end are now covered. Also: `switcher_has_targets`/`other_live_workspace_exists` recorded as orphaned-and-removed (sec 4/10.5); `render_notice` distinctness from the `subagent_service.cpp` overload stated. `Revision: 6` bump. |

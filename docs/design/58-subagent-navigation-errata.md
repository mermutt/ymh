# 58 — Subagent Navigation Errata (enter / view / return)

```
Status: verified (Rev 6) · GATED — the gate closed after **6 revisions**: on
        Rev 6 all five adversarial reviewers returned **PASS**, and Oracle's
        fourth and final independent pass returned **PASS**, stating the spec may
        be marked `verified` (0 open HIGH/MEDIUM; the four residual LOWs were
        documentation-only and are swept here). This is the design-first gate
        artifact; implementation may now begin (AGENTS.md "The rule"). It is
        authored in two parts, Part A (UI) first, then Part B (implementation),
        per the user's explicit ordering ("Design UI first and then design
        implementation and then implement").
Component: 58 (errata) — additive. Amends 00-architecture.md §20.25 and
           10-supervisor-tui.md §4.8 (the deferred subagent-panel design) by
           reference; it does not rewrite them. Amends 22-D6 (`/sessions`
           catalog scope), 45-I5 (Live leaf predicate), 45-I20/45-I24 (Tab),
           57-I3/57-I9 (footer/delete scope), and 50-I9 (`/sessions` resume
           surface); extends 45-I8/45-D5 (Esc precedence) and 51-D4 (delete
           contract) for the new view; §11 names every amendment and accounts
           for every invariant in 22/45/50/51/55/57.
Depends on: 00-architecture.md §20.25 (:3054-3110), §54 F1-F12 (:4832-4856),
            §9.11 F5 (:1378-1390), tmux/SSH terminals (:95);
            10-supervisor-tui.md §4.8 (:569-618); 22-switcher-sessions-errata.md
            §3/§7/§8/§10 (`SW1`–`SW26`, `22-D1`–`22-D6`); 45-ui-interaction-errata.md
            D5 (:533-580), D9 (:186), I5 (:1646), I8 (:1649), I20 (:1661),
            I24 (:1665); 48-ui-and-config-errata.md D2 (:149-...);
            50-skills-mcp-and-session-lifecycle-errata.md I7 (:440), I9 (:445);
            51-reasoning-fold-prompt-emphasis-and-tables-errata.md D4 (:723-...),
            I19-I27 (:1008-1016); 55-multi-agent-delegation-errata.md (verified)
            D3 (:441-489), D5 (:654-697), D7 (:812-821), I3 (:1410);
            57-switcher-sessions-popup-errata.md (verified) D4 (:531-571),
            D5 (:573-...), I3 (:812), I9 (:818), I11/I13 (:820-822).
Scope: three user-requested capabilities on the active session's subagents:
       (A) ENTER a subagent of the current session; (B) VIEW that subagent's
       conversation while it is still running (ongoing) AND after it has
       finished (completed); (C) RETURN to the main agent. Design only.
Supersedes/Amends: see §11. Adds no RPC and no persistence (pinned). §6 is the
       normative, exhaustive edit list; every mechanism claim elsewhere cites it.
```

**How to read this spec.** Part A (§1–§5) is the complete interaction and visual
design. Part B (§6–§7) is the implementation design and is only meaningful once
Part A is settled. §8–§12 (invariants, failure modes, dsh mapping, amendments,
test plan) apply to both. **§6 is normative and exhaustive**, in three parts:
**§6.A** declares every new symbol (declaration + body sketch + callers),
**§6.B** is the numbered `E1`–`E47` edit list (file + symbol + change), and
**§6.C** is the state-lifetime table. Every decision, §11 entry, and §14 entry
that asserts a mechanism cites a §6.A/§6.B/§6.C item; if a mechanism is not in
§6, it is not part of this design, and no name may be used anywhere in this spec
unless §6.A defines it or it is pre-existing code cited by `file:line`.

**Rev 4 scope (restructure + fixes).** Rev 1 was rejected with ≥5 independent
HIGH defects; Rev 2 and Rev 3 each resolved their round but were rejected by the
five-reviewer + Oracle re-check. The recurring generator was structural: §6
enumerated *edits to existing code* while **new symbols were only sketched**, and
no one tabulated **state lifetimes** — so every round found new undefined
symbols, and the Rev 3 HIGH (a child re-entered blank) was a state-lifetime
defect. Rev 4 restructures §6 and fixes every HIGH/MEDIUM:

- **§6.A (new symbols).** Every new symbol — `disarm(id)`, `viewed_children_`,
  `subagent_status_glyph`, `reconcile_subagent_path_for`, `ws`, `sync_subagent_subscriptions`,
  `SwitcherSourcePolicy` and its consumers, `CommandContext::subagents`,
  `forget_session`, `viewed_status`/`subagent_status_prefix`, `openSubagents`,
  `viewedSession`/`ensureSubagentState`/`ensureSessionState` — now has a
  declaration, a body sketch, and its callers.
- **§6.B (edits).** The `E1`–`E47` list is corrected and completed (three
  previously missing `eraseSession`/`eraseWorkspace` call sites; the History
  `r`-refresh gate; the adapter dedup).
- **§6.C (state lifetime).** One row per state, one column per transition; the
  `applied_event_ids_` row is the HIGH-1 mechanism.
- **HIGH-1** re-entry rendered empty: `applied_event_ids_` is now per-session and
  `forget_session` clears a popped child's ids (A10/E46/§6.C).
- **MEDIUM-1…7** (viewed_children_, `I23`/E4/History-refresh, the picker-source
  status-prefix bug, undefined `disarm(id)`, the missing erase call sites +
  missing-state fallback, undefined `ws`, the `50-I9` reason) and the LOW sweep;
  §14 records each disposition. No design question is left open (§13).

**Rev 5 scope (fix pass).** The Rev 4 re-check (R1–R5 + Oracle) found no
structural defect but four blocking items, all inside the normative §6.A/§6.B/
§6.C, plus a LOW sweep. Rev 5 applies them and leaves no design question open:

- **HIGH (`connection` + E37 ordering).** `sync_subagent_subscriptions` used a
  bare `connection` that does not exist (`SupervisorApp` holds `connections_`, a
  `map<WorkspaceId, unique_ptr<SupervisorConnection>>`, `supervisor.cpp:3547`);
  it now resolves through `connection_for` (A5), null-guarded so the E37 call
  after `evict_dead_workspaces` erased the connection is a safe no-op (A5's E37
  ordering note; the exact E37 mechanism is corrected in Rev 6 — the child's
  workspace is recorded at track time, see the Rev 6 scope).
- **MEDIUM (§6.C delete column / `58-H23`).** Children are in neither switcher
  source and a viewed workspace is live, so `Ctrl+D` can never delete it; the
  column is renamed `delete (external)` (`session.delete` E43 / scan eviction
  E37/E45), `E44` is recorded defensive/unreachable for a viewed child, and
  `58-H23` drives the real paths.
- **MEDIUM (`source ==` arithmetic).** Ten comparisons exist; the spec removes
  four (E4/E32/E5) and leaves six — corrected in §4/A3/A3.1/A3.2/I23.
- **MEDIUM (`45-I2`/`45-D1`).** E8 consumes every key while a child is viewed, so
  these — and, after the re-audit, `45-D2` — are suspended/scoped in the view
  (three suspensions) and named in §11.1 rather than marked unaffected; the
  re-audited `45-I25` is **not** a 58 scope — it was already superseded by
  verified `51-D4`/`51-A4` before this feature (§11.1).
- **LOWs.** A10's close wording; E38 names `handle_disconnect` (`:449`); §6.C's
  picker-cursor anchors; E25's rationale (children emit no `SessionCreated`);
  E42's duplicate `:1463` sync removed; A7's `node.title`/source anchors; A6's
  pop-vs-in-flight-subscribe guard.

**Rev 6 scope (fix pass).** The Rev 5 re-check (R1–R5) returned three PASSes and
two DO NOT APPROVEs on two narrow MEDIUMs, plus an accumulated LOW sweep. Rev 6
fixes both MEDIUMs and every LOW; no `src/`/`tests/` file changed and the spec is
still **not verified**:

- **MEDIUM-A (link-`Dead` prose vs the table).** `58-D8`, `58-I14`, and §6.C's
  `applied_event_ids_` daemon-death cell now say the path is cleared by the
  **eviction** (`E37`), not by the link transition: `E36` is inert while the child
  state survives (`E41`/§6.C agree). The `applied_event_ids_` row now splits the
  two halves (link `Dead` = unchanged; eviction = `forget_session`), like the
  `subscriptions_`/`tracked_` rows (R2-M1; R1-L5).
- **MEDIUM-B (E37 "nullptr" justification was false).** `viewed_children_` is now
  a `map<SessionId, WorkspaceId>` that records each child's owning workspace at
  **track time** (A5). On the E37 eviction path `eraseWorkspace` has already erased
  the child state and promoted `activeWorkspaceId` (`ui_model.cpp:861-873`), so the
  old derived lookup resolved `connection_for` to the **promoted** workspace and
  untracked on the wrong connection. With the recorded workspace,
  `connection_for(child_ws)` is genuinely `nullptr` after `connections_.erase`
  (`supervisor.cpp:1579`), and `eraseSession(child_ws, id)` is a no-op. The
  justification is corrected in A5's E37 note, E37, and §6.C (R3-M1).
- **LOWs.** A5's track direction is now genuinely idempotent (the id is recorded
  every sync and `track` is re-attempted; `SupervisorConnection::track` dedups
  `tracked_`, `supervisor_connection.cpp:64-73`) (R2-L2); E25's §5 rationale no
  longer implies a child `SessionCreated` (R1-L1); §6.C's parent-`SubagentModel`
  delete cell cites `F12` only (R1-L2); E43/E44/E45 (and E36/E37) drop the
  redundant explicit sync — they call `reconcile_subagent_path()`, which syncs
  (R1-L3); `45-I25` is recorded as a **pre-existing** supersession by
  `51-D4`/`51-A4`, and the §11.2 suspension count is **3** (R5-LOW-1); §6.C's
  `delete (external)` column becomes `close/delete (external)`, adds `E17`
  (`SessionClosed`), and qualifies `E43` as an external-client/test-seam path
  (R3-L1); §6.C's `delete_arm` daemon-death cell is exact for the Subagents
  picker (R3-L2).

---

# Part A — UI design

## 1. Current behaviour (verified, `file:line`)

1. **The `subagents:` strip is read-only and one line.** `render_subagents`
   (`src/ui/ui_render.cpp:441-460`) renders `"subagents:"` then, per
   `SubagentView`, `" [<8-hex id> <state_glyph>]"` plus a 32-char summary. It is
   placed only when non-empty (`build_ui`, `:1597-1599`) between the transcript
   and the composer. The glyph comes from `state_glyph`
   (`src/ui/ui_render.cpp:138-155`); `short_id` is the first 8 chars
   (`:209-211`).
2. **The strip model is `SubagentView{id, summary, state}`**
   (`include/ymh/ui/ui_model.hpp:264-268`), held in
   `SessionUiState::subagents` (`:283`) as `SubagentModel{agents}`
   (`:270-272`). **No sort** — insertion order; the update loop is
   `UiModel::apply`, `SubagentUpdated` (`src/ui/ui_model.cpp:1086-1098`).
3. **The strip only ever shows finished children.** The only producer of
   `SubagentUpdated` is `SubagentFanIn` (`src/ui/ui_event_adapter.cpp:137-146`).
   The spawn edge `payload::SubagentSpawned{subagent, task}`
   (`include/ymh/session/events.hpp:217-220`), appended to the parent log by
   `append_spawned` (`src/agent/subagent_service.cpp:371-380`), has **no UI
   event**. Consequence: a running child is invisible in the strip.
4. **`Completed` and idle-running are indistinguishable.** Fan-in maps
   `Completed → AgentState::Idle`, `Cancelled → Idle`, `Failed → Error`
   (`ui_event_adapter.cpp:139-143`); `state_glyph(Idle)` is `"o"`. A finished
   child and an idle child render identically.
5. **The viewed transcript is exactly `activeWorkspace().activeSessionId()`**
   (`build_ui`, `src/ui/ui_render.cpp:1559-1566`; `render_conversation`,
   `:406-430`). Switching sessions calls `UiModel::focusSessionIn`
   (`src/ui/ui_model.cpp:1237-1261`), which changes `activeSessionId` and
   `activeWorkspaceId`; the previous `SessionUiState` survives in
   `UiModel::sessions` (`:583`).
6. **Key contract (current).** `handle_event_inner`
   (`src/ui/supervisor.cpp:3393-3467`) routes popups first — `exitConfirm`
   `:3398`, `dialog` `:3401`, `Context` `:3405`, `Notice` `:3408`, `Switcher`
   `:3411`, `ModelPicker` `:3414` — then globals: `Ctrl+S`/`Ctrl+P` `:3417`,
   `Ctrl+Q` `:3425`, `Ctrl+C` `:3429`, `Ctrl+N` `:3433`, `Ctrl+O` `:3437`,
   `PageUp`/`PageDown` `:3441`/`:3445`, `Ctrl+Home`/`Ctrl+End` `:3449`/`:3453`,
   `Shift+Up`/`Shift+Down` `:3457`/`:3461`, then `handle_input` `:3465`. In the
   composer, `Esc` (`handle_input`, `:3086-3109`) hides command hints, else
   arms/confirms the interrupt; `Enter` submits (`:3110-3139`); `Tab`/`TabReverse`
   complete/cycle agent presets (`:3140-3163`). **`Ctrl+T` is unbound**: a
   tree-wide search for `CtrlT`/`Ctrl+T` in `src/`, `include/`, `tests/` is
   empty; FTXUI defines `Event::CtrlT`
   (`build/_deps/ftxui-src/include/ftxui/component/event.hpp:86`); the composer
   control keys are only `Ctrl+U`/`Ctrl+W`/`Ctrl+E`
   (`:3229`/`:3236`/`:3244`). 57-D4 pins that an open popup consumes its keys
   before the globals.
7. **The picker vocabulary.** `/sessions` and `Ctrl+S` are the **same** overlay
   (`SwitcherOverlayModel`, `include/ymh/ui/ui_model.hpp:418-456`), distinguished
   by `SwitcherSource::Live`/`History` (`:381-384`), opened by
   `UiModel::openSwitcher` (`src/ui/ui_model.cpp:1210-1215`) / `openHistory`
   (`:1342-1427`); `render_switcher` (`src/ui/ui_render.cpp:1146-1182`); Enter →
   `handle_switcher` (`src/ui/supervisor.cpp:2768-2821`). The codebase rule is
   "extend, do not duplicate" (22-D3, §4.3).
8. **Children are sessions.** A child has `SessionHeader.parentSession` set and
   `kind == SessionKind::Subagent` (`include/ymh/session/session.hpp:36,58`);
   `childId` **is** its `SessionId` and lives in the **same** daemon as the
   parent (55-D3, `docs/design/55-multi-agent-delegation-errata.md:467-481`).
   `session.list` enumerates the whole store including children, with
   `SessionSummary.kind` (`include/ymh/transport/protocol.hpp:377-389`) and a
   `live` flag that is true iff a resident agent exists
   (`src/host/host_runtime.cpp:543-546`). Because `refresh_sessions` tracks every
   `live` entry (`src/ui/supervisor.cpp:1703`) and creates a `SessionCell`
   (`:1711-1718`), a **resident** child today leaks into the Live switcher
   (`SwitcherOverlayModel::open` walks `WorkspaceModel::sessions`,
   `src/ui/ui_model.cpp:1295-1317`). The **History** source is a direct disk
   read (`openHistory`, `:1342-1427`) of `SessionPersistence::list()`
   (`src/ui/session_catalog.cpp:136-144`), which keeps `kind == Subagent` rows —
   so children also appear in `/sessions` today (the 22-D6 break, §58-D7).
9. **The overlay is source-branched in the shipped code.** `handle_switcher`
   checks `Ctrl+D` first, source-agnostically (`supervisor.cpp:2770`); `Tab`
   unconditionally calls `toggleExpand` (`:2792-2794`); `Return` is
   History-vs-else (`:2801-2815`); `render_switcher`/`switcher_disarmed_rows`
   branch on `history` for title, heading, footer, and empty string
   (`ui_render.cpp:1040-1051,1151,1180`, `:967`). A third source therefore
   inherits **delete**, **Tab-collapse**, and **reparent-by-focus** unless all
   are pinned; §58-D9 replaces these ad-hoc branches with one policy table.
10. **`DaemonDied` is not emitted in production.** `WorkspaceEventKind::DaemonDied`
    exists (`include/ymh/ui/ui_event.hpp:57-65`) and `UiModel::apply` handles it
    by setting `daemonStatus = Dead` only (`src/ui/ui_model.cpp:1187-1190`); it
    does **not** call `eraseWorkspace`. The production adapter maps only
    `SessionCreated → SessionOpened`, `SessionClosed → SessionClosed`,
    `DaemonShuttingDown → DaemonStopping` (`src/ui/ui_event_adapter.cpp:366-385`);
    nothing emits `DaemonDied`. The real workspace clearing is
    `evict_dead_workspaces` → `model_.eraseWorkspace(id)` on a presence scan
    (`src/ui/supervisor.cpp:1561-1590`, erase at `:1584`), gated by
    `switcher_eviction_candidates`/`ensure_in_flight_` (`:1571-1578`). §58-D8 and
    §58-H8 are anchored to that real hook (Rev 3; R1-N1/R3-N3/R4#4/R5-N3/Oracle).
11. **The adapter creates cells on `SessionOpened`.** `UiModel::apply`'s
    `SessionOpened` case calls `ensureSessionIn` (`src/ui/ui_model.cpp:1197-1199`),
    fed by `SessionCreated → SessionOpened` (`ui_event_adapter.cpp:366-371`); and
    `onSessionEnvelope` calls `ensureSessionIn` for any envelope whose session is
    unknown (`ui_event_adapter.cpp:299-301`). Both create a `SessionCell`. §58-D7
    must guard both, not only `refresh_sessions` (Rev 3; R1-N3/R2-N3).
12. **The catalog predicate is kind-blind.** `catalog_has_session`
    (`src/ui/ui_model.cpp:692-707`) returns true for any entry id, including
    `kind == "subagent"`; the Live leaf loop consults it at `:1308`. §58-D7 makes
    it kind-aware so `45-I5` stays true (Rev 3; R2-N1/R5-N1/Oracle).
13. **The read-only guard's blast radius.** `handle_input` is a single function
    (`src/ui/supervisor.cpp:3055-3258`) that owns composer submit, history recall,
    command completion, and `Tab`/`TabReverse` agent cycling
    (`:3140-3163`). A top-of-function "consume every non-`Esc` key" guard would
    also disable `45-D9`/`45-I20` cycling; §58-D3/D4 therefore specify the guard
    as an explicit pass-through policy and §11 names `45-I9`/`45-I20`/`45-I24`
    (Rev 3; R5-N6/Oracle).

## 2. Decisions (58-D)

### 58-D1 — Affordance: a picker overlay (`/subagents` + `Ctrl+T`), reusing the switcher widget

**Decision.** Entering a subagent is done from a **picker overlay**: a new slash
command `/subagents` and the unbound key **`Ctrl+T`** both open it. The picker is
the existing `SwitcherOverlayModel`/`render_switcher` widget extended with a
third source, `SwitcherSource::Subagents` (22-D3 "extend, do not duplicate"; the
reuse decision is §58-D9, the per-source data is the policy table §6.A). Rows are
the **current view level's direct subagents**, from
`viewedSession()->subagents.agents` (§58-D2/§58-D5) — the active session's
children at depth 0, a viewed child's children at depth > 0, so nesting is
reachable (§58-D2). `Enter` enters the highlighted child; `Esc`/`Ctrl+C`
close the picker; `Ctrl+T` toggles it closed. **`Ctrl+D` is a consumed no-op for
this source** (§58-D8, §58-D9) — the picker is read-only and non-deletable. With
**no active/viewed session** (fresh launch before the first lazy session,
`50-I7`), `Ctrl+T`/`/subagents` do not deref: `open_subagents` guards and pushes
the notice `no session to show subagents for` (§6.B E13, §58-I21).

**Key choice (`Ctrl+T`, not `Ctrl+B`).** Rev 1 chose `Ctrl+B`; it is the **tmux
prefix** and this project explicitly targets tmux/SSH (`00-architecture.md:95`).
Rev 2/3 keep `Ctrl+T` but state the rationale precisely (Rev 3; R5-N5): under the
default tmux configuration `Ctrl+B` is not unreachable — it is reachable as
`Ctrl+B Ctrl+B` — but the first press is swallowed by tmux as a prefix, so
`Ctrl+B` costs an extra keystroke and collides with the prefix that drives the
terminal class this project is built for. `Ctrl+T` is unbound (§1.6),
FTXUI-supported (`event.hpp:86`), not a tmux prefix (tmux's default prefix is
`Ctrl+B`; the common alternate is `Ctrl+A` — neither is `Ctrl+T`), and mnemonic
for the subagen**t** tree. `/subagents` remains the tmux-proof discoverable path
for the root view; inside a child view the composer is read-only, so `Ctrl+T` is
the only nesting key — hence a non-prefix key is **required**, not merely
preferred.

**Rejected alternatives (recorded, not left open).**

- *Make the strip focusable* — would require new FTXUI focus traversal for a
  single non-interactive row and would compete with the composer for `Tab`/arrows
  (F6, `00` §54 :4850). Rejected.
- *Numbered shortcuts* — `1..9` are unbound today but would be a second,
  undiscoverable namespace for one feature; the picker already carries labels.
  Rejected.
- *A bare key with no picker* (cycle through children) — cannot express "which
  child", and has no natural place to show running/finished state. Rejected.
- *Extending `Ctrl+S` itself* (children under the active session) — the Live
  switcher's Enter means "focus this session"; overloading it would conflate
  "enter a child view" with "switch the active session" (58-D2). Rejected.
- *Keep `Ctrl+B` as a second binding for tmux users* — it collides with the tmux
  prefix and costs an extra keystroke in the target environment; the binding would
  be misleading. Rejected.

**Justification.** The picker reuses the proven overlay, its viewport/clamp
(`clamp_cursor`, `src/ui/ui_model.cpp:1442-1469`), and its Esc/Ctrl+C close
semantics; `/subagents` mirrors `/sessions` discoverability via the command
registry (`CommandRegistry::builtin`, `src/ui/command_registry.cpp:152-300`), and
`Ctrl+T` gives a fast path that works under tmux. The strip gains a
discoverability **hint** only (text, not a second interaction), rendered when
subagents exist.

### 58-D2 — Entering is a **view scope**, not a session switch (nesting pinned)

**Decision.** Add `UiModel::subagent_path`, a stack of `SessionId` scoped to the
active session. `Enter` in the picker **pushes** the child id; it **never** calls
`focusSessionIn`. The active workspace/session (`activeWorkspaceId`,
`WorkspaceModel::activeSessionId`) is unchanged, so "return to the main agent" is
a pop, not a re-resume. A stack (not a single id) supports nesting: a child's own
strip lists its children (spec 55's model is recursive), so `main › child ›
grandchild` works with the same operation. `focusSessionIn`/`focusWorkspace`
clear the stack (a session/workspace switch always lands on that session's main
agent) and the supervisor then releases the dropped children's subscriptions
(§6.B E16/E22, §58-I22).

**Nesting is genuinely reachable (Rev 2 fix; R1-H1/R3-M1/R4-H1/Oracle).** With
`subagent_path` non-empty, `Ctrl+T` opens the picker **at the current level** —
its rows are `viewedSession()->subagents.agents`, i.e. the viewed child's own
children — and `Enter` pushes the next level. `Esc` (not `Ctrl+T`) is the pop
key. Worked example: main spawns A; A spawns B. `Ctrl+T` → picker lists A;
`Enter` → `path = {A}`, transcript = A; `Ctrl+T` → picker now lists **A's**
children (B); `Enter` → `path = {A, B}`, transcript = B, and screen state (e)
`main › a1b2c3d4 › c3d4e5f6` renders. There is no key that both opens and pops
(Rev 1's defect): the open key is `Ctrl+T` in every Conversation state and the
pop key is `Esc`. The strip in a child view is rendered from `viewedSession()`
(§58-D3, §6.B E31/E35) so the child's children are visible as the affordance.

**Rows come from `viewedSession()`, not `activeSession()` (Rev 2 fix;
Oracle).** `SwitcherOverlayModel::openSubagents(const UiModel&)` builds its
leaves from `viewedSession()->subagents.agents` (§6.A A7), with a null guard
returning an empty node so a just-reconciled path cannot deref (§6.B E13).
Building from `activeSession()` would list the main session's children at every
depth, making nesting incoherent.

**Why not `focusSessionIn`.** It would reparent the child as the *active session*
of the workspace, so returning would re-focus the parent and the nesting
(`main › child › grandchild`) would be lost; also the user's words are "come back
to main agent", i.e. a nested view, not a tab switch. (Verified 55-D3 **permits**
resuming a stored child — "the child survives as a stored session that can be
resumed" (`55:484-485`) — so this is a UI-scope decision, not a 55-D3
prohibition; `--resume <child>` still opens a child as the active session,
§11.1.)

### 58-D3 — The subagent view replaces the transcript pane; the composer is read-only

**Decision.** When `subagent_path` is non-empty, the transcript pane renders the
**deepest path entry's** `SessionUiState` via the unchanged `render_conversation`
(`src/ui/ui_render.cpp:406-430`) — same scroll (`ConversationScroll`,
`include/ymh/ui/ui_model.hpp:129-146`), same fold machinery (`expand_all_folds`,
`:294`), same tail-follow (`position()` `:137`; `focusPositionRelative`,
`ui_render.cpp:428`). The main agent's transcript is **not** shown side-by-side:
terminal width is the constraint (SSH-first), and the existing model is one
transcript per session.

- **Breadcrumb.** A dedicated row directly under `render_header`
  (`ui_render.cpp:1237`) shows the path and the viewed child's status:
  `↳ <root> › <8-hex> <status-glyph> · <summary>` plus a right-aligned
  `Esc return`. It is a new row, **not** the header's right slot, which is
  reserved by RB-10 (`UI_SURFACE_INVENTORY.md:19-34`). **`<root>` is the active
  session's display title** (the same value `render_header` reads from the
  active `SessionCell`, `ui_render.cpp:1243-1254`), rendered as `main` only when
  the active session has no display title — never a hardcoded `main` (Rev 2 fix;
  R1-M3: `/sessions` or `--resume` could make a child the active session, and
  §58-D7/§58-I13 now forbid that through the UI, but the breadcrumb must not lie
  if it happens). The hint string is exactly `Esc return` (one spelling
  everywhere; Rev 2 fix for R4-M5/R5-LOW-1).
- **Composer (dispatch).** Disabled (dimmed) with the hint
  `(viewing subagent <8-hex> — Ctrl+T children · Esc return)`. Rationale: the
  supervisor is not the child's parent *agent*; only the parent agent may
  `send_message` a child (55-D5,
  `docs/design/55-multi-agent-delegation-errata.md:656-678`). The UI therefore
  exposes **no** `agent.*` call on the child. **Rev 2 (R1-M4/R3-M3): the
  read-only composer is a dispatch guard, not a render decision** — `handle_input`
  gains a top-of-function branch that, while `subagent_path` is non-empty,
  consumes keys as an explicit policy (§6.B E8): the `Esc` pop returns true; all
  other keys return true (consumed) — no `submit`, no `complete`, no draft
  mutation, no `agent.*` RPC. This disables `45-D9`/`45-I20` composer Tab
  agent-cycling and `45-I3` command completion **while a child is viewed**; §11
  names both (`45-I3`/`45-I20` scope extension), and main-view behaviour is
  unchanged. The `active()`-based `handle_input` state is the **parent**
  (`activeSessionId` is unchanged, §58-I1); without the guard a keystroke typed at
  depth ≥ 1 would land in the parent's draft.
- **Composer (render; Rev 3; MED-11).** `render_input` (`ui_render.cpp:486-518`)
  currently resolves `activeWorkspace().activeSessionId()` and always draws the
  editable draft/caret; `build_ui` sets `composer = active`
  (`ui_render.cpp:1563`). §6.B E33/E35 pin both: while `subagent_path` is
  non-empty, `render_input` renders the read-only hint row and **no** draft/caret,
  and `build_ui` resolves the transcript/strip/scroll-hint from
  `viewedSession()` and passes the viewed state to `render_input`. Without this
  edit the composer would render the **child's** empty editable input (G6 false).
- **Scroll/fold keys** act on the **viewed** session, not the active one
  (`PageUp`/`PageDown`/`Ctrl+Home`/`Ctrl+End`/`Shift+Up`/`Shift+Down`/`Ctrl+O`).
  Rev 2 pins this in code: `scroll_by`/`scroll_to_top`/`scroll_to_bottom`/
  `toggle_folds` resolve `viewedSession()` instead of `active()`
  (`supervisor.cpp:2633-2675`, currently `active()`; §58-I12, §6.B E9).
- **Status line (Rev 3; R1-N5; Rev 4 MEDIUM-3).** The status bar's left segment
  is prefixed `subagent <8-hex> · running/finished` while a child is viewed; the
  aggregate counts are unchanged. §6.B E34 pins the `render_status` edit
  (`ui_render.cpp:607-660`; the left segment is built from `active->status` at
  `:653`). The prefix is computed by `subagent_status_prefix` from the **viewed
  child's** `SubagentStatus` (§6.A A8) — it is **not** policy data and does not
  read `model_.switcher.source`, so it survives a `Ctrl+S` while viewing.
- **Strip.** Rendered from `viewedSession()` (§58-I3), so a viewed child shows
  **its own** children (Rev 2 fix; R1-M6).

### 58-D4 — Returning: `Esc` pops one level; `Ctrl+T` opens at the current level

**Decision.** In `UiMode::Conversation`:

- `Ctrl+T` **opens the subagents picker at the current view level** (rows from
  `viewedSession()`), whether `subagent_path` is empty or not; `Ctrl+T` while the
  picker is open closes it **only for `SwitcherSource::Subagents`** (§6.B E3,
  Rev 3; R3-N5).
- `Esc` with a non-empty `subagent_path` **pops one level** (to the parent child,
  or to main when the path empties) and consumes the key. `Esc` with an empty
  path keeps today's hints/interrupt behaviour (`handle_input`, `:3086-3109`).
- **Esc precedence (Rev 2 correction; R5-MEDIUM-1).** Path-pop is checked
  **before** the command-hint and interrupt-arm branches
  (`supervisor.cpp:3086-3109`). Rev 1 justified this with "the composer is
  read-only, so those branches are unreachable" — that premise is **false**: the
  interrupt-arm branch gates on `model_.has_active_turn()` (`:3097,3103`), not
  the draft, and a parent turn is normally active while a child runs (the child
  was spawned during it). The correct statement is: the subagent view is a new
  sub-context in which `Esc` means "pop", so the pop branch precedes the
  interrupt-arm branch unconditionally. This **extends** verified `45-I8`
  (`docs/design/45-ui-interaction-errata.md:1649`, "Esc ... is a no-op when no
  list is visible") and `45-D5` (`:533-580`) for the subagent view; §11 names
  both. Consequence: while viewing a child with an active parent turn, `Esc`
  pops the view and does **not** arm the parent's interrupt; the parent interrupt
  remains reachable by returning to main first. A pending arm on the popped state
  is disarmed (§6.B E15).
- **`Ctrl+C` in the child view is a consumed no-op (Rev 2 fix; R1-H2/Oracle).**
  The global `Ctrl+C` (`:3429`) calls `cancelActive()` (`:495-504`), which targets
  `activeWorkspace()->activeSessionId()` — the **parent** — so Rev 1's "stop what
  I'm viewing" would cancel the parent's turn and leave the child running
  orphaned. Because interrupting a child from the UI is out of scope and
  model-facing (55-D5, §58-D8), the design **suppresses** it: while
  `subagent_path` is non-empty and no popup owns the key, `Ctrl+C` is consumed
  (`handle_event_inner` gains the guard **before** the global at `:3429`) and
  does nothing. `Esc` is the return key. (Suppressing is chosen over remapping to
  the viewed child: a UI `agent.cancel` on a child is exactly the model-facing
  control 55-D5/D8 forbid, and adding it here would reopen that decision.)
- **`Ctrl+Q` while the picker is open is a consumed no-op** (57-D4, `:531-571`).
- Popup ownership (57-D4) is untouched: `Esc`/`Ctrl+C` still close the picker
  first; the `subagent_path` guards live in `handle_input`/`handle_event_inner`
  **after** the popup guards.
- **Globals that stay live while viewing (Rev 3; R4#5/R1-LOW-H13).** `Ctrl+S`/
  `Ctrl+P` (`:3417`), `Ctrl+N` (`:3433`), and the scroll globals are processed
  **before** `handle_input`, so they are not consumed by the read-only guard.
  They remain functional and, because a session/workspace switch clears the path
  (§58-I2), they safely leave the view. §6.B E8 does **not** gate them. `58-H13`
  therefore asserts only "no `agent.*` RPC while viewing", not "no
  `session.create`" (Rev 3; R4#5).

**Key-contract table (new/changed rows in bold; Rev 2: `Ctrl+B` removed).**

| Key | Context | Action |
|---|---|---|
| **Ctrl+T** | Conversation (any path) | open the subagents picker at the current view level |
| **Ctrl+T** | Subagents picker open | close the picker |
| Esc | Live/History switcher open | close the switcher (existing; `Ctrl+T` does **not** close these — Rev 3; R3-N5) |
| **Esc** | Subagents picker open | close the picker (existing) |
| **Esc** | Conversation, `subagent_path` non-empty | pop one level (return) |
| Esc | Conversation, path empty | hints / interrupt arm (existing) |
| **Enter** | Subagents picker, child selected | enter that subagent (push + track) |
| **Enter** | Subagents picker, empty leaf | consumed no-op; picker **stays open** (§6.B E4, §58-I8) |
| **Ctrl+D** | Subagents picker | consumed no-op (**delete disabled**) |
| Ctrl+D | Live/History switcher | arm/confirm delete (existing 51-D4/57) |
| **Ctrl+C** | Subagents picker open | close the picker (existing) |
| **Ctrl+C** | Conversation, path non-empty | consumed no-op (**does not cancel the parent**) |
| **Ctrl+Q** | Subagents picker | consumed no-op (57-D4) |
| Arrow/j/k | Subagents picker | existing nav |
| **Tab** | Subagents picker | consumed no-op (§6.B E2; `45-I24` scope extension) |
| Tab | Live/History switcher | `toggleExpand` (existing) |
| **Tab/Shift+Tab** | Conversation, path non-empty | consumed by the read-only guard; composer agent-cycle suspended (§6.B E8; `45-I20` scope extension) |
| **Ctrl+N / Ctrl+S / Ctrl+P** | Conversation, path non-empty | global, unchanged; clears the path via §58-I2 (Rev 3; R4#5) |
| **PageUp/Down, Ctrl+Home/End, Shift+Up/Down, Ctrl+O** | Conversation | act on the **viewed** session |

No collision: `Ctrl+T` is unbound (§1.6); `Esc`/`Ctrl+C` remain the popup close
keys; the global exit (`Ctrl+Q`), delete (`Ctrl+D` in the Live/History switcher),
fold (`Ctrl+O`), and switcher (`Ctrl+S`/`Ctrl+P`) bindings are unchanged for
their existing sources. The harness key seam (`supervisor.cpp:3815-3873`) gains a
`"ctrl-t"` mapping (`Event::CtrlT`); it never needed `"ctrl-b"` (§6.B E39).

### 58-D5 — Picker/strip source: the parent's own event log (spawn **and** fan-in)

**Decision.** Map the existing core event `EventType::SubagentSpawned`
(`include/ymh/core/event.hpp:71`) to a **new** UI event `SubagentSpawned` so a
child appears in the strip/picker at spawn with status `Running`; extend
`SubagentUpdated` and `SubagentView` with a `SubagentStatus` so running vs
finished is unambiguous. The parent's event log is authoritative and is fully
replayed on subscribe/resume (`SupervisorConnection::subscribe_one` falls back to
`from = Beginning`, `src/ui/supervisor_connection.cpp:256-318`), so the strip and
picker are correct for a resumed parent and for children spawned before this
feature shipped (the spawn event is already appended today,
`subagent_service.cpp:371-380`). **No new RPC is needed for the list.** A child's
**own** state accumulates its own children the same way once it is entered and
subscribed (its log contains its own `SubagentSpawned` edges), which is what
makes the nesting picker (§58-D2) work.

### 58-D6 — Conversation source: the child's own event log, via the existing per-session subscription

**Decision.** Entering a child materializes a normal `SessionUiState` for the
child and subscribes the supervisor to the child session with the **existing**
mechanism: `SupervisorConnection::track(childId)`
(`src/ui/supervisor_connection.cpp:64-72`) then `event.subscribe` from
`Beginning` (`subscribe_one`, `:256-318`; RPC `kEventSubscribe`,
`include/ymh/transport/protocol.hpp:534`, served at
`src/transport/protocol_server.cpp:524-525,574`). `UiEventAdapter::onSessionEnvelope`
routes the child's events into the child's state by `envelope.session`
(`src/ui/ui_event_adapter.cpp:279-303`), exactly as for any session. Details and
the completed-child proof are in §7. On pop the child is **untracked and its
subscription released** (§58-I18) and its state erased (§58-I19); re-entry
re-subscribes from `Beginning`.

**Tracking rule (single, consistent — Rev 3; R1-N2/R2-N3/MED-12).** Exactly the
**entered** children are tracked, and this is enforced on all three creation
paths:

1. `refresh_sessions` skips `kind == "subagent"` entries **before** the `track()`
   loop and **before** the cell loop (`supervisor.cpp:1694-1718`), so a resident
   child is neither auto-tracked nor given a `SessionCell` (§6.B E18). This
   supersedes Rev 2's ambiguous "skip when creating cells".
2. `enter_subagent` tracks the child explicitly and records its `SubscriptionId`
   (§6.B E14/E28).
3. `SessionOpened` (adapted from `HostNoticeKind::SessionCreated`,
   `ui_event_adapter.cpp:366-371`) creates a `SessionUiState` but **no cell**
   (`ensureSessionState`, §6.B E25), so no `SessionOpened` can leak a cell for a
   child — a child spawned in-process emits no `SessionCreated` at all
   (`agent_registry.cpp:149`; E25); and the adapter's `onSessionEnvelope` guard
   (`ui_event_adapter.cpp:299-301`) finds the state `enter_subagent` already
   materialized, so it too creates no cell.

A path-shrinking event (pop, switch, close, death) releases the dropped
children's subscriptions via `sync_subagent_subscriptions()` (§6.B E16/E22/E36/E37).

### 58-D7 — A viewed child is never a `SessionCell`, never in the Live switcher, and never a `/sessions` row

**Decision.** A child `SessionUiState` is materialized **without** a
`SessionCell` by a new `UiModel::ensureSubagentState`, which (a) inserts the
`SessionUiState` with `subagent == true` (and its `workspace`) and (b) erases any
existing `SessionCell` for that id. `enter_subagent` calls it **before** `track`,
so when the child's first envelope arrives the adapter's guard
`if (model_.sessions.find(envelope.session) == model_.sessions.end())`
(`src/ui/ui_event_adapter.cpp:299-301`) is already false and no cell is created.
`refresh_sessions` additionally skips `kind == "subagent"` entries before
**both** `track()` and cell creation (`src/ui/supervisor.cpp:1694-1718`), and
`apply(SessionOpened)` creates state without a cell (§6.B E18/E25). This prevents
the new view from adding children to the Live switcher
(`SwitcherOverlayModel::open`, `src/ui/ui_model.cpp:1263-1340`) and also fixes
the pre-existing leak of **resident** children into the Live switcher.

**Rev 2: the History (`/sessions`) half is now implemented and named (R5-HIGH-1 /
Oracle).** Rev 1's `58-I13` claimed a child "never appears ... as a `/sessions`
top-level selection", but `58-D7` filtered only the **Live** path. The History
source reads the disk catalog directly (`openHistory`,
`src/ui/ui_model.cpp:1342-1427`; `read_workspace_history`,
`src/ui/session_catalog.cpp:136-144`), which keeps `SessionKind::Subagent` rows,
and verified `22-D6` (`docs/design/22-switcher-sessions-errata.md:702-720`) pins
`/sessions` to *every* row from `list()`. Rev 2 therefore:

- **Filters subagent rows from the History builder.** `SwitcherOverlayModel::openHistory`
  skips entries whose `kind == "subagent"` (`SessionHistoryEntry::kind` is
  `session_kind_name`, `src/ui/session_catalog.cpp:62-73`) — the same place the
  focused-session exclusion already lives (`ui_model.cpp:1385-1390`; §6.B E19).
  The filter is applied **only** to the `/sessions` overlay; the shared reader
  stays unfiltered so the `--resume` resolver is unaffected
  (`include/ymh/ui/session_catalog.hpp:69-80`).
- **Refuses to focus a child as a top-level session (defense in depth).**
  `select_history` looks up the target `SessionNode` in
  `model_.switcher.workspaces` (the History node's `kind` is set at
  `ui_model.cpp:1398`); if `node.kind == "subagent"` it pushes the notice
  `subagent sessions are viewed from their parent (/subagents)` and does **not**
  call `resume_from_history` (§6.B E20). This guards a stale History snapshot
  that still carries a subagent row after the E19 filter; it is a UI-scope guard
  (58-D7), **not** a 55-D3 prohibition — 55-D3 permits resuming a stored child
  (`55:484-485`), and `--resume <child>` remains unfiltered (§11.1).
- **Makes the Live leaf predicate kind-aware.** `catalog_has_session`
  (`src/ui/ui_model.cpp:692-707`) skips `entry.kind == "subagent"`, so a child
  fails the Live predicate even if a stray `SessionCell` existed (§6.B E27).
  This is the precise amendment of verified `45-I5` (`45:1646`); §11 names it.
- **Explicitly amends `22-D6`** in §11, with the reason and anchors.

`22-D3` (`/sessions` reuses the overlay) and `45-I6` (Live ⊆ History) are not
contradicted: children are in neither source, so the subset relation over roots
holds.

### 58-D8 — Edges (all decided)

| Situation | Behaviour |
|---|---|
| Child finishes while viewed | Its `SubagentStatus` becomes terminal via the parent's `SubagentFanIn`; the breadcrumb/status line change to `✓`/`✗`/`-` + `finished`; the view **stays** (the user may keep reading). No forced exit. |
| Child deleted / closed / evicted while viewed | `SessionClosed` notice (`src/ui/supervisor.cpp:856-866`) or a `session.delete` reply for a path id → `reconcile_subagent_path()` pops that id and everything deeper, untracks/releases each (§58-I18), erases its state (§58-I19), and pushes the status notice `subagent session closed — returned to main`. |
| Workspace daemon dies while viewed | **Rev 3 (R1-N1/R3-N3/R4#4/R5-N3/Oracle):** `WorkspaceEventKind::DaemonDied` is **not emitted in production** (`ui_event_adapter.cpp:366-385` maps Opened/Closed/Stopping only) and `UiModel::apply(DaemonDied)` only flips liveness (`ui_model.cpp:1187-1190`). The path is therefore cleared by the **real** hook, the presence-scan eviction: `evict_dead_workspaces` calls `reconcile_subagent_path()` immediately after `model_.eraseWorkspace(id)` (`supervisor.cpp:1561-1589`, erase at `:1584`) (§6.B E37). `on_link_state(Dead)` also calls `reconcile_subagent_path()` (E36), but it is **inert**: the link transition does not erase the child state, so the reconcile pops nothing and the path is cleared by the **eviction** (`E37`), **not** by the link transition (Rev 4/Rev 6; Oracle NEW-5). The stale-state window is bounded by the presence scan. `UiModel::apply(DaemonDied)` stays liveness-only; the design does **not** assert it clears the path. |
| No active/viewed session (`50-I7` lazy create) | `Ctrl+T`/`/subagents` push the notice `no session to show subagents for` and open nothing (never deref `nullptr`) — §6.B E13, §58-I21. |
| No subagents (session exists, zero children) | `/subagents`/`Ctrl+T` opens the picker with the empty-state leaf `(no subagents)`; no crash, no notice. `Enter` on the empty leaf is a consumed no-op and the picker **stays open** (§6.B E4). |
| Many subagents | The picker reuses the switcher viewport and scrolls; the strip stays one line (may clip — the picker is authoritative). No extra subscription is opened until a child is entered. |
| Stale picker cursor | `SwitcherOverlayModel::clamp_cursor` (`src/ui/ui_model.cpp:1442-1469`) after every resnapshot. |
| New child spawns while viewing | The parent's (or viewed child's) `SubagentSpawned` updates that level's strip; the open picker resnapshots; the current view is unaffected. |
| Nested child | `Enter` pushes; `Esc` pops one level; `Ctrl+T` re-opens at the new level. The path is a simple path in the durable parent→child tree: each push targets an existing direct child, and a session has exactly one `parentSession` (`include/ymh/session/session.hpp:57-58`), so the path cannot cycle; its depth is bounded by the stored tree height (the daemon's `presets.max_depth` admission, 55-D7, bounds newly spawned chains). The UI adds no cap (§58-I16). |
| Child log unreadable / unknown | `event.subscribe` returns `UnknownSession`; `subscribe_one` now reports it through `SupervisorSink::on_subscribe_error` (`src/ui/supervisor_connection.cpp:280-288`), and the supervisor reconciles the path and pushes the notice `subagent unavailable: <reason>` (the existing status-bar notice ring, `ui_render.cpp:617`). Return keys still work. A `SubscriptionLimit` reply is reported the same way. |
| Child running and streaming | Tail-follow is on at entry (`scroll.following = true`); `PageUp` un-follows and raises the existing unseen marker (`onNewContent`, `src/ui/ui_model.cpp:517-521,1166-1172`). |
| Interrupting a child from the UI | **Out of scope.** 55-D5 makes `interrupt_agent` a model-facing tool; the UI does not call `agent.cancel` on a child. `Esc` pops the view and `Ctrl+C` is suppressed (§58-D4). Recorded, not left open. |
| Ctrl+D in the picker | **Disabled for the Subagents source.** `handle_switcher` gates the `Ctrl+D` branch on the source policy; for `Subagents` it is a consumed no-op. The Live/History delete contract (51-D4/57) is unchanged (§58-I17). |
| Ctrl+C in the child view | **Suppressed** (consumed no-op); it never cancels the parent (§58-D4). |

### 58-D9 — Widget reuse vs a dedicated overlay (decision; Oracle question 1)

**Decision: keep `SwitcherOverlayModel`/`render_switcher`/`handle_switcher` and
make every source divergence the shared widget/handler reads a row in one
`SwitcherSourcePolicy` table (§6.A).** Rev 2 claimed "exactly **three** source
branches", but the real divergences are nine (window title, heading, footer text,
empty-state string, Enter action, Ctrl+D action, Tab action, `Ctrl+T`-close,
History `r`-refresh) and already exceeded Rev 2's own "fourth divergence"
escalation
trigger. Rev 4 implements Oracle's "split the policy, keep the widget"
recommendation: the **mechanism** (viewport, `clamp_cursor`, filter, scroll,
frame, Esc/Ctrl+C close) stays shared in the widget, and the **content/policy**
becomes data (`kSwitcherPolicy`, §6.A) consulted by its **six pinned consumers**
— E1 (`ctrl_d_enabled`), E2 (`tab_expands`), E3 (`ctrl_t_closes`), E4 (`enter`),
E5 (`window_title`/`heading`/`footer`/`empty_state`), E32 (`r_refreshes`). This
spec introduces **no new `source ==` branch** and removes the four it owns (E4's
`Subagents` test `supervisor.cpp:2805`, E32's History `r`-refresh `:2796`, and E5's
two renderer `history` tests `ui_render.cpp:960`/`:1148`); the six remaining
pre-existing comparisons are rebuild/eligibility mechanics enumerated in §6.A
A3.2 and unchanged (§58-I23). Two Rev 3 rows are **removed because they were dead
fields**, not divergences the widget
reads: `row_source` (rows are built by the three explicit builders
`open`/`openHistory`/`openSubagents`, A3/A7) and `status_prefix` (the status line
is not the switcher; E34 keys it on the viewed child, A8/MEDIUM-3).

**Rationale.** 22-D3 ("extend, do not duplicate") is the codebase rule; the
overlay's viewport, `clamp_cursor`, filter, scroll, Esc/Ctrl+C close, and the
`render_switcher` frame are reused wholesale. A dedicated overlay model would
duplicate ~150–250 lines (view/clamp/render + goldens) and would still need the
same Enter/delete policy, so it buys nothing while introducing a second key
owner (F6). Each divergence is now a single testable table cell. This matches the
Oracle's recommendation. **Escalation rule:** if a future source needs a
divergence the policy table cannot express as data (a new *mechanism*, not new
content), split into a dedicated overlay; recorded so the decision is not
re-litigated each round.

**Cost if split (recorded for the next round).** A dedicated `SubagentPickerModel`
would need its own `open/moveDown/moveUp/clamp/close`, its own renderer and
goldens, and would not inherit the switcher's 57-D4 popup ownership for free; the
net line count is positive and the risk (two overlays drifting on Esc/Ctrl+C) is
higher. Rejected for now.

## 3. Screen states (ASCII)

**(a) Main agent with a live strip + hint.**
```
┌──────────────────────────────────────────────────────────────┐
│ ymh · /home/yury/proj                                         │
├──────────────────────────────────────────────────────────────┤
│ user: profile the parser hot path                             │
│ assistant: spawning a subagent…                               │
│ ...                                                     ⟳ tail│
├──────────────────────────────────────────────────────────────┤
│ subagents: [a1b2c3d4 >] profile parser                        │
├──────────────────────────────────────────────────────────────┤
│ > _                                                           │
├──────────────────────────────────────────────────────────────┤
│ main · running                            Ctrl+T subagents    │
└──────────────────────────────────────────────────────────────┘
```

**(b) Subagents picker (opened at the current level).** The window title is
`subagents` and the bold heading row is also `subagents`, exactly as the History
source renders `sessions` as both window title and heading (Rev 3; R1-M2 — the
sketch now shows the pinned heading).
```
        ┌─ subagents ───────────────────┐
        │ subagents                     │
        │ ymh · /home/yury/proj         │
        │   a1b2c3d4 > profile parser   │
        │   f0e1d2c3 ✓ lint cleanup     │
        │                               │
        │ Enter enter · Esc close       │
        └───────────────────────────────┘
```

**(c) Subagent view, child running (transcript replaced).**
```
┌──────────────────────────────────────────────────────────────┐
│ ymh · /home/yury/proj                                         │
│ ↳ main › a1b2c3d4 > · profile parser            Esc return    │
├──────────────────────────────────────────────────────────────┤
│ user: profile the parser hot path                             │
│ assistant: running the profiler…                              │
│   ▸ shell  {"cmd":"perf record …"}                            │
│ ...                                                     ⟳ tail│
├──────────────────────────────────────────────────────────────┤
│ (viewing subagent a1b2c3d4 — Ctrl+T children · Esc return)    │
├──────────────────────────────────────────────────────────────┤
│ subagent a1b2c3d4 · running              0 active · 0 waiting │
└──────────────────────────────────────────────────────────────┘
```

**(d) Child finished while viewed.**
```
│ ↳ main › a1b2c3d4 ✓ · profile parser            Esc return    │
│ ...                                                           │
│ subagent a1b2c3d4 · finished                                  │
```

**(e) Nested view (`Ctrl+T` at depth 1 lists the child's children).**
```
│ ↳ main › a1b2c3d4 > › c3d4e5f6 > · grep logs    Esc return    │
```

**(f) Empty picker.**
```
        ┌─ subagents ────────────────┐
        │ subagents                  │
        │ (no subagents)             │
        │ Esc close                  │
        └────────────────────────────┘
```

**(g) Child deleted while viewed (status-bar notice ring).**
```
│ subagent session closed — returned to main                    │
```
This is the notice ring segment of the status bar (`ui_render.cpp:617`), **not**
a modal overlay and **not** a bare transcript row (Rev 2 fix; R1-L3).

## 4. Accessibility of long / streaming content

The child view reuses the main transcript's machinery unchanged: `yframe` +
`vscroll_indicator` + `focusPositionRelative` (`ui_render.cpp:427-429`), the
per-session `ConversationScroll` (`ui_model.hpp:129-146`), the **per-session**
fold flag `SessionUiState::expand_all_folds` (`ui_model.hpp:294`; Rev 2 corrects
§4's earlier "global fold flag" wording — the flag is per-`SessionUiState`, so
the viewed child folds independently), and the shipped UI output caps
(`ui_model.cpp:70-71,1001,1005,1022-1024`). No new buffer, no new cap. A running
child's deltas reach the child's `SessionUiState` through the same event path as
any session; the unseen marker is raised when content arrives while scrolled up.

## 5. Not in scope (recorded)

- Side-by-side / split transcript panes.
- Mouse interaction with the strip or breadcrumb.
- Prompting or interrupting a child from the supervisor UI (55-D5 keeps those
  model-facing; `Ctrl+C` is suppressed, §58-D4).
- Cross-daemon children (impossible: a child lives in its parent's daemon,
  55-D3).

---

# Part B — Implementation design

## 6. C++ interface sketches and the normative edit list

**Normative rule (Rev 4; P1/P2).** §6 is the **single source of truth** for every
code change **and every new symbol**. It has three normative parts:

- **§6.A — New symbols (normative).** Every symbol this spec introduces, with its
  **declaration**, a **body sketch**, and its **callers**. Nothing referenced
  anywhere in this spec is defined anywhere else: if a name appears in a D-decision,
  an invariant, a failure mode, a test, or §11/§14 and is not pre-existing code
  cited by `file:line`, it is declared here.
- **§6.B — Pinned edits to existing code (exhaustive, normative).** The numbered
  `E1`–`E47` list: each is `file · symbol · change` with a verified anchor.
- **§6.C — State-lifetime table.** One row per piece of state, one column per
  transition (enter / pop / switch / close / close-delete-external / daemon death).
  The Rev 3
  HIGH-1 defect (a child re-entered **blank**) was a state-lifetime defect; this
  table is the mechanism that prevents a recurrence.

§2/§11/§14 may cite **only** §6.A/§6.B/§6.C; a mechanism not listed in §6 is not
part of the design.

### 6.A New symbols (normative)

Every symbol below is new (or an amendment to a pre-existing declaration, marked
**amended**). `file:line` anchors point at the pre-existing code the symbol is
built on; a bare `:N` is the same file as the heading.

#### A1 · UI events and enums — `include/ymh/ui/ui_event.hpp`

```cpp
// 58-D5. Four terminal/live states, distinct from AgentState.
enum class SubagentStatus : std::uint8_t { Running, Completed, Failed, Cancelled };

struct SubagentSpawned {                 // 58-D5 (NEW UI event)
    SessionId   session;                 // parent
    SessionId   subagent;                // child
    std::string task;                    // -> SubagentView::summary
};

struct SubagentUpdated {                 // 58-D5 (amended: +status)
    SessionId      session;              // parent
    SessionId      subagent;             // child
    std::string    summary;
    AgentState     state = AgentState::Idle;
    SubagentStatus status = SubagentStatus::Completed;   // from SubagentOutcome
};
```

The `UiEvent` variant (`ui_event.hpp:226-251`) gains `SubagentSpawned`.
**Callers:** produced by `UiEventAdapter::adapt` (E30); consumed by
`UiModel::apply` (E24). `AgentState` stays the live-process state
(`include/ymh/agent/agent.hpp:31-39`); `SubagentStatus` is the durable
spawn→fan-in edge state, and the two are rendered by different glyphs (A8).

#### A2 · `SubagentView` — `include/ymh/ui/ui_model.hpp:264-268` (amended)

```cpp
struct SubagentView {
    SessionId      id;                    // the child's SessionId
    std::string    summary;
    AgentState     state = AgentState::Idle;
    SubagentStatus status = SubagentStatus::Running;     // NEW
};
```

`SubagentModel{std::vector<SubagentView> agents}` (`:270-272`) and
`SessionUiState::subagents` (`:283`) are unchanged. **Callers:** `apply`
(SubagentSpawned/Updated, E24), `render_subagents` (E31), `viewed_status` (A8),
`SwitcherOverlayModel::openSubagents` (A7).

#### A3 · `SwitcherSource`, `SwitcherEnter`, `SwitcherSourcePolicy` — `include/ymh/ui/ui_model.hpp`

```cpp
enum class SwitcherSource : std::uint8_t { Live, History, Subagents };   // amended

enum class SwitcherEnter : std::uint8_t { Focus, Resume, EnterChild };

// 58-D9/P2: the per-source policy. Data, not branches.
struct SwitcherSourcePolicy {
    std::string_view window_title;
    std::string_view heading;
    std::string_view footer;         // History carries " · r refresh"
    std::string_view empty_state;    // History overrides while loading
    SwitcherEnter    enter;          // consumed by E4
    bool             ctrl_d_enabled; // false for Subagents
    bool             tab_expands;    // false for Subagents
    bool             ctrl_t_closes;  // true only for Subagents
    bool             r_refreshes;    // true only for History (E32)
};
[[nodiscard]] const SwitcherSourcePolicy& switcher_policy(SwitcherSource) noexcept;
```

```cpp
const SwitcherSourcePolicy& switcher_policy(SwitcherSource source) noexcept {
    return kSwitcherPolicy[static_cast<std::size_t>(source)];
}
```

`kSwitcherPolicy[3]` is defined by the table in §6.A. **Consumers (exhaustive):**
E1 (`ctrl_d_enabled`), E2 (`tab_expands`), E3 (`ctrl_t_closes`), E4 (`enter`),
E5 (`window_title`/`heading`/`footer`/`empty_state`), E32 (`r_refreshes`). No
other code reads the policy; this spec introduces **no new `source ==` branch**
and removes the four it owns — E4 (`supervisor.cpp:2805`), E32 (`:2796`), and E5
(`ui_render.cpp:960`, `:1148`) — and the six remaining pre-existing comparisons
are enumerated in A3.2 and unchanged (§58-I23).

**Removed from Rev 3's struct (recorded).** `SwitcherRowSource row_source` and
`std::string_view status_prefix` are deleted, not merely unreferenced.
*`row_source`:* rows are built by three explicit builders — `open` (Live,
`ui_model.cpp:1263`), `openHistory` (History, `:1342`), `openSubagents`
(Subagents, A7) — each called from its own open path; encoding the choice as data
with no reader was a dead field (Rev 3 review). *`status_prefix`:* the status
line is **not** the switcher; Rev 3's `render_status` read it via
`model_.switcher.source`, so `Ctrl+S` while viewing silently dropped the prefix
(MEDIUM-3). It is replaced by `subagent_status_prefix` (A8), keyed on the
**viewed child's** status, not on the picker's source.

#### A3.1 · The `SwitcherSourcePolicy` table (P2)

`kSwitcherPolicy[3]` (indexed by `SwitcherSource`) is the **only** place the nine
per-source **presentation/action divergences** live. `render_switcher`/
`switcher_disarmed_rows` and `handle_switcher` consult it; this spec introduces
**no new `source ==` comparison** and removes the four it owns — E4
(`supervisor.cpp:2805`), E32 (`:2796`), E5 (`ui_render.cpp:960`, `:1148`). The
remaining pre-existing `source ==` comparisons are enumerated in A3.2 — they are
rebuild/eligibility mechanics, not divergences, and are unchanged (§58-I23).
Row construction is **not** a policy row: it is performed by the three explicit
builders `open`/`openHistory`/`openSubagents` (A3, A7).

| field | Live | History | Subagents |
|---|---|---|---|
| `window_title` | `workspaces` | `sessions` | `subagents` |
| `heading` | `Switcher` | `sessions` | `subagents` |
| `footer` | `j/k move · Tab expand · Ctrl+D delete · Enter focus · Esc close` | `stored sessions` (+ ` · partial` + ` · r refresh · Ctrl+D delete · Enter resume · Esc close`) | `Enter enter · Esc close` |
| `empty_state` | `(no workspaces)` | `(no stored sessions)` (or `loading stored sessions…`) | `(no subagents)` |
| `enter` | `Focus` (`focusSessionIn`/`focusWorkspace`) | `Resume` (`select_history`) | `EnterChild` (`enter_subagent`; no-op on empty leaf) |
| `ctrl_d_enabled` | `true` | `true` | `false` |
| `tab_expands` | `true` | `true` | `false` |
| `ctrl_t_closes` | `false` | `false` | `true` |
| `r_refreshes` | `false` | `true` | `false` |

The Live `heading` is `Switcher` and its window title `workspaces` (the shipped
values, `ui_render.cpp:1151,1180`); History and Subagents reuse one string for
both (the shipped History behaviour). Empty rows omit the heading when
`heading.empty()` (never today). Every field is consumed (A3); `footer` and
`empty_state` are consumed by E5, `enter` by E4, and the four bools by
E1/E2/E3/E32.

#### A3.2 · Pre-existing `source ==` comparisons (enumerated; unchanged)

58-I23 is exact only because the following comparisons are enumerated. None is a
per-source **presentation/action** divergence; none is introduced or modified by
this spec; all are rebuild/eligibility mechanics.

- `supervisor.cpp:956/959` — the catalog callback rebuilds an open History
  switcher (`openHistory`) or Live switcher (`resnapshot_switcher`). Subagents is
  deliberately **not** rebuilt (its rows come from `viewedSession()`, A7).
- `supervisor.cpp:1236` — `resnapshot_switcher()` rebuilds only a Live switcher.
- `supervisor.cpp:2955` — `delete_highlighted_workspace` refuses a Live-source
  workspace delete (51-D4.10). Subagents never reaches it (E1 disables `Ctrl+D`).
- `supervisor.cpp:3014` — `refresh_after_delete` re-reads the History catalog,
  else resnapshots.
- `ui_model.cpp:875` — `eraseWorkspace` resnapshots only a Live switcher.

The policy table (A3.1) is the single source for the nine divergences; these six
comparisons (five bullets — `:956`/`:959` are two) are rebuild/eligibility
mechanics and are out of its scope. They are listed here so no mechanism is
asserted without a §6 anchor.

#### A4 · `UiModel` — `subagent_path`, viewers, materializers, `disarm`

`include/ymh/ui/ui_model.hpp` (`UiModel` public API; `sessions` is
`std::map<SessionId, SessionUiState>` at `:583`, `activeWorkspaceId` at `:578`):

```cpp
// 58-D2: the active session's subagent focus path (outermost first).
// Empty == main agent. Scoped to the active workspace/session.
std::vector<SessionId> subagent_path;

// 58-D2: the deepest path entry's state, or the active session when the path is
// empty. Returns nullptr (NEVER default-constructs) when the deepest id has no
// materialized state; callers reconcile first (58-I14).
[[nodiscard]] SessionUiState*       viewedSession();
[[nodiscard]] const SessionUiState* viewedSession() const;

// 58-D7: materialize a SessionUiState (subagent == true) and erase any existing
// SessionCell for it; called BEFORE track so the adapter's ensureSessionIn guard
// never creates a cell.
SessionUiState& ensureSubagentState(const WorkspaceId&, const SessionId&);

// 58-D7: materialize a SessionUiState WITHOUT a SessionCell (used by
// apply(SessionOpened) so a SessionCreated notice cannot leak a cell).
SessionUiState& ensureSessionState(const WorkspaceId&, const SessionId&);

// 58-D4/MEDIUM-4: clear the per-session Esc arm of a child leaving the view.
void disarm(const SessionId& id);
```

```cpp
SessionUiState* UiModel::viewedSession() {
    if (subagent_path.empty()) return activeSession();
    return session(subagent_path.back());            // nullptr if absent
}
const SessionUiState* UiModel::viewedSession() const {
    if (subagent_path.empty()) {
        const auto ws = workspaces.find(activeWorkspaceId);
        return ws == workspaces.end() ? nullptr : session(ws->second.activeSessionId());
    }
    return session(subagent_path.back());
}

SessionUiState& UiModel::ensureSessionState(const WorkspaceId& ws, const SessionId& id) {
    SessionUiState& state = sessions[id];
    if (state.id.value.empty())      state.id = id;
    if (state.workspace.value.empty()) state.workspace = ws;
    return state;                                    // NO ensureCellIn
}

SessionUiState& UiModel::ensureSubagentState(const WorkspaceId& ws, const SessionId& id) {
    SessionUiState& state = ensureSessionState(ws, id);
    state.subagent = true;
    const auto wit = workspaces.find(ws);
    if (wit != workspaces.end()) {                   // 58-D7/I13: drop any cell
        auto& cells = wit->second.sessions;
        cells.erase(std::remove_if(cells.begin(), cells.end(),
                                   [&id](const SessionCell& c) { return c.id == id; }),
                    cells.end());
    }
    return state;
}

void UiModel::disarm(const SessionId& id) {
    const auto it = sessions.find(id);
    if (it == sessions.end()) return;                // idempotent no-op
    it->second.esc_arm = EscArm::Disarmed;
    it->second.esc_armed_at.reset();
    dirty.mark(id, UiDirtyFlag::Input);
}
```

`SessionUiState::subagent` (bool, default `false`) and `esc_arm`
(`ui_model.hpp:300-301`) are **amended** declarations. **Callers:**
`enter_subagent` (A5) calls `ensureSubagentState`; `apply(SessionOpened)` (E25)
calls `ensureSessionState`; `viewedSession()` is called by E9/E13/E21/E33/E34/E35;
`disarm` is called by `sync_subagent_subscriptions` (A5).

#### A5 · `SupervisorApp` view-scope methods — `src/ui/supervisor.cpp`

`SupervisorApp` is a private class in `src/ui/supervisor.cpp`; the methods below
are private. **`ws` (MEDIUM-6):** the workspace a viewed child belongs to is the
active workspace; each method that needs it binds it once as
`const WorkspaceId ws = model_.activeWorkspaceId;` (`ui_model.hpp:578`). A child
lives in its parent's daemon (55-D3), so there is no second workspace to resolve.
`sync_subagent_subscriptions` records each tracked child's owning workspace at
track time (`viewed_children_` is a `map<SessionId, WorkspaceId>`), so a dropped
child is untracked/erased against the workspace it was materialized in even after
an eviction has removed its state.

```cpp
// member (58-I22/MEDIUM-1; Rev 6 R3-M1): the children the view currently tracks,
// each mapped to the workspace it was materialized in. The single source of truth
// for "tracked for the view"; maintained only by sync. The workspace is recorded
// at track time, NOT derived from model_.sessions at drop time: on the E37
// eviction path `eraseWorkspace` has already erased the child's state (and
// promoted activeWorkspaceId to another live workspace, ui_model.cpp:861-873)
// before sync runs, so a derived lookup would resolve `connection_for` to the
// WRONG connection and untrack there.
std::map<SessionId, WorkspaceId> viewed_children_;

void            open_subagents();                    // E13
void            enter_subagent(const SessionId& id);
void            return_subagent();
void            reconcile_subagent_path();
void            reconcile_subagent_path_for(const SessionId& id,
                                            const std::string& reason);
void            sync_subagent_subscriptions();
SessionUiState* viewed();

// NEW (Rev 5; HIGH): the ONLY way §6.A touches `connections_`. Resolves the
// workspace's live connection, or nullptr when it has none (never
// `connections_.at`). Null-safe by construction: `sync_subagent_subscriptions`
// is reachable *after* `evict_dead_workspaces` has erased the connection (E37),
// so a missing connection must make the subscription calls no-ops, not throw.
SupervisorConnection* connection_for(const WorkspaceId& ws);
```

```cpp
SessionUiState* SupervisorApp::viewed() { return model_.viewedSession(); }

// Rev 5 (HIGH): the only connection resolver. `connections_` is
// `std::map<WorkspaceId, std::unique_ptr<SupervisorConnection>>`
// (`supervisor.cpp:3547`), not a single member, so a bare `connection` cannot
// compile. A missing entry is normal, not an error: E37 runs sync *after*
// `evict_dead_workspaces` (`supervisor.cpp:1579`) has erased the connection, and
// `on_scan` (`:1538`) can erase it under a boot-id change. Returning nullptr
// makes the subscription calls below no-ops while the model/adapter cleanup
// still runs — see the E37 ordering note.
SupervisorConnection* SupervisorApp::connection_for(const WorkspaceId& ws) {
    const auto it = connections_.find(ws);
    return it == connections_.end() ? nullptr : it->second.get();
}

// 58-I22/MEDIUM-1: the ONLY place that tracks/untracks view children. Derives the
// tracked map from subagent_path; untracks + forgets + erases every child that
// left it, and tracks every child that entered it. Idempotent, and safe on a
// workspace whose connection is already gone (E37/58-I18).
void SupervisorApp::sync_subagent_subscriptions() {
    const std::set<SessionId> keep(model_.subagent_path.begin(), model_.subagent_path.end());
    for (auto it = viewed_children_.begin(); it != viewed_children_.end();) {
        const SessionId id = it->first;
        if (keep.count(id) == 0) {
            const WorkspaceId child_ws = it->second;  // recorded at track time (Rev 6)
            model_.disarm(id);                       // clear the popped child's Esc arm
            if (SupervisorConnection* c = connection_for(child_ws); c != nullptr) {
                c->untrack(id);                      // sends event.unsubscribe (E28)
            }
            adapter_.forget_session(id);             // HIGH-1: drop its replay ids
            model_.eraseSession(child_ws, id);       // erase its SessionUiState (I19)
            model_.dirty.markAggregate();
            it = viewed_children_.erase(it);
        } else {
            ++it;
        }
    }
    for (const SessionId& id : model_.subagent_path) {
        const WorkspaceId child_ws = model_.activeWorkspaceId;
        viewed_children_.insert_or_assign(id, child_ws);  // record for the drop diff
        if (SupervisorConnection* c = connection_for(child_ws); c != nullptr) {
            c->track(id);                        // subscribe; `track` dedups `tracked_`
        }
    }
}

void SupervisorApp::enter_subagent(const SessionId& id) {
    const WorkspaceId ws = model_.activeWorkspaceId;
    SessionUiState& state = model_.ensureSubagentState(ws, id);
    state.subagent = true;
    model_.subagent_path.push_back(id);
    model_.dirty.markAggregate();
    sync_subagent_subscriptions();                   // tracks id, inserts viewed_children_
}

// 58-I25: pop BEFORE any erase. sync does the untrack/forget/erase for the pop.
void SupervisorApp::return_subagent() {
    if (model_.subagent_path.empty()) return;
    model_.subagent_path.pop_back();
    model_.dirty.markAggregate();
    sync_subagent_subscriptions();
}

// 58-D8: pop every id whose state is gone (closed/deleted/evicted), then release.
void SupervisorApp::reconcile_subagent_path() {
    while (!model_.subagent_path.empty() &&
           model_.session(model_.subagent_path.back()) == nullptr) {
        model_.subagent_path.pop_back();
    }
    model_.dirty.markAggregate();
    sync_subagent_subscriptions();
}

void SupervisorApp::reconcile_subagent_path_for(const SessionId& id,
                                                const std::string& reason) {
    if (std::find(model_.subagent_path.begin(), model_.subagent_path.end(), id) ==
        model_.subagent_path.end()) {
        return;
    }
    reconcile_subagent_path();                       // apply(SessionClosed) already erased it
    push_notice("subagent session " + reason + " — returned to main");
}
```

`open_subagents` (E13) is defined with its E entry. **Callers of
`sync_subagent_subscriptions()` (pinned; Rev 4; Oracle NEW-3; Rev 6 R1-L3):**
`enter_subagent` and `return_subagent` (above); `reconcile_subagent_path` (above),
which is the sole sync for the erase/eviction call sites — E36 `on_link_state(Dead)`
(`:1624`), E37 `evict_dead_workspaces` (`:1584`), and E42/E43/E44/E45 all call
`reconcile_subagent_path()` only (the redundant direct syncs were removed in
Rev 6); and E22's `focusWorkspace`/`focusSessionIn`/`focusSession` call sites
(`supervisor.cpp:1463`, `:1478`, `:1667`, `:2762`, `:2808`, `:2810`), which call
`sync` directly. No other call site.

**E37 ordering (Rev 5; HIGH; mechanism corrected Rev 6).** E37 calls `sync`
**after** `model_.eraseWorkspace(id)` (`:1584`), and that order is **required**:
`reconcile_subagent_path()` pops a path id only when `model_.session(id)` is
already `nullptr` (A5), which only `eraseWorkspace` produces.
`evict_dead_workspaces` erases the connection at `:1579`, *before*
`eraseWorkspace`, so the dropped child's connection is gone at sync time. The drop
is resolved by **recording each child's workspace at track time**
(`viewed_children_` is a `map<SessionId, WorkspaceId>`, A5), **not** by deriving it
from `model_.sessions`: after `eraseWorkspace` the child state is gone and
`activeWorkspaceId` has been promoted to another live workspace
(`ui_model.cpp:861-873`), so a derived lookup would resolve `connection_for` to
the promoted workspace and call `untrack`/`eraseSession` on the **wrong**
connection (Rev 6; R3-M1). With the recorded workspace, `connection_for(child_ws)`
is legitimately `nullptr` (the connection died with its workspace), so `untrack` is
skipped while `model_.disarm`/`forget_session`/`eraseSession` and the
`viewed_children_` diff still run; `eraseSession(child_ws, id)` is a no-op once the
workspace is gone. A reorder is rejected because syncing before `eraseWorkspace`
would see the child's state still present and pop nothing. §6.C's daemon-death
column records both halves (link `Dead` vs eviction).

#### A6 · `SupervisorConnection::subscriptions_`, `untrack`, `subscribe_one`, `SupervisorSink`

```cpp
// ── include/ymh/ui/supervisor_connection.hpp ────────────────────────────────
struct SupervisorSink {
    // … existing on_envelope/on_notice/on_permission/on_state (:86-91) …
    // NEW (58-I20/E29): a per-session subscribe failed.
    std::function<void(const SessionId&, const std::string&)> on_subscribe_error;
};

class SupervisorConnection {
    // members (amended): tracked_ (:179), subscribed_ (:180), cursors_ (:178) …
    std::map<SessionId, protocol::SubscriptionId> subscriptions_;   // NEW (E28)
};
```

```cpp
void SupervisorConnection::untrack(const SessionId& session) {
    protocol::SubscriptionId id;
    {
        std::lock_guard lock(mutex_);
        tracked_.erase(std::remove(tracked_.begin(), tracked_.end(), session), tracked_.end());
        if (const auto it = subscriptions_.find(session); it != subscriptions_.end()) {
            id = it->second;
            subscriptions_.erase(it);
        }
        subscribed_.erase(session);
        cursors_.erase(session);
    }
    if (id.value != 0) {                             // best-effort release (58-I18)
        nlohmann::json params{{"subscription", id.value}};
        try { static_cast<void>(connection_->request(protocol::method::kEventUnsubscribe,
                                                     params, config_.request_timeout)); }
        catch (const std::exception&) {}
    }
}
```

`subscribe_one` (`:256-316`) **amends** both `request()` calls to capture the
result: `const auto result = connection_->request(...)`, then records
`subscriptions_[session] = result.subscription`; the `UnknownSession` branch
(`:281-288`) records no id and invokes `sink.on_subscribe_error`; the
`SubscriptionLimit` (`protocol.hpp:183`) branch does the same (it is not handled
today). **Callers:** `track`/`untrack` (E14/E15/E28), `sync_subagent_subscriptions`
(A5), and the sink wired at `supervisor.cpp:842-881`.

```cpp
// Rev 5 (LOW; closes the 58-I18 pop/subscribe race): `untrack` can run while a
// `subscribe_one` request is in flight, so the id it records below may belong to
// a session the view already dropped. Record it ONLY if the session is still
// tracked; otherwise release the just-created subscription here, because
// `untrack` (which already ran and found no id) cannot.
protocol::SubscriptionId created;
{
    std::lock_guard lock(mutex_);
    if (std::find(tracked_.begin(), tracked_.end(), session) != tracked_.end()) {
        subscriptions_[session] = result.subscription;
        subscribed_.insert(session);
    } else {
        created = result.subscription;
    }
}
if (created.value != 0) {                        // best-effort release
    nlohmann::json params{{"subscription", created.value}};
    try { static_cast<void>(connection_->request(protocol::method::kEventUnsubscribe,
                                                 params, config_.request_timeout)); }
    catch (const std::exception&) {}
}
```

The same still-tracked test is applied to the `cursor_invalid` retry branch
(`:296-315`). With this guard, `untrack` and an in-flight `subscribe_one` cannot
leave an orphan daemon subscription, so 58-I18 holds exactly.

#### A7 · `SwitcherOverlayModel::openSubagents` — `src/ui/ui_model.cpp` (new)

```cpp
void SwitcherOverlayModel::openSubagents(const UiModel& model) {
    workspaces.clear();
    cursor = SwitcherCursor{};
    filter.reset();
    disarm_delete();                                 // 58-D8/R3-N6
    const SessionUiState* v = model.viewedSession(); // null-safe (58-I21)
    WorkspaceNode node;
    node.id    = model.activeWorkspaceId;
    // Rev 5 (LOW): the node title is the workspace label, not the raw id —
    // same rule as `open` (`ui_model.cpp:1282`); the raw id is only the
    // no-model fallback.
    const auto wit = model.workspaces.find(model.activeWorkspaceId);
    node.title = wit != model.workspaces.end()
                     ? (wit->second.title.empty() ? wit->second.cwd : wit->second.title)
                     : model.activeWorkspaceId.value;
    if (v != nullptr) {
        for (const SubagentView& a : v->subagents.agents) {
            SessionNode leaf;
            leaf.id    = a.id;
            leaf.title = a.summary.empty() ? a.id.value : a.summary;
            leaf.state = a.state;
            leaf.kind  = "subagent";                 // session_catalog.hpp convention
            node.sessions.push_back(std::move(leaf));
        }
    }
    workspaces.push_back(std::move(node));
    revalidate_switcher_cursor(*this, model);        // same clamp as open (:1339)
}
```

**Caller:** `open_subagents` (E13). The `source` field is set by the caller
(`supervisor.cpp:968`/`ui_model.cpp:1211` pattern), not by the builder. With no
children the node has no leaves and `render_switcher` shows `policy.empty_state`
(`(no subagents)`).

#### A8 · Glyph and status-prefix helpers — `src/ui/ui_render.cpp`

```cpp
// include/ymh/ui/ui_render.hpp (NEW; beside build_ui at :43)
[[nodiscard]] const char* subagent_status_glyph(SubagentStatus status);

// src/ui/ui_render.cpp
const char* subagent_status_glyph(SubagentStatus status) {
    switch (status) {
        case SubagentStatus::Running:   return ">";
        case SubagentStatus::Completed: return "✓";
        case SubagentStatus::Failed:    return "✗";
        case SubagentStatus::Cancelled: return "-";
    }
    return "?";
}

// file-local: the parent whose strip holds the viewed child's status.
[[nodiscard]] const SessionUiState* view_parent(const UiModel& model) {
    if (model.subagent_path.empty()) return nullptr;
    if (model.subagent_path.size() == 1) {
        const auto ws = model.workspaces.find(model.activeWorkspaceId);
        return ws == model.workspaces.end() ? nullptr
                                            : model.session(ws->second.activeSessionId());
    }
    return model.session(model.subagent_path[model.subagent_path.size() - 2]);
}

// file-local: the deepest viewed child's status, from its parent's strip.
[[nodiscard]] std::optional<SubagentStatus> viewed_status(const UiModel& model) {
    if (model.subagent_path.empty()) return std::nullopt;
    const SessionId& child = model.subagent_path.back();
    const SessionUiState* parent = view_parent(model);
    if (parent == nullptr) return std::nullopt;
    for (const SubagentView& a : parent->subagents.agents) {
        if (a.id == child) return a.status;
    }
    return std::nullopt;
}

// file-local (58-D4/D8): the status-line prefix. NOT policy data (MEDIUM-3).
[[nodiscard]] std::string subagent_status_prefix(const SessionId& id,
                                                 SubagentStatus status) {
    return "subagent " + short_id(id) + " · " +
           (status == SubagentStatus::Running ? "running" : "finished");
}
```

**Callers:** `subagent_status_glyph` — `render_subagents` (E31) and the
`58-G5` render test. `viewed_status`/`subagent_status_prefix` — `render_status`
(E34) only. `short_id` is the shipped helper (`ui_render.cpp:209-211`).

#### A9 · `CommandContext::subagents` — `include/ymh/ui/command_registry.hpp` (new field)

```cpp
struct CommandContext {
    // … existing fields (:16-48); `sessions` at :42 …
    std::function<void()> subagents;     // NEW (E11)
};
```

**Callers:** assigned by `dispatch_command` (E10, `supervisor.cpp:2509` pattern);
invoked by the `builtins()` `subagents` `Command` (E12,
`command_registry.cpp:236-242` pattern).

#### A10 · `UiEventAdapter::forget_session` + per-session dedup (HIGH-1) — `include/ymh/ui/ui_event_adapter.hpp`

```cpp
// amended members (:92-93): dedup keyed PER SESSION, so a popped child's ids can
// be cleared on pop (HIGH-1). EventId is a global UUIDv4 (event.hpp:40), so
// per-session keying changes no cross-session behaviour.
std::map<SessionId, std::set<std::string>>    applied_event_ids_;
std::map<SessionId, std::deque<std::string>>  applied_event_order_;

void forget_session(const SessionId& id);        // NEW (HIGH-1)
```

```cpp
void UiEventAdapter::forget_session(const SessionId& id) {
    applied_event_ids_.erase(id);
    applied_event_order_.erase(id);
}
```

`onSessionEnvelope` (`:283-303`) is **amended**: it looks up
`auto& ids = applied_event_ids_[envelope.session]` and
`auto& order = applied_event_order_[envelope.session]`, dedups against `ids`, and
applies the `kMaxAppliedEventIds` bound (`:12,294-297`) to `order` per session.
**Callers:** `sync_subagent_subscriptions` (A5) — on pop/switch/delete/daemon
death. It is **not** called on a picker **close**: closing the picker leaves
`subagent_path` unchanged, so no child is forgotten (§6.C pins
`applied_event_ids_` close = unchanged). This is the HIGH-1 fix: without it,
`ensureSubagentState` on re-entry creates a fresh state but every replayed UUID
is still in the global set, so all events are dropped and the transcript renders
blank (58-I10).

### 6.B Pinned edits to existing code (exhaustive, normative)

Each entry is `E<n> · <file> · <symbol> — <change>`. `E*` is cited by §2/§11/§14.
Anchors were re-verified against the tree for Rev 4.

- **E1 · `src/ui/supervisor.cpp` · `handle_switcher`** (`:2768-2818`), top of the
  function, before the `CtrlD` branch (`:2770`): read
  `const auto& policy = switcher_policy(model_.switcher.source);` and
  `if (event == ftxui::Event::CtrlD) { if (!policy.ctrl_d_enabled) return true;
  return handle_switcher_delete(event); }` — delete disabled for Subagents
  (§58-I17).
- **E2 · `handle_switcher` · `Tab` branch** (`:2792-2794`): gate it —
  `if (event == ftxui::Event::Tab) { if (!policy.tab_expands) return true;
  model_.switcher.toggleExpand(); return true; }` (Rev 3; R1-L1/`45-I24`).
- **E3 · `handle_switcher` · close keys** (`:2773-2783`): extend the existing
  `Escape || CtrlC` close (`:2778`) to also match `ftxui::Event::CtrlT` **only
  when** `policy.ctrl_t_closes` — `Ctrl+T` must **not** close Live/History
  (Rev 3; R3-N5/`58-H19`).
- **E4 · `handle_switcher` · `Return` branch** (`:2803-2816`, **FIXED Rev 4;
  MEDIUM-2**): dispatch on `policy.enter`, not on `source ==`. Replace the
  `History`/else split with
  `if (event == ftxui::Event::Return) { const SwitcherCursor cursor =
  model_.switcher.cursor; switch (policy.enter) { case SwitcherEnter::Resume:
  select_history(cursor); break; case SwitcherEnter::EnterChild: if
  (cursor.session.has_value()) { enter_subagent(*cursor.session);
  model_.switcher.close(); model_.mode = UiMode::Conversation;
  catalog_visible_.store(false); } return true;  /* empty leaf: consumed no-op,
  picker STAYS OPEN */ case SwitcherEnter::Focus: if (cursor.session.has_value())
  model_.focusSessionIn(cursor.workspace, *cursor.session); else
  model_.focusWorkspace(cursor.workspace); break; } model_.switcher.close();
  model_.mode = UiMode::Conversation; catalog_visible_.store(false); return true;
  }`. No `source ==` remains (58-I23); the empty leaf stays open (Rev 3; R4#3).
- **E5 · `src/ui/ui_render.cpp` · `switcher_disarmed_rows` + `render_switcher`**
  (`:958-1054`, `:1146-1182`): replace every `history` branch with the policy —
  window title (`policy.window_title`, `:1180`), heading (`policy.heading`,
  `:1151`), footer (`policy.footer`; the History `· partial`/refresh suffix is
  part of the History cell, `:1040-1051`), empty state (`policy.empty_state` +
  History's loading override, `:963-969`). No `source ==` remains in the
  renderer. (This subsumes Rev 3's duplicate E32; E32 is repurposed below.)
- **E6 · `handle_event_inner`** (`:3393-3466`), after the popup guards
  (`:3398-3414`) and **before** the `Ctrl+C` global (`:3429`): add
  `if (event == ftxui::Event::CtrlC && !model_.subagent_path.empty()) return true;`
  (§58-D4 suppression).
- **E7 · `handle_event_inner`**, alongside the globals: add
  `if (event == ftxui::Event::CtrlT) { open_subagents(); return true; }` (opens
  the picker at the current level).
- **E8 · `handle_input`** (`:3055-3258`): at the top (after the `state == nullptr`
  repair, `:3056-3072`), add the read-only guard: when
  `!model_.subagent_path.empty()`, handle `Escape` as pop
  (`return_subagent(); return true;`) and consume every other key
  (`return true;`) — no `submit`, no `complete`, no draft mutation, no `agent.*`
  RPC (§58-I4, §58-D3). This branch precedes the interrupt-arm branch
  (`:3086-3109`). It is a **dispatch guard** and does **not** run before the
  `handle_event_inner` globals (so `Ctrl+N`/`Ctrl+S`/`Ctrl+P` remain live;
  §58-D4).
- **E9 · `scroll_by`/`scroll_to_top`/`scroll_to_bottom`/`toggle_folds`**
  (`:2633-2675`): resolve `viewed()` instead of `active()` (§58-I12).
- **E10 · `dispatch_command`** (`:2431-2512`): add
  `context.subagents = [this] { open_subagents(); };` (next to
  `context.sessions = [this] { open_sessions(); };`, `:2509`).
- **E11 · `include/ymh/ui/command_registry.hpp` · `CommandContext`**
  (`:16-48`): add `std::function<void()> subagents;` (beside `sessions`, `:42`;
  A9).
- **E12 · `src/ui/command_registry.cpp` · `CommandRegistry::builtin()`**
  (`:152-300`): add a `subagents` `Command` (beside `sessions`, `:236-242`) whose
  handler calls `context.subagents()` when set. Without E11+E12 `/subagents` is
  `unknown command: /subagents (try /help)` (`:145`) and `context.subagents` does
  not compile (Rev 3; R2-N2/R3-N2/Oracle).
- **E13 · `open_subagents()`** (new; body A5): `SessionUiState* v =
  model_.viewedSession(); if (v == nullptr) { push_notice("no session to show
  subagents for"); return; }` — then `model_.switcher.source =
  SwitcherSource::Subagents; model_.switcher.openSubagents(model_);
  model_.mode = UiMode::Switcher; model_.dirty.markAggregate();`. `openSubagents`
  calls `disarm_delete()` and null-guards internally (Rev 3; R4#1 HIGH-2, R3-N6;
  A7).
- **E14 · `enter_subagent(id)`** (body A5): `ensureSubagentState(ws, id)` **before**
  `sync_subagent_subscriptions()` (which calls `track`); `ws` is bound as
  `model_.activeWorkspaceId` (A5/MEDIUM-6). Never `focusSessionIn` (§58-I1). The
  body marks `dirty.markAggregate()` (LOW: Rev 3 pinned no dirty mark here).
- **E15 · `return_subagent()`** (Rev 3; HIGH-1/R3-N1; body A5): `pop_back()` then
  `sync_subagent_subscriptions()`. The pop precedes every erase (58-I25); the
  undefined `disarm(id)`/`untrack(id)`/`eraseSession(...)` calls are replaced by
  `sync`, which calls the now-defined `UiModel::disarm(id)` (A4/MEDIUM-4),
  `untrack`, `forget_session`, and `eraseSession`.
- **E16 · `reconcile_subagent_path()`** (new; body A5): pop every id whose state
  is gone, then `sync_subagent_subscriptions()`. **Ownership (Rev 3;
  R1-N4/R4-N4/MED-8):** a `SupervisorApp` method; it mutates `model_.subagent_path`
  and calls `untrack`. `UiModel::eraseSession`/`eraseWorkspace` do **not** call it
  and never re-enter. `viewed_children_` (A5) is the diff set
  (`map<SessionId, WorkspaceId>`).
- **E17 · `SessionClosed` handler** (`:857-873`): after the existing `untrack`
  (`:863`) and `adapter_.onHostNotice` (`:866`), call
  `reconcile_subagent_path_for(*notice.session, "closed")` (A5); if the closed id
  was on the path, the §58-D8 notice is pushed. (`apply(SessionClosed)` already
  erased the state at `ui_model.cpp:1200-1203`, so the reconcile pops.)
- **E18 · `refresh_sessions`** (`:1680-1738`): in the `live` collection loop
  (`:1689-1699`) read `entry.value("kind", std::string{})` and
  `if (kind == "subagent") continue;` **before** `live.emplace_back` (`:1696`) —
  so the skip is upstream of **both** `track()` (`:1703`) and the cell loop
  (`:1711-1718`). Single tracking rule (§58-I22; Rev 3; R2-N3/R5-N2/MED-9/MED-12).
  `session.list` carries `kind` (`protocol.cpp:606`).
- **E19 · `SwitcherOverlayModel::openHistory`** (`src/ui/ui_model.cpp:1342-1426`):
  skip `entry.kind == "subagent"` in the session loop (beside the focused
  exclusion); the shared reader stays unfiltered (§58-D7; Rev 3; R5-N2/MED-9).
  `SessionNode.kind` is set at `:1398`.
- **E20 · `select_history`** (`:2753-2766`): look up the target `SessionNode` in
  `model_.switcher.workspaces` (its `kind` is set at `ui_model.cpp:1398`); if
  `node.kind == "subagent"`, push the notice `subagent sessions are viewed from
  their parent (/subagents)` and return without `resume_from_history` (Rev 3;
  R5-N2/MED-9). E20 checks `node.kind`, **not** the policy (Rev 4; I23 wording).
- **E21 · `UiModel::viewedSession()`** (new; body A4): if `subagent_path` empty →
  `activeSession()` (`:673-679`); else `session(subagent_path.back())` (never
  `operator[]`; `nullptr` if absent). `activeSession()`/`session()` unchanged.
- **E22 · `UiModel::focusWorkspace`** (`:1217-1223`), **`focusSessionIn`**
  (`:1237-1261`), and **`focusSession`** (`:1226-1235`): `subagent_path.clear()`
  (Rev 2; R3-M2). **Pinned sync call sites (Rev 4; MEDIUM-1/Oracle NEW-3):** the
  supervisor calls `sync_subagent_subscriptions()` after every call —
  `focusSession` at `:1463` (`apply_resume_success`), `focusSessionIn` at `:1478`
  (`recover_unknown_session`) and `:1667` (`activate_session`), `focusWorkspace` at
  `:2762` (`select_history`), and the E4 `Focus` branch at `:2808`/`:2810`. No
  dropped child's subscription leaks.
- **E23 · `UiModel::eraseSession`** (`:818-833`) and **`eraseWorkspace`**
  (`:848-882`): remove cells/state only; **no** reconcile call and **no** re-entry
  (Rev 3; MED-8). The reconcile is performed by the supervisor at each erase call
  site (E42–E45).
- **E24 · `UiModel::apply`** (`:1084-1099`): `apply(SubagentSpawned)` appends
  `SubagentView{id, task, Idle, Running}` if absent; `apply(SubagentUpdated)`
  updates summary/state/status (the existing loop `:1086-1098` gains
  `agent.status = e.status;`).
- **E25 · `UiModel::apply` · `SessionOpened`** (`:1195-1199`): call
  `ensureSessionState(workspace, *event.session)` (state, **no cell**) instead of
  `ensureSessionIn`. `SessionOpened` is adapted from
  `HostNoticeKind::SessionCreated` (`ui_event_adapter.cpp:366-371`), which only
  the RPC create/resume/fork paths emit (`protocol_server.cpp:401/423/434`) — a
  child spawned in-process (`AgentRegistry::createChild` →
  `SessionManager::createSession`, `agent_registry.cpp:149`) emits none. The edit
  is therefore a **defensive** guard against any `SessionOpened` creating a cell
  for a viewed child, whose state is materialized only by `ensureSubagentState`
  (A4/E14). Cells for normal live sessions are created by `refresh_sessions`
  (E18). (Rev 5; R1-L4/R3-L2: the prior "a `SessionCreated` notice for a child"
  rationale named a path children do not take.)
- **E26 · `UiModel::ensureSessionIn`** (`:739-754`): when the existing state has
  `subagent == true`, return it without `ensureCellIn`; `ensureSubagentState`
  (A4) sets `subagent = true` and erases any existing cell; `ensureSessionState`
  is the no-cell materializer (Rev 3; MED-12).
- **E27 · `UiModel::catalog_has_session`** (`:692-707`): skip
  `entry.kind == "subagent"` (the Live leaf predicate becomes kind-aware;
  Rev 3; MED-3; amends `45-I5`). The `--resume` resolver is a different function
  and is unaffected.
- **E28 · `src/ui/supervisor_connection.cpp` · `untrack`** (`:75-82`) and
  **`subscribe_one`** (`:256-316`): bodies in A6. `untrack` submits
  `event.unsubscribe {subscription}` (best effort) using the recorded id before
  erasing `tracked_`/`subscribed_`/`subscriptions_`/`cursors_`; `subscribe_one`
  records the `SubscriptionId` from `SubscribeResult` (`protocol.hpp:333-336`) and
  reports `UnknownSession`/`SubscriptionLimit` via `on_subscribe_error`.
- **E29 · `include/ymh/ui/supervisor_connection.hpp` · `SupervisorSink`**
  (`:86-91`): add `std::function<void(const SessionId&, const std::string&)>
  on_subscribe_error;` (A6) and wire it in the supervisor's sink setup
  (`supervisor.cpp:842-881`).
- **E30 · `src/ui/ui_event_adapter.cpp` · `adapt()`**: add
  `case EventType::SubagentSpawned -> UiEvent{SubagentSpawned{session,
  payload.subagent, payload.task}}` and extend the `SubagentFanIn` case
  (`:137-147`) to carry the `SubagentStatus` from `payload.outcome`.
- **E31 · `src/ui/ui_render.cpp` · `render_subagents`** (`:441-460`): use
  `subagent_status_glyph(agent.status)` (A8), not `state_glyph(agent.state)`.
- **E32 · `handle_switcher` · History `r`-refresh** (`:2796-2802`, **repurposed
  Rev 4; MEDIUM-2**): gate the branch on `policy.r_refreshes` instead of
  `source == History` —
  `if (policy.r_refreshes && event.is_character() && event.character() == "r") { … }`.
  This removes the second unlisted `source ==` and makes the History footer's
  `r refresh` policy data (A3). (Rev 3's E32 duplicated E5; folded here.)
- **E33 · `render_input`** (`:486-519`): when `!model.subagent_path.empty()`,
  render the read-only hint `(viewing subagent <8-hex> — Ctrl+T children · Esc
  return)` and **no** draft/caret (Rev 3; MED-11/`G6`).
- **E34 · `render_status`** (`:607-660`, **FIXED Rev 4; MEDIUM-3**): when
  `!model.subagent_path.empty()`, and `const auto st = viewed_status(model);
  st.has_value()`, prefix the left segment with
  `subagent_status_prefix(model.subagent_path.back(), *st)` (A8) — keyed on the
  **viewed child's status**, never on `model_.switcher.source`; when the path is
  empty or `viewed_status` is `nullopt`, no prefix. The aggregate right segment
  and the `active->status` model segment (`:653`) are unchanged. (Rev 3 indexed
  the policy by `switcher.source`, so `Ctrl+S` while viewing silently dropped the
  prefix.)
- **E35 · `build_ui`** (`:1558-1630`, **FIXED Rev 4; MEDIUM-5**): resolve the
  transcript/strip/scroll-hint/composer/status from `model.viewedSession()`,
  falling back to `active` when `viewedSession()` is **nullptr** — including the
  *missing-state* case (non-empty path, erased state), not only the empty-path
  case. `const SessionUiState* viewed = model.viewedSession(); const SessionUiState*
  pane = viewed != nullptr ? viewed : active;` and pass `pane` down. The fallback
  is defensive; E42–E45 reconcile the path at every erase site.
- **E36 · `on_link_state`** (`:1624-1664`): when `state ==
  SupervisorLinkState::Dead` and the dead workspace scopes `subagent_path`, call
  `reconcile_subagent_path()` (which syncs; A5). **Inert in
  practice (Rev 4; Oracle NEW-5/L3):** the state is not erased until eviction, so
  this is a no-op; it is kept as a fast-path guard, and the real clear is E37.
  `D8`'s daemon-death row therefore clears the path only at the presence-scan
  eviction (E37), never at this hook (Rev 6; R2-M1).
- **E37 · `evict_dead_workspaces`** (`:1561-1589`): after
  `model_.eraseWorkspace(id)` (`:1584`), call `reconcile_subagent_path()` (which
  syncs; A5) (Rev 3; MED-1). This call is **after** the `connections_.erase` at
  `:1579`; each dropped child's workspace was recorded at track time, so
  `connection_for(child_ws)` is `nullptr` (the connection died with its workspace)
  and sync's subscription calls are no-ops while its model/adapter cleanup still
  runs (A5's E37-ordering note; Rev 5 HIGH, mechanism corrected Rev 6). The order
  is required: the reconcile pops only once `eraseWorkspace` has removed the child
  state.
- **E38 · `supervisor_connection.cpp` · every `subscribed_.clear()` site**
  (`attempt_attach` `:214`, **`handle_disconnect` `:449`**, reconnect `:488`):
  clear `subscriptions_` beside `subscribed_` at all three, so a stale id can
  never be sent by a later `untrack` (Rev 3; R2-N5). `handle_disconnect` is the
  link-`Dead` path §6.C's `subscriptions_` daemon-death cell depends on; on
  eviction the whole connection (and its `subscriptions_`) is discarded at
  `connections_.erase` (E37). (Rev 5; R5-LOW-2 names the site explicitly.)
- **E39 · `supervisor.cpp` harness key seam** (`:3815-3871`): add a `"ctrl-t"`
  mapping to `ftxui::Event::CtrlT` (beside `"ctrl-d"` `:3855`, `"ctrl-s"` `:3861`).
- **E40 · `open_sessions`/`context.sessions`** (`:967-976`, `:2509`): unchanged
  (the History path is only filtered at E19/E20).
- **E41 · `UiModel::apply` · `DaemonDied`** (`:1187-1190`): unchanged
  (liveness-only; the path is cleared at E37 — E36 is the fast-path guard, inert
  until eviction). Recorded so the design does not assert a mechanism that does
  not exist.
- **E42 · `supervisor.cpp` · `recover_unknown_session`** (`:1471`): after
  `model_.eraseSession(...)` (`:1471`), call `reconcile_subagent_path()` (which
  syncs; A5) (Rev 4; MEDIUM-5). The subsequent `focusSessionIn` (`:1478`) sync is
  already pinned by E22, so E42 does **not** re-pin `apply_resume_success`
  (`:1463`). (Rev 5; R2-L2/R3-L4 duplicate removed.)
- **E43 · `supervisor.cpp` · `apply_session_deleted`** (`:2934`): after
  `model_.eraseSession(workspace, session)` (`:2934`), call
  `reconcile_subagent_path()` (which syncs; A5) (Rev 4; MEDIUM-5). This is the
  `session.delete` reply D8 asserts. **Qualified (Rev 6; R3-L1):** it is an
  **external-client / test-seam** path, not a supervisor-UI path — no UI action can
  delete a viewed child (Subagents `Ctrl+D` is disabled, E1; a child is in neither
  Live (E18/E27) nor History (E19); `delete_highlighted_workspace` refuses a Live
  source and any live daemon, `:2955-2956`), so production `session.delete` never
  targets a child; `58-H23` therefore *injects* the reply. E44 cannot fire either.
- **E44 · `supervisor.cpp` · `apply_workspace_deleted`** (`:2994`): after
  `model_.eraseWorkspace(workspace)` (`:2994`), call `reconcile_subagent_path()`
  (which syncs; A5). **Defensive; unreachable with a viewed
  child (Rev 5; R3-M1).** Its only caller is `delete_highlighted_workspace`
  (`:2990`), which refuses a Live source and any workspace with a live daemon
  (`:2955-2956`); a viewed child's workspace is live by construction, so it never
  fires with a child on the path. The real viewed-child removals are the
  `SessionClosed` notice (E17) and scan eviction (E37/E45); `session.delete` (E43)
  is external-client only. E44 is retained because the erase site is real and the
  reconcile is correct for the non-view case.
- **E45 · `supervisor.cpp` · `on_scan`** (`:1540`): after
  `model_.eraseWorkspace(spec.id)` (`:1540`), call `reconcile_subagent_path()`
  (which syncs; A5) (Rev 4; the third `eraseWorkspace` site,
  not named by Rev 3's reviewers). This is the boot-id-change eviction; together
  with E37 it covers the daemon-death deletion of a viewed child.
- **E46 · `src/ui/ui_event_adapter.{hpp,cpp}` · per-session dedup +
  `forget_session`** (HIGH-1; body A10): key `applied_event_ids_`/
  `applied_event_order_` by `SessionId`, amend `onSessionEnvelope` (`:283-303`) to
  use the per-session containers, and add `forget_session` (A10). Without this,
  E15/E16's erase leaves the child's UUIDs in a global set and re-entry renders
  blank.
- **E47 · `src/ui/ui_model.cpp` · `UiModel::disarm`** (MEDIUM-4; body A4):
  declare `void disarm(const SessionId& id);` on `UiModel` (`ui_model.hpp`, beside
  `viewedSession`) and define it. E15/E16's `disarm(id)` resolves to this method
  (Rev 3 pinned a symbol that did not exist).

No RPC interface changes beyond the already-defined `event.unsubscribe`
(`include/ymh/transport/protocol.hpp:535`, served at
`src/transport/protocol_server.cpp:526-541`); no persistence interface changes
(58-D5/58-D6, §7).

### 6.C State-lifetime table (Rev 6; the HIGH-1 mechanism)

One row per piece of state; one column per transition. **Transitions:**
**enter** = `Enter` in the picker pushes a child; **pop (Esc)** = leave one view
level; **switch** = `Ctrl+S`/`/sessions`/`Ctrl+N` focuses another session
(`focusSessionIn`/`focusWorkspace`); **close** = the picker closes
(`Esc`/`Ctrl+C`/`Ctrl+T`-toggle) or the app exits (`Ctrl+Q`); **close/delete
(external)** = the viewed child is removed by a `SessionClosed` notice (E17, the
genuinely external close) or an external `session.delete` reply (E43), or a scan
eviction (`on_scan`/`evict_dead_workspaces`, E45/E37). E43 is an
**external-client/test-seam** path: no supervisor UI action can delete a viewed
child (Subagents `Ctrl+D` is disabled, E1; a child is in neither Live (E18/E27)
nor History (E19); `delete_highlighted_workspace` (`supervisor.cpp:2955-2956`)
refuses a Live source and any workspace with a live daemon — a viewed child's
workspace is live by construction, so `Ctrl+D` there yields `stop the workspace
first`). **daemon death** = the workspace's link goes `Dead` / the workspace is
evicted. The link transition alone is inert (`E36`: `on_link_state(Dead)` calls
`reconcile_subagent_path()`, but the child state survives so the reconcile pops
nothing); the clear is the eviction (`E37`), which erases the connection
(`supervisor.cpp:1579`) *before* `eraseWorkspace` (`:1584`), so a dropped child is
untracked/erased against the workspace recorded at track time (A5), not a
re-derived one. Cells cite the E entry or A subsection that implements them.

| state | enter | pop (Esc) | switch | close | close/delete (external) | daemon death |
|---|---|---|---|---|---|---|
| `subagent_path` (`UiModel`) | `push_back(id)` (E14/A5) | `pop_back()` (E15/A5) | `clear()` (E22) | unchanged (closing the picker does not leave the view) | `reconcile` pops the closed/deleted id and everything deeper (E17 `SessionClosed`; E43 external `session.delete`; E37/E45 eviction; E44 defensive) | `reconcile` clears the path (E37; E36 fast-path guard, inert until eviction) |
| `viewed_children_` (`SupervisorApp`) | `sync` inserts + `track` (A5) | `sync` erases + `untrack` (A5) | `sync` clears all (E22/A5) | unchanged | `sync` erases the dropped ids (E17/E43/E37/E45) | `sync` clears all (E37; E36 inert) |
| child `SessionUiState` (`UiModel::sessions`) | `ensureSubagentState` materializes, no cell (A4/E14) | `eraseSession` (E15/A5) | `eraseSession` for each dropped child (E22/A5) | unchanged | `eraseSession` (E17/E43) / `eraseWorkspace` (E37/E45) | `eraseWorkspace` (E37) |
| `applied_event_ids_` / `_order_` (per session; `UiEventAdapter`) | fresh per-session set; replay from `Beginning` (A10) | `forget_session(id)` (HIGH-1; A5/A10) | `forget_session` per dropped child (A5/A10) | unchanged | `forget_session` (A5/A10) | link `Dead` → **unchanged** (E36 inert; the child state survives, so nothing is forgotten); eviction → `forget_session` per dropped child (E37/A5/A10) |
| `subscriptions_` (`SupervisorConnection`) | `subscribe_one` records the id (A6/E28) | `untrack` sends `event.unsubscribe`, erases (A6/E28) | `untrack` per dropped child (A5) | unchanged | `untrack` (E17 `:863`; A5) | link `Dead` → `subscribed_`/`subscriptions_` cleared (E38 `handle_disconnect` `:449`); eviction → connection (and its `subscriptions_`) discarded (E37) |
| `tracked_` (`SupervisorConnection`) | `track(id)` (A5) | `untrack` erases (A6) | `untrack` per dropped child (A5) | unchanged | `untrack` (E17 `:863`; A5) | link `Dead` → unchanged (connection kept for reconnect; E36 inert); eviction → connection discarded (E37) |
| `esc_arm` (per child `SessionUiState:300-301`) | new state `Disarmed` (A4) | `disarm(id)` then erase (A4/A5) | erased with the state (A5) | unchanged | erased with the state (A5) | erased with the state (E37) |
| `delete_arm` (`SwitcherOverlayModel:428`) | picker opened → `disarm_delete()` (A7/E13) | picker already closed | `Ctrl+N` → no picker, stays `Disarmed`; `Ctrl+S` opens the picker → `disarm_delete()` (A7) | `close()` calls `disarm_delete()` (`ui_model.cpp:1432`) | re-snapshot → `open`/`openHistory` → `disarm_delete()` (`ui_model.cpp:1265`/`:1344`) | **Live** switcher only: `resnapshot_switcher` (`supervisor.cpp:1234-1239`) → `open` → `disarm_delete()` (`ui_model.cpp:1265`); a Subagents picker is **not** re-disarmed (`eraseWorkspace` resets only its `cursor`, `ui_model.cpp:878`) — harmless, the Subagents source never arms (`ctrl_d_enabled == false`, E1/58-I17) |
| picker `cursor` (`SwitcherOverlayModel:421`) | picker closes → `close()` resets it (`:1430`) | picker already closed | `open`/`openHistory` re-clamp (`ui_model.cpp:1339`/`:1425`) | `close()` resets it (`:1430`) | `resnapshot_switcher` re-clamps (`:1557`,`:3019`) | `resnapshot_switcher` re-clamps (`:1662`) |
| parent `SubagentModel` (strip) | unchanged (the child was already added at spawn/fan-in, E24) | unchanged | unchanged | unchanged | **not** pruned by the child's deletion (the strip is spawn/fan-in-driven); entering the deleted id yields the F12 notice (E4 subscribe failure; E20 is the `/sessions` refusal, not the picker) | erased with the workspace (E37) |

**Why this table matters (HIGH-1).** Rev 3 asserted `applied_event_ids_` was
"per-materialized-state"; it was one adapter-global set that no transition
cleared. The row above pins the lifetime: it is **per session** and is
**forgotten on pop** (`forget_session`, A10) — so re-entry replays content, and
58-I10 holds.

## 7. Where a subagent's conversation lives (verified) and how it is read

**Storage.** A child is an ordinary session row in the workspace's
`<workspace>/.ymh/sessions.db`: `SessionHeader.parentSession` is set and
`kind == SessionKind::Subagent` (`include/ymh/session/session.hpp:36,58`); the
child's events are appended to the same store as any session
(`SessionPersistence`, `include/ymh/session/session_persistence.hpp:71-85`). The
parent→child edge is durable twice: `payload::SubagentSpawned` in the parent log
(`events.hpp:217-220`, appended at `subagent_service.cpp:371-380`) and
`SessionHeader.parentSession` on the child. The child lives in the **same
daemon** as its parent (55-D3, `docs/design/55-multi-agent-delegation-errata.md:467-481`),
so no cross-daemon read exists.

**Reading it — over RPC (chosen).** `event.subscribe` from `Beginning`:

- The supervisor already tracks a set of sessions and subscribes each with
  `event.subscribe` (`SupervisorConnection::track`, `subscribe_one`;
  `src/ui/supervisor_connection.cpp:64-72,256-318`), with cursor resume and a
  `Beginning` fallback.
- The daemon's `handle_subscribe` (`src/transport/protocol_server.cpp:574`)
  first calls `host_.readEvents(session, after, batch)` in a loop (`:624`) —
  this is the full replay — and, unless `replay_only`, keeps the subscription
  live (`:637-646`).
- `session_known` → `host_.sessionExists` → `runtime_.store().load(id).has_value()`
  (`src/transport/protocol_server.cpp:795-797`;
  `src/host/host_runtime.cpp:1233-1239`), i.e. **the store**, not the live
  registry. `readEvents` also only requires a stored header
  (`host_runtime.cpp:1241-1252`).
- **Therefore a completed child whose live process is gone is retrieved
  identically**: it is stored (`sessionExists == true`), `readEvents` returns its
  whole log, and the subscription simply never receives another live event. The
  replay-only variant is the same handler with `replay_only == true`, exposed as
  `session.replay` (`protocol_server.cpp:435-436`; `kSessionReplay`,
  `protocol.hpp:521`).
- A running child is the same path: replay, then live deltas until it drains.

**Why both "live model" and "event log", and why not a third source.**

- The parent's in-memory `SubagentModel` never holds the child's conversation
  (`SubagentView` has only `id/summary/state/status`), and a completed child has
  no resident agent — so a **live-only** source is insufficient by construction.
- The child's own event log (via the subscription) is the single source for the
  transcript, and the parent's event log is the source for the list; both are
  replayed by the same mechanism.
- A **direct disk read** (`SessionPersistence::openReadOnly(...).read(childId)`,
  the 22-D6 `SessionCatalogReader` pattern,
  `docs/design/22-switcher-sessions-errata.md:702-720`) is **rejected** for this
  view: the active session's daemon is always attached (a child is only reachable
  through its parent's live session), so RPC replay is authoritative, reuses the
  cursor/dedup machinery (`ui_event_adapter.cpp:288`), and delivers live deltas;
  a second disk read would fork the source of truth and miss streaming content.
  The disk path remains solely the History catalog's.
- `session.replay` (replay-only) is **not** used directly either: a single
  `event.subscribe` covers both the completed and the running case, so the view
  does not need to choose.

**Tracking lifetime (Rev 2 fix; R2-1/Oracle; Rev 4 rule; MED-12/HIGH-1).**
`track(childId)` on push to `subagent_path`; `untrack(childId)` on pop, switch,
close, or death, all through `sync_subagent_subscriptions()`
(§58-D2/§58-D4/§58-D6/§6.B E14/E15/E16/E22/E36/E37; §6.A A5). The picker does
**not** subscribe (it lists from the parent's `SubagentModel`), so browsing is
free; **only entered children are tracked**, and `refresh_sessions` skips
subagent rows before `track()` (§6.B E18). Rev 1's `untrack` erased only local
state and never released the daemon subscription, so each re-enter re-subscribed
and the daemon's `max_subscriptions_per_client{64}` (`protocol.hpp:216`) could be
exhausted, after which `subscribe_one` silently returned
(`supervisor_connection.cpp:280-288`) and the view rendered permanently empty.
Rev 2 pins `untrack` to send `event.unsubscribe` with the recorded
`SubscriptionId` (§58-I18), so the cap is not consumed by re-entry.

**Re-entry content (Rev 4; HIGH-1).** Rev 3 claimed the `applied_event_ids_`
dedup (`ui_event_adapter.cpp:288-295`) was "per-materialized-state"; it was **one
adapter-global set** (`ui_event_adapter.hpp:92`) that no transition cleared. After
§58-I19 erased the child state on pop, a re-entry replayed from `Beginning` but
every id was still in the global set, so all events were dropped and the
transcript rendered **blank**, violating 58-I10. Rev 4 keys the dedup **per
session** and clears it with `UiEventAdapter::forget_session(id)` on every
pop/switch/close/delete/death (§6.A A10, §6.B E46, §6.C); re-entry now replays
its full stored log. `EventId` is a global UUIDv4 (`include/ymh/core/event.hpp:40`),
so per-session keying changes no cross-session behaviour, and the 8192-entry
`kMaxAppliedEventIds` bound (`ui_event_adapter.cpp:12`) is applied per session.

## 8. Invariants (58-I)

| ID | Invariant |
|---|---|
| 58-I1 | Entering a subagent never calls `focusSessionIn`; `activeWorkspaceId` and `activeSessionId` are unchanged (58-D2). |
| 58-I2 | `subagent_path` is scoped to the active session and is cleared by `focusSessionIn`, `focusWorkspace`, and `focusSession` (E22), and reconciled by the supervisor on every `eraseSession`/`eraseWorkspace` call site (E16/E17/E36/E37/E42–E45), the `SessionClosed` handler, and daemon death. `UiModel` itself never calls `untrack`; every path shrink releases the dropped children via `sync_subagent_subscriptions()` (§6.A A5; 58-D2/58-D8). |
| 58-I3 | The transcript pane, the strip, the scroll hint, and the composer hint render `viewedSession()`; with an empty path this is exactly the active session, so main-agent behaviour is byte-identical to today (58-D3). |
| 58-I4 | While a child is viewed the composer is read-only **as a dispatch guard**: `handle_input` consumes every key except the `Esc` pop (E8), so no `submit`, no `complete`, no draft mutation, and no `agent.*` RPC can be sent (58-D3, 55-D5). |
| 58-I5 | A child appears in the strip/picker from `SubagentSpawned` (Running), not only at `SubagentFanIn` (58-D5). |
| 58-I6 | `subagent_status_glyph` renders `Running`/`Completed`/`Failed`/`Cancelled` as four distinct glyphs (`>`/`✓`/`✗`/`-`); `Completed` is never rendered as a running/idle child (58-D5). |
| 58-I7 | The picker lists exactly `viewedSession()`'s direct subagents (the current view level), from `viewedSession()->subagents.agents`; no other workspace or session appears (58-D1/§6.A). |
| 58-I8 | Picker `Enter` enters (push) and never changes the active session; on the empty leaf it is a consumed no-op and the picker **stays open** (E4); `Esc`/`Ctrl+C` close; `Ctrl+D` is a consumed no-op (58-D1/D2). |
| 58-I9 | A child's transcript is materialized only from that child's own event log through the existing per-session subscription; no new RPC, no new persistence, and no `agent.*` RPC (58-D6). |
| 58-I10 | Entering a **non-resident** (completed) child replays its full stored log; residency is not required (58-D6, §7). |
| 58-I11 | `Ctrl+T` is bound only as in §58-D4; while any popup guard is true the popup consumes it before the global binding (57-D4, `supervisor.cpp:3398-3414`). |
| 58-I12 | Scroll/fold keys act on `viewedSession()` (the child when in view), never on the active session while a child is viewed (58-D3, E9). |
| 58-I13 | A child `SessionUiState` (`subagent == true`) is never a `SessionCell`, never appears in the Live switcher, and is filtered out of the `/sessions` History source; no `refresh_sessions`/`SessionOpened`/adapter path creates a cell for one (58-D7, E18/E19/E25/E26). |
| 58-I14 | If the focused child is closed/deleted/evicted, or its daemon dies, the path is popped to a valid depth, the child's state is erased, and a status notice is shown; `viewedSession()` never default-constructs an empty state. The daemon-death pop happens at the **eviction** (`E37`), not at the link-`Dead` transition — `E36` is inert while the child state still exists (58-D8, E16/E17/E37/E42–E45). |
| 58-I15 | Entering a child sets its scroll to following (tail); a running child streams; the unseen/fold machinery is reused unchanged (58-D3). |
| 58-I16 | `subagent_path` is a simple path in the durable parent→child tree: every push targets an existing direct child of `viewedSession()`; a session has exactly one `parentSession` (`session.hpp:57-58`), so the path cannot cycle; its depth is bounded by the stored tree height. The UI adds no cap; the daemon's `presets.max_depth` admission (55-D7, `subagent_service.cpp:215`) bounds newly spawned chains. No UI-side `presets` read is required. |
| 58-I17 | Delete is disabled for `SwitcherSource::Subagents` (`Ctrl+D` is a consumed no-op, E1); the 51-D4/57 delete contract for `Live`/`History` is unchanged (58-D8/§6.A). |
| 58-I18 | `untrack` releases the daemon subscription (`event.unsubscribe` with the recorded `SubscriptionId`, E28); re-entering does not leak subscriptions and cannot exhaust `max_subscriptions_per_client` (58-D6/§7). The pop-vs-in-flight-subscribe race is closed by A6's guard: `subscribe_one` records the id only while the session is still in `tracked_`, else it releases the just-created subscription itself (Rev 5; R1-L6). |
| 58-I19 | A child `SessionUiState` is erased on pop/reconcile and its replay-dedup ids are forgotten (`forget_session`, A10/E46), so `UiModel::sessions` does not grow one entry per distinct entered child and a re-entry replays its full log (58-D8, 58-I10). |
| 58-I20 | A subscribe failure (`UnknownSession` or `SubscriptionLimit`) is reported through `SupervisorSink::on_subscribe_error` (E28/E29), reconciles the path, and surfaces as a status notice; it is never silently swallowed (58-F12). |
| 58-I21 | With no active/viewed session, `Ctrl+T`/`/subagents` push the notice `no session to show subagents for` and open nothing; `open_subagents`/`openSubagents` never deref a null `viewedSession()` (E13; 50-I7). |
| 58-I22 | Exactly the entered children are tracked: `refresh_sessions` skips `kind == "subagent"` before `track()` and before cell creation (E18); `SessionOpened` creates state without a cell (E25); `viewed_children_` is the tracked map (`SessionId`→owning `WorkspaceId`, A5), maintained only by `sync_subagent_subscriptions()` at its pinned call sites (§6.A A5; `enter_subagent`/`return_subagent`/`reconcile_subagent_path`, and E22's focus sites), so any path shrink releases exactly the dropped children against the workspace they were materialized in. |
| 58-I23 | Every per-source presentation/action divergence the shared widget/handler reads (window title, heading, footer, empty state, Enter, Ctrl+D, Tab, `Ctrl+T`-close, History `r`-refresh) comes from `SwitcherSourcePolicy` (§6.A A3/A3.1), consumed **only** by E1/E2/E3/E4/E5/E32, each dispatching on a policy field; the spec introduces **no new `source ==` branch** and removes the four it owns (E4 `supervisor.cpp:2805`; E32 `:2796`; E5 `ui_render.cpp:960`, `:1148`). The six remaining pre-existing `source ==` comparisons (five bullets in A3.2 — `:956`/`:959` are two) are rebuild/eligibility mechanics, enumerated with anchors in §6.A A3.2 and unchanged; row construction is by the three explicit builders (`open`/`openHistory`/`openSubagents`), not a branch; E20 checks `node.kind`, not the policy. |
| 58-I24 | While `subagent_path` is non-empty, `render_input` renders the read-only hint and no draft/caret (E33/E35). |
| 58-I25 | `return_subagent` pops `subagent_path` **before** erasing the state; no `pop_back()` ever runs on an empty or reconciled-away vector (E15; HIGH-1). |
| 58-I26 | `catalog_has_session` excludes `kind == "subagent"` entries, so a child fails the Live leaf predicate even if a stray `SessionCell` existed (E27; amends 45-I5). |

## 9. Failure modes (F1–F12 tagged; `—` when no tag applies)

| ID | Tag | Failure | Disposition |
|---|---|---|---|
| 58-F1 | F6 | A stale `subagent_path` survives a session/workspace switch and shows the previous session's child. | 58-I2; `focusSessionIn`/`focusWorkspace` clear the path (E22); the supervisor releases dropped subscriptions (E16). |
| 58-F2 | — | Entering a child accidentally calls `focusSessionIn`, losing the return path. | 58-I1; `enter_subagent` only pushes/tracks; the `Return` branch is policy-pinned (E4). |
| 58-F3 | F6 | `Ctrl+T` collides with a global binding. | 58-I11; verified unbound (`supervisor.cpp:3229/3236/3244`, `:3417-3464`, `event.hpp:86`); a future binding must yield to the popup guard. |
| 58-F4 | F6 | `Esc` in the subagent view arms the parent's interrupt instead of returning. | 58-D4; the path-pop branch precedes the hint/interrupt-arm branches (E8, `:3086-3109`); it does **not** rely on the composer being read-only. |
| 58-F5 | F5 | A huge child transcript is rendered unbounded. | 58-I15; the child is a normal `SessionUiState`, and the shipped UI caps each tool output at `kMaxToolOutput` (64 KiB) / conversation at `kMaxToolConversationOutput` (4 KiB) identically for any session (`ui_model.cpp:70-71,1001,1005,1022-1024`). 58 adds no new cap and no unbounded path. The §9.11 "active session + expanded subagents" materialization is not implemented in the shipped UI (`00:1378-1390`); the viewed child is **not** the active session, so it gets the same (bounded) per-session treatment, recorded as a deliberate scope decision. |
| 58-F6 | F11 | Parent vs child id duality: the child's events are routed into the parent state (or vice versa). | 58-I9; the adapter keys by `envelope.session` (`ui_event_adapter.cpp:285,299-301`); the path stores child `SessionId`s. |
| 58-F7 | F3 | A late event arrives for a child that was just deleted/closed. | 58-I14; reconciliation on `SessionClosed`/delete (E17); adapter dedup (`:288`). |
| 58-F8 | F7 | Child events mark the parent dirty and jitter the active view. | Dirty bits are keyed by session id (`ui_model.cpp:884-1173`); the child state has its own dirty key. |
| 58-F9 | F10 | A completed child is treated as resident/resumable. | 58-I10; the view reads the store, not the registry. |
| 58-F10 | F9 | `Esc`/`Ctrl+C` popping the view is confused with cancelling a turn. | 58-D4; `Esc` pops and `Ctrl+C` is suppressed while a child is viewed, so neither reaches `cancelActive()` (`:495-504`). |
| 58-F11 | F8 | Many children open many subscriptions; re-entry leaks them. | 58-I9/58-I18/58-I22/§7; only entered children are tracked, browsing opens none, and `untrack` releases the subscription. |
| 58-F12 | — | Child log unreadable / unknown session / subscription limit. | 58-I20; `subscribe_one` reports `UnknownSession`/`SubscriptionLimit` via `SupervisorSink::on_subscribe_error` (`supervisor_connection.cpp:280-288`, E28/E29); the supervisor reconciles the path and pushes the status notice `subagent unavailable: <reason>`. |
| 58-F13 | F6 | `Ctrl+D` in the Subagents picker deletes a finished child's durable transcript. | 58-I17; the `Ctrl+D` branch is gated by the source policy (E1) and is a consumed no-op for the picker. |
| 58-F14 | F9 | `Ctrl+C` while viewing a child silently cancels the parent's turn and orphans the child. | 58-D4/58-I4; `Ctrl+C` is consumed while `subagent_path` is non-empty (E6); it never reaches the global `cancelActive()` (`:3429`). |
| 58-F15 | F8 | Re-entering a child re-subscribes without releasing the previous subscription, exhausting the daemon cap. | 58-I18; `untrack` sends `event.unsubscribe` (E28); the `SubscriptionLimit` reply is surfaced (58-I20). |
| 58-F16 | — | `Ctrl+T`/`/subagents` with no active session derefs a null `viewedSession()`. | 58-I21; `open_subagents` guards and pushes the notice (E13). |
| 58-F17 | F6 | `Tab` in the picker collapses the single synthetic node, so `Enter` then hits the Live fallthrough. | 58-I23; the Tab gate (E2) consumes it for Subagents. |
| 58-F18 | — | `return_subagent` double-pops (empty-vector UB / wrong level). | 58-I25; pop-before-erase (E15). |
| 58-F19 | — | A stale `subscriptions_` id after reconnect unsubscribes another session. | E38 clears `subscriptions_` on reconnect. |
| 58-F20 | — | A child re-entered after `Esc` renders **blank**: the popped child's replay-dedup ids survive the state erase, so every replayed event is dropped. | 58-I19/58-I10; the dedup is per session and `forget_session(id)` clears it on pop (§6.A A10, §6.B E46, §6.C); `58-U24`/`58-H22` assert re-entry **content**. |

## 10. dsh (DeepSeek Harness) mapping

dsh is a headless harness whose UI is a plugin (`00-architecture.md`); it has no
switcher/overlay widget, so this errata's UI mechanism is a **non-mirror** of
dsh by construction. The durable/session semantics it rides (event-sourced
sessions, one daemon per workspace, per-session subscriptions) **mirror** dsh.
Per 56-D6 every non-mirror cell states why the divergence is acceptable and cites
an anchor:

| Concern | dsh | 58 disposition |
|---|---|---|
| Subagent navigation UI | No TUI; a plugin consumes the event stream | **Non-mirror** — 58 is a TUI interaction; dsh has no equivalent surface (`00:95`, `10 §4.8`). The durable child session is dsh's own model; the UI is additive. |
| Child transcript read | Reads the session store | **Mirror** — `event.subscribe` from `Beginning` reads the store (`protocol_server.cpp:795-797`; `host_runtime.cpp:1233-1239`). |
| Child list source | Parent event log | **Mirror** — `SubagentSpawned`/`SubagentFanIn` in the parent log (55-D3). |
| Per-source overlay policy | No overlay | **Non-mirror** — `SwitcherSourcePolicy` is a TUI presentation detail with no dsh counterpart; the mechanism (one policy table; no new ad-hoc `source ==` branch)
is a local design rule (§6.A A3/A3.1/A3.2, 58-I23). |
| Keybinding focus (F6) | One key owner; popups consume keys before globals | **Mirror** of F6 (`00-architecture.md:4850`, 57-D4). |

## 11. Supersedes / amends (explicit)

### 11.1 Named amendments and supersessions

- **Amends `00-architecture.md` §20.25** (`:3054-3110`): the designed
  per-subagent `SubagentViewModel` with a capped `tail` and `expanded` flag
  (steps 2, 4, 6 at `:3062-3069`) is **not** the navigation mechanism. 58
  reuses the per-session `SessionUiState`/`render_conversation` and a view scope
  instead. Reason: that design was never implemented (no `SubagentCoalescer` /
  `SubagentViewModel` in `src/`/`include/`), and the shipped strip
  (`ui_render.cpp:441-460`) is a flat, read-only line. `00` §20.25's batching
  rationale is not contradicted; it is simply not needed for a one-child view.
- **Amends `10-supervisor-tui.md` §4.8** (`:569-618`): `SubagentEnvelope`,
  `SubagentDelta`, `SubagentViewModel`, and `SubagentCoalescer` remain
  unimplemented design; 58 does not implement them. The shipped model is
  `SubagentView` (`ui_model.hpp:264-268`), which 58 extends with
  `SubagentStatus`.
- **Amends `UI_SURFACE_INVENTORY.md` Surface 2** (`:36-56`): the per-session
  bottom list is already removed (RB-11 shipped; `render_session_bar` no longer
  exists). 58 adds that child sessions are never `SessionCell`s (58-D7).
- **Amends verified `22-D6`** (`docs/design/22-switcher-sessions-errata.md:702-720`):
  `/sessions` no longer lists `kind == "subagent"` rows. Reason: child navigation
  is `/subagents` (58-D1) and a child is never a top-level selection (58-D7), so
  listing it would only expose a row that Enter must refuse (E20). This is a
  UI-scope decision, **not** a 55-D3 prohibition: verified 55-D3 permits resuming
  a stored child (`55:484-485`), and `--resume <child>` still opens one (§50-I9
  below). The filter is applied in the History builder
  (`SwitcherOverlayModel::openHistory`, E19) and the `select_history` guard
  (E20), so the shared reader and the `--resume` resolver are unaffected
  (`session_catalog.hpp:69-80`). This is the 57-D1/SW19 silent-break class; it is
  named here rather than left implicit.
- **Amends verified `45-I5`** (`docs/design/45-ui-interaction-errata.md:1646`):
  the Live leaf predicate now excludes subagent rows. `catalog_has_session`
  (`src/ui/ui_model.cpp:692-707`) skips `entry.kind == "subagent"` (E27), so a
  child is **not** "in the latest catalog snapshot" for the Live predicate even
  though it remains in the catalog for other consumers. Reason: 58-D7 forbids a
  child `SessionCell`, so the child can never be a Live leaf; the invariant is
  made true by the predicate, not weakened by a scope note. (Rev 3; R2-N1/R5-N1.)
- **Amends verified `45-I24`** (`:1665`, switcher-overlay Tab = expand) and
  **`45-I20`** (`:1661`, composer Tab agent-cycling) for the new source/view:
  the Subagents picker consumes `Tab` (E2) and the read-only guard consumes
  composer `Tab`/`Shift+Tab` (E8). Reason: the picker has a single synthetic
  node (no expand) and the subagent view has a read-only composer (no agent
  cycle). Live/History Tab and main-view cycling are unchanged. The related
  `45-I3` (Tab never navigates the command list) and `45-I9` (hints_dismissed
   only cleared by a prefix edit) are **unaffected**: the picker is not the
   composer command list, and the read-only view performs no draft edits.
   (Rev 3; MED-6/MED-7/R5-N6.)
- **Suspends verified `45-I2`** (`:1643`, when the command list is active
  ArrowUp/ArrowDown move the selection, otherwise they recall history) and
  **`45-D1`** (`:178`, Arrow precedence: command list first, history otherwise)
  in the subagent view: E8 consumes **every** key except the `Esc` pop while
  `subagent_path` is non-empty, so `ArrowUp`/`ArrowDown` never reach
  `handle_input`'s selection/history logic. Consequence stated: view a child,
  press `ArrowUp` → E8 returns `true`, no history recall. The same guard scopes
  **`45-D2`** (`:179`, the `/` list: arrows navigate, Tab completes) to the
  editable composer — it is not reachable in the read-only view. Main-view
  behaviour is unchanged (E8 fires only while the path is non-empty). (Rev 5;
  R5-MEDIUM-1 and the R5 input-handling re-audit.) `45-I25` is **not** a 58 scope:
  it was already **superseded by verified `51-D4`/`51-A4`**
  (`docs/design/51-reasoning-fold-prompt-emphasis-and-tables-errata.md:73,131` —
  exit moved to `Ctrl+Q`) before this feature, so 58 neither suspends nor scopes
  it (Rev 6; R5-LOW-1).
- **Amends verified `57-I3`** (`:812`, both footers contain `Ctrl+D delete`) and
  **`57-I9`** (`:818`, Ctrl+D deletes the highlighted session in the switcher
  overlay) by **scoping them to the two catalog sources**: the Subagents picker's
  footer is `Enter enter · Esc close` and its `Ctrl+D` is a consumed no-op. Both
  are now data in the policy table (§6.A); the Live/History delete contract is
  unchanged. (Rev 3; Oracle HIGH/MEDIUM-5.)
- **Narrows verified `50-I9`** (`:445`, explicit resume opens the stored
  session): the `/sessions`/Ctrl-S surfaces no longer list child sessions (58-D7),
  so resume-through-listing applies to root/fork sessions; `--resume <id>` (the
  resolver, unfiltered — `cli.cpp:431-466`) still opens a child by id, exactly as
  verified 55-D3 states ("the child survives as a stored session that can be
  resumed", `55:484-485`). Reason: the `/sessions`/Ctrl-S surfaces are a
  root/fork navigation affordance and child navigation is `/subagents` (58-D1);
  this **narrows** the listing surfaces, it does not forbid child resume. (Rev 4;
  MEDIUM-7.)
- **Extends verified `45-I8` / `45-D5`** (`docs/design/45-ui-interaction-errata.md:1649`,
  `:533-580`): in the new subagent view, `Esc` pops the path **before** the
  command-hint and interrupt-arm branches of `handle_input` (E8,
  `supervisor.cpp:3086-3109`). This does not change 45's Conversation behaviour
  (no path ⇒ unchanged); it adds a sub-context. The related `48-D2` Esc-arm is
  disarmed on pop (E15). Consequence stated: while viewing a child with an active
  parent turn, `Esc` returns instead of arming the parent interrupt.
- **Extends verified `51-D4`** (`docs/design/51-reasoning-fold-prompt-emphasis-and-tables-errata.md:723-...`,
  `:1008-1016`, I19-I27): the delete contract is unchanged for `Live`/`History`;
  58 disables delete only for the Subagents source (58-I17, E1). `51-I20`/`51-I21`
  are unchanged for the two catalog sources.
- **Extends `57-D4`** (`docs/design/57-switcher-sessions-popup-errata.md:531-571`):
  the Subagents source is a third `UiMode::Switcher` popup and inherits the popup
  guard (58-I11); `open_subagents` sets `UiMode::Switcher` (E13) so `Ctrl+Q` is
  consumed. `57-D5` ordering applies to `Live`/`History` only; the Subagents
  source is a single synthetic node with no ordering rule, so `57-I11`/`57-I13`
  (`:820-822`, "both switcher sources") are **not** broken — their scope is now
  "the two catalog sources". (Rev 2 note retained; Rev 3 policy table.)
- **Does not amend** the 51-D4/57 **exit** contract; 58 adds `Ctrl+T` and scopes
  `Esc`/`Ctrl+C` only while `subagent_path` is non-empty (§58-D4). Spec 57's
  cited line anchors have drifted ~3 lines from the current tree (57 cites
  `supervisor.cpp:3395-3411`; the guards are now `:3398-3414`) — this spec cites
  the current tree.

### 11.2 The invariant sweep (Rev 6; every invariant in 22/45/50/51/55/57)

Per the Oracle's mechanical process fix, every invariant in every spec this
feature touches is accounted for below: either **named** as amended/superseded
(with the §11.1 anchor), or **unaffected** with a one-line reason. No invariant
is left unaccounted for. Counts: **22** = SW1–SW26 + 22-D1–D6 (32); **45** =
I1–I30 + D1–D10 (40); **50** = I1–I24 + D1–D5 (29); **51** = I1–I27 + D1–D4
(31); **55** = I1–I36 + D1–D13 (49); **57** = I1–I20 + D1–D7 (27). Total **208**
items; **12** named amendments/narrowings (22-D6, 45-I5, 45-I20, 45-I24, 50-I9,
57-I3, 57-I9, plus the scoped extensions of 45-I8/45-D5, 51-D4, 57-D4, 22-D3/SW11,
57-I11/57-I13) and **3** input-handling suspensions/scopes in the subagent view
(45-I2, 45-D1, 45-D2; §11.1). `45-I25` is a **pre-existing** supersession (by
verified `51-D4`/`51-A4`, exit moved to `Ctrl+Q`) carried in the sweep, not a 58
change (Rev 6; R5-LOW-1); the remainder are proven unaffected.

**22 (`SW1`–`SW26`, `22-D1`–`22-D6`).**

| ID | Disposition |
|---|---|
| SW1 Live render predicate | Unaffected — 58 adds a source; the Live predicate (`live` and `Attached`/`Stopping`) is unchanged. |
| SW2 scanOnce live predicate | Unaffected. |
| SW3 eviction never removes Connecting/Live | Unaffected — E37 adds a reconcile after eviction, not a candidacy change. |
| SW4 eviction deferred during ExitConfirm | Unaffected. |
| SW5 Live switcher contains no non-live node | Unaffected. |
| SW6 no registry writes by eviction/switcher | Unaffected. |
| SW7 `/sessions` never blocks the UI thread | Unaffected — E19 filters in-memory rows on the UI thread; no new I/O. |
| SW8 `/sessions` read-only open | Unaffected. |
| SW9 `/sessions` enumerates registry rows only | Unaffected. |
| SW10 per-workspace read failure contained | Unaffected. |
| SW11 `/sessions` reuses the overlay | **Extended** — a third source is added to the same model (§11.1, §6.A); no second overlay. |
| SW12 `ensureRunning` not on the UI thread | Unaffected. |
| SW13 `/sessions` selection issues `session.resume` | Unaffected — E20 refuses a child before `resume_from_history`. |
| SW14 `pending_resume_` one per workspace | Unaffected. |
| SW15 `--resume` resolves own workspace | Unaffected — the resolver is unfiltered (E27 touches `catalog_has_session`, not the resolver). |
| SW16 `--new` beats `--resume` | Unaffected. |
| SW17 workspace ordering title asc | Unaffected — the Subagents source is one synthetic node with no ordering (§6.A). |
| SW18 exit prompt independent of switcher | Unaffected. |
| SW19 catalog snapshot `capturedAtMs`/`complete` | Unaffected — 58 changes no snapshot field; the History `partial` marker is preserved (E5). |
| SW20 Connecting/Detached/Dead never rendered in Live | Unaffected. |
| SW21 eviction skips `ensure_in_flight_` | Unaffected — E37 runs after the existing gate. |
| SW22 `on_scan` boot_id re-attach | Unaffected. |
| SW23 failure notice never calls `ensureSessionIn` | Unaffected — E25 uses `ensureSessionState` (no cell). |
| SW24 spawn worker lifetime | Unaffected. |
| SW25 resume success-path guard | Unaffected. |
| SW26 catalog reader lifetime | Unaffected. |
| 22-D1 switcher/session ordering | Unaffected for Live/History; the Subagents source is a single synthetic node with no ordering rule (§6.A). |
| 22-D2 Live source fields | Unaffected. |
| 22-D3 reuse the overlay | **Extended** — 58 adds a third source to the same overlay (§11.1, §6.A); no second overlay. |
| 22-D4 History read strategy | Unaffected. |
| 22-D5 workspace identity | Unaffected. |
| 22-D6 `/sessions` lists every `list()` row | **NAMED amended** by 58-D7 (§11.1): subagent rows are filtered (E19/E20). |

**45 (`45-I1`–`45-I30`, `45-D1`–`45-D10`).**

| ID | Disposition |
|---|---|
| 45-I1 History prompts+commands | Unaffected. |
| 45-I2 Arrow in command list | **NAMED suspended** (§11.1): E8 consumes ArrowUp/ArrowDown while a child is viewed, so neither the command selection nor history recall runs; main composer unchanged. |
| 45-I3 Tab never navigates the command list | Unaffected — the picker Tab gate (E2) is the switcher overlay, not the composer command list, and the read-only view consumes Tab (E8) so the list is never active there. |
| 45-I4 CompletionCycle gone | Unaffected. |
| 45-I5 Live leaf iff | **NAMED amended** (§11.1): the catalog predicate excludes subagents (E27), so a child is not a Live leaf. |
| 45-I6 Live is a subset of History | Unaffected — children are in neither source. |
| 45-I7 no workspace node suppressed | Unaffected — 58 suppresses no workspace node; only child session leaves (not workspace nodes) are excluded. |
| 45-I8 Esc hides list / no-op | **Extended** (§11.1): the view pop precedes this branch (E8); main-view behaviour unchanged. |
| 45-I9 `hints_dismissed` only cleared by a prefix edit | Unaffected — the read-only view performs no draft edits. |
| 45-I10 `activeSessionId` modeled after selection | Unaffected. |
| 45-I11 `handle_input` never returns false | Unaffected — the guard returns true. |
| 45-I12 `CommandContext.session` is `ensureActiveSession()` | Unaffected. |
| 45-I13 `mcp.status` params | Unaffected. |
| 45-I14 `mcp.status` fields | Unaffected. |
| 45-I15 `mcp.status` empty | Unaffected. |
| 45-I16 `/status` renders | Unaffected. |
| 45-I17 status model segment never empty | Unaffected — E34 adds a left-segment prefix; the model segment is unchanged. |
| 45-I18 connectivity derived | Unaffected. |
| 45-I19 `/exit` row | Unaffected. |
| 45-I20 Tab agent cycling only when draft empty | **NAMED amended** (§11.1): suspended while `subagent_path` is non-empty (E8). |
| 45-I21 `agent.select` requires `can_select` | Unaffected. |
| 45-I22 `agent.list` per-agent flags | Unaffected. |
| 45-I23 no-presets notice | Unaffected. |
| 45-I24 switcher overlay Tab expand | **NAMED amended** (§11.1): the Subagents source consumes Tab (E2); Live/History unchanged. |
| 45-I25 Ctrl+D exit confirmation | **Superseded (pre-existing)** by verified `51-D4`/`51-A4` (`51:73,131`; exit moved to `Ctrl+Q`) **before** 58 — not a 58 suspension or scope; 58 relies on no Ctrl+D exit binding (Rev 6; R5-LOW-1). |
| 45-I26 `agent.list`/`agent.select` strict | Unaffected. |
| 45-I27 protocol version additive | Unaffected. |
| 45-I28 `StatusModel::agent` | Unaffected. |
| 45-I29 profiles | Unaffected. |
| 45-I30 shared JSON schema | Unaffected. |
| 45-D1 Arrow precedence | **NAMED suspended** (§11.1): E8 consumes ArrowUp/ArrowDown in the subagent view; the main composer is unchanged. |
| 45-D2 `/` list completion | **Scoped** (§11.1): the read-only view's composer consumes every key (E8), so the `/` list is never active there; the main composer is unchanged. |
| 45-D3 Live subset of History | Unaffected. |
| 45-D4 focused exclusion | Unaffected. |
| 45-D5 Esc hides list | **Extended** (§11.1): the view pop precedes it (E8). |
| 45-D6 `mcp.status` | Unaffected. |
| 45-D7 `/status` | Unaffected. |
| 45-D8 alias display | Unaffected. |
| 45-D9 agent cycle | **Amended** (§11.1): suspended in the subagent view (E8). |
| 45-D10 model-on-focus/self-heal | Unaffected. |

**50 (`50-I1`–`50-I24`, `50-D1`–`50-D5`).**

| ID | Disposition |
|---|---|
| 50-I1 absolute user root | Unaffected. |
| 50-I2 discovery no writes/symlink | Unaffected. |
| 50-I3 workspace skill no shadow | Unaffected. |
| 50-I4 file command no shadow reserved | Unaffected. |
| 50-I5 `$ARGUMENTS` once | Unaffected. |
| 50-I6 skill identity/trust | Unaffected. |
| 50-I7 fresh launch creates a session | Unaffected — 58's null guard (E13) exists precisely to respect the lazy-create window. |
| 50-I8 cwd workspace initial active | Unaffected. |
| 50-I9 explicit resume opens the stored session | **NAMED narrowed** (§11.1): `/sessions`/Ctrl-S no longer list children; `--resume <id>` unchanged. |
| 50-I10 reconnect keeps `activeSessionId` | Unaffected. |
| 50-I11 MCP child env overlay | Unaffected. |
| 50-I12 MCP stdio not Minimal | Unaffected. |
| 50-I13 env never logged | Unaffected. |
| 50-I14 `spawn()`/`run()` share env under Inherit | Unaffected. |
| 50-I15 Minimal seeded keys | Unaffected. |
| 50-I16 `stderr_path` | Unaffected. |
| 50-I17 `mcp.log_child_stderr` global | Unaffected. |
| 50-I18 PTY inherits env | Unaffected. |
| 50-I19 MCP cwd default | Unaffected. |
| 50-I20 MCP env not filtered | Unaffected. |
| 50-I21 MCP cwd root-confined | Unaffected. |
| 50-I22 MCP global-layer only | Unaffected. |
| 50-I23 workspace tier fail-closed | Unaffected. |
| 50-I24 trust record | Unaffected. |
| 50-D1 skill_roots | Unaffected. |
| 50-D2 fresh-launch harness | Unaffected. |
| 50-D3 `ProcessEnvMode` | Unaffected. |
| 50-D4 workspace config mcp_servers | Unaffected. |
| 50-D5 trust store | Unaffected. |

**51 (`51-I1`–`51-I27`, `51-D1`–`51-D4`).**

| ID | Disposition |
|---|---|
| 51-I1 user bar/tint | Unaffected — 58 adds no theme change. |
| 51-I2 theme default blue | Unaffected. |
| 51-I3 user foreground | Unaffected. |
| 51-I4 colour off still draws the bar | Unaffected. |
| 51-I5 tint omitted when off | Unaffected. |
| 51-I6 caret after gutter | Unaffected. |
| 51-I7 new Theme fields appended | Unaffected. |
| 51-I8 paint/paint_bg seams | Unaffected. |
| 51-I9 paint_bg file-local | Unaffected. |
| 51-I10 table header/rows | Unaffected. |
| 51-I11 cell inline spans | Unaffected. |
| 51-I12 no cell content discarded | Unaffected. |
| 51-I13 grid width bound | Unaffected. |
| 51-I14 column alignment | Unaffected. |
| 51-I15 empty/ragged cells | Unaffected. |
| 51-I16 table extension present | Unaffected. |
| 51-I17 render pure/no-throw | Unaffected. |
| 51-I18 malformed table pipe fallback | Unaffected. |
| 51-I19 Ctrl+Q `begin_exit(true)` | Unaffected. |
| 51-I20 Ctrl+D targets session else workspace | **NAMED scoped** (§11.1): unchanged for Live/History; the Subagents source consumes Ctrl+D (E1). |
| 51-I21 arm/confirm | Unaffected for Live/History; the Subagents source never arms (58-I17). |
| 51-I22 session delete RPC | Unaffected. |
| 51-I23 workspace delete registry-only | Unaffected. |
| 51-I24 delete workspace keeps `.ymh/` | Unaffected. |
| 51-I25 refusal notice | Unaffected. |
| 51-I26 delete arm UI-only | Unaffected. |
| 51-I27 re-clamp after delete | Unaffected. |
| 51-D1 withdrawn reasoning fold | Unaffected. |
| 51-D2 theme tokens | Unaffected. |
| 51-D3 tables | Unaffected. |
| 51-D4 delete contract | **NAMED extended** (§11.1): Subagents consumes Ctrl+D; Live/History unchanged. |

**55 (`55-I1`–`55-I36`, `55-D1`–`55-D13`).**

| ID | Disposition |
|---|---|
| 55-I1 delegation mode per instance | Unaffected. |
| 55-I2 `run_in_background` resolution | Unaffected. |
| 55-I3 child has `parentSession` + `Subagent` kind | **Relied upon** — 58 keys on `kind == "subagent"` (E18/E19/E27) and `parentSession`; 58 changes nothing about it. |
| 55-I4 continuable child is resident | Unaffected — 58 views it via the existing subscription (58-D6). |
| 55-I5 no log read before settlement | Unaffected. |
| 55-I6 `onSettled` once | Unaffected. |
| 55-I7 background epoch fan-in | Unaffected. |
| 55-I8 non-Completed errored | Unaffected. |
| 55-I9 depth checked vs `presets.max_depth` | **Relied upon** — 58-I16 cites it as the depth bound; unchanged. |
| 55-I10 `max_live_subagents` | Unaffected. |
| 55-I11 roster composition once | Unaffected. |
| 55-I12 tool list leaf-scoped | Unaffected. |
| 55-I13 provider+model together | Unaffected. |
| 55-I14 route inheritance | Unaffected. |
| 55-I15 effort clear | Unaffected. |
| 55-I16 route preflight | Unaffected. |
| 55-I17 durable route | Unaffected. |
| 55-I18 `send_message` acceptance only | Unaffected. |
| 55-I19 `send_message` delivery | Unaffected. |
| 55-I20 authorize parent-child only | **Relied upon** — 58's read-only composer sends no `agent.*` (58-D3); unchanged. |
| 55-I21 `interrupt_agent` current turn only | Unaffected. |
| 55-I22 `list_agents` continuable only | Unaffected. |
| 55-I23 foreground delegation | Unaffected. |
| 55-I24 delegation tool `ParallelSafe` | Unaffected. |
| 55-I25 background activation via `SessionActivator` | Unaffected. |
| 55-I26 no registry mutex across agent calls | Unaffected. |
| 55-I27 daemon restart preserves child | Unaffected — 58 reconciles a closed child (E17) without changing restart semantics. |
| 55-I28 scope statement per child | Unaffected. |
| 55-I29 no new durable EventType/wire notification | **Relied upon** — 58 adds a *UI* event (`SubagentSpawned`) and maps the existing `EventType::SubagentSpawned`; no durable event/wire change (58-D5). |
| 55-I30 `presets.max_depth` single source | Unaffected — 58-I16 cites it; no second maxDepth. |
| 55-I31 unreported settlement record | Unaffected. |
| 55-I32 activation epoch | Unaffected. |
| 55-I33 fan-out admission atomic | Unaffected. |
| 55-I34 killed background child Cancelled | Unaffected. |
| 55-I35 foreground settlement has failure path | Unaffected. |
| 55-I36 continuable slot release | Unaffected. |
| 55-D1 two delegation modes | Unaffected. |
| 55-D2 tool surface | Unaffected. |
| 55-D3 `run_in_background` / durable child in parent daemon | **Relied upon** — 58-D6/§7 read the child from the parent's daemon; unchanged. |
| 55-D4 settlement notice | Unaffected. |
| 55-D5 `send_message`/`interrupt_agent`/`list_agents` model-facing | **Relied upon** — 58-D3 exposes no UI `agent.*`; unchanged. |
| 55-D6 per-child route | Unaffected. |
| 55-D7 `maxDepth` / fan-out cap | **Relied upon** — 58-I16 cites it; unchanged. |
| 55-D8 admission `createChild` | Unaffected. |
| 55-D9 failure semantics | Unaffected. |
| 55-D10 `SubagentRunner` settlement race | Unaffected. |
| 55-D11 parallelism/scheduling | Unaffected. |
| 55-D12 route persistence | Unaffected. |
| 55-D13 `SessionActivator` seam | Unaffected. |

**57 (`57-I1`–`57-I20`, `57-D1`–`57-D7`).**

| ID | Disposition |
|---|---|
| 57-I1 History footer no captured segment | Unaffected. |
| 57-I2 `nowMs` source | Unaffected. |
| 57-I3 both footers contain `Ctrl+D delete` | **NAMED scoped** (§11.1): the Subagents footer is `Enter enter · Esc close` (E5/§6.A). |
| 57-I4 armed confirm text on the row | Unaffected for Live/History; Subagents never arms. |
| 57-I5 arming does not change line count | Unaffected. |
| 57-I6 confirmation placement/suffix | Unaffected. |
| 57-I7 disarm triggers | Unaffected — `openSubagents` calls `disarm_delete()` (E13). |
| 57-I8 single Ctrl+D never deletes | Unaffected for Live/History; Subagents consumes it. |
| 57-I9 Ctrl+D deletes the highlighted session else workspace | **NAMED scoped** (§11.1): unchanged for Live/History; Subagents consumes it (E1). |
| 57-I10 Ctrl+Q consumed while a popup is open | **Relied upon** — `open_subagents` sets `UiMode::Switcher` (E13), so the guard applies. |
| 57-I11 effective-root workspace first (both sources) | **Scoped** (§11.1) — "both switcher sources" now means the two catalog sources; the Subagents source is a single synthetic node with no ordering. |
| 57-I12 lastUsedAt ordering | Unaffected — the Subagents source has no ordering. |
| 57-I13 ordering applied in open/openHistory | **Scoped** (§11.1): "both switcher sources" now means the two catalog sources; the Subagents source is built by `openSubagents` and applies no ordering. |
| 57-I14 session order | Unaffected. |
| 57-I15 popup guard blocks globals | **Relied upon** — 58-I11; `open_subagents` sets the mode (E13). |
| 57-I16 popup width | Unaffected — `render_switcher`'s frame/width logic is reused; E5 changes only text. |
| 57-I17 highlight element | Unaffected. |
| 57-I18 badge colour | Unaffected. |
| 57-I19 Live `lastUsedAt` snapshot | Unaffected. |
| 57-I20 badge ellipsize | Unaffected. |
| 57-D1 drop captured footer segment | Unaffected. |
| 57-D2 both footers carry the delete hint | **Extended** (§11.1) — the third source's footer is policy data (E5); the two catalog sources unchanged. |
| 57-D3 delete-arm rendering | Unaffected. |
| 57-D4 popup key ownership | **Relied upon** — 58-I11; `open_subagents` sets `UiMode::Switcher` (E13). |
| 57-D5 switcher ordering | Unaffected — applies to the two catalog sources; the Subagents source has no ordering. |
| 57-D6 re-sort while open | Unaffected. |
| 57-D7 badge/name ellipsize | Unaffected. |

## 12. Test plan

ID scheme: `58-U*` unit/model, `58-G*` golden render, `58-H*` harness, `58-P*`
PTY. The implementation phase writes the tests; this spec names files and
assertions. Existing tests that must be updated: `SubagentPanelRendered`
(`tests/unit/ui_render_golden_test.cpp:624-637`) once the strip gains a status
glyph.

### 12.1 Unit — model (`tests/unit/ui_model_test.cpp`)

- **58-U1** `SubagentPathPushPop` — `enter_subagent(a)` pushes; `return_subagent`
  pops; empty path ⇒ `viewedSession() == activeSession()`.
- **58-U2** `SubagentPathClearedOnSessionSwitch` — `focusSessionIn` **and**
  `focusWorkspace` clear `subagent_path` (58-I2).
- **58-U3** `ViewedSessionReturnsDeepestChild` — path `{a,b}` ⇒ `viewedSession()`
  is `b`; a path id with no state returns `nullptr`, never a default-constructed
  state (58-I14).
- **58-U4** `SubagentSpawnedAddsRunningEntry` — applying `SubagentSpawned` adds a
  `SubagentView` with `status == Running` (58-I5).
- **58-U5** `SubagentFanInMarksTerminal` — `Completed/Failed/Cancelled` set
  distinct statuses (58-I6).
- **58-U6** `EnsureSubagentStateHasNoCell` — `ensureSubagentState` creates the
  `SessionUiState` (with `workspace` and `subagent == true`) and **no**
  `SessionCell` (58-I13).
- **58-U7** `PickerListsOnlyViewedChildren` — `openSubagents` builds one node
  whose leaves are exactly `viewedSession()->subagents.agents`; at depth 1 they
  are the viewed child's children, not the active session's (58-I7).
- **58-U8** `RefreshSessionsSkipsSubagents` — a `session.list` entry with
  `kind == "subagent"` creates no cell **and** opens no subscription/track
  (58-D7/58-I22; Rev 3 extends Rev 2's cell-only assertion).
- **58-U9** `SessionOpenedCreatesNoCellForSubagent` — applying a `SessionOpened`
  for a session already materialized as `subagent` creates no `SessionCell`
  (58-I13/E25; Rev 3; R1-N3/R2-N3).
- **58-U10** `CatalogPredicateExcludesSubagent` — `catalog_has_session` returns
  false for an entry whose `kind == "subagent"` (58-I26; amends 45-I5; Rev 3).
- **58-U11** `SubagentPathCannotCycle` — pushing a chain never revisits an id;
  each push requires a direct child (58-I16).
- **58-U12** `ChildFinishWhileViewedStays` — terminal status arrives while
  viewed; the path and the transcript stay (58-D8).
- **58-U13** `PickerClampManyChildren` — 100 children: `clamp_cursor` keeps the
  cursor valid after every resnapshot (58-D8).
- **58-U14** `StaleCursorClamped` — a cursor pointing at a removed child is
  re-clamped (58-D8).
- **58-U15** `SpawnWhileViewingUpdatesStrip` — a `SubagentSpawned` for the viewed
  child's own child appears in the viewed strip (58-D8).
- **58-U16** `SubagentStateErasedOnPop` — after `return_subagent`, the child's
  `SessionUiState` is gone from `UiModel::sessions` (58-I19).
- **58-U17** `HistoryFiltersSubagents` — `openHistory` excludes
  `kind == "subagent"` rows (58-I13; amends 22-D6).
- **58-U18** `ReturnSubagentPopOrder` (Rev 3; HIGH-1) — path `{A,B}`, return B ⇒
  path `{A}` (not main), B's state erased, exactly one pop; path `{A}`, return A
  ⇒ empty path and **no** `pop_back()` on an empty vector (ASan-clean). Exercises
  `eraseSession` with no reconcile re-entry (58-I25/E15/E23).
- **58-U19** `OpenSubagentsNullGuard` (Rev 3; HIGH-2) — with a workspace but no
  session (`activeSession()`/`viewedSession()` null), `open_subagents` pushes the
  `no session to show subagents for` notice, sets no `UiMode::Switcher`, and does
  not crash; `openSubagents` on a null viewed state builds the empty node
  (58-I21/E13).
- **58-U20** `SelectHistoryRefusesSubagent` (Rev 3; MED-9) — a cursor on a
  History node with `kind == "subagent"` pushes the notice and does **not** call
  `resume_from_history` (E20).
- **58-U21** `SwitcherPolicyTable` (Rev 4; P2) — `switcher_policy` returns the
  pinned per-source values (title/heading/footer/empty/enter/ctrl_d/tab/ctrl_t/
  r_refresh) for all three sources, and **no other field exists** (the Rev 3
  `row_source`/`status_prefix` rows are gone; §6.A A3/A3.1, 58-I23).

### 12.2 Unit — adapter (`tests/unit/ui_event_adapter_test.cpp`)

- **58-U22** `SubagentSpawnedAdapted` — `EventType::SubagentSpawned` ⇒ one
  `UiEvent::SubagentSpawned{session, subagent, task}`.
- **58-U23** `SubagentFanInCarriesStatus` — the `SubagentUpdated` carries the
  status derived from `SubagentOutcome`.
- **58-U24** `ForgetSessionClearsDedup` (Rev 4; HIGH-1) — feed the adapter two
  envelopes for session A and one for session B; `forget_session(A)` then re-feeding
  A's envelopes applies them again (they are no longer deduped) while B's remains
  deduped; `forget_session` on an absent id is a no-op (A10/E46).

### 12.3 Golden render (`tests/unit/ui_render_golden_test.cpp`)

- **58-G1** `SubagentBreadcrumbRendered` — path non-empty ⇒ the rendered screen
  contains `↳ `, the active session's root title (or `main`), the short id, and
  the exact hint `Esc return`; with an empty path it does not (58-I3). One
  spelling only.
- **58-G2** `SubagentViewReplacesTranscript` — the child's transcript text is
  present and the main transcript's unique text is absent.
- **58-G3** `SubagentPickerRendered` — the Subagents source renders the
  **window frame** `─ subagents ─` (distinct from the strip's always-rendered
  `subagents:` label, `ui_render.cpp:446`), the heading row, and the footer
  `Enter enter · Esc close` (58-D1/§6.A; Rev 3 adds the heading assertion,
  R1-M2).
- **58-G4** `SubagentPickerEmptyState` — no children ⇒ `(no subagents)`.
- **58-G5** `SubagentStripRunningGlyph` — a Running child renders
  `subagent_status_glyph(Running)` (`>`), distinct from `Completed` (`✓`) and
  `Failed` (`✗`) (58-I6).
- **58-G6** `SubagentComposerReadOnly` — while a child is viewed the composer
  row renders the read-only hint and no draft/caret; `build_ui` passes the
  viewed state to `render_input` (58-I24/E33/E35; Rev 3; MED-11).
- **58-G7** `SubagentBreadcrumbCellWidth` — the breadcrumb row's width is
  measured in **cells** with `ftxui::string_width` (never `ToString().size()`),
  because `↳`/`›`/`✓`/`✗` are multi-byte UTF-8; asserts the right-aligned
  `Esc return` is not clipped at the pinned widths (Rev 2; R4-LOW-9, following
  the 57 cell rule at `ui_render_golden_test.cpp:2557`).
- **58-G8** `SubagentStatusLinePrefix` (Rev 3; R1-N5) — while viewing a running
  child the status bar's left segment starts `subagent <8-hex> · running`; the
  aggregate right segment is unchanged; after a terminal fan-in it reads
  `finished` (E34/§6.A).

### 12.4 Harness (`tests/unit/errata58_ui_test.cpp`)

- **58-H1** `OpenEnterReturn` — `Ctrl+T` opens the picker; `Enter` enters;
  `Esc` returns to main; `activeSessionId` is unchanged throughout (58-I1/I8).
- **58-H2** `EscPrecedenceInView` — `Esc` in the view pops and does not arm the
  interrupt even with an active turn (58-I11, 58-F4; extends 45-I8).
- **58-H3** `PickerConsumesCtrlT` — `Ctrl+T` while the picker is open closes it
  (58-D4).
- **58-H4** `CompletedChildReplays` — with a stored, non-resident child (FakeLLM
  child allowed to finish), entering it renders its transcript (58-I10). This is
  the load-bearing test for §7.
- **58-H5** `DeletedChildPopsWithNotice` — a `SessionClosed`/delete for a path id
  pops the path and pushes the notice (58-I14).
- **58-H6** `ScrollKeysActOnViewedChild` — `PageUp` changes the child's scroll,
  not the parent's (58-I12).
- **58-H7** `ChildNotInLiveSwitcher` — a child `SessionUiState` never appears in
  `SwitcherOverlayModel::open` (58-I13).
- **58-H8** `DaemonDeathClearsPathViaProductionHook` (Rev 3; R1-N1/R3-N3/R4#4/
  R5-N3/Oracle) — **replaces** Rev 2's vacuous test: do **not** inject the
  test-only `DaemonDied` event. Instead drive the production path — a link
  transition to `SupervisorLinkState::Dead` and a presence-scan eviction
  (`on_link_state`/`evict_dead_workspaces`) — and assert the path is reconciled
  and the transcript returns to the active session. A separate assertion records
  that `UiModel::apply(DaemonDied)` alone does **not** clear the path (58-I14/
  E36/E37, 58-F1).
- **58-H9** `PickerDeleteDisabled` — `Ctrl+D` twice in the Subagents picker sends
  **no** `session.delete` and the child survives (58-I17).
- **58-H10** `CtrlCSuppressedInView` — `Ctrl+C` while viewing sends **no**
  `agent.cancel`; the parent turn is untouched (58-F14).
- **58-H11** `UnsubscribeOnPop` — enter then return submits exactly one
  `event.subscribe` and one `event.unsubscribe` for the child; a second
  enter/return pair leaves the tracked/subscribed set at its pre-entry size
  (58-I18).
- **58-H12** `UnknownChildSurfacesNotice` — entering a child whose log was
  deleted yields the `subagent unavailable:` notice and pops the path; the view
  never renders blank (58-I20/F12).
- **58-H13** `NoAgentRpcWhileViewing` — an RPC-spy harness asserts the submitted
  method set while a child is viewed contains **no** `agent.*` method (58-I4;
  Rev 3 removes the `session.create` clause, which `Ctrl+N` legitimately
  produces; R4#5).
- **58-H14** `NestedPushPopReachable` — `Ctrl+T` at depth 1 lists the child's
  children; `Enter` reaches depth 2; `Esc` pops back one level (58-D2; the Rev 1
  unreachable-nesting regression).
- **58-H15** `CtrlCCancelsAtRootOnly` — `Ctrl+C` at depth 0 still cancels;
  at depth ≥ 1 it is consumed (58-D4).
- **58-H16** `HistorySelectSubagentRefused` — even if a subagent row reaches the
  History cursor, `select_history` refuses it with the notice and does not change
  `activeSessionId` (58-D7/E20; a UI-scope guard, not a 55-D3 prohibition).
- **58-H17** `SlashSubagentsDispatches` (Rev 3; R2-N2/R3-N2) — typing
  `/subagents` dispatches the registered command and opens the picker (or pushes
  the no-session notice); it is **not** `unknown command`. Covers E10/E11/E12.
- **58-H18** `PickerTabNoCollapse` (Rev 3; R1-L1/MED-6) — `Tab` in the Subagents
  picker is a consumed no-op; the single synthetic node stays expanded and a
  following `Enter` enters the child rather than hitting the Live fallthrough
  (E2/58-F17).
- **58-H19** `CtrlTDoesNotCloseCatalogSwitchers` (Rev 3; R3-N5) — with the
  Live or History switcher open, `Ctrl+T` does **not** close it; with the
  Subagents picker open it does (E3).
- **58-H20** `EnterEmptyLeafStaysOpen` (Rev 3; R4#3/MED-10) — with a session
  that has zero children, `/subagents` then `Enter` leaves `UiMode::Switcher`
  and the picker open (E4/58-I8).
- **58-H21** `ViewSwitchReleasesChild` (Rev 3; MED-12) — enter a child, then
  `Ctrl+N`/`Ctrl+S`: the view clears via 58-I2 and the child's subscription is
  released (`sync_subagent_subscriptions`), leaving the tracked set at its
  pre-entry size (58-I22/E16/E22).
- **58-H22** `ReentryRendersContent` (Rev 4; **HIGH-1**) — `Ctrl+T`, `Enter` a
  child, assert its transcript renders, `Esc`, `Ctrl+T`, `Enter` the **same**
  child, and assert the transcript **content** renders again (not merely the
  subscribe count H11 checks). This is the regression test for the global-dedup
  blank-render defect; it fails without `forget_session` (58-I10/I19; A10/E46;
  58-F20).
- **58-H23** `DeleteViewedChildReconciles` (Rev 5; R1-M2/R3-M1; qualified Rev 6
  R3-L1) — view child C, then exercise the two external paths that can remove a
  viewed child: (1) **inject** an external-client `session.delete` reply for C
  (`apply_session_deleted`, E43) — no supervisor UI action can delete a viewed
  child, so this is a test seam; (2) kill C's workspace daemon so the presence
  scan evicts it (E45/E37). In both, the path is reconciled,
  `viewedSession()` is non-null, `build_ui` does not deref a stale path, and the
  transcript returns to a valid session (58-I14; E35). The test also asserts the
  **unreachable** path is refused: `Ctrl+D` on C's workspace in the Live switcher
  yields the `stop the workspace first` notice and never calls
  `apply_workspace_deleted` (E44; `supervisor.cpp:2955-2956`). This replaces Rev
  4's `Ctrl+D`-twice scenario, which could not run (children are in neither
  switcher source).

### 12.5 PTY / live

- **58-P1** `SubagentNavigationPty` — **hermetic**, in
  `tests/unit/ui_supervisor_pty_test.cpp` (FakeLLM): spawn a continuable child,
  `Ctrl+T`, `Enter`, assert the child's transcript and breadcrumb render, `Esc`
  returns. **Must not be gated on `YMH_LIVE_LLM`** (that file has no
  `live_enabled()` and `tests/support/host_harness.hpp:110` strips the variable;
  gating a FakeLLM test on it is spec 57's exact bug class). Rev 2 fix for
  R4-H2.
- **58-P2** (opt-in, `YMH_LIVE_LLM=1`) — **`tests/unit/ui_live_pty_test.cpp`**
  (`:24-29`), against real DeepSeek: delegate a task, enter the running child,
  observe streaming, let it finish, observe `finished`, return. Rev 2 moves this
  off the hermetic file (R4-H2).

## 13. Open questions

None. Every design choice above is decided and recorded (§58-D1–D9, §5); every
Rev 1–5 gate finding is dispositioned in §14; and §11.2 accounts
for every invariant in the six touched specs. The spec is `draft` and must pass
the component gate before any code (AGENTS.md).

## 14. Revision log

- **Rev 6 (2026-09-26)** — fix pass, applying the Rev 5 re-check (R1–R5: three
  PASSes, two DO NOT APPROVEs on two narrow MEDIUMs). Every change is inside the
  normative §6.A/§6.B/§6.C or its §11/§12 echo; no `src/`/`tests/` file changed.
  The Rev 6 re-check then returned all five reviewers **PASS** and Oracle's final
  pass **PASS**, closing the gate — the spec is **verified (Rev 6)**. The final
  gate sweep also corrected four documentation-only LOWs (the Rev 5 scope
  paragraph no longer counts `45-I25` among the suspensions, which are **3**; the
  Rev 3/4 log no longer pins the daemon-death clear to the inert
  `on_link_state(Dead)`; E16's `viewed_children_` wording matches its type; and
  the `DESIGN_STATUS.md` row is updated).
  - **MEDIUM-A — link-`Dead` prose contradicted the table.** `58-D8`'s first
    clause and `58-I14` now say the path is cleared by the **eviction** (`E37`),
    not the link transition (`E36` is inert while the child state survives);
    §6.C's `applied_event_ids_` daemon-death cell splits the halves (link `Dead` =
    unchanged; eviction = `forget_session`) like `subscriptions_`/`tracked_`
    (R2-M1; R1-L5).
  - **MEDIUM-B — the E37 "`connection_for(ws)` is nullptr" justification was
    false.** `viewed_children_` is now `map<SessionId, WorkspaceId>`, recording
    each child's owning workspace at **track time** (A5). `eraseWorkspace` erases
    the child state and promotes `activeWorkspaceId` (`ui_model.cpp:861-873`)
    before sync, so the old derived lookup hit the **promoted** workspace's
    connection and untracked there; the recorded workspace makes
    `connection_for(child_ws)` genuinely `nullptr` after `connections_.erase`
    (`supervisor.cpp:1579`). Corrected in A5's E37 note, E37, and §6.C (R3-M1).
  - **LOWs:** A5 track direction genuinely idempotent (`track` re-attempted;
    `SupervisorConnection::track` dedups `tracked_`,
    `supervisor_connection.cpp:64-73`) (R2-L2); E25's §5 rationale corrected
    (R1-L1); parent-`SubagentModel` delete cell cites `F12` only (R1-L2);
    E36/E37/E43/E44/E45 drop the redundant explicit sync (R1-L3); `45-I25`
    recorded as a pre-existing `51-D4`/`51-A4` supersession and the §11.2 count is
    **3** (R5-LOW-1); §6.C `delete (external)` → `close/delete (external)` with
    `E17` added and `E43` qualified (R3-L1); `delete_arm` daemon-death cell made
    exact for a Subagents picker (R3-L2).
- **Rev 5 (2026-09-26)** — fix pass, applying the Rev 4 re-check (R1–R5 +
  Oracle). No finding was disputed. Every change is inside the normative
  §6.A/§6.B/§6.C or its §11/§12 echo; no `src/`/`tests/` file changed and the
  spec is still **not verified**.
  - **HIGH — `connection` undeclared + E37 ordering:** A5 now declares and
    defines `SupervisorConnection* SupervisorApp::connection_for(const WorkspaceId&)`
    (null-guarded over `connections_`, `supervisor.cpp:3547`), and
    `sync_subagent_subscriptions` uses it for both `untrack` and `track`. The E37
    call is **after** `evict_dead_workspaces`' `connections_.erase` (`:1579`), so
    the guard makes the subscription calls no-ops while the model/adapter cleanup
    still runs; a reorder is rejected because `reconcile_subagent_path()` only
    pops once `eraseWorkspace` has removed the state. §6.C's daemon-death column
    agrees (link `Dead` vs eviction).
  - **MEDIUM — §6.C delete column / `58-H23`:** the column is `delete (external)`
    (`session.delete` E43 / scan eviction E37/E45); `Ctrl+D` cannot delete a
    viewed child (children in neither source; `delete_highlighted_workspace`
    refuses a Live source and a live daemon, `:2955-2956`). `E44` is recorded
    defensive/unreachable with a viewed child; `58-H23` renamed
    `DeleteViewedChildReconciles` and drives E43/E37/E45 plus the refused-`Ctrl+D`
    assertion.
  - **MEDIUM — `source ==` arithmetic:** corrected to **four removed** (E4
    `:2805`, E32 `:2796`, E5 `ui_render.cpp:960`/`:1148`) and **six remaining**
    (`:956`, `:959`, `:1236`, `:2955`, `:3014`, `ui_model.cpp:875`) in §4, A3,
    A3.1, A3.2 and `58-I23`.
  - **MEDIUM — §11.2 false "unaffected":** `45-I2`/`45-D1` are suspended in the
    subagent view (E8 consumes ArrowUp/ArrowDown) and named in §11.1; the
    re-audit also scoped `45-D2` (the `/` list) and `45-I25` (Ctrl+D exit
    confirmation), and fixed `57-I13` to Scoped. The §11.2 preamble accounts for
    the four.
  - **LOWs:** A10's `forget_session` caller list drops `close`; E38 names
    `handle_disconnect` (`supervisor_connection.cpp:449`); §6.C's picker-cursor
    row cites `ui_model.cpp:1339`/`:1425`; E25's rationale corrected (children
    emit no `SessionCreated`, `agent_registry.cpp:149`); E42's duplicate
    `:1463` sync removed (E22 owns it); A7's `node.title` uses the workspace label
    and its `source` anchor is `supervisor.cpp:968`/`ui_model.cpp:1211`; A6 adds
    the pop-vs-in-flight-subscribe guard so `58-I18` holds.
- **Rev 4 (2026-09-26)** — restructure + fixes, applying the Rev 3 Oracle pass
  and the five Rev 3 re-checks (R1–R5). No finding was disputed. The primary
  change is structural: §6 is split into **§6.A** (new symbols: declaration +
  body + callers), **§6.B** (`E1`–`E47`), and **§6.C** (state-lifetime table),
  so new symbols and state lifetimes are pinned instead of sketched.
  - **HIGH-1 (re-entry renders blank):** `applied_event_ids_` was one
    adapter-global set that no transition cleared, while `E15` erased the child
    state on pop, so a re-entry replayed into a set that already held every id.
    Fixed by keying the dedup **per session** and adding
    `UiEventAdapter::forget_session(id)`, called on every pop/switch/close/
    delete/death (§6.A A10, §6.B E46, §6.C); §7's false "per-materialized-state"
    claim corrected; `58-U24`/`58-H22` assert re-entry **content**.
  - **MEDIUM-1 (`viewed_children_`):** declared and populated via
    `sync_subagent_subscriptions()`; all call sites pinned (§6.A A5, E22).
  - **MEDIUM-2 (`I23`/E4/policy):** `E4` now dispatches on `policy.enter`; the
    History `r`-refresh is a policy field (E32); `I23`'s consumer list is the
    six real consumers; `row_source`/`status_prefix` deleted as dead fields.
  - **MEDIUM-3 (picker-source status prefix):** `render_status` no longer reads
    `switcher.source`; it uses `subagent_status_prefix` on the viewed child (E34,
    §6.A A8).
  - **MEDIUM-4 (`disarm(id)`):** defined as `UiModel::disarm` (§6.A A4/E47).
  - **MEDIUM-5 (§6.B not exhaustive):** the three missing
    `eraseSession`/`eraseWorkspace` sites (plus the `on_scan` site) get E42–E45;
    `E35` falls back on a **missing** state too; `58-H23` covers it.
  - **MEDIUM-6 (`ws`):** defined as `model_.activeWorkspaceId` (§6.A A5).
  - **MEDIUM-7 (`50-I9` reason):** corrected — 55-D3 **permits** resuming a stored
    child; the `/sessions` filter is a UI-scope decision (58-D7), and
    `--resume <child>` remains unfiltered.
  - **LOWs:** `E38` pins all three `subscribed_.clear()` sites (`:214/:449/:488`);
    `subagent_status_glyph`/`reconcile_subagent_path_for` declared in §6.A;
    `E32` repurposed (was a duplicate of E5); `I23` no longer claims "close keys"
    beyond `ctrl_t_closes`; `E14` marks dirty; `E36`'s inertness stated; the
    `DESIGN_STATUS.md` row updated.
- **Rev 3 (2026-09-26)** — applies the Rev 2 re-check (R1 UI, R2 data source,
  R3 key contract, R4 invariants/tests, R5 conformance, Oracle). No finding was
  disputed. Resolutions:
  - **P1 (normative §6):** §6.B is now the exhaustive numbered `E1`–`E41` edit
    list (adding the `/subagents` Command registration E10–E12, `refresh_sessions`
    E18, `select_history` E20, `openHistory` E19, `render_input` E33, the
    status-line prefix E34, the reconcile call sites E16/E17/E36/E37, the Tab
    gate E2, and the `Ctrl+T`-close gating E3); §2/§11/§14 cite `E*`.
  - **P2 (`SwitcherSourcePolicy`):** §6.A (A3.1) is the per-source policy table; D9 is
    rewritten to "split the policy, keep the widget"; §58-I23 forbids ad-hoc
    `source ==` branches (Oracle Q1).
  - **HIGH-1 (R3-N1):** `return_subagent` pops **before** erasing; `eraseSession`
    no longer reconciles (E15/E23); 58-I25; 58-U18.
  - **HIGH-2 (R4#1):** `open_subagents` null-guards and pushes a notice;
    `openSubagents` null-guards; 58-I21/E13; 58-U19/58-F16.
  - **MED-1 (daemon death):** the clear is pinned to the **eviction**
    (`evict_dead_workspaces`, E37) — `on_link_state(Dead)` (E36) is inert — not
    the production-dead `DaemonDied`; 58-H8 is replaced with a production-path
    test.
  - **MED-2 (`/subagents`):** `CommandContext::subagents` + the `builtin()`
    `subagents` Command pinned (E11/E12); 58-H17.
  - **MED-3 (`45-I5`):** `catalog_has_session` is kind-aware (E27); 45-I5 named.
  - **MED-4 (`57-I3`) / MED-5 (`57-I9`):** both named and scoped to the catalog
    sources; the Subagents footer/delete are policy data.
  - **MED-6 (`45-I24`) / MED-7 (Tab, `45-D9`/`45-I20`/`45-I3`):** the Tab gate
    (E2) and the read-only-guard scope (E8) are pinned and named.
  - **MED-8 (reconcile ownership):** reconcile is a `SupervisorApp` method;
    `UiModel::eraseSession`/`eraseWorkspace` never call it and never re-enter
    (E16/E23).
  - **MED-9 (`refresh_sessions`/`select_history`/`openHistory`):** pinned
    (E18/E19/E20); `select_history` looks up the node kind from the switcher
    node.
  - **MED-10 (empty-leaf Enter):** pinned to stay open (E4); 58-H20.
  - **MED-11 (`G6`/`render_input`):** `render_input`/`build_ui` pinned
    (E33/E35); 58-I24/58-G6.
  - **MED-12 (tracking contradiction):** one rule — `refresh_sessions` skips
    before `track()` and cells (E18), `SessionOpened` creates no cell (E25),
    path shrinks release via `sync_subagent_subscriptions` (E16/E22); 58-I22;
    58-U8/U9/U21.
  - **MED-13 (`R1-M2` heading):** sketch (b) now shows the pinned heading row.
  - **MED-14 (`50-I9`):** named/narrowed for the `/sessions`/Ctrl-S surfaces.
  - **Invariant sweep:** §11.2 accounts for all 208 invariant/decision items in
    22/45/50/51/55/57 (12 named, the rest proven unaffected).
  - **LOWs:** `58-H13` drops the `session.create` clause (R4#5); Tab collapse
    (E2); status-line prefix (E34); `Ctrl+T` close gated to Subagents (E3);
    the 8192 dedup bound noted as per-state (§7); `subscriptions_` cleared on
    reconnect (E38); `openSubagents` disarms (E13); heading/string mismatches
    fixed; tmux rationale corrected (`Ctrl+B Ctrl+B`).
- **Rev 2 (2026-09-26)** — applied the six-reviewer gate (R1 UI, R2 data source,
  R3 key contract, R4 invariants/tests, R5 conformance, Oracle). No finding was
  disputed. Resolutions: nesting at the current level; delete disabled for the
  Subagents source; `Ctrl+C` suppressed; the `Return` branch pinned; path cleared
  by `focusWorkspace`/`focusSessionIn` and reconciled; the History builder
  filters subagent rows and `select_history` refuses a child; the implementation
  edits pinned; `untrack` sends `event.unsubscribe` and `on_subscribe_error`
  surfaces failures; `45-I8`/`45-D5` named; the test plan moved `58-P2` to the
  live file; child states erased on pop; `58-H13` RPC-spy and `58-G7` cell-width.
- **Rev 1 (2026-09-26)** — initial draft. Part A (UI) then Part B
  (implementation). Decisions 58-D1–58-D8; invariants 58-I1–58-I16; failure
  modes 58-F1–58-F12. No new RPC, no new persistence. Rejected by the gate (≥5
  HIGH).

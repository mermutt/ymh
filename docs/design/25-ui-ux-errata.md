# 25 — UI/UX Errata: Status Line, Command Palette, Exit, Skills, Plan Mode, and Localcode Import

```
Status: draft (Rev 5) — NOT verified. No implementation until the re-gate passes
        with zero open HIGH/MEDIUM findings. Rev 5 is intended to be
        verification-complete (0 HIGH, 0 MEDIUM open); only the gate may declare
        it verified.
Revision: Rev 5 (re-gate round 3: M-1 memo re-keyed on the controller's own
        commits, NEW-1 malformed-plan exit review closed, NEW-2 plan preview
        line/height-bounded, NEW-3 force-ask dialog offers no dead persistent
        grant; plus the cheap LOWs in §15)
Component: 25 (errata) — amends 01-session.md, 06-agent-loop.md,
           10-supervisor-tui.md, 15-mcp-adapter.md, 17-ui-transcript-errata.md,
           20-skills.md, 21-config-jsonc-errata.md, 22-switcher-sessions-errata.md
Depends on: 00-architecture.md (verified), 01-session.md (verified),
            02-persistence.md (verified), 06-agent-loop.md (verified),
            07-tools-execution.md (verified), 08-llm-provider.md (verified),
            09-permissions.md (verified), 10-supervisor-tui.md (verified),
            13-context-compaction.md (verified), 15-mcp-adapter.md (verified),
            17-ui-transcript-errata.md (verified),
            20-skills.md (verified), 21-config-jsonc-errata.md (verified),
            22-switcher-sessions-errata.md (verified)
Scope: the eight user requirements below, resolved into pinned decisions
       (25-D1…25-D16), invariants (UX1–UX47), failure modes (UX-F1–UX-F22), pinned
       C++ interfaces, and a test plan. Rev 5 re-keys the plan projection memo on
       the controller's own commits (not the session's last `Sequence`), closes the
       malformed-plan blind review, bounds the plan preview by lines as well as
       bytes so the dialog options stay visible, and suppresses the dead
       "Always allow" grant on a forced review (one additive `force_ask`
       wire/event field). Rev 2 added ONE durable session event type
       (`plan/mode`), one additive RPC (`session.set_mode`), and one always-registered
       tool (`exit_plan_mode`). Rev 3 pins the exit-review preview path (a
       `bounded_summary` `exit_plan_mode` branch — no protocol change), moves the
       `PlanModeController` into `WorkspaceRuntime` with an `AgentServices::plan_mode`
       seam, makes the MCP-id dedupe terminate, adds pre-write semantic validation to
       the localcode import, and makes `exit_plan_mode` non-destructive. Rev 4 pins
       the forced review to a new non-mutating `PermissionRequest::force_ask`
       (replacing the unimplementable "loop forces Ask"), forces `required=false` on
       every imported MCP server (closing the last import brick path), and runs the
       queued-plan flush from an all-exit scope guard so error/cancel cannot lose a
       selection. No persistence schema change, no `kProtocolVersion` bump. Plan mode
       follows the DeepSeek Harness (dsh) design: it is logged collaboration state,
       not a capability restriction.
```

---

## 1. Purpose, scope, and supersession map

### 1.1 The eight requirements (verbatim)

1. *"Instead of showing 'idle' in the bottom left corner, dislpay `agent` (plan or
   build) followed by model name, then current up/down and ligtning numbers,
   followed by TPS and context consumption percentage with a line like 10 chars
   with solid vs gray chars representing consumed/total context."*
2. *"Bug: when enter '/' and <tab> the autofilled command is out of sync with the
   highlighted item in the list of commands. It is one command behind"*
3. *"When user typed '/' and then let's say 'sk' or press <tab> some item from the
   list will be highlit. If user hits <Enter> - autopopulate the highlighted
   command and execute it."*
4. *"When user entered /exit command exit from app without pop up dialogue. Leave
   the confirmatin dialogue only for Ctlr+d"*
5. *"Add a featue: On first start when ~/.config/ymh dir is not present but
   ~/.localcode/config.json is present ask user (in command line before UI) if he
   wants to automatically create ymh settings from localcode settings. You can make
   this verbage better than I did"*
6. *"Delete /skill command leave only /skills - 'list available skills'"*
7. *"make /quit an alias to /exit"*
8. *"Use the same 'mcp_servers' structure in ~/.config/ymh/config.json as in
   ~/.localcode. So that initial and subsequent copying of localcode config will be
   easy."*

The user also directed: *"do it all without stopping to ask, and commit when done."*
Spec 25 therefore **makes and records** every decision it needs; §14 lists the
interpretations a human should review. (Committing is an execution step, not a
design step; the spec author does not touch git.)

The user later directed that ymh's plan mode **follow the DeepSeek Harness (dsh)
design** rather than an invented agent-mode/read-only-seam design. Rev 2 implements
that directive (see §1.2 and §15).

### 1.2 What this changes, in one sentence

The bottom status line is redesigned around a per-session, **event-sourced dsh plan
mode** (logged `plan/mode`, a `plan:policy` prompt section, an always-registered
`exit_plan_mode` review tool, and `/plan` / `/plan off`), plus TPS and a 10-cell
context bar; the slash-command palette's off-by-one highlight is fixed and Enter
accepts the highlight; `/exit` stops prompting while Ctrl+D keeps the prompt;
`/skill` is deleted and `/skills` remains; `/quit` becomes a real alias of `/exit`;
and the config file gains a localcode-compatible top-level `mcp_servers` object plus
a first-run importer that can seed the ymh config from `~/.localcode/config.json`.

**Plan mode does not restrict tools.** It is guidance plus a review gate. Capability
enforcement stays where it already lives: `SandboxMode::ReadOnly`
(`include/ymh/execution/environment.hpp:22-26`), `tool_is_mutating`
(`src/policy/permission_policy.cpp:160-174`) and its enforcement in
`RulePermissionPolicy::evaluate` (`src/policy/permission_policy.cpp:204-207`). Plan
mode never reads or writes sandbox mode or the approval policy.

### 1.3 Supersession map

#### 1.3.1 Superseded / withdrawn

| ID | Prior text | Change |
|---|---|---|
| 25-S1 | `10-supervisor-tui.md` status-line rendering: the left segment is `state_name(agent_state)` and the literal `"idle"` when no session is active (`src/ui/ui_render.cpp:359`). | **Superseded by 25-D1**: the leading segment becomes the plan label (`plan`/`build`); the activity state is retained only when non-idle; token counters always render for an active session; `status.note` and `model.notices` are retained as trailing segments. |
| 25-S2 | `10-supervisor-tui.md` / `17-ui-transcript-errata.md` §5 RB-08 completion semantics: Tab inserts `names[applied]` but the highlight follows `cycle->index` (`src/ui/supervisor.cpp:1726` vs `:1730`). | **Superseded by 25-D8**: the highlight tracks the inserted candidate (`applied`). |
| 25-S3 | `17-ui-transcript-errata.md` §5 Enter semantics: Return dispatches the raw `input.draft` and never consults `command_hint_selected` (`src/ui/supervisor.cpp:1922-1937`). | **Superseded by 25-D9**: Enter accepts and executes the highlighted candidate when the draft is a non-empty bare command prefix. |
| 25-S4 | `20-skills.md` §5.8 / §6.1 / §7.2: the `/skill <name>` activation command and its `CommandContext::skill` callback. | **Superseded by 25-D11**: `/skill` is deleted; `/skills` (list, `--show`) remains. The model-invoked `skill` tool is unchanged. |
| 25-S5 | `21-config-jsonc-errata.md` §6 top-level allowlist `{ui, agent, workspace, permissions, logging, llm, mcp, skills}` (`src/config/config.cpp:537-539`). | **Superseded by 25-D13**: `mcp_servers` joins the allowlist; the `[mcp].server` array remains accepted (deprecated, mutually exclusive per layer). |
| 25-S6 | `15-mcp-adapter.md` §4.1/§5.6 MCP server config: `transport` + array `env` of `"K=V"` strings, one `id` per array element. | **Amended by 25-D13**: the new `mcp_servers` object map accepts localcode's `type`/`env` object and normalizes the key to a valid ymh id; the old array form still loads. |

#### 1.3.1a Rev-1 decisions withdrawn in Rev 2

| Withdrawn | Rev-1 content | Why withdrawn (Rev 2) |
|---|---|---|
| 25-W1 | 25-D3 `ToolSchema::mutating` capability | **dsh plan mode does not restrict tools.** Enforcement is the existing `SandboxMode::ReadOnly` + `tool_is_mutating` path; a parallel capability flag is dead weight and was the root of gate H1. |
| 25-W2 | 25-D4 Plan-mode classification (read-only allowlist, shell denied, MCP conservative) | Same as 25-W1. No tool is denied because of plan state. |
| 25-W3 | 25-D2 `AgentMode{Plan,Build}`, `SessionModeStore`, in-memory per-session mode, `session.set_mode{mode}` | Replaced by the dsh design: durable `plan/mode` event + projection (25-D2), boolean active, no `Build` enum value. |
| 25-W4 | 25-D15 `/build` command | There is no build concept in dsh. `build` survives only as ymh's display label for the inactive state (§14 Q1); leaving plan mode is `/plan off`. |
| 25-W5 | 25-D12 `read_only` server field + MCP `readOnlyHint` plumbing (`McpServerSettings::read_only`, `McpToolInfo` read-only field, `read_only` parse entry) | Existed only to serve 25-W2. Removed with it; `mcp_servers` keeps no `read_only` key. |
| 25-W6 | 25-D2 `session.set_mode` query/set of an in-memory store | Superseded by 25-D5: `session.set_mode {session, active}` appends the durable `plan/mode` event; the reply echoes the effective state. |

#### 1.3.2 Amended

| ID | Amended clause | Change |
|---|---|---|
| 25-A1 | `10-supervisor-tui.md` §status line; `17-ui-transcript-errata.md` token-counter line; `22-switcher-sessions-errata.md` §3.6 | New segment set and ordering (note + notice retained); `render_status` gains a width parameter and a pinned overflow policy; bar geometry precedence fixed (25-D1, 25-D6, 25-D7). |
| 25-A2 | `10-supervisor-tui.md` command palette (§5.3-ish) | Highlight/insert sync and Enter-accepts-highlight; bare `/` + Enter keeps the listing (25-D8/D9). |
| 25-A3 | `10-supervisor-tui.md` exit flow; `16-daemon-ownership.md` §4.2 callers | `/exit` bypasses the confirmation dialog; Ctrl+D keeps it (25-D10). The spec-16 orphaning set and teardown are unchanged. |
| 25-A4 | `10-supervisor-tui.md` command registry; `20-skills.md` §5.8 | `Command` gains an `aliases` field; `/quit` is an alias of `/exit`; `/plan` is added; `/build` is not (25-D9/D12, 25-D5). |
| 25-A5 | `20-skills.md` §6.1 command list | `/skills` description becomes exactly "list available skills"; `/skill` is removed (25-D11). |
| 25-A6 | `01-session.md` §4.4/§4.5 durable event set | Additive event type `PlanMode` (wire `plan/mode`), payload `{active: bool}` (25-D2). |
| 25-A7 | `06-agent-loop.md` permission step | `exit_plan_mode` is routed through the generic permission Ask flow while plan mode is active (forced via `PermissionRequest::force_ask`; `destructive=false`); the dialog summary is the 4096-byte plan preview; approval records a pending exit applied at the next accepted step boundary or on any turn exit (25-D4, N3/N5). |
| 25-A8 | `06-agent-loop.md` / `08-llm-provider.md` system-prompt assembly | `SessionContextAssembler` gains an optional `plan:policy` prompt-section provider; config gains `agent.plan.section` (25-D3). |
| 25-A9 | `15-mcp-adapter.md` §4.1/§5.6 | New `mcp_servers` object shape; full localcode key set (`type`, `command`, `args`, `env`, `url`, `headers`, `cwd`); key normalization; no `read_only` (25-D13). |
| 25-A10 | `21-config-jsonc-errata.md` §6 (config load gate) | First-run localcode import runs in the CLI layer before scaffolding, TUI-only, interactive-only, with a size bound and 0600 temp+rename (25-D14/D15). |
| 25-A11 | `09-permissions.md` permission request; `06-agent-loop.md` permission step | Additive non-mutating `PermissionRequest::force_ask`; `RulePermissionPolicy::evaluate` returns `Ask` when it is set (after the `SandboxMode::ReadOnly` deny, before rules/`tool_defaults`/`default_verdict`/grants). Used only by `exit_plan_mode` while plan is active (25-D4, N3/N44). |

#### 1.3.3 Retained (explicitly not changed)

- The aggregate right-hand widget `"<N> active · <N> waiting"` and its flash
  (`src/ui/ui_render.cpp:378-383`, spec 22) is **kept** (25-D1).
- `state_name` (`src/ui/ui_render.cpp:96-114`) and `AgentState`
  (`include/ymh/agent/agent.hpp:35-43`) are unchanged; only `render_status` stops
  rendering the idle word.
- `status.note` and `UiModel::notices` remain **rendered** in `render_status`
  (spec 22 §3.6 / 22-A8, `docs/design/22-switcher-sessions-errata.md:148,1155-1156`);
  Rev 1's rebuild dropped them, Rev 2 restores them as trailing segments (gate H2/H3).
- The spec-16 exit machinery (`begin_exit`, `compute_orphaning_set`,
  `open_exit_prompt`, `confirm_exit`, `teardown_daemons`,
  `src/ui/supervisor.cpp:446-616`) is unchanged except for the caller split.
- The existing enforcement layer is unchanged and **not** coupled to plan mode:
  `SandboxMode::ReadOnly` (`include/ymh/execution/environment.hpp:22-26`),
  `tool_is_mutating` (`src/policy/permission_policy.cpp:160-174`), the pre-rule
  deny (`src/policy/permission_policy.cpp:204-207`), and `McpServerSettings`
  (`include/ymh/config/config.hpp:112-129`) gains no `read_only` field.
- The `[mcp]` tuning section (timeouts, reconnect, `max_servers`, …) is unchanged.
- No persistence schema change: `plan/mode` rides the existing generic event table
  (`02-persistence.md` is not amended); the only schema-adjacent change is the
  additive `EventType` enumerator (25-A6).
- The `/context` overlay (spec 18) is unchanged; 25 only reads its RPC reply.
- The `skill` **tool** (model-invoked, `include/ymh/skills/skill_tool.hpp`) is
  unchanged; only the `/skill` slash command is deleted.

### 1.4 Scope boundaries

- **In scope**: `src/ui/ui_render.cpp`, `src/ui/ui_model.cpp`, `src/ui/supervisor.cpp`,
  `src/ui/command_registry.cpp`, `src/ui/ui_event_adapter.cpp`,
  `include/ymh/ui/{ui_model,ui_event,ui_render,command_registry}.hpp`,
  `include/ymh/agent/{context_assembler.hpp,agent_loop.hpp,agent.hpp,workspace_runtime.hpp}` +
  `src/agent/{context_assembler.cpp,agent_loop.cpp,workspace_runtime.cpp}`,
  `include/ymh/session/events.hpp` + `src/session/events.cpp` + `src/session/session.cpp` +
  `include/ymh/session/plan_mode.hpp (new)`,
  `include/ymh/core/event.hpp` + `src/core/event.cpp`,
  `include/ymh/agent/plan_mode_controller.hpp (new)` +
  `src/agent/plan_mode_controller.cpp (new)` (owned by `WorkspaceRuntime`, so it
  lives in `agent/`, not `host/`),
  `include/ymh/host/host_runtime.hpp` + `src/host/host_runtime.cpp`,
  `src/tools/plan_tools.cpp (new)` + daemon tool registration,
  `include/ymh/permission/permission_broker.hpp` + `src/permission/permission_broker.cpp`
  (the `exit_plan_mode` summary branch, 25-D4),
  `include/ymh/mcp/{mcp_manager.hpp,mcp_types.hpp}` + `src/mcp/{mcp_manager.cpp,mcp_types.cpp}`
  (id normalization/dedupe and the extracted `validate_mcp_server`, 25-D13/D16),
  `include/ymh/transport/protocol.hpp` + `src/transport/{protocol.cpp,protocol_server.cpp}`,
  `include/ymh/config/config.hpp`, `src/config/config.cpp`,
  `src/mcp/mcp_transport.cpp` (the `${VAR}` escape only),
  `include/ymh/cli/{cli.hpp,wiring.hpp}` + `src/cli/{cli.cpp,wiring.cpp}` (the built-in
  `plan_section` default), and the tests named in §13.
- **Out of scope**: remote transport, TOML (retired — a leftover `config.toml`
  stays invisible, 21-D11), LSP tools, the switcher/sessions model (spec 22),
  context compaction (spec 13), and any change to the durable event log **schema**.
- **Explicitly not built**: a plan-mode tool filter, a `ToolSchema::mutating` field,
  a per-server `read_only` config key, an `AgentMode` enum, a `SessionModeStore`,
  and a `/build` command.

### 1.5 Terminology (pinned)

| Term | Meaning |
|---|---|
| **Plan mode** | dsh-style per-session collaboration state. `active=true` adds the `plan:policy` prompt section and enables the `exit_plan_mode` review; it restricts no tool. |
| **plan/mode** | The durable session event (`payload::PlanMode{active}`) that is the single source of truth. Last event wins; a log with none folds to inactive. |
| **Projection** | A pure fold over the session event log. `plan_mode_active(events)` restores plan state on resume/fork/replay. |
| **Exit review** | The user-facing approval presented when the model calls `exit_plan_mode` while active; delivered through the generic permission Ask/Allow/Deny flow. |
| **TPS** | Throughput of the most recently completed assistant message: its `usage.output_tokens` divided by the wall time between `AssistantMessageStarted` and `AssistantMessageFinished`. |
| **Context consumption** | `context_used_tokens / context_window_tokens` for the active session, taken from the existing `context.show` snapshot. |
| **Palette** | The slash-command completion list (`command_hints`) and its highlight (`command_hint_selected`). |
| **Import** | The first-run localcode→ymh config generation described in 25-D14/D15. |
| **build** | ymh display label for the inactive state only. Not an enum, not a command, not a mode. |

---

## 2. Amendment register

| ID | Decision | Amends |
|---|---|---|
| 25-D1 | Status-line segment order (mode, state, model, counters, tps, context, note, notice), always-on counters, width/overflow policy | 10, 17, 22 |
| 25-D2 | Durable `plan/mode` event + `plan_mode_active` projection | 01, 06 |
| 25-D3 | `plan:policy` prompt section + `agent.plan.section` config | 06, 08, 21 |
| 25-D4 | `exit_plan_mode` tool + generic review flow + step-boundary exit | 06, 07 |
| 25-D5 | `session.set_mode` RPC + `/plan` / `/plan off` commands | 06, 10 |
| 25-D6 | TPS computation + dirty marking | 10 |
| 25-D7 | Context-consumption source and 10-cell bar (geometry precedence fixed) | 10, 13, 18 |
| 25-D8 | Palette highlight off-by-one fix | 10, 17 |
| 25-D9 | Enter accepts the highlighted command; bare `/` keeps the listing | 10, 17 |
| 25-D10 | `/exit` skips the dialog; Ctrl+D keeps it | 10, 16 |
| 25-D11 | Delete `/skill`; keep `/skills` | 20 |
| 25-D12 | `Command::aliases`; `/quit` → `/exit` | 10 |
| 25-D13 | localcode-compatible `mcp_servers` (full key set, key normalization, `${VAR}` escape, old-shape migration) | 15, 21 |
| 25-D14 | First-run import trigger, wording, default, decline, malformed/oversized handling | 21 |
| 25-D15 | Import mapping, validation, directory creation, secret handling, 0600 temp+rename | 21 |
| 25-D16 | Import semantic (daemon-startup) validation: per-server skip + whole-document backstop | 15, 21 |
| **Withdrawn** | 25-W1…25-W6 (§1.3.1a) — do not implement | — |

Rev 3 amends four already-registered decisions without changing their numbers:
25-D2 (controller ownership + `AgentServices::plan_mode` seam), 25-D4 (exit-review
preview through `bounded_summary`, `destructive=false`, forced Ask), 25-D5
(the RPC reply never writes `plan_active`; turn-end flush), and 25-D13
(terminating id dedupe). 25-D16 is new. Every Rev-3 change and the re-gate finding
it closes is listed in §15.

Rev 4 amends 25-D4 (the forced review is a new non-mutating
`PermissionRequest::force_ask` + `RulePermissionPolicy::evaluate` short-circuit,
replacing the unpinnable "loop forces Ask"; `exit_plan_mode` short-circuits before
the permission block when plan is inactive) and 25-D16 (the importer forces
`required=false` and pins the validation predicate/precedence and the
no-bad-global-config guarantee), and corrects the 25-D2 ownership text
(`erase` runs in `deleteSession`, not the detach-only `closeSession`). The
turn-end flush becomes an all-exit scope guard (25-D5). Every Rev-4 change and the
re-gate finding it closes is listed in §15.

---

## 3. D1/D2/D3/D4/D5 — Status line and dsh plan mode

### 3.1 Current state (verified against the tree)

`render_status(const UiModel&, const SessionUiState*, const Theme&)` is the only
bottom-line renderer (`src/ui/ui_render.cpp:357-389`), called once from `build_ui`
at `src/ui/ui_render.cpp:942`. Today it emits:

```
<state_name(agent_state) or "idle"> [· <model>] [· ↑<in> ↓<out> ⚡<cached>] [· <note>] [· <notice>]   <N active · N waiting>
```

- `state_name` returns `"idle"` for `AgentState::Idle` (`src/ui/ui_render.cpp:96-114`).
- `StatusModel` holds `model`, `input_tokens`, `output_tokens`, `cached_tokens`,
  `agent_state`, `last_error`, `note` (`include/ymh/ui/ui_model.hpp:176-184`); it is
  a member of `SessionUiState` (`:210`).
- Counters are populated from `AssistantMessageFinished.usage` and
  `TokenUsageUpdated.usage` (`src/ui/ui_model.cpp:678-680`, `:788-791`).
- `status.note` is set by `StatusChanged` (`src/ui/ui_model.cpp:793-795`);
  `model.notices.back()` is appended by the current renderer
  (`src/ui/ui_render.cpp:372-377`) and is the workspace-independent failure surface
  spec 22 pins (`docs/design/22-switcher-sessions-errata.md:148,1155-1156`).
- **Plan state does not exist.** No `AgentMode`, `agent_mode`, `PermissionMode`,
  or plan/build field anywhere in `include/` or `src/`.
- **`SandboxMode::ReadOnly` exists independently** (`include/ymh/execution/environment.hpp:22-26`)
  and is enforced by `RulePermissionPolicy::evaluate` via `tool_is_mutating`
  (`src/policy/permission_policy.cpp:160-174,204-207`). It is **not** the plan-mode
  mechanism and Rev 2 does not touch it.
- **TPS does not exist.** No rate computation or storage; the only timing
  primitives are `LLMResponse::latency` (diagnostics only), `ToolResult::duration`,
  and the spinner clock (`include/ymh/llm/llm_provider.hpp:58`;
  `include/ymh/session/events.hpp:134`). UI events
  `AssistantMessageStarted`/`Finished` carry **no timestamps**
  (`include/ymh/ui/ui_event.hpp:83-100`).
- **Context window**: four candidate sources, none wired to the status line —
  `CompactionSettings::context_window_tokens` (default `0` = unknown,
  `include/ymh/config/config.hpp:45-58`), the runtime `ContextBudget::window_tokens`
  inside the `ContextSnapshot` returned by `context.show`
  (`include/ymh/agent/context_snapshot.hpp:64-68`, `:86-96`), provider
  `ModelInfo::max_context_tokens` (`include/ymh/llm/llm_provider.hpp:24`), and the
  compactor policy (`include/ymh/agent/compactor.hpp:34`).
- Theme has `color`, `user_block`, `completion_selected` and the helpers
  `paint`/`paint_bg`; `GrayDark` is already used for the FreeSpace cell
  (`include/ymh/ui/theme.hpp`; `src/ui/ui_render.cpp:29-41`, `:653-654`).

### 3.2 Decision (25-D1) — status-line composition

`render_status` is rebuilt from an ordered segment list. Segments, in display
order:

| # | Segment | Content | Always? |
|---|---|---|---|
| 1 | mode | `"plan"` when `status.plan_active`, else `"build"` | yes (for an active session) |
| 2 | state | `state_name(agent_state)` | only when `agent_state != AgentState::Idle` |
| 3 | model | `status.model` | when non-empty |
| 4 | counters | `↑<input_tokens> ↓<output_tokens> ⚡<cached_tokens>` | yes (even all-zero) |
| 5 | tps | `"<x.y> tps"` or `"— tps"` | yes |
| 6 | context | `"[<10 cells>] <pct>"` | yes |
| 7 | note | `status.note` | when non-empty |
| 8 | notice | `model.notices.back().text` | when `!model.notices.empty()` |
| — | aggregate | `"<N> active · <N> waiting"` | always, right-aligned |

- Segments 1–8 are joined with `" · "` (unchanged separator); the aggregate is
  the right widget and remains last.
- Segment 1 replaces the literal `"idle"`; when a session is active and the agent
  is idle the line starts with `build` (or `plan`), not `idle`.
- When `active == nullptr` (no session yet) the line renders only the aggregate
  widget, exactly as today (no `idle`, no mode).
- Segment 4 now renders whenever a session is active, not only when a counter is
  non-zero (requirement 1 says "current up/down and lightning numbers").
- **Segments 7 and 8 are retained** (gate H2/H3). Spec 22 §3.6 and 22-A8 are
  **not** superseded. This is the workspace-independent notice surface that
  spec 22 introduced; dropping it would re-break the invisibility bug spec 22 fixed.

**Width-aware degradation and overflow (25-A1).** `render_status` gains an
`int width` parameter (threaded from `TerminalSize::width` at `build_ui`). The
pinned algorithm:

1. Compute `right` (the aggregate string) and `right_w = ftxui::string_width(right)`.
2. `avail = width - right_w - 1` (one column of gap); `avail = max(avail, 1)`.
3. **`mode` and `model` are reserved first (N7).** Segment 1 (`mode`) is always
   included; if `string_width(mode) > avail`, `mode` is ellipsized (`…`) to
   `avail`. Segment 3 (`model`) is then included whenever at least 4 columns
   remain after `mode`; it is ellipsized (`…`) to those remaining columns, never
   below 4. `model` is **not** in the optional priority list below and is never
   silently dropped while ≥4 columns remain. The reserved width used for `model`
   in the fit test is its **ellipsized** width, not its full width.
4. Consider the remaining **optional** segments in inclusion-priority order
   `counters > context > note > notice > tps > state`. A segment is included iff
   the display width of the already-included segments (mode, model, and any
   earlier optional segment) plus the separator plus the candidate fits `avail`.
   `state` is last because `AgentState::Idle` renders no state segment anyway.
5. Included segments are always emitted in **display order**; only inclusion is
   priority-ordered.
6. The aggregate widget is always emitted as the right element and is never
   removed from the element tree; at widths narrower than `right_w` FTXUI clips
   the right edge (recorded as §14 Q11).

This keeps the context bar (the user's headline feature) at moderate widths and
drops the least load-bearing segment first, while guaranteeing mode and model
degrade by ellipsis rather than disappearance.

**Mode colour.** Segment 1 is painted `Color::Yellow` when
`status.plan_active == true`, and left unpainted when false (no new `Theme`
field; `paint` honours `theme.color`). Recorded as §14 Q2.

### 3.3 Decision (25-D2) — plan mode is a durable, event-sourced projection (dsh)

Plan mode is **logged per-session collaboration state**, exactly as dsh defines it:
one whole-value-replace event; the last one wins; a log with none folds to
inactive; resume and fork restore it by folding the log.

**New durable event.** Additive to `01-session.md`:

```cpp
// include/ymh/session/events.hpp (namespace ymh::payload)
struct PlanMode {
    bool active = false;
};

// include/ymh/core/event.hpp — EventType gains one enumerator
enum class EventType : std::uint16_t {
    // …existing unchanged…
    SessionRenamed,      // wire: session/renamed
    PlanMode,            // wire: plan/mode   (25-D2)
    McpServerStatusChanged,
};

// include/ymh/session/events.hpp — registration (both directions)
template <> struct SessionEventMap<EventType::PlanMode> {
    using type = payload::PlanMode;
};
template <> struct EventTraits<payload::PlanMode> {
    static constexpr EventType type = EventType::PlanMode;
};

// include/ymh/session/events.cpp — JSON
// to_json:   json = nlohmann::json{{"active", value.active}};
// from_json: value.active = json.value("active", false);
```

`wire_name(EventType::PlanMode) == "plan/mode"`, `parse_event_type("plan/mode")`
returns `EventType::PlanMode`, and `all_event_types()` includes it. The generic
event table already stores arbitrary `(type, payload)`, so there is **no schema
change** and no migration.

**Projection (read side).** A pure fold, used by resume/fork/replay and by the
status line:

```cpp
// include/ymh/session/plan_mode.hpp  (NEW)
namespace ymh {

// Folds the session log: the last `plan/mode` wins; an empty log (or one with no
// `plan/mode`) is inactive. O(n), never throws.
[[nodiscard]] bool plan_mode_active(const EventRange& events) noexcept;

} // namespace ymh
```

`deriveMessages` (`src/session/session.cpp:360+`) ignores `PlanMode` (it is
non-surface: it contributes no message). `session_export.cpp` renders it as a
one-line marker only if it already renders non-surface events; otherwise it is
skipped like `SessionRenamed`.

**Daemon-side controller.** The truth is the log; the controller only serializes
appends and holds an *uncommitted* selection. There is **no** `SessionModeStore`
and no in-memory authoritative mode.

```cpp
// include/ymh/agent/plan_mode_controller.hpp  (NEW; owned by WorkspaceRuntime)
namespace ymh {

enum class PlanModeSetResult : std::uint8_t {
    Unchanged,   // selection equals the logged state
    Committed,   // appended now (no open turn)
    Queued,      // open turn: applied at the next accepted step boundary
    Cancelled,   // an opposite queued selection was dropped
};

class PlanModeController {
public:
    // Appends a committed `plan/mode` to the session's durable log. Injected so
    // the controller is testable without a SessionStore.
    using AppendFn = std::function<void(const SessionId&, payload::PlanMode)>;

    // The fold, injectable only so a test can count folds (N10/M-1 Rev 5).
    // Production passes `plan_mode_active` (the default).
    using ProjectionFn = std::function<bool(const EventRange&)>;

    explicit PlanModeController(AppendFn append,
                                ProjectionFn project = plan_mode_active);

    // Reads the projection from the session's log. A non-authoritative memo
    // keyed by `SessionId` alone MUST back the per-step `assemble` call so the
    // log is folded at most once per session (N10/M-1 Rev 5); the log remains
    // the only source of truth (UX13). Cleared by `erase`.
    [[nodiscard]] bool active(const Session& session) const;

    // `turn_open` is supplied by the caller (RPC: agent status != "Idle"; agent
    // loop: true). Idle => append now; open => queue. Any `set()` first drops a
    // pending model-requested exit (L-2): the user's explicit selection
    // supersedes it. If the requested value equals the **logged** state and an
    // opposite selection is pending, the result is `Cancelled` (the pending
    // selection is dropped) — checked before the `Unchanged` case (L-1). If the
    // requested value equals the logged state and nothing is pending, the result
    // is `Unchanged`.
    PlanModeSetResult set(const SessionId& session, bool turn_open, bool active);

    // Queues a model-requested exit (the approved `exit_plan_mode` review). The
    // append happens at the next accepted step boundary; no prompt section change
    // is observable until then.
    void request_exit(const SessionId& session);

    // Called by the agent loop at the top of every accepted step, before request
    // assembly. Commits any queued selection and returns the now-effective state.
    bool apply_pending_at_step_start(Session& session);

    // Called once when the turn ends (N5): commits any selection/exit still
    // queued because the turn produced no further accepted step. Idempotent,
    // null-safe, and noexcept (LOW-4 Rev 5): an append failure is absorbed.
    bool flush_pending_at_turn_end(Session& session) noexcept;

    void erase(const SessionId& session) noexcept;

private:
    struct ProjectionMemo {
        bool valid  = false;   // false until the first fold for this session
        bool active = false;   // the folded (or last committed) value
    };

    // The single append path: invalidate the memo, append after releasing
    // `mutex_` (N12), then record the committed value. Never throws past its
    // caller's append failure handling.
    void commit_(const SessionId& id, bool value);

    AppendFn                              append_;
    ProjectionFn                          project_;
    mutable std::mutex                    mutex_;
    std::map<SessionId, bool>             pending_;   // uncommitted selection
    std::map<SessionId, bool>             pending_exit_;
    // Non-authoritative projection memo (N10/M-1 Rev 5). **Exact key:
    // `SessionId` alone.** There is deliberately NO `Sequence` in the key: the
    // session's last `Sequence` advances on every appended event, so keying on
    // it re-folded the whole log on every step. The memo is written by (a) the
    // first `active()` fold for a session and (b) every `commit_()` (the
    // controller is the sole writer of `plan/mode`), and dropped by `erase`.
    // Because no other writer exists, a valid entry cannot go stale, so a step
    // that appends no `plan/mode` never re-folds. Never consulted by
    // resume/fork/replay.
    mutable std::map<SessionId, ProjectionMemo> memo_;
};

} // namespace ymh
```

**Lock discipline (N12).** `mutex_` guards only the `pending_`/`pending_exit_`
maps. `append_` is invoked **after** releasing `mutex_`, because
`Session::append` → `EventBus::publishCommitted` runs committed handlers
**synchronously** (`src/session/session.cpp:580`; `src/core/event_bus.cpp:262-267`)
and a handler that calls `active()` would otherwise self-deadlock. The controller
never re-enters itself and no committed handler calls back into it.

**Memoization (N10/M-1 Rev 5).** `active(session)` folds the log **at most once
per session**:

- **Exact key: `SessionId` alone.** The entry is `ProjectionMemo{valid, active}`.
  There is deliberately **no `Sequence`** in the key: the session's last
  `Sequence` advances on every appended event (`StepStarted`, `ToolCall`, …),
  which is exactly why the Rev-3/Rev-4 `(SessionId, last Sequence)` key re-folded
  the whole log on every step and defeated the memo.
- **Where stored:** the private `mutable std::map<SessionId, ProjectionMemo>
  memo_`, guarded by `mutex_`.
- **Invalidation rule:** the controller is the **sole writer** of `plan/mode`.
  Every append goes through one private helper `commit_(id, value)` which
  (a) sets `memo_[id].valid = false` under `mutex_` **before** calling `append_`,
  (b) calls `append_` after releasing `mutex_` (N12), and (c) on success sets
  `memo_[id] = {true, value}` under `mutex_`. If `append_` throws
  (`LeaseLost`/`StoreError`), the entry stays invalid and the next `active()`
  re-folds the durable log, so the memo can never disagree with the log.
  `active()` stores a fold result only if the entry is still invalid
  (compare-and-set), so a concurrent commit always wins. `erase(id)` drops the
  entry.
- **Proof the fold is skipped.** `plan/mode` is appended only from `set()` (idle
  path), `apply_pending_at_step_start`, and `flush_pending_at_turn_end`, all via
  `commit_`. A step that appends no `plan/mode` therefore calls neither `commit_`
  nor `project_`; `memo_[id].valid` stays true and `active()` returns the cached
  value without reading `session.events()`. Only the first `active()` per session
  (attach/resume/fork; the memo is process-local) folds. Per-turn cost is O(1)
  amortized with one O(n) fold at attach — not O(n) per step (UX12). UX-U33
  injects a counting `ProjectionFn` and asserts exactly one fold across N
  consecutive `active()` calls and zero further folds after a commit.

**Ownership and injection (pinned; closes M1/N4).** The controller is owned by
`WorkspaceRuntime::Impl` (`src/agent/workspace_runtime.cpp`), not by
`HostRuntime`. This is forced by construction order: the daemon builds
`WorkspaceRuntime` first and `HostRuntime` wraps it
(`src/host/workspace_host.cpp:503` then `:543`; `HostRuntime` takes
`WorkspaceRuntime&`, `src/host/host_runtime.cpp:234-247`), while the assembler and
  the agent loop — which read plan state — are owned by `WorkspaceRuntime`
  (`src/agent/workspace_runtime.cpp:110,143,179,190`; `agents_` is at `:190`,
  L-6 Rev 5). The wiring is:

- `WorkspaceRuntime::Impl` declares `PlanModeController plan_mode_;` and
  constructs it with `AppendFn = [this](const SessionId& id, payload::PlanMode mode)
  { if (auto session = sessions_.sessionPtr(id)) { session->append(mode); } }`.
  The lookup is `SessionManager::sessionPtr(const SessionId&)`
  (`include/ymh/session/session_manager.hpp:68`), the same accessor
  `AgentRegistry::registerAgent` already uses (`src/agent/agent_registry.cpp:118`).
- `AgentServices` (`include/ymh/agent/agent_loop.hpp:43-67`) gains one field,
  `PlanModeController* plan_mode = nullptr;`, set to `&plan_mode_` in the
  `WorkspaceRuntime::Impl` body **before** `agents_` is constructed
  (`src/agent/workspace_runtime.cpp:138-159`), so every `AgentLoop` receives it
  through the existing `services_` copy in `AgentRegistry::registerAgent`
  (`:110-120`).
- The loop calls `services_.plan_mode->apply_pending_at_step_start(session_)` at
  the top of each step, immediately after `session_.append(StepStarted{…})` and
  before `services_.context->assemble(...)`
  (`src/agent/agent_loop.cpp:635-649`); and it calls
  `services_.plan_mode->request_exit(session_.id())` after an approved
  `exit_plan_mode` review **only when the tool returned `ToolOutcome::Ok`**
  (§3.5, NEW-1 Rev 5). `services_.plan_mode == nullptr` (e.g. a headless runtime
  without the daemon controller) means plan mode is unavailable: `exit_plan_mode`
  fails closed.
- **Turn-end flush on every exit (N5; Rev 4).** `runTurn` has no single
  "after the step loop" point: the step loop returns directly from every terminal
  path — normal `TurnEnded` (`src/agent/agent_loop.cpp:796-804`), `TurnCancelled`
  (`:783-787`, `:819-823`), `TurnFailed` (`:788-794`), step limit (`:824-827`),
  context-assembly failure (`:642-645`, `:650-653`, `:666-669`, `:736-739`),
  missing provider (`:697-700`), and coalescer `LeaseLost`/`StoreError`
  (`:748-756`). The flush is therefore installed as a **scope guard** immediately
  after `session_.append(payload::TurnStarted{…})` (`:612`), whose destructor
  calls `services_.plan_mode->flush_pending_at_turn_end(session_)`. It runs on
  every exit, including error and cancellation; it is idempotent (a selection
  already committed by `apply_pending_at_step_start` is not re-appended),
  null-safe, and **`noexcept`** (LOW-4 Rev 5): the flush catches any
  `LeaseLost`/`StoreError` from its own `Session::append` and drops the pending
  entry instead of propagating, so a destructor during stack unwinding can never
  `std::terminate`. **The same guard is installed in `runMaintenanceTurn`
  immediately after its `TurnStarted`** (`src/agent/agent_loop.cpp:546`; LOW-5
  Rev 5), so a selection queued during a compaction turn is committed at that
  turn's exit rather than deferred. Because it runs at scope exit it may append
  `plan/mode` **after** the terminal event; that is correct for a last-wins log
  fold and is the pinned ordering (this replaces the Rev-3 "before the terminal
  event" wording). If the session was disposed first, the append lambda's
  `sessionPtr` lookup fails and the flush is a no-op.
- `HostRuntime` reaches the same instance through a new
  `WorkspaceRuntime::plan_mode()` accessor for the `session.set_mode` handler
  (below) and calls `runtime_.plan_mode().erase(id)` from `deleteSession`
  (`src/host/host_runtime.cpp:681-737`), **not** from `closeSession`
  (`:665-669`), which is an explicit detach-only no-op (24-D10) and must not drop
  a pending selection on a mere UI detach.

**Wire surface (additive).** One new JSON-RPC method, registered in
`include/ymh/transport/protocol.hpp` (next to `kSessionRename`), added to the
catalog in `src/transport/protocol.cpp:615-625`, and handled in
`src/transport/protocol_server.cpp` next to `kSessionRename`:

```
session.set_mode
  params: { "session": <SessionId>, "active": <bool> }
  result: { "session": <SessionId>, "active": <bool>, "pending": <bool> }
```

- `active` is required and must be a JSON boolean; otherwise `InvalidParams`.
- Unknown session ⇒ `InvalidParams`.
- **Handler plumbing pinned (LOW-1 Rev 5).** `HostRuntime` gains one additive
  method, mirroring `renameSession` (`include/ymh/host/host_runtime.hpp:160`,
  `src/host/host_runtime.cpp:596-620`), so `ProtocolServer` never touches the
  controller directly:

  ```cpp
  // include/ymh/transport/host.hpp — additive, next to SessionRenamedResult (:60)
  struct SetModeResult {
      SessionId session;
      bool      active = false;    // effective selection
      bool      pending = false;   // true iff queued
  };

  // TransportHost (:92, next to renameSession) gains one pure virtual:
  virtual SetModeResult setSessionMode(const nlohmann::json& params) = 0;

  // include/ymh/host/host_runtime.hpp — the override
  protocol::SetModeResult setSessionMode(const nlohmann::json& params) override;
  ```

  `HostRuntime::setSessionMode` mirrors `HostRuntime::renameSession`
  (`src/host/host_runtime.cpp:596-620`): it reads `params["session"]` (string) and
  `params["active"]` (bool), throwing the mapped `InvalidParams` error for a
  missing/wrong-typed param or an unknown session (the `agentStatus` call below
  already throws `UnknownSession` for an unknown id); then it calls
  `runtime_.plan_mode().set(id, agentStatus(id) != "Idle", active)`
  (L3: `HostRuntime::agentStatus` returns `std::string`, `"Running"`/`"Idle"`,
  `src/host/host_runtime.cpp:903-915` — the comparison is to the literal `"Idle"`,
  not an enum). It resolves the `Session&` with
  `runtime_.sessions().sessionPtr(id)` (`include/ymh/session/session_manager.hpp:68`)
  and returns `{id, effective, pending}`: `pending = (result == Queued)` and
  `effective = (result == Committed || result == Queued) ? active
  : runtime_.plan_mode().active(*session)`. The `ProtocolServer` branch next to
  `kSessionRename` (`src/transport/protocol_server.cpp:402-403`) calls
  `host_.setSessionMode(request.params)` (the member is a `TransportHost&`,
  `include/ymh/transport/protocol_server.hpp:177`) and responds
  `{session, active: result.active, pending: result.pending}`. Because the
  interface gains a pure virtual, the test double
  `tests/support/fake_transport_host.hpp:19` must add an override (a trivial
  `{id, active, pending=false}` echo); it is enumerated in §13.1.
- `result.active` is the **effective** selection (logged when committed, or the
  pending selection when queued); `result.pending` is true iff queued.
- There is **no `kProtocolVersion` bump** (additive method, same policy as spec
  20's `skills.*` methods). A daemon that does not know the method answers
  `MethodNotFound`; the supervisor then shows a one-line notice and keeps the
  logged state it folded from the event stream.

**How the status line gets the state (N6/L4).** The supervisor subscribes to the
session event stream and replays the log on attach; a new UI event
`PlanModeChanged{session, active}` is produced from the durable `PlanMode` event
(§3.9) and folded into `StatusModel::plan_active`. No query RPC is needed: the
status line reads the session state, not a separate store. **The `session.set_mode`
reply is never used to write `plan_active`.** The reply carries no `Sequence`, so
"reply newer than event" is not decidable (N6); the durable event is the only
writer. The reply's `pending` flag may drive a transient status-bar notice
(`"plan change queued"`) but must not change the rendered mode. A committed
selection publishes its `PlanModeChanged` synchronously
(`src/session/session.cpp:580` → `src/core/event_bus.cpp:262-267`), so the mode
flips on the same event-loop turn.

### 3.4 Decision (25-D3) — the `plan:policy` prompt section

While plan mode is active, a deployment-owned guidance section is included in each
model request. This is dsh's `plan:policy` section (first-party prompt order 500);
ymh has a single system message, so the section is appended to it.

**Config.** `AgentDefaults` (`include/ymh/config/config.hpp:60-66`) gains one
field, parsed from `agent.plan.section` (a new `plan` object under `agent`):

```cpp
struct AgentDefaults {
    // …existing fields unchanged…
    std::string plan_section;   // 25-D3: guidance while plan mode is active;
                                // empty => built-in default (below)
};
```

- `apply_agent`'s `reject_unknown` gains `"plan"`; a new `apply_plan` rejects
  unknown keys under `agent.plan` and reads the single key `section`
  (`read_string`). A non-string `section` ⇒ `ConfigError`.
- `AgentConfig` (`include/ymh/agent/agent.hpp:94-104`) also gains
  `std::string plan_section;`, resolved by `to_agent_config`
  (`src/cli/wiring.cpp:167-180`) exactly like `system_prompt`: an empty
  `config.agent.plan_section` becomes the built-in `kDefaultPlanSection`
  (declared next to `default_system_prompt()`, `include/ymh/cli/wiring.hpp:42`,
  `src/cli/wiring.cpp:48`). This is how the resolved text reaches the
  `WorkspaceRuntime` provider without re-reading the config.
- **Built-in default** (`kDefaultPlanSection`), used when `plan_section` is empty:

  ```
  You are in plan mode. Explore the codebase and design a concrete plan before
  acting. Every tool remains available, but do not make changes; when the plan is
  ready, present it by calling exit_plan_mode. The user may leave plan mode with
  /plan off.
  ```

  This mirrors dsh's "guidance, not enforcement" stance and names the exit tool.

**Assembler seam.** `SessionContextAssembler`
(`include/ymh/agent/context_assembler.hpp:59-69`) gains an optional provider:

```cpp
class SessionContextAssembler final : public ContextAssembler {
public:
    SessionContextAssembler(ToolRegistry& tools, std::string systemPrompt);

    // 25-D3: optional. When set and it returns non-empty for the assembled
    // session, the text is appended to the system message as an additional
    // paragraph ("\n\n"). The session is passed directly (M1): `assemble` already
    // has it, so no lookup is needed and the signature matches
    // `PlanModeController::active(const Session&)`.
    void set_plan_policy_provider(
        std::function<std::string(const Session&)> provider);

    std::vector<Message> assemble(const Session&, const TurnContext&) const override;
    std::vector<ToolSchema> tools() const override;

private:
    ToolRegistry&                              tools_;
    std::string                                systemPrompt_;
    std::function<std::string(const Session&)> plan_policy_;   // 25-D3
};
```

`assemble` (`src/agent/context_assembler.cpp:37-52`): if
`systemPrompt_.empty()` **and** the plan section is non-empty, a system message is
still created (the plan section alone is a valid system prompt); otherwise the
section is appended to the existing system text separated by `"\n\n"`. When plan
mode is inactive the provider returns empty and the request is byte-identical to
today (dsh: "inactive mode adds no tokens").

**Wiring (pinned; M1/N4).** `WorkspaceRuntime::Impl` sets the provider in its
constructor body, after `agent_config_` and `plan_mode_` exist and before
`agents_` is built:

```cpp
assembler_.set_plan_policy_provider(
    [this](const Session& session) -> std::string {
        return plan_mode_.active(session) ? agent_config_.plan_section : std::string{};
    });
```

The lambda captures `this`, so the `Session&` handed to `assemble` is used
directly — the exact session lookup is `Session::events()` inside
`plan_mode_active`, not a map lookup. (An implementation may instead capture
`&plan_mode_` and `&agent_config_.plan_section`; both outlive the assembler.)

**KV-cache cost (N11).** The plan section is part of the system message, so
entering or leaving plan mode changes the request prefix from that message onward
and invalidates the provider's prefix/KV cache for the next request. This is an
explicit user action (`/plan`, `/plan off`, or an approved review), not a per-step
churn; the cost is accepted and recorded as §14 Q15.

### 3.5 Decision (25-D4) — `exit_plan_mode` and the generic review flow

The model-facing exit tool is `exit_plan_mode`. It stays registered while plan mode
is **inactive** as well, so entering or leaving plan mode changes only the prompt
section, not the request tool catalog (dsh).

```cpp
// src/tools/plan_tools.cpp  (NEW)
namespace ymh {

class ExitPlanModeTool final : public Tool {
public:
    ToolSchema schema() const override;
    Task<ToolResult> execute(const ToolContext&, const ToolArguments&) override;
};

// schema():
//   name.value  = "exit_plan_mode"
//   description = "Present the completed plan for user review and leave plan
//                  mode on approval. Only meaningful while plan mode is active."
//   destructive = false         // 25-D4/N3: it mutates no workspace state; a
//                               // true value would make tool_is_mutating() true
//                               // and SandboxMode::ReadOnly would hard-deny the
//                               // review before any rule
//   input_schema = {
//     "type": "object",
//     "properties": { "plan": { "type": "string",
//                               "description": "The complete plan to review" } },
//     "required": ["plan"],
//     "additionalProperties": false
//   }

} // namespace ymh
```

The tool is registered unconditionally at daemon startup (with the other builtins
in `workspace_runtime`), before `ToolRegistry::freeze()`. It appends no durable
events (tool contract X6/X8).

**Routing (pinned; `src/agent/agent_loop.cpp:432-514`; N3 Rev 4).** For a tool
call named `exit_plan_mode`:

- **Plan inactive**: the loop short-circuits **before** the permission block: in
  `executeToolCall`, before constructing the `PermissionRequest`
  (`src/agent/agent_loop.cpp:432`), if `call.name == "exit_plan_mode"` and plan is
  inactive it appends a normal `ToolResult{outcome=Error,
  output="exit_plan_mode is only valid in plan mode"}` and returns `false`, so
  **no permission request is made** (the tool's own `execute` also fails closed if
  ever reached). Plan activity is read via
  `services_.plan_mode != nullptr && services_.plan_mode->active(session_)`.
- **Malformed plan argument (NEW-1 Rev 5)**: still before constructing the
  `PermissionRequest`, if `call.name == "exit_plan_mode"`, plan is active, and
  `call.arguments` is not an object, has no `"plan"` member, has a `"plan"` that
  is not a string, or has a string that is empty or whitespace-only, the loop
  appends `ToolResult{outcome=Error, output="exit_plan_mode requires a non-empty
  string 'plan' argument"}` and returns `false`, so **no permission request is
  made**. The JSON-schema `required:["plan"]` is not enforced before the Ask (the
  Ask precedes `tools->execute`: `src/agent/agent_loop.cpp:452-503` vs `:533`), so
  this pre-Ask check is load-bearing: without it the review is blind (the dialog
  shows only `tool: exit_plan_mode`) and a stray Allow exits plan mode on an
  empty plan.
- **Plan active**: the agent loop presents the call through the **generic option
  flow** — the existing permission gate/resolver (`services_.gate` when present,
  else `services_.policy` + `services_.permission_resolver`,
  `src/agent/agent_loop.cpp:452-503`). The review is forced by a **non-mutating
  request flag**, not by `destructive`:
  - `ymh::PermissionRequest` (`include/ymh/policy/permission_policy.hpp:54-70`)
    gains one additive field `bool force_ask = false;`. The loop sets
    `request.force_ask = true` for `exit_plan_mode` while plan is active, before
    calling `gate->resolve()` / `policy->evaluate()`.
  - `RulePermissionPolicy::evaluate` (`src/policy/permission_policy.cpp:204`)
    gains one short-circuit **after** the `SandboxMode::ReadOnly` mutating deny and
    **before** rule matching, `tool_defaults`, `default_verdict`, and grants:
    `if (request.force_ask) return PolicyVerdict::Ask;`. This makes the review
    unavoidable even under `default_verdict == Allow`
    (`include/ymh/policy/permission_policy.hpp:106`), a broad `"*"` Allow rule, or
    a remembered `AllowAlways` grant (grants are stored by `remember`, `:254-278`,
    but never consulted because the short-circuit precedes them). It works on
    **both** the gate path (`PermissionGate::resolve` calls `policy_.evaluate`
    internally, `src/policy/permission_policy.cpp:318`) and the direct policy
    path.
  - `force_ask` is **not** consulted by `tool_is_mutating`
    (`src/policy/permission_policy.cpp:160-173`), which reads only
    `request.destructive` and a fixed tool-name list. With `destructive=false` the
    classification stays false and the ReadOnly pre-rule deny (`:205-207`) does
    **not** fire; the review remains reachable in ReadOnly. Plan mode still reads
    no sandbox/approval state (UX20).
  - When no resolver/gate is attached (headless), the forced Ask fails closed with
    `"exit_plan_mode requires an interactive review"` — never a silent approval.
- **The plan preview (pinned; closes H1/N1).** `PermissionRequest` (the core
  struct, `include/ymh/policy/permission_policy.hpp:54-70`) has **no `summary`
  member**; the dialog text is the *wire* `protocol::PermissionRequest::summary`
  (`include/ymh/transport/protocol.hpp:249-256`). The daemon builds it in
  `bounded_summary(const PermissionRequest&, bool)` inside
  `src/permission/permission_broker.cpp:50-80`, called by
  `make_wire` (`:82-95`). That helper gains one branch:
  - new file-local constants `kMaxPlanSummary = 4096` (bytes) and
    `kMaxPlanSummaryLines = 12` (NEW-2 Rev 5; the byte cap alone allowed ~2000
    newline-separated lines, whose `ftxui::paragraph` rows pushed the option rows
    and the hint off-screen);
  - if `request.tool == "exit_plan_mode"` **and** `request.arguments` is an object
    whose `"plan"` member is a **non-empty string**, the summary base is that
    string instead of `request.tool`; it is capped at **both**
    `kMaxPlanSummaryLines` lines and `kMaxPlanSummary` bytes (whichever binds
    first) and, when either cap truncates, the retained text is suffixed with the
    **existing ASCII marker `"..."`** (three dots, as at `:74`) — not the Unicode
    `"…"` the Rev-2 text implied;
  - if `request.tool == "exit_plan_mode"` and the plan is missing, empty, or not a
    string (reachable only from a non-loop caller, since the loop's argument gate
    above rejects it first), the summary base is the literal
    `"exit_plan_mode (no plan provided)"` — never a blind `tool`-only review
    (NEW-1 Rev 5);
  - every other tool keeps the current 256-byte (`kMaxSummary`) behaviour
    unchanged.
  The preview is read from the **core** `request.arguments`, so it is independent
  of `config.arguments_max_bytes` (default 65536,
  `include/ymh/policy/permission_policy.hpp:103`), which only bounds the separate
  `wire.arguments` copy (`bounded_arguments`, `:35-48`). A plan larger than the
  preview still rides the wire in `arguments` up to that bound.
- **Rendering path (end-to-end; verified against the tree).**
  `PermissionBroker::make_wire` sets `wire.summary` →
  `ProtocolServer::onPermissionRequest` broadcasts `notify::kPermissionRequest`
  (`src/transport/protocol_server.cpp:893-908`) → the supervisor forwards it
  (`src/ui/supervisor.cpp:751`) →
  `UiEventAdapter::onPermissionRequest(WorkspaceId, protocol::PermissionRequest)`
  copies it into `PermissionRequested::summary` (`src/ui/ui_event_adapter.cpp:297-310`;
  the wire summary is non-empty, so the 512-byte `summarize_tool_arguments`
  fallback at `:30-38` is not used) → `UiModel::apply(PermissionRequested)` sets
  `dialog.summary` (`src/ui/ui_model.cpp:741-750`) → `render_dialog` draws
  `ftxui::paragraph(dialog.summary)` under `tool: exit_plan_mode`
  (`src/ui/ui_render.cpp:391-399`). No new dialog type or UI event type is added;
  the only additive surfaces are one wire/event field (`force_ask`, below) and
  the render branch that reads it.
  **Dialog height bound (NEW-2 Rev 5):** `render_dialog` wraps the plan paragraph
  in a height-constrained frame of at most `kMaxPlanPreviewRows = 12` rendered
  rows (`ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, kMaxPlanPreviewRows) |
  ftxui::frame`) before the separator and option rows, so the option rows (four
  normally, two for a forced review) and the `↑/↓ select · Enter confirm` hint are
  always emitted even when a single
  long line would otherwise wrap past the terminal bottom. The summary caps in
  `bounded_summary` bound the string; the frame bounds the layout. UX-G8 asserts
  the option rows and hint remain visible for a 200-line plan. (The in-process
  `onPermissionRequest(SessionId, …)` overload is test-only; it is not on the
  daemon review path.)
  - **`force_ask` reaches the dialog (NEW-3 Rev 5).** The wire
    `protocol::PermissionRequest` (`include/ymh/transport/protocol.hpp:249-256`)
    gains one additive optional field `bool force_ask = false;`, set by
    `make_wire` from the core request (`src/permission/permission_broker.cpp:82-95`)
    and serialized with `json.value("force_ask", false)` (additive, so an old
    daemon omitting it yields the current four-option dialog;
    `src/transport/protocol.cpp:502-520`). `PermissionRequested`
    (`include/ymh/ui/ui_event.hpp:134-139`) gains `bool force_ask = false;`,
    copied in `UiEventAdapter::onPermissionRequest`
    (`src/ui/ui_event_adapter.cpp:297-310`); `UiModel::apply(PermissionRequested)`
    sets `dialog.force_ask` and `dialog.selected = force_ask ? 1 : 0`
    (`src/ui/ui_model.cpp:741-750`). `PermissionDialogModel`
    (`include/ymh/ui/ui_model.hpp:364-371`) gains `bool force_ask = false;`.
  - **Allow / Deny — decision routing (NEW-1/NEW-3 Rev 5).** On
    **Deny** the existing denial path runs; the model receives a
    `ToolResult{outcome=Denied, output=<reason>}` carrying the review feedback,
    and plan mode stays active. On **Allow** the tool executes and returns
    `{"approved": true}` only when `services_.plan_mode != nullptr`; the loop
    calls `services_.plan_mode->request_exit(session_.id())` **only if
    `result.outcome == payload::ToolOutcome::Ok`** — a tool error (e.g. no
    controller, or a plan the tool's own `execute` rejects) never exits plan mode.
    The loop gate is placed immediately after the result is appended
    (`src/agent/agent_loop.cpp:541`), gated on
    `call.name == "exit_plan_mode" && result.outcome == payload::ToolOutcome::Ok`.
  - **`AllowAlways` is not offered for a forced review (NEW-3 Rev 5).** A
    `force_ask` review is exempt from grants by design (the short-circuit
    precedes them), so a remembered `grant:exit_plan_mode` is dead. The dialog
    therefore renders **only** `{"Allow once", "Deny"}` for a `force_ask` request:
    `render_dialog` (`src/ui/ui_render.cpp:401-413`) uses the option table
    `{"Allow once", "Deny"}` with an iteration bound of `2` when
    `dialog.force_ask`, else the existing four;
    `handle_dialog` (`src/ui/supervisor.cpp:1790-1826`) wraps
    ArrowUp/Down modulo the active option count and maps Return as
    `0 ⇒ Allow/Once`, `1 ⇒ Deny/Once` for the forced case (the existing
    `0/1/2/3` mapping otherwise). `dialog.selected` defaults to `1` (Deny) for a
    forced review, so a stray Enter denies rather than blindly approving. The
    broker never records a grant for a forced review:
    `PermissionBroker::onDecision`
    (`src/permission/permission_broker.cpp:282-293`) skips
    `policy.remember(...)` when `entry->core.force_ask`, and the loop's
    direct-policy path skips its `policy->remember(...)`
    (`src/agent/agent_loop.cpp:497-502`) for the same reason. The user sees no
    "Always allow" that would silently no-op; the model still receives
    `{"approved": true}` on Allow once. A raw client that sends
    `scope=Always` for a forced request has the decision honoured for that one
    review and the grant discarded.
- The approval does **not** append during the tool batch: dsh keeps plan guidance
  for the rest of the current tool batch. The pending exit is committed by
  `apply_pending_at_step_start` at the next accepted step boundary, or by the
  turn-end flush (§3.6), which appends `plan/mode{active:false}`.

This satisfies the user's directive that "another interaction provider presents the
same request through its generic option flow": the review is an ordinary
permission request, not a plan-specific capability check.

### 3.6 Decision (25-D5) — `/plan` and `/plan off`

Commands stay **outside model history** (dsh). `/plan` and `/plan off` are
supervisor-local; a non-empty suffix becomes one ordinary user message via the
existing `agent.steer` path.

`CommandContext` (`include/ymh/ui/command_registry.hpp:18-38`) gains:

```cpp
    // 25-D5: `active` selects plan mode; a non-empty `message` is steered as one
    // user message after the selection (so it is composed under plan guidance).
    std::function<void(bool active, const std::string& message)> plan_mode;
```

`CommandRegistry::builtin()` (`src/ui/command_registry.cpp`) registers exactly one
new command:

```cpp
registry.add(Command{
    "plan", "enter plan mode (/plan off to leave)", plan_handler});
```

`plan_handler` pins:

- trimmed args empty ⇒ `plan_mode(true, "")`.
- trimmed args exactly `off` ⇒ `plan_mode(false, "")`.
- any other non-empty args ⇒ `plan_mode(true, args)` (the trimmed remainder).
- no active session ⇒ `append_system(context, "no active session")` and no RPC.

Supervisor wiring (in `Supervisor::Impl`, where the other `CommandContext`
callbacks are set, `src/ui/supervisor.cpp:1660-1663`):

1. `submit_to(workspace, kSessionSetMode, {{"session", id}, {"active", active}}, reply)`
   where `reply` **never writes `session->status.plan_active`** (N6/L4). The mode
   is rendered only from the folded `PlanModeChanged` event stream (§3.3). On an
   error reply (or `MethodNotFound`) the supervisor shows the one-line notice and
   keeps the folded state.
   - **Interim display (N5; Rev 4).** While a selection is queued the status line
     continues to show the **logged** state — it never renders `plan` before the
     `plan/mode{active:true}` event exists. When `result.pending == true` the
     supervisor sets the transient status-bar notice to exactly
     `"plan change queued"` (`/plan`). **Notice source for the exit path
     (L-3 Rev 5):** the daemon-initiated exit produces no `session.set_mode`
     reply, so the supervisor sets `"plan exit queued"` itself when it forwards
     an **Allow** decision for a review whose `dialog.tool == "exit_plan_mode"`
     (`Supervisor::resolve_dialog`, `src/ui/supervisor.cpp:1828-1832`); it never
     infers the notice from the tool result. The notice is cleared on the next
     `PlanModeChanged` for that session or when the turn ends, whichever is
     first. If the daemon commits the selection before the reply arrives, the
     event wins and the reply never flips the mode.
   - **Reconciliation on turn end / failure / cancel (N5; Rev 4).** On normal turn
     end, error, or cancellation the daemon's scope-guard flush (§3.3) commits the
     queued selection, so the log and the UI converge without a further accepted
     step. If the daemon dies before the flush, the pending selection is
     process-local and is dropped on reload (the log state stands; UX-F3).
     **Precedence (L-1 Rev 5):** when a selection is pending, `set()` is
     evaluated against the **logged** state; if the requested value equals the
     logged state but differs from the pending selection, the result is
     `Cancelled` and the pending selection is dropped — this case is checked
     before the `Unchanged` case, so a stale pending can never override the
     user's later choice. **Pending exit (L-2 Rev 5):** any `set()` first clears
     `pending_exit_[id]`; an explicit `/plan` (or `/plan off`) supersedes a
     model-requested exit that has not yet committed.
2. If `message` is non-empty, `submit_to(workspace, kAgentSteer,
   {{"session", id}, {"message", <one-text-block>}}, nullptr)` **after** the
   `session.set_mode` reply is accepted.

There is **no `/build` command** (25-W4). Leaving plan mode is `/plan off` (or an
approved `exit_plan_mode` review). The command registry's `/help` row is exactly
`/plan  enter plan mode (/plan off to leave)`.

### 3.7 Decision (25-D6) — TPS

- `StatusModel` gains `std::optional<double> tps` (`nullopt` = unknown).
- `SessionUiState` gains a non-durable
  `std::optional<std::chrono::steady_clock::time_point> stream_started_at`.
- `UiModel` event handling:
  - `AssistantMessageStarted{session,message}` for the active session sets
    `stream_started_at = now()`.
  - `AssistantMessageFinished{session,message,text,usage}`: if
    `stream_started_at` is set and `usage` has `output_tokens > 0`, compute
    `elapsed = now() - *stream_started_at`; if `elapsed >= 250 ms`, set
    `tps = output_tokens / seconds(elapsed)`; otherwise leave the previous value.
    Clear `stream_started_at`.
  - `TokenUsageUpdated` does not change TPS (usage is emitted at message end, not
    incrementally).
- The clock is injectable for tests: `UiModel::set_now_reader(ClockReader)`
  (default `std::chrono::steady_clock::now`), mirroring the `ClockReader` idiom
  already used by `PermissionBroker` (`include/ymh/permission/permission_broker.hpp:49`).
- Rendering: `fmt("{:.1f} tps", *tps)` when set, else `"— tps"`.
- **Dirty marking (gate M11).** Every write to `tps` (and to the context fields in
  25-D7) is followed by `model_.dirty.mark(session, UiDirtyFlag::Status)`. The
  `StatusModel` is not otherwise re-rendered; without the mark the new value would
  not appear.
- **Assumption**: `usage.output_tokens` is per-response (DeepSeek/OpenAI
  semantics). If a provider reports cumulative turn usage, TPS is inflated; this
  is recorded as §14 Q4.

### 3.8 Decision (25-D7) — context consumption and the 10-cell bar

- `StatusModel` gains `std::uint64_t context_used_tokens = 0` and
  `std::uint64_t context_window_tokens = 0`.
- **Source**: the supervisor reuses the existing `context.show` RPC
  (`protocol::method::kContextShow`, `include/ymh/transport/protocol.hpp:509`),
  whose reply already carries `ContextSnapshot::used_tokens` and
  `ContextBudget::window_tokens`. The supervisor issues the call (a) on session
  attach and (b) after each `AssistantMessageFinished`, and copies
  `snapshot.used_tokens` / `snapshot.budget.window_tokens` into the active
  session's `StatusModel` (then marks Status dirty, §3.7). This is read-only and
  adds no daemon or wire surface. It does **not** open the `/context` overlay
  (`model_.context` stays untouched); the existing overlay path at
  `src/ui/supervisor.cpp:2031-2059` is unchanged.
- Fallbacks: if `window_tokens == 0`, seed from
  `config.agent.compaction.context_window_tokens` (may still be 0 = unknown). If
  the snapshot has not loaded, `context_used_tokens = status.input_tokens`
  (the last request's prompt size is a sound proxy for context consumed).
- **Bar**: exactly 10 cells. **Precedence is pinned (gate M14):**

  ```
  filled = (window == 0) ? 0 : clamp((used * 10) / window, 0, 10);
  if (window > 0 && used > 0 && filled == 0) {
      filled = 1;               // visibility floor; never applies when window == 0
  }
  ```

  `window == 0` ⇒ all 10 cells empty and percent `"—"`; the visibility floor is
  never applied to an unknown window.
  - Glyphs: `█` (U+2588) for filled, `░` (U+2591) for empty — both single-width,
    matching the existing UTF-8 status glyphs `↑ ↓ ⚡`.
  - Colour: filled `paint(text, Color::Green)` normally; `Color::Yellow` when the
    percentage ≥ 80 %, `Color::Red` when ≥ 95 %; empty
    `paint(text, Color::GrayDark)` (the FreeSpace colour,
    `src/ui/ui_render.cpp:653-654`). `paint` honours `theme.color`; with colour
    off the glyphs still distinguish consumed vs total.
  - Percent: reuse `format_percent(used, window)` (`src/ui/ui_render.cpp:722-729`),
    which returns `"—"` when the window is unknown. The context segment is
    `"[" + bar + "] " + percent`.

### 3.9 C++ interface sketch (pinned)

```cpp
// include/ymh/ui/ui_model.hpp — additive/renamed fields
struct StatusModel {
    std::string   model;
    std::int64_t  input_tokens  = 0;
    std::int64_t  output_tokens = 0;
    std::int64_t  cached_tokens = 0;
    AgentState    agent_state   = AgentState::Idle;
    std::string   last_error;
    std::string   note;
    // 25-D1/D2/D6/D7
    bool                      plan_active = false;   // folded from plan/mode
    std::optional<double>     tps;                    // nullopt => "— tps"
    std::uint64_t             context_used_tokens = 0;
    std::uint64_t             context_window_tokens = 0;
};

// include/ymh/ui/ui_event.hpp — new variant member
struct PlanModeChanged {
    SessionId session;
    bool      active = false;
};

// src/ui/ui_render.cpp — INTERNAL (not exported; L1)
// `render_status` stays file-local to src/ui/ui_render.cpp:357 and is declared in
// NO header. It gains the `int width` parameter and is called only from
// `build_ui` (src/ui/ui_render.cpp:942), which receives `TerminalSize`. Tests
// drive the status line through the already-exported `render_to_ansi`
// (include/ymh/ui/ui_render.hpp:42-43), exactly as every existing golden does
// (tests/unit/ui_render_golden_test.cpp:180,334). Do NOT add a `render_status`
// declaration to the header.
static ftxui::Element render_status(const UiModel& model,
                                    const SessionUiState* active,
                                    const Theme& theme,
                                    int width);

// include/ymh/policy/permission_policy.hpp — additive field (25-D4; N3 Rev 4)
struct PermissionRequest {
    // …existing fields unchanged (include/ymh/policy/permission_policy.hpp:54-70)…
    bool force_ask = false;   // non-mutating: evaluate() returns Ask before rules,
                              // tool_defaults, default_verdict, and grants.
                              // NOT read by tool_is_mutating().
};

// include/ymh/transport/protocol.hpp — additive wire field (NEW-3 Rev 5)
struct PermissionRequest {
    // …existing fields unchanged (include/ymh/transport/protocol.hpp:249-256)…
    bool force_ask = false;   // set by make_wire; json.value("force_ask", false)
};

// include/ymh/ui/ui_event.hpp — additive event field (NEW-3 Rev 5)
struct PermissionRequested {
    // …existing fields unchanged (include/ymh/ui/ui_event.hpp:134-139)…
    bool force_ask = false;   // copied from the wire by UiEventAdapter
};

// include/ymh/ui/ui_model.hpp — additive dialog field (NEW-3 Rev 5)
struct PermissionDialogModel {
    // …existing fields unchanged (include/ymh/ui/ui_model.hpp:364-371)…
    bool force_ask = false;   // true => render/accept only {Allow once, Deny},
                              // default selected = 1 (Deny)
};

// include/ymh/agent/agent_loop.hpp — AgentServices additive field (M1/N4)
struct AgentServices {
    // …existing fields unchanged (include/ymh/agent/agent_loop.hpp:43-67)…
    PlanModeController* plan_mode = nullptr;   // 25-D2; null => plan unavailable
};

// include/ymh/agent/context_assembler.hpp — provider signature (M1/N4)
void set_plan_policy_provider(std::function<std::string(const Session&)> provider);

// src/ui/supervisor.cpp — caller split (25-D10)
void requestExit() override { begin_exit(/*allow_prompt=*/false); }  // /exit
// Ctrl+D (src/ui/supervisor.cpp:2145) calls begin_exit(/*allow_prompt=*/true).
void begin_exit(bool allow_prompt);
```

`PlanModeChanged` is produced in `src/ui/ui_event_adapter.cpp` from
`EventType::PlanMode` and handled by `UiModel::apply` (`src/ui/ui_model.cpp:607+`):
it sets `status.plan_active` and marks `UiDirtyFlag::Status`. The status line is a
pure function of the model; the plan state reaches it through the same event stream
that already restores sessions on attach. The supervisor never derives it from the
`session.set_mode` reply (§3.3, N6/L4).

---

## 4. D8/D9 — Command palette

### 4.1 Current state (verified)

`complete_command` (`src/ui/supervisor.cpp:1692-1733`) freezes a
`CompletionCycle` on the first Tab. The defect:

```cpp
const std::size_t applied = cycle->index % count;          // :1725
input.draft = "/" + cycle->names[applied];                 // :1726  inserted text
input.cursor = input.draft.size();
cycle->draft = input.draft;
cycle->index = reverse ? (applied + count - 1) % count     // :1728  advances
                       : (applied + 1) % count;
state.command_hint_selected = cycle->index;                // :1730  HIGHLIGHT
set_command_hints(state, cycle->names);                    // :1732
```

The inserted name is `names[applied]` but the highlight is `cycle->index`, which
has already advanced to `applied ± 1`. Hence "one command behind". `set_command_hints`
(`:1735-1744`) rebuilds `command_hints` in the same order as the frozen cycle, so
indices are directly comparable. `render_command_hints`
(`src/ui/ui_render.cpp:316-338`) draws `>` and the accent colour on
`command_hint_selected`.

The Enter handler (`src/ui/supervisor.cpp:1922-1937`) reads `input.draft` and
calls `dispatch_command(text)`; it never consults `command_hint_selected` or
`command_hints`. `CommandRegistry::dispatch` (`src/ui/command_registry.cpp:95-115`)
splits the body and, for an unknown name, appends
`"unknown command: /<name> (try /help)"` and returns true. So today `/sk` +
Enter prints `unknown command: /sk`.

### 4.2 Decision (25-D8) — highlight tracks the inserted candidate

`state.command_hint_selected = applied;` replaces `= cycle->index;` at
`src/ui/supervisor.cpp:1730`. `cycle->index` still advances so the next Tab/Shift+Tab
moves the cycle. Consequences:

- First Tab: text `names[0]`, highlight row 0.
- Next Tab: text `names[1]`, highlight row 1.
- Shift+Tab from row 1 (after two Tabs, `cycle->index == 2`): the inserted name is
  `names[2]` and the highlight is row 2; the internal index moves to 1 so the next
  Shift+Tab steps to `names[1]`. (Rev 1's prose said `names[1]`; corrected per gate
  Oracle L2.)

### 4.3 Decision (25-D9) — Enter accepts and executes the highlight

In `handle_input`, before `dispatch_command`:

```
if (event == Return) {
    std::string text = input.draft;
    if (accept_highlight(state, text)) {   // 25-D9
        input.draft = text;                // the completed command
    }
    if (dispatch_command(text)) {
        …existing reset (push_history, clear draft/hints)…;
        return true;                       // L-6 Rev 5: never fall through to submit
    }
    submit(text);                          // only a non-command reaches the model
    return true;
}
```

The `return true` inside the dispatch branch is load-bearing (LOW-6 Rev 5): the real
handler returns there and never reaches `submit`
(`src/ui/supervisor.cpp:1922-1937`), so a literal reading of the earlier
`if (dispatch_command(text)) { …reset… } submit(text);` sketch would submit
command text to the model.

`accept_highlight` (file-local, `src/ui/supervisor.cpp`) is:

- Returns false unless the draft starts with `/`, contains no space/tab, and
  `!state.command_hints.empty()`.
- **Returns false when the draft is exactly `/`** (gate M7). A bare `/` + Enter
  keeps today's behaviour: `dispatch("/")` lists the commands; it must **not**
  execute the first registered command (`/new`).
- Clamps `selected = min(state.command_hint_selected, hints.size()-1)`.
- Returns true with `text = "/" + state.command_hints[selected].name`.

This covers both requirement-3 cases: `/` + `sk` (hints are populated by
`refresh_hints` on every keystroke, `src/ui/supervisor.cpp:1669-1683`) and
`/` + Tab, then Enter. When the draft is already a full command (e.g. `/exit`)
and hints exist, accepting the highlight yields the same canonical name and
dispatch is unchanged. Hints are cleared by the existing dispatch-reset block
(`:1926-1932`).

**Completion ordering note.** `refresh_hints` and `set_command_hints` both use
`registry_.complete(prefix)` order, so `command_hint_selected` indexes the same
list that is rendered.

---

## 5. D10 — `/exit` without a dialog; Ctrl+D keeps it

### 5.1 Current state (verified)

- `/exit` handler → `context.request_exit()` (`src/ui/command_registry.cpp:223-229`).
- `requestExit()` → `begin_exit()` (`src/ui/supervisor.cpp:427`).
- `begin_exit()` (`:446-460`): if no orphaning workspaces → `confirm_exit({})`
  immediately; if `--yes`/`no_prompt` → `confirm_exit(orphaning)`; else
  `open_exit_prompt(orphaning)` → `UiMode::ExitConfirm` dialog.
- Ctrl+D → `requestExit()` (`src/ui/supervisor.cpp:2145-2148`), i.e. the same path.
- The dialog is `render_exit_confirm` (`src/ui/ui_render.cpp:419-470`), keyed by
  `model_.exitConfirm` (`include/ymh/ui/ui_model.hpp:432`).

So today `/exit` shows the dialog **only** when daemons would be orphaned.

### 5.2 Decision (25-D10)

Split the caller. `begin_exit` gains a parameter:

- `/exit` (`requestExit()` override) → `begin_exit(/*allow_prompt=*/false)`:
  always `confirm_exit(orphaning)` (or `confirm_exit({})` when empty). The
  orphaning set is still computed and torn down exactly as spec 16 pins; only the
  interactive prompt is skipped.
- Ctrl+D (`src/ui/supervisor.cpp:2145`) → `begin_exit(/*allow_prompt=*/true)`:
  today's behaviour — prompt when the orphaning set is non-empty and `--yes` was
  not passed.

`--yes`/`no_prompt` continues to auto-confirm on both paths. The dialog, its
key handling (`handle_exit_confirm`, `:677`), and the renderer are unchanged; they
are simply unreachable from `/exit`.

---

## 6. D11 — Delete `/skill`; keep `/skills` (amends spec 20)

### 6.1 Current state (verified)

`src/ui/command_registry.cpp:195-208` registers both:

```cpp
registry.add(Command{"skills", "list discovered skills (--show NAME for detail)", …});
registry.add(Command{"skill",  "load a skill's instructions into context",       …});
```

`CommandContext` declares both `skills` and `skill` callbacks
(`include/ymh/ui/command_registry.hpp:24-37`), wired in
`src/ui/supervisor.cpp:1662-1663` to `request_skills` / `request_skill`. Spec 20
§5.8 pins both, §6.1 lists both, and §7.2 defines explicit `/skill <name>`
activation. `/skills --show <name>` and the model-invoked `skill` tool remain
part of the subsystem.

### 6.2 Decision (25-D11)

- Remove the `"skill"` `registry.add(...)` entry from
  `src/ui/command_registry.cpp`.
- Remove the `CommandContext::skill` callback and its supervisor wiring; remove
  the `/skill` line from spec 20 §5.8/§6.1 by amendment here. The
  `request_skill` supervisor method and the model-invoked `skill` **tool**
  (`include/ymh/skills/skill_tool.hpp`) are retained — only the slash command is
  deleted. (Gate Oracle L5: the now-uncalled `request_skill` becomes dead code;
  remove it too, or keep it only if another caller exists — the spec pins
  **remove it** to avoid a `-Werror` unused-member risk.)
- Change the `/skills` description to exactly `"list available skills"` (the
  user's wording). `/skills --show <name>` is retained (it is part of `/skills`,
  not a separate command). The `--show` affordance is no longer advertised in the
  description; recorded as §14 Q6.
- Tab completion of `/sk` now yields only `/skills`.
- Spec 20 §7.2 ("Explicit `/skill <name>`") is **withdrawn**; invocation is via
  the model-invoked `skill` tool only. Recorded as §14 Q5.
- **Known functional regression (gate L8):** with the default
  `skills.expose_workspace=false`, workspace-authored skills are only reachable
  through `/skill`; after deletion the model tool cannot reach them either. This
  is recorded, not silently ignored, in §14 Q5.

The additive-command insertion point stays before the `/help` `listed` snapshot
(`src/ui/command_registry.cpp:230-241`), as spec 20 §5.8 already requires.

---

## 7. D12 — `/quit` as an alias of `/exit`

### 7.1 Current state (verified)

`Command` is `{name, description, handler}` with **no alias field**
(`include/ymh/ui/command_registry.hpp:44-48`). `find` matches the exact name;
`complete` matches the name prefix; `dispatch` uses `find`. There is no alias
mechanism anywhere.

### 7.2 Decision (25-D12)

Add an `aliases` field **appended after `handler`** so every existing aggregate
initialiser `{name, description, handler}` stays valid:

```cpp
struct Command {
    std::string              name;
    std::string              description;
    std::function<void(CommandContext&, const std::string& args)> handler;
    std::vector<std::string> aliases;   // 25-D12
};
```

- `CommandRegistry::find(name)` matches `name` first, then any alias.
- `dispatch` therefore resolves `/quit` to the `exit` command; the handler body
  is **not duplicated** (single source of truth).
- **`/help` alias rendering is pinned (gate M8).** The help handler
  (`src/ui/command_registry.cpp:230-239`) currently builds
  `std::vector<std::pair<std::string,std::string>> listed` from
  `command.name`/`description`. It is changed to append
  `" (alias: /" + alias + ")"` for each alias, in registration order, to the
  description string. The rendered row is exactly:
  `  /exit  quit the supervisor (alias: /quit)`.
- `complete(prefix)` continues to match **canonical names only**; aliases are
  dispatch-and-help surfaces. Typing `/quit` exactly works; `/qu<Tab>` does not
  complete. Justification: keeping `complete` returning `const Command*` avoids
  an ambiguous alias-vs-canonical candidate list, and requirement 7 only asks
  that `/quit` be an alias. Recorded as §14 Q7.
- The `exit` entry becomes
  `Command{"exit", "quit the supervisor", exit_handler, {"quit"}}`.

---

## 8. D13 — localcode-compatible `mcp_servers` (amends 15 and 21)

### 8.1 Current state (verified)

The shipped ymh shape is nested and array-based:

```jsonc
{ "mcp": { "server": [ { "id": "a", "transport": "stdio", "command": "…",
                          "args": ["…"], "env": ["K=V", "OTHER=${VAR}"] } ] } }
```

- Top-level allowlist is `{ui, agent, workspace, permissions, logging, llm, mcp,
  skills}`; `mcp_servers` is rejected with `ConfigError`
  (`src/config/config.cpp:537-539`).
- `McpServerSettings` fields are exactly
  `id, enabled, required, transport, command, args, env, cwd, url, header_env,
  protocol_version, allowed_tools, denied_tools, default_verdict,
  call_timeout_ms, max_result_bytes` (`include/ymh/config/config.hpp:112-129`).
  `env` is `std::vector<std::string>` of `"K=V"` strings.
- `${VAR}` expansion (only `${VAR}`, **not** `$VAR`) happens in `resolve_mcp_env`
  (`src/mcp/mcp_transport.cpp:21-52`, the expansion at `:34-38`); a missing
  variable throws `McpError{ConfigInvalid}`.
- `is_valid_mcp_server_id` is `[a-z][a-z0-9_]{0,31}`, ≤32 chars
  (`src/mcp/mcp_types.cpp:65-75`); hyphen and uppercase are rejected.
- The localcode shape is `{"mcp_servers": {"<name>": {"type": "stdio",
  "command": "…", "args": [...], "env": {"K": "V"}}}}` (object map; `env` object;
  `type` instead of `transport`; no `id`), and may use hyphens in names and
  `http`/`sse` transports with `url`/`headers`.
- The user's real `~/.localcode/config.json` uses exactly this shape, with
  literal secret values in `env` (e.g. tokens) and an optional `"type":"stdio"`.

### 8.2 Decision (25-D13)

Adopt the localcode shape as a **new top-level key `mcp_servers`**, while keeping
the old `[mcp].server` array accepted:

```jsonc
{
  "mcp_servers": {
    "brave-search": {               // localcode name; normalized to a valid ymh id
      "type": "stdio",              // optional; default "stdio"; "http"|"sse" map to "http_sse"
      "command": "python3",
      "args": ["/opt/mcp/example.py"],
      "env": { "EXAMPLE_TOKEN": "<literal-or-${VAR}>", "EXAMPLE_URL": "http://…" },
      "url": "https://example.test/sse",   // http_sse
      "headers": { "Authorization": "Bearer …" },  // flattened to header_env
      "cwd": "/opt/mcp",
      "enabled": true,              // ymh extension, optional, default true
      "required": false,            // ymh extension
      "allowed_tools": [],          // ymh extension
      "denied_tools": [],           // ymh extension
      "default_verdict": "ask",     // ymh extension
      "call_timeout_ms": 60000,     // ymh extension
      "max_result_bytes": 1048576   // ymh extension
    }
  }
}
```

Pinned rules:

1. **The object key is the server name** and accepts localcode's grammar:
   non-empty, at most 64 chars, every char in `[A-Za-z0-9_.-]`. Anything else ⇒
   `ConfigError`. The key is **normalized** to a valid ymh id (gate M1):
   ```
   normalize_mcp_server_id(key):
     lower = lowercase(key)
     replace '-' , '.' , ' ' with '_'; drop every other char
     if empty or first char not [a-z]: prefix "s"
     truncate to 32
     if still empty: "server"
   ```
   `McpServerSettings::id` is the normalized value. Two keys that normalize to
   the same id are disambiguated in ascending key order by the following
   **terminating** algorithm (M2/N9). `is_valid_mcp_server_id`
   (`src/mcp/mcp_types.cpp:65-80`) caps an id at 32 chars, so the suffix is
   appended to a **base truncated first** — a 32-char base is never re-truncated
   after the suffix is added:

   ```
   dedupe_ids(keys ascending):
     used = {}                       # assigned ids
     for key in ascending order:
       base = normalize_mcp_server_id(key)
       id   = base
       if id in used:
         id = ""
         for n = 2 .. kMaxMcpDedupeAttempts:      # kMaxMcpDedupeAttempts = 10000
           suffix    = "_" + decimal(n)           # "_2", "_3", …
           room      = 32 - suffix.size()         # reserve suffix space
           candidate = base.substr(0, room) + suffix
           if candidate not in used:
             id = candidate; break
         if id == "":
           throw ConfigError("mcp_servers: too many id collisions for '" + key + "'")
       used.insert(id); assign id to key
   ```

   **Properties.** Each `candidate` is a valid id: `base` is valid and non-empty
   (`normalize_mcp_server_id` guarantees first char `[a-z]`, remainder
   `[a-z0-9_]`), `room ≥ 26` for every `n ≤ 10000`, and the suffix `_<digits>` is
   legal, so the first character and grammar are preserved. `used` guarantees the
   assigned ids are pairwise distinct. The loop is bounded by
   `kMaxMcpDedupeAttempts`, so it terminates; on exhaustion it fails **loudly**
   with `ConfigError` rather than emitting a duplicate. Normalization is
   deterministic (nlohmann sorts object keys). Tool namespacing downstream uses
   the normalized id only.
2. `type` is accepted as an alias of `transport`. If both are present and differ
   ⇒ `ConfigError`. Accepted values: `stdio`, `http_sse`; the localcode synonyms
   `http` and `sse` map to `http_sse` (gate M2). A non-`stdio` server requires a
   non-empty `url`.
3. `env` is an **object**; `headers` is an **object**; both are flattened to the
   existing `vector<string>` of `"K=V"` entries (`headers` → `header_env`). Values
   may be literal or `${VAR}` references (expanded by the unchanged
   `resolve_mcp_env`). Keys are emitted in sorted order. A non-string value ⇒
   `ConfigError` naming the server and key. **Recorded (N13):** `header_env` is
   parsed and stored but is **not consumed** by any shipped transport — only
   `StdioMcpTransport` exists (`include/ymh/mcp/mcp_transport.hpp:65`) and the
   manager's factory is hardcoded to `make_stdio_mcp_client`
   (`src/mcp/mcp_manager.cpp:46-49`). The `headers` mapping is forward-compatible
   storage for a future HTTP/SSE transport; it is not a live feature (§14 Q16).
4. Every ymh extension field above is optional and parsed with the existing
   `McpServerSettings` defaults; the full existing key set (`cwd`, `url`,
   `header_env`, `protocol_version`, …) is accepted. **Unknown keys remain a hard
   `ConfigError`** (strictness preserved).
5. There is **no `read_only` key** and no MCP `readOnlyHint` handling (25-W5);
   plan mode does not classify MCP tools.
6. The `[mcp]` section keeps its tuning knobs (`max_servers`, timeouts, reconnect,
   `max_frame_bytes`, `allow_network_servers`, …), unchanged.

**`${VAR}` escaping (gate M3).** The implementation expands only `${VAR}`. To let
the importer copy a literal value that contains `${`, `resolve_mcp_env` gains one
additive escape rule: the sequence `$${` emits a literal `${` and scanning
continues after the brace (so `$${TOKEN}` resolves to `${TOKEN}`). `$$` not
followed by `{` is unchanged. The importer escapes every literal `${` in a copied
value as `$${` (25-D15). The spec's Rev-1 `$VAR` wording was wrong; the pinned
syntax is `${VAR}`.

**Migration (backward compatibility; gate Oracle M4).**

- Per layer: if `mcp_servers` is present **and** `[mcp].server` is non-empty in the
  **same layer** ⇒ `ConfigError`
  (`"define MCP servers in mcp_servers or mcp.server, not both"`). No silent merge.
  The rule is layer-local, so a workspace `[mcp].server` does not false-positive
  against a global `mcp_servers`.
- If only `[mcp].server` is present in a layer ⇒ loads exactly as before
  (deprecated, no warning printed).
- If only `mcp_servers` is present ⇒ loads via the new path.
- **Cross-layer semantics (pinned):** the existing layered loader
  (`src/config/config.cpp:871-877`) applies the workspace layer over the global
  one; `apply_mcp` replaces the server array wholesale per layer
  (`:452-460`). Therefore a workspace layer that defines either key replaces the
  entire global server set — this is existing behaviour, now stated explicitly.
- Both shapes feed the same `Config::mcp.servers`, so nothing downstream changes.

Justification for compatibility over a hard error: the global layer is
**required** (`21-D12`), so a hard error on the old shape would brick every
existing ymh config on upgrade. Recorded as §14 Q8.

### 8.3 C++ interface sketch (pinned)

```cpp
// include/ymh/config/config.hpp — additive parse entries (struct unchanged)
// Parses the localcode-shaped `mcp_servers` object into `mcp.servers`, applying
// the key normalization and `type`/`headers`/`env` rules above. Throws ConfigError
// on any shape/grammar violation. `source` is used only for error text.
void apply_mcp_servers_object(McpSettings& mcp, const nlohmann::json& table,
                              const std::filesystem::path& source);

// include/ymh/mcp/mcp_types.hpp — additive declaration (definition in
// src/mcp/mcp_types.cpp). It MUST be header-visible because `config.cpp`
// (`apply_mcp_servers_object`) calls it from a different TU (L-5 Rev 5).
// Deterministic normalization of a localcode server name to a valid ymh id.
[[nodiscard]] std::string normalize_mcp_server_id(std::string_view name);

// include/ymh/config/config.hpp — allowlist addition
// apply_document's reject_unknown gains "mcp_servers" (src/config/config.cpp:537-539).

// src/mcp/mcp_transport.cpp — resolve_mcp_env gains the `$${` escape (25-D13).

// include/ymh/mcp/mcp_manager.hpp — extracted validation (25-D16; M2/N2)
// `McpManager::validate()` (src/mcp/mcp_manager.cpp:70-103) becomes a thin wrapper
// over these two free functions so the importer can run the daemon's own semantic
// checks before writing. Both return an empty string when valid, else a
// human-readable reason that NEVER contains an env/header value.
[[nodiscard]] std::string validate_mcp_server(const McpServerConfig& server,
                                              const McpConfig& config,
                                              const ToolConfig& tools);
[[nodiscard]] std::string validate_mcp_config(const McpConfig& config,
                                              const ToolConfig& tools);

// 25-D16 (Rev 4): the importer passes the daemon's effective ToolConfig (the
// daemon default-constructs `tool_config_`, src/agent/workspace_runtime.cpp:93)
// and forces `required = false` on every copied server before validating — a
// required server that fails to start rethrows from McpManager::start
// (src/mcp/mcp_manager.cpp:228-230,244-246), which would brick every start.

// include/ymh/mcp/mcp_types.hpp — bound used by the dedupe above; header-visible
// for the same cross-TU reason (L-5 Rev 5).
inline constexpr std::size_t kMaxMcpDedupeAttempts = 10000;
```

---

## 9. D14/D15 — First-run localcode import

### 9.1 Current state (verified)

- `run_cli` order (`src/cli/cli.cpp:742-788`): `--host` short-circuit → `parse_cli`
  → `Version`/`Config` → `resolve_workspace` → **`scaffold_for_invocation(...)`**
  → `command_loads_config` → `load_invocation_config` → dispatch. The scaffold
  call is at `src/cli/cli.cpp:770`.
- `scaffold_config` (`src/config/config.cpp:738-763`) creates the global directory
  (`ensure_directory`, `:738-745`) and writes the default `config.jsonc` only if
  it does not exist (`write_default_config` uses a bare `std::ofstream`,
  `:688-712`); an explicit `--config` target is never auto-created (`21-D12`/`21-D14`).
- The only interactive prompt helper is `workspace_stop_may_proceed`
  (`src/cli/cli.cpp:564-588`), the idiom to reuse: injected `std::istream&`/
  `std::ostream&`, an `interactive` bool, `std::getline`, trim, accept `y`/`Y`.
  The config layer is deliberately I/O-free, so the prompt must live in the CLI.
- `~/.localcode/config.json` top-level keys (the user's real file):
  `auto_compact_enabled`, `auto_compact_percent`, `auto_delegate`,
  `default_profile`, `keep_going`, `max_concurrent_tasks`, `mcp_servers`,
  `model_invocable`, `orchestrate`, `permission`, `profiles`, `providers`,
  `skip_permissions`, `smart_agent`. `permission` is a list of
  `{match, decision}` glob rules; `providers` maps name→`{type, base_url,
  api_key}` (inline secrets); `profiles` maps name→`{provider, model, max_tokens,
  context_window}`.

### 9.2 Decision (25-D14) — trigger, ordering, wording, default

**Trigger (all must hold):**

1. `invocation.command == CliInvocation::Command::Tui` (interactive first start
   only; `ymh run`/`list`/`show`/`replay`/`fork` never prompt).
2. `invocation.config_path` is empty (an explicit `--config` is never replaced).
3. The conventional global config **directory** does not exist:
   `!std::filesystem::exists(default_global_config_path().parent_path())`.
4. `~/.localcode/config.json` exists and is a regular file
   (`localcode_config_path()` = `$HOME/.localcode/config.json`).
5. `::isatty(STDIN_FILENO) != 0` and `::isatty(STDOUT_FILENO) != 0` (never
   block a script; mirrors `src/cli/cli.cpp:314`).

**Malformed / unreadable / oversized localcode file (gate M4).** Before prompting,
the CLI reads the file with a hard size bound
`kLocalcodeImportMaxBytes = 4u * 1024u * 1024u`:

- unreadable (open/read error), not valid JSON, or larger than the bound ⇒ print
  exactly one line to `stderr`:
  `ymh: localcode config at <path> is <unreadable|not valid JSON|larger than 4 MiB>; skipping import`
  and proceed directly to `scaffold_for_invocation`. No prompt, no partial state.
- valid JSON of the wrong top-level type (not an object) is treated as
  "not valid JSON" for this purpose.

**Ordering:** the import step runs in `run_cli` **before**
`scaffold_for_invocation`. If accepted it writes the global config, so the
subsequent scaffold sees the file and does not overwrite. If declined, scaffold
proceeds and writes the default. `command_loads_config` is not consulted for the
prompt (Tui always loads config), so no ordering hazard.

**Wording (pinned; better than the user's draft):**

```
ymh: first run — no config at <global-config-path>.
     Found localcode settings at <localcode-path>.
     Import MCP servers, model, compaction, and concurrency? (Permission rules
     and API keys are not imported; ymh keeps its own permission prompts.)
     [Y/n] 
```

Printed to `stdout`; the prompt is flushed. `<global-config-path>` and
`<localcode-path>` are the resolved absolute paths.

**Answer handling:**

- Empty line ⇒ **yes** (the prompt shows `[Y/n]`).
- `y`/`Y`/`yes` (case-insensitive) ⇒ yes.
- `n`/`N`/`no` (case-insensitive) ⇒ no.
- EOF / read failure ⇒ no (never hang).
- Any other input ⇒ re-prompt once, then treat as no (bounded; no infinite loop).
- Non-interactive ⇒ no prompt at all, no message; proceed to scaffold.

**On no:** print nothing further; `scaffold_for_invocation` writes the default
config as today. No partial state.

**On yes:** generate, validate, and write (25-D15). On any generation/validation
failure, print a clear message and fall back to the default scaffold (never a
partial or invalid config). Recorded as §14 Q9.

### 9.3 Decision (25-D15) — mapping, validation, secrets, file mode

**Key mapping (only ymh-supported keys; everything else is ignored):**

| localcode key | ymh key | Rule |
|---|---|---|
| `mcp_servers` (object) | `mcp_servers` (object) | Copied through the 25-D13 normalization (names, `type`/`http`/`sse`, `env`/`headers` objects, `url`, `cwd`). Values, including secrets, are copied with `${` escaped as `$${`. **`required` is forced to `false`** (N2 Rev 4) — see validation step 1; `enabled` is preserved. |
| `auto_compact_enabled` (bool) | `agent.compaction.enabled` | Direct. |
| `auto_compact_percent` (number) | `agent.compaction.threshold_ratio` | `percent / 100.0`; clamp to `(0,1]`. |
| `max_concurrent_tasks` (int) | `llm.default.max_concurrency` | Direct if `> 0`. |
| `skip_permissions == true` | — | **Not imported** (gate M13). ymh keeps its permission defaults; the importer prints one note: `ymh: note: localcode 'skip_permissions' is not imported; permission prompts stay enabled`. |
| `default_profile` + `profiles[name]` + `providers[profile.provider]` | `llm.default.base_url`, `llm.default.model`, `agent.compaction.context_window_tokens` | Only when `default_profile` names an existing profile and its `provider` entry's `type` is exactly `"openai-compatible"` (`LlmSettings::provider` default, `include/ymh/config/config.hpp:97-103`); any other `type` is skipped. Map `base_url` and `model`; map `context_window` to `context_window_tokens`. Do **not** copy `max_tokens`. |
| `providers[*].api_key` | — | **Ignored.** ymh's `LlmSettings` stores only `api_key_env` (an env-var *name*), not an inline key (`include/ymh/config/config.hpp:97-103`). The generated config leaves `api_key_env` at its default and the user sets the env var. |
| `permission` (glob rule list) | — | **Ignored**, with one note when non-empty: per-glob rules are not representable as ymh's three coarse modes. |
| `orchestrate`, `smart_agent`, `keep_going`, `model_invocable`, `auto_delegate`, `default_profile`'s unmapped fields | — | Ignored. |

If a mapped key has the wrong JSON type, it is skipped (the rest still maps); a
mapping that would produce an invalid document is caught by validation below.

**Validation before write (mandatory; gate H4; N2/N8).** Build the ymh JSONC
object in memory, then:

1. **Semantic validation against the daemon's own rules (25-D16; N2).** Shape
   validation is not enough: `apply_jsonc_file` never calls
   `McpManager::validate()` (`src/config/config.cpp:766-797`), so a generated
   config can parse yet make `WorkspaceRuntime::create` throw at the next start
   (`src/agent/workspace_runtime.cpp:129`), which — because the global layer is
   required — would brick every subsequent start. The CLI layer
   (`maybe_import_localcode_config`, which can include the MCP headers without
   making `config.hpp` depend on `mcp_manager.hpp`) therefore runs the daemon's
   own checks on the in-memory document before writing:
   - Parse the generated `mcp_servers` object into a defaults `Config` with
     `apply_mcp_servers_object` (the same parse path the loader uses). That
     populates `McpSettings`/`McpServerSettings` (`include/ymh/config/config.hpp:112-135`),
     so the importer then converts with `to_mcp_config(candidate_config)`
     (`include/ymh/cli/wiring.hpp:28`, definition `src/cli/wiring.cpp:99`) to get
     the `McpServerConfig`/`McpConfig` the validators take (LOW-2 Rev 5).
   - For each server, call
     `validate_mcp_server(server, candidate_mcp, ToolConfig{})` (the free function
     extracted from `McpManager::validate()`, §8.3). If it returns a non-empty
     reason, **erase that server's key** from `document["mcp_servers"]` and print
     exactly one stderr line
     `ymh: import: skipping mcp server '<key>': <reason>`. The reason is the
     daemon's own text and never contains an env/header value
     (`resolve_mcp_env` errors name the variable, not its value).
   - Then call `validate_mcp_config(candidate_mcp, ToolConfig{})` on the reduced
     set as a backstop (id grammar, duplicate ids). A non-empty result aborts the
     import (step 5).
   This closes the parse-passing/runtime-throwing brick paths: an `http`/`sse`
   server mapped to `http_sse` while `allow_network_servers` is false (the
   default, `include/ymh/config/config.hpp:149`; gate
   `src/mcp/mcp_manager.cpp:79-86`), a stdio server whose `${VAR}` is unset
   (`resolve_mcp_env`, `src/mcp/mcp_manager.cpp:92-96`), and a server whose
   `max_result_bytes` exceeds the daemon's `tool_result_max_bytes`
   (`:98-101`; the daemon default-constructs its `ToolConfig`,
   `src/agent/workspace_runtime.cpp:93`, so the importer's `ToolConfig{}` is the
   same value the daemon uses). **Reason precedence:** the non-`stdio` transport
   check runs first, so such a server is omitted with exactly one line
   `ymh: import: skipping mcp server '<key>': no http_sse transport implemented`
   even when `allow_network_servers` is true (only `stdio` is implemented,
   `src/mcp/mcp_manager.cpp:46-49`); otherwise `validate_mcp_server` supplies the
   reason (`"http_sse requires allow_network_servers"`, the unset variable name,
   a `max_result_bytes` reason, …).
   - **`required` is never imported (N2; Rev 4).** `build_localcode_import`
     (§9.4) forces `required = false` in the generated object for every copied
     server; if the localcode server had `"required": true`, the CLI prints one note
     `ymh: import: server '<key>': 'required' is not imported (server starts non-required)`.
     Rationale: a `required=true` server that fails to start rethrows
     (`McpManager::start`, `src/mcp/mcp_manager.cpp:228-230,244-246`), and
     `WorkspaceRuntime::Impl` constructs the manager and calls `mcp_->start({})`
     during `create` (`src/agent/workspace_runtime.cpp:129-135`), so such a server
     would re-brick every start. The importer can prove a server **validates**
     (above); it can never prove a server **starts**, so it must not mark any
     server required. `required` is a ymh extension (§8.2 rule 4), so dropping it
     is always shape-valid.
   - **Sufficiency of the guarantee (L-4 Rev 5).** The imported keys that can
     prevent a start are the MCP servers **and** the mapped provider `base_url`.
     Permission rules and skills are **not** imported (see the mapping table
     above), so per-server `validate_mcp_server` plus `required=false` makes the
     MCP half complete; `validate_mcp_config` is the whole-document backstop (id
     grammar, duplicate ids), and a non-empty result aborts the import (step 5).
     For the provider half, `build_localcode_import` maps `base_url` only when it
     passes the daemon's own `is_http_url` predicate
     (`src/llm/provider_registry.cpp:22,65`); otherwise the LLM mapping is omitted
     with exactly one note. This is required because an invalid `base_url` does
     **not** throw at `WorkspaceRuntime::create`: `ProviderRegistry::create`
     returns `unexpected` (`src/llm/provider_registry.cpp:65-67`), so
     `runtime_->provider() == nullptr` and the daemon exits
     `HostExitCode::StartupRejected` (`src/host/workspace_host.cpp:503-505`) —
     a non-throwing brick that the throws-only argument did not cover.
   - **A failed import leaves no bad global config.** The trigger already requires
     the global config *directory* to be absent (§9.2 trigger 3), so there is no
     previous global config to corrupt. On any failure the temp is unlinked and
     the default scaffold runs (step 5), and the generated document is never
     `rename`d unless **both** the shape check (`apply_jsonc_file`) and the
     semantic checks (`validate_mcp_server`/`validate_mcp_config`) pass.
2. **Create the global config directory** with
   `std::filesystem::create_directories(global_config.parent_path(), ec)` (gate M5
   / Oracle L8). On failure: print the fallback message and let the scaffold try.
3. Serialize to a sibling temp file `<global-config>.import.tmp`, opened with
   POSIX flags **before any bytes are written**:
   `::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)` (gate H4).
   The temp file therefore has mode `0600` from creation; no window exists in
   which secrets are world-readable. If `O_EXCL` fails because a stale temp
   exists, unlink it once and retry; a second failure aborts the import.
4. Apply the temp file to a **fresh defaults `Config`** with
   **`apply_jsonc_file(validation_config, tmp, /*required=*/true)`** —
   explicitly this function, **not `load_config`** (N8). `load_config` reads the
   fixed real paths and enforces global-layer presence
   (`src/config/config.cpp:871-877`), so following the Rev-2 wording literally
   would validate the wrong file (or none). `apply_jsonc_file`
   (`:766-797`) is the only entry that can be pointed at the temp. It performs
   shape/type validation; the semantic guarantee is step 1. Only on success
   `::rename(tmp, global_config)` (same directory, atomic). `rename` preserves the
   `0600` mode, so the final `config.jsonc` is `0600`.
5. On any failure: `::unlink(tmp)`, print
   `ymh: imported config failed validation: <error>; writing the default config instead`,
   and let the scaffold write the default.

This guarantees ymh never writes a config it cannot load, never leaves a
secret-bearing temp file behind, and never creates a world-readable secret file.
The default scaffold keeps its current mode (it contains no secrets).

**Secrets.** `mcp_servers[*].env` and `headers` values are copied into the user's
own config file. The importer and all logging:

- must **never** log env/header values, the localcode file body, or the generated
  body;
- must not include env/header values in any `ConfigError`/notice text;
- creates the imported `config.jsonc` with mode `0600` (owner read/write).

This is the only place ymh stores secrets in config; `llm.api_key_env` remains a
reference. Recorded as §14 Q10.

### 9.4 C++ interface sketch (pinned)

```cpp
// include/ymh/config/config.hpp — pure mapping (no file I/O, unit-testable)
// Builds a ymh config document (JSON object) from a parsed localcode document,
// including ALL mapped servers. Returns std::nullopt and fills `error` on an
// unrecoverable shape problem. Never logs, never touches the filesystem, never
// copies provider api_key, escapes literal `${` as `$${` in every copied MCP
// value, and forces `required=false` on every copied server (N2 Rev 4). It does
// NOT run the semantic MCP validation (that lives in the CLI layer, which can
// include the MCP headers without making config.hpp depend on mcp_manager.hpp).
[[nodiscard]] std::optional<nlohmann::json> build_localcode_import(
    const nlohmann::json& localcode, std::string& error);

// `$HOME/.localcode/config.json`.
[[nodiscard]] std::filesystem::path localcode_config_path();

// include/ymh/cli/cli.hpp — CLI-layer orchestration (I/O + prompt)
// Returns true iff a config was imported and written. Runs only under 25-D14's
// trigger. `in`/`out` are injected for tests. Creates the global directory and
// uses a 0600 temp + rename.
[[nodiscard]] bool maybe_import_localcode_config(const CliInvocation& invocation,
                                                 const std::filesystem::path& global_config,
                                                 bool interactive,
                                                 std::istream& in,
                                                 std::ostream& out,
                                                 std::ostream& err);

// include/ymh/cli/cli.hpp — pinned size bound
inline constexpr std::size_t kLocalcodeImportMaxBytes = 4u * 1024u * 1024u;
```

`run_cli` calls `maybe_import_localcode_config(...)` immediately before
`scaffold_for_invocation` (`src/cli/cli.cpp:770`), Tui-only.

---

## 10. Invariants

Numbered `UX1`–`UX47`; testable and cited. Rev-1 invariants that described the
withdrawn tool-denial design (Rev-1 UX7–UX9, UX-F2/F3) are retired; see §15.
Rev 3 amends UX5, UX12, UX18, UX19, UX34 and adds UX42–UX43. Rev 4 amends UX12,
UX18, UX19, UX41–UX43 and adds UX44–UX45. Rev 5 amends UX12, UX18, UX19, UX45 and
adds UX46–UX47.

| ID | Invariant |
|---|---|
| UX1 | **Mode replaces idle.** For an active session, the status line's first segment is `plan` when `status.plan_active`, else `build`; the literal `"idle"` never renders. `AgentState::Idle` renders no state segment. |
| UX2 | **Segment order.** Display order is mode, state (non-idle only), model, counters, tps, context, note, notice, joined by `" · "`, then the right aggregate widget. |
| UX3 | **Counters always on.** For an active session the counters segment renders even when all three are zero. |
| UX4 | **Note and notice retained.** `status.note` (when non-empty) and `model.notices.back()` (when non-empty) render in `render_status`, preserving spec 22 §3.6 / 22-A8. |
| UX5 | **Width degradation.** `render_status` receives the terminal width; `mode` is always included and `model` is reserved second (included, ellipsized to ≥4 columns, whenever ≥4 remain after `mode`); the remaining optional segments are included by priority `counters > context > note > notice > tps > state` using the ellipsized `model` width in the fit test; the right widget is always emitted. |
| UX6 | **Bar geometry.** Exactly 10 cells; `window==0` ⇒ 10 empty + `"—"`; otherwise `filled = clamp((used*10)/window, 0, 10)`, forced to 1 only when `window>0 && used>0 && filled==0`. |
| UX7 | **TPS is per completed message.** `tps = output_tokens / seconds(Finished − Started)`, computed only when `output_tokens > 0` and `elapsed ≥ 250 ms`; otherwise the previous value is retained. |
| UX8 | **TPS clock is injectable.** Unit tests drive TPS through `UiModel::set_now_reader`; production defaults to `steady_clock::now`. |
| UX9 | **Status dirty marking.** Every write to `tps`, `context_used_tokens`, or `context_window_tokens` marks `UiDirtyFlag::Status` for that session. |
| UX10 | **Context source is read-only.** The supervisor reads `context.show`; it does not mutate `model_.context` or `UiMode` when refreshing the status line. |
| UX11 | **Plan mode is durable and whole-value.** Plan state is the last `plan/mode` event in the session log; a log with none is inactive. |
| UX12 | **Plan state is a projection, memoized by `SessionId`.** `plan_mode_active(events)` is the only read path for policy/prompt purposes; resume, fork, and replay restore the state by folding the log. A non-authoritative memo keyed by `SessionId` alone **must** back the per-step `active()` call so the log is folded at most once per session (N10/M-1 Rev 5). The memo is written by the first `active()` fold and by every controller commit, is invalidated only by the controller's own commits (the sole `plan/mode` writer), is dropped on `erase`, and must never be read as truth. Keying on the session's last `Sequence` is **forbidden**: it advances on every appended event and re-folds every step. |
| UX13 | **No in-memory authority.** There is no `AgentMode` enum and no authoritative mode store; the `PlanModeController` may hold only an *uncommitted* selection and a non-authoritative memo. |
| UX14 | **Plan state is per-session and daemon-side.** Two sessions may have different plan states; the status line reads the folded event stream, not another session's state. |
| UX15 | **Prompt section.** While active, the `plan:policy` section is appended to the system prompt; while inactive it contributes no bytes. |
| UX16 | **Stable tool catalog.** `exit_plan_mode` is registered in both states; entering/leaving plan mode changes only the prompt section, never `ContextAssembler::tools()`. |
| UX17 | **Exit tool fails closed outside plan mode.** `exit_plan_mode` outside plan mode returns an error `ToolResult` and makes no permission request. |
| UX18 | **Review uses the generic option flow, shows a real plan, and is never blind.** While active, an `exit_plan_mode` call whose `plan` argument is a non-empty string forces an ordinary permission Ask via `PermissionRequest::force_ask` (even under `default_verdict == Allow`, a broad Allow rule, or a remembered grant) whose dialog summary is the `plan` argument truncated to `kMaxPlanSummary = 4096` bytes **and** `kMaxPlanSummaryLines = 12` lines with the ASCII `"..."` marker, rendered inside a height-bounded frame so the option rows and hint stay visible (NEW-2 Rev 5). A missing/empty/non-string `plan` makes **no** permission request: the loop appends an error `ToolResult` and plan mode cannot exit (NEW-1 Rev 5). Allow ⇒ `{"approved":true}` only on `ToolOutcome::Ok`, then `request_exit`; Deny ⇒ the existing denial path with the reason as feedback. The tool is `destructive=false` and `force_ask` is not read by `tool_is_mutating`, so `SandboxMode::ReadOnly` does not pre-rule deny it. |
| UX19 | **Exit commits at the step boundary or on any turn exit.** An approved review records a pending exit **only if the tool returned `ToolOutcome::Ok`** (a failed/misrouted `exit_plan_mode` never exits plan mode, NEW-1 Rev 5); `plan/mode{active:false}` is appended at the next accepted step boundary, not during the tool batch. If no further accepted step occurs, the turn-exit scope guard calls `flush_pending_at_turn_end` on **every** exit path (normal end, error, cancellation, lease/store failure, step limit) **and in `runMaintenanceTurn` after its `TurnStarted`** (LOW-5 Rev 5), so the append may land after the terminal event (last-wins fold). |
| UX20 | **Plan mode restricts no tool.** No code path consults plan state to allow/deny a tool; sandbox mode and the approval policy are independent and unread by plan mode. |
| UX21 | **Commands stay outside history.** `/plan`, `/plan off`, and their results never become session events; a non-empty `/plan <message>` suffix becomes exactly one user message via `agent.steer`. |
| UX22 | **No protocol bump.** `session.set_mode` is additive; `kProtocolVersion` is unchanged; a daemon that lacks it yields a supervisor notice, not a failure. |
| UX23 | **`build` is a label only.** There is no `AgentMode::Build`, no `/build` command, and no `build` config value; `build` is the display label for `plan_active == false`. |
| UX24 | **Highlight/insert sync.** After any `complete_command`, `command_hint_selected` indexes the candidate whose name is in `input.draft`. |
| UX25 | **Enter accepts the highlight.** With a non-empty bare `/`-prefix draft (not exactly `/`) and non-empty `command_hints`, Enter completes to `command_hints[clamped selected]` and dispatches it; a bare `/` or a space in the draft dispatches the draft unchanged. |
| UX26 | **Dispatch clears hints.** The existing dispatch-reset block clears `command_hints` and resets the completion state. |
| UX27 | **`/exit` never prompts.** The `/exit` command path calls `begin_exit(false)` and reaches `confirm_exit` without `open_exit_prompt`. |
| UX28 | **Ctrl+D may prompt.** Ctrl+D calls `begin_exit(true)`, preserving spec 16's dialog when the orphaning set is non-empty and `--yes` is absent. |
| UX29 | **Spec-16 teardown unchanged.** Both paths compute the orphaning set and call the same `confirm_exit`/`teardown_daemons`; only the prompt is skipped. |
| UX30 | **`/skill` is gone.** No registry entry, no `CommandContext::skill`, no `/help` row for `skill`; `/skills` description is exactly `"list available skills"`. |
| UX31 | **Skill tool retained.** The model-invoked `skill` tool and `/skills --show` are unchanged. |
| UX32 | **Aliases are dispatch-and-help only.** `find`/`dispatch` resolve aliases; `complete` matches canonical names only; handlers are not duplicated; `/help` renders `(alias: /quit)`. |
| UX33 | **`/quit` ≡ `/exit`.** `/quit` resolves to the same `Command` and the same handler. |
| UX34 | **`mcp_servers` is top-level and object-shaped.** The key is accepted by `apply_document`; the object key is the localcode server name, normalized deterministically to a valid ymh id. Colliding normalized ids are disambiguated by the terminating algorithm of §8.2 rule 1 (suffix appended to a base truncated first, bounded by `kMaxMcpDedupeAttempts`, `ConfigError` on exhaustion) — never by re-truncating a 32-char id. |
| UX35 | **Full key set + strictness.** `type`/`transport`, `command`, `args`, `env`, `url`, `headers`, `cwd`, `protocol_version`, and the ymh extensions are accepted; unknown keys are a `ConfigError`; `type`/`transport` conflict is a `ConfigError`. |
| UX36 | **`env`/`headers` flattening and escaping.** `{"K":"V"}` becomes `"K=V"`; `${VAR}` expansion is unchanged; a literal `${` copied by the importer is written as `$${` and resolves back to `${`. |
| UX37 | **Migration is layer-local and mutually exclusive.** `mcp_servers` + non-empty `[mcp].server` in one layer ⇒ `ConfigError`; across layers the workspace layer replaces the global server array wholesale. |
| UX38 | **Import trigger is narrow.** TUI only, no explicit `--config`, global dir absent, `~/.localcode/config.json` a regular file ≤4 MiB with valid JSON, both stdin and stdout TTYs. |
| UX39 | **Import ordering and directory creation.** The import runs before `scaffold_for_invocation` and creates the global config directory before writing; an accepted import prevents the scaffold from overwriting. |
| UX40 | **Import default is yes; EOF/non-interactive is no.** `[Y/n]`; empty = yes; unknown re-prompts once then no; EOF = no; non-TTY = skip silently. |
| UX41 | **Import is validated and secret-safe before write.** The generated document is shape-validated by `apply_jsonc_file(candidate, tmp, /*required=*/true)` from a sibling temp file opened `0600` **and** semantically validated by `validate_mcp_server`/`validate_mcp_config` before any write; every copied server is forced `required=false`; on failure nothing invalid is written, the temp is unlinked, and the default scaffold runs; the final file is `0600`; secrets never appear in logs/error text; `skip_permissions` is not imported. |
| UX42 | **Exit review survives ReadOnly.** `exit_plan_mode` is `destructive=false` and `PermissionRequest::force_ask` is not read by `tool_is_mutating`, so the classification is false for it and `SandboxMode::ReadOnly` does not pre-rule deny the review; the forced Ask still runs the resolver. No other tool's classification changes. |
| UX43 | **Imported MCP servers are startable.** Every server written by the importer passes the daemon's own `validate_mcp_server` and is written with `required=false`; a server that fails validation is omitted with exactly one stderr note, so the written global config always starts the daemon. |
| UX44 | **Forced review is non-mutating and never remembered.** `PermissionRequest::force_ask` makes `RulePermissionPolicy::evaluate` return `Ask` after the ReadOnly deny and before rules, `tool_defaults`, `default_verdict`, and grants; `tool_is_mutating` never reads it; it is set only for `exit_plan_mode` while plan is active; a forced decision is never recorded as a grant (`PermissionBroker::onDecision` and the loop's direct-policy path skip `remember` when `force_ask`). |
| UX45 | **Every turn exit flushes pending plan state, and never throws.** A scope guard installed after `TurnStarted` (in both `runTurn` and `runMaintenanceTurn`) calls `flush_pending_at_turn_end` on normal end, error, cancellation, lease/store failure, and step limit; the call is idempotent, null-safe, and `noexcept` (an append failure inside the flush is swallowed/logged, never propagated from a destructor — LOW-4 Rev 5); the appended `plan/mode` may follow the terminal event. |
| UX46 | **A forced review offers no persistent option.** When `dialog.force_ask` is true the dialog renders and accepts exactly `{Allow once, Deny}` (default `Deny`), so the user is never offered an "Always allow" that the policy would silently ignore; a raw `scope=Always` decision for a forced request is honoured for that review and its grant discarded (NEW-3 Rev 5). |
| UX47 | **A plan exit requires a real plan.** `exit_plan_mode` with a missing, empty, whitespace-only, or non-string `plan` argument makes no permission request, returns an error `ToolResult`, and does not exit plan mode; only a `ToolOutcome::Ok` result triggers `request_exit` (NEW-1 Rev 5). |

---

## 11. Failure modes

Prefix `UX-F#` (trigger / symptom / recovery).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| UX-F1 | `session.set_mode` fails (daemon down, `MethodNotFound`) | The UI would show a mode the daemon does not enforce | Never write `plan_active` from the reply at all; render only the folded event stream, show a one-line notice (UX22, N6) |
| UX-F2 | A plan change is requested mid-turn | Prompt section and log disagree | Queue the selection; commit at the next accepted step boundary **or on any turn exit** via the `flush_pending_at_turn_end` scope guard (UX13/UX19, N5) |
| UX-F3 | An `exit_plan_mode` review is dismissed / the daemon reloads while pending | A stale exit could commit | The pending exit is process-local; on reload it is dropped and plan mode stays active; `/plan off` is the manual escape (UX19). A pending exit is **not** dropped merely because the turn ends: the turn-end flush commits it (N5) |
| UX-F4 | `usage.output_tokens` is cumulative for some provider | Inflated TPS | Recorded assumption; TPS is cosmetic and never feeds policy (UX7, §14 Q4) |
| UX-F5 | First message elapsed < 250 ms | A wild TPS spike | Skip the update, retain the previous value (UX7) |
| UX-F6 | `context.show` reply malformed or the session is gone | Stale/zero context bar | Keep the last good values; on a malformed reply leave `window=0` and render `—` (UX6/UX10) |
| UX-F7 | `window_tokens == 0` and config window is 0 | Division by zero / `NaN` | `format_percent` returns `"—"`; bar all empty; the visibility floor does not apply (UX6) |
| UX-F8 | Tab/Shift+Tab interleaved | Highlight/insert drift | Highlight is always set to `applied` after the draft is written (UX24) |
| UX-F9 | Enter with `command_hint_selected` out of range | Out-of-bounds index | Clamp to `hints.size()-1` (UX25) |
| UX-F10 | Enter on a draft containing a space (e.g. `/skills --show x`) | The highlight would clobber arguments | `accept_highlight` returns false; dispatch the draft (UX25) |
| UX-F11 | `/exit` while a daemon is orphaning | The user expects a dialog | By decision, `/exit` exits; Ctrl+D is the prompting path (UX27/UX28, §14 Q11) |
| UX-F12 | Ctrl+D with `--yes` | Dialog suppressed | `no_prompt` still auto-confirms (UX28) |
| UX-F13 | `mcp_servers` and `[mcp].server` both non-empty in one layer | Silent, ambiguous server set | `ConfigError`; no partial load (UX37) |
| UX-F14 | localcode `env`/`headers` object contains a non-string value | Flattening would lose data | `ConfigError` naming the server and key; no config written (UX35) |
| UX-F15 | Import generation yields an unloadable document | ymh would be bricked on next start | Semantic-validate with `validate_mcp_server`/`validate_mcp_config`, force `required=false`, **and** shape-validate `apply_jsonc_file(candidate, tmp, /*required=*/true)` from a `0600` temp before rename; on failure unlink and write the default (UX41) |
| UX-F16 | localcode file contains secrets and logging is at debug | Secret leak | Import and mapping never log values; the temp/final file is `0600`; the generated body is not logged (UX41) |
| UX-F17 | localcode file is malformed/unreadable/oversized | A prompt over an unusable file | Skip the import with one stderr line; proceed to scaffold (UX38) |
| UX-F18 | A localcode server name is not a valid ymh id (hyphen/uppercase) | The whole import would fail validation | Normalize deterministically; disambiguate collisions; never fail on the name alone (UX34) |
| UX-F19 | An imported server fails the daemon's own validation (http/sse while `allow_network_servers=false`, unset `${VAR}`, bad `max_result_bytes`) or is marked `required=true` in localcode | The written global config would make every future start throw | `validate_mcp_server` omits a failing server and prints exactly one stderr note; `required` is forced `false` with one note; the rest of the config is written; a whole-document failure aborts to the default scaffold (UX43) |
| UX-F20 | A queued selection/exit exists when the turn ends (normally, by error, or by cancellation) with no further accepted step | The log never records the user's choice; the UI and log diverge | The turn-exit scope guard calls `flush_pending_at_turn_end` on every exit path (including `runMaintenanceTurn`); it commits exactly once, is `noexcept`, and may append `plan/mode` after the terminal event (UX19/UX45, N5) |
| UX-F21 | The model calls `exit_plan_mode` with a missing/empty/non-string `plan` | A blind review (dialog shows only `tool: exit_plan_mode`) and an exit on an empty plan | The loop's pre-Ask argument gate makes no permission request and appends an error `ToolResult`; `request_exit` requires `ToolOutcome::Ok` (UX18/UX47, NEW-1 Rev 5) |
| UX-F22 | The user picks "Always allow" on an `exit_plan_mode` review | A dead `grant:exit_plan_mode` that never suppresses the next prompt — a surprise | The forced dialog omits the persistent options (only Allow once/Deny, default Deny) and neither the broker nor the loop records a grant for a `force_ask` request (UX44/UX46, NEW-3 Rev 5) |

---

## 12. dsh mapping

- **Plan mode IS event-sourced.** This is the Rev-2 correction: `plan/mode` is a
  log-only, non-surface, whole-value-replace session event; the `plan` projection
  folds the log so resume, fork, and replay restore the state. There is no live
  mirror and no authoritative in-memory mode.
- **Guidance, not enforcement.** While active, a deployment-owned `plan:policy`
  section is included in each request; every tool stays callable. Sandbox mode and
  approval policy enforce restrictions **independently** and do not read or write
  plan state (ymh already has both: `SandboxMode::ReadOnly` +
  `tool_is_mutating` + `RulePermissionPolicy::evaluate`).
- **Stable tool catalog.** `exit_plan_mode` remains registered while plan mode is
  inactive, so entering or leaving changes only the prompt section, never the
  request tool catalog.
- **Review through a generic provider.** The exit review is presented through the
  generic permission Ask/Allow/Deny flow, which is exactly dsh's allowance for a
  non-Web interaction provider ("another interaction provider presents the same
  request through its generic option flow"). The plan itself is the dialog
  summary (`bounded_summary`'s `exit_plan_mode` branch, §3.5), so the review is
  not a blind Allow/Deny. The generic flow is honored faithfully: a forced review
  offers no persistent option (dsh's generic provider does not invent one either)
  and a malformed plan never reaches the dialog (NEW-1/NEW-3 Rev 5).
- **The log is the only mode authority.** The `session.set_mode` reply is
  advisory: it may carry `pending` for a notice, but the rendered mode is always
  the folded event stream (N6/L4) — the same "logged collaboration state" rule dsh
  applies. A queued change is flushed at turn end (N5), so no accepted selection
  is lost.
- **The projection is memoized, not authoritative.** `plan_mode_active` folds the
  log; a `SessionId`-keyed, process-local memo (invalidated by the controller's
  own commits, never by the session's `Sequence`) keeps the per-step cost O(1)
  (N10/M-1 Rev 5). The memo is a cache of the fold, never a second truth.
- **Recorded dsh divergences (L5/L6).** (a) *Ordering.* dsh assembles a proposed
  step with the selected state and appends `plan/mode` from `agent/pre-step` only
  when the step is accepted; ymh calls `apply_pending_at_step_start` at the top of
  an already-accepted step, immediately after `StepStarted` and before
  `assemble`, so the append and the now-effective state both land before the
  request is built — equivalent for request assembly, different mechanically.
  (b) *`pending` is process-local.* dsh derives it from logged `/plan` command
  runs; ymh keeps commands out of history (25-D5) and holds `pending` in the
  controller's in-memory maps. A daemon reload drops it and the log state stands
  (UX-F3).
- **Commands outside history.** `/plan`, `/plan off`, and their results stay
  outside model history; a non-empty suffix becomes one user message via
  `agent.steer()`.
- **Read-only projections.** TPS and context consumption are read-side projections
  (wall-clock deltas and the `context.show` snapshot), never durable facts.
- **Explicit control.** Plan changes are explicit user/RPC commands, never
  inferred; there is no auto-switch.
- **Config as an explicit layer.** The localcode import is a one-shot, validated,
  user-confirmed write to the required global layer; it never weakens the
  required-global rule and never auto-creates an explicit `--config` target.
- **Strictness preserved.** Unknown keys, bad types, and ambiguous MCP server
  definitions remain hard `ConfigError`s; the import refuses to write anything
  the loader would reject. Beyond shape, the import runs the daemon's own
  `validate_mcp_server`/`validate_mcp_config` before writing (25-D16), so it
  cannot produce a config that parses but fails at `WorkspaceRuntime::create`.

---

## 13. Test plan

Concrete IDs; extend existing files where named. Deterministic tests use the Fake
LLM / in-memory stores; live tests are opt-in (`YMH_LIVE_LLM=1`). Every
persistence-adjacent assertion counts rows **and** leases **and** junctions where
relevant (spec 23's rule).

### 13.1 Existing tests that MUST change (gate M5)

| Test | Why it breaks | Required change |
|---|---|---|
| `tests/unit/ui_render_golden_test.cpp:175` (`kGolden` literal `"idle · test-model · ↑12 ↓3 ⚡0 … 0 active · 0 waiting"`) | Segment 1 is now `build`, counters are always on, and the bar/tps segments are added | Update the literal to `build · test-model · ↑12 ↓3 ⚡0 · [░░░░░░░░░░] —` + aggregate (tps dropped at 72 columns) |
| `tests/integration_two_process_test.cpp:768,807` | They assert `/exit` prompts `"Exiting will terminate"` and require `y`; 25-D10 removes that prompt | Rewrite to assert `/exit` exits immediately (no prompt); keep the Ctrl+D prompt case |
| `tests/integration_live_e2e_test.cpp:284,321` | Same `/exit` prompt assertion | Same rewrite; keep the Ctrl+D path |
| `tests/unit/ui_supervisor_pty_test.cpp:469,585,665,794` (`wait_for("active ·")`) | Safe as long as the aggregate is never dropped (UX5); enumerated so a naive width change does not regress them | No change required; assert the aggregate is still present |
| `tests/unit/ui_supervisor_pty_test.cpp:590,608,670,739,828,848` (Ctrl+D `\x04`) | Ctrl+D keeps prompting (UX28) | No change required |
| `tests/support/fake_transport_host.hpp:19` (`FakeTransportHost final : protocol::TransportHost`) | `TransportHost` gains the pure virtual `setSessionMode` (§3.3, LOW-1 Rev 5), so the fake no longer implements the interface | Add a trivial override returning `{session, active, pending=false}` |

No other test references `"idle"`, the left status segment, or `render_status`
(gate Oracle M5 / critic L7).

### 13.2 Unit tests

| ID | Test | Asserts |
|---|---|---|
| UX-U1 | `plan/mode` event round-trip | `to_json`/`from_json` preserve `active`; `wire_name`/`parse_event_type("plan/mode")` agree; `all_event_types()` includes it |
| UX-U2 | `plan_mode_active` fold | empty log ⇒ false; one true ⇒ true; last false wins; last true wins; ignores other events |
| UX-U3 | `PlanModeController` idle set | no open turn ⇒ `Committed`, appends exactly one `plan/mode`, `active()` reflects it |
| UX-U4 | `PlanModeController` open-turn queue | open turn ⇒ `Queued`, no append; `apply_pending_at_step_start` appends exactly one and returns the new state; opposite queued selection ⇒ `Cancelled`; a selection still queued when the turn ends is committed by `flush_pending_at_turn_end` exactly once (N5) |
| UX-U5 | `exit_plan_mode` fails closed | inactive plan ⇒ error `ToolResult`, no permission request; active plan with `plan_mode == nullptr` ⇒ error, no request |
| UX-U6 | `exit_plan_mode` review + plan preview (H1/N1/N14; NEW-1/NEW-2 Rev 5) | active plan + Allow ⇒ `{"approved":true}` and a pending exit **only on `ToolOutcome::Ok`**; Deny ⇒ denied result, plan stays active; approval commits `plan/mode{false}` at the next step boundary **or** at turn end. **Seam (LOW-3 Rev 5):** drive `PermissionBroker` with the existing `FakeTransport` (`tests/unit/permission_broker_test.cpp:25`) and inspect the emitted `protocol::PermissionRequest::summary` — no direct `bounded_summary` call (it is in an anonymous namespace). Assert the summary equals the `plan` argument; a >4096-byte plan is capped at 4096 bytes with `"..."`; a 20-line plan is capped at 12 lines with `"..."`; a missing/empty/non-string `plan` yields the literal `"exit_plan_mode (no plan provided)"` summary. The rendered dialog (`render_to_ansi` over the `PermissionRequested` model) contains the plan text, not the tool name |
| UX-U7 | Prompt section | active ⇒ system prompt contains the section; inactive ⇒ request is byte-identical to no-plan; the provider is `std::function<std::string(const Session&)>` and is invoked with the assembled `Session&` (no id lookup) |
| UX-U8 | Plan does not restrict | with plan active, every registered tool still passes the policy path unchanged; `SandboxMode::ReadOnly` still denies `tool_is_mutating` tools independently; `tool_is_mutating(exit_plan_mode request)` is false (N3) |
| UX-U9 | `render_status` segment order (wide) | line is `plan · <model> · ↑… ↓… ⚡… · <x.y> tps · [██████░░░░] <pct> · <note> · <notice>` + aggregate |
| UX-U10 | `render_status` narrow degradation | at a narrow width tps is dropped before note/notice, then context, then counters; `mode` and the **ellipsized** `model` (≥4 columns) remain; the fit test uses the ellipsized model width; a boundary width where only 4 columns remain for `model` still includes it (N7) |
| UX-U11 | Bar geometry | `filled` for 0/1/49/80/95/100 %; `window==0` ⇒ 10 empty even when `used>0`; `window>0 && used>0` forces ≥1 |
| UX-U12 | Bar colours | green/yellow/red thresholds; empty `GrayDark`; colour-off still distinguishes glyphs |
| UX-U13 | TPS computation + dirty mark | injected clock: 100 output tokens over 2 s ⇒ `50.0 tps`; `<250 ms` retains the prior value; `output_tokens==0` no update; each update marks Status dirty |
| UX-U14 | `complete_command` highlight sync | after Tab, `command_hint_selected` names the same candidate as `input.draft`; repeated Tab and Shift+Tab stay synced |
| UX-U15 | Enter accepts the highlight | `/sk` + Enter dispatches `/skills`; `/` + Enter dispatches `/` (listing, not `/new`); `/skills --show x` + Enter dispatches the full draft |
| UX-U16 | alias resolution + help | `find("quit")` returns the `exit` command; `dispatch("/quit")` calls the exit handler; `complete("qu")` does not return an alias; `/help` renders `(alias: /quit)` |
| UX-U17 | `/skill` removed | `find("skill")` is null; `/skills` description is `list available skills`; `request_skill` is gone |
| UX-U18 | `/exit` no-dialog | with a non-empty orphaning set, `begin_exit(false)` calls `confirm_exit` and never `open_exit_prompt` |
| UX-U19 | Ctrl+D dialog | `begin_exit(true)` with a non-empty set and no `--yes` calls `open_exit_prompt` |
| UX-U20 | `mcp_servers` parse + id dedupe | object map; `type`/`transport` alias; `http`/`sse` ⇒ `http_sse`; `env`/`headers` object flattening; full key set; name normalization incl. hyphen/uppercase; **two 32-char keys that normalize identically get distinct valid ids** (the second is `base[0:30] + "_2"`), and `kMaxMcpDedupeAttempts` exhausted ⇒ `ConfigError` (M2/N9). **Also: an import fixture with more than `max_servers` (8) valid stdio servers must not throw at create** — every copied server is `required=false`, so the overflow degrades to `Failed`, never a brick (N2 residual Rev 5) |
| UX-U21 | `mcp_servers` strictness | unknown key, bad `env` value type, invalid name char, `type`/`transport` conflict ⇒ `ConfigError` |
| UX-U22 | migration | old array alone loads; new object alone loads; both in one layer ⇒ `ConfigError`; workspace layer replaces global wholesale; both produce the expected `Config::mcp.servers` |
| UX-U23 | `${VAR}` escape | `resolve_mcp_env` expands `${VAR}`; `$${VAR}` yields literal `${VAR}`; missing var still throws |
| UX-U24 | import mapping | a localcode fixture maps `mcp_servers` (normalized), compaction, concurrency; `skip_permissions` is not mapped; provider `api_key` and the `permission` list are ignored; unmapped keys ignored |
| UX-U25 | import validation + mode | a document the loader rejects is not written; the default scaffold result is returned; the temp is unlinked; the written file mode is `0600`; the temp is created `0600` |
| UX-U26 | import prompt | empty = yes, `n` = no, EOF = no, unknown re-prompts once, non-interactive = skip with no output |
| UX-U27 | import secrets | no env/header value appears in captured logs/`out`/`err`; the generated body is not logged |
| UX-U28 | import malformed/oversized | invalid JSON, unreadable file, or >4 MiB ⇒ one stderr line, no prompt, scaffold runs |
| UX-U29 | import semantic validation (N2/N8) | `validate_mcp_server` is called per mapped server; an `http`/`sse` server with `allow_network_servers=false`, a stdio server with an unset `${VAR}`, and a server whose `max_result_bytes` exceeds the daemon cap are each omitted with exactly one stderr note; a localcode `"required": true` is forced to `false` with one note and never written; the remaining config is written; a whole-document `validate_mcp_config` failure aborts to the default scaffold; `apply_jsonc_file(candidate, tmp, /*required=*/true)` (not `load_config`) is the shape check |
| UX-U30 | exit review under ReadOnly + forced Ask (N3/N44; NEW-3 Rev 5) | with `SandboxMode::ReadOnly` and plan active, an `exit_plan_mode` call reaches the resolver (dialog) and is not pre-rule denied; `tool_is_mutating` is false for it; with `default_verdict == Allow`, a broad `"*"` Allow rule, and a remembered `AllowAlways` grant, `RulePermissionPolicy::evaluate(force_ask=true)` still returns `Ask` while the same request with `force_ask=false` returns `Allow` (proving the field is the switch). **Dialog option set:** a `force_ask` `PermissionRequested` renders only `Allow once`/`Deny` with `selected == 1` (Deny), and `handle_dialog` Return on index 1 yields Deny/Once; `PermissionBroker::onDecision` for a `force_ask` core request does **not** call `policy.remember` (the grant set is unchanged) |
| UX-U31 | turn-exit flush (N5; LOW-4/LOW-5 Rev 5) | a `/plan` (or approved exit) queued on the last step of a turn appends exactly one `plan/mode` when the turn exits; the test drives normal `TurnEnded`, a provider error (`TurnFailed`), a cancellation (`TurnCancelled`), **and a maintenance turn** (`runMaintenanceTurn`, whose `TurnStarted` also installs the guard) and asserts the flush ran in each case (the append may follow the terminal event); a re-fold of the log matches the committed state; a second flush is a no-op; a session whose `append` throws inside the flush does not propagate from the guard destructor (the guard/flush is `noexcept`) |
| UX-U32 | reply is not authoritative (N6/L4) | a `session.set_mode` reply with `pending=true` leaves `StatusModel::plan_active` unchanged; only a `PlanModeChanged` derived from the log flips it |
| UX-U33 | projection memo folds once (N10/M-1 Rev 5) | construct `PlanModeController` with a counting `ProjectionFn`; the first `active()` folds once (counter `== 1`); N consecutive `active()` calls with **no** intervening commit leave the counter at `1` and return the cached value; a committed `plan/mode` updates the memo without folding (counter still `1`) and the next `active()` reflects it; a different `SessionId` folds once for its own entry; `erase` drops the entry so the next `active()` folds once more. A log whose last event is a non-`plan/mode` event (a `StepStarted`) does not re-fold |
| UX-U34 | malformed plan makes no review (NEW-1 Rev 5) | an active-plan `exit_plan_mode` call with `plan` absent, `""`, `"   "`, or a non-string appends exactly one error `ToolResult` and makes **no** permission request (the fake transport sees nothing); `request_exit` is not called, so the log gains no `plan/mode{false}` and plan mode stays active |
| UX-U35 | `force_ask` reaches the dialog (NEW-3 Rev 5) | `make_wire` round-trips `force_ask` through `to_json`/`from_json` (`json.value("force_ask", false)`); `UiEventAdapter::onPermissionRequest` copies it into `PermissionRequested`; `UiModel::apply` sets `dialog.force_ask` and `dialog.selected == 1`; `render_to_ansi` shows `Allow once`/`Deny` and not `Always allow`; a legacy wire object without the field yields `force_ask == false` and the four-option dialog |

### 13.3 Golden TUI render tests

Extend `tests/unit/ui_render_golden_test.cpp` (`ConversationSnapshot` at
`:178-184`, `StatusShowsTokenUsage` at `:331-338`).

| ID | Test | Asserts |
|---|---|---|
| UX-G1 | `ConversationSnapshot` updated | the 72-wide golden line is `build · test-model · ↑12 ↓3 ⚡0 · [░░░░░░░░░░] —` + right widget (tps dropped by width) |
| UX-G2 | wide status golden | a new ≥110-wide case shows mode, state (when thinking), model, counters, tps, context, note, notice, and the exact percent |
| UX-G3 | plan-mode golden | mode renders `plan` in `Color::Yellow`; inactive renders `build` |
| UX-G4 | palette golden | after Tab, the `>` row and the draft name are the same command |
| UX-G5 | exit dialog golden | unchanged renderer; `/exit` path produces no dialog frame |
| UX-G6 | colour-off status | the bar is still readable as `█`/`░` with `Theme{false}` |
| UX-G7 | plan-review dialog golden (N1/N14; NEW-2 Rev 5) | a `PermissionRequested` whose `summary` is a plan renders `tool: exit_plan_mode` plus the plan paragraph; a >4096-byte plan renders exactly 4096 bytes plus the `"..."` marker; a 20-line plan renders exactly 12 lines plus the marker |
| UX-G8 | long-plan dialog keeps its options (NEW-2 Rev 5) | a `force_ask` `PermissionRequested` whose `summary` is a 200-line plan renders the plan inside the height-bounded frame and still emits `Allow once`, `Deny`, and the `↑/↓ select · Enter confirm` hint; `Always allow` is absent |

### 13.4 Integration / PTY tests

| ID | Test | Asserts |
|---|---|---|
| UX-I1 | `/sk` + Enter under PTY | `/skills` executes (listing), not `unknown command` |
| UX-I2 | Tab then Enter under PTY | the highlighted command executes; no drift |
| UX-I3 | `/plan` then `exit_plan_mode` approved | the review dialog appears **and its body shows the plan text** (not the tool name); approval logs `plan/mode{false}` at the step boundary; the transcript shows the exit (H1/N1/N14) |
| UX-I4 | `/plan` then a write request | the write is **not** blocked by plan mode (guidance only); with `SandboxMode::ReadOnly` it is blocked by the existing policy |
| UX-I5 | `/plan off` | plan guidance disappears from the next request; a new `plan/mode{false}` event is logged |
| UX-I6 | `/plan <message>` | one user message is steered after activation; no `/plan` event leaks into model history |
| UX-I7 | resume a plan-active session | the plan state is restored by folding the log; the status line shows `plan` |
| UX-I8 | `/exit` with an orphaning daemon | no dialog; exit code 0; daemon torn down |
| UX-I9 | Ctrl+D with an orphaning daemon | the dialog appears; `n` cancels; `y` exits |
| UX-I10 | `/quit` | behaves exactly like `/exit` |
| UX-I11 | `/skills` and `/skill` | `/skills` lists; `/skill` is unknown |
| UX-I12 | first-run import accepted | with an isolated `XDG_CONFIG_HOME` and a localcode fixture, the prompt appears before the UI, the generated config loads, and the daemon starts; the file is `0600` |
| UX-I13 | first-run import declined | `n` ⇒ the default scaffold is written |
| UX-I14 | existing config | with a global config present, no prompt appears |
| UX-I15 | non-interactive first run | no prompt; the default scaffold is written |
| UX-I16 | malformed localcode file | no prompt; one stderr line; the default scaffold is written |
| UX-I17 | old `[mcp].server` config | still loads and starts the same servers |
| UX-I18 | import with an unusable server (N2; N2 residual Rev 5) | a localcode fixture containing an `http`/`sse` server (with default `allow_network_servers=false`), a stdio server with an unset `${VAR}`, and a `"required": true` server: the import writes a config, skips the first two with one stderr note each, forces the third non-required with one note, and the daemon starts successfully. A second fixture with more than `max_servers` valid stdio servers still starts (all non-required, overflow degrades to `Failed`) |

### 13.5 Live-terminal verification (opt-in, `YMH_LIVE_LLM=1`)

1. Launch `ymh` in a real terminal; confirm the status line shows
   `build · <model> · ↑… ↓… ⚡… · <tps> · [bar] <pct>` and that TPS populates
   after the first DeepSeek response and the bar moves after tool results.
2. `/plan`, then ask the model to produce a plan; confirm the `exit_plan_mode`
   review appears **and the dialog shows the plan text**, approve it, confirm plan
   guidance disappears from the next request, and confirm the status line returns
   to `build`.
3. Confirm `/plan` does **not** block any tool; then enable
   `SandboxMode::ReadOnly` and confirm a write is still denied by the existing
   policy, proving the two mechanisms are independent.
4. Type `/sk`, confirm the highlight matches the autofilled text, press Enter,
   confirm `/skills` runs.
5. `/exit`; confirm the app exits immediately with no dialog even with another
   workspace's daemon attached; then Ctrl+D and confirm the dialog appears.
6. Remove `~/.config/ymh` in a scratch `HOME`, place a localcode config with a
   hyphenated server name and an `${…}` literal, start `ymh`, confirm the import
   prompt, accept, and confirm the MCP servers from localcode start; confirm the
   generated file is `0600` and no secret appears in the logs.
7. In plan mode, ask for a long multi-line plan; confirm the review dialog shows
   the plan preview and that the `Allow once`/`Deny` rows and the key hint remain
   visible (the preview scrolls/truncates, never the options). Confirm no
   `Always allow` row is offered for the review.

---

## 14. Open questions / interpretations

These are the interpretations made on the user's behalf. Each is a candidate
correction.

| ID | Interpretation | Where pinned |
|---|---|---|
| Q1 | The status word for the inactive state is ymh's display label `build` (the user's requirement 1 wording); there is no `AgentMode::Build`, no `/build`, and no build config value. | 25-D1/D5, UX23 |
| Q2 | Plan mode is painted `Color::Yellow`; no `Theme` field is added. | 25-D1 |
| Q3 | `exit_plan_mode`'s review is an ordinary permission Ask; the dialog summary is the `plan` argument truncated to `kMaxPlanSummary = 4096` bytes **and** `kMaxPlanSummaryLines = 12` lines with the ASCII `"..."` marker, rendered in a height-bounded frame so the option rows stay visible, produced by a dedicated `bounded_summary` branch — **not** a `PermissionRequest.summary` field (there is none). A malformed/absent plan makes no review at all. ymh has no free-form review text field, so a Deny carries only the decision reason as feedback. | 25-D4, UX18/UX47 |
| Q4 | `usage.output_tokens` is **per-response**; if a provider reports cumulative usage, TPS is inflated (cosmetic only). | 25-D6, UX7 |
| Q5 | Deleting `/skill` removes explicit skill activation entirely; only the **model-invoked `skill` tool** and `/skills --show` remain. With the default `skills.expose_workspace=false`, workspace-authored skills become unreachable — recorded as a known functional regression, not a security change. | 25-D11, UX30/UX31 |
| Q6 | `/skills`' description is exactly the user's `"list available skills"`; the `--show` affordance still works but is no longer advertised in the description. | 25-D11, UX30 |
| Q7 | Aliases participate in **dispatch and `/help` only**, not Tab completion (keeps `complete()` unambiguous). | 25-D12, UX32 |
| Q8 | The old `[mcp].server` array is **kept for backward compatibility** (mutually exclusive per layer) rather than hard-errored. | 25-D13, UX37 |
| Q9 | On import failure the importer **falls back to the default config** rather than exiting non-zero. | 25-D14, UX41 |
| Q10 | Imported `mcp_servers[*].env`/`headers` literal values (secrets) are copied into the user's `0600` config; `providers[*].api_key` is **not** copied (no ymh field); `skip_permissions` is **not** imported. | 25-D15, UX41 |
| Q11 | `/exit` skips the dialog **even when daemons would be orphaned** (spec-16 teardown still runs); Ctrl+D is the only prompting path. | 25-D10, UX27–UX29 |
| Q12 | The status line may **drop TPS first** at narrow widths, keeping counters, the context bar, note, and notice. | 25-D1, UX5 |
| Q13 | `session.set_mode` is a **new additive RPC method** that logs `plan/mode`; the supervisor derives the displayed state **only** from the folded event stream and never from the reply (the reply has no `Sequence`, so "newer" is undecidable). The reply's `pending` flag may show a transient notice. | 25-D2/D5, UX11/UX22, N6 |
| Q14 | Server-name normalization is deterministic and silent (no rename notice); collisions are disambiguated in ascending key order by the terminating suffix algorithm (base truncated first, bounded by `kMaxMcpDedupeAttempts`). | 25-D13, UX34, M2 |
| Q15 | Entering/leaving plan mode changes the system message and therefore invalidates the provider's prefix/KV cache for the next request; this is accepted because it is an explicit user action, not per-step churn. | 25-D3, N11 |
| Q16 | `headers` are parsed into `header_env` for forward compatibility but are not consumed by any shipped transport (only stdio exists); this is recorded, not a live feature. | 25-D13, N13 |
| Q17 | The importer **omits** a server that fails the daemon's own validation and prints one note per omission, rather than importing it and bricking the daemon; a whole-document validation failure still falls back to the default config (Q9). | 25-D16, UX43, N2 |
| Q18 | The importer forces `required=false` on every copied server (with one note when localcode had `required=true`), because it can prove a server validates but never that it starts; a required server that fails to start would brick every future start. | 25-D16, UX41/UX43, N2 |
| Q19 | The turn-exit flush runs from a scope guard, so a queued `plan/mode` may be appended **after** the terminal event; this is accepted because the projection is a last-wins fold and the flush is idempotent. | 25-D4/D5, UX19/UX45, N5 |
| Q20 | A forced review offers no persistent decision: the dialog shows only `Allow once`/`Deny` (default `Deny`), and a `force_ask` decision is never recorded as a grant. The user is not offered an "Always allow" that the policy would ignore; a raw client sending `scope=Always` still gets the decision for that one review. | 25-D4, UX44/UX46, NEW-3 Rev 5 |
| Q21 | The plan projection memo is keyed by `SessionId` and invalidated by the controller's own commits (the controller is the sole `plan/mode` writer), **not** by the session's last `Sequence`; the first `active()` per session folds, and a step that appends no `plan/mode` never folds. The memo is process-local and never read as truth. | 25-D2, UX12, N10/M-1 Rev 5 |

---

## 15. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 1 | 2026-09-18 | Initial authoring: 15 decisions (25-D1…25-D15), invariants UX1–UX34, failure modes UX-F1–UX-F16, pinned interfaces, test plan, interpretations. Status **draft** — not yet independently verified. |
| Rev 2 | 2026-09-18 | **dsh plan-mode pivot + two-gate fixes.** (1) Replaced the invented agent-mode/read-only-seam design with dsh's plan mode: new durable event `plan/mode` (`payload::PlanMode{active}`, 25-A6), the `plan_mode_active` projection, a daemon `PlanModeController`, the `plan:policy` prompt section (config `agent.plan.section`), the always-registered `exit_plan_mode` tool reviewed through the generic permission Ask flow, and `/plan` / `/plan off` (with `/plan <message>` steered as one user message). Withdrew 25-W1…25-W6 (old D3/D4/D2/D15/D12-read_only/`session.set_mode` setter); plan mode no longer restricts tools and no longer couples to sandbox/policy — the existing `SandboxMode::ReadOnly` + `tool_is_mutating` + `RulePermissionPolicy::evaluate` path is referenced instead. This dissolves both gates' H1/H2 (fail-open MCP `mutating`) and the `read_only`/`readOnlyHint` findings. (2) Fixed surviving findings: restored `status.note` + `model.notices` as trailing status segments (Oracle H2 / critic H3); pinned the import temp file `0600` at creation + `rename` and the global-directory creation (critic H4/M5); pinned the `${VAR}`-only syntax plus a `$${` escape (critic M3); full `mcp_servers` key set and `http`/`sse` mapping (Oracle M3 / critic M2); hyphenated-name normalization (critic M1); malformed/oversized localcode handling (critic M4); `skip_permissions` no longer imported (critic M13); bar-geometry precedence `window==0` first (critic M14); bare `/` + Enter keeps the listing (critic M7); `/help` alias rendering pinned (critic M8); `/plan` command wiring pinned (critic M9); plan colour pinned (critic M10); TPS/context dirty marking pinned (critic M11); long-model ellipsis/overflow policy (critic M6); existing-test enumeration incl. the golden literal and the `/exit`-prompt integration tests (Oracle M5 / critic L6/L7); stale `file:line` cites corrected (Oracle L3 / critic L1). Status **draft** — pending independent re-verification. |
| Rev 3 | 2026-09-18 | **Re-gate fixes.** (1) **H1/N1 — the exit review now shows the plan.** Removed the non-existent `PermissionRequest.summary` field from the design; pinned a `bounded_summary` `exit_plan_mode` branch in `src/permission/permission_broker.cpp:50-80` (new `kMaxPlanSummary = 4096`, ASCII `"..."` marker, reads `arguments["plan"]` from the core request so it is independent of `arguments_max_bytes`), and traced the full daemon→wire→supervisor→dialog path (`protocol_server.cpp:893-908` → `supervisor.cpp:751` → `ui_event_adapter.cpp:297-310` → `ui_model.cpp:741-750` → `ui_render.cpp:391-399`). Added UX-U6/UX-G7/UX-I3 dialog-content assertions (N14). (2) **M1/N4 — controller ownership/injection pinned.** `PlanModeController` now lives in `WorkspaceRuntime::Impl` (not `HostRuntime`), matching the real construction order (`workspace_host.cpp:503` before `:543`); the loop reaches it via a new `AgentServices::plan_mode` field; the assembler provider is `std::function<std::string(const Session&)>` (no id lookup); the `AppendFn` uses `SessionManager::sessionPtr`; `HostRuntime` reaches it via `WorkspaceRuntime::plan_mode()` and calls `erase` in close/delete. (3) **M2/N9 — dedupe terminates.** §8.2 rule 1 now truncates the base **before** appending `_2…`, bounded by `kMaxMcpDedupeAttempts = 10000`, with a loud `ConfigError` on exhaustion, so 32-char ids can never collide. (4) **N2/N8 (critic-only HIGH/MEDIUM, oracle missed) — the import can no longer brick the daemon.** New 25-D16: per-server `validate_mcp_server` + whole-document `validate_mcp_config` (extracted from `McpManager::validate()`, `src/mcp/mcp_manager.cpp:70-103`) run **before** any write; a failing server is omitted with one stderr note; §9.3 step 4 names `apply_jsonc_file(candidate, tmp, /*required=*/true)` explicitly (not `load_config`). (5) **N3 — ReadOnly-safe review.** `exit_plan_mode` is `destructive=false` and the loop forces `PolicyVerdict::Ask`, so `SandboxMode::ReadOnly` does not pre-rule deny the review (UX42). (6) **N5/N6/L4 — no phantom mode.** The `session.set_mode` reply never writes `plan_active` (the log is the only writer); a `flush_pending_at_turn_end` call guarantees a queued selection/exit is committed before the terminal event. (7) **N7 — width algorithm made literal:** `model` is reserved with `mode` and is not in the optional priority list; the fit test uses its ellipsized width. (8) **LOWs:** L1 `render_status` documented as file-local and test-driven via `render_to_ansi`; L2 `src/mcp/mcp_transport.cpp` path corrected; L3 `agentStatus(...) != "Idle"` literal; L5/N10 memo note; N11 KV-cache note; N12 lock released before `append_`; N13 `header_env` unconsumed note; N14 test-plan additions. Status **draft** — pending re-gate. |
| Rev 4 | 2026-09-18 | **Re-gate round 2 — the critic's residual findings, re-verified against the tree.** (1) **N2 (HIGH) — the import can still brick via `required`.** Rev 3 validated MCP servers but asserted "the import never sets `required`", which is false: the importer copies the whole localcode object through the 25-D13 key set, so a localcode `"required": true` server is written, and a required server that fails to start rethrows from `McpManager::start` (`src/mcp/mcp_manager.cpp:228-230,244-246`) during `WorkspaceRuntime::Impl` construction (`src/agent/workspace_runtime.cpp:129-135`) — bricking every start. §9.3 now forces `required=false` on every copied server (with one note), pins the validation predicate (`validate_mcp_server`/`validate_mcp_config`, extracted from `McpManager::validate()` at `src/mcp/mcp_manager.cpp:70-103`; §8.3), the omission-reason precedence (non-`stdio` first), the `ToolConfig{}` equivalence to the daemon's default-constructed `tool_config_` (`src/agent/workspace_runtime.cpp:93`), the "only MCP can throw at create" sufficiency argument, and the no-previous-config/no-rename-unless-valid guarantee. UX41/UX43, UX-F15/UX-F19, UX-U29, UX-I18, Q18 updated. (2) **N3 (MEDIUM) — the forced review had no hook.** Rev 3 said "the loop forces `PolicyVerdict::Ask`", but `PermissionGate::resolve` evaluates the policy itself (`src/policy/permission_policy.cpp:318`) and the loop only calls `gate->resolve`/`policy->evaluate` — there is no way to force Ask, so `default_verdict == Allow` or a broad rule would auto-approve the review. Rev 4 adds the non-mutating `PermissionRequest::force_ask` (additive, `include/ymh/policy/permission_policy.hpp:54-70`) and a `RulePermissionPolicy::evaluate` short-circuit returning `Ask` after the ReadOnly deny and before rules/`tool_defaults`/`default_verdict`/grants (`src/policy/permission_policy.cpp:204`); `tool_is_mutating` never reads it, so ReadOnly still reaches the review. The inactive-plan case now short-circuits **before** the permission block so "no permission request" is true. §3.5/§3.9, 25-A7/25-A11, UX18/UX42/UX44, UX-U30 updated. (3) **N5 (MEDIUM) — the flush missed error/cancel exits.** Rev 3 pinned `flush_pending_at_turn_end` "after the step loop", but `runTurn` returns directly from ~11 terminal sites (normal end `:796-804`, cancellation `:783-787`/`:819-823`, failure `:788-794`, step limit `:824-827`, context-assembly `:642-653,666-669,736-739`, no provider `:697-700`, lease/store `:748-756`), so those paths would silently lose a queued selection. Rev 4 installs the flush as a scope guard after `TurnStarted` (`:612`) so it runs on every exit (idempotent, null-safe; the append may follow the terminal event), and pins the interim display (`"plan change queued"`/`"plan exit queued"` notice, never an optimistic mode flip) and the reconciliation on end/error/cancel. §3.3/§3.6, UX19/UX45, UX-F2/UX-F20, UX-U31, Q19 updated. (4) **N4 residual — `closeSession` must not erase.** Rev 3 claimed `erase` runs from `closeSession`, but `HostRuntime::closeSession` (`src/host/host_runtime.cpp:665-669`) is an explicit detach-only no-op (24-D10); Rev 4 moves the `erase` call to `deleteSession` (`:681-737`) only. (5) **N10 (LOW) — memo made mandatory.** The controller must keep a `(SessionId, last Sequence)`-keyed memo (UX12). (6) **N14 — tests added/refined** for the `force_ask` bypass, the error/cancel flush, and the `required` strip. Status **draft** — pending re-gate. |
| Rev 5 | 2026-09-18 | **Re-gate round 3 — the remaining MEDIUMs and the cheap LOWs.** Intended to be verification-complete (0 HIGH, 0 MEDIUM open); **NOT** declared verified — the gate does that. (1) **Oracle M-1 — the N10 memo was keyed wrong.** The Rev-4 memo keyed on the session's last `Sequence`, which advances on every appended event, so `active()` re-folded the whole log every step. §3.3 now keys the memo by `SessionId` alone with a `valid` bit, written by the first fold and by every controller commit (invalidate-before-append, set-after), and dropped by `erase`; the controller is the sole `plan/mode` writer, so a valid entry cannot go stale and a step that appends no `plan/mode` never folds. Adds a `ProjectionFn` test seam. UX12 amended; UX-U33, Q21 added. (2) **Critic NEW-1 — blind review / exit on a malformed plan.** §3.5 adds a pre-Ask argument gate (missing/empty/whitespace/non-string `plan` ⇒ error `ToolResult`, no permission request) and gates `request_exit` on `ToolOutcome::Ok`; `bounded_summary` emits `"exit_plan_mode (no plan provided)"` if ever reached. UX18/UX19/UX47, UX-F21, UX-U6/UX-U34 updated/added. (3) **Critic NEW-2 — an unbounded multi-line preview pushed the option rows off-screen.** §3.5 adds `kMaxPlanSummaryLines = 12` alongside the 4096-byte cap and a height-bounded frame (`kMaxPlanPreviewRows = 12`) in `render_dialog`; the forced-review default selection is `Deny`. UX-G7 updated; UX-G8 added. (4) **Critic NEW-3 — `AllowAlways` was a dead no-op on a forced review.** The forced dialog now renders/accepts only `{Allow once, Deny}`; `PermissionBroker::onDecision` and the loop's direct-policy path skip `remember` for a `force_ask` request. Adds one additive wire/event field `force_ask` (JSON `value("force_ask", false)`), `PermissionDialogModel::force_ask`, and the two-option render/handle branch. UX44/UX46, UX-F22, UX-U30/UX-U35 updated/added. (5) **Oracle LOWs:** L-1 `Cancelled`-vs-`Unchanged` precedence pinned; L-2 `set()` clears `pending_exit_`; L-3 `"plan exit queued"` source pinned to the forwarded Allow at `resolve_dialog`; L-4 sufficiency now covers the non-throwing `base_url` ⇒ `StartupRejected` path; L-5 `normalize_mcp_server_id`/`kMaxMcpDedupeAttempts` moved to `mcp_types.hpp`; L-6 `workspace_runtime.cpp:189→190`; L-7 folded into NEW-1. (6) **Critic LOWs:** LOW-1 `HostRuntime::setSessionMode`/`SetModeResult` pinned; LOW-2 `to_mcp_config` named in the import validation step; LOW-3 UX-U6 now drives `PermissionBroker` + `FakeTransport`; LOW-4 guard/flush `noexcept`; LOW-5 guard also installed in `runMaintenanceTurn`; LOW-6 §4.3 pseudocode now shows `return true`. **Consciously left:** none — every LOW was cheap and real. Status **draft** — intended verification-complete (0 HIGH, 0 MEDIUM); pending re-gate. |

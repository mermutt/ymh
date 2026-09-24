# 55 — Multi-Agent Delegation (errata)

Status: **draft (Rev 6)** — NOT verified. No implementation may begin until this
spec is independently gated (AGENTS.md "The rule"). It is an **errata**: it
amends `06` §7 and §4, `42` §3.3–§3.5, `44` §3.3 and §5.5 (jobs), `54` §3–§5,
`01`/`37` (provenance, the subagent `SubagentFanIn` event, and the persisted
`finish_reason`), `02` (`SessionOptions`/header), `28` (`LlmRuntime` route
preflight), and `24` §1.3.3;
it **builds on**
`43` §1–§2 (the Wave-5 creation-path/header contracts) rather than amending it.
The three lists — this header, §1.3, and §8 — are kept in exact correspondence
(see §1.3).

Revision: **Rev 6** — applies a late seventh-reviewer re-check whose report
arrived after Rev 5 (a substantive MEDIUM predicate gap plus cleanups; see §12
for the finding-by-finding disposition). Rev 5 → **0 HIGH / 1 MEDIUM / 4 LOW**
→ Rev 6. The MEDIUM: `notice_expected` was defined as "true iff the epoch was
background-registered", where "background" was itself defined as "a background
start (one-shot background, or the first `continuable` start)" — but D4/I32/U18/
U29 require a `send_message` that opens a new epoch to produce a new notice. A
`send_message`-opened epoch is not a "background start", so its fan-in would be
`notice_expected == false` and the replay rule would silently drop that notice —
the same class as the Rev 4 foreground-spam bug, from the opposite direction
(too narrow). Fixed by defining the predicate as **"the epoch was not opened by
a foreground-blocking activation"**: every non-foreground epoch — a background
start **or** a `send_message`-opened epoch — is background-registered
(`notice_expected == true`) and gets the job; only a `continuable` foreground
call is `false`. Reconciled across D4 (opener, step 1/2/4, the foreground note),
D13, §4, §8 `A11`, I7/I31/I32, F19, and U16/U18/U28/U29 (55-R6-M1). LOW: §4's
`notice_plugin`/`notice_text` snippet now shows the members **inside**
`struct JobStart`/`JobSnapshot`/`JobOutcome` (it previously floated bare fields
between comment blocks, so the pinned sketch did not compile); D4's replay rule
now reads "exactly one notice per unreported background fan-in per replay pass
(idempotent under repeat)" to match the at-least-once guarantee; §12 gains a
finding-id legend mapping `55-Hn` to the Rev 2 gate's `H1`–`H13` (and `55-Rk-*`
to later rounds); and F1/F4/F6 test-row citations (now bidirectional with `U2`/`U6`)
plus the §11 spec-`28` reference are confirmed present (55-R6-L1..L4).

Revision: **Rev 5** — applies the sixth-reviewer re-check (three reviewers whose
findings converged; see §12 for the finding-by-finding disposition). Rev 4 →
**2 HIGH / 3 MEDIUM / 4 LOW** → Rev 5. The two HIGH fixes: (1) the
settlement-correlation **ordinal** is pinned as `existing_count + 1` — the
1-based position this epoch's settlement fan-in will occupy — at every site that
defines it, so the fast-path `notice_plugin` marker and the ordinal replay
derives from the log always agree (55-R5-H1); and (2) the dsh notice-**text**
override gains a pinned seam — `JobSnapshot::notice_text`, supplied at settlement
through `JobOutcome::notice_text` (55-A12) — so `JobWakeupPolicy` can render dsh's
text for `kind == "subagent"` instead of the file-local generic `notice_text`
(55-R5-H2). MEDIUM: the additive persisted `finish_reason` is registered as
**55-A13** with its codec omit-when-default rule (§1.3/§8/header); `A12` pins
that `JobRegistry::start` copies `notice_plugin`/`notice_text` into the snapshot
(55-R5-M3); §1.4 now discloses the Rev 4 durable additions (`A10`/`A11`/`A12`).
LOW: spec `28` added to §11; F1/F4/F6 test-row citations pinned; the
`events.hpp` anchor corrected to `224-229`; I29 and §1.4 name the two new codec
fields.

Revision: **Rev 4** — applies the fifth-reviewer re-check (two HIGH/MEDIUM
defects in the settlement mechanism plus LOWs; see §12 for the finding-by-finding
disposition). Rev 3 → **1 HIGH / 1 MEDIUM / 8 LOW** → Rev 4. The two substantive
fixes: (1) the replay rule was scoped to **background-registered** epochs only —
`payload::SubagentFanIn` gains a durable `notice_expected` bit (55-A11) and
foreground fan-ins never participate, so a foreground delegation no longer
receives a spurious settlement notice on the next parent resume (55-R4-H1); and
(2) the `subagent-settlement:<child>#<ordinal>` correlation is now carried on the
registered job's `notice_plugin` override (55-A12), so the pinned
`JobWakeupPolicy` fast path can actually stamp it and
`replayUnreportedSettlements` can reconstruct it (55-R4-M1). LOWs: dsh's
"unavailable parent" rejection added (D5/F9); the "never yields zero notices"
over-claim softened to "delivers on the next parent resume / daemon start"
(I7/I31); D4's "exactly one notice" reconciled with at-least-once; the §9.4
`agent_loop.hpp` citation corrected to `:91`. Already-applied Rev 3 LOWs
(`ids.hpp:19`, §2.5 preset citations, D7 single predicate, D6/I16 validation
owner, D8 member declaration) were re-verified in place.

Revision: **Rev 3** — applies the second independent re-check (four reviewers)
plus the mandatory **symbol/access-level sweep** of every pinned sketch. Rev 2 →
**2 HIGH / 8 MEDIUM / 10 LOW** → Rev 3 (see §12 for the finding-by-finding
disposition and the sweep result). The root-cause fix: the pinned route preflight
called the **private** `LlmRuntime::resolve_adapter`, so this revision pins a
public `LlmRuntime::preflight_route` accessor (55-A10) and registers the
`llm_runtime.hpp` amendment in §1.3/§8/§4. Also corrected: `onSettled` fires at
the **terminal event** with a **separate** drain check (D4/D10 reconciled); the
`SessionActivator` and `SubagentRunner` interfaces are pinned in §4; `settle`
disposes a one-shot child but leaves a continuable child **resident**; the durable
route is the `(endpoint, profile_id)` pair everywhere; and F13/F17/I1/I25/I26/
I28/I30/I35 now have test rows. The access-level sweep found **no other**
violation beyond the three known instances (all now fixed).

Revision: **Rev 2** — applies the five-reviewer adversarial gate (scope, mode,
route, interface, completeness). Rev 1 → **13 HIGH / 18 MEDIUM / 14 LOW** → Rev 2
(see §12 for the full finding-by-finding disposition). The largest corrections
are scope: the job subsystem is **entirely unwired** (55-H1), so the settlement
mechanism is **built**, not reused; the settlement guarantee is restated as
**at-least-once over a durable record** with a replay rule (55-H2); and an
activation is a **residency epoch** with drain semantics (55-H3), not a single
terminal event. Rev 1 authors the delegated-subagent capability ymh lacks: the
two dsh delegation modes (`one-shot`, `continuable`), the `run_in_background`
surface, the `send_message`/`interrupt_agent`/`list_agents` control tools, the
per-child route-selection surface (`provider`/`model`/`reasoning_effort` +
`list_subagent_models`), the depth and breadth bounds, the per-child `persona`/
`toolFilter`, the "errors, never partial success" failure contract, the
settlement-notice mechanism, and the mandatory repair of the `SubagentRunner`
settlement race (`src/agent/subagent.cpp:49`).

---

## 1. Purpose, scope, and the user decision

### 1.1 The gap

`docs/design/26-dsh-alignment.md` G25 records ymh's subagent support as
**Partial**: "ymh has the event vocabulary and a `subagent.hpp` seam but no
composition inheritance, no depth, no fixed-scope statement, no continuable
control tools." Two of those are now false in isolation (the Wave-5 preset
system implements composition inheritance, depth, and the fixed-scope
statement — `42`), but the **delegation path itself has never been wired**:

- `SubagentRunner` has **zero production construction sites** (§2.1).
- `apply_child_composition` and `check_delegation_depth` have **zero production
  call sites** (§2.1).
- No model-facing `subagent` tool exists (§2.3).
- `06` §7 explicitly records the async scheduler as a deliberate omission
  (§2.4).

This spec closes the gap and does so **faithfully to dsh** (the user's verbatim
decision: *"I'd go more 'dsh' faithul"*). It mirrors **both** delegation modes
and the full per-child route-selection surface; it does not collapse them to
one mode.

### 1.2 What this changes, in one sentence

ymh gains a `SubagentService` (the ymh analogue of dsh's `SubagentRuntime`), a
family of model-facing delegation tools (`subagent`, `subagent_continuable`,
`send_message`, `interrupt_agent`, `list_agents`, `list_subagent_models`), and
a fixed settlement mechanism — built on the **existing** preset roster,
`AgentRegistry`, `LLMPool`, `TurnExecutor`, and the spec-54 per-request routing
seam. The job subsystem (`JobRegistry`/`JobWakeupPolicy` and the job tools) is
**not** "existing production machinery": it is verified library code with **no
production wiring at all** (`44:87`), so this spec **wires it for the first
time** (§2.3, §2.5, §2.6.6, §3 D4) rather than reusing it verbatim. The
activation seam is also **not** directly injectable today (`HostRuntime` owns
it, §3 D11/D13, 55-H9), so this spec injects a `SessionActivator` seam.

### 1.3 Supersession map

| Amended / superseded | Verified anchor | New behaviour |
|---|---|---|
| `06` §7 "Synchronous v1 (decision (i))" and its deliberate omission (`06:937-939`, `:1141-1142`, `:1342`, `:1374-1375`) | `include/ymh/agent/subagent.hpp:3-6` | Delegation is no longer synchronous-only. `one-shot` keeps the foreground wait; a new `one-shot` **background** mode and a new `continuable` mode are added. The `whenIdle`-based settlement is replaced (55-D10). |
| `42` §3.3 / §3.4 / §3.5 | `include/ymh/agent/preset.hpp:150-156`, `:169-170`, `:232`; `src/agent/preset.cpp:789-833`, `:875-884` | `apply_child_composition` and `check_delegation_depth` acquire their **first production call sites** (the delegation admission path). Their behaviour is unchanged. |
| `44` §3.3 "Subagent producer" | `docs/design/44-goals-jobs-commands.md:377-384` | The producer named there is made real: a delegated activation is registered as a `kind == "subagent"` job owned by the parent. |
| `06` §7 "Caps" (`06:943-944`) | `include/ymh/execution/resource_governor.hpp:20-29` | A **global fan-out cap** (`ResourceCaps::max_live_subagents`) is added; `LLMPool`/`TurnExecutor` remain the downstream backpressure (55-D7). |
| `24` §1.3.3 "Retained: One active session per workspace" | `docs/design/24-agent-lifetime-errata.md:285-292` | Retained for user sessions. **Delegated children are resident sessions that are activated through the same `TurnExecutor`**; the one-active-session rule is not widened for user sessions, and the delegation admission cap (not the activation policy) bounds fan-out. |
| `54` §3–§5 | `docs/design/54-multi-endpoint-routing-errata.md`; `include/ymh/agent/model_selection.hpp:32-39` | A per-child route override is layered over the parent's effective `ModelSelection`; the child's durable route is the source of truth. |
| `28` `LlmRuntime` route preflight | `include/ymh/llm/llm_runtime.hpp:237-238` (public `prepare_call`), `:262` (private `resolve_adapter`) | Adds a **public** `preflight_route` accessor (55-A10) so `SubagentService` can run the D6 step-4 route preflight without a `friend` (55-R3-H1). |
| `01` §3 / `37` provenance; `01` §4 subagent events | `include/ymh/agent/provenance.hpp:150-197`; `include/ymh/session/events.hpp:224-229` | Adds the optional `MessageSource::sender` field and its codec (55-A6; 55-D12) **and** the `SubagentFanIn::notice_expected` bit (55-A11; 55-R4-H1). |
| `01` §4 turn/assistant events | `include/ymh/session/events.hpp:67` (`TurnEnded`); the assistant-message payload | Adds an additive persisted `finish_reason` (55-A13; 55-D9) so the token-limit/refusal stop-reason rows are derivable. |
| `44` §5.5 job types / §3.5 wakeup policy | `include/ymh/jobs/job_registry.hpp:54-76`; `include/ymh/jobs/job_wakeup.hpp:24-36` | Adds `JobStart`/`JobSnapshot::notice_plugin` **and** `JobSnapshot`/`JobOutcome::notice_text` (settlement-supplied) so `JobWakeupPolicy` stamps the `subagent-settlement:<child>#<ordinal>` correlation and renders dsh's text (55-A12; 55-R4-M1; 55-R5-H2/M3). |
| `02` `SessionOptions` / `SessionHeader` | `include/ymh/session/session_manager.hpp:29-43`; `include/ymh/session/session.hpp:44-66` | Adds `endpoint`/`profile_id`/`reasoning_effort`/`max_tokens` to `SessionOptions` **and** persists `(endpoint, profile_id)` on `SessionHeader` so the durable route is rebuildable (55-A7; 55-D6; 55-H4). |
| `06` §4 `Agent` surface | `include/ymh/agent/agent.hpp:134-155` | Adds `Agent::onSettled` (55-A8; 55-D10). |
| `43` §1–§2 (Wave-5 dependency errata) — **build-on, not amended** | `docs/design/43-wave5-dependency-errata.md` | Consumed, not changed: this spec is a downstream consumer of `43`'s creation-path copy and header codec. Listed here so the header, §1.3, and §8 agree (55-H13). |

**Correspondence.** This table, the header, and §8's amendment register name
the same amended specs. `06` appears twice (its §7 synchronous-v1 decision and
its §4 `Agent` surface) and `01`/`37` appears twice (provenance/subagent events
and the turn/assistant `finish_reason`); every §8 `55-A*` row has a row here.
`43` is the only
row here without a `55-A*` counterpart — it is a **build-on**, not an amendment
(§8), so §1.3 lists it and §8 does not.

### 1.4 What this spec does and does **not** change

**It does change** (all additive; see §3 D12 and §4): `MessageSource::sender`
(55-A6; a `37` codec addition, omitted when empty so existing records are
byte-identical), `SessionOptions`/`SessionHeader` route fields — including the
persisted `(endpoint, profile_id)` pair (55-A7), `Agent::onSettled` (55-A8), the
public `LlmRuntime::preflight_route` accessor (55-A10), the durable
`SubagentFanIn::notice_expected` bit (55-A11; default `false`, decodes absent as
`false`), the `JobStart`/`JobSnapshot::notice_plugin` and
`JobSnapshot`/`JobOutcome::notice_text` overrides (55-A12), and the additive
persisted `finish_reason` (55-A13; omitted when its default, decodes absent as
the default). It also adds new types and **class members** (`SubagentService`,
`AgentRegistry::createChild`/`AgentRegistry::liveSubagentCount` as members,
`ResourceCaps::max_live_subagents`, …) whose symbols must be registered in
`tests/fixtures/spec_symbol_catalog.json` (§9.5).

**It does not change:**

- It adds **no new `EventType`** and **no new wire notification**. The durable
  carriers are the existing `SubagentSpawned`/`SubagentFanIn` events and the
  existing job-wakeup message; the live UI sees them through the existing event
  stream (35). (The additive `MessageSource::sender`,
  `SubagentFanIn::notice_expected`, `JobStart`/`JobSnapshot::notice_plugin`,
  `JobSnapshot`/`JobOutcome::notice_text`, and `finish_reason` fields above are
  codec extensions, not new events or notifications.)
- It does **not** widen the permission model. The fixed-scope statement
  (`kDelegationScopeStatement`) is already registered by
  `apply_child_composition`; the approval-`never` target remains OQ-6 of `42`.
- It does **not** add remote/out-of-process providers. The only provider is the
  in-process `spawn` provider (dsh's default backend).
- It does **not** change `presets.max_depth` as the single depth source
  (`42-D18`).
- It does **not** implement the `fork`-seeded provider (dsh's `fork` backend).
  It is recorded as a follow-on (55-OQ-4).

---

## 2. Current state (verified against the shipped tree)

Every claim in this section was checked with `grep -n`/`sed` against the working
tree at the time of writing.

### 2.1 The delegation machinery exists but is unwired

- `SubagentRunner` is declared in `include/ymh/agent/subagent.hpp:17-31` and
  defined in `src/agent/subagent.cpp:9-80`. **Zero** construction sites exist in
  `src/`; the only constructions are in tests
  (`tests/unit/agent_registry_test.cpp:299`, `:327`, `:365`).
- `AgentPresetRoster::apply_child_composition` is defined at
  `src/agent/preset.cpp:789-833`. Its only callers are tests
  (`tests/unit/agent_preset_test.cpp:702`, `:738`, `:775`, `:792`;
  `tests/unit/spec52_presets_test.cpp:396`). **No production caller.**
- `check_delegation_depth` is defined at `src/agent/preset.cpp:875-884`
  (declared `include/ymh/agent/preset.hpp:169-170`). Its only callers are tests
  (`tests/unit/agent_preset_test.cpp:802-809`). **No production caller.**
- `AgentErrorCode::DelegationDepthExceeded` exists
  (`include/ymh/agent/agent.hpp:70-71`), so the refusal code is already pinned.

**Consequence.** The Wave-5 composition/depth machinery is a tested library
with no live consumer. The delegation path is the missing consumer, and this
spec is its design.

### 2.2 The settlement race in `SubagentRunner`

`src/agent/subagent.cpp:22-68` creates the child through
`registry_.create(options_)` (`:22`) and then reads its log:

```cpp
child->send(std::move(message));   // :48
child->whenIdle([]() {});          // :49  <-- the defect
...
std::shared_ptr<Session> childSession = sessions_.sessionPtr(childSessionId);
const EventRange         events       = childSession->events();   // :53
```

`Agent::whenIdle` takes a callback and returns `void`
(`include/ymh/agent/agent.hpp:153`; impl `src/agent/agent_loop.cpp:341-360`).
It **does not block**: if the agent is already idle it runs the callback
immediately; otherwise it queues it for `flushIdleCallbacks()`. Passing an
empty callback `[](){}` therefore neither waits nor signals, and the very next
statement scans the child's event list. When the child's turn is not already
complete (a concurrent activation, a background-driven child, or any future
non-inline start), the scan observes an **incomplete** list and can mis-report
`Completed` on a still-running child (the reverse scan finds no terminal event
and falls through to the default `Completed`, `src/agent/subagent.cpp:51-68`).

**This is a correctness defect that must be fixed before anything depends on the
runner** (55-D10). No current production code depends on it, so the fix is
safe to land as part of this work.

### 2.3 No model-facing `subagent` tool, and the job subsystem is unwired

**No Tool named `subagent` is constructed or registered.** The stronger claim
that *every* `"subagent"` occurrence in `src/` is a session kind or event payload
is **false** and is retracted: there are three further reservations that must not
be mistaken for a tool —

- `src/prompt/order.cpp:69-70` reserves the prompt-ordering slots
  `TOOL_SUBAGENT`/`tool:subagent` (ordering only; no tool is built);
- `src/agent/preset.cpp:449` declares the capability
  `{"subagents", CapabilityDisposition::In}` (a preset capability flag);
- `src/ui/ui_event_adapter.cpp:145` and `src/ui/ui_render.cpp:434-440` render
  `SubagentUpdated` (a live-UI view of subagent events).

Session kinds/payloads remain at `src/session/session.cpp:169`, `:181`;
`src/session/events.cpp:627`, `:631`; `src/cli/session_cli.cpp:48`.

**The job subsystem has no production wiring.** `grep -rn '#include "ymh/jobs'
src/` returns only the two implementation files themselves
(`src/jobs/job_wakeup.cpp:1`, `src/jobs/job_registry.cpp:1`); `JobRegistry` is
never instantiated in production, no `JobWakeupPolicy` is ever constructed, and
`make_job_output_tool`/`make_job_list_tool`/`make_job_kill_tool`
(`include/ymh/tools/job_tools.hpp:22-27`) are called only in
`tests/unit/job_test.cpp`. The registration range
`src/agent/workspace_runtime.cpp:223-233` registers the builtin tools, git,
`exit_plan_mode`, terminal, skill, and MCP tools — **not** the job tools. `44:87`
confirms it: *"The shipped tree has **no** goal, job, or log-only command
concept."* This spec therefore **wires** `JobRegistry`/`JobWakeupPolicy` and the
job tools for the first time (55-D4, 55-H1); it does not "reuse" running
machinery.

The builtin tool roster is `make_read_file_tool`, `make_write_file_tool`,
`make_edit_file_tool`, `make_grep_tool`, `make_glob_tool`, `make_shell_tool`
(`include/ymh/tools/builtin_tools.hpp:15-23`), plus git, plan, terminal, skill,
and MCP. There is no delegation tool.

### 2.4 The deliberate omission this spec lifts

`06` §7 (`docs/design/06-agent-loop.md:937-939`) and its omissions list
(`:1141-1142`, `:1342`, `:1374-1375`) record:

> **Synchronous v1 (decision (i)).** The spawning tool awaits the child's
> terminal state via `whenIdle()`; the parent stays in `CallingTool` until
> fan-in. Detached/async subagents and the §20.25 delta coalescer are deferred.

This spec lifts the "detached/async subagents" half of that omission (the delta
coalescer stays deferred).

### 2.5 The machinery to build on

| Facility | Location | What it gives this spec |
|---|---|---|
| `AgentPresetRoster` + `mount`/`scope_for`/`compose_from` | `include/ymh/agent/preset.hpp:207`, `:212`, `:223`; `src/agent/preset.cpp:728`, `:776`, `:829` | The live leaf scope for a child; the tool list is `prompt_->assemble(scope).tools` (`src/agent/context_assembler.cpp:80-85`). |
| `apply_child_composition` | `src/agent/preset.cpp:789-833` | Join + delegation statement + persona shadow + tool restriction, in one call. |
| `ChildComposition` | `include/ymh/agent/preset.hpp:149-152` | `{persona?, tool_filter?}`. |
| `ToolRestriction` | `include/ymh/agent/preset.hpp:35-38` | `{allow, deny}`; deny wins; intersect-only. |
| `check_delegation_depth` | `include/ymh/agent/preset.hpp:169-170`; `src/agent/preset.cpp:875-884` | Depth pre-flight, `DelegationDepthExceeded`. |
| `PresetConfig::max_depth` (default `3`) | `include/ymh/agent/preset.hpp:108`; `src/config/config.cpp:1128`, `:1143-1148` | The single depth source (`42-D18`). |
| `kDelegationScopeStatement` | `include/ymh/agent/preset.hpp:156-161` | Registered verbatim by `apply_child_composition`. |
| `AgentRegistry` | `include/ymh/agent/agent_registry.hpp:41-42` (`create`/`resume`), `:48` (`findShared`), `:93` (`registerAgent` decl); `src/agent/agent_registry.cpp:60` (`create`), `:101` (`registerAgent`), `:151` (`dispose`), `:177` (`finalizeAll`) | `create`/`resume`/`findShared`/`getShared`/`dispose`/`finalizeAll`; the lifetime owner. No `createChild`/`liveSubagentCount` today — this spec adds them (§3 D8). |
| `SessionOptions` (already carries `depth`, `parentSession`, `agent_preset`, `permission_preset`) | `include/ymh/session/session_manager.hpp:29-43` | Child session creation inputs. |
| `SessionHeader::depth` | `include/ymh/session/session.hpp:59-61` | The durable, monotone depth. |
| `JobRegistry` + `JobWakeupPolicy` | `include/ymh/jobs/job_registry.hpp`, `include/ymh/jobs/job_wakeup.hpp:24-36`; `src/jobs/job_wakeup.cpp:103-141` | Verified **library** code with **no production wiring** (`44:87`; §2.3). This spec instantiates/registers them for the first time. The policy delivers **at most one notice per in-memory job** and drops it silently when the owner is not resident (`job_wakeup.cpp:97-106`); the durable guarantee is **built** here as **at-least-once** over the parent log (55-D4, 55-H2). |
| `job_output`/`job_list`/`job_kill` tools | `include/ymh/tools/job_tools.hpp:22-27` | Built but **not registered** today (§2.3); wired by this spec for one-shot background delegation. |
| `LLMPool` | `include/ymh/agent/llm_pool.hpp:43`; `src/agent/workspace_runtime.cpp:186` | Per-daemon bounded LLM slots. |
| `TurnExecutor` | `include/ymh/agent/turn_executor.hpp:41-85` | Per-daemon bounded turn workers + queue. |
| `HostRuntime::activateSession` | `src/host/host_runtime.cpp:934-946` | Submits a turn body to `TurnExecutor` (asynchronous) but also mutates `active_session_`. This spec wraps the submit in a `SessionActivator` seam that does **not** set `active_session_` (55-D11, 55-H9). |
| Spec-54 routing | `include/ymh/agent/model_selection.hpp:32-39`, `:43-59`; `include/ymh/llm/llm_runtime.hpp:237`, `:262`; `docs/design/54-multi-endpoint-routing-errata.md` §3–§5 | `ModelSelection{model, model_name, endpoint, parameters, profile, provider}`; `resolve_adapter` (exact-route preflight, **private** — 55-A10 adds the public `preflight_route` wrapper) and `prepare_call`. |
| `ContextForm::Relay` | `include/ymh/agent/provenance.hpp:46`, `:62`, `:88` | Already exists, unused; the `send_message` form. |
| `SubagentSpawned`/`SubagentFanIn` | `include/ymh/session/events.hpp:213-230`, `:427-433` | Durable parent-log edges. |

### 2.6 Runtime facts that constrain the design (verified)

1. **`send` runs a turn inline when the agent is idle.** `AgentLoop::enqueue`
   calls `activate()` when `!running_` (`src/agent/agent_loop.cpp:200-214`), and
   `activate()` runs `runTurn()` synchronously until no turn trigger remains
   (`:216-250`). A `send` from a tool-execution thread therefore **blocks that
   thread** for the child's whole turn.
2. **Asynchronous activation exists only through `HostRuntime::activateSession`,
   which submits to `TurnExecutor`** (`src/host/host_runtime.cpp:934-946`). This
   spec extracts that submit behind a `SessionActivator` seam so a background
   child is not marked `active_session_` (55-D11, 55-H9).
   `TurnExecutor` has `max_workers = ResourceCaps::max_llm_concurrency` (default
   `4`) and `queue_capacity` the same (`include/ymh/agent/turn_executor.hpp:48-53`;
   `docs/design/24-agent-lifetime-errata.md:287-289`). A full queue makes
   `submit` return `false`, surfaced as `InboxFull`
   (`src/host/host_runtime.cpp:941-943`).
3. **Parallel tool calls run on `std::async` threads**, bounded by
   `ToolScheduleConfig::max_parallel_tool_calls` (default `10`,
   `include/ymh/agent/agent.hpp:96-97`; `src/agent/agent_loop.cpp:1232-1245`,
   `:1315`). An `Exclusive` tool is a barrier; a `ParallelSafe` tool may
   overlap.
4. **The per-agent tool list is the prompt-scope assembly**
   (`src/agent/agent_loop.cpp:567`: `request.tools = services_.context->tools(active_scope());`),
   where `active_scope()` is `presets->scope_for(id_)`
   (`src/agent/agent_loop.cpp:407-414`). A leaf-scoped `ToolRestriction` set by
   `apply_child_composition` therefore filters the child's tools with no new
   mechanism.
5. **`ToolContext` carries no calling-agent identity** (`include/ymh/tools/tool_context.hpp:25-63`);
   the job tools solve this with a `JobOwnerResolver`
   (`include/ymh/tools/job_tools.hpp:20-27`). The delegation tools need the same
   pattern.
6. **The `JobWakeupPolicy` *class* delivers at most one notice per in-memory
   job — it is not started in any daemon.** A job's terminal record is
   first-wins; an unreported completion is `inject`ed into a busy owner or
   `followup`ed (wakeup) to an idle owner (`src/jobs/job_wakeup.cpp:103-141`);
   the notice is a plugin-sourced `UserMessage`/`ContextInjected` with
   `ContextForm::Notice` and an **empty** `MessageSource::plugin`
   (`src/jobs/job_wakeup.cpp:21-42`); 55-A12 adds the per-job `notice_plugin`
   override (stamped on the notice it builds) and the per-snapshot
   `notice_text` override (used in place of the file-local generic text), so
   this spec can stamp both the settlement correlation and dsh's text. The class
   is
   **never constructed in production** (`44:87`; §2.3), so its guarantee is a
   property of the class, not of the shipped daemon. Four further limits make
   the guarantee weaker than "durable, exactly one" (55-H2):
   - `on_job_done` **silently returns** when the owner is not resident
     (`src/jobs/job_wakeup.cpp:97-106`) — no retry, no persistence;
   - `JobRegistry` is **purely in-memory** (no persistence, no replay);
   - nothing turns the durable `SubagentFanIn` back into a notice on resume
     (`src/session/session.cpp:540` ignores it; `src/agent/preset.cpp:847` uses
     it only for `is_blank`);
   - even with a resident parent, `inject`/`followup` push only to the
     **in-memory** inbox (`src/agent/agent_loop.cpp:196-214`), so a crash before
     drain loses the notice.

   This spec therefore **builds** a durable unreported-settlement record and a
   replay rule, and states the guarantee as **at-least-once** (55-D4).
7. **`AgentRegistry` uses one shared `AgentConfig` for every agent**
   (`src/agent/agent_registry.cpp:115-135`); a child's route is therefore
   carried by its **session header** and its `ModelSelectionController`, not by
   a per-child `AgentConfig`. `effective_model_selection()` prefers the durable
   session selection (`src/agent/agent_loop.cpp:537-556`).

---

## 3. Decisions (55-D)

### 55-D1 — Two delegation modes, mirrored from dsh

ymh implements **both** dsh modes as first-class, selectable per tool instance
via `DelegationToolConfig::background_mode`:

- **`one-shot`** — every call creates a **fresh** child with its own session,
  own system prompt, and **zero parent context** (dsh's `spawn` provider).
- **`continuable`** — every call starts a **persistent, resident** child and
  returns a durable `childId` for later messages.

`one-shot` is the default `background_mode` (dsh's default). Both modes can be
mounted at once as two tool instances with distinct `toolName`s (55-D2). The
mode is a property of the **tool instance**, never of a call.

### 55-D2 — The tool surface

The daemon wires a small family of tools, mirroring dsh's
`dsh-tool-subagent` + `dsh-tool-subagent-control` split:

| Tool | Registered by | Model-facing fields |
|---|---|---|
| `subagent` | one-shot delegation instance | `description`, `prompt`, `run_in_background?`, and — when route selection is enabled — `provider?`, `model?`, `reasoning_effort?` |
| `subagent_continuable` | continuable delegation instance | same fields; `run_in_background` **defaults true** |
| `send_message` | global control | `agent_id`, `message` |
| `interrupt_agent` | global control | `agent_id` |
| `list_agents` | separate loadable control plugin (`dsh-tool-subagent-control/list-agents`; the control root ships only `send_message`+`interrupt_agent`) | `scope?` (`children` default \| `descendants`) |
| `list_subagent_models` | registered once when any instance enables route selection | `provider?`, `model?` |

One instance per delegation target, each with a distinct `toolName`; the tool
exists exactly while its provider exists. The default composition mounts
`subagent` (one-shot) and `subagent_continuable` (continuable), plus the three
control tools and `list_subagent_models`. A deployment may mount fewer
(55-OQ-1).

**Rendering of results** (mirrors dsh's `render`):

| Outcome | Tool text |
|---|---|
| foreground completed | the child's final assistant text |
| one-shot background | `started background subagent job <jobId>` |
| continuable start | `started subagent <childId>` |
| any failure | the stop-reason headline + diagnostic + labelled partial output (55-D9) |

### 55-D3 — `run_in_background`, and what a "durable child" is in ymh

`resolveDelegationRun` mirrors dsh exactly:

```
if !enable_run_in_background:
    run_in_background == true  -> errored result ("run_in_background is disabled …")
    run_in_background == false
else:
    run_in_background = request.run_in_background ?? (background_mode == continuable)
```

- **one-shot, foreground** (`run_in_background == false`): the call waits for
  the child's terminal state and returns its final text. The parent's turn
  blocks (the tool occupies one tool-scheduling slot, §2.6(3)).
- **one-shot, background** (`run_in_background == true`): the run is registered
  as a `kind == "subagent"` **job** owned by the parent; the call returns
  `started background subagent job <jobId>` immediately; the child's final text
  is read with the existing `job_output`, and the run is stopped with
  `job_kill`. This is `44` §3.3 made real.
- **continuable, background** (omitted or `true`): a resident child is created
  and the call returns `started subagent <childId>` **without waiting**; the
  runtime delivers one settlement notice (55-D4); `send_message` sends more work.
- **continuable, foreground** (`run_in_background == false`): waits for the
  current activation and returns its final text, leaving the child resident.

**What "durable child" means in ymh.** A continuable child is a **resident
`AgentLoop` plus its durable session** inside the *same* workspace daemon. It
is not a separate process and not a supervisor-level object.

- **Same daemon.** The child lives in the workspace daemon that owns the
  parent, so it shares the daemon's `AgentRegistry`, `LLMPool`,
  `TurnExecutor`, permission policy, and `ExecutionEnvironment` root. No
  transport hop is involved.
- **Durable session.** The child's event log is the child's own
  `<workspace>/.ymh/sessions.db` history, exactly like any session. The
  parent→child edge is durable twice over: `payload::SubagentSpawned` in the
  parent log and `SessionHeader.parentSession` on the child.
- **`childId` is the child's `SessionId`.** It is stable across daemon
  restarts and is the `agent_id` accepted by `send_message`/`interrupt_agent`/
  `list_agents`.
- **Restart semantics.** A daemon restart does not preserve the live agent: an
  in-flight activation is cancelled by teardown (which appends exactly one
  terminal event, `24-D10`/AL25), and the child survives as a **stored session**
  that can be resumed. `send_message` to a cold direct child resumes it
  (55-D5). A settlement notice for an activation interrupted by shutdown is
  produced from that activation's terminal event, so the parent is never left
  believing a killed activation completed.

### 55-D4 — The settlement notice (durable record + replay; at-least-once per **background-registered** residency epoch)

dsh settles an Activation only after "agent activity to finish, an empty inbox,
and no owned children" (one notice per residency epoch, **not** per turn), and
delivers "one settlement notice when the child's Activation ends."

**Activation = residency epoch (55-H3).** In ymh an activation is a *residency
epoch*: it opens when a **resident-making activation** makes the child resident —
a one-shot **background** start, a `continuable` start (foreground or background),
or a `send_message` that revives a child whose previous epoch has already closed —
and it closes only when **all** hold:

- the child is idle (`AgentState::Idle`, no in-flight turn),
- its inbox is empty (no queued/steered message), and
- it owns no live children.

`onSettled` is the **terminal-event signal**: it fires once when the child's
terminal event is appended. Settlement (the epoch close) is a **separate drain
check**: the service then evaluates the drain condition above and defers
settlement until it holds. `onSettled` firing therefore does not itself settle the
epoch (55-R3-H2). A `send_message` (Steer or followup) that arrives while the epoch
is open **does not open a new activation** — it extends the current
epoch, so **one** notice is produced when the child finally drains, and the
epoch keeps the registration of its opener (an epoch opened by a foreground
blocking activation still produces none; see the predicate below). A
`send_message` to a child whose epoch is already closed opens a **new** epoch and
therefore produces a new notice; that new epoch is **background-registered**
exactly like a background start (below). This is dsh's drain semantics; a
per-terminal-event rule would emit a premature notice for a steered child and
then none for the queued turn (55-H3).

**Durability is built here, not inherited (55-H1/55-H2).** The shipped job
subsystem is unwired (§2.3), `JobWakeupPolicy` silently drops a notice when the
owner is not resident, `JobRegistry` is in-memory, and a crash between in-memory
`inject`/`followup` and drain loses the notice. ymh therefore pins a **durable
unreported-settlement record** and a **replay rule**:

1. When an epoch starts **asynchronously** — any non-foreground-blocking opener:
   a background start **or** a `send_message` that opens a new epoch — the
   `SubagentService` registers it
   as a `kind == "subagent"` job owned by the parent `AgentId` (`44` §3.2) and
   stamps the job with the **notice source** `notice_plugin =
   "subagent-settlement:<child-session-id>#<ordinal>"` (55-D13/§4 pin the seam).
   **The ordinal is `existing_count + 1`**, where `existing_count` is the number
   of `SubagentFanIn` events already in the parent log for that child at
   registration time: it is the 1-based position this epoch's settlement fan-in
   (step 2) will occupy, and it is the same value step 3/step 4 derive by
   counting the log **after** step 2 appends. Stamping `existing_count` without
   the `+1` would make the fast-path marker disagree with replay's derived
   ordinal, so `replayUnreportedSettlements` would find "no matching notice" and
   re-deliver a duplicate on every parent resume / daemon start (55-R5-H1).
   `JobRegistry::start` copies `notice_plugin` verbatim into the snapshot
   (55-A12; 55-R5-M3). This is the first production use of
   `JobRegistry`/`JobWakeupPolicy` (§2.3). A **foreground-blocking** activation
   (a `continuable` foreground call) registers **no** job and is never a replay
   candidate (below).
2. On settlement the service appends **`payload::SubagentFanIn{subagent,
   outcome, summary, notice_expected}`** to the **parent** log
   (`include/ymh/session/events.hpp:224-229`; the additive `notice_expected` bit
   is 55-A11). `notice_expected` is `true` **iff** the epoch was
   **background-registered** (step 1) — equivalently, **iff the epoch was not
   opened by a foreground-blocking activation**. Every background start *and*
   every `send_message`-opened epoch is `true`; only an epoch opened by a
   `continuable` **foreground** call is `false`.
   It is the durable discriminator the replay rule filters on. This is the
   structured, durable edge; **one fan-in per residency epoch**.
3. The model-facing notice is a plugin-sourced `UserMessage`/`ContextInjected`
   with `ContextForm::Notice` and a **durable correlation**: its
   `MessageSource::plugin` is `subagent-settlement:<child-session-id>#<ordinal>`,
   where `<ordinal>` is the 1-based count of `SubagentFanIn` events for that
   child in the parent log **after step 2 appends** — i.e. exactly the
   `existing_count + 1` pinned in step 1 (a child can settle more than one
   epoch, so the child id alone is ambiguous). The plugin string is produced
   through the **pinned seam** (55-A12): the registered job carries it as
   `notice_plugin`, so the resident fast path's `JobWakeupPolicy` stamps it, and
   `replayUnreportedSettlements` reconstructs the same string from the fan-in.
   The notice **text** is likewise produced through the pinned seam: at
   settlement the service supplies dsh's rendered text as
   `JobOutcome::notice_text`, which `JobRegistry::settle` copies onto
   `JobSnapshot::notice_text`, and `deliver` uses in place of the file-local
   generic `notice_text` (55-A12; 55-R5-H2). `settle()` therefore needs **no**
   notice-source parameter — the source lives on the registered job snapshot —
   and replay derives it from the fan-in ordinal. Because the notice is itself
   appended to the parent log, its presence **is** the durable "delivered"
   marker.
4. **Replay rule.** On parent resume and on daemon start, `SubagentService`
   scans the parent log for every **background** `SubagentFanIn`
   (`notice_expected == true` — every non-foreground epoch, including one opened
   by `send_message`) with **no** matching
   `subagent-settlement:<child>#<ordinal>` notice and delivers exactly one notice
   per unreported background fan-in per replay pass (idempotent under repeat),
   then appends the delivery record. **Foreground fan-ins
   (`notice_expected == false`) never participate** — they deliberately carry no
   model-facing notice (below), so replay must not manufacture one. The
   in-memory `JobRegistry.reported` flag is an optimization for the resident fast
   path, **never** the durability mechanism. This covers the
   owner-not-resident path (`src/jobs/job_wakeup.cpp:97-106`), the
   in-memory-inbox crash window, and a daemon restart.

**Guarantee (honest).** Settlement-notice delivery is **at-least-once** per
**background-registered (non-foreground)** residency epoch: the durable `SubagentFanIn` is the source of
truth and replay guarantees eventual delivery on the next parent resume / daemon
start; a crash between append and drain may re-deliver the same correlated
notice (idempotent by `subagent-settlement:<child>#<ordinal>`). It is **not**
exactly-once under crash (55-H2), and if the parent is never resumed the notice
is simply never delivered. 55-I7/I31 are restated accordingly.

**Notice text (dsh-faithful override for `kind == subagent`).** The shipped
`notice_text` is a **file-local free function** consumed unconditionally by
`deliver` (`"Background job <id> <status>[: <detail>]"`,
`src/jobs/job_wakeup.cpp:15-22`, `:123`); it is **not** a field and has no hook,
so the text cannot be overridden through any shipped path. This revision pins
the seam (55-A12; 55-R5-H2): an optional `JobSnapshot::notice_text` is used by
`deliver` **in place of** the generic text when set. Because dsh's text embeds
the child's closing message — known only at settlement — the service supplies
the rendered text at settlement as `JobOutcome::notice_text`, which
`JobRegistry::settle` copies onto `JobSnapshot::notice_text`;
`JobStart::notice_text` exists for symmetry and is only a registration-time
base. For a
`kind == "subagent"` job the service sets it to dsh's text:

```
Background subagent <child-id> finished and will do no further work unless you
send it more.
Its closing message:
<content>
```

The closing message is the child's final assistant text; for a
failed/cancelled/killed child it is the headline + diagnostic of 55-D9. Absent
(`nullopt`) reproduces today's generic text byte-for-byte for every other job
kind. 55-OQ-11 records whether the generic text should instead be used for
parity.

**Quiet.** `CompletionDelivery::Quiet` suppresses the model-facing notice
(`deliver()` returns early, `src/jobs/job_wakeup.cpp:129-131`). It does **not**
suppress the durable `SubagentFanIn`. For a delegated child the notice is the
entire deliverable, so this spec **overrides `Quiet` for `kind == "subagent"`**
(the generic job policy is unchanged); 55-I7 is therefore unconditional for
subagent jobs.

**A killed child still notifies.** `JobRegistry::kill` sets
`snapshot.reported = true` **before** settle (`src/jobs/job_registry.cpp:144`;
another assignment at `:122`) and
`on_job_done` early-returns on `reported` (`src/jobs/job_wakeup.cpp:98`), so a
killed child would get **no** notice — unlike dsh, which delivers a notice line
for a stopped child. This spec pins that the settlement path **does not
pre-mark `reported` on kill** (or emits the notice explicitly), so a killed
background child produces a `Cancelled` notice (55-F15).

**Why this and not a new event.** `44` pins first-wins settlement, owner
fencing, the wakeup policy, and the bounded self-exciting chain; the durable edge
is `SubagentFanIn`. Adding an event type would duplicate that machinery and would
need a wire/consumer amendment this spec does not need.

**Foreground-blocking** activations do not need a model-facing notice: the tool
result carries the text, and `SubagentFanIn` is still appended for the durable
edge — with `notice_expected == false`. Both foreground-blocking forms — the
one-shot foreground run and the `continuable` **foreground** start — are the only
`false` cases; a `send_message`-opened epoch is background-registered and
**does** get a notice. A foreground fan-in is therefore **excluded from
the replay rule**; replaying it would deliver exactly the notice the spec says it
does not need (55-R4-H1).

### 55-D5 — `send_message`, `interrupt_agent`, `list_agents`

**`send_message(agent_id, message)`** — mirrors
`dsh-tool-subagent-control`:

- **Authorization.** A live agent may target its **direct continuable child**;
  a resident continuable child may also target its **direct parent**. Any other
  target (self, sibling, stale, non-ancestor) is an errored result. The edge is
  checked against `SessionHeader.parentSession`; the sender identity is the
  caller's session.
- **Delivery.** A **busy** target receives the message at its nearest step
  boundary through `Agent::steer`; an **idle** target starts a turn through
  `Agent::followup`; a **cold direct child** (not resident) is resumed first,
  then receives it. These map 1:1 onto dsh's Steer / start-a-turn / resume
  lifecycle.
- **Return.** Only **acceptance**, never a reply: the accepted message's stable
  `MessageId` (`using MessageId = std::string`, `include/ymh/session/ids.hpp:19`).
  A rejection (`InboxFull`, `AgentDisposed`, **unavailable parent** — a resident
  continuable child whose direct parent cannot be reached, dsh's failure —
  unknown/disposed/descriptor-less/foreign target) is an errored result stating
  the message was not delivered.
- **Framing.** The message is framed `Agent <sender-session-id> sent a
  message:\n<message>` and recorded with `MessageSource::Kind::Plugin`,
  `plugin = "agent-message"`, and `ContextForm::Relay` (55-D12 pins the
  optional structured `sender`).

**`interrupt_agent(agent_id)`** — stops **only** the target's current turn via
`Agent::cancel()` (`include/ymh/agent/agent.hpp:151`): queued messages stay
parked, descendants keep running, and the child remains available for
follow-ups. It returns when the stop request is **accepted**, not when the
target is quiet. Interrupting an already-idle target is an accepted no-op
(`cancel()` is a no-op when no turn is in flight, `src/agent/agent_loop.cpp:252-260`).
Self/sibling/stale/non-ancestor callers, an unknown/disposed target, and a
descriptor-less (no resumable header) target all get errored results.

**`list_agents(scope?)`** — lists the calling agent's continuable children:
`children` (default, direct) or `descendants` (whole subtree, stable pre-order),
each with its **durable direct-parent `SessionId`** and **depth**. Status is
derived from the live registry: `running` (`AgentStatus::Running` or
`hasPendingWork()`), `idle` (resident and `AgentState::Idle`), or `ready`
(stored, not resident — resumable). **One-shot children are absent** because
they cannot accept `send_message`. Unreadable candidates appear as diagnostics
and never abort the listing.

### 55-D6 — Per-child route selection

**Tool fields.** When `model_selection` is enabled for an instance, the
delegation tool exposes optional `provider`, `model`, and `reasoning_effort`,
and the shared `list_subagent_models` tool is registered once.

**`modelSelectionSettings` is NOT mirrored (fidelity ledger).** In dsh,
`modelSelectionSettings` is a **host session-recorded allow-list**: it is
recorded in the top-level Session, inherited by children, is "unchanged by later
settings edits", and `list_subagent_models` registers **only when that policy
exists**. ymh substitutes a static per-tool `bool model_selection` (55-D12),
which is a coarser, non-inherited, config-time switch. This is a **deliberate
non-mirror**, recorded for fidelity honesty; the closest ymh analogue is the
session-pinned `permission_preset` (`52-D15`), not a recorded allow-list.

**The meaning of `provider` in ymh.** Spec 54 makes the **endpoint name** the
unique route identity (`54-D2`); `ProviderId` is deliberately not unique. The
model-facing field keeps dsh's name `provider` for fidelity, but its **value is
an endpoint name** (the `llm.endpoints` key; `""` = the anonymous default
endpoint). `list_subagent_models` returns endpoint names, not provider ids. This
is recorded as 55-OQ-2 (rename to `endpoint`).

**Call shape.** A call supplies `provider` **and** `model` together, or only
`reasoning_effort` when the configured/parent/provider defaults already provide
the route. A half route is an errored result. dsh accepts a **model-only** call
and inherits the provider; ymh is deliberately stricter (the endpoint is the
route identity, 54-D2), recorded as 55-OQ-12.

**Precedence (ymh-mapped, in order):**

0. **Provider-advertised route defaults (dsh-only; not mirrored)** — dsh's
   `provider.agentRouteDefaults` (`index.js:394`) is the **backend/provider-
   advertised** default route, spread as the base **before** config at
   `index.js:496-504` and overridden by any configured value. It is **not**
   inherited from the parent's provider/endpoint (the Rev-2 text was wrong,
   55-M7). ymh has no backend-advertised route defaults: its analogue is the
   configured default (`llm.active_model` / the endpoint default), which is
   already the terminal fallback of step 5. Step 0 is therefore recorded as a
   **deliberate non-mirror** (like `modelSelectionSettings`); it is not a separate
   ymh layer and does not overlap step 5's parent-effective layer.
1. **Configured child defaults** — `DelegationToolConfig::agent_options`
   (`endpoint`/`model`/`reasoning_effort`/`max_tokens`), dsh's tool config.
2. **Per-call model fields** — overlay (1).
3. **Route-aware effort merging** — if the effective route changed and the call
   supplied no effort, the inherited route-owned effort is **cleared** (dsh's
   rule); an explicit effort always wins.
4. **Route preflight** — before the child is created, `SubagentService` resolves
   the effective `LlmCallConfig` through the **public**
   `LlmRuntime::preflight_route(config) const`
   (`include/ymh/llm/llm_runtime.hpp`; 55-A10, 55-R3-H1) — a thin public wrapper
   over the private `resolve_adapter` that binds **no** call generation — which
   throws `NoProviderRouteError` on failure. (`prepare_call` is public, but it
   binds an adapter generation and returns a `Task<PreparedCall>`, so it is not
   used for a side-effect-free preflight.) A failed preflight is an errored
   result and creates no child. **This preflight
   validates only the `(endpoint, profile_id)` route; it does NOT validate the
   model id or the effort value** (`src/llm/llm_runtime.cpp:368-441`). So
   "preflight is the authority, not catalog membership" was false (55-H5): a
   bogus model/effort passes preflight and fails on the child's first request
   (55-F17). To keep "creates no child", `AgentRegistry::createChild` validates
   `model`/`reasoning_effort` against the catalog/adapter **before any session is
   created** (the admission seam owns all validation; 55-D8); catalog membership
   is therefore **required** for a child route,
   and the Rev-1 "unlisted model accepted" claim is deleted (55-H4).
5. **Default = the parent's effective route.** When no layer (1)–(3)
   contributes, the child inherits the parent's **effective** `ModelSelection`
   — `ModelSelectionController::effective(parent_session)`
   (`include/ymh/agent/model_selection.hpp:59`), falling back exactly as
   `AgentLoop::effective_model_selection` does when the controller returns
   `nullopt` (the configured/session fallback, `src/agent/agent_loop.cpp:538-556`).
   The chain is **total**: the child always resolves a route. This replaces dsh's
   "parent's latest logged request, then creation options" with ymh's durable,
   single-source selection. `max_tokens` is retained from the configured
   defaults (dsh's `maxTokens` retention).

**The child's durable route.** The resolved route is written to the child's
session at creation (`SessionOptions.model`/`model_name`, plus the additive
route fields of 55-D12) **and** persisted on `SessionHeader` as
`(endpoint, profile_id)` (55-A7; 55-H4). `SessionManager::createSession` today
copies only `model`/`model_name` into `SessionHeader`
(`src/session/session_manager.cpp:52-70`; `include/ymh/session/session.hpp:44-66`
carries no endpoint/profile), and `ModelSelectionController::durable_` rebuilds
solely from `header.model_name` else `header.model` via `ModelCatalog::find`
(`src/agent/model_selection.cpp:167-176`), which ignores endpoint/profile and
returns `nullopt` for an unlisted id. Without the persisted pair an unlisted
model silently reverts to the config default on resume, and a literal wire id
shared by two endpoints can rebuild the **wrong** route (55-H4). With it, the
child's `ModelSelectionController` rebuilds the same `ModelSelection` on every
request and on resume. The child's own `session.set_model` may later change it,
exactly like any session.

**Catalog membership is required (revised).** Because the preflight validates
only the route (55-H5), a child route must name a catalog member:
`AgentRegistry::createChild` validates `model`/`reasoning_effort` against the
catalog/adapter before any session is created (the admission seam owns all
validation; 55-D8). `list_subagent_models` advertises **only routable endpoints**
(the routability source is injected in 55-D13), and the Rev-1 claim that an
"unlisted model id" is accepted is deleted (55-H4). Semantics:

- no arguments → routable endpoints;
- `provider` (an endpoint name) → that endpoint's advertised `llm.models`
  entries;
- `provider` + `model` → that exact model's profile and its accepted
  `reasoning_effort` values.

**Reasoning-effort vocabulary.** ymh models effort as a free string
(`include/ymh/config/config.hpp:77`: `"low" | "medium" | "high" | "xhigh"`)
and has no adapter-advertised per-model effort list (dsh's
`model.reasoning.efforts`). `list_subagent_models` therefore reports the
configured/adapter-accepted effort vocabulary. `AgentRegistry::createChild`
validates the exact effort against that vocabulary before any session is created
(the route preflight alone does **not** validate it, 55-H5). Adapter-advertised
effort metadata is 55-OQ-3.

### 55-D7 — `maxDepth` and the fan-out cap

**Depth.** `maxDepth` is `presets.max_depth` (default `3`), the single source of
truth (`42-D18`). The delegation tool has **no second copy**. Each attempted
start calls `check_delegation_depth(parent_depth, presets.max_depth)` where
`parent_depth` is the caller's folded `SessionHeader::depth`
(`include/ymh/session/session.hpp:59-61`), and refuses with
`AgentErrorCode::DelegationDepthExceeded` as an **errored result**. The tool
stays **visible at the cap** (dsh's rule): the check is per attempted start,
not a registration-time removal. `max_depth == 0` forbids delegation entirely.

**Breadth (new).** Depth alone does not bound a tree, and neither
`max_concurrency` (per-endpoint, spec 52) nor `LLMPool`
(`ResourceCaps::max_llm_concurrency`, a provider-call slot pool) is a fan-out
budget: both admit arbitrarily many *agents* that merely block on a slot. This
spec adds a **global per-daemon fan-out cap**:

```cpp
// include/ymh/execution/resource_governor.hpp — additive
struct ResourceCaps {
    // … existing fields …
    std::size_t max_live_subagents{8};   // 55-D7: live delegated children, per daemon
};
```

- **What it counts:** live delegated child agents — **resident with an open
  epoch** (55-D4) — across **all** parents in the daemon. It is distinct from
  `max_llm_concurrency` (provider-call slots) and from `TurnExecutor`
  (turn workers).
- **Where it is enforced:** in the delegation **admission** step, inside
  `AgentRegistry::createChild` (55-D8), **before** any session is created.
  A start that would exceed the cap is an **errored result** and creates **no
  session and no agent** (no orphan).
- **Admission is an atomic reservation, not a read-then-create (55-H7).**
  Checking `liveSubagentCount()` before `createSession` is a TOCTOU: the count
  increments only at registration *after* the session exists, and
  `AgentRegistry::create` deliberately does **not** hold `mutex_` across
  `createSession` (`src/agent/agent_registry.cpp:60-83`; D11 forbids holding the
  registry mutex across `Session`/`AgentLoop` calls), so two concurrent
  `createChild` calls can both observe `N-1`. `createChild` therefore performs an
  **atomic pre-increment reservation** (a dedicated admission counter, or
  `live_subagent_count_++` under `mutex_` with rollback on any later failure)
  before `createSession`, and releases the reservation on success (the count then
  tracks the registered agent) or rollback (55-F3/F14, 55-I10).
- **When a slot is released (continuable children, revised).** A continuable
  child is "live" while it is resident and its epoch is open (55-D4). To avoid
  eight idle-but-resident continuable children permanently starving all future
  delegation under the default `8`, a continuable child releases its slot when
  its epoch **closes** (idle + empty inbox + no owned children, 55-D4): the
  session stays durable and resumable, but it no longer counts against
  `max_live_subagents`; a later `send_message` re-reserves a slot before it
  resumes the child, and if no slot is free the resume is an errored result.
  This gives continuable children the same bound as one-shot children while
  keeping them resumable. 55-OQ-5 records the alternative (a separate
  continuable bound).
- **Interaction with the other bounds:** `ToolScheduleConfig::max_parallel_tool_calls`
  (default `10`) bounds how many parallel delegation calls may overlap in one
  step; `TurnExecutor` (`4` workers + `4` queue) and `LLMPool` (`4` slots) are
  downstream backpressure. The fan-out cap is the **admission** bound that keeps
  agent creation from outrunning all three. Default `8` is chosen to sit below
  the sum of the per-step and executor bounds while leaving headroom for
  sequential delegation; the value is a product decision (55-OQ-5).

### 55-D8 — Admission: `AgentRegistry::createChild`

Child creation is a single new seam on the lifetime owner, so every bound is
enforced in one place and no half-created child can exist:

```cpp
// include/ymh/agent/agent_registry.hpp — additive
struct ChildSpawnRequest {
    SessionOptions             options;      // cwd, kind=Subagent, parentSession, depth, preset
    std::uint32_t              parent_depth = 0;
    std::uint32_t              max_depth;    // REQUIRED: the caller passes the
                                             // resolved presets.max_depth (42-D18);
                                             // no second default lives here, so a
                                             // non-3 deployment cap is never ignored.
    std::optional<ChildComposition> composition;   // persona / tool_filter
    std::optional<ChildRoute>  route;              // 55-D12
};

class AgentRegistry {
    // … existing create/resume/findShared/getShared/dispose/finalizeAll …
    // Applies, in order: depth pre-flight, route/model/effort validation,
    // composition pre-validation, atomic fan-out reservation, session create,
    // agent registration + preset mount, apply_child_composition (when a roster
    // is present), and route application. Returns a typed error and leaves NO
    // child on any failure (compensating rollback, below). First production
    // caller of check_delegation_depth and apply_child_composition.
    [[nodiscard]] std::expected<AgentId, AgentError>
    createChild(const ChildSpawnRequest& request);

    // Live delegated-child count for the fan-out cap (55-D7).
    [[nodiscard]] std::size_t liveSubagentCount() const noexcept;
};
```

The order matters: `apply_child_composition` requires the child's `AgentId`
and the parent's live leaf, so it runs **after** registration/mount and
**before** the child's first request. The child's `SessionOptions.kind` is
`SessionKind::Subagent`, `parentSession` is the parent session, and
`depth` is `parent.depth + 1` (a fork inherits depth; a spawn increments it,
`42-D8`).

**Compensating rollback (55-H4, 55-H7, 55-medium).** "Leaves NO child on any
failure" is not free: `apply_child_composition` throws `UnknownAgent`
(`src/agent/preset.cpp:789-833`) **after** a durable session exists, and
`SessionManager::createSession` can throw `StoreUnavailable`/`LeaseLost` while
registration can fail. `createChild` therefore pre-validates everything it can
before the session exists (depth, route/model/effort, and that the requested
composition resolves against the roster), and on any post-create failure it runs
a **compensating rollback**: release the atomic reservation, dispose the agent,
and mark the just-created session abandoned (`SessionManager`'s close/abandon
path) so no half-created child survives (55-F16, 55-F21, 55-F22).

### 55-D9 — Failure semantics: errors, never partial success

Mirroring dsh's `stopReasonError` / `withDiagnosticAndPartialText`:

- A child that does not end `Completed` yields a tool result with
  `ToolOutcome::Error` (or `Cancelled` for an aborted run) — **never** a
  `Ok`/partial success. `SubagentFanIn.outcome` is `Failed`/`Cancelled`
  (`payload::SubagentOutcome`, `include/ymh/session/events.hpp:218-222`).
- The result text is the dsh stop-reason headline, mapped from the child's
  **persisted turn outcome**:
  | outcome source | headline |
  |---|---|
  | `TurnEnded`, `finish_reason == Stop` | (success) |
  | `TurnCancelled` | `subagent run was cancelled` |
  | `TurnFailed` | `subagent run failed` |
  | `TurnEnded`, `finish_reason == Length` | `subagent run hit its token limit before finishing` |
  | `TurnEnded`, `finish_reason == ContentFilter` | `subagent declined the task` |
  | anything else | `subagent run ended abnormally (<reason>)` |
- **Outcome source (55-medium).** `TurnEnded` carries only `TurnId`
  (`include/ymh/session/events.hpp:67`); `FinishReason::Length`/`ContentFilter`
  are parsed (`src/llm/openai_adapter.cpp:90,96`) but never read by `AgentLoop`,
  and `payload::AssistantMessage` has no finish field — so a token-limit stop is
  today indistinguishable from a normal `TurnEnded`. This spec pins an
  **additive persisted `finish_reason`** (on the assistant-message payload or the
  `TurnEnded` payload; no new `EventType`; registered as 55-A13), so the
  length/refusal rows are derivable. The codec follows the peer-additive rule
  (`A6`/`A11`): the field is **omitted when it holds its default** (`Stop`, the
  same value a pre-change record implies) and **decoded as the default when
  absent**, so existing records are byte-identical. Until that lands those two
  rows are not derivable and are detected only as a plain `TurnEnded` (recorded
  in F7's detection column). This is a required enabling change, not an optional
  refinement.
- The headline is followed by a `Diagnostic:` line (when present) and a clearly
  labelled `Partial output before the run ended:` block. **Diagnostic text is
  never conflated with the child's assistant output** (dsh's separation).
- A disposal failure is combined with the result failure via an aggregate, so
  disposal can never mask the real error (dsh's `settleForegroundRun`). The
  aggregate is the return value of `SubagentService::settle` (55-D13), which
  therefore cannot be `void`.

`payload::SubagentOutcome` needs no new enumerator: max-tokens and refusal are
`Failed` with a `detail`/`summary`. (A future refinement may add them; 55-OQ-6.)

### 55-D10 — The `SubagentRunner` settlement race fix (mandatory)

The `whenIdle([]() {})` pattern is replaced by a **real settlement await**.
Because `whenIdle` runs immediately when the agent is already idle
(`src/agent/agent_loop.cpp:341-360`), a callback registered *before* an
activation can be consumed by that immediate fire and never re-fire; ymh
therefore pins an additive `Agent` method that **always defers**:

```cpp
// include/ymh/agent/agent.hpp — additive
// 55-D10: register a one-shot settlement callback. It fires exactly once and is
// ALWAYS posted/deferred: it is NEVER invoked synchronously inside onSettled,
// even when the agent is idle. It fires when the first activation whose terminal
// event was appended at or after registration reaches that terminal event; the
// callback is NOT the drain/settlement condition — the service separately checks
// idle + empty inbox + no owned children and closes the epoch only when all hold
// (55-D4, 55-R3-H2). If the agent is idle and its last event is already terminal at
// registration, the callback is posted and drained on the next activate() exit,
// not inside the registering call. A disposed agent never fires it.
virtual void onSettled(std::function<void()> callback) = 0;
```

`AgentLoop` queues the callback in `settle_callbacks_` and drains it at the
**end** of `activate()`'s while-loop, after the terminal event is appended and
the state is set `Idle` (the same place `flushIdleCallbacks()` runs,
`src/agent/agent_loop.cpp:243-249`), with a terminal-event predicate that ignores
a registration made after the current terminal event. Delivery is **always
posted/deferred**, so 55-I6's "never synchronously during registration" holds
without exception; the "already terminal at registration" case is implemented as
a post-then-drain, never a synchronous call.

`SubagentRunner::run` is rewritten to:

1. record the child's start sequence,
2. register `child->onSettled(...)` to capture the terminal outcome,
3. start the activation (`send`/`followup`, or the `SessionActivator` seam for
   background),
4. **await settlement** (foreground) or resolve the activation's job (background)
   through a promise/future with a **failure/timeout path**: `send`/`enqueue`
   may return `InboxFull`/`AgentDisposed` (the callback then never fires) and a
   child may hang, so the await resolves on settlement, on a submit failure, on
   a settlement timeout, or on disposal (55-F21) — it never hangs the caller,
5. only then read the child's terminal event and build the result.

`SubagentRunner` obtains its child through `AgentRegistry::createChild` (not the
bare `registry_.create(options_)` it calls today, `src/agent/subagent.cpp:22`),
so depth, fan-out admission, and composition are enforced on the one-shot path
too (55-D8; 55-medium).

No code may read a child's log to decide its outcome before settlement. This is
the precondition for every other decision in this spec.

### 55-D11 — Parallelism and scheduling

- **Foreground children block their caller.** Each occupies one tool-scheduling
  slot. Multiple `ParallelSafe` delegation calls in one assistant message run
  on `std::async` threads bounded by `max_parallel_tool_calls` (default `10`,
  §2.6(3)). The delegation tool is declared `ParallelSafe` (dsh's
  `isConcurrencySafe: () => true`), so independent delegations started together
  overlap.
- **Background/continuable children run asynchronously through a
  `SessionActivator` seam** (interface pinned in §4; injected into
  `SubagentService`, 55-D13) that
  submits to `TurnExecutor` (`max_workers` = `4`, queue `4`). The parent's turn
  does **not** block for them. The seam must **not** call
  `HostRuntime::activateSession`: that path mutates `active_session_`
  (`src/host/host_runtime.cpp:945`) — the daemon's active/focused marker that
  also blocks deletion (`:898`) — so a background child would become
  "active"/undeletable and change `host.info`. The child-activation path is a
  dedicated submit that never sets `active_session_` (55-H9, 55-medium).
- **A full executor queue is a transient admission failure surfaced as an
  errored result, and the child is disposed.** `TurnExecutor::submit` returning
  `false` (surfaced today as `InboxFull`, `src/host/host_runtime.cpp:941-944`)
  triggers the same compensating rollback as any `createChild` failure (55-D8):
  dispose the agent and abandon the session, so no orphan remains. Rev 1 claimed
  "the child session is finalized; no orphan" without specifying the disposal;
  this pins it (55-F13).
- **The fan-out cap (55-D7) is the admission bound**; `TurnExecutor` and
  `LLMPool` are backpressure. A child may be resident but blocked waiting for
  an executor worker or an LLM slot — that is intended (bounded queueing), not
  a correctness bug.
- **Thread-safety.** `SubagentService` state is confined to the daemon's
  executor/session-append discipline: job registration and the parent
  `SubagentFanIn` append happen on the settling thread; `AgentRegistry` map
  access uses its existing `mutex_` rules (`24-D18`); no registry mutex is held
  across a call into `AgentLoop`/`Session`/`SessionManager`.

### 55-D12 — New shared types (additive)

```cpp
// include/ymh/agent/subagent_types.hpp (new)
struct ChildRoute {                       // the resolved per-child route
    std::string                endpoint;  // 54-D2 endpoint name; "" = anonymous default
    std::optional<std::string> profile_id;// 54 profile id (55-H4: durable)
    std::string                model;     // wire id
    std::string                model_name;// llm.models entry name; "" for a literal
    std::optional<std::string> reasoning_effort;
    std::optional<std::uint32_t> max_tokens;
};

struct ChildAgentOptions {                // DelegationToolConfig::agent_options
    std::optional<std::string>   endpoint;
    std::optional<std::string>   profile_id; // 55-H4
    std::optional<std::string>   model;
    std::optional<std::string>   reasoning_effort;
    std::optional<std::uint32_t> max_tokens;
};

struct DelegationToolConfig {
    std::string                   provider;               // provider name on ctx.subagents
    std::string                   tool_name = "subagent"; // distinct per instance
    enum class BackgroundMode : std::uint8_t { OneShot, Continuable };
    BackgroundMode                background_mode = BackgroundMode::OneShot;
    bool                          enable_run_in_background = true;
    bool                          model_selection = false; // exposes route fields + list tool
    std::optional<ChildAgentOptions> agent_options;
    std::optional<std::string>    persona;
    std::optional<ToolRestriction> tool_filter;
    // No maxDepth: presets.max_depth is the single source (42-D18, 55-D7).
};
```

```cpp
// include/ymh/session/session_manager.hpp — additive to SessionOptions
std::optional<std::string>   endpoint;          // 55-D6 child route (endpoint name)
std::optional<std::string>   profile_id;        // 55-D6 child route (54 profile id)
std::optional<std::string>   reasoning_effort;  // 55-D6 child route
std::optional<std::uint32_t> max_tokens;        // 55-D6 child route

// include/ymh/session/session.hpp — additive to SessionHeader (55-H4; 55-A7)
// Persisted so ModelSelectionController::durable_ rebuilds the exact route.
std::optional<std::string>   endpoint;
std::optional<std::string>   profile_id;
```

`SessionManager::createSession` must copy the route fields into `SessionHeader`
(not only `model`/`model_name` as today, `src/session/session_manager.cpp:52-70`),
and `ModelSelectionController::durable_` must prefer `(endpoint, profile_id)` over
`ModelCatalog::find(model_name)` (`src/agent/model_selection.cpp:167-176`) so an
unlisted model no longer silently reverts and a literal id shared by two
endpoints no longer rebuilds the wrong route (55-H4).

```cpp
// include/ymh/agent/provenance.hpp — additive to MessageSource (codec in 37)
std::string sender;   // Kind::Plugin relay: the sender's SessionId; "" otherwise
```

`ContextForm::Relay` already exists (`include/ymh/agent/provenance.hpp:46`);
only the `sender` field and its codec are added, omitted when empty (byte
identity for existing records).

### 55-D13 — `SubagentService` (the ymh `SubagentRuntime`)

```cpp
// include/ymh/agent/subagent_service.hpp (new)
class SubagentService {
public:
    struct StartRequest {
        AgentId                     parent;       // resolved from the tool context
        std::string                 label;        // = args.description
        std::string                 prompt;
        std::optional<ChildAgentOptions> agent_options;  // per-call route override
        std::optional<ChildComposition>  composition;    // persona / tool_filter
        bool                        run_in_background = false;
    };

    // one-shot: fresh child; returns the terminal text (foreground) or a job id
    // (background). continuable: returns the durable childId.
    Task<StartResult> startOneShot(StartRequest);
    Task<StartResult> startContinuable(StartRequest);

    // send_message / interrupt_agent / list_agents backends.
    Task<SendResult>  sendMessage(const SessionId& sender, const SessionId& target,
                                  const std::string& message);
    Task<void>        interrupt(const SessionId& sender, const SessionId& target);
    std::vector<ChildDescriptor> list(const SessionId& caller, ListScope scope) const;

    // settlement: append SubagentFanIn to the parent, resolve the activation job,
    // and return the combined result+disposal outcome so a disposal failure can
    // never mask the real error (55-D9). Never `void`. Disposal is mode-scoped
    // (55-M3): a one-shot child is disposed; a continuable child releases its
    // fan-out slot and stays resident (disposed only on explicit teardown).
    // `notice_expected` is `true` iff the epoch was background-registered —
    // i.e. iff it was not opened by a foreground-blocking activation, so a
    // `send_message`-opened epoch is `true` (the service sets it from its own
    // epoch record written at epoch open, 55-D4 step 1).
    // There is deliberately NO notice-source parameter: the plugin lives on the
    // registered job's `notice_plugin`, and the dsh text is supplied at
    // settlement as `JobOutcome::notice_text` (55-A12; 55-R5-H2).
    std::expected<SettlementResult, AgentError>
        settle(const SessionId& parent, const SessionId& child,
               payload::SubagentOutcome outcome, std::string summary,
               bool notice_expected);

    // 55-D4: replay rule — deliver one notice per unreported **background**
    // (non-foreground) SubagentFanIn (`notice_expected == true`); the plugin is reconstructed
    // from the fan-in as `subagent-settlement:<child>#<ordinal>` (55-A12).
    void replayUnreportedSettlements(const SessionId& parent);
};
```

`SubagentService` is constructed by `WorkspaceRuntime` alongside the roster and
the job registry. Its injected dependencies are:

- `AgentRegistry&` — admission (`createChild`/`liveSubagentCount`) and disposal;
- `SessionManager&` — session creation/read and the settlement-replay scan;
- `AgentPresetRoster*` — composition;
- `JobRegistry&` + `JobWakeupPolicy&` — the (first-wired) job subsystem; the
  service registers each background-registered (non-foreground) epoch's job with
  `notice_plugin =
  "subagent-settlement:<child>#<existing_count+1>"` (55-A12; the ordinal is the
  1-based position this epoch's fan-in will occupy, 55-D4 step 1) and supplies
  the rendered dsh text at settlement via `JobOutcome::notice_text`, so the
  policy's fast-path notice carries both the durable correlation and the
  correct text;
- `ModelSelectionController&` **plus a `RouteCatalog&`** (the route-catalog /
  routability seam; §4) — D6 step 5 calls `effective(parent_session)` and step 4
  needs the effective route; without this injection `SubagentService` cannot
  implement its own D6 (55-H6). The same `RouteCatalog&` backs
  `list_subagent_models` (§4 factory signature);
- `LlmRuntime&` — the public `preflight_route` route preflight (55-A10, 55-R3-H1);
- `SessionActivator&` — the background-activation seam (55-D11); it submits to
  `TurnExecutor` without setting `HostRuntime::active_session_` (55-H9). It is
  injected rather than reaching through `HostRuntime`, because `HostRuntime` is
  constructed **with** `WorkspaceRuntime`
  (`include/ymh/host/host_runtime.hpp:96`) while `SubagentService` is constructed
  **by** `WorkspaceRuntime` — injecting `HostRuntime` would be circular;
- `EventBus&` — the live UI.

`ResourceGovernor&` is **not** injected: the fan-out cap lives on `AgentRegistry`
(55-D7/D8) and nothing in the service reads the governor (55-low).

The model-facing tools are thin adapters over it (dsh's "thin adapters over
`ctx.subagents`"). `SubagentRunner` is retained as the one-shot run primitive
and fixed per 55-D10; **the one-shot path must create through `createChild`**
(see 55-D8/55-medium) rather than calling `registry_.create` directly as it does
today (`src/agent/subagent.cpp:22`).

---

## 4. C++ interface sketches (pinned)

```cpp
// ── include/ymh/agent/agent.hpp ─────────────────────────────────────────────
class Agent {
    // … existing …
    // 55-D10: never fires synchronously during registration; first-wins.
    virtual void onSettled(std::function<void()> callback) = 0;
};

// ── include/ymh/agent/agent_registry.hpp ────────────────────────────────────
struct ChildSpawnRequest { /* §55-D8 */ };
class AgentRegistry {
    // … existing create/resume/findShared/getShared/dispose/finalizeAll …
    // 55-H8: these are MEMBERS (D8's `AgentRegistry::` scope), not free
    // functions — a free `liveSubagentCount() const` is ill-formed.
    [[nodiscard]] std::expected<AgentId, AgentError>
    createChild(const ChildSpawnRequest& request);
    [[nodiscard]] std::size_t liveSubagentCount() const noexcept;
};

// ── include/ymh/execution/resource_governor.hpp ─────────────────────────────
struct ResourceCaps {
    // … existing …
    std::size_t max_live_subagents{8};   // 55-D7
};

// ── include/ymh/session/events.hpp — additive to SubagentFanIn (55-A11) ─────
struct SubagentFanIn {
    SessionId       subagent;
    SubagentOutcome outcome = SubagentOutcome::Completed;
    std::string     summary;
    bool            notice_expected = false;   // true iff background-registered; false only for a foreground-blocking epoch (55-D4)
};

// ── include/ymh/jobs/job_registry.hpp + job_wakeup.hpp — additive (55-A12) ──
// A per-job notice source on BOTH JobStart and JobSnapshot. When set,
// JobWakeupPolicy stamps `MessageSource::Kind::Plugin` + `plugin = *notice_plugin`
// on the notice it builds (the inject path AND the followup/wake path). Absent
// (nullopt) reproduces today's empty-plugin behaviour byte-for-byte.
struct JobStart {
    // … existing fields (kind, label, output_limit_bytes, owner, run) …
    std::optional<std::string> notice_plugin;   // per-job correlation source
    std::optional<std::string> notice_text;     // registration-time base only
};
struct JobSnapshot {
    // … existing fields (id, kind, label, output_limit_bytes, owner, status,
    //   detail, started_at, finished_at, reported) …
    std::optional<std::string> notice_plugin;   // copied from JobStart by `start`
    // A per-snapshot text override. When set, `deliver` uses `*notice_text` in
    // place of the file-local generic `notice_text(snapshot)` (src/jobs/
    // job_wakeup.cpp:15-22). The subagent text embeds the child's closing
    // message, known only at settlement, so the service supplies it on the
    // terminal `JobOutcome` and `JobRegistry::settle` copies it onto the
    // settled snapshot; `JobStart::notice_text` exists for symmetry.
    std::optional<std::string> notice_text;
};
struct JobOutcome {
    // … existing fields (status, detail, output) …
    // Settlement-supplied dsh text; `JobRegistry::settle` copies it onto the
    // settled `JobSnapshot::notice_text`.
    std::optional<std::string> notice_text;
};

// ── include/ymh/agent/subagent_service.hpp (new) ────────────────────────────
enum class ListScope : std::uint8_t { Children, Descendants };
struct ChildDescriptor {
    SessionId                child;
    std::optional<SessionId> parent;     // durable direct parent
    std::uint32_t            depth = 0;
    std::string              label;      // session title
    enum class Status : std::uint8_t { Running, Idle, Ready } status = Status::Ready;
    std::optional<std::string> diagnostic;
};
struct SendResult { MessageId message_id; };              // acceptance only (MessageId = std::string)
struct SettlementResult {                                 // 55-D13: result + disposal aggregate
    payload::SubagentOutcome outcome = payload::SubagentOutcome::Completed;
    std::optional<std::string> disposal_error;            // never masks `outcome`
};
struct StartResult {
    enum class Kind : std::uint8_t { Foreground, BackgroundJob, Continuable };
    Kind                       kind = Kind::Foreground;
    std::string                text;       // foreground final text
    std::string                job_id;     // Kind::BackgroundJob
    SessionId                  child;      // Kind::Continuable
};
class SubagentService { /* §55-D13 */ };

// ── include/ymh/llm/llm_runtime.hpp — additive (55-A10; 55-R3-H1) ───────────
class LlmRuntime {
    // … existing public prepare_call / stream / list_providers …
    // 55-R3-H1: public route preflight. Resolves the effective route through the
    // private `resolve_adapter` WITHOUT binding a call generation; throws
    // `NoProviderRouteError` on a miss. Used by SubagentService D6 step 4.
    void preflight_route(const LlmCallConfig& config) const;
};

// ── include/ymh/agent/subagent_types.hpp (new) — 55-D6/D13 ──────────────────
// The route-catalog / routability seam. It backs both the D6 step-4 model/effort
// validation and `list_subagent_models`. The live implementation composes
// `ModelCatalog` with `LlmRuntime`'s registered routes. This is the accessor
// 55-D13 injects; `is_routable_endpoint` is a member here (before this revision
// it was invoked but never declared — 55-M4 / the Rev-3 sweep).
class RouteCatalog {
public:
    virtual ~RouteCatalog() = default;
    // Endpoint names that currently resolve to a registered route ("" = the
    // anonymous default endpoint). `list_subagent_models` with no arguments.
    [[nodiscard]] virtual std::vector<std::string> routable_endpoints() const = 0;
    [[nodiscard]] virtual bool is_routable_endpoint(std::string_view endpoint) const = 0;
    // `llm.models` entry names advertised by an endpoint.
    [[nodiscard]] virtual std::vector<std::string> models_for(std::string_view endpoint) const = 0;
    // Accepted `reasoning_effort` values for an endpoint+model (empty = free-form).
    [[nodiscard]] virtual std::vector<std::string> efforts_for(std::string_view endpoint,
                                                              std::string_view model) const = 0;
    // Catalog membership of a wire model id under an endpoint (D6 validation).
    [[nodiscard]] virtual bool is_catalog_member(std::string_view endpoint,
                                                 std::string_view model) const = 0;
};

// ── include/ymh/agent/session_activator.hpp (new) — 55-D11/D13 ──────────────
// The background-activation seam. `SubagentService` submits a child activation
// through this seam; the live implementation forwards to `TurnExecutor::submit`
// WITHOUT setting `HostRuntime::active_session_` (55-H9). It is injected rather
// than reaching through `HostRuntime`, which would be circular (`HostRuntime` is
// constructed WITH `WorkspaceRuntime`, which constructs `SubagentService`).
class SessionActivator {
public:
    virtual ~SessionActivator() = default;
    // Submit one activation for `child`. Returns `false` (surfaced as
    // `InboxFull`) when the executor queue is full; the caller then rolls the
    // child back (55-F13). The activation runs asynchronously; settlement is the
    // drain epoch (55-D4), observed separately from this submit.
    [[nodiscard]] virtual bool submit(const SessionId& child) = 0;
};

// ── include/ymh/agent/subagent.hpp — existing class, pinned (55-D10; 55-M6) ─
class SubagentRunner {
public:
    // Existing ctor, unchanged (pinned so the three existing tests can be
    // assessed; `tests/unit/agent_registry_test.cpp:299`, `:327`, `:365`).
    SubagentRunner(AgentRegistry& registry, SessionManager& sessions,
                   Session& parent, SessionOptions options);
    // Rewritten per 55-D10: creates the child through
    // `AgentRegistry::createChild` (55-D8), starts the activation, and awaits
    // settlement; never reads an incomplete log (55-I5). The existing
    // synchronous signature/return are retained (55-M6).
    payload::SubagentOutcome run(const std::string& task, std::string& summary);
};

// ── include/ymh/tools/subagent_tools.hpp (new) ──────────────────────────────
// The caller resolver mirrors JobOwnerResolver (ToolContext carries no agent id).
using SubagentCallerResolver = std::function<std::optional<SessionId>(const ToolContext&)>;
[[nodiscard]] std::unique_ptr<Tool> make_subagent_tool(SubagentService&,
                                                       SubagentCallerResolver,
                                                       DelegationToolConfig);
[[nodiscard]] std::unique_ptr<Tool> make_send_message_tool(SubagentService&, SubagentCallerResolver);
[[nodiscard]] std::unique_ptr<Tool> make_interrupt_agent_tool(SubagentService&, SubagentCallerResolver);
[[nodiscard]] std::unique_ptr<Tool> make_list_agents_tool(SubagentService&, SubagentCallerResolver);
[[nodiscard]] std::unique_ptr<Tool> make_list_subagent_models_tool(const RouteCatalog&);

// ── the tool schema (one-shot foreground shape) ─────────────────────────────
// name: "subagent"  concurrency: ParallelSafe  destructive: false
// input_schema: {
//   description: string (required),
//   prompt:      string (required),
//   run_in_background: boolean (optional; one-shot default false,
//                                continuable default true),
//   provider:    string (optional; endpoint name; requires model),
//   model:       string (optional; requires provider),
//   reasoning_effort: string (optional)
// }
// execute: resolve caller -> SubagentService::startOneShot/startContinuable ->
//   Foreground: { out = text }            (Ok)
//   BackgroundJob: { out = "started background subagent job " + job_id }
//   Continuable:   { out = "started subagent " + child.value }
//   failure:       { outcome = Error|Cancelled, out = headline + diagnostic +
//                    labelled partial output }   (55-D9)
```

`list_agents` returns one line per child (`<child> parent=<parent> depth=<n>
status=<status> <label>`), `list_subagent_models` returns the endpoint/model/
effort lines of 55-D6.

**Header dependencies (declaration-before-use, Rev-3 sweep).**
`subagent_types.hpp` (the `ChildRoute`/`ChildAgentOptions`/`RouteCatalog` types,
55-D12/§4) is included by `agent_registry.hpp` (for `ChildSpawnRequest`),
`subagent_service.hpp`, and `subagent_tools.hpp`; `session_activator.hpp` is
included by `subagent_service.hpp`. `LlmRuntime::preflight_route` is declared in
`llm_runtime.hpp` before any caller. This keeps every pinned use declared before
use across the new headers.

---

## 5. Invariants (55-I)

| ID | Invariant |
|---|---|
| 55-I1 | Every delegation is a `one-shot` or `continuable` instance of a `DelegationToolConfig`; the mode is never inferred from a call. |
| 55-I2 | `run_in_background` resolution is exactly `request ?? (mode == continuable)`; a forced `true` under `enable_run_in_background == false` is an errored result. |
| 55-I3 | A one-shot child is fresh: its session has `parentSession` set, `SessionKind::Subagent`, and no inherited conversation. |
| 55-I4 | A continuable child is a resident `AgentLoop` plus a durable session in the same daemon; its `childId` is its `SessionId` and is stable across restarts. |
| 55-I5 | No code reads a child's log to decide its outcome before settlement (55-D10). |
| 55-I6 | `onSettled` fires exactly once; delivery is **always posted/deferred** (never synchronously inside `onSettled`, even when the agent is idle); it never fires after `dispose()`. |
| 55-I7 | Each **background-registered residency epoch** — every non-foreground epoch: a background start *or* a `send_message` that opens a new epoch — appends exactly one `SubagentFanIn` (`notice_expected == true`) to its parent and yields **at-least-once** delivery of one correlated notice; the restart/crash/non-resident path delivers on the **next parent resume / daemon start** (if the parent is never resumed, delivery is zero). An epoch opened by a `continuable` **foreground** activation appends `SubagentFanIn` with `notice_expected == false` and yields **no** notice (replay never touches it). |
| 55-I8 | A non-`Completed` child always yields an errored/cancelled tool result, never a success (55-D9). |
| 55-I9 | Depth is checked per attempted start against `presets.max_depth`; the tool stays visible at the cap; `max_depth == 0` forbids delegation. |
| 55-I10 | The live delegated-child count never exceeds `ResourceCaps::max_live_subagents`; a rejected start creates no session and no agent. |
| 55-I11 | When a roster is present, `apply_child_composition` runs exactly once per child, after registration and before the child's first request; with no roster it runs zero times (never a half-composed child). |
| 55-I12 | A child's tool list is the leaf-scoped prompt assembly; a `tool_filter` can only narrow, never widen (deny wins). |
| 55-I13 | A call supplies `provider` and `model` together; a half route is an errored result. |
| 55-I14 | When no configured/call layer selects a route, the child inherits the parent's **effective** `ModelSelection`; the parent's pending mid-flight switch is respected exactly as `buildRequest` does. |
| 55-I15 | Changing the effective route without an explicit `reasoning_effort` clears the inherited route-owned effort. |
| 55-I16 | Route preflight (`LlmRuntime::preflight_route`) runs before child creation and validates only the `(endpoint, profile_id)` route; `model`/`reasoning_effort` are validated against the catalog/adapter inside `AgentRegistry::createChild` before any session is created. A failed validation is an errored result and creates no child; catalog membership is **required**. |
| 55-I17 | A child's durable route is written at creation and persisted as the `(endpoint, profile_id)` pair on `SessionHeader` (the header already carries `model`/`model_name`); `ModelSelectionController::durable_` prefers the pair and rebuilds the same `ModelSelection` on every request and on resume. |
| 55-I18 | `send_message` returns acceptance only (a stable `messageId`), never a reply; a rejection is an errored result. |
| 55-I19 | `send_message` delivery is Steer when the target is busy, a new turn when idle, and resume-then-deliver when cold. |
| 55-I20 | `send_message`/`interrupt_agent` authorize only a direct parent↔child edge; every other caller gets an errored result. |
| 55-I21 | `interrupt_agent` stops only the current turn; queued messages and descendants are untouched; an idle target is an accepted no-op. |
| 55-I22 | `list_agents` includes only continuable children, reports the durable direct parent and depth, and never aborts on an unreadable candidate. |
| 55-I23 | Foreground delegation — resolved `run_in_background == false`, **including a continuable child started foreground** — blocks its caller; a child whose resolved flag is `true` never blocks the parent turn. |
| 55-I24 | The delegation tool is `ParallelSafe`; independent delegations started in one step overlap up to `max_parallel_tool_calls`. |
| 55-I25 | Background activation goes through the injected `SessionActivator` seam (which submits to `TurnExecutor`); it never runs inline on the parent's worker and never sets `HostRuntime::active_session_`. |
| 55-I26 | No registry mutex is held across a call into `AgentLoop`/`Session`/`SessionManager`; `SubagentService` obeys `24-D18`. |
| 55-I27 | A daemon restart preserves the child as a stored, resumable session and the durable parent edge; an interrupted activation yields a terminal event and a notice, never a phantom completion. |
| 55-I28 | `kDelegationScopeStatement` is registered verbatim for every delegated child; no child can widen its scope. |
| 55-I29 | No new `EventType`, no new wire notification; the durable carriers are `SubagentSpawned`/`SubagentFanIn` and the job-wakeup message. The only schema changes are additive codec fields on existing carriers — `MessageSource::sender`, `SubagentFanIn::notice_expected`, `JobStart`/`JobSnapshot::notice_plugin`, `JobSnapshot`/`JobOutcome::notice_text`, and `finish_reason` — each omitted when default and decoded absent as default (55-A6/A11/A12/A13). |
| 55-I30 | `presets.max_depth` is the single depth source; no delegation-tool config carries a second `maxDepth`, and `ChildSpawnRequest::max_depth` is supplied by the caller (no literal default). |
| 55-I31 | The durable unreported-settlement record is the parent-log `SubagentFanIn` with `notice_expected == true` (a **background** fan-in, i.e. any non-foreground epoch — including one opened by `send_message`); on parent resume and on daemon start the service delivers one notice per unreported background fan-in (replay), so the notice survives a crash before drain. Fan-ins from an epoch opened by a `continuable` **foreground** activation (`notice_expected == false`) are never replayed. |
| 55-I32 | An activation is a residency epoch: it settles only when the child is idle **and** its inbox is empty **and** it owns no live children; `send_message` extends an open epoch (keeping its opener's registration) and opens a new **background-registered** epoch — hence a new notice — only after it closes. |
| 55-I33 | Fan-out admission is an atomic reservation taken before `createSession` and rolled back on any failure; two concurrent `createChild` calls can never both admit at `N-1`. |
| 55-I34 | A killed background child yields a `Cancelled` notice; the settlement path does not pre-mark the job `reported` on kill. |
| 55-I35 | The foreground settlement await has a failure/timeout/disposal path and never hangs the caller when `send`/`enqueue` rejects or the child never settles. |
| 55-I36 | A continuable child releases its fan-out slot when its epoch closes and re-reserves one before it resumes; no set of idle-resident continuable children can permanently starve delegation. |

---

## 6. Failure modes (55-F)

Shared findings `F1`..`F12` (`00` §54) apply. Component-local:

| ID | Failure | Detection | Handling |
|---|---|---|---|
| 55-F1 | Settlement race (the §2.2 defect) | a child outcome read before its terminal event | 55-D10 removes the read-before-settle path; `onSettled` gates all outcome reads (test 55-U2) |
| 55-F2 | Depth exceeded | `check_delegation_depth` returns `DelegationDepthExceeded` | errored result; no child created |
| 55-F3 | Fan-out cap exceeded | `liveSubagentCount() >= max_live_subagents` at admission | errored result; no session, no agent |
| 55-F4 | Half route (`provider` xor `model`) | tool-boundary validation | errored result; no child created (test 55-U6) |
| 55-F5 | Unknown/unroutable route | `LlmRuntime::preflight_route` throws `NoProviderRouteError` | errored result; no child created |
| 55-F6 | Route change without effort | precedence step 3 | inherited route-owned effort cleared (test 55-U6) |
| 55-F7 | Child fails/cancels/token-limits/refuses | terminal `TurnFailed`/`TurnCancelled`, or `TurnEnded` with persisted `finish_reason == Length`/`ContentFilter` (55-D9; the length/refusal rows are not derivable until `finish_reason` is persisted) | errored/cancelled result with headline + diagnostic + labelled partial output |
| 55-F8 | Child disposal fails during settlement | aggregate of result + disposal (`SettlementResult.disposal_error`) | the result failure is never masked |
| 55-F9 | `send_message` to unknown/foreign/stale/**disposed**/descriptor-less target, or **unavailable parent** | edge check / registry lookup | errored result stating not delivered |
| 55-F10 | `send_message` inbox full | `InboxResult::InboxFull` | errored result; message not delivered |
| 55-F11 | `send_message` to a descriptor-less child, or a cold-resume failure (`UnknownSession`/`StoreError`) | no resumable header / resume throws | errored result; message not delivered |
| 55-F12 | `interrupt_agent` to self/sibling/non-ancestor/unknown/**disposed**/descriptor-less target | edge check | errored result |
| 55-F13 | Executor queue full on background start | `SessionActivator` submit returns `false` (`InboxFull`) | errored result; the child is disposed and the session abandoned (compensating rollback, 55-D8), no orphan |
| 55-F14 | Fan-out/live count race | atomic reservation under `AgentRegistry::mutex_` (taken before `createSession`) | exactly one of two concurrent starts wins; the other is an errored result with the reservation rolled back |
| 55-F15 | Child killed by daemon shutdown mid-activation | teardown terminal event (`24-D10`) | notice reports `killed`/`cancelled`; never `completed` |
| 55-F16 | `apply_child_composition` throws (`UnknownAgent`) | roster leaf missing | errored result; child disposed (no half-composed child) |
| 55-F17 | Route preflight passes, adapter rejects at request time | first-request `ProviderFailed` | child turn fails; surfaced as a failed result/notice |
| 55-F18 | Unreadable child in `list_agents` | store/registry read failure | diagnostic line; listing continues |
| 55-F19 | Settlement notice rejected by a full parent inbox, or owner not resident | `InboxResult::InboxFull` on `followup`, or `on_job_done` early-return (`job_wakeup.cpp:97-106`) | falls back to `inject`; if that is rejected or the owner is absent, the durable **background** `SubagentFanIn` (`notice_expected == true`; any non-foreground epoch, including one opened by `send_message`) remains and the **replay rule** (55-D4) delivers one correlated notice per unreported background fan-in on the next parent resume / daemon start (idempotent; replay may re-deliver) |
| 55-F20 | Duplicate settlement (late producer) | first-wins job settlement + `subagent-settlement:<child>#<ordinal>` correlation | one `SubagentFanIn`; replay may re-deliver the same correlated notice (idempotent, at-least-once) |
| 55-F21 | Child never terminates / disposed before settle | foreground settlement timeout, or `AgentDisposed` on the await | the await resolves with a timeout/disposal errored result; the caller never hangs (55-D10) |
| 55-F22 | Generic child-creation failure | `SessionManager::createSession` throws `StoreUnavailable`/`LeaseLost`, or registration fails | errored result; compensating rollback releases the reservation and abandons the session (55-D8) |

---

## 7. dsh mapping

| dsh concept | ymh equivalent | Notes |
|---|---|---|
| `SubagentRuntime` (`start`, `startContinuable`, `sendMessage`, `interrupt`, `listChildren`, `listDescendants`) | `SubagentService` (55-D13) | thin tool adapters over the service |
| `dsh-tool-subagent` (`provider`, `toolName`, `backgroundMode`, `enableRunInBackground`, `agentOptions`, `persona`, `toolFilter`, `maxDepth`) | `DelegationToolConfig` (55-D12) + `make_subagent_tool` | `maxDepth` is **not** on the tool: `presets.max_depth` (42-D18) |
| `dsh-tool-subagent-control` root (`send_message`, `interrupt_agent`) | the two control tools (55-D2, 55-D5) | `ContextForm::Relay` already exists |
| `dsh-tool-subagent-control/list-agents` (separately loadable plugin) | `list_agents` (55-D2, 55-D5) | Rev 1 mis-attributed `list_agents` to the control root; dsh loads it separately |
| `run_in_background` + `job_output`/`job_kill` | `JobRegistry` + the job tools | one-shot background only; **both are wired for the first time here** (`44:87`; 55-H1) |
| settlement notice | `SubagentFanIn` (durable record, `notice_expected == true`) + a `kind == subagent` job notice carrying `notice_plugin` (correlation) and `notice_text` (dsh text) | **at-least-once** via the replay rule, **background-registered (non-foreground) epochs only** — including a `send_message`-opened epoch (55-D4; 55-R4-H1/M1; 55-R5-H2; 55-R6-M1); not exactly-once, and **built**, not reused |
| `provider.agentRouteDefaults` (backend/provider-advertised) → tool config → route-aware effort merge → exact-route preflight | 55-D6 precedence **0–5** | step 0 is the **backend-advertised** base (`index.js:394`, spread at `index.js:496-504`), **not** a parent-inherited value; ymh has no analogue, so step 0 is a **deliberate non-mirror** (55-M7). Step 5 is the parent-effective layer |
| parent's latest logged request / creation options | parent's effective `ModelSelection` (54) | durable, single source |
| `list_subagent_models` | `list_subagent_models` | lists **only routable endpoints** (55-D6, 55-H5); not "advisory" |
| `modelSelectionSettings` (host session-recorded allow-list, inherited, unchanged by later edits; gates `list_subagent_models` registration) | a static per-tool `bool model_selection` (55-D12) | **NOT mirrored** (fidelity ledger, 55-D6); coarser, non-inherited, config-time |
| `tool:<toolName>` system-prompt guidance | not mirrored | ymh has `TOOL_SUBAGENT`/`tool:subagent` prompt-ordering slots (`src/prompt/order.cpp:69-70`) but no per-tool guidance emission; recorded as not mirrored |
| `maxDepth` / `depthLimit` capability | `presets.max_depth` + `check_delegation_depth` | already present |
| `one-shot` vs `continuable` | `background_mode` | both implemented |
| `fork` provider (seeded) | not in scope | 55-OQ-4 |
| `acp` / out-of-process providers | not in scope | in-process `spawn` only |
| `isConcurrencySafe: () => true` | `ToolConcurrencyMode::ParallelSafe` | 55-D11 |

---

## 8. Amendment register

| ID | Kind | Amended clause | Verified anchor | New behaviour |
|---|---|---|---|---|
| 55-A1 | **SUPERSEDE** | `06` §7 synchronous-v1 + omission | `docs/design/06-agent-loop.md:937-939`, `:1141-1142` | supersedes the synchronous-v1 decision: delegation gains background + continuable; the settlement is `onSettled`-based |
| 55-A2 | AMEND | `42` §3.3/§3.4/§3.5 | `src/agent/preset.cpp:789`, `:875` | first production call sites for `apply_child_composition`/`check_delegation_depth` |
| 55-A3 | AMEND | `44` §3.3 | `docs/design/44-goals-jobs-commands.md:377-384` | the `subagent` job producer is implemented |
| 55-A4 | AMEND | `06` §7 caps | `docs/design/06-agent-loop.md:943-944` | adds `ResourceCaps::max_live_subagents` |
| 55-A5 | AMEND | `24` §1.3.3 retained rules | `docs/design/24-agent-lifetime-errata.md:285-292` | clarifies that delegated children are activated via the `SessionActivator`/`TurnExecutor` seam and bounded by the fan-out cap, not by widening the one-active-session rule |
| 55-A6 | AMEND | `01`/`37` provenance | `include/ymh/agent/provenance.hpp:150-197` | adds the optional `MessageSource::sender` field + codec (omitted when empty) |
| 55-A7 | AMEND | `02` `SessionOptions`/`SessionHeader` | `include/ymh/session/session_manager.hpp:29-43`; `include/ymh/session/session.hpp:44-66` | adds `endpoint`/`profile_id`/`reasoning_effort`/`max_tokens` to `SessionOptions` **and** persists `(endpoint, profile_id)` on `SessionHeader` (55-H4) |
| 55-A8 | AMEND | `06` §4 `Agent` surface | `include/ymh/agent/agent.hpp:134-155` | adds `onSettled` |
| 55-A9 | AMEND | `54` §3–§5 routing | `docs/design/54-multi-endpoint-routing-errata.md`; `include/ymh/agent/model_selection.hpp:32-39` | layers a per-child route override over the parent's effective `ModelSelection`; the child's durable route is the source of truth |
| 55-A10 | AMEND | `28`/`54` `LlmRuntime` route preflight | `include/ymh/llm/llm_runtime.hpp:237-238` (public `prepare_call`), `:262` (private `resolve_adapter`) | adds a **public** `preflight_route` accessor so `SubagentService` runs the D6 step-4 route preflight without a `friend` (55-R3-H1) |
| 55-A11 | AMEND | `01` §4 subagent events | `include/ymh/session/events.hpp:224-229`; `src/session/events.cpp:635-650` | adds the `SubagentFanIn::notice_expected` bit (default `false`; decodes absent as `false`) so the replay rule scopes to **background-registered** epochs (any non-foreground epoch, including one opened by `send_message`) and never re-notifies a fan-in from a foreground-blocking activation (55-R4-H1) |
| 55-A12 | AMEND | `44` §5.5 job types / §3.5 wakeup policy | `include/ymh/jobs/job_registry.hpp:54-76`; `include/ymh/jobs/job_wakeup.hpp:24-36`; `src/jobs/job_wakeup.cpp:15-22`, `:123-141`; `src/jobs/job_registry.cpp:75-92`, `:168-181` | adds `JobStart`/`JobSnapshot::notice_plugin` **and** `JobSnapshot`/`JobOutcome::notice_text`; `JobWakeupPolicy` stamps the plugin as `MessageSource::plugin` and uses `notice_text` in place of the file-local generic text; `JobRegistry::start` copies `notice_plugin`/`notice_text` into the snapshot and `settle` copies `JobOutcome::notice_text` onto the settled snapshot, so the `subagent-settlement:<child>#<ordinal>` correlation and dsh's text are implementable through the pinned path (55-R4-M1; 55-R5-H2/M3) |
| 55-A13 | AMEND | `01` §4 turn/assistant events | `include/ymh/session/events.hpp:67` (`TurnEnded`); the assistant-message payload; `src/session/events.cpp` (codec) | adds an additive persisted `finish_reason` (on the assistant-message payload or the `TurnEnded` payload; no new `EventType`), omitted when its default and decoded absent as the default, so the length/refusal stop-reason rows of 55-D9 are derivable (55-D9; 55-R5-M2) |

**`43` is a build-on, not an amendment.** `43` (Wave-5 dependency errata) is
already verified and pins the creation-path copy/header codec this spec consumes;
it is listed in §1.3 as a build-on row and deliberately has **no** `55-A*` row
here (55-H13). This table, §1.3, and the header name the same amended specs:
`06` (twice: §7 and §4), `42`, `44` (twice: §3.3 and §5.5), `24`, `01`/`37`
(three times: provenance, subagent events, and the turn/assistant `finish_reason`),
`02`, `54`, `28`.

---

## 9. Test plan (55-U)

Conventions follow §44. `ctest` is **not** parallel-safe and must not run during
a build; run the suite only after the build completes.

### 9.1 Unit

| ID | Test | Asserts |
|---|---|---|
| 55-U1 | `onSettled` semantics | fires exactly once; **always posted/deferred** (never synchronously, even when already terminal at registration); fires after a terminal event; does not fire after `dispose()`; the pre-start registration case (idle, no terminal event) does not fire early (55-I5/I6, 55-D10). |
| 55-U2 | `SubagentRunner` settlement | the rewritten runner waits for settlement and never reads an incomplete log; a slow child yields `Completed` only after `TurnEnded`; a still-running child is never reported `Completed` (the §2.2 regression; 55-F1). |
| 55-U3 | `run_in_background` resolution | `{one-shot,continuable} × {omitted,true,false} × enable_run_in_background` truth table (55-I2). |
| 55-U4 | Depth | `check_delegation_depth` at/below/above the cap and `max_depth == 0`; the tool stays visible at the cap and the start is an errored result (55-I9, 55-F2). |
| 55-U5 | Fan-out cap + atomic admission | concurrent `createChild` calls never exceed `max_live_subagents` (the reservation closes the TOCTOU); a rejected start creates no session/agent; a continuable child releases its slot at epoch close and re-reserves on resume (55-I10/I33/I36, 55-F3/F14). |
| 55-U6 | Route precedence | configured vs call overlay; half route rejected (55-F4); route change clears inherited effort (55-F6); explicit effort wins; parent-effective default (55-I13–I15, 55-F4/F6). |
| 55-U7 | Route preflight | an unroutable endpoint/profile_id fails before child creation; a **bogus model/effort** fails validation before child creation (the preflight alone does not check them); an unlisted model is rejected (catalog membership required) (55-I16, 55-F5). |
| 55-U8 | `list_subagent_models` | no-arg endpoints; endpoint models; endpoint+model efforts; unknown endpoint rejected; half args rejected. |
| 55-U9 | `send_message` delivery | busy→Steer, idle→followup, cold→resume+deliver; returns acceptance only (`MessageId`); foreign/unknown/self/disposed/descriptor-less rejected; a cold-resume `UnknownSession`/`StoreError` is an errored result (55-I18/I19/I20, 55-F9/F10/F11). |
| 55-U10 | `interrupt_agent` | cancels only the current turn; idle target is an accepted no-op; self/sibling/non-ancestor/unknown/disposed/descriptor-less rejected (55-I21, 55-F12). |
| 55-U11 | `list_agents` | children vs descendants pre-order; durable parent + depth; one-shot children absent; unreadable candidate is a diagnostic (55-I22, 55-F18). |
| 55-U12 | Failure text | each stop reason maps to its headline; diagnostic and partial output are labelled and separate; disposal failure does not mask the result (55-I8, 55-F7/F8). |
| 55-U13 | Composition | with a roster, `apply_child_composition` runs once per child after mount and before the first request; with no roster it runs zero times; persona shadows; tool filter narrows only (55-I11/I12, 55-F16). |
| 55-U14 | `MessageSource::sender` codec | omitted when empty; existing records byte-identical; round-trips when set (55-A6). |
| 55-U15 | Durable route | `SessionOptions` route fields + persisted `SessionHeader` `(endpoint, profile_id)` rebuild the same `ModelSelection` on request and on resume; an unlisted model and a literal id shared by two endpoints do **not** revert/rebind wrongly (55-I17, 55-H4). |

### 9.2 Integration (FakeLLM)

| ID | Test | Asserts |
|---|---|---|
| 55-U16 | one-shot foreground | a parent turn calls `subagent` and receives the child's final text; `SubagentSpawned` + `SubagentFanIn{Completed, notice_expected=false}` in the parent log (a foreground-blocking activation — one of the two `notice_expected == false` cases, the other being a `continuable` foreground start), and **no** settlement notice is emitted; a subsequent replay delivers **nothing** for it (55-I3/I7, 55-R4-H1). |
| 55-U17 | one-shot background | returns `started background subagent job <id>`; `job_output` reads the text; `job_kill` cancels; one wakeup notice (55-D3/D4). |
| 55-U18 | continuable lifecycle | `subagent_continuable` returns `started subagent <id>`; the child is resident; after the first epoch closes, `send_message` opens a new **background-registered** epoch (`notice_expected == true`); two fan-ins, two notices (55-I4/I7/I32). |
| 55-U19 | restart | after a simulated daemon restart the child is a stored session; `send_message` resumes it; an activation interrupted by shutdown reports `killed`/`cancelled`, never `completed` (55-I27, 55-F15). |
| 55-U20 | parallelism | two `ParallelSafe` delegations in one step overlap up to `max_parallel_tool_calls`; a background child never blocks the parent turn (55-I23/I24). |
| 55-U21 | per-child route | a child created with an explicit route issues its provider call on that route; a child with none uses the parent's effective route (55-I14). |
| 55-U22 | depth end-to-end | a depth-`max_depth` child's `subagent` call is refused with an errored result (55-I9). |
| 55-U23 | fan-out end-to-end | `max_live_subagents` concurrent children admitted; the next is refused (55-I10). |
| 55-U24 | no new wire surface | `kProtocolVersion` unchanged; the event stream carries no new type; the notice arrives as a plugin `Notice` message (55-I29). |
| 55-U28 | settlement replay | a parent log with a **background** `SubagentFanIn` (`notice_expected=true`) and no delivered notice (owner not resident) yields exactly one correlated notice on resume/daemon start, whose `subagent-settlement:<child>#<ordinal>` ordinal equals the registration-time `existing_count + 1` (55-D4 step 1/step 3); the same holds for a `send_message`-opened epoch's fan-in; replaying again is idempotent; a `continuable`-foreground `SubagentFanIn` (`notice_expected=false`) in the same log is **never** replayed (55-I7/I31, 55-F19/F20, 55-R4-H1, 55-R5-H1). |
| 55-U29 | activation epoch | a steered/queued message extends the open epoch → for a **background-opened** epoch, one notice after drain and no premature notice, while an epoch opened by a `continuable` **foreground** start keeps its opener's registration and produces **no** notice when extended (55-D4); a `send_message` after the epoch closes opens a new **background-registered** epoch → a second notice that replay also honors (`notice_expected=true`) (55-I7/I32, 55-D4). |
| 55-U30 | killed child | killing a background child yields a `Cancelled` notice, never silence (55-I34, 55-F15). |
| 55-U31 | settlement timeout/disposal | a child that never settles and a child disposed before settle both resolve the foreground await with an errored result; the caller never hangs (55-F21, 55-I35). |
| 55-U32 | generic create failure | `createSession` throwing `StoreUnavailable`/`LeaseLost` (or a registration failure) yields an errored result, rolls back the reservation, and leaves no session/agent (55-F22, 55-I33). |
| 55-U33 | notice text + Quiet | the `kind == subagent` notice uses dsh's text including the closing message, rendered through the pinned `JobSnapshot::notice_text`/`JobOutcome::notice_text` seam (55-A12); `CompletionDelivery::Quiet` does not suppress it (55-D4; 55-R5-H2). |
| 55-U34 | executor queue full (background start) | a full `TurnExecutor` queue makes `SessionActivator::submit` return `false`; the start is an errored result, the child is disposed, the session abandoned, and no orphan remains (55-F13, 55-D8/D11). |
| 55-U35 | preflight passes, adapter rejects at request time | a route that passes the D6 preflight but whose adapter rejects the first request yields a failed child turn, surfaced as an errored result/notice — never a phantom success (55-F17). |
| 55-U36 | delegation mode is per-instance | the mode is read from `DelegationToolConfig::background_mode` and never inferred from a call; two instances with distinct `toolName`s can mount both modes at once (55-I1, 55-D1/D2). |
| 55-U37 | activation seam | a background start submits through `SessionActivator` and never runs inline on the parent's worker and never sets `HostRuntime::active_session_` (55-I25, 55-D11). |
| 55-U38 | registry-mutex discipline | no registry mutex is held across a call into `AgentLoop`/`Session`/`SessionManager` during delegation (55-I26, 24-D18). |
| 55-U39 | fixed delegation scope | `kDelegationScopeStatement` is registered verbatim for every delegated child; a child cannot widen its scope (55-I28, 55-D8). |
| 55-U40 | single depth source | `presets.max_depth` is the only depth source; no delegation-tool config carries a second `maxDepth`; `ChildSpawnRequest::max_depth` is supplied by the caller (55-I30, 42-D18). |

### 9.3 PTY / live (opt-in)

| ID | Test | Asserts |
|---|---|---|
| 55-U25 | real DeepSeek one-shot | `YMH_LIVE_LLM=1` drives a real foreground delegation and returns the child's text. |
| 55-U26 | real continuable | a background continuable child settles with one notice and accepts `send_message`; the TUI renders the notice and the child session. |
| 55-U27 | route override | a child on a second endpoint/model completes against the real provider. |

### 9.4 Test-running note

`ctest` is **not** parallel-safe and must not run during a build. The
`onSettled`/settlement tests are timing-sensitive and must use the deterministic
fake clock the loop already injects (`include/ymh/agent/agent_loop.hpp:91`, the
`StreamClock` alias),
never a wall-clock sleep.

### 9.5 Existing tests that must change (55-H11)

Adding the pure virtual `Agent::onSettled` breaks **five** existing test
doubles. Each `FakeAgent final : public Agent` must gain an `onSettled`
override (a no-op or a stored callback):

| File | Class | `whenIdle` override today |
|---|---|---|
| `tests/unit/goal_test.cpp` | `FakeAgent` (`:38`) | `:59` |
| `tests/unit/job_test.cpp` | `FakeAgent` (`:30`) | `:55` |
| `tests/unit/command_test.cpp` | `FakeAgent` (`:31`) | `:48` |
| `tests/unit/agent_preset_test.cpp` | `FakeAgent` (`:48`) | `:67` |
| `tests/unit/spec52_presets_test.cpp` | `FakeAgent` (`:112`) | `:131` |

**`SubagentRunner` tests (55-M6).** The three existing constructions in
`tests/unit/agent_registry_test.cpp` (`:299`, `:327`, `:365`) assert the
pre-rewrite synchronous path and must be updated for the D10 rewrite (the runner
now creates through `AgentRegistry::createChild` and awaits settlement, and no
test may read an incomplete log). The pinned ctor/signature is in §4.

**Symbol catalog.** New symbols (`SubagentService`, `Agent::onSettled`,
`ResourceCaps::max_live_subagents`, `ChildRoute`, `ChildSpawnRequest`,
`AgentRegistry::createChild`, `AgentRegistry::liveSubagentCount`,
`SessionActivator`/`SessionActivator::submit`, `RouteCatalog`/
`RouteCatalog::is_routable_endpoint`, `LlmRuntime::preflight_route`,
`SettlementResult`, `MessageSource::sender`,
`SessionHeader::endpoint`/`profile_id`, `SubagentFanIn::notice_expected`,
`JobSnapshot::notice_plugin`, `JobStart::notice_plugin`,
`JobSnapshot::notice_text`, `JobOutcome::notice_text`, the persisted
`finish_reason`, …) must be added to
`tests/fixtures/spec_symbol_catalog.json`, or `spec_catalog_test.cpp:471` fails.

---

## 10. Open questions (55-OQ)

Genuine product decisions; none blocks the design, each needs a user call before
or during implementation.

- **55-OQ-1 — Default tool surface.** Should the default composition mount
  **both** `subagent` and `subagent_continuable`, or only `subagent` (dsh's
  default `backgroundMode: one-shot`)? The spec pins both for out-of-the-box
  coverage; dsh ships one unless configured.
- **55-OQ-2 — Field name `provider` vs `endpoint`.** The model-facing field
  keeps dsh's `provider`, but its value is the ymh **endpoint name** (54-D2),
  which is not a "provider id". Rename to `endpoint` for honesty, or keep
  `provider` for dsh fidelity?
- **55-OQ-3 — Adapter-advertised reasoning efforts.** ymh has a free-string
  effort vocabulary and no per-model advertised effort list (dsh's
  `model.reasoning.efforts`). Does `list_subagent_models` report only the
  configured/accepted vocabulary, or should an adapter-effort catalog be added?
- **55-OQ-4 — The `fork` (seeded) provider.** dsh's `fork` backend seeds the
  child with a prefix of the parent log. ymh has `SessionManager::forkSession`
  (`include/ymh/session/session_manager.hpp:54`) and `SessionKind::Fork`. Is a
  seeded delegation provider wanted, and if so is it a third `background_mode`
  or a separate provider?
- **55-OQ-5 — Fan-out cap value and scope.** Default `8` global per daemon; is
  the right default per-parent instead, and what value? Should it be a
  user-facing config key (ResourceCaps has no config wiring today)?
- **55-OQ-6 — `SubagentOutcome` granularity.** Should max-tokens/refusal become
  distinct `SubagentOutcome` enumerators (durable schema change) or stay
  `Failed` + detail (as pinned)?
- **55-OQ-7 — `list_agents` status vocabulary.** ymh's `AgentStatus` is
  `{Idle, Running}` and `AgentState` is richer; is `ready` (stored, not
  resident) the right third status, and should it require a store read?
- **55-OQ-8 — Background executor-queue behavior.** When the `TurnExecutor`
  queue is full, should a background start fail (pinned) or park the child and
  activate it later? Parking needs a pending-activation queue the daemon does
  not have today.
- **55-OQ-9 — `send_message` structured sender.** The additive
  `MessageSource::sender` field (55-D12) vs recovering the sender from the
  framed text. The field is pinned as the default; confirm the codec/schema
  treatment.
- **55-OQ-10 — Permission scope of a delegated child.** `42` OQ-6 (approval
  `never`) is still unassigned. Does this spec need to pin the seam, or does it
  inherit the `42` open question unchanged?
- **55-OQ-11 — Settlement notice text.** Pin dsh's `Background subagent … finished
  and will do no further work unless you send it more.` + closing message (as
  specified), or keep the generic `Background job <id> <status>` text for
  uniformity with other job kinds?
- **55-OQ-12 — Model-only route calls.** dsh accepts a model-only call and
  inherits the provider; ymh requires `provider`+`model` together (the endpoint
  is the route identity, 54-D2). Keep the stricter ymh rule, or inherit the
  parent endpoint for a model-only call?

---

## 11. References

- `docs/design/00-architecture.md` §54 (shared failure modes F1–F12).
- `docs/design/01-session.md` (session header/events), `docs/design/02-*`
  (`SessionOptions`), `docs/design/37-*` (provenance codec) — amended per §1.3/§8.
- `docs/design/06-agent-loop.md` §4 (`Agent`/`AgentServices`), §5.9 (`LLMPool`),
  §6 (activation), §7 (subagents), §9.11 (caps).
- `docs/design/09-permissions.md` (the host-wide policy; `42` OQ-6).
- `docs/design/24-agent-lifetime-errata.md` (lifetime, teardown, `TurnExecutor`).
- `docs/design/26-dsh-alignment.md` §2.4.2 (subagents), G25.
- `docs/design/28-llm-service-boundary-errata.md` (`LlmRuntime` route preflight;
  the public `preflight_route` accessor, 55-A10).
- `docs/design/35-live-notification-errata.md` (`event.live`).
- `docs/design/42-agent-presets.md` §3.3–§3.5, §4 (depth, composition, statement).
- `docs/design/43-wave5-dependency-errata.md` §2 (creation-path copy).
- `docs/design/44-goals-jobs-commands.md` §3 (jobs), §5.5 (job types).
- `docs/design/54-multi-endpoint-routing-errata.md` §3–§5 (routes,
  `ModelSelection`, `resolve_adapter`).
- `docs/design/52-endpoints-models-and-dsh-agent-presets.md` (`permission_preset`,
  endpoint/model catalog).
- dsh: `@deepseek-ai/dsh-tool-subagent` (README.md, lib/index.js — incl.
  `index.js:394` `providerRouteDefaults`), `@deepseek-ai/dsh-tool-subagent-control`
  (README.md) and its separately loadable `list-agents` plugin.

---

## 12. Revision log

**Finding-id legend.** Findings are cited as `55-<round>-<id>`, where `<round>`
is the review round and `<id>` is `Hn`/`Mn`/`Ln` (HIGH/MEDIUM/LOW). The Rev 2
five-reviewer gate's findings have no round infix in prose: a bare `55-Hn`
(e.g. `55-H4`, `55-H13`) means Rev 2 finding `Hn`, matching the **Rev 2** log
entries below (which list `H1`–`H13`). A bare `55-Mn`/`55-Ln` (e.g. `55-M3`,
`55-M7`) likewise means a **Rev 3** MEDIUM/LOW finding, since the Rev 2 log
enumerated only its HIGHs. Later rounds carry their round, e.g.
`55-R3-H1` (Rev 3), `55-R4-H1` (Rev 4), `55-R5-H1` (Rev 5), `55-R6-*` (Rev 6).

- **Rev 1** — first draft. Pins the two delegation modes, the tool surface, the
  `run_in_background` truth table, the durable-child/restart semantics, the
  job-wakeup settlement notice, the `send_message`/`interrupt_agent`/
  `list_agents` control tools, the per-child route-selection precedence and
  `list_subagent_models`, `maxDepth` (reusing `presets.max_depth`) plus the new
  `ResourceCaps::max_live_subagents` fan-out cap, per-child `persona`/
  `toolFilter`, the errors-never-partial-success contract, the mandatory
  `SubagentRunner` settlement fix (`onSettled`), and the parallelism model.
- **Rev 2** — five-reviewer adversarial gate (scope, mode, route, interface,
  completeness): **13 HIGH / 18 MEDIUM / 14 LOW**, all applied. (Prose cites these
  as `55-Hn`, e.g. `55-H4` = `H4` below; later rounds use `55-Rk-*`.)
  - **H1** the job subsystem is entirely unwired — §2.3/§2.5/§2.6.6/§1.2 now
    state it is library code with no production wiring (`44:87`) that this spec
    **builds**, not reuses.
  - **H2** the settlement notice's durability was false — D4 pins a durable
    unreported-settlement record + a replay rule (parent resume / daemon start)
    and restates the guarantee as **at-least-once** (I7/I31).
  - **H3** activation granularity — D4 defines an activation as a residency epoch
    with drain semantics (idle + empty inbox + no owned children); a
    `send_message` extends an open epoch (I32).
  - **H4** the child's durable route was not rebuildable — D6/A7/D12 persist
    `(endpoint, profile_id)` on `SessionHeader` and require catalog membership; the
    "unlisted accepted" claim is deleted (I16/I17).
  - **H5** the preflight signature/guarantee were wrong —
    `resolve_adapter(const LlmCallConfig&, RetryPolicy&) const` validates only the
    route; D6 validates model/effort before `createChild`.
  - **H6** D13 injects `ModelSelectionController&` + a catalog/routability
    accessor.
  - **H7** the fan-out cap had a TOCTOU — D7/D8 pin an atomic reservation with
    rollback (I33).
  - **H8** `createChild`/`liveSubagentCount` are shown as `AgentRegistry` members
    in §4 (matching D8 and compiling).
  - **H9** D13 injects a `SessionActivator` seam (no circular `HostRuntime`).
  - **H10** F19/F20 now have test rows (U28).
  - **H11** §9.5 lists the five `FakeAgent` doubles that must add `onSettled`.
  - **H12** D6 gains the `provider.agentRouteDefaults` layer (step 0); §7 parity
    corrected.
  - **H13** §1.3/§8/header reconciled; §8 gains a Kind column (55-A1 SUPERSEDE)
    and a 54 row (55-A9); `43` is a build-on, not amended.
  - **MEDIUM** — `modelSelectionSettings` recorded as NOT mirrored (§7);
    §1.3/§1.4 rows completed; the §2.3 "every occurrence" claim retracted (prompt
    slots/capability/UI named); D4 notice text pinned to dsh's; `Quiet` overridden
    for `kind==subagent`; killed-child notice fixed; I6/D10 reconciled to
    always-posted delivery; I23 scoped to the resolved flag; I11 qualified to
    "when a roster is present"; D9 outcome source pinned to a persisted
    `finish_reason` + F7 detection; D8 compensating rollback; F21 (settlement
    timeout/disposal) and F22 (generic create failure) added; D7 continuable slot
    release; D11 child activation no longer sets `active_session_` and disposes on
    submit failure; D10 await timeout; the one-shot path now creates via
    `createChild`; D6 step 5 cites the `effective_model_selection` fallback.
  - **LOW** — F9/F12 extended to unknown/disposed/descriptor-less; F11
    cold-resume; F19 retry mechanism pinned (replay); `MessageId` used;
    `ResourceGovernor` dropped from DI; I25 flagged; `list_agents` re-attributed
    to the separate `list-agents` plugin; `tool:<toolName>` guidance recorded
    not-mirrored; line citations corrected (`preset.hpp:149-152`/`:156-161`,
    `agent_loop.cpp:1315`, `subagent.cpp:22`, registry refs); model-only
    half-route recorded (55-OQ-12); the `max_depth` literal default removed;
    routability in DI; `profile_id` added to `ChildRoute`/`ChildAgentOptions`;
    `settle()` returns `SettlementResult`; new symbols added to the symbol-catalog
    test (§9.5).
- **Rev 3** — second independent re-check (four reviewers) + the mandatory
  **symbol/access-level sweep** of every pinned sketch (§3/§4): **2 HIGH /
  8 MEDIUM / 10 LOW**, all applied.
  - **H1 (access)** — the D6 step-4 preflight called the **private**
    `LlmRuntime::resolve_adapter` (`include/ymh/llm/llm_runtime.hpp:262`; only
    `AdapterHandle`/`InterceptorHandle` are friends at `:246-247`), which
    `SubagentService` cannot call. Fixed by pinning a **public**
    `LlmRuntime::preflight_route` accessor (55-A10), registered in §1.3/§8/§4; the
    D6 call, I16, F5 and the §2.6 facility row now name it. `prepare_call` is
    public but binds an adapter generation and returns a `Task<PreparedCall>`, so
    it is not used for a side-effect-free preflight.
  - **H2 (settlement)** — D4 said `onSettled` "fires at the first terminal event"
    while D10 said it fires after the drain; mutually exclusive. Reconciled to the
    mechanism: `onSettled` is the **terminal-event signal** and settlement (the
    epoch close) is a **separate drain check** (idle + empty inbox + no owned
    children). D4 and the D10 pinned comment both state this (I6/U1 unchanged).
  - **M1** — `SessionActivator` was injected/mandated/invariant/listed but never
    defined; pinned in §4 (`submit(const SessionId&) -> bool`) and referenced
    from D11; F13's detection now matches the pinned signature.
  - **M2** — F13/F17 and I1/I25/I26/I28/I30/I35 had no test rows; added
    **U34–U40** (F13, F17, I1, I25, I26, I28, I30) and cross-referenced I35 on
    U31. No coverage is claimed that was not added.
  - **M3** — `settle` unconditionally disposed the child, contradicting
    continuable residency (D3/I4/D5/D7); qualified: **one-shot disposes;
    continuable releases its slot and stays resident** (disposed only on explicit
    teardown).
  - **M4** — `make_list_subagent_models_tool` could not implement its pinned
    behaviour (it carried neither the routability accessor nor `SubagentService&`);
    the signature now takes `const RouteCatalog&` (the §4 seam).
  - **M5** — I17 claimed the durable route was `(endpoint, profile_id, model)`
    while D6/§1.3/A7/§4 said the pair; reconciled to the **pair** everywhere (the
    header already carries `model`/`model_name`), and §1.3's `SessionOptions`
    list now includes `profile_id`.
  - **M6** — the three existing `SubagentRunner` tests were unnamed and the
    runner unpinned; §4 now pins the existing ctor/`run` signature and §9.5 names
    `tests/unit/agent_registry_test.cpp:299`, `:327`, `:365` for update.
  - **M7** — D6 step 0 mis-described dsh's `provider.agentRouteDefaults` as
    parent-inherited; restated as the **backend/provider-advertised** base
    (`index.js:394`, spread at `index.js:496-504`), recorded as a **deliberate
    non-mirror** (no ymh analogue), and §7's "mirrored" claim corrected.
  - **M8** — D10 step 3 still said `activateSession`; renamed to the
    `SessionActivator` seam (D11/D13/I25 already forbid
    `HostRuntime::activateSession`).
  - **LOW** — §1.4 no longer calls `createChild`/`liveSubagentCount` "free
    functions" (they are members); §2.5/§2.6.6 reconciled to **at most one**
    notice per in-memory job; F19 reworded to "one correlated notice per
    unreported fan-in (idempotent; replay may re-deliver)"; D4's job_registry
    cite corrected to `:144` (`:122` also noted); `ids.hpp:18` → `:19`; §2.5
    preset citations corrected to `preset.hpp:207`/`:212`/`:223` and
    `preset.cpp:728`/`:776`/`:829`; D8's snippet is now a member declaration
    matching §4; D7's counted set is one predicate ("resident with an open
    epoch"); the D6/I16 model/effort-validation owner is now
    `AgentRegistry::createChild` (D8's admission seam), not the service.
  - **Access-level sweep result.** Every symbol referenced in §3's pinned
    snippets and §4 was checked for existence, access level, declaration order,
    and member-vs-namespace placement. The three known defect instances are all
    fixed: (1) `is_routable_endpoint` is now a declared `RouteCatalog` member;
    (2) `createChild`/`liveSubagentCount` are `AgentRegistry` members in both D8
    and §4; (3) the private `resolve_adapter` call is replaced by the public
    `preflight_route` (55-A10). **No other violation was found**: `Agent::`
    `followup`/`steer`/`cancel`/`whenIdle`, `AgentPresetRoster::mount`/
    `compose_from`/`scope_for`/`apply_child_composition`, the free
    `check_delegation_depth`, `AgentRegistry::create`/`resume`/`findShared`/
    `getShared`/`dispose`/`finalizeAll`, `SessionManager::createSession`/
    `forkSession`, `LlmRuntime::prepare_call`, and `TurnExecutor::submit` are all
    public and reachable from the pinned call sites; the only new types are
    additive (`RouteCatalog`, `SessionActivator`, `LlmRuntime::preflight_route`,
    the §4 structs). `SubagentRunner`'s ctor/`run` are public.
- **Rev 4** — fifth-reviewer re-check: **1 HIGH / 1 MEDIUM / 8 LOW**, all
  applied. The two substantive defects were both in the settlement mechanism.
  - **H1 (foreground replay)** — the replay rule scanned **every**
    `SubagentFanIn` with no matching notice, but foreground activations append a
    fan-in and deliberately emit **no** notice (D4's foreground paragraph), so
    the next parent resume/daemon start spuriously notified every foreground
    delegation. Fixed by giving `payload::SubagentFanIn` a durable
    `notice_expected` bit (55-A11; default `false`, absent decodes as `false`),
    set `true` only for background-registered epochs; the replay rule now scans
    `notice_expected == true` only, and the foreground paragraph states that
    foreground fan-ins are excluded. I7 and I31 restated; U16 and U28 updated to
    assert a foreground fan-in is never replayed. (55-R4-H1)
  - **M1 (unimplementable correlation)** — D4 required the notice's
    `MessageSource::plugin` to be `subagent-settlement:<child>#<ordinal>`, but
    `JobWakeupPolicy` built its messages with an **empty** plugin
    (`src/jobs/job_wakeup.cpp:21-42`) and no pinned seam carried the string.
    Fixed by adding `JobStart`/`JobSnapshot::notice_plugin` (55-A12): the
    service stamps it at background-job registration (ordinal computed as the
    existing fan-in count **+ 1** — corrected in Rev 5, see below),
    `JobWakeupPolicy` applies it on both the inject and
    wake paths, and `replayUnreportedSettlements` reconstructs it from the
    fan-in. `settle()` gains an explicit `bool notice_expected` (and
    deliberately no notice-source parameter, since the source lives on the job
    snapshot); D13, §4, §2.6(6), and §8 updated. (55-R4-M1)
  - **LOW** — D5 and F9 now include dsh's **unavailable parent** rejection;
    I7/I31 softened from "never yields zero notices" to "delivers on the next
    parent resume / daemon start (zero if never resumed)"; D4's "exactly one
    notice" reconciled with at-least-once and scoped to background epochs;
    §9.4's `agent_loop.hpp:90` corrected to `:91` (the `StreamClock` alias);
    §7's settlement row scoped to background epochs; §1.3/§8/header correspondence
    extended for the two new amendment rows. Re-verified already-applied Rev 3
    LOWs: `ids.hpp:19` (D5), §2.5 preset citations
    (`preset.hpp:207`/`:212`/`:223`, `preset.cpp:728`/`:776`/`:829`), D7's single
    "resident with an open epoch" predicate, D6/I16 model/effort validation owned
    by `AgentRegistry::createChild`, and D8's member-form snippet.
- **Rev 5** — sixth-reviewer re-check (three reviewers, converging findings):
  **2 HIGH / 3 MEDIUM / 4 LOW**, all applied.
  - **H1 (off-by-one ordinal)** — D4 step 1 computed the correlation ordinal
    "at registration from the existing `SubagentFanIn` count for that child",
    while step 3 defined `<ordinal>` as the 1-based count **after** step 2
    appends this epoch's fan-in. The `+1` was never stated, so an implementer
    stamping `existing_count` would make the fast-path `notice_plugin` marker
    disagree with replay's derived ordinal; `replayUnreportedSettlements` would
    find no matching notice and re-deliver a duplicate on every parent resume /
    daemon start (breaking U28 and F20's marker-based idempotence). Fixed by
    pinning `ordinal = existing_count + 1` (the 1-based position this epoch's
    fan-in will occupy) at D4 step 1, D4 step 3, §4's dependency bullet, and the
    Rev 4 log entry. (55-R5-H1)
  - **H2 (unpinned notice-text override)** — D4 said "the service overrides
    [`notice_text`] to dsh's text", but `notice_text` is a **file-local free
    function** consumed unconditionally by `deliver`
    (`src/jobs/job_wakeup.cpp:15-22`, `:123`) and there was no field or hook to
    override it, so U33 was unimplementable as pinned. Fixed by adding
    `JobSnapshot::notice_text` (plus `JobOutcome::notice_text`, supplied at
    settlement because dsh's text embeds the closing message known only then;
    `JobStart::notice_text` exists for symmetry) and pinning that `deliver` uses
    it in place of the generic text (55-A12). (55-R5-H2)
  - **M2 (`finish_reason` unregistered)** — D9 pinned an additive persisted
    `finish_reason` but there was no amendment row and no codec rule, breaking
    the header's exact-correspondence claim. Registered as **55-A13** in
    §1.3/§8/header with the peer-additive rule: omitted when default (`Stop`),
    decoded as default when absent. (55-R5-M2)
  - **M3 (`notice_plugin` propagation unpinned)** — A12 added the field but did
    not state that `JobRegistry::start` copies `start.notice_plugin` into the
    snapshot (the shipped `start` copies field-by-field), so the policy could
    read `nullopt` and stamp nothing. Pinned the copy for both
    `notice_plugin`/`notice_text` in D4 step 1 and the A12 row. (55-R5-M3)
  - **M4 (§1.4 stale)** — §1.4's "It does change" list and its codec-extension
    parenthetical omitted the Rev 4 durable additions (`A10`/`A11`/`A12`). Added
    them (and `A13`). (55-R5-M4)
  - **LOW** — spec `28` added to §11; F1/F4/F6 now cite their test rows
    (`U2`/`U6`); the `SubagentFanIn` anchor corrected to `events.hpp:224-229` in
    §1.3, A11, and D4 step 2; I29 and §1.4 name the new codec fields
    (`notice_expected`, `notice_plugin`, `notice_text`, `finish_reason`).
- **Rev 6** — late seventh-reviewer re-check (the reviewer's Rev 4 re-check
  arrived after Rev 5): **0 HIGH / 1 MEDIUM / 4 LOW**, all applied.
  - **M1 (predicate too narrow)** — `notice_expected` was pinned as "true iff the
    epoch was background-registered", and "background" was pinned in D4 as "a
    background start (one-shot background, or the first `continuable` start)".
    But D4 itself, I32, U18 and U29 require a `send_message` to a **closed** epoch
    to open a new epoch and produce a new notice. A `send_message`-opened epoch is
    not a "background start", so under the narrow predicate its `SubagentFanIn`
    would carry `notice_expected == false` and the replay rule would skip it —
    silently dropping exactly the notice D4/I32/U18/U29 require (the same class as
    the Rev 4 foreground-spam bug, from the opposite direction). Fixed by
    redefining the predicate as **"the epoch was not opened by a foreground-
    blocking activation"**: every non-foreground epoch — a background start **or**
    a `send_message`-opened epoch — is background-registered, gets the
    `kind == "subagent"` job, and is `notice_expected == true`; only an epoch
    opened by a `continuable` **foreground** call is `false`. Reconciled so one
    rule is described at every site: D4 (the epoch opener, step 1 "starts
    asynchronously", step 2 predicate, step 4 replay filter, the guarantee, and
    the closing foreground note), D13 (both comments + the job-registration
    dependency bullet), §4 (`SubagentFanIn::notice_expected` comment), §8 `A11`,
    I7, I31, I32, F19, U16, U18, U28, U29. (55-R6-M1)
  - **L1 (non-compiling §4 snippet)** — the additive `JobStart`/`JobSnapshot`/
    `JobOutcome` fields were shown as bare `std::optional<std::string>` members
    floating between two comment blocks, not inside their structs, so the pinned
    sketch did not compile. Now shown inside each struct with its existing fields
    elided. (55-R6-L1)
  - **L2 (replay wording)** — D4 step 4 read "delivers exactly one notice for
    each", which reads like exactly-once and conflicts with the at-least-once
    guarantee. Reworded to "delivers exactly one notice per unreported background
    fan-in per replay pass (idempotent under repeat)". (55-R6-L2)
  - **L3 (finding-id legend)** — prose cites `55-H4`/`55-H5`/… while the Rev 2 log
    defines `H1`–`H13` with no prefix and no legend. Added a finding-id legend at
    the head of §12 mapping bare `55-Hn` to Rev 2's `Hn` (and `55-Rk-*` to later
    rounds), and noted it in the Rev 2 entry. (55-R6-L3)
  - **L4 (citations verified)** — F1 already cites `U2` and F4/F6 cite `U6` (the
    Rev 5 fix); made it bidirectional by adding `55-F1` to `U2` (`U6` already
    names `F4`/`F6`). §11 already names spec `28`
    (`docs/design/28-llm-service-boundary-errata.md`); confirmed present. (55-R6-L4)

# 56 — dsh Fidelity-Gap Closure (errata)

Status: **verified (Rev 2)** — the five-reviewer gate PASSED (0 open HIGH / 0
MEDIUM); implementation may begin (AGENTS.md "The rule"). It is an **errata**: it
amends `36` §2.1 and §2.6 (the `AssembleContext` shape and the tool-gated
section-suppression mechanism) and `26-dsh-alignment-part2.md` §4.3.3 (the frozen
`AssembleContext` sketch), and it **supersedes specific rows** of `55` §7 (the
`tool:<toolName>` guidance row and the four non-mirror rows — including the
additive `maxDepth: 'provider-managed'` row). It **builds on**
`55` rather than replacing it: `55` remains the delegation contract; `56` only
closes the gaps `55` recorded and pins the process correction the user agreed to.

Revision: **Rev 2** — applies a five-reviewer finding set (see §12). It closes
the `tool:<toolName>` system-prompt guidance gap (the priority), disposes of each
recorded `55` §7 non-mirror with a justification rather than a record, and pins
the "a non-mirror must carry a reason" convention. §10 lists the open questions;
none blocks the design.

---

## 1. Purpose, scope, and the user decision

### 1.1 The gaps this closes

Spec `55` is the verified (Rev 6) delegation contract. Its §7 dsh-mapping table
records seven divergences from dsh's `@deepseek-ai/dsh-tool-subagent` /
`-control`. Two of them are **closed** by `55` itself (`maxDepth` reuses
`presets.max_depth`; `one-shot`/`continuable` is `background_mode`). The
remaining five items are recorded, not resolved:

1. **`tool:<toolName>` system-prompt guidance — not mirrored.** dsh emits a
   `tool:<toolName>` system-prompt section while a continuable delegation tool is
   visible, telling the model to prefer background delegation. `55` §7 records
   "not mirrored" and points at `src/prompt/order.cpp:69-70`. **The finding this
   spec acts on: the slots already exist and are empty.** Nothing is blocked; the
   text was never written.
2. **`modelSelectionSettings` — not mirrored** (`55` §7; `55-D6`).
3. **`provider.agentRouteDefaults` — deliberate non-mirror** (`55` §7; `55-D6`
   step 0).
4. **`maxDepth: 'provider-managed'`** — unrecorded in the §7 table but pinned in
   `55-D7`; requires an out-of-process provider.
5. **`fork` / `acp` / out-of-process backends** — "not in scope" in §7.

The user's **process correction**: a spec that declares a dsh (or any reference)
**non-mirror** must state a **justification** for the divergence, not merely
record it. "Recorded as not mirrored" is insufficient. Part C pins this.

### 1.2 What this changes, in one sentence

`56` makes the reserved `tool:subagent` prompt-ordering slot real by emitting the
dsh delegation guidance per continuable delegation instance while that instance's
tool is visible, suppressing it whenever a tool restriction removes the tool;
and it converts `55` §7's bare non-mirror records into justified dispositions.

### 1.3 Supersession and amendment map

| Target | Kind | What changes |
|---|---|---|
| `36` §2.1 (`AssembleContext`, `:118-121`) | **amend** | add `visible_tools` (56-D2 / 56-A1) |
| `36` §2.6 (tool presentation, `:503-505`) | **amend** | pin the concrete suppression mechanism the section text uses (56-D2/D3 / 56-A2) |
| `36` §2.1 `assemble()` waterfall (`:181-208`) | **amend** | resolve the final tool list before evaluating section/context text (56-D2 / 56-A3) |
| `26-D15` (`26-dsh-alignment-part2.md:153`) / `26-dsh-alignment.md:961` | **realize** | the "suppressed when the tool is restricted away" rule now has an implementation seam (56-D2 / 56-A4) |
| `26-dsh-alignment-part2.md` §4.3.3 (`:398-401`) | **amend** | add `const std::set<std::string>* visible_tools` to the frozen `AssembleContext` sketch (56-D2 / 56-A9) |
| `55` §7 row `tool:<toolName>` system-prompt guidance | **supersede** | "not mirrored" → mirrored per 56-D1 (§7 / 56-A5) |
| `55` §7 row `modelSelectionSettings` | **supersede** | "NOT mirrored … coarser, non-inherited, config-time" → deliberate scope decision (56-D5.1 / 56-A6) |
| `55` §7 row `provider.agentRouteDefaults` | **supersede** | "deliberate non-mirror" → unbuilt-feature deferral (56-D5.2 / 56-A6) |
| `55` §7 row `maxDepth` / `depthLimit` capability (`55:1493`) + `55-D7` (`55:812`) | **additive** | no `55` §7 row exists for `'provider-managed'`; add the deferral row anchored to `55-D7` (56-D5.3 / 56-A10) |
| `55` §7 per-mode tool-schema descriptions | **additive** | no `55` §7 row exists for per-mode schema descriptions; add the disposition row anchored to `56-OQ-2` (56-A11) |
| `55` §7 row `acp` / out-of-process | **supersede** | "not in scope" → justified by `00` §55 (`:4893`, the plugin-ecosystem omission) (56-D5.4 / 56-A6) |
| `55` §7 row `fork` | **supersede** | deliberate scope decision — `fork` is an in-process seeded provider, not a plugin-ecosystem bridge; `00` §55 does **not** cover it (56-D5.4 / 55-OQ-4) |
| `AGENTS.md` §Conventions (`:142-159`) | **amend** | the "non-mirror carries a reason" convention + its review gate (56-D6 / 56-A7) |
| `DESIGN_STATUS.md` spec-56 row | **amend** | status → `draft (Rev 2)` (56-A8) |

The map is exhaustive; §8 is its stable-ID form. `56` does **not** change any
`55` decision, invariant, failure mode, interface, or test; it only adds the
guidance section, the visibility seam that gates it, and the dispositions.

### 1.4 What this spec does and does not change

**In scope.** The `tool:<toolName>` guidance text, its registration, its
emission/removal conditions, its multi-instance handling, the `AssembleContext`
visibility seam, and the fidelity dispositions.

**Out of scope.** The delegation tool schemas, the settlement notice, route
selection, admission/depth/fan-out, and every other `55` contract. dsh's
per-mode tool **schema descriptions** (one-shot vs continuable inline text) are
not mirrored here; that divergence is recorded as 56-OQ-2, not silently closed.

---

## 2. Current state (verified against the shipped tree)

### 2.1 The reserved slots exist and are empty

`src/prompt/order.cpp:69-70` reserves both spellings of the order slot:

```cpp
{"TOOL_SUBAGENT", 2800},
{"tool:subagent", 2800},
```

`include/ymh/prompt/order.hpp` mirrors the value as
`SectionOrder::ToolSubagent = 2800`. The table is the **only** place either name
appears in the **source tree**: a grep of `src/` + `include/` for `TOOL_SUBAGENT`
/ `tool:subagent` matches only `src/prompt/order.cpp:69-70` (the design docs
naturally mention both spellings), and a source-tree grep for `name = "tool:`
finds no section registration anywhere. **No guidance is emitted today.**

`section_order()` resolves a name by exact match and otherwise throws
`ConfigError("unknown prompt section order name '…'")`
(`src/prompt/order.cpp:98-105`). Consequently
`section_order("tool:subagent_continuable")` **throws**; the registration must
look up the order by the dsh **constant** `"TOOL_SUBAGENT"` and set the section
`name` independently (56-D1).

### 2.2 The suppression rule is already pinned but not built

Spec `36` §2.6 already states the intended behavior
(`docs/design/36-prompt-registry.md:503-505`):

> Guidance sections register at their `TOOL_*` orders and are **suppressed when
> the tool is restricted away**.

and `26-dsh-alignment.md:961` and `26-D15`
(`26-dsh-alignment-part2.md:153`) say the same. But no mechanism exists
to suppress anything: `AssembleContext` carries only `scope` and `signal`
(`include/ymh/prompt/system_prompt.hpp:31-34`), and `assemble()` evaluates every
section's `text` **before** it computes the tool list
(`src/prompt/system_prompt.cpp:381-453` sections; `:487-495` the tool provider
and the scope `tool_filter` chain). A section callback therefore cannot know
whether its tool survived restrictions. `36` pinned the rule; `56` builds the
seam.

### 2.3 The shipped delegation tool surface

`WorkspaceRuntime`'s constructor mounts two delegation instances
(`src/agent/workspace_runtime.cpp:425-448`; the config declarations start at
`:425`):

| Instance | `tool_name` | `background_mode` | `enable_run_in_background` |
|---|---|---|---|
| one-shot | `subagent` | `OneShot` | `true` (default) |
| continuable | `subagent_continuable` | `Continuable` | `true` (default) |

`DelegationToolConfig` defaults `enable_run_in_background = true` and
`background_mode = OneShot` (`include/ymh/agent/subagent_types.hpp:66-70`). The
tool's `run_in_background` resolution is
`request.value_or(background_mode == Continuable)`, with a forced `true` under a
disabled flag returning an errored result
(`src/tools/subagent_tools.cpp:128-138`). The tool provider is the raw registry
(`src/agent/workspace_runtime.cpp:341-342`):
`prompt_.set_tool_provider([this](const AssembleContext&) { return tools_.schemas(); });`.

### 2.4 Why a section cannot see the effective tool set today

`assemble()` builds `assembly.tools` last, after the section loop
(`src/prompt/system_prompt.cpp:487-544`): provider schemas → scope
`tool_filter` chain → `tool_order` permutation/selection. The `PromptSection::text`
signature is `std::function<std::string(const AssembleContext&)>`
(`include/ymh/prompt/system_prompt.hpp:36-41`), and `AssembleContext` has no
tool field. An empty section `text` is dropped
(`src/prompt/system_prompt.cpp:429-431`), which is the natural suppression
primitive. `56-D2` supplies the missing input.

---

## 3. Decisions (56-D)

### 56-D1 — The `tool:<toolName>` guidance section

**One section per continuable delegation instance.** For each mounted
`DelegationToolConfig` with `enable_run_in_background == true` **and**
`background_mode == Continuable`, the delegation wiring registers a
`PromptSection`:

- `name  = "tool:" + config.tool_name` (unique per instance; dsh's
  `name: \`tool:${toolName}\``, `index.js:577`),
- `order = section_order("TOOL_SUBAGENT")` (the dsh **constant**, 2800; **not**
  `section_order("tool:" + tool_name)`, which throws — §2.1),
- `text  = visibility-gated guidance` (56-D3).

The registration condition mirrors dsh exactly: the dsh section is registered
only `if (backgroundEnabled && continuable)` (`index.js:576`). A one-shot
instance does **not** register a section even when background is enabled — its
guidance lives in the tool schema description in dsh
(`index.js:400`). ymh does not mirror those schema descriptions (56-OQ-2), but
the **section** condition is mirrored verbatim.

**The pinned text (verbatim dsh, `{tool}` substituted).** The section text for
tool `{tool}` is exactly:

```
Use {tool} in the background by default. Start independent delegations together in one assistant message and continue useful work while they run. Set `run_in_background: false` only when your next action depends on that subagent's result. When a background run settles, the runtime sends you a notice containing its outcome and any final assistant message.
```

This is dsh's `index.js:579` text with `toolName` interpolated; the source README
documents it at `@deepseek-ai/dsh-tool-subagent/README.md:160-165`. With the
shipped continuable tool name, `{tool}` is `subagent_continuable`. The text is a
single paragraph; it contains no newlines.

**Why the text is not ymh-localized.** The statement "the runtime sends you a
notice containing its outcome and any final assistant message" is true in ymh:
`55-D4` pins a durable `SubagentFanIn` plus a `kind == subagent` job notice
carrying the dsh text. Keeping the wording verbatim preserves dsh's KV-cache
prefix character and its model-facing semantics; any localization is 56-OQ-5.

**Pinned helper.** The text is produced by a pure function so the golden test
and the wiring share one source (56-D1, §4).

### 56-D2 — The visibility seam (`AssembleContext::visible_tools`)

`AssembleContext` gains one additive field:

```cpp
// 56-D2: names of the tools the model can call in this assembly — the
// post-restriction, post-`tool_order` names of `PromptAssembly::tools`.
// `assemble()` points this at a local set for the section/context text pass.
// `nullptr` means "outside `assemble()`": tool-gated text MUST treat every
// tool as invisible (fail closed). The pointee never outlives the call.
const std::set<std::string>* visible_tools = nullptr;
```

and a helper:

```cpp
[[nodiscard]] bool tool_visible(const AssembleContext& context, std::string_view name);
// == context.visible_tools != nullptr && context.visible_tools->contains(name)
```

**The `assemble()` reorder (pinned).** `assemble()` resolves the final tool list
**before** evaluating section/context text, then evaluates the text with a local
copy of the context whose `visible_tools` points at that list:

```text
1. chain        := scope_chain(context.scope)
2. provider_tools := tool_provider_(context)                 [if any]
3. for each layer in chain: provider_tools := layer.tool_filter(provider_tools)
4. sort provider_tools by name; resolve tool_order into assembly.tools
   (NO early return here — see below)
5. visible := { s.name for s in assembly.tools }
6. gated := context; gated.visible_tools := &visible
7. assemble sections, contexts, and variables with `gated` (empty text still
   drops); a single `return assembly;` at the end
```

**The empty-`tool_order_` early return is removed.** The existing block
(`src/prompt/system_prompt.cpp:487-544`) contains
`if (tool_order_.empty()) { assembly.tools = std::move(provider_tools); return assembly; }`
(`:502-505`). `tool_order_` is **empty by default** — `SystemPrompt(std::vector<std::string> tool_order = {})`
(`include/ymh/prompt/system_prompt.hpp:166`) and `ToolsSettings::tool_order` has
no initializer (`include/ymh/config/config.hpp:319`). Moved verbatim, that early
`return` would fire *before* sections/contexts/variables are assembled, so every
default-constructed `SystemPrompt` (dozens of tests, and any workspace without an
explicit `tools.tool_order`) would render an **empty** system prompt — silently
breaking 56-I9 and 56-F7 in production. The reorder therefore **splits** the
block: steps 2–4 compute `provider_tools` → filters → sort → `assembly.tools`
with the early return deleted, and only the section/context/variable assembly
that already follows the loop runs under the single trailing `return assembly;`.
The final `assembly.tool_order = tool_order_;` assignment stays where it is.

Steps 2–4 are the existing block moved ahead of the section loop (`:411-451`);
the section/context merge (global layer then scope layers) is unchanged. This is
**behavior-preserving for every existing section whose `text` is a pure function
of its inputs** (no shipped section reads `visible_tools`, 56-I9); it is **not**
behavior-preserving for a tool provider or `tool_filter` callback that has side
effects, because those now run before the section/context `text` callbacks
rather than after — a real, documented evaluation-order change, not merely a
`ConfigError`-precedence change. The `ConfigError`s thrown by an invalid
`tool_order` (duplicate entry, unknown entry, wrong `<unlisted-tools>` count) and
by a provider/`tool_filter` callback now fire before section evaluation instead
of after; the observable outcome (a thrown `ConfigError`) is identical.

**Why the field, not a lookup.** dsh's callback reads
`runtimeCtx.tools.get(toolName, context.scope)` — a scope-aware tool registry.
ymh has no scope-aware tool lookup: restrictions are `tool_filter` closures
owned by the prompt registry (`src/agent/preset.cpp:817`;
`include/ymh/agent/preset.hpp:35-38`), applied inside `assemble()`. Carrying the
resolved names on the assembly context is the least-complex seam that lets the
text callback answer the one question dsh's callback answers, without a
back-reference into `SystemPrompt` (which would recurse).

### 56-D3 — Emission and removal conditions

The guidance section is present in `PromptAssembly.sections` **iff both** hold:

1. **Registration condition (static):** the instance is continuable and
   `enable_run_in_background` is true (56-D1). Otherwise no section exists.
2. **Visibility condition (per assembly):** `tool_visible(gated, tool_name)` is
   true, i.e. the tool name is in `PromptAssembly.tools`.

Because an empty `text` is dropped (`src/prompt/system_prompt.cpp:429-431`), the
visibility condition is implemented by returning `std::string{}` when the tool is
not visible. This makes **removal exactly the dsh rule**: a tool restriction that
removes the tool also removes the guidance, so the prompt never advertises a tool
the model cannot call (`README.md:132`; `index.js:579`).

**Which restrictions count.** Visibility is evaluated against the **final**
`assembly.tools`, so *every* narrowing path suppresses the guidance. The only
path that can narrow is the scope `tool_filter` (preset `toolFilter` / child
composition): a valid `tool_order` **cannot** omit a provider tool. `tool_order`
requires exactly one `<unlisted-tools>` entry and expands it to *all* unnamed
provider tools (`src/prompt/system_prompt.cpp:507-543`), so its output always
contains every provider tool exactly once — it **reorders, never narrows**
(`36` §2.6 calls it a "presentation permutation"). An entry naming a tool that
is not in `provider_tools` throws `ConfigError` rather than silently dropping
it. This is strictly correct: `assembly.tools` is what
`AgentLoop::buildRequest` sends (`36` §2.6).

**Depth does not suppress.** `max_depth == 0` forbids delegation but the tool
stays visible (`55-I9`); the guidance therefore stays visible too, matching dsh's
"the tool stays visible at the cap" (`README.md:63`). Suppression is about tool
visibility, not about whether a call will succeed.

### 56-D4 — More than one instance

The reserved slot is an **order**, not a name: every instance uses
`order = 2800` and a distinct `name = "tool:" + tool_name`. `assemble()` sorts by
`(order, name)` (`src/prompt/system_prompt.cpp:417-424`), so N continuable
instances produce N sections in deterministic code-unit name order, each
independently gated. Restricting one instance removes only its section; the
others remain.

Collision: two instances with the same `tool_name` collide on the section name
and `register_section` throws `ConfigError("duplicate prompt section '…'")`
(`src/prompt/system_prompt.cpp:250-252`). `55-D2` already requires a distinct
`toolName` per instance, so this is a loud config error, not a silent overwrite
(56-F3).

The section handle is held for the lifetime of the prompt: the wiring stores it
in a `std::vector<SectionHandle>` member declared **after** `prompt_` so it
destructs before `prompt_` (the existing `default_prompt_` pattern,
`src/agent/workspace_runtime.cpp:510-512`).

### 56-D5 — Fidelity ledger: dispositions (Part B)

`55` §7 recorded non-mirrors without reasons. Each item below is either closed or
carries a justification. The superseding §7 rows are in §7.

#### 56-D5.1 — `modelSelectionSettings` → **deliberate scope decision** (substitution accepted)

**dsh.** `modelSelectionSettings: true` samples the Host's exact-route
authorization preference when a fresh top-level Session is composed; the
non-empty provider/model allow-list is **recorded in the Session**, **inherited
by children**, and **unchanged by later settings edits**; the tool then exposes
`provider`/`model`/`reasoning_effort` and registers `list_subagent_models`
(`README.md:47,67`; `index.js:255` the setting schema; `index.js:172` defines
`registerListSubagentModels` and `index.js:389` is its call site).

**ymh.** A static per-tool `bool model_selection` (`55-D12`;
`include/ymh/agent/subagent_types.hpp:70`) exposes the same three route fields
in the tool schema, gated per-instance by that flag
(`src/tools/subagent_tools.cpp:101-105`). It does **not** gate the
`list_subagent_models` registration: `make_list_subagent_models_tool` is added
**unconditionally** (`src/agent/workspace_runtime.cpp:447-448`), and both
shipped delegation instances hardcode `model_selection = true`
(`:429`, `:437`), so the two surfaces happen to coincide in the default
composition rather than being coupled by the flag.

**Disposition: the substitution is accepted, with a reason.** Three observations
make the dsh setting's *dynamic* semantics unnecessary in ymh, and name the one
capability that is genuinely lost:

1. **Freeze is vacuous in ymh.** dsh's "unchanged by later settings edits" exists
   because a Host settings controller can mutate the preference mid-life. ymh's
   configuration is load-time JSONC read once per daemon; `model_selection` and
   the endpoint/model catalog do not change while the daemon lives. There is no
   later edit to defend against, so a session-recorded copy adds no protection.
2. **Route inheritance is defaulting, not authorization.** `55-I14` applies only
   when **no** configured or call layer selects a route: it makes a child with no
   explicit route inherit the parent's **effective** `ModelSelection` (durable
   per session, `54`), so it *defaults* the no-explicit-route case. It is **not**
   an authorization boundary — a call or preset layer that names a route still
   wins — so it does **not** by itself establish "a child cannot route outside
   what the parent could" (that framing is vacuous). The genuine divergence is
   the missing per-session allow-list in observation 3, and the route preflight
   (`55-I16`) only validates that a chosen route is a catalog member.
3. **The lost capability is a per-session *subset* allow-list.** dsh can record
   a list **narrower** than the daemon's full catalog and authorize only that
   subset. ymh has no per-session route subset: `list_subagent_models` lists all
   routable endpoints and the tool validates membership, but nothing restricts a
   child to a recorded subset. That is the real divergence.

**Why not close it now.** Closing it means a new durable session policy, a
settings-inheritance path, and a freeze rule — a feature larger than this errata,
with no ymh consumer that mutates route policy mid-session. The bias is to the
least-complex solution that fulfills the requirement; the lost subset capability
is recorded as 56-OQ-3 so it is not silently dropped. If a session-scoped route
subset is ever wanted, the closure path is the `permission_preset` analogue
(`52-D15`) plus a durable `SessionHeader` allow-list that gates both the tool
schema and `list_subagent_models` registration.

#### 56-D5.2 — `provider.agentRouteDefaults` → **unbuilt feature, justified deferral**

**dsh.** `subagentProvider.agentRouteDefaults` is a **backend/provider-advertised**
route base (`index.js:394`), spread as precedence step 0 before tool config and
overridden by any configured value (`index.js:496-504`). It is **not** inherited
from the parent (`55-M7`).

**ymh.** No adapter advertises route defaults; `55-D6` step 0 is empty and the
configured default (`llm.active_model` / the endpoint default) is the terminal
fallback at step 5.

**Disposition: this is an unbuilt feature, not an architectural absence.** The
`LLMProvider` seam (`08`) is a virtual interface; an optional
`agentRouteDefaults()` accessor could be added additively and consumed by
`SubagentService` before the config layer. It is deferred **with a reason**: no
shipped adapter advertises such defaults, so adding the capability without a
consumer would be speculative code, and the configured default already provides
the base route step 0 exists to seed. The seam is left open (56-OQ-4), which is
the opposite of an architectural impossibility.

#### 56-D5.3 — `maxDepth: 'provider-managed'` → **unbuilt feature, justified deferral**

**dsh.** `maxDepth` accepts a natural number or `'provider-managed'`; the latter
"leaves the recursion budget to an out-of-process provider" and is also the
escape hatch when the provider lacks the `depthLimit` capability
(`README.md:53,63`; `index.js:269,377`).

**ymh.** Depth is always a numeric `presets.max_depth` (`uint32`, default 3)
enforced in-process by `check_delegation_depth` at every attempted start
(`55-D7`, `55-I9`, `55-I30`).

**Disposition: representable but meaningless in ymh; defer as an unbuilt
feature.** The value *is* representable — a `uint32` cap could be widened to a
variant, or `0` repurposed — so this is not an "unrepresentable" state. It is
merely **meaningless** without a provider boundary: ymh has only in-process
delegation (`AgentRegistry::createChild`), so a numeric cap is always
enforceable and there is no out-of-process provider to hand the budget to. The
capability therefore rides on the missing out-of-process provider, which `00`
§55 deliberately omits (56-D5.4). Defer until such a provider exists; if one
arrives, the value becomes a second `max_depth` representation (56-OQ-4 family).

#### 56-D5.4 — `fork` / `acp` / out-of-process backends → **deliberate scope decision**

**dsh.** The provider names are `spawn`, `fork`, and `acp` (`README.md:45`).
`fork` is an **in-process seeded provider**: it seeds a child from a prefix of
the parent log and, for KV-cache reuse, inherits the parent's provider and model
rather than selecting a child route (`README.md:140,213`; `55-OQ-4`). `acp` (and
the out-of-process Codex/Claude backends) bridge **external agent processes**
(`README.md:45`).

**ymh.** Delegation is in-process; `SessionKind::Fork` and
`SessionManager::forkSession` exist as a **user-facing session-fork** feature
(`00-architecture.md:773-779,1321-1332`), not as a delegation provider.

**Disposition: justified by the architecture's deliberate scope, cited rather
than restated — with `fork` split out from the out-of-process backends.**
`00-architecture.md` §55 ("DeepSeek Harness Comparison", `:4860`) lists
`large-scale plugin ecosystem` among the capabilities the project **deliberately
omits initially** (`:4893`). That reason covers **`acp`/Codex/Claude** exactly:
they exist to plug external agent processes into the provider set, and ymh's
provider set is compiled in. It does **not** cover `fork`, which is in-process
and therefore not a plugin-ecosystem bridge; `fork`'s omission is instead a
**deliberate scope decision** about delegation modes, already recorded in
`55-OQ-4` (whether a seeded delegation provider is wanted later). Neither is an
unbuilt gap to be closed.

### 56-D6 — The process correction (Part C): a non-mirror carries a reason

**The convention (durable).** A spec that declares a dsh (or any other
reference) **non-mirror** — a capability deliberately not mirrored — must state a
**justification** for the divergence, not merely record it. "Recorded as not
mirrored" is insufficient. An acceptable justification names **which** of these
it is and **why**:

- **architectural absence** — the capability contradicts a pinned architecture
  decision; cite the decision (`00` §55's omitted list is the canonical example);
- **unbuilt feature** — the seam could carry it additively, but no consumer
  exists yet; name the seam and the missing consumer;
- **deliberate scope decision** — a product/scope call; cite the decision or the
  open question that records it.

A justification that only restates the omission ("not in scope", "coarser") is
**not** a justification.

**The anchor requirement (enforceable).** Every non-mirror cell must cite a
**concrete anchor**: either a decision ID (`00`-§55's omitted-list entry, a
`55-OQ-n`, a `52-D15`, …) or a `file:line`. A cell whose reason cites nothing —
or whose "reason" restates the omission ("not mirrored", "coarser", "not in
scope") — is a **finding**, not a disposition.

**The verification gate (enforceable).** The requirement binds the **independent
reviewer**, not only the author. When a spec is reviewed for the `verified`
gate, the reviewer **MUST log a finding** for any non-mirror cell that restates
the omission or cites no anchor; the spec **cannot be marked `verified`** while
such a finding is open. This is deliberately a gate obligation: spec `55` was
marked `verified` while carrying bare non-mirror records, so an author-side-only
rule has already failed once and must be backstopped by review. The rule applies
**before a spec is marked `verified`** — i.e. it is a precondition of the gate,
not a follow-up.

**Where it belongs.** Two edits to `AGENTS.md`, both amended by this spec and
both shown in §4.5:

1. `AGENTS.md` §Conventions — one bullet, so the rule is visible to every spec
   author (the anchor + review-gate obligation; exact wording in §4.5).
2. The component-spec template — the "Each component spec must contain … dsh
   mapping" bullet (`AGENTS.md:142-145`). The mapping table's non-mirror cells
   must carry the justification inline, and the spec's fidelity-ledger section
   (where one exists) must state the disposition (closed / justified).

The single §Conventions bullet satisfies the "visible to every author" intent;
the template edit makes the anchor obligation explicit where the mapping table is
specified.

`56-D5` and §7 are the worked example: every `55` §7 non-mirror now carries a
justification with a concrete anchor.

---

## 4. C++ interface sketches (pinned)

### 4.1 `AssembleContext` + `tool_visible` (56-D2)

```cpp
// include/ymh/prompt/system_prompt.hpp — additive
#include <set>  // new

struct AssembleContext {
    std::optional<ScopeKey> scope;
    CancellationToken*      signal = nullptr;
    // 56-D2: the model-facing tool names visible in this assembly — the
    // post-restriction, post-`tool_order` names of `PromptAssembly::tools`.
    // `assemble()` points this at a local set for the section/context text
    // pass. `nullptr` means "outside `assemble()`": tool-gated text MUST treat
    // every tool as invisible (fail closed). Never retained past the call.
    const std::set<std::string>* visible_tools = nullptr;
};

// 56-D2: the fail-closed membership test.
[[nodiscard]] bool tool_visible(const AssembleContext& context, std::string_view name);
```

```cpp
// src/prompt/system_prompt.cpp — additive
bool tool_visible(const AssembleContext& context, std::string_view name) {
    return context.visible_tools != nullptr &&
           context.visible_tools->contains(std::string{name});
}
```

### 4.2 The guidance text (56-D1)

```cpp
// include/ymh/agent/subagent_types.hpp — additive (pure; no prompt dependency)
// The verbatim dsh `tool:<toolName>` guidance, `{tool}` = `tool_name`.
[[nodiscard]] std::string delegation_guidance_text(std::string_view tool_name);
```

```cpp
// src/agent/subagent_types.cpp (or the tool unit) — additive
std::string delegation_guidance_text(std::string_view tool_name) {
    return "Use " + std::string{tool_name} +
           " in the background by default. Start independent delegations "
           "together in one assistant message and continue useful work while "
           "they run. Set `run_in_background: false` only when your next action "
           "depends on that subagent's result. When a background run settles, "
           "the runtime sends you a notice containing its outcome and any "
           "final assistant message.";
}
```

### 4.3 The delegation wiring (56-D1/D4)

```cpp
// src/agent/workspace_runtime.cpp — add to the include list (:16-51)
#include "ymh/prompt/order.hpp"   // section_order(); system_prompt.hpp is already
                                  // included (:42) for PromptSection/SectionHandle

// member, declared after prompt_ so it destructs before prompt_:
std::vector<SectionHandle> delegation_guidance_;

// per delegation instance (near :425-439) — capture identity BEFORE the move.
// make_subagent_tool takes DelegationToolConfig BY VALUE and the mount passes
// std::move(config) (src/tools/subagent_tools.cpp:375-378), so reading
// `config.tool_name` after the add reads a MOVED-FROM string (empty):
// section name would be "tool:", the text would name an empty tool, and a
// second continuable instance would collide with a duplicate-name ConfigError.
const std::string tool_name = continuable.tool_name;   // copy, pre-move
const bool        guidance_enabled =
    continuable.enable_run_in_background &&
    continuable.background_mode == DelegationToolConfig::BackgroundMode::Continuable;

registrations_.push_back(tools_.add(make_subagent_tool(
    *subagent_service_, SubagentCallerResolver{}, std::move(continuable))));

if (guidance_enabled) {   // use the captured copy, NOT the moved-from config
    PromptSection guidance;
    guidance.name  = "tool:" + tool_name;
    guidance.order = section_order("TOOL_SUBAGENT");   // constant, not the name
    guidance.text  = [tool = tool_name](const AssembleContext& context) {
        if (!tool_visible(context, tool)) {
            return std::string{};
        }
        return delegation_guidance_text(tool);
    };
    delegation_guidance_.push_back(prompt_.section(std::move(guidance)));
}
```

### 4.4 The `assemble()` reorder (56-D2)

```cpp
// src/prompt/system_prompt.cpp — the only structural change
PromptAssembly SystemPrompt::assemble(const AssembleContext& context) const {
    PromptAssembly assembly;
    const std::vector<const ScopeLayer*> chain = scope_chain(context.scope);

    // 56-D2 steps 2-4: resolve the final tool list FIRST (block moved from
    // :487-544). TWO of the block's returns are REMOVED:
    //   (a) the empty-tool_order_ early return
    //       if (tool_order_.empty()) { assembly.tools = std::move(provider_tools); return assembly; }
    //       (:502-505), and
    //   (b) the block's trailing tail return at :544.
    // Both would return before sections/contexts/variables are built. tool_order_
    // is empty by default (hpp:166; config.hpp:319), so (a) alone would render an
    // EMPTY system prompt by default; (b) would do so on every path. The only
    // return is the new trailing one at the end of assemble().
    std::vector<ToolSchema> provider_tools;
    if (tool_provider_) { provider_tools = tool_provider_(context); }
    for (const ScopeLayer* layer : chain) {
        if (layer->tool_filter) { provider_tools = layer->tool_filter(std::move(provider_tools)); }
    }
    std::sort(provider_tools.begin(), provider_tools.end(), /* by name */);
    assembly.tool_order = tool_order_;
    if (tool_order_.empty()) {
        assembly.tools = std::move(provider_tools);   // assignment only, NO return
    } else {
        // ... resolve tool_order into assembly.tools (existing logic) ...
    }

    // 56-D2 steps 5-6: the gated context for section/context text.
    std::set<std::string> visible;
    for (const ToolSchema& schema : assembly.tools) { visible.insert(schema.name.value); }
    AssembleContext gated = context;
    gated.visible_tools   = &visible;

    // ... assemble sections/contexts/variables with `gated` (unchanged) ...

    return assembly;   // single exit, after every field is assembled
}
```

### 4.5 The `AGENTS.md` convention line (56-D6, Part C)

Two edits to `AGENTS.md`, both required by 56-D6.

**(1) A new §Conventions bullet**, after the "Each component spec must contain …"
bullet:

> - **Fidelity divergences carry a reason.** When a spec records a dsh (or other
>   reference) **non-mirror** — a capability deliberately not mirrored — it must
>   state *why* the divergence is acceptable (an architectural absence, an
>   unbuilt feature with its seam and missing consumer, or a deliberate scope
>   decision with a citation), not merely record that it is not mirrored.
>   "Recorded as not mirrored" is insufficient; the reason goes inline in the
>   dsh-mapping table's non-mirror cell **and cites a concrete anchor** — a
>   decision ID (`00` §55's omitted list, a `55-OQ-n`, a `52-D15`, …) or a
>   `file:line`. A cell whose reason restates the omission ("not in scope",
>   "coarser") or cites nothing is a finding, not a disposition: independent
>   review **must log it** and the spec **cannot be marked `verified`** while it
>   is open — the rule applies **before a spec is marked `verified`**. See
>   `56-dsh-fidelity-gap-closure-errata.md` §3 (56-D6) for the convention and
>   worked examples.

**(2) The component-spec template bullet** (`AGENTS.md:142-145`) is extended so
the mapping table carries the anchor obligation where it is specified:

> - Each component spec must contain: C++ interface sketches, invariants,
>   F#-tagged failure modes (F1–F12, §54), dsh mapping, and a test plan. Match the
>   style of `00-architecture.md`. **Every non-mirror cell in the dsh-mapping
>   table carries its justification with a concrete anchor (56-D6).**

---

## 5. Invariants (56-I)

| ID | Invariant |
|---|---|
| 56-I1 | A guidance section is emitted **iff** the instance is continuable with `enable_run_in_background == true` **and** its `tool_name` is in `PromptAssembly.tools`. |
| 56-I2 | The registration condition is exactly `enable_run_in_background && background_mode == Continuable`; a one-shot instance never registers a guidance section (dsh `index.js:576`). |
| 56-I3 | A guidance section's `name` is `"tool:" + tool_name` (asserted explicitly, 56-U2) and its `order` is `section_order("TOOL_SUBAGENT")` (2800). `section_order` is never called with a derived `tool:<name>` string. |
| 56-I4 | `tool_visible` is fail-closed: a `nullptr` `visible_tools` yields `false`; no tool-gated text is emitted outside `assemble()`. |
| 56-I5 | The guidance text is the pinned verbatim dsh sentence with `{tool}` = `tool_name` and no other change. |
| 56-I6 | Guidance is gated on the **final** `assembly.tools`, so every narrowing path suppresses it; the only path that can narrow is the scope `tool_filter` (`tool_order` only reorders — 56-D3), and no guidance is ever emitted for a tool the model cannot call. |
| 56-I7 | N continuable instances yield N sections in `(order, name)` order, each independently gated; restricting one does not affect the others. |
| 56-I8 | The guidance section is presentation-only: it never adds, removes, or reorders `assembly.tools` (asserted, 56-U15). |
| 56-I9 | The `assemble()` reorder is behavior-preserving for every section whose `text` is a pure function of its inputs (no shipped section reads `visible_tools`) **and** for pure tool providers/`tool_filter` callbacks; section/context ordering is unchanged. It is **not** behavior-preserving for a side-effecting provider/filter, which now runs before the text callbacks (56-D2). |
| 56-I10 | The guidance text is part of the rendered system prompt and therefore of `system_prompt_digest` (`36-I10`); tool presence/absence changes the prefix, matching dsh's KV-cache note (`README.md` "KV Cache effect"). |
| 56-I11 | `AssembleContext::visible_tools` is read only inside section/context `text` callbacks invoked by `assemble()`; no caller retains or dereferences it after the call (call-scoped contract asserted, 56-U14). |
| 56-I12 | A duplicate guidance section name is a loud `ConfigError`, never a silent overwrite (`register_section`). |
| 56-I13 | `56` changes no `55` decision, invariant, failure mode, interface, or test; the delegation contract is untouched. |
| 56-I14 | `render()` and `tools()` resolve the same effective tool set for the same scope, because both call `assemble()` with that scope (`src/agent/context_assembler.cpp:51` and `:82`). This is what makes 56-I6 hold: the guidance is gated on the same `assembly.tools` the request actually sends. |

---

## 6. Failure modes (56-F)

Shared findings (`F1`–`F12`, `00` §54) apply where relevant; `56-F*` are local.

| ID | Failure | Detection / guard |
|---|---|---|
| 56-F1 | Guidance emitted while the tool is restricted away. | Impossible by construction: the text returns `""` when `tool_visible` is false, and empty text is dropped. Test 56-U3. |
| 56-F2 | Tool visible but guidance missing (a continuable+background instance). | The wiring always registers when the condition holds; 56-U2. |
| 56-F3 | Two instances share a `tool_name`, so their section names collide. | `register_section` throws `ConfigError` at startup; `55-D2` already requires distinct names; 56-U8. |
| 56-F4 | `visible_tools` outlives its pointee (dangling read). | The field is documented call-scoped; `assemble()` passes a local; `tool_visible` is fail-closed on `nullptr`; 56-I11, asserted by 56-U14. |
| 56-F5 | A one-shot background instance emits guidance (contra dsh). | The registration condition excludes it; 56-U7. |
| 56-F6 | The guidance text drifts from the pinned dsh wording. | Golden test 56-U1 compares the exact string. |
| 56-F7 | The `assemble()` reorder changes section/context ordering or an existing golden prompt. | 56-U9 + the existing prompt-registry/preset/golden suites must stay green; ordering is `(order, name)` unchanged. |
| 56-F8 | Guidance section present in a scope whose tool provider lacks the tool. | Visibility is computed from the final `assembly.tools` (56-I6), so an absent tool yields no section; 56-U3. |
| 56-F9 | A hand-built `AssembleContext` is passed straight to a section `text` with no `visible_tools`. | `tool_visible` returns false (fail closed); 56-U5. |
| 56-F10 | Guidance registered while the tool is not mounted (e.g. delegation wiring skipped). | The wiring registers only in the same branch that mounts the tool; no tool ⇒ no section; 56-U10. |
| 56-F11 | The guidance text is accidentally localized or reformatted. | 56-U1/U11 pin the exact bytes; a digest change is intentional and observable. |

---

## 7. dsh mapping (superseding + additive rows)

These rows **supersede** the named rows of `55` §7, **except** two rows that are
**additive/new** — `55` §7 has no row for either. The first is the
`maxDepth: 'provider-managed'` row (`55` §7's `maxDepth` / `depthLimit` row at
`55:1493` covers only the numeric cap, "already present"). The second is the
**per-mode tool-schema-descriptions** row (56-OQ-2), which supersedes no `55` §7
row and is new here. The `55` table's other rows stand unchanged. Every
non-mirror cell below carries a concrete anchor (56-D6).

| dsh concept | ymh equivalent | Disposition |
|---|---|---|
| `tool:<toolName>` system-prompt guidance (`index.js:576-579`; `README.md:160-165`) | a `tool:<toolName>` section at order `TOOL_SUBAGENT` (2800), text gated by `AssembleContext::visible_tools` (56-D1/D2/D3) | **Mirrored.** Emitted while the continuable instance's tool is visible; suppressed when a restriction removes it (56-I1/I6). |
| `modelSelectionSettings` (host session-recorded allow-list, inherited, frozen; gates `list_subagent_models` registration) | static per-tool `bool model_selection` (`55-D12`) | **Deliberate scope decision** (56-D5.1): freeze is vacuous (the config is loaded once at daemon start and never reloaded — `load_config`, `include/ymh/config/config.hpp:422`); route inheritance is defaulting, not authorization (`55-I14`); the lost capability is a per-session *subset* allow-list (56-OQ-3). Anchor: `55` §7 `modelSelectionSettings` row (`55:1491`) + `55-D6`. |
| `provider.agentRouteDefaults` (backend-advertised base route) | none; configured default is the terminal fallback (`55-D6` step 5) | **Unbuilt feature, justified deferral** (56-D5.2): the `LLMProvider` seam (`08`) could carry it additively, but no adapter advertises it and no consumer exists (56-OQ-4). Anchor: `55` §7 `agentRouteDefaults` row (`55:1488`) + `55-D6` step 0. |
| `maxDepth: 'provider-managed'` (`README.md:53,63`) | numeric `presets.max_depth` only (`55-D7`) | **Additive row — unbuilt feature, justified deferral** (56-D5.3): representable but meaningless without an out-of-process provider, which ymh does not have; the numeric cap is always enforceable in-process. Anchor: `55-D7` (`55:812`) and the `55` §7 `maxDepth`/`depthLimit` row (`55:1493`). |
| `fork` provider (seeded, in-process; `README.md:140,213`) | `SessionKind::Fork` is a user-facing session fork, not a delegation provider | **Deliberate scope decision** (56-D5.4): `fork` is in-process and inherits the parent route for KV reuse, so it is **not** a plugin-ecosystem bridge; whether a seeded provider is wanted is `55-OQ-4`. Anchor: `55-OQ-4`. |
| `acp` / out-of-process (Codex/Claude) providers (`README.md:45`) | none (in-process `spawn` only) | **Deliberate scope decision** (56-D5.4): `00` §55 (`:4860`) deliberately omits the large-scale plugin ecosystem (`:4893`) these external-process backends belong to; ymh's provider set is compiled in. Anchor: `00-architecture.md:4893`. |
| per-mode tool schema descriptions (`index.js:400`) | generic `"Delegate a task to a subagent."` (`src/tools/subagent_tools.cpp:108`) | **Deliberate scope decision** (56-OQ-2): the generic description is mode-agnostic and adequate; per-mode wording is a UX refinement tracked as 56-OQ-2, not silently dropped. Anchor: `src/tools/subagent_tools.cpp:108` + `56-OQ-2`. |

---

## 8. Amendment register

| ID | Target | Kind | Change |
|---|---|---|---|
| 56-A1 | `36` §2.1 `AssembleContext` sketch (`:118-121`) | amend | add `const std::set<std::string>* visible_tools = nullptr;` |
| 56-A2 | `36` §2.6 (`:503-505`) | amend | pin the concrete suppression mechanism: a `TOOL_*` guidance section returns `""` unless `tool_visible(context, tool_name)` |
| 56-A3 | `36` §2.1 `assemble()` waterfall (`:181-208`) | amend | resolve the final tool list before evaluating section/context text (56-D2) |
| 56-A4 | `26-D15` (`26-dsh-alignment-part2.md:153`) / `26-dsh-alignment.md:961` | realize | the "suppressed when the tool is restricted away" rule now has its seam |
| 56-A5 | `55` §7 guidance row (`55:1492`) | supersede | mirrored per §7 |
| 56-A6 | `55` §7 `modelSelectionSettings` (`55:1491`) / `agentRouteDefaults` (`55:1488`) / `fork`/`acp` (`55:1495-1496`) rows | supersede | justified dispositions per §7 / 56-D5 |
| 56-A7 | `AGENTS.md` §Conventions (`:142-159`) | amend | the non-mirror-carries-a-reason convention + anchor/review-gate obligation (56-D6) |
| 56-A8 | `DESIGN_STATUS.md` spec-56 row | amend | status → `draft (Rev 2)` |
| 56-A9 | `26-dsh-alignment-part2.md` §4.3.3 `AssembleContext` sketch (`:398-401`) | amend | add `const std::set<std::string>* visible_tools = nullptr;` (56-D2) |
| 56-A10 | `55` §7 `maxDepth`/`depthLimit` row (`55:1493`) + `55-D7` (`55:812`) | additive | add a `maxDepth: 'provider-managed'` deferral row anchored to `55-D7` (no `55` §7 row exists for the value) (56-D5.3) |
| 56-A11 | `55` §7 per-mode schema descriptions | additive | add the per-mode-schema-description disposition row anchored to `56-OQ-2` (no `55` §7 row exists for it) |

---

## 9. Test plan (56-U)

Conventions follow `44`/`55` §9. `ctest` is not parallel-safe; run the suite only
after the build completes. All tests are hermetic (no live LLM).

### 9.1 Unit

| ID | Test | Asserts |
|---|---|---|
| 56-U1 | guidance text | `delegation_guidance_text("subagent_continuable")` equals the pinned verbatim string byte-for-byte (56-I5, 56-F6/F11). |
| 56-U2 | emitted when visible | a `SystemPrompt` with a tool provider returning `subagent_continuable` and a registered guidance section assembles with that section present, its `name == "tool:subagent_continuable"`, in `(order,name)` position 2800 (56-I1/I3, 56-F2). |
| 56-U3 | **absent when restricted** | the same setup plus a scope `tool_filter` denying `subagent_continuable` yields `.tools` without the tool **and** no guidance section — the whole point (56-I6, 56-F1). |
| 56-U4 | `tool_order` reorders, never narrows | a valid `tool_order` (with exactly one `<unlisted-tools>`) yields `.tools` containing **every** provider tool exactly once, only permuted — it cannot omit one; an entry naming a non-provider tool throws `ConfigError`; and a guidance section for a still-present tool stays (56-D3, 56-I6). |
| 56-U5 | fail-closed | a hand-built `AssembleContext{}` passed directly to the section `text` returns `""` (`visible_tools == nullptr`) (56-I4, 56-F9). |
| 56-U6 | multi-instance | two continuable instances `a`/`b` both visible ⇒ two sections in name order; restricting `a` leaves only `b` (56-I7). |
| 56-U7 | registration condition | a continuable instance with `enable_run_in_background == false`, and a one-shot instance with it true, each register **no** section (56-I2, 56-F5). |
| 56-U8 | duplicate name | two sections named `tool:x` make the second `register_section` throw `ConfigError` (56-I12, 56-F3). |
| 56-U9 | reorder safety | an existing section that does not read `visible_tools` renders identically before/after the reorder; the prompt-registry/preset ordering tests stay green (56-I9, 56-F7). |
| 56-U10 | wiring | the shipped default composition renders exactly one guidance section (`tool:subagent_continuable`) and none for `subagent`; a composition that never mounts a continuable tool renders none (56-I1/I2, 56-F10). |
| 56-U11 | determinism/digest | two assemblies of identical inputs are byte-identical; enabling vs restricting the tool changes `system_prompt_digest` (56-I10). |
| 56-U14 | call-scoped contract | a `SystemPrompt::assemble` call whose local `visible` set is destroyed on return leaves the caller's `AssembleContext` with `visible_tools == nullptr` (unchanged); a section `text` invoked with the caller's original context (not `gated`) returns `""` (56-I11, 56-F4). |
| 56-U15 | presentation-only | adding the guidance section does not change `PromptAssembly.tools` (name set, order, or count) versus the same assembly without it (56-I8). |

### 9.2 Integration (FakeLLM)

| ID | Test | Asserts |
|---|---|---|
| 56-U12 | child composition | a delegated child whose preset `toolFilter` denies `subagent_continuable` renders a system prompt with no delegation guidance, while the parent does (the dsh removal rule end to end; 56-I6); additionally asserts the request `.tools` and the guidance gating derive from the same `active_scope()`, so `render()` and `tools()` resolve the same effective set (56-I14). |
| 56-U13 | cap does not suppress | at `max_depth == 0` the tool and its guidance remain visible even though a start is refused (`55-I9`; 56-D3). |

### 9.3 Existing tests that must change

**None are expected to change.** The reorder is behavior-preserving for pure
sections and providers (56-I9), and no existing section reads `visible_tools`.
Specifically, the four `.sha256` prompt fixtures
(`tests/fixtures/prompt/prompt_{default,minimal,config_override,persona_suffix}.sha256`)
are **unchanged**: `prompt_golden_test.cpp` builds a bare `SystemPrompt` via
`register_default_prompt` and never mounts the delegation wiring, so **no
guidance section is ever registered** in that fixture — the new section cannot
reach the golden output.

The following suites are verified unaffected and must stay green (56-U9):
`prompt_golden_test.cpp`, `prompt_registry_test.cpp`,
`prompt_presentation_test.cpp`, `spec_catalog_test.cpp`, and
`workspace_runtime_test.cpp`. `assemble()` always sets `visible_tools` itself
(56-D2), so a caller cannot make the section appear; a golden changes only if the
fixture itself registers the guidance section — a reviewed, intentional change
(56-I10), not a consequence of this errata.

---

## 10. Open questions (56-OQ)

Genuine product decisions; none blocks the design.

- **56-OQ-1 — One-shot guidance.** dsh emits no `tool:<toolName>` section for
  one-shot background instances (the condition is continuable-only). Should ymh
  extend the section to one-shot background instances (a local divergence), or
  keep strict dsh parity (pinned)?
- **56-OQ-2 — Per-mode schema descriptions.** dsh's `subagent` schema description
  differs by mode (`index.js:400`: continuable vs one-shot vs foreground-only).
  ymh's description is the generic "Delegate a task to a subagent."
  (`src/tools/subagent_tools.cpp:108`). Mirror the per-mode text?
- **56-OQ-3 — Close `modelSelectionSettings`.** Should ymh add a session-recorded
  route allow-list (the lost subset capability, 56-D5.1) gating both the tool
  schema and `list_subagent_models`? If yes, is the `permission_preset` analogue
  (`52-D15`) the right model?
- **56-OQ-4 — Adapter route defaults.** Should the `LLMProvider` seam gain an
  optional `agentRouteDefaults()` for a future adapter, and would a numeric
  `'provider-managed'` representation ride along (56-D5.2/D5.3)?
- **56-OQ-5 — Configurable guidance text.** Should the section text be
  overridable from `config.jsonc` (e.g. `presets` or a `prompt.guidance` key), or
  stay a compiled-in constant for dsh parity and cache stability?

---

## 11. References

- `docs/design/00-architecture.md` §54 (shared failure modes F1–F12); §55
  "DeepSeek Harness Comparison" (`:4860`) and its deliberately-omitted list
  (`:4893`).
- `docs/design/26-dsh-alignment.md` §2.3.6–§2.3.7 (tool presentation, guidance
  suppression, KV discipline; `:961` the suppression sentence);
  `26-dsh-alignment-part2.md` §4.3.3 (`AssembleContext`, `:398-401`), `26-D15`
  (`:153`), `26-I6`, and the `TOOL_*` guidance comment (`:512-513`).
- `docs/design/36-prompt-registry.md` §2.1 (`AssembleContext`, `:118-121`),
  §2.1 `assemble()` waterfall (`:181-208`), §2.6 (`:492-524`, `:503-505` the
  suppression rule), §2.7 (render pipeline), `36-I10` (digest).
- `docs/design/55-multi-agent-delegation-errata.md` §7 (the superseded rows,
  `:1488,1491,1492,1493,1495-1496`), `55-D2`/`55-D6`/`55-D7`/`55-D12`,
  `55-I9`/`55-I14`/`55-I16`/`55-I30`, `55-OQ-4`.
- `docs/design/52-endpoints-models-and-dsh-agent-presets.md` `52-D15`
  (`permission_preset`, the session-pinned analogue).
- Shipped tree: `src/prompt/order.cpp:69-70,98-105`;
  `include/ymh/prompt/system_prompt.hpp:31-41,166`;
  `src/prompt/system_prompt.cpp:381-453,487-544` (and `:429-431` the empty-text
  drop; `:502-505` the removed early return; `:507-543` the `tool_order`
  expansion); `include/ymh/config/config.hpp:319`;
  `include/ymh/agent/subagent_types.hpp:61-70`;
  `src/tools/subagent_tools.cpp:91-118,128-138,375-378`;
  `src/agent/workspace_runtime.cpp:341-342,425-448,510-512`;
  `src/agent/context_assembler.cpp:51,82`;
  `include/ymh/agent/preset.hpp:35-38,164`; `src/agent/preset.cpp:817`.
- dsh: `@deepseek-ai/dsh-tool-subagent` `README.md:45,47,53,63,67,132,140,160-165,213`;
  `lib/index.js:172,255,269,369-377,389,394,400,496-504,576-579`.

---

## 12. Revision log

- **Rev 1** — first draft. Closes the `tool:<toolName>` guidance gap with the
  verbatim dsh text, a per-instance section at the reserved `TOOL_SUBAGENT`
  order, and the `AssembleContext::visible_tools` fail-closed visibility seam
  that makes a tool restriction remove the guidance (Part A). Disposes of each
  recorded `55` §7 non-mirror with a justification: `modelSelectionSettings`
  justified substitution (56-D5.1), `provider.agentRouteDefaults` unbuilt-feature
  deferral (56-D5.2), `maxDepth: 'provider-managed'` justified deferral
  (56-D5.3), `fork`/`acp` justified by `00` §55's deliberate omission (56-D5.4)
  (Part B). Pins the "a non-mirror must carry a reason" convention in `AGENTS.md`
  and the spec template (56-D6, Part C).

- **Rev 2** — applies a five-reviewer finding set: **5 HIGH, 11 MEDIUM, 10 LOW**,
  all fixed (no open items). Numbering additions: invariant `56-I14`, tests
  `56-U14`/`56-U15`, amendment IDs `56-A9`/`56-A10`/`56-A11`; `56-D5.1`'s label
  and the §7 `maxDepth`/schema-description rows changed as noted.

  **HIGH.**
  - **H1** — the `assemble()` reorder would have moved the empty-`tool_order_`
    early return (`system_prompt.cpp:502-505`) ahead of section assembly, and
    `tool_order_` is empty by default (`system_prompt.hpp:166`;
    `config.hpp:319`), so every default prompt would render empty. §4.4 and
    56-D2 now **split** the block (compute provider tools/filters/sort →
    `assembly.tools` with the early return **removed**), build `visible`/`gated`,
    then assemble sections/contexts/variables under a single trailing
    `return assembly;`; the spec states "the empty-`tool_order_` early return is
    removed".
  - **H2** — §4.3 read `config.tool_name` *after* `std::move(config)` into the
    by-value `make_subagent_tool` (`subagent_tools.cpp:375-378`), yielding an
    empty tool name and a duplicate-name error on a second instance. The sketch
    now captures `const std::string tool_name` plus the two flags **before** the
    move and registers from the captured copy.
  - **H3** — `section_order` is declared only in `include/ymh/prompt/order.hpp`
    and was not included. §4.3 now shows `#include "ymh/prompt/order.hpp"` in the
    include list (`system_prompt.hpp` was already included at `:42`).
  - **H4** — the 56-D6 process correction was author-side and unenforceable.
    56-D6/§4.5 now require a **concrete anchor** (decision ID or `file:line`) in
    every non-mirror cell, make it a **reviewer gate obligation** (log a finding;
    the spec cannot be marked `verified` while open), and state it applies
    **before a spec is marked `verified`**. `56-D5.1`'s label is renamed from
    "justified substitution" to the permitted category **deliberate scope
    decision**; D5.3 is labelled **unbuilt feature, justified deferral**.
  - **H5** — `56-D2` amends `AssembleContext`, also pinned in the frozen
    `26-dsh-alignment-part2.md` §4.3.3. Added the amendment row to §1.3 and §8
    (`56-A9`).

  **MEDIUM.**
  - **M1** — `56-D3`/`56-I6`/`56-F8`/`56-U4` asserted an impossible narrowing
    path: a valid `tool_order` always contains every provider tool exactly once
    (`<unlisted-tools>` is required and expands to all unnamed tools,
    `system_prompt.cpp:507-543`), so it only reorders. D3/I6 now say the scope
    `tool_filter` is the only narrowing path; U4 is rewritten to assert the
    reorder-never-narrows contract and the unknown-entry `ConfigError`; F8 is
    rescoped to a `tool_filter`.
  - **M2** — added the `maxDepth: 'provider-managed'` row to §1.3 and §8
    (`56-A10`), anchored to `55-D7` and the `55` §7 `maxDepth`/`depthLimit` row.
  - **M3** — `56-D5.1` observation 2 overstated containment: `55-I14` is
    defaulting, not authorization (it applies only when no configured/call layer
    selects a route). Reworded; the genuine divergence is observation 3.
  - **M4** — `56-D5.4` cited `README.md:83` (the design-concept paragraph) for
    fork/acp semantics; corrected to `README.md:45,140,213`. `fork` is split out
    as an **in-process seeded provider** (`55-OQ-4`); only `acp`/Codex/Claude are
    the plugin-ecosystem (out-of-process) backends covered by `00` §55.
  - **M5** — `56-D5.1` cited `index.js:582-587` for `list_subagent_models`;
    corrected to the definition `index.js:172` and the call `index.js:389`.
  - **M6** — `56-F4` had no test row; added `56-U14` (call-scoped contract) and
    referenced it from F4 and I11.
  - **M7** — `56-F5` cited `56-U9` → corrected to `56-U7`; `56-F7` cited `56-U6`
    → corrected to `56-U9`.
  - **M8** — §9.3 was self-contradictory. It now states the four `.sha256`
    fixtures are **unchanged** (`prompt_golden_test.cpp` renders a bare
    `AssembleContext{}`, so the fail-closed empty section is dropped) and names
    the verified-unaffected suites; the "which the default does" clause is
    deleted.
  - **M9** — the unmirrored per-mode tool schema descriptions (`index.js:400`)
    were recorded only as OQ-2; added a §7 row with an anchor and a reason.
  - **M10** — `56-I9` now scopes behavior-preservation to **pure** sections and
    providers/filters; the evaluation-order change and the extended
    `ConfigError`-precedence note are stated in 56-D2.
  - **M11** — added `56-I14`: `render()` and `tools()` share the same scope via
    two `assemble()` calls (`context_assembler.cpp:51,82`), which is what makes
    `56-I6` hold.

  **LOW.**
  - **L1** — the §7 `maxDepth: 'provider-managed'` row is labelled **additive**
    (no `55` §7 row exists) and listed in §1.3.
  - **L2** — §1.3 now lists `56-A3` and `56-A8` (and `A9`/`A10`); the map is
    declared exhaustive.
  - **L3** — `56-D6` promised two amended places; §4.5 now shows **both**
    `AGENTS.md` edits (the new bullet and the template-bullet extension).
  - **L4** — `56-U2` now asserts `name == "tool:" + tool_name` explicitly; added
    `56-U15` (presentation-only, I8); I8/I11 reference their tests.
  - **L5** — §2.2 tool-provider citation corrected to `:487-489`; §2.4's range
    aligned to `487-544` (with §11).
  - **L6** — §2.3 and §11 `workspace_runtime.cpp` range corrected to `425-448`
    (config declarations start at `:425`).
  - **L7** — §2.1's grep claim is scoped to the **source tree** (the design docs
    legitimately contain the strings).
  - **L8** — `26-D15` is attributed to `26-dsh-alignment-part2.md:153`, not
    `26-dsh-alignment.md`.
  - **L9** — `56-D5.1` claimed `workspace_runtime.cpp:448` shows gating;
    corrected: `make_list_subagent_models_tool` is added **unconditionally**, and
    both shipped instances hardcode `model_selection = true` (`:429`, `:437`).
  - **L10** — `56-D5.3`'s "unrepresentable" is corrected to **representable but
    meaningless** without an out-of-process provider.

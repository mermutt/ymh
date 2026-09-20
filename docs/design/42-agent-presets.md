# 42 — Agent Presets (Wave 5)

```
Status: Rev 2 written · verified: — · reviewer: —
Authority: owning spec for `26-dsh-alignment-part2.md` §5 Wave 5 (decisions
           `26-D16`, `26-D17`). The reserved filename `29-agent-presets.md`
           named in `26-dsh-alignment-part2.md` §5 Stage B (line 1394) is
           **unavailable**: specs 27 through 41 are taken. This file is that
           spec, numbered **42**. Its four cross-spec dependency contracts are
           pinned in `43-wave5-dependency-errata.md` (authored with this rev).
Component: 42 (owning spec). Owns the Wave-5 collaboration composition
           component: the agent roster, the standing mount plus per-session
           joined scopes, the blank-session-only switch, the
           `agent_preset/selected` event, `apply_child_composition`, the depth
           limit, the fixed delegation scope, and the session header's preset
           id. It amends `01-session.md` (the preset event + the header field),
           `23-session-lifecycle-errata.md` (the header/creation contract),
           `06-agent-loop.md` (`AgentServices` / child creation),
           `24-agent-lifetime-errata.md` (monotone child depth),
           `36-prompt-registry.md` (activates the reserved
           `AssembleContext.scope`), `20-skills.md` (children inherit the
           parent's skill set), and `21-config-jsonc-errata.md` (the
           `presets.*` keys). It does not edit any of those files in place.
Depends on: `26-dsh-alignment.md` (design source; §1.2, §2.4.1, §2.4.2),
            `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS) §4.2,
            §4.3.8, §4.3.9, §4.3.9.1, §4.3.9.2, §4.4, §4.5, §4.6, §4.9,
            §5 Stage B, §5 Wave 5, §6 OQ8;
            `36-prompt-registry.md` (Wave 3; sections/persona/scope seam):
            verified per `DESIGN_STATUS.md:55`, **required before Wave-5 code**;
            `29-event-family-errata.md` (the event-family contract; verified per
            `DESIGN_STATUS.md:41`; its consumer matrix carries the
            `AgentPresetSelected` row);
            `43-wave5-dependency-errata.md` (the `01` header/event codec, the
            `23` creation-path copy, the `21` `presets.*` keys, and the `06`
            child-creation seam; **required before Wave-5 code**);
            `30-architecture-cascade-errata.md` §3 (S3/S4; the header cascade);
            `01-session.md`, `06-agent-loop.md`, `23-session-lifecycle-errata.md`,
            `24-agent-lifetime-errata.md`, `20-skills.md`,
            `21-config-jsonc-errata.md` (all verified); the working tree at
            authoring time.
Scope: pin, for Wave 5, (1) preset identity and the roster (`26-D16`);
       (2) the one-standing-mount-per-preset model and the per-session joined
       scope chain; (3) the blank-session-only switch and the
       `agent_preset/selected` event; (4) `apply_child_composition` and the
       child composition (`26-D17`); (5) the delegation depth limit; (6) the
       fixed delegation scope; and (7) the session header's preset id, which is
       the `23` contract break. Design only, no code.
Supersedes: `26-dsh-alignment-part2.md` §5 Stage B (line 1394), which reserved
            the name `29-agent-presets.md`. That number is taken by the
            event-family errata; the reserved artifact is this file.
```

This document is a **pin**, not a proposal. Every `file:line` below was
re-derived against the working tree at authoring time. The roster, the scope
chain, the preset file format, the `agent_preset/selected` event, and the header
fields **do not exist in the tree yet**; they are pinned here as the contract
Wave-5 code must satisfy. The four cross-spec contracts that Wave-5 code also
requires are pinned in `43-wave5-dependency-errata.md`. Where the design source
and a verified spec disagreed, Rev 1 recorded the disagreement as an open
question; Rev 2 resolves each one in §12 with the rejected alternative named, so
no disagreement is silently resolved.

---

## 1. Purpose, numbering, and the staging constraint

### 1.1 Why this spec exists

`26-dsh-alignment.md` §1.2 puts "the way of working and collaboration" in scope:
"presets that compose a session, subagents that join the parent composition,
goals, background jobs, and slash commands that are not model messages"
(`26-dsh-alignment.md:61-63`). Wave 5 is the first half of that layer: presets
and subagent composition. Goals, jobs, and commands are Wave 6 and are out of
scope here.

The shipped tree has **no** preset concept. `AgentServices` carries the tool
registry, permission policy, `LlmRuntime`, and the prompt registry, all as
process-wide or host-wide pointers (`include/ymh/agent/agent_loop.hpp:49-70`).
A session's composition is therefore identical for every session in a workspace.
`SubagentRunner` creates a child with `kind = Subagent` and the parent id
(`src/agent/subagent.cpp:18-19`) and nothing else: no composition join, no depth,
and no fixed delegation scope (`include/ymh/agent/subagent.hpp:17-31`). Wave 5
supplies the missing composition seam.

### 1.2 Authority and relationship to the other specs

- `26-dsh-alignment.md` §2.4 is the **design source**. §2.4.1 pins the
  standing-mount plus joined-scope model and the blank-session-only switch
  (`:989-1057`); §2.4.2 pins `applyChildComposition`, the depth knob, and the
  fixed delegation statement (`:1059-1114`). The verbatim dsh texts quoted
  there are the copy target.
- `26-dsh-alignment-part2.md` §4.3.8 is the **pinned C++ sketch**
  (`:739-800`); §4.3.9/§4.3.9.1 pin the event wire name and JSON keys
  (`:844`, `:891`, `:954`); §4.3.9.2 pins the consumer obligation
  (`:991`, `:1006`); §4.4 classifies the break (`:1088`); §4.5 pins `26-F9`
  (`:1141-1142`); §4.9 pins the config keys (`:1320-1323`); §5 Wave 5 is the
  scope (`:1485-1492`); §6 OQ8 pins the format divergence (`:1621-1624`).
- `36-prompt-registry.md` owns the section/context registry, persona, and the
  `AssembleContext.scope` seam. This spec is an **additive errata to `36`**:
  Wave 3 registers the global layer only and leaves `AssembleContext.scope`
  reserved and always absent (`36-prompt-registry.md:104-110`), and Wave 3
  defers "per-scope shadowing and any scope-mount rule" to Wave 5
  (`:854-858`). This spec supplies that rule.
- `29-event-family-errata.md` owns the `01` event-family contract. Its
  consumer matrix already carries the `AgentPresetSelected` row
  (`29-event-family-errata.md:390`) with "Wave 5" as the implementing wave, so
  this spec pins the **append site and semantics**, not the codec extension
  rule.
- `30-architecture-cascade-errata.md` §3 pins the cascade: S3, the session
  header change (`:164-183`), and S4, the collaboration surfaces (`:185-205`).
  This spec is the owning artifact S3/S4 point at.
- `01-session.md` owns `SessionHeader` (`:149-190`) and `deriveMessages`. This
  spec is an **errata to `01`** for the preset field and the new event.
- `23-session-lifecycle-errata.md` owns the creation path
  (`:88-89`, `23-D52` at `:369`) and the subagent-kind fix
  (`23-A18` at `:214`, `23-D53` at `:370`). The preset id rides that creation
  path, so `23` needs an errata before Wave-5 code (the `26` register names
  `23` as the gate, `26-dsh-alignment-part2.md:154`).
- `24-agent-lifetime-errata.md` owns agent lifetime and the no-raw-handle rule
  (`24-D17`). Children stay ordinary agents; depth is a new persisted
  attribute and changes no lifetime rule (`26-dsh-alignment-part2.md:1089`).
- `20-skills.md` owns the skill catalog. D17 makes a child inherit the parent's
  skill set (`26-dsh-alignment-part2.md:1085`).

### 1.3 In scope

1. **Preset identity**: `AgentPreset`, its id, its display metadata, and its
   composition rows, discovered from a configured root.
2. The **roster**: `AgentPresetRoster` with `list`, `resolve`, `mount`,
   `compose_from`, `composed_preset`, `leaf_for`, `config`, `select`, and
   `standing_key_for`.
3. The **standing mount**: one composition per preset id per process, mounted
   under a standing scope key, plus the **per-session joined scope** chain.
4. The **blank-session-only switch** and its predicate.
5. The **`agent_preset/selected`** durable event
   (`payload::AgentPresetSelected`).
6. **`apply_child_composition`** and `ChildComposition`.
7. The **depth limit** (`presets.max_depth`, default 3; 0 forbids delegation;
   carried on `PresetConfig` — 42-D18).
8. The **fixed delegation scope** (fixed approval policy plus the fixed
   runtime-context statement).
9. The **session header preset id**, the `23` contract break.
10. The `presets.*` config keys (`26-dsh-alignment-part2.md:1320-1323`) plus
    the Wave-5 `presets.max_depth` key (42-D18).

### 1.4 Out of scope

- **Goals, background jobs, and the command surface.** They are Wave 6
  (`26-dsh-alignment-part2.md:1494-1501`). `GoalRef`/`GoalSnapshot`/`Job` appear
  in the §4.3.8 sketch only as shared value types and are not pinned here.
- **Continuable subagents and the control tools** (`send_message`,
  `interrupt_agent`, `list_agents`). `26-D17` names "continuable control tools"
  (`26-dsh-alignment-part2.md:155`) but the Wave-5 bullet list does not
  implement them. This spec pins the one-shot composition join only; the
  continuable delivery/control surface is **explicitly out of Wave 5** and is
  owned by a future **additive errata to `07-tools-execution.md`** (the
  `spawn_subagent` tool surface, `07 §5.2`), recorded as 42-D16 and OQ-3.
- **Remote / ACP / SDK / Claude-Code / Codex subagent providers.** Only
  in-process spawn/fork (`26-dsh-alignment-part2.md:1519-1520`).
- **Cordis, `cordis.yml`, `!!js`.** ymh keeps JSONC; only the composition
  semantics are mirrored (`26-dsh-alignment-part2.md:1505-1506`,
  `26-dsh-alignment.md:69-71`).
- **PTC / `run_code`.** `Native` only (`26-dsh-alignment-part2.md:1507-1511`).
- **Any `kSchemaVersion` or `kProtocolVersion` bump.** Neither changes
  (`26-dsh-alignment-part2.md:1170-1189`, `26-I12`).
- **Implementation code.**

### 1.5 The Wave 3 dependency (the staging constraint)

**Wave 5 requires Wave 3 (`36-prompt-registry.md`) to be verified before any
Wave-5 code.** A preset composes *sections, persona, tools, and skills*, and
none of those seams exist before Wave 3. `26-dsh-alignment-part2.md:1491-1492`
states it directly: "requires Wave 3 (sections/persona/tools) to compose."
`36-prompt-registry.md:86` lists "Presets, subagent composition, goals, jobs,
commands (Waves 5 and 6)" as out of scope, which is the same dependency stated
from the other side.

The consequence for the interface sketches in §3 is that this spec **extends**
`SystemPrompt` (it does not replace it). Wave 3's `AssembleContext.scope` field
exists and is reserved; Wave 5 activates it. Wave 3's `SystemPrompt` is
"Single-threaded: owned by `WorkspaceRuntime` and called only on the agent
executor thread" (`36-prompt-registry.md:153-154`, `26-I10`); the roster
inherits that threading contract and is **not** thread-safe by design.

Wave 4 (`40-output-retention.md`) is a sequencing neighbour, not a hard
dependency: Wave 5 needs Wave 3's provenance and section model, not Wave 4's
scheduler or retention library. The migration plan serializes Wave 3 → Wave 4
(`26-dsh-alignment-part2.md:1477`), so Wave 5 lands after both in practice.

---

## 2. The preset model

### 2.1 Preset identity and the roster

A **preset** is one directory containing a composition file plus display
metadata and optional skill roots (`26-dsh-alignment.md:991-993`). ymh uses
**JSONC**, not `agent.cordis.yml`: presets are "JSONC directories under a
configured root (mirroring `roots`/`includeShippedRoot`/`includeUserRoot`)"
(`26-dsh-alignment-part2.md:1621-1624`, OQ8). The format divergence is
deliberate; the composition **semantics** are copied.

A preset **selects** four things for one session (`26-dsh-alignment.md:1000-1003`):

1. **tools** (a `ToolRestriction` over the host registry),
2. **prompt sections** (scoped `PromptSection` registrations),
3. **skills** (the skill roots/catalog entries the session may see), and
4. the **persona** (the scoped `deployment:persona-prefix`/`-suffix`).

A preset **does not own the registries**. `ToolRegistry`, `PermissionPolicy`,
the sandbox/execution environment, persistence, and the model route stay
host-owned (`26-dsh-alignment.md:1013-1015`). ymh therefore spells "the preset
selects tools" as a **scope-scoped filter over `ToolRegistry::schemas()`
(`include/ymh/tools/tool_registry.hpp:90`)**, never as a second registry. This
is the load-bearing divergence from dsh's plugin registrations and is pinned by
42-D10.

Resolution rules:

- `resolve(nullopt)` returns the configured default preset
  (`presets.default`, `26-dsh-alignment-part2.md:1321`). If no default is
  configured and no id is given, resolution **fails loud** (42-F1); there is no
  implicit empty preset.
- `resolve(id)` matches an exact id. An unknown id fails loud. ymh does **not**
  prefix-match or fuzzy-match.
- Discovery scans the shipped root and the user root according to
  `presets.include_shipped_root` / `presets.include_user_root`
  (`26-dsh-alignment-part2.md:1322-1323`), plus `presets.root` when set
  (`:1320`). A duplicate id across roots is a load failure (42-F8).

### 2.1.1 The preset file schema and the loader (pinned, 42-D17)

OQ-5 is resolved here: the row vocabulary is pinned by this spec, and the
loader is owned by `AgentPresetRoster` (not `21`, which owns only the
`presets.*` config keys). A preset is a directory `<root>/<preset-id>/`
containing exactly one composition file, `preset.jsonc` (JSONC, 42-D1).
`list()` enumerates the immediate subdirectories of each configured root and
reads `<dir>/preset.jsonc`:

```jsonc
{
  "id": "standard",            // optional; defaults to the directory name.
                               // If present, must equal the directory name.
  "display_name": "Standard",  // optional; defaults to `id`.
  "rows": [                    // optional; defaults to [].
    {
      "id": "persona",         // required; unique within the preset
      "group": "core",         // optional; grouping only, no behavior
      "disabled": false,       // optional; default false
      "persona": {             // optional
        "prefix": "You are ...",
        "suffix": "cwd: {{cwd}}"
      },
      "tools": {               // optional ToolRestriction; deny wins
        "allow": ["read", "grep"],
        "deny":  ["shell"]
      },
      "skills": ["skills/editing"],  // optional; preset-relative roots
      "sections": [            // optional PromptSectionSpec list
        { "name": "extra", "order": 50, "complete": false, "text": "..." }
      ],
      "config": {}             // optional; opaque, row-specific JSONC
    }
  ]
}
```

The mapping is exact: `id`/`display_name` → `AgentPreset::id`/`display_name`;
the directory → `AgentPreset::source_path`; each element of `rows` → one
`PresetRow`. `persona.prefix`/`suffix` → `persona_prefix`/`persona_suffix`;
`tools` → `tool_filter`; `skills` → `skill_roots`; `sections` → `sections`;
`config` → the opaque `config` string (the JSONC value is serialized verbatim).

**Loader validation (owned by `AgentPresetRoster`; 42-D17, 42-F8).** Every
violation is a load-time `PresetLoadError` that names the file and key; none is
a silent skip:

- An **unknown key** at any level (top-level object, row, `persona`, `tools`,
  or a section spec) fails load.
- A missing or empty `rows[].id`, or a duplicate `rows[].id` within one
  preset, fails load.
- A duplicate `sections[].name` across the rows of one preset fails load (the
  standing mount is one layer and forbids duplicate names, `36 §2.1`).
- A top-level `id` that is present and differs from the directory name fails
  load.
- A duplicate preset `id` across roots fails load (42-F8).
- `tools.allow`/`tools.deny`/`skills` must be string arrays, and each element
  of `sections` must match `PromptSectionSpec`; a type mismatch fails load.
- `skills` roots are resolved against the preset directory through
  `ExecutionEnvironment::resolve()`; a root that escapes the workspace root
  fails load (F5, `AGENTS.md` path-safety rule).

A directory with no `preset.jsonc` is **not** a preset and is skipped (not a
failure); a present-but-invalid `preset.jsonc` is a failure. An unknown **tool
name** inside `tools` is deliberately **not** a load failure: the restriction is
a scope-scoped filter over the host registry (42-D10), so a name the host does
not offer simply matches nothing at assembly time.

### 2.2 The standing mount and per-session joined scopes

The precise dsh model is "**One standing composition per preset.** A preset is
mounted once per process under a standing scope; agents join by parenting their
scope key to the mount, so the mount's registrations and listeners cover every
joined agent and no sibling preset's" (`26-dsh-alignment.md:1033-1037`). The
per-session behavior is "a property of the **composition's scope**, not a
second mount" (`:1037-1038`).

ymh adopts this exactly:

- **One standing mount per preset id per `WorkspaceHost` process.** The roster
  keeps a map `preset id -> standing ScopeKey`. `mount` is idempotent: the
  second call for the same id reuses the existing mount and registers nothing
  twice (42-I1, 42-F2). The standing key is minted by `standing_key_for(id)`.
- **A session joins by scope parentage.** Each agent has an `AgentContext`
  (`26-dsh-alignment-part2.md:751`) whose `scope` is a **leaf** `ScopeKey`. The
  leaf's parent is the standing key. `SystemPrompt::assemble` resolves the full
  chain, global layer first, then each ancestor, then the leaf, applying
  shadowing by name (`36-prompt-registry.md:164`).
- **The scope chain is live, not header-derived.** `compose_from(child, parent)`
  reads the parent's **live** scope chain (`26-dsh-alignment.md:1086-1089`),
  because "a parent that switched preset while blank runs on the newer
  composition and its header still names the older one."

`ScopeKey` stays `std::string` (`26-dsh-alignment-part2.md:747`). The roster
mints two shapes, which are the only ones the seam must understand:

```text
standing key : "preset:<preset-id>"
leaf key     : "session:<session-id>"
```

The chain is `session:<id>` -> `preset:<id>` -> global. This spec pins those
two prefixes; any other string is treated as a leaf whose parent is absent
(the global layer only), which keeps the `36` contract total for a
scope-less Wave-3 agent.

**Shadowing rule.** Within one name, the leaf wins over the standing mount, and
the standing mount wins over the global layer. Two siblings never see each
other's registrations because their leaves parent different standing keys.
Duplicate names **within one layer** throw (`36-prompt-registry.md:102-103`);
shadowing across layers is legal.

**Persona.** The standing mount registers the preset's persona as the scoped
`deployment:persona-prefix` / `-suffix`. The global default persona remains
registered in the global layer. A child's `ChildComposition.persona` (if any)
is a leaf-scoped shadowing section, so it overrides both.

### 2.3 The blank-session-only switch

A session can switch preset only while it "has produced nothing, no messages or
tool calls. After that, the composition is fixed for the session's life,
because swapping tools mid-conversation would leave logged tool calls the new
composition cannot make" (`26-dsh-alignment.md:1049-1053`).

ymh pins the predicate as **42-I3 / 42-D4**: a session is **blank** iff its
event log contains none of

```text
UserMessage, AssistantMessage, ToolCall, ToolResult,
ContextInjected, ContextCompaction,
SubagentSpawned, SubagentFanIn, LlmRequestHeader
```

`SessionStarted`, `SessionRenamed`, `PlanMode`, and `TokenUsage` do not make a
session non-blank. `ContextInjected` is included because Wave 3 turns the
instructions loader and runtime contexts into durable user-role messages
(`36-prompt-registry.md:859-862`); once they are appended, the request series
has begun. `LlmRequestHeader` is included because it marks a dispatched series
(`26-D2`). This predicate is **stricter** than dsh's prose ("no messages or
tool calls") and matches its rationale. The stricter reading is pinned here
because the looser one would allow a switch after the first request, which is
exactly the failure the rule exists to prevent.

`select(agent, preset)`:

1. Resolves the target preset. Unknown id fails loud (42-F1).
2. Resolves the agent's live leaf (`leaf_for(agent.id())`, 42-D15) and its
   `Session` (`sessions_.sessionPtr(agent.session())`, 42-D15), then tests the
   blank predicate against the session log. Non-blank fails with a typed
   rejection (42-F4); the session keeps its current composition and **no event
   is appended**.
3. Swaps the agent's live leaf scope to the new preset's standing mount,
   rewires the chain, and updates the roster's leaf map.
4. Appends `agent_preset/selected` to the session's own log via
   `Session::append` **after** the swap commits (42-I9, 42-F11), so a replayed
   log never names a preset that did not actually run
   (`26-dsh-alignment.md:1053-1057`).
5. Starts a new request series on the next turn. ymh has **no `tools/change`
   event**: dsh emits one, but ymh's durable record of a composition change is
   the `agent_preset/selected` event plus the next `LlmRequestHeader`, whose
   `tool_names` and `tool_schema_digests` already capture the catalog change
   (`26-D2`, `26-I6`, `26-dsh-alignment-part2.md:1133-1136`). This is 42-D6.

### 2.4 The effective composition

For one agent, the **effective composition** is the ordered merge:

```text
global layer
  -> preset:<standing-id>            (the standing mount: sections, persona, tool filter, skills)
     -> session:<session-id>          (per-session layers: child persona, child tool filter, child skills)
```

It is materialized by `SystemPrompt::assemble` into a `PromptAssembly`
(`36-prompt-registry.md:139-145`), whose `sections`, `contexts`, `tools`,
`variables`, and `tool_order` are exactly what Wave 3 pins. Wave 5 adds no new
assembly field; it only supplies the scope chain the assembly already accepts
and the scope-scoped registrations that feed it.

`composed_preset(ctx)` returns the preset id of the **live** leaf's parent
standing mount. This is the value the subagent path records and the value a
"running preset" projection reports. It can differ from the header's preset id
after a blank-session switch (42-I4).

### 2.5 Config wiring

The `presets.*` keys are pinned by `26-dsh-alignment-part2.md:1320-1323`, plus
the Wave-5 depth key (42-D18):

| Key | Type | Default | Notes |
|---|---|---|---|
| `presets.root` | path | unset | extra root; OQ8 (`:1320`) |
| `presets.default` | string | unset | required when any preset is configured |
| `presets.include_shipped_root` | bool | `true` | |
| `presets.include_user_root` | bool | `true` | |
| `presets.max_depth` | int >= 0 | `3` | NEW (42-D18); 0 forbids delegation |

These are **new keys**; `21-config-jsonc-errata.md` does not yet carry them
(grep for `presets.` in `21` returns nothing). The `21` errata is part of the
Wave-5 gate. The required-global-layer rule, the JSONC-only rule, and the
"never auto-create an explicit `--config`" rule are untouched, so the change is
**additive** (`26-dsh-alignment-part2.md:1086`). They parse into the roster's
`PresetConfig` (§3.1).

`presets.max_depth` is **ymh's home for the depth knob** (OQ-4 resolved by
42-D18): `26 §4.9` (`:1320-1323`) has no depth key, and the design source puts
`maxDepth` on the delegation *tool* config (`26-dsh-alignment.md:1094`). ymh
pins it once, on `PresetConfig`, and the delegation tool reads it from there
rather than owning a second copy (one source of truth, `26-I7`). The divergence
from dsh's placement is deliberate and additive.

---

## 3. C++ interfaces (pinned)

All types live in `namespace ymh`. Signatures are transcribed from
`26-dsh-alignment-part2.md` §4.3.8 (`:744-772`) and refined only where a
refinement is load-bearing; every refinement is called out.

### 3.1 Shared value types

```cpp
// include/ymh/agent/preset.hpp

// Forward declarations for types referenced by reference/pointer only.
struct ChildComposition;   // defined in §3.3
class  Session;            // ymh/session/session.hpp
class  SessionManager;     // ymh/session/session_manager.hpp
class  Agent;              // ymh/agent/agent.hpp

// 26 §4.3.8 (:746-751). Unchanged.
using GoalId   = std::uint64_t;              // Wave 6; declared here as a shared type only
using ScopeKey = std::string;                // preset mount scope key
struct ToolRestriction {
    std::vector<std::string> allow;          // empty allow == all not denied
    std::vector<std::string> deny;           // deny wins over allow
};
struct AgentContext {
    AgentId  agent;
    ScopeKey scope;                          // the leaf; parent is the standing key
};

// NEW (42-D17). The `36` `PromptSection` (36 §2.1, :123-128) without its `text`
// closure: a row carries literal text and the roster binds it into
// `PromptSection::text` at mount. name/order/complete are exactly 36's.
struct PromptSectionSpec {
    std::string  name;                       // unique within a layer (36 §2.1)
    std::int32_t order = 0;                  // ascending; ties by code-unit name (36-I1)
    bool         complete = false;           // at most one effective (36 §2.2)
    std::string  text;                       // section body; bound at mount
};

// One preset's composition row (42-D17 pins the schema; §2.1.1 the file format).
// Shape pinned at a semantic level.
struct PresetRow {
    std::string                        id;         // unique within the preset
    std::string                        group;      // grouping only; no behavior
    bool                               disabled = false;
    std::optional<std::string>         persona_prefix;
    std::optional<std::string>         persona_suffix;
    std::optional<ToolRestriction>     tool_filter;
    std::vector<std::string>           skill_roots;
    std::vector<PromptSectionSpec>     sections;   // scoped sections to register
    std::optional<std::string>         config;     // opaque JSONC; row-specific
};

// 26 §4.3.8 (:755). The rows are opaque to callers; the roster owns their meaning.
struct AgentPreset {
    std::string               id;
    std::string               display_name;
    std::filesystem::path     source_path;   // preset directory (JSONC)
    std::vector<PresetRow>    rows;          // composition rows; see 42-D17
};

// NEW (42-D18): the parsed `presets.*` block (the roster's config). 26 §4.9
// (:1320-1323) plus the Wave-5 depth knob (OQ-4 resolved).
struct PresetConfig {
    std::optional<std::filesystem::path> root;                 // presets.root
    std::optional<std::string>           default_id;           // presets.default
    bool                                 include_shipped_root = true;
    bool                                 include_user_root    = true;
    std::uint32_t                        max_depth = 3;        // presets.max_depth (42-D18)
};
```

`PresetRow` is new (the §4.3.8 sketch writes `/* rows */`). It is the ymh
spelling of dsh's `{id, name, group, isolate, config, disabled, inject}`
(`26-dsh-alignment.md:1000-1001`); `isolate` and `inject` have no ymh analogue
and are deliberately absent. The declaration order is pinned (`PromptSectionSpec`
→ `PresetRow` → `AgentPreset`) so every member type is defined before it is
used (42-D17). `PromptSectionSpec` is the `36` `PromptSection` without the
`text` closure, which the roster binds at mount time.

### 3.2 The roster

```cpp
// include/ymh/agent/preset.hpp

class AgentPresetRoster {
public:
    // 42-D15: `sessions` resolves an agent's live `Session` (for the blank
    // predicate and the post-commit append). The roster also keeps the
    // `AgentId -> AgentContext` leaf map that bridges the thin `Agent&` to a
    // live scope (42-I2).
    AgentPresetRoster(SystemPrompt& prompt, ToolRegistry& tools,
                      SkillCatalog& skills, SessionManager& sessions,
                      PresetConfig config);

    AgentPresetRoster(const AgentPresetRoster&) = delete;
    AgentPresetRoster& operator=(const AgentPresetRoster&) = delete;

    // Discovery + resolution. `resolve(nullopt)` returns the configured default.
    // An unknown/absent default or id throws PresetNotFound (42-F1).
    [[nodiscard]] std::vector<AgentPreset> list() const;
    [[nodiscard]] const AgentPreset& resolve(std::optional<std::string> id) const;

    // The standing key for a preset id; pure. 26 §2.4.1 `standingKeyFor`.
    [[nodiscard]] ScopeKey standing_key_for(std::optional<std::string> id) const;

    // Mount once per preset id per process; idempotent. Sets `ctx.scope` to the
    // session leaf, parents it to the standing key, and records `ctx` in the
    // leaf map under `ctx.agent`. 26 §4.3.8 (:759).
    void mount(AgentContext& ctx, std::optional<std::string> id);

    // Child joins the parent's LIVE composition; returns the preset id for the
    // session header. 26 §4.3.8 (:761).
    [[nodiscard]] std::string compose_from(AgentContext& child, AgentContext& parent);

    // The preset id the live chain runs (42-I4). 26 §2.4.1 `composedPreset`.
    [[nodiscard]] std::string composed_preset(const AgentContext& ctx) const;

    // The effective config (the parsed `presets.*` block). Read by the
    // delegation tool's depth pre-flight (§3.4) so `max_depth` has one owner.
    [[nodiscard]] const PresetConfig& config() const noexcept;

    // The `Agent& -> AgentContext` bridge (42-D15): the live leaf for an agent.
    // Throws UnknownAgent when the agent was never mounted. Reads the leaf map
    // populated by `mount`/`apply_child_composition`.
    [[nodiscard]] AgentContext leaf_for(AgentId agent) const;

    // Blank-session-only switch; appends `agent_preset/selected` after commit.
    // Resolves the leaf via `leaf_for` and the `Session` via `sessions_`.
    // Throws CompositionFixed when the session is not blank (42-F4) and
    // UnknownAgent when the agent is unmounted.
    void select(Agent& agent, const std::string& preset);

    // 42-D15: the §4.3.8 free function is a roster member in ymh because `Agent`
    // exposes no scope; it resolves the parent's live leaf via `leaf_for`.
    // One call; "child with no join" is unrepresentable (26-F9, :1141-1142).
    void apply_child_composition(AgentContext& child, Agent& parent,
                                 const ChildComposition& composition);

    // The blank predicate (42-I3). Pure over the log.
    [[nodiscard]] static bool is_blank(const Session& session);

private:
    // preset id -> standing ScopeKey (42-I1).
    std::unordered_map<std::string, ScopeKey>     standing_;
    // AgentId -> live leaf AgentContext (42-I2, 42-D15).
    std::unordered_map<std::string, AgentContext> leaves_;
    SystemPrompt&   prompt_;
    ToolRegistry&   tools_;
    SkillCatalog&   skills_;
    SessionManager& sessions_;
    PresetConfig    config_;
};
```

Four refinements over the §4.3.8 sketch are load-bearing and called out:

- `standing_key_for` and `composed_preset` are added. The sketch's
  `mount`/`compose_from` cannot be implemented or tested without a way to
  name the standing mount and to read the live preset, and `26-dsh-alignment.md`
  §2.4.1 lists both as roster methods (`:1030-1032`). The sketch is a minimum,
  not a maximum.
- **The roster takes `SessionManager&` and keeps the leaf map** (42-D15). The
  §4.3.8 sketch's constructor lists only `SystemPrompt`/`ToolRegistry`/
  `SkillCatalog`/config, but `select` must test `is_blank(Session)` and append
  `AgentPresetSelected`; it cannot do either without the session seam. The
  roster resolves the `Session` through `SessionManager::sessionPtr`
  (`include/ymh/session/session_manager.hpp:68`) and appends through
  `Session::append` (`include/ymh/session/session.hpp:256-264`). No separate
  `EventBus` is needed: `Session::append` already publishes the committed event.
- **`apply_child_composition` is a roster member, not the §4.3.8 free
  function** (42-D15). The sketch's signature is `apply_child_composition(
  AgentContext&, Agent& parent, const ChildComposition&)`, but ymh's `Agent`
  exposes no scope (`include/ymh/agent/agent.hpp:129-149` has only
  `id()`/`session()`), so a free function cannot obtain the parent's live
  composition. As a member it calls `leaf_for(parent.id())` and then
  `compose_from(child, parent_ctx)`. The one-call atomicity (42-I5) is
  unchanged; only the linkage is pinned.
- `select` throws a typed `CompositionFixed` rather than silently no-op'ing.
  This follows `26-I7` ("fail loud over silent degradation"). The caller (the
  UI command path) turns it into a status-bar notice; it never mutates the log.

`SystemPrompt` gains exactly one method, the scope-mount rule Wave 3 deferred
(`36-prompt-registry.md:854-858`):

```cpp
// include/ymh/prompt/system_prompt.hpp (Wave-5 extension)

class ScopeHandle { /* move-only RAII; unregisters on destruction */ };

// Register a child scope keyed by `key`, parented to `parent`. A duplicate key
// in the same parent throws (36 §2.1). Returns a handle whose lifetime bounds
// the scope's registrations. The scope's registrations are visible only to
// agents whose leaf chain passes through it.
[[nodiscard]] ScopeHandle SystemPrompt::scope(ScopeKey parent, ScopeKey key);
```

`AssembleContext.scope` (`36-prompt-registry.md:118-121`) becomes the leaf
`ScopeKey`; `assemble` resolves the chain to the root and applies shadowing.
The `36` field is unchanged in type; only its "always absent" rule
(`36-prompt-registry.md:104-110`) is lifted.

### 3.3 The child composition

```cpp
// include/ymh/agent/preset.hpp

// 26 §4.3.8 (:766-769). Unchanged.
struct ChildComposition {
    std::optional<std::string>     persona;
    std::optional<ToolRestriction> tool_filter;
};

// 42-D15: roster member, not the §4.3.8 free function (see §3.2). One call;
// "child with no join" is unrepresentable (26-F9, :1141-1142). Reads the
// parent's LIVE scope chain via `leaf_for(parent.id())`.
void AgentPresetRoster::apply_child_composition(AgentContext& child, Agent& parent,
                                                const ChildComposition& composition);
```

**The parent bridge (42-D15).** `apply_child_composition` obtains the parent's
live composition in exactly one way: it looks up `parent.id()` in the roster's
`AgentId -> AgentContext` leaf map (`leaf_for`, §3.2), copies the returned
context into a local (`AgentContext parent_ctx = leaf_for(parent.id());`), and
passes that local to `compose_from` (which takes a non-const `AgentContext&`).
The leaf map is populated by `mount` for a root agent and by
`apply_child_composition` itself for a child (step 1 below), so every agent that
can delegate has a live leaf. A `parent.id()` absent from the map throws
`UnknownAgent` (42-F15); it never falls back to a header-derived or
standing-mount-only scope, which would silently join the wrong composition.

`apply_child_composition` performs, atomically:

1. **Join.** `compose_from(child, parent_ctx)` (with `parent_ctx` the local copy
   from `leaf_for(parent.id())`) parents the child leaf to the parent's live
   chain and records the child in the leaf map. This is the step whose omission
   produces an empty tool registry (`26-F9`). Because the function is one call,
   a caller cannot join without also applying the child's shadowing rows.
2. **Delegation-scope statement.** Register the fixed runtime-context statement
   (42-D9) as a leaf-scoped runtime context, so it is durable and user-role
   (`36-prompt-registry.md:859-862`). The statement text is pinned verbatim in
   §3.5.
3. **Persona shadow.** If `composition.persona` is set, register a
   leaf-scoped `deployment:persona-prefix` shadowing the preset's and the
   global persona.
4. **Tool restriction.** If `composition.tool_filter` is set, register a
   leaf-scoped filter that is **intersected** with the preset's filter. Deny
   wins; a child can only narrow, never widen (42-I7, `26-I8`).
5. **Skill inheritance.** The child inherits the parent's skill set (D17,
   `26-dsh-alignment-part2.md:1085`). No extra call is needed: the join in
   step 1 brings the parent's scoped skill roots, and the child's own
   `ChildComposition` carries no skill field.

### 3.4 Depth and creation

The child is created through the existing `AgentRegistry::create`
(`include/ymh/agent/agent_registry.hpp:41`) with a `SessionOptions`
(`include/ymh/session/session_manager.hpp:29-36`) that now carries depth:

```cpp
struct SessionOptions {
    std::filesystem::path    cwd;
    std::string              serverProfile;
    std::string              model;
    std::string              title;
    SessionKind              kind          = SessionKind::Root;
    std::optional<SessionId> parentSession = std::nullopt;
    std::uint32_t            depth         = 0;   // NEW (26-D17); root 0, spawn parent+1
    std::optional<std::string> agent_preset;      // NEW (26-D16); header's start preset
};
```

The delegation tool (the Wave-5 in-process spawn/fork path) enforces the depth
limit **before** `create`:

```cpp
// The delegation tool's pre-flight. Returns nullopt when delegation is allowed.
[[nodiscard]] std::optional<AgentError>
check_delegation_depth(std::uint32_t parent_depth, std::uint32_t max_depth);
```

Rules (42-I6, 42-D8, 42-D18):

- Root sessions are depth `0`.
- A spawned child is `parent.depth + 1`. A forked session **inherits** the
  source's depth (a fork is the same agent lineage, not a delegation).
- Delegation is refused when `child_depth > max_depth`. `max_depth == 0`
  forbids delegation entirely (`26-dsh-alignment.md:1094`).
- `max_depth` is `PresetConfig::max_depth` (the `presets.max_depth` key,
  §2.5/§3.1), default `3` (`26-dsh-alignment.md:1094`). It is the single source
  of truth for the delegation tool's pre-flight; there is no second copy on the
  tool config (42-D18). A non-integer or negative value fails config load loud
  (`26-I7`).
- Depth is **monotone**: it is written once at creation and never decreases.
  It is persisted (see §4.3) so resume/fork observe the same value.

### 3.5 The fixed delegation scope

A delegated child receives, at composition time, a scope that **cannot be
widened from inside** (`26-I8`: "A child's scope is fixed and non-widenable"):

- **Approval policy `never`.** The child's permission policy is fixed to the
  deny-requiring-approval behavior (`26-dsh-alignment.md:1095`). An operation
  that would need approval is rejected automatically; the child cannot retry
  its way to a grant.
- **Fixed sandbox/execution scope.** The child's `ExecutionEnvironment` root is
  the same workspace root as the parent's, resolved once through
  `ExecutionEnvironment::resolve()` (`AGENTS.md` path-safety rule). The child
  cannot `chdir` or re-root.
- **The fixed runtime-context statement.** Pinned verbatim from the
  authoritative shipped source (`dsh-subagent/lib/index.js:519`, quoted at
  `26-dsh-alignment.md:1099-1101`):

  ```markdown
  You are a delegated subagent: your permission scope was fixed when you were started and cannot be widened from inside this session — operations that require approval are rejected automatically. When the task needs access beyond that scope, do not retry the denied operation; state the limitation in your reply so the delegating agent can handle it.
  ```

  The README prints a divergent wording ("the job needs access beyond that
  scope"); the source is authoritative (`26-dsh-alignment.md:1103-1105`). This
  spec pins the source wording. The statement is registered as a leaf-scoped
  runtime context, so it appears in the child's rendered prompt and is
  replay-verifiable.

---

## 4. The session header and the `23` contract break

### 4.1 The new fields

`SessionHeader` currently ends at `metadata`
(`include/ymh/session/session.hpp:44-58`; `01-session.md:160-173`). Wave 5 adds
two fields:

```cpp
struct SessionHeader {
    /* ... existing fields ... */
    std::optional<std::string> metadata;       // opaque JSON extension bag; no schema (01 §3)
    std::optional<std::string> agent_preset;   // NEW (26-D16): the preset the session STARTED with
    std::uint32_t              depth = 0;      // NEW (26-D17): monotone delegation depth
};
```

Semantics (`26-dsh-alignment.md:1056-1057`): "The creation header names the
preset a session started with; the `agentPreset` projection names the one it
runs." So `agent_preset` is **the start preset**, frozen at creation. A
blank-session switch does **not** rewrite it; it appends
`agent_preset/selected`, and the running preset is
`composed_preset(ctx)` / the last selection event (42-I4).

`agent_preset == nullopt` means the session predates presets (a legacy session)
or was created with no default configured. It is **not** synthesized on read
(42-F13).

`depth` is the monotone delegation depth (`§3.4`): root `0`, spawn
`parent + 1`, fork inherits. It is persisted (see §4.3) so resume/fork observe
the same value.

### 4.2 Lifecycle

- **Create.** `SessionManager::createSession` copies
  `SessionOptions.agent_preset` and `SessionOptions.depth` into the header,
  exactly as it already copies `kind`/`parentSession`
  (`23-session-lifecycle-errata.md:858-864`; `43-wave5-dependency-errata.md`
  §2). The roster resolves `nullopt` to the configured default before the copy,
  so a preset-enabled workspace never writes `nullopt`.
- **Resume.** The header's `agent_preset` names the standing mount to rejoin.
  If the session's log carries a later `agent_preset/selected`, the resumed
  agent joins the **selected** preset's standing mount, and `composed_preset`
  reports it. The header is not rewritten.
- **Fork.** The fork joins the parent's live composition at fork time (the
  same rule as `compose_from`), records the resulting id in its own header, and
  inherits the parent's depth.
- **Subagent spawn.** `apply_child_composition` joins the parent's live chain;
  the child header's `agent_preset` is the parent's `composed_preset`.
- **Attach (M2).** The supervisor receives the header over the socket
  unchanged. `agent_preset` is a header attribute, not a filter, so `/sessions`
  and the live-only switcher rules (`22-switcher-sessions-errata.md`) are
  untouched (`26-dsh-alignment-part2.md:1087`).

### 4.3 Storage (no DDL) — OQ-1 resolved

The typed fields are persisted in the **existing** `sessions.metadata JSON`
column (`src/session/session_persistence.cpp:47`), not in new columns
(`26-dsh-alignment-part2.md:1170-1189`; `30-architecture-cascade-errata.md:176-180`).
No structural DDL change occurs, so `kSchemaVersion` stays `1`, `migrate_fresh`
is unchanged, and there is no `migrate_v1_to_v2`
(`26-dsh-alignment-part2.md:1170-1179`).

**The encoding (42-D19).** The `sessions.metadata` column is a JSON object. The
header codec owns a small **reserved-key namespace** inside it:

```jsonc
{ "agent_preset": "standard",   // reserved: SessionHeader.agent_preset
  "depth": 0,                   // reserved: SessionHeader.depth
  "<reserved opaque key>": { }  // the existing SessionHeader.metadata string,
                                // stored verbatim under a reserved key
}
```

- `SessionHeader.agent_preset` and `SessionHeader.depth` are **typed C++ fields**
  (§3.4/§4.1). The codec writes them as the reserved keys `agent_preset` and
  `depth` of the `sessions.metadata` object and reads them back into the typed
  fields.
- The existing `SessionHeader.metadata` remains an **opaque JSON string** with no
  schema commitments (`01-session.md:172`, `:188-190`): the codec stores that
  string verbatim under a reserved key and strips the reserved keys out of it on
  read, so a consumer of `metadata` still sees only its own opaque keys and
  `metadata` stays inert (`01` I23). It is still not part of the projection.
- This is why `26 §4.4`/`30 §S3` ("the frozen header field list grows") and
  `26 §4.6` ("ride the existing `sessions.metadata JSON` column") are
  **compatible**: the *C++ field list* grows while the *DDL* does not. The
  amendment to `01`'s "no schema" sentence — the field is opaque to consumers,
  while the header codec owns the reserved keys — is pinned in
  `43-wave5-dependency-errata.md` §1 and is a Wave-5 gate (OQ-1 resolved).

The rejected alternative (keep `agent_preset` only as a key inside the opaque
blob, with no typed C++ field) is recorded in §12 (OQ-1): it leaves the field
list unchanged, contradicting `26 §4.4` and `30 §S3`.

### 4.4 The break, stated

This is a **breaking change to `01`'s frozen `SessionHeader` field list**,
classified `Brk. (session header)` (`26-dsh-alignment-part2.md:154`,
`26-dsh-alignment-part2.md:1088`; `30-architecture-cascade-errata.md:171-174`).
The owning errata are `01` (the field and its codec) and `23` (the creation
path and lifecycle), pinned in `43-wave5-dependency-errata.md` §1/§2 and gated
in Wave 0 Stage B (`26-dsh-alignment-part2.md:1394`). Lifecycle rules (lease,
resume, fork) are preserved; only the field list grows.

---

## 5. Events

### 5.1 `agent_preset/selected`

`26-dsh-alignment-part2.md` §4.3.9 pins the type, wire name, and payload:

| EventType | `wire_name` | Payload | Notes |
|---|---|---|---|
| `AgentPresetSelected` | `agent_preset/selected` | `payload::AgentPresetSelected` | appended after commit (`:844`) |

```cpp
// include/ymh/session/events.hpp
namespace payload {
// dsh agent-preset/selected (dsh-agent-presets session.d.ts:25-29).
struct AgentPresetSelected { std::string agent_preset; };   // 26 §4.3.9 (:891)
} // namespace payload
```

JSON keys (`26-dsh-alignment-part2.md:954`): `agent_preset` string, and
nothing else. The event envelope carries `event_id` and `timestamp`; the
payload adds no id or timestamp (`26-dsh-alignment-part2.md:942-944`).

The wire name is the **slash/underscore form** (`agent_preset/selected`), not
dsh's hyphenated `agent-preset/selected`; the divergence is deliberate and only
the semantics are copied (`26-dsh-alignment-part2.md:832-835`). The CamelCase
enum value is `AgentPresetSelected`.

### 5.2 Append rule and consumers

**Append site.** `AgentPresetRoster::select` appends exactly one
`AgentPresetSelected` — through `Session::append` on the agent's session — after
the scope swap commits (42-I9, 42-F11). No other path appends it. The event is
**durable** and lives in the session log.

**Consumer obligations** are owned by the `01` event-family errata and are
already recorded in the `29` consumer matrix
(`29-event-family-errata.md:390`, `26-dsh-alignment-part2.md:991`):

| Consumer | Obligation |
|---|---|
| `deriveMessages` (`src/session/session.cpp:363`, switch at `:376`) | explicit ignore (metadata); this switch has **no `default:`**, so a missing case is a hard `-Werror=switch` build break (`26-dsh-alignment-part2.md:818`, `:1002-1005`) |
| transcript / UI (`ui_event_adapter`, `session_export`, `session_cli`) | preset row; these switches have `default:`, so a missing case is a silent drop (`26-dsh-alignment-part2.md:819-822`, `:1006-1010`) |
| compaction & token accounting | excluded; not model-visible content (`26-dsh-alignment-part2.md:991`) |

`AgentPresetSelected` is user-visible, so rule 2 of
`26-dsh-alignment-part2.md:1006-1010` applies: the four `default:`-bearing
switches must render it explicitly or record why not.

### 5.3 No `tools/change` event

ymh does **not** add dsh's `tools/change` event. dsh emits it on a committed
preset switch because the preset decides the tool schemas the model sees
(`26-dsh-alignment.md:1053-1056`). In ymh, the catalog change is already
captured durably by the next `LlmRequestHeader`'s `tool_names` and
`tool_schema_digests`, which start a new request series on any change
(`26-D2`, `26-I6`, `26-dsh-alignment-part2.md:1133-1136`). Adding a second
record of the same change would create two sources of truth. This is 42-D6.

---

## 6. Invariants

These extend `26-I1`..`26-I12` and `36-I1`..`36-I12`. `42-I*` are local to this
spec.

- **42-I1 (one standing mount).** For each preset id, at most one standing
  composition exists per process. `mount` is idempotent. Guard: the roster's
  `preset id -> standing ScopeKey` map; a second mount registers nothing twice
  (42-F2).
- **42-I2 (exactly one live chain).** Every agent's `AgentContext.scope` is a
  leaf with exactly one parent chain ending at a standing mount, and the roster
  holds exactly one leaf per `AgentId` in its `AgentId -> AgentContext` map. No
  agent is scope-less once mounted. Guard: `mount`/`apply_child_composition`
  record the leaf; the assembler resolves the chain; `leaf_for` is the only
  bridge from `Agent&` to the live leaf (42-D15).
- **42-I3 (blank-only switch).** `select` succeeds only when
  `is_blank(session)` holds, with the predicate pinned in §2.3 and the session
  resolved through `SessionManager::sessionPtr`. After the first message or tool
  call the composition is fixed for the session's life. Guard: the predicate is
  the single test; a non-blank select throws and appends nothing (42-F4).
- **42-I4 (header vs projection).** `SessionHeader.agent_preset` is the
  **start** preset, frozen at creation. The **running** preset is
  `composed_preset(ctx)`, equivalently the header when no selection event
  exists, else the last `agent_preset/selected`. Guard: `select` never rewrites
  the header; `composed_preset` reads the live leaf.
- **42-I5 (atomic child composition).** `apply_child_composition` joins the
  parent's live chain and applies the child's persona/tool shadow in **one
  call**. A child composed without a join is unrepresentable (`26-F9`). Guard:
  the API shape; the join is the first step and cannot be skipped.
- **42-I6 (monotone depth).** Depth is written once at creation: root `0`,
  spawn `parent + 1`, fork inherits. It never decreases. Delegation is refused
  when the child depth would exceed `max_depth`; `max_depth == 0` forbids
  delegation. Guard: the pre-flight in §3.4 and the persisted value (42-F6).
- **42-I7 (fixed delegation scope).** A child's permission scope is fixed
  (approval `never`), its execution root is the parent's workspace root, and
  the fixed delegation statement is present in its rendered prompt. A child can
  only **narrow** a tool restriction, never widen it (`26-I8`). Guard: the
  intersection rule in §3.3 and the statement registration (42-F7).
- **42-I8 (deterministic composition).** Two mounts of the same preset with
  identical inputs register identical sections/tools/skills. Rows sort
  canonically; a disabled row registers nothing; an unknown row target fails
  loud (`26-I7`). Guard: golden rendered-prompt hashes per preset (42-F8,
  42-F9).
- **42-I9 (append-after-commit).** `agent_preset/selected` is appended only
  after the scope swap commits. A rejected switch appends nothing. Guard: the
  ordered steps in §2.3 (42-F11).
- **42-I10 (no schema/protocol bump).** The preset id and depth ride the
  existing `sessions.metadata` column; `kSchemaVersion` stays `1` and
  `kProtocolVersion` stays `1` (`26-I12`, `26-dsh-alignment-part2.md:1170-1189`).
  Guard: the DDL is byte-identical; the wire envelope is unchanged.
- **42-I11 (UI is a consumer).** No roster, scope, preset, or composition type
  appears in the UI layer, and no UI type enters the roster (`26-I10`,
  `00 §4.1`, `06 A17`). The UI reads `AgentPresetSelected` rows and calls
  `select` through the command surface only.
- **42-I12 (single-threaded roster).** The roster is owned by
  `WorkspaceRuntime` and called only on the agent executor thread, inheriting
  `36`'s contract (`36-prompt-registry.md:153-154`, `26-I10`). It is not
  thread-safe by design; concurrent `mount`/`select` from two threads is
  undefined and no lock is provided (42-F14).

---

## 7. Failure modes

### 7.1 Shared findings (`F1`..`F12`, `§54`)

This spec is subject to the project's shared failure taxonomy
(`00-architecture.md` §54). The relevant shared findings are: F1 (silent
degradation), F2 (state divergence between two sources of truth), F5 (unsafe
path resolution), F7 (unbounded growth), F9 (cross-session leakage), and F11
(orphaned identity). Every `42-F#` below names the shared finding it is an
instance of.

### 7.2 Component-local failure modes

- **42-F1: unknown or absent preset.** A resolution of an unknown id, or of
  `nullopt` with no configured default, silently yields an empty composition.
  **Guard:** `resolve` throws `PresetNotFound`; there is no implicit empty
  preset (§2.1). Instance of F1.
- **42-F2: sibling leak / double mount.** A preset mounted twice registers its
  sections and tools twice, so a sibling sees another preset's rows. **Guard:**
  42-I1; the standing map makes `mount` idempotent; each leaf parents exactly
  one standing key. Instance of F2, F9.
- **42-F3: child composed without the join.** A child is created with no
  parent join, so it sees an empty tool registry and none of the parent's
  prompt sections. **Guard:** 42-I5; `apply_child_composition` is one call
  (`26-F9`, `26-dsh-alignment-part2.md:1141-1142`). Instance of F1.
- **42-F4: switch after the first message.** A preset switch after the session
  produced a message leaves logged tool calls the new composition cannot make.
  **Guard:** 42-I3; `select` tests the blank predicate and throws
  `CompositionFixed`, appending nothing. Instance of F2.
- **42-F5: header/projection divergence read as a bug.** A reader assumes the
  header names the running preset after a blank-session switch. **Guard:** 42-I4;
  children and projections read the live chain; the header is documented as the
  start preset (`26-dsh-alignment.md:1086-1089`). Instance of F2.
- **42-F6: depth overflow or non-monotone depth.** A child is spawned past
  `max_depth`, or a resumed/forked child's depth is recomputed to a lower
  value, enabling unbounded recursion. **Guard:** 42-I6; the pre-flight refuses
  over-depth delegation, depth is persisted and never decreases. Instance of
  F7, F11.
- **42-F7: child widens its scope.** A child retries a denied operation, or
  acquires an approval the parent did not have. **Guard:** 42-I7; approval is
  fixed to `never`, the execution root is fixed, the delegation statement is
  present, and tool restrictions only narrow (`26-I8`). Instance of F9.
- **42-F8: duplicate or unknown preset row.** Two roots define the same preset
  id, or a preset file carries an unknown key / a missing or duplicate
  `rows[].id`, and the loader silently keeps one. **Guard:** 42-I8 and the
  loader validation in §2.1.1; a duplicate id, an unknown key, and a
  missing/duplicate row id all fail at load (`26-I7`). Instance of F1.
- **42-F9: non-deterministic composition.** Two mounts of the same preset
  produce different section order or tool order. **Guard:** 42-I8; canonical
  `(order, name)` sort (`36-I1`) and golden rendered-prompt hashes. Instance of
  F2.
- **42-F10: cross-preset inheritance.** A child inherits a sibling preset's
  registrations instead of its parent's chain. **Guard:** 42-I2; `compose_from`
  reads the parent's live leaf, not the standing mount's global set. Instance
  of F9.
- **42-F11: event before commit.** `agent_preset/selected` is appended before
  the swap commits, so a replayed log names a preset that did not run. **Guard:**
  42-I9; the append is the last step. Instance of F2.
- **42-F12: dropped event row.** `AgentPresetSelected` is missing from a
  consumer switch: a hard build break in `deriveMessages`, a silent drop in the
  four `default:` switches. **Guard:** the `29` consumer matrix row
  (`29-event-family-errata.md:390`) plus `-Werror=switch` and the
  `all_event_types()` round-trip test (`26-dsh-alignment-part2.md:1002-1005`).
  Instance of F1.
- **42-F13: legacy session with no preset.** A pre-Wave-5 session is read and
  a preset id is synthesized, misreporting its composition. **Guard:** 42-I4;
  `agent_preset == nullopt` is preserved and never synthesized; a preset-enabled
  workspace resolves the default only at creation. Instance of F2.
- **42-F14: concurrent roster mutation.** Two threads call `mount`/`select`
  concurrently. **Guard:** 42-I12; the roster is single-threaded by contract
  and provides no lock; the Wave-5 test asserts the single-thread contract
  rather than attempting thread safety. Instance of F2.
- **42-F15: unmounted agent.** `select` or `apply_child_composition` is called
  for an `AgentId` absent from the roster's leaf map, so the parent's live
  composition cannot be resolved. **Guard:** 42-D15; `leaf_for` throws
  `UnknownAgent`; there is no fallback to a header-derived or standing-only
  scope, which would silently join the wrong composition. Instance of F1, F2.

---

## 8. dsh mapping

| ymh | dsh source | Notes |
|---|---|---|
| `AgentPresetRoster` | `dsh-agent-presets` roster service (`lib/types/index.d.ts:60-383`; `26-dsh-alignment.md:1030-1032`) | `list`/`resolve`/`mount`/`composeFrom`/`composedPreset`/`select`/`standingKeyFor` map 1:1; `standing_key_for`/`composed_preset`/`leaf_for` added per §3.2 |
| `AgentPreset` + `PresetRow` | preset directory + `agent.cordis.yml` rows (`26-dsh-alignment.md:991-1001`) | JSONC, not Cordis (`26-dsh-alignment-part2.md:1621-1624`); `isolate`/`inject` have no ymh analogue |
| standing mount | "One standing composition per preset" (`dsh-agent-presets/README.md:99`; `26-dsh-alignment.md:1033-1037`) | one per preset id per process |
| per-session joined scope | "the scoped layers joined by each session make the effective composition per-session" (`26-dsh-alignment.md:1045-1047`) | `ScopeKey` chain; 42-I2 |
| blank-session-only switch | `dsh-agent-presets/README.md:81` (`26-dsh-alignment.md:1049-1053`) | stricter predicate; 42-D4 |
| `AgentPresetSelected` | dsh `agent-preset/selected` (`dsh-agent-presets session.d.ts:25-29`; `26-dsh-alignment-part2.md:890-891`) | ymh wire name `agent_preset/selected`; append after commit |
| (no `tools/change`) | dsh emits `tools/change` on a committed switch (`26-dsh-alignment.md:1053-1054`) | ymh records the change via the next `LlmRequestHeader`; 42-D6 |
| `apply_child_composition` | `applyChildComposition` (`dsh-subagent/lib/types/child-agent.d.ts:84-106`; `26-dsh-alignment.md:1070-1077`) | one call; join + persona + filter; roster member (42-D15) |
| `ChildComposition` | dsh `ChildComposition { persona?, toolFilter? }` (`26-dsh-alignment.md:1073-1076`) | identical shape |
| `max_depth` | `maxDepth` (default 3; 0 forbids) (`26-dsh-alignment.md:1094`) | 42-I6, 42-D18; ymh key `presets.max_depth` |
| fixed delegation scope | `approvalPolicy: 'never'` + fixed runtime statement (`26-dsh-alignment.md:1095-1101`) | source wording is authoritative |
| child skill inheritance | D17 (`26-dsh-alignment-part2.md:1085`) | via the join; no separate field |
| `SessionHeader.agent_preset` | creation header names the start preset (`26-dsh-alignment.md:1056-1057`) | the `23` break; typed field persisted as a `metadata` reserved key (42-D19) |

---

## 9. Dependencies

Wave 5 has one hard spec dependency and four cross-spec contracts that must be
pinned by errata before any Wave-5 code. The four contracts are **pinned in
`43-wave5-dependency-errata.md`** (authored with this rev, numbered 43 because
31–42 are taken); that file is the owning artifact for each and is a **blocking
prerequisite** for Wave-5 code, per the Wave-5 Stage-B gate
(`26-dsh-alignment-part2.md:1394`) and the design-first rule (`AGENTS.md`).

**Hard dependency.**

1. `36-prompt-registry.md` (Wave 3). Sections, persona, the
   `AssembleContext.scope` seam, and the `SystemPrompt` extension point. §1.5.
   Verified per `DESIGN_STATUS.md:55`.

**Blocking prerequisite errata (pinned in `43-wave5-dependency-errata.md`).**
Each row names the exact contract that must be verified before Wave-5 code; the
contract is pinned in `43`, not merely referenced here.

| Prereq | Target | Exact contract to pin | 43 § |
|---|---|---|---|
| PR-1 | `01-session.md` | the `SessionHeader.agent_preset`/`depth` typed fields; the reserved-key `metadata` encoding (§4.3) and the `01` "no schema" amendment; the `AgentPresetSelected` `EventType`/`wire_name`/`SessionEventMap`/`EventTraits`/`to_json`/`from_json` (`{agent_preset}` only); the `deriveMessages` case and the `all_event_types()` round-trip | §1 |
| PR-2 | `23-session-lifecycle-errata.md` | the creation-path copy: `SessionOptions` gains `agent_preset`/`depth`; `SessionManager::createSession` copies them into the header and `validateHeader` accepts them; the header read/write path carries the reserved keys | §2 |
| PR-3 | `21-config-jsonc-errata.md` | the `presets.root`/`default`/`include_shipped_root`/`include_user_root` keys plus `presets.max_depth` (int >= 0, default 3), their types/defaults, and the `PresetConfig` parse | §3 |
| PR-4 | `06-agent-loop.md` | the additive child-creation seam: `AgentServices` gains `AgentPresetRoster* presets`; the delegation tool calls `apply_child_composition` with the child's `ChildComposition` before `registry.create`; the depth pre-flight reads `PresetConfig::max_depth` | §4 |

**Consumed as verified contracts (no errata required unless noted):**

- `26-dsh-alignment.md` (verified) §2.4.1/§2.4.2: the design source.
- `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS): the pinned
  interfaces, events, config, and wave scope.
- `29-event-family-errata.md`: the event-family contract and the
  `AgentPresetSelected` consumer row (`:390`). **Verified** per
  `DESIGN_STATUS.md:41`; its in-file status block (`:4`) is stale (OQ-2
  resolved). The `01` payload codec remains PR-1 because `26 §4.3.9`
  (`:826-830`) delegates the `to_json`/`from_json` bodies to the `01` errata;
  `29 §4.4` (`:390`) only reserves the row for Wave 5.
- `30-architecture-cascade-errata.md` §3: S3/S4 cascade.
- `24-agent-lifetime-errata.md`: lifetime invariants are preserved
  (`26-dsh-alignment-part2.md:1089`); depth is an additive persisted attribute,
  so **no `24` errata is required**.
- `20-skills.md`: the catalog contract is unchanged; D17 is additive
  (`26-dsh-alignment-part2.md:1085`).
- `07-tools-execution.md`: `ToolRegistry::schemas()`
  (`include/ymh/tools/tool_registry.hpp:90`) is consumed read-only by the
  scope-scoped filter; no `07` contract change (the continuable-control-tool
  surface is a separate future errata, 42-D16).
- `02-persistence.md`: no DDL change; the metadata column already exists
  (`src/session/session_persistence.cpp:47`).

---

## 10. Decision register

| ID | Decision | Class | Rationale |
|---|---|---|---|
| 42-D1 | **Preset format is JSONC directories**, not Cordis/`agent.cordis.yml`. A preset is a directory with a composition file; `PresetRow` is the ymh row shape. | New | `26 §5.1` (`:1505-1506`), `26 §6` OQ8 (`:1621-1624`). |
| 42-D2 | **One standing mount per preset id per process; per-session join by scope parentage.** The per-session behavior is a property of the scope, not a second mount. | New | `26-dsh-alignment.md:1033-1047`. |
| 42-D3 | **Activate `36`'s reserved `AssembleContext.scope`** with a scope chain (`global -> preset:<id> -> session:<sid>`) and cross-layer shadowing; `SystemPrompt::scope` is the mount rule Wave 3 deferred. | Add. (`36` errata) | `36-prompt-registry.md:104-110`, `:854-858`. |
| 42-D4 | **Blank predicate** is the absence of `UserMessage`, `AssistantMessage`, `ToolCall`, `ToolResult`, `ContextInjected`, `ContextCompaction`, `SubagentSpawned`, `SubagentFanIn`, `LlmRequestHeader`. | New | `26-dsh-alignment.md:1049-1053`; stricter than dsh's prose, matching its rationale. |
| 42-D5 | **`SessionHeader.agent_preset`** is a typed field naming the start preset; it is not rewritten by a switch. | Brk. (`01`, `23`) | `26 §4.4` (`:1088`), `30 §S3` (`:164-174`). |
| 42-D6 | **`agent_preset/selected` only; no `tools/change`.** The next `LlmRequestHeader` records the catalog change. | New | `26-D2`/`26-I6`; avoids two sources of truth. |
| 42-D7 | **`apply_child_composition` is atomic** and reads the parent's live chain. | New | `26-F9` (`:1141-1142`), `26-dsh-alignment.md:1080-1089`. |
| 42-D8 | **Depth is monotone**: root 0, spawn parent+1, fork inherits; default `max_depth` 3; 0 forbids. | Add. | `26-dsh-alignment.md:1094`; `26 §4.4` (`:1089`). |
| 42-D9 | **Fixed delegation scope**: approval `never`, fixed execution root, and the verbatim source statement as a leaf-scoped runtime context. | New | `26-I8`, `26-dsh-alignment.md:1095-1105`. |
| 42-D10 | **Tools are selected by a scope-scoped `ToolRestriction` over the host registry**, not by preset-owned registrations. | New | `26-dsh-alignment.md:1000-1003`, `:1013-1015`. |
| 42-D11 | **Children inherit the parent's skill set** via the join; no child skill field. | Add. | D17 (`26-dsh-alignment-part2.md:1085`). |
| 42-D12 | **`presets.root`/`default`/`include_shipped_root`/`include_user_root`** are the config surface. | Add. (`21`) | `26 §4.9` (`:1320-1323`). |
| 42-D13 | **Resume/fork/attach**: resume rejoins the header's start preset (or the last selected); fork joins the parent's live chain and inherits depth; attach passes the header unchanged. | New | `26-dsh-alignment.md:1056-1057`, `:1086-1089`. |
| 42-D14 | **No `kSchemaVersion`/`kProtocolVersion` bump**; preset id and depth ride the existing `metadata` column. | Add. | `26 §4.6` (`:1170-1189`), `26-I12`. |
| 42-D15 | **The `Agent& -> AgentContext` bridge.** The roster takes `SessionManager&` and keeps an `AgentId -> AgentContext` leaf map (`leaf_for`); `select` resolves the session via `sessionPtr` and appends via `Session::append`; `apply_child_composition` is a **roster member**, not the §4.3.8 free function, because `Agent` exposes no scope. | Add. (refines `26 §4.3.8`) | `agent.hpp:129-149`; `session_manager.hpp:68`; `session.hpp:256-264`; `26-F9` (`:1141-1142`). |
| 42-D16 | **Continuable subagents / control tools are out of Wave 5**; owned by a future additive errata to `07-tools-execution.md`. | Out of scope | `26-D17` (`26-dsh-alignment-part2.md:155`); Wave-5 bullets (`:1487-1488`). |
| 42-D17 | **The preset-file schema and loader are pinned by this spec** (§2.1.1): the JSONC directory + `preset.jsonc` format, the row vocabulary, and load-time validation; the loader is the roster's, not `21`'s. | New (resolves OQ-5) | `26 §6` OQ8 (`:1621-1624`). |
| 42-D18 | **`presets.max_depth`** (int >= 0, default 3) is the depth knob, carried on `PresetConfig`; one source, read by the delegation pre-flight. | Add. (`21`) | `26-dsh-alignment.md:1094`; `26 §4.9` (`:1320-1323`). |
| 42-D19 | **Header storage encoding.** Typed `agent_preset`/`depth` ride reserved keys of the `sessions.metadata` JSON object; the opaque `metadata` string stays schema-free to consumers, and the header codec owns the reserved namespace (amends `01`). | Add. (`01`) | `26 §4.6` (`:1174-1175`); `01-session.md:172`,`:188-190`. |

---

## 11. Test plan

The suite follows §44 (unit, integration with fakes, golden TUI render, replay)
and the `26 §5.2` additions. Wave-5 tests use `FakeLLM` and a fake preset root;
no live LLM is required.

### 11.1 Unit tests

1. `resolve(nullopt)` returns the configured default; `resolve(unknown)` throws
   `PresetNotFound`; `resolve(nullopt)` with no default throws (42-F1).
2. `list()` returns the union of the configured roots honoring
   `include_shipped_root`/`include_user_root`; a duplicate id across roots
   fails (42-F8).
3. `standing_key_for(id)` is pure and stable; two calls return the same key.
4. `mount` twice for one id registers once (42-I1, 42-F2): assert the
   `SystemPrompt` section/tool count is unchanged after the second call.
5. Scope-chain resolution: a section registered in the standing mount is
   visible to a leaf; a sibling leaf does not see it (42-F10).
6. Shadowing: a leaf section with the same name as a standing/global section
   wins; duplicate names **within one layer** throw (42-D3).
7. `is_blank` truth table over each event kind in the predicate, plus the
   non-blanking kinds (`SessionStarted`, `SessionRenamed`, `PlanMode`,
   `TokenUsage`).
8. `select` on a blank session swaps the scope and appends exactly one
   `AgentPresetSelected`; `select` on a non-blank session throws
   `CompositionFixed` and appends nothing (42-I3, 42-I9, 42-F4, 42-F11).
9. `ToolRestriction` intersection: deny wins; a child's allow cannot add a
   tool the parent's filter denies (42-I7).
10. Depth arithmetic: root 0; spawn parent+1; fork inherits; `max_depth == 0`
    refuses; child_depth `> max_depth` refuses (42-I6, 42-F6).
11. The `Agent& -> AgentContext` bridge: `leaf_for` returns the mounted leaf and
    throws `UnknownAgent` for an unmounted id; `apply_child_composition` with an
    unmounted parent throws and registers no child (42-D15, 42-F15).
12. Preset-file schema: an unknown key, a missing/duplicate `rows[].id`, a
    duplicate `sections[].name`, and a duplicate preset id across roots each
    fail load (42-D17, 42-F8); a directory with no `preset.jsonc` is skipped.
13. `presets.max_depth` parses into `PresetConfig::max_depth` (default 3); a
    negative or non-integer value fails config load (42-D18).

### 11.2 Integration tests (FakeLLM)

1. **Child sees the parent's tools and sections.** Spawn a child via the
   delegation path, drive one turn with `FakeLLM`, and assert the child's
   `LlmRequestHeader.tool_names` is the parent's set (plus/minus the child
   filter) and the child's rendered prompt contains the parent's standing
   sections (42-I5, 42-F3).
2. **Fixed delegation statement.** The child's first request's rendered prompt
   contains the verbatim statement from §3.5; the child's approval path rejects
   an approval-requiring operation without a grant (42-I7, 42-F7).
3. **Switch while blank.** Create a session, `select` a second preset, then
   prompt: the `LlmRequestHeader` reflects the second preset's tool set, and the
   log carries one `agent_preset/selected` before the header (42-I9).
4. **Switch after the first message is rejected.** Prompt once, `select`, and
   assert `CompositionFixed`; the tool set of the next request is unchanged and
   no event was appended (42-F4).
5. **Header carries the start preset.** Create with `agent_preset = X`, switch
   to `Y` while blank, and assert the header still reads `X` while
   `composed_preset` reads `Y` (42-I4, 42-F5).
6. **Depth limit.** Spawn to `max_depth`, then attempt one more; the attempt
   fails with a typed error and no child session is created (42-F6).
7. **Resume/fork/attach.** Resume a switched session and assert the rejoined
   composition; fork a session and assert the fork's header/depth; attach and
   assert the header crosses the socket unchanged (42-D13).
8. **No schema/protocol bump.** Open the session DB and assert `user_version`
   is `1`; assert `kProtocolVersion` is unchanged (42-I10).

### 11.3 Golden and replay tests

1. **Golden rendered-prompt hashes per preset.** For each shipped preset and a
   fixed config, hash `render_prompt(assemble(ctx))`; two runs must match
   (42-I8, 42-F9; `36` §11.2).
2. **Golden transcript row.** `AgentPresetSelected` renders the pinned preset
   row; the row is present in the transcript, the export, and the CLI output
   (42-F12).
3. **Replay round-trip.** Record a session that switches preset while blank;
   replay it and assert `composed_preset` at every request matches the record,
   and that the `agent_preset/selected` event round-trips through
   `to_json`/`from_json` with only the `agent_preset` key (42-I9,
   `26 §4.3.9.1`).
4. **Event-family completeness.** `all_event_types()` includes
   `AgentPresetSelected`; the wire round-trip covers it; the `deriveMessages`
   switch has an explicit case (build gate, 42-F12).

### 11.4 Failure-mode coverage matrix

| Failure | Test |
|---|---|
| 42-F1 | 11.1(1) |
| 42-F2 | 11.1(4) |
| 42-F3 | 11.2(1) |
| 42-F4 | 11.1(8), 11.2(4) |
| 42-F5 | 11.2(5) |
| 42-F6 | 11.1(10), 11.2(6) |
| 42-F7 | 11.2(2) |
| 42-F8 | 11.1(2), 11.1(12) |
| 42-F9 | 11.3(1) |
| 42-F10 | 11.1(5) |
| 42-F11 | 11.1(8), 11.3(3) |
| 42-F12 | 11.3(2), 11.3(4) |
| 42-F13 | 11.2(7) |
| 42-F14 | asserted by contract (42-I12); no concurrent-mutation test by design |
| 42-F15 | 11.1(11) |

---

## 12. Resolved questions (formerly open)

Rev 2 resolves the five questions Rev 1 recorded. Each is now pinned; the
resolution and the rejected alternative are stated so the decision is auditable.
None is silently resolved: the rejected alternative is named in each case.

- **OQ-1 (resolved by 42-D19, §4.3): typed field + reserved-key encoding.**
  `SessionHeader.agent_preset`/`depth` are typed C++ fields (so `26 §4.4` /
  `30 §S3`'s "field list grows" holds), and they are persisted as reserved keys
  of the `sessions.metadata` JSON object (so `26 §4.6`'s "no new column" holds).
  `01`'s `metadata` stays opaque to consumers because the header codec strips
  the reserved keys from the opaque bag on read; the `01` "no schema" amendment
  is pinned in `43-wave5-dependency-errata.md` §1. **Rejected:** making
  `agent_preset` a pure accessor over the opaque blob — it leaves the C++ field
  list unchanged, contradicting `26 §4.4` and `30 §S3`.
- **OQ-2 (resolved): `29` is verified; `01` still owns the payload codec.**
  `29-event-family-errata.md` is verified (`DESIGN_STATUS.md:41`); its in-file
  status block (`:4`) is stale. `29` owns the event-family contract and the
  consumer row (`:390`), but `26 §4.3.9` (`:826-830`) delegates the
  `to_json`/`from_json` bodies to the `01` errata, so PR-1 (`43 §1`) remains
  required.
- **OQ-3 (resolved by 42-D16, §1.4): out of scope, owner named.**
  `send_message`/`interrupt_agent`/`list_agents` are **not** Wave 5; they are
  owned by a future additive errata to `07-tools-execution.md` (the
  `spawn_subagent` surface). Wave 5 pins only the one-shot composition join.
- **OQ-4 (resolved by 42-D18, §2.5): `presets.max_depth`.** The knob is
  `presets.max_depth` (int >= 0, default 3), parsed into
  `PresetConfig::max_depth` and read once by the delegation pre-flight.
  **Rejected:** `tools.subagent.max_depth` (a second copy on the tool config,
  matching dsh's placement) and a per-preset row (no preset-specific depth
  semantics are pinned).
- **OQ-5 (resolved by 42-D17, §2.1.1): the schema is pinned here.** The
  `preset.jsonc` format, the row vocabulary, and the loader validation are
  pinned by this spec; the loader is the roster's. `21`'s errata owns only the
  `presets.*` config keys (PR-3), not the preset-file vocabulary.

---

## 13. References

**Design source and pinned interfaces**

- `26-dsh-alignment.md` §1.2 (`:45-75`), §2.4.1 (`:987-1057`), §2.4.2
  (`:1059-1114`).
- `26-dsh-alignment-part2.md` §4.2 (`:129-162`), §4.3.8 (`:739-800`),
  §4.3.9 (`:802-935`), §4.3.9.1 (`:936-975`), §4.3.9.2 (`:976-1022`), §4.4
  (`:1073-1089`), §4.5 (`:1117-1160`), §4.6 (`:1161-1238`), §4.9
  (`:1295-1331`), §5 Stage B (`:1386-1399`), §5 Wave 5 (`:1485-1492`), §6 OQ8
  (`:1621-1624`).
- `30-architecture-cascade-errata.md` §3 S3 (`:164-183`), S4 (`:185-205`),
  the renumber note (`:323`).

**Owning specs this amends**

- `36-prompt-registry.md` (verified per `DESIGN_STATUS.md:55`; its in-file
  status block `:3` is stale; scope reservation `:104-110`, interfaces
  `:112-166`, `36-I5` `:854-858`, out of scope `:86`).
- `01-session.md` `SessionHeader` (`:149-190`), `deriveMessages` (the
  projection).
- `23-session-lifecycle-errata.md` (status `:3`, creation path `:88-89`,
  `23-A18` `:214`, `23-D52`/`23-D53` `:369-370`).
- `24-agent-lifetime-errata.md` `24-D17`.
- `20-skills.md` (skill catalog; D17 inheritance via
  `26-dsh-alignment-part2.md:1085`).
- `21-config-jsonc-errata.md` (the `presets.*` keys; not yet present).
- `29-event-family-errata.md` (verified per `DESIGN_STATUS.md:41`; its in-file
  status block `:4` is stale; supersession/renumber `:19-25`, consumer row
  `:390`).
- `43-wave5-dependency-errata.md` (this spec's four blocking prerequisites: the
  `01` header/event codec, the `23` creation-path copy, the `21` `presets.*`
  keys, and the `06` child-creation seam).

**Shipped-code anchors (working tree at authoring time)**

- `include/ymh/session/session.hpp:44-58` (`struct SessionHeader`).
- `include/ymh/session/session_manager.hpp:29-36` (`struct SessionOptions`).
- `include/ymh/agent/agent.hpp:24` (`AgentId`), `:88-94` (`ContextMessage`).
- `include/ymh/agent/agent_loop.hpp:49-70` (`AgentServices`, `SystemPrompt*`
  prompt at `:64`).
- `include/ymh/agent/agent_registry.hpp:41` (`create`).
- `include/ymh/agent/subagent.hpp:17-31` (`SubagentRunner`),
  `src/agent/subagent.cpp:15-48` (`run`; kind/parent at `:18-19`).
- `include/ymh/core/event.hpp:51` (`enum class EventType`), `:83` (`wire_name`),
  `:86` (`parse_event_type`), `:90` (`all_event_types`).
- `src/session/session.cpp:363` (`deriveMessages`), `:376` (the switch, no
  `default:`).
- `src/session/session_persistence.cpp:36-51` (`sessions` DDL; `metadata` at
  `:47`).
- `include/ymh/tools/tool_registry.hpp:90` (`schemas()`).
- `include/ymh/skills/skill_catalog.hpp:34` (`SkillCatalog`).
- `include/ymh/agent/preset.hpp` (new; §3 types and the roster).
- `include/ymh/session/session_manager.hpp:68` (`sessionPtr`),
  `include/ymh/session/session.hpp:256-264` (`Session::append`).

**dsh sources (via `26`)**

- `dsh-agent-presets` roster (`lib/types/index.d.ts:60-383`), README
  (`:81`, `:99`), `session.d.ts:25-29`.
- `dsh-subagent` (`lib/types/index.d.ts:97-296`), `child-agent.d.ts:84-106`,
  `lib/index.js:519`, README (`:152`).
- `dsh-tool-subagent` config (`maxDepth` default 3).

---

## 14. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 2 | 2026-09-20 | Gate-fix rev, resolving the Rev 1 independent gate (2 HIGH / 5 MEDIUM / 4 LOW). **HIGH:** `select` gains the `SessionManager&` dependency (it must call `is_blank` and `Session::append`); the `Agent& -> AgentContext` bridge is pinned via the `AgentId -> leaf` map and `apply_child_composition` becomes a roster member (42-D15). **MEDIUM:** OQ-1 header encoding resolved (42-D19); `PresetConfig` defined and `presets.max_depth` pinned (42-D18); the `preset.jsonc` schema + loader validation pinned (42-D17); §9 rewritten as a blocking-prerequisite register whose contracts are pinned in the new `43-wave5-dependency-errata.md`; D17 continuable tools given an owner (42-D16). **LOW:** `deriveMessages` switch citation corrected to `:376`; the `29`/`36` statuses reconciled with `DESIGN_STATUS.md`; `PromptSectionSpec` pinned and the declaration order fixed. All five Rev-1 open questions resolved in §12. |
| Rev 1 | 2026-09-20 | Initial pin. Wave-5 owning spec, numbered 42 because the reserved `29-agent-presets.md` name is taken. Pins the roster, standing mount plus joined scopes, blank-session-only switch, `agent_preset/selected`, `apply_child_composition`, depth, fixed delegation scope, and the `SessionHeader.agent_preset` field. Five open questions recorded; none silently resolved. |

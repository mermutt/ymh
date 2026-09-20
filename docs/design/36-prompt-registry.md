# 36 — Prompt Registry (Wave 3)

Status: Rev 1 written · verified: — · reviewer: —
Authority: owning spec for `26-dsh-alignment-part2.md` §5 Wave 3
(decisions `26-D4`, `26-D5`, `26-D6`, `26-D7`, `26-D14`, `26-D15`). The
reserved filename `27-system-prompt.md` named in `26-dsh-alignment-part2.md`
§5 Stage B (line 1392) is **unavailable**: specs 27 through 35 are taken. This
file is that spec, numbered **36**.

This spec pins the **prompt system** and the **message provenance** model. It
does not write implementation code. It depends on the top-level gate having
closed; `30-architecture-cascade-errata.md` §6 records P1/P2/P3 (2026-09-19) and
the gate pass.

---

## 1. Purpose and scope

### 1.1 Why this spec exists

`26-dsh-alignment.md` §3.2 names gap 2 (G18/G19/G20/G21) as "the largest
behavioural delta and the one that most changes model behaviour": ymh has a
config-driven base prompt plus a skill-index append, but no section registry, no
order table, no variables, no persona slots, and no sourced runtime snapshots
(`26-dsh-alignment.md:1282-1286`). Wave 3 is where that delta lands.

Wave 1 already logs a durable request header whose `system_prompt_digest` is the
SHA-256 of the rendered prompt (`28-llm-service-boundary-errata.md:551-563`).
Wave 1 deliberately carries the **current** `AgentConfig::system_prompt`
(`26-dsh-alignment-part2.md:1418-1422`). Wave 3 replaces that source with a
deterministic registry and keeps the digest contract intact.

### 1.2 Authority and relationship to the other specs

- `26-dsh-alignment.md` §2.3 is the **design source** (registry, order tables,
  persona, AGENTS.md, runtime contexts, tool presentation, KV discipline). The
  verbatim dsh texts are quoted there and are the copy target.
- `26-dsh-alignment-part2.md` §4.3.3, §4.3.10, §5 (Wave 3), §4.9 and §5.2 are
  the pinned interfaces, config keys, wave scope, and test strategy.
- `01-session.md` owns the message model, the `ContextInjected` payload, and
  `deriveMessages()`. This spec is an **errata to `01`** for the provenance
  reshape (§3) and the `ContextInjected.role` default (§3.4).
- `34-assembler-replay-errata.md` owns the replay harness and the reconstruction
  rule. This spec feeds it a deterministic `render()` (§4).
- `28-llm-service-boundary-errata.md` owns `LlmRequestHeader` and the freeze /
  digest contract. This spec changes the **source** of the rendered prompt, not
  the header shape.
- `07-tools-execution.md` owns `ToolSchema` and the tool registry. This spec
  consumes `ToolRegistry::schemas()` for `PromptAssembly.tools`.
- `20-skills.md` owns the skill catalog. This spec changes where its index lives
  (§2.6) per `26 §4.4`.

Where this spec and a prior verified spec disagree, the prior spec wins and the
disagreement is recorded as an open question (§12), per the task rule.

### 1.3 In scope

1. The ordered **section registry** and `PromptContext` registry, with RAII
   handles, strict variables, and the assembly waterfall.
2. The canonical `SECTION_ORDERS` / `CONTEXT_ORDERS` tables (ymh allocation).
3. `assemble()` and `render_prompt()`.
4. **Persona** as `deployment:persona-prefix` (order 0) and
   `deployment:persona-suffix` (order 10200), with `complete` mode.
5. The **workspace-instructions loader** (`AGENTS.md` / `CLAUDE.md`): git-root
   discovery, required `max_bytes`, broad-to-specific, `<system-reminder>`
   wrapping, durable user-role message.
6. **Runtime-context snapshots**: sourced user-role material under the
   supersession header, distinct from sections.
7. **Tool presentation `Native`** and `tool_order` canonicalization.
8. **Message provenance**: `MessageSource`, `ContextForm`, `ContextFormed`, and
   the `SystemMessage` / `ToolResultMessage` projection types, reshaping the
   `01` message model.
9. Migration of `default_system_prompt()` (`src/cli/wiring.cpp:48-52`) to a
   registered `harness:identity` section plus a default persona.
10. `ContextInjected.role` default `System` → `User` (`26-D7`).
11. The provenance and prompt effect on `template_digest` /
    `system_prompt_digest` and the replay contract.
12. The skill catalog's move from a system-prompt append to a catalog-form
    context (`26 §4.4` row 20).

### 1.4 Out of scope

- `Ptc` and `Both` tool presentation. They are **reserved enum values** that
  fail loud at load (`26-I7`, `26-F14`); the PTC SDK generator is `26-D22`
  (deferred). Only `Native` is implemented.
- Presets, subagent composition, goals, jobs, commands (Waves 5 and 6).
- Changing the `session.persist_prompt_text` default. P2 keeps it `false`
  (`30-architecture-cascade-errata.md:377-380`).
- Any `kSchemaVersion` or `kProtocolVersion` bump. Neither changes (`26-I12`,
  `26 §4.6`).
- Implementation code.

---

## 2. The prompt system model

### 2.1 The ordered section registry

A `SystemPrompt` registry holds ordered sections, ordered contexts, variable
providers, and a tool provider. Registration returns a move-only RAII handle
whose destructor unregisters; this is ymh's C++ spelling of dsh's
`() => void` disposer (`26-dsh-alignment.md:656-671`). Duplicate names in one
layer throw; scoped sections and variables shadow globals of the same name.

```cpp
// include/ymh/prompt/system_prompt.hpp
namespace ymh {

// One assembly's merge-extensible context (dsh AssembleContext,
// 26-dsh-alignment.md:625-628). `scope` absent means the global layer only.
struct AssembleContext {
    std::optional<ScopeKey> scope;
    CancellationToken*      signal = nullptr;
};

struct PromptSection {
    std::string name;       // unique within a layer; duplicate throws
    std::int32_t order = 0; // ascending; ties by code-unit name order
    std::function<std::string(const AssembleContext&)> text;  // "" drops
    bool complete = false;  // at most one effective; >1 fails assemble()
};

struct PromptContext {
    std::string name;
    std::int32_t order = 0; // contexts join ascending
    std::function<std::string(const AssembleContext&)> text;  // "" contributes nothing
};

struct AssembledSection { std::string name; std::string text; bool complete = false; };
struct AssembledContext { std::string name; std::string text; };

struct PromptAssembly {
    std::vector<AssembledSection> sections;
    std::vector<AssembledContext> contexts;
    std::vector<ToolSchema>       tools;        // canonical order
    std::map<std::string, std::optional<std::string>> variables;
    std::vector<std::string>      tool_order;   // exactly one "<unlisted-tools>" rest
};

// Move-only RAII registrations. The registry owns the storage; a handle must
// not outlive its SystemPrompt.
class SectionHandle { /* ~, move; non-copyable */ };
class ContextHandle { /* ~, move; non-copyable */ };
class VariableHandle { /* ~, move; non-copyable */ };

// Single-threaded: owned by WorkspaceRuntime and called only on the agent
// executor thread (26-I10 / 26 §4.3.3:437-438). Not thread-safe by design.
class SystemPrompt {
public:
    SectionHandle  section(PromptSection);
    ContextHandle  context(PromptContext);
    VariableHandle variable(std::string name,
                            std::function<std::optional<std::string>(const AssembleContext&)>);
    // Tool provider registration is one per registry in Wave 3 (07-owned).
    void           set_tool_provider(std::function<std::vector<ToolSchema>(const AssembleContext&)>);

    // Merge global+scope (scope shadows same name), canonicalize by (order,name),
    // run the assembly waterfall, restore at most one effective complete section,
    // validate tool_order. Fail loud (26-I7).
    [[nodiscard]] PromptAssembly assemble(const AssembleContext&) const;
    // assemble() + render_prompt(). The system text the loop sends.
    [[nodiscard]] std::string render(const AssembleContext&) const;
};

// Strict interpolation: unknown/malformed {{var}} throws; empty sections drop;
// the rest join with "\n\n". A lone "{{" without a later "}}" is literal prose,
// and a substituted value is not scanned again. Assumes an assembly already
// validated by assemble().
[[nodiscard]] std::string render_prompt(const PromptAssembly&);

} // namespace ymh
```

`assemble()` is the loop's input path (`26-dsh-alignment-part2.md:457-460`):
the loop takes `assemble(ctx).tools` for the request tool list, `.variables`
for rendering, and `render(ctx)` for the system message. A registry returning
only a string could not drive a request.

**Assembly rules (pinned, from `26-dsh-alignment.md:680-694`).**

1. Sections concatenate ascending by `(order, name)`; ties by code-unit name
   order.
2. Empty text drops the section.
3. At most one **effective** complete section; more than one fails assembly.
   A `complete` section is restored after the waterfall as the sole section.
4. `render_prompt` interpolates strict `{{variable}}`, drops empty sections, and
   joins the rest with blank lines. Malformed, unknown, or undefined references
   throw. A lone `{{` is literal. Substituted values are not rescanned.
5. Contexts join ascending by `order`; an empty context contributes nothing.
6. `tool_order` contains the model-facing tool names in order with
   `<unlisted-tools>` exactly once. Unknown names fail at assembly; omitted means
   lexicographic order.

### 2.2 The canonical order tables

The dsh tables are the allocation authority (`26-dsh-alignment.md:696-748`,
`SECTION_ORDERS` at `dsh-system-prompt/lib/index.js:10-42` and `CONTEXT_ORDERS`
at `:43-47`). ymh preserves every dsh numeric value for a shared name so the
prose ordering matches, and reserves the rest. A ymh-local name takes an unused
value in a gap and **never reuses a dsh value**. Values are `std::int32_t`.

```cpp
// include/ymh/prompt/order.hpp
namespace ymh {

enum class SectionOrder : std::int32_t {
    // --- dsh values, verbatim (shared names) ---
    HarnessIdentity            = -1000,
    DeploymentPersonaPrefix    =     0,
    PlanPolicy                 =   500,
    TeamPolicy                 =   600,   // reserved (no ymh equivalent)
    PtcOnly                    =   800,   // reserved (D22 deferred)
    FileReference              =   900,   // reserved
    ToolShell                  =  1000,   // dsh TOOL_BASH; ymh `shell`
    ToolRead                   =  1100,   // ymh `read_file`
    ToolWrite                  =  1200,   // ymh `write_file`
    ToolEdit                   =  1300,   // ymh `edit_file`
    ToolGlob                   =  1400,   // ymh `glob`
    ToolGrep                   =  1500,   // ymh `grep`
    ToolJobs                   =  1600,   // reserved (Wave 6)
    ToolPty                    =  1700,   // ymh `terminal` (spec 14)
    ToolWebSearch              =  2000,   // reserved
    ToolWebFetch               =  2100,   // reserved
    ToolLsp                    =  2200,   // reserved (§28 candidate)
    ToolSessionQuery           =  2300,   // reserved
    ToolGoal                   =  2400,   // reserved (Wave 6)
    ToolCordis                 =  2500,   // reserved
    ToolWorkflow               =  2600,   // reserved
    ToolRalph                  =  2700,   // reserved
    ToolSubagent               =  2800,   // reserved (Wave 5)
    ToolReport                 =  2900,   // reserved
    ToolsSdk                   =  5000,   // reserved (PTC)
    DeliverableFileReferences  =  9000,   // reserved
    StructuredOutput           =  9900,   // reserved
    HarnessSource              = 10000,   // reserved
    WebSurface                 = 10100,   // reserved
    DeploymentPersonaSuffix    = 10200,
    // --- ymh-local allocations in dsh gaps ---
    ToolGit                    =  1050,   // ymh `git_status` / `git_diff`
    ToolSkill                  =  1800,   // reserved for a skill guidance section
    ToolPlan                   =  1900,   // ymh `exit_plan_mode` guidance
};

enum class ContextOrder : std::int32_t {
    SandboxPolicy       = 110,   // reserved (execution environment)
    ApprovalPolicy      = 115,   // reserved (spec 09 permissions)
    SubagentDelegation  = 120,   // reserved (Wave 5)
};

} // namespace ymh
```

Access is by name through `getSectionOrder` / `getContextOrder` semantics
(`26-dsh-alignment.md:747-748`); ymh exposes `section_order(name)` and
`context_order(name)` free functions over a `std::string_view` so config keys can
name orders without hardcoding integers in two places.

**"first-party prompt order 500"** means exactly `SectionOrder::PlanPolicy ==
500` (`26-dsh-alignment.md:745-746`). The plan-mode policy section already exists
as `default_plan_section()` (`src/cli/wiring.cpp:54-59`); Wave 3 registers it at
order 500 through the registry, replacing the `SessionContextAssembler` plan
provider (`src/agent/context_assembler.cpp:36-54`).

### 2.3 Persona

Deployment persona is two sections: `deployment:persona-prefix` at order 0
(`complete` iff configured) and `deployment:persona-suffix` at order 10200
(`26-dsh-alignment.md:750-794`). A per-scope persona shadows the deployment
defaults for that scope. Mounting a persona globally is a load error.

```cpp
struct PersonaConfig {
    std::string prefix;                    // required
    std::string suffix;                    // default ""
    bool        complete = false;          // prefix becomes the whole prompt
    bool        include_runtime_context = true;
};
```

**Default persona (shipped text, verbatim).** From the `standard` preset
(`26-dsh-alignment.md:770-778`):

```
prefix: You are a coding agent powered by the {{model}} model.
suffix: Your working directory is {{cwd}}.
```

`{{model}}` and `{{cwd}}` are session-invariant: the effective model is fixed at
session start and the daemon chdirs to its workspace root once at startup
(`AGENTS.md`, path-safety rule). They are therefore safe in the rendered prompt
(§4). `include_runtime_context = true` means the registry contributes the
runtime-context snapshot of §2.5; setting it false suppresses it for that scope.

The `minimal` preset (`26-dsh-alignment.md:780-788`) is out of Wave 3 scope
because presets are Wave 5. The `PersonaConfig.complete` mechanism is still
pinned here so Wave 5 mounts it without an errata.

### 2.4 Workspace instructions (`AGENTS.md` / `CLAUDE.md`)

`dsh-agent-instructions` does **not** register a system-prompt section; it
appends a durable **user-role** message (`26-dsh-alignment.md:900-903`). ymh
copies that shape.

```cpp
struct InstructionFileConfig {
    std::vector<std::string> project_root_markers{".git"};
    std::vector<std::string> candidates{"AGENTS.md", "CLAUDE.md"};
    std::vector<std::string> local_candidates{"AGENTS.local.md", "CLAUDE.local.md"};
    bool                     load_local = false;
    std::size_t              max_bytes;            // REQUIRED when enabled; no default
    std::size_t              max_source_bytes = 1048576;
};

struct InstructionFile {
    std::string               display_path;   // workspace-relative for the notice
    std::string               scope;          // directory the file governs
    std::string               content;        // post-truncation bytes
    bool                      truncated = false;
    std::size_t               original_bytes = 0;
};

struct LoadedInstructions {
    std::vector<InstructionFile> files;       // most-specific first after render
    std::vector<std::string>     omitted;     // broad files dropped by budget
    std::optional<std::string>   budget_notice; // verbatim budget marker
};

class InstructionLoader {
public:
    explicit InstructionLoader(InstructionFileConfig, const ExecutionEnvironment&);
    // First request only; not watched. Root discovery uses project_root_markers.
    [[nodiscard]] LoadedInstructions load();
    // Nested discovery on a successful read/write/edit that reaches a deeper dir.
    [[nodiscard]] LoadedInstructions refresh_for(const ResolvedPath& touched);
};
```

**Discovery rule (pinned).** User-global first (`$XDG_CONFIG_HOME/ymh/AGENTS.md`
else `$HOME/.config/ymh/AGENTS.md`; the `DSH_HOME` analogue), then for each
directory from the project root down to cwd, **every present candidate** in
`candidates` order, then `local_candidates` in order when `load_local` is true,
appended after the base files and never shadowing them
(`26-dsh-alignment-part2.md:476-499`). The project root is the nearest ancestor
containing a `project_root_markers` entry. First-match-wins is explicitly **not**
the rule (Rev 2 of part2 removed it).

**Budget rule (pinned).** Truncation is not a binary search. Rendering keeps the
most specific files first: it drops whole broader files before truncating the
most-specific file, and emits the verbatim budget marker naming omitted and
truncated paths. Rendered bytes never exceed `max_bytes`. An over-budget broad
file is ignored; during refresh it is treated as temporarily unavailable rather
than removed (`26-dsh-alignment.md:914-922`). The marker:

```
Workspace instruction budget <maxBytes> bytes: omitted <paths>; truncated <path> from <n> to <m> bytes
```

**Framing (verbatim, `26-dsh-alignment.md:925-941`).** The durable message
carries:

```
The following workspace instructions may be relevant to your work. Use them as guidance when applicable. More specific instructions take precedence over broader ones. They do not override system, developer, or direct user instructions.
```

followed per file by

```
Instructions from: AGENTS.md

<project content>
```

or, for a nested scope,

```
Additional instructions from: <displayPath>

These instructions apply to work under `<scope>`. Use them as guidance when relevant; more specific instructions take precedence. They do not override system, developer, or direct user instructions.

<file content>
```

**Wrapping.** The whole message is wrapped in `<system-reminder>` tags, matching
the skill-catalog framing (`26-dsh-alignment.md:878-892`). This is the
`26-D6` `<system-reminder>` requirement.

**Removal and change notices (verbatim, `26-dsh-alignment.md:943-948`).** A
removal appends `Instructions removed: <path>\n\nThe previously loaded
instructions from this file no longer apply.` A change appends `Updated
instructions from: <path>`, a blank line, `This file changed after it was
loaded. Use the following content instead of the previously loaded instructions
from this file.`, a blank line, then the content. These are durable user-role
messages; they never mutate an earlier message.

**Materialization.** Each load (first or refresh) appends a durable
`payload::ContextInjected` with `role = Role::User`,
`source.kind = Plugin`, `context.form = ContextForm::Instructions`. The event
type is `01`-owned; Wave 3 adds the provenance fields (§3). The rendered prompt
is **not** affected by workspace instructions, so a mid-session `AGENTS.md`
change does not force a new request series; it appends a message after the
reusable prefix, which is the append-only KV rule
(`26-dsh-alignment.md:978-982`).

### 2.5 Runtime contexts

Runtime contexts append a sourced **user-role** snapshot under the verbatim
header (`26-dsh-alignment.md:804-811`):

```
Current runtime context. This snapshot supersedes earlier runtime-context snapshots.

<body>
```

They are **not** sections and **not** part of `render()`. They are
`PromptAssembly.contexts` with `ContextForm::Snapshot`, materialized as a
durable `payload::ContextInjected` with `role = Role::User` and a non-empty
`context.sections` (one named section per source). The snapshot body names its
sources, at minimum: the working directory, the effective model, and the current
date. Because a new snapshot supersedes earlier ones without deleting them, the
projection keeps them in order; the header text tells the model the latest one
wins.

This is why `26-D7` reclassifies `ContextInjected.role`: the existing default is
`Role::System` (`include/ymh/session/events.hpp:168`; the part2 text cites
`:155`, which is stale, see §12 Q1), the opposite of dsh's user-role snapshot.

### 2.6 Tool presentation `Native`

```cpp
enum class ToolPresentationMode : std::uint8_t { Native, Ptc, Both };
```

`Native` presents each visible tool schema as a function definition. `Ptc` and
`Both` are **reserved** and fail loud at load (`26-I7`, `26-F14`); the PTC SDK
generator is `26-D22`, deferred. The default is `Native`
(`26-dsh-alignment.md:952-961`).

Guidance sections register at their `TOOL_*` orders and are **suppressed when
the tool is restricted away**. This is the tool-presentation half of `26-I6`:
the tool catalog is stable across modes, so plan mode changes only
`PlanPolicy` (order 500) and never adds or removes a tool. `assemble().tools`
comes from the registered tool provider (backed by `ToolRegistry::schemas()`),
already in canonical order; `tool_order` canonicalizes the model-facing list and
inserts unlisted tools lexicographically at the single `<unlisted-tools>` rest.

**Skill catalog relocation.** Spec `20`'s catalog index is currently appended to
`AgentConfig::system_prompt` in `make_agent_config`
(`src/agent/workspace_runtime.cpp:48-63`). Wave 3 relocates it to a durable
user-role `<system-reminder>` message with `ContextForm::Catalog`, matching the
verbatim dsh text (`26-dsh-alignment.md:878-892`) and `26 §4.4` row 20. This
removes the last non-registry writer to `system_prompt` and keeps the rendered
prompt deterministic.

### 2.7 The render pipeline

```text
InstructionLoader (opt-in) ──► ContextInjected(User, Instructions)   [durable]
SystemPrompt::assemble(ctx) ──► PromptAssembly
    .sections ──► render_prompt ──► system text ──► header.system_prompt_digest
    .contexts ──► ContextInjected(User, Snapshot/Catalog)            [durable]
    .tools    ──► LLMRequest.tools
SessionContextAssembler::assemble:
    messages := deriveMessages(session)
    if system text non-empty: prepend SystemMessage{text, digest}   [projection only]
```

`render()` is a pure function of `(registry state, AssembleContext, variables)`.
It does not read the clock, the filesystem, or the environment. The runtime
snapshot and workspace instructions are messages, not render inputs.

---

## 3. Message provenance

### 3.1 `MessageSource` / `ContextForm`

`26-D14` names `MessageSource` and `ContextForm`; they are pinned in
`26-dsh-alignment-part2.md:1023-1068` and restated here as the owning spec.

```cpp
// include/ymh/agent/provenance.hpp
namespace ymh {

// dsh ContextForm (message.d.ts:42-54): a SEMANTIC vocabulary, never visual.
// `None` is the documented default (an absent/unknown value is opaque content).
enum class ContextForm : std::uint8_t {
    None, Instructions, Catalog, Snapshot, Notice, Relay, Recall
};

struct ContextSnapshotSection { std::string name; std::string text; };

// dsh ContextFormed is a discriminated union keyed by `form`; ymh flattens it to
// one defaulted struct and enforces the coupling at construction (26-I7):
// Snapshot requires non-empty `sections`; Notice requires non-empty `summary`.
struct ContextFormed {
    ContextForm                         form = ContextForm::None;
    std::vector<ContextSnapshotSection> sections;   // required iff form == Snapshot
    std::string                         summary;    // required iff form == Notice
};

// dsh MessageSourceMap (message.d.ts:94-104), flattened. Divergence (stated):
// dsh's ModelMessageSource carries `replayState`; ymh keeps replay state once on
// the assistant settlement event (34-I6), not here.
struct MessageSource {
    enum class Kind : std::uint8_t { User, Plugin, Model, Tool };
    Kind                      kind = Kind::User;
    std::string               plugin;                 // Kind::Plugin
    ContextFormed             context;                // Kind::Plugin
    std::optional<ToolCallId> call;                   // Kind::Tool
    std::string               provider;               // Kind::Model
    std::string               model;                  // Kind::Model
};

} // namespace ymh
```

`MessageSource.kind` answers **who produced this**; `ContextForm` answers **what
kind of thing it is**. The two axes are independent.

### 3.2 `Message` and the provenance fields

`Message` is pinned by `include/ymh/agent/message.hpp` and reused by `01` §4.5.
Wave 3 adds two optional, defaulted fields. This is additive to the pinned
member layout: no existing field is renamed or reordered.

```cpp
// include/ymh/agent/message.hpp (amended)
struct Message {
    Role                      role = Role::User;
    std::vector<ContentBlock> content;
    std::optional<ToolCallId> tool_call_id;
    // 36 additions:
    std::optional<MessageSource> source;    // who produced it (36 §3.1)
    std::optional<ContextFormed> context;   // what kind of thing it is
};
```

### 3.3 `SystemMessage` and `ToolResultMessage`

`SystemMessage` and `ToolResultMessage` are **projection-level** types, not
durable event payloads. This resolves the `26-D14` wording without adding a new
durable event: the system prompt is stored in the request header (digest always,
text under the opt-in) and prepended by `SessionContextAssembler::assemble`
(`src/agent/context_assembler.cpp:41-65`); a tool result is already a durable
`payload::ToolResult` and is projected into a `Role::Tool` message by
`deriveMessages()` (`01-session.md:766-770`).

```cpp
// include/ymh/agent/provenance.hpp (continued)

// The prepended system message (projection only; never a durable event).
// `prompt_digest` is the SHA-256 the request header carries (36 §4).
struct SystemMessage {
    MessageId                    id;
    std::string                  text;
    std::string                  prompt_digest;
    MessageSource                source;   // Kind::Plugin, ContextForm::None
};

// A projected tool-role message. `context` carries an optional Notice (e.g. a
// retention or truncation notice, Wave 4) without changing the durable payload.
struct ToolResultMessage {
    ToolCallId       call;
    std::string      name;
    ToolOutcome      outcome = ToolOutcome::Ok;
    std::string      output;
    bool             truncated = false;
    std::optional<std::string> error;
    ContextFormed    context;   // form == Notice iff a notice is present
};
```

The durable payloads gain provenance fields (additive to `01` §4.5):

| Payload | Added field | Default source |
|---|---|---|
| `payload::UserMessage` | `MessageSource source` | `Kind::User` |
| `payload::ContextInjected` | `MessageSource source`, `ContextFormed context` | `Kind::Plugin` |
| `payload::ToolResult` | `MessageSource source`, `ContextFormed context` | `Kind::Tool`, `call` set |
| `payload::AssistantMessage` | `MessageSource source` | `Kind::Model`, `provider`/`model` set |

`payload::AssistantChunk` and `payload::ToolCall` are unchanged: a chunk is a
live delta and a tool call is already inside the assistant content (`01` I12,
I14). The `to_json`/`from_json` bodies are the `01` errata's obligation; the
wire keys are `source` and `context`, both omitted when at their default, which
keeps old rows decodable and new rows forward-fenced by the existing unknown-key
tolerance of the payload codecs.

### 3.4 `ContextInjected.role` default changes to `User`

`payload::ContextInjected` currently defaults `role = Role::System`
(`include/ymh/session/events.hpp:168`). Wave 3 changes the default to
`Role::User` (`26-D7`). The projection is unchanged (`messages += Message{
payload.role, payload.text }`, `01-session.md:772-773`); only the default value
moves. An explicit `Role::System` is still legal and still projects as a system
message, so a session that stored an explicit system-role context decodes and
replays identically.

This is a **breaking default change** for new writes and a **no-op for existing
durable rows** because the stored JSON carries the role explicitly. The `01`
errata must state the default in the payload definition and in the codec's
default-construction path.

### 3.5 Projection changes in `deriveMessages()`

`deriveMessages()` remains a pure function of `(header, events())`
(`01-session.md:726-731`). Wave 3 changes three things:

1. `ContextInjected` projects with its provenance carried onto the `Message`
   (source/context), not just role and text.
2. `ToolResult` projects with `MessageSource{Kind::Tool, call}` and any
   `ContextFormed` notice.
3. `UserMessage` and `AssistantMessage` carry their source.

The system message is still **not** projected by `deriveMessages()`; it is
prepended by `SessionContextAssembler::assemble`, exactly as today, so the
replay harness's reconstruction rule (`34 §9.2`) is unchanged. The projection
switch (`src/session/session.cpp:462`) is one of the five consumer switches that
must gain a case for any new durable type (`26 §4.3.9`); Wave 3 adds no new
durable type, so the switch only changes bodies.

---

## 4. Digest, replay, and the persisted-prompt contract

### 4.1 `system_prompt_digest` and `template_digest`

Wave 1 pinned the header (`28-llm-service-boundary-errata.md:551-563`):

- `system_prompt_digest` is the SHA-256 hex of the **rendered** prompt.
- `template_digest` is the SHA-256 of `FrozenRequest::canonical_template()`,
  which includes the full rendered `system_prompt`.
- The full text is stored only under `session.persist_prompt_text`; otherwise
  the durable record is the digest alone (`26-I11`).

Wave 3 changes the **source** of the rendered text: it is
`SystemPrompt::render(ctx)` instead of `AgentConfig::system_prompt`. The digest
contract does not change. Concretely:

```cpp
// AgentLoop::buildRequest (Wave 3 form)
PromptAssembly assembly = prompt_.assemble(assemble_context);
const std::string system_text = render_prompt(assembly);
const std::string prompt_digest = sha256_hex(system_text);
// A header is logged iff config, prompt_digest, tool_names/schemas, or purpose
// changed (26 §4.3.2 loop rule). prompt_digest is compared, not the text.
```

`src/agent/agent_loop.cpp:444-471` already computes a prompt digest from the
assembled system message; Wave 3 swaps `assembled_system_prompt()` for the
registry render. `src/cli/wiring.cpp:181-182` currently chooses between
`default_system_prompt()` and `agent.system_prompt`; Wave 3 registers the chosen
text as the identity source (§5).

### 4.2 The rendered prompt is deterministic and replay-verifiable

Because `render()` is pure (§2.7), replay can re-derive the exact text from the
live registries and verify `sha256(render()) == header.system_prompt_digest`,
exactly as `34 §9.2` step 3 describes (`34-assembler-replay-errata.md:686-692`):
`header.system_prompt` when the opt-in stored it, else the caller-supplied
rendered prompt, else empty. Wave 3 makes the "caller-supplied" path
**reproducible**: the harness calls `SystemPrompt::render()` with the recorded
`AssembleContext` and compares.

The registries that replay re-derives from are: the section/context/variable
registry, the tool registry (`ToolRegistry::schemas()`), the persona config, and
the enabled workspace-instruction files (which are messages, not render inputs,
so they are reconstructed from the log prefix instead). A change to any
render-affecting registry forces a new header and a new request series
(`26-I6`, `26-F6`). A change to an instruction file does not, because it appends
a message after the prefix (§2.4).

### 4.3 `persist_prompt_text` stays `false` (P2)

`30-architecture-cascade-errata.md` §6 records the user decision of 2026-09-19:
**keep the default `false`**. The Wave-3 checkpoint may record a baseline with
the flag on in a scratch workspace, but the shipped default does not change.
This spec does not touch `session.persist_prompt_text`; it only changes what the
digest is computed over.

### 4.4 Replay interaction summary

| Quantity | Wave 1 | Wave 3 |
|---|---|---|
| Rendered prompt source | `AgentConfig::system_prompt` (`wiring.cpp:181-182`) | `SystemPrompt::render()` |
| `system_prompt_digest` | SHA-256 of that text | unchanged contract, new source |
| `template_digest` | includes rendered prompt | unchanged |
| Full prompt persisted | only under opt-in | only under opt-in |
| Replay prompt re-derivation | `ReplayEnv.rendered_system_prompt` | `SystemPrompt::render(ctx)` |
| Messages | `deriveMessages(prefix)` | unchanged, provenance carried |
| Runtime snapshot / instructions | n/a | durable user-role messages in the prefix |
| New durable event types | none | none |
| `kSchemaVersion` / `kProtocolVersion` | unchanged | unchanged |

---

## 5. Configuration

New JSONC keys, additive to spec `21` (`26-dsh-alignment-part2.md:1295-1330`).
All are optional except where noted. The global-layer-only rule applies only to
`session.persist_prompt_text`; the prompt keys may appear in either layer.

| Key | Type | Default | Notes |
|---|---|---|---|
| `agent.system_prompt` | string | built-in | existing key; when non-empty it **overrides** `harness:identity` (backward compatible) |
| `prompt.persona.prefix` | string | built-in | required when persona enabled; overrides the shipped prefix |
| `prompt.persona.suffix` | string | `""` | |
| `prompt.persona.complete` | bool | `false` | |
| `prompt.persona.include_runtime_context` | bool | `true` | |
| `prompt.instructions.enabled` | bool | `false` | |
| `prompt.instructions.max_bytes` | int | — | **required when enabled** |
| `prompt.instructions.project_root_markers` | string[] | `[".git"]` | |
| `prompt.instructions.candidates` | string[] | `["AGENTS.md","CLAUDE.md"]` | |
| `prompt.instructions.local_candidates` | string[] | `["AGENTS.local.md","CLAUDE.local.md"]` | |
| `prompt.instructions.load_local` | bool | `false` | |
| `tools.presentation` | `"native"` | `"native"` | `ptc`/`both` fail loud (D22) |
| `tools.tool_order` | string[] | lexicographic | exactly one `<unlisted-tools>` rest |

Config load order is unchanged (`21` §6): global then workspace, workspace
overrides. `agent.system_prompt` is read from the existing `[agent]` table
(`src/config/config.cpp:314-322`); an empty value means "use the built-in
identity", preserving the current `wiring.cpp:181-182` semantics.

---

## 6. Invariants

These extend `26-I1`–`26-I12`. `36-I*` are local to this spec.

- **36-I1 (determinism).** Two assemblies of identical inputs produce
  byte-identical `PromptAssembly` and `render_prompt` output. Guard: canonical
  `(order, name)` sort, drop-empty, strict variables, one `complete`, and the
  golden hash tests (§11.2). This is the executable form of `26-F5`.
- **36-I2 (one complete).** At most one **effective** complete section exists.
  More than one fails `assemble()`; zero is legal.
- **36-I3 (strict variables).** `render_prompt` throws on an unknown, malformed,
  or undefined `{{variable}}`. A lone `{{` with no later `}}` is literal. A
  substituted value is never rescanned.
- **36-I4 (tool order).** `tool_order` contains each model-facing name at most
  once and `<unlisted-tools>` exactly once. Unknown names fail at assembly.
  Omitting `tool_order` means lexicographic order.
- **36-I5 (persona singleton).** `deployment:persona-prefix` is registered at
  most once per effective scope; mounting it globally is a load error. `complete`
  makes the prefix the sole section (the `minimal` preset semantics).
- **36-I6 (instructions are messages).** Workspace instructions and runtime
  contexts never enter `render()`. They are durable user-role messages. A change
  to either never forces a new request series; it appends after the reusable
  prefix (`26-I6`).
- **36-I7 (budget).** Rendered instruction bytes never exceed `max_bytes`.
  Broader files are dropped before the most-specific file is truncated, and a
  budget marker names every omitted and truncated path.
- **36-I8 (provenance coupling).** A `ContextFormed` with `form == Snapshot`
  requires a non-empty `sections`; `form == Notice` requires a non-empty
  `summary`; a violation fails at construction (`26-I7`). `MessageSource.kind`
  gates its fields: `plugin`/`context` only for `Plugin`, `call` only for `Tool`,
  `provider`/`model` only for `Model`.
- **36-I9 (role default).** A `ContextInjected` with no explicit role is
  user-role. An explicit role is preserved verbatim. Existing durable rows are
  unaffected because the stored JSON is explicit.
- **36-I10 (digest source).** `system_prompt_digest` is always
  `sha256_hex(SystemPrompt::render(ctx))`. The full text is persisted only under
  `session.persist_prompt_text` (P2: default `false`). Replay re-derives the text
  and verifies the digest; it never fabricates it (`26-I11`).
- **36-I11 (no mutation).** `SystemPrompt` is single-threaded and owned by
  `WorkspaceRuntime`, called only on the agent executor thread. No extension,
  adapter, or UI mutates a registered section after a request is frozen
  (`26-I2`).
- **36-I12 (identity migration).** With `agent.system_prompt` empty,
  `harness:identity` renders the shipped identity text and the default persona
  supplies the prefix/suffix. With it non-empty, the configured text replaces
  `harness:identity`; no other section changes.
- **36-I13 (tool presentation).** Only `Native` loads. `Ptc`/`Both` fail loud at
  load (`26-F14`). A restricted-away tool suppresses its guidance section and
  drops its schema, and the resulting catalog change starts a new series
  (`26-I6`).
- **36-I14 (append-only).** Instruction change/removal notices and runtime
  snapshots are appended, never edits or deletions of earlier events
  (`26-I9`-style; the log is the source of truth, `26-I1`).

---

## 7. Failure modes

### 7.1 Shared findings (`F1`–`F12`, `§54`)

The applicable shared findings are inherited by reference: the prompt system is
part of the agent loop and the session projection, so the loop's terminal
guarantee (`26-I4`) and the session's event-sourcing invariant (`01` §12) apply.
This spec adds no shared-finding exception.

### 7.2 Component-local failure modes

- **36-F1 (prompt non-determinism).** Two assemblies of identical inputs differ.
  Guard: `36-I1`; a golden test hashes the rendered prompt (`26-F5`).
- **36-F2 (duplicate section/context).** A duplicate name in one layer silently
  shadows. Guard: registration throws; `assemble()` rejects a merge collision
  that is not an intentional scope shadow.
- **36-F3 (multiple complete sections).** Two `complete` sections render
  concatenated. Guard: `assemble()` fails when more than one is effective
  (`36-I2`).
- **36-F4 (unknown variable).** A `{{var}}` with no provider renders literally.
  Guard: `render_prompt` throws (`36-I3`).
- **36-F5 (bad `tool_order`).** A missing or duplicated `<unlisted-tools>` rest,
  or an unknown name, is silently ignored. Guard: `assemble()` fails (`36-I4`).
- **36-F6 (persona mounted globally).** A persona registers a global prefix and
  leaks into every scope. Guard: load error (`36-I5`).
- **36-F7 (instruction budget overflow).** A large `AGENTS.md` exceeds
  `max_bytes` or is truncated silently. Guard: the drop-broader-first rule and
  the verbatim budget marker (`36-I7`).
- **36-F8 (instructions leaked into the prompt).** An instruction file is
  rendered as a section, changing the digest mid-session. Guard: the loader
  appends a message only; `36-I6`.
- **36-F9 (runtime snapshot in the prompt).** The clock or cwd enters `render()`
  and makes the digest non-reproducible. Guard: runtime material is a message;
  `render()` is pure (`36-I6`, `36-I10`).
- **36-F10 (provenance form mismatch).** `form == Snapshot` without `sections`,
  or `Notice` without `summary`, is accepted. Guard: construction throws
  (`36-I8`).
- **36-F11 (silent role flip).** A new `ContextInjected` writes system-role by
  default and the model reads injected context as instructions. Guard:
  `36-I9`; the default is user-role and the codec default is pinned.
- **36-F12 (reserved presentation accepted).** `Ptc`/`Both` loads and emits an
  empty or wrong catalog. Guard: fail loud at load (`36-I13`, `26-F14`).
- **36-F13 (replay prompt mismatch).** Replay re-derives a different prompt and
  the digest check is skipped. Guard: `34 §9.2` step 3 plus the Wave-3 negative
  test (`36-I10`; `26-F13`).
- **36-F14 (instruction notice mutates history).** A change notice rewrites an
  earlier message instead of appending. Guard: `36-I14`.
- **36-F15 (legacy prompt source).** A session with no registry render (a Wave-1
  header) is replayed as if the registry were authoritative. Guard: the
  `ReplayMismatch{MissingHeader}` / legacy path (`26-F15`); Wave-3 replay must
  treat a header-less attempt as legacy and not fabricate a prompt.

---

## 8. dsh mapping

| dsh concept | ymh type / site | Reference |
|---|---|---|
| `SystemPrompt` service (`index.d.ts:220`) | `ymh::SystemPrompt` | `26 §2.3.1:656-671` |
| `PromptSection` / `PromptContext` | `PromptSection` / `PromptContext` | `26 §2.3.1:630-641` |
| `assemble()` waterfall | `SystemPrompt::assemble()` | `26 §2.3.1:673-678` |
| `renderPrompt()` | `render_prompt()` | `26 §2.3.1:683-686` |
| `SECTION_ORDERS` / `CONTEXT_ORDERS` | `SectionOrder` / `ContextOrder` | `26 §2.3.2:696-748` |
| `dsh-persona` (`prefix`/`suffix`/`complete`) | `PersonaConfig` + orders 0/10200 | `26 §2.3.3:750-794` |
| harness identity (order −1000) | `harness:identity` section | `26 §2.3.4:798-802` |
| runtime-context header | `RuntimeContextSnapshot` user-role message | `26 §2.3.4:804-811` |
| `dsh-agent-instructions` | `InstructionLoader` + `ContextForm::Instructions` | `26 §2.3.5:900-950` |
| `dsh-agent-tool-presentation` (`native`) | `ToolPresentationMode::Native` | `26 §2.3.6:952-961` |
| `MessageSourceMap` / `ContextForm` | `MessageSource` / `ContextForm` / `ContextFormed` | `26 part2 §4.3.10:1023-1068` |
| `SystemMessage` / `ToolResultMessage` | projection structs (§3.3) | `26-D14:152` |
| skill catalog `<system-reminder>` | `ContextForm::Catalog` message | `26 §2.3.4:878-892`; `26 §4.4` row 20 |

**Deliberate divergences (stated).** (a) ymh uses `std::string` and RAII handles
where dsh uses TypeScript closures and disposers. (b) ymh's `ContextFormed` is a
flattened struct with construction-time coupling checks, not a discriminated
union. (c) ymh keeps replay state on the assistant settlement event, not on
`MessageSource`. (d) ymh's tool names differ (`read_file` vs `read`), so the
`ToolRead` order is shared but the name is not.

---

## 9. Dependencies

**Upstream (must be verified before Wave-3 code, per `AGENTS.md`):**

| Spec | What Wave 3 needs | Status |
|---|---|---|
| `26-dsh-alignment.md` | design source, verbatim texts | reference (not a component spec) |
| `26-dsh-alignment-part2.md` | pinned interfaces, wave scope | reference |
| `01-session.md` errata | provenance fields on payloads, `ContextInjected.role` default, projection bodies | **required** |
| `08-llm-provider.md` / `28` errata | `LlmRequestHeader`, `FrozenRequest`, digest contract | verified (Wave 1) |
| `34-assembler-replay-errata.md` | replay harness reconstruction rule | verified (Wave 2) |
| `07-tools-execution.md` | `ToolSchema`, `ToolRegistry::schemas()` | verified |
| `21-config-jsonc-errata.md` errata | new prompt keys, `session.persist_prompt_text` (P2) | **required** |
| `17-transcript` errata | provenance row kinds (`26-D14` names `17`) | **required** |
| `20-skills.md` errata | catalog relocation to `ContextForm::Catalog` | **required** |

**Downstream (Wave 3 feeds):**

- `34`'s replay harness (a production-adjacent test) gains the registry render.
- Wave 4 (retention/pruning) consumes `ToolResultMessage.context` notices.
- Wave 5 (presets) mounts `PersonaConfig` and per-scope registries.
- Wave 6 (goals/jobs/commands) uses the reserved orders.

**Code sites Wave 3 touches (for the implementer; no code here):**
`src/cli/wiring.cpp:48-52,181-182`; `src/agent/workspace_runtime.cpp:48-63,112`;
`src/agent/agent_loop.cpp:22-33,444-471`; `src/agent/context_assembler.cpp:33-65`;
`include/ymh/session/events.hpp:166-170`; `include/ymh/agent/message.hpp`.

---

## 10. Decision register

| ID | Decision | Add./Brk. | Owning spec |
|---|---|---|---|
| **36-D1** | New component `include/ymh/prompt/` holding `SystemPrompt`, the order enums, `PersonaConfig`, `InstructionLoader`, and the provenance header `include/ymh/agent/provenance.hpp`. | New | 36 |
| **36-D2** | Order tables preserve every dsh numeric value for a shared name; ymh-local names use unused gap values (`ToolGit=1050`, `ToolSkill=1800`, `ToolPlan=1900`) and never reuse a dsh value. | New | 36 |
| **36-D3** | Persona is sections at orders 0 and 10200; the default prefix/suffix are the shipped dsh texts; `complete` is implemented now for Wave 5. | Add. | 36 |
| **36-D4** | The instructions loader is opt-in, requires `max_bytes`, uses every-present-candidate (not first-match), drops broader files before truncating the most-specific, and appends a `<system-reminder>`-wrapped user-role message. | New | 36 |
| **36-D5** | Runtime contexts are `PromptAssembly.contexts` with `ContextForm::Snapshot`, materialized as sourced user-role `ContextInjected`; they are never render inputs. | New | 36 |
| **36-D6** | Tool presentation ships `Native` only; `Ptc`/`Both` fail loud at load; guidance sections are suppressed when the tool is restricted away. | Add. | 07, 36 |
| **36-D7** | `SystemMessage`/`ToolResultMessage` are **projection-level** structs, not durable payloads; the durable payloads gain additive provenance fields. | Brk. (`01` struct/codec) | 01, 36 |
| **36-D8** | `payload::ContextInjected.role` default changes `System` → `User`; explicit roles are preserved. | Brk. (default) | 01, 36 |
| **36-D9** | `render()` is pure over registry state and session-invariant variables; clock/cwd/filesystem never enter it. | New | 36 |
| **36-D10** | `system_prompt_digest = sha256(render())`; `template_digest` continues to include the rendered prompt; the header shape is unchanged. | Add. | 28, 36 |
| **36-D11** | `session.persist_prompt_text` stays `false` (P2). No change to the key, its layer rule, or its semantics. | None | 21, 30 |
| **36-D12** | `default_system_prompt()` migrates to `harness:identity` + the default persona; a non-empty `agent.system_prompt` overrides `harness:identity` only. | Brk. (text) | 36 |
| **36-D13** | The skill catalog index moves from `AgentConfig::system_prompt` to a `ContextForm::Catalog` user-role message. | Brk. (spec 20 text) | 20, 36 |
| **36-D14** | The registry is single-threaded, owned by `WorkspaceRuntime`, called only on the agent executor thread. | New | 36 |
| **36-D15** | `InstructionLoader` loads on first request and is not watched; nested files are discovered on a successful `read`/`write`/`edit` reaching a deeper directory. | New | 36 |
| **36-D16** | No new durable event type, no `kSchemaVersion` bump, no `kProtocolVersion` bump. | None | 01, 36 |

---

## 11. Test plan

Strategy follows `§44` and `26-dsh-alignment-part2.md` §5.2 (T-M12). Hermetic
fixtures come from `FakeLLM` runs; live tests stay opt-in (`YMH_LIVE_LLM=1`).

### 11.1 Unit tests

- **Registry / order.** `(order, name)` sort with ties; empty-section drop;
  scope shadowing; duplicate-name rejection; `getSectionOrder` /
  `getContextOrder` name lookup.
- **Variables.** Strict `{{var}}`; unknown/malformed throws; lone `{{` literal;
  substituted value not rescanned; `complete` restoration.
- **Persona.** Prefix at 0, suffix at 10200; `complete` makes the prefix the sole
  section; global mount rejected; `include_runtime_context = false` suppresses
  the snapshot.
- **Instructions.** Root discovery via `.git`; every-present-candidate order;
  `load_local` append; `max_bytes` required; broader-file drop before
  most-specific truncation; budget marker text; removal/change notice text;
  `<system-reminder>` wrapping; not-watched first load; nested refresh on a deep
  `read`.
- **Runtime contexts.** Supersession header verbatim; sourced sections;
  `ContextInjected{role=User}`; `include_runtime_context` gating.
- **Provenance.** `ContextFormed` coupling failures; `MessageSource` field
  gating; projection carries source/context; `ContextInjected` default role.
- **Tool presentation.** `Native` only; `Ptc`/`Both` load failure; `tool_order`
  validation; guidance suppression on restriction.
- **Identity migration.** Empty `agent.system_prompt` → shipped identity +
  persona; non-empty → override only; no other section changes.

### 11.2 Golden rendered-prompt hashes

One golden SHA-256 per preset/config combination (`26-dsh-alignment-part2.md`
§5.2 item 3). The matrix:

| Fixture | Config | Expected |
|---|---|---|
| `prompt_default` | `agent.system_prompt=""`, persona default, `Native` | hash A |
| `prompt_config_override` | `agent.system_prompt="<custom>"` | hash B |
| `prompt_persona_suffix` | custom persona suffix with `{{cwd}}` | hash C |
| `prompt_instructions_on` | instructions enabled | hash D (render unchanged from A; instructions are a message) |
| `prompt_plan_mode` | plan mode active | hash E (only `PlanPolicy` differs) |
| `prompt_minimal` | `persona.complete=true` | hash F (persona is the whole prompt) |

Each fixture is generated by `FakeLLM` and checked in under
`tests/fixtures/prompt/` with a generator test that regenerates and diffs, per
§5.2 item 2. The golden hash is over `render_prompt(assemble(ctx))`, not over
the wire bytes.

**Negative goldens:** duplicate section name; more than one `complete`; unknown
`{{var}}`; missing/duplicated `<unlisted-tools>`; unknown `tool_order` name.
Each must fail `assemble()`/`render_prompt` with the specific error.

### 11.3 Digest and replay tests

- **Positive.** For every `LlmRequestHeader`, rebuild the template from the
  header plus the live registries and assert
  `rebuild.template_digest() == header.template_digest`. When
  `persist_prompt_text` is off, assert the header carries **no** `system_prompt`
  key and `sha256(render()) == header.system_prompt_digest`; when on, assert the
  stored text is byte-identical.
- **Prompt digest negative.** Perturb one persona variable and assert the
  prompt-digest check fails (`36-F13`).
- **Registry mismatch negative.** Mutate one tool schema and assert the template
  rebuild fails loud as a registry mismatch (`26-F6`).
- **Legacy negative.** A Wave-1 header with no registry render is marked legacy
  and not fabricated (`36-F15`).
- **Config-layer negative.** A workspace-layer `session.persist_prompt_text`
  raises `ConfigError` (`34 §9.5` item 7; `26-I11`).

### 11.4 Integration tests

- **Provenance round-trip.** Append `UserMessage`, `ContextInjected`
  (instructions/snapshot/catalog), and `ToolResult` with provenance; reopen the
  DB; assert the projection and the source/context fields survive.
- **Replay with instructions.** Record a session, edit `AGENTS.md`, refresh, and
  assert a new message appends with **no** new request series.
- **Replay with plan mode.** Toggle plan mode and assert only the order-500
  section changes and the tool catalog is stable (`26-I6`).
- **Skill catalog.** Assert the catalog message is a `ContextForm::Catalog`
  user-role message and that `AgentConfig::system_prompt` no longer carries it.
- **Mixed-version wire.** A new supervisor attached to an old daemon decodes old
  event types and skips unknown ones (`26 §4.6`).

### 11.5 Failure-mode coverage matrix

Every `36-F*` maps to at least one test above. The matrix is:

| Failure | Test |
|---|---|
| 36-F1 | 11.2 positive goldens |
| 36-F2 | 11.2 negative (duplicate) |
| 36-F3 | 11.2 negative (`>1 complete`) |
| 36-F4 | 11.2 negative (unknown var) |
| 36-F5 | 11.2 negative (`tool_order`) |
| 36-F6 | 11.1 persona (global mount) |
| 36-F7 | 11.1 instructions (budget) |
| 36-F8 | 11.4 replay with instructions |
| 36-F9 | 11.2 `prompt_instructions_on` hash equals default |
| 36-F10 | 11.1 provenance (coupling) |
| 36-F11 | 11.1 provenance (role default) |
| 36-F12 | 11.1 tool presentation |
| 36-F13 | 11.3 prompt digest negative |
| 36-F14 | 11.1 instructions (notice) |
| 36-F15 | 11.3 legacy negative |

---

## 12. Open questions

- **Q1 (line drift).** `26-dsh-alignment-part2.md:504` cites
  `events.hpp:155` for the `ContextInjected.role` default; the current tree has
  `ContextInjected` at `include/ymh/session/events.hpp:166-170` with the default
  at `:168`. This spec cites `:168`. The prior line number is stale, not a
  semantic conflict. No action needed beyond the citation fix in the `01` errata.
- **Q2 (`SystemMessage`/`ToolResultMessage` nature).** `26-D14` says
  "specializations" without saying durable or projection. This spec interprets
  them as projection-level (§3.3) because the system prompt is header state and
  the tool result is already a durable payload. If the `01` errata instead
  introduces them as durable payloads, this spec's §3.3 must be re-cut. Recorded
  as an interpretation, not a contradiction.
- **Q3 (identity text).** `26-D5` says the default persona mirrors dsh's shipped
  text and Wave 3 migrates `default_system_prompt()`. This spec retires the ymh
  text in favour of the dsh identity plus persona. The ymh-specific guidance
  ("prefer reading files before editing") is covered by the `tool:read` /
  `tool:write` guidance sections. If the user wants the ymh identity text kept,
  it becomes a `harness:identity` override, not a registry change.
- **Q4 (`ToolGit` / `ToolPlan` orders).** dsh has no git or plan-tool guidance
  sections, so `1050` and `1900` are ymh-local allocations in dsh gaps. If dsh
  later allocates those values, ymh must move its local orders, which is a
  registry change that starts a new request series. Accepted risk.
- **Q5 (runtime snapshot body).** The exact source list (cwd, model, date, and
  what else) is not pinned by `26`; this spec names the minimum three. The body
  is a message, so it can be extended without a digest change. Pinned here as an
  implementation detail, not a design gate.
- **Q6 (instruction tie-breaking).** `26-dsh-alignment.md:950` explicitly does
  not re-assert dsh's tie-breaking among `AGENTS.md` and `CLAUDE.md` in one
  directory; this spec uses "every present candidate in `candidates` order"
  (`26-dsh-alignment-part2.md:480-485`), which is deterministic. If the `20`
  errata pins a different rule, this spec follows it.
- **Q7 (instruction budget unit).** The dsh marker says `bytes` and the config
  key is `max_bytes`; this spec treats both as UTF-8 byte counts, matching the
  existing size parsing in `src/config/config.cpp`. If a later spec pins code
  points, the marker text changes with it.

---

## 13. References

- `docs/design/26-dsh-alignment.md` §1.2, §2.3 (prompt system), §3.2, §4.4.
- `docs/design/26-dsh-alignment-part2.md` §4.1 (principles), §4.2 (register),
  §4.3.3 (registry), §4.3.10 (provenance), §4.4, §4.5, §4.9, §5 (waves), §5.2.
- `docs/design/01-session.md` §4.4, §4.5, §6.3, §14.
- `docs/design/34-assembler-replay-errata.md` §9 (replay harness), §11, §17.
- `docs/design/28-llm-service-boundary-errata.md` §5.2-§5.4.
- `docs/design/30-architecture-cascade-errata.md` §6 (P1/P2/P3).
- `docs/design/07-tools-execution.md` §3.2 (`ToolSchema`).
- `include/ymh/agent/message.hpp:31-75`; `include/ymh/session/events.hpp:166-170`;
  `include/ymh/tools/tool.hpp:39`.
- `src/cli/wiring.cpp:48-59,181-182`;
  `src/agent/workspace_runtime.cpp:48-63,112`;
  `src/agent/agent_loop.cpp:22-33,444-471`;
  `src/agent/context_assembler.cpp:33-65`.

---

## 14. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-19 | Initial owning spec for Wave 3. Re-numbers the reserved `27-system-prompt.md` to 36 because 27-35 are taken. Pins the registry, order tables, persona, instructions loader, runtime contexts, tool presentation, provenance, identity migration, and the digest/replay contract. |


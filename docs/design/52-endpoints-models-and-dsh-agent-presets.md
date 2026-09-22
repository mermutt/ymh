# 52 — Named Endpoints & Models, and dsh-Aligned Agent Presets

```
Status: verified (Rev 2) — the five-reviewer adversarial gate PASSED (0 open
        HIGH / 0 MEDIUM). Design only; no code may be written for
        either part until this spec is `verified` (AGENTS.md, the rule).
        Rev 2 applies the first independent adversarial review (five reviewers:
        scope, schema, import, Part B, completeness); every finding is recorded
        in §12 with its fix. Seven HIGH, fourteen MEDIUM, eighteen LOW.

Authority: owning spec for the user request of 2026-09-22 ("read the keys,
        endpoints and models [from localcode]"; "for profiles … closely follow
        dsh … number of agents, permissions, prompts etc."). Part A extends the
        single-endpoint/single-model LLM config into named endpoints + named
        models and extends the localcode import to fill them. Part B adds a
        shipped, dsh-aligned agent-preset roster on top of the mechanism already
        pinned by `42-agent-presets.md`.

Component: 52 (owning spec). Two sub-parts, one gate. Part A owns
        `llm.endpoints`, `llm.models`, `llm.active_model`, the model-resolution
        function, and the localcode `providers`/`profiles` import expansion.
        Part B owns the shipped preset roster (`minimal`, `standard`; `ptc` and
        `cordis` reserved), the preset persona/instruction/permission binding,
        the shipped-presets install root, and the in/out capability list.

Depends on: `42-agent-presets.md` (verified; the preset mechanism Part B
        builds on), `43-wave5-dependency-errata.md` (verified; the `presets.*`
        config keys), `21-config-jsonc-errata.md` (verified; the strict loader),
        `46-permissions-ui-errata.md` (verified; D12 owns the localcode import
        and the literal-`api_key` rule), `08-llm-provider.md`,
        `09-permissions.md`, `47-muse-glimmer-support.md` (verified; the
        model-profile seam), `26-dsh-alignment.md` +
        `26-dsh-alignment-part2.md`, `36-prompt-registry.md`,
        `20-skills.md`, `44-goals-jobs-commands.md`, and the working tree at
        authoring time.

Relation to: `00-architecture.md` §38 "Profiles" (line 3986) — **realized** with a
        concrete mechanism (the spec-42 preset, extended here); §38's `profiles/`
        tree is superseded. It **confirms**
        `00-architecture.md` §55 (line 4889) stands: the Cordis-configuration
        omission is untouched; 52 adds no Cordis format, plugin dependency graph,
        or hot module replacement. It also **confirms** `00 §3` Non-Goals (line
        195) stand: capability compositions are not on that list, and "full
        Cordis compatibility" / "complex workflow authoring" remain out. See §1.3.
```

This document is a **pin**, not a proposal. Every `file:line` claim in §2 was
re-derived against the working tree at authoring time with `grep -n` / `sed`.
Claims that could not be verified are recorded as open questions in §11 rather
than asserted.

**A correction to the request's premise, stated up front.** ymh already has a
dsh-aligned agent-preset mechanism: `42-agent-presets.md` is **verified** and
**implemented** (`include/ymh/agent/preset.hpp`, `src/agent/preset.cpp`, 613
lines). Part B therefore does **not** introduce a new "profile/preset concept";
it extends the verified spec-42 mechanism with (a) a *shipped roster* mirroring
dsh's `minimal`/`standard`/`ptc`/`cordis`, (b) the persona / instruction /
permission binding dsh's presets carry, and (c) the explicit in/out capability
list. Introducing a second, competing `profiles.*` config section would collide
with spec 42's `presets.*`; §5.1 pins that it is not introduced.

---

## 1. Purpose, scope, and supersession map

### 1.1 In-scope requirements (verbatim)

From the user (2026-09-22):

> "Make ymh able to read the keys, endpoints and models [from localcode]. Do not
> follow profiles. For profiles I want ymh to closely follow dsh - everything
> from there, like number of agents, permissions, prompts etc. So when importing
> localcode config place information into ~/.config/ymh/config.json in a manner
> compatible with dsh. I will be asking you to periodically review dsh updates
> and import into ymh everything related to agent handling (not code but design,
> prompts, etc)."

Three sentences, three pinned interpretations:

1. **"read the keys, endpoints and models"** → Part A: the localcode import
   must carry **all** `providers` (as ymh endpoints, with their keys) and **all**
   `profiles` (as ymh models), not only the `default_profile`'s provider/model as
   it does today.
2. **"Do not follow profiles"** → localcode `profiles` are **not** imported as
   ymh "profiles". They are decomposed into ymh endpoints + models. ymh's
   profile-shaped concept follows dsh instead.
3. **"closely follow dsh … number of agents, permissions, prompts"** → Part B:
   ymh's preset concept mirrors dsh's *agent presets* (capability compositions:
   tools, persona, instructions, permissions, subagent behaviour), not model
   configs. A periodic dsh re-sync process is requested; §11 OQ-2 records it as
   a product decision.

### 1.2 What this changes, in one sentence

ymh gains **named endpoints and named models** in `config.jsonc` (and imports
all of localcode's `providers`/`profiles` into them), and gains a **shipped,
dsh-aligned agent-preset roster** with persona / instruction / permission
binding, without introducing a Cordis-compatible configuration format and
without a second preset-root config section.

### 1.3 Amendment / supersession map

| # | Spec | What 52 does | Why |
|---|---|---|---|
| 00 | `00-architecture.md` §55 `:4889` ("Cordis-compatible configuration" deliberate omission) | **Confirms §55 stands.** The omission list is untouched: 52 adds no Cordis configuration, no plugin dependency graph, and no hot reload. 52 only reuses spec 42's preset composition, which itself adopted the *semantics* without Cordis config. | The user asks ymh to follow dsh's agent handling; that does not require a Cordis format. |
| 00 | `00-architecture.md` §38 `:3986` ("Profiles") | **Realizes** the profile/bundle idea as the spec-42 preset mechanism, extended by Part B. §38's `profiles/` tree (default/local/remote/minimal/coding/research) is superseded by `presets.<id>/preset.jsonc`. | §38 was a sketch; 42 pinned the mechanism. |
| 00 | `00-architecture.md` §3 Non-Goals `:195` | **Confirms §3 stands** for the agent-preset layer: capability compositions are not a listed non-goal, and "complex workflow authoring" / "full Cordis compatibility" remain out. | Same as above. |
| 08 | `08-llm-provider.md` §5.1 `:620-654`, §5.2 `:669`, §5.3 `:693`, §6.4 `:855` | **Amends**: `LLMProviderConfig` is constructed from a `ResolvedModel` (endpoint + model entry), not from a flat `llm` section; multiple endpoints coexist. `api_key` literal is already amended by 46-D12.3 and is unchanged here. | Part A. |
| 09 | `09-permissions.md` §3.3 `:325`, §2.3 | **Amends**: adds `permissions.presets` (named sandbox+approval bundles) and per-session permission-preset pinning; the `Profile` rule layer (`PolicyRule::Layer::Profile`) is activated by preset permission binding. | Part B permission binding; dsh's `dsh-permission-presets`. |
| 21 | `21-config-jsonc-errata.md` §7.1 `:1128`, §7.7 `:1242`, §7.9 `:1364`, §7.10 `:1387` | **Amends**: adds `llm.endpoints`, `llm.models`, `llm.active_model`, `permissions.presets`, `permissions.default_preset`, `agent.sandbox`; extends the strict loader with a named-map rule (§3.5). The typo-is-fatal guarantee is preserved. | Part A schema; Part B permission/sandbox keys. |
| 25 | `25-ui-ux-errata.md` D14/D15 (`:1738-1810`) | **Amends by reference** via 46-D12.9: the import trigger is unchanged (first-run, TUI-only, no `--config`, interactive). Part A only widens *what* is written, not *when*. | No new trigger. |
| 42 | `42-agent-presets.md` | **Extends, does not replace.** 42 owns the roster mechanism, `PresetRow`, the scope chain, and `presets.*`. 52 owns the *shipped roster*, the persona `complete`/`include_runtime_context` row extension, the permission binding, and the shipped-root path. 42's `presets.*` keys are reused verbatim; no `profiles.*` key is added. | The request's "profiles" = dsh agent presets = spec 42's presets. |
| 43 | `43-wave5-dependency-errata.md` | **Amends by reference**: adds the new row fields and config keys to the Wave-5 dependency contract. | Keeps the 42/43 contract coherent. |
| 46 | `46-permissions-ui-errata.md` D12 (`:2467-2776`) | **Extends**: D12's mapping is retained in full; 52-D7/D8 add the all-providers/all-profiles mapping and correct the forced-`muse-glimmer` defect. | The user wants all endpoints/models, not just the default. |
| 28 | `28-llm-service-boundary-errata.md` §3.1 (`:109`, `:291`, `:365`), §1.1 (`:120`) | **Amends**: `to_provider_config` / `LLMProviderConfig` construction is fed from a `ResolvedModel` instead of the flat `llm` section; 28's `LlmRuntime` seam and `prepare_call` contract are otherwise untouched. 28 cites the current constructor at `src/cli/wiring.cpp:70-74`. | Part A must not silently diverge from 28's provider-boundary pin. |
| 47 | `47-muse-glimmer-support.md` §4.2 (`:312`), §10.2 (`:1067`) | **Amends**: the model profile becomes a per-model property (`llm.models.<n>.profile`), not a global forced value; adds `import_profile_id_for_model()`. | The current import forces `muse-glimmer` on every openai-compatible provider (`src/config/config.cpp:1690-1696`). |
| 10 | `10-supervisor-tui.md` | **No key change / coordination only.** The `/model`/`/endpoint`/`/permission` pickers remain spec 10/25; 52 only supplies the config keys and resolution they will surface. §1.4 excludes the UI. | Keeps the UI contract owned by 10. |
| 44 | `44-goals-jobs-commands.md` | **No key change / coordination only.** Goals/jobs/commands are not preset-gated in v1 (52-OQ-5); Part B only records whether a preset may carry them. | Avoids a second owner for goals/jobs. |
| 20 | `20-skills.md` §5.6 (`:1031`) | **No key change.** `skills.*` and `presets.*` are disjoint. 52 confirms `presets.*` is spec 42's agent-preset root config, not a skill setting. | Avoid the collision the request warns about. |
| 36 | `36-prompt-registry.md` §2.3, §2.4 | **Amends by reference**: the preset persona row binds through the existing `deployment:persona-prefix`/`-suffix` sections and the existing `InstructionLoader`; the runtime context becomes preset-scope-aware. | Part B persona/instructions. |

### 1.4 Scope boundaries

- **In scope:** the `llm.endpoints`/`llm.models` schema and resolution; the
  all-providers/all-profiles localcode import; the shipped preset roster and its
  persona/instruction/permission binding; the in/out capability list; the dsh
  re-sync process *decision*.
- **Out of scope:** any UI surface (spec 10/25 own `/model`, `/endpoint`,
  `/permission` pickers); a keyring (deferred by 46); remote/SSH transports
  (out of scope, `DESIGN_STATUS.md`); a Cordis-compatible config format
  (`00 §55`); the PTC `run_code` SDK, `workflow`, and `ralph` (`26 §5.1`);
  `send_message`/`interrupt_agent`/`list_agents` (spec 42 OQ-3); goal/job
  *changes* (spec 44 owns them; Part B only records whether a preset may carry
  them); per-preset model selection (52-OQ-6).
- **Prompt parity is partial, and stated plainly.** Of dsh's *prompt* surface,
  v1 carries only the **persona** (prefix/suffix/`complete`/runtime-context).
  Plan mode already exists in ymh (spec 25-D3) but is **not preset-controlled**;
  compaction prompts and tool descriptions are host-global; per-preset
  instruction `maxBytes`/candidates are **absent** (the `standard`
  `instructions` row is a no-op marker). Part B therefore genuinely delivers
  *agents, tools, and permissions*, and only the persona leg of *prompts*; this
  is recorded as 52-OQ-13 and is not represented as full dsh prompt parity.
  See §5.2 and §5.5.

### 1.5 Terminology (pinned)

- **Endpoint** — a named connection: provider kind, `base_url`, credentials,
  timeouts, retry, `max_concurrency`. ymh today has exactly one (the flat
  `llm.*` fields). **Model** — a named generation target: an endpoint reference,
  a wire model id, and generation parameters (`max_tokens`, `context_window`,
  `reasoning_effort`, model-profile id, sampling). ymh today has exactly one
  (`llm.model`).
- **Agent preset** — spec 42's unit: a JSONC directory `<root>/<id>/preset.jsonc`
  selecting tools, prompt sections, skills, and persona for one session
  (`42 §2.1`). This is the ymh spelling of dsh's agent preset. It is **not** a
  model config.
- **Permission preset** — a named `{sandbox, approval}` bundle (dsh
  `dsh-permission-presets`), mapped onto ymh's `SandboxMode` +
  `PermissionDefaults`.
- **Shipped preset** — a preset that ymh installs under the compiled-in
  shipped-presets root (see §5.4). Today `default_presets_shipped_root()`
  returns an empty path (`src/agent/preset.cpp:307-309`), so no shipped preset
  exists yet.

---

## 2. Current state (verified)

### 2.1 The single endpoint and single model

`LlmSettings` is one flat struct (`include/ymh/config/config.hpp:122-150`):
`provider`, `base_url`, `model`, `api_key_env`, optional `api_key`,
`max_tokens`, `reasoning_effort`, `profile`, sampling fields, `max_concurrency`,
timeouts, and `retry`. There is no notion of a second endpoint or a second
model.

`apply_llm` (`src/config/config.cpp:406-531`) accepts **two shapes**: a flat
`llm.provider`/`llm.model`/… and a nested `llm.default.{…}` object. When
`llm.default` exists it becomes the read target and the flat keys are accepted
but **ignored** — a pinned quirk retained by `21 §7.7` (`:1279-1284`, J12).
Both shapes are amended by 46-D12 to carry `api_key` and `max_tokens`
(`src/config/config.cpp:429-446`).

`effective_model` (`src/config/config.cpp:1734-1739`) is the whole selection
algorithm: `agent.model` if non-empty, else `llm.model`. `agent.model` is set by
the `agent.model` config key (`src/config/config.cpp:297`), `YMH_AGENT_MODEL`
(`src/config/config.cpp:1364-1366`), or `--model` (`src/cli/cli.cpp:128-130`).
The CLI also overrides `llm.provider`/`llm.base_url`/`llm.api_key_env` on the
single endpoint (`src/cli/cli.cpp:131-139`).

The provider is built by `to_provider_config` (`src/cli/wiring.cpp:61-81`) and
`to_agent_config` (`src/cli/wiring.cpp:198-209`); the sandbox is **hardcoded**
to `SandboxMode::Workspace` (`src/cli/wiring.cpp:203`) with no config key
(verified: `grep -n "agent.sandbox\|\"sandbox\"" src/ include/` returns only the
hardcode and the enum).

### 2.2 The strict loader

`reject_unknown` (`src/config/config.cpp:80-101`) walks the object's keys and
fails on any key not in a static `initializer_list`. It is applied at every
level: top level (`apply_document`, `:941-946`), `llm` (`:408-413`), `llm.default`
(`:420-425`), `llm.default.retry` (`:390-392`), and every section. Unknown key →
`ConfigError` (`fail`, `:49-51`); `21 §7.10` (`:1387-1396`) pins this as
"fail loud". The loader **cannot** enumerate user-defined map keys, so a new
`endpoints`/`models` map needs an extension (§3.5).

Layer order (`load_config`, `src/config/config.cpp:1404-1413`): defaults →
global (required) → workspace (optional) → `YMH_*` env (`apply_env_overrides`,
`src/config/config.cpp:1332-1398`) → CLI (`src/cli/cli.cpp:128-146`). Scalars are
last-writer-wins; arrays replace wholesale (`21 §3.4`, `:492-501`).

### 2.3 The literal `api_key` rule

46-D12.2 supersedes "secrets never in `Config`": `llm.api_key` is the one
permitted literal secret, accepted **only in the global layer**, only from a
`0600` file (`require_private_config_file`, `src/config/config.cpp:55-63`;
`apply_llm` guard, `:429-438`). It outranks `api_key_env` and is never logged,
rendered, or event-logged; the redactor masks `"api_key"\s*:\s*"…"`
(`46-D12.11`). `ProviderRegistry::create` skips the `api_key_env` validation
when a literal key is present (`46-D12.2`).

### 2.4 The localcode import

The import is split across two files — **the request's "verified facts" are
partly wrong on this point**, and the correction is pinned here:

- **`src/config/config.cpp`** owns the pure mapping:
  `build_localcode_import` (`:1572-1732`), `localcode_config_path` (`:1264-1269`),
  and the helpers `escape_localcode_reference` (`:1424`),
  `map_localcode_tool` (`:1481`), `is_valid_localcode_decision` (`:1495`),
  `map_localcode_mcp_server` (`:1537`), `is_http_url` (`:1460`).
  It is declared in `include/ymh/config/config.hpp:349-353`.
- **`src/cli/cli.cpp`** owns the CLI-side helpers:
  `prompt_import_yes` (`:650`), `read_localcode_document` (`:671`),
  `localcode_provider` (`:716`), `print_localcode_notes` (`:742`),
  `validate_imported_mcp` (`:768`), `write_imported_config` (`:851`), and
  `maybe_import_localcode_config` (`:944-1000`), invoked at `:1183`.
  **Defect (fixed in Rev 2):** `print_localcode_notes` resolves the provider
  type only through `localcode_provider` (`:716-740`), which follows
  `default_profile`; with a real config (`default_profile = balanced` → `ted-ai`)
  it emits **no** note for a skipped `bedrock` provider, so the skip is silent
  (52-F7 / 52-F20). Rev 2 replaces this emitter (§4.1).

Today `build_localcode_import` maps **only the `default_profile`'s provider and
model**:

- `mcp_servers` → `mcp_servers` (`:1583-1600`);
- `auto_compact_enabled`/`auto_compact_percent` →
  `agent.compaction.enabled`/`threshold_ratio` (`:1602-1614`);
- `max_concurrent_tasks` → `llm.default.max_concurrency` (`:1615-1620`);
- `skip_permissions` → `permissions.default = "allow"` (`:1622-1625`);
- `permission` object → `permissions.rules[]` (`:1627-1655`);
- `default_profile` → `profiles.<name>` → `providers.<name>`:
  `type` `"openai-compatible"`/`"openai-compat"` → openai-compatible
  (`:1682-1689`); then `llm.default.profile = "muse-glimmer"` **unconditionally**
  (`:1690-1696`), `llm.default.api_key` (`:1697-1700`),
  `llm.default.max_tokens` (`:1701-1706`),
  `agent.compaction.context_window_tokens` (`:1707-1711`),
  `llm.default.base_url` + `llm.default.model` (`:1712-1727`).

Two defects follow from this and are fixed by Part A:

1. **Only the default provider/model are imported.** The other providers
   (`itg`, `ted-ai`, `bedrock`) and profiles (`strong`, `balanced`, `cat-md1`,
   `cheap`, `cat-gma`) are dropped.
2. **`muse-glimmer` is forced on every openai-compatible import**
   (`:1690-1696`), including `nvidia/nemotron-3-super` and `gemma-4-31B-it`,
   whose wire behaviour the Muse profile is not for. The `default_import_profile_id`
   seam (`src/llm/model_profile.cpp:52-54`) returns the single Muse id.

The trigger is create-only and unchanged (`maybe_import_localcode_config`,
`:944-1000`): TUI only, no `--config`, the conventional global config
**directory** must be absent, localcode must be a regular file, interactive. If
the directory exists the import is skipped entirely — **no merge** (`:953-956`).
The write is a `0600` temp + rename (`write_imported_config`, `:851-898`), and
the mapped document is validated through `apply_jsonc_file(..., required=true)`
before it lands.

### 2.5 The existing agent-preset mechanism (spec 42)

Spec 42 is **verified** (`DESIGN_STATUS.md:64`) and implemented. The mechanism
is:

- `include/ymh/agent/preset.hpp`: `PresetRow` (`:53-63`) with `id`, `group`,
  `disabled`, `persona_prefix`, `persona_suffix`, `tool_filter`, `skill_roots`,
  `sections`, `config`; `AgentPreset` (`:65-70`); `PresetConfig` (`:73-79`) with
  `root`, `default_id`, `include_shipped_root`, `include_user_root`, `max_depth`;
  `AgentPresetRoster` (`:135-189`).
- `src/agent/preset.cpp`: `parse_preset` (`:80`), `register_preset_rows`
  (`:419-461`), `mount` (`:463`), `select` (`:584`),
  `apply_child_composition` (`:524`), `check_delegation_depth` (`:602`).
- Preset format is **JSONC directories** `<root>/<id>/preset.jsonc`
  (`42 §2.1.1`), not `agent.cordis.yml` (`42-D1`, `26 §6` OQ8).
- `presets.*` config keys (`root`/`default`/`include_shipped_root`/
  `include_user_root`/`max_depth`) are parsed by `apply_presets`
  (`src/config/config.cpp:775-798`) into `PresetsSettings`
  (`include/ymh/config/config.hpp:230-237`).
- `register_preset_rows` applies `persona_prefix`/`persona_suffix` as scoped
  `deployment:persona-prefix`/`-suffix` sections (`src/agent/preset.cpp:435-452`)
  and intersects `tool_filter`s (`:453-460`).
- **Gap:** `register_preset_rows` does **not** honor a `complete` flag on the
  persona row, and `PresetRow` has no `include_runtime_context` field
  (verified: `PresetRow` at `preset.hpp:53-63`; the persona branches at
  `preset.cpp:435-452` set no `complete`). dsh's `minimal` preset needs both.

### 2.6 Instruction-file loading (already dsh-shaped)

`include/ymh/prompt/instructions.hpp` already implements dsh's
`dsh-agent-instructions` shape: candidates default to
`{"AGENTS.md", "CLAUDE.md"}` (`:22`), local candidates `{"AGENTS.local.md",
"CLAUDE.local.md"}` (`:23`), broad-to-specific discovery, `<system-reminder>`
wrapping, and `max_bytes` (required when enabled). It is registered at
`src/agent/workspace_runtime.cpp:199-201` when
`config.prompt.instructions_enabled`. The config keys are parsed by
`apply_prompt` (`src/config/config.cpp:860-904`). dsh's
`agent-instructions: maxBytes: 65536` maps 1:1 onto
`prompt.instructions.max_bytes`.

### 2.7 The persona seam (already dsh's standard persona)

`include/ymh/prompt/persona.hpp` pins `PersonaConfig { prefix, suffix, complete,
include_runtime_context }` (`:16-20`) and `register_persona` (`:32`). The shipped
default (`src/prompt/persona.cpp:13-16`) is **byte-identical to dsh's `standard`
preset persona**: prefix `"You are a coding agent powered by the {{model}}
model."`, suffix `"Your working directory is {{cwd}}."` (compare
`standard/agent.cordis.yml:24-30`). It is applied at
`src/agent/workspace_runtime.cpp:186`.

**Gap:** `apply_prompt` accepts only `{"instructions"}`
(`src/config/config.cpp:861`); the `prompt.persona.*` keys listed in
`26-dsh-alignment-part2.md:1302-1305` are **not parsed**. The persona is always
`default_persona_config()`. Part B binds persona through the preset rows
instead (spec 42 already does this for prefix/suffix), and does not add
`prompt.persona.*`.

### 2.8 Sandbox and permissions

`SandboxMode` is `Workspace | ReadOnly | Unrestricted`
(`include/ymh/execution/environment.hpp:22-26`). It is hardcoded to `Workspace`
in wiring (`src/cli/wiring.cpp:203`); there is **no config key**. `PermissionDefaults`
(`include/ymh/config/config.hpp:95-105`) has `shell`/`write`/`read`/
`default_verdict`/`rules`; `PermissionConfig` (`include/ymh/policy/permission_policy.hpp:141-147`)
has `default_verdict` + `rules` + `tool_defaults`. `PolicyRule::Layer` already
has a `Profile` value (`include/ymh/policy/permission_policy.hpp:86-95`), but no
code path stamps it from a preset (verified: the config parser stamps
`Global`/`Project`; grep for `Layer::Profile` in `src/` returns no producer).

---

## 3. Part A — Named endpoints and models

### 3.1 Decision 52-D1: the schema

Two new maps under `llm`, plus an active-model selector:

```jsonc
"llm": {
  "endpoints": {
    "ted-ai": {
      "provider": "openai-compatible",          // default "openai-compatible"
      "base_url": "http://openai.tedai.ibm.com",
      "api_key": "sk-...",                       // literal; GLOBAL layer only; 0600
      "api_key_env": "TEDAI_API_KEY",            // fallback when api_key absent
      "headers": { "X-Client": "ymh" }, // optional; GLOBAL layer only; redacted
      "max_concurrency": 4,
      "connect_timeout_ms": 10000,
      "idle_timeout_ms": 60000,
      "request_timeout_ms": 120000,
      "retry": { "max_attempts": 3, "base_delay_ms": 500, "max_delay_ms": 30000,
                 "jitter": 0.25, "honor_retry_after": true }
    },
    "itg": {
      "provider": "openai-compatible",
      "base_url": "http://105.140.238.68:4000/v1"
    }
  },
  "models": {
    "balanced": {
      "endpoint": "ted-ai",                      // REQUIRED; names an endpoint
      "model": "Muse-Glimmer-30B",               // REQUIRED; wire id
      "profile": "muse-glimmer",                 // model-profile id; "" = inert
      "max_tokens": 8192,
      "context_window": 32768,
      "reasoning_effort": "low",
      "temperature": 1.0,
      "top_p": 0.95,
      "top_k": 64
    },
    "cheap": { "endpoint": "itg", "model": "nvidia/nemotron-3-super" }
  },
  "active_model": "balanced"                     // names an llm.models entry
}
```

- `endpoints.<name>` owns the **connection**. `models.<name>` owns the
  **generation**.
- `models.<name>.endpoint` is required and must name an existing endpoint
  (52-I2). `models.<name>.model` is required and non-empty. A missing or empty
  `endpoint`/`model` is a `ConfigError` at load (52-F19), never a request-time
  surprise. In contrast, an **empty endpoint entry** `{}` is **valid**: it yields
  the built-in endpoint defaults (`provider = "openai-compatible"`,
  `base_url = "https://api.deepseek.com/v1"`), which is the documented way to
  declare "the default connection under a name". A named endpoint with an empty
  `base_url` is therefore only produced by explicitly setting `"base_url": ""`;
  that is accepted (the adapter resolves it at request time) and is *not* the
  same as `{}`.
- `models.<name>.profile` is the model-profile id; unknown non-empty ids are a
  `ConfigError` at load, exactly as `llm.default.profile` is today
  (`src/config/config.cpp:1409-1411`).
- `llm.default` and the flat `llm.*` keys are **retained unchanged** as the
  *default endpoint + default literal model* (52-D4).
- `llm.active_model` names a model entry; an unknown name is a `ConfigError`
  (52-F1).

**Names.** An endpoint or model name is a non-empty string of at most 64 bytes
matching `[A-Za-z0-9][A-Za-z0-9._-]*`. The grammar keeps error text and
`--model <name>` unambiguous. A name that violates it is a `ConfigError`
(52-F5). Duplicate keys within one JSONC object are last-wins (nlohmann default,
`21 §7.10` J14); across layers, maps merge per entry (52-D4).
- **Entry names are an independent namespace.** `default`, `active_model`,
  `provider`, `base_url`, `retry`, … are legal *endpoint/model names*; a name is
  never compared against the schema allowlist, and a model named `default` is
  unrelated to the sibling `llm.default` object. No reserved words are rejected
  (an entry named `active_model` is legal and distinct from `llm.active_model`).
  The only constraint is the name grammar above.
- **Selectors are independent too.** `llm.active_model` may name a model whose
  name is `default`; `--endpoint default` selects `llm.endpoints.default`.

### 3.2 Decision 52-D2: model → endpoint reference

A model references its endpoint by **name**, not by copying `base_url`/keys.
This keeps one source of truth for a connection shared by several models (the
user's `strong`/`balanced`/`cat-md1` all use `ted-ai`). A model that names a
missing endpoint fails at load (52-F2), never at request time.

### 3.3 Decision 52-D3: active-model resolution and precedence

```cpp
namespace ymh {

struct ResolvedEndpoint {
    std::string  name;                       // "" for the anonymous default
    ProviderId   provider;                   // registry key
    std::string  base_url;
    std::string  api_key_env;
    std::optional<std::string> api_key;      // literal (global/0600 only)
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::chrono::milliseconds request_timeout{120'000};
    RetrySettings              retry;
    std::size_t                max_concurrency = 4;
};

struct ResolvedModel {
    ResolvedEndpoint             endpoint;
    std::string                  model_name;      // llm.models entry name; "" for a literal id
    std::string                  model_id;        // wire id, never empty
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::uint32_t> context_window;
    std::optional<std::string>   reasoning_effort;
    ModelProfile                 profile;         // inert when id empty
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;
    std::optional<std::string>   tool_choice;
    std::vector<std::string>     stop;
    std::optional<std::uint32_t> seed;
    std::string                  source;          // selector ORIGIN: "agent.model" |
                                                  // "llm.active_model" |
                                                  // "llm.default.model" | "builtin"
};

// `ResolvedModel`/`ResolvedEndpoint` are NEW types introduced by this spec (no
// definition exists in the tree today). `model_name` is the selected
// `llm.models` entry name ("" for the literal-id/default paths); `source` is the
// selector that won, never the resolved name. Use `model_name` /
// `endpoint.name` to report what was resolved (52-F3).
//
// Total, deterministic, noexcept-throwing only ConfigError. Replaces
// `effective_model` as the provider/agent construction input; `effective_model`
// is retained as a thin wrapper returning `resolve_model(config).model_id`.
[[nodiscard]] ResolvedModel resolve_model(const Config& config);

} // namespace ymh
```

Resolution order (first non-empty selector wins):

1. **`agent.model`** — dual interpretation, pinned: if the string equals a key
   in `llm.models`, that model entry is selected (with its endpoint); otherwise
   the string is a **literal wire id** bound to the default endpoint. This keeps
   `--model deepseek-flash` (a literal id) working while allowing
   `--model balanced` (a name). Name match takes precedence (52-F6).
2. **`llm.active_model`** — **name only**; must resolve to a model entry, else
   `ConfigError` (52-F1).
3. **`config.llm.model`** — the single already-resolved field (produced by the
   layer-by-layer `apply_llm` passes of `21 §7.7`; see below); a literal wire id
   on the default endpoint. Backward-compatible path. **Not** "`llm.default.model`
   or flat `llm.model`": after loading there is no nested-vs-flat distinction to
   evaluate, because `apply_llm` (`src/config/config.cpp:406-531`) collapses the
   `llm.default` object and the flat keys into one read target before writing the
   field. Step 3 reads `config.llm.model` **verbatim**.
4. **built-in `"deepseek-flash"`** on the built-in default endpoint.

The **default endpoint** is the accumulated `config.llm.*` connection fields
after **per-document section selection** (`21 §7.7`) applied **layer-by-layer**:
each document's `apply_llm` picks either its own `llm.default` object or its own
flat keys, and writes the shared `Config`. A later layer's *flat* keys therefore
DO override an earlier layer's *nested* object — this is per-document, not a
"nested wins" merge, and it is exactly what preserves 52-I5's byte-identical
guarantee. Do **not** describe the default endpoint as a merge of the flat fields
and the `llm.default` object. A model entry's endpoint overrides the default
endpoint's connection fields for that selection only.

**Observability caveat (pinned).** If the nested-vs-flat *origin* must ever be
observable to `resolve_model` (e.g. to report which document supplied a field),
that requires a **new `Config` field** carrying the provenance; `LlmSettings`
(`include/ymh/config/config.hpp:122-150`) is flat scalars and `apply_llm` throws
the distinction away. Rev 2 does **not** add such a field: the semantics are the
existing loader's, and no resolution decision depends on provenance.

**Empty selectors (pinned).** An empty selector string is treated as **unset**
and the next rule is tried: `agent.model = ""`, `llm.active_model = ""`, and
`YMH_LLM_ENDPOINT=""` / `--endpoint ""` all fall through. A *non-empty*
`YMH_LLM_ENDPOINT`/`--endpoint` that names no `llm.endpoints` entry is a
`ConfigError` at load (52-F2), never a silent fall-through to the default
endpoint.

**New env/CLI selectors (additive):** `YMH_LLM_ACTIVE_MODEL` (sets
`llm.active_model`), `YMH_LLM_ENDPOINT` (selects an endpoint by name for the
literal-id path), `--endpoint <name>` (CLI, sets `YMH_LLM_ENDPOINT`'s effect).
`--model` keeps its current meaning (sets `agent.model`) and gains the dual
name-or-id interpretation. `--provider`/`--base-url`/`--api-key-env` keep
overriding the default endpoint's connection fields.

### 3.4 Decision 52-D4: backward compatibility and layering

- A config that uses only `llm.default`/`llm.*`/`agent.model` and no new keys
  resolves to **exactly** today's `LLMProviderConfig` and `AgentConfig`
  (52-I5). No new key is required.
- `llm.endpoints` and `llm.models` are **maps**; across layers they merge
  **per entry**, and a colliding entry name replaces **WHOLESALE** (last writer
  wins for the whole entry), unlike arrays which replace wholesale. This is the
  one new merge semantic; it is additive and does not change any existing key's
  merge. There is **no field-level merge**: a workspace-layer
  `llm.endpoints.ted-ai = { "base_url": … }` replaces the global
  `llm.endpoints.ted-ai = { "base_url": …, "api_key": … }` entirely, and the
  absent fields revert to the `EndpointSettings` **built-in defaults**, *not* to
  the shadowed global entry. (Field-level merge was rejected: it would make
  "`api_key` is global-layer only" a per-document accident and silently mix
  layers.)
- **Shadowing a literal credential is a load error (52-F18).** Because
  replacement is wholesale and `api_key`/`headers` are global-layer only
  (52-D6, 52-I4), a workspace-layer entry that collides with a global-layer entry
  which carried a literal `api_key` (or credential-bearing `headers`) would drop
  the secret and fail auth at request time. The loader therefore fails with a
  clear `ConfigError` (`workspace endpoint '<n>' shadows a global endpoint with a
  literal api_key`) rather than silently discarding it. A workspace entry may
  still shadow a global entry that carried **no** credential.
- `agent.model` (config/env/CLI) continues to outrank `llm.model`, now with the
  dual interpretation of 52-D3.
- The retained `21 §7.7` quirk (flat keys ignored when `llm.default` is present)
  is unchanged and explicitly preserved (52-OQ-8).

### 3.5 Decision 52-D5: extending the strict loader

`reject_unknown` is extended with one new helper, used only for the two named
maps:

```cpp
// Validates a user-named map: each value must be an object, its keys must match
// the name grammar, and each value's own keys must be in `allowed`. The map's
// keys are names, not schema keys, so they are not enumerated — but every key
// INSIDE each value is, so the typo-is-fatal guarantee is preserved.
void reject_unknown_named_map(const Json& table,
                              std::string_view table_name,
                              std::initializer_list<std::string_view> allowed,
                              const std::filesystem::path& source);
```

Pinned properties:

- The top-level `llm` allowlist gains `"endpoints"`, `"models"`,
  `"active_model"`.
- `llm.endpoints.<name>` values are checked against the endpoint allowlist
  (`provider`, `base_url`, `api_key`, `api_key_env`, `headers`,
  `max_concurrency`, `connect_timeout_ms`, `idle_timeout_ms`,
  `request_timeout_ms`, `retry`).
- `llm.models.<name>` values are checked against the model allowlist
  (`endpoint`, `model`, `profile`, `max_tokens`, `context_window`,
  `reasoning_effort`, `temperature`, `top_p`, `top_k`, `tool_choice`, `stop`,
  `seed`).
- An unknown key **inside** an endpoint/model entry fails with the qualified
  name `llm.endpoints.<name>.<key>` (52-F24).
- **Nested maps recurse.** `reject_unknown_named_map` recurses into
  `endpoint.<name>.retry` against the **retry allowlist** (`max_attempts`,
  `base_delay_ms`, `max_delay_ms`, `jitter`, `honor_retry_after`) with the
  qualified label `llm.endpoints.<name>.retry`, mirroring `apply_retry`
  (`src/config/config.cpp:390-403`). It validates `endpoint.<name>.headers` as a
  JSON **object of strings** (`is_object()` and every value `is_string()`),
  mirroring the `read_string_object_as_env` shape used by `apply_llm`
  (`src/config/config.cpp:390-403`, `:660-679`). A non-object `headers` or a
  non-string header value is a `ConfigError` (never silently dropped).
- **Explicit type checks (52-F24/52-F19).** The loader pins
  `is_object()` for `llm.endpoints`, `llm.models`, and (when present)
  `llm.default`, and `is_string()` for `llm.active_model` and each
  `endpoint`/`model`/`provider`/`base_url`/`api_key_env`/`profile` value, with a
  clean `ConfigError` message of the existing form
  (`invalid type for '<label>'`). This mirrors the shipped checks
  (`src/config/config.cpp:415-418` for `llm.default`; the `section` lambda at
  `:958-962` for top-level sections).
- The map's own keys are validated against the name grammar, never against an
  allowlist. This is the only place a key is not schema-checked, and it is
  deliberate: the user defines the names.
- Unknown keys anywhere else remain fatal, exactly as today.

### 3.6 Decision 52-D6: literal keys with multiple endpoints

- `api_key` is permitted **per endpoint** and only in the **global layer**; a
  workspace-layer `api_key` is a `ConfigError` (mirrors
  `src/config/config.cpp:429-432`). The `0600` check
  (`require_private_config_file`) applies to the file, once, for any endpoint
  key present.
- **`headers` is a secret side-channel and is treated as one (52-F22).** A
  header value can carry a credential (`Authorization`, `X-Api-Key`, a tenant
  token), so `headers` is **global-layer only**, exactly like `api_key`: a
  workspace-layer `endpoints.<n>.headers` is a `ConfigError`, and the `0600`
  check applies when any endpoint entry has `headers`. Header *values* are added
  to the redaction surface: the 46-D12.11 redactor (`src/llm/redaction.cpp:52`)
  is extended so a `"headers"` object's string values are masked in the same way
  `"api_key"` is. This is stricter than §3.1's "NON-secret" comment, which is
  corrected: headers are *usually* non-secret, but the schema cannot tell, so the
  safe rule is global-only + redacted.
- **Failure-path prints are redacted (52-I4).** The import failure paths that
  print `error.what()` — `validate_imported_mcp`'s `fail_import`
  (`src/cli/cli.cpp:770-774`), `write_imported_config`'s `fail_import`
  (`:853-856`), and the validation catch (`:899-903`) — are wrapped in
  `redact_secrets` to match the already-redacted call sites (`:972`, `:989`).
  "Never logged" means every path, including failures.
- The flat `llm.api_key` (and `llm.default.api_key`) remains valid and applies
  to the **default endpoint**; it is not a third location.
- The redaction rule (`46-D12.11`) is unchanged and already matches nested
  `"api_key"` keys; 52 adds nested `"headers"` values to it.
- Multiple keys in one `0600` global file are the accepted model; a keyring
  stays deferred (46).

### 3.7 Invariants (Part A)

- **52-I1 (name well-formedness).** Every endpoint/model name is non-empty,
  ≤64 bytes, and matches `[A-Za-z0-9][A-Za-z0-9._-]*`. Guard: the loader
  grammar check (52-F5).
- **52-I2 (no dangling endpoint / required fields present).** Every
  `llm.models.<n>.endpoint` names an existing `llm.endpoints` entry, and every
  `llm.models.<n>` has a non-empty `endpoint` and `model`; resolution fails at
  load, never at request time (52-F2, 52-F19).
- **52-I3 (total resolution).** `resolve_model` returns a `ResolvedModel` with a
  non-empty `model_id`, a `model_name` consistent with `source`, and a populated
  endpoint for every valid config; it is deterministic and depends only on
  `Config` (52-I5, 52-F1).
- **52-I4 (secret containment).** A literal `api_key` and any credential-bearing
  `headers` value are read only from the global layer and only from a file with
  no group/other bits; they are never logged, rendered, or event-logged, on any
  path including failures. Guard: the existing `0600` + global-only checks,
  extended to every endpoint and to `headers`; redaction extended to header
  values; failure-path prints wrapped in `redact_secrets` (52-F4, 52-F18,
  52-F22).
- **52-I5 (byte-identical backward compatibility).** For any config with no
  `endpoints`/`models`/`active_model` key, `resolve_model` yields the same
  endpoint and model id that `to_provider_config`/`effective_model` produce
  today, and the serialized request body is byte-identical.
- **52-I6 (typo-is-fatal preserved).** The only keys not schema-checked are the
  user-defined endpoint/model names; every key inside an entry is checked, and
  an unknown one is a `ConfigError`. Guard: `reject_unknown_named_map`.
- **52-I7 (no profile forcing).** The model-profile id comes from the selected
  model entry (or the flat `llm.profile` on the default endpoint); no code path
  forces a profile based on provider type (fixes §2.4 defect 2).

### 3.8 Failure modes (Part A)

Component-local tags `52-F#`, each an instance of the shared finding it names,
using the taxonomy **asserted by specs 42/44** (`42 §7.1`, `44 §8.1`): F1 silent
degradation, F2 two sources of truth, F5 unsafe path resolution, F7 unbounded
growth, F9 cross-session leakage, F10 unauthorized action, F11 orphaned
identity. **Citation corrected (Rev 2):** `00 §54`'s own F1–F12 list
(`00-architecture.md:4832-4846`) is a *different* label set (F1 path/process
isolation, F2 background permission, …); the numbers below follow the 42/44
usage, not §54's literal text. Rev 2 adds F18–F23. Part A therefore spans
**F1–F10 and F18–F23**; the Part-B block F11–F17 is defined in §5.10. The global
set **F1–F23 is contiguous** (§7).

- **52-F1: unknown active model.** `llm.active_model` names no `llm.models`
  entry, so the run silently falls back to a built-in model. **Guard:** 52-D3
  step 2 throws `ConfigError`. Instance of F1 (silent degradation).
- **52-F2: dangling endpoint reference.** `llm.models.<n>.endpoint` names a
  missing endpoint; the first request fails or, worse, silently uses the default
  endpoint. **Guard:** 52-I2 load-time check. Instance of F2 (two sources of
  truth).
- **52-F3: name collision across layers.** A model/endpoint name is defined in
  the global and workspace layers with different bodies; an ambiguous merge
  yields the wrong connection. **Guard:** replacement is **wholesale** per entry
  (52-D4); the resolved entry is deterministic (the last layer wins) and is
  reported by `ResolvedEndpoint::name` and `ResolvedModel::model_name` — **not**
  by `ResolvedModel::source`, which is the selector origin, not the resolved
  name. Instance of F2.
- **52-F4: literal key leak.** An endpoint `api_key` (or a credential-bearing
  `headers` value) is placed in the workspace layer, or the global file is
  group/world-readable, or the key reaches a log/render/event on any path.
  **Guard:** 52-I4 / 46-D12.2, extended to `headers` (global-only + redacted) and
  to the import failure-path prints. Instance of F1.
- **52-F5: malformed name.** A name with a space, slash, or empty string makes
  the CLI selector ambiguous. **Guard:** 52-I1 grammar. Instance of F1.
- **52-F6: ambiguous `agent.model`.** A literal wire id that also happens to
  equal a model name is resolved as the **name** (pinned), which can surprise a
  user who meant the literal id. **Guard:** 52-D3 pins name precedence, and the
  resolution **emits a notice** (`--model '<id>' matched model name '<n>'; using
  the named entry`) rather than resolving silently; the resolution `source` and
  `model_name` are logged. Instance of F2.
- **52-F7: silently dropped provider.** A `bedrock` (or other unknown type)
  provider is dropped without a note, so the user believes it imported.
  **Guard:** 52-D7 collects one note **in the same iteration as the mapping**,
  and the emitter prints every collected note, so note count == skip count
  (52-F20). Instance of F1.
- **52-F8: profile forced on the wrong model.** `muse-glimmer` is applied to a
  non-Muse model (today's behaviour, `src/config/config.cpp:1690-1696`),
  changing wire behaviour. **Guard:** 52-I7 + `import_profile_id_for_model`.
  Instance of F1.
- **52-F9: import overwrites an existing config.** A re-run clobbers a
  hand-edited global config. **Guard:** the create-only trigger is retained
  (52-D9); a re-import is an explicit command, not automatic. Instance of F2.
- **52-F10: secret in the event log.** A resolved `ResolvedModel` is dumped into
  a request header or log. **Guard:** 52-I4; `LlmRequestHeader` never carries a
  key (08/28). Instance of F1.
- **52-F18: shadowed credential silently dropped.** A workspace-layer endpoint
  entry replaces a same-named global entry **wholesale** and the global entry
  held a literal `api_key`/credential `headers`; the secret disappears and every
  request 401s. **Guard:** 52-D4 load error (`workspace endpoint '<n>' shadows a
  global endpoint with a literal api_key`). Instance of F2.
- **52-F19: missing/empty required model field.** `llm.models.<n>` omits (or
  empties) `endpoint` or `model`, so a model entry resolves to the wrong
  endpoint or an empty wire id at request time. **Guard:** 52-I2/52-I3 load-time
  `ConfigError`; an empty *endpoint entry* `{}` is valid by contrast (52-D1).
  Instance of F2.
- **52-F20: one malformed import entry discards the whole import.** A single
  malformed `mcp_servers.<n>` entry throws `LocalcodeImportError`
  (`src/config/config.cpp:1589-1592`) and `build_localcode_import` returns
  `nullopt`, so every mapped endpoint/model is lost. **Guard:** per-entry
  skip-with-note (matching `validate_imported_mcp`, `src/cli/cli.cpp:768-790`),
  and the import still lands. Instance of F1.
- **52-F21: import write interrupted mid-write.** A crash between temp-file
  creation and rename leaves a partial or truncated global `config.jsonc`, which
  then fails the next load. **Guard:** the existing atomic write
  (`write_imported_config`, `src/cli/cli.cpp:851-910`: `O_EXCL` temp in the same
  directory, `fsync`-equivalent close, then `rename`) — a crash leaves only the
  `*.import.tmp` file, never a partial target. Instance of F2.
- **52-F22: secret in `endpoints.<n>.headers`.** A credential placed in a header
  value (workspace layer, or logged/rendered) leaks. **Guard:** 52-I4 — `headers`
  is global-layer only, subject to the `0600` check, and its values are added to
  the redactor (52-D6). Instance of F1.
- **52-F23: keyless endpoint.** An `openai-compatible` endpoint has neither
  `api_key` nor a resolvable `api_key_env`, so the first request fails with a
  401. **Guard:** the import emits a note for a keyless endpoint (52-D7); the
  config is **not** rejected (a keyless local endpoint is legitimate), and the
  provider surfaces a typed auth error at request time. Instance of F1.
- **52-F24: strict-loader violation inside a named-map entry.** An unknown key
  inside `llm.endpoints.<n>` / `llm.models.<n>` or inside a nested `retry`
  object; a non-object `endpoints`/`models`/`default`; a non-string
  `active_model`; or a `headers` value that is not a string. **Guard:** 52-I6 —
  `reject_unknown_named_map` validates every key INSIDE an entry against the
  entry allowlist and recurses into `retry`, so only the entry NAME is free; the
  loader fails with a qualified label. Instance of F1.

---

## 4. Part A — The localcode import expansion

### 4.1 Decision 52-D7: `providers` → endpoints

For **every** `providers.<name>` entry (not just the default profile's):

| localcode | ymh | rule |
|---|---|---|
| `type == "openai-compat"` or `"openai-compatible"` | `llm.endpoints.<name>.provider = "openai-compatible"` | canonicalized (existing, `:1687`) |
| `base_url` (non-empty, http/https) | `llm.endpoints.<name>.base_url` | non-http(s) → **skip the endpoint with a note** (existing `is_http_url`, `:1460-1462`) |
| `api_key` (non-empty) | `llm.endpoints.<name>.api_key` | literal; global layer; `0600` |
| no `api_key` and no resolvable `api_key_env` | — | **keyless endpoint**; imported with a note, not rejected (52-F23) |
| `type == "bedrock"` | — | **no ymh equivalent today**; skipped with a note. `region`/`profile` are dropped. |
| any other `type` (non-empty string) | — | skipped with a note |
| `type` missing or empty | — | skipped with a note |

**Notes are collected in the mapping pass (pinned; fixes the Rev-1 silent-skip
defect).** `build_localcode_import` returns a `{ document, notes }` result (an
out-`std::vector<std::string>& notes`, or a small result struct) and pushes one
note **in the same iteration that decides to skip** an entry. The CLI emitter
prints every collected note verbatim (redacted); it does **not** re-derive notes
by walking `providers`/`profiles` again, and the Rev-1
`print_localcode_notes`/`localcode_provider` default-profile-only helper
(`src/cli/cli.cpp:716-763`) is **replaced**. Contract: **note count == skip
count** for the import, so no skip can be silent. This is what makes 52-F7 and
52-F20 honest.

`bedrock` is called out explicitly: ymh's only registered provider is
`openai-compatible` (`08 §5.1`, `register_builtin_providers`), and
`ProviderRegistry::create` rejects an unknown provider name
(`include/ymh/llm/provider_registry.hpp:49-57`). A Bedrock adapter is **not**
introduced by this spec; the provider is skipped with a note, and adding a
Bedrock adapter is an open question (52-OQ-9).

A provider is imported even if no profile references it: the endpoint map is
the user's full set of connections.

### 4.2 Decision 52-D8: `profiles` → models

For **every** `profiles.<name>` entry:

| localcode | ymh | rule |
|---|---|---|
| `provider` | `llm.models.<name>.endpoint` | must name an imported endpoint; a dangling reference skips the model with a note |
| `model` | `llm.models.<name>.model` | required; a missing/empty model skips the entry with a note |
| `max_tokens` (> 0) | `llm.models.<name>.max_tokens` | retained (46-D12.4) |
| `context_window` | `llm.models.<name>.context_window` | retained (moved from `agent.compaction.context_window_tokens`) |
| model id matches Muse | `llm.models.<name>.profile = "muse-glimmer"` | via `import_profile_id_for_model(model_id)` |
| otherwise | `profile` omitted | inert; corrects §2.4 defect 2 |
| `default_profile` | `llm.active_model = <name>` | only when that profile imported; else omitted |

**Per-entry skip, never whole-import abort (52-F20).** A malformed entry in
`providers`, `profiles`, or `mcp_servers` is skipped **with a note** and the rest
of the import lands. Rev 1 inherited the shipped all-or-nothing behaviour
(`src/config/config.cpp:1589-1592` returns `nullopt` on the first malformed MCP
entry, discarding every mapped endpoint/model). Rev 2 pins the
`validate_imported_mcp` posture (`src/cli/cli.cpp:768-790`): the offending entry
is dropped, a note is collected, and the import proceeds. An import that maps
**zero** endpoints/models is still a successful write (with notes), not an error.

`context_window` moves from the global `agent.compaction.context_window_tokens`
to the per-model `context_window` (the model-specific value is now per model).
The compaction resolver reads the active model's `context_window`; if none is
set, the existing `agent.compaction.context_window_tokens` remains the fallback.
This is additive and does not break a config that only sets the global key.

**`import_profile_id_for_model` (new seam).** `src/llm/model_profile.cpp` gains:

```cpp
// Returns the model-profile id to pre-set for a wire model id, or "" when the
// model needs no profile. The ONLY place that knows which model ids map to
// which built-in profile (47-I6); callers never name a built-in profile.
[[nodiscard]] std::string_view import_profile_id_for_model(std::string_view model_id) noexcept;
```

For v1 it returns `"muse-glimmer"` iff `model_id` starts with `"muse-glimmer"`
(case-insensitive), and `""` otherwise. `default_import_profile_id()` is
retained for backward compatibility but is no longer used by the importer.
This is an amendment to 47 §4.2/§10.2.

### 4.3 Decision 52-D9: merge / overwrite policy

The trigger is **unchanged and create-only** (52-D9):
`maybe_import_localcode_config` (`src/cli/cli.cpp:944-1000`) still fires only
when the conventional global config **directory** is absent, in TUI mode, with
no explicit `--config`, interactively. Therefore **there is no merge with an
existing ymh config** — the import *creates* the global config. The workspace
layer, `YMH_*` env, and CLI then override ordinary keys; per-endpoint
`api_key` has no override path (52-D6). A re-import/merge command for an
existing config is **not** introduced here; it is open question 52-OQ-3.

### 4.4 What is dropped, and why

| localcode | Disposition | Why |
|---|---|---|
| `profiles[*].provider`/`model`/`max_tokens`/`context_window` | imported as a **model** | the user's "do not follow profiles": ymh's profile-shaped concept is the dsh agent preset, not a model config |
| `providers[*].type == "bedrock"` | dropped with note | no ymh adapter; 52-OQ-9 |
| non-http(s) `base_url` | endpoint dropped with note | the adapter requires an http(s) endpoint |
| `auto_delegate`, `keep_going`, `model_invocable`, `orchestrate`, `smart_agent` | ignored | unchanged from 46-D12.7 |
| localcode `permission` flat array | not mapped (note) | not the real shape (46-D12.6) |
| `providers[*].region`/`profile` (bedrock) | dropped | only meaningful to a Bedrock adapter |

### 4.5 Mapping table (full, pinned)

| localcode source | ymh target | Rule |
|---|---|---|
| `providers.<n>.type` | `llm.endpoints.<n>.provider` | `openai-compat`/`openai-compatible` → `openai-compatible`; `bedrock`/other, or missing/empty → skipped with a note |
| `providers.<n>.base_url` | `llm.endpoints.<n>.base_url` | non-empty http(s) only; otherwise skipped with a note |
| `providers.<n>.api_key` | `llm.endpoints.<n>.api_key` | non-empty literal; global layer; `0600` |
| `providers.<n>` with no key and no resolvable `api_key_env` | — | keyless endpoint; imported with a note (52-F23) |
| `profiles.<p>.provider` | `llm.models.<p>.endpoint` | dangling/missing/empty → skip model with note |
| `profiles.<p>.model` | `llm.models.<p>.model` | required non-empty; missing/empty → skip entry with note |
| `profiles.<p>.max_tokens` | `llm.models.<p>.max_tokens` | `> 0` only |
| `profiles.<p>.context_window` | `llm.models.<p>.context_window` | retained |
| model id → profile | `llm.models.<p>.profile` | `import_profile_id_for_model` |
| `default_profile` | `llm.active_model` | only if that profile imported |
| `skip_permissions == true` | `permissions.default = "allow"` | unchanged (46-D12.5) |
| `permission.<tool>[].match/.decision` | `permissions.rules[]` | unchanged (46-D12.6) |
| `mcp_servers` | `mcp_servers` | unchanged (25-D13); a malformed entry is skipped with a note, not fatal (52-F20) |
| `auto_compact_enabled`/`auto_compact_percent` | `agent.compaction.enabled`/`threshold_ratio` | unchanged |
| `max_concurrent_tasks` | `llm.default.max_concurrency` | unchanged |
| `auto_delegate`, `keep_going`, `model_invocable`, `orchestrate`, `smart_agent` | — | ignored |

Every "skipped with a note" row above pushes exactly one note in the mapping pass
(§4.1); §4.1 and §4.5 are kept qualifier-for-qualifier aligned in Rev 2.

---

## 5. Part B — dsh-aligned agent presets

### 5.1 Decision 52-D10: reconciliation with spec 42

Spec 42 owns the mechanism; 52 owns the **content**. Specifically:

- 52 **reuses** `AgentPresetRoster`, `PresetRow`, `preset.jsonc`, the scope
  chain, `presets.*`, `max_depth`, and `apply_child_composition`. It changes
  none of 42's invariants.
- 52 adds **row fields**: `persona` (typed as the existing
  `std::optional<PersonaConfig> persona` — see §5.8; **not** a new
  `PersonaRow`), `permission_preset`, and `model` (optional; see 5.7). The legacy
  flat `persona_prefix`/`persona_suffix` fields are retained. Setting `persona`
  **and** either legacy field in the same row is a load error (52-F15): one JSON
  `persona` object has one source of truth.
- 52 adds the **shipped-presets root** (`default_presets_shipped_root()`
  currently returns `{}`, `src/agent/preset.cpp:307-309`) and ships two
  presets there.
- 52 adds `permissions.presets`, `permissions.default_preset`, and
  `agent.sandbox` (new keys, §3 amendment to 21/09).
- 52 explicitly **does not** add a `profiles.*` config section. `presets.*` is
  the only preset-root config, so the request's collision concern does not
  arise.

**Prompt parity is partial (stated plainly; see §1.4, 52-OQ-13).** Part B's
verdict: **agents, tools, and permissions are genuinely delivered**; **prompts
are the weakest leg**. Only the **persona** is truly carried. Plan mode exists
in ymh (spec 25-D3) but is **not preset-controlled**; compaction prompts and tool
descriptions are host-global; per-preset instruction `maxBytes`/candidates are
**absent** (the `standard` `instructions` row is a no-op marker, §5.5). This spec
does **not** claim full dsh prompt parity.

### 5.2 Decision 52-D11: the roster

Mirror dsh's four preset ids, adapted to ymh's actual tool set:

| dsh preset | ymh id | Disposition |
|---|---|---|
| `minimal` | `minimal` | **shipped**: single-tool composition (one-shot `shell`; dsh's *persistent* shell maps to ymh's PTY `terminal`, spec 14), persona `complete`, runtime context suppressed |
| `standard` | `standard` | **shipped**: all ymh tools, dsh's standard persona |
| `ptc` | `ptc` | **reserved, unmountable**: ymh has no PTC-mode TypeScript SDK (`26-D22`, `26 §5.1`). Mounting fails loud (`PresetUnavailable`). |
| `cordis` | `cordis` | **reserved, unmountable**: ymh has no plugin system (`00 §55` omission). Mounting fails loud. |

**Compaction and instructions are not preset-controlled in v1 (corrected in Rev
2).** Rev 1's table claimed `minimal` has "compaction absent" and `standard` has
"instructions enabled, compaction on". Neither is representable: no
`PresetRow`/persona field can enable them, `CompactionSettings::enabled` defaults
false and is the global `agent.compaction` (`include/ymh/config/config.hpp:54-65`),
`prompt.instructions_enabled` defaults false (`config.hpp:207`), and §5.5 itself
calls the `standard` `instructions` row "a no-op marker". The roster therefore
differs only in **tools**, **persona**, and **permissions** in v1. Making
compaction/instructions preset-scoped is 52-OQ-13 (out of scope for Rev 2).

The ids and their display names are pinned in §5.3. Whether to mirror the names
verbatim is open question 52-OQ-1 (marked resolved in Rev 2: keep them, the user
asked to follow dsh closely). `ptc`/`cordis` being *reserved* rather than
*absent* is deliberate: a user typing `--preset ptc` gets a clear "not shipped"
error, not a silent empty composition (52-F11). Reserved ids are **not** returned
by `list()` (which returns discovered presets only); they are reported by
`reserved_ids()` (§5.8, M-P1).

### 5.3 Decision 52-D12: the shipped presets

Shipped under `<shipped-root>/<id>/preset.jsonc` (§5.4):

`minimal/preset.jsonc`:

```jsonc
{
  "id": "minimal",
  "display_name": "Minimal",
  "rows": [
    {
      "id": "persona",
      "persona": {
        "prefix": "You are a helpful software engineer assistant.",
        "complete": true,
        "include_runtime_context": false
      }
    },
    { "id": "shell", "tools": { "allow": ["shell"] } }
  ]
}
```

`standard/preset.jsonc`:

```jsonc
{
  "id": "standard",
  "display_name": "Standard",
  "rows": [
    {
      "id": "persona",
      "persona": {
        "prefix": "You are a coding agent powered by the {{model}} model.",
        "suffix": "Your working directory is {{cwd}}."
      }
    },
    { "id": "instructions" }
  ]
}
```

Adaptations from dsh, each deliberate:

- **minimal's single tool.** dsh minimal allows only a *persistent* shell
  (`@deepseek-ai/dsh-tool-bash-persistent`). ymh's persistent shell is the PTY
  `terminal` tool (spec 14), which is conditional on
  `environment_->pty().available()` (`src/agent/workspace_runtime.cpp:161-163`).
  The shipped `minimal` therefore allows the **one-shot `shell`** (always
  available) so the preset is total. A user who wants `terminal` writes their own
  preset under a **different id** (duplicate shipped ids are a load failure,
  §5.4); the shipped root is not overridable in place. (Rev 1's
  "`include_shipped_root`-local override" was incoherent — `include_shipped_root`
  is a `bool` and duplicate ids are a load failure — and is dropped.)
- **minimal's persona.** dsh's prefix is `"You are a helpful software engineer
  assistant."` with `complete: true` and `includeRuntimeContext: false`
  (`minimal/agent.cordis.yml:9-14`). ymh pins the same three values. This is
  the reason for the `PersonaConfig` `complete` / `include_runtime_context` row
  extension (§5.5).
- **standard's persona.** ymh's shipped default persona is already byte-identical
  (`src/prompt/persona.cpp:13-16`); the preset row restates it so the preset is
  self-describing and survives a change to `default_persona_config()`.
- **`ptc`/`cordis`.** Reserved ids with no `preset.jsonc`; `resolve()` throws
  `PresetUnavailable` (a subclass of `PresetError`, §5.8). They are **not**
  returned by `list()`; `reserved_ids()` reports them so the user sees why they
  cannot be mounted (M-P1).

### 5.4 Decision 52-D13: the shipped-presets root

`default_presets_shipped_root()` (`src/agent/preset.cpp:307-309`) returns `{}`
today, so `roots()` skips it (`:320-336`). 52 pins a real path:

```cpp
// Compile-time install location, e.g. "<prefix>/share/ymh/presets", injected by
// CMake as YMH_PRESETS_DIR. Empty when the build does not install presets, in
// which case the shipped root contributes nothing (and the roster load emits one
// warning; see 52-F16).
[[nodiscard]] std::filesystem::path default_presets_shipped_root();
```

- The CMake install step places `presets/{minimal,standard}/preset.jsonc` under
  `${CMAKE_INSTALL_DATADIR}/ymh/presets`.
- A dev build falls back to `<source>/presets` when `YMH_PRESETS_DIR` is unset
  and the source directory exists; otherwise empty.
- `presets.include_shipped_root` (default `true`) gates it, unchanged from 42.
- Discovery order is shipped → user (`~/.config/ymh/presets`) → `presets.root`,
  and a duplicate id across roots is a load failure (42-F8). A user root may
  therefore override a shipped preset only by giving it a different id;
  overriding a shipped id is a duplicate-id failure (52-OQ-10, marked resolved in
  Rev 2: keep the duplicate-id failure).
- **`list()` / `reserved_ids()` contract (M-P1).** `list()` returns only
  **discovered** presets (`src/agent/preset.cpp:380-383`) and never reserved ids;
  a new `reserved_ids()` accessor returns the compiled-in `{"ptc","cordis"}`.
  `resolve(id)` throws `PresetUnavailable` when `id` is reserved and not
  discovered, and `PresetNotFound` otherwise. No `AgentPreset` availability flag
  is added; `PresetUnavailable` is retained (L-P4).

### 5.5 Decision 52-D14: persona, instruction, and runtime-context binding

- **Persona.** The preset's `persona` row binds through the existing scoped
  `deployment:persona-prefix`/`-suffix` sections (spec 36, `register_persona`).
  `register_preset_rows` is extended to honor `complete` (it currently does not,
  §2.5) and `include_runtime_context`.
- **Runtime context.** `PersonaConfig::include_runtime_context` already exists
  and is honored **globally** at `src/agent/workspace_runtime.cpp:189-197` (the
  snapshot is registered only when the flag is true). The preset-level flag is
  the **same field**, so precedence is pinned explicitly: for a preset session
  the effective value is the preset row's `include_runtime_context`; the global
  registration (`:189-197`, always from `default_persona_config()`) applies
  **only when no preset is mounted**. Precedence: preset > global default. The
  context **registers at scope mount time** (when the preset's standing scope is
  created, before the first message), not lazily at assembly. A per-preset
  suppression needs a scoped registration: the runtime context becomes a
  **scope-level context** registered under the preset's standing scope. Interface:

  ```cpp
  // ScopeHandle already exposes add_context (include/ymh/prompt/system_prompt.hpp:145).
  // 52 pins that the default prompt registers runtime context through the
  // roster's standing scope when a preset is active, honoring
  // PersonaConfig::include_runtime_context; the global path
  // (workspace_runtime.cpp:189-197) is used only when no preset is mounted.
  ```
- **Instructions.** dsh's `agent-instructions: { maxBytes: 65536 }` maps onto
  the existing `prompt.instructions.*` (`include/ymh/prompt/instructions.hpp`,
  §2.6). In v1 a preset does **not** carry its own instruction config; the
  global/workspace setting applies. A preset row field for per-preset
  `maxBytes`/candidates is open question 52-OQ-4. The shipped `standard` preset
  declares an `instructions` row as a no-op marker; mounting it does not change
  the loader config.

### 5.6 Decision 52-D15: tool composition and permission binding

**Tools.** Unchanged from 42-D10: a `ToolRestriction` over the host registry
(`PresetRow.tool_filter`), deny wins, unknown names match nothing. A preset can
only narrow (42-I7). The shipped `standard` has no filter (all tools); the
shipped `minimal` allows only `shell`.

**Permissions.** New config (amendment to 09/21):

```jsonc
"permissions": {
  "presets": {
    "workspace-write":    { "sandbox": "workspace",    "approval": "ask"   },
    "danger-full-access": { "sandbox": "unrestricted", "approval": "never" }
  },
  "default_preset": "workspace-write"
}
```

- `sandbox`: `"workspace"` → `SandboxMode::Workspace`; `"read-only"` →
  `SandboxMode::ReadOnly`; `"unrestricted"` → `SandboxMode::Unrestricted`
  (`include/ymh/execution/environment.hpp:22-26`).
- `approval`: `"ask"` → `permissions.default` stays `"ask"`; `"never"` →
  `permissions.default = "allow"` (the 46-D1 master switch; hard denies and
  `force_ask` are never bypassed).
- A preset row may bind a permission preset:
  `{ "id": "permissions", "permission_preset": "workspace-write" }`. The
  binding is stamped as `PolicyRule::Layer::Profile` (the enum value exists,
  `include/ymh/policy/permission_policy.hpp:86-95`; no producer exists today).
- Resolution at session creation: the session's preset's `permission_preset` >
  `permissions.default_preset` > built-in `workspace-write`. The resolved
  preset is **pinned** at creation and never recomputed on resume (52-I12;
  dsh `dsh-permission-presets` README, "Session defaults").
- `agent.sandbox` is added as a config key (`"workspace"|"read-only"|
  "unrestricted"`) replacing the hardcode at `src/cli/wiring.cpp:203`, so a
  deployment without permission presets can still set the sandbox.
- The `/permission` command surface (dsh's UI) is **out of scope** (spec 10/25).

### 5.7 Decision 52-D16: agent capabilities in/out

| Capability | Disposition | Reason |
|---|---|---|
| Subagents (spawn/fork) | **in** — already spec 42 | `AgentPresetRoster::apply_child_composition`, `presets.max_depth` |
| Skills | **in** — already spec 42/20 | `PresetRow.skill_roots`, `SkillCatalog` |
| Plan mode | **in (already ymh, spec 25-D3), but not preset-controlled in v1** | `agent.plan` is host-global; dsh's per-preset plan prompt has no ymh seam (52-OQ-13) |
| Compaction | **in (already ymh, spec 13), but not preset-controlled in v1** | `agent.compaction` is host-global (`config.hpp:54-65`); a preset cannot disable it (52-OQ-13) |
| Goals / jobs / commands | **in as of spec 44**, but **not selected by a preset** in v1 | spec 44 owns them; a preset does not gate them (52-OQ-5) |
| Workflows (`tool-workflow`) | **out** | `26 §5.1`; no ymh workflow engine |
| `ralph` | **out** | `26 §5.1` |
| PTC `run_code` SDK | **out** | `26-D22`, `26 §5.1` |
| Remote / ACP / Claude-Code / Codex subagent providers | **out** | `26 §5.1` |
| Plugin system / Cordis config / hot reload | **out** | `00 §55` |
| Image / attachment pipeline | **out** | `26 §5.1` |
| Per-preset model selection | **out in v1** (52-OQ-6) | dsh's `modelSelectionSettings` has no ymh seam yet |

A preset that names an out-of-scope capability is a **load error**, never a
silent no-op (52-F13).

### 5.8 C++ interface sketches (Part B, pinned)

```cpp
// include/ymh/agent/preset.hpp — additive to spec 42.
namespace ymh {

// PersonaConfig is REUSED verbatim (include/ymh/prompt/persona.hpp:16-20);
// PersonaRow is DELETED (P-H1): it was a field-for-field clone and the loader
// already parses the JSON `persona` object. Include "ymh/prompt/persona.hpp".
struct PresetRow {
    std::string                    id;
    std::string                    group;
    bool                           disabled = false;
    std::optional<PersonaConfig>   persona;            // NEW; supersedes the pair below
    std::optional<std::string>     persona_prefix;     // legacy (42), still honored
    std::optional<std::string>     persona_suffix;     // legacy (42), still honored
    std::optional<ToolRestriction> tool_filter;
    std::vector<std::string>       skill_roots;
    std::vector<PromptSectionSpec> sections;
    std::optional<std::string>     permission_preset;  // NEW; names permissions.presets
    std::optional<std::string>     model;              // NEW; reserved (52-OQ-6)
    std::optional<std::string>     config;
};
// Load rule (52-F15): `persona` set together with `persona_prefix` or
// `persona_suffix` in the same row is a load error.

// 52-D11: a known-but-not-shipped preset id (ptc, cordis).
class PresetUnavailable : public PresetError {
public:
    using PresetError::PresetError;
};

// 52-D13.
[[nodiscard]] std::filesystem::path default_presets_shipped_root();

// 52-D11/M-P1: reserved ids are not discovered, so they are not in list().
[[nodiscard]] std::vector<std::string> reserved_ids();   // {"ptc", "cordis"}

// AgentPresetRoster contract (M-P1, L-P4):
//   list()          -> only DISCOVERED presets (src/agent/preset.cpp:380-383);
//                      never reserved ids; no availability flag is added.
//   reserved_ids()  -> the compiled-in reserved set.
//   resolve(id)     -> PresetUnavailable if id is reserved and undiscovered;
//                      PresetNotFound otherwise.

} // namespace ymh
```

```cpp
// include/ymh/config/config.hpp — Part A + Part B config additions.
namespace ymh {

struct EndpointSettings {                 // 52-D1
    std::string  provider = "openai-compatible";
    std::string  base_url;
    std::string  api_key_env;
    std::optional<std::string> api_key;   // global layer only; 0600
    std::vector<std::pair<std::string, std::string>> headers;  // global layer only; values redacted
    std::size_t                max_concurrency = 4;
    std::chrono::milliseconds  connect_timeout{10'000};
    std::chrono::milliseconds  idle_timeout{60'000};
    std::chrono::milliseconds  request_timeout{120'000};
    RetrySettings              retry;
};

struct ModelSettings {                    // 52-D1
    std::string                  endpoint;   // required; names an endpoint
    std::string                  model;      // required; wire id
    std::string                  profile;    // model-profile id; "" = inert
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::uint32_t> context_window;
    std::optional<std::string>   reasoning_effort;
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;
    std::optional<std::string>   tool_choice;
    std::vector<std::string>     stop;
    std::optional<std::uint32_t> seed;
};

struct LlmSettings {
    // … existing flat fields (default endpoint + literal model) unchanged …
    std::map<std::string, EndpointSettings, std::less<>> endpoints;   // NEW
    std::map<std::string, ModelSettings,    std::less<>> models;      // NEW
    std::optional<std::string> active_model;                          // NEW
};

struct PermissionPresetSettings {         // 52-D15
    std::string sandbox  = "workspace";   // "workspace"|"read-only"|"unrestricted"
    std::string approval = "ask";         // "ask"|"never"
};

struct PermissionDefaults {
    // … existing fields …
    std::map<std::string, PermissionPresetSettings, std::less<>> presets;  // NEW
    std::string default_preset;                                            // NEW
};

struct AgentDefaults {
    // … existing fields …
    std::string sandbox = "workspace";    // NEW; replaces the wiring hardcode
};

[[nodiscard]] ResolvedModel resolve_model(const Config& config);   // 52-D3

} // namespace ymh
```

### 5.9 Invariants (Part B)

- **52-I8 (roster totality).** Every id dsh ships (`minimal`, `standard`, `ptc`,
  `cordis`) is either mountable or reserved-and-fails-loud; none silently yields
  an empty composition. Guard: `PresetUnavailable` (52-F11).
- **52-I9 (persona fidelity).** The shipped `standard` persona equals
  `default_persona_config()` and dsh's `standard` strings; the shipped `minimal`
  persona is `complete`, has no suffix, and suppresses the runtime context.
- **52-I10 (scoped suppression).** `PersonaConfig::include_runtime_context ==
  false` suppresses the runtime-context snapshot only for the preset's sessions,
  never globally; the preset value takes precedence over the global default
  (§5.5).
- **52-I11 (narrowing only).** A preset's tool filter and permission preset can
  only narrow, never widen, the deployment's tools and permissions (42-I7).
  Guard: `Preset52_NoWiden` (§9.1).
- **52-I12 (permission pinning).** A session's permission preset is resolved at
  creation and never recomputed on resume/fork; a later config change does not
  alter an existing session.
- **52-I13 (no phantom capabilities).** A preset that references a capability
  ymh does not ship (workflow, ralph, PTC, plugin) fails load; it is never a
  no-op.
- **52-I14 (single preset-root config).** `presets.*` is the only preset-root
  config; no `profiles.*` section exists.

### 5.10 Failure modes (Part B)

- **52-F11: reserved preset mounted silently.** `ptc`/`cordis` resolve to an
  empty composition, so the model runs with no tools and no persona. **Guard:**
  52-I8; `resolve` throws `PresetUnavailable`. Instance of F1.
- **52-F12: runtime context leaks.** A `minimal` session still receives the
  global runtime-context snapshot. **Guard:** 52-I10 scoped registration.
  Instance of F9 (cross-session leakage).
- **52-F13: phantom capability.** A preset names `workflow`/`ptc`/`plugin` and
  the row is ignored, so the user believes it applied. **Guard:** 52-I13 load
  error. Instance of F1.
- **52-F14: permission widened on resume.** A resumed session recomputes its
  permission preset from the current config and acquires a wider sandbox.
  **Guard:** 52-I12 creation-time pinning. Instance of F2.
- **52-F15: persona defined twice.** Two rows define
  `deployment:persona-prefix` in the same scope (the registry throws, `36 §2.1`),
  **or** one row sets `persona` together with the legacy
  `persona_prefix`/`persona_suffix` (two sources of truth for one JSON object).
  **Guard:** 42's existing duplicate-name check; one `persona` row per preset in
  the shipped files; and the `persona` + legacy-pair combination is a load error
  (L-P3). Instance of F2.
- **52-F16: shipped root missing.** The install does not place the shipped
  presets, so `standard` is unknown and the default is absent. **Behaviour:**
  the roster load emits **one** warning when `include_shipped_root` is true and
  the root is empty (the Rev-1 52-F18 tag is merged here, M-C2);
  `resolve(nullopt)` with no default still fails loud (42-F1). Instance of F1.
- **52-F17: permission preset unknown.** A row names a `permissions.presets`
  entry that does not exist, so the session silently keeps the default. **Guard:**
  load-time check; unknown name is a `ConfigError`. Instance of F1.

---

## 6. Invariants (consolidated)

Part A: 52-I1..52-I7 (config schema + model resolution; extends `21`'s loader
invariants and the `26`/`28` LLM boundary). Part B: 52-I8..52-I14 (agent
presets; extends `42-I1..42-I12`, with `26-I1..26-I12` where the shared prompt /
delegation seam applies). The two parts have **separate lineages** and are not
both "all extend 42 and 26". Every `52-F#` names the shared finding it
instantiates using the taxonomy **asserted by `42 §7.1` / `44 §8.1`** — not
`00 §54`'s literal F1–F12 text, whose labels differ (§3.8).

## 7. Failure modes (consolidated)

`52-F1..52-F24`, contiguous (Part A: F1–F10, F18–F24; Part B: F11–F17). The
Rev-1 F18 (shipped-root warning) is merged into F16 as a behaviour (M-C2).
Coverage matrix in §9.5.

## 8. dsh (DeepSeek Harness) mapping

| ymh (52) | dsh source | Notes |
|---|---|---|
| `llm.endpoints.<n>` | (none) | dsh's model route is host-owned; endpoints are a ymh-native extension. |
| `llm.models.<n>` + `active_model` | (none) | dsh has no named-model config; the model route is a host plugin. |
| `presets.<id>/preset.jsonc` | `presets/<id>/agent.cordis.yml` | JSONC, not Cordis (`42-D1`, `26 §6` OQ8). |
| `minimal` | `dsh-agent-presets/presets/minimal` | persona `complete`/no runtime context; dsh's persistent shell → ymh one-shot `shell` (the persistent one is the PTY `terminal`, spec 14) |
| `standard` | `presets/standard` | all tools; dsh persona; `agent-instructions` maxBytes 65536 (deployment-global, not preset-controlled in v1) |
| `ptc` (reserved) | `presets/ptc` | no PTC TypeScript SDK (`26-D22`). |
| `cordis` (reserved) | `presets/cordis` | no plugin system (`00 §55`). |
| `persona` row | `@deepseek-ai/dsh-persona` `config` | `prefix`/`suffix`/`complete`/`includeRuntimeContext` map 1:1. |
| `prompt.instructions.*` | `@deepseek-ai/dsh-agent-instructions` `maxBytes` | already implemented (`instructions.hpp`). |
| `permissions.presets` | `dsh-permission-presets` `presets` | `sandbox` + `approval` bundle; `workspace-write`/`danger-full-access` shipped names. |
| `permissions.default_preset` | `defaultPreset` | pinned at session creation; not retroactive. |
| `permission_preset` row | preset + permission bundle | dsh selects the permission preset per session; ymh binds it per preset row. |
| `presets.max_depth` | subagent `maxDepth` | already 42-D18. |
| `apply_child_composition` | `applyChildComposition` | already 42-D7. |
| (no `isolate`/`inject`) | `isolate`/`inject` rows | no ymh analogue (`42 §8`). |

## 9. Test plan

### 9.1 Unit (hermetic, no LLM, no daemon)

- `Config52_EndpointsParse`: a document with two endpoints and three models
  parses; `resolve_model` returns the right endpoint + wire id.
- `Config52_ModelUnknownEndpoint`: a dangling `endpoint` → `ConfigError` (52-F2).
- `Config52_ActiveModelUnknown`: `active_model` names nothing → `ConfigError`
  (52-F1).
- `Config52_NameGrammar`: bad names (empty, space, slash, >64 bytes) →
  `ConfigError` (52-F5).
- `Config52_UnknownKeyInsideEntry`: an unknown key inside an endpoint/model
  entry → `ConfigError` with the qualified name (52-I6).
- `Config52_BackwardCompat`: the pre-52 config fixture resolves byte-identically
  to today's `to_provider_config`/`effective_model` (52-I5).
- `Config52_LiteralKeyWorkspaceLayer`: a workspace-layer `api_key` →
  `ConfigError`; a global-layer `api_key` in a `0644` file → `ConfigError`
  (52-I4).
- `Config52_AgentModelDual`: `agent.model = "balanced"` selects the model entry;
  `agent.model = "some-wire-id"` uses the default endpoint (52-D3, 52-F6).
- `Import52_AllProvidersAllProfiles`: the real localcode fixture imports both
  providers and all five profiles as endpoints/models; `default_profile` sets
  `active_model`.
- `Import52_BedrockSkippedWithNote`: a `bedrock` provider is skipped and a note
  is emitted (52-F7).
- `Import52_MuseProfileOnlyForMuse`: `Muse-Glimmer-30B` gets
  `profile = "muse-glimmer"`; `gemma-4-31B-it` and `nvidia/nemotron-3-super` get
  none (52-I7, 52-F8).
- `Preset52_Roster` / `Preset52_ReservedIds`: `list()` returns only
  `minimal`/`standard`; `reserved_ids()` returns `ptc`/`cordis`;
  `resolve("ptc")` throws `PresetUnavailable` (52-I8, 52-F11, M-P1).
- `Preset52_MinimalPersonaComplete`: mounting `minimal` yields a complete
  persona and no runtime context (52-I9, 52-I10).
- `Preset52_StandardPersonaEqualsDefault`: the shipped `standard` persona equals
  `default_persona_config()` (52-I9).
- `Preset52_PermissionPinning`: a session's permission preset is fixed at
  creation; a config change does not alter it (52-I12, 52-F14).
- `Preset52_PhantomCapabilityFails`: a row naming `workflow` → load error
  (52-I13, 52-F13).
- `Preset52_NoWiden`: a preset whose tool filter/permission preset would add a
  tool or widen the sandbox is refused (52-I11; M-C1).
- `Preset52_ProfilesSectionRejected`: a `profiles.*` section → `ConfigError`
  (52-I14; M-C1).
- `Preset52_ShippedRootMissing`: no `YMH_PRESETS_DIR` → one warning and
  `standard` absent (52-F16).
- `Preset52_UnknownPermissionPreset`: a row naming a missing
  `permissions.presets` entry → `ConfigError` (52-F17).
- `Preset52_PersonaDuplicateRejected`: two rows defining
  `deployment:persona-prefix` in one scope → load error (52-F15).
- `Preset52_PersonaAndLegacyPairRejected`: one row with both `persona` and
  `persona_prefix` → load error (52-F15; L-P3).
- `Config52_EntryShadowWholesale`: a workspace endpoint entry with the same name
  as a global entry replaces it **wholesale** (absent fields revert to built-in
  defaults), and a workspace entry shadowing a global entry that carried a
  literal `api_key` → `ConfigError` (52-F3, 52-F18).
- `Config52_HeadersGlobalOnly`: a workspace-layer `endpoints.<n>.headers` →
  `ConfigError`; a `0644` global file with headers → `ConfigError`; header
  values are redacted in output (52-F4, 52-F22).
- `Config52_MissingRequiredModelField`: `models.<n>` missing/empty `endpoint` or
  `model` → `ConfigError`; `endpoints.<n> = {}` parses to built-in defaults
  (52-F19).
- `Config52_TypeCheckEndpointsModels`: non-object `llm.endpoints`/`llm.models`,
  non-string `llm.active_model`, non-object `headers`, non-object `retry` →
  `ConfigError` with the existing message form (M-S2).
- `Config52_SecretNeverLogged`: a resolved `ResolvedModel` never carries the
  literal key into a request header, log, or event, on success **and** on the
  import failure paths (52-I4, 52-F10, L-I1).
- `Import52_ReimportClobber`: a re-run over an existing global config is a no-op
  (create-only trigger retained) (52-F9).
- `Import52_MalformedEntrySkippedWithNote`: one malformed `mcp_servers.<n>`
  entry is skipped with a note and the remaining endpoints/models still land
  (52-F20).
- `Import52_WriteAtomicity`: a simulated failure mid-write leaves no partial
  global `config.jsonc` (only the temp file) (52-F21).
- `Import52_KeylessEndpointNote`: an `openai-compatible` provider with no key
  is imported with a note, not rejected (52-F23).

### 9.2 Golden

- A golden `config.jsonc` round-trip: the import output for the real localcode
  document is byte-stable (the importer is pure; `build_localcode_import`).
- A golden `preset.jsonc` for each shipped preset (parse → `AgentPreset`).

### 9.3 Integration (FakeLLM)

- `UI52_ResolveModelEndToEnd`: a two-endpoint config drives `FakeLLM` through
  the selected model; the request body carries the right `model` and
  `max_tokens`.
- `UI52_ImportEndToEnd`: first-run TUI import of the real localcode shape writes
  a `0600` global config that `apply_jsonc_file(..., required=true)` accepts,
  with endpoints, models, and `active_model` (extends `UI46_I5_ImportEndToEnd`).
- `UI52_MinimalPresetRun`: a `minimal` session sees only `shell` and no runtime
  context; a `standard` session sees all tools.

### 9.4 Live (opt-in, `YMH_LIVE_LLM=1`)

- A two-endpoint live run against **repository fixture endpoints** (e.g. the
  checked-in `tests/fixtures/` names; the endpoint names are configurable, not
  hardcoded to the author's `itg`/`ted-ai`), skipped without keys, asserting the
  selected model reaches the right `base_url` (L-C3). Existing live tests stay
  opt-in.

### 9.5 Failure-mode coverage

Every `52-F#` (F1–F23) has at least one named test above; the Rev-1 absolute
claim was **false** (no test existed for F3, F9, F10, F15, F16, F17, F18). Rev 2
adds `Config52_EntryShadowWholesale` (F3/F18), `Import52_ReimportClobber` (F9),
`Config52_SecretNeverLogged` (F10), `Preset52_PersonaDuplicateRejected` (F15),
`Preset52_ShippedRootMissing` (F16), `Preset52_UnknownPermissionPreset` (F17),
plus the F19–F23 tests and the `Preset52_NoWiden`/`Preset52_ProfilesSectionRejected`
invariant tests. The remaining matrix is completed when the spec is implemented.

## 10. dsh re-sync process (requested)

The user will periodically ask to re-sync dsh updates. This spec **requests** a
documented process but does **not** create it (52-OQ-2): the recommended
artifact is a short `docs/design/dsh-sync.md` checklist that diffs
`~/.dsh/profiles/node_modules/@deepseek-ai/dsh-agent-presets/presets/`,
`dsh-permission-presets`, and `dsh-agent-instructions` against this spec's §5,
§8 and §4.4, and opens a spec-52 errata for each drift. Until that artifact
exists, the re-sync is a manual review.

## 11. Open questions / interpretations

1. **52-OQ-1 — mirror dsh preset names verbatim? — RESOLVED (Rev 2).** Keep
   `minimal`/`standard`/`ptc`/`cordis` verbatim; `ptc`/`cordis` reserved. The
   user asked to follow dsh closely.
2. **52-OQ-2 — is the periodic dsh re-sync a documented process?** Recommendation:
   add `docs/design/dsh-sync.md`; not created by this spec.
3. **52-OQ-3 — re-import / merge into an existing ymh config.** The trigger is
   create-only; a merge command (e.g. `ymh config import-localcode`) is not
   introduced. Product decision needed.
4. **52-OQ-4 — per-preset instruction config.** dsh's preset carries
   `agent-instructions.maxBytes`; v1 uses the global `prompt.instructions.*`.
   Whether a preset row may override it is open.
5. **52-OQ-5 — do presets gate goals/jobs?** Spec 44 ships goals/jobs globally;
   a preset cannot currently select them. Whether a preset should is open.
6. **52-OQ-6 — per-preset model selection.** dsh's subagent
   `modelSelectionSettings` has no ymh seam; `PresetRow.model` is reserved.
7. **52-OQ-7 — shipped-root install path.** Pinned as
   `${CMAKE_INSTALL_DATADIR}/ymh/presets`; packaging (deb/rpm/brew) is open.
8. **52-OQ-8 — retain the `llm.default` flat-key quirk? — RESOLVED (Rev 2).**
   Retain it (`21 §7.7` J12); fixing it is a separate semantic change. §3.3 now
   reads `config.llm.model` verbatim and no longer depends on the distinction.
9. **52-OQ-9 — add a Bedrock adapter?** localcode's `bedrock` provider has no
   ymh equivalent; the spec skips it with a note. Adding the adapter is a
   separate component spec.
10. **52-OQ-10 — user override of a shipped preset id. — RESOLVED (Rev 2).**
    A duplicate id across roots fails load (42-F8); a user cannot shadow a
    shipped preset with the same id. Keep the duplicate-id failure; a user root
    overrides only by using a different id.
11. **52-OQ-11 — `context_window` placement.** Moved from
    `agent.compaction.context_window_tokens` to `llm.models.<n>.context_window`
    with the global key as fallback. Whether to deprecate the global key is open.
12. **52-OQ-12 — CLI/env surface. — RESOLVED (Rev 2).** The §3.3 spellings are
    **final**: `--endpoint <name>`, `YMH_LLM_ACTIVE_MODEL`, `YMH_LLM_ENDPOINT`.
    They are config/resolution keys, not UI controls, so no spec 10/25 review is
    required; the UI pickers (`/model`, `/endpoint`) remain spec 10/25's and are
    out of scope (§1.4).
13. **52-OQ-13 — preset-scoped prompts (the weak leg).** v1 carries only the
    persona. Plan mode (spec 25-D3), compaction prompts (spec 13), tool
    descriptions, and per-preset instruction `maxBytes`/candidates are
    deployment-global and **not** preset-controlled. Whether to add
    `PresetRow.instructions` / `PresetRow.plan` / `PresetRow.compaction` and a
    scoped prompt registration is open; until then this spec does **not** claim
    dsh prompt parity (§1.4, §5.1, §5.2).

## 12. Revision log

- **Rev 1 (2026-09-22, draft).** Initial authoring. Part A pins named
  endpoints/models and the all-providers/all-profiles import; Part B pins the
  shipped dsh-aligned roster and its persona/instruction/permission binding.
  Records the spec-42 reconciliation, the `00 §55`/§38 narrowing, and 12 open
  questions. Not reviewed.
- **Rev 2 (2026-09-22, draft).** Applies the first independent adversarial
  review (five reviewers: scope, schema, import, Part B, completeness). Every
  finding is listed with its fix:

  **HIGH.**
  - **S-H1** — §3.3 step 3 now reads the single field `config.llm.model`
    verbatim; the default endpoint is described as per-document section
    selection (`21 §7.7`) applied layer-by-layer, not a "nested wins" merge; the
    provenance caveat (a new `Config` field would be required) is pinned.
  - **S-H2** — §3.4 pins colliding entries replace **wholesale** (absent fields
    revert to built-in defaults, not the shadowed layer) and adds 52-F18
    (shadowing a global literal credential is a load error).
  - **I-H1** — §4.1/§3.8 pin note collection **in the same iteration** as the
    mapping (`build_localcode_import` returns notes); the default-profile-only
    `print_localcode_notes`/`localcode_provider` helper is replaced; note count
    == skip count.
  - **C-H1** — §9.1 adds named tests for F3, F9, F10, F15, F16, F17 (and the
    new F18–F23); §9.5 drops the false absolute claim and lists the gaps it
    fixed.
  - **C-H2** — §11 OQ-12 is **resolved**: the §3.3 spellings are final; the
    selectors are config keys, not UI.
  - **P-H1** — `PersonaRow` is deleted; `PresetRow.persona` reuses
    `std::optional<PersonaConfig>` (`persona.hpp:16-20`); §5.1/§5.3/§5.8/§5.9
    updated.
  - **P-H2** — §5.2 no longer claims compaction/instructions are preset-
    controlled; stated as "not preset-controlled in v1" (52-OQ-13).

  **MEDIUM.**
  - **M-S1** — §3.5 pins `reject_unknown_named_map` recursion into `retry` with
    the retry allowlist and `headers` as an object of strings.
  - **M-S2** — §3.5 pins explicit `is_object()`/`is_string()` checks with clean
    `ConfigError`s.
  - **M-S3** — `ResolvedModel` gains `model_name`; F3's guard no longer cites
    `source` for the resolved name; §3.3 states the types are new.
  - **M-S4** — §3.1/§3.7 pin required non-empty `endpoint`/`model`, add 52-F19,
    and state an empty endpoint entry `{}` is valid.
  - **M-S5** — §3.6/§3.7 make `headers` global-layer-only, subject to `0600`
    and redaction; add 52-F22.
  - **M-I1** — §4.1/§4.5 pin keyless endpoints (note, not reject); add 52-F23.
  - **M-I2** — §4.1/§4.2 pin per-entry skip-with-note; add 52-F20.
  - **M-C1** — §9.1 adds `Preset52_NoWiden` (I11) and
    `Preset52_ProfilesSectionRejected` (I14).
  - **M-C2** — old F18 merged into F16 as a behaviour; §7 range updated.
  - **M-C3** — §3.8/§6 cite the 42/44 taxonomy, not `00 §54`'s literal labels.
  - **M-P1** — §5.4/§5.8 pin `list()` (discovered only) + `reserved_ids()` +
    `resolve` throwing `PresetUnavailable`.
  - **M-P2** — §5.3 drops the incoherent `include_shipped_root`-local override.
  - **M-P3** — §5.5 pins the `PersonaConfig::include_runtime_context` precedence
    (preset > global) and the scope-mount registration time.
  - **M-P4** — §5.7 adds PLAN MODE and COMPACTION rows (both in ymh, not
    preset-controlled).

  **LOW.**
  - **L-S1** — §3.1 pins entry names as an independent namespace.
  - **L-S2** — §3.3 pins empty-selector = unset and unknown-endpoint = error.
  - **L-S3** — §3.8 F6 emits a notice on name-vs-id ambiguity.
  - **L-I1** — §3.6/52-I4 require `redact_secrets` on the import failure-path
    prints.
  - **L-I2** — §4.1/§4.5 qualifiers aligned (non-empty, missing type, keyless).
  - **L-C1** — §6 splits the Part A / Part B lineages.
  - **L-C2** — OQ-1, OQ-8, OQ-10 marked resolved.
  - **L-C3** — §9.4 uses configurable repository fixtures, not the author's
    `itg`/`ted-ai`.
  - **L-C4** — 52-F21 (import write atomicity) and 52-F22 (`headers` secrets)
    added.
  - **L-P1** — minimal's tool relabeled one-shot `shell`; PTY `terminal` noted.
  - **L-P2** — §2.7/§5.3 persona.cpp `:13-16`; §5.4 `roots()` `:320-336`.
  - **L-P3** — `persona` + legacy pair in one row is a load error (52-F15).
  - **L-P4** — `PresetUnavailable` retained with `reserved_ids()`; `list()`
    exposes no reserved ids.
  - **L-SC1** — §2.4 cites each helper line individually.
  - **L-SC2** — §1.3 adds the spec-28 amendment row.
  - **L-SC3** — §1.3 adds spec-10 and spec-44 coordination rows.
  - **L-SC4** — §1.3/header reworded to "confirms §55 stands".
  - **L-SC5** — §2.1/§2.2 citations fixed (`config.cpp:297`; env in
    `load_config`; CLI `cli.cpp:128-146`); 46-D12 range ends `:2776`.

  **Honesty.** §1.4/§5.1/§5.2/§5.7 state plainly that prompts are the weakest
  leg (persona only) and record it as 52-OQ-13. Failure modes renumbered to
  F1–F23 (contiguous); Part A F1–F10 + F18–F23, Part B F11–F17. All
  cross-references (§3.7/§3.8/§5.9/§5.10/§6/§7/§9.5) updated. Not yet reviewed.

# 47 — Muse-Glimmer-30B Support: the Model-Profile Seam, Forced First Tool Call, the `<|eom|>` Stop Guard, Leaked-Call Detection, the Native-ATEM Detection Grammar, Argument Normalization, the Sampling/Tool-Choice Config Surface, `top_k`, Per-Provider Capabilities, `xhigh`, and the Reversibility Contract

```
Status: **draft (Rev 4)** — awaiting the delta re-gate. Rev 4 responds to the
        Oracle final pass (`/tmp/opencode/spec47-oracle-final.md`: **DO NOT
        APPROVE — 0 HIGH / 2 MEDIUM / 3 LOW**, rev-3 findings only; both Rev-3
        HIGHs confirmed fixed). Every final ID (47-O2-M1/M2, 47-O2-L1/L2/L3) is
        resolved in §23.6. The two MEDIUMs are pin/interface defects, not
        architecture: the tautological `wire_tools_present` conjunct and its
        test are removed (the 400 hazard is closed structurally by the
        unchanged body builder), and the leaked-call parser result shape is
        widened to a `LeakedParse` so every row of the §7.2.3 taxonomy is
        decidable. This spec amends the owning specs (08, 06/31, 21, 41); it
        introduces no new component and no new subsystem. It is the third
        provider errata after
        28-llm-service-boundary-errata.md and 41-retry-executor.md, and, like
        46, it is written so that every "current state" claim is reproducible
        from the shipped tree (HEAD `e74b4a242`).

        Rev 1 authored decisions 47-D1 … 47-D11 from the approved plan
        (`.sisyphus/plans/muse-glimmer-support.md`) and corrected one factual
        error in that plan's evidence table (finding A mislabels the `<|eom|>`
        token id) and the plan's D7 "current state" claim (`llm.default.
        reasoning_effort` and `llm.default.max_tokens` are already settable).
        Gate 1 independently confirmed the token ruling (`<|eom|>` = 200007,
        `<|eot|>` = 200008, and D3 keys on the token string) and confirmed the
        D7 correction; both are retained unchanged in Rev 4.

        Rev 2 was a corrective revision to Gate 1 (GATE FAIL — 3 HIGH /
        13 MEDIUM / 5 LOW): it restated the reversibility invariant, pinned the
        leaked-call posture, keyed the forced first call on the turn-local step,
        and re-verified every citation.

        Rev 3 is a **scope reduction plus two layer fixes**, not a new feature:

        * **47-O-H1 — normalization moved to the correct layer.** D6's inbound
          rule is pinned at the **assembler's finalize** (after all streamed
          fragments are concatenated, `src/llm/tool_call_assembler.cpp:78-109`),
          not per delta inside `handle_tool_call`
          (`src/llm/openai_adapter.cpp:462-505`). Parsing each delta corrupted
          normally fragment-streamed `arguments`; the whole-object delta is the
          only genuinely new inbound gap and is the only thing normalized at
          `handle_tool_call` (§9.2).
        * **47-O-H2 — the forced call is gated on the internal tool list.** Rev 3
          attempted a separate `wire_tools_present` conjunct; Rev 4 (47-O2-M1)
          removes it as a tautology: `build_chat_completions_body` takes no
          profile and emits `tools[]` iff `request.tools` is non-empty, and
          `stream()` rejects a tools-bearing request before body build when the
          capability is absent, so no reachable state separates the wire from
          the internal `!request.tools.empty()` guard. The 400 hazard is closed
          structurally, and 47-F22 is the single forward-looking note (47-F41
          merged; §5.2, §16).
        * **D5 descoped to detection-only (the Oracle's structural finding).**
          ymh renders tool *declarations* and parses tool *calls*, but the
          conversation history is rendered by the server's Jinja chat template
          and the model's native output is a multi-message channel stream — a
          component ymh does not own, so the conversion path is unverifiable.
          The `"muse-glimmer-atem"` profile, the ATEM declaration renderer, the
          history echo-back, and the ATEM→`ToolCall` converter are **removed**.
          D5's §8.2 grammar is retained as the **native-ATEM detection
          grammar**: native ATEM syntax in assistant content is detected and
          surfaced as a typed error, never silently converted, never executed
          (§8, §1.4). A server that translates to standard `tool_calls` — the
          realistic case — is fully supported. If the internal endpoint turns
          out to pass ATEM through untranslated, that is a **new spec**, not
          this one.
        * **47-O-M1…M5, L1…L3** resolved: the whole-content rule applies only
          where conversion executes (G-JSON), not where detection errors
          (G-ATEM); the decoder's real inputs are pinned; `buildRequest`'s
          missing turn-local parameter is pinned; D4/D5 tests move to the real
          adapter over a fake transport; the scaffold placeholder, `top_k: 0`,
          and the D4 taxonomy are pinned.

        Rev 4 is a **targeted delta**, not a new feature:

        * **47-O2-M1 — the wire-tools guard was a tautology.** Rev 3's
          `wire_tools_present` conjunct had no input: `build_chat_completions_
          body` takes no profile and emits `tools[]` iff `request.tools` is
          non-empty (`src/llm/openai_adapter.cpp:714-725`), no `ModelProfile`
          field can omit `tools[]` (§15), and `stream()` rejects a tools-bearing
          request before body build when the capability is absent (`:895-898`).
          The conjunct and `MuseD2_WireToolsGuard` are removed; 47-I3 states the
          hazard is structurally impossible, 47-F22 is the single
          forward-looking note (47-F41 merged), and 47-I16 keeps only
          `MuseBodyGolden_ProfileOn` (§5.2, §16, §17, §18).
        * **47-O2-M2 — the leaked-call result shape could not express the
          decision table.** `parse_leaked_json_call` now returns a `LeakedParse`
          carrying `calls` plus `complete_block_seen`/`malformed_block_seen`, so
          "one or more blocks", "complete-but-malformed" (rows 2/4 →
          `MalformedToolCall`), and "no block" (rows 6/7 → text) are all
          decidable; two-block and malformed-block tests are added (§7.2.3,
          §7.2.5, §15, §16 47-I5, §18.4).
        * **47-O2-L1/L2/L3** — F22/F41 deduplicated; the streamed-fragments test
          relabelled a regression pin (H1's fail-first tests are named); the
          three mis-layered "pure-detector" tests are re-pointed at `finalize` /
          the adapter (§17, §18.4, §18.5).

        DESIGN ONLY. No implementation code, patches, or diffs. Interface
        sketches in §15 are declarations, not definitions.
```

---

## 1. Purpose, scope, and supersession map

### 1.1 The eleven decisions (from the approved plan)

| D | Decision |
|---|---|
| D1 | **Model profile seam** — `llm.default.profile` selects a named `ModelProfile`; the sole gate for every Muse-specific branch; absent/empty = today's behaviour byte-for-byte. |
| D2 | **Forced first tool call** — under the profile, `tool_choice: "required"` on the first request of a turn (the turn-local step counter is 1), reverting to the configured value on every later step. An explicitly configured `tool_choice` (including `"auto"`) is the operator's opt-out. |
| D3 | **Stop-token guard** — reject the message-framing special tokens (`<|eom|>` above all) as stop strings; pin the error and the validation points. |
| D4 | **Leaked-call detection** — recognise a *complete* `<tool_call>{…}</tool_call>` block in assistant *content*; convert a whole-content block to a structured call or fail with a typed error. A partial/malformed fragment, or a complete block embedded in surrounding prose, is text (never executed); a convertible block is never silently treated as a final answer. The native ATEM form in content is delegated to D5 (detection → typed error, never converted). |
| D5 | **Native-ATEM detection grammar** — the ATEM grammar of §8.2 is retained as the **detection grammar** only: a complete native `<atem:invoke>…</atem:invoke>` in an assistant tool-channel body (or bare in content) is detected and surfaced as a typed error. ymh does **not** render ATEM declarations, does not render history, and does not convert native ATEM to `ToolCall`; that conversion/rendering path is out of scope (§1.4, §8.3.6). |
| D6 | **Argument normalization** — tolerate `null`/`[]`/omitted `tool_calls`; normalize `arguments` that is `""`/`null`/non-object to `"{}"`. Inbound normalization runs at the **assembler's finalize** (after fragments are concatenated); the only per-delta handling is the whole-object delta, which is never fragmented. Echo-back emits `"{}"` for a non-object. |
| D7 | **Config surface** — expose `temperature`, `top_p`, `top_k`, `tool_choice`, `stop`, `seed` under `llm.default`; pin the keys, the strict-loader registration, and the precedence. |
| D8 | **`top_k` end-to-end** — config → `LlmCallConfig` → request body → parameter equality. |
| D9 | **Per-provider capabilities** — make `ProviderCapabilities` declarable per provider/model instead of one fixed `openai_compatible_capabilities()`. |
| D10 | **`reasoning_effort` accepts `xhigh`** in the type, docs and schema; confirm nothing clamps it. |
| D11 | **Reversibility contract** — pinned as an invariant: Muse-named identifiers confined to the profile table, one revertible commit, the default profile inert (test-pinned), a documented `git revert` procedure. |

### 1.2 What this changes, in one sentence

A named **model profile** (`llm.default.profile`) becomes the single switch that
turns on a set of provider behaviours — a forced first tool call, a stop-token
denylist, leaked-call detection, native-ATEM detection, argument normalization,
and a per-model capability declaration — while the sampling and tool-choice
knobs (`temperature`, `top_p`, `top_k`, `tool_choice`, `stop`, `seed`) become
first-class config, all in one commit that a single `git revert` removes. ymh
continues to send standard OpenAI messages and `tools[]`; the server's chat
template owns all ATEM rendering, and ymh only *detects* native ATEM syntax when
a server fails to translate it.

### 1.3 Amendment register

| ID | Decision | Amends |
|---|---|---|
| 47-D1 | `llm.default.profile` + the `ModelProfile` type and lookup; profile sampling defaults | 08 §5.1/§5.3, 21 §3.3, 04 §8 (`LLMProviderConfig` gains `profile`) |
| 47-D2 | Forced first tool call from the turn-local step counter | 08 §6.2, 06 §5/§11, 31 (agent-loop errata), 41 §6.4 |
| 47-D3 | Stop-token denylist (config + adapter) | 08 §6.2, 21 §3.3 |
| 47-D4 | Leaked/native call detection in the OpenAI-compatible adapter | 08 §4.2/§4.5, 26 §4.3 |
| 47-D5 | The native-ATEM detection grammar, profile-gated and detection-only | 08 §4.2/§6.2 |
| 47-D6 | `arguments`/`tool_calls` normalization | 08 §4.2, `tool_call_assembler` contract |
| 47-D7 | `llm.default.{temperature,top_p,top_k,tool_choice,stop,seed}` | 08 §3.1/§5.3, 21 §3.3/§7.5 |
| 47-D8 | `top_k` in `GenerationParameters`/`LlmCallConfig`/body/equality | 08 §3.1/§6.2, 26 §4.3.1 |
| 47-D9 | `ModelProfile` capability overlay; `LLMProviderConfig.profile` | 08 §3.4/§5.1/§5.4 |
| 47-D10 | `reasoning_effort` documents `xhigh` (type already accepts it) | 08 §3.1, 21 §3.3 |
| 47-D11 | The reversibility contract | 00 §9 (commit hygiene), this spec §19 |

### 1.4 Scope boundaries

**In scope:** the eleven decisions above, the native-ATEM detection grammar
(§8.2), the config surface, the adapter/loop/registry changes, the tests, and
the reversibility contract.

**Out of scope (explicit):**

- **ATEM conversion and rendering (the Oracle's structural finding).** ymh does
  not render ATEM tool declarations, does not render ATEM history/echo-back, and
  does not convert native ATEM output into `ToolCall`. Rationale: the "codec" is
  one-and-a-half-directional — ymh can render tool *declarations* and parse tool
  *calls*, but the conversation **history is rendered by the server's Jinja chat
  template** and the model's native output is a **multi-message channel stream**,
  not a bare invoke. ymh does not own that component, so the conversion path
  cannot be verified and its round-trip tests are unimplementable (47-O-M1/M2).
  D5 therefore keeps only the **detection** grammar: native ATEM syntax in
  content is detected and surfaced as a typed error, never converted and never
  executed. **If the internal endpoint turns out to pass ATEM through
  untranslated, supporting it is a NEW spec, not this one** (§8.3.6). The
  realistic case — a server that translates to standard `tool_calls` — is fully
  supported.
- **Context-window / token-count tuning.** User directive: the configured
  context values are correct and no work is to be done there. `ModelInfo::
  max_context_tokens` stays `0` (unknown) — `src/llm/openai_adapter.cpp:855-860`.
- **Vision / image input.** Muse-Glimmer has a ViT-G/14 perception encoder; ymh
  is text-only. The `Image` content path (`src/llm/openai_adapter.cpp:260-267`)
  is untouched.
- **Activating the retry executor.** Spec 41 pins `RetryExecutor` but does not
  activate it (`41-retry-executor.md:141-150`, `:530-538`); this spec only pins
  the *interaction* with D2 (§5.5) and activates nothing.
- **Remote SSH/TCP transport.** Out of scope for the project (`AGENTS.md`).
- **`chat_template_kwargs` / `reasoning_strength`.** Muse's actual
  reasoning-length knob is the template variable `reasoning_strength`
  (`low|medium|high|xhigh`), not `reasoning_effort`; see §13.4 and §20 Q1. This
  spec pins `reasoning_effort` per D10 and records the discrepancy as a
  **decided** interpretation (not an open question): no `chat_template_kwargs`
  surface is added.

### 1.5 Terminology (pinned)

- **Model profile** = a `ModelProfile` value (47-D1) resolved from
  `llm.default.profile`. **Inert profile** = one whose `id` is empty; every
  profile-gated branch is then a no-op (§16, 47-I1).
- **Profile seam** = the set of generic symbols through which a profile acts:
  `ModelProfile`, `find_model_profile`, `is_known_model_profile`,
  `LLMProviderConfig::profile`, `AgentConfig::profile`, the generic
  leaked-call/native-ATEM detector functions, and the profile-gated branches in
  the adapter, the agent loop, the assembler, and the registry. **Muse-named
  identifier** = any identifier or string literal that names Muse (`muse`,
  `Muse`, `muse-glimmer`, case-insensitively). **Muse-only symbols** = the
  Muse-named identifiers; they live only in `src/llm/model_profile.cpp` and the
  Muse test files (§19.2, 47-I6). The detector carries no Muse name and is
  *generic* code that a profile flag switches on.
- **Muse profile** = the built-in `"muse-glimmer"` profile. There is **no**
  ATEM/`tool_protocol` profile in Rev 4: ATEM rendering/conversion is out of
  scope (§1.4), so the Muse profile simply enables the native-ATEM **detection**
  flag (47-D5).
- **Leaked call** = a tool-call syntax that appears in assistant *content*
  rather than in the structured `tool_calls` field: the `<tool_call>{…}
  </tool_call>` JSON form (G-JSON, detected **and** converted when it is the
  whole content) or the native ATEM form (G-ATEM, detected and surfaced as a
  typed error, never converted) (47-D4/D5).
- **Native-ATEM detection** = the D5 presence test for a complete
  `<atem:invoke>…</atem:invoke>` block in an assistant tool-channel body or
  bare in content; its outcome is a typed `MalformedToolCall` error, never a
  converted call.
- **Control token** = one of Muse's framing special tokens
  `{<|begin_of_text|>, <|end_of_text|>, <|start|>, <|message|>, <|eom|>,
  <|eot|>}` (47-D3). **Message delimiter** = `<|eom|>`; **turn delimiter** =
  `<|eot|>`.
- **Forced first call** = the 47-D2 override that sends `tool_choice:
  "required"` on a turn's first provider request.
- **Effective tool choice** = the `tool_choice` value that actually reaches the
  wire after the 47-D2 override and the D7 config value are combined.
- **Parameter equality** = `call_config_equals` (`src/llm/llm_runtime.cpp:
  127-133`), the single change test used by `PreparedCall::stream` to reject a
  frozen config that differs from the prepared one (`:180-184`).

---

## 2. Evidence base

### 2.1 Findings (primary sources)

| # | Finding | Primary source |
|---|---|---|
| A | `<|eom|>` is end-of-*message*, **not** end-of-turn; the turn continues after it. The model's `eos` is `[200001, 200008]` = `[<|end_of_text|>, <|eot|>]`. A client that stops on `<|eom|>` truncates a turn after its first message and **collapses multi-call turns**. | `meta-models/Muse-Glimmer-30B-GGUF` model card, "Never stop on `<|eom|>`"; `meta-models/Muse-Glimmer-30B` prompting guide, "Special tokens" table. |
| B | The stock model produced a well-formed tool call only **98.3%** of the time when one was required; it names the right function in its reasoning and then keeps deliberating — a "decisiveness and format gap". | `PursuitOfDataScience/Muse-Glimmer-30B-ToolCall-LoRA` model card. |
| C | Forcing `tool_choice: "required"` on iteration 1 (reverting to `"auto"` after any assistant turn with tool calls, and never overriding an explicit non-`auto` choice) deterministically fixes B-class failures on OpenAI-compatible endpoints. | n8n issue #31135. |
| D | The model's native tool syntax is **ATEM** (`atem:invoke`, `to=` recipient channel); the tool-calling finetune's training data also leaks Toucan's `<tool_call>{…}</tool_call>` JSON into assistant prose — a syntax the Muse template never uses (7.4% of records). | LoRA model card; `meta-models/Muse-Glimmer-30B` prompting guide, "Tool calling". |
| E | Recommended sampling `temperature=1.0, top_p=0.95, top_k=64`; reasoning strengths `low`/`medium`/`high`/**`xhigh`** (default `high`). | GGUF model card, "Sampling defaults" and "Reasoning cannot be switched off". |
| F | OpenAI-compatible servers vary on empty tool calls (`null`/`[]`/omitted) and on `arguments` being `""`/`null`/a non-object; strict backends 400. | Reported (`harness-rs-models`); the shipped tolerance is verified in code (§9.1). |

### 2.2 Token identities (authoritative)

Derived from the checkpoint's `tokenizer_config.json` `extra_special_tokens`
(2,048 entries; index 0 = `<|begin_of_text|>` = id 200000, matching the model
card's `eos = [200001, 200008]`):

| Token | Id | Role |
|---|---|---|
| `<|begin_of_text|>` | 200000 | BOS |
| `<|end_of_text|>` | 200001 | EOS |
| `<|eom|>` | **200007** | end of *message* — the turn continues |
| `<|eot|>` | **200008** | end of *turn* — the model stops here |
| `<|start|>` | 200022 | opens a turn header |
| `<|message|>` | 200023 | separates the header from the body |

### 2.3 Corrections to the approved plan

1. **Finding A's token id is wrong.** The plan writes "`<|eom|>` (200008)". The
   model card assigns **200008 to `<|eot|>`** (the legitimate turn delimiter)
   and `<|eom|>` is **200007** (§2.2). This matters: a guard that rejected
   "200008" would reject the model's own EOS, and a guard that keyed on the id
   rather than the token string would be off by one. **47-D3 keys on the token
   string `<|eom|>` and records the id as 200007.**
2. **D7's "current state" is wrong.** The plan says "today only
   `agent.reasoning_effort` is settable". In fact `llm.default.reasoning_effort`
   and `llm.default.max_tokens` are already accepted and mapped
   (`src/config/config.cpp:399-403`, `:417-434`; `src/cli/wiring.cpp:204-211`).
   The genuinely missing keys are `profile`, `temperature`, `top_p`, `top_k`,
   `tool_choice`, `stop`, `seed` (§10.1). 47-D7 pins the real delta.

---

## 3. Already satisfied — no change

These are verified against the shipped tree and require **no behavioural
change**. They are pinned as retained/regression, not as new work.

| Item | Evidence | Disposition |
|---|---|---|
| Parallel tool calls are executed | `src/agent/agent_loop.cpp:1149` (all `response.tool_calls` passed to `execute_tool_calls`), `:1177-1184` (parallel bound `max_parallel_tool_calls`, default 10 at `include/ymh/agent/agent.hpp:96`), `:1221-1296` (the in-flight scheduler) | **already satisfied — no change** (47-I11) |
| `reasoning_content` / `reasoning` parsing | `src/llm/openai_adapter.cpp:440-450` (both field names, guarded by `capabilities_.reasoning`) | **already satisfied — no change** |
| `tool_calls` is array-guarded | `src/llm/openai_adapter.cpp:452` (`is_array()` before iteration) | **already satisfied — no change** |
| `stream_options.include_usage` | `src/llm/openai_adapter.cpp:727-730` (guarded by `capabilities.usage_streaming`) | **already satisfied — no change** |
| `finish_reason` mapping | `src/llm/openai_adapter.cpp:84-98` + `:410-414` (`stop`/`length`/`tool_calls`/`function_call`/`content_filter`) | **already satisfied — no change** |
| `stop` / `tool_choice` pass-through in the adapter | `src/llm/openai_adapter.cpp:742-747` | **already satisfied — no change**; 47-D3 adds a *guard*, not a new emission |
| Empty `tool_calls` (`null`/`[]`/omitted) tolerated inbound | `src/llm/openai_adapter.cpp:452` (null/omitted skip; `[]` iterates zero) | **already satisfied — no change** (47-D6) |
| Empty/absent `arguments` becomes `{}` | `src/llm/openai_adapter.cpp:475-478` (non-string ignored) + `src/llm/tool_call_assembler.cpp:94-96` (empty/blank → `json::object()`) | **already satisfied — no change** (47-D6) |
| `reasoning_effort` is not clamped | `std::optional<std::string>` (`include/ymh/llm/llm_call_config.hpp:35`), copied verbatim (`src/cli/wiring.cpp:204-208`, `src/agent/agent_loop.cpp:537`), emitted verbatim (`src/llm/openai_adapter.cpp:748-750`); no validator anywhere | **already satisfied — no change**; 47-D10 updates docs only |
| `llm.default.max_tokens` / `reasoning_effort` settable | `src/config/config.cpp:417-434`; `src/cli/wiring.cpp:209-211`, `:204-208` | **already satisfied — no change** (corrects plan D7) |

---

## 4. D1 — The model profile seam

### 4.1 Current state (verified)

- There is no profile concept. `LlmSettings` carries provider, base URL, model,
  key reference, `max_tokens`, `reasoning_effort`, concurrency, timeouts and
  retry only (`include/ymh/config/config.hpp:121-141`).
- The provider adapter is constructed with **one fixed capability set**:
  `register_builtin_providers` hard-codes `openai_compatible_capabilities()`
  into the factory (`src/llm/provider_registry.cpp:102-111`, the call at `:108`).
- `AgentConfig` carries `GenerationParameters` but no profile
  (`include/ymh/agent/agent.hpp:108-123`); `LLMProviderConfig` carries no
  profile (`include/ymh/llm/provider_registry.hpp:26-41`).
- Config layering is built-in → global → workspace → env → CLI
  (`include/ymh/config/config.hpp:6-12`; `src/config/config.cpp:1309-1315`).

### 4.2 Decision (47-D1)

Introduce a `ModelProfile` value and a lookup keyed by `llm.default.profile`.

**4.2.1 The type.** `ModelProfile` is a *generic* declaration (no Muse names);
its fields are the seam through which a profile acts:

```cpp
// include/ymh/llm/model_profile.hpp  (new)
struct ModelProfile {
    std::string id;                                  // "" = inert (47-I1)
    bool force_first_tool_call = false;              // 47-D2
    bool normalize_tool_arguments = false;           // 47-D6
    bool detect_leaked_tool_calls = false;           // 47-D4 (G-JSON)
    bool detect_atem_tool_calls = false;             // 47-D5 (G-ATEM detection only)
    std::vector<std::string> forbidden_stop_tokens;  // 47-D3
    ProfileCapabilities capabilities;                // 47-D9
    std::optional<double>        temperature;        // 47-D1.4
    std::optional<double>        top_p;              // 47-D1.4
    std::optional<std::uint32_t> top_k;              // 47-D1.4
};
```

**4.2.2 The lookup.** `find_model_profile(std::string_view)` returns a pointer
to a static built-in profile, or `nullptr` for `""` and unknown ids;
`is_known_model_profile(std::string_view)` returns whether a non-empty id
resolves. Both are total, `noexcept`, and case-sensitive. The table lives in
`src/llm/model_profile.cpp` (the only Muse-named value table, §19.2).

**4.2.3 The config key and its default.** `llm.default.profile` (string),
default `""`. The key is added to **both** allowed-key lists
(`src/config/config.cpp:388-392` flat `llm`, `:399-403` `llm.default`) and to
`LlmSettings`. **A non-empty id that does not resolve is a `ConfigError`**
(`"unknown llm.default.profile: …"`), consistent with the strict loader
(`include/ymh/config/config.hpp:14-15`, `src/config/config.cpp:79-100`). An
absent/empty id is the inert profile and is never an error.

**4.2.4 Profile sampling defaults.** The profile may declare default sampling
values (finding E: `temperature=1.0`, `top_p=0.95`, `top_k=64` for Muse). The
resolution order is **explicit config > profile default > omit** (pinned,
47-I8). Defaults are applied in `to_agent_config` only when the corresponding
`config.llm.*` value is unset. A profile default never overrides an explicit
config value and never introduces a key into the wire body unless the profile
supplies it.

**4.2.5 Where the profile flows.** `to_agent_config` resolves the profile and
sets `AgentConfig.profile` (for 47-D2); `to_provider_config` resolves it and
sets `LLMProviderConfig.profile` (for 47-D3/D4/D5/D6/D9). Resolution is done
once per config load in the wiring layer; the adapter and loop read only the
resolved value. The agent loop and adapter never call `find_model_profile`
themselves — that keeps the Muse id string in one place.

### 4.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F12 | A typo'd profile id | Silent loss of all Muse behaviour | `ConfigError` at load (47-I2) |
| 47-F17 | A future edit reads a profile flag directly in an unrelated module | The seam leaks | §19.2 grep check + review |
| 47-F18 | Explicit config and a profile default disagree | Ambiguous precedence | Explicit wins (47-I8) |

---

## 5. D2 — Forced first tool call

### 5.1 Current state (verified)

- The turn loop is `for (std::size_t stepNumber = 1;; ++stepNumber)`
  (`src/agent/agent_loop.cpp:929`). Each iteration builds one request
  (`:970`, `:1102`).
- `buildRequest` sets `config.tool_choice = config_.parameters.tool_choice`
  (`src/agent/agent_loop.cpp:543`) and copies the whole parameter block into the
  request (`:531`). `tool_choice` is never set by the wiring today, so it is
  `nullopt` and the body omits the field (`src/llm/openai_adapter.cpp:745-747`).
- If a response carries no tool calls, the turn ends as a final answer
  (`src/agent/agent_loop.cpp:1133-1141`). Therefore **step ≥ 2 always follows an
  assistant message that emitted tool calls**; step 1 is the only step that can
  have none.
- The in-adapter retry loop is `for (int attempt = 0; attempt < 2; ++attempt)`
  (`:982`); the separate durable `RetryExecutor` is **pinned, not activated**
  (`41-retry-executor.md:141-150`, `:530-538`).

### 5.2 Decision (47-D2)

When `profile.force_first_tool_call` is true, the **effective tool choice** for
a request is computed from the **turn-local step counter**, never from the
assembled message history:

```
force_required  =  profile.force_first_tool_call
                && turn_step == 1                  // loop-local, reset per turn
                && !request.tools.empty()          // internal list
                && !configured_tool_choice.has_value()
```

- **The turn-local parameter must be added (47-O-M4).** `buildRequest`'s shipped
  signature is `FrozenRequest buildRequest(const std::vector<Message>& messages,
  TurnId turn, StepId step)` (`src/agent/agent_loop.cpp:522-523`). The `step`
  argument is the **session-global** `StepId` from `session_.nextStepId()`
  (`:930`; `include/ymh/session/session.hpp:297`, `:317`; advanced on every
  `StepStarted`, `src/session/session.cpp:624-625`) — it is **not** turn-local,
  and using it would force `required` on the first step of the *entire session*
  and never again. Rev 4 pins the signature change
  `buildRequest(messages, turn, step, std::size_t turn_step)` and both call
  sites (`:970`, `:1102`) pass the loop-local `stepNumber` declared at `:929`
  (§15). `turn_step` is 1 on the first request of a turn and increases by one per
  completed step; it is **not** derived from `messages`.
- **No separate wire guard (47-O2-M1).** `tool_choice: "required"` with no
  `tools` is a 400 on OpenAI, vLLM, and llama.cpp, but the guard is the
  `!request.tools.empty()` clause above and nothing else. No reachable state
  separates "`request.tools` is non-empty" from "the outgoing body carries
  `tools[]`": `build_chat_completions_body` takes **no profile** and emits
  `tools[]` iff `!request.tools.empty()` (`src/llm/openai_adapter.cpp:703-756`,
  `:714-725`; unchanged by 47-I16), no `ModelProfile` field can omit `tools[]`
  (§15), and `stream()` rejects a tools-bearing request with `UnsupportedModel`
  **before** the body is built when `capabilities_.tool_calls` is false
  (`:895-898`). Rev 3's `wire_tools_present` conjunct was therefore a tautology
  with no input source and is **removed**; the 400 hazard is closed
  **structurally** by the unchanged body builder, not by a guard. 47-F22 is the
  single forward-looking note (47-F41 merged into it): a future profile that
  adds a `tools[]`-omission seam would need its own guard and test, and none
  exists in this design.
- If `force_required`, the effective value is `"required"`; otherwise it is the
  configured value unchanged (absent ⇒ the body omits `tool_choice`,
  `src/llm/openai_adapter.cpp:745-747`).
- The override is applied to **both** `request.parameters.tool_choice` (the wire
  body source, `src/llm/openai_adapter.cpp:745`) **and** `config.tool_choice`
  (the frozen-config/template-digest source, `src/agent/agent_loop.cpp:534-543`;
  `src/llm/llm_runtime.cpp:36`). They must agree or `call_config_equals` and the
  body disagree (47-I3).
- **Safeguards (from finding C):** no override when no tools are offered
  (`request.tools` empty); no override of an **explicitly configured** choice
  (`none`, `required`, a named function, **or `"auto"`**); no override on any
  step after the first.
- **Pure-question posture (M2).** An explicit `tool_choice` — including
  `"auto"` — is the operator's opt-out: with `llm.default.tool_choice: "auto"`
  the profile never forces a call, so a user who does not want finding C's fix
  on a per-deployment basis can disable it without leaving the profile. With no
  `tool_choice` configured (the default) **every profiled turn begins with a
  forced call**, including a pure question with tools offered. This is the
  documented trade-off of finding C (its n8n fix targets turns that *should*
  call; ymh applies it to all profiled turns). If the model has no useful tool
  it is expected to answer in a call's `arguments`/prose rather than loop; the
  opt-out and the failure mode 47-F37 are the escape hatches.

### 5.3 Why a turn-local step counter (and not the assembled history)

The n8n reference implementation keys on "any prior assistant turn with
`tool_calls`" (finding C). Rev 1 translated that to a predicate over the
assembled `messages`; **that is unsound**, because the loop rewrites the
assembled history. Compaction (`src/agent/agent_loop.cpp:952-967` under
pressure, `:1095-1102` under context overflow) replaces the prefix with a
summary and then re-assembles (`:961`, `:1097`); once the assistant `ToolUse`
message falls outside the retained window, a history predicate would be true
again on a step ≥ 2 and re-force a call (M1). The turn-local counter avoids
this entirely:

- **Compaction-proof.** `stepNumber` is loop-local (`src/agent/agent_loop.cpp:
  929`) and is never rewritten by compaction; a step ≥ 2 always has
  `turn_step ≥ 2`, regardless of what `messages` contains.
- **Restart-safe.** A turn is never resumed mid-flight: `AgentRegistry::resume`
  (`src/agent/agent_registry.cpp:77`) resumes a *session*, and the loop always
  begins a new turn at step 1. The counter therefore needs no persistence.
- **Replay-safe.** Replay reproduces the frozen per-step `FrozenRequest`
  (`src/llm/llm_runtime.cpp:150-166`) rather than recomputing the choice, so the
  value that was sent is the value that is replayed.
- **Idempotent** under the in-band retry loop (`src/agent/agent_loop.cpp:982`):
  a retry within a step keeps the same `turn_step`, so it keeps the same choice.

### 5.4 Step-1 equivalence (now definitional)

`force_required` is true exactly when `turn_step == 1`; the predicate *is* the
step, not an inference about it. Step 1 is the only step of a turn with no prior
assistant tool-call message (§5.1), and every later step has `turn_step ≥ 2`.
The equivalence holds under compaction because compaction changes `messages`,
not `turn_step`; the M1 failure ("forced call re-triggers after compaction") is
structurally impossible.

### 5.5 Interaction with the pinned retry executor (spec 41)

- The effective tool choice is baked into the `LlmCallConfig` at step start
  (`src/agent/agent_loop.cpp:534-543`) and frozen into the `FrozenRequest`
  (`src/llm/llm_runtime.cpp:150-152`). A retry of that step replays the frozen
  config, so a retried step 1 still sends `"required"` and a retried step ≥ 2
  still sends `"auto"`.
- **The executor must not recompute `tool_choice`.** When spec 41 is activated,
  its replay path must reuse the frozen `LlmCallConfig` (spec 41 §6.4 owns the
  executor; this spec pins only that D2's value is step-scoped and frozen).
- Because a retry is step-scoped and the predicate is turn-scoped, a retry can
  never flip the choice mid-step (47-I3). Activation of spec 41 does not change
  D2.

### 5.6 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F3 | `force_first_tool_call` with no tools offered (the internal list is empty) | The backend 400s on `tool_choice: "required"` with no `tools` | The `!request.tools.empty()` guard (47-I3) |
| 47-F22 | A future profile adds a `tools[]`-omission seam while `force_first_tool_call` is on (no such seam exists in this design) | `tool_choice: "required"` with no `tools` → 400 | Structurally impossible today: `build_chat_completions_body` is unchanged and no `ModelProfile` field can omit `tools[]` (47-I3/47-I16, §8.3.2). Rev-3 47-F41 is merged here (47-O2-M1) |
| 47-F4 | The override is applied on step ≥ 2 (e.g. a history predicate after compaction) | The model is forced to call again instead of answering | The `turn_step == 1` predicate (47-I3, §5.3) |
| 47-F5 | The override clobbers an explicit choice | The user's intent is lost | The explicit-choice guard — any configured value (including `"auto"`) wins (47-I3) |
| 47-F19 | `request.parameters` and `config` disagree after the override | Body and digest diverge; `ConfigMismatch` on `PreparedCall::stream` | Set both in `buildRequest` (47-I3) |
| 47-F37 | The backend rejects `tool_choice: "required"` (400) | The turn fails with `BadRequest` (non-retryable, `src/llm/openai_adapter.cpp:126-136`) | Set `llm.default.tool_choice: "auto"` to opt out, or drop the profile |

---

## 6. D3 — Stop-token guard

### 6.1 Current state (verified)

- `stop` is `std::vector<std::string>` (`include/ymh/llm/llm_call_config.hpp:38`,
  `include/ymh/llm/llm_request.hpp:45`) and is emitted verbatim when non-empty
  (`src/llm/openai_adapter.cpp:742-744`).
- **No `stop` config key exists** (`src/config/config.cpp:388-392`, `:399-403`),
  so today `stop` is always empty in production. 47-D7 adds the key; 47-D3
  guards it.
- There is no validation of stop values anywhere.

### 6.2 Decision (47-D3)

**6.2.1 The denylist.** A profile may declare `forbidden_stop_tokens`. The Muse
profile declares the six framing control tokens (§1.5). The load-bearing entry
is **`<|eom|>` (id 200007)**: stopping on it truncates a turn after its first
message and collapses multi-call turns (finding A). The other five are rejected
because a client stop string is matched against decoded text, whereas these are
tokenizer-level control tokens: supplying them is at best a no-op and at worst
interacts badly with server-side tokenization. `<|eot|>` (200008) and
`<|end_of_text|>` (200001) are the model's own terminators; the server already
stops there, so a client stop string for them is redundant.

**6.2.2 Where it is validated (two points).**

1. **Config load (`apply_llm`, `src/config/config.cpp:386-451`).** After reading
   `profile` and `stop` from the same section, if the resolved profile declares
   `forbidden_stop_tokens` and any `stop` entry equals one (exact,
   case-sensitive), `fail(source, "llm.default.stop must not contain '" +
   offending + "': it ends a message, not the turn")` → `ConfigError`, where
   `offending` is the actual entry (`<|eom|>`, `<|eot|>`, `<|start|>`, …).
   `profile` is read with a fallback to the accumulated `config.llm.profile`
   (the same pattern as `provider`/`base_url`/`model`,
   `src/config/config.cpp:426-432`) and `stop` is read with a fallback to the
   accumulated `config.llm.stop`, so a profile set in the global layer and a
   `stop` set in the workspace layer **are** caught: `load_config` applies both
   layers onto one `Config` (`src/config/config.cpp:1309-1315`) and the
   workspace-layer `apply_llm` sees the global profile as its fallback. The
   check is therefore not best-effort for JSONC layers.
2. **Adapter (`OpenAICompatibleProvider::stream`, before body build, around
   `src/llm/openai_adapter.cpp:892-915`).** This is **authoritative** for every
   path that does not pass through `apply_llm` (environment overrides at
   `src/config/config.cpp:1309-1313`, a future CLI flag, or a programmatic
   caller). If `config_.profile` declares forbidden tokens and
   `request.parameters.stop` contains one, return
   `failed(make_error(LLMErrorCode::BadRequest, "stop token '" + offending +
   "' is forbidden by profile '" + config_.profile.id + "'"))`. The profile id
   is interpolated from the value, never hard-coded, so the message carries no
   Muse name outside the profile table (47-I6). `BadRequest` is not retryable
   (`src/llm/openai_adapter.cpp:126-147`) and maps to
   `AgentErrorCode::ProviderFailed` (`src/agent/agent.cpp:26-37`), surfacing a
   `TurnFailed` (`src/agent/agent_loop.cpp:1125-1131`).

**6.2.3 No-profile posture.** Without a profile the denylist is empty and `stop`
passes through exactly as today (47-I1).

### 6.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F1 | A user sets `stop: ["<|eom|>"]` under the profile | Multi-call turns collapse after the first call | Config `ConfigError` + adapter `BadRequest` (47-I4) |
| 47-F2 | A future CLI flag sets a forbidden stop, bypassing config | The guard is skipped | The adapter guard is authoritative (47-I4) |
| 47-F20 | A workspace layer omits `stop` while the global layer set a forbidden one | If `stop` is read without a fallback (`read_string_array` returns an empty vector when absent, `src/config/config.cpp:194-214`), the global entry is cleared instead of rejected | The D7 read preserves the accumulated `stop` when the key is absent (§10.2.1); the adapter guard backstops (47-I4) |

---

## 7. D4 — Leaked / native call-syntax detection

### 7.1 Current state (verified)

- `handle_delta` emits every `content` string as `TextDelta`
  (`src/llm/openai_adapter.cpp:432-438`) and only assembles structured
  `tool_calls` (`:452-459`, `:462-505`).
- If a model leaks call syntax into content, the adapter treats it as prose;
  the loop sees `response.tool_calls.empty()` and ends the turn as a final
  answer (`src/agent/agent_loop.cpp:1133-1141`). **No detection exists.**

### 7.2 Decision (47-D4)

When `profile.detect_leaked_tool_calls` is true, the decoder recognises the
leaked `<tool_call>` JSON grammar in assistant **content** and lifts a
whole-content block into the structured channel. The native ATEM grammar is
handled by D5 as a **detection** (a typed error), never a conversion (§7.2.7).

**7.2.1 G-JSON — the leaked `<tool_call>` form.** A block
`<tool_call>` `ws?` JSON-object `ws?` `</tool_call>` where the object is either

```json
{ "name": "<tool>", "arguments": { … } }
```

or

```json
{ "function": { "name": "<tool>", "arguments": { … } } }
```

The body is parsed with `nlohmann::json`; `arguments` must be an object (or is
normalized per 47-D6). This grammar is the exact shape the LoRA card reports
leaking (finding D).

**7.2.2 G-ATEM — the native form (detection only, D5).** A complete
`<atem:invoke>…</atem:invoke>` block (§8.2), optionally wrapped in the
`<atem:function_calls>` framing and optionally preceded by the
`to=<tool><|message|>` channel header of §8.2.1. G-ATEM is **detected, not
converted**: when `profile.detect_atem_tool_calls` is true, a detection produces
a terminal `MalformedToolCall` (§7.2.7, §8.3). This is the D5 arm and the only
ATEM behaviour in Rev 4.

**7.2.3 The G-JSON decision table (pinned).** The **whole-content rule** of
§7.2.4 governs every row: "content" below means the entire trimmed content. The
table applies **only to G-JSON**, because only G-JSON is converted; the G-ATEM
rows are in §7.2.7.

| Condition | Outcome |
|---|---|
| Content is one or more complete G-JSON blocks (nothing but whitespace outside them), every name is an **offered** tool, and every argument set parses to an object (after normalization) | Convert: emit `ToolCallStarted`/`ToolCallDelta`/`ToolCallFinished` (via `emit`, `src/llm/openai_adapter.cpp:363-372`), append to `tool_calls_`, force `response.finish = FinishReason::ToolCalls` (§7.2.5, L1) |
| Content is a complete G-JSON block whose **outer JSON is not an object** or **does not carry a string `name`** (e.g. `<tool_call>[1,2]</tool_call>`) | Terminal `LLMErrorCode::MalformedToolCall` (the outer body must be a JSON object with a string name) |
| Content is a complete G-JSON block whose name is **not** offered | Terminal `LLMErrorCode::MalformedToolCall` → `StreamError` → `AgentErrorCode::ProviderFailed` |
| Content is a complete G-JSON block with an offered name whose `arguments` is unparseable as JSON and not normalizable | Terminal `LLMErrorCode::MalformedToolCall` |
| Content is a complete G-JSON block with an offered name whose `arguments` is `""`/`null`/a non-object under `normalize_tool_arguments` | Normalize to `{}` (47-D6) and convert |
| A partial/malformed fragment (a delimiter missing) | Text (no conversion, no error) |
| A complete G-JSON block **embedded in surrounding non-whitespace content** (prose, a quoted example, a fenced snippet) | Text (never executed); a warning is logged naming the leak; the turn may end as a final answer |

The taxonomy is now disjoint (47-O-L3): the **outer** JSON not being an object
is `MalformedToolCall`; a non-object **`arguments`** value under normalization is
`{}`; only a **missing delimiter** (an incomplete block) is text.

**The parser result shape (47-O2-M2).** `parse_leaked_json_call` returns a
`LeakedParse` (§15), not a single `std::optional<LeakedCall>`: it carries `calls`
(one entry per complete whole-content block whose outer JSON is an object and
whose `name` is a string) plus two flags — `complete_block_seen` (a complete
`<tool_call>…</tool_call>` block constitutes the whole trimmed content, so
nothing but whitespace is outside it) and `malformed_block_seen` (a complete
whole-content block could not yield a `LeakedCall`). The parser stays pure:
offered-name and normalization remain in `finalize` (47-O-M3). `finalize` maps
the shape onto the rows above: `!complete_block_seen` → text (rows 6/7);
`malformed_block_seen`, a non-offered name, or unparseable offered-name
`arguments` → `MalformedToolCall` (rows 2/3/4); an all-offered,
all-normalizable `calls` set → conversion (rows 1/5). Row 1's "one or more
blocks" is therefore expressible, and `calls.size() > 1` converts every block.
A complete block whose outer JSON is not an object is decidable
(`malformed_block_seen`) from "no block at all" (`!complete_block_seen`), which
the Rev-3 single-optional shape conflated.

**7.2.4 False-positive posture (pinned).** Conversion is conservative and
purely syntactic; there is no semantic "intent" signal. Four rules:

1. **Whole-content only (H3).** A complete block is converted only when it
   constitutes the *entire* trimmed content. A complete block surrounded by
   prose — a docs example, a test fixture, a quoted explanation, a fenced
   snippet — is **never executed**. This is the mitigation for the
   false-positive class: ordinary prose that merely *discusses* tools is text.
   The cost is that a genuine leak embedded in prose is not auto-lifted; it is
   logged as a warning and left visible as text (47-F38).
2. **Offered name.** The parsed name must exactly match an offered tool
   (`request.tools`); otherwise `MalformedToolCall` (a name that is not offered
   is more likely a hallucination or prose than a call).
3. **Object arguments.** Arguments must parse to a JSON object after
   normalization; otherwise `MalformedToolCall`.
4. **Reasoning is never scanned.** For G-JSON conversion, reasoning arrives on
   the separate `reasoning_content`/`reasoning` channel
   (`src/llm/openai_adapter.cpp:440-449`), not in content, so a call quoted in
   reasoning cannot false-positive. For G-ATEM detection, a `to=self` body is
   stripped before scanning (§7.2.7).

The G-JSON conversion rule is **stricter than the reference parser**, which
extracts `<atem:invoke>` anywhere in the response: the checkpoint
`response_template`'s `tool_calls` field uses
`open_pattern = "<atem:invoke\\b[^>]*?\\bname=\"(?P<name>[^\"]+)\">"` and
`close = "</atem:invoke>"` with **no `to=`/recipient requirement and no
whole-content rule**. Rev 1 claimed ymh "mirrors the reference parser's rule
that invokes are extracted only from a tool-recipient body"; that claim was
factually false (H3). For a **conversion** (G-JSON), ymh deliberately trades
recall for precision: an embedded leak becomes visible text plus a warning,
never a silent execution. For a **detection** (G-ATEM), a false positive is a
visible typed error, not a silent execution, so the whole-content rule is not
required (§7.2.7).

**7.2.5 Where and how.** Detection runs in `OpenAiStreamDecoder::finalize`
(`src/llm/openai_adapter.cpp:336-360`) after `finalize_tool_calls()`, and only
when `tool_calls_.empty()` and no terminal error is set. The decoder gains a
bounded `content_text_` accumulator (appended in `handle_delta`, capped by the
existing `max_arguments_bytes`; overflow stops accumulation, never fails the
stream) so detection sees the whole content. Detection is **after** standard
assembly, so a response that already produced structured calls is never
re-interpreted (no double-execution).

The detection logic lives in the generic leaked-call / native-ATEM detector
(`include/ymh/llm/leaked_call_detector.hpp`, §15). **The decoder's host inputs
are pinned (47-O-M3).** The shipped decoder holds only
`(sink, capabilities_, max_arguments_bytes, sse_line_bytes)` and is constructed
at `src/llm/openai_adapter.cpp:935` (ctor `:300-307`); it has no profile and no
offered-name set. Rev 4 adds a `ToolCallPolicy` parameter carrying the resolved
`ModelProfile*` (null when inert) and a `std::span<const ToolName>` of the
offered names built from `request.tools`; `stream()` builds the policy before
constructing the decoder. The detector's **parse functions are pure**
(`parse_leaked_json_call`, `detect_native_atem_calls`, §15); the **offered-name
test** and the **normalization policy** run in `finalize`, where the policy is
in scope — the parse functions cannot perform them (47-O-M3).
`parse_leaked_json_call` returns the `LeakedParse` result shape of §15 (`calls` +
`complete_block_seen`/`malformed_block_seen`), and `finalize` maps that shape
onto the §7.2.3 taxonomy (text / `MalformedToolCall` / conversion); the mapping
is pinned so a complete-but-malformed block is never mistaken for "no block"
(47-O2-M2). A converted
G-JSON call is assigned a synthetic id `"leaked_<index>"`; ids are unique within
the response and never empty.

When a conversion occurs, `finalize` emits `Finished{FinishReason::ToolCalls}`
even if the server sent `finish_reason: "stop"`, because the conversion adds
tool calls and a server `stop` would otherwise end the turn (L1). The emitted
reason is
`converted ? FinishReason::ToolCalls
           : (finish_seen_ ? finish_
                           : (tool_calls_.empty() ? FinishReason::Stop
                                                  : FinishReason::ToolCalls))`,
replacing the shipped expression at `src/llm/openai_adapter.cpp:349-351`.

**7.2.6 Retained text and replay.** The leaked text remains in the streamed
content (it was emitted before `finalize`); conversion adds the structured call
but does not retroactively suppress text. The turn is therefore *not* treated as
a final answer when a call is converted. Suppressing the leaked text from the
durable message is **out of scope** (a streaming holdback would be a larger
change); it is recorded as 47-F7.

The replay consequence is **bounded** (L5): replay reproduces the frozen
`FrozenRequest` (`src/llm/llm_runtime.cpp:150-166`) and the durable
`payload::ToolCall`/`ToolResult` events (`include/ymh/session/events.hpp:131-153`);
it does **not** call the provider and does **not** re-run the detector, so a
replayed leaked turn neither re-executes nor re-detects. The only residual is
that the durable assistant message still contains the raw syntax, so the
transcript shows it — accepted, and the structured call is the authoritative
record. Pinned by `MuseReplay_LeakedTurnDoesNotReexecute` (§18.4).

**7.2.7 The G-ATEM detection arm (D5, 47-O-M1).** When
`profile.detect_atem_tool_calls` is true, `finalize` runs
`detect_native_atem_calls(content)` (§8.3) after `finalize_tool_calls()`, gated
on `tool_calls_.empty()` and no terminal error. Detection is
**presence-based**, not whole-content:

1. Split `content` by the channel grammar (§8.2.1) into messages. A `to=self`
   body is reasoning and is **not scanned**; a `to=user` body is final content
   and is **not scanned**; a `to=<tool>` body is the tool channel and is
   scanned.
2. If the content carries **no** channel framing, scan the whole trimmed content
   (a bare native invoke).
3. A positive result is one or more **complete** `<atem:invoke>…</atem:invoke>`
   blocks (open **and** close present; an incomplete block is text). The
   `<atem:function_calls>` wrapper is optional framing.

A positive detection is a terminal `LLMErrorCode::MalformedToolCall` whose
detail names the offending tool(s) and the profile id; it is **never converted**
and **never executed** (47-F39). Because the outcome is a visible error, not a
silent execution, the Rev 2 whole-content rule is **withdrawn for G-ATEM**
(47-O-M1): the rule exists to stop a *quoted* block from being executed, and
nothing is executed here. The one residual false-positive class is a bare
(unframed) ATEM quote in content; it is accepted and recorded (47-F40). The
raw-endpoint claim of Rev 2 is withdrawn with the conversion path: see §8.3.6.

### 7.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F6 | A complete G-JSON block names a non-offered tool | A hallucinated call could execute | `MalformedToolCall` (47-I5) |
| 47-F7 | The leaked G-JSON text is retained in content | The transcript shows raw call syntax | Accepted; the converted call still executes (§7.2.6); replay does not re-execute (47-I14) |
| 47-F8 | ATEM quoted in a `to=self` reasoning body | A false detection | `to=self` bodies are not scanned (47-I5, §7.2.7) |
| 47-F15 | Content exceeds the accumulator cap | Detection is incomplete | Cap stops accumulation; no conversion/detection for the truncated tail; stream still completes |
| 47-F21 | The detector runs before standard assembly | Double execution | Detection is gated on `tool_calls_.empty()` (§7.2.5) |
| 47-F38 | A complete G-JSON block is embedded in prose (a quoted example, a fenced snippet) | If it were converted, a non-call would execute | Whole-content rule: it is text + a warning log, never executed (§7.2.4); the missed genuine leak ends as a visible final answer |
| 47-F39 | A complete native ATEM invoke is present in content (bare, or in a `to=<tool>` body) | Silently treating a native call as prose would end the turn with no tool run | `MalformedToolCall` typed error naming the tool(s) and profile (§7.2.7, §8.3); never converted, never executed |
| 47-F40 | A native ATEM invoke is quoted in a `to=user` final answer or a bare unframed quote | A false detection | `to=user` bodies are ignored; a bare unframed quote is an accepted residual (§7.2.7) |

---

## 8. D5 — Native-ATEM detection grammar

### 8.1 Current state (verified)

The adapter sends tool declarations as OpenAI `tools[]`
(`src/llm/openai_adapter.cpp:714-725`) and parses only structured `tool_calls`
(`:452-459`). There is no ATEM awareness anywhere in the tree.

### 8.2 The ATEM grammar (pinned)

The authoritative grammar is the checkpoint's `tokenizer_config.json`
`response_template` (reproduced below), corroborated by `chat_template.jinja`
and the prompting guide.

**8.2.1 Channel framing.** A Muse turn is a sequence of channel-scoped
messages:

```text
<|start|>assistant to=self<|message|>…reasoning…<|eom|>
<|start|>assistant to=<tool><|message|><atem:function_calls><atem:invoke name="<tool>"><atem:parameter name="<arg>">value</atem:parameter></atem:invoke></atem:function_calls><|eom|>
<|start|>assistant to=user<|message|>…final answer…<|eot|>
```

- `to=self` routes to reasoning; `to=user` routes to content; any other
  recipient opens a tool channel. The `content.open` pattern requires `to=user`
  (`response_template`: `content.open_pattern = to=user<\|message\|>`), so a
  message is content **only** when its header names `to=user`; the prompt's
  trailing `<|start|>assistant` is matched by the separate `start_anchor`, and a
  header-less first assistant message is anchored there, not by `content.open`.
- `<|eom|>` continues the turn; `<|eot|>` ends it. The prompt ends with
  `<|start|>assistant`, so a turn's first message may be header-less.

**8.2.2 EBNF.**

```text
turn        := message+ final
message     := header body terminator
header      := "<|start|>assistant" ( " to=" recipient )? "<|message|>"
recipient   := "self" | "user" | tool-name
terminator  := "<|eom|>" | "<|eot|>"
body        := reasoning-text | content-text | atem-block
atem-block  := ( "<atem:function_calls>" )? invoke+ ( "</atem:function_calls>" )?
invoke      := "<atem:invoke" attr* "name=\"" tool-name "\"" attr* ">"
               param* "</atem:invoke>"
param       := "<atem:parameter" attr* "name=\"" key "\"" attr* ">"
               value "</atem:parameter>"
value       := json-value | raw-scalar
attr        := ( not '>' )*
```

**8.2.3 The regexes (from `response_template`).** These are the pinned patterns
a detector must use; the grammar is regex-based, not XML (the template says "The
output is not expected to be valid XML and is parsed with regular
expressions").

```text
start_anchor      = <|start|>assistant
content.open      = to=user<\|message\|>
reasoning.open    = to=self<\|message\|>
tool_calls.open   = <atem:invoke\b[^>]*?\bname="(?P<name>[^"]+)">
tool_calls.close  = </atem:invoke>
parameter.tag     = <atem:parameter\b[^>]*?\bname="(?P<key>[^"]+)"[^>]*?>(?P<value>.*?)</atem:parameter>
```

`tool_calls.repeats = true` (one turn may carry several invokes). The pinned
patterns key on `<atem:invoke>…</atem:invoke>`; the outer
`<atem:function_calls>` is **optional framing**, not a detection requirement
(M4). The reference `response_template` likewise detects `tool_calls` via its
`open_pattern`/`close` pair on `<atem:invoke>` with no wrapper requirement, so
a wrapper-less invoke is accepted by both parsers.

**Detection uses only `start_anchor`, `content.open`, `reasoning.open`,
`tool_calls.open`, and `tool_calls.close`.** `parameter.tag` is retained as
reference evidence only: detection does not parse argument values (no
conversion, §8.2.4), so it never invokes that pattern.

**8.2.4 Reference render/parse (out of scope; retained as evidence).** The
checkpoint's `chat_template.jinja` also renders tool declarations and encodes
argument values (`render_atem`, `render_tool_defs`, `render_system_meta`), and
its `response_template` decodes them (`value_parser = { name: "json", args:
{ allow_non_json: true } }`, so `true`/`false`/`null`/numbers and JSON
arrays/objects are typed and everything else is a raw string). ymh implements
**none** of this (47-O-M2): the server template owns declaration and history
rendering, and Rev 4 does not render ATEM at all. Two facts are recorded as
evidence for a future spec — not as ymh behaviour:

- The declaration catalog is a system-block text with a preamble, a
  `// Function schemas` header, one `{"name","description","parameters"}` object
  per tool (the shape of `canonical_tool_schema`,
  `src/llm/llm_runtime.cpp:135-141`), and a `# Valid recipients: …` line. ymh's
  `ToolSchema` has no namespace field (`include/ymh/tools/tool.hpp:45`), so a
  future renderer would list each offered tool name verbatim.
- Render and parse are **not** exact inverses for a string argument whose text
  is itself valid JSON (`"123"` renders as `123` and parses back as the number
  `123`). This is the reference parser's accepted type-loss, not a ymh bug.

No ymh symbol exists for any of the above in Rev 4. The `atem_codec.hpp`/`.cpp`
component, `render_atem_tool_declarations`, `parse_atem_calls`, `AtemCall`, the
`AtemCodec_*` tests, and the `muse_atem_declarations.golden` fixture are
**removed** from the commit (§19.2). The Rev 2 value-encoding/decoding and
declaration sections are deleted with them.

### 8.3 Decision (47-D5)

**8.3.1 One profile; detection only.** There is no ATEM/`tool_protocol` profile
and no new config key. The single built-in `"muse-glimmer"` profile sets
`detect_atem_tool_calls = true`. The documented deployments (vLLM
`--tool-call-parser muse_glimmer`, llama.cpp `--jinja`) translate ATEM to
standard `tool_calls` server-side, so the standard path is fully supported and
the detection arm never fires. The detection arm exists so that if a server
does **not** translate, ymh surfaces a clear typed error instead of silently
treating a native call as prose.

**8.3.2 ymh emits standard OpenAI messages and `tools[]`.** No ATEM text is
rendered. `build_chat_completions_body` is **unchanged**
(`src/llm/openai_adapter.cpp:703-756`; declaration
`include/ymh/llm/openai_adapter.hpp:93-96`): `tools[]` is emitted whenever
`request.tools` is non-empty (`:714-725`), `map_message` emits `arguments` as a
JSON string (`block.arguments.dump()`, `:280`), and `role: tool` results stay
`{"role":"tool","tool_call_id":<id>,"content":<text>}` (`:240-243`). This is
byte-identical to the inert path (47-I1) and is why **no built-in profile — and
no `ModelProfile` field — can omit `tools[]`** when `request.tools` is non-empty
(47-I16, 47-O2-M1); the forced-first-call predicate therefore needs no separate
wire guard (§5.2). A server that applies the
Muse Jinja template renders the declarations and the whole conversation
(including history) itself from these standard messages; ymh does not
participate (§1.4).

**8.3.3 Detection (the only ATEM behaviour).** When
`profile.detect_atem_tool_calls` is true, `OpenAiStreamDecoder::finalize` calls
`detect_native_atem_calls(content)` (§7.2.7). A positive result is a terminal
`LLMErrorCode::MalformedToolCall` naming the tool(s) and the profile id. The
detector:

- recognises a complete `<atem:invoke>…</atem:invoke>` (open **and** close)
  using the `tool_calls.open`/`tool_calls.close` patterns of §8.2.3;
- treats `<atem:function_calls>` as optional framing (§8.2.3, M4);
- ignores `to=self` (reasoning) and `to=user` (final content) bodies; scans
  `to=<tool>` bodies, or the whole trimmed content when no framing is present;
- never converts, never executes, and never emits `ToolCallStarted`/`Delta`/
  `Finished` for a native ATEM invoke.

**8.3.4 Dormancy when the endpoint translates.** When the endpoint translates,
the model's native ATEM never reaches ymh's `content` — the server emits
standard `tool_calls` — so the G-ATEM detector never fires and the G-JSON leak
arm (47-D4) remains active for the independent `<tool_call>` JSON leak. There
is nothing to switch: the shipped `"muse-glimmer"` profile is correct for a
translating endpoint.

**8.3.5 Verification gap.** Whether `ted-ai` translates is unknown: the endpoint
is unreachable from the dev sandbox. Phase 3 records the exact live check (a
scripted tool call, a multi-call turn, and a leaked-syntax turn) (§20 Q3).

**8.3.6 The conversion/rendering path is out of scope (the Oracle's structural
finding).** Rev 1/Rev 2 attempted to render ATEM tool declarations, to echo
prior assistant tool calls back in mapping form, and to convert native ATEM
output into `ToolCall`. That machinery cannot be verified: the conversation
history is rendered by the **server's Jinja chat template**, not ymh, and the
model's native output is a **multi-message channel stream**, not a bare invoke
(47-O-M1/M2). Rev 2's round-trip tests (`AtemCodec_ValueTypes`,
`AtemCodec_RenderRoundTrips`, `AtemCodec_JsonLookingStringIsTyped`) had no ymh
function to round-trip with, because the pinned codec exposed no
`render_atem_call`/`render_atem_history` (47-O-M2). All of it is removed from
this spec and from the commit.

**Decision.** D5 is **detection-only**. ymh detects native ATEM syntax and
surfaces a typed error; it never converts and never executes. If the internal
endpoint turns out to pass ATEM through untranslated, supporting that endpoint
— including any render/history machinery — is a **NEW spec, not this one**. The
grammar of §8.2 is retained solely as the detection grammar.

### 8.4 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F13 | A non-translating endpoint is used with the Muse profile | The model emits native ATEM; ymh cannot convert it | `MalformedToolCall` typed error (§7.2.7, §8.3.3); use a translating endpoint, or a new spec (§8.3.6) |
| 47-F22 | A future profile adds a `tools[]`-omission seam while `force_first_tool_call` is on (no such seam exists in this design) | `tool_choice: "required"` with no `tools` → 400 | Structurally impossible: `build_chat_completions_body` is unchanged and no `ModelProfile` field can omit `tools[]` (§8.3.2, 47-I3/47-I16, 47-O2-M1); Rev-3 47-F41 merged here |
| 47-F23 | A leaked G-JSON block's arguments exceed the assembly cap | `MalformedToolCall` | The existing cap (`include/ymh/llm/provider_registry.hpp:39`) |
| 47-F24 | Native ATEM is quoted in content prose | If it were converted, a non-call would execute | It is never converted; detection ignores `to=user`/`to=self` bodies, and a bare quote is a visible typed error (47-F40) |
| 47-F39 | Native ATEM is detected in a `to=<tool>` body (or bare) | Silently treating the native call as prose would end the turn with no tool run | `MalformedToolCall` typed error (§7.2.7, §8.3.3); never converted |

---

## 9. D6 — Argument normalization

### 9.1 Current state (verified)

- **Inbound `tool_calls`**: only processed when it `is_array()`
  (`src/llm/openai_adapter.cpp:452`). `null`/omitted are skipped; `[]` iterates
  zero. Already satisfied.
- **Inbound `arguments`**: read only when it `is_string()`
  (`src/llm/openai_adapter.cpp:475-478`); otherwise `arguments` stays empty.
  `""`/whitespace then becomes `{}` in the assembler
  (`src/llm/tool_call_assembler.cpp:94-96`). Already satisfied for `""`/`null`/
  absent.
- **Gaps (not satisfied):**
  - `arguments` as a JSON **object** (a non-strict server) is silently ignored
    → `{}` (data loss), not an error (`src/llm/openai_adapter.cpp:475-478`).
  - `arguments` as the string `"null"`/`"[]"`/`"42"`/`"\"x\""` parses to a
    non-object and raises `MalformedToolCall`
    (`src/llm/tool_call_assembler.cpp:98-106`).
  - **Echo-back**: `map_message` emits `block.arguments.dump()`
    (`src/llm/openai_adapter.cpp:280`); a non-object `arguments` (e.g. `null`)
    dumps to `null`, which strict backends reject with 400.

### 9.2 Decision (47-D6)

Under `profile.normalize_tool_arguments`, normalization is split by layer
(47-O-H1). The **rule** is: the logical argument value is a JSON object; a
non-object (including `""`/`null`/`"[]"`/`"42"`) becomes `{}`.

- **Whole-object delta** (`handle_tool_call`,
  `src/llm/openai_adapter.cpp:462-505`): if `arguments` is a JSON **object**,
  serialize it once with `.dump()` and feed that single string as one `onDelta`
  (`:497-498`). This is safe under streaming because an object value is never
  fragmented — it arrives whole in one delta. If `arguments` is a string, feed
  the fragment **unchanged** as today; **do not parse it per delta**. If it is
  any other type or absent, feed nothing (the assembler's blank→`{}` rule
  covers it).
- **Finalize** (`ToolCallAssembler::onFinished`,
  `src/llm/tool_call_assembler.cpp:78-109`): the assembler gains a normalization
  policy passed at construction (from the resolved profile). After the
  accumulated fragments are concatenated, it parses the **complete** string
  once: an object is used; under normalization a non-object or an unparseable
  value becomes `nlohmann::json::object()` (`{}`); without normalization the
  shipped errors are unchanged. The same policy is applied in `take_ordered`
  (`:111-136`) for consistency. **This is the layer fix**: Rev 2's rule parsed
  each streamed delta inside `handle_tool_call`, so a normal fragment stream
  (`{"pa`, `th":…`) was replaced by `{}` at every delta and the accumulator
  became `"{}{}…"` → `MalformedToolCall` on every streamed call (47-O-H1).
- **Echo-back** (`map_message`, `src/llm/openai_adapter.cpp:270-291`): emit
  `"{}"` when `block.arguments` is not an object; otherwise emit
  `block.arguments.dump()` as today (string-form). Rev 2's Atem mapping-form
  echo-back is removed with the ATEM rendering path (§8.3.6): no profile emits
  mapping-form arguments, and `build_chat_completions_body`/`map_message` are
  unchanged (47-I1).

Without the profile, both paths are byte-for-byte today's behaviour (47-I1).
The already-satisfied cases (§9.1) are unchanged in both postures and are
regression-pinned, not re-implemented.

**`FakeLLM` does not exercise this path.** `FakeLLM` is a direct `LLMProvider`
that emits `ToolCallStarted`/`ToolCallDelta`/`ToolCallFinished` itself
(`include/ymh/llm/fake_llm.hpp:47`, `src/llm/fake_llm.cpp:170-185`) and never
enters `handle_tool_call` or the assembler; a `FakeLLM` test cannot fail on a
normalization bug (47-O-H1/M5). The D6 tests must drive
`OpenAICompatibleProvider` with a fake `HttpTransport` and stream a JSON object
argument in ≥3 fragments (§18.5).

### 9.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F10 | A server sends `arguments` as an object, or as a string that parses to a non-object | Arguments silently lost (`{}`) or `MalformedToolCall` | Whole-object delta dumps once; finalize normalizes a non-object/ unparseable value to `{}` under the profile (§9.2) |
| 47-F11 | A server sends `arguments: null` on echo-back | A strict backend 400s | Emit `"{}"` (profiled) |
| 47-F25 | Normalization masks a genuinely malformed call | A call runs with `{}` | Accepted for non-object input; an object that fails to assemble still errors |
| 47-F42 | Rev 2's per-delta normalization is reintroduced | A normal fragment stream is corrupted to `"{}{}…"` → `MalformedToolCall` on every streamed call | Normalization lives only at `handle_tool_call`'s whole-object case and the assembler's finalize; a streamed-fragment adapter test fails otherwise (47-O-H1, §18.5) |

---

## 10. D7 — Config surface for sampling and tool choice

### 10.1 Current state (verified)

`apply_llm` accepts, at both the flat `llm` level and the nested `llm.default`
level: `provider`, `base_url`, `model`, `api_key_env`, `api_key`, `max_tokens`,
`reasoning_effort`, `max_concurrency`, `connect_timeout_ms`,
`idle_timeout_ms`, `request_timeout_ms`, `retry`
(`src/config/config.cpp:388-392`, `:399-403`). `max_tokens` and
`reasoning_effort` are read at `:417-434`. `LlmSettings` mirrors these
(`include/ymh/config/config.hpp:121-141`).

**The genuinely missing keys are `profile`, `temperature`, `top_p`, `top_k`,
`tool_choice`, `stop`, `seed`** (the plan's claim that only
`agent.reasoning_effort` is settable is corrected in §2.3).

### 10.2 Decision (47-D7)

**10.2.1 Keys and types.** Added to `LlmSettings` and to **both** allowed-key
lists (`src/config/config.cpp:388-392` and `:399-403`):

| Key | Type | Validation |
|---|---|---|
| `profile` | string | `""` inert; non-empty must resolve (47-D1.3) |
| `temperature` | number | finite, `[0.0, 2.0]` |
| `top_p` | number | finite, `(0.0, 1.0]` |
| `top_k` | non-negative integer | `0` is mapped to `nullopt` **at read time** (omitted) |
| `tool_choice` | string | non-empty; `auto`/`none`/`required` or a function name |
| `stop` | array of strings | profile denylist check (47-D3) |
| `seed` | non-negative integer | `[0, 2^32)` |

New read helpers follow the existing pattern: `read_optional_double`
(analogous to `read_optional_string`, `src/config/config.cpp:180-192`) and a
`member`+range-checked read for `top_k`/`seed` (analogous to `max_tokens`,
`:417-423`); `stop` uses `read_string_array` (`:194-214`).

**`top_k: 0` means omit (47-O-L2, pinned).** Rev 2 said "`0` = omit" but pinned
the body builder as `if (parameters.top_k.has_value()) body["top_k"] = …`
(§11.2 item 6), so a configured `0` would have reached the wire as `"top_k": 0`.
Rev 4 maps a configured `0` to `nullopt` **in `apply_llm` at read time**, so a
`0` never enters `GenerationParameters` or `LlmCallConfig` and the body-builder
rule stays `has_value()`-based. This keeps `call_config_equals` consistent (no
`0`-vs-`nullopt` mismatch).

**Layer-preservation (pinned).** `read_string_array` returns an empty vector
when the key is absent (`src/config/config.cpp:194-214`), so `stop` must be
assigned **only when the key is present**, falling back to the accumulated
`config.llm.stop` (47-F20). `profile` is read with the same accumulated-config
fallback as `provider`/`base_url`/`model` (`:426-432`), which is what makes the
D3 cross-layer check effective (§6.2.2).

**10.2.2 Precedence (pinned).**

- `llm.default.<k>` **shadows** the flat `llm.<k>` for all reads: when the
  `default` object is present, `section = nested` and only the nested object is
  read (`src/config/config.cpp:394-405`). The two are **not merged**; a flat key
  is ignored when `default` exists. The new keys follow this exactly.
- Explicit config > profile sampling default > omit (47-D1.4, 47-I8).
- `agent.reasoning_effort` > `llm.reasoning_effort` (existing,
  `src/cli/wiring.cpp:204-208`); the new sampling keys have no `agent.*`
  counterpart and are sourced solely from `llm.default`.
- The environment override layer (`apply_env_overrides`,
  `src/config/config.cpp:1237-1307`) is unchanged; no new `YMH_*` variable is
  introduced by this spec.

**10.2.3 Mapping.** `to_agent_config` (`src/cli/wiring.cpp:193-213`) copies
`config.llm.{temperature,top_p,top_k,tool_choice,stop,seed}` into
`agent.parameters` (`GenerationParameters`), applying the 47-D1.4 default only
when unset. `buildRequest` then copies them into `LlmCallConfig`
(`src/agent/agent_loop.cpp:534-543`).

**10.2.4 First-run scaffold (L4).** The scaffold template `kDefaultConfigJsonc`
(`src/config/config.cpp:963-1021`) is the user-facing first-run example. It is
updated in this commit: the new keys are added as commented-out optional entries
under `llm.default`, and the two existing `// "low" | "medium" | "high"`
comments (`:984`, `:1006`) gain `"xhigh"` (47-D10). The scaffold keeps loading
to the built-in defaults (all new keys commented out), so the strict loader
still accepts it. Pinned by `MuseD7_ScaffoldMentionsNewKeys` (§18.6), which
loads the scaffold text and asserts each new key and `"xhigh"` appear.

**The `profile` example must not name Muse (47-O-L1).** The scaffold lives in
`src/config/config.cpp`, which is **not** excluded by the
`MuseProfile_MuseNamesConfinedToProfileTable` grep (47-I6). A natural example
such as `// "profile": "muse-glimmer"` would make that test fail. The pinned
example is therefore the **non-Muse placeholder** `// "profile": ""` (empty =
inert); the Muse ids are documented only in `src/llm/model_profile.cpp`. The
scaffold test also asserts the literal `muse` does **not** appear in the
scaffold text.

### 10.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F26 | A typo'd sampling key | It is silently ignored | Strict loader: unknown key = `ConfigError` |
| 47-F27 | `temperature` out of range | A backend 400 | `ConfigError` at load |
| 47-F28 | A flat `llm.temperature` alongside `llm.default` | The flat key is ignored | Documented shadowing (§10.2.2) |

---

## 11. D8 — `top_k` end-to-end

### 11.1 Current state (verified)

`top_k` does not exist in any type: not in `GenerationParameters`
(`include/ymh/llm/llm_request.hpp:41-49`), not in `LlmCallConfig`
(`include/ymh/llm/llm_call_config.hpp:32-45`), not in the body builder
(`src/llm/openai_adapter.cpp:732-753`), not in `call_config_equals`
(`src/llm/llm_runtime.cpp:127-133`). The other extensions (`top_p`, `seed`,
`tool_choice`) exist and are wired; `top_k` is the missing one.

### 11.2 Decision (47-D8)

Add `top_k` through the full chain:

1. `GenerationParameters` — `std::optional<std::uint32_t> top_k`
   (`include/ymh/llm/llm_request.hpp:41-49`).
2. `LlmCallConfig` — same field (`include/ymh/llm/llm_call_config.hpp:32-45`).
3. `to_json`/`from_json` — emit/decode `top_k` when set
   (`include/ymh/llm/llm_call_config.hpp:83-133`). Additive: an old persisted
   `LlmRequestHeader` without `top_k` decodes to `nullopt` (47-I10).
4. `call_config_equals` — add `left.top_k == right.top_k`
   (`src/llm/llm_runtime.cpp:127-133`). This makes a `top_k` mismatch a
   `PreparedCallError::ConfigMismatch` (`src/llm/llm_runtime.cpp:180-184`).
5. `buildRequest` — `config.top_k = config_.parameters.top_k` alongside the
   existing `top_p`/`seed`/`tool_choice` copies (`src/agent/agent_loop.cpp:
   539-543`). (The signature also gains `turn_step` for 47-D2, §5.2/§15; the
   `top_k` copy is independent of it.)
6. `build_chat_completions_body` — `if (parameters.top_k.has_value())
   body["top_k"] = *parameters.top_k` (`src/llm/openai_adapter.cpp:732-753`).
   Because `apply_llm` maps a configured `0` to `nullopt` at read time
   (§10.2.1), `has_value()` is the exact "emit" condition and `"top_k": 0`
   never reaches the wire (47-O-L2).
7. `template_json`/`canonical_json` pick it up automatically because they
   serialize `config` (`src/llm/llm_runtime.cpp:36`, `:162-166`).

`top_k` is an OpenAI-*non-standard* extension; under the profile the Muse
default is `64` (finding E). Without a profile it is absent unless explicitly
configured (47-I1). A backend that rejects `top_k` surfaces a 400 →
`BadRequest` (non-retryable); the remedy is to unset the key or the profile
default (47-F14).

### 11.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F14 | A backend rejects `top_k` | 400 `BadRequest` | Unset `llm.default.top_k` / profile default |
| 47-F29 | `top_k` set on the frozen config but not the prepared config | `ConfigMismatch` | Set both via `buildRequest` (47-I7) |
| 47-F30 | An old persisted request lacks `top_k` | Decode failure | `from_json` defaults to `nullopt` (47-I10) |

---

## 12. D9 — Per-provider capabilities

### 12.1 Current state (verified)

- `ProviderCapabilities` (`include/ymh/llm/llm_provider.hpp:29-37`) is passed to
  the adapter constructor (`include/ymh/llm/openai_adapter.hpp:74-76`,
  `src/llm/openai_adapter.cpp:840-845`) and consulted for the reasoning guard
  (`:899-902`), the tool-calls guard (`:895-898`), `usage_streaming` (`:728`) and
  reasoning parsing (`:440`).
- The value is a single fixed function, `openai_compatible_capabilities()`
  (`include/ymh/llm/provider_registry.hpp:72-75`, `src/llm/provider_registry.cpp:
  91-100`), hard-coded into the factory (`:102-111`, the call at `:108`). It
  cannot be declared per provider or per model.

### 12.2 Decision (47-D9)

Make capabilities declarable through the profile and carried on the provider
config:

- `LLMProviderConfig` (`include/ymh/llm/provider_registry.hpp:26-41`) gains
  `ModelProfile profile;` (inert by default).
- The factory computes `ProviderCapabilities caps = openai_compatible_
  capabilities(); apply_profile_capabilities(caps, config.profile);` before
  constructing the adapter (`src/llm/provider_registry.cpp:102-111`).
- `apply_profile_capabilities(ProviderCapabilities&, const ModelProfile&)`
  overlays each **declared** flag (`ProfileCapabilities`, §15): `nullopt` keeps
  the base value; `true`/`false` overrides it. The overlay is total and
  additive (47-I9).
- The Muse profile declares `streaming=true`, `tool_calls=true`,
  **`parallel_tool_calls=true`**, `reasoning=true`, `usage_streaming=true`,
  `prompt_caching=true`.
- `ModelInfo::max_context_tokens` remains `0` (unknown)
  (`src/llm/openai_adapter.cpp:855-860`); context sizing is out of scope
  (§1.4).

**12.2.0 D9 is generic future-proofing, and a behavioural no-op for Muse
today (M8).** `openai_compatible_capabilities()` already sets exactly those six
flags to `true` (`src/llm/provider_registry.cpp:91-100`), so the Muse overlay
changes nothing: D9's value is the *seam* (a provider/model can now declare
capabilities through the profile), not a Muse capability delta. This is stated
explicitly rather than implied. Consequently
`MuseD9_MuseProfileDeclaresReasoningAndParallel` as named in Rev 1 was
**vacuous** — it would pass even if `apply_profile_capabilities` were a stub,
because the base already has both flags. Rev 2 replaces it with
`MuseD9_OverlayOverridesSyntheticBase` (§18.7), which builds a base with
`parallel_tool_calls=false` and `reasoning=false`, applies the Muse profile, and
asserts both become `true`; that test fails on a stub overlay. D9 is retained
(not dropped) because the reversibility contract and the per-model capability
declaration need one place to declare per-model truthfulness (47-F31).

**12.2.1 The `parallel_tool_calls=true` nuance.** The prompting guide says Muse
"supports one tool call per turn"; the GGUF card warns that stopping on
`<|eom|>` "collapses parallel tool calling", and the vLLM guide says "one call
per message; read consecutive assistant messages". The truthful reading is:
Muse emits **one call per channel message** and a turn may contain several such
messages; a translating server may aggregate them into one `tool_calls` array
or expose consecutive assistant messages. ymh's adapter assembles calls by
`index` (`src/llm/openai_adapter.cpp:469`, `:481-495`), so a `tool_calls` array
of any length survives. The profile therefore declares
`parallel_tool_calls=true` for the ymh capability, and 47-I11 pins that ymh's
loop already executes N calls (`src/agent/agent_loop.cpp:1149`, `:1177-1184`).
A server that returns only the first of several calls is 47-F9, a live-check
item (§8.3.5).

### 12.3 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F9 | A server returns only the first of N calls | Later calls are lost | Live check (§8.3.5); D3 keeps the turn alive |
| 47-F31 | A profile declares a capability the endpoint lacks | A guard passes that should fail | Capabilities are truthful by contract (`include/ymh/llm/llm_provider.hpp:27-28`) |
| 47-F32 | The overlay is not total | A declared flag is dropped | 47-I9 + a unit test |

---

## 13. D10 — `reasoning_effort` accepts `xhigh`

### 13.1 Current state (verified)

`reasoning_effort` is `std::optional<std::string>` in `LlmCallConfig`
(`include/ymh/llm/llm_call_config.hpp:35`), `GenerationParameters`
(`include/ymh/llm/llm_request.hpp:47`) and `LlmSettings`
(`include/ymh/config/config.hpp:135`). It is copied verbatim
(`src/cli/wiring.cpp:204-208`, `src/agent/agent_loop.cpp:537`) and emitted
verbatim (`src/llm/openai_adapter.cpp:748-750`). A tree-wide search finds **no
validation and no clamp** of its value.

### 13.2 Decision (47-D10)

`"xhigh"` **already passes through end-to-end — no code change is required.**
The only changes are documentary: the four comments that actually carry the
value list — `include/ymh/llm/llm_request.hpp:47`
(`// "low"|"medium"|"high"`), `include/ymh/config/config.hpp:72`
(`// "low" | "medium" | "high"`), and the scaffold template
`src/config/config.cpp:984` and `:1006` (both `// "low" | "medium" | "high"`)
— gain `"xhigh"`, and the config reference in spec 21 lists it. Rev 1 also
cited `include/ymh/llm/llm_call_config.hpp:35` and
`include/ymh/config/config.hpp:135`, but those two lines are bare field
declarations with **no comment** (M10); the citations are removed. A test pins
no-clamp: a configured `"xhigh"` reaches the wire body unchanged
(`body["reasoning_effort"] == "xhigh"`, `src/llm/openai_adapter.cpp:748-750`)
and survives `call_config_equals` (`src/llm/llm_runtime.cpp:129`).

### 13.3 Verification

The absence of a clamp is the claim; the test is the evidence. No type change,
no enum, no schema change.

### 13.4 Recorded discrepancy (decided in §20 Q1)

Muse's reasoning-length knob is the chat-template variable
`reasoning_strength` (`low|medium|high|xhigh`, default `high`), passed as
`chat_template_kwargs: {"reasoning_strength": "xhigh"}`; the GGUF card states
`"reasoning_effort": "none"` has no effect on Muse. So `reasoning_effort:
"xhigh"` may be ignored by a Muse endpoint even though it is not clamped.
**Decision (recorded, not open):** this spec does **not** add a
`chat_template_kwargs` surface — it is not one of D1–D11 and would widen the
one-commit scope; the rationale is in §20 Q1, and a Muse deployment that needs
`reasoning_strength` is a separate follow-up spec.

### 13.5 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F33 | A future edit adds a value whitelist | `xhigh` is rejected | The 47-D10 test fails |
| 47-F34 | The endpoint ignores `reasoning_effort` | Reasoning length is unchanged | §13.4 / §20 Q1 |

---

## 14. D11 — The reversibility contract

### 14.1 Current state (verified)

The repository is a single tree with no profile concept; the plan requires the
whole change to land as **one commit** that a single `git revert` removes
entirely (`.sisyphus/plans/muse-glimmer-support.md` §"Reversibility design").

### 14.2 Decision (47-D11)

Pin the contract as invariants (§16, 47-I1/47-I6/47-I12) and a procedure (§19):

- **Confined Muse identity.** Every Muse-named identifier lives only in
  `src/llm/model_profile.cpp` and the Muse test files; the leaked-call /
  native-ATEM detector is *generic* code with no Muse name, switched on by a
  profile flag. The edited files reference only generic symbols (§19.2, 47-I6).
- **Inert default.** With `llm.default.profile` absent/empty, every
  profile-gated branch is a no-op and the wire body is byte-identical to
  today's. This is **test-pinned** (a golden body test plus a full FakeLLM
  turn), not asserted in prose.
- **One commit.** All changes land together; new files are removed by revert,
  edited files are restored, and `tests/CMakeLists.txt` is restored.
- **Documented revert.** The commit message names the revert command and lists
  the new files and the confined Muse identifiers (§19.2).
- **Rehearsal.** Phase 3 runs `git revert --no-commit <sha>` → build → full
  suite green → `git revert --abort`.

### 14.3 Backward compatibility (no migration)

Adding `top_k` to `LlmCallConfig::from_json` is additive: a persisted
`LlmRequestHeader` written before this change decodes with `top_k = nullopt`
(`include/ymh/llm/llm_call_config.hpp:108-133`). No session-DB schema changes,
no registry changes, no event-shape changes (47-I10). Replaying a pre-change
request that had `tool_choice = "required"` continues to send it; replay is a
faithful reproduction, not a behaviour change.

### 14.4 Failure modes

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 47-F16 | A future commit introduces a Muse-named identifier outside the profile table | Revert hygiene erodes; the name outlives the feature | `MuseProfile_MuseNamesConfinedToProfileTable` grep test (47-I6) + review |
| 47-F35 | A revert leaves a stale config key | `ConfigError` on a pre-Muse config | New keys are additive; a pre-Muse config has none of them |
| 47-F36 | A revert fails to build | The feature is not removable | Phase-3 rehearsal |

---

## 15. C++ interface sketches (pinned)

Only new/changed symbols are shown; unchanged members are elided with `…`.
Every sketch pins the headers it needs.

```cpp
// ── include/ymh/llm/model_profile.hpp  (new; Muse-only values live in the .cpp)
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace ymh {

// 47-D9: per-profile capability declaration. `nullopt` keeps the base
// (openai_compatible_capabilities()); true/false overrides it.
struct ProfileCapabilities {
    std::optional<bool> streaming;
    std::optional<bool> tool_calls;
    std::optional<bool> parallel_tool_calls;
    std::optional<bool> reasoning;
    std::optional<bool> usage_streaming;
    std::optional<bool> prompt_caching;
};

// 47-D1: the profile seam. The type is generic; the built-in Muse values are
// defined in src/llm/model_profile.cpp only.
struct ModelProfile {
    std::string                  id;                        // "" = inert
    bool                         force_first_tool_call = false;   // 47-D2
    bool                         normalize_tool_arguments = false; // 47-D6
    bool                         detect_leaked_tool_calls = false; // 47-D4 (G-JSON)
    bool                         detect_atem_tool_calls = false;   // 47-D5 (G-ATEM detect only)
    std::vector<std::string>     forbidden_stop_tokens;     // 47-D3
    ProfileCapabilities          capabilities;              // 47-D9
    std::optional<double>        temperature;               // 47-D1.4
    std::optional<double>        top_p;                     // 47-D1.4
    std::optional<std::uint32_t> top_k;                     // 47-D1.4
};

// nullptr for "" and unknown ids; total and noexcept (47-I2).
[[nodiscard]] const ModelProfile* find_model_profile(std::string_view id) noexcept;
[[nodiscard]] bool is_known_model_profile(std::string_view id) noexcept;

} // namespace ymh
```

```cpp
// ── include/ymh/config/config.hpp  (changed)
namespace ymh {
struct LlmSettings {
    …
    std::string                  profile;             // 47-D1  (default "")
    std::optional<double>        temperature;         // 47-D7
    std::optional<double>        top_p;               // 47-D7
    std::optional<std::uint32_t> top_k;               // 47-D7/D8
    std::optional<std::string>   tool_choice;         // 47-D7
    std::vector<std::string>     stop;                // 47-D7
    std::optional<std::uint32_t> seed;                // 47-D7
};
} // namespace ymh
```

```cpp
// ── include/ymh/llm/llm_request.hpp  (changed)
namespace ymh {
struct GenerationParameters {
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;             // 47-D8 (new)
    std::optional<std::uint32_t> max_output_tokens;
    std::vector<std::string>     stop;
    std::optional<std::string>   tool_choice;
    std::optional<std::string>   reasoning_effort;  // "low"|"medium"|"high"|"xhigh" (47-D10)
    std::optional<std::uint32_t> seed;
};
} // namespace ymh
```

```cpp
// ── include/ymh/llm/llm_call_config.hpp  (changed)
namespace ymh {
struct LlmCallConfig {
    ProviderId                   provider;
    ModelId                      model;
    std::optional<std::string>   reasoning_effort;
    std::optional<double>        temperature;
    std::optional<std::uint32_t> max_tokens;
    std::vector<std::string>     stop;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;             // 47-D8 (new)
    std::optional<std::uint32_t> seed;
    std::optional<std::string>   tool_choice;
};
// to_json/from_json gain `top_k`; call_config_equals gains the top_k term.
} // namespace ymh
```

```cpp
// ── include/ymh/llm/provider_registry.hpp  (changed)
namespace ymh {
struct LLMProviderConfig {
    …
    ModelProfile profile;   // 47-D1/D9 (inert by default)
};

// 47-D9: overlay declared flags onto the base capability set.
void apply_profile_capabilities(ProviderCapabilities& capabilities,
                                const ModelProfile& profile) noexcept;
} // namespace ymh
```

```cpp
// ── include/ymh/agent/agent.hpp  (changed)
namespace ymh {
struct AgentConfig {
    ProviderId           provider;
    ModelId              model;
    GenerationParameters parameters;
    ModelProfile         profile;   // 47-D2 (inert by default)
    …
};
} // namespace ymh
```

```cpp
// ── include/ymh/llm/leaked_call_detector.hpp  (new; generic, no Muse name)
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
namespace ymh {

// 47-D4 G-JSON: scans the whole trimmed content (the whole-content rule of
// §7.2.4 is enforced by the caller: `content` is the entire trimmed content)
// for complete <tool_call>{…}</tool_call> blocks. The parser is pure: it
// reports structure only.
//  - `calls`: one entry per complete whole-content block whose outer JSON is an
//    object and whose `name` is a string. `arguments` is the raw value
//    (object/string/null/…) and is normalized or rejected by the caller.
//  - `complete_block_seen`: at least one complete block constitutes the whole
//    trimmed content (nothing but whitespace outside it).
//  - `malformed_block_seen`: a complete whole-content block could not yield a
//    LeakedCall (outer JSON not an object, or no string `name`).
// The caller applies the offered-name test and normalization (47-O-M3) and maps
// the shape onto the §7.2.3 taxonomy (47-O2-M2).
struct LeakedCall { std::string name; nlohmann::json arguments; };
struct LeakedParse {
    std::vector<LeakedCall> calls;
    bool complete_block_seen = false;
    bool malformed_block_seen = false;
};
[[nodiscard]] LeakedParse parse_leaked_json_call(std::string_view content);

// 47-D5 G-ATEM detection: complete <atem:invoke>…</atem:invoke> blocks found in
// a to=<tool> body (or bare in the trimmed content); to=self/to=user bodies are
// ignored. Names in document order. nullopt = none. NEVER converts.
[[nodiscard]] std::optional<std::vector<std::string>> detect_native_atem_calls(
    std::string_view content);
} // namespace ymh
```

```cpp
// ── include/ymh/llm/tool_call_assembler.hpp  (changed; 47-O-H1)
namespace ymh {
// Applied at finalize, after all fragments are concatenated.
enum class ToolArgumentPolicy : std::uint8_t {
    Strict,            // shipped: non-object / unparseable => MalformedToolCall
    NonObjectToEmpty,  // profiled: non-object / unparseable => {}
};
class ToolCallAssembler {
public:
    explicit ToolCallAssembler(std::size_t max_arguments_bytes,
                               ToolArgumentPolicy policy = ToolArgumentPolicy::Strict);
    …
};
} // namespace ymh
```

```cpp
// ── include/ymh/llm/openai_adapter.hpp  (changed; 47-O-M3)
#include <span>
#include "ymh/llm/model_profile.hpp"
#include "ymh/tools/tool.hpp"
namespace ymh {
// The decoder's pinned inputs. `profile == nullptr` => inert (no detection,
// no normalization). Built in OpenAICompatibleProvider::stream() from
// config_.profile and request.tools.
struct ToolCallPolicy {
    const ModelProfile*       profile = nullptr;
    std::span<const ToolName> offered;   // request.tools names
};

// Unchanged: no ATEM rendering, so no profile parameter is needed (§8.3.2).
[[nodiscard]] nlohmann::json build_chat_completions_body(
    const LLMRequest& request,
    const ProviderCapabilities& capabilities);
} // namespace ymh
```

```cpp
// ── src/llm/openai_adapter.cpp  (internal; changed)
class OpenAiStreamDecoder {
public:
    OpenAiStreamDecoder(StreamSink& sink,
                        ProviderCapabilities capabilities,
                        ToolCallPolicy policy,          // 47-O-M3
                        std::size_t max_arguments_bytes,
                        std::size_t sse_line_bytes);
    …
};
// Constructed at :935 with ToolCallPolicy{&config_.profile, offered_names};
// `offered_names` is a local std::vector<ToolName> built from request.tools.
// The assembler is constructed from the same policy:
//   assembler_(max_arguments_bytes,
//              profile && profile->normalize_tool_arguments
//                  ? ToolArgumentPolicy::NonObjectToEmpty
//                  : ToolArgumentPolicy::Strict)
```

```cpp
// ── include/ymh/agent/agent_loop.hpp  (changed; 47-D2/47-O-M4)
class AgentLoop {
    …
    // The 4th parameter is the loop-local `stepNumber` (:929); the shipped
    // 3-arg form (which receives the session-global StepId) is insufficient.
    [[nodiscard]] FrozenRequest buildRequest(const std::vector<Message>& messages,
                                             TurnId turn,
                                             StepId step,
                                             std::size_t turn_step);
};
```

```cpp
// ── src/llm/model_profile.cpp  (new; the ONLY Muse-only value table)
namespace {
constexpr std::string_view kMuseGlimmerProfileId = "muse-glimmer";
const ModelProfile kMuseGlimmerProfile{ /* force_first_tool_call, G-JSON detect,
    G-ATEM detect, normalize_tool_arguments, capability overlay, sampling
    defaults 1.0/0.95/64, forbidden control tokens */ };
} // namespace
```

---

## 16. Invariants

Numbered `47-I#`. The "Gate" column is `Y` (gates this change) or `pin`
(retained/regression behaviour, excluded from the gate).

| ID | Gate | Invariant |
|---|---|---|
| 47-I1 | Y | With `llm.default.profile` absent/empty, `find_model_profile` returns `nullptr`, every profile-gated branch is a no-op, capabilities equal `openai_compatible_capabilities()`, and the serialized request body is byte-identical to the pre-change body. Test-pinned (`MuseProfile_InertBodyGolden`, `MuseTurn_InertProfileMatchesBaseline`). |
| 47-I2 | Y | `find_model_profile` is total and `noexcept`; a non-empty `llm.default.profile` that does not resolve is a `ConfigError` at load; `""` is never an error (`src/config/config.cpp:386-451`, `src/llm/model_profile.cpp`). |
| 47-I3 | Y | `tool_choice = "required"` is sent iff `profile.force_first_tool_call` ∧ `turn_step == 1` ∧ `!request.tools.empty()` ∧ `tool_choice` is **unset**; otherwise the configured value is sent unchanged. `turn_step` is the loop-local `stepNumber` passed to `buildRequest` (not the session-global `StepId`); the predicate uses it, not the assembled `messages` (compaction-proof, §5.3). There is **no** `wire_tools_present` conjunct (47-O2-M1): the 400 hazard from `required` with no `tools` is structurally impossible because `build_chat_completions_body` takes no profile and emits `tools[]` iff `!request.tools.empty()` (`src/llm/openai_adapter.cpp:703-756`, `:714-725`), no `ModelProfile` field can omit it, and `stream()` rejects a tools-bearing request before body build when `capabilities_.tool_calls` is false (`:895-898`). The effective value is set on both `request.parameters.tool_choice` and `config.tool_choice` (`src/agent/agent_loop.cpp:522-543`, `include/ymh/agent/agent_loop.hpp:169`). |
| 47-I4 | Y | Under the profile, no `stop` entry equals a control token (`<|eom|>` = 200007 above all): config-load `ConfigError` (message interpolates the offending token) for JSONC layers, and adapter `LLMErrorCode::BadRequest` (message interpolates the profile id) on any path (`src/llm/openai_adapter.cpp:892-915`). |
| 47-I5 | Y | **G-JSON (conversion).** A **complete** leaked `<tool_call>{…}</tool_call>` block that constitutes the whole trimmed content is never treated as a final answer: offered name + object args → a structured call; outer JSON not an object, non-offered name, or an offered name with unparseable/non-normalizable args → `MalformedToolCall`. A partial/malformed fragment, or a complete block embedded in prose, is text (never executed). **One or more** complete whole-content blocks are all converted. The parser result is a `LeakedParse` (`calls` + `complete_block_seen`/`malformed_block_seen`, §15), so `finalize` can distinguish "no block" (text) from "a complete block is malformed" (`MalformedToolCall`) and can represent multiple blocks (47-O2-M2); offered-name and normalization stay in `finalize` (47-O-M3). Reasoning is never scanned. **G-ATEM (detection).** A complete native invoke in a `to=<tool>` body (or bare in trimmed content) → `MalformedToolCall`; `to=self`/`to=user` bodies are ignored; never converted, never executed (`src/llm/openai_adapter.cpp:336-360`, `:440-449`). |
| 47-I6 | Y | Every Muse-named identifier (case-insensitive `muse`, `muse-glimmer`) appears only in `src/llm/model_profile.cpp` and the Muse test files; the leaked-call/native-ATEM detector carries no Muse name; with an inert profile no profile-gated branch mutates the body (47-I1). Pinned by `MuseProfile_MuseNamesConfinedToProfileTable` (grep `include/`+`src/`, excluding `model_profile.cpp`) and `MuseProfile_InertBodyGolden`. |
| 47-I7 | Y | `top_k` is present in `LlmCallConfig` and `GenerationParameters` and reaches the wire body; a mismatch between the frozen config and the prepared config is a `ConfigMismatch` (`src/llm/llm_runtime.cpp:127-133`, `:180-184`; `src/llm/openai_adapter.cpp:732-753`). |
| 47-I8 | Y | Profile sampling defaults fill only unset values; an explicit `llm.default.<k>` always wins; a profile default never adds a key the profile does not declare (`src/cli/wiring.cpp:193-213`). |
| 47-I9 | Y | `apply_profile_capabilities` is total: every declared `ProfileCapabilities` flag overrides the base; every `nullopt` keeps it (`src/llm/provider_registry.cpp:91-111`). |
| 47-I10 | pin | Adding `top_k` is backward compatible: a persisted `LlmCallConfig` without `top_k` decodes to `nullopt`; no schema/event change (`include/ymh/llm/llm_call_config.hpp:108-133`). |
| 47-I11 | pin | ymh executes all assembled tool calls (parallel scheduling already shipped): `src/agent/agent_loop.cpp:1149`, `:1177-1184`, `:1221-1296`. |
| 47-I12 | Y | All changes are one commit; the new files and the confined Muse identifiers are enumerated in §19.2; the default profile is inert (47-I1). |
| 47-I13 | Y | Native-ATEM detection is a **presence test**, not a conversion: a complete `<atem:invoke>…</atem:invoke>` in a `to=<tool>` body (or bare in trimmed content) yields a terminal `MalformedToolCall`; `to=self`/`to=user` bodies are ignored; detection is gated on `tool_calls_.empty()` and runs after `finalize_tool_calls()`. No ATEM text is rendered and no ATEM call is executed (`src/llm/openai_adapter.cpp:336-360`). Pinned by the `MuseD5_*` tests (§18.4). |
| 47-I14 | pin | Replay does not re-run the detector: a replayed leaked turn reproduces the frozen request and durable tool events and never re-detects or re-executes (§7.2.6). Pinned by `MuseReplay_LeakedTurnDoesNotReexecute`. |
| 47-I15 | Y | `ToolCallAssembler` normalization is applied at **finalize**: with `NonObjectToEmpty`, a fully concatenated `arguments` string that parses to a non-object (or fails to parse) becomes `{}`; with `Strict` it is `MalformedToolCall` as shipped. Per-delta string fragments are never parsed (`src/llm/tool_call_assembler.cpp:78-109`, `src/llm/openai_adapter.cpp:462-505`). Pinned by `MuseD6_*` over the real adapter (47-O-H1, §18.5). |
| 47-I16 | Y | No built-in profile omits `tools[]` from the outgoing body when `request.tools` is non-empty, and **no `ModelProfile` field can** omit it: `build_chat_completions_body` takes no profile and emits `tools[]` iff `!request.tools.empty()`, and `map_message` is unchanged from the inert path (`src/llm/openai_adapter.cpp:703-756`, `:235-294`). Pinned by `MuseBodyGolden_ProfileOn` (47-O-H2/47-O2-M1). |

---

## 17. Failure modes

Prefix `47-F#` (trigger / symptom / recovery). The `§54` column maps each to
the closest architecture finding (`00-architecture.md` §54); `—` means
errata-local. Every row has a §18 test or is an explicit documented no-op.

| ID | §54 | Trigger | Symptom | Recovery |
|---|---|---|---|---|
| 47-F1 | F6 | `stop: ["<|eom|>"]` under the profile | Multi-call turns collapse | Config + adapter guard (47-I4) |
| 47-F2 | F6 | A future path sets a forbidden stop | Guard skipped | Adapter guard is authoritative (47-I4) |
| 47-F3 | F6 | Forced required with no tools offered (internal list) | Backend 400 | `!request.tools.empty()` guard (47-I3) |
| 47-F4 | F6 | Forced required on a later step (e.g. a history predicate after compaction) | Repeated calls, no answer | `turn_step == 1` predicate (47-I3) |
| 47-F5 | F6 | Override clobbers an explicit choice | User intent lost | Any configured choice (incl. `"auto"`) wins (47-I3) |
| 47-F6 | F10 | Leaked block names a non-offered tool | Hallucinated call | `MalformedToolCall` (47-I5) |
| 47-F7 | — | Leaked text retained in content | Raw syntax in the transcript | Accepted; the call still executes (§7.2.6) |
| 47-F8 | F10 | ATEM quoted in a `to=self` reasoning body | False detection | `to=self` bodies are not scanned (47-I5, §7.2.7) |
| 47-F9 | F6 | Server returns only the first of N calls | Later calls lost | Live check (§8.3.5) |
| 47-F10 | F10 | `arguments` sent as an object, or as a string that parses to a non-object | Args lost (`{}`) or `MalformedToolCall` | Whole-object dump at `handle_tool_call` + finalize normalization (47-D6/47-I15) |
| 47-F11 | F10 | `arguments: null` echoed | Strict backend 400 | Emit `"{}"` (47-D6) |
| 47-F12 | F6 | Unknown profile id | Muse behaviour silently off | `ConfigError` (47-I2) |
| 47-F13 | F6 | A non-translating endpoint emits native ATEM in content | No tool run; silently treated as prose would end the turn | `MalformedToolCall` typed error (47-I13, §8.3.3); use a translating endpoint; conversion is a new spec (§8.3.6) |
| 47-F14 | F6 | Backend rejects `top_k` | 400 | Unset the key/default |
| 47-F15 | F8 | Content exceeds the detector cap | Detection incomplete | Cap stops accumulation; stream completes |
| 47-F16 | — | Muse-named identifier outside the profile table | Revert hygiene erodes | Grep test `MuseProfile_MuseNamesConfinedToProfileTable` (47-I6) |
| 47-F17 | — | A module reads a profile flag directly | Seam leaks | Review + §19.2 |
| 47-F18 | F6 | Config and profile default disagree | Ambiguous value | Explicit wins (47-I8) |
| 47-F19 | F10 | `request.parameters` and `config` diverge | `ConfigMismatch` | Set both (47-I3) |
| 47-F20 | F6 | A workspace layer omits `stop` while the global layer set a forbidden one | Without a fallback, `read_string_array` clears the global entry instead of rejecting it | The D7 read preserves the accumulated `stop` (§10.2.1); the adapter guard backstops (47-I4) |
| 47-F21 | F10 | Detector runs before standard assembly | Double execution | Gate on `tool_calls_.empty()` |
| 47-F22 | F6 | A future profile adds a `tools[]`-omission seam while `force_first_tool_call` is on (no such seam exists in this design) | `tool_choice: "required"` with no `tools` → 400 | Structurally impossible: `build_chat_completions_body` is unchanged and no `ModelProfile` field can omit `tools[]` (47-I3/47-I16, 47-O2-M1). Rev-3 47-F41 merged here |
| 47-F23 | F8 | Leaked G-JSON args exceed the assembly cap | `MalformedToolCall` | Existing cap (`include/ymh/llm/provider_registry.hpp:39`) |
| 47-F24 | F10 | Native ATEM quoted in content | If converted, a non-call would execute | Never converted; detection ignores `to=user`/`to=self` bodies; a bare quote is a visible typed error (47-F40, §7.2.7) |
| 47-F25 | F10 | Normalization masks a malformed call | Runs with `{}` | Accepted for non-objects; object failures still error |
| 47-F26 | F6 | Typo'd sampling key | Silently ignored | Strict loader (`ConfigError`) |
| 47-F27 | F6 | Out-of-range sampling | Backend 400 | `ConfigError` at load |
| 47-F28 | F6 | Flat + nested sampling keys | Flat ignored | Documented shadowing |
| 47-F29 | F10 | `top_k` on one config only | `ConfigMismatch` | `buildRequest` sets both (47-I7) |
| 47-F30 | F10 | Old persisted request lacks `top_k` | Decode failure | `from_json` default (47-I10) |
| 47-F31 | F6 | Profile declares an absent capability | A guard passes wrongly | Truthful-capability contract |
| 47-F32 | — | Overlay not total | A declared flag dropped | 47-I9 test |
| 47-F33 | F6 | A future whitelist rejects `xhigh` | `xhigh` rejected | 47-D10 test |
| 47-F34 | F6 | Endpoint ignores `reasoning_effort` | Reasoning unchanged | §20 Q1 |
| 47-F35 | F6 | Revert leaves a stale key | Pre-Muse config errors | New keys additive |
| 47-F36 | — | Revert fails to build | Feature not removable | Phase-3 rehearsal |
| 47-F37 | F6 | The backend rejects `tool_choice: "required"` | 400 `BadRequest` (non-retryable) | Set `llm.default.tool_choice: "auto"` to opt out, or drop the profile (§5.2) |
| 47-F38 | F10 | A complete G-JSON call block is embedded in prose | If converted, a non-call would execute | Whole-content rule: text + warning, never executed (§7.2.4); a genuine embedded leak ends as a visible final answer |
| 47-F39 | F6 | Native ATEM is detected in a `to=<tool>` body (or bare) | Silently treating the native call as prose would end the turn with no tool run | `MalformedToolCall` typed error (§7.2.7, §8.3.3); never converted, never executed |
| 47-F40 | F10 | Native ATEM is quoted in a `to=user` final answer or a bare unframed quote | A false detection | `to=user` bodies are ignored; a bare unframed quote is an accepted residual (§7.2.7) |
| 47-F42 | F10 | Rev 2's per-delta normalization is reintroduced | A fragment stream is corrupted to `"{}{}…"` → `MalformedToolCall` on every streamed call | Normalization lives at the whole-object case + the assembler's finalize (47-I15); a streamed-fragment adapter test fails otherwise (47-O-H1) |

---

## 18. Test plan

Tests marked **(new)** fail on the pre-change tree; **(pin)** are
retained-behaviour regression pins (excluded from the gate).

### 18.1 Unit — profile (D1, D11)

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseProfile_LookupEmptyIsNull` | 47-I1/I2 | new |
| `MuseProfile_LookupUnknownIsNull` | 47-I2 | new |
| `MuseProfile_KnownIdsResolve` | 47-D1/D5 | new |
| `MuseProfile_ConfigUnknownIdIsError` | 47-I2, 47-F12 | new |
| `MuseProfile_ConfigAbsentIsInert` | 47-I1 | new |
| `MuseProfile_InertBodyGolden` | 47-I1 (byte-identical body) | new |
| `MuseProfile_InertCanonicalJsonGolden` | 47-I1, 47-I14 (replay digest) | new |
| `MuseProfile_MuseNamesConfinedToProfileTable` | 47-I6, 47-F16 | new (grep `include/`+`src/`, excluding `model_profile.cpp`) |
| `MuseProfile_ScaffoldHasNoMuseName` | 47-I6, 47-O-L1 | new (the scaffold text contains no `muse`; the `profile` example is `""`) |
| `MuseProfile_SamplingDefaultsFillUnset` | 47-I8, 47-F18 | new |
| `MuseProfile_ExplicitBeatsProfileDefault` | 47-I8 | new |

### 18.2 Unit — forced first tool call (D2)

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseD2_FirstStepForcesRequired` | 47-I3 | new |
| `MuseD2_SecondStepDoesNotForce` | 47-I3 | new |
| `MuseD2_ForceAfterCompactionStaysOff` | 47-I3/§5.3, 47-F4 | new (step ≥ 2 with the ToolUse message compacted away) |
| `MuseD2_NoToolsDoesNotForce` | 47-I3, 47-F3 | new |
| `MuseD2_ExplicitNonePreserved` | 47-I3, 47-F5 | new |
| `MuseD2_ExplicitRequiredPreserved` | 47-I3 | new |
| `MuseD2_ExplicitAutoIsOptOut` | 47-I3, 47-F37 | new (configured `"auto"` is not overridden) |
| `MuseD2_OverrideInBothParametersAndConfig` | 47-I3, 47-F19 | new |
| `MuseD2_TurnLocalStepNotSessionStep` | 47-I3/§5.2, 47-O-M4 | new (turn 2 step 1 forces `required` even though the session step counter is > 1) |
| `MuseD2_NoProfileLeavesChoiceUnset` | 47-I1 | pin |
| `MuseD2_RetryReplaysFrozenChoice` | 47-I3/§5.5 | new |

### 18.3 Unit — stop guard (D3)

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseD3_ConfigEomStopIsError` | 47-I4, 47-F1 | new |
| `MuseD3_ConfigErrorNamesOffendingToken` | 47-I4, 47-F1/M12 | new (the message contains the actual token, e.g. `<|eot|>`) |
| `MuseD3_CrossLayerForbiddenStopIsCaught` | 47-I4, 47-F20 | new (global profile + workspace `stop`) |
| `MuseD3_AdapterRejectsMergedForbiddenStop` | 47-I4, 47-F2/F20 | new |
| `MuseD3_AllowedStopPasses` | 47-I4 | new |
| `MuseD3_NoProfileStopUnchanged` | 47-I1 | pin |
| `MuseD3_DenylistIsTheSixControlTokens` | 47-D3, §1.5 | new (behavioural: the profile's `forbidden_stop_tokens` equals the six control-token strings; `<|eot|>` is rejected as a client stop, while the id documentation is a comment) |

### 18.4 Unit — leaked-call detection and native-ATEM detection (D4, D5)

Pure-detector unit tests (the parse functions report structure only — a
`LeakedParse` for G-JSON, names for G-ATEM; offered-name/normalization/error and
event-emission outcomes are produced in `finalize` and are asserted in the
adapter-level block below, 47-O2-L3):

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseD4_WholeContentLeakedJsonIsLifted` | 47-I5, 47-F6 | new (pure: one complete block → `calls.size() == 1` and `complete_block_seen`; `finalize` converts it) |
| `MuseD4_TwoBlocksAreBothConverted` | 47-I5, 47-O2-M2 | new (pure: two complete whole-content blocks → `calls.size() == 2`; not expressible under the Rev-3 single-optional shape) |
| `MuseD4_OuterJsonNotObjectIsError` | 47-I5, 47-O-L3 | new (pure: `<tool_call>[1,2]</tool_call>` → `complete_block_seen && malformed_block_seen` with `calls.empty()`; `finalize` raises `MalformedToolCall`) |
| `MuseD4_CompleteMalformedBlockIsNotText` | 47-I5, 47-O2-M2 | new (pure: `<tool_call>not-json</tool_call>` → `malformed_block_seen`; distinguishable from `MuseD4_PartialFragmentIsText`) |
| `MuseD4_EmbeddedBlockInProseIsText` | 47-I5, 47-F38 | new (pure: a quoted block in prose → `!complete_block_seen`; a warning is logged) |
| `MuseD4_LeakedUnknownToolIsError` | 47-I5, 47-F6 | new (pure: `calls[0].name` is returned; `finalize` rejects the non-offered name) |
| `MuseD4_PartialFragmentIsText` | 47-I5 | new (pure: open delimiter without close → `!complete_block_seen`) |
| `MuseD4_NoDoubleExecutionWhenStructuredCallsPresent` | 47-F21 | new (adapter gate: `finalize` does not scan when `tool_calls_` is non-empty) |
| `MuseD4_DetectorCapBoundsMemory` | 47-F15 | new |
| `MuseD5_NativeAtemInToolChannelIsDetected` | 47-I13, 47-F39 | new (pure: the realistic three-message turn — `to=self`, `to=<tool>`, `to=user` — returns the tool-channel invoke name(s); the terminal `MalformedToolCall` is asserted by `MuseAdapter_NativeAtemInStreamedContentErrors`) |
| `MuseD5_WrapperlessInvokeIsDetected` | 47-I13, §8.2.3/M4 | new (pure: the `<atem:function_calls>` wrapper is optional) |
| `MuseD5_BareInvokeIsDetected` | 47-I13 | new (pure: no framing; a bare complete invoke returns a name) |
| `MuseD5_AtemInReasoningBodyIsIgnored` | 47-I5, 47-F8 | new (pure: `to=self` invoke → `nullopt`) |
| `MuseD5_AtemInFinalAnswerIsIgnored` | 47-F40 | new (pure: `to=user` invoke → `nullopt`) |
| `MuseD5_IncompleteInvokeIsText` | 47-I13 | new (pure: open without close → `nullopt`) |
| `MuseD5_DetectionDisabledByFlag` | 47-I1 | new (inert profile: native ATEM in content is plain text) |
| `MuseReplay_LeakedTurnDoesNotReexecute` | 47-I14, 47-F7 | pin |

Adapter-level (real `OpenAICompatibleProvider` + fake `HttpTransport`,
`tests/unit/openai_adapter_test.cpp:127`; 47-O-M5):

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseAdapter_LeakedJsonInStreamedContentIsLifted` | 47-I5, 47-O-M5 | new (SSE content deltas carrying `<tool_call>{…}</tool_call>` → one structured `ToolCallFinished`) |
| `MuseAdapter_TwoLeakedBlocksBothExecute` | 47-I5, 47-O2-M2 | new (SSE content carrying two complete `<tool_call>…</tool_call>` blocks → two structured `ToolCallFinished`) |
| `MuseAdapter_OfferedNameUnparseableArgsIsError` | 47-I5, 47-F6/M3 | new (an offered name whose `arguments` is an unparseable string → terminal `MalformedToolCall`, not text; moved from the pure-detector table, 47-O2-L3) |
| `MuseAdapter_NativeAtemInStreamedContentErrors` | 47-I13, 47-O-M5 | new (SSE content deltas carrying a framed native invoke → terminal `MalformedToolCall`) |
| `MuseAdapter_NativeAtemEmitsNoToolCallEvents` | 47-I13, 47-F39 | new (a detected native invoke emits no `ToolCallStarted`/`ToolCallDelta`/`ToolCallFinished`; moved from the pure-detector table, 47-O2-L3) |
| `MuseAdapter_OfferedNameCheckUsesRequestTools` | 47-O-M3, 47-I5 | new (a leaked G-JSON block naming an offered tool converts; the same block naming an un-offered tool errors — the decoder's `ToolCallPolicy.offered` is populated from `request.tools`) |

### 18.5 Unit — normalization and top_k (D6, D8)

Pure-assembler / config unit tests:

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseD6_FinalizeNonObjectStringBecomesEmpty` | 47-I15, 47-D6, 47-F10 | new (a complete string `"[]"`/`"null"`/`"42"` → `{}` under `NonObjectToEmpty`) |
| `MuseD6_FinalizeUnparseableBecomesEmpty` | 47-I15, 47-D6 | new (under the policy; `Strict` still errors) |
| `MuseD6_StrictPolicyUnchanged` | 47-I15, 47-I1 | pin (no profile → shipped `MalformedToolCall`) |
| `MuseD6_ObjectArgumentsArePreserved` | 47-D6 | new |
| `MuseD6_EchoBackNullArgumentsIsEmptyObject` | 47-D6, 47-F11 | new |
| `MuseD6_NoProfileNonObjectStillErrors` | 47-I1 | pin |
| `MuseD8_TopKReachesBody` | 47-I7 | new |
| `MuseD8_TopKZeroIsOmitted` | 47-I7, 47-O-L2 | new (configured `0` → `nullopt` → no `top_k` key) |
| `MuseD8_TopKMismatchIsConfigMismatch` | 47-I7, 47-F29 | new |
| `MuseD8_TopKInCanonicalJson` | 47-I7 | new |
| `MuseD8_OldConfigDecodesWithoutTopK` | 47-I10, 47-F30 | new |

Adapter-level (real `OpenAICompatibleProvider` + fake `HttpTransport`;
47-O-H1/M5):

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseAdapter_ObjectArgumentsWholeDeltaIsNormalized` | 47-I15, 47-F10 | new (one delta whose `arguments` is a JSON object → assembled as that object; the shipped `handle_tool_call` ignores a non-string `arguments`, so this fails pre-change — H1's fail-first test) |
| `MuseAdapter_StreamedFragmentsAreNotCorrupted` | 47-I15, 47-F42, 47-O-H1 | pin (regression guard: a JSON object argument streamed in ≥3 `arguments` string fragments assembles intact. It **passes on the pre-change tree**, so it is a regression pin, not a fail-first test; H1's fail-first tests are `MuseAdapter_ObjectArgumentsWholeDeltaIsNormalized` and `MuseAdapter_NonObjectStringNormalizedAtFinalize`) |
| `MuseAdapter_NonObjectStringNormalizedAtFinalize` | 47-I15, 47-D6 | new (`"[]"` streamed as fragments → `{}`; the shipped `Strict` policy raises `MalformedToolCall`, so this fails pre-change — H1's fail-first test) |

### 18.6 Unit — config surface (D7, D10)

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseD7_AllSamplingKeysAccepted` | 47-D7 | new |
| `MuseD7_UnknownSamplingKeyIsError` | 47-F26 | new |
| `MuseD7_TemperatureRangeIsError` | 47-F27 | new |
| `MuseD7_NestedShadowsFlat` | 47-F28 | new |
| `MuseD7_KeysMapToParameters` | 47-D7 | new |
| `MuseD7_StopSurvivesLayerWithoutKey` | 47-F20, §10.2.1 | new (workspace layer omitting `stop` does not clear the global value) |
| `MuseD7_ScaffoldMentionsNewKeys` | 47-D7, 47-D10, 47-L4 | new (loads `kDefaultConfigJsonc`; asserts the new keys and `"xhigh"` appear) |
| `MuseD10_XhighReachesBodyUnclamped` | 47-D10, 47-F33 | new |

### 18.7 Unit — capabilities (D9)

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseD9_OverlayDeclaredFlags` | 47-I9 | new |
| `MuseD9_OverlayKeepsBaseForNullopt` | 47-I9, 47-F32 | new |
| `MuseD9_OverlayOverridesSyntheticBase` | 47-D9, 47-F32/M8 | new (base with `parallel_tool_calls=false`/`reasoning=false` → overlay sets both true; fails on a stub) |
| `MuseD9_NoProfileUsesFixedCapabilities` | 47-I1 | pin |

### 18.8 Integration (FakeLLM)

`FakeLLM` is used **only** for loop-level behaviour (D1/D2). It is a direct
`LLMProvider` that emits its own `ToolCall*` events
(`src/llm/fake_llm.cpp:170-185`) and never enters `OpenAiStreamDecoder` or
`build_chat_completions_body`, so it cannot test D4/D5/D6 (47-O-M5). Those tests
are the adapter-level tests in §18.4/§18.5.

| Test | Covers | Non-vacuity |
|---|---|---|
| `MuseTurn_ForcedFirstCallThenAnswer` | 47-D2 end-to-end | new |
| `MuseTurn_ParallelCallsAllExecute` | 47-I11, 47-F9 | pin (shipped behaviour) |
| `MuseTurn_InertProfileMatchesBaseline` | 47-I1 | new |

### 18.9 Golden

| Test | Covers |
|---|---|
| `MuseBodyGolden_ProfileOff` | 47-I1 (byte-identical body, committed fixture) |
| `MuseBodyGolden_ProfileOn` | 47-D7/D8/47-I16 (sampling + top_k + tool_choice; `tools[]` still emitted) |
| `MuseCanonicalJsonGolden_ProfileOff` | 47-I1/47-I14 (the `call_config_equals`/replay digest is unchanged with no profile) |

### 18.10 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- **Default-profile regression:** the existing live suite against real DeepSeek
  with no profile set must be unchanged — the strongest evidence for 47-I1.
- **Muse live (if the endpoint is reachable from the user's machine):** a
  scripted check that (a) a standard `tool_calls` response is emitted and
  executed, (b) a multi-call turn survives (D3), (c) a leaked `<tool_call>` JSON
  turn is converted (D4), and (d) a native-ATEM turn (if the endpoint does not
  translate) surfaces the typed error (D5). If unreachable, the exact command is
  recorded as a verification gap (§8.3.5).

---

## 19. Reversibility contract

### 19.1 The commit

All changes land as **one commit**. The commit message names the revert command,
lists the new files, and lists the Muse-named identifiers:

```text
git revert <sha>   # restores the pre-Muse tree
```

### 19.2 The revert list: new files, confined Muse identifiers, edited seam

**New files (removed by revert).** These carry all new code; none is referenced
by a file outside this commit.

- `include/ymh/llm/model_profile.hpp` — the generic `ModelProfile` and
  `ProfileCapabilities` types and the two lookups. The *type* is generic (no
  Muse name); it is the seam through which a profile acts.
- `src/llm/model_profile.cpp` — **the only file that contains a Muse-named
  identifier**: `kMuseGlimmerProfileId` (`"muse-glimmer"`) and
  `kMuseGlimmerProfile`. (Rev 2's `kMuseGlimmerAtemProfileId` /
  `kMuseGlimmerAtemProfile` are removed with the ATEM profile, §8.3.1.)
- `include/ymh/llm/leaked_call_detector.hpp` + `src/llm/leaked_call_detector.cpp`
  — the **generic** G-JSON converter and native-ATEM detector
  (`parse_leaked_json_call`, `detect_native_atem_calls`, `LeakedCall`,
  `LeakedParse`). No Muse
  name. (Rev 2's `atem_codec.hpp`/`.cpp`, `render_atem_tool_declarations`,
  `parse_atem_calls`, `AtemCall` are **removed**.)
- `tests/unit/muse_*_test.cpp` (the Muse test files) and
  `tests/fixtures/muse_*` (the golden fixtures) — the only test files that may
  contain Muse names. The Rev 2 `muse_atem_declarations.golden` fixture is
  removed.

**Confined Muse identifiers.** Every identifier or string literal naming Muse
(case-insensitive `muse`, `muse-glimmer`) lives only in
`src/llm/model_profile.cpp` and the Muse test files. The detector is generic;
the adapter and loop read only generic `ModelProfile` fields/flags. This is the
corrected form of Rev 1's un-implementable "Muse-only symbol" invariant (H1):
it is a property of *names*, and it is checkable by a grep test.

**Edited files (restored by revert).** Each gains only **profile-gated generic
branches** and references only generic symbols (`ModelProfile`, its flags, the
detector functions, `top_k`, the config keys):

- `include/ymh/config/config.hpp`, `src/config/config.cpp` — keys + validation
  + the scaffold (`kDefaultConfigJsonc`).
- `src/cli/wiring.cpp` — profile resolution and parameter mapping.
- `include/ymh/llm/provider_registry.hpp`, `src/llm/provider_registry.cpp` —
  `LLMProviderConfig::profile`, `apply_profile_capabilities`.
- `include/ymh/llm/openai_adapter.hpp`, `src/llm/openai_adapter.cpp` — the
  decoder's `ToolCallPolicy` input, the stop guard, leaked-call detection,
  native-ATEM detection, and the whole-object normalization case. (No body
  signature change: `build_chat_completions_body` is unchanged, §8.3.2.)
- `include/ymh/llm/tool_call_assembler.hpp`, `src/llm/tool_call_assembler.cpp`
  — the `ToolArgumentPolicy` finalize policy.
- `include/ymh/agent/agent_loop.hpp`, `src/agent/agent_loop.cpp` — forced first
  tool call and the `buildRequest` turn-local parameter.
- `include/ymh/agent/agent.hpp` — `AgentConfig::profile`.
- `include/ymh/llm/llm_request.hpp`, `include/ymh/llm/llm_call_config.hpp`,
  `src/llm/llm_runtime.cpp` — `top_k`.
- `tests/CMakeLists.txt` — the new test files.

### 19.3 Why a one-step revert is safe

1. **Confined Muse identity** (47-I6). Every Muse-named identifier is in
   `src/llm/model_profile.cpp` or a Muse test file, all of which are new files
   removed by the revert. The edited files reference only generic symbols, so
   removing the new files and restoring the edited ones leaves no dangling
   reference. The adapter's stop-guard message interpolates `config_.profile.id`
   rather than a literal, so no Muse name is baked into the adapter (§6.2.2).
2. **Inert default** (47-I1). The default path is byte-identical and
   test-pinned, so a revert cannot change default behaviour.
3. **No migration** (47-I10). No schema, registry, or event-shape change; a
   pre-Muse config has none of the new keys.
4. **Rehearsed** (Phase 3): `git revert --no-commit <sha>` → build → full suite
   green → `git revert --abort`.

---

## 20. Recorded interpretations (decided offline)

The user is offline; each item below is a **decision with rationale**, not an
open question. No item blocks the gate.

- **Q1 — `reasoning_effort` vs `reasoning_strength` (decided: keep
  `reasoning_effort` only).** Muse's actual reasoning-length control is the
  chat-template variable `reasoning_strength` (`low|medium|high|xhigh`), sent as
  `chat_template_kwargs: {"reasoning_strength": "xhigh"}`; `reasoning_effort:
  "none"` has no effect (GGUF card). D10 pins the pass-through of
  `reasoning_effort` only. **Decision:** do **not** add a
  `chat_template_kwargs` surface in this spec — it is not one of D1–D11, it
  would expand the one-commit surface, and the operator can already express
  `xhigh` via `reasoning_effort` (which Muse may ignore; 47-F34). If a Muse
  deployment needs `reasoning_strength`, that is a separate follow-up spec.
- **Q2 — retained leaked text (decided: accept; bound replay).** 47-D4 converts
  a leaked G-JSON call but leaves the leaked text in the assistant message.
  **Decision:** no streaming holdback (it would be a larger change and would
  alter the stream contract); the raw text is accepted and visible, the
  structured call is the authoritative record, and replay does **not**
  re-execute or re-detect (§7.2.6, 47-I14). A future holdback is a separate
  spec.
- **Q3 — endpoint translation (decided: ship `"muse-glimmer"`; no ATEM
  profile).** Whether the internal `ted-ai` endpoint translates ATEM to standard
  `tool_calls` is unknown (unreachable from the sandbox). **Decision:** the
  shipped default is `"muse-glimmer"` (the standard path). If the Phase-3 live
  check shows no translation, Rev 4 does **not** silently fall back to a
  conversion path: native ATEM in content is detected and surfaced as a typed
  error (47-I13, §8.3.3), and supporting an untranslated endpoint is a **new
  spec** (§8.3.6). The diagnostic is the honest outcome; a conversion path would
  be unverifiable machinery (47-O-M1/M2).
- **Q4 — `parallel_tool_calls=true` (decided: declare true).** The docs say one
  call per message while the GGUF card implies multi-call turns. **Decision:**
  47-D9 declares `parallel_tool_calls=true` (the conservative choice for ymh,
  whose loop already executes N calls, 47-I11) and pins the nuance (§12.2.1);
  revisit only if the live check shows the server returns only the first call
  (47-F9).
- **Q5 — token-id correction (decided: key on the string).** §2.3 corrects the
  plan's finding A (`<|eom|>` = 200007, `<|eot|>` = 200008). **Decision:** the
  D3 denylist keys on the token **string**, so even if a checkpoint's tokenizer
  differs from the published `tokenizer_config.json`, the guard protects the
  right token; only the documentation id would change. Gate 1 independently
  confirmed this ruling.
- **Q6 — leaked-call precision (decided: whole-content for conversion,
  presence for detection).** H3 showed that a complete G-JSON block quoted in
  prose could be converted and executed. **Decision:** a G-JSON block is
  converted only when it is the entire trimmed content; an embedded block is
  text plus a warning, never executed (§7.2.4, 47-F38). The cost (a genuine leak
  embedded in prose ends as a visible final answer) is accepted. For **G-ATEM
  detection**, the whole-content rule is withdrawn (47-O-M1): detection yields a
  visible typed error, never an execution, so requiring whole-content would only
  make the realistic framed native turn undetectable; `to=self`/`to=user` bodies
  are ignored instead (§7.2.7).
- **Q7 — ATEM conversion/rendering (decided: out of scope; new spec if
  needed).** The Oracle's structural finding: ymh can render tool declarations
  and parse calls, but the history is rendered by the server's Jinja template and
  native output is a multi-message channel stream, so the conversion path is
  unverifiable (47-O-M1/M2). **Decision:** D5 is detection-only; the ATEM
  profile, declaration renderer, history echo-back, and converter are removed.
  If the endpoint is untranslated, that is a new spec (§1.4, §8.3.6).
- **Q8 — normalization layer (decided: assembler finalize).** D6's inbound
  normalization runs at the assembler's finalize after concatenation; the only
  per-delta handling is the whole-object case (an object is never fragmented).
  **Decision:** Rev 2's per-delta parse is deleted because it corrupted normally
  fragment-streamed arguments (47-O-H1); the rule is pinned at the finalize seam
  and tested over the real adapter (§9.2, §18.5).

---

## 21. Revision log

- **Rev 1 (initial draft).** Authors 47-D1 … 47-D11 from the approved plan
  (`.sisyphus/plans/muse-glimmer-support.md`). Current-state claims verified
  against the shipped tree (HEAD `e74b4a242`):
  `include/ymh/config/config.hpp:69-76`, `:121-141`;
  `src/config/config.cpp:79-214`, `:386-451`, `:1204-1315`;
  `src/cli/wiring.cpp:61-77`, `:193-213`;
  `include/ymh/llm/llm_call_config.hpp:32-45`, `:78`, `:83-133`;
  `include/ymh/llm/llm_request.hpp:41-61`;
  `include/ymh/llm/llm_provider.hpp:29-37`;
  `include/ymh/llm/provider_registry.hpp:26-41`, `:72-75`;
  `src/llm/provider_registry.cpp:91-111`;
  `src/llm/openai_adapter.cpp:84-98`, `:235-294`, `:336-360`, `:402-419`,
  `:432-505`, `:703-756`, `:840-915`;
  `src/llm/tool_call_assembler.cpp:78-136`;
  `src/llm/llm_runtime.cpp:25-45`, `:127-133`, `:147-166`, `:180-184`,
  `:294-299`;
  `include/ymh/agent/agent.hpp:55-71`, `:108-123`;
  `src/agent/agent.cpp:26-37`;
  `src/agent/agent_loop.cpp:522-543`, `:929-970`, `:982`, `:1063-1072`,
  `:1133-1141`, `:1149`, `:1170-1173`, `:1177-1184`, `:1221-1296`;
  `src/agent/workspace_runtime.cpp:321-334`;
  `tests/CMakeLists.txt:1-70`.
  Primary external sources: the Muse-Glimmer-30B GGUF model card, the
  prompting guide, the `tokenizer_config.json` `response_template`, the
  ToolCall-LoRA model card, and n8n issue #31135.
  Not yet reviewed. **Verification status: awaiting the adversarial critic and
  the Oracle design gate.**
- **Rev 2 (Gate-1 response).** Responds to `/tmp/opencode/spec47-gate1.md`
  (GATE FAIL — 3 HIGH / 13 MEDIUM / 5 LOW); every gate-1 ID is resolved or
  rebutted in §23. Scope is unchanged (D1–D11). The load-bearing changes:
  - **H1** — the reversibility invariant is restated as a *true, testable*
    property: every Muse-named identifier is confined to
    `src/llm/model_profile.cpp` and the Muse tests; the ATEM codec and the
    leaked-call detector are split into generic components; the edited files
    reference only generic symbols (§19.2, 47-I6,
    `MuseProfile_MuseNamesConfinedToProfileTable`).
  - **H2** — the full ATEM history/echo-back path is pinned: mapping-form
    `arguments` (JSON object) under Atem, string-form under OpenAi, id-paired
    tool results, and a `MuseTurn_AtemMultiStepRoundTrip` test (§8.3.6,
    §9.2, 47-I13).
  - **H3** — the D4 posture is now conservative and coherent: whole-content
    conversion only; an embedded/quoted complete block is text plus a warning,
    never executed; the false "reference-parser tool-recipient" claim is
    deleted; the "never a final answer" headline is scoped to convertible
    blocks (§7.2.3/§7.2.4, 47-I5, 47-F38).
  - **M1/M2** — D2 keys on the turn-local `stepNumber` (compaction-proof) and
    documents the pure-question trade-off with an explicit-`tool_choice`
    opt-out plus 47-F37 (§5.2/§5.3, 47-I3).
  - **M3/M4/M5/M6/M7/M9/M13** — one answer for unparseable offered-name args
    (`MalformedToolCall`); wrapper-optional ATEM detection; the full declaration
    block pinned (namespace divergence, `# Valid recipients`, example); the
    JSON-looking-string type-loss accepted and pinned; the body-builder
    signature and injection rule pinned; the codec/detector split; `tools[]`
    omitted under Atem.
  - **M8** — D9 justified as generic future-proofing (a behavioural no-op for
    Muse today) with a non-vacuous overlay test.
  - **M10/M11/M12** — `§13.2` now cites only the four real comments
    (`llm_request.hpp:47`, `config.hpp:72`, `config.cpp:984`, `:1006`);
    `§12.1` cites `include/ymh/llm/provider_registry.hpp:72-75`; the D3 message
    interpolates the offending token and the profile id, the false
    cross-layer-miss premise is deleted, and F20 is re-derived.
  - **L1–L5** — finish override pinned; channel framing corrected; vacuous
    tests replaced; the scaffold updated; the replay consequence bounded.
  All citations re-opened against HEAD `e74b4a242`:
  `include/ymh/llm/provider_registry.hpp:26-41`, `:72-75`;
  `include/ymh/llm/llm_call_config.hpp:32-45` (`:35` has no comment);
  `include/ymh/llm/llm_request.hpp:41-49`, `:58`;
  `include/ymh/llm/llm_provider.hpp:29-37`;
  `include/ymh/llm/openai_adapter.hpp:74-76`, `:93-96`;
  `include/ymh/config/config.hpp:72`, `:121-141` (`:135` has no comment);
  `include/ymh/tools/tool.hpp:45`;
  `include/ymh/agent/agent.hpp:96`, `:108-123`;
  `src/config/config.cpp:180-214`, `:386-451`, `:963-1021` (`:984`, `:1006`),
  `:1309-1315`;
  `src/llm/provider_registry.cpp:91-111`;
  `src/llm/openai_adapter.cpp:84-98`, `:126-147`, `:235-294`, `:336-360`,
  `:402-419`, `:432-449`, `:452-505`, `:703-756`, `:855-860`, `:892-915`;
  `src/llm/tool_call_assembler.cpp:94-106`;
  `src/llm/llm_runtime.cpp:25-45`, `:127-133`, `:150-166`, `:180-184`;
  `src/agent/agent.cpp:26-37`;
  `src/agent/agent_loop.cpp:522-543`, `:909-970`, `:982`, `:1095-1102`,
  `:1125-1141`, `:1149`, `:1177-1184`;
  `src/agent/agent_registry.cpp:77`;
  `include/ymh/session/events.hpp:131-153`;
  `tests/CMakeLists.txt:1-70`.
  **Verification status: awaiting the Oracle design pass.**
- **Rev 3 (Oracle response + D5 descope).** Responds to
  `/tmp/opencode/spec47-oracle.md` (**DO NOT APPROVE — 2 HIGH / 5 MEDIUM /
  3 LOW**, all new to Rev 2); every Oracle ID (47-O-H1…47-O-L3) is resolved or
  rebutted in §23.4. The load-bearing changes:
  - **47-O-H1 (D6 layer)** — inbound normalization moved from per-delta
    `handle_tool_call` to the **assembler's finalize** after concatenation; the
    whole-object delta is the only per-delta case; `FakeLLM` bypasses the path
    and cannot test it (§9.2, §15, 47-I15, 47-F42, §18.5 adapter tests).
  - **47-O-H2 (D2 wire guard)** — `required` is forced only when the outgoing
    body carries `tools[]`; the ATEM `tools[]` omission is removed (no profile
    omits `tools[]`), and the combination is pinned as 47-F22/47-F41
    (§5.2, 47-I3/47-I16).
  - **D5 descope (47-O-M1/M2)** — the `"muse-glimmer-atem"` profile, the ATEM
    declaration renderer, the history echo-back, and the ATEM→`ToolCall`
    converter are removed; D5 is **detection-only** and the §8.2 grammar is the
    detection grammar; an untranslated endpoint is a new spec (§1.4, §8, §20
    Q3/Q7, §19.2).
  - **47-O-M1** — the whole-content rule applies only to G-JSON conversion;
    G-ATEM detection is presence-based over the tool-channel body and ignores
    `to=self`/`to=user` (§7.2.7, 47-I13, §18.4).
  - **47-O-M3** — the decoder's inputs are pinned (`ToolCallPolicy` with the
    resolved profile and offered names); the parse functions are pure and the
    offered-name/normalization checks run in `finalize` (§7.2.5, §15).
  - **47-O-M4** — `buildRequest` gains the loop-local `turn_step` parameter;
    §5.2 states the shipped signature receives the session-global `StepId`
    (§5.2, §15, `MuseD2_TurnLocalStepNotSessionStep`).
  - **47-O-M5** — D4/D5/D6 end-to-end tests move to the real adapter over a
    fake `HttpTransport`; `FakeLLM` is kept only for D1/D2 loop tests (§18.4,
    §18.5, §18.8).
  - **47-O-L1/L2/L3** — the scaffold `profile` example is the non-Muse
    placeholder `""`; a configured `top_k: 0` maps to `nullopt` at read time;
    the D4 taxonomy is split (outer non-object → `MalformedToolCall`,
    non-object `arguments` → `{}`, missing delimiter → text) (§10.2.4, §10.2.1,
    §7.2.3).
  All citations re-opened against HEAD `e74b4a242`:
  `include/ymh/llm/provider_registry.hpp:26-41`, `:39`, `:72-75`;
  `include/ymh/llm/tool_call_assembler.hpp:26`, `:30-43`;
  `include/ymh/llm/openai_adapter.hpp:74-76`, `:93-96`;
  `include/ymh/llm/llm_request.hpp:41-49`, `:58`;
  `include/ymh/llm/llm_call_config.hpp:32-45`;
  `include/ymh/llm/llm_provider.hpp:27-37`;
  `include/ymh/llm/fake_llm.hpp:47`;
  `include/ymh/tools/tool.hpp:34-37`, `:45`;
  `include/ymh/agent/agent.hpp:96`, `:108-124`;
  `include/ymh/agent/agent_loop.hpp:169`;
  `include/ymh/session/session.hpp:297`, `:317`;
  `include/ymh/config/config.hpp:72`, `:121-141` (`:135` has no comment);
  `src/config/config.cpp:180-214`, `:386-451`, `:963-1021` (`:984`, `:1006`),
  `:1309-1315`;
  `src/llm/provider_registry.cpp:91-111`;
  `src/llm/openai_adapter.cpp:84-98`, `:126-147`, `:235-294`, `:300-307`,
  `:336-360`, `:402-419`, `:432-505`, `:703-756`, `:855-860`, `:892-915`,
  `:935`;
  `src/llm/tool_call_assembler.cpp:78-109`, `:111-136`;
  `src/llm/fake_llm.cpp:170-185`;
  `src/llm/llm_runtime.cpp:25-45`, `:127-133`, `:150-166`, `:180-184`;
  `src/agent/agent.cpp:26-37`;
  `src/agent/agent_loop.cpp:522-543`, `:929-970`, `:982`, `:1095-1102`,
  `:1125-1141`, `:1149`, `:1177-1184`;
  `src/agent/agent_registry.cpp:77`;
  `src/session/session.cpp:624-625`;
  `include/ymh/session/events.hpp:131-153`;
  `tests/unit/openai_adapter_test.cpp:127`;
  `tests/CMakeLists.txt:1-70`.
  **Verification status: awaiting the Oracle re-gate.**
- **Rev 4 (Oracle-final response).** Responds to
  `/tmp/opencode/spec47-oracle-final.md` (**DO NOT APPROVE — 0 HIGH / 2 MEDIUM /
  3 LOW**, rev-3 findings only; both Rev-3 HIGHs confirmed fixed); every final
  ID (47-O2-M1/M2, 47-O2-L1/L2/L3) is resolved in §23.6. The load-bearing
  changes:
  - **47-O2-M1 (the H2 tautology).** The `wire_tools_present` conjunct and
    `MuseD2_WireToolsGuard` are removed. `build_chat_completions_body` takes no
    profile and emits `tools[]` iff `request.tools` is non-empty
    (`src/llm/openai_adapter.cpp:703-756`, `:714-725`), no `ModelProfile` field
    can omit `tools[]` (§15), and `stream()` rejects a tools-bearing request
    before body build (`:895-898`), so the internal `!request.tools.empty()`
    guard is the only guard. 47-I3 states the structural impossibility; 47-I16
    is pinned by `MuseBodyGolden_ProfileOn` alone; 47-F22 absorbs 47-F41
    (§5.2, §8.3.2, §16, §17, §18.2).
  - **47-O2-M2 (the parser result shape).** `parse_leaked_json_call` returns
    `LeakedParse { std::vector<LeakedCall> calls; bool complete_block_seen;
    bool malformed_block_seen; }`, and `finalize` maps it onto the §7.2.3
    taxonomy. `MuseD4_TwoBlocksAreBothConverted`,
    `MuseD4_CompleteMalformedBlockIsNotText`, and
    `MuseAdapter_TwoLeakedBlocksBothExecute` pin the multi-block and
    malformed-complete cases (§7.2.3, §7.2.5, §15, §16 47-I5, §18.4).
  - **47-O2-L1/L2/L3.** 47-F41 merged into 47-F22 (one row, cross-referenced
    from §5.6/§8.4); `MuseAdapter_StreamedFragmentsAreNotCorrupted` relabelled a
    regression pin with H1's fail-first tests named; the three mis-layered
    pure-detector tests re-pointed
    (`MuseAdapter_OfferedNameUnparseableArgsIsError`,
    `MuseD5_NativeAtemInToolChannelIsDetected`,
    `MuseAdapter_NativeAtemEmitsNoToolCallEvents`) (§17, §18.4, §18.5).
  Citations re-opened against HEAD `e74b4a242` for the Rev-4 delta:
  `src/llm/openai_adapter.cpp:703-756`, `:714-725`, `:895-898`;
  `include/ymh/llm/openai_adapter.hpp:93-96`;
  `src/llm/tool_call_assembler.cpp:56-76`, `:78-109`, `:111-136`;
  `include/ymh/tools/tool.hpp:34-37`, `:45-52`.
  **Verification status: awaiting the Oracle delta re-gate.**

### 21.1 Addendum — import pre-sets the profile via the seam

The localcode import (`build_localcode_import`) now pre-sets `llm.default.profile`
whenever the imported provider is openai-compatible, **regardless of the imported
model** (the user's explicit requirement: the profile is added even when the model
is not Muse, and is adjusted from there). The pre-set is skipped when the supplied
id is empty, and the "no `llm` section" outcomes for a non-openai-compatible
provider or a document with no default profile are unchanged
(`UI46_D12_BedrockTypeSkipped`, `UI46_D12_NoProfileNoImport`).

The id is not named in `src/config/config.cpp`. It is obtained through a
neutrally-named accessor, `default_import_profile_id()` (declared in
`include/ymh/llm/model_profile.hpp`, defined in `src/llm/model_profile.cpp`), and
passed into the import as the `default_profile_id` parameter by the CLI layer
(`maybe_import_localcode_config`). The literal therefore remains confined to
`src/llm/model_profile.cpp` and the Muse test files, and
`MuseProfile.MuseNamesConfinedToProfileTable` (47-I6) still passes honestly.

**Consequence for the revert.** This pre-set writes the literal
`profile: "muse-glimmer"` into a generated `config.jsonc`, which is user state
outside the tree. If the Muse code is ever reverted (47-D11, §19), that leftover
key would name a profile the reverted binary no longer knows, and config load
would fail with an unknown-profile `ConfigError` (`src/config/config.cpp`). A
revert must therefore also clean the key from any config it generated (or the
user must remove `llm.default.profile` first). This addendum does not change
47-D11's one-commit contract; it records the generated-state caveat that contract
must account for.

---

## 22. dsh mapping

This spec is an errata on the dsh-alignment surface (`26-dsh-alignment.md`,
`26-dsh-alignment-part2.md §4.3.1`); it adds no dsh concept. Each decision maps
to an existing dsh construct or a provider-boundary adaptation:

| D | dsh construct | Nature of the mapping |
|---|---|---|
| 47-D1 | Provider/model configuration | A named `ModelProfile` is ymh-local selection state layered over the provider config; dsh has no profile concept, so this is an ymh seam, not a dsh extension. |
| 47-D2 | `GenerateOptions.tool_choice` | The forced first call sets the existing tool-choice option; no new option. |
| 47-D3 | `GenerateOptions.stop` | A client-side validation/guard on an existing option. |
| 47-D4 | Tool-call normalization at the provider boundary | Lifting leaked syntax into the structured `tool_calls` channel is provider-boundary repair; the agent-loop contract is unchanged. |
| 47-D5 | Provider-boundary detection | Native ATEM is a provider-specific wire format behind the same `LLMProvider` seam; ymh detects it and errors rather than adapting it, so dsh's message/tool model is untouched. |
| 47-D6 | `GenerateOptions` tolerance | Normalizing `arguments`/`tool_calls` at the boundary, consistent with the assembler contract. |
| 47-D7 | Options surface | New ymh config keys that populate existing `GenerateOptions`/`LlmCallConfig` fields; dsh's six-field core is unchanged. |
| 47-D8 | Provider extension | `top_k` is a non-standard extension beyond dsh's core options, carried additively. |
| 47-D9 | Capability advertisement (08 §3.4) | The profile overlays the existing truthful-capability set; no dsh change. |
| 47-D10 | Reasoning-effort option | Documentation of an existing pass-through; no type change. |
| 47-D11 | Feature-flag / commit hygiene | The whole errata is one revertible commit; dsh alignment is preserved by the inert default. |

---

## 23. Gate response (Gate 1 → Rev 2; Oracle → Rev 3; Oracle-final → Rev 4)

Every finding in `/tmp/opencode/spec47-gate1.md` (H1–H3, M1–M13, L1–L5) is
resolved or rebutted in §23.1–§23.2; "Where" names the section in force when the
disposition was written (Rev 2). Every finding in
`/tmp/opencode/spec47-oracle.md` (47-O-H1…47-O-L3) is resolved or rebutted in
§23.4. Every finding in `/tmp/opencode/spec47-oracle-final.md` (47-O2-M1,
47-O2-M2, 47-O2-L1/L2/L3) is resolved in §23.6. Severity is the report's.

**Rev-3 supersession note.** The D5 descope (§1.4, §8.3.6) removes the ATEM
conversion/rendering path. Gate-1 dispositions that pinned that path are
therefore **superseded** by Rev 3, not contradicted: **H2** (mapping-form history
echo-back), **M7** (body-builder profile parameter for the ATEM catalog), and
**M13** (`tools[]` omitted under Atem) no longer describe the design — there is
no ATEM rendering, no body-builder signature change, and no `tools[]` omission.
Their underlying findings are closed *by removal*. All other gate-1
dispositions stand.

### 23.1 Gate-1 findings

| Finding | Sev | Disposition | Where |
|---|---|---|---|
| H1 | HIGH | **Resolved.** The invariant is restated to a true, testable property: every Muse-named identifier is confined to `src/llm/model_profile.cpp` + Muse tests; the ATEM codec and G-JSON detector are generic, split components; edited files reference only generic symbols. `MuseProfile_NoMuseSymbolsOutsideSeam` is replaced by `MuseProfile_MuseNamesConfinedToProfileTable` (grep `include/`+`src/`, excluding `model_profile.cpp`). The one-commit-revert guarantee is retained (§19.3). | §1.5, §8.3.1, §15, §16 (47-I6), §19.2, §19.3, §18.1 |
| H2 | HIGH | **[Superseded by Rev 3 — the descope removed the ATEM history path; closed by removal.]** **Resolved (Rev 2).** The full ATEM history path is pinned: mapping-form `arguments` (JSON object) under Atem, string-form under OpenAi/inert; `role: tool` results id-paired; multi-step rendering; both-directions mapping table. `MuseTurn_AtemMultiStepRoundTrip` + `MuseD6_AtemEchoBackIsMappingForm`. | §8.3.6, §9.2, §16 (47-I13), §18.5, §18.8 |
| H3 | HIGH | **Resolved.** False "tool-recipient body" claim deleted; the reference parser's `open_pattern` is quoted accurately; conversion is whole-content-only, so a quoted/embedded complete block is text + a warning, never executed; `MalformedToolCall` for non-offered/unparseable; the headline is scoped to convertible blocks. | §1.1 (D4), §7.2.3, §7.2.4, §7.2.5, §7.3 (47-F38), §16 (47-I5), §18.4 |
| M1 | MEDIUM | **Resolved.** The predicate keys on the turn-local `stepNumber`, not assembled history; compaction cannot re-force; rationale for restart/replay safety restated; `MuseD2_ForceAfterCompactionStaysOff`. | §5.2, §5.3, §5.4, §16 (47-I3), §18.2 |
| M2 | MEDIUM | **Resolved (accepted trade-off + opt-out).** Documented that every profiled turn begins with a forced call; an explicit `tool_choice` (including `"auto"`) is the opt-out; 47-F37 for a backend that rejects `required`. | §5.2, §5.6 (47-F37), §17 (47-F37), §18.2 |
| M3 | MEDIUM | **Resolved.** One rule: offered name + unparseable/non-normalizable args → `MalformedToolCall`; normalization only maps `""`/`null`/non-object → `{}`; text only for a missing delimiter. §7.2.3, 47-I5 and tests aligned. | §7.2.3, §16 (47-I5), §18.4 |
| M4 | MEDIUM | **Resolved.** Detection keys on `<atem:invoke>…</atem:invoke>`; the `<atem:function_calls>` wrapper is optional framing, matching the reference parser. EBNF, regex note, §8.3.3 and a wrapper-less test updated. | §8.2.2, §8.2.3, §8.3.3, §18.4 |
| M5 | MEDIUM | **[Superseded by Rev 3 — the declaration renderer is removed; closed by removal.]** **Resolved (Rev 2).** The full declaration block is pinned (namespace divergence, `// Function schemas`, the real example, `# Valid recipients`, `Reasoning strength`), golden-tested against a committed fixture. | §8.2.4 (was §8.2.6), §18.9 |
| M6 | MEDIUM | **[Superseded by Rev 3 — the value round-trip tests are removed; closed by removal.]** **Resolved (Rev 2).** The JSON-looking-string type-loss is stated and accepted as an ATEM limitation; `AtemCodec_RenderRoundTrips` is qualified; `AtemCodec_JsonLookingStringIsTyped` pins it. | §8.2.4 (was §8.2.5.1), §18.4 |
| M7 | MEDIUM | **[Superseded by Rev 3 — no body-builder signature change; closed by removal.]** **Resolved (Rev 2).** `build_chat_completions_body` gains the `ModelProfile&` parameter; the no-system-message injection rule is pinned; the call site and golden seam are named. | §8.3.2, §15, §18.9 |
| M8 | MEDIUM | **Resolved.** D9 is justified as generic future-proofing (a behavioural no-op for Muse today); the vacuous test is replaced by `MuseD9_OverlayOverridesSyntheticBase`. | §12.2, §12.2.0, §18.7 |
| M9 | MEDIUM | **[Strengthened by Rev 3 — there is no ATEM codec at all; both arms live in one generic detector.]** **Resolved (Rev 2).** The G-JSON detector is split into `leaked_call_detector.hpp/.cpp`; under `"muse-glimmer"` the ATEM render/parse arms are dormant while the JSON arm remains active; the wording is corrected. | §7.2.5, §8.3.1, §15, §19.2 |
| M10 | MEDIUM | **Resolved.** §13.2 cites only the four real comments (`llm_request.hpp:47`, `config.hpp:72`, `config.cpp:984`, `:1006`); the two comment-less anchors are removed (they remain only as type-location citations in §13.1). | §13.2, §10.2.4 |
| M11 | MEDIUM | **Resolved.** The path is corrected to `include/ymh/llm/provider_registry.hpp:72-75`. | §12.1 |
| M12 | MEDIUM | **Resolved.** The D3 config message interpolates the offending token; the adapter message interpolates `config_.profile.id`; the false "best-effort / cross-layer miss" caveat is deleted (the accumulated-config fallback catches it); F20 is re-derived to the `read_string_array` clearing case and the read semantics are pinned. | §6.2.2, §10.2.1, §6.3 (47-F20), §17 (47-F20), §18.3, §18.6 |
| M13 | MEDIUM | **[Superseded by Rev 3 — no profile omits `tools[]`; closed by removal.]** **Resolved (Rev 2).** Under Atem, `tools[]` is omitted so no endpoint can double-render the catalog; 47-F22 is re-scoped to an endpoint whose parser requires `tools[]`. | §8.3.2, §8.4 (47-F22), §17 (47-F22) |
| L1 | LOW | **Resolved.** The finish override is pinned: a conversion emits `Finished{ToolCalls}` even when the server sent `stop`; the exact expression is given. | §7.2.3, §7.2.5, §16 (47-I5) |
| L2 | LOW | **Resolved.** The framing bullet now states that `content.open` requires `to=user`; the header-less first message is anchored by `start_anchor`. | §8.2.1 |
| L3 | LOW | **[Partly superseded by Rev 3 — `AtemDeclarationsGolden` is removed.]** **Resolved (Rev 2).** `MuseD3_EotIsTheTurnDelimiter` → behavioural `MuseD3_DenylistIsTheSixControlTokens`; `MuseD9_…DeclaresReasoningAndParallel` → non-vacuous `MuseD9_OverlayOverridesSyntheticBase`; `MuseTurn_ParallelCallsAllExecute` → **pin**; golden fixtures pinned (`MuseBodyGolden_ProfileOff`, `MuseCanonicalJsonGolden_ProfileOff`, `AtemDeclarationsGolden`). | §18.3, §18.7, §18.8, §18.9 |
| L4 | LOW | **Resolved.** The scaffold `kDefaultConfigJsonc` is updated with the new keys and the `"xhigh"` comments; `MuseD7_ScaffoldMentionsNewKeys` pins it. | §10.2.4, §18.6 |
| L5 | LOW | **Resolved.** The replay consequence is bounded: replay reproduces the frozen request and durable events, never re-detects or re-executes; `MuseReplay_LeakedTurnDoesNotReexecute`. | §7.2.6, §16 (47-I14), §18.4 |

### 23.2 Rebuttals

- **M12 (F20 premise).** The report's reading is accepted: `apply_llm` reads
  keys with a fallback to the accumulated config and `load_config` applies
  layers onto one `Config`, so the original "profile global + stop workspace is
  missed" premise is **false** and is deleted. The failure mode is re-derived to
  the real `read_string_array`-clearing behaviour; the cross-layer check itself
  is now asserted (with a test), not rebutted.
- **L2 (no-recipient content).** Accepted as a spec error and corrected, not
  rebutted: `content.open` requires `to=user`.
- All other findings are resolved; none is left as a non-blocking open
  question. The token ruling (`<|eom|>` = 200007, `<|eot|>` = 200008, D3 keys on
  the string) is retained unchanged, as Gate 1 independently confirmed.

### 23.3 Re-gate checklist

- [x] Gate-1 H1: 47-I6 is true and testable; the grep test is implementable.
- [x] Gate-1 H2: superseded by the Rev-3 descope (no ATEM history echo-back);
      the finding is closed by removal.
- [x] Gate-1 H3: coherent conservative detector posture; no quoted block
      executes.
- [x] Gate-1 M1–M13 resolved or rebutted; M7/M13 superseded by the descope.
- [x] Gate-1 L1–L5 addressed.
- [x] Every gate-1 citation re-opened (see §21 Rev 2 list).

### 23.4 Oracle findings (Rev 2 → Rev 3)

| Finding | Sev | Disposition | Where |
|---|---|---|---|
| 47-O-H1 | HIGH | **Resolved.** Inbound normalization moved to the assembler's finalize (after concatenation); `handle_tool_call` only dumps a whole-object delta once. The per-delta parse that corrupted fragment streams is deleted. 47-I15, 47-F42, and adapter-level streamed-fragment tests pinned. | §9.2, §9.3 (47-F42), §15, §16 (47-I15), §17 (47-F42), §18.5 |
| 47-O-H2 | HIGH | **Resolved.** `force_required` requires the outgoing body to carry `tools[]`; the ATEM `tools[]` omission is removed so no built-in profile omits it; the combination is pinned as 47-F22/47-F41 and `MuseD2_WireToolsGuard`. **[Corrected by Rev 4 — 47-O2-M1: the `wire_tools_present` conjunct and `MuseD2_WireToolsGuard` were tautological (no input can make them differ from `!request.tools.empty()`); both are removed and the hazard is recorded as structurally impossible. 47-F41 is merged into 47-F22. See §23.6.]** | §5.2, §5.6 (47-F41), §8.3.2, §16 (47-I3/47-I16), §17 (47-F22/F41), §18.2 |
| 47-O-M1 | MEDIUM | **Resolved by the descope.** G-ATEM is detection-only; the whole-content rule is withdrawn for detection and replaced by a presence test over the tool-channel body that ignores `to=self`/`to=user`. The realistic three-message turn is a pinned test. The raw-endpoint conversion claim is withdrawn. | §7.2.7, §8.3.3, §8.3.6, §16 (47-I13), §18.4 |
| 47-O-M2 | MEDIUM | **Resolved by removal.** No `render_atem_call`/`render_atem_history` is invented and no round-trip is claimed: the ATEM codec, `render_atem_tool_declarations`, `parse_atem_calls`, `AtemCall`, the `AtemCodec_*` tests, and the declarations fixture are removed. | §1.4, §8.2.4, §8.3.6, §18.4, §19.2 |
| 47-O-M3 | MEDIUM | **Resolved.** The decoder's real inputs are pinned as `ToolCallPolicy { const ModelProfile*; std::span<const ToolName> offered; }`; the parse functions are pure, and the offered-name and normalization checks run in `finalize`. | §7.2.5, §15, §18.4 (`MuseAdapter_OfferedNameCheckUsesRequestTools`) |
| 47-O-M4 | MEDIUM | **Resolved.** `buildRequest` is pinned to gain the loop-local `turn_step` parameter (both call sites pass `stepNumber`); §5.2 states the shipped 3-arg form receives the session-global `StepId` and is insufficient. | §5.2, §15, §18.2 (`MuseD2_TurnLocalStepNotSessionStep`) |
| 47-O-M5 | MEDIUM | **Resolved.** D4/D5/D6 end-to-end tests move to the real `OpenAICompatibleProvider` over a fake `HttpTransport`; `FakeLLM` is retained only for D1/D2 loop tests, with the bypass stated. | §9.2, §18.4, §18.5, §18.8 |
| 47-O-L1 | LOW | **Resolved.** The scaffold `profile` example is the non-Muse placeholder `""`; `MuseProfile_ScaffoldHasNoMuseName` asserts no `muse` in the scaffold. | §10.2.4, §18.1 |
| 47-O-L2 | LOW | **Resolved.** A configured `top_k: 0` maps to `nullopt` at read time, so the body builder's `has_value()` condition is exact; `MuseD8_TopKZeroIsOmitted`. | §10.2.1, §11.2, §18.5 |
| 47-O-L3 | LOW | **Resolved.** The D4 taxonomy is split: outer JSON not an object → `MalformedToolCall`; non-object `arguments` under normalization → `{}`; only a missing delimiter → text. | §7.2.3, §16 (47-I5), §18.4 (`MuseD4_OuterJsonNotObjectIsError`) |

**Oracle rebuttals: none.** Every Oracle finding is accepted and fixed (two by
removal via the descope). The token ruling (`<|eom|>` = 200007,
`<|eot|>` = 200008, D3 keys on the string) is retained unchanged; the Oracle
accepted it and did not revisit it.

### 23.5 Rev-3 re-gate checklist

- [x] 47-O-H1: normalization at the assembler finalize; streamed-fragment test.
- [x] 47-O-H2: `required` gated on wire `tools[]`; no profile omits it. (The
      separate wire conjunct is removed in Rev 4 as a tautology — §23.6,
      47-O2-M1.)
- [x] 47-O-M1/M2: D5 detection-only; conversion/rendering removed; new-spec note.
- [x] 47-O-M3: `ToolCallPolicy` inputs pinned.
- [x] 47-O-M4: `buildRequest` `turn_step` pinned.
- [x] 47-O-M5: D4/D5/D6 tests over the real adapter.
- [x] 47-O-L1/L2/L3: scaffold placeholder, `top_k: 0`, D4 taxonomy.
- [x] Every invariant testable and true of the descoped design; no ATEM
      conversion machinery retained.
- [x] Every citation re-opened (see §21 Rev 3 list).

### 23.6 Oracle-final findings (Rev 3 → Rev 4)

| Finding | Sev | Disposition | Where |
|---|---|---|---|
| 47-O2-M1 | MEDIUM | **Resolved (tautology removed).** Rev 3's `wire_tools_present` conjunct had no input: `build_chat_completions_body` takes no profile and emits `tools[]` iff `request.tools` is non-empty (`src/llm/openai_adapter.cpp:703-756`, `:714-725`), no `ModelProfile` field can omit `tools[]` (§15), and `stream()` rejects a tools-bearing request before body build when `capabilities_.tool_calls` is false (`:895-898`). The conjunct and `MuseD2_WireToolsGuard` are deleted; 47-I3 states the hazard is structurally impossible; 47-I16 is pinned by `MuseBodyGolden_ProfileOn` alone; 47-F22 absorbs 47-F41 as a forward-looking note with no test. | §5.2, §8.3.2, §16 (47-I3/47-I16), §17 (47-F22), §18.2, §23.4 (H2 note) |
| 47-O2-M2 | MEDIUM | **Resolved (result shape pinned).** `parse_leaked_json_call` returns `LeakedParse { std::vector<LeakedCall> calls; bool complete_block_seen; bool malformed_block_seen; }` instead of `std::optional<LeakedCall>`. `finalize` maps the shape onto the §7.2.3 taxonomy: `!complete_block_seen` → text (rows 6/7); `malformed_block_seen`, a non-offered name, or unparseable offered-name args → `MalformedToolCall` (rows 2/3/4); an all-offered, all-normalizable `calls` set → conversion (rows 1/5). Row 1's "one or more blocks" is expressible; two-block and malformed-complete tests are pinned. | §7.2.3, §7.2.5, §15, §16 (47-I5), §18.4 |
| 47-O2-L1 | LOW | **Resolved.** 47-F41 is merged into 47-F22 (one row, cross-referenced from §5.6/§8.4/§17); the duplicate trigger/symptom/recovery pair is gone. | §5.6, §8.4, §17 |
| 47-O2-L2 | LOW | **Resolved.** `MuseAdapter_StreamedFragmentsAreNotCorrupted` is relabelled a regression pin (it passes pre-change); the H1 fail-first tests `MuseAdapter_ObjectArgumentsWholeDeltaIsNormalized` and `MuseAdapter_NonObjectStringNormalizedAtFinalize` are named in its row. | §18.5 |
| 47-O2-L3 | LOW | **Resolved.** The three mis-layered "pure-detector" tests are re-pointed: `MuseAdapter_OfferedNameUnparseableArgsIsError` and `MuseAdapter_NativeAtemEmitsNoToolCallEvents` move to the adapter-level block; `MuseD5_NativeAtemInToolChannelIsError` becomes the pure `MuseD5_NativeAtemInToolChannelIsDetected` (names returned), with the terminal error asserted by `MuseAdapter_NativeAtemInStreamedContentErrors`. | §18.4 |

**Oracle-final rebuttals: none.** Every final finding is accepted and fixed. The
token ruling (`<|eom|>` = 200007, `<|eot|>` = 200008, D3 keys on the string) and
the D5 descope (detection-only; no ATEM conversion machinery) are retained
unchanged; the Oracle-final report accepted both.

### 23.7 Rev-4 re-gate checklist

- [x] 47-O2-M1: the `wire_tools_present` tautology and `MuseD2_WireToolsGuard`
      are removed; 47-I3/47-I16/47-F22 state the structural closure.
- [x] 47-O2-M2: the `LeakedParse` result shape makes every §7.2.3 row decidable;
      two-block and malformed-block tests added.
- [x] 47-O2-L1: F22/F41 deduplicated.
- [x] 47-O2-L2: the streamed-fragments test is a labelled regression pin; H1's
      fail-first tests exist and are named.
- [x] 47-O2-L3: the three mis-layered tests are re-pointed at the implementing
      component.
- [x] Token ruling preserved (`<|eom|>` = 200007, `<|eot|>` = 200008, keyed on
      the string).
- [x] D5 descope preserved (detection-only; no ATEM conversion machinery).
- [x] Every Rev-4-touched citation re-opened (see §21 Rev 4 list); D1–D11 still
      covered; the change remains one revertible commit.

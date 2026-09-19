# 26 — dsh Alignment, Part 2: Target Design, Migration, Open Questions

Status: **verified — Rev 7** (GATE PASS: 0 HIGH / 0 MEDIUM / 5 LOW; design only, no implementation).
Companion to `26-dsh-alignment.md` (§1 purpose, §2 dsh mechanics, §3 gap
analysis, §7 revision log). This file contains §4 (target design), §5 (migration
plan and test strategy), §6 (open questions), and the Appendix.

Every type referenced by a decision below is either defined in this file or
already exists in the tree with a cited `file:line`. Where a type is new, its
definition is pinned here so an implementer never has to guess. Where a decision
contradicts a verified spec, it is classified **Brk.** and its owning spec is
added to the Wave 0 gate (§5).

---

## 4. Target design for ymh

### 4.1 Design principles and invariants

These hold for every decision below.

- **26-I1 — The log is the source of truth.** Every request, response, retry,
  prompt change, and collaboration action is an append-only session event. No
  mechanic may depend on in-memory state that the log cannot rebuild. (This
  extends the already-verified `00 §9.1` and `01` event-sourcing invariant.)
- **26-I2 — Requests are frozen and reconstructable from the log plus the
  running code's registries, under a pinned canonical serialization.** A
  dispatched request is immutable by type. "Reconstructable" is defined over two
  pinned forms, because a request's *template* is series-stable while its
  *messages* grow every step:
  - `canonical_template()` is the request-invariant part: `schema_version`,
    `envelope` (`session_id` + optional `purpose`), `config`, the full rendered
    `system_prompt`, and the canonical tool schemas. `template_digest()` is its
    lowercase SHA-256 hex. The rendered prompt is **always** an input to the
    digest; its *text* is persisted in the header only under the explicit
    `session.persist_prompt_text` opt-in (`26-I11`), and the default durable
    record is `system_prompt_digest`.
  - `canonical_json()` is `canonical_template()` plus the ordered `messages` of
    one concrete dispatch.

  Replay rebuilds the template from the last `LlmRequestHeader` through
  `buildRequest()` — re-deriving the tool schemas from the live `ToolRegistry` —
  and asserts `rebuild.template_digest() == header.template_digest`; it
  reconstructs the messages for the attempt from the logged message events. The
  header is therefore a **changed-snapshot** record (dsh's `callConfigEquals`
  model, `dsh-llm/lib/types/call-config.d.ts:1-23`), not a per-request digest: it
  carries the rendered prompt's **digest** (the full text only under the
  `session.persist_prompt_text` opt-in), the tool **names** in canonical order,
  and a per-tool **schema digest**, and the schema bodies are re-derived from
  code and verified against those digests. A prompt change still forces a new
  header because the prompt is always an input to `template_digest`. This is
  dsh's own contract — the summarizer request is "reconstructable from log +
  code" (`dsh-compaction/lib/types/types.d.ts:49-51`) — and it is deliberately
  weaker than "reconstructable from pinned data alone": a replay against a
  changed tool registry fails the digest and is reported as a registry mismatch,
  never fabricated. Provider wire bytes are not the target; the canonical form
  is.
  No extension, adapter, or UI may mutate a request after dispatch.
- **26-I3 — One provider attempt per stream; retry is a separate, durable
  executor.** `LLMProvider::stream` remains one attempt; retry is a listener at
  the step boundary and records `llm/retry` before each wait.
- **26-I4 — Streams terminate at the loop boundary.** Every loop turn ends in
  exactly one `TurnEnded`/`TurnCancelled`/`TurnFailed`. A provider normalizes
  throws and iterator failures to a non-`Completed` `LLMResponse`; on
  cancellation the provider may emit **no** terminal sink event, and the loop
  supplies the terminal (`agent_loop.cpp:742-746`). The guarantee is on the
  loop's `LLMResponse`, not on the adapter sink.
- **26-I5 — The prompt is an ordered, deterministic assembly.** Sections sort by
  `(order, name)`, empty sections drop, strict `{{var}}`, one `complete`
  section max, `tool_order` contains exactly one `<unlisted-tools>` rest. The
  prefix is stable; dynamic material appends after it. Enforcement happens in
  `SystemPrompt::assemble`, not in the renderer (§4.3.3).
- **26-I6 — The tool catalog is stable across modes.** A mode change alters only
  its policy section; it never adds or removes tools. Any tool-set change (e.g.
  an MCP server connecting) starts a new request series and is logged.
- **26-I7 — Fail loud over silent degradation.** Unknown prompt variables,
  duplicate section names, `>1 complete`, duplicate adapters, `maxDepth` without
  a depth-capable provider, a `ContextFormed` whose `form` requires a field it
  does not have (Snapshot without `sections`, Notice without `summary`), and
  selecting a reserved-but-unimplemented
  presentation mode all fail at load/assembly. (Matches dsh's "Fail loud, no
  silent degradation", `/tmp/opencode/dsh-collab.md:1802-1804`.)
- **26-I8 — Authorization by ownership, not secrecy.** Jobs, goals, and
  subagents are fenced by session/agent id; ids are predictable. A child's scope
  is fixed and non-widenable.
- **26-I9 — Compaction/pruning are projections with shadow accounting.** Never
  delete; append a replacement that cites the shadowed node and is preceded by
  its shadow-price event.
- **26-I10 — UI is a consumer.** No prompt/LLM/collaboration type may appear in
  the UI layer, and no UI type may enter these seams (`00 §4.1`, `06 A17`).
- **26-I11 — Prompt text is durable only under a separate, explicitly-named
  opt-in.** The session event log is the authoritative trace, but the rendered
  system prompt embeds `AGENTS.md`, the skill catalog, and possibly file content,
  so it is subject to the shipped no-prompt-logging policy
  (`include/ymh/llm/redaction.hpp:4-6`; `include/ymh/config/config.hpp:84-86`).
  The `llm/request_header` event **always** carries `system_prompt_digest`
  (SHA-256 hex of the rendered prompt) plus the invariant config/envelope/tool
  fields (`config`, `turn`, `step`, `session_id`, `purpose`, `tool_names`,
  `tool_schema_digests`, `template_digest`); the full rendered `system_prompt`
  text is written **only** when the dedicated `session.persist_prompt_text` key
  is explicitly set (bool, default `false`, new `[session]` section, **global
  layer only** — a workspace-layer occurrence is a `ConfigError`, so a
  repo-controlled file cannot silently enable durable prompt capture). This key
  governs the **session-DB copy only** and is deliberately **distinct from
  `logging.log_prompts`** (`config.hpp:84-88`), which governs the spdlog/log
  surface and never touches the session DB. Absent the opt-in, replay re-derives
  the prompt from the live registries and verifies it against the digest, and
  never fabricates it. Whether to *ever* enable durable full-prompt persistence
  is a product decision for the user; the mechanism's default is unambiguously
  safe (digest only).
- **26-I12 — The event vocabulary is decoupled from the wire envelope version.**
  A durable event type that crosses the daemon↔supervisor socket is **not** an
  envelope-shape change, so the first wave that emits a new type does **not** bump
  `kProtocolVersion` (`include/ymh/transport/protocol.hpp:153`, stays `1`). The
  receiver tolerates an unknown event `type` on the wire by **skipping** that
  `SessionEnvelope` (it does not call `on_envelope`, and it still advances the
  per-session cursor to the notification's `cursor`) instead of failing the
  connection. The loud forward-fence lives **only** on the durable/on-disk decode
  (`src/session/session_persistence.cpp:228-232` `CorruptionError`; `01` S3), never
  on the wire. A `kProtocolVersion` bump is reserved for a change to the
  **envelope** shape (`SessionEnvelope` = `{session, event}`, `05` T5) or the
  handshake semantics. This is what makes the spec-24 outliving-daemon case work:
  a non-last supervisor's daemon outlives it (`24` §1.3.3/`AL23`/`AL25`), so a
  **new** supervisor regularly attaches to an **old** daemon process; holding the
  version at `1` keeps the handshake matching and the new supervisor understands
  every type the old daemon can emit, whereas a bump would reject the attach with
  `UnsupportedProtocol` (`src/transport/protocol_server.cpp:285-293`).

### 4.2 Decision register

`Add.` = additive to the named verified spec; `Brk.` = breaking (requires an
errata/amendment before code, per `AGENTS.md`). "Owning spec" is where the
errata is gated. "Wave" is the migration wave that implements it (§5). Where a
decision is actually breaking, the register says so even when a prior draft
called it additive.

| ID | Decision | Add./Brk. | Owning spec | Wave |
|---|---|---|---|---|
| 26-D1 | Introduce a provider-neutral `LlmRuntime` service above `LLMProvider`; the loop no longer holds a raw provider. | **Brk. (06, 08)** — replaces the pinned `AgentServices`/provider seam, not an addition beside it | 06, 08 | 1 |
| 26-D2 | Introduce `LlmCallConfig` + `call_config_equals`; log a durable **request-header** event as a *changed snapshot* — at each request-series start and whenever `config`, the rendered prompt, the tool catalog (names or schemas), or `purpose` changes (not on every dispatch); the loop builds requests from the logged header. **Does not persist `RequestId`** (spec 08: "Never persisted", `llm_request.hpp:28-30`); retries correlate by `retryId` + `(turn, step)` and the header is found positionally (last header before the attempt), so no self-referential sequence field is stored. Always carries `system_prompt_digest`; persists the full prompt text only under the dedicated `session.persist_prompt_text` opt-in (D23, `26-I11`). | Add. | 01, 08, 21 | 1 |
| 26-D3 | Freeze the request envelope before dispatch; add `session_id` and an optional `purpose` (`Compaction`/`SessionTitle`; absent = ordinary conversation) to `LLMRequest`, and log both in the request header so the template digest's `envelope` is reconstructable; `prepare_call` binds one adapter generation; pin `FrozenRequest::canonical_template()`/`canonical_json()`. | Add. | 08 | 1 |
| 26-D4 | Introduce the ordered **prompt registry**: `PromptSection`, `PromptContext`, `PromptAssembly`, `assemble()`, `render_prompt()`, and the canonical `SECTION_ORDERS`/`CONTEXT_ORDERS` tables (verbatim). | New | new `27-system-prompt.md` | 3 |
| 26-D5 | Introduce **persona** as sections 0/10200 with `complete` mode; default persona mirrors dsh's shipped text. | Add. | 27 | 3 |
| 26-D6 | Introduce the **workspace-instructions loader** (`AGENTS.md`/`CLAUDE.md`, `.git` root, required `maxBytes`, `<system-reminder>` framing, durable user-role message). | New | 27 | 3 |
| 26-D7 | Introduce **runtime-context snapshots** (sourced user-role, supersession header) distinct from sections; `ContextInjected.role` default changes from `System` to `User` for snapshots. | Brk. (default/payload) | 27, 01 | 3 |
| 26-D8 | Make one canonical **`BlockAssembler`** for text/reasoning/tool-call blocks, with `interrupted_blocks()` and terminal `finish` default. | Add. | 08 | 2 |
| 26-D9 | Embed the **compact assistant stream** and a `ReplayEnvelope` in `assistant/message`; retain non-message attempts as `assistant/attempt`. Legacy durable `AssistantChunk` rows become read-only (never appended to the log by new binaries, never migrated). New binaries still **publish** `AssistantChunk` as a **live-only** stream event (like `McpServerStatusChanged`, `core/event.hpp:72-74`) so live rendering keeps its deltas; see the §4.3.9.2 consumer matrix. | Brk. (assistant payload + event durability split) | 01, 08 | 2 |
| 26-D10 | Replace serial tool execution with a **bounded-parallel scheduler** (exclusive barriers + rolling pool, default 10) and synthetic error results on abort. | Add. | 06 | 4 |
| 26-D11 | Introduce the **retention library** (`Deque`, `ChunkedList`, `ItemRetainer`/`TextRetainer`, `RetentionNotice`) and route tool output through it; `ToolResult` gains omission metadata. | Brk. (spec 07 `ToolResult`/clamp contract) | 07, new `28-output-retention.md` | 4 |
| 26-D12 | Introduce the **tool-result pruner** (8192/4096/1024 **code points**, `PRUNE_MARKER`) with a `context/prune` shadow-price event. | Add. | 13, 01 | 4 |
| 26-D13 | Add compaction **trigger taxonomy** (`pressure`/`context_overflow`), `compact_if_needed`/`compact_now`, shadowed-seq accounting, and summary `provider`/`model` logging; the summarizer routes through `LlmRuntime`. | Brk. (spec 13: "does not add fields to the payload") | 13, 01, 08 | 4 |
| 26-D14 | Add **message provenance** (`MessageSource` + semantic `ContextForm`) and the `SystemMessage`/`ToolResultMessage` specializations. | Brk. (struct/codec) | 01, 17, 27 | 3 |
| 26-D15 | Add **tool presentation mode `Native`** (each visible schema), per-tool guidance sections, and `toolOrder` canonicalization. `Ptc`/`Both` are reserved enum values that fail loud if selected; the PTC SDK generator is **D22 (deferred)**. | Add. | 07, 27 | 3 |
| 26-D16 | Add **presets / per-session composition** with a standing mount + per-session joined scopes, blank-session-only switching, and `agent_preset/selected`. | Brk. (session header) | new `29-agent-presets.md`, 01, 23 | 5 |
| 26-D17 | Add **subagent composition inheritance**, depth, fixed delegation scope, and continuable control tools. | Add. | 06, 29 | 5 |
| 26-D18 | Add **goals** (durable objective, phases, round driver, `/goal`). | New | new `30-goals-jobs-commands.md`, 01 | 6 |
| 26-D19 | Add **background jobs** (owner-scoped registry, `job_output`/`job_list`/`job_kill`, wakeup policy). | New | 30, 01 | 6 |
| 26-D20 | Add the **log-only command surface** (`command/run`/`command/done`, agent-scoped shadowing). | Add. | 30, 01 | 6 |
| 26-D21 | Add **repeat-tool reminders** (thresholds `[3,5,8]`) and formalize step steering. | Add. | 06 | 4 |
| 26-D22 | **Deferred:** PTC `run_code` presentation + generated SDK. Not implemented; the `Ptc`/`Both` enum values exist only to fail loud. | New (deferred) | new `27` | — |
| 26-D23 | **Prompt-text persistence policy:** the `llm/request_header` event always carries `system_prompt_digest`; the full rendered `system_prompt` text is stored **only** under the dedicated `session.persist_prompt_text` key (bool, default `false`, new `[session]` section, **global layer only**) — a session-DB opt-in deliberately distinct from `logging.log_prompts` (which governs spdlog only and never the session DB). The default durable record is the digest; reconstruction re-derives the prompt and verifies the digest when the text is absent (`26-I11`). Whether to ever enable it is a user product decision; the mechanism is safe by default. | Add. (new `21` key) | 01, 08, 21 | 1 |
| 26-D24 | **Event-vocabulary decoupling (no version bump):** keep `kProtocolVersion` at `1`; the wire receiver skips an unknown event `type` (cursor still advances) instead of failing the connection, while the on-disk/replay decode keeps its loud `CorruptionError` fence. A `kProtocolVersion` bump is reserved for an envelope-shape or handshake change, not for a new event type (`26-I12`). | Add. (`01` wire-vocabulary rule) | 01 | 1 |

### 4.3 Interface sketches (C++23)

These are sketches, not final headers. The owning spec pins exact names and the
component gate must pass before code, per `AGENTS.md`. Every new type this file
relies on is defined here or already exists in the tree with a cited
`file:line`; a type named only in prose is a defect, and a Wave-0 owning spec may
refine a name only through its own errata.

#### 4.3.1 `LlmRuntime` and the frozen request (26-D1, D3)

```cpp
namespace ymh {

// Provider-neutral call configuration. `max_tokens` is the single canonical
// sampling-budget name; the existing GenerationParameters::max_output_tokens
// maps onto it 1:1 in exactly one place (buildRequest), so there is one source
// of truth at dispatch (see §4.9, and open question 3).
struct LlmCallConfig {
    ProviderId                   provider;
    ModelId                      model;
    std::optional<std::string>   reasoning_effort;
    std::optional<double>        temperature;
    std::optional<std::uint32_t> max_tokens;
    std::vector<std::string>     stop;
    // ymh extensions beyond dsh's 6-field config (dsh dropped these): the
    // OpenAI-compatible adapter still emits them on the wire
    // (openai_adapter.cpp:736-752), so they are logged and covered by
    // canonical_json() (N-L2). Removed from both together if ever dropped.
    std::optional<double>        top_p;
    std::optional<std::uint32_t> seed;
    std::optional<std::string>   tool_choice;
};

// dsh GenerateOptions.purpose (types.d.ts:438-443): auxiliary-call
// classification. Absent = an ordinary conversation request; an adapter may map
// it to model-hidden transport metadata. Pinned values: Compaction,
// SessionTitle (dsh's 'compaction' | 'session-title'). D3 adds
// `SessionId session_id` and `std::optional<CallPurpose> purpose` to
// `LLMRequest`; ordinary requests leave `purpose` unset.
enum class CallPurpose : std::uint8_t { Compaction, SessionTitle };

// The ONLY change test. No defaulted operator<=>/operator== is declared:
// call_config_equals is the documented semantic (dsh callConfigEquals), so
// there is no second, subtly different equality.
[[nodiscard]] bool call_config_equals(const LlmCallConfig&, const LlmCallConfig&) noexcept;

// Immutable, detached request envelope. `freeze` deep-copies and pins the
// canonical serialization; nothing may mutate after freeze.
class FrozenRequest {
public:
    [[nodiscard]] static FrozenRequest freeze(LLMRequest request, LlmCallConfig config);
    [[nodiscard]] const LLMRequest&   get() const noexcept;
    [[nodiscard]] const LlmCallConfig& config() const noexcept;

    // Pinned canonical serialization (contract in §4.3.2): deterministic JSON,
    // sorted object keys, canonical tool schemas, config. Stable across runs,
    // platforms, and builds.
    // The TEMPLATE is the request-invariant part (schema_version, envelope,
    // config, system prompt, tools); its digest is what the logged header
    // carries, because the header is a changed snapshot and is not re-logged
    // per dispatch. The full JSON adds the ordered `messages` of one dispatch
    // and is what the determinism half of the harness compares.
    [[nodiscard]] std::string canonical_template() const;
    [[nodiscard]] std::string template_digest() const;  // SHA-256 hex of canonical_template()
    [[nodiscard]] std::string canonical_json() const;   // template + messages
    [[nodiscard]] std::string digest() const;           // SHA-256 hex of canonical_json()
private:
    LLMRequest   request_;
    LlmCallConfig config_;
};

// One model call whose config and adapter registration were resolved together.
// One-shot: `stream` consumes the object; a second call or a config mismatch
// throws PreparedCallError (§4.7). Non-const for that reason.
class PreparedCall {
public:
    [[nodiscard]] const LlmCallConfig& config() const noexcept;
    [[nodiscard]] const RetryPolicy&   retry_policy() const noexcept;
    [[nodiscard]] Task<LLMResponse> stream(FrozenRequest request,
                                           StreamSink sink,
                                           CancellationToken cancel);
private:
    bool consumed_ = false;
};

// Registration/discovery types (previously undefined).
struct ProviderInfo {
    ProviderId                id;
    std::vector<ModelId>      models;
    ProviderCapabilities       capabilities;
};
class AdapterHandle {                 // RAII; move-only; destructor unregisters
public:
    AdapterHandle(AdapterHandle&&) noexcept;
    AdapterHandle& operator=(AdapterHandle&&) noexcept;
    ~AdapterHandle();
    AdapterHandle(const AdapterHandle&) = delete;
};

// The `llm/stream` waterfall analogue: a registered interceptor wraps the next
// call. Registration returns an RAII handle; ordering is registration order.
using StreamNext        = std::function<Task<LLMResponse>(const FrozenRequest&, StreamSink, CancellationToken)>;
using StreamInterceptor = std::function<Task<LLMResponse>(const FrozenRequest&, StreamSink, CancellationToken, StreamNext)>;
class InterceptorHandle { /* RAII; move-only; destructor unregisters */ };

// The provider-neutral service. The loop holds an LlmRuntime&, not an
// LLMProvider*. Adapters are owned by shared_ptr: the runtime keeps the
// registration alive until every in-flight dispatch that captured it has
// finished, so destroying an AdapterHandle removes the route but cannot free an
// adapter still being dispatched (§4.8). Registry mutations and interceptor
// registration are mutex-guarded; stream/prepare_call copy what they need under
// the lock and then release it.
class LlmRuntime {
public:
    AdapterHandle     register_adapter(std::vector<ProviderId> routes,
                                       std::shared_ptr<LLMProvider> adapter);
    [[nodiscard]] std::vector<ProviderInfo> list_providers() const;
    InterceptorHandle add_stream_interceptor(StreamInterceptor);
    [[nodiscard]] Task<PreparedCall> prepare_call(LlmCallConfig, CancellationToken) const;
    [[nodiscard]] Task<LLMResponse>  stream(const FrozenRequest&, StreamSink, CancellationToken);
};

} // namespace ymh
```

`PreparedCall::stream` is non-`const` (T-M1): the one-shot consume-and-mismatch
semantics require mutable state. The mismatch error is `PreparedCallError`
(§4.7), mapped at the loop boundary.

#### 4.3.2 The logged request header (26-D2) and the reconstruction contract

New session event (additive to the `SessionEventMap` in
`include/ymh/session/events.hpp`). **`RequestId` is deliberately absent**: spec
08 pins it "Never persisted" (`llm_request.hpp:28-30`). Retries correlate by
`retryId` + `(turn, step)` (part 1 §2.1.7); a header is found **positionally**
(the last header before an attempt), so no self-referential sequence field is
stored.

```cpp
namespace payload {

// A changed snapshot, not a per-request record: logged at a request-series
// start and whenever any header field changes (call_config_equals == false, the
// tool catalog changes (names or schemas), the rendered prompt changes, or
// `purpose` changes).
// This is what makes the request TEMPLATE reconstructable (from the log plus
// the live registries; 26-I2) and detects silent per-call drift. It does NOT
// carry a per-request digest: messages grow every step, so a digest over them
// would be stale the moment the next step is appended.
struct LlmRequestHeader {
    TurnId                   turn = 0;
    StepId                   step = 0;
    SessionId                session_id;       // envelope half; also implicit in the owning session
    std::optional<CallPurpose> purpose;        // envelope half; absent == ordinary conversation
    LlmCallConfig            config;           // provider/model/effort/sampling
    std::string              system_prompt_digest; // SHA-256 hex of the rendered prompt (always)
    std::optional<std::string> system_prompt;  // full text ONLY under session.persist_prompt_text (26-I11)
    std::vector<std::string> tool_names;       // canonical order
    std::vector<std::string> tool_schema_digests;  // per-tool SHA-256 of canonical schema JSON
    std::string              template_digest;  // SHA-256 of FrozenRequest::canonical_template()
    bool                     starts_series = true;  // true iff config/prompt/tools changed (KV boundary)
};

} // namespace payload
```

**Loop rule.** `buildRequest()` derives `LlmCallConfig` from the last
`LlmRequestHeader`; a proposed change is applied only if
`!call_config_equals(proposed, held)`, in which case a new header is logged with
`starts_series = true`. A tool-catalog (names or schemas) or rendered-prompt
change also forces a new
header and a new series; a `purpose` change forces a new header with
`starts_series = false` unless it also changed config/prompt/tools. No header is
logged for an unchanged template, and none is needed: the messages of each
dispatch are the append-only message events. This is the direct analogue of
`call-config.d.ts:1-6,33-40`.

**Reconstruction contract (26-I2).** `canonical_template()` is a deterministic
JSON object:
`schema_version`, `envelope` (`session_id` + optional `purpose`; D3 — adapters
may map both to transport metadata, so they are covered by the digest),
`config` (every `LlmCallConfig` field that can reach the
provider wire, including the `top_p`/`seed`/`tool_choice` extensions; §4.9),
`system_prompt` (the full rendered text held in memory — always hashed into
`template_digest`, but persisted in the header only when
`session.persist_prompt_text` is set; `26-I11`/D23), and `tools` (each schema's
`name`, `description`, `parameters` with recursively sorted keys).
`canonical_json()` is the same object plus `messages` (role + ordered content
blocks; tool-call arguments as their raw JSON string). No
timestamps, ids, or iteration-order-dependent data.
Replay rebuilds the **template** from the last header (re-deriving the tool
schemas from the live `ToolRegistry` and verifying them against
`tool_schema_digests`) and asserts
`rebuild.template_digest() == header.template_digest`; it reconstructs the
attempt's messages from the logged message events and asserts that two rebuilds
of `canonical_json()` are byte-identical. Nothing is fabricated: a replay that
finds no header for an attempt marks it **legacy / unreconstructable** (§4.6),
and a replay whose re-derived schemas do not match the digests fails loud rather
than guessing.

**Prompt-text policy (D23, `26-I11`).** The header always records
`system_prompt_digest` plus the invariant config/envelope/tool fields. The full
`system_prompt` text is present only when `session.persist_prompt_text` — a
dedicated, explicitly-named session-DB opt-in (bool, default `false`, new
`[session]` section, **global layer only**) — is set; otherwise replay
re-derives the prompt from the live registries, asserts `sha256(rendered) ==
header.system_prompt_digest`, and fails loud on a mismatch. When the text is
present it must be byte-identical to the in-memory rendered prompt, and replay
additionally asserts that equality; the digest check still runs, so the
reconstruction contract is unchanged whether or not the text is stored.

This opt-in governs the **session-DB copy only** and is deliberately **distinct
from `logging.log_prompts`** (`config.hpp:84-88`, `config.cpp:787`), whose
shipped contract is (a) off by default and (b) prompt bodies redacted even when
on (`redact_secrets`, `logging.cpp:176`, `redaction.cpp:52`); that key governs
the spdlog/log surface only (`logging.cpp:97`, `cli.cpp:1137`), shares no code
path with the event log, and is **not** reused here. Keeping the two keys
separate means the config comment's "redacted even when it is on" promise is
never contradicted: the session-DB key has its own name and its own documented
semantics (store the rendered text byte-for-byte; no redaction).

**Mechanism vs policy.** This pins the *mechanism* for durable prompt
persistence; whether to ever enable it remains a product decision for the user
(the same class of decision as the no-downgrade one-way door, §4.6). The
mechanism's default is unambiguously safe: a default install persists only
`system_prompt_digest`, so the no-prompt-logging rule (`redaction.hpp:4-6`;
`config.hpp:84-86`) is honoured without a second thought.

#### 4.3.3 The prompt registry (26-D4, D5, D6, D7, D15)

```cpp
namespace ymh {

// Merge-extensible context for one assembly (dsh AssembleContext).
struct AssembleContext {
    std::optional<ScopeKey> scope;    // absent => global layer only
    CancellationToken*      signal = nullptr;
};

// Registry inputs.
struct PromptSection {
    std::string name;      // unique within a layer; duplicate throws
    std::int32_t order = 0;// ascending; ties by code-unit name
    std::function<std::string(const AssembleContext&)> text;  // "" drops
    bool complete = false; // at most one effective; >1 fails assemble()
};
struct PromptContext {
    std::string name;
    std::int32_t order = 0;
    std::function<std::string(const AssembleContext&)> text;  // "" contributes nothing
};

// Assembly outputs. `complete` and `tool_order` live here so assemble() can
// enforce the singleton/rest rules; render_prompt only renders a validated
// assembly (T-M4).
struct AssembledSection { std::string name; std::string text; bool complete = false; };
struct AssembledContext { std::string name; std::string text; };
struct PromptAssembly {
    std::vector<AssembledSection> sections;
    std::vector<AssembledContext> contexts;
    std::vector<ToolSchema>       tools;        // canonical order
    std::map<std::string, std::optional<std::string>> variables;
    std::vector<std::string>      tool_order;   // exactly one "<unlisted-tools>" rest
};

// RAII registrations (dsh returns `() => void` disposers). Move-only;
// destructor unregisters. SystemPrompt owns the registry; handles must not
// outlive it.
class SectionHandle { public: ~SectionHandle(); SectionHandle(SectionHandle&&) noexcept;
                      SectionHandle(const SectionHandle&) = delete; };
class ContextHandle { public: ~ContextHandle(); ContextHandle(ContextHandle&&) noexcept;
                      ContextHandle(const ContextHandle&) = delete; };

// Single-threaded: owned by WorkspaceRuntime, called only on the agent
// executor thread. Not thread-safe by design; the executor serializes assembly.
class SystemPrompt {
public:
    SectionHandle section(PromptSection);
    ContextHandle context(PromptContext);
    // Merge global+scope (scope shadows same name), canonicalize by (order,name),
    // run the assemble waterfall, restore one complete section, validate
    // tool_order. Fail-loud: duplicate name, >1 complete, missing/duplicate rest.
    [[nodiscard]] PromptAssembly assemble(const AssembleContext&) const;
    [[nodiscard]] std::string render(const AssembleContext&) const;  // assemble + render_prompt
};

// Strict interpolation: unknown/malformed {{var}} throws; drop empty sections;
// join with "\n\n". Assumes an assembly already validated by assemble().
[[nodiscard]] std::string render_prompt(const PromptAssembly&);

} // namespace ymh
```

`SystemPrompt::assemble` is the output path to the loop (T-H5): the loop
consumes `assemble(ctx).tools` for the request's tool list, `.variables` for
rendering, and `render(ctx)` for the system message. A registry that only
returned a `std::string` could not drive a request; this one does.

Persona (`27`):

```cpp
struct PersonaConfig {
    std::string prefix;                     // required
    std::string suffix;                     // default ""
    bool        complete = false;           // prefix becomes the whole prompt
    bool        include_runtime_context = true;
};
// Registers `deployment:persona-prefix` (order 0, complete iff configured)
// and `deployment:persona-suffix` (order 10200); shadows deployment defaults
// for the mounted scope. Mounting globally is a load error.
```

Workspace instructions (`27`). `max_bytes` is **required** when the loader is
enabled (no default), matching dsh; the standard preset supplies 65536. The
loader's rule follows dsh's field semantics
(`dsh-agent-instructions/README.md:64-66`): user-global first, then for each
directory from root to cwd, **every present candidate** in `candidates` order
(`AGENTS.md` then `CLAUDE.md`; dsh's candidates are base file names "loaded in
each project directory", not first-match-wins), then `local_candidates` in order
when `load_local` is true, appended after the base files and never shadowing
them. (Rev 2's "`AGENTS.md` if present else `CLAUDE.md`" was a first-match rule
with no dsh basis; it is removed.)

```cpp
struct InstructionFileConfig {
    std::vector<std::string> project_root_markers{".git"};
    std::vector<std::string> candidates{"AGENTS.md", "CLAUDE.md"};
    std::vector<std::string> local_candidates{"AGENTS.local.md", "CLAUDE.local.md"};
    bool                     load_local = false;
    std::size_t              max_bytes;         // REQUIRED when enabled; no default
    std::size_t              max_source_bytes = 1048576;
};
// Loads user-global first, then root->cwd broad-to-specific; broader files
// omitted before the most-specific is truncated; appends a durable user-role
// message with the verbatim framing (part 1 §2.3.5).
```

Runtime contexts (`27`) append a sourced **user-role** snapshot under the
verbatim header `Current runtime context. This snapshot supersedes earlier
runtime-context snapshots.` This is why `26-D7` reclassifies: the existing
`ContextInjected.role` default is `Role::System` (`events.hpp:155`), the
opposite of dsh's user-role snapshot (T-M14).

Tool presentation (`26-D15`):

```cpp
enum class ToolPresentationMode : std::uint8_t { Native, Ptc, Both };
// Native: each visible schema. Ptc/Both are RESERVED and fail loud at load
// (26-I7); the PTC SDK generator is D22 (deferred). Guidance sections register
// at TOOL_* orders and are suppressed when the tool is restricted away.
// toolOrder canonicalizes the model-facing list; unlisted tools are inserted
// lexicographically at the single "<unlisted-tools>" rest.
```

#### 4.3.4 Canonical assembler + assistant stream (26-D8, D9)

```cpp
namespace ymh {

// Adapter-private, lossless-JSON replay state carried by a successful terminal
// `finish` (dsh ReplayEnvelope). DELIBERATE DIVERGENCE from dsh's shape
// (`dsh-llm/lib/types/types.d.ts:331-350`: `{ response: unknown; blocks?: ... }`):
// ymh wraps it as `{ provider, version, state }` so the producer is identified
// and the payload is versioned; `state` is opaque to ymh. Empty when the
// adapter has no replay state.
struct ReplayEnvelope {
    std::string    provider;
    std::uint32_t  version = 1;
    nlohmann::json state;              // adapter-private; must be JSON-serializable
};

// A stream event with the wall-clock offset at which the sink received it.
struct TimedStreamEvent {
    std::chrono::milliseconds at{0};
    StreamEvent               event;
};

// Single canonical chunk->message algorithm (mirrors dsh BlockAssembler).
class BlockAssembler {
public:
    void push(const StreamEvent&);                       // in stream order
    [[nodiscard]] std::vector<ContentBlock> blocks() const;
    // Prefix an interrupted stream can safely finalize; tool calls are dropped
    // because interruption precedes dispatch.
    [[nodiscard]] std::vector<ContentBlock> interrupted_blocks() const;
    [[nodiscard]] std::optional<Usage> usage() const;    // Finished.usage authoritative; UsageEvent advisory
    [[nodiscard]] FinishReason finish() const;           // {Stop} if no Finished
    [[nodiscard]] std::optional<ReplayEnvelope> replay_state() const;
};

// One packed delta run (dsh AssistantStreamRun): deltas are packed without
// joining their boundaries; `time0_ms` plus the `dt_ms` gaps reconstruct each
// delta's original timestamp. Only delta chunks are packed — every non-delta
// chunk is a separate raw `ChunkRecord`, so there is **no** second full raw list
// (dsh assistant-stream.d.ts:16-49).
struct TextRun      { std::size_t index; std::int64_t time0_ms; std::vector<std::int64_t> dt_ms; std::vector<std::string> texts; };
struct ReasoningRun { std::size_t index; std::int64_t time0_ms; std::vector<std::int64_t> dt_ms; std::vector<std::string> texts; };
struct ToolCallRun  { std::size_t index; std::int64_t time0_ms; std::vector<std::int64_t> dt_ms;
                      ToolCallId id; std::optional<std::string> name; std::vector<std::string> args; };
struct ChunkRecord  { std::int64_t time_ms; StreamEvent event; };  // non-delta chunk verbatim

// The compact attempt stream is a discriminated union (dsh AssistantStreamRecord),
// not a struct that keeps both a packed and a raw copy.
using AssistantStreamRecord = std::variant<TextRun, ReasoningRun, ToolCallRun, ChunkRecord>;

class AssistantStreamAccumulator {
public:
    // Returns the detached immutable timed event for live publication, without
    // racing the accumulator. Deliberate divergence: dsh returns a
    // `TimedStreamChunk` (its chunk vocabulary); ymh returns a `TimedStreamEvent`
    // because the sink and the accumulator speak `StreamEvent` (T-L4).
    [[nodiscard]] TimedStreamEvent push(const TimedStreamEvent&);
    // The detached immutable record list embedded in a durable attempt event.
    [[nodiscard]] std::vector<AssistantStreamRecord> snapshot() const;
};

// Validating expansion of compact records back to the exact timed chunk sequence.
[[nodiscard]] std::vector<TimedStreamEvent> expand(const std::vector<AssistantStreamRecord>&);

} // namespace ymh
```

**Stream-algebra changes required by D9 (T-H3).** The `Finished` stream event
(`stream.hpp:159-162`) gains `std::optional<ReplayEnvelope> replay_state` (the
transient carrier from the adapter to the assembler; dsh's `finish` chunk carries
`replayState`, `dsh-llm/lib/types/types.d.ts:352-389`). The
`payload::AssistantMessage` event gains `std::vector<AssistantStreamRecord>
stream` and `std::optional<ReplayEnvelope> replay_state`, so the assembled
message carries its exact compact attempt stream and its replay state (dsh
`assistant/message` embeds `stream`: `dsh-session/lib/types/types.d.ts:309-317`).
The event is the **single durable home** for replay state: `MessageSource` does
not repeat it (§4.3.10). This is a stated divergence from dsh, which stores it on
the model source; ymh keeps it on the settlement event because the event is the
settlement authority and the source lands in a later wave. `ToolCallFinished.call`
(the parsed object) is authoritative; `arguments_fragment` is the raw mirror
(T-L5).

**Settlement model (T-H4, dsh-aligned).** dsh commits exactly one durable event
per settled attempt: `assistant/message` carries the assembled message **and its
compact timed stream**, while `assistant/attempt` retains a failed, retried,
cancelled, or stream-error attempt that committed no model-visible history
(`dsh-session/README.md:84`, `types.d.ts:309-327`). ymh follows that model:
`AssistantMessage` embeds the compact stream; a new `assistant/attempt` event
retains non-message attempts. This removes Rev 2's third representation (a
standalone `assistant/stream` event). Legacy `AssistantChunk` events stay
readable forever but are **not appended by new binaries** (§4.6).

#### 4.3.5 Bounded-parallel tool scheduling (26-D10)

```cpp
struct ToolScheduleConfig {
    std::size_t max_parallel_tool_calls = 10;   // dsh DEFAULT_MAX_PARALLEL_TOOL_CALLS
};

// One assistant step's committed result set. `results` is model-ordered.
struct ToolScheduleOutcome {
    std::vector<ToolResult> results;
    bool                    aborted = false;    // true iff cancellation/abort drained the step
};

// Accepts a context contribution produced by a tool; returns false to reject
// (full/duplicate). Keeps the scheduler free of ContextAssembler details.
using ContextAcceptor = std::function<bool(const ContextMessage&)>;

// Exclusive calls form barriers; parallel-safe calls use a bounded rolling
// pool. Policy, results, and result context remain model-ordered. Abort drains
// started calls and records synthetic error results for unstarted calls so
// replay stays valid.
Task<ToolScheduleOutcome> execute_tool_calls(AgentLoop&, TurnId, StepId,
                                             std::vector<ToolCallAssembled>,
                                             CancellationToken,
                                             ContextAcceptor);
```

#### 4.3.6 Retention library (26-D11)

```cpp
// dsh dsh-deque: circular, amortized O(1) push/pop at both ends; removed
// entries are cleared immediately and storage shrinks at a quarter live
// capacity. (Spec 28 owns the exact representation; this is the pinned shape.)
template <class T> class Deque {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    void                       push_back(T value);
    void                       push_front(T value);
    [[nodiscard]] std::optional<T> pop_front();   // nullopt iff empty
    void                       clear() noexcept;
};

// dsh dsh-chunked-list: persistent append-only list; appending copies at most
// one 64-value chunk and shares the unchanged older chunks. Empty is `nullopt`.
template <class T> struct ChunkedList {
    static constexpr std::size_t kChunkSize = 64;
    std::vector<T>                values;             // newest chunk, insertion order
    std::shared_ptr<const ChunkedList<T>> previous;   // absent == oldest chunk
};
template <class T>
[[nodiscard]] ChunkedList<T> append_chunked_list(std::optional<ChunkedList<T>> head, T value);
template <class T>
[[nodiscard]] std::vector<T> iterate_chunked_list(const std::optional<ChunkedList<T>>& head);

// Omission metadata. `count` is meaningful only for `Exact`; dsh is a
// discriminated union and ymh keeps the discriminant explicit (T-L3).
enum class OmittedKind : std::uint8_t { None, Exact, Unknown };
struct Omitted { OmittedKind kind = OmittedKind::None; std::size_t count = 0; };

struct PushDecision { bool accepted = true; Omitted omitted; };
template <class T> struct RetainedItems { std::vector<T> items; Omitted omitted; };
struct RetainedText { std::string text; Omitted omitted; };
enum class TextRetentionStrategy : std::uint8_t { Head, Tail, HeadTail };
struct RetentionNotice { Omitted omitted; std::vector<std::string> omitted_labels; };
using RecoveryTextFn = std::function<std::string(const RetentionNotice&)>;

template <class T> class ItemRetainer {     // head-only in v1
public:
    explicit ItemRetainer(std::size_t max_items);
    PushDecision push(T);
    [[nodiscard]] RetainedItems<T> finish() const;
};
class TextRetainer {                        // head / tail / headTail, byte-oriented
public:
    explicit TextRetainer(TextRetentionStrategy);
    PushDecision push(std::span<const std::byte>);   // UTF-8-boundary safe at finish
    [[nodiscard]] RetainedText finish() const;
};
[[nodiscard]] std::string format_retention_notice(const RetentionNotice&, RecoveryTextFn);
```

**Mapping onto `ToolResult` (spec 07, breaking).** The retained text becomes
`ToolResult.output`; the notice is appended to `output`; `ToolResult.truncated`
becomes `omitted.kind != OmittedKind::None`; and `ToolResult` gains
`OmittedKind omitted_kind` and `std::size_t omitted_count` so replay can recover
exact omission facts. The serialized-size clamp in spec 07
(`07-tools-execution.md:1127-1135,1147-1149`) is replaced by the retainer for
all tool call sites; this is why D11 is `Brk.` and `07` is in the Wave 0 gate.

#### 4.3.7 Tool-result pruner and compaction triggers (26-D12, D13)

```cpp
// Budgets are counted in Unicode code points (dsh codePointLength), NOT bytes.
struct ToolResultPruneConfig {
    std::size_t threshold_code_points = 8192;
    std::size_t head_code_points      = 4096;
    std::size_t tail_code_points      = 1024;
};
inline constexpr std::string_view kPruneMarker =
    "\n\n[... tool result middle pruned ...]\n\n";   // dsh PRUNE_MARKER, verbatim

struct PruneResult {
    std::size_t             pruned = 0;
    std::vector<Sequence>   replacements;   // seqs of the appended replacement events
};

// Replay-safe, model-free: appends a replacement tool-result event that cites
// the shadowed node, immediately preceded by a `context/prune` shadow-price
// event. Never deletes.
class ToolResultPruner {
public:
    [[nodiscard]] PruneResult prune_session(Session&);
};

enum class CompactionTrigger : std::uint8_t { Pressure, ContextOverflow };

// Existing ContextCompactor gains the two entry points. Its constructor is
// RE-SEAMED from `LLMProvider&` to `LlmRuntime&` so the summarizer request
// routes through the service like every other call (T-M9); `compactor.hpp:113`
// currently takes `LLMProvider&`, and Wave 1 removes raw providers from the
// loop, so the seam must move in Wave 1 and the entry points land in Wave 4.
Task<std::optional<CompactionResult>> compact_if_needed(CompactionTrigger, CancellationToken);
Task<CompactionResult>                compact_now(CancellationToken);
```

#### 4.3.8 Presets and collaboration (26-D16..D21)

These are sketches only; the owning specs (`29`, `30`) pin them. All previously
undefined types are named here.

```cpp
// Stable ids and shared value types.
using GoalId   = std::uint64_t;
using ScopeKey = std::string;                       // preset mount scope key
struct ToolRestriction { std::vector<std::string> allow; std::vector<std::string> deny; };
struct GoalBlockReason { std::string code; std::string message; };
struct GoalRef { GoalId id = 0; std::uint64_t revision = 0; };  // dsh GoalRef
struct AgentContext { AgentId agent; ScopeKey scope; };   // handle passed to the roster

// 29: a preset selects tools, prompt sections, skills, and persona for one
// session; one standing mount per preset, per-session joined scopes.
struct AgentPreset { std::string id; std::string display_name; /* rows */ };
class AgentPresetRoster {
public:
    [[nodiscard]] const AgentPreset& resolve(std::optional<std::string> id) const;
    void mount(AgentContext&, std::optional<std::string> id);
    // Child joins parent's LIVE composition; returns the preset id for history.
    [[nodiscard]] std::string compose_from(AgentContext& child, AgentContext& parent);
    // Empty-session-only switch; appends `agent_preset/selected` after commit.
    void select(Agent&, const std::string& preset);
};

// 29: applied in ONE call so "child with no join" is unrepresentable.
struct ChildComposition {
    std::optional<std::string>   persona;
    std::optional<ToolRestriction> tool_filter;
};
void apply_child_composition(AgentContext& child, Agent& parent,
                             const ChildComposition&);

// 30: goals
enum class GoalPhase : std::uint8_t { Active, Paused, Blocked, Complete };
struct GoalSnapshot { GoalId id; std::uint64_t revision; std::string objective;
                      GoalPhase phase; std::optional<GoalBlockReason> blocked;
                      std::uint32_t max_goal_rounds; };

// 30: jobs
enum class JobStatus : std::uint8_t { Running, Stopping, Completed, Killed, Failed };
struct Job { std::string kind; std::uint64_t ordinal; JobStatus status; std::string label; };

// 30: commands — handler-facing input/outcome; the durable events are in §4.3.9.
using CommandId = std::uint64_t;
struct CommandInput {
    std::string name;
    std::string raw_input;                 // verbatim, separator whitespace included
    SessionId   session;
    AgentId     agent;
};
enum class CommandOutcomeKind : std::uint8_t { Success, Error };
struct CommandOutcome {
    CommandOutcomeKind kind = CommandOutcomeKind::Success;
    std::string        text;               // rendered outcome; error text on Error
};
using CommandHandler = std::function<CommandOutcome(const CommandInput&, Agent&)>;
struct CommandSpec { std::string name; std::string description;
                     std::optional<std::string> input_hint; CommandHandler handler; };
```

#### 4.3.9 New session events: wire names and codecs (T-M10)

"Additive" understates the surface. Every new event touches **two** layers:

1. **The codec**, in `include/ymh/core/event.hpp`: the `EventType` enum
   (`:51-75`), `wire_name` (`:80`), `parse_event_type` (`:83`),
   `all_event_types` (`:87`), the `SessionEventMap<Type>` specialization
   (`:118`), the `EventTraits<Payload>` specialization (`:129`), and the
   payload's `to_json`/`from_json`.
2. **The consumers.** There are exactly **70 `case EventType::` labels** across
   five translation units. One of them has **no `default:`**, so a new
   enumerator without a case is a hard `-Werror=switch` build break; the other
   four have a `default:`, so a missing case is a **silent drop**:

   | Consumer | EventType switch(es) | Cases | `default:` | Failure if not updated |
   |---|---|---|---|---|
   | `src/session/session.cpp` (`deriveMessages`, `:375`) | 1 | 22 | **none** | **hard build break** (`-Werror=switch`) |
   | `src/ui/ui_event_adapter.cpp` | 3 (`:60`,`:84`,`:205`) | 25 | yes (3×) | silent drop of live/transcript rows |
   | `src/ui/session_export.cpp` | 1 (`:200`) | 9 | yes | silent drop in export |
   | `src/cli/session_cli.cpp` | 1 (`:91`) | 8 | yes | silent drop in CLI output |
   | `src/cli/headless.cpp` | 1 (`:196`) | 6 | yes | silent drop in `ymh run` output |

The table below pins the event types and wire names; the payload structs and
their JSON key schemas are pinned immediately after it (§4.3.9.1), and the
per-type consumer obligations are pinned in §4.3.9.2. The `to_json`/`from_json`
bodies are mechanical transcriptions of those key schemas, owned by the `01`
errata. **The `01` errata must add a case to every one of the five switches for
every new durable type** — including the projection switch, which has no
`default:` and therefore gates the build.

These wire names use ymh's existing slash/underscore convention (`01 §4.3`), not
dsh's hyphenated names (`llm/retry-started`, `agent-preset/selected`,
`compaction/prune`); the divergence is deliberate and only the event
*semantics* are copied.

| EventType | `wire_name` | Payload | Notes |
|---|---|---|---|
| `LlmRequestHeader` | `llm/request_header` | `payload::LlmRequestHeader` | §4.3.2; no `RequestId` |
| `LlmRetry` | `llm/retry` | `payload::LlmRetry` | `mode`-discriminated union (part 1 §2.1.7) |
| `LlmRetryStarted` | `llm/retry_started` | `payload::LlmRetryStarted` | `{retryId,turn,step,retry}` |
| `AssistantAttempt` | `assistant/attempt` | `payload::AssistantAttempt` | failed/cancelled/retried attempt; compact records. `AssistantMessage` gains the embedded `stream` (no separate stream event) |
| `ContextPrune` | `context/prune` | `payload::ContextPrune` | shadow price; precedes replacement |
| `AgentPresetSelected` | `agent_preset/selected` | `payload::AgentPresetSelected` | appended after commit |
| `CommandRun` | `command/run` | `payload::CommandRun` | log-only, standalone append |
| `CommandDone` | `command/done` | `payload::CommandDone` | log-only, standalone append |
| `GoalChange` | `goal/change` | `payload::GoalChange` | durable goal mutation (dsh session event `goal/change`: `GoalChangeMeta`); compare-and-set on `{id,revision}` |
| `JobChanged` | `job/changed` | `payload::JobChanged` | owner-scoped |

```cpp
namespace payload {

// Retry correlation id (part 1 §2.1.7; dsh RetryId).
using RetryId = std::uint64_t;

// dsh LlmRetryEventData (dsh-llm-retry types.d.ts:13-34): a mode-discriminated
// union. `max_retries` is present ONLY on `normal`; every other field required.
struct LlmRetry {
    enum class Mode : std::uint8_t { Normal, Always };
    RetryId                      retry_id = 0;
    TurnId                       turn = 0;
    StepId                       step = 0;
    ProviderId                   provider;
    Mode                         mode = Mode::Normal;
    std::string                  policy_key;
    std::uint32_t                retry = 0;
    std::optional<std::uint32_t> max_retries;   // Mode::Normal only
    std::chrono::milliseconds    delay{0};
    LLMError                     failure;
};

struct LlmRetryStarted { RetryId retry_id = 0; TurnId turn = 0; StepId step = 0; std::uint32_t retry = 0; };

// Failed/retried/cancelled/stream-error attempt that committed no message.
struct AssistantAttempt {
    TurnId                             turn = 0;
    StepId                             step = 0;
    std::vector<AssistantStreamRecord> stream;
};

// dsh compaction/prune shadow price (dsh-compaction types.d.ts:88-98). Precedes
// the replacement synchronously; never deletes.
struct ContextPrune {
    Sequence              shadowed_start = 0;
    Sequence              shadowed_end = 0;
    std::vector<Sequence> shadowed_seqs;
    std::uint64_t         shadowed_token_count = 0;
};

// dsh agent-preset/selected (dsh-agent-presets session.d.ts:25-29).
struct AgentPresetSelected { std::string agent_preset; };

// dsh command/run + command/done (dsh-commands types.d.ts:99-124). Log-only.
// dsh's CommandSource has only `user` (merge-extensible, types.d.ts:69-75);
// ymh adds Agent for agent-scoped shadowing (D20).
enum class CommandSource : std::uint8_t { User, Agent };
struct CommandRun {
    CommandId                  command_id = 0;
    std::string                name;
    std::optional<std::string> args;       // absent when recordInput=false
    CommandSource              source = CommandSource::User;
};
enum class CommandDoneKind : std::uint8_t { Success, Error };
struct CommandDone {
    CommandId                  command_id = 0;
    CommandDoneKind            kind = CommandDoneKind::Success;
    std::optional<std::string> text;
    std::optional<Sequence>    source_event_seq;
};

// dsh durable goal mutation, session event `goal/change`: GoalChangeMeta
// (dsh-goal/lib/types/domain.d.ts:12-32). Snapshot changes carry the fresh
// GoalSnapshot plus the admitted round count; a clear carries only the ref.
// (dsh's `goal/changed` at domain.d.ts:68-73,86-90 is a LIVE Cordis
// notification, not a session event, and is deliberately not copied.)
enum class GoalOperation : std::uint8_t { Create, Edit, Pause, Resume, Complete, Block, Clear };
struct GoalChange {
    GoalOperation               operation = GoalOperation::Create;
    std::optional<GoalSnapshot> goal;          // present for non-Clear (dsh GoalSnapshotChangeMeta)
    std::optional<GoalRef>      cleared;       // present for Clear (dsh GoalRef)
    std::uint32_t               rounds_started = 0;   // non-Clear
};

// ymh-local job registry change (owner-scoped; no dsh session-event analogue).
struct JobChanged {
    AgentId       owner;
    std::string   kind;
    std::uint64_t ordinal = 0;
    JobStatus     status = JobStatus::Running;
    std::string   label;
};

} // namespace payload
```

#### 4.3.9.1 Pinned JSON key schemas (26-D2/D9/D12/D16..D21; closes N-M3)

The `to_json`/`from_json` bodies are hand-written (`src/session/events.cpp`), so
the C++ struct does not by itself pin the wire keys. The keys below are pinned;
an implementing errata may not rename one without amending this table. Conventions
match the existing codecs: `snake_case` keys, enums as lowercase strings,
`optional` fields omitted when unset and read with a default, and no
timestamps/ids beyond the fields listed (the core event envelope already carries
`event_id` and `timestamp`).

| Payload | JSON object keys |
|---|---|
| `payload::LlmRequestHeader` | `turn` int, `step` int, `session_id` string, `purpose` string (`"compaction"｜"session_title"`, omitted when unset), `config` object, `system_prompt_digest` string, `system_prompt` string (**omitted unless `session.persist_prompt_text`; default off, global layer only**), `tool_names` string[], `tool_schema_digests` string[], `template_digest` string, `starts_series` bool |
| `LlmCallConfig` (nested) | `provider` string, `model` string, `reasoning_effort` string (omitted when unset), `temperature` number (omitted), `max_tokens` int (omitted), `stop` string[], `top_p` number (omitted), `seed` int (omitted), `tool_choice` string (omitted) |
| `payload::LlmRetry` | `retry_id` int, `turn` int, `step` int, `provider` string, `mode` string (`"normal"｜"always"`), `policy_key` string, `retry` int, `max_retries` int (**only** when `mode=="normal"`), `delay_ms` int, `failure` `LLMError` object (existing spec-08 codec) |
| `payload::LlmRetryStarted` | `retry_id` int, `turn` int, `step` int, `retry` int |
| `payload::AssistantAttempt` | `turn` int, `step` int, `stream` `AssistantStreamRecord[]` |
| `payload::ContextPrune` | `shadowed_start` int, `shadowed_end` int, `shadowed_seqs` int[], `shadowed_token_count` int |
| `payload::AgentPresetSelected` | `agent_preset` string |
| `payload::CommandRun` | `command_id` int, `name` string, `args` string (omitted when `recordInput=false`), `source` string (`"user"｜"agent"`) |
| `payload::CommandDone` | `command_id` int, `kind` string (`"success"｜"error"`), `text` string (omitted), `source_event_seq` int (omitted) |
| `payload::GoalChange` | `operation` string (`"create"｜"edit"｜"pause"｜"resume"｜"complete"｜"block"｜"clear"`), `goal` `GoalSnapshot` object (omitted on clear), `cleared` object `{id int, revision int}` (omitted on non-clear), `rounds_started` int |
| `GoalSnapshot` (nested) | `id` int, `revision` int, `objective` string, `phase` string (`"active"｜"paused"｜"blocked"｜"complete"`), `blocked` object `{code string, message string}` (omitted unless blocked), `max_goal_rounds` int |
| `payload::JobChanged` | `owner` string, `kind` string, `ordinal` int, `status` string (`"running"｜"stopping"｜"completed"｜"killed"｜"failed"`), `label` string |
| `payload::AssistantMessage` (D9 additions) | existing `id`, `content`, `usage` (omitted) **plus** `stream` `AssistantStreamRecord[]` and `replay_state` object (omitted when none) |
| `payload::ContextCompaction` (D13 additions) | existing `boundary`, `summary`, `token_estimate`, `model`, `created_at` **plus** `provider` string, `shadowed_start` int, `shadowed_end` int, `shadowed_seqs` int[], `shadowed_token_count` int |

`AssistantStreamRecord[]` entries are discriminated by `type`:
`{"type":"text_chunks","index":int,"time0_ms":int,"dt_ms":int[],"texts":string[]}`,
`{"type":"reasoning_chunks",…}`,
`{"type":"tool_call_chunks","index":int,"time0_ms":int,"dt_ms":int[],"id":…,"name":string(omitted),"args":string[]}`,
`{"type":"chunk","time_ms":int,"event":<existing StreamEvent codec>}`.
The `type` strings deliberately use ymh's underscore convention; dsh's are
hyphenated (`text-chunks`, …), consistent with the stated wire-name divergence.

Adding an `EventType` value is a codec change, not a free extension: `parse_event_type`
rejects unknown strings loudly (S3), so an older binary cannot read a newer
session DB (§4.6).

#### 4.3.9.2 Consumer matrix for the new event types (closes the 70-switch gap)

The `01` errata owns this matrix and may not ship a new type without filling in
its row. "Ignore" means an explicit `case ...: break;` in the projection switch
(no `default:` there) and, in the four `default:`-bearing switches, either an
explicit no-op case or a documented fall-through to `default` — the choice is
recorded, never implicit.

| EventType | `deriveMessages` projection (`session.cpp`) | transcript / UI (`ui_event_adapter`, `session_export`, `session_cli`) | compaction & token accounting |
|---|---|---|---|
| `LlmRequestHeader` | ignore (metadata) | no row; never rendered | excluded (not model-visible content) |
| `LlmRetry` | ignore | optional retry notice (status only) | excluded |
| `LlmRetryStarted` | ignore | no row | excluded |
| `AssistantAttempt` | ignore (committed no message) | optional debug row only | excluded |
| `ContextPrune` | ignore — the replacement `ToolResult` carries the text | prune notice row | the **replacement** is counted; the prune event is not |
| `AgentPresetSelected` | ignore | preset row | excluded |
| `CommandRun` | ignore (log-only) | command row | excluded |
| `CommandDone` | ignore (log-only) | command row | excluded |
| `GoalChange` | ignore | goal row | excluded |
| `JobChanged` | ignore | job row | excluded |
| `AssistantChunk` (existing type, **new live-only role**) | ignore | **live delta feed (required)** — see §4.3.4/Wave 2 | excluded (durable stream lives in `assistant/message`) |
| `AssistantMessage` (changed) | assistant message (existing) | existing | counted as assistant output |
| `ContextCompaction` (changed) | existing projection | existing | excluded (projection) |

Rules the errata must satisfy:

1. **Build gate.** The projection switch (`session.cpp:375`) has no `default:`;
   every new durable type gets an explicit case. This is checked by
   `-Werror=switch`, and `all_event_types()` round-trip tests (`01 §15.1`) must
   cover every new type.
2. **No silent drops for user-visible events.** `AgentPresetSelected`,
   `CommandRun`/`CommandDone`, `GoalChange`, and `JobChanged` are user-visible;
   the four `default:`-bearing switches must render them explicitly (or record
   why not). Internal events may fall through, but only with a documented reason
   in the errata.
3. **Token accounting.** No metadata event is counted toward the context/token
   estimate; only message-bearing events (`UserMessage`, `AssistantMessage`,
   tool results, compaction summaries) contribute.
4. **Live vs durable.** `AssistantChunk` stays in the codec vocabulary (legacy
   durable rows remain decodable) but new binaries do **not** append it; it is
   published as a live-only event on the bus, like `McpServerStatusChanged`
   (`core/event.hpp:72-74`). The durable settlement is `AssistantMessage.stream`.
   Consumers that need live deltas (headless stdout, the UI adapter) read the
   live event; consumers that need history read the durable event. The `01`
   errata owns whether `AssistantChunk` is moved out of the compile-time
   `SessionEventMap` or kept with an append guard.

#### 4.3.10 Message provenance (26-D14)

D14 names `MessageSource`/`ContextForm`; they are pinned here, mirroring dsh
`message.d.ts:17-104`. `MessageSource.kind` answers *who produced this*;
`ContextForm` answers *what kind of thing it is*, and the two axes are
independent.

```cpp
namespace ymh {

// dsh ContextForm (message.d.ts:42-54): a SEMANTIC vocabulary, never visual.
// `None` is the documented default (dsh: an absent/unknown value is presented
// as opaque content).
enum class ContextForm : std::uint8_t {
    None, Instructions, Catalog, Snapshot, Notice, Relay, Recall
};

// One named contribution to a Snapshot-form context (dsh ContextSnapshotSection).
struct ContextSnapshotSection { std::string name; std::string text; };

// Producer-declared form plus the fields that form requires. dsh's ContextFormed
// is a discriminated union keyed by `form` (message.d.ts:71-89); ymh flattens it
// to one defaulted struct for storage and enforces the same coupling at
// construction (26-I7): `form == Snapshot` requires a non-empty `sections`,
// `form == Notice` requires a non-empty `summary`; a violation fails loud.
struct ContextFormed {
    ContextForm                         form = ContextForm::None;
    std::vector<ContextSnapshotSection> sections;   // required iff form == Snapshot
    std::string                         summary;    // required iff form == Notice
};

// dsh MessageSourceMap (message.d.ts:94-104), flattened for C++.
// Divergence (stated): dsh's ModelMessageSource carries `replayState`; ymh keeps
// replay state once, on the assistant settlement event (§4.3.4), not here.
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

The provenance types are additive to `Message` (`01` errata, Wave 3). They are
referenced by D6/D7/D14 and by the `17` transcript row kinds.

### 4.4 Breaking-change classification against the recent verified specs

The task names `10, 11, 17, 20, 21, 22, 23, 24`. Classification below is about
**specification text**, not code. This table is reconciled with the register in
§4.2; where the prior draft contradicted itself (D9/D16, D11/D13), the register
wins and the spec text is corrected.

| Spec | Decision(s) affecting it | Class | Why |
|---|---|---|---|
| **10** supervisor-tui | D16 (preset selection UI), D18/D19 (goal/job surfaces), D20 (commands) | **Additive** | New UI affordances; no existing surface contract changes. Must not alter the live-only switcher rule (`22`) or transcript rendering. |
| **11** m2-errata | D16 (session header + events), D2/D9/D13 (new event types) | **Additive for D2; breaking for D9/D13/D16** | New event kinds are additive, but D9 changes the assistant-message payload, D13 adds `ContextCompaction` fields against spec 13's "does not add fields to the payload", and D16 changes the session header. Each is gated by its owning-spec errata before its wave (Wave 0 Stage B, §5). |
| **17** ui-transcript-errata | D4/D7 (prompt/context provenance shown), D9 (stream records), D11/D12 (tool-row retention notices), D14 (provenance), D15 (guidance), D20/D21 (command/reminder rows) | **Additive** | The transcript gains new row kinds/`source.form` rendering. The verified render contracts (fixed chrome budget, generation guard, `has_error` boolean) must be preserved; no change to them is proposed. |
| **20** skills | D6 (shared `<system-reminder>`/`ContextForm` framing), D17 (child skills join composition), D15 (skill guidance ordering) | **Additive** | Spec 20's catalog/`SkillTool` contract is unchanged; the change is that the catalog message is now a sourced `catalog`-form context and children inherit the parent's skill set. If spec 20 pins the exact catalog text, updating it to the verbatim dsh text is an **errata** (text-only), not a semantic break. |
| **21** config-jsonc-errata | D4/D5/D6/D15/D16 (new config keys) | **Additive** | New optional keys under existing/new sections (names pinned in §4.9). The required-global-layer rule, JSONC-only rule, and "never auto-create an explicit `--config`" rule are untouched. A new required key would be breaking; none is proposed (`max_bytes` is required only when the instructions loader is enabled). |
| **22** switcher-sessions-errata | D16 (preset recorded per session) | **Additive** | `/sessions` still reads stored sessions from disk; a preset id is a new stored attribute, not a filter or a live-only rule change. |
| **23** session-lifecycle-errata | D16 (session header names preset), D18 (goal state across resume/fork) | **Brk. (errata required)** | The session header gains a preset id and a goal projection, so the frozen header field list changes. Lifecycle rules (lease, resume, fork) are preserved. `23` is in the Wave 0 gate. |
| **24** agent-lifetime-errata | D10 (scheduler holds the agent), D11 (retention: no lifetime change), D17 (children are agents with monotone depth) | **Additive** | The loop co-ownership invariant (`24-D1`, `agent_loop.hpp:71-73`) is preserved. Children are ordinary agents; monotone depth is a new persisted field. D24 no longer bumps `kProtocolVersion`, so `24` `AL20` (`:1692`) and §1.3.3 (`:295`) — "`kProtocolVersion` unchanged" — remain accurate; **no spec-24 errata is required** (closes Rev-5 gate finding N-2). |

The provider/architecture seams (not in the task's named list but architecture-level):

| Spec | Decision(s) affecting it | Class | Why |
|---|---|---|---|
| **00** architecture | D1 (provider seam), D9/D2 (event family), D16/D18 (presets/goals), D24 (event-vocabulary decoupling) | **Brk. (errata required)** | The top-level gate changes: `LLMProvider`+registry → `LlmRuntime`, the `EventType` family grows, and the session header changes. `00` is the first Wave-0 up-front item (§5). The wire envelope version is **unchanged** (D24). |
| **06** agent-loop | D1 (loop holds `LlmRuntime&`, not `LLMProvider*`), D10 (scheduler), D17 (children), D21 (reminders) | **Brk. (D1); additive (D10/D17/D21)** | D1 replaces the pinned `AgentServices::provider` seam (`agent_loop.hpp:64`) — the register classifies it `Brk. (06,08)`. D10/D17/D21 are additive behind it. |
| **08** llm-provider | D1 (service boundary), D2/D3 (freeze/digest/header), D8 (assembler), D9 (replay state), D13 (summarizer routing), D23 (prompt-text policy) | **Brk. (D1/D2/D3); additive (D8/D9)** | D1 inserts a service above the provider and moves retry/serialization out of the adapter; the `LLMProvider` contract itself is retained. |
| **05** transport | D24 (receiver tolerance for unknown event types), D2/D9 (new event types over the socket) | **Additive** | The envelope shape (`SessionEnvelope` = `{session,event}`, T5 `:1229`), the handshake, and `kProtocolVersion = 1` are unchanged; the only new rule is that the client skips an unknown event `type` (cursor still advances) instead of failing the connection. The §2.1 enum-string rule (`:201-203`) is unaffected — it governs the pinned enums, not the event vocabulary. |
| **04** host-daemon / **16** daemon-ownership | D24 (no version change) | **Unaffected** | `kProtocolVersion` stays `1` and the handshake is unchanged, so the version constant and the handshake outcome do not change. Ownership/lifetime rules are untouched. |
| **07** tools-execution | D11 (retention), D15 (presentation) | **Brk. (D11 `ToolResult`/clamp); additive (D15)** | Retention changes the pinned `ToolResult`/`clamp_tool_result` contract. |
| **13** compaction | D12 (pruner), D13 (payload fields) | **Brk. (D13)** | D13 adds `ContextCompaction` fields against spec 13's "does not add fields to the payload". |
| **15** mcp-adapter | D15, `26-I6` (a tool-set change starts a new request series) | **Additive** | MCP keeps its namespaced tool contract; the change is that a server connect/disconnect is a logged series boundary. |

**Cascade coverage — the specs not classified above are unaffected in their
pinned interfaces.** `02` persistence (no schema/DDL change, §4.6), `03`
registry (untouched), `09` permissions (no semantics change; `ReadOnly` status is
unchanged), `14` PTY (behind the execution seam, untouched), `18` context-errata
and `25` ui-ux-errata (additive config keys only, §4.9), `19` session-rename
(untouched). `10/11/17/20/21/22/23/24` are classified in the table above. Any
spec not in this list is out of scope; the `00` errata (Wave-0 up-front) is the
gate that confirms the list is complete.

Up-front Wave-0 freezes (block Wave 1): `00`, the `01` event-family contract,
`08` — see §5. JIT-gated before its own wave: `05`, `06`, `07`, `13`, `17`,
`23`, `27`, `28`, `29`, `30`, and the touched `10`/`11`/`20`/`21`/`22`.

### 4.5 Failure modes (F-style, to be pinned by the owning specs)

- `26-F1` — request config drift: a proposed `LlmCallConfig` differs from the
  logged header and is neither logged nor rejected. Guard: `26-D2` logs every
  real change; `call_config_equals` is the only change test.
- `26-F2` — frozen request mutated after dispatch. Guard: `FrozenRequest` is
  immutable by type; no `mutable`/`const_cast` path; a debug assertion checks a
  sentinel.
- `26-F3` — non-terminal stream: adapter returns without `Finished`/`StreamError`.
  Guard: `LlmRuntime::stream` normalizes to `Failed` with `MalformedResponse`.
- `26-F4` — retry after the first dispatched event. Guard: the existing barrier
  (`stream.hpp:111`) is preserved and the retry executor records `llm/retry`
  before the wait.
- `26-F5` — prompt non-determinism: two assemblies of identical inputs differ.
  Guard: canonical sort `(order, name)`, drop-empty, strict variables, one
  `complete`; a golden test hashes the rendered prompt.
- `26-F6` — tool-catalog drift mid-session. Guard: `tool_names` +
  `tool_schema_digests` in the header; any change starts a new series and is
  logged (`26-I6`), and the re-derived schema must match its digest or replay
  fails loud.
- `26-F7` — malformed stream grows memory / corrupts a completed block. Guard:
  `BlockAssembler` ignores deltas for a closed index (dsh's rule).
- `26-F8` — pruned replacement loses the original. Guard: replacement cites the
  shadowed node; original event remains in the log; `context/prune` precedes it.
- `26-F9` — child composed without the parent join (empty tool registry). Guard:
  `apply_child_composition` is one call; a child with no join is unrepresentable.
- `26-F10` — goal continuation runs without human authority. Guard: create/edit/
  pause/resume require a top-level turn; resume/fork disarms; round cap default
  256; at cap record `round-limit`.
- `26-F11` — job output read/stopped by a non-owner. Guard: owner-session fence.
- `26-F12` — command becomes a model message. Guard: `command/run`/`command/done`
  are log-only standalone appends.
- `26-F13` — replay non-determinism: a request template or a concrete request
  rebuilt from the log differs from what was dispatched. Guard: the §4.3.2
  canonical-serialization contract — `template_digest()` equality for the header
  and byte-identical `canonical_json()` across two rebuilds per attempt — plus
  the replay-determinism harness (§5.2), which fails on any mismatch.
- `26-F14` — reserved presentation mode silently accepted. Guard: selecting
  `Ptc`/`Both` fails loud at load (`26-I7`); D22 is deferred.
- `26-F15` — old session opened by a new binary with no header. Guard: attempts
  are marked legacy/unreconstructable; no header is synthesized (§4.6).
- `26-F16` — concurrent scheduler tool mutates shared state. Guard: the
  concurrency contract in §4.8; TSan tests.

### 4.6 Backward compatibility (T-M11)

Compatibility is **two-axis**. The **durable/on-disk axis** is strict: an older
binary must refuse a newer session DB loudly rather than misread it. The **wire
axis** is tolerant: the socket is a live projection of the log, so a receiver
skips an event type it does not know and stays connected. `kProtocolVersion`
fences the wire **envelope** shape and handshake only; it is not an event-
vocabulary counter (`26-I12`/`26-D24`).

- **No schema bump; `kSchemaVersion` stays 1.** `kSchemaVersion` tracks
  **structural shape only** — tables, columns, indexes (`session_persistence.hpp:68`).
  This program changes no structural shape: every new event is a row in the
  existing `events(sequence, session_id, event_id, timestamp, type, payload)`
  table (`session_persistence.cpp:53-58`), and the D16 header fields (`preset`,
  `goal`) ride the existing `sessions.metadata JSON` column (`:47`) rather than
  new columns. `migrate_fresh` (`:569-580`) is unchanged; there is **no**
  `migrate_v1_to_v2`, no DDL delta, and therefore **no call site** to pin. (Rev 3
  proposed a 1→2 bump with zero DDL; that contradicted this very definition — a
  bump with no structural change — so Rev 4 removes it.)
- **Forward-incompatibility fence (durable/on-disk axis).** New event *types* are
  not a shape change, so `user_version` does not gate them; the **on-disk/replay**
  decode rejects unknown type names loudly (`src/session/session_persistence.cpp:228-232`
  `CorruptionError`; `01` S3), which is the actual fence: an older binary refuses
  a newer session DB at the first unknown event instead of silently misreading it.
  Because `kSchemaVersion` stays 1, a read-only open keeps today's exact-match
  rule (`session_persistence.cpp:682`) and `/sessions` (spec `22`) continues to
  read schema-1 workspaces with no relaxation. A future change that adds a real
  column is the first structural change and introduces `migrate_vN_to_vN+1` and
  a bump at that point.
- **Wire axis: unknown event types are skipped, not fatal.** The receiver decodes
  `StreamNotification` → `SessionEnvelope` → `Event` (`src/transport/protocol.cpp:266-268,321-326`)
  and must **skip** a `SessionEnvelope` whose event `type` it does not recognize:
  log it, do not call `on_envelope`, and still advance the per-session cursor to
  the notification's `cursor` (`src/ui/supervisor_connection.cpp:310-323`) so a
  reconnect does not replay a poison pill. Skipping is safe because the durable
  truth is the on-disk log, which a newer binary re-reads. The
  old-binary-facing-a-newer-daemon direction is a downgrade (below) and fails
  when it hits a type it cannot decode; that is the loud behavior we want for an
  un-rebuilt binary, and it is why the envelope version is not the vocabulary
  counter.
- **Payload evolution on an existing event type.** New fields on a *known*
  event (`AssistantMessage.stream`/`replay_state`, the `ContextCompaction`
  fields) are additive JSON: each is optional or defaulted, and `from_json` must
  ignore unknown keys so an older reader degrades gracefully instead of
  rejecting the row. A field that cannot be made optional is a breaking payload
  change and must be carried by a new event type.
- **Header-less resumed sessions.** A pre-program session has no
  `LlmRequestHeader`. On replay, an attempt with no preceding header is marked
  `legacy` and its reconstruction is reported as unavailable — never fabricated.
  New turns on a resumed legacy session write headers normally.
- **Legacy durable `AssistantChunk`.** Retained and readable forever, but new
  binaries do not append it to the log. New sessions embed the compact stream in
  `assistant/message` and retain non-message attempts in `assistant/attempt`;
  `expand()` is defined over those records, and the transcript falls back to a
  legacy `assistant/chunk` row when no embedded/compact record exists. New
  binaries still **publish** `AssistantChunk` as a live-only event so live
  consumers keep their deltas (D9, §4.3.9.2, Wave 2).
- **New event types.** On the durable axis, a session DB containing the new event
  types is not readable by an older binary (the on-disk fence above); this
  forward-incompatibility is a **one-way door and an explicit product decision**:
  the Wave-0 freeze must record the user's acceptance (the alternative — a
  migration/dual-read window — is not proposed). Downgrades are unsupported. On
  the wire axis, the same new type is **skipped** by an older receiver, so it does
  not by itself break the connection.
- **Protocol version (`kProtocolVersion` stays `1`).** The event vocabulary is
  **decoupled** from the wire envelope version: new durable event types do **not**
  bump `kProtocolVersion`, which fences the **envelope** shape (`SessionEnvelope`
  = `{session,event}`, `05` T5) and the handshake only. This is what makes the
  spec-24 outliving-daemon case work: a non-last supervisor's daemon outlives it
  (`24` §1.3.3, `AL23`/`AL25`), so a **new** supervisor frequently attaches to an
  **old** daemon process. With the version held at `1` the handshake matches
  (`protocol_server.cpp:285-293`) and the new supervisor understands every type
  the old daemon can emit (a superset); with a bump the exact-match handshake
  would reject the attach outright, forcing a co-upgrade that spec 24 says is not
  required. Spec `24` `AL20` (`:1692`) / §1.3.3 (`:295`) ("`kProtocolVersion`
  unchanged") therefore remain accurate — **no spec-24 errata is required**. A
  future change that alters the envelope shape or handshake semantics is the only
  thing that bumps the version; that bump is then `Brk. (05)` and gates spec `05`
  in Stage B.
- **Registry DB** (`0x594D4802`, schema 2) is untouched.
- **Rollout order.** Reader support (parse new events, tolerate a missing
  header) ships in the same wave as writer support; there is no separate
  downgrade path.

### 4.7 Error taxonomy (completeness)

- `LLMErrorCode` is a closed enum (`stream.hpp:40-56`). D1 adds one value,
  `InvalidPreparedCall`, as an additive `08` errata; the `llm/retry` and
  `llm/request_header` payloads reuse the existing `LLMError`/`LlmFailure`
  shapes.
- A `PreparedCall` misuse (double dispatch or config mismatch) throws
  `PreparedCallError { InvalidPreparedCall, Consumed, ConfigMismatch }`. At the
  loop boundary it maps to `AgentErrorCode::ProviderFailed`; at load/assembly it
  maps to `ConfigError` (process exit 2), per the existing config contract.
- Prompt-assembly failures (duplicate name, `>1 complete`, malformed/unknown
  `{{var}}`, bad `tool_order`) throw `ConfigError` at load/assembly (`26-I7`).
- Retention/pruner failures are `ToolError` values; a pruner that cannot produce
  a replay-safe replacement leaves the original untouched and reports
  `PruneResult{pruned = 0}`.

### 4.8 Concurrency and cancellation (completeness)

- **`LlmRuntime` registry and adapter lifetime.** `register_adapter` takes a
  `std::shared_ptr<LLMProvider>` and stores it; `add_stream_interceptor` stores
  its callable. Both mutations take an internal mutex; `stream`/`prepare_call`
  take the mutex only long enough to copy the selected adapter's `shared_ptr`
  and the interceptor chain, then release it before dispatching. A dispatch
  therefore holds a `shared_ptr` for its whole lifetime. `AdapterHandle`'s
  destructor locks the registry and erases the routes, but the adapter object is
  destroyed only when the last in-flight dispatch releases it — there is no
  use-after-free. "No registration may race a dispatch" is a consequence of
  shared ownership plus the short critical section, not an unenforced assertion.
  A `PreparedCall` also holds the `shared_ptr` it bound at `prepare_call`, so its
  adapter generation stays alive through `stream`.
- **Scheduler vs. shared services (a contract, not a question list).** The
  bounded pool may call a shared service concurrently only when that service is
  documented safe for concurrent calls for the duration of a step; otherwise the
  scheduler serializes it behind an exclusive barrier. Before Wave 4 code, the
  owning specs **must** classify each shared service as exactly one of: (a)
  const/immutable for the step (`ToolRegistry` lookups are read-only after
  load), (b) every mutating entry point is mutex-guarded (`Session` append, the
  job registry), or (c) executor-confined and never run on the pool
  (`PermissionGate`, `ContextAssembler`). `06`/`07`/`09`/`13` pin this
  (a)/(b)/(c) classification as a Wave-0 gate item; a service that cannot be
  classified blocks the Wave-4 scheduler.
- **Abort semantics.** Cancellation stops new dispatches, drains started calls,
  and records synthetic results for unstarted calls so the log stays replayable.
  A tool never starts after cancellation is observed.
- **`SystemPrompt`/`ContextAssembler`** are executor-thread-confined; assembly
  is serialized by the loop. No prompt type crosses a thread boundary.
- **Tests:** deterministic barrier/pool tests plus TSan on the scheduler and the
  runtime registry, and a destroy-the-handle-while-a-dispatch-is-in-flight test
  that must not fault.

### 4.9 Config wiring (completeness)

New JSONC keys, all optional except where noted (spec `21` errata, additive):

| Key | Type | Default | Notes |
|---|---|---|---|
| `agent.system_prompt` | string | built-in | existing key; becomes the base section source |
| `prompt.persona.prefix` | string | — | required when persona enabled |
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
| `tools.tool_order` | string[] | lexicographic | one `<unlisted-tools>` rest |
| `retention.item_max` | int | 0 (unbounded) | |
| `retention.text_max_bytes` | int | 0 | |
| `retention.strategy` | `"head"｜"tail"｜"head_tail"` | `"head_tail"` | |
| `compaction.prune.threshold_code_points` | int | 8192 | code points |
| `compaction.prune.head_code_points` | int | 4096 | |
| `compaction.prune.tail_code_points` | int | 1024 | |
| `presets.root` | path | — | open question 8 |
| `presets.default` | string | — | |
| `presets.include_shipped_root` | bool | `true` | |
| `presets.include_user_root` | bool | `true` | |
| `goals.max_rounds` | int | 256 | |
| `goals.blocked_after_consecutive_rounds` | int | 3 | |
| `jobs.wait_timeout_ms` | int | 30000 | |
| `jobs.max_wait_timeout_ms` | int | 600000 | |
| `jobs.completion_delivery` | `"wakeup"｜"quiet"` | `"wakeup"` | |
| `jobs.max_consecutive_wakes` | int | 3 | |
| `session.persist_prompt_text` | bool | `false` | new `[session]` section; **global layer only**; durable full-prompt opt-in (D23), distinct from `logging.log_prompts` |

`GenerationParameters` and `LlmCallConfig` are not two sources of truth:
`buildRequest` maps `GenerationParameters` into `LlmCallConfig` exactly once,
and `LlmCallConfig` is what is logged and compared. The canonical sampling
budget name is `max_tokens`; `GenerationParameters::max_output_tokens` is
deprecated in favour of it (T-M5). ymh's adapter still emits `top_p`, `seed`,
and `tool_choice` on the wire (`openai_adapter.cpp:736-752`), so they are
**added to `LlmCallConfig` and to `canonical_json()`'s `config` object** — a
wire-affecting field absent from the digest would make the reconstruction
contract unsound. This is a deliberate superset of dsh's 6-field
`LlmCallConfig` (`call-config.d.ts:16-23`), which dropped those fields; if a
later decision removes them from the wire, they are removed from both the
config and the digest together (open question 3).

`logging.log_prompts` is an **existing** key (`include/ymh/config/config.hpp:84-88`,
`src/config/config.cpp:787`) and is **not** reused: it governs the spdlog/log
surface only, and its shipped contract is that prompt bodies are redacted even
when it is on (`logging.cpp:176`, `redaction.cpp:52`). Durable prompt
persistence gets its own key, `session.persist_prompt_text` (bool, default
`false`, new `[session]` section, **global layer only**), so the two concerns
are never conflated. The default (`false`) keeps prompt text out of the session
DB entirely (`26-I11`).

---

## 5. Phased migration plan

Each wave is independently shippable and testable. Waves 0–4 are the LLM/
output/prompt core; waves 5–6 are the collaboration layer. Every wave's owning
spec is gated in Wave 0, per `AGENTS.md`'s two-gate rule.

### Wave 0 — Pin the specs (no code)

Wave 0 has **two stages**. Only Stage A blocks Wave 1; Stage B is JIT-gated
before its own wave's code. Nothing is coded until its owning spec is verified.

**Stage A — up-front freeze (blocks Wave 1).** Three items, in order, each with
Oracle PASS and zero open HIGH/MEDIUM:

- **A0. `00-architecture.md`** — confirm the amended provider/prompt/event seams
  and the new session events do not violate a top-level invariant, and update the
  seam inventory (the register and §4.4 both gate `00`).
- **A1. `01` errata — the event-family contract only** — the
  `EventType`/`wire_name` extension rule, the two-axis compatibility rule (§4.6),
  the live-only vs durable split, the consumer matrix (§4.3.9.2), and the
  `llm/request_header` event + codec.
- **A2. `08` errata — the service boundary** — `LlmRuntime`/`PreparedCall`/
  `FrozenRequest`, freeze/serialization/digest, and
  `LLMErrorCode::InvalidPreparedCall`.

Wave 1 needs nothing else: its header carries the **current**
`AgentConfig::system_prompt` (below), so spec `27` is **not** a Wave-1
dependency. Freezing the prompt registry before Wave 1 would buy zero Wave-1
risk reduction.

**Stage B — JIT-gated per wave (verified before that wave's code).**

| Wave | Specs verified before its code |
|---|---|
| 1 | `21` errata — the new `session.persist_prompt_text` key (D23); the Stage-A A1 `01` errata already pins the D24 wire-vocabulary rule; no `05` change |
| 2 | `01` assistant-stream payloads; `08` assembler/replay errata |
| 3 | `27-system-prompt.md` (new); `01` provenance payloads; `17`/`21` errata |
| 4 | `28-output-retention.md` (new); `06`/`07`/`13` errata |
| 5 | `29-agent-presets.md` (new); `23` errata; `01` preset event |
| 6 | `30-goals-jobs-commands.md` (new); `01` goal/job/command events |
| any | `10`/`11`/`20`/`22` errata only where that wave changes them |

Every owning spec is still verified before its wave's code, per `AGENTS.md`'s
two-gate rule; Stage B defers the *verification date*, not the requirement.

### Wave 1 — LLM service boundary, logged header, frozen request (D1–D3)

- Add `LlmRuntime`, `PreparedCall`, `AdapterHandle`, `ProviderInfo`,
  `LlmCallConfig`/`call_config_equals`, `FrozenRequest` with
  `canonical_template()`/`template_digest()`/`canonical_json()`/`digest()`, the
  `llm/stream` interceptor registration, and
  the `payload::LlmRequestHeader` event.
- Point `AgentLoop::buildRequest` at the runtime; derive the header from the log.
- Header prompt policy (D23): always write `system_prompt_digest` + the invariant
  config/envelope/tool fields; write the full `system_prompt` only when
  `session.persist_prompt_text` is explicitly enabled (default `false`,
  global-layer only).
- Do **not** bump `kProtocolVersion` (D24): `llm/request_header` is a vocabulary
  addition, handled by the receiver-skip rule pinned in the `01` errata; the
  envelope version stays `1`.
- Re-seam `ContextCompactor`'s constructor from `LLMProvider&` to `LlmRuntime&`
  (T-M9) so Wave 4 can route the summarizer.
- The Wave-1 header carries the **current** `AgentConfig::system_prompt`
  (config override or `default_system_prompt()`, `wiring.cpp:48-52`), which
  exists before Wave 3; it is not a rendered-registry hash. This removes the
  prior draft's "no model-visible change vs `system_prompt_hash`" conflict
  (T-L7).
- Tests: FakeLLM (spec `45`) drives header logging, drift detection,
  reconstruction-digest equality, and `PreparedCall` mismatch/one-shot.
- Rationale: smallest change that unlocks replay. **No model-visible
  behaviour changes.**

### Wave 2 — Canonical assembler + assistant stream/replay (D8, D9)

- Add `BlockAssembler`, `AssistantStreamAccumulator`, `ReplayEnvelope`,
  `TimedStreamEvent`, `AssistantStreamRecord`; add `replay_state` and the
  embedded compact `stream` to the `payload::AssistantMessage` payload and
  `replay_state` to the `Finished` stream event; add the `assistant/attempt`
  event.
- Stop **persisting** `AssistantChunk` in new sessions; legacy durable rows stay
  readable (no migration). New binaries keep **publishing** `AssistantChunk` as a
  live-only event (`core/event.hpp:72-74`) so live deltas survive.
- **Consumer migration (required, not optional).** `src/cli/headless.cpp:197-202`
  builds `ymh run`'s stdout purely from `AssistantChunk` and has no
  `AssistantMessage` case; `src/ui/ui_event_adapter.cpp:90-108` derives live
  `AssistantMessageStarted` from it. Wave 2 must (a) keep the live delta path on
  the live-only event, and (b) make headless and the transcript/export paths read
  `assistant/message` (embedded `stream`) for durable text. Without (b),
  `ymh run` prints nothing and returns empty text on a resumed/replayed session.
  The `session_export.cpp`/`session_cli.cpp` fallback to legacy `assistant/chunk`
  is retained.
- Tests: golden assembly from recorded chunk fixtures, interrupted-stream
  blocks, replay expansion round-trip, `replay_state` propagation, a
  **headless-output test** (`ymh run` text from `assistant/message` with no
  `AssistantChunk`), and a **live-delta test** (UI adapter receives the live-only
  event).
- Rationale: replay fidelity before the prompt system, so prompt changes can be
  diffed against recorded attempts.

### Wave 3 — Prompt system and provenance (D4, D5, D6, D7, D14, D15)

- Add the section registry, order tables, `assemble()`, persona, instructions
  loader, runtime contexts, tool presentation (`Native`), and message
  provenance (`MessageSource`/`ContextForm`, `SystemMessage`/`ToolResultMessage`).
- Migrate the hardcoded `default_system_prompt()` (`wiring.cpp:48-52`) to a
  registered `harness:identity` section + default persona; make `AGENTS.md`
  loading opt-in via config with the required byte budget.
- `ContextInjected.role` default changes to `User` for snapshots (D7).
- Tests: golden rendered-prompt hashes per preset/config; strict-variable
  failures; duplicate/`complete` failures; `tool_order` validation; AGENTS.md
  budget/truncation/removal notices; provenance round-trip.
- Rationale: this is the behavioural heart of "copy dsh". It depends on Wave 1's
  logged header to record the prompt and series boundaries.

### Wave 4 — Output processing (D10, D11, D12, D13, D21)

- Bounded-parallel tool scheduler; retention library and tool-output routing;
  tool-result pruner (code points); compaction trigger taxonomy/shadow
  accounting and the `LlmRuntime`-routed summarizer; repeat-tool reminders.
- Tests: scheduler ordering/barriers/abort-synthetics + TSan; retainer
  exact-omission goldens; pruner replay round-trip; compaction trigger selection.
- **Dependency: serialize Wave 3 → Wave 4.** This wave is *not* independent of
  the prompt system: `ContextAcceptor` (§4.3.5) consumes `ContextMessage`
  (`agent.hpp:88`) and D13/D14 both reshape the spec-13 message model, so Wave 4
  must land after Wave 3. It improves context pressure and
  replay safety once Wave 3's provenance exists. `clamp_tool_result`
  (`tool.hpp:82`) is retired in favour of the retainer once all call sites
  migrate.

### Wave 5 — Presets and subagent composition (D16, D17)

- Add the roster, standing mount + per-session joined scopes, blank-session-only
  switch, `agent_preset/selected`, `apply_child_composition`, depth, fixed scope.
- Tests: preset resolution/default; child sees parent's tools/sections; switch
  rejected after first message; header carries the preset.
- Rationale: requires Wave 3 (sections/persona/tools) to compose. This wave
  changes the session header (`23` errata, gated in Wave 0).

### Wave 6 — Goals, jobs, commands (D18, D19, D20)

- Add goal state + round driver, job registry + tools + wakeup policy, and the
  log-only command surface.
- Tests: goal disarm across resume/fork; cap/`round-limit`; job ownership fence;
  command run/done events and agent-scoped shadowing.
- Rationale: last because it depends on the prompt/context system, the job model
  (PTY/subagent as producers), and the command surface (UI).

### 5.1 Explicit exclusions

- **No Cordis / `cordis.yml` / `!!js`.** ymh keeps JSONC config and its own
  composition; only composition *semantics* are mirrored.
- **No PTC `run_code` SDK generator (D22, deferred).** `26-D15` ships `Native`
  only; `Ptc`/`Both` are reserved enum values that fail loud if selected. The
  `PTC_ONLY`/`TOOLS_SDK`/`TOOL_WORKFLOW`/`TOOL_RALPH` order slots remain
  reserved but unregistered. This resolves the prior §5.1/D15 contradiction
  (W-H2): the exclusion is a deferred decision, not a hole inside Waves 1–4.
- **No `tool-workflow`, no `ralph`.**
- **No image/attachment pipeline.** ymh has an `Image` content block
  (`message.hpp:74`) but no attachment service; `FileBlock`/image offload and
  pricing are out of scope. `FileReference` (order 900) is reserved only.
- **No plan-mode UI or `exit_plan_mode` review.** Only the `plan:policy` section
  text and the stable-catalog rule are adopted (D15/I6); the interactive review
  is deferred.
- **No remote/ACP/SDK/Claude-Code/Codex subagent providers.** Only in-process
  spawn/fork.
- **No TUI redesign.** Waves 3–6 add row kinds and panels; the verified `10`/`17`
  chrome-budget and live-only-switcher rules are preserved.
- **No SSH/TCP transport** (already out of scope, `DESIGN_STATUS.md`).

### 5.2 Test strategy (T-M12)

The suite follows §44 (unit, integration with fakes, golden TUI render, replay).
Additions required by this spec:

1. **Replay-determinism harness (new).** Record a session against `FakeLLM`;
   for every `LlmRequestHeader`, rebuild the request **template** from the header
   and the running registries (tool schemas re-derived from the live
   `ToolRegistry`) and assert
   `rebuild.template_digest() == header.template_digest`; for every dispatched
   attempt, rebuild `canonical_json()` twice and assert the two are
   byte-identical. When `session.persist_prompt_text` is off (the default), the
   harness asserts the header carries **no** `system_prompt` key and that
   `sha256(rendered) == header.system_prompt_digest`; when it is on, it also
   asserts the stored text is byte-identical. This is the executable form of
   `26-I2`/`26-F13`/`26-I11`. A companion negative test mutates one tool schema
   and asserts the template rebuild fails loud as a registry mismatch; a second
   negative test perturbs the prompt and asserts the prompt-digest check fails;
   a third asserts a workspace-layer `session.persist_prompt_text` raises
   `ConfigError` (the key is global-only).
2. **Fixture provenance.** Hermetic fixtures are generated by `FakeLLM` runs and
   checked in under `tests/fixtures/` with a generator test that regenerates and
   diffs them; no real-LLM transcript is a hermetic fixture. Live tests remain
   opt-in (`YMH_LIVE_LLM=1`).
3. **Golden prompt hashes.** One golden rendered-prompt hash per
   preset/config combination, plus negative tests for duplicate names, `>1
   complete`, unknown `{{var}}`, and bad `tool_order`.
4. **Scheduler concurrency.** Deterministic barrier/pool ordering tests,
   abort-drain/synthetic-result tests, and a TSan run over the scheduler and the
   `LlmRuntime` registry.
5. **Compatibility (durable/on-disk axis).** Open a checked-in pre-program DB
   fixture (schema 1); assert there is no migration (the version is unchanged),
   that a header-less attempt is marked legacy and not fabricated, that a
   **read-only** open still succeeds under the exact-match rule, and that an
   unknown event type **fails loud** in the on-disk/replay decode
   (`decode_event`, `session_persistence.cpp:228-232`).
6. **Mixed-version wire compatibility (D24).** (a) A handshake with a genuinely
   mismatched **envelope** `protocol_version` is still rejected with
   `UnsupportedProtocol` (`protocol_server.cpp:285-293`) — the envelope fence is
   retained. (b) A new supervisor attached to an old daemon (handshake `1`==`1`)
   decodes every old event type normally. (c) A `SessionEnvelope` carrying an
   unknown event `type` is **skipped**: `on_envelope` is not invoked, the
   connection stays open, and the per-session cursor still advances to the
   notification's `cursor` (so a reconnect does not loop on the skipped event).
   (d) The on-disk replay of that same unknown type still fails loud, proving the
   two axes are independent.
7. **Codec round-trip.** Extend the exhaustive `all_event_types()` wire/encode
   round-trip test (`01 §15.1`) to every new event in §4.3.9.
8. **Property tests.** Canonical-serialization round-trip and strict-variable
   interpolation fuzz.

---

## 6. Open questions and interpretations

These are decisions made on the user's behalf, stated so they can be reversed
cheaply. Items resolved in Rev 2 are marked **RESOLVED**.

1. **Which dsh mechanics are "all mechanics"?** LLM interface, output
   processing, prompt system, and the collaboration way-of-working are in;
   Cordis, PTC/workflow/ralph, attachments/images, and remote transports are out
   (§5.1). **Corrected:** the PTC SDK generator is deferred via D22 and does
   **not** sit inside Waves 1–4; the other exclusions do not block Waves 1–4.
2. **Do we copy the prompt prose verbatim?** Yes for the load-bearing texts:
   harness identity, persona, plan policy, AGENTS.md framing, tool guidance and
   schema descriptions, skill catalog, repeat-tool reminder, model-switch
   notice. ymh keeps its own `harness:identity` wording (the current
   `default_system_prompt()`) and adopts dsh's *structure*; switching to the
   literal DeepSeek sentence is a one-line change in Wave 3.
   **Recommendation: keep ymh's identity, copy dsh's structure and tool
   guidance.**
3. **`LlmCallConfig` vs `GenerationParameters`.** One source of truth:
   `GenerationParameters` maps into `LlmCallConfig` in `buildRequest`, and
   `LlmCallConfig` is what is logged/compared. Fields dsh dropped (`top_p`,
   `seed`, `tool_choice`) stay in `GenerationParameters` **and** are logged in
   `LlmCallConfig`/`canonical_json()` because ymh's adapter still emits them
   (§4.9, N-L2). Open: whether to drop the knobs entirely from the wire
   (`dsh-llm/README.md:154`). **Recommendation: keep them and log them; if they
   are dropped, drop them from wire, config, and digest together.**
4. **Retry location.** The plan adds durable retry events and a step-boundary
   executor but does not immediately move the adapter's internal barrier. Open:
   fully externalize retry in Wave 1 or only add logging. **Recommendation: add
   logging in Wave 1; externalization is a later optional refactor.**
5. **Assistant-stream event shape (D9).** **RESOLVED (Rev 3):** follow dsh
   exactly — embed the compact `AssistantStreamRecord[]` in `assistant/message`
   and retain non-message attempts as `assistant/attempt`. Do **not** add a
   third standalone stream event. Legacy `AssistantChunk` is read-only and never
   migrated.
6. **`Message` provenance (D14).** **RESOLVED:** provenance lives on the shared
   `Message` as `MessageSource`/`ContextForm` (D14, Wave 3), because D6/D7 need
   it and because putting it only on `ContextInjected` cannot express assistant
   model-source provenance. This is breaking and gated by the `01` errata.
7. **Goals/jobs/commands scope.** These are a large program. Waves 5–6 make them
   optional. Open: whether the user wants them at all, or only the
   LLM/prompt/output core. **Recommendation: ship Waves 0–4 first; decide 5–6
   after.**
8. **Preset config format.** dsh uses `agent.cordis.yml`; ymh uses JSONC
   directories under a configured root (mirroring
   `roots`/`includeShippedRoot`/`includeUserRoot`). Deliberate divergence; only
   the *semantics* are copied.
9. **Order-table ownership.** The dsh numbers are copied verbatim, including
   gaps and slots for features ymh does not ship. **Recommendation: keep
   verbatim.**
10. **Scale acknowledgment.** This is a 7-wave, multi-spec program. Wave 1
    (service boundary + logged header + frozen request) delivers the highest
    replay/observability value with zero model-visible change; Wave 3 (prompt
    system) delivers the highest behavioural change.

---

## Appendix — Sources

- dsh packages: real root
  `/home/yury/.npm/_npx/1e7f6d9597241db0/node_modules/@deepseek-ai/`
  (`~/.dsh/profiles/node_modules/@deepseek-ai/` is a symlink farm; recursive
  grep on the symlink path silently misses files). Packages cited:
  `dsh-llm, dsh-llm-deepseek, dsh-llm-retry, dsh-agent, dsh-agent-loop, dsh-deque,
  dsh-chunked-list, dsh-output-retention, dsh-system-prompt,
  dsh-agent-instructions, dsh-persona, dsh-agent-tool-presentation,
  dsh-agent-presets, dsh-plan-mode, dsh-invariants, dsh-compaction,
  dsh-compaction-basic, dsh-compaction-tool-result-pruner, dsh-goal, dsh-jobs,
  dsh-commands, dsh-tool-fs-search, dsh-tool-goal, dsh-subagent,
  dsh-subagent-in-process-driver, dsh-tool-skill, dsh-app-boot, dsh-base`.
  Sources: `README.md`, `lib/types/*.d.ts`, `*.cordis.yml`, and — **prompt prose
  and centrally allocated constants only** — `lib/index.js`. No other minified
  `lib/*.js` is used as evidence.
- Working studies: `/tmp/opencode/dsh-prompts.md`, `/tmp/opencode/dsh-collab.md`
  (indexes only; every load-bearing quote re-checked against the package).
- ymh: `docs/design/00-architecture.md` (§10, §11, §12, §14, §31–§33),
  `06-agent-loop.md`, `07-tools-execution.md`, `08-llm-provider.md`,
  `09-permissions.md`, `13-context-compaction.md`, `18-context-errata.md`,
  `20-skills.md`, `21-config-jsonc-errata.md`, `22-switcher-sessions-errata.md`,
  `23-session-lifecycle-errata.md`, `24-agent-lifetime-errata.md`,
  `DESIGN_STATUS.md`; `include/ymh/llm/*`, `include/ymh/agent/*`,
  `include/ymh/session/events.hpp`, `include/ymh/core/event.hpp`,
  `include/ymh/policy/permission_policy.hpp`, `include/ymh/tools/*`,
  `include/ymh/execution/environment.hpp`, `src/llm/*`, `src/agent/*`,
  `src/execution/*`, `src/policy/*`, `src/session/*`, `src/cli/wiring.cpp`.

---

*Rev 2 (2026-09-18): repair pass after the independent gate
(`/tmp/opencode/gate26.md`). Full revision log in
`26-dsh-alignment.md` §7. Part 2 changes: reclassified D7/D11/D13/D14/D16 as
breaking and reconciled §4.4; defined every previously unpinned type
(`ReplayEnvelope`, `AssistantStreamRecord`, `TimedStreamEvent`, `AssembleContext`,
`ToolScheduleOutcome`, `ContextAcceptor`, the retention types, `PruneResult`,
`ToolRestriction`, `AgentContext`, `GoalId`, `GoalBlockReason`, `CommandHandler`,
`Job`, `Deque`, `ChunkedList`); removed persisted `RequestId`; added the
canonical serialization/reconstruction contract; added the prompt `assemble()`
output path; fixed `PreparedCall` constness; moved `render_prompt` enforcement to
`assemble()`; re-seamed `ContextCompactor` to `LlmRuntime&`; switched the pruner
to code points and `PRUNE_MARKER`; added the new-event codec table, the migration
plan, the error taxonomy, the concurrency contract, the config key table, the
real test strategy, and the expanded Wave 0 gate.*

*Rev 3 (2026-09-18): repair pass after the re-gate (`/tmp/opencode/regate26.md`;
open HIGH = 1, MEDIUM = 6, LOW = 5). Full revision log in `26-dsh-alignment.md`
§7. Part 2 changes: restated the reconstruction contract as "log plus running
registries" (N-H1); reshaped `AssistantStreamRecord` to dsh's discriminated
union, embedded the compact stream in `assistant/message`, added
`assistant/attempt`, and made `AssistantChunk` legacy read-only (N-M1); defined
`MessageSource`/`ContextForm`/`ContextFormed`/`ContextSnapshotSection` and the
command-handler types (N-M2); pinned every new-event payload and stated the
deliberate wire-name divergence (N-M3); added the migration DDL/versioning/
read-only rules (N-M5); pinned adapter lifetime and the scheduler service
classification (N-M6); added `00` to Wave 0, folded `top_p`/`seed`/`tool_choice`
into `LlmCallConfig`/`canonical_json()`, removed `header_seq`, completed
`Deque`/`ChunkedList`, renamed `build_request` to `buildRequest`, pinned
`CallPurpose`, and aligned the instructions loader to dsh (N-L1–N-L5).*

*Rev 4 (2026-09-19): repair pass after the Rev-3 re-gate
(`/tmp/opencode/regate26-rev3.md`; open HIGH = 1, MEDIUM = 3, LOW = 6). Full
revision log in `26-dsh-alignment.md` §7. Part 2 changes: chose the
changed-snapshot + **template-digest** design and made `26-I2`, D2, §4.3.2,
§4.5 (`26-F13`) and the §5.2 harness agree — added
`FrozenRequest::canonical_template()`/`template_digest()`, renamed the header
field to `template_digest`, and excluded `messages` from the header digest
(NEW-H1); added `session_id`/`purpose` to `LlmRequestHeader` (NEW-M1); pinned the
JSON key schemas for every new/changed payload in §4.3.9.1 (NEW-M2); replaced
`GoalChanged`/`goal/changed` with the durable `goal/change` → `GoalChange`
(NEW-M3); removed D2's stale sequence clause, corrected the ymh citations and
deleted the unsupported `supervisor.cpp:1487`, single-homed `replay_state` on the
assistant event, removed the zero-DDL schema bump (schema stays 1), stated the
`push()` return-type divergence, and pinned `ContextFormed` field coupling via
`26-I7` (NEW-L1–NEW-L6).*

*Rev 5 (2026-09-19): repair pass after the Rev-4 re-gate
(`/tmp/opencode/regate26-rev4.md`; open HIGH = 0, MEDIUM = 5, LOW = 5). The five
MEDIUMs were first identified by the adversarial planning panel
(`/tmp/opencode/hyperplan-bundle.md`, `/tmp/opencode/hyperplan-r2.md`) and then
independently confirmed by the re-gate. Full revision log in
`26-dsh-alignment.md` §7. Part 2 changes: added `26-I11`/`26-D23` (prompt text
durable only under `logging.log_prompts`; header always carries
`system_prompt_digest`) and threaded it through `26-I2`, D2, §4.3.2, §4.3.9.1,
§4.9 and the §5.2 harness; added `26-I12`/`26-D24` (bump `kProtocolVersion`
1→2 in Wave 1) and the §4.6/§5.2 protocol rules; added §4.3.9.2 (the
70-switch consumer matrix) and made `AssistantChunk` a live-only publication in
D9/§4.6/Wave 2 with the headless/UI migration; restaged Wave 0 into Stage A
(up-front `00`+`01`+`08`) and Stage B (JIT per wave); reclassified D1 as
`Brk. (06,08)` and extended §4.4 with 00/06/08/05/04/16/07/13/15 rows and a
cascade-coverage note; escalated the no-downgrade door to a product sign-off;
corrected the Wave 4 dependency rationale; stated the `ReplayEnvelope`
divergence; fixed the `permission_policy.hpp:120` citation (Rev-4 LOWs).*

*Rev 6 (2026-09-19): repair pass after the Rev-5 re-gate
(`/tmp/opencode/regate26-rev5.md`; open HIGH = 0, MEDIUM = 2, LOW = 4). Rev 6
resolves **M-3 → N-2** only; **M-1/N-1 (prompt-logging policy) is deliberately
left open** — it is the user's decision, not this revision's. Part 2 changes:
replaced `26-I12` and `26-D24` with the **decoupled** design (option (b)) —
`kProtocolVersion` stays `1`, new event types do not bump it, the wire receiver
**skips** an unknown event `type` (advancing the per-session cursor) while the
on-disk/replay decode keeps its loud `CorruptionError` fence
(`session_persistence.cpp:228-232`, `01` S3); rewrote §4.6 as an explicit
two-axis compat story and pinned the spec-24 outliving-daemon rationale;
reclassified §4.4 rows `00`/`05` and marked `04`/`16` unaffected, with `24` now
explicitly consistent with `AL20`/§1.3.3 (no spec-24 errata required — the Rev-5
gate's N-2 conflict is dissolved by removing the bump); updated the §5 Stage-B
Wave-1 row and the Wave-1 task list; replaced §5.2 test 5/6 with the two-axis
compatibility harness (mixed-version attach, wire skip + cursor advance, loud
on-disk decode). No dsh claim changes.*

*Rev 7 (2026-09-19): repair pass for finding M-1 → N-1 (durable full-prompt
persistence). `logging.log_prompts` is no longer reused as the durable opt-in
(that reuse conflated the spdlog surface with session-DB persistence and
contradicted `config.hpp:84-86` / `21` §7.6). A dedicated
`session.persist_prompt_text` key (bool, default `false`, new `[session]`
section, **global layer only**) now gates the session-DB copy and is distinct
from `logging.log_prompts` (spdlog only). Reconciled `26-I11`, `26-I2`, D2, D23,
the `LlmRequestHeader` struct and its §4.3.9.1 JSON key row, §4.3.2
(reconstruction contract + mechanism-vs-policy note), §4.9 (new key row; no
reuse) and the §5.2 harness (default asserts no stored text; global-only
negative test). No dsh claim changes — dsh has no durable prompt-persistence
config and the changed-snapshot `callConfigEquals` model is untouched. The
policy decision remains the user's; the mechanism defaults to digest-only.*

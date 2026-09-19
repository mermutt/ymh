# 28 — LLM Service Boundary Errata (dsh alignment, Wave 0 A2)

```
Status: written · verified: — · reviewer: — (tracked in DESIGN_STATUS.md)
Revision: Rev 3 — pins the `LlmCallConfig::provider` source, the cross-cutting
          decision that gate31's spec-31 MEDIUM and gate32's spec-32 M2 converged
          on: `AgentConfig::provider` (a new field on `agent.hpp:94-105`),
          populated by `to_agent_config` from `config.llm.provider`
          (`config.hpp:101`; `wiring.cpp:63,174-190`), with the runtime's
          registered default route when unset and a loud typed
          `LLMErrorCode::NoProviderRoute` failure at `prepare_call` — §3.5,
          §5.3, §7, §8, §9 (L26), §10 (L-F27), §13.8, 28-D9. Specs 31/32
          **reference** this pin; they define no competing source. Rev 2 — closes
          the gate28 MEDIUMs and LOWs. (1) `PreparedCall::retry_policy()`
          is sourced from `LLMProvider::retry_policy()`, captured by
          `register_adapter` (signature unchanged) — §3.1, §6.1. (2)
          `canonical_template()`'s `system_prompt` and `system_prompt_digest` basis
          is pinned to the frozen request's `Role::System` message text, and §5.4 is
          reconciled so a plan-mode turn's digest matches replay — §4.2, §5.4.
          LOWs: (3) `to_string(InvalidPreparedCall)` = `"invalid_prepared_call"` and
          the exhaustive-switch audit — §7; (4) the loop's dispatch path is
          `prepare_call`→`PreparedCall::stream` — §3.1; (5) adapter ownership stated
          once (the runtime owns registered adapters) — §3.3, §3.4, 28-D2; (6)
          `schema_version` constant/lifecycle pinned — §4.2, 28-D4. Rev 0 is retained
          below.
Component: 28 (errata) — amends 08-llm-provider.md by reference; also pins
           changes owned by 06-agent-loop.md §4/§5 and 13-context-compaction.md
           §5.2 (each owned by its own errata)
Depends on: docs/design/26-dsh-alignment-part2.md (Rev 7, GATE PASS) §4.1
            (26-I2/I3/I4/I11), §4.2 (26-D1/D2/D3/D23), §4.3.1, §4.3.2, §4.3.4,
            §4.6, §4.7, §4.8, §5 Wave 0 A2 and Wave 1; docs/design/26-dsh-
            alignment.md §2.1.1–§2.1.3, §2.1.7 (the dsh contract);
            08-llm-provider.md (verified); the tree (all `file:line` re-derived)
Scope:    pin the provider-neutral call service (`LlmRuntime`) above
          `LLMProvider`; the deep-frozen request and its canonical serialization
          /template-digest reconstruction contract; the logged request header as
          a changed snapshot; one provider attempt per stream with retry as a
          separate concern; the terminal result at the service boundary; the
          `ContextCompactor` re-seam; `LLMErrorCode::InvalidPreparedCall`; and
          the `LlmCallConfig::provider` source (`AgentConfig::provider` → the
          runtime's default route → a loud typed no-route failure).
Supersedes: (quoted with anchors; each is a 08 clause this errata replaces)
          - 08 §3.7 :469-492 "Retryable codes (only before any event is
            dispatched) … Retry policy is config-driven" — the pre-first-event
            barrier (L7) is **retained**; the *placement* of retry (the adapter
            comment "Applied by the adapter", `include/ymh/llm/llm_provider.hpp:
            39-40`; characterized "Retry lives **inside** the adapter" in 26
            §3 G5 :1220) is superseded by the separate-executor end state
            (26-I3), with the no-double-retry migration constraint in §6.
          - 08 §5.1 :656-659 "spec 06 owns the resolved `LLMProvider` instance
            and schedules `stream()` on the `LLMPool` (decision (n))" — the loop
            now holds an `LlmRuntime&`, not a resolved `LLMProvider*` (26-D1).
          - 08 §14.1(n) :1340-1343 "`ProviderRegistry` ownership … spec 06 owns
            the resolved provider" — superseded by §3.4 below.
          - 08 §3.1 :238-245 `LLMRequest` — extended additively with
            `session_id` and `purpose` (26-D3); the existing fields are retained.
          - 08 §3.1 :228-236 `GenerationParameters` — retained; `max_output_tokens`
            is deprecated in favour of `LlmCallConfig::max_tokens` (T-M5), mapped
            once in `buildRequest`.
Retained: L1–L17 (L7 explicitly retained; only the retry-placement text of
          §3.7/the adapter comment is superseded, as noted above), the
          `LLMProvider` adapter contract (§3.4), the stream-event algebra (§3.2),
          `StreamSink`/`SinkFlow` (§3.3), `LLMResponse` as the terminal value,
          the `LLMPool` caller-brackets discipline (08 decision (r), 06 §5.9),
          `FakeLLM`, and the config layering (§5.3).
```

> **Numbering note (authoritative).** This document **claims number 28**.
> `26-dsh-alignment-part2.md` §5 reserved `27`–`30` for `27-system-prompt.md`,
> `28-output-retention.md`, `29-agent-presets.md`, and
> `30-goals-jobs-commands.md`; that reservation is **stale**. The actual Wave-0
> allocation is: **27 = session locking** (`27-session-locking-errata.md`),
> **28 = LLM service boundary** (this file), **29 = event family**
> (`29-event-family-errata.md`), **30 = architecture cascade**
> (`30-architecture-cascade-errata.md`). The Wave-3+ specs must therefore be
> **renumbered to 31+** when their errata are written; this errata does not
> reserve a number for them.

**Naming note.** Invariants local to this errata extend the 08 `L` namespace as
**`L18`–`L26`** (08 ends at `L17`, `08 §10 :981-1057`); failure modes extend as
**`L-F19`–`L-F27`** (08 ends at `L-F18`); decisions are **`28-D1`–`28-D9`**.
The `28-D#` prefix cannot collide with `26-D#` (the alignment register) or with
the 08 decision letters `(a)`–`(u)`.

**What this errata does not do.** It does not rewrite `08-llm-provider.md`; every
superseded clause is quoted above with its anchor and its replacement is given
here, matching the convention of `21-config-jsonc-errata.md` and
`23-session-lifecycle-errata.md`. It does not pin the `llm/request_header` or
`llm/retry` **wire codecs** (owned by `29-event-family-errata.md`, keys pinned in
`26` §4.3.9.1 :943-948) or the `session.persist_prompt_text` config key (owned by
`21`). It records the ownership gaps and the required 21 amendment in §13 rather
than inventing a resolution.

---

## 1. Purpose and scope

`08-llm-provider.md` pins `LLMProvider` as **the** LLM seam and the loop holds a
raw `LLMProvider*`. The verified alignment design (`26` §4.3.1, §4.2) inserts a
provider-neutral **call service** `LlmRuntime` above the adapters, freezes every
dispatched request, logs a durable request header as a changed snapshot, and
moves retry to a separate durable executor. This errata pins that boundary so
Wave 1 code can be written.

### 1.1 Current state (verified against the tree)

- The loop's `AgentServices` holds `ProviderRegistry* providers`
  (`include/ymh/agent/agent_loop.hpp:53`), `LLMProvider* provider` (`:64`), and
  `LLMProviderConfig provider_config` (`:65`); it calls
  `services_.provider->stream(request, sink, turnToken).get()` directly
  (`src/agent/agent_loop.cpp:781`).
- `AgentLoop::buildRequest` builds a plain `LLMRequest` from `AgentConfig::model`
  and `AgentConfig::parameters` (`src/agent/agent_loop.cpp:410-419`;
  `include/ymh/agent/agent.hpp:94-104`). Nothing freezes it and nothing logs a
  request header.
- Retry lives **inside** the adapter: the OpenAI-compatible adapter loops
  `max_attempts` times (`src/llm/openai_adapter.cpp:923-1000`) and retries only
  while `!decoder.any_event()` (`:983`); `is_retryable_code`
  (`include/ymh/llm/stream.hpp:112-123`) is the code filter; `RetryPolicy` is a
  field of `LLMProviderConfig` (`include/ymh/llm/provider_registry.hpp:34`,
  `include/ymh/llm/llm_provider.hpp:41-47`). Retries are not logged.
- `ContextCompactor` takes `LLMProvider&` (`include/ymh/agent/compactor.hpp:113-117`,
  `src/agent/compactor.cpp:117-126`), stores `provider_`
  (`include/ymh/agent/compactor.hpp:140`), and calls
  `provider_.stream(request, collect, cancel).get()`
  (`src/agent/compactor.cpp:252`).
- The daemon owns one `ProviderRegistry providers_` and one
  `std::unique_ptr<LLMProvider> provider_`
  (`src/agent/workspace_runtime.cpp:198,200`), wires them into `AgentServices`
  (`:165-167`), and constructs the compactor from `*provider_` (`:170-172`).
- `include/ymh/session/events.hpp` has **no** request/header event (grep for
  `request`/`header` finds only `ToolCall::requestedAt` at `:117`).

### 1.2 Target (pinned here)

`LlmRuntime` is the loop's handle. Requests are frozen before dispatch and
reconstructable from the log. A durable `LlmRequestHeader` is appended as a
**changed snapshot**. `LLMProvider::stream` is **one provider attempt**; retry is
a separate durable executor. Every service-level `stream()` returns a terminal
`LLMResponse`. `ContextCompactor` is re-seamed to `LlmRuntime&`.
`LlmCallConfig::provider` is config-sourced from `AgentConfig::provider`; an
unset value resolves to the runtime's registered default route, and a request
with no matching route fails loud at `prepare_call` (§3.5).

---

## 2. Breaking-change classification

Per `26` §4.2 :139 and §4.4 :1091-1093, restated precisely:

| Decision | Class | Owning spec | Wave |
|---|---|---|---|
| 26-D1 — `LlmRuntime` above `LLMProvider`; loop no longer holds a raw provider | **Brk. (06, 08)** | 06, 08 | 1 |
| 26-D2 — `LlmCallConfig` + `call_config_equals` + logged header changed snapshot | **Add.** in the register; **Brk.** for the 08 dispatch path (§4.4 :1093) | 01, 08, 21 | 1 |
| 26-D3 — freeze the request envelope; `session_id`/`purpose`; one adapter generation per `PreparedCall` | **Add.** in the register; **Brk.** for the 08 dispatch path (§4.4 :1093) | 08 | 1 |
| 26-D8/D9 — canonical assembler + assistant stream/replay | Add. | 08 | 2 |
| 26-D13 — summarizer routes through `LlmRuntime` | Brk. (spec 13 payload) | 13, 01, 08 | 4 |

The register marks D2/D3 `Add.` because the `LLMProvider` **adapter contract** is
retained; §4.4 marks the 08 impact `Brk. (D1/D2/D3)` because the loop's pinned
dispatch path changes. Both are true and are not contradictory: the adapter
contract is additive-safe, the loop seam is breaking. **`08 §3.4` (`LLMProvider`)
is retained; `AgentServices::provider` (`agent_loop.hpp:64`) is replaced.**

**Rev 3 (28-D9) is additive.** The provider-source pin adds
`AgentConfig::provider` (a new field on the runtime config, no JSON key of its
own) and `LLMErrorCode::NoProviderRoute` (a new closed-enum value). The
`LLMProvider` adapter contract is unchanged, and no wire codec changes: it is an
`Add.` pin owned by `28` (§3.5), consumed by `31`/`32`.

---

## 3. The `LlmRuntime` seam (26-D1)

### 3.1 Interface (pinned)

```cpp
namespace ymh {

// Provider-neutral call configuration. `max_tokens` is the single canonical
// sampling-budget name; GenerationParameters::max_output_tokens maps onto it
// 1:1 in exactly one place (buildRequest), so there is one source of truth at
// dispatch (26 §4.3.1 :177-180; T-M5).
struct LlmCallConfig {
    ProviderId                   provider;   // source + default-route fallback: §3.5
    ModelId                      model;
    std::optional<std::string>   reasoning_effort;
    std::optional<double>        temperature;
    std::optional<std::uint32_t> max_tokens;
    std::vector<std::string>     stop;
    // ymh extensions beyond dsh's 6-field config; the adapter emits them on the
    // wire (src/llm/openai_adapter.cpp:736-752), so they are logged and covered
    // by canonical_json() (26 §4.3.1 :188-194).
    std::optional<double>        top_p;
    std::optional<std::uint32_t> seed;
    std::optional<std::string>   tool_choice;
};

// dsh GenerateOptions.purpose (26-dsh-alignment.md §2.1; part2 §4.3.1 :197-203).
// Absent = an ordinary conversation request.
enum class CallPurpose : std::uint8_t { Compaction, SessionTitle };

// The ONLY change test. No defaulted operator==/operator<=> is declared:
// call_config_equals is the documented semantic (dsh callConfigEquals), so there
// is no second, subtly different equality (26 §4.3.1 :205-208).
[[nodiscard]] bool call_config_equals(const LlmCallConfig&, const LlmCallConfig&) noexcept;

// Immutable, detached request envelope. `freeze` deep-copies and pins the
// canonical serialization; nothing may mutate after freeze (26-D3, 26-F2).
class FrozenRequest {
public:
    [[nodiscard]] static FrozenRequest freeze(LLMRequest request, LlmCallConfig config);
    [[nodiscard]] const LLMRequest&    get() const noexcept;
    [[nodiscard]] const LlmCallConfig& config() const noexcept;

    // Canonical serialization (§4 below).
    [[nodiscard]] std::string canonical_template() const;
    [[nodiscard]] std::string template_digest() const;  // SHA-256 hex of canonical_template()
    [[nodiscard]] std::string canonical_json() const;   // template + ordered messages
    [[nodiscard]] std::string digest() const;           // SHA-256 hex of canonical_json()
private:
    LLMRequest   request_;
    LlmCallConfig config_;
};

// One model call whose config and adapter registration were resolved together.
// One-shot: `stream` consumes the object; a second call or a config mismatch
// throws PreparedCallError (§7). Non-const for that reason (26 §4.3.1 :235-247).
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

struct ProviderInfo {
    ProviderId                id;
    std::vector<ModelId>      models;
    ProviderCapabilities      capabilities;
};

// RAII; move-only; destructor unregisters the routes (26 §4.3.1 :255-261).
class AdapterHandle {
public:
    AdapterHandle(AdapterHandle&&) noexcept;
    AdapterHandle& operator=(AdapterHandle&&) noexcept;
    ~AdapterHandle();
    AdapterHandle(const AdapterHandle&) = delete;
};

// The `llm/stream` waterfall analogue: a registered interceptor wraps the next
// call. Registration returns an RAII handle; ordering is registration order.
using StreamNext        = std::function<Task<LLMResponse>(
                              const FrozenRequest&, StreamSink, CancellationToken)>;
using StreamInterceptor = std::function<Task<LLMResponse>(
                              const FrozenRequest&, StreamSink, CancellationToken, StreamNext)>;
class InterceptorHandle { /* RAII; move-only; destructor unregisters */ };

// The provider-neutral service. The loop holds an LlmRuntime&, not an
// LLMProvider*. Adapters are owned by shared_ptr; the runtime keeps a
// registration alive until every in-flight dispatch that captured it has
// finished (26 §4.3.1 :269-284, §4.8 :1259-1270).
class LlmRuntime {
public:
    AdapterHandle register_adapter(std::vector<ProviderId> routes,
                                   std::shared_ptr<LLMProvider> adapter);
    [[nodiscard]] std::vector<ProviderInfo> list_providers() const;
    InterceptorHandle add_stream_interceptor(StreamInterceptor);
    [[nodiscard]] Task<PreparedCall> prepare_call(LlmCallConfig, CancellationToken) const;
    [[nodiscard]] Task<LLMResponse>  stream(const FrozenRequest&, StreamSink, CancellationToken);
};

} // namespace ymh
```

**Retry-policy source (gate28 MEDIUM-1 pin).** `PreparedCall::retry_policy()` is
sourced from the adapter, captured at registration — not from a config held by
the executor. `LLMProvider` gains **one non-pure virtual accessor** (an additive
amendment to the 08 §3.4 adapter contract):

```cpp
// include/ymh/llm/llm_provider.hpp:61 (current) → target: one added method.
class LLMProvider {
    // … id()/stream()/capabilities()/models() unchanged …
    // The provider's configured retry/backoff policy (08 §3.7). Non-pure so test
    // doubles need not override; OpenAICompatibleProvider returns its stored
    // LLMProviderConfig::retry.
    [[nodiscard]] virtual RetryPolicy retry_policy() const { return {}; }
};
```

`register_adapter`'s signature is **unchanged** (`std::vector<ProviderId>,
std::shared_ptr<LLMProvider>`, §3.1 :236-237; 26 §4.3.1 :278-279): it captures
`adapter->retry_policy()` into the registration record. This is chosen over a
`RetryPolicy` parameter on `register_adapter` because it leaves the frozen 26
§4.3.1 signature byte-for-byte and keeps a single source of truth: the adapter's
stored `LLMProviderConfig::retry` (`include/ymh/llm/provider_registry.hpp:34`),
which the daemon holds (`src/agent/workspace_runtime.cpp:199`), populates via
`to_provider_config` (`src/cli/wiring.cpp:70-74`), and passes through the factory
into `OpenAICompatibleProvider::config_` (`include/ymh/llm/openai_adapter.hpp:87`).
`prepare_call` binds the captured policy into the `PreparedCall`; a policy change
therefore requires a re-registration (a new adapter generation), consistent with
26-D3's one-generation binding.

**Dispatch path (gate28 LOW-2 pin).** The loop uses `prepare_call` →
`PreparedCall::stream` (one-shot, one bound adapter generation), as 26-D3
requires; the loop never calls the direct `LlmRuntime::stream`. The direct
`LlmRuntime::stream(const FrozenRequest&, …)` remains the public
interceptor-chain entry for callers that already hold a `FrozenRequest` and need
no one-shot generation binding (it copies the currently-selected adapter under
the lock, §3.3). `ContextCompactor` (§8) likewise uses `prepare_call` →
`PreparedCall::stream`.

### 3.2 `LLMRequest` extension (26-D3)

`LLMRequest` (`include/ymh/llm/llm_request.hpp:54-61`) gains two fields, additively:

```cpp
struct LLMRequest {
    ModelId                  model;
    std::vector<Message>     messages;
    std::vector<ToolSchema>  tools;
    GenerationParameters     parameters;
    RequestId                request_id = 0;      // never persisted (see below)
    std::chrono::milliseconds deadline{0};
    SessionId                session_id;           // NEW (26-D3)
    std::optional<CallPurpose> purpose;            // NEW; absent == conversation
};
```

`RequestId` stays **"Never persisted"** (`llm_request.hpp:28-30`; 26-D2 :140).
Retries correlate by `retry_id` + `(turn, step)`, and a header is found
**positionally** (the last header before the attempt); no self-referential
sequence field is stored.

### 3.3 Ownership and lifetime (26 §4.8)

- **One `LlmRuntime` per workspace daemon**, alongside the existing
  `ProviderRegistry providers_` (`src/agent/workspace_runtime.cpp:198`). The
  daemon **owns the runtime**; the runtime replaces the loop's direct handle.
- **Ownership stated once: the runtime owns the registered adapters**, via the
  `shared_ptr` it stores. The daemon constructs adapter instances through the
  factory at boot and calls `register_adapter`, but keeps **no owning reference**
  to them (`src/agent/workspace_runtime.cpp:199-200` today holds the config and a
  single `unique_ptr`; Wave 1 moves that handle into the runtime).
  `AdapterHandle`'s destructor erases the routes, but the adapter object is
  destroyed only when the last `shared_ptr` — the registration or an in-flight
  dispatch — is released. A `PreparedCall` holds the `shared_ptr` it bound at
  `prepare_call`, so its adapter generation survives through `stream`
  (26 §4.8 :1259-1270).
- **Mutex discipline.** Registry mutation and interceptor registration take an
  internal mutex; `stream`/`prepare_call` copy the selected adapter's
  `shared_ptr` and the interceptor chain under the lock, then release it before
  dispatching. No registration may race a dispatch — this is a consequence of
  shared ownership plus the short critical section, not an assertion.
- **Interceptors** run in registration order and wrap the next call; their
  handle is RAII (26 §4.3.1 :263-267).

### 3.4 Relationship to `ProviderRegistry` and how the loop reaches it

- `ProviderRegistry`/`ProviderFactory`/`LLMProviderConfig`
  (`include/ymh/llm/provider_registry.hpp:25-57`), `register_builtin_providers`
  and `make_default_provider_registry` (`:60-63`) are **retained as the
  construction path**: the daemon creates adapter instances through the factory
  at boot and registers them into the runtime. `create()` keeps its pinned
  contract (validate by name, never read the secret, never touch the network;
  `08 §5.1 :661-667`).
- **`AgentServices` changes (owned by the 06 errata).** The three fields
  `providers` (`agent_loop.hpp:53`), `provider` (`:64`), and `provider_config`
  (`:65`) are replaced by a single `LlmRuntime* runtime`. `AgentLoop` reaches
  the service only through `services_.runtime`; no `LLMProvider*` remains in
  `AgentServices` (26-D1; L18).
- `AgentRegistry`'s fallback provider construction
  (`src/agent/agent_registry.cpp:37-44,111`) moves to the daemon; the registry
  no longer creates adapters. This is 06-owned.
- **Ownership supersession.** `08 §5.1 :656-659` and `08 §14.1(n) :1340-1343`
  ("spec 06 owns the resolved `LLMProvider` instance") are superseded: the
  daemon owns the runtime; the runtime owns its registered adapters (§3.3); the
  loop owns neither.
- `ProviderRegistry` has no `unregister` today; route removal is the runtime's
  `AdapterHandle`. Whether the factory stays the only construction path or the
  runtime also exposes `list_providers`-backed discovery is pinned by the
  runtime interface above (`list_providers` returns `ProviderInfo`).

### 3.5 The `LlmCallConfig::provider` source (Rev 3)

This is the cross-cutting decision that **gate31's spec-31 MEDIUM and gate32's
spec-32 M2** independently converged on. `31-agent-loop-errata.md` (Rev 2) and
`32-compaction-errata.md` (Rev 1) **reference this pin**; they define no
competing source.

- **Source.** `LlmCallConfig::provider` is sourced from a new
  **`AgentConfig::provider`** field (`include/ymh/agent/agent.hpp:94-105`),
  populated by `to_agent_config` from `config.llm.provider`
  (`include/ymh/config/config.hpp:101`; `src/cli/wiring.cpp:63,174-190`) — the
  same config knob that names the adapter's route today, so there is one source,
  not two. It is config-driven, mirroring how a localcode profile names its
  provider (`25-ui-ux-errata.md` §9.1 :1735:
  `profiles[name] = {provider, model, …}`).
- **Layer, default, scope.** `AgentConfig::provider` has no separate JSON key of
  its own: it inherits `config.llm.provider`'s layering (global then workspace
  JSONC, workspace overrides; `21` §7.7) and its default `"openai-compatible"`
  (`config.hpp:101`). `AgentConfig` is constructed once per daemon by
  `to_agent_config`, so the field is daemon-scoped and fixed for the daemon's
  lifetime; it is **empty** iff `config.llm.provider` is empty.
- **Empty = the runtime's default route.** `LlmCallConfig::provider` is a
  required field (the interface above) but may carry the empty sentinel. An
  empty value means "resolve the runtime's registered **default route** at
  `prepare_call`"; `buildRequest` does not invent a route. The runtime's default
  route is a **28-owned representation** (dsh has no explicit default-route
  concept): the **first non-empty `ProviderId` of the first adapter registered,
  in registration order**, computed at route-lookup time over the currently
  registered routes (a route is a non-empty id; an empty registration
  contributes none). When no adapter/route is registered there is no default
  route. `resolve_call_config` is deliberately **not** added: the provider is
  resolved by `prepare_call`'s route lookup, not by a separate service call.
  `prepare_call` stores the input `LlmCallConfig` **verbatim** in
  `PreparedCall::config()`: it binds the resolved adapter but does **not**
  back-fill `config.provider`, so the §7 one-shot
  `call_config_equals(request.config(), config())` check holds for sentinel
  calls.
- **No route is a loud typed failure.** `prepare_call` resolves
  `config.provider`: an empty value selects the default route, a non-empty value
  must match a registered route exactly. If neither resolves (the named provider
  is unregistered, or the value is empty and no default route exists),
  `prepare_call` fails **before producing a `PreparedCall`** with a typed
  `NoProviderRouteError` carrying the new additive enumerator
  `LLMErrorCode::NoProviderRoute` (§7) and `detail = "no route for
  config.provider"`. It is never a silent fallback to another provider. The
  direct `LlmRuntime::stream` route lookup raises the same error. At the loop
  boundary it normalizes to `AgentErrorCode::ProviderFailed` (`31` A-F23); the
  compactor maps it to `CompactionError::Code::NoProviderRoute` (`32` §2.1,
  C-F24).

---

## 4. The frozen request and the reconstruction contract (26-D3, 26-I2)

### 4.1 Deep freeze

`FrozenRequest::freeze(LLMRequest, LlmCallConfig)` deep-copies the request and
config and pins the canonical serialization. `FrozenRequest` is **immutable by
type**: no `mutable` member, no `const_cast` path, and a debug sentinel checks
that no byte changed after freeze (26-F2). Once frozen, neither the loop nor an
interceptor nor an adapter can rewrite it (26-I2).

### 4.2 Canonical serialization

`canonical_template()` is a deterministic JSON object:

- `schema_version` — the canonical-serialization schema version, an integer
  constant `kTemplateSchemaVersion = 1` at introduction (gate28 LOW-4 pin). It is
  **independent of** the session-DB `kSchemaVersion` (stays `1`, §13.6) and the
  wire `kProtocolVersion` (stays `1`, 26-D24). It changes only when the template
  field set or the canonicalization rules change in a way that re-bases every
  `template_digest`; that is a breaking change to the reconstruction contract and
  must be recorded (pre-existing digests are no longer comparable).
- `envelope` — `session_id` + optional `purpose` (D3: an adapter may map both to
  transport metadata, so they are covered by the digest)
- `config` — every `LlmCallConfig` field that can reach the provider wire,
  including the `top_p`/`seed`/`tool_choice` extensions
- `system_prompt` — the **text of the frozen request's `Role::System` message**
  (`messages[0]`; the assembler inserts at most one, at index 0 —
  `src/agent/context_assembler.cpp:44-63`). This is the *assembled* text, not the
  bare `AgentConfig::system_prompt`: in plan mode the assembler appends
  `AgentConfig::plan_section` (the assembler's plan branch invokes the injected
  `plan_policy_` at `src/agent/context_assembler.cpp:46-53`; the
  `plan_mode_.active(session)` gate is `src/agent/workspace_runtime.cpp:151`,
  provider wired at `:149-152`). When no `Role::System` message
  exists, it is the empty string (still serialized, so the field is always
  present). **Always hashed** into `template_digest`; persisted in the header only
  under the opt-in (§5.4).
- `tools` — each schema's `name`, `description`, `parameters` with recursively
  sorted object keys

`canonical_template()` extracts `system_prompt` from `messages[0]` while
excluding the remaining `messages`; `canonical_json()` adds the full ordered
`messages` (§4.3). This extraction rule is the single definition of the prompt
input and of `system_prompt_digest`.

`canonical_json()` is the same object plus `messages` (role + ordered content
blocks; tool-call arguments as their raw JSON string). Rules (26 §4.3.2
:341-353): sorted object keys, stable across runs/platforms/builds; **no
timestamps, ids, or iteration-order-dependent data**; tool schemas in canonical
order.

### 4.3 Template vs full request

- `template_digest()` is SHA-256 hex of `canonical_template()`. It is what the
  logged header carries, because the header is a changed snapshot and is **not**
  re-logged per dispatch.
- `digest()` is SHA-256 hex of `canonical_json()`. It is what the determinism
  half of the replay harness compares across two rebuilds of one attempt.

### 4.4 Reconstruction guarantee (26-I2)

`canonical_template()` is a **changed-snapshot** record, not a per-request
digest: messages grow every step, so a digest over them would be stale the
moment the next step is appended.

Replay:

1. Finds the last `LlmRequestHeader` before the attempt (positionally).
2. Rebuilds the template from it — re-deriving the tool schemas from the live
   `ToolRegistry` and verifying each against `header.tool_schema_digests`.
3. Asserts `rebuild.template_digest() == header.template_digest`.
4. Reconstructs the attempt's messages from the logged message events and asserts
   two rebuilds of `canonical_json()` are byte-identical.

**Nothing is fabricated.** A replay that finds no header for an attempt marks it
**legacy / unreconstructable** (26-F15; §4.6). A replay whose re-derived schemas
do not match the digests **fails loud** rather than guessing (26-F6, 26-F13).

### 4.5 Tool-catalog and prompt changes

A tool-catalog change (names **or** schemas) or a rendered-prompt change forces a
new header and a **new series** (`starts_series = true`), because the tool
schemas and the prompt are inputs to `template_digest`. Because the prompt is the
assembled `Role::System` text (§4.2), toggling plan mode changes the digest and
therefore starts a new series. This is the direct analogue of dsh
`call-config.d.ts:1-6` "logs changed snapshots instead of allowing silent
per-call drift" (26 §4.3.2 :330-339, 26-I6).

---

## 5. The logged request header (26-D2)

### 5.1 Changed snapshot, not per-dispatch

`payload::LlmRequestHeader` is appended **at a request-series start** and
**whenever any header field changes**:

- `call_config_equals(proposed, held) == false` (the only config change test), or
- the tool catalog changes (names or schemas), or
- the rendered prompt changes, or
- `purpose` changes.

No header is logged for an unchanged template, and none is needed: the messages
of each dispatch are the append-only message events. `starts_series = true` iff
config/prompt/tools changed (the KV-cache boundary); a `purpose`-only change logs
a new header with `starts_series = false` (26 §4.3.2 :324, :330-339).

### 5.2 Payload shape (pinned)

```cpp
namespace payload {
struct LlmRequestHeader {
    TurnId                     turn = 0;
    StepId                     step = 0;
    SessionId                  session_id;       // envelope half
    std::optional<CallPurpose> purpose;          // envelope half; absent == conversation
    LlmCallConfig              config;           // provider/model/effort/sampling
    std::string                system_prompt_digest; // SHA-256 hex, always
    std::optional<std::string> system_prompt;    // ONLY under session.persist_prompt_text
    std::vector<std::string>   tool_names;       // canonical order
    std::vector<std::string>   tool_schema_digests;  // per-tool SHA-256
    std::string                template_digest;  // SHA-256 of canonical_template()
    bool                       starts_series = true;
};
} // namespace payload
```

The `to_json`/`from_json` **wire keys** are owned by `29-event-family-errata.md`
(keys pinned in `26` §4.3.9.1 :945-946): `turn`, `step`, `session_id`, `purpose`
(`"compaction"｜"session_title"`, omitted when unset), `config` object,
`system_prompt_digest`, `system_prompt` (omitted unless the opt-in),
`tool_names`, `tool_schema_digests`, `template_digest`, `starts_series`. This
errata pins the C++ payload and the reconstruction semantics; it does not
duplicate the codec.

### 5.3 Loop rule

`buildRequest()` derives `LlmCallConfig` from the last `LlmRequestHeader`; a
proposed change is applied only if `!call_config_equals(proposed, held)`, in
which case a new header is logged with `starts_series = true`. The header is
found positionally at replay; `RequestId` is never persisted (26-D2 :140).

- **Provider is part of the config.** `buildRequest()` sets the proposed
  `LlmCallConfig::provider` from `AgentConfig::provider` (§3.5). Because
  `provider` is a `LlmCallConfig` field, a provider change is a config change
  (`call_config_equals` false) and forces a new header with
  `starts_series = true`, exactly like a model or parameter change. A change in
  the *effective default route* (adapter re-registration) is **not** a config
  change: the header still stores the empty sentinel and `template_digest`
  hashes it, so no series boundary is emitted. That is acceptable because routes
  are boot-stable (§3.3) and the reconstruction contract is over the template,
  not the adapter identity.
- **First dispatch of a fresh session.** On a fresh (or legacy) session
  `held_config_` is `nullopt` (`31` §5.3 :456-468; L20), so there is no held
  header to read. The proposed config — including the provider — is therefore
  built entirely from `AgentConfig` and logged as the first header; the loop
  never waits for a later dispatch to obtain the provider, and the empty
  provider sentinel is resolved by the runtime at `prepare_call` (§3.5), not
  back-filled into the held config. `held_config_` is only the comparison
  baseline for subsequent dispatches.

### 5.4 Prompt-text policy (D23, 26-I11)

- The header **always** carries `system_prompt_digest`.
- The full `system_prompt` text is present **only** when
  `session.persist_prompt_text` — a bool, default `false`, new `[session]`
  section, **global layer only** — is set. When absent, replay re-derives the
  system text (Wave 1: `AgentConfig::system_prompt` + the active plan section,
  §4.2; Wave 3+: the prompt registry), asserts
  `sha256(re-derived) == header.system_prompt_digest`, and fails loud on mismatch.
  When present, the text must be byte-identical to the in-memory assembled
  `Role::System` text and the digest check still runs (26 §4.3.2 :364-373).
- This key is **distinct from `logging.log_prompts`**
  (`include/ymh/config/config.hpp:83-88`), whose shipped contract is off by
  default and redacts prompt bodies even when on
  (`include/ymh/llm/redaction.hpp:4-6`). The session-DB key has its own name and
  semantics (store byte-for-byte; no redaction) and shares no code path with
  spdlog. The key itself is owned by `21`.
- **Wave-1 scope and digest basis (gate28 MEDIUM-2 reconciliation).** The
  header's `system_prompt_digest` is SHA-256 of the assembled `Role::System`
  message text (§4.2): the `AgentConfig::system_prompt` (config override or
  `default_system_prompt()`, `src/cli/wiring.cpp:48-52`; effective assignment
  `:179-180`) **plus the active plan-mode paragraph** when
  `plan_mode_.active(session)` is true (`src/agent/workspace_runtime.cpp:151`;
  the assembler's plan branch is `src/agent/context_assembler.cpp:46-53`,
  provider wired at `:149-152`). Wave 1 writes the
  digest of that assembled text; it is not a rendered-registry hash (26 §5 Wave 1
  :1414-1418). The shorthand "the Wave-1 header carries
  `AgentConfig::system_prompt`" is therefore read through this rule, so a
  plan-mode turn's digest matches replay's re-derived system text. No
  model-visible behaviour changes.

---

## 6. One provider attempt per stream; terminal result (26-I3, 26-I4)

### 6.1 The pinned end state

- **`LLMProvider::stream` is exactly one provider attempt.** The adapter must not
  loop over attempts (26-I3 :59-61).
- **Retry is a separate, durable executor** at the step boundary; it records
  `llm/retry` before each wait and `llm/retry_started` after the wait succeeds
  (part 1 §2.1.7 :418-436; event payloads pinned by `29`, keys in `26` §4.3.9.1
  :947-948). The executor has no config of its own: `register_adapter` captures
  `adapter->retry_policy()` at registration (§3.1), and `prepare_call` binds the
  captured policy into the `PreparedCall`, surfaced as
  `PreparedCall::retry_policy()`.
- **The pre-first-event barrier is retained** (L7; `stream.hpp:111`). Retry is
  allowed only before any `StreamEvent` — including `UsageEvent` — is dispatched
  (26-F4).
- **Terminal result at the boundary.** `LlmRuntime::stream` returns a terminal
  `LLMResponse` (`Completed`/`Cancelled`/`Failed`) for every call. Adapter throws
  and iterator-construction/iteration failures are normalized to one terminal
  failure; a return without `Finished`/`StreamError` is normalized to `Failed`
  with `MalformedResponse` (26-I4 :62-67, 26-F3 :1121-1122). On cancellation the
  adapter may emit **no** terminal sink event; the loop supplies the durable
  terminal (26-I4).
- The `LLMResponse`/`StreamOutcome`/`LLMErrorCode` types are unchanged
  (`include/ymh/llm/llm_provider.hpp:51-59`, `include/ymh/llm/stream.hpp:33-56`).

### 6.2 Migration constraint (no double retry)

The current adapter-internal retry loop (`src/llm/openai_adapter.cpp:923-1000`)
is **not removed in Wave 1**; removing it without the separate executor would be
a model-visible behaviour change, contradicting "Wave 1 … No model-visible
behaviour changes" (26 §5 :1421-1422). The pinned rule is:

> **Exactly one retry owner at any time.** The adapter's in-adapter retry is
> removed **only when** the separate retry executor lands, in the same change.
> Both active at once is the `L-F23` double-retry defect.

The retry executor's **wave is not assigned** in `26 §5` (it lists no retry
executor item in Waves 1–6); this errata records that gap in §13 rather than
inventing a wave.

### 6.3 The loop's existing re-attempt is not an adapter retry

The loop already performs one explicit `ContextLengthExceeded` compaction
re-attempt (`src/agent/agent_loop.cpp:771-809`; `06 §5.7 :817-824`). That is a
**new step/request** with a new `PreparedCall`, not an adapter retry, and is
preserved (08 decision (d) is retained). One `PreparedCall` dispatches exactly
once (§7).

---

## 7. `LLMErrorCode` additions: `InvalidPreparedCall` and `NoProviderRoute`

`LLMErrorCode` is a closed enum (`include/ymh/llm/stream.hpp:40-56`). This errata
adds **two** values, additively:

```cpp
enum class LLMErrorCode : std::uint8_t {
    /* … existing values unchanged … */
    InvalidPreparedCall,
    NoProviderRoute,        // Rev 3; §3.5, §7 (route lookup failure)
};
```

A `PreparedCall` misuse — a second dispatch or a config mismatch between the
`FrozenRequest` and the `PreparedCall` — throws `PreparedCallError`:

```cpp
struct PreparedCallError {
    enum class Reason : std::uint8_t { Consumed, ConfigMismatch };
    LLMErrorCode code = LLMErrorCode::InvalidPreparedCall;
    Reason       reason;
    std::string  detail;
};
```

`NoProviderRouteError` is the carrier for the §3.5 route-lookup failure — it is
not a `PreparedCall` misuse (no `PreparedCall` is ever produced):

```cpp
struct NoProviderRouteError {
    LLMErrorCode code = LLMErrorCode::NoProviderRoute;
    ProviderId   provider;   // the requested id; empty => no default route exists
    std::string  detail = "no route for config.provider";
};
```

Mapping for `PreparedCallError` (26 §4.7 :1247-1250): at the loop boundary it
maps to `AgentErrorCode::ProviderFailed`; at load/assembly it maps to
`ConfigError` (process exit 2). The `PreparedCall::stream` path checks
`call_config_equals(request.config(), config())` before dispatch and consumes
the one-shot flag; a second call or mismatch is a programming error and is
never silently retried.

`NoProviderRouteError` maps the same way at the loop
(`AgentErrorCode::ProviderFailed`, §3.5) and additionally to
`CompactionError::Code::NoProviderRoute` in the compactor (`32` C-F24). It is
raised by the route lookup **before** any `PreparedCall` exists, so it never
reaches the one-shot/mismatch path.

**Wire/diagnostic string and switch audit (gate28 LOW-1 pin; extended Rev 3).**
`to_string(LLMErrorCode::InvalidPreparedCall)` returns exactly
`"invalid_prepared_call"` and `to_string(LLMErrorCode::NoProviderRoute)` returns
exactly `"no_provider_route"` (snake_case, matching the existing values).
Because `to_string` is the only exhaustive, `default:`-less switch over
`LLMErrorCode` (`include/ymh/llm/stream.hpp:69-87`), adding either enumerator
requires adding its `case` in the same change — otherwise the build breaks under
`-Wswitch`/`-Werror`, which `AGENTS.md` forbids suppressing. No event codec
serializes `LLMErrorCode` today (no reference under `src/session/`), so the
strings are diagnostic-only; they are pinned now so a future codec has stable
values.

The `LLMError`/`LlmFailure` shapes are reused verbatim by the `llm/retry` and
`llm/request_header` payloads; no error code beyond `InvalidPreparedCall` and
`NoProviderRoute` is added (26 §4.7 :1243-1246).

---

## 8. `ContextCompactor` re-seam (T-M9, Wave 1)

`ContextCompactor`'s constructor is re-seamed from `LLMProvider&` to
`LlmRuntime&` so Wave 4 can route the summarizer (26 §5 Wave 1 :1412-1413):

```cpp
// include/ymh/agent/compactor.hpp:113-117 (current) → target:
ContextCompactor(LlmRuntime&           runtime,
                 LLMPool&              pool,
                 const TokenEstimator& estimator,
                 CompactionPolicy      policy,
                 WallClock             clock = std::chrono::system_clock::now);
```

- The member `LLMProvider& provider_` (`include/ymh/agent/compactor.hpp:140`)
  becomes `LlmRuntime& runtime_`; the call at `src/agent/compactor.cpp:252`
  becomes `prepare_call(config, cancel)` → `PreparedCall::stream(frozen, collect,
  cancel)` with `purpose = CallPurpose::Compaction`.
- The compactor still brackets its call with one `LLMPool` slot (08 decision (r),
  06 §5.9; `include/ymh/agent/llm_pool.hpp`), and its summarizer model resolution
  (`src/agent/compactor.cpp:103-113`) is unchanged.
- The daemon's construction (`src/agent/workspace_runtime.cpp:170-172`) passes
  the runtime instead of `*provider_`.
- **Summarizer provider (closes gate32 M2).** The compactor's
  `LlmCallConfig::provider` comes from the same source as the loop's (§3.5):
  `CompactionPolicy::provider` (`32` §2.1 :132-148) is populated from
  `AgentConfig::provider`, and an empty value resolves to the runtime default
  route at `prepare_call`. A missing route surfaces as
  `CompactionError::Code::NoProviderRoute` (`32` C-F24). `32` owns the 13-side
  field and the error mapping; this errata owns the source and the runtime
  failure.
- **13-owned text change.** `13-context-compaction.md §5.2 :604-610` and
  `:633` pin the old constructor; they require a 13 errata before Wave 1 code.
  This errata records the dependency; it does not amend 13.

---

## 9. Invariants (extending 08 §10)

**L18 — Single service boundary.** The loop holds `LlmRuntime&`; adapters are
reached only through it. No `LLMProvider*` appears in `AgentServices`, and no
caller registers routes except through `register_adapter`. (26-D1)

**L19 — Frozen before dispatch.** Every dispatched request is a `FrozenRequest`;
it is immutable by type and no `const_cast`/`mutable` path mutates it after
freeze. (26-D3, 26-F2)

**L20 — Reconstructable from the log.** A dispatched request's **template** is
reconstructable from the last `LlmRequestHeader` plus the live registries, and
`rebuild.template_digest() == header.template_digest`. A request with no header
is **legacy/unreconstructable**, never fabricated; a re-derived tool schema that
does not match its digest fails loud. (26-I2, 26-F6, 26-F13, 26-F15)

**L21 — Header is a changed snapshot.** The header is appended at a series start
and on any change to config, prompt, tool catalog, or purpose — never per
dispatch. `call_config_equals` is the only config change test; `RequestId` is
never persisted. (26-D2)

**L22 — Exactly one retry owner; one attempt per stream.** The pinned end state
is: `LLMProvider::stream` performs exactly one provider attempt and retry is a
separate durable executor. Until that executor lands, the adapter's
pre-first-event retry is the **single** owner; the adapter loop and the executor
must never be active together (§6.2). The pre-first-event barrier (L7) always
holds. (26-I3)

**L23 — Terminal at the service boundary.** `LlmRuntime::stream` returns a
terminal `LLMResponse` for every call; adapter throws and non-terminal returns
are normalized (`Failed`/`MalformedResponse`). The guarantee is on the service
`LLMResponse`, not on the adapter sink; the loop supplies the durable terminal.
(26-I4, 26-F3)

**L24 — `PreparedCall` is one-shot.** One `PreparedCall` dispatches exactly once;
a second dispatch or a config mismatch is `InvalidPreparedCall`. (26-D3, §7)

**L25 — Prompt text is opt-in durable.** The header always carries
`system_prompt_digest`; the full text is stored only under
`session.persist_prompt_text`, and replay verifies the digest in both cases.
(26-I11, D23)

**L26 — Provider id is config-sourced and route-resolved at the boundary.**
`LlmCallConfig::provider` is sourced from `AgentConfig::provider` (itself from
`config.llm.provider`); an empty value resolves to the runtime's registered
default route at `prepare_call`, and a request with no matching route fails loud
with `NoProviderRouteError{NoProviderRoute}` before any `PreparedCall` is
produced — never a silent fallback to another provider. (`31` A23, `32` C-F24;
§3.5)

L1–L17 remain in force; L7 is explicitly retained, L4 is re-scoped to the
adapter sink (the service-level terminal is L23), and L16 (no exception crosses
the **provider** seam) is unchanged — `PreparedCallError` is raised **above**
the provider seam and is normalized at the loop boundary (§7).

---

## 10. Failure modes (extending 08 §11.2)

| L-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **L-F19** | Config drift: a proposed `LlmCallConfig` differs from the logged header and is neither logged nor rejected | `call_config_equals` at `buildRequest` | Log a new header with `starts_series = true`; never dispatch a silent drift (26-F1) |
| **L-F20** | Frozen request mutated after dispatch | immutability by type + debug sentinel | Compile-time prevention; no `const_cast`/`mutable`; test asserts sentinel (26-F2) |
| **L-F21** | Non-terminal adapter return (no `Finished`/`StreamError`) | `LlmRuntime::stream` post-condition | Normalize to `Failed` with `MalformedResponse` (26-F3) |
| **L-F22** | Retry after the first dispatched event | `is_retryable_code` + `!any_event()` barrier (`stream.hpp:111`, `openai_adapter.cpp:983`) | Terminal; the executor records `llm/retry` before any wait (26-F4) |
| **L-F23** | Double retry (adapter loop **and** separate executor both active) | one-retry-owner rule (§6.2) | Exactly one owner; removal of adapter retry co-scheduled with the executor |
| **L-F24** | `PreparedCall` double dispatch or config mismatch | one-shot flag + `call_config_equals` | `PreparedCallError{InvalidPreparedCall}`; `ProviderFailed` at the loop, `ConfigError` at load (§7) |
| **L-F25** | Replay template digest mismatch | `rebuild.template_digest() != header.template_digest` | Fail loud; never guess or synthesize a header (26-F13) |
| **L-F26** | Tool-catalog drift mid-series | `tool_names`/`tool_schema_digests` in the header | New header + new series; re-derived schema must match its digest (26-F6) |
| **L-F27** | No route for `config.provider` | `prepare_call`/`stream` route lookup (§3.5) | Loud typed `NoProviderRouteError{NoProviderRoute}` with `detail = "no route for config.provider"`; no `PreparedCall` is produced; the loop normalizes to `ProviderFailed`, the compactor to `CompactionError::Code::NoProviderRoute`; never a silent fallback (`31` A-F23, `32` C-F24) |

---

## 11. dsh mapping

| dsh concept | ymh after this errata | Reference |
|---|---|---|
| `LlmRuntime` (`ctx.llm`), the provider-neutral service | `LlmRuntime` above `LLMProvider` | 26 §2.1.1, §4.3.1; L18 |
| `registerAdapter` / `AdapterRegistrationHandle` | `register_adapter` / `AdapterHandle` | 26 §4.3.1 :255-261, :278-279 |
| `llm/stream` waterfall | `add_stream_interceptor` / `StreamInterceptor` | 26 §4.3.1 :263-267 |
| `prepareCall` / `PreparedLlmCall` (deep-frozen config) | `prepare_call` / `PreparedCall` / `FrozenRequest` | 26 §4.3.1 :210-247; L19, L24 |
| `GenerateOptions.provider` / `LlmCallConfig.provider` | `AgentConfig::provider` (config-driven) → `LlmCallConfig::provider`, runtime default route when empty | 26 §2.1.1 :127, :183; §3.5; L26 |
| `resolveCallConfig` (materializes adapter defaults) | **not added**; the provider is resolved by `prepare_call`'s route lookup instead (§3.5) | 26 §2.1.1 :130 |
| `callConfigEquals` + logged changed snapshots | `call_config_equals` + `payload::LlmRequestHeader` | 26 §4.3.2; L21 |
| "One provider attempt per stream; retry separate" | `LLMProvider::stream` one attempt; durable executor | 26-I3; L22 |
| Terminal `finish`/`error`/`aborted` | terminal `LLMResponse` at the service boundary | 26-I4; L23 |
| `ReplayEnvelope` / assistant stream | Wave 2 (`BlockAssembler`, D8/D9) | 26 §4.3.4 |
| `llm/retry`, `llm/retry-started` events | separate retry executor (wave unassigned, §13) | 26 §4.3.9.1 :947-948 |

---

## 12. Test plan

Deterministic, offline (`FakeLLM`, spec 08 §7; `include/ymh/llm/fake_llm.hpp`):

**Unit.**
1. `call_config_equals`: field-wise, including `stop[]` order sensitivity and the
   `top_p`/`seed`/`tool_choice` extensions; no other equality is used.
2. `FrozenRequest`: `canonical_template()`/`canonical_json()` are byte-identical
   across repeated calls and across process runs; `template_digest()` is the
   SHA-256 of `canonical_template()`; no ids/timestamps in the output.
3. `PreparedCall`: one successful dispatch; a second call throws
   `Consumed`; a mismatched `FrozenRequest` throws `ConfigMismatch`; both carry
   `InvalidPreparedCall`.
4. `LlmRuntime::stream` normalization: a `FakeLLM` returning without a terminal
   event becomes `Failed`/`MalformedResponse`; a throwing adapter becomes a
   terminal failure, never an escaping exception.
5. Adapter lifetime: destroying an `AdapterHandle` during an in-flight dispatch
   does not free the adapter (TSan-clean); a later dispatch to the removed route
   is `ConfigError`/no-adapter.

**Integration (`FakeLLM` through `LlmRuntime`).**
6. Header logging: exactly one header at series start; **no** header for an
   unchanged template on the next step; a new header on a config change, a tool
   change, a prompt change, and a `purpose`-only change (with
   `starts_series == false`).
7. Reconstruction: rebuild the template from the logged header and assert
   `template_digest()` equality; rebuild `canonical_json()` twice and assert
   byte-identity; a session with no header is marked legacy, not fabricated.
8. Tool-catalog drift: change a tool schema, assert a new series and that a
   mismatched re-derived schema fails loud.
9. Prompt-text policy: default off persists only the digest; with the opt-in the
   stored text is byte-identical and the digest check still runs.

**Replay / concurrency.**
10. `06`-owned replay harness: an attempt's request is reconstructable end to end
    from the log + live registries (26 §5.2).
11. TSan: concurrent `register_adapter`/`AdapterHandle` destruction versus
    in-flight `stream`.

**Provider routing (Rev 3).**
12. `buildRequest` sets `LlmCallConfig::provider` from `AgentConfig::provider`;
    an empty value is the sentinel and `prepare_call` selects the first
    registered route; a named but unregistered provider, and an empty provider
    with no registered route, both fail with
    `NoProviderRouteError{NoProviderRoute}` and produce no `PreparedCall`; the
    loop normalizes to `ProviderFailed` and the compactor to
    `CompactionError::Code::NoProviderRoute` (`L26`, `L-F27`). A provider change
    between dispatches logs a new header with `starts_series = true`.

**Live (opt-in).** No new live test is required by this errata; the existing
opt-in live layer (`YMH_LIVE_LLM=1`) exercises the same `LlmRuntime` path once
Wave 1 lands.

---

## 13. Open items and ownership gaps (not resolved here)

1. **06 errata is required and unassigned.** 26-D1 is `Brk. (06, 08)` with
   owning spec **06**: `AgentServices::provider`/`providers`/`provider_config`
   (`include/ymh/agent/agent_loop.hpp:53,64,65`), the `AgentRegistry` constructor
   (`06 §4.1 :435-437`), the loop's provider call (`06 §5.1 :600-612`), and the
   fallback provider construction (`src/agent/agent_registry.cpp:37-44,111`)
   must be amended. `26 §5` Stage A lists only `00`/`01`/`08` as Wave-0
   up-front items, so no Wave-0 owner is assigned to `06`. **Wave 1 cannot start
   until a `06` errata exists.** Flagged to the lead.
2. **Retry executor wave unassigned.** `26 §5` Waves 1–6 contain no retry
   executor item, yet `26-I3` and the `llm/retry`/`llm/retry-started` events
   (§4.3.9.1) require it. This errata pins the one-attempt contract and the
   no-double-retry migration constraint (§6.2) but does not invent a wave.
3. **13 errata required.** `13-context-compaction.md §5.2 :604-610,633` pins the
   `LLMProvider&` constructor; it must be amended to `LlmRuntime&` before Wave 1
   code (`06 §5.3`'s frozen `Compactor` seam is unaffected).
4. **Wire codecs are 29-owned.** The `llm/request_header` keys are pinned in
   `26` §4.3.9.1 :945-946 and implemented by `29-event-family-errata.md`; this
   errata pins only the C++ payload and semantics.
5. **`session.persist_prompt_text` is 21-owned.** Pinned in `26` §4.9 :1326;
   this errata consumes it.
6. **No schema/version change.** `kSchemaVersion` stays `1`; `llm/request_header`
   is a new event row, not a structural change; the durable forward fence is the
   loud `CorruptionError` on an unknown type
   (`src/session/session_persistence.cpp:228-232`; 26 §4.6 :1176-1185). The wire
   envelope version stays `1` (26-D24), so the `05` receiver-skip rule applies.
7. **`08` in-place text remains the verified baseline.** This errata amends by
   reference; `DESIGN_STATUS.md` should record that `08` now has errata `28` and
   is re-gated together with it. (Tracker edit is the lead's.)
8. **21 config-surface amendment required (Rev 3).** Adding
   `AgentConfig::provider` touches spec 21's config allowlist and mapping
   surface: `to_agent_config` gains a new `Config`→`AgentConfig` mapping from
   `config.llm.provider`
   (`21` §5.4's `to_agent_config` row, `:532`; when applying, the row's stale
   `src/cli/wiring.cpp:167-180` should read `174-189`). The JSON key
   `llm.default.provider` is **already** in 21's allowlist (`21` §7.7 :1242-1253;
   the key-set table `:407`), so no new JSON key is introduced by this pin; the
   required amendment is the mapping-table record (and the §7.3 `agent` note).
   If a distinct `agent.provider` override key is ever wanted, that is a
   separate 21 decision, not this pin. Recorded as a required 21 amendment; this
   errata does not edit 21.

---

## 14. Revision log

| Rev | Change |
|---|---|
| 3.1 | **Citation-only hygiene pass (no design change).** Fixes the gate28 Rev-3 LOWs: the stale `31 §5.3 :423-431` cross-ref → `31 §5.3 :456-468` (§5.3); the imprecise `plan_mode_.active(session)` anchor — the gate is `src/agent/workspace_runtime.cpp:151`, while `src/agent/context_assembler.cpp:46-53` is the plan branch (§4.2, §5.4); the stale `src/cli/wiring.cpp:167-180` range in the §13.8 21-amendment advisory → `174-189`; and clarifying sentences for `PreparedCall::config()` storing the input config verbatim (§3.5) and the default-route sentinel being invisible to `call_config_equals` (§5.3). No decision, invariant, or failure mode changed. |
| 3 | Pins the `LlmCallConfig::provider` source — the cross-cutting decision gate31's spec-31 MEDIUM and gate32's spec-32 M2 converged on. `AgentConfig::provider` (new field, `agent.hpp:94-105`) is populated by `to_agent_config` from `config.llm.provider` (`config.hpp:101`; `wiring.cpp:63,174-190`); an empty value resolves to the runtime's registered default route (a 28-owned representation: the first route of the first adapter registered) at `prepare_call`; a no-route request is a loud typed `NoProviderRouteError{LLMErrorCode::NoProviderRoute}` with `detail = "no route for config.provider"`, never a silent fallback — §3.5, §5.3, §7, §8, §9 (L26), §10 (L-F27), §11, §12, §13.8, 28-D9. Specs 31/32 reference this pin. Records the required 21 config-surface amendment. |
| 2 | Closes the gate28 findings. **MEDIUM-1:** `PreparedCall::retry_policy()` is sourced from a new non-pure `LLMProvider::retry_policy()` accessor captured by `register_adapter` (signature unchanged; `LLMProviderConfig::retry` stays the single source of truth) — §3.1, §6.1. **MEDIUM-2:** `canonical_template()`'s `system_prompt` and `system_prompt_digest` basis is pinned to the frozen request's `Role::System` message text, and §5.4 is reconciled so the plan-mode divergence is removed — §4.2, §4.5, §5.4. **LOW-1:** `to_string(InvalidPreparedCall)` = `"invalid_prepared_call"` plus the exhaustive-switch audit — §7. **LOW-2:** the loop's dispatch path is pinned to `prepare_call`→`PreparedCall::stream` — §3.1. **LOW-3:** adapter ownership is stated once (the runtime owns the registered adapters) — §3.3, §3.4, 28-D2. **LOW-4:** `kTemplateSchemaVersion = 1` and its change rule are pinned — §4.2, 28-D4. |
| 0 | Initial Wave-0 A2 errata. Pins `LlmRuntime`/`PreparedCall`/`AdapterHandle`/`InterceptorHandle`/`ProviderInfo`, `LlmCallConfig`/`call_config_equals`/`CallPurpose`, `FrozenRequest` + canonical serialization/reconstruction, `payload::LlmRequestHeader` as a changed snapshot, one-attempt-per-stream with the separate-retry end state and the no-double-retry constraint, the `ContextCompactor` re-seam, `LLMErrorCode::InvalidPreparedCall`, `L18–L25`, `L-F19–L-F26`, and the `28-D1–28-D8` decisions. Records three ownership gaps (06 errata, retry-executor wave, 13 errata) without inventing a resolution. Claims number 28; supersedes the stale 27–30 reservation. |

---

## 15. Decisions (28-D1–28-D9)

- **28-D1** — `LlmRuntime` is the loop's only LLM handle; `AgentServices` carries
  `LlmRuntime*` and no `LLMProvider*`. (26-D1; L18)
- **28-D2** — `ProviderRegistry`/factory are retained as the boot construction
  path; adapters are registered into the runtime and owned by it via
  `shared_ptr` (the daemon keeps no owning reference, §3.3). (26 §4.8)
- **28-D3** — Requests are deep-frozen before dispatch; `FrozenRequest` is
  immutable by type. (26-D3; L19)
- **28-D4** — The canonical serialization is `schema_version` + `envelope` +
  `config` + `system_prompt` + `tools`; `canonical_json()` adds ordered
  `messages`; no ids/timestamps/iteration order. `schema_version` is
  `kTemplateSchemaVersion = 1` and `system_prompt` is the frozen request's
  `Role::System` message text. (26 §4.3.2; §4.2)
- **28-D5** — The header is a changed snapshot, appended on config/prompt/tools/
  purpose change, never per dispatch; `call_config_equals` is the only change
  test; `RequestId` is never persisted. (26-D2; L21)
- **28-D6** — `LLMProvider::stream` is one provider attempt; retry is a separate
  durable executor; exactly one retry owner at a time; the L7 barrier is
  retained. (26-I3; L22; §6.2)
- **28-D7** — `LlmRuntime::stream` returns a terminal `LLMResponse`; non-terminal
  returns are `Failed`/`MalformedResponse`; the loop supplies the durable
  terminal. (26-I4; L23)
- **28-D8** — `ContextCompactor` takes `LlmRuntime&`; its summarizer call is a
  `CallPurpose::Compaction` dispatch under one `LLMPool` slot. (T-M9)
- **28-D9** — `LlmCallConfig::provider` is sourced from `AgentConfig::provider`
  (populated from `config.llm.provider`); an empty value resolves to the
  runtime's registered default route at `prepare_call`; a request with no
  matching route fails loud with `NoProviderRouteError{NoProviderRoute}`,
  normalized to `ProviderFailed` at the loop and
  `CompactionError::Code::NoProviderRoute` in the compactor. Specs 31/32
  reference this pin. (`31` A23/A-F23, `32` C-F24; L26; §3.5)

---

## 16. References

- `docs/design/26-dsh-alignment-part2.md` (Rev 7, GATE PASS): §4.1 (26-I2/I3/I4/
  I11), §4.2 :139-141, §4.3.1 :172-291, §4.3.2 :293-390, §4.3.4 :518-609,
  §4.3.9.1 :943-948, §4.4 :1087-1093, §4.5 :1113-1155, §4.6 :1157-1239,
  §4.7 :1241-1255, §4.8 :1257-1290, §4.9 :1326-1333, §5 Wave 0/1 :1358-1422.
- `docs/design/26-dsh-alignment.md`: §2.1.1–§2.1.3 (the dsh service, chunk
  protocol, terminal result), §2.1.7 (retry as a separate package), §3 G1–G6.
- `docs/design/08-llm-provider.md` (verified): §3.1, §3.4, §3.7, §5.1, §10,
  §11.2, §14.1(d)(n)(r).
- Tree anchors: `include/ymh/agent/agent_loop.hpp:53,64,65`;
  `src/agent/agent_loop.cpp:410-419,771-809`; `include/ymh/agent/compactor.hpp:
  113-117,140`; `src/agent/compactor.cpp:117-126,252`;
  `src/agent/workspace_runtime.cpp:149-152,165-172,198-200`;
  `src/agent/agent_registry.cpp:37-44,111`;
  `include/ymh/llm/llm_provider.hpp:41-47,51-59,61-79`;
  `include/ymh/llm/llm_request.hpp:28-30,40-48,54-61`;
  `include/ymh/llm/stream.hpp:33-56,69-87,111-123,160-167`;
  `include/ymh/llm/provider_registry.hpp:25-37,43-57,60-63`;
  `include/ymh/llm/openai_adapter.hpp:87`;
  `src/llm/openai_adapter.cpp:736-752,923-1000`;
  `src/agent/context_assembler.cpp:44-63`;
  `include/ymh/agent/llm_pool.hpp`; `include/ymh/llm/fake_llm.hpp`;
  `include/ymh/llm/redaction.hpp:4-6`; `include/ymh/config/config.hpp:83-88`;
  `src/cli/wiring.cpp:48-52,70-74,179-180`; `include/ymh/session/events.hpp:117`;
  `src/session/session_persistence.cpp:228-232`.
- Rev 3 tree anchors: `include/ymh/agent/agent.hpp:94-105` (no `provider` field
  today), `include/ymh/config/config.hpp:101` (`LlmSettings::provider`),
  `src/cli/wiring.cpp:63,174-190` (`to_provider_config`/`to_agent_config`);
  `include/ymh/agent/agent_loop.hpp:158` (the target `held_config_` site —
  `compactions_this_turn_` today, per `31` §5.3).
- Referencing errata: `docs/design/31-agent-loop-errata.md` (Rev 2) §5.3
  :438-447 (A23, A-F23, 31-D7) and `docs/design/32-compaction-errata.md` (Rev 1)
  §2.1 :132-148 and C-F24 — both consume this errata's provider-source pin and
  define no competing source.
- Conventions: `21-config-jsonc-errata.md` §15 (revision log),
  `23-session-lifecycle-errata.md` (amends-by-reference).

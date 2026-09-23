# 54 — Multi-Endpoint Routing (errata)

Status: **verified (Rev 6)** — the five-reviewer adversarial gate PASSED (0 open
HIGH / 0 MEDIUM). Implementation may begin (AGENTS.md "The rule").

Revision: Rev 6 — four residuals from the final fix pass applied (2 MEDIUM /
2 LOW). **(MEDIUM)** The compactor paired the summarizer's `config.profile_id`
with the **session** model's profile, but `config.model` comes from
`resolve_summarizer_model` (`compactor.cpp:131-141`, `:259`), which may be
`policy_.summarizer_model` — a different model. The normal path pairs the profile
with the requested model (`selected.model == config.model`), so the summarizer
ran with the session model's adapter-side shaping. 54-D7 now pairs the profile
with the requested summarizer model: the session's durable route when it is the
session's model, a named override's own endpoint **and** profile when it differs,
and the session's endpoint with an inert `""` for a literal override; §4 and
54-OQ-7 updated to match. **(MEDIUM)** The third `ModelSelection` aggregate-init
site (`src/agent/agent_loop.cpp:539`) has no endpoint source: `AgentConfig` has
no endpoint field and `to_agent_config` sets none, so it could only supply `""`
and silently route a no-controller session to the anonymous default. 54-D2 and
54-A9 now pin the additive `AgentConfig::endpoint` (from
`resolved.endpoint.name`) and note the production path always sets
`services_.model_selection` (`workspace_runtime.cpp:268`), so this is a
harness/test path. **(LOW)** 54-F8/54-U12's "never the inert `(endpoint, "")`
pair" was false when the requested model has no profile; reworded to "never the
omitted-profile pair when the requested model has a profile". **(LOW)** The
within-endpoint profile-rebind clauses (54-I9, 54-D6, 54-U18) are qualified to
**named / Endpoint-kind** routes: the anonymous default endpoint keeps its
profile-blind Provider-kind startup route (vacuous today, false once a second
default-endpoint profile exists). 54-U1's "inert-profile treatment" wording is
verified unchanged.
Rev 5 — four residuals from two independent adversarial re-checks
applied (1 MEDIUM / 3 LOW). **(MEDIUM)** 54-U1's clause claiming the lazily
built provider has "the same provider config as the pre-54 default path" was
false and contradicted 54-D5: the provider *configs* differ in `model` (54-D5
does not copy a model id) and in the named endpoint's connection fields
(54-D3), so the clause now claims only the same **inert-profile treatment**.
**(LOW)** The byte-identity claim is qualified to configs with no named
endpoints **and an empty default profile** (`llm.profile` unset): `llm.profile`
is independent of named endpoints (`src/config/config.cpp:2329`), so a set
default profile yields a non-empty `profile_id`, writes the new key, and takes
the usual schema-version treatment — made consistent at 54-D2, 54-I6, and
54-A14. **(LOW)** The §4 compactor helpers are renamed
`durable_endpoint_name`/`durable_profile_id` (with an explicit "must not call
`ModelSelectionController::effective()`" note) so they cannot be miswired to
the pending-wins accessor (54-D7). **(LOW)** 54-I2's "a pair no request selects
is never constructed" is scoped to **named-endpoint** pairs, since the
anonymous default endpoint's provider is built eagerly at startup.
Rev 4 — two residual findings from the independent adversarial
re-check applied (1 MEDIUM / 1 LOW), both exposed by the Rev 3 profile-aware
route key. **(MEDIUM)** The empty/unknown `profile_id` lookup is now pinned: the
resolver binds a function-local static inert `ModelProfile{}` when
`find_model_profile` returns `nullptr` (`total` means does-not-throw, not
non-null), so the supported `profile_id == ""` case of 54-I6/54-U1 has a defined
non-null binding (54-D1, 54-D5, §4, 54-I6, 54-U1). **(LOW)** The compactor now
sets `config.profile_id` from the same durable-selection source as
`config.endpoint`, so the summarizer builds/reuses the session's `(endpoint,
profile_id)` provider instead of the inert `(endpoint, "")` one — previously
masked by the endpoint-only route key (54-D7, §4, 54-A8, 54-F8, 54-U12).
Rev 3 — re-check finding set applied (1 HIGH / 3 MEDIUM / 5 LOW). The
headline defect is closed: the Endpoint-kind **route key is now profile-aware**
(`RouteKey{Endpoint, endpoint_name, profile_id}`), so the per-`(endpoint,
profile_id)` rebind this spec claims is actually reachable — previously the
endpoint-NAME-only route let a second profile hit the first profile's route
without ever consulting the cache. The construction-race clauses are reworded to
the serialized-under-`endpoint_cache_mutex_` mechanism (no losing provider),
`set_route_resolver`'s one-shot invariant gains a test (54-U22), `model_id` is
dropped as an inert resolver parameter, and the typed key is threaded through
`Registration`/the route store. Rev 2 — five-reviewer finding set applied
(8 HIGH / 11 MEDIUM / 15 LOW): the failure-path silent wrong-endpoint fallback
is closed, the `RouteResolver` carries model identity, the lazy-route
const/ownership/retry/cache-key problems are pinned, and the
durable-endpoint-identity gap at `create`/`fork` is closed. Rev 1 — initial
draft, authored from the reported `/model` defect (`model: EndpointNotRouted` on
a cross-endpoint pick) after the spec-53 landing.

Component: 54 (errata) — amends `53-eager-daemon-and-model-switching-errata.md`,
`52-endpoints-models-and-dsh-agent-presets.md`,
`28-llm-service-boundary-errata.md`, `08-llm-provider.md`,
`01-session.md` and `32-compaction-errata.md` by reference. This is the
follow-on that 53-OQ-3 explicitly reserved ("Multi-endpoint routing … a
separate errata?").

Depends on: `53` (verified Rev 2), `52` Part A (verified Rev 2), `28` (verified
Rev 3), `01` (verified), `06` (verified), `08` (verified), `13`/`32` (verified).

Scope: make the daemon route a request to the endpoint named by the session's
**effective model selection**, instead of to the single endpoint bound at daemon
startup. It pins a lazy per-endpoint provider cache, a per-request route
selector, the replacement for 53-I9's endpoint guard, the per-endpoint provider
lifecycle, the compaction/subagent paths, and the durable endpoint identity on
resume. It does **not** implement anything; it is a design spec.

---

## 1. Purpose, scope, and the user decisions

### 1.1 The reported defect (user report, edited into a bullet list — not a verbatim quote)

> After spec 53 landed, the user tested the `/model` picker:
> - Picking a model on the SAME endpoint as the startup model: works.
> - Picking a model on a DIFFERENT endpoint: shows
>   `model: EndpointNotRouted`.

### 1.2 What this changes, in one sentence

The daemon learns to hold **one route per routable `(endpoint, profile)` pair**
and to select the route **per request** from the session's current model, so
`/model` can switch a session onto a model served by a different `llm.endpoints`
entry — while keeping the endpoint-identity comparison that turned a silent
wrong-endpoint request into a loud rejection (53-I9).

### 1.3 Supersession map

| ID | Spec / decision | What 54 does | Why |
|---|---|---|---|
| 54-A1 | 53-I9, 53-F6 (`53:1035`, `:1055`) | **Supersedes.** The guard no longer requires the target endpoint to equal the daemon's startup endpoint. It rejects only a target whose endpoint is not in the daemon's routable set (54-D4). | The defect. |
| 54-A2 | 53 §10.1 recorded risk "Cross-endpoint switching is not supported in Rev 1" (`53:1235-1238`), 53-OQ-3 (`53:1272-1275`) | **Resolves.** Cross-endpoint switching is supported; OQ-53-3 is closed. | R1. |
| 54-A3 | 53 §10.1 recorded risk "Provider-side profile shaping does not follow a mid-session switch" (`53:1239-1247`), 53-OQ-10 (`53:1298-1306`) | **Narrows.** Adapter-side profile is now bound **per `(endpoint, profile_id)`** at that provider's construction, not globally at startup. The Endpoint-kind route key carries the same pair, so dispatch cannot reuse another profile's provider. A switch that changes the endpoint or the profile id re-binds it; one that keeps both does not. The residual per-request adapter profile is re-recorded as 54-OQ-1. | 54-D6. |
| 54-A4 | 53 §8 dsh mapping "an unroutable session blocks the composer" (`53:1098`) | **Amends.** ymh no longer has "unroutable sessions" for a valid config (54-I4); the composer-block parity item is re-recorded as 54-OQ-5. | R1. |
| 54-A5 | 28 §3.5 `LlmCallConfig::provider` source (`28:389-440`), 28 §3.1 interface (`28:175-215`), 28 §5.3 `call_config_equals`/`starts_series` header boundary (`28:575-599`), 31 §5.3 `held_config_` (`31:424-468`) | **Amends (additive).** `LlmCallConfig` gains `endpoint` and `profile_id`; route resolution is **endpoint-first**, with `provider` retained as the default/legacy fallback **only when `endpoint` is empty**. The `provider` field keeps its 28 §3.5 source and its provenance use. Because §4 amends `call_config_equals`, a change of either new field is a config change and starts a new header series (28 §5.3); `held_config_`'s comparison baseline (31 §5.3) gains the same fields. | 54-D2/D3. |
| 54-A6 | 08 §5.1–§5.3 "`LLMProviderConfig` is constructed from a `ResolvedModel`" (`52:107`), single-`provider_` daemon (`28:127-130`) | **Amends.** `LLMProviderConfig` is constructed per `(endpoint, profile_id)`; multiple providers coexist in one daemon. | 54-D5. |
| 54-A7 | 01-session.md `SessionHeader` (`session.hpp:44-66`), 53-D6/53-A12 durable `SessionModelChanged` fold (`53:591-648`, `:1081`) | **Amends (additive).** `SessionHeader` gains `model_name`; the fold materializes it beside `model`, and durable selection resolution prefers the name. | 54-D8. |
| 54-A8 | 32-compaction-errata / 13 compaction summarizer route (`compactor.cpp:268-269`, `wiring.cpp:284`) | **Amends.** The summarizer request routes through the **requested summarizer model's** route (`config.endpoint` + `config.profile_id`): the session's durable route when the summarizer model is the session's, a named override's own endpoint **and** profile when it differs, and the session's endpoint with an inert profile for a literal override (54-D7). Not the startup endpoint. | 54-D7. |
| 54-A9 | 53-D7/H4 `ModelSelection` field list (`53:655-666`), 53-D5 step 3 (`53:571-573`) | **Amends (additive).** `ModelSelection` gains `endpoint`; `set_session_model` builds it from the entry's `ResolvedEndpoint`. Inserting the field at position 3 breaks the three aggregate-init sites `src/host/host_runtime.cpp:802`, `src/agent/workspace_runtime.cpp:197` (the `ResolveFn` site 54-D8 depends on), and `src/agent/agent_loop.cpp:539`; all three must be updated in the same change. The third additionally needs `AgentConfig::endpoint` (from `resolved.endpoint.name` in `to_agent_config`), because `AgentConfig` has no endpoint field today. | 54-D2/D3. |
| 54-A10 | 52 §1.4 scope boundary "any UI surface … out of scope (spec 10/25 own `/model`)" (`52:121-133`) | **Retained (not amended).** The `/model` picker remains 53-owned; 54 changes only the daemon's routing behind it. | R1. |

The register also records 54-A11–54-A14 (§7): the inverted 53-U13 test, and
52-I2/52-I4/52-I5 (relied on / extended / preserved). They are not
supersessions and so do not appear in this map.

### 1.4 What this spec does **not** change

- The `session.set_model` RPC shape, profile (Interactive-only), reply
  (`SetModelResult`), and the QUEUED-at-step-boundary semantics (53-D5/53-I7).
- `session.set_model`'s per-session scope (53-I8): the daemon still never writes
  config and never touches `llm.active_model`.
- The picker keymap (53-I10) and the status-line model segment (53-I11).
- The event name (`session/model`) and the `SessionModelChanged` payload shape;
  54 only adds a folded header field.
- Path safety, the registry, daemon ownership (spec 16), and transport framing.
- A keyring, remote/SSH transport, and config hot reload (all out of scope).

---

## 2. Current state (verified against the shipped tree)

Every `file:line` below was re-derived from the tree at Rev 1; the symptom is
reproduced by reading the four call sites in §2.1–§2.3.

### 2.1 The daemon registers exactly one route

`WorkspaceRuntime::Impl` owns one `LlmRuntime runtime_`, one
`std::optional<AdapterHandle> adapter_handle_`, and one
`LLMProviderConfig provider_config_` (`src/agent/workspace_runtime.cpp:330`,
`:332`). At construction it registers **one** route, keyed by the startup
provider id:

```cpp
// src/agent/workspace_runtime.cpp:283-291
if (provider != nullptr) {
    std::shared_ptr<LLMProvider> adapter = std::move(provider);
    std::vector<ProviderId>      routes;
    if (!provider_config_.provider.empty()) {
        routes.push_back(provider_config_.provider);
    } else {
        routes.push_back(adapter->id());
    }
    adapter_handle_ = runtime_.register_adapter(std::move(routes), std::move(adapter));
}
```

`register_adapter` appends one `Registration` whose `routes` is that vector
(`src/llm/llm_runtime.cpp:241-263`). `resolve_adapter` matches the requested
`ProviderId` against the registered routes and throws `NoProviderRouteError`
otherwise (`src/llm/llm_runtime.cpp:333-359`). (§2.1 describes the **shipped**
tree; 54 changes `Registration::routes` from `std::vector<ProviderId>` to
`std::vector<RouteKey>` — §4, 54-D1.)

### 2.2 The daemon builds one provider config, from the STARTUP endpoint

`make_workspace_runtime` builds a single `LLMProviderConfig` from
`to_provider_config(options.config)` (`src/agent/workspace_runtime.cpp:396`).
`to_provider_config` calls `resolve_model(config)` and copies **that one**
`ResolvedEndpoint` (`src/cli/wiring.cpp:86-105`). `resolve_model` is the startup
selector: `agent.model` → `llm.active_model` → `llm.model` → builtin
(`src/config/config.cpp:2364-2385`), with a trailing `llm.active_endpoint`
override that replaces the endpoint only when no named model was selected
(`:2380-2383`). So even though `config.llm.endpoints`
describes every endpoint, only the startup endpoint reaches the runtime.

### 2.3 The endpoint guard rejects every other endpoint

The `session.set_model` handler compares the target entry's endpoint identity
against `model_catalog().default_entry().endpoint` and throws
`EndpointNotRouted` on any difference:

```cpp
// src/host/host_runtime.cpp:791-798
// 53-I9: compare the full ResolvedEndpoint identity (name/base_url), not
// the ProviderId, which two distinct endpoints commonly share.
const ResolvedEndpoint& registered = runtime_.model_catalog().default_entry().endpoint;
if (entry->endpoint.name != registered.name ||
    entry->endpoint.base_url != registered.base_url) {
    throw_mapped(WireError{protocol::code_value(protocol::AppCode::EndpointNotRouted),
                           "EndpointNotRouted"});
}
```

`EndpointNotRouted = -32021` (`include/ymh/transport/protocol.hpp:195`). The
comparison is **correct and must not be loosened**: an earlier *design* revision
(not observable in the tree — the shipped guard was committed already comparing
endpoint identity, so the pre-guard ProviderId behaviour is historical and
cannot be re-derived from git history) compared the `ProviderId`, which two
distinct endpoints commonly share (`openai-compatible`), so a cross-endpoint
switch passed the check and the request was sent to the wrong
`base_url`/`api_key`. The guard turned a silent wrong-endpoint bug into a loud
rejection; the fix is to make routing support N endpoints, not to weaken the
guard.

### 2.4 The config already describes every endpoint

`EndpointSettings` (`include/ymh/config/config.hpp:182-195`) carries
`provider`, `base_url`, `api_key`, `api_key_env`, `headers`, `max_concurrency`,
timeouts and `retry`; `LlmSettings::endpoints` is a named map
(`config.hpp:246`). `ResolvedEndpoint` (`config.hpp:474-486`) is the resolved
form, and `ModelCatalogEntry::endpoint` is a `ResolvedEndpoint`
(`include/ymh/agent/model_selection.hpp:84-90`). `ModelCatalog::build` resolves
every `llm.models` entry's endpoint via `endpoint_for`
(`src/agent/model_selection.cpp:92-111`, `:291-298`). So the daemon already
holds the full endpoint description; only the runtime binding is single.

### 2.5 The dispatch path selects a route by ProviderId

`AgentLoop::effective_model_selection` returns the session's effective
`ModelSelection` (`src/agent/agent_loop.cpp:538-557`). `buildRequest` maps it
into an `LlmCallConfig`, setting `config.provider = selected.provider`
(`:577-579`) — a `ProviderId`, **not** an endpoint identity. `runStep` calls
`services_.runtime->prepare_call(request->config(), turnToken)` per request
(`:1073`), and `prepare_call` resolves `config.provider`
(`src/llm/llm_runtime.cpp:294-298`). `ModelSelection` itself carries no endpoint
name (`include/ymh/agent/model_selection.hpp:32-38`), so even a correct
per-request route could not be selected today.

### 2.6 Other request paths also bind the startup endpoint

- **Compaction.** `ContextCompactor` sets `config.provider = policy_.provider`
  (`src/agent/compactor.cpp:268-269`), and `to_compaction_policy` sets
  `policy.provider = resolved.endpoint.provider` from the **startup**
  `resolve_model` (`src/cli/wiring.cpp:284`). A session switched to another
  endpoint would still summarize through the startup endpoint.
- **Subagent.** `SubagentRunner` creates a distinct child session with
  `options_` inherited from the parent (`src/agent/subagent.cpp:12-27`); its
  model is resolved at `session.create` (53-D2). Subagent inheritance is
  unpinned (53-OQ-9).

### 2.7 Provider construction is cheap; provider *validation* is the hazard

`OpenAICompatibleProvider`'s constructor only stores config/capabilities/
transport (`src/llm/openai_adapter.cpp:999-1004`); `CurlHttpTransport` is
stateless (`include/ymh/llm/openai_adapter.hpp:73-81`) and the curl handle is
created per request (`src/llm/openai_adapter.cpp:927`). The daemon's one
`LLMPool` is sized from `ResourceCaps::max_llm_concurrency`
(`src/agent/workspace_runtime.cpp:170`); per-endpoint `max_concurrency` is
parsed into `ResolvedEndpoint` but **never consumed** (verified: the only
`max_concurrency` reads are the config parser and the resolved struct). So
construction is cheap and there is no per-endpoint connection pool to own.

But `ProviderRegistry::create` validates the config
(`src/llm/provider_registry.cpp:56-75`): an unknown provider id, a non-`http(s)`
`base_url`, and — when there is no literal key — an empty/invalid `api_key_env`
are all `ConfigError`. A named endpoint's `api_key_env` defaults to `""`
(`config.hpp:185`), so a hand-written keyless endpoint
`{"provider":"openai-compatible","base_url":"…"}` **fails construction** even
though 52-F23 pins a keyless endpoint as legitimate and "the config is not
rejected". Today this is masked because only the startup endpoint is ever
constructed.

### 2.8 Verdict

The failure is not in the guard; it is the absence of a multi-endpoint runtime.
The daemon needs (i) a route per routable `(endpoint, profile)` pair, (ii) a
per-request route selector carried in the call config, and (iii) a construction
point that does
not turn an unused or keyless endpoint into a daemon-start failure. The
reported `model: EndpointNotRouted` is the guard correctly refusing a switch the
runtime cannot honor.

---

## 3. Decisions (54-D)

### 54-D1 — The routing model: lazy, cached, per-endpoint construction (option b)

Three options were evaluated.

**(a) Eager: construct a provider for every `llm.endpoints` entry at startup.**
Rejected. Construction is cheap (§2.7), but `ProviderRegistry::create` rejects a
keyless/bad-`base_url`/unknown-provider endpoint (§2.7), so one broken *unused*
endpoint would fail the entire daemon — a regression, and contrary to 52-F23
("the config is not rejected"). It also constructs endpoints that no
`llm.models` entry references.

**(b) Lazy + cached: construct an endpoint's provider on the first request that
selects it, cache it for the daemon's lifetime.** **Chosen.** It constructs only
the endpoints actually selected, isolates a broken endpoint's failure to the
requests that use it, and keeps the startup path byte-for-byte unchanged.

**(c) Construct per `(endpoint, model)` or per request.** Rejected. The
provider's `model` field is not used for dispatch — the request carries the wire
model id (`LLMRequest.model`, `include/ymh/llm/llm_request.hpp:56-65`) — so a
provider per model buys nothing. Per-request construction would rebuild the
provider (and its capability gates) on every call for no benefit.

**Pinned mechanism.** `LlmRuntime` gains an optional injected `RouteResolver` — a
callback that constructs (or returns a cached) provider for a named endpoint,
**carrying the profile identity the provider must be built with**:

```cpp
using RouteResolver = std::function<std::shared_ptr<LLMProvider>(
    std::string_view endpoint_name, std::string_view profile_id)>;
```

`profile_id` is required: the provider config is per `(endpoint, model profile)`
(54-D5), `LlmCallConfig` carries no profile today, so without it the resolver
could not reach the selected model's profile and named endpoint models would
silently lose their profile capabilities (`forbidden_stop_tokens`
`src/llm/openai_adapter.cpp:1055-1057`, `normalize_tool_arguments` `:568-569`,
capability gates `src/llm/provider_registry.cpp:131`). `profile_id` (not the
expanded `ModelProfile`) is carried because it is the canonical, serializable
identity (`ModelProfile::id`, `""` = inert; `find_model_profile` is
total/noexcept, 47-I2) and keeps 52-I5 byte-identity intact; the resolver maps it
to the `const ModelProfile&` that 54-D5's overload takes. **The lookup may return
`nullptr`**: `find_model_profile` is total in the *does-not-throw* sense, **not**
in the non-null sense, and returns `nullptr` for `""` — the explicitly supported
no-profile case (54-I6/54-U1) — and for any unknown id
(`src/llm/model_profile.cpp:40-47`). The resolver therefore substitutes a
**function-local static inert `ModelProfile{}`** (`id == ""`, byte-identical to
`profile_for("")`, `src/agent/model_selection.cpp:37-42`) whenever the lookup is
null, so the pinned `const ModelProfile&` overload always binds and no null deref
is possible; an empty/unknown `profile_id` means "no profile" and yields the
inert profile. (A `const ModelProfile&`
resolver parameter is equivalent but would force a non-serializable struct into
the call config.) `model_id` is **not** carried: the provider's `model` field is
dispatch-inert (option (c) above; the per-request `LLMRequest.model` is
authoritative), so making it part of the resolver identity would only create a
spurious `(endpoint, model, profile)` key whose collision the cache cannot
express. `LlmCallConfig` therefore gains **both** `endpoint` and `profile_id`
(54-D2).

`WorkspaceRuntime::Impl` installs a resolver closing over its
`ProviderRegistry`, the startup `Config`'s endpoint map, the `ModelCatalog`, and
its own lazy-route state:

- `std::map<std::pair<std::string, std::string>, std::shared_ptr<LLMProvider>>
  endpoint_cache_;` keyed by `(endpoint_name, profile_id)` — a daemon-global,
  cross-session cache (Rev 2 H6); and
- `std::map<std::pair<std::string, std::string>, AdapterHandle>
  endpoint_handles_;` — the **single owner** of every lazily registered route's
  RAII handle, keyed by the **same `(endpoint_name, profile_id)` pair** as the
  cache (Rev 3). Keying the handles map by endpoint name alone would make the
  second profile's insertion move-assign into the first profile's
  `AdapterHandle`, whose `operator=` calls `remove_generation`
  (`src/llm/llm_runtime.cpp:194-206`) and thereby evicts the first profile's
  route; the pair key prevents that. Populate it with `try_emplace` (a fresh
  pair) / `insert_or_assign` (a replacement) — `operator[]` is unavailable
  (`AdapterHandle` is move-only and non-default-constructible,
  `include/ymh/llm/llm_runtime.hpp:133-154`).

The resolver is the only writer for lazy routes. On a call it takes
`endpoint_cache_mutex_` and checks the cache; on a miss it builds an
`LLMProviderConfig` via the 54-D5 overload, calls `providers_.create(...)`, then
registers the route with `LlmRuntime::register_endpoint_route(name, profile_id,
provider)` (an `Endpoint`-kind `RouteKey{Endpoint, name, profile_id}`) and
**retains the returned `AdapterHandle` in `endpoint_handles_`** before returning
the provider. The check-build-insert runs entirely under
`endpoint_cache_mutex_`, so two threads racing the same `(endpoint, profile_id)`
pair cannot both construct a provider: the first builds and caches it, the
second observes the cache hit and reuses it — exactly one provider is
constructed and retained per pair (54-I2/I11). Retaining the handle is mandatory: `AdapterHandle::~AdapterHandle`
calls `remove_generation` (`src/llm/llm_runtime.cpp:208-210`), so a route whose
handle is dropped is unregistered immediately (Rev 2 H4). `WorkspaceRuntime::Impl`
keeps its existing `std::optional<AdapterHandle> adapter_handle_` for the startup
route; the per-`(endpoint, profile)` map covers the lazy routes and the two do
not alias.

**Constness (Rev 2 H3).** `resolve_adapter` stays `const` and performs **no**
mutation: it only reads `registrations_` (under `mutex_`) and, on a miss, calls
the resolver **outside** `mutex_`, then re-locks and re-reads the now-registered
route. The resolver runs in `WorkspaceRuntime::Impl`'s non-const context, so its
`register_endpoint_route` call is legal. Registration is thus **delegated**, not
performed by the const method, which is how the `const` signature is reconciled.
(The rejected alternative — inserting from inside `resolve_adapter` — would
require making the route store `mutable` and adding a private non-RAII
`insert_route` helper, because the const method cannot `push_back`/call the
non-const `register_adapter` (`include/ymh/llm/llm_runtime.hpp:196`; the const
`resolve_adapter` is at `:225` and the route store `registrations_` at `:232`);
54 does not take it.)

**Retry sourcing (Rev 2 LOW).** A lazy route's `Registration.retry` is captured by
`register_endpoint_route` (and by `register_adapter` for the startup route)
calling `adapter->retry_policy()` **before** taking `mutex_`
(`src/llm/llm_runtime.cpp:246-248`), exactly as the startup route is. The
resolver returns only the provider; `resolve_adapter` reads the retry from the
re-registered `Registration` on its post-resolver lookup, so no path leaves
`PreparedCall.retry_` unsourced. An implementation must not call
`retry_policy()` while holding the registry lock.

**Route keys (Rev 2 LOW / Rev 3 HIGH).** `"@" + name` is one-sided: a provider id
literally named `@foo` collides with endpoint `foo`'s route. 54 therefore pins a
typed route key `RouteKey{kind, name, profile_id}` with
`kind ∈ {Provider, Endpoint}`, not string concatenation. The **anonymous
default** keeps its `Provider`-kind key byte-for-byte (`{Provider, provider_id,
""}`, 54-I6); each named endpoint uses an `Endpoint`-kind key
`{Endpoint, endpoint_name, profile_id}`. The `profile_id` component is
**required**, not decorative: an Endpoint-kind key of endpoint name alone would
make a second profile's request *hit* the first profile's route in 54-D3 step 1
and return its provider without ever consulting the cache — the exact
per-`(endpoint, profile_id)` rebind 54-D6/54-I9 promise. With the pair in the
key, `{Endpoint, E, P1}` and `{Endpoint, E, P2}` are distinct routes.

`LlmRuntime` stays provider-neutral: it only calls the injected factory. This
mirrors the existing "call the virtual before/after the lock" discipline
(`register_adapter` calls `retry_policy()` before locking, `:246-248`;
`list_providers` calls the virtuals after unlocking, `:265-284`).

### 54-D2 — The route identity is the endpoint name plus the profile id, carried per request

`ResolvedEndpoint::name` is the **only** unique endpoint identity: it is the
`llm.endpoints` map key, and `""` denotes the anonymous default endpoint
(`config.hpp:474-475`, `52-D3`). `ProviderId` is deliberately **not** unique
(two endpoints commonly share `openai-compatible`, which is exactly why 53-I9
compares `name`/`base_url`). The route key is the pair **(endpoint name,
profile id)** (54-D1): the endpoint name selects the connection/credentials, the
profile id selects the adapter-side shaping, and both are needed to identify a
provider.

Pinned:

- `LlmCallConfig` (`include/ymh/llm/llm_call_config.hpp:32-46`) gains
  `std::string endpoint;` — the endpoint **name**; `""` = the anonymous default
  endpoint — and `std::string profile_id;` — the selected model's profile id
  (`ModelProfile::id`, `""` = inert), required so the resolver can build the
  provider with the model's profile (54-D1/H2). Both keys are omitted from
  `to_json` when empty (52-I5).
- `ModelSelection` (`include/ymh/agent/model_selection.hpp:32-38`) gains
  `std::string endpoint;` (same meaning).
- `buildRequest` sets `config.endpoint = selected.endpoint` and
  `config.profile_id = selected.profile.id`
  (`src/agent/agent_loop.cpp:577-579`).
- **Aggregate-init sites.** Inserting `ModelSelection::endpoint` at position 3
  (after `model`, `model_name`) breaks the three brace-init sites
  `src/host/host_runtime.cpp:802`, `src/agent/workspace_runtime.cpp:197` (the
  `ResolveFn` site 54-D8 depends on), and `src/agent/agent_loop.cpp:539`; all
  three are updated in the same change (54-A9). The first two supply
  `entry->endpoint.name`. The third is `AgentLoop::effective_model_selection`'s
  no-controller fallback (`agent_loop.cpp:537-539`); its source `AgentConfig`
  (`include/ymh/agent/agent.hpp:109-126`) has **no endpoint field today**, so it
  can only supply `""`. **Pin the additive field `AgentConfig::endpoint`,
  populated from `resolved.endpoint.name` in `to_agent_config`
  (`src/cli/wiring.cpp:229-243`)**, so the fallback carries the resolved default
  endpoint instead of silently falling to the anonymous default route. This keeps
  a no-controller session on the endpoint its configured default model selects.
  Reachability: production always sets `services_.model_selection`
  (`src/agent/workspace_runtime.cpp:268`), so the fallback is a harness/test
  path; pinning the field keeps it consistent with `buildRequest` regardless.
- Route keys are **typed and profile-aware**, so neither a provider id can
  collide with an endpoint nor one profile with another: `RouteKey{kind, name,
  profile_id}` with `kind ∈ {Provider, Endpoint}`. The **anonymous default
  endpoint** keeps its existing `Provider`-kind key (`provider_config_.provider`,
  `profile_id == ""`), preserving the default path exactly; each **named
  endpoint** registers an `Endpoint`-kind key `{Endpoint, name, profile_id}`. (A
  raw `"@" + name` scheme is rejected: a provider id literally named `@foo`
  would collide with endpoint `foo`.)
- `resolve_adapter` resolves `config.endpoint` first (an `Endpoint`-kind
  `RouteKey{Endpoint, config.endpoint, config.profile_id}`), and only when it is
  empty falls back to the existing `Provider`-kind `config.provider` /
  default-route logic.

**Why a new field rather than overloading `provider`.** 28 §3.5 pins
`LlmCallConfig::provider`'s source and semantics, 53-D7/H4 pins
`ModelSelection.provider = entry.endpoint.provider`, and
`model_message_source(provider, model)` (`agent_loop.cpp:1123`) consumes it for
provenance. Re-purposing `provider` as an endpoint name would silently change
all three. The additive `endpoint` field keeps each meaning intact.

**Byte-identity (52-I5).** `to_json(LlmCallConfig)` already omits empty
optionals (`llm_call_config.hpp:84-109`); `endpoint` and `profile_id` are each
written only when non-empty. For a config with no named endpoints **and an
empty default profile** (`llm.profile` unset) every request has `endpoint == ""`
and `profile_id == ""`, so the event and canonical-template bytes are unchanged
and `kTemplateSchemaVersion` (`llm_runtime.hpp:36`) is **not** bumped. A
non-empty default `profile_id` — a config that sets `llm.profile` independently
of named endpoints (`src/config/config.cpp:2329`) — writes the new key and so
takes the usual schema-version treatment. Both are new optional keys, not a
re-basing of the canonicalization.

### 54-D3 — Per-request route selection (the single-route design becomes N-route)

**Yes, the current single-route design must become per-request route
selection.** The route cannot be per-daemon because a session's model (and
therefore its endpoint) is per-session (53-I8), and two sessions in one
workspace may use different endpoints concurrently (54-F4).

Pinned resolution order in `resolve_adapter` (replacing the provider-only
lookup at `src/llm/llm_runtime.cpp:333-359`):

1. If `config.endpoint` is non-empty, look for a registered `Endpoint`-kind
   route `RouteKey{Endpoint, config.endpoint, config.profile_id}` — i.e. matched
   on **both** the endpoint name **and** the profile id (Rev 3 HIGH). On hit,
   return that adapter. A different profile on the same endpoint is a *miss*
   here, not a hit, so it proceeds to step 2 and builds its own provider.
2. On miss, if a `RouteResolver` is installed, call it with
   `(config.endpoint, config.profile_id)`, **outside** the registry mutex. The
   resolver owns the cache and the registration (54-D1/H4): it performs the
   check-build-insert under `endpoint_cache_mutex_` and retains the
   `AdapterHandle`. On a non-null result, `resolve_adapter` re-locks and re-reads
   the route (which now exists); because the check-build-insert is serialized
   under `endpoint_cache_mutex_`, a concurrent request for the same pair finds
   the cache populated and the resolver returns the already-built provider —
   exactly one provider is constructed and retained per pair, and there is no
   losing provider (54-I11). On a null result, continue.
3. **Only when `config.endpoint` is empty**, fall through to the legacy lookup:
   match `config.provider` against registered routes, then the runtime default
   route. When `config.endpoint` is **non-empty**, steps 1–2 are the *only*
   allowed resolution and a miss goes straight to step 4 — a named endpoint must
   never fall back to the default/provider route (Rev 2 H1 / 54-I5 / 54-I10).
4. If nothing resolves, throw `NoProviderRouteError` (28 §3.5) — never a silent
   fallback to another endpoint.

`prepare_call` and `stream` pass the whole `LlmCallConfig` (or its `endpoint` +
`provider`) to the lookup (`:294-298`, `:301-307`); the `PreparedCall` keeps the
input config verbatim (28 §3.5).

**Why step 3 is gated (Rev 2 H1).** For a named endpoint, `config.provider` is the
endpoint's provider id (e.g. `openai-compatible`) — exactly the route key of the
anonymous startup adapter (`src/agent/workspace_runtime.cpp:283-292`). An
unguarded fallback would therefore dispatch a request for endpoint A to the
**default** endpoint's `base_url`/`api_key`, carrying a model meant for another
endpoint: the exact silent wrong-endpoint/credential bug 53-I9 exists to
prevent, reintroduced on the failure path. Gating on `config.endpoint.empty()`
keeps 54-I5/54-I10 true on every path, and matches 54-D3 step 4 and 54-D1.

### 54-D4 — The new endpoint guard

The `session.set_model` guard (`src/host/host_runtime.cpp:791-798`) is
replaced. It still resolves the target through `ModelCatalog::find` (unknown →
`InvalidParams`, 53-F5, unchanged). The guard now tests **membership of the
target's endpoint NAME** in the daemon's routable set — it no longer compares
`base_url` (that was a 53-I9 leftover; the name is the identity, 54-D2) and
never consults `ProviderId`:

```cpp
// 54-D4 (replaces the 53-I9 equality check at host_runtime.cpp:791-798)
// `runtime_.is_routable_endpoint(name)` consults the daemon's startup endpoint
// snapshot: true for the anonymous default (`""`) and for every
// `llm.endpoints` name. HostRuntime does not own the Config.
if (!runtime_.is_routable_endpoint(entry->endpoint.name)) {
    // Defensive only: 52-I2 makes this unreachable for a valid config.
    throw_mapped(WireError{protocol::code_value(protocol::AppCode::EndpointNotRouted),
                           "EndpointNotRouted"});
}
```

`is_routable_endpoint` is declared in the §4 `WorkspaceRuntime` sketch (Rev 2 H5).
It tests membership of the endpoint **name** only: the empty string is the
anonymous default, any other value must be a key of the startup
`llm.endpoints` map. `base_url` and `ProviderId` are not part of the test.

Pinned:

- **Reject** only a target whose endpoint **name** is not in the daemon's
  routable set (the empty anonymous default, or a named `llm.endpoints` entry in
  the startup config).
- **Accept** any same- or cross-endpoint model whose endpoint name is routable.
- `ProviderId` alone never satisfies and never defeats the guard (a shared
  `ProviderId` is not consulted; membership is by name).
- The guard tests **routability only**; it does not construct the endpoint. A
  routable endpoint whose construction fails is committed by `set_model` and
  surfaces on the next request (54-F12) — accepted, because a switch-time probe
  would construct on `/model` and duplicate the request-path failure (and would
  reintroduce the eager-construction regression 54-D1(a) rejects).
- `EndpointNotRouted` (`-32021`) is **retained** for wire compatibility and the
  defensive branch, but is no longer produced for any valid config: 52-I2 makes
  every `llm.models.<n>.endpoint` name an existing `llm.endpoints` entry at
  config-load time (`52-I2`, `52-F2`), and the catalog is built from that same
  config.
- **What an unknown endpoint yields.** An endpoint cannot be named directly by
  the RPC (the parameter is a model name/id), so the observable errors are:
  - unknown model name/id → `InvalidParams` (53-F5, unchanged);
  - a catalog entry with an unresolvable endpoint (an invariant violation, not a
    user input) → `EndpointNotRouted` (defensive).
  See 54-OQ-6 for whether to retire the code once it is provably unreachable.

### 54-D5 — Provider / config lifecycle

- **Per-endpoint `LLMProviderConfig`.** A new overload
  `to_provider_config(const ResolvedEndpoint&, const ModelProfile&)` builds one
  config from an endpoint's `ResolvedEndpoint` plus the selected model's profile.
  The existing `to_provider_config(const Config&)` (`src/cli/wiring.cpp:86-105`)
  remains for the default/startup path; it delegates the endpoint/profile copy to
  the overload and additionally sets `provider.model = resolved.model_id` so the
  startup provider's bytes and its `models()` metadata are unchanged. The
  overload copies `provider`, `base_url`, `api_key_env`, `api_key`, `headers`,
  the three timeouts, and `profile` from the arguments. The `ModelProfile&`
  argument is always non-null: when `find_model_profile(profile_id)` returns
  `nullptr` (empty or unknown id, 54-I6/54-U1) the resolver binds the inert
  `ModelProfile{}` instead (54-D1), so this overload needs no nullable/optional
  profile parameter. It converts
  `ResolvedEndpoint::retry` (`RetrySettings`) field-by-field into
  `LLMProviderConfig::retry` (`RetryPolicy`) — `max_attempts`, `base_delay`,
  `max_delay`, `jitter`, `honor_retry_after` — exactly as
  `src/cli/wiring.cpp:99-103` already does. It does **not** copy
  `max_concurrency` (inert today, 54-OQ-2) and does **not** copy a model id:
  `LLMProviderConfig::model` is a default-only field never consulted for
  dispatch (54-D1 option (c); only `OpenAICompatibleProvider::models()` reads it,
  `src/llm/openai_adapter.cpp:1014-1019`), so a lazily built provider is
  constructed with an empty model and the per-request `LLMRequest.model` is
  authoritative. This is why `model_id` is neither a `RouteResolver` parameter
  nor part of the cache key: two models sharing `(endpoint, profile)` share one
  provider with no dispatch collision. Note `profile` comes from the
  `ModelProfile` argument, not from `ResolvedEndpoint`: that struct has no
  profile field (the profile lives on `ResolvedModel`, `config.hpp:498`), so the
  earlier "exactly the fields `ResolvedEndpoint` carries" phrasing was wrong.
- **Connection pool / retry / timeouts.** Each endpoint's provider carries its
  own `retry` and timeouts (per-endpoint in config). There is **no** per-endpoint
  connection pool to own: the transport is stateless (§2.7). `max_concurrency`
  is **inert** today (the daemon's `LLMPool` is `ResourceCaps`-sized,
  `workspace_runtime.cpp:170`); 54 does not change that — recorded as 54-OQ-2.
- **Config changes.** Config is loaded once at daemon start (21/52/53); the
  endpoint map is a startup snapshot. The daemon never reloads and never writes
  config. A later on-disk edit is not observed by the daemon (mirrors the
  53 §10.1 point-in-time picker). The route cache lives for the daemon's
  lifetime; a daemon restart rebuilds it.
- **Failure is per endpoint.** A construction failure (54-F1/F2) surfaces on
  the request that selected the endpoint as a typed provider error; other
  endpoints and sessions are unaffected.
- **Secrets.** Each provider holds only its own endpoint's key/headers; a
  request for endpoint A can never carry endpoint B's credentials (54-I10).

### 54-D6 — The mid-flight interaction, and the new profile truth

53 pinned: a switch is **queued** while a turn is open and applied at the next
step boundary; an in-flight request is never mutated (53-I7); the switch is
per-session (53-I8). 54 preserves all of that. What changes:

- **Routing follows the switch.** The pending `ModelSelection` now carries the
  endpoint name (54-D2). At the step boundary it is committed and the next
  `buildRequest` stamps the new `config.endpoint` and `config.profile_id`;
  `resolve_adapter` selects and (on first use) constructs that endpoint's
  provider. The in-flight request keeps its old `LlmCallConfig`, hence its old
  endpoint/profile.
- **Loop-side profile** continues to follow the selection (53-D7) — unchanged.
- **Adapter-side profile — the honest new truth.** 53 §10.1 pinned that the
  adapter's `LLMProviderConfig.profile` is baked once from the **startup** model
  (`wiring.cpp:104`) and therefore never follows a switch. 54 binds it when the
  provider for a `(endpoint, profile_id)` pair is constructed, from the model
  that triggered construction. So:
  - a **cross-endpoint** switch re-binds adapter-side shaping (it uses the new
    endpoint's provider, built with the new model's profile);
  - a **within-endpoint** switch on a **named endpoint** that changes the profile
    id also re-binds it: the Endpoint-kind route key carries the profile id
    (54-D1/Rev 3), so the new profile's request **misses** the old profile's route
    at 54-D3 step 1 and builds its own provider (a different cache key, 54-D1/H6).
    The **anonymous default endpoint** is profile-blind: it keeps its existing
    `Provider`-kind route key (`profile_id == ""`), so a within-default profile
    change does not re-bind. This is vacuous today (the default entry carries a
    single fixed profile), but becomes false the moment a second profile is
    allowed on the default endpoint; see 54-I9;
  - only a switch that keeps both the endpoint and the profile id reuses the
    provider — which is correct, because the shaping is identical.
  The 53 limitation is therefore **narrowed to profiles the cache cannot
  distinguish** (an unidentifiable/inline profile, or a future per-request
  override). Full per-request adapter shaping remains open as 54-OQ-1, which
  supersedes 53-OQ-10. This is recorded, not hidden.
- **Scope — cross-session (Rev 2 H6).** The `(endpoint, profile_id)` cache is
  **daemon-global**, shared by every session in the workspace, not per-session.
  So adapter-side profile binding is a cross-session property: a provider built
  for `(endpoint, profile A)` serves every session that selects that pair.
  Keying by `profile_id` is what keeps a `tool_calls=false` capability baked by
  session A from making session B's tool-calling requests on the same endpoint
  fail with `UnsupportedModel`; keying by endpoint alone would have made that
  bleed cross-session. 54-I9 states this scope explicitly.

### 54-D7 — Compaction and other non-conversation request paths

A summarizer request is an LLM request and must route to the **requested
summarizer model's** endpoint (the session's, unless a named override wins),
never the startup endpoint. Pinned:

- `ContextCompactor` gains a read-only reference to the `ModelCatalog` (it is
  constructed after `model_selection_`, `workspace_runtime.cpp:183-198`,
  `:296-297`). In `runCompaction`, after `resolve_summarizer_model`
  (`compactor.cpp:131-141`, `:259`), it resolves the summarizer's **route** so
  that `config.profile_id` always pairs with the model actually requested
  (`config.model`), mirroring the normal path (`config.profile_id =
  selected.profile.id`, where `selected.model == config.model`; 54-D2). Pinned:
  - When the resolved summarizer model is the session's **durable** model (the
    common case), the route is the session's durable selection — the header's
    `model_name` through the catalog, 54-D8 — for endpoint, profile id, **and**
    provider, so the summarizer reuses the session's `(endpoint, profile_id)`
    provider.
  - When it is a **different** named `llm.models` entry (a
    `policy_.summarizer_model` override), that entry supplies the endpoint **and**
    the profile id: the override's own route, extending 54-OQ-7 from the
    endpoint to the profile.
  - When it is a literal / has no catalog entry, the endpoint falls back to the
    session's durable endpoint (else the catalog's default entry) and
    `config.profile_id` is the inert `""`. A literal model has no catalog profile
    to pair with, and reusing the session model's profile would apply the wrong
    adapter-side shaping (`forbidden_stop_tokens`, `normalize_tool_arguments`,
    capability gates) to a different model.
  It sets `config.endpoint`, `config.profile_id`, and `config.provider`
  (`compactor.cpp:268-269`). **This corrects Rev 4's blanket "`config.profile_id`
  is sourced from the same durable selection as `config.endpoint`"**: that rule
  paired the session model's profile with the summarizer's model whenever the two
  differed. The profile must follow the model, not the session. The compactor has
  only a `ModelCatalog`, so it cannot see a pending selection — the durable
  header is the only correct source.
- If the resolved endpoint is not routable, the compactor takes its existing
  `NoProviderRoute` failure path (`compactor.cpp:296-301`), never a fallback.
- The summarizer model may legitimately differ from the session model
  (`policy_.summarizer_model`); when the resolved model is the session's model
  the endpoint **and profile id** are the session's current route, because that
  is the connection the user is paying for and the session's model selection
  expresses. For a **literal** override only the **endpoint** is the session's;
  the profile id is the inert `""` (bullet 3 — a literal has no catalog profile
  to pair with). When the override instead names an
  `llm.models` entry, that entry's own endpoint **and profile id** win, so the
  adapter-side shaping always matches the requested model (recorded as 54-OQ-7).

### 54-D8 — Durable endpoint identity on resume/fork

`SessionModelChanged` already carries `{model, model_name}`
(`include/ymh/session/events.hpp:258-261`), but the fold materializes only
`header_.model = payload.model` (`src/session/session.cpp:630-632`, `:691-693`,
`:572-574`). `ModelSelectionController::durable_` resolves
`session.header().model` (a wire id) through `ResolveFn`
(`src/agent/model_selection.cpp:154-184`). Two `llm.models` entries may share a
wire id on **different endpoints**; `ModelCatalog::find` returns the
sorted-first match (`model_selection.cpp:301-323`) — a NAMED entry, even ahead
of the default — so a resumed/forked session can silently route to the wrong
endpoint, the very class of bug 53-I9 guarded against.

**Where identity is lost (Rev 2 MEDIUM).** The durable identity exists only in the
**event log**. The `create` and `fork` paths still discard it, and the durable
store is the SQLite `sessions` table, not `SessionHeader::to_json`/`from_json`:

- `createSession` resolves the picked name to a wire id and assigns
  `options.model = entry->model_id` (`src/host/host_runtime.cpp:652`), dropping
  `entry->name`; `SessionStarted` (`events.hpp:33-37`) has no `model_name`
  field, so the created header has none either.
- `Session::fork` copies only `child.model = parent.header().model`
  (`src/session/session.cpp:751`); the child header gets no `model_name`.
- The durable store writes/reads the `sessions` table
  (`src/session/session_persistence.cpp:36-51`; `read_header_row` `:404-421`),
  which persists `model` and the `metadata` JSON envelope via
  `encode_metadata_column`/`decode_metadata_column` (`:251-348`) — not
  `to_json`/`from_json`.

Pinned:

- `SessionHeader` (`session.hpp:44-66`) gains `std::optional<std::string>
  model_name;` (or an empty-string field; the implementer picks the sentinel,
  matching the existing optional style).
- `SessionOptions` (`session_manager.hpp:29-42`) and `SessionStarted`
  (`events.hpp:33-37`) each gain `std::string model_name;` (or optional); the
  `session.create` handler sets both `options.model = entry->model_id` **and**
  `options.model_name = entry->name` (`host_runtime.cpp:652`), so the name
  reaches the header at creation.
- `Session::fork` copies `child.model_name = parent.header().model_name` beside
  `child.model` (`session.cpp:751`).
- The fold in `Session::append`/`reload` materializes `header_.model_name` from
  `SessionModelChanged.model_name` beside `model`.
- **Durable persistence.** The `sessions` table must recover the name: either a
  new `model_name` column read by `read_header_row`, or — preferred, no schema
  migration — the existing `metadata` JSON envelope, extending
  `encode_metadata_column`/`decode_metadata_column`
  (`session_persistence.cpp:251-348`) so `read_header_row` (`:404-421`) restores
  it. (`SessionHeader::to_json`/`from_json` (`session.cpp:269-325`) are not the
  durable path; they may still carry the field for other consumers, but they do
  not satisfy this requirement.)
- `ModelSelectionController::durable_` resolves by `model_name` when present
  (an entry name is unambiguous), falling back to the wire id for pre-54
  sessions.

This is the additive 01/53-A12 amendment; the event payload is unchanged.

---

## 4. C++ interface sketches (pinned)

```cpp
// ── include/ymh/llm/llm_call_config.hpp ─────────────────────────────────────
struct LlmCallConfig {
    ProviderId                   provider;   // 28 §3.5 source; default/legacy fallback
    std::string                  endpoint;   // NEW (54-D2): endpoint NAME; "" = anonymous default
    std::string                  profile_id; // NEW (54-D1/H2): ModelProfile::id; "" = inert
    ModelId                      model;
    // … existing fields unchanged …
};
// to_json: write "endpoint"/"profile_id" only when non-empty (52-I5 byte-identity).
// call_config_equals: add `&& left.endpoint == right.endpoint
//   && left.profile_id == right.profile_id` (src/llm/llm_runtime.cpp:127-133),
//   so an endpoint or profile change starts a new series (28 §5.3).

// ── include/ymh/llm/llm_runtime.hpp ─────────────────────────────────────────
// 54-D1: injected lazy factory, carrying the profile identity so the provider is
// built with the selected model's profile (Rev 2 H2 / Rev 3). `model_id` is not
// passed: the provider's model is dispatch-inert (54-D1 option (c)). Empty name
// is never passed (the default endpoint keeps its Provider-kind route). Returns
// nullptr on a construction failure; resolve_adapter then fails loud. The
// resolver does the check-build-insert under its own cache mutex and retains the
// AdapterHandle (Rev 2 H4); resolve_adapter re-reads the route afterwards.
using RouteResolver = std::function<std::shared_ptr<LLMProvider>(
    std::string_view endpoint_name, std::string_view profile_id)>;

// 54-D1/LOW + Rev 3 HIGH: typed, profile-aware route key. Provider-kind keys use
// profile_id == ""; Endpoint-kind keys are {Endpoint, endpoint_name, profile_id}.
enum class RouteKind : std::uint8_t { Provider, Endpoint };
struct RouteKey {
    RouteKind   kind;
    std::string name;
    std::string profile_id;
    friend bool operator==(const RouteKey&, const RouteKey&) = default;
};

class LlmRuntime {
public:
    // Existing:
    AdapterHandle register_adapter(std::vector<ProviderId> routes,
                                   std::shared_ptr<LLMProvider> adapter);
    // 54-D1/Rev 3: registers a named-endpoint route keyed by the (endpoint,
    // profile) pair. register_adapter keeps its ProviderId API and maps each id
    // to RouteKey{Provider, id, ""}. The caller retains the returned
    // AdapterHandle (Rev 2 H4).
    AdapterHandle register_endpoint_route(std::string_view endpoint_name,
                                          std::string_view profile_id,
                                          std::shared_ptr<LLMProvider> adapter);
    [[nodiscard]] Task<PreparedCall> prepare_call(LlmCallConfig config,
                                                  CancellationToken cancel) const;
    // NEW: startup-only, one-shot — a second call must assert/throw (54-I13).
    void set_route_resolver(RouteResolver resolver);
private:
    // 54-D3: const and non-mutating; delegates registration to `resolver_`
    // outside `mutex_`, then re-locks and re-reads (H3).
    [[nodiscard]] std::shared_ptr<LLMProvider> resolve_adapter(
        const LlmCallConfig& config, RetryPolicy& out_retry) const;
    mutable std::mutex         mutex_;          // guards registrations_
    // 54/Rev 3: Registration::routes changes from std::vector<ProviderId> to
    // std::vector<RouteKey>; register_adapter maps ProviderId ->
    // RouteKey{Provider, id, ""}.
    std::vector<Registration>  registrations_;
    std::optional<RouteResolver> resolver_;     // set once at startup
};

// ── include/ymh/agent/model_selection.hpp ───────────────────────────────────
struct ModelSelection {
    std::string          model;        // wire id
    std::string          model_name;   // llm.models entry name; "" for a literal
    std::string          endpoint;     // NEW (54-D2): endpoint name; "" = default
    GenerationParameters parameters;
    ModelProfile         profile;
    ProviderId           provider;
};

// ── include/ymh/agent/workspace_runtime.hpp / .cpp ──────────────────────────
class WorkspaceRuntime {
public:
    // Rev 2 H5: membership of the endpoint NAME in the startup snapshot
    // ("" = anonymous default). Never consults ProviderId or base_url.
    [[nodiscard]] bool is_routable_endpoint(std::string_view name) const;
    // …
private:
    // Impl owns the cache + handles + registry + endpoint snapshot and installs
    // the resolver:
    //   std::map<std::pair<std::string, std::string>,
    //            std::shared_ptr<LLMProvider>> endpoint_cache_;  // (endpoint, profile_id)
    //   std::map<std::pair<std::string, std::string>, AdapterHandle>
    //       endpoint_handles_;  // sole owner, keyed (endpoint, profile) (Rev 2 H4/Rev 3)
    //   mutable std::mutex endpoint_cache_mutex_;
    //   std::optional<AdapterHandle> adapter_handle_;            // startup route
};
// Resolver body (54-D1/H4): take endpoint_cache_mutex_; on a cache miss build
// LLMProviderConfig from the endpoint's ResolvedEndpoint + find_model_profile(
// profile_id). The lookup is NULLABLE — find_model_profile returns nullptr for
// ""/unknown ids (the "" no-profile case is explicitly supported, 54-I6/54-U1) —
// so bind the inert profile rather than dereferencing null:
//   static const ModelProfile kInertProfile{};             // id == ""
//   const ModelProfile* p = find_model_profile(profile_id);
//   const ModelProfile& profile = (p != nullptr) ? *p : kInertProfile;
// to_provider_config(endpoint, profile); providers_.create; register_endpoint_route(
// name, profile_id, provider) and retain the AdapterHandle in endpoint_handles_
// (pair key); insert; return. Never call
// under LlmRuntime::mutex_ (retry_policy() is taken by register_adapter before
// that lock, :246-248).

// ── src/agent/agent_loop.cpp — buildRequest ─────────────────────────────────
LlmCallConfig config;
config.provider   = selected.provider;
config.endpoint   = selected.endpoint;       // NEW (54-D2)
config.profile_id = selected.profile.id;     // NEW (54-D1/H2)
config.model      = selected.model;
// …

// ── src/agent/compactor.cpp — runCompaction (54-D7) ─────────────────────────
// Pseudocode only: `summarizer_route(...)` and `endpoint_provider_id(name)` are
// NOT pinned symbols. The route MUST pair
// the profile id with the model actually requested (`config.model`), exactly as
// the normal path does (config.profile_id = selected.profile.id, selected.model
// == config.model; 54-D2):
//   - resolved model == session's durable model → session's durable route
//     (endpoint + profile id from the header's model_name, 54-D8);
//   - resolved model names a different llm.models entry → that entry's endpoint
//     AND profile id (54-OQ-7);
//   - resolved model is a literal / no entry → session's durable endpoint (else
//     the catalog default) with the inert profile id "" (a literal has no catalog
//     profile; the session model's profile would mis-shape it).
// The durable lookup MUST read the durable header, not
// `ModelSelectionController::effective()` (pending-wins): a queued switch is
// applied only at step boundaries, so `effective()` would diverge from the
// request path during a pending switch (54-D7).
LlmCallConfig config;
const auto route  = summarizer_route(model, session, catalog);  // 54-D7
config.endpoint   = route.endpoint;    // requested model's endpoint (54-D7/OQ-7)
config.profile_id = route.profile_id;  // PAIRED with `model`, not the session's (54-D7)
config.provider   = endpoint_provider_id(config.endpoint);
config.model      = model;

// ── include/ymh/session/session.hpp — SessionHeader (54-D8) ─────────────────
struct SessionHeader {
    // … existing fields …
    std::string                model;
    std::optional<std::string> model_name;   // NEW: llm.models entry name, when known
};
```

---

## 5. Invariants (54-I)

| ID | Invariant |
|---|---|
| 54-I1 | Every LLM request carries its endpoint identity in `LlmCallConfig::endpoint`; the route is selected **per request** from the session's effective selection, never from a daemon-global binding. Two sessions on different endpoints dispatch concurrently without interference. |
| 54-I2 | Exactly **one provider is retained and registered per `(endpoint, profile_id)`** for the daemon's lifetime (the cache key **and** the Endpoint-kind route key), on the first request that selects it; a **named-endpoint** pair no request selects is never constructed (the anonymous default endpoint's provider is built eagerly at startup). The check-build-insert is serialized under `endpoint_cache_mutex_`, so exactly one provider is constructed and retained per pair — there is no losing provider (54-I11); a failed construction is not retained and is retried on the next request (54-F1). |
| 54-I3 | The `session.set_model` guard tests membership of the target's endpoint **name** (empty = the anonymous default) in the daemon's routable set; `base_url` is not compared, and a shared `ProviderId` never satisfies or defeats it. It rejects only a target whose endpoint name is not routable. |
| 54-I4 | The daemon's routable endpoints are exactly the startup config's anonymous default endpoint plus every `llm.endpoints` entry. For a valid config every `ModelCatalog` entry's endpoint is routable (52-I2), so `EndpointNotRouted` is not reachable from user input. |
| 54-I5 | A request is never sent to an endpoint other than the one in its call config. A route miss fails loud with `NoProviderRouteError`; there is no fallback to another endpoint and no wrong-endpoint send. |
| 54-I6 | For a config with no named endpoints **and an empty default profile** (`llm.profile` unset), the request/event bytes, the route, and the default-route fallback are unchanged (52-I5 preserved); the `endpoint` and `profile_id` keys are omitted when empty. A non-empty default `profile_id` from a set `llm.profile` writes the new key and takes the usual schema-version treatment (54-D2). An empty `profile_id` is a **supported** no-profile case: the resolver binds the inert `ModelProfile{}` (`id == ""`) rather than dereferencing the nullable `find_model_profile` result (54-D1; never a null deref, 54-U1). |
| 54-I7 | A session's endpoint survives daemon restart via the durable model selection; resume resolves by entry name when known, else by wire id (54-D8). |
| 54-I8 | The daemon never writes config and never reloads it; the config/endpoint map is a **startup snapshot** and point-in-time. The route cache is **runtime-populated** (lazily, on first selection) and lives for the daemon's lifetime; a daemon restart rebuilds it. |
| 54-I9 | Adapter-side profile is bound per `(endpoint, profile_id)` at that provider's construction; for **named endpoints** the **Endpoint-kind route key carries the same pair**, so dispatch for one profile can never return another profile's provider; the cache is daemon-global (cross-session, 54-D6). On a named/Endpoint-kind route, a switch that changes the endpoint **or** the profile id re-binds it; a switch that keeps both does not. The anonymous default endpoint keeps its profile-blind `Provider`-kind startup route (`profile_id == ""`), so a within-default profile change does not re-bind — vacuous today (the default entry carries a single fixed profile), false the moment a second profile is allowed on the default endpoint. |
| 54-I10 | Per-endpoint credentials never cross endpoints: a request routed to endpoint A carries only A's key/headers (52-I4 extended), on every path including a resolver miss/failure (no fallback to the default endpoint, 54-D3/H1). |
| 54-I11 | Exactly one route/provider exists per `(endpoint, profile_id)` pair (the Endpoint-kind route key), including after any concurrent construction race: the check-build-insert is serialized under `endpoint_cache_mutex_`, so exactly one provider is constructed and retained and no losing provider is ever built. The startup default keeps its separate `Provider`-kind route. |
| 54-I12 | The queued-switch semantics are unchanged: an in-flight request keeps the endpoint it started with; the new endpoint applies at the next step boundary (53-I7). |
| 54-I13 | `set_route_resolver` is **one-shot**: it may be called at most once, before any request; a second call asserts/throws and `resolver_` is never replaced while a request may be in flight (Rev 2 LOW). |

---

## 6. Failure modes (54-F)

Continue the repo's `F#`-tagged convention with the spec-local `54-F` prefix
(disjoint from `§54 F1–F12`; the shared taxonomy is the 42/44 usage cited by 52
§3.8).

| ID | Trigger | Symptom | Recovery |
|---|---|---|---|
| 54-F1 | Keyless endpoint: no literal `api_key` and an empty/invalid `api_key_env` (52-F23) | the endpoint's provider construction fails **on first use** with a typed `ConfigError`/`ProviderFailed`; other endpoints/sessions are unaffected; the daemon still starts | set `api_key`/`api_key_env`; 54 pins that the next request retries construction (see 54-OQ-8) |
| 54-F2 | An endpoint's `base_url` is unreachable | the request that selected it fails after the endpoint's retry policy; other sessions/endpoints unaffected | fix `base_url`; no daemon restart needed for the retry to succeed (the route is cached, the request is re-attempted) |
| 54-F3 | A model references an endpoint absent from the startup config (a client-side picker row added after daemon start) | the daemon's catalog is the startup snapshot; an unknown model → `InvalidParams`; a known model keeps the startup endpoint (no silent re-route) | restart the daemon to pick up the new config |
| 54-F4 | Two sessions on different endpoints run concurrently | each request routes to its own endpoint via its own provider; no cross-talk and no shared mutable request state (54-I1/I10) | none |
| 54-F5 | A subagent is created on a different endpoint from its parent | the subagent is a distinct session; its model resolves at `session.create` (pinned); routing is per-request, so a different endpoint is supported with no constraint | none (53-OQ-9 retained; the resolution point is pinned, so 54-OQ-4 is narrowed to *which* model is inherited, not *when* it resolves) |
| 54-F6 | Two threads select the same not-yet-built `(endpoint, profile_id)` concurrently | serialized under `endpoint_cache_mutex_`: the first constructs and retains the provider, the second observes the cache hit and reuses it; exactly one provider is constructed (54-I2/I11) | none |
| 54-F7 | Resume/fork of a session whose wire id is shared by two entries on different endpoints | resolved by entry name (54-D8) when the name was folded; a pre-54 session without a name falls back to the sorted-first wire-id match (today's behavior, recorded) | re-issue `/model` to pin the intended entry |
| 54-F8 | Compaction on a session switched to a named endpoint | the summarizer routes through the requested model's endpoint **and profile id** (54-D7), never the startup endpoint; the omitted-profile pair is never used when the requested model **has** a profile (an empty `profile_id` is its correct pair when it has none) | none |
| 54-F9 | Two models on one **named** endpoint have different profile ids | each `(endpoint, profile_id)` gets its own provider (cache **and** route key, 54-I2/I9/H6): the second profile's request misses the first's route and builds its own provider; the profiles never bleed, intra-session or cross-session. The anonymous default endpoint is profile-blind (Provider-kind route, `profile_id == ""`), so this case cannot arise there (54-I9) | none (the 53 §10.1 limitation is narrowed to profiles the cache cannot distinguish; see 54-OQ-1) |
| 54-F10 | No route resolves (`endpoint` unknown and `provider` unregistered, or the resolver returns null) | `NoProviderRouteError` at `prepare_call` (28 §3.5), loud, before a `PreparedCall` exists; no silent fallback | fix the config/model; the error names the route |
| 54-F11 | The `/model` picker offers a model the daemon's startup catalog does not know | `InvalidParams` (53-F5); no event, no pending entry | restart the daemon, or pick a row the daemon knows (54-OQ-3) |
| 54-F12 | `/model` commits a switch to an endpoint that is **routable** (54-D4 passes) but whose provider construction fails (`create` error / keyless, 54-F1) | `set_model` commits the switch (guard checks routability only); the failure surfaces on the **next request** as a typed `ConfigError`/`ProviderFailed`, isolated to that session/endpoint; the daemon and other sessions are unaffected | fix the endpoint config, then re-issue `/model` (or restart the daemon) to retry construction (54-OQ-8) |

---

## 7. Amendment register

| ID | Amended / superseded clause | Verified code anchor | New behaviour |
|---|---|---|---|
| 54-A1 | 53-I9 (`53:1035`), 53-F6 (`53:1055`) | `src/host/host_runtime.cpp:791-798` | The endpoint guard no longer requires equality with the startup endpoint; it rejects only a non-routable endpoint (54-D4). |
| 54-A2 | 53 §10.1 (`53:1235-1238`), 53-OQ-3 (`53:1272-1275`) | `src/agent/workspace_runtime.cpp:283-291`; `src/llm/llm_runtime.cpp:333-359` | Cross-endpoint switching is supported; OQ-53-3 closed. |
| 54-A3 | 53 §10.1 profile risk (`53:1239-1247`), 53-OQ-10 (`53:1298-1306`) | `src/cli/wiring.cpp:104`; `src/llm/openai_adapter.cpp:1055`, `:1103`; `src/llm/provider_registry.cpp:131` | Adapter profile is bound per `(endpoint, profile_id)` at construction; a switch changing either re-binds it (54-D6/H6). Full per-request shaping is 54-OQ-1. |
| 54-A4 | 53 §8 (`53:1098`) | — | "Unroutable session blocks the composer" parity item re-recorded as 54-OQ-5. |
| 54-A5 | 28 §3.5 (`28:389-440`), 28 §3.1 (`28:175-215`), 28 §5.3 `call_config_equals`/`starts_series` (`28:575-599`), 31 §5.3 `held_config_` (`31:424-468`) | `include/ymh/llm/llm_call_config.hpp:32-46`; `src/llm/llm_runtime.cpp:127-133`, `:333-359` | `LlmCallConfig` gains `endpoint` **and** `profile_id`; route resolution is endpoint-first, with provider fallback **only when `endpoint` is empty**. A change of either field starts a new header series (28 §5.3); `held_config_` carries the same field. |
| 54-A6 | 08 §5.1–§5.3 (`52:107`), single-`provider_` daemon (`28:127-130`) | `src/agent/workspace_runtime.cpp:395-417`; `src/llm/provider_registry.cpp:56-75` | Multiple providers coexist; one per `(endpoint, profile_id)`, built lazily. |
| 54-A7 | 01-session.md `SessionHeader` (`session.hpp:44-66`), 53-D6/53-A12 (`53:591-648`, `:1081`) | `src/session/session.cpp:630-632`, `:751`; `src/session/session_manager.hpp:29-42`; `include/ymh/session/events.hpp:33-37`; `src/session/session_persistence.cpp:251-348`, `:404-421`; `src/agent/model_selection.cpp:154-184` | `SessionHeader.model_name` folded and carried through `SessionOptions`/`SessionStarted` at create, copied at fork, and persisted in the `sessions` table (metadata envelope) so `read_header_row` recovers it; durable resolution prefers the name. |
| 54-A8 | 32/13 compaction summarizer route | `src/agent/compactor.cpp:131-141`, `:259`, `:268-269`; `src/cli/wiring.cpp:284` | Summarizer routes through the requested summarizer model's `(endpoint, profile_id)` — the session's durable route when it is the session's model, else the named override's own endpoint and profile (54-D7/OQ-7). |
| 54-A9 | 53-D7/H4 (`53:655-666`), 53-D5 step 3 (`53:571-573`) | `include/ymh/agent/model_selection.hpp:32-38`; `include/ymh/agent/agent.hpp:109-126`; `src/cli/wiring.cpp:229-243`; `src/host/host_runtime.cpp:802`, `:801-804`; `src/agent/workspace_runtime.cpp:197`; `src/agent/agent_loop.cpp:539` | `ModelSelection` gains `endpoint`; the three aggregate-init sites are updated in the same change, and `AgentConfig` gains `endpoint` (from `resolved.endpoint.name`) so the `agent_loop.cpp:539` fallback supplies a real endpoint name. |
| 54-A10 | 52 §1.4 (`52:121-133`), 53-A7 (`53:84`) | `src/ui/command_registry.cpp:174-194` | Retained: `/model` stays 53-owned; 54 changes only daemon routing. |
| 54-A11 | 53-U13 `HostRuntimeTest.SetModelCrossEndpointRejected` (`tests/unit/host_runtime_test.cpp:738-752`), which asserted 53-I9/53-F6 rejection | `src/host/host_runtime.cpp:791-798` | **Inverted** by 54-D4: the cross-endpoint target is now accepted and appends one event (54-U6). The old assertion is removed, not merely relaxed. |
| 54-A12 | 52-I2 (every `llm.models.<n>.endpoint` names an existing `llm.endpoints` entry) | `src/agent/model_selection.cpp:92-111` | **Relied on** by 54-I4: routability of every catalog entry is guaranteed at config-load time. |
| 54-A13 | 52-I4 (secrets are global-layer only; no cross-endpoint leakage) | `src/llm/provider_registry.cpp:56-75`; `src/cli/wiring.cpp:86-105` | **Extended** by 54-I10: each per-endpoint provider holds only its own key/headers, including on the resolver-miss path. |
| 54-A14 | 52-I5 (byte-identity for configs with no named endpoints and an empty default profile) | `include/ymh/llm/llm_call_config.hpp:84-109` | **Preserved** by 54-I6 for configs with no named endpoints **and an empty default profile** (`llm.profile` unset): `endpoint`/`profile_id` are omitted when empty; `kTemplateSchemaVersion` is not bumped. A non-empty default `profile_id` writes the new key (54-D2). |

---

## 8. Test plan (54-U)

Conventions follow §44. `ctest` is **not** parallel-safe and must not run during
a build; run the suite only after the build completes.

### 8.1 Unit

| ID | Test | Asserts |
|---|---|---|
| 54-U1 | `LlmCallConfig.endpoint`/`profile_id` serialization, and the empty-profile resolution | `endpoint == ""` and `profile_id == ""` omit the keys; the default-path canonical JSON and event bytes are byte-identical to pre-54 (52-I5/54-I6). A request with `profile_id == ""` (and with an unknown id) resolves through the resolver without a null deref and builds its provider with the inert `ModelProfile{}` (`id == ""`), i.e. the same **inert-profile treatment** as the pre-54 default path (54-D1). |
| 54-U2 | `resolve_adapter` route selection | `endpoint` set → the `Endpoint`-kind `RouteKey{Endpoint, name, profile_id}` route; the same endpoint with a **different** `profile_id` is a **miss** (not the first profile's route); empty `endpoint` → the `Provider`-kind route; unknown → `NoProviderRouteError`; a provider id named `@foo` does not collide with endpoint `foo`. |
| 54-U3 | Lazy construction + cache | the resolver is invoked once per `(endpoint, profile_id)`; a second request reuses it; an unused pair is never built; two profiles on one **named** endpoint get **distinct providers and distinct routes** (54-I2/I9/H6); the anonymous default endpoint keeps the profile-blind `Provider`-kind route, so no profile split is expected there (54-I9). |
| 54-U4 | Construction failure | a resolver returning null (or a `create` error) yields `NoProviderRouteError`; the failure is not cached as a success (54-F1/F10). |
| 54-U5 | Catalog endpoint identity | a wire id shared by two entries resolves by name to the right endpoint (`model_selection.cpp:301-323`). |
| 54-U6 | Cross-endpoint `set_model` accepted | the existing `SetModelCrossEndpointRejected` (`tests/unit/host_runtime_test.cpp:738-752`) is **inverted**: `cross` (endpoint `other`) is accepted and appends one event (54-D4). |
| 54-U7 | Guard negatives | unknown model → `InvalidParams`; unknown session → `UnknownSession`; neither appends (53-F4/F5 retained). |
| 54-U8 | `SessionHeader.model_name` | folds on append/reload; durable resolution prefers it; old rows (no field) fall back to wire id (54-D8). |
| 54-U9 | Race | two threads selecting one `(endpoint, profile_id)` are serialized under `endpoint_cache_mutex_`: the resolver constructs exactly once, the second thread observes the cache hit, and exactly one provider/route is retained (54-I2/I11). |
| 54-U18 | Profile binding per `(endpoint, profile_id)` (54-I9/H7) | two endpoints with **different** profiles: each endpoint's provider keeps its own profile; a cross-endpoint switch re-binds; on a **named endpoint**, a same-endpoint switch that keeps the same profile does not re-bind and a switch to a different profile **does** re-bind — the new profile's request **misses** the old profile's Endpoint-kind route and builds its own provider (54-D1/Rev 3). The anonymous default endpoint is profile-blind (Provider-kind route), so a within-default profile change does not re-bind — vacuous today (54-I9). Also asserts the cross-session case: session B on the same `(endpoint, profile)` reuses session A's provider without capability bleed. |
| 54-U19 | Guard: shared `ProviderId`, different endpoints (54-I3 second clause) | two catalog entries share a wire id on different endpoints: the guard accepts the routable one and rejects a non-routable one by **name**, regardless of the shared `ProviderId` (neither satisfied nor defeated). |
| 54-U22 | `set_route_resolver` one-shot (54-I13) | a second `set_route_resolver` call asserts/throws and leaves `resolver_` unchanged; the first resolver stays installed and no request observes a replaced resolver. |

### 8.2 Integration (fake transport / fake LLM)

| ID | Test | Asserts |
|---|---|---|
| 54-U10 | Two sessions, two endpoints, interleaved | each request's `base_url` is the endpoint named by that session's model; no cross-talk. **Credential capture (54-I10):** the captured request for endpoint A carries A's `api_key` and A's `headers` and never B's, and vice versa. |
| 54-U11 | Mid-turn switch | a queued switch applies at the step boundary; the in-flight request keeps its endpoint; the next request uses the new one (54-I12). |
| 54-U12 | Compaction routing | a session on a named endpoint compacts through the requested summarizer model's `(endpoint, profile_id)` provider (the same pairing as the normal request path), not the startup endpoint; the omitted-profile pair is never used when the requested model **has** a profile (an empty `profile_id` is its correct pair when it has none) (54-D7). |
| 54-U13 | Subagent on another endpoint | parent and child route independently (54-F5). |
| 54-U14 | Config drift | a picker row unknown to the startup catalog → `InvalidParams`; a known row keeps the startup endpoint (54-F3/F11). |
| 54-U15 | Keyless endpoint | one keyless endpoint fails its own request loudly; a second endpoint still serves (54-F1). |
| 54-U20 | Unreachable `base_url` (54-F2/H8) | with a fake transport that errors only for endpoint B: B's request fails after its own retry policy; endpoint A still serves and other sessions/endpoints are unaffected. |
| 54-U21 | Switch-time construction failure (54-F12) | a routable endpoint whose construction fails: `set_model` commits the switch (guard passes), the next request fails loudly and in isolation, and re-issuing `/model` retries construction. |

### 8.3 PTY / live (opt-in)

| ID | Test | Asserts |
|---|---|---|
| 54-U16 | Live cross-endpoint `/model` | with `YMH_LIVE_LLM=1` and a second endpoint (or a local mock server), pick a model on the second endpoint and confirm the request reaches the second `base_url`; the status line shows the new model name. |
| 54-U17 | Live keyless | a keyless endpoint produces a loud, redacted error and does not disturb the default endpoint. |

### 8.4 Test-running note

`ctest` is not parallel-safe. Do not run it during `cmake --build`; run it after
the build completes (`ctest --test-dir build --output-on-failure`).

**Spec-catalog coverage (Rev 2 LOW / Rev 3 MEDIUM).** The new spec-bearing
symbols introduced by this errata (`RouteResolver`, `RouteKey`/`RouteKind` with
its `profile_id` member and defaulted `operator==`, `is_routable_endpoint`,
`register_endpoint_route`, `set_route_resolver`, `SessionOptions::model_name`,
`SessionStarted::model_name`, `LlmCallConfig::endpoint`,
`LlmCallConfig::profile_id`) must be added to
`tests/fixtures/spec_symbol_catalog.json` and covered by
`tests/unit/spec_catalog_test.cpp`, or that suite will fail on the new surface.

---

## 9. Open questions (54-OQ)

1. **54-OQ-1 — Full per-request adapter profile** (supersedes 53-OQ-10). 54
   already keys providers **and routes** by `(endpoint, profile_id)` and threads
   `profile_id` through the call config (54-D1/H2/H6/Rev 3), so adapter-side
   shaping follows every profile-id change and the 53 §10.1 limitation is removed
   for identifiable profiles. What remains open is a profile that is **not**
   identifiable by id (an inline/per-request override): does that warrant
   per-request `ToolCallPolicy` shaping, or is the id sufficient? 54 narrows the
   question to that residual (54-D6/54-I9).
2. **54-OQ-2 — Per-endpoint `max_concurrency`.** It is parsed into
   `ResolvedEndpoint` but never consumed; the daemon's `LLMPool` is
   `ResourceCaps`-sized. Should the pool become per-endpoint (or per-route)?
   Out of scope for 54.
3. **54-OQ-3 — Daemon-sourced model list.** The `/model` picker is a
   client-side snapshot of `config.llm.models` (53 §10.1). Should the daemon
   expose the routable catalog over RPC so the picker cannot offer a row the
   daemon will reject (`InvalidParams`)? 54 keeps 53's behavior.
4. **54-OQ-4 — Subagent model inheritance** (53-OQ-9 retained). The resolution
   *point* is pinned by 54-F5: the child's model resolves at `session.create`.
   What remains open is *which* selection the child is created with — the
   parent's effective endpoint/model or the config default. 54 supports either
   at the routing layer, so only the creation-time default is unpinned.
5. **54-OQ-5 — Reachability / composer block.** dsh raises a composer block when
   no adapter serves a route (53 §8). ymh has no unroutable sessions for a valid
   config; a reachability probe or a blocked/disabled composer state is
   deferred.
6. **54-OQ-6 — Retire `EndpointNotRouted`?** Once the guard is provably
   unreachable for a valid config (54-I4), should the `AppCode` be retired or
   kept as a defensive code? 54 keeps it (wire compatibility) and marks the
   branch defensive.
7. **54-OQ-7 — Summarizer override endpoint and profile.** If
   `policy_.summarizer_model` names an `llm.models` entry on a different endpoint
   **or with a different profile** than the session, which route wins? 54 pins
   the override's own endpoint **and** profile id for a named entry (so the
   profile pairs with the requested model, 54-D7); for a literal override, the
   session's endpoint with an inert profile id. The exact resolution mechanism
   (catalog lookup by wire id vs. by entry name) is deferred.
8. **54-OQ-8 — Recoverability of a failed endpoint.** A construction failure is
   not cached as a success (54-F1), so the next request retries. Should a
   successful construction after an earlier failure be observed without a
   daemon restart? 54 pins "yes" (retry on next request); recorded for review.
9. **54-OQ-9 — `headers`/`api_key` redaction across endpoints.** 52-I4 covers
   the secrets; confirm the new per-endpoint provider error paths (54-F1/F2)
   never render a key or a header value. Implementation detail; pinned as a test
   assertion (54-U15/54-U17).

---

## 10. Revision log

- **Rev 6** — four residuals from the final fix pass applied (2 MEDIUM / 2 LOW).
  - **MEDIUM — compactor paired the summarizer model with the session profile.**
    54-D7/§4 set `config.profile_id` from the session's durable selection, but
    `config.model` comes from `resolve_summarizer_model`
    (`compactor.cpp:131-141`, `:259`) and may be `policy_.summarizer_model`, a
    different model. The normal path pairs the profile with the requested model
    (`selected.model == config.model`), so the summarizer ran with the session
    model's adapter-side shaping (`forbidden_stop_tokens`,
    `normalize_tool_arguments`, capability gates). 54-D7 now pairs the profile
    with the requested summarizer model (session route when it is the session's
    model; a named override's own endpoint **and** profile when it differs; the
    session's endpoint with an inert `""` for a literal). §4 sketch and 54-OQ-7
    (now endpoint **and profile**) updated; 54-A8, 54-F8, 54-U12 aligned.
  - **MEDIUM — third aggregate-init site had no endpoint source.**
    `src/agent/agent_loop.cpp:539` builds the no-controller fallback
    `ModelSelection` from `AgentConfig`, which has no endpoint field and whose
    `to_agent_config` (`src/cli/wiring.cpp:229-243`) sets none, so it could only
    supply `""` — silently routing a no-controller session to the anonymous
    default. 54-D2/54-A9 now pin the additive `AgentConfig::endpoint` populated
    from `resolved.endpoint.name` in `to_agent_config`, and note reachability:
    production always sets `services_.model_selection`
    (`src/agent/workspace_runtime.cpp:268`), so this is mainly a harness/test
    path.
  - **LOW — 54-F8/54-U12 overclaimed about the inert pair.** "Never the inert
    `(endpoint, "")` pair" is false when the requested model has no profile —
    then `(endpoint, "")` *is* its correct pair. Reworded to "never the
    omitted-profile pair when the requested model **has** a profile".
  - **LOW — profile-rebind clauses unqualified.** 54-I9, 54-D6, and 54-U18
    stated the within-endpoint profile rebind unconditionally, but it is carried
    by the **Endpoint-kind** route key; the anonymous default endpoint uses the
    profile-blind Provider-kind startup route. Qualified to named/Endpoint-kind
    routes, with the default-endpoint case noted as vacuous today (a single
    fixed default profile) and false once a second default profile is allowed.
- **Rev 5** — four residuals from two independent adversarial re-checks applied
  (1 MEDIUM / 3 LOW).
  - **MEDIUM — 54-U1 overstated provider-config identity.** 54-U1 claimed the
    lazily built provider has "the same provider config as the pre-54 default
    path", contradicting 54-D5 (the lazy overload does **not** copy a model id)
    and 54-D3 (the resolver runs only for a non-empty endpoint, so the config
    carries the named endpoint's `base_url`/`api_key`). Reworded to the same
    **inert-profile treatment**; the provider-config-identity comparison is
    dropped.
  - **LOW — byte-identity claim overbroad.** `llm.profile` is independent of
    named endpoints: `resolve_model` sets `out.profile =
    resolved_profile(config.llm.profile)` (`src/config/config.cpp:2329`), which
    flows to the default catalog entry and a non-empty `config.profile_id`. A
    config with no named endpoints but a set default profile therefore writes
    the new key and changes the bytes. Qualified consistently at 54-D2, 54-I6,
    and 54-A14 to "no named endpoints **and an empty default profile**", with an
    explicit note that a non-empty default `profile_id` takes the usual
    schema-version treatment.
  - **LOW — compactor helpers misnamed.** The §4 helpers
    `effective_endpoint_name`/`effective_profile_id` are renamed
    `durable_endpoint_name`/`durable_profile_id` and gain an explicit "must not
    call `ModelSelectionController::effective()`" note, so they cannot be wired
    to the pending-wins accessor and diverge from the request path during a
    queued switch (54-D7).
  - **LOW — 54-I2 lost its qualifier.** "A pair no request selects is never
    constructed" now reads "a **named-endpoint** pair …", since the anonymous
    default endpoint's provider is built eagerly at startup.
- **Rev 4** — two residual findings from the independent adversarial re-check
  applied (1 MEDIUM / 1 LOW), both exposed by the Rev 3 profile-aware route key.
  - **MEDIUM — empty/unknown `profile_id` binding.** The resolver maps
    `profile_id` to a `const ModelProfile&`, but `find_model_profile` is
    total/noexcept **only in the does-not-throw sense** and returns `nullptr` for
    `""` (`src/llm/model_profile.cpp:40-47`) and unknown ids. `""` is an
    explicitly supported input (54-I6/54-U1: a model with no profile, e.g. on a
    named endpoint), so the pinned `const ModelProfile&` could not bind for a
    supported case. **Chosen: option (a)** — the resolver substitutes a
    function-local static inert `ModelProfile{}` (`id == ""`, byte-identical to
    `profile_for("")`, `src/agent/model_selection.cpp:37-42`) when the lookup is
    null, and the `const ModelProfile&` overload is kept. Pinned in 54-D1,
    54-D5, the §4 resolver body, 54-I6, and 54-U1. (Option (b), a nullable
    `const ModelProfile*` overload, was rejected: it ripples a nullable argument
    through `to_provider_config(const Config&)` for no gain.)
  - **LOW — compactor omitted `config.profile_id`.** The §4 `runCompaction`
    sketch set `config.endpoint` and `config.provider` but not
    `config.profile_id`, so with the Rev 3 `(endpoint, profile_id)` route key the
    summarizer would build/look up `(endpoint, "")` — the inert profile — and
    diverge from the normal request path; the pre-Rev-3 endpoint-only key masked
    it. 54-D7 and the sketch now set `config.profile_id` from the **same**
    durable selection as `config.endpoint` (the resolved catalog entry's
    `profile` id, 54-D8); 54-A8, 54-F8, and 54-U12 updated to match.
- **Rev 3** — re-check finding set applied (1 HIGH / 3 MEDIUM / 5 LOW).
  - **HIGH (headline, three independent reviewers)** — the Endpoint-kind route
    key was endpoint-**name**-only while only the cache key was
    `(endpoint, profile_id)`, so a second profile's request **hit** the first
    profile's route in 54-D3 step 1 and returned its provider without ever
    consulting the resolver/cache. `RouteKey` is now
    `{kind, name, profile_id}`; `register_endpoint_route(endpoint_name,
    profile_id, adapter)`; 54-D3 step 1 matches on **both**; and
    `endpoint_handles_` is keyed by the same pair (a name-keyed map let the
    second profile's handle assignment evict the first via
    `AdapterHandle::operator=`, `src/llm/llm_runtime.cpp:194-206`). Reconciled
    to one mechanism: 54-D1, 54-D2, 54-D3, 54-D6, 54-I2, 54-I9, 54-I11, 54-F9,
    54-U2, 54-U3, 54-U18, 54-OQ-1, §4. The 54-I2 ("per `(endpoint,
    profile_id)`") vs 54-I11 ("exactly one route per endpoint") contradiction is
    resolved explicitly: **both now say per `(endpoint, profile_id)`**.
  - **MEDIUM** — 54-F6/54-I11/54-U9 reworded to the pinned mechanism: the
    check-build-insert is serialized under `endpoint_cache_mutex_`, so duplicate
    construction **cannot** happen and "the losing provider is discarded" is
    removed. New test 54-U22 for the 54-I13 `set_route_resolver` one-shot
    invariant (a second call asserts/throws; `resolver_` is not replaced).
    §8.4's spec-catalog list gains `LlmCallConfig::endpoint`.
  - **LOW** — `model_id` was a dead parameter (54-D1 option (c): the provider's
    `model` field is dispatch-inert); it is **dropped** from `RouteResolver` and
    from `to_provider_config(const ResolvedEndpoint&, const ModelProfile&)`, and
    54-D5 pins that `LLMProviderConfig::model` is **not** copied for lazily built
    providers (only `OpenAICompatibleProvider::models()` reads it), so no
    `(endpoint, profile)` collision exists. The typed key is threaded through the
    route store: `Registration::routes` → `std::vector<RouteKey>`,
    `RouteKey::operator==` pinned, `register_adapter` maps each `ProviderId` to
    `RouteKey{Provider, id, ""}`; §2.1 now marks its `vector<ProviderId>` as the
    shipped (pre-54) state. `endpoint_handles_` population pinned as
    `try_emplace`/`insert_or_assign` (`operator[]` is unavailable for the
    move-only, non-default-constructible `AdapterHandle`,
    `include/ymh/llm/llm_runtime.hpp:133-154`). The three `ModelSelection`
    aggregate-init anchors were **verified against the tree** —
    `src/host/host_runtime.cpp:802`, `src/agent/workspace_runtime.cpp:197`,
    `src/agent/agent_loop.cpp:539` — and 54-D2/54-A9 already cite the same,
    correct lines. 54-D1's `register_adapter` parenthetical corrected to
    `include/ymh/llm/llm_runtime.hpp:196` (the old `src/llm/llm_runtime.hpp:225`,
    `:232` named the const `resolve_adapter` and the `registrations_` store, and
    the path was wrong).
- **Rev 2** — five-reviewer finding set applied (8 HIGH / 11 MEDIUM / 15 LOW).
  - **H1** (confirmed by two reviewers) — the failure-path silent
    wrong-endpoint fallback is closed: 54-D3 step 3 is gated on
    `config.endpoint.empty()`; for a named endpoint a resolver miss goes
    straight to step 4 (`NoProviderRouteError`). 54-I5/54-I10 and 54-D1 now hold
    on every path.
  - **H2** (two reviewers) — `RouteResolver` now carries model identity:
    `(endpoint_name, model_id, profile_id)`, and `LlmCallConfig` gains
    `profile_id`; 54-D3 step 2, 54-D5, 54-D6 and §4 updated. D6's profile claim
    is now implementable.
  - **H3** — constness reconciled: registration is delegated to the resolver
    (non-const context, outside `mutex_`); `resolve_adapter` stays `const` and
    non-mutating; the mutable-store/private-`insert_route` alternative is
    documented as rejected (54-D1, §4).
  - **H4** — one owner pinned: the resolver registers the route and retains the
    `AdapterHandle` in `WorkspaceRuntime::Impl::endpoint_handles_`; the
    check-build-insert runs under `endpoint_cache_mutex_`; `LlmRuntime` no
    longer re-registers (54-D1, §4).
  - **H5** — `is_routable_endpoint` is now declared in the §4 `WorkspaceRuntime`
    sketch (and referenced from 54-D4).
  - **H6** — the cache is keyed by `(endpoint_name, profile_id)`, not endpoint
    name alone; the cross-session scope is stated in 54-D6/54-I9 and the
    capability-bleed hazard (54-F9) is removed.
  - **H7** — 54-I9 gains test row 54-U18 (cross-endpoint re-binds; same
    `(endpoint, profile)` does not; same endpoint/different profile does;
    cross-session reuse).
  - **H8** — 54-F2 gains integration test row 54-U20 (fake transport errors only
    for endpoint B; A still serves).
  - **MEDIUM** — 54-I2 reworded (retained-once, races build-then-discard,
    failures retried); new failure mode 54-F12 (switch-time construction
    failure) + test 54-U21; 54-I3/54-D4 corrected to endpoint-**name**
    membership (`base_url`/`ProviderId` not consulted); 54-I8 reworded
    (snapshot vs runtime-populated cache); 54-D8 now carries `model_name`
    through `SessionOptions`/`SessionStarted`, `Session::fork`, and the
    `sessions` table metadata envelope (`read_header_row`), not
    `to_json`/`from_json`; §1.3 54-A5 adds 28 §5.3 and 31 §5.3; 54-U10 asserts
    captured key/header isolation; new guard test 54-U19 (shared wire id, two
    endpoints); §7 adds 52-I2/52-I4/52-I5 (54-A12/A13/A14); 54-OQ-4 reconciled
    with 54-F5; the three `ModelSelection` aggregate-init sites are pinned
    (54-D2, 54-A9).
  - **LOW** — §2.3 ProviderId history marked unverifiable/historical; §2.4
    struct ranges corrected (`182-195`, `474-486`); 54-A8/§2.6 `wiring.cpp:284`;
    54-A9/§7 canonical `53:571-573`; §2.2 `resolve_model` `2364-2385` +
    `active_endpoint` `2380-2383`; §1.1 relabelled (not verbatim); 54-A3
    `53:1298-1306`; typed `RouteKey` replaces the undefined `EndpointKey` and
    the collision-prone `"@"+name`; lazy-route retry sourcing pinned
    (`retry_policy()` outside the registry lock); `set_route_resolver` one-shot
    pinned as 54-I13; 54-D5 corrected (`ResolvedEndpoint` has no `profile`,
    carries `max_concurrency`; explicit `RetrySettings`→`RetryPolicy`
    conversion); 54-D7 "durable selection"; §4 compactor helpers labelled
    pseudocode; 54-A11 registers the inverted 53-U13; §8.4 notes the
    spec-catalog fixture/test coverage.
- **Rev 1** — initial draft. Authored from the reported defect after spec 53
  landed. Routing model (b) chosen; new endpoint guard pinned; 53 §10.1 profile
  limitation narrowed (not removed); compaction and durable-endpoint paths
  included. Awaiting independent gate review before any implementation.

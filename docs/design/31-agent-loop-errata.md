# 31 — Agent-Loop Errata: The `LlmRuntime` Seam (spec-06 amendment)

```
Status: written · verified: — · reviewer: —
Revision: Rev 2 — fixes the gate-31 MEDIUM (the `LlmCallConfig::provider`
          source) and the six LOWs. `LlmCallConfig::provider` is now sourced from
          a new `AgentConfig::provider` (config-driven, set by `to_agent_config`
          from `config.llm.provider`), defaulting to the runtime's registered
          default route when unset; a missing route is a loud typed
          `prepare_call` failure (`A-F23`). `28` (the `LlmCallConfig` owner) is
          amended to pin this same source — the cross-cutting decision that also
          closes the `13` errata's M2; this errata consumes it and defines no
          competing source. Rev 1 was the initial write: the Wave-1 **blocking
          prerequisite** recorded by `30-architecture-cascade-errata.md` §5.1
          (item 1) and `28-llm-service-boundary-errata.md` §13.1, pinning the
          `06`-owned half of `26-D1` (`Brk. (06, 08)`): the `AgentServices`
          provider seam (`LLMProvider*` → `LlmRuntime*`), the `AgentRegistry`
          construction path, and the loop's provider call.
Component: 31 (errata) — amends `06-agent-loop.md` §4.1/§5.1/§5.2/§5.3/§5.7/§10/
           §11.2 by reference. It does **not** edit `06-agent-loop.md` in place,
           and it does **not** amend `08` (that is `28`) or `13` (that is `32`).
Depends on: `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS) §4.2
            (26-D1 :139), §4.3.1 :172-291 (the runtime/frozen-request sketch),
            §4.3.2 :293-390 (the logged header + reconstruction contract),
            §4.4 :1087-1093 (the `06` classification :1092), §4.8 :1257-1290,
            §4.9 :1291-1351, §5 Wave 1 :1397-1422;
            `28-llm-service-boundary-errata.md` (verified Rev 2; amended for the
            provider-source decision) §3.1/§3.3/§3.4/§5.1/§5.3/§6.2/§6.3/§8/
            §9 (L18–L25)/§10 (L-F19–L-F26)/§13;
            `29-event-family-errata.md` (verified) §4.2 :306-310 (the
            `LlmRequestHeader` projection is metadata/ignored);
            `30-architecture-cascade-errata.md` §5.1 :261-293;
            `06-agent-loop.md` (verified); `13-context-compaction.md`
            (verified); the working tree (all `file:line` re-derived).
Scope:    pin the loop-side seam that the verified `LlmRuntime` service boundary
          forces on `06`: (1) `AgentServices` carries one `LlmRuntime*` and no
          raw provider/registry/config; (2) `AgentRegistry` no longer creates
          adapters — its `ProviderRegistry&` convenience ctor becomes
          `LlmRuntime&`, its provider fallback and `provider_`/`providerStorage_`
          members are removed; (3) `AgentLoop` dispatches through
          `prepare_call` → `PreparedCall::stream` under one `LLMPool` slot, and
          `buildRequest` derives its config from the last logged
          `LlmRequestHeader` (with `LlmCallConfig::provider` from
          `AgentConfig::provider`, runtime default route when empty); (4) the
          loop invariants/failure modes that follow.
          Design only — no code, no behavior change.
Supersedes: (quoted with anchors; each is a clause this errata replaces or
          re-scopes)
          - `06 §4.1 :433-437` "`AgentRegistry(SessionManager&, ResourceGovernor&,
            ToolRegistry&, PermissionPolicy&, ProviderRegistry&,
            ContextAssembler&, AgentConfig);`" — the `ProviderRegistry&`
            parameter is replaced by `LlmRuntime&`; the `AgentServices` ctor
            (`:435-437` text, tree `include/ymh/agent/agent_registry.hpp:29`) is
            unchanged in shape.
          - `06 §5.1 :601-604` "`request := buildRequest(messages, model,
            options)` … `response := provider.stream(request, sink, cancel)`" —
            the per-call config and the direct `provider.stream` call are
            replaced by a header-derived `FrozenRequest` and
            `prepare_call`/`PreparedCall::stream` (26-D1/D3).
          - `06 §5.2 :700-702` "The summarization call goes through the same
            `LLMProvider` seam" — the seam is the runtime, per `28 §8`.
          - `06 §5.3 :681-706` `Compactor` seam — the frozen `Compactor`
            interface is **retained**; only the concrete `ContextCompactor`
            construction changes (13-owned, `32`).
          - `06 §10 A12 :1042-1044` "Every provider call … is bracketed by one
            slot acquire/release" — retained verbatim; the bracketed unit is now
            `prepare_call` + one `PreparedCall::stream`.
          - `08 §5.1 :656-659` and `08 §14.1(n)` "spec 06 owns the resolved
            `LLMProvider` instance" — already superseded by `28 §3.4`; this
            errata removes the last `06`-side residue (`AgentRegistry::provider_`,
            `src/agent/agent_registry.cpp:37-44,111`).
          - `26 §5`'s Stage-B Wave-4 scheduling of the `06` errata
            (`26p2:1389`) — moved to the Wave-1 gate by `30 §5.1`.
Amends:   `06-agent-loop.md` §4.1 (ctor shape), §5.1 (step 2), §5.2/§5.3
          (provider-seam prose), §5.7 (the re-attempt is a new call), §10
          (invariants, additively), §11.2 (failure modes, additively) — all by
          reference, not in place. Also pins the `AgentConfig::provider` field
          (`include/ymh/agent/agent.hpp:94-104`, the config the loop consumes;
          `31-D7`).
Numbering: claims spec number **31**. `28`/`29`/`30` are the Wave-0 Stage-A
          freezes; `32` is claimed by the sibling `13` compaction errata
          (lead's assignment map). The four Wave-3+ component specs that
          `26 §5` reserved as `27-system-prompt.md`/`28-output-retention.md`/
          `29-agent-presets.md`/`30-goals-jobs-commands.md` therefore renumber
          to **33+** (the `26 §5` "renumber to 31+" statement in
          `29-event-family-errata.md` is superseded by this claim).
```

This document is a **pin**, not a proposal. Every count, `file:line`, and
behavior below was verified against the working tree before it was written. The
tree today has **no** `LlmRuntime`, **no** `FrozenRequest`, **no**
`payload::LlmRequestHeader`, and the loop holds a **raw `LLMProvider*`**. This
errata does not claim Wave 1 is implemented; it pins the `06`-owned seam so Wave
1 *can* be written, and it records what remains unimplemented.

---

## 1. Purpose, numbering, scope

### 1.1 The problem

`26-D1` (`26p2:139`) is classified `Brk. (06, 08)` with **two** owning specs.
`08`'s half is frozen by the verified `28-llm-service-boundary-errata.md`
(`LlmRuntime`/`PreparedCall`/`FrozenRequest`, the logged header, the one-attempt
rule). `06`'s half was left unassigned: `26 §5` Stage A lists only `00`/`01`/`08`
as up-front items, so no Wave-0 owner existed for `06`, and `26 §5` schedules the
`06` errata in Stage-B Wave 4 (`26p2:1389`) — *after* Wave 1, which already
changes `AgentLoop::buildRequest` (`:1404`) and re-seams `ContextCompactor`
(`:1412-1413`). `28 §13.1 :795-802` and `30 §5.1 :276-283` both flag this as a
sequencing defect. This errata closes it: `06`'s Wave-1 surface is pinned here,
before any Wave-1 code.

### 1.2 What this pins

1. The `AgentServices` provider seam: the three fields `providers`
   (`include/ymh/agent/agent_loop.hpp:53`), `provider` (`:64`), and
   `provider_config` (`:65`) are replaced by a single `LlmRuntime* runtime`
   (`28 §3.4 :351-355`); `pool` and every other field are retained.
2. The `AgentRegistry` construction path: the `ProviderRegistry&` convenience
   ctor parameter becomes `LlmRuntime&`; the fallback adapter construction
   (`src/agent/agent_registry.cpp:37-44`) and the provider injection
   (`:111`, `include/ymh/agent/agent_registry.hpp:95,97`) are removed. Adapter
   construction/registration is the daemon's boot step (`28 §3.3`).
3. The loop's provider call: `AgentLoop::buildRequest`
   (`src/agent/agent_loop.cpp:410-419`, called at `:742`/`:809`) derives an
   `LlmCallConfig` from the last logged `LlmRequestHeader` and returns a
   `FrozenRequest`; the dispatch at `:781` becomes
   `prepare_call` → `PreparedCall::stream` under one `LLMPool` slot.
4. The provider id: `LlmCallConfig::provider` is sourced from a new
   `AgentConfig::provider` (`agent.hpp:94-104`; set by `to_agent_config` from
   `config.llm.provider`, `src/cli/wiring.cpp:63,174-190`); when it is empty the
   runtime's registered default route is used. A missing route is a loud typed
   `prepare_call` failure (`A-F23`). `28` (amended) owns the pin; this errata
   consumes it and defines no second source (`31-D7`).
5. The loop invariants (`06 §10`) and failure modes (`06 §11.2`) that follow,
   additively (A19–A23, A-F19–A-F23).

### 1.3 In scope / out of scope

**In scope (this errata owns):** `06` §4.1, §5.1 (step 2), §5.2/§5.3
provider-seam prose, §5.7's re-attempt, §10, §11.2.

**Out of scope (owned elsewhere, cited not pinned):**

- The `LlmRuntime`/`PreparedCall`/`FrozenRequest` **interfaces** and the frozen
  request/canonicalization contract — `28` (08 errata) §3.1, §4.
- The `payload::LlmRequestHeader` **codec and wire keys** — `29` (01 errata);
  the header C++ payload/semantics — `28 §5`.
- The `ContextCompactor` constructor text — `13` errata (`32`); this errata
  only states the interaction (`28 §8`).
- The `session.persist_prompt_text` key — `21`.
- The retry **executor** (its wave is unassigned) — recorded in §10, not
  invented.
- `26-D10` (scheduler), `26-D17` (children), `26-D21` (reminders) — the
  additive `06` decisions, deferred to their own errata per `26p2:1092`.

### 1.4 Numbering

See the header. This errata claims **31**; `32` is the `13` errata. The `26 §5`
Wave-3+ reservations renumber to **33+**. No file under `docs/design/` is edited
by this errata except this one.

---

## 2. Amendment register

| `06` anchor | Current (verified) | Pinned target | Class |
|---|---|---|---|
| §4.1 `AgentRegistry` shape :433-437 | two ctors; the 6-ref one takes `ProviderRegistry&` | the `AgentServices` ctor unchanged; the convenience ctor takes `LlmRuntime&` | **Brk.** |
| §4.2 step 4 :477 "construct the AgentLoop bound to … provider" | `agent_registry.cpp:37-44,111` constructs/injects a provider | fallback + `provider_`/`providerStorage_` removed; daemon owns adapters | **Brk.** |
| `AgentConfig` (new field) `agent.hpp:94-104` | no `provider` field (`to_agent_config` sets no provider, `wiring.cpp:174-190`) | adds `ProviderId provider`; `LlmCallConfig::provider` from it, runtime default route when empty (`31-D7`, `A23`) | **Brk.** (06-owned) |
| §5.1 step 2 :600-612 | `buildRequest(messages, model, options)`; `provider.stream(...)` | header-derived `FrozenRequest`; `prepare_call`→`PreparedCall::stream` | **Brk.** |
| §5.2 :700-702 | "same `LLMProvider` seam" | "same `LlmRuntime` seam" (`28 §8`) | **Brk.** (text) |
| §5.3 `Compactor` seam :681-706 | `Compactor` virtual interface | **retained**; concrete ctor is `13`-owned | none |
| §5.7 :817-820 | one-shot compaction re-attempt | **retained**; it is a new `PreparedCall`, not an adapter retry (`28 §6.3`) | additive |
| §10 A12 :1042-1044 | one `LLMPool` slot per provider call | **retained**; the bracketed unit is `prepare_call`+`stream` | none |
| §10 (new) | — | A19–A23 | additive |
| §11.2 (new) | — | A-F19–A-F23 | additive |

The only **breaking** changes are the three `26-D1` items; everything else is
additive or retained.

---

## 3. The `AgentServices` seam change (26-D1)

### 3.1 Current shape (verified against the tree)

`AgentServices` (`include/ymh/agent/agent_loop.hpp:44-70`) carries, among the
non-LLM services:

```cpp
// include/ymh/agent/agent_loop.hpp (current)
ProviderRegistry*     providers = nullptr;      // :53
LLMProviderConfig     provider_config;          // :65
LLMProvider*          provider = nullptr;       // :64
LLMPool*              pool = nullptr;           // :66
```

- `agent_loop.hpp` includes `ymh/llm/provider_registry.hpp` (`:31`) for
  `LLMProviderConfig` and forward-declares `class LLMProvider;` (`:42`).
- The loop's only provider use is `services_.provider->stream(request, sink,
  turnToken).get()` (`src/agent/agent_loop.cpp:781`) behind a null check
  (`:766-768`). `services_.providers` and `services_.provider_config` are never
  read in `agent_loop.cpp` (grep-verified): they exist solely for the
  `AgentRegistry` fallback (§4).
- `provider_config` is an `LLMProviderConfig` (the `ProviderRegistry` header's
  type); the daemon keeps its own copy for the factory
  (`src/agent/workspace_runtime.cpp:199`).

### 3.2 Pinned target

```cpp
// include/ymh/agent/agent_loop.hpp (target) — 06 §4.1 amended by reference
// 08-owned runtime header (28 §3.1): included, not forward-declared — it
// defines LlmCallConfig (needed by `held_config_`, §5.3) and LlmRuntime.

struct AgentServices {
    using PermissionResolver =
        std::function<PermissionOutcome(const PermissionRequest&, CancellationToken)>;

    SessionManager*       sessions = nullptr;
    ResourceGovernor*     governor = nullptr;
    ToolRegistry*         tools = nullptr;
    PermissionPolicy*     policy = nullptr;
    PermissionGate*       gate = nullptr;
    // 26-D1 / 31-D1: the provider-neutral service. Replaces `providers`,
    // `provider`, and `provider_config`; no `LLMProvider*` remains here.
    LlmRuntime*           runtime = nullptr;
    ContextAssembler*     context = nullptr;
    ExecutionEnvironment* execution = nullptr;
    Logger*               logger = nullptr;
    OutputSink*           output = nullptr;
    Compactor*            compactor = nullptr;
    ContextCompactor*     context_compactor = nullptr;   // unchanged
    TokenEstimator*       estimator = nullptr;
    LLMPool*              pool = nullptr;                // retained (A12)
    PermissionResolver    permission_resolver;
    PlanModeController*   plan_mode = nullptr;
};
```

- The `provider_registry.hpp` include (`agent_loop.hpp:31`) is dropped; the
  `LLMProvider` forward declaration (`:42`) is replaced by the 08-owned runtime
  header include, which defines `LlmRuntime` **and** `LlmCallConfig`. The header
  must include it (not forward-declare) because `held_config_` is a
  `std::optional<LlmCallConfig>` data member (§5.3). `agent_loop.cpp` gets the
  same header instead of `llm_provider.hpp` (`src/agent/agent_loop.cpp:13`).
- The field is a **pointer** (`LlmRuntime* runtime`), matching the nullable
  style of every other `AgentServices` member and preserving the existing
  null-check semantics (`:766-768`). The loop dereferences it for the call; the
  invariant is stated as "the loop holds `LlmRuntime&`" in the same sense as
  `28-L18` ("The loop holds `LlmRuntime&`").
- `pool` is **not** one of the three replaced fields (`28 §3.4` names exactly
  `providers`/`provider`/`provider_config`); it stays, because `A12` requires
  the caller to bracket every call.

### 3.3 What is retained / superseded

- **Retained:** `pool` (`A12`); `compactor`/`context_compactor` (`06 §5.3`,
  `13`); every non-LLM field; the `AgentServices`-by-value ctor
  (`include/ymh/agent/agent_registry.hpp:29`).
- **Superseded:** `08 §5.1 :656-659` / `08 §14.1(n)` ("spec 06 owns the
  resolved `LLMProvider` instance") — already superseded by `28 §3.4`; this
  errata removes the `06`-side residue (§4). The loop owns **no** adapter and
  **no** runtime: the daemon owns the runtime, and the runtime owns its
  registered adapters (`28 §3.3`, `28-D2`).

---

## 4. `AgentRegistry` construction path

### 4.1 Current (verified against the tree)

- `include/ymh/agent/agent_registry.hpp:29`:
  `AgentRegistry(AgentServices services, AgentConfig config);`
- `:30-36`: the convenience ctor
  `AgentRegistry(SessionManager&, ResourceGovernor&, ToolRegistry&,
  PermissionPolicy&, ProviderRegistry&, ContextAssembler&, AgentConfig)`.
  **No in-tree call site** exists for it (grep-verified); the daemon uses the
  `AgentServices` ctor (`src/agent/workspace_runtime.cpp:176`).
- `src/agent/agent_registry.cpp:17-31` `make_services(...)` sets
  `services.providers = &providers` (`:28`).
- `:35-56` the `AgentServices` ctor runs a **provider fallback**:
  `:37-44` — if `services_.provider == nullptr && services_.providers != nullptr`,
  it calls `services_.providers->create(services_.provider_config)` into
  `providerStorage_`, then `provider_ = providerStorage_ ? … : services_.provider`.
- `:110-111` `registerAgent` copies `services_` and injects
  `services.provider = provider_` into each new `AgentLoop`.
- Members `providerStorage_` (`include/ymh/agent/agent_registry.hpp:95`) and
  `provider_` (`:97`); the header includes `llm_provider.hpp` (`:20`) and
  `provider_registry.hpp` (`:21`).

### 4.2 Pinned target

`28 §3.4 :356-358` pins the direction: *"`AgentRegistry`'s fallback provider
construction (`src/agent/agent_registry.cpp:37-44,111`) moves to the daemon; the
registry no longer creates adapters."* This errata pins the concrete shape:

1. **The `AgentServices` ctor is unchanged** (by value, plus `AgentConfig`).
2. **The convenience ctor's `ProviderRegistry& providers` parameter becomes
   `LlmRuntime& runtime`** (`06 §4.1` amended). `make_services` sets
   `services.runtime = &runtime` instead of `services.providers`. The two-ctor
   class shape of `06 §4.1` is thus preserved with one parameter type changed.
   (The daemon does not call it today; it remains a supported alternate
   construction path and is not a second adapter-construction path.)
3. **The provider fallback is removed** (`agent_registry.cpp:37-44`): the
   `AgentServices` ctor no longer calls `ProviderRegistry::create`, no longer
   owns a provider, and no longer defaults `provider_`.
4. **The members `providerStorage_` and `provider_` are removed**
   (`agent_registry.hpp:95,97`); `registerAgent` no longer sets
   `services.provider` (`agent_registry.cpp:111`). `poolStorage_`/`pool_`
   (`:96,98`) are **retained** — the `LLMPool` fallback is unrelated to the
   provider seam and is not superseded.
5. **Includes:** `provider_registry.hpp` (`:21`) is dropped; `llm_provider.hpp`
   (`:20`) is dropped (no remaining `LLMProvider` use in the header once
   `provider_`/`providerStorage_` are gone); a `class LlmRuntime;` forward
   declaration is added.

The registry therefore holds no LLM handle at all: `AgentServices` (copied per
agent) carries the `LlmRuntime*`, and `registerAgent` only re-injects `pool`
(its own fallback) as today.

### 4.3 Daemon wiring (the new adapter-construction site)

`src/agent/workspace_runtime.cpp` is the sole adapter-construction and
registration site (`28 §3.3`):

- **Current** (`:154-174`): `services_.providers = &providers_` (`:165`),
  `services_.provider_config = provider_config_` (`:166`),
  `services_.provider = provider_.get()` (`:167`);
  `compactor_ = std::make_unique<ContextCompactor>(*provider_, …)` (`:170-173`);
  `agents_ = std::make_unique<AgentRegistry>(services_, agent_config_)` (`:176`).
  Members `providers_`/`provider_config_`/`provider_` (`:198-200`).
- **Target (Wave 1):** the daemon creates adapter instances through the retained
  factory (`register_builtin_providers`/`make_default_provider_registry`,
  `provider_registry.hpp:60-63`) at boot, registers them into its one
  `LlmRuntime` via `register_adapter`, and keeps **no owning reference**
  (`28 §3.3 :321-333`). It wires `services_.runtime = &runtime_` and drops the
  three provider fields. The compactor is constructed from `runtime_` (13-owned,
  §6). `agents_` construction is unchanged.
- `WorkspaceRuntime::provider()` (`include/ymh/agent/workspace_runtime.hpp:149`)
  and `provider_config()` (`:158`) are daemon accessors; whether they survive
  Wave 1 is a `04`/daemon-surface question, **not** a `06`-owned interface.
  This errata records it as an open item (§13) rather than pinning it.

---

## 5. The loop's provider call

### 5.1 Current (verified against the tree)

```cpp
// src/agent/agent_loop.cpp (current)
LLMRequest AgentLoop::buildRequest(const std::vector<Message>& messages) const {   // :410
    LLMRequest request;
    request.model    = config_.model;                 // AgentConfig (agent.hpp:95)
    request.messages = messages;
    if (services_.context != nullptr) {
        request.tools = services_.context->tools();   // ToolSchema list
    }
    request.parameters = config_.parameters;          // AgentConfig (agent.hpp:96)
    return request;
}

// step loop (:742, :766-781)
LLMRequest request = buildRequest(messages);
if (services_.provider == nullptr) { appendTurnFailed(turn, ProviderFailed, "no provider configured"); return; }
for (int attempt = 0; attempt < 2; ++attempt) {       // :771  (compaction re-attempt)
    if (services_.pool != nullptr) { … acquire … }     // :772-779
    response = services_.provider->stream(request, sink, turnToken).get();   // :781
    if (response.outcome != Failed || response.error.code != ContextLengthExceeded) break;
    … compactor …                                     // :795-805
    request = buildRequest(messages);                 // :809  (re-assembled)
}
```

Nothing freezes the request, nothing logs a request header, and the model/config
come from `AgentConfig` per call (`28 §1.1 :100-103`).

### 5.2 Pinned dispatch path (26-D1/D3)

`06 §5.1` step 2 is amended to:

```text
# 2. provider call, one pool slot (A12, §5.9); §5.7 one-shot re-attempt
frozen := buildRequest(messages, turn, step)   # header-derived; logs a changed header (§5.3)
for attempt in {First, CompactionRetry}:
    slot := llmPool.acquire(cancel)            # cancellable; RAII, bounded by F8, §5.9
    call := services_.runtime->prepare_call(frozen.config(), cancel).get()
    response := call.stream(std::move(frozen), sink, cancel).get()   # exactly ONE attempt (28-L24)
    # `slot` is RAII (`include/ymh/agent/llm_pool.hpp:23-38`): it releases at
    # scope exit on success, failure, and cancel — there is no `release()` call (A12).
    if response.outcome != Failed or response.error.code != ContextLengthExceeded:
        break
    if attempt == CompactionRetry:
        break                                  # still overflowing -> §5.7 CompactionFailed
    compactor.run(messages)                    # one-shot compaction (§5.3, §5.7)
    messages := contextAssembler.assemble(session, turn, step)  # re-assemble
    frozen := buildRequest(messages, turn, step)   # NEW call/step, not a re-dispatch
```

- The bracketed unit is `prepare_call` **plus** one `PreparedCall::stream`
  (`A12` retained; `28 §6.1 :545-565`).
- `PreparedCall` is **one-shot** (`28-L24`): the loop never calls `stream` twice
  on the same `PreparedCall`. The compaction re-attempt builds a **new**
  `FrozenRequest` and a **new** `PreparedCall` (`28 §6.3 :582-588`); it is a new
  step/request, not an adapter retry.
- `LlmRuntime::stream` returns a terminal `LLMResponse` (`28-L23`); the loop maps
  it to the terminal event exactly as today (`06 §5.7`), unchanged.
- The null check (`:766-768`) becomes `services_.runtime == nullptr` and keeps
  the same terminal `TurnFailed{ProviderFailed, "no provider configured"}`
  before any `prepare_call` (`A-F19`).

### 5.3 `buildRequest` derives config from the logged header (26-D2)

`buildRequest` is amended to (i) accept `turn`/`step`, (ii) map
`GenerationParameters` into an `LlmCallConfig` **exactly once** (`26 §4.9
:1328-1334`; `T-M5`) with the provider id pinned below, (iii) reconcile that
proposal against the last logged `LlmRequestHeader` via `call_config_equals`,
(iv) append a new header iff config/prompt/tools/purpose changed
(`28 §5.1 :459-472`), and (v) return the
deep-frozen envelope:

```cpp
// include/ymh/agent/agent_loop.hpp (target) — 06 §5.1 amended
// The 08-owned runtime header (defines LlmCallConfig/FrozenRequest/PreparedCall/
// LlmRuntime) is included: the `held_config_` member below needs the complete
// LlmCallConfig type, so a forward declaration is not sufficient (28 §3.1).

[[nodiscard]] FrozenRequest buildRequest(const std::vector<Message>& messages,
                                         TurnId turn, StepId step);
```

`buildRequest` is private (`agent_loop.hpp:140`). Because the header includes
the runtime header, both `agent_loop.hpp` and `agent_loop.cpp` see the complete
`FrozenRequest`/`LlmCallConfig` types.

- **Provider source (`31-D7`; the cross-cutting decision).** `LlmCallConfig::provider`
  is sourced from a new `AgentConfig::provider` (`agent.hpp:94-104`), populated
  by `to_agent_config` from `config.llm.provider` (`src/cli/wiring.cpp:63,
  174-190`). When it is empty, `buildRequest` leaves the provider unset and the
  runtime resolves its **registered default route** at `prepare_call`; a missing
  route (the named provider is unregistered, or no default route exists) is a
  loud typed `prepare_call` failure normalized at the loop to `ProviderFailed`
  (`A-F23`), never a silent fallback. `28` (amended) pins the source and the
  default-route representation; this errata consumes it and defines no competing
  source.
- **Config source.** `AgentConfig::provider`/`model`/`parameters`
  (`agent.hpp:94-104`) are the *proposed* values; the *held* values come from the
  last `LlmRequestHeader` (`28 §5.3 :502-507`). If they differ, the loop logs a
  new header with the proposed config and `starts_series = true`; it never
  dispatches a silent drift (`28-L-F19`). If they match, no header is logged and
  the held config is used.
- **Held-config lifetime.** The loop holds
  `std::optional<LlmCallConfig> held_config_` (worker-only, alongside
  `compactions_this_turn_`, `agent_loop.hpp:158`); this member needs the
  complete `LlmCallConfig` type, which is why `agent_loop.hpp` includes the
  08-owned runtime header (§3.2). It is initialized at loop construction by
  scanning `Session::events()` (`include/ymh/session/session.hpp:244`)
  backwards for the last `LlmRequestHeader`; `nullopt` only when the log has
  none (new session, or a legacy session whose pre-header portion is
  unreconstructable, `28-L20`). `LlmRequestHeader` is **projection-invisible**
  (`29 §4.2 :306-310`: `deriveMessages` must `case LlmRequestHeader:
  break;`), so there is no projection value to read — the read path is the
  `events()` scan. The codec is `29`-owned; this errata pins only the read path
  and the timing (at construction).
- **First dispatch (fresh or legacy session).** When `held_config_` is `nullopt`
  the *proposed* config — provider from `AgentConfig::provider` (or the runtime
  default route when empty) plus model/parameters — is logged as the first
  header with `starts_series = true` (`A-F22`). It cannot be filled in later:
  `PreparedCall::stream` checks `call_config_equals(request.config(), config())`
  (`28 §7 :618-620`), so the config frozen into the request is the one
  `prepare_call` binds.
- **Freeze and digest.** `buildRequest` freezes via
  `FrozenRequest::freeze(std::move(request), config)` (`28 §3.1 :189-203`) and
  the header's `template_digest` is `FrozenRequest::template_digest()`
  (`28 §4.2`). `RequestId` is never persisted (`28 §3.2 :314-317`); the header is
  found positionally at replay (`28 §5.3`).
- **Prompt text.** The header always carries `system_prompt_digest`; the full
  `system_prompt` only under `session.persist_prompt_text` (`28 §5.4`,
  `28-L25`). The Wave-1 prompt basis is the current `AgentConfig::system_prompt`
  (`agent.hpp:103`), not a prompt-registry hash (`26p2 :1414-1418`).
- **Tool catalog.** `services_.context->tools()` still supplies the schemas
  (`06 §5.2`); the header records `tool_names` + `tool_schema_digests` in
  canonical order (`28 §5.2`), and a catalog change forces a new header/series
  (`28-L-F26`). The assembler's `tools()` signature is unchanged.
- The payload type, codec, and wire keys are `28`/`29`-owned; this errata pins
  only that the **loop is the appender** and the timing (before dispatch).

### 5.4 The compaction re-attempt is not an adapter retry

`06 §5.7 :817-820` ("The loop does not retry a mid-stream failure … The one
exception is the single `ContextLengthExceeded` compaction retry") is **retained
verbatim**. `28 §6.3 :582-588` confirms it is a new step/request with a new
`PreparedCall`. The adapter's in-adapter pre-first-event retry
(`src/llm/openai_adapter.cpp:923-1000`) is **not removed in Wave 1** and remains
the single retry owner until the separate executor lands (`28 §6.2`). The loop
adds **no** retry of its own.

---

## 6. `ContextCompactor` interaction (13-owned)

- `AgentServices::context_compactor` (`agent_loop.hpp:62`) stays a
  `ContextCompactor*`; the loop's use of it (`agent_loop.cpp:386-405,690-698,
  726-727,798-805`) is unchanged.
- The **concrete** `ContextCompactor` ctor re-seams from `LLMProvider&` to
  `LlmRuntime&` (`include/ymh/agent/compactor.hpp:113-117`; member `:140`; call
  `src/agent/compactor.cpp:252`) per `28 §8 :639-664`. That text is
  **13-owned** and is pinned by the sibling errata `32`; this errata does not
  duplicate it.
- The frozen `Compactor` virtual interface (`06 §5.3 :686-692`) is
  **unaffected** — it has no provider parameter (`28 §13.3 :807-809`).
- The daemon constructs the compactor from the runtime instead of `*provider_`
  (`workspace_runtime.cpp:170-173`).

---

## 7. Breaking-change classification

Per `26p2 §4.4 :1092`, spec `06` is **`Brk. (D1); additive (D10/D17/D21)`**. The
`26 §4.2` register classifies `26-D1` as **`Brk. (06, 08)`** (`:139`).

- **Breaking (`06`-owned, pinned here):** the `AgentServices` field replacement,
  the `AgentRegistry` ctor parameter and fallback removal, the new
  `AgentConfig::provider` field, and the loop's dispatch/`buildRequest` change.
  These are not additions beside the pinned seam — they replace it.
- **Additive (`06`-owned, deferred):** `D10` (scheduler), `D17` (children),
  `D21` (reminders). This errata pins none of them; they get their own errata
  before their waves (`26p2:1092`; `26 §5` Waves 4/5).
- **Unaffected:** `A1`–`A18` except where §8 states an amendment; `06 §4.2`'s
  create transaction order; the one-agent-per-session rule; the subagent and
  terminal-event semantics.

---

## 8. Invariants (extending `06 §10`)

**A19 — Single LLM service boundary.** `AgentServices` carries `LlmRuntime*` and
no `LLMProvider*`, `ProviderRegistry*`, or `LLMProviderConfig`; `AgentLoop`
reaches adapters only through the runtime, and `AgentRegistry` holds no LLM
handle. (`26-D1`; `28-L18`; §3, §4)

**A20 — One attempt per `PreparedCall`.** The loop dispatches each
`PreparedCall` exactly once; the `ContextLengthExceeded` re-attempt builds a new
`FrozenRequest` and a new `PreparedCall` as a new step, and is never a second
`stream()` on the same call. (`28-L24`, `28 §6.3`)

**A21 — Header-driven request construction.** `buildRequest` derives its
`LlmCallConfig` from the last logged `LlmRequestHeader`; a proposed change is
logged (with `starts_series` per `28 §5.1`) before dispatch and never applied
silently. (`26-D2`; `28-L21`)

**A22 — No adapter construction in the agent layer.** `AgentRegistry` and
`AgentLoop` never call `ProviderRegistry::create` or a provider factory; adapter
construction/registration is the daemon's boot step, and the runtime owns the
registered adapters. (`28 §3.3`, `28-D2`)

**A23 — Provider id is config-driven and route-resolved at the boundary.**
`buildRequest` sets `LlmCallConfig::provider` from `AgentConfig::provider`
(populated by `to_agent_config`); when it is empty the runtime's registered
default route is used. A missing route is a loud typed `prepare_call` failure,
never a silent fallback to another provider. (`28` amended; `31-D7`; `A-F23`)

**Retained unchanged:** `A1`–`A18`. `A12` is retained and its bracketed unit is
`prepare_call` + one `PreparedCall::stream` (§5.2). `A2`/`A10` (exactly one
terminal event; failure ≠ cancel) are unchanged. `A17` (no UI dependency) is
unaffected — `LlmRuntime` is a core type.

---

## 9. Failure modes (extending `06 §11.2`)

| A-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **A-F19** | No runtime configured | `services_.runtime == nullptr` | Terminal `TurnFailed{ProviderFailed, "no provider configured"}` **before** any `prepare_call`; no event appended beyond the turn's terminal (retains the current `agent_loop.cpp:766-768` behavior) |
| **A-F20** | `PreparedCall` misuse | second `stream()` on a consumed call, or a config mismatch | `PreparedCallError{InvalidPreparedCall}` normalized at the loop to `ProviderFailed` (`28-L-F24`); the loop never re-dispatches a `PreparedCall` (`A20`) |
| **A-F21** | Silent config drift | `call_config_equals(proposed, held) == false` at `buildRequest` and no header logged | Defect (`28-L-F19`): log a new header with `starts_series = true`; never dispatch a drift |
| **A-F22** | Legacy/unreconstructable request | resumed session whose log has no `LlmRequestHeader` for a dispatch | The first new dispatch logs a header (`starts_series = true`); the pre-header portion is **legacy/unreconstructable** and is never fabricated (`28-L20`, `28-L-F25`) |
| **A-F23** | No route for `config.provider` | `prepare_call` route lookup (`28` amended) | Loud typed failure normalized at the loop to `ProviderFailed`; never a silent fallback to another provider; no `PreparedCall` is produced (`A23`) |

`A-F1`–`A-F18` are unchanged. `A-F1` (provider terminal failure) now observes the
terminal `LLMResponse` returned by `LlmRuntime::stream` (`28-L23`); `A-F10`
(compaction fails) is unchanged.

---

## 10. Retry-executor ownership gap (recorded, not resolved)

`26p2 §5` Waves 1–6 assign **no** wave to the separate durable retry executor
required by `26-I3` and the `llm/retry`/`llm/retry-started` events
(`26p2 §4.3.9.1 :947-948`); `28 §6.2 :578-580` and `28 §13.2 :803-806` record
the same gap. This errata **repeats the record and claims nothing**:

- Wave 1 does **not** implement the retry executor. The adapter's in-adapter
  pre-first-event retry (`src/llm/openai_adapter.cpp:923-1000`) remains the
  **single** retry owner (`28 §6.2`; `28-L22`).
- The loop's `ContextLengthExceeded` re-attempt is **not** the executor (§5.4).
- Exactly one retry owner at any time; removing the adapter loop is co-scheduled
  with the executor's landing (`28-L-F23`).

An owning wave must be assigned by the lead before that executor is coded.

---

## 11. dsh mapping

The dsh mapping of the service boundary is `28 §11 :732-744`. The loop-side
additions:

| dsh concept | ymh after this errata | Reference |
|---|---|---|
| the agent holds `ctx.llm` (the runtime), not a provider | `AgentServices::runtime` is the loop's only LLM handle | `28-L18`; §3 |
| `prepareCall` → `PreparedLlmCall.stream` | `prepare_call` → `PreparedCall::stream` under one pool slot | `28 §3.1`; §5.2 |
| `callConfigEquals` + logged changed snapshots drive the request | `buildRequest` derives from the last `LlmRequestHeader` | `28-L21`; §5.3 |
| the agent's provider/model selection drives the call | `AgentConfig::provider` → `LlmCallConfig::provider`; runtime default route when empty | `31-D7`; §5.3 |
| adapter registration is a runtime/boot concern | the daemon registers; the registry constructs none | `28 §3.3`; §4 |

---

## 12. Test plan

Deterministic, offline (`FakeLLM`, `include/ymh/llm/fake_llm.hpp`; spec `45`).
Wave 1 may only start once `28`/`29`/`30`/`31`/`32` are verified.

**Unit.**

1. **`AgentServices` shape.** A compile-time/static test asserts no
   `LLMProvider*`/`ProviderRegistry*`/`LLMProviderConfig` member and a single
   `LlmRuntime* runtime`; `pool` is present (`A19`).
2. **`AgentRegistry` constructs no adapter.** Constructing a registry with a null
   runtime does not call any provider factory and registers no agent; the
   `provider_`/`providerStorage_` members are gone (`A22`); the convenience ctor
   accepts `LlmRuntime&`.
3. **Dispatch path.** A `FakeLLM` registered in a `FakeRuntime` records exactly
   one `prepare_call` + one `stream` per step; the `LLMPool` slot is held for the
   whole call and released on success, failure, and cancel (`A12`, `A20`).
4. **One-shot.** A test that forces a second dispatch of the same `PreparedCall`
   observes `InvalidPreparedCall` normalized to `ProviderFailed` (`A-F20`).
5. **Compaction re-attempt.** A `FakeLLM` returning `ContextLengthExceeded` once
   then success yields **two** distinct `prepare_call`s with distinct
   `FrozenRequest`s (new step), never a re-dispatch (`A20`; `28 §6.3`).
6. **Null runtime.** No runtime → terminal `TurnFailed{ProviderFailed}` before
   any `prepare_call` (`A-F19`).
7. **Header timing.** `buildRequest` logs a header on the first dispatch and on
   any config/prompt/tool/purpose change, and **not** per dispatch on an
   unchanged template (`A21`; `28-L21`).
8. **Provider source.** `buildRequest` sets `LlmCallConfig::provider` from
   `AgentConfig::provider`; an empty value uses the runtime's registered default
   route; a named provider with no route yields the typed `A-F23` failure
   normalized to `ProviderFailed` (`A23`).

**Integration (FakeLLM).** A full turn through `WorkspaceRuntime` appends exactly
one `llm/request_header` per series; a resumed session with a logged header logs
none until a change (`A21`); a legacy session (no header) logs one and replays
the pre-header portion as legacy, never fabricated (`A-F22`).

**Callers to migrate (build-break list).** Every site that sets the removed
`AgentServices` provider fields moves to `services.runtime` (tests: a
`FakeRuntime`):
- production: `src/agent/agent_registry.cpp:17-31` (`make_services`), `:37-44`
  (fallback), `:111` (injection); `src/agent/workspace_runtime.cpp:165-167`
  (the three fields), `:170-173` (compactor construction).
- tests: `tests/support/agent_test_env.hpp:34` (`make_agent_services`), `:60`
  (`services.provider = &provider`), `:105` (registry construction);
  `tests/unit/agent_registry_test.cpp:132` (registry construction).

**Invariant coverage.** A19–A23 are added to the `06 §13.7` invariant matrix;
A-F19–A-F23 to the `06 §13.6` failure-mode matrix.

---

## 13. Open items / ownership gaps

1. **Retry executor wave unassigned.** Recorded in §10; not resolved here.
2. **`WorkspaceRuntime::provider()`/`provider_config()` accessors.** After Wave 1
   the daemon holds a runtime, not a `unique_ptr<LLMProvider>`
   (`include/ymh/agent/workspace_runtime.hpp:149,158`;
   `src/agent/workspace_runtime.cpp:198-200`). Whether these daemon-surface
   accessors are re-typed or removed is a `04`/daemon question, not a `06`-owned
   interface. Flagged, not pinned.
3. **`LlmRuntime` definition header path.** `28 §3.1` pins the interface but not
   the file. This errata requires `agent_loop.hpp` (and therefore
   `agent_loop.cpp`) to include the 08-owned runtime header — it must be a real
   include, not a forward declaration, because `held_config_` needs the complete
   `LlmCallConfig` (§5.3); the exact path is `08`-owned.
4. **`13` errata (`32`) owns the `ContextCompactor` ctor text.** Cited in §6;
   not amended here.
5. **`29` owns the `llm/request_header` codec/wire keys; `21` owns
   `session.persist_prompt_text`.** This errata consumes both, pins neither.
6. **`26-D10`/`D17`/`D21` remain future `06` errata** (`26p2:1092`); this errata
   deliberately pins only `D1`'s `06` surface.
7. **The provider-source pin depends on `28`'s amendment.** This errata consumes
   the `AgentConfig::provider` → `LlmCallConfig::provider` source, the
   default-route-when-empty rule, and the `A-F23` no-route failure; `28` (the
   `LlmCallConfig` owner) must carry that amendment before Wave-1 code
   (`31-D7`).

---

## 14. Revision log

| Rev | Change |
|---|---|
| 1 | Initial write. Pins the `06`-owned half of `26-D1` as a Wave-1 blocking prerequisite: the `AgentServices` provider seam (`providers`/`provider`/`provider_config` → `LlmRuntime* runtime`), the `AgentRegistry` ctor/fallback removal, the loop's `buildRequest`→`prepare_call`→`PreparedCall::stream` dispatch, invariants A19–A22, failure modes A-F19–A-F22, and the retry-executor ownership-gap record. All `file:line` re-derived against the tree. |
| 2 | Fixes the gate-31 MEDIUM and six LOWs. (M) Pins the provider-id source: new `AgentConfig::provider` → `LlmCallConfig::provider`, runtime default route when empty, loud typed `prepare_call` failure on a missing route (new `A23`/`A-F23`, `31-D7`), consuming `28`'s forthcoming amendment. (L1) `held_config_` needs the complete `LlmCallConfig`, so `agent_loop.hpp` includes the 08-owned runtime header. (L2) The last `LlmRequestHeader` is read by scanning `Session::events()`; `29` pins the event as projection-invisible. (L3) `§9 (L18–L25)` range corrected. (L4) `LLMPool::Slot` is RAII; no explicit `release()`. (L5) Test callers enumerated. (L6) No-route failure mode added. All changed `file:line` re-derived against the tree. |

---

## 15. Decisions (31-D1–31-D7)

- **31-D1** — `AgentServices` replaces `providers`/`provider`/`provider_config`
  with a single `LlmRuntime* runtime`; `pool` and every other field are
  retained; no `LLMProvider*` remains. (`26-D1`; `28 §3.4`; `A19`)
- **31-D2** — The `AgentRegistry` convenience ctor's `ProviderRegistry&`
  parameter becomes `LlmRuntime&`; the provider fallback and the
  `provider_`/`providerStorage_` members are removed; `registerAgent` no longer
  injects a provider. (`28 §3.4`; `A22`)
- **31-D3** — The loop dispatches via `prepare_call` → one
  `PreparedCall::stream`, bracketed by one `LLMPool` slot; the
  `ContextLengthExceeded` re-attempt is a new call/step, not a re-dispatch.
  (`26-D3`; `28 §6.3`, `28-L24`; `A20`)
- **31-D4** — `buildRequest` derives `LlmCallConfig` from the last logged
  `LlmRequestHeader` and logs a changed header before dispatch. (`26-D2`;
  `28 §5.3`; `A21`)
- **31-D5** — The daemon is the sole adapter-construction/registration site; the
  runtime owns the registered adapters; the agent layer owns none. (`28 §3.3`,
  `28-D2`)
- **31-D6** — This errata pins only `26-D1`'s `06`-owned surface; `26-D10`/
  `D17`/`D21` are deferred to their own errata before their waves. (`26p2:1092`)
- **31-D7** — `LlmCallConfig::provider` is sourced from a new
  `AgentConfig::provider` (config-driven, `to_agent_config` from
  `config.llm.provider`); when it is empty the runtime's registered default
  route is used; a missing route is a loud typed `prepare_call` failure
  (`A-F23`). `28` (amended) owns the pin; no competing source is defined.
  (`28` amended; `A23`)

---

## 16. References

- `docs/design/26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS): §4.2
  :139 (26-D1, `Brk. (06,08)`), §4.3.1 :172-291, §4.3.2 :293-390, §4.4
  :1087-1093 (the `06` row :1092), §4.8 :1257-1290, §4.9 :1291-1351, §5 Wave 1
  :1397-1422.
- `docs/design/28-llm-service-boundary-errata.md` (verified Rev 2; amended for
  the provider-source decision): §1.1 :93-120, §3.1 :154-296, §3.3 :319-341,
  §3.4 :342-366, §4 :370-456, §5 :457-540, §6 :541-591, §7 :592-638, §8 :639-664,
  §9 (L18–L25) :668-714, §10 (L-F19–L-F26) :717-728, §13 :793-823.
- `docs/design/30-architecture-cascade-errata.md` §5.1 :261-293 (the two Wave-1
  blocking prerequisites).
- `docs/design/06-agent-loop.md` (verified): §4.1 :428-466, §4.2 :468-508, §5.1
  :545-647, §5.2 :648-680, §5.3 :681-707, §5.7 :807-826, §5.9 :835-893, §10
  :996-1067, §11.2 :1094-1119, §13.6 :1253-1275, §13.7 :1276-1299, §14.1
  :1302-1361.
- `docs/design/13-context-compaction.md` (verified) §5.2; sibling errata `32`
  (compaction) owns the ctor text.
- `docs/design/29-event-family-errata.md` (verified): the `llm/request_header`
  codec/wire keys; §4.2 :306-310 (`LlmRequestHeader` is projection-invisible
  — `deriveMessages` ignores it, so the loop reads it via `Session::events()`).
- Tree anchors: `include/ymh/agent/agent_loop.hpp:31,42,44-70,53,64,65,140,158`;
  `src/agent/agent_loop.cpp:13,410-419,742,766-781,795-809`;
  `include/ymh/agent/agent_registry.hpp:20,21,29,30-36,95,96,97,98`;
  `src/agent/agent_registry.cpp:17-31,35-56,37-44,58-66,110-111`;
  `include/ymh/agent/workspace_runtime.hpp:149,158`;
  `src/agent/workspace_runtime.cpp:154-176,165-167,170-173,198-200,260-282,322-323`;
  `include/ymh/agent/compactor.hpp:113-117,140`; `src/agent/compactor.cpp:252`;
  `include/ymh/llm/llm_provider.hpp:38-47,51-59`;
  `include/ymh/llm/provider_registry.hpp:25-63`;
  `include/ymh/llm/llm_request.hpp:54-61`;
  `include/ymh/agent/agent.hpp:95-96,103`;
  `include/ymh/agent/llm_pool.hpp:23-38`; `include/ymh/session/session.hpp:244`;
  `src/cli/wiring.cpp:63,174-190`;
  `tests/support/agent_test_env.hpp:34,60,105`;
  `tests/unit/agent_registry_test.cpp:132`;
  `src/llm/openai_adapter.cpp:923-1000`.

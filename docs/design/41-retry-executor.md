# 41: Retry Executor (the durable step-boundary retry owner)

```
Status: Rev 1 written · verified: pending · reviewer: pending
Revision: Rev 1 (2026-09-19). Initial write. Owns the separate durable retry
          executor that `30-architecture-cascade-errata.md` §5.1 records as a
          named ownership gap, and that `28-llm-service-boundary-errata.md`
          §6.2/§13.2 and `31-agent-loop-errata.md` §10 record as an unassigned
          wave. This spec **pins the executor and its durable event record but
          does not activate it** (see §1.5). Activation timing stays an open
          user question, honoring `26` OQ4 (`26-dsh-alignment-part2.md`
          :1604-1607), whose recorded recommendation is "add logging in Wave 1;
          externalization is a later optional refactor". This spec does **not**
          promote that recommendation.
Component: 41 (owning spec): pins a new seam above the `LlmRuntime` boundary
           (28) and consumes the settlement predicate of 34; also owns the
           `LlmRetry`/`LlmRetryStarted` event codec that 29 §1.3/§8 left
           explicitly unowned.
Depends on: docs/design/26-dsh-alignment.md (verified Rev 7) §2.1.7 :400-436;
            docs/design/26-dsh-alignment-part2.md (verified Rev 7) §4.3.9.1
            :837-872, :946-951, §5 :1386-1501, §6 OQ4 :1604-1607;
            docs/design/28-llm-service-boundary-errata.md (verified Rev 3)
            §3.1, §6, §13.2, L18-L26;
            docs/design/29-event-family-errata.md (verified) §1.3, §3.1, §4.4,
            §8;
            docs/design/30-architecture-cascade-errata.md (verified Rev 2) §5.1,
            §5.3;
            docs/design/31-agent-loop-errata.md (verified) §5.2, §5.4, §10;
            docs/design/34-assembler-replay-errata.md (Rev 3 verified; Rev 4
            pending re-gate, §7 unchanged) §7;
            docs/design/08-llm-provider.md (verified) §3.7, L6, L7.
Scope: (1) the executor's placement at a durable step boundary; (2) the retry
       policy and backoff; (3) the `llm/retry` and `llm/retry_started` durable
       events and their codec; (4) the interaction with the per-attempt
       settlement predicate (34 §7) and the `LlmRuntime` boundary (28 §3.1);
       (5) the idempotency and no-double-call guarantees; (6) the failure modes,
       invariants, dsh mapping, decision register, and test plan; (7) the
       activation disposition of OQ4 as an open user question.
```

---

## 1. Purpose, authority, numbering

### 1.1 The recorded ownership gap

Three verified specs record the same gap without resolving it:

- `30-architecture-cascade-errata.md` §5.1 :295-302: "**Named ownership gap
  (recorded, not resolved): the retry executor has no wave.** `26-I3` and the
  `llm/retry`/`llm/retry-started` events require a separate durable retry
  executor, but `26 §5` Waves 1-6 contain **no retry-executor item**. This is a
  genuine ownership gap: no wave owns the executor."
- `28-llm-service-boundary-errata.md` §13.2 :945-948: "Retry executor wave
  unassigned. `26 §5` Waves 1-6 contain no retry executor item, yet `26-I3` and
  the `llm/retry`/`llm/retry-started` events (§4.3.9.1) require it."
- `31-agent-loop-errata.md` §10 :601-615: "An owning wave must be assigned by
  the lead before that executor is coded."

The gap has two halves. The **design** half is unowned: no spec describes the
executor, its boundary, its policy, or its durable record. The **event** half is
also unowned: `29-event-family-errata.md` §1.3 :82-87 and §8 :480-483 refuse to
freeze `LlmRetry`/`LlmRetryStarted` and state that "the Wave-1 owning errata must
either add them under §3.1 or state that retries stay unlogged; leaving them
silently unowned is a defect this errata refuses to introduce."

This spec closes both halves by **owning** them. It does not invent a wave
(`30 §5.1` :301-302 explicitly declined to invent one), and it does not activate
the executor (`26` OQ4).

### 1.2 What this spec pins

1. The executor's **placement**: a durable step boundary, inside one
   `StepStarted`/`StepEnded` pair (§3).
2. The **policy and backoff** source and math (§4).
3. The **durable record**: `EventType::LlmRetry` (`llm/retry`) and
   `EventType::LlmRetryStarted` (`llm/retry_started`), their C++ payloads, their
   JSON keys, and their codec obligations under `29 §3.1` (§5).
4. The **C++ interface sketches** (§6).
5. The **settlement interaction** (34 §7) and the **`LlmRuntime` interaction**
   (28 §3.1) (§7, §8).
6. The **idempotency and no-double-call guarantees** (§9).
7. The **failure modes**, **invariants**, **dsh mapping**, **dependency list**,
   **decision register**, and **test plan** (§10-§16).

### 1.3 In scope / out of scope

**In scope.** The executor seam, the retry events and their codec, the policy
decision function, the ordering against settlement, and the activation
disposition.

**Out of scope.**

- The adapter's in-adapter retry loop
  (`src/llm/openai_adapter.cpp:923-1000`). It is retained as the single retry
  owner while the executor is pinned (`28-L22`, `28 §6.2`). Removing it is
  co-scheduled with activation, not done here.
- The loop's `ContextLengthExceeded` compaction re-attempt
  (`src/agent/agent_loop.cpp:999-1004`; `31 §5.4`). It is a **new step/request**,
  not an executor retry (§3.3).
- The `LlmRequestHeader` codec and the freeze/digest contract (`28 §4`, `28 §5`).
  The executor consumes the `FrozenRequest`; it does not change it.
- The per-attempt assembler/accumulator lifetime and the settlement predicate
  (`34-D5`, `34-D6`). The executor drives attempts; 34 still settles them.
- Any config key. The policy is read from `PreparedCall::retry_policy()`
  (`28 §3.1` :280-301), which already carries `LLMProviderConfig::retry`
  (`include/ymh/llm/provider_registry.hpp:34`). No new key is introduced.
- Implementation code. This is a design artifact.

### 1.4 Numbering

Specs `31`-`40` are on disk (`ls docs/design`); `40-output-retention.md` is the
Wave-4 output-retention sibling (not a dependency of this spec). The task
assigns `41`. This file **claims number 41**. It supersedes nothing: it is the
first owner of the executor. It resolves the `29 §1.3`/§8 open item by taking
ownership of the two retry events, and it resolves the `30 §5.1`/`28 §13.2`/`31
§10` ownership gap by owning the executor design. It does **not** assign a wave
(§1.5, RE-D4).

### 1.5 Activation disposition (honoring OQ4)

`26-dsh-alignment-part2.md` §6 OQ4 :1604-1607 states:

> **Retry location.** The plan adds durable retry events and a step-boundary
> executor but does not immediately move the adapter's internal barrier. Open:
> fully externalize retry in Wave 1 or only add logging. **Recommendation: add
> logging in Wave 1; externalization is a later optional refactor.**

Two facts fix the disposition.

1. OQ4 is recorded as "decisions made on the user's behalf, stated so they can
   be reversed cheaply" (`26-dsh-alignment-part2.md` :1580-1581). It is a
   recommendation, not a user decision.
2. Wave 1 has landed **without** the logging-only path: `events.hpp` defines
   `AssistantMessage` (`:106-114`), `AssistantAttempt` (`:118-122`), and
   `LlmRequestHeader` (`:233`) but **no** `LlmRetry`/`LlmRetryStarted`, and the
   adapter still owns retry.

Therefore this spec's disposition is:

- **The executor is PINNED, not activated.** The full design is frozen here so a
  later wave can implement it without re-opening the seam. No runtime behavior
  changes.
- **The events are owned and pinned but NOT emitted while pinned.** Retries stay
  unlogged. This is the explicit "state that retries stay unlogged" branch that
  `29 §8` :481-482 requires, and it resolves the 29 open item by ownership.
- **Activation timing is an OPEN QUESTION (RE-OQ1), a user decision.** This spec
  does not decide between activating the executor, doing the logging-only path,
  or leaving retries unlogged. The recorded OQ4 recommendation is quoted, not
  adopted.

This is the only honest reading that neither silently promotes OQ4 nor invents a
wave. `30 §5.1` :301-302 ("records the gap rather than inventing a wave") and
`31 §10` :615 ("An owning wave must be assigned by the lead") are both honored.

---

## 2. Current state (verified against the tree)

Line anchors below were checked against the working tree at authoring time.
Where a prior spec's anchor has drifted, the correction is stated.

| Fact | Anchor | Note |
|---|---|---|
| The adapter owns retry | `src/llm/openai_adapter.cpp:923-1000` | the `for attempt ... max_attempts` loop |
| The pre-first-event barrier | `src/llm/openai_adapter.cpp:983` | `!decoder.any_event()`; 28 cites `stream.hpp:111`, which has drifted (see §19) |
| Retryable-code filter | `include/ymh/llm/stream.hpp:119-131` | `is_retryable_code`; 28 cites `stream.hpp:112-123`, drifted |
| Backoff math | `src/llm/openai_adapter.cpp:661-671` | `backoff_delay(policy, attempt, jitter_sample)` |
| Retry-After parse | `src/llm/openai_adapter.cpp:673-...` | `parse_retry_after` / `retry_after_from` |
| Cancellable sleep | `src/llm/openai_adapter.cpp:991-993` | `sleep_with_cancel` |
| `RetryPolicy` | `include/ymh/llm/llm_provider.hpp:39-47` | `max_attempts`, `base_delay`, `max_delay`, `jitter`, `honor_retry_after` |
| Policy source | `include/ymh/llm/provider_registry.hpp:34` (`LLMProviderConfig::retry`) → `PreparedCall::retry_policy()` `include/ymh/llm/llm_runtime.hpp:108` | captured at `register_adapter` (`28 §3.1`) |
| One-shot call | `include/ymh/llm/llm_runtime.hpp:100-124` | `PreparedCall`; `28-L24` |
| `prepare_call` / `stream` | `include/ymh/llm/llm_runtime.hpp:205-212` | route resolution and interceptor entry |
| Loop dispatch | `src/agent/agent_loop.cpp:936-997` | `prepare_call` then one `call.stream(...)`; settlement at `:981-997` |
| Compaction re-attempt | `src/agent/agent_loop.cpp:999-1004` | `ContextLengthExceeded`; a new step, not an executor retry |
| Settlement predicate | `docs/design/34-assembler-replay-errata.md:437-488` | `Completed → AssistantMessage`, else `AssistantAttempt` |
| No retry events in the tree | `include/ymh/session/events.hpp` (no `LlmRetry`) | the codec is unbuilt |

The loop's current dispatch is a single attempt per `PreparedCall`
(`agent_loop.cpp:938-942`) with the compaction re-attempt as the only second
dispatch (`:999-1004`). There is no step-boundary retry layer.

---

## 3. Placement: the durable step boundary

### 3.1 The step model

A turn is a sequence of steps; each step is bracketed by `StepStarted` and
`StepEnded` (`src/agent/agent_loop.cpp:755-756`, `:1055`, `:1072`). The request
for a step is built by `AgentLoop::buildRequest` (`:486-...`) and dispatched
under a `PreparedCall` (`:938-942`).

The executor sits **inside** one open step, between `StepStarted` and
`StepEnded`, wrapping the dispatch. Every provider attempt it makes carries the
**same** `(turn, step)` as the enclosing step. Retries do not open a new step and
do not append a second `StepStarted`.

This is the "durable step boundary" of `26-I3` :59-61 and
`26-dsh-alignment.md:405-406` ("`@deepseek-ai/dsh-llm-retry` executes the policy
at durable agent-step boundaries"). The boundary is the point at which the step's
durable state is known: the settled attempts so far, the request header, and the
retry record. The decision is derived from the log, not from in-memory drift
(`26-dsh-alignment.md:494-496`).

### 3.2 Why not the adapter

The adapter is a leaf: it never touches the session log, store, registry, or
`EventBus` (`include/ymh/llm/llm_provider.hpp:3-5`, L1). A durable retry record
requires an append, which a leaf cannot make. `26-I3` therefore keeps
`LLMProvider::stream` as exactly one attempt and moves retry to the step
boundary. The executor is a session-scoped collaborator, not a provider concern.

### 3.3 The executor retry vs the compaction re-attempt

These are distinct and must not be conflated:

| | Executor retry | Compaction re-attempt |
|---|---|---|
| Trigger | retryable pre-first-event failure (`RateLimited`, `ServerError`, `NetworkError`, `Timeout`, `MalformedResponse`) | `ContextLengthExceeded` |
| Step | same `(turn, step)` | new `(turn, step)` |
| Request | same `FrozenRequest` | new `FrozenRequest` after compaction |
| Record | `llm/retry` + `llm/retry_started` | new `LlmRequestHeader` (if config changed) |
| Owner | this spec (pinned) | `31 §5.4` :503-512, retained |

`ContextLengthExceeded` is **non-retryable** for the executor
(`include/ymh/llm/stream.hpp:119-131` excludes it; `08 §3.7` :471-473 lists it as
non-retryable). The loop's handling of it is a new step, not an executor retry
(RE-I12).

---

## 4. The retry policy and backoff

### 4.1 Policy source

The executor has **no config of its own** (`26-dsh-alignment.md:436`,
`dsh-llm-retry/lib/types/index.d.ts:14`). It reads the policy bound to the call:

```
LLMProviderConfig::retry                    (provider_registry.hpp:34)
  -> adapter->retry_policy()                (captured at register_adapter; 28 §3.1)
  -> PreparedCall::retry_policy()           (llm_runtime.hpp:108)
  -> RetryExecutor
```

The policy is captured per attempt at `prepare_call` time, so a route or
adapter-generation change between attempts is reflected in the next attempt's
policy. The executor never mutates the policy.

### 4.2 Decision function

The decision is pure and side-effect free:

```
decide_retry(response, policy, any_event_dispatched, retries_so_far, retry_id)
  -> RetryDecision
```

Rules:

1. `any_event_dispatched == true` → no retry (L7; `08 §3.7` :474-482; 26-F4).
2. `response.outcome != Failed` → no retry.
3. `!is_retryable_code(response.error.code)` → no retry
   (`include/ymh/llm/stream.hpp:119-131`).
4. `retries_so_far + 1 >= policy.max_attempts` → no retry (budget bound). The
   first attempt counts as one, so `max_attempts = 3` allows at most two
   retries.
5. Otherwise retry, with `retry_index = retries_so_far + 1`.

The function is the only place the retry decision is made; the executor loop is
a thin driver over it. This makes the whole policy table-testable without I/O.

### 4.3 Backoff schedule

The delay reuses the adapter's math (`src/llm/openai_adapter.cpp:661-671`):

```
exponent = retry_index - 1
base     = base_delay * 2^exponent
capped   = min(base, max_delay)
factor   = 1 + jitter * (2 * clamp(jitter_sample, 0, 1) - 1)
delay    = max(0, capped * factor)
```

`jitter_sample` is a `[0, 1]` draw from an injectable source. The adapter uses a
process-local `next_jitter_sample()`; the executor takes a
`JitterSource` seam so tests are deterministic (RE-D8, RE-F13).

For `RateLimited` with `honor_retry_after` set, the delay is overridden by a
valid `Retry-After` (`08 §3.7` :485-486), exactly as the adapter does at
`openai_adapter.cpp:986-989`. An absent or invalid header falls back to the
backoff (RE-F18).

The wait is cancellable (`08-L6`, `08 §3.7` :483-484). A cancel during the wait
aborts immediately and does not start the next attempt.

### 4.4 Mode mapping (`normal` vs `always`)

`26` pins a `mode`-discriminated union (`26-dsh-alignment-part2.md:856-870`):
`Mode::Normal` carries `max_retries`, `Mode::Always` does not.

ymh's `RetryPolicy` (`include/ymh/llm/llm_provider.hpp:41-47`) is effectively
`normal`: it always has `max_attempts`. Therefore:

- The executor emits `Mode::Normal` with
  `max_retries = max_attempts - 1` (RE-D11).
- `Mode::Always` is a **reserved** value with no producer in ymh today. It must
  still round-trip through the codec, and the family test must cover it. Whether
  ymh ever exposes an "always retry" policy is a product question (RE-OQ2).

This keeps the pinned union honest without inventing a config surface.

---

## 5. The durable retry record (events)

### 5.1 Event inventory

Two new durable `EventType` values, owned here:

| EventType | `wire_name` | Payload | Durability | Source |
|---|---|---|---|---|
| `LlmRetry` | `llm/retry` | `payload::LlmRetry` | durable | 26 §4.3.9.1 :840, :950 |
| `LlmRetryStarted` | `llm/retry_started` | `payload::LlmRetryStarted` | durable | 26 §4.3.9.1 :841, :951 |

Both are `deriveMessages`-ignored, token-excluded, and optional status-only in
the UI (`26-dsh-alignment-part2.md:987-988`; `29 §4.4` :387-388). The wire names
follow ymh's slash/underscore convention, not dsh's hyphenated names; the
divergence is deliberate and pinned (`26-dsh-alignment-part2.md:832-835`).

The payloads and keys are reproduced from `26` and are not re-invented here.

### 5.2 `llm/retry`

```
Durable, non-surface record of one provider-routed retry scheduled after a
failed request attempt. Written BEFORE the backoff wait.
```

C++ payload (`26-dsh-alignment-part2.md:856-870`):

```cpp
namespace ymh::payload {

using RetryId = std::uint64_t;   // dsh RetryId

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

} // namespace ymh::payload
```

JSON keys (`26-dsh-alignment-part2.md:950`): `retry_id` int, `turn` int,
`step` int, `provider` string, `mode` string (`"normal"` or `"always"`),
`policy_key` string, `retry` int, `max_retries` int (only when `mode=="normal"`),
`delay_ms` int, `failure` `LLMError` object (the existing spec-08 codec,
`include/ymh/llm/stream.hpp:330`, `:345`).

`failure` is the settled attempt's terminal `LLMError`
(`LLMResponse::error`, `include/ymh/llm/llm_provider.hpp:56`). It is the actual
error that triggered the retry, never a synthesized one (RE-I17, RE-F15).

`policy_key` names the policy in force. ymh has no policy registry; the pinned
value is a deterministic function of the policy fields (for example a short
digest or a canonical `"normal/<max_attempts>/<base_ms>/<max_ms>/<jitter>"`
string). The exact spelling is an implementation detail of the activation wave,
but it must be deterministic and stable across a resume (RE-I18).

### 5.3 `llm/retry_started`

```
Durable transition written AFTER a retry wait succeeds and BEFORE the next
request attempt starts.
```

C++ payload (`26-dsh-alignment-part2.md:872`):

```cpp
struct LlmRetryStarted {
    RetryId       retry_id = 0;
    TurnId        turn = 0;
    StepId        step = 0;
    std::uint32_t retry = 0;
};
```

JSON keys (`26-dsh-alignment-part2.md:951`): `retry_id` int, `turn` int,
`step` int, `retry` int.

`retry_id` matches the preceding `llm/retry`; `retry` matches its `retry` field.
A wait that is cancelled or that throws before completing writes **no**
`llm/retry_started` (RE-I6, RE-F4).

### 5.4 Codec obligations (`29 §3.1`)

Adding the two types must satisfy the six-point checklist of
`29-event-family-errata.md:115-146`:

1. `EventType` enum value.
2. `wire_name` entry in `kWireNames` (`llm/retry`, `llm/retry_started`).
3. `parse_event_type` inverse.
4. `SessionEventMap<...>` specialisation.
5. `EventTraits<payload::...>` specialisation.
6. `to_json`/`from_json` bodies (hand-written, `src/session/events.cpp`).

Plus the `29` rules: an explicit no-`default:` case in the projection switch
(`29-I2`), an `all_event_types()` entry (`29-I1`), and version neutrality
(`29-I6`: neither `kProtocolVersion` nor `kSchemaVersion` changes).

### 5.5 Consumer and projection obligations

- `deriveMessages` ignores both events (`29 §4.4` :387-388). They add no
  model-visible history.
- The token estimate excludes both (`29-I5`; `26-dsh-alignment-part2.md:987-988`).
- The transcript/UI may show an optional status-only retry notice; it must not
  render a message row. This matches the "optional retry notice (status only)"
  row in `29 §4.4`.
- The durable decode stays loud on an unknown type (`CorruptionError`); the wire
  decode skips an unknown type with a cursor advance (`29 §3.3`, Axis A/B). The
  two axes never share a decode (`29-I3`).

### 5.6 Ordering with settlement

For one attempt that fails and is retried, the durable order is:

```
StepStarted (once, enclosing)
  attempt N:   stream -> flush -> settle(AssistantAttempt)        (34-D6)
  llm/retry                                                        (before wait)
  wait (cancellable)
  llm/retry_started                                                (after wait)
  attempt N+1: stream -> flush -> settle(...)
  ...
  final attempt: stream -> flush -> settle(AssistantMessage | AssistantAttempt)
StepEnded (once, enclosing)
```

The failed attempt is settled **before** `llm/retry` is appended, so the record
of what was attempted precedes the record of the retry (RE-I4). The final attempt
settles normally per 34 §7.1. The retry events are metadata; they are **not**
settlements, and a step still emits exactly one settlement per provider attempt
(RE-I5).

---

## 6. C++ interface sketches

All sketches are design artifacts. No implementation code is written here.

### 6.1 `RetryId` allocation

`RetryId` is a session-scoped monotonic counter (`payload::RetryId`,
`std::uint64_t`). It is allocated before the `llm/retry` append. On resume it is
initialized from the log as `max(retry_id seen) + 1`; it is never reused
(RE-I10, RE-F7).

### 6.2 `RetryDecision` and `decide_retry`

```cpp
namespace ymh {

// The pure decision for one failed attempt. No I/O, no clock, no session.
struct RetryDecision {
    bool                      retry = false;
    std::uint32_t             retry_index = 0;        // 1-based; the retry being scheduled
    payload::LlmRetry::Mode   mode = payload::LlmRetry::Mode::Normal;
    std::string               policy_key;
    std::uint32_t             max_retries = 0;        // Mode::Normal only
    std::chrono::milliseconds delay{0};
    RetryId                   retry_id = 0;
};

// 26-I3 / 08-L7. `any_event_dispatched` is the L7 barrier; `retries_so_far`
// is the count already scheduled in this step.
[[nodiscard]] RetryDecision decide_retry(const LLMResponse& response,
                                         const RetryPolicy& policy,
                                         bool               any_event_dispatched,
                                         std::uint32_t      retries_so_far,
                                         RetryId            retry_id,
                                         double             jitter_sample);

} // namespace ymh
```

### 6.3 `AttemptHooks` (the 34 seam)

The executor drives attempts; the loop keeps per-attempt assembly and settlement
(34-D5/D6). The loop supplies hooks so 34's contract is unchanged.

```cpp
namespace ymh {

// One dispatched provider attempt's lifecycle, owned by the caller (the loop),
// so 34-D5 (fresh assembler/accumulator/coalescer) and 34-D6 (settle exactly
// one durable event per attempt) stay in the loop.
struct AttemptHooks {
    // Build a fresh sink for this attempt. The loop closes over a fresh
    // assembler/accumulator/coalescer and the attempt's stream epoch (34 §5.1).
    std::function<StreamSink()> make_sink;

    // Settle this attempt after `stream` returns (34-D6): AssistantMessage on
    // Completed, AssistantAttempt otherwise. Must append exactly one event.
    // `any_event` reports whether the sink saw a dispatched StreamEvent.
    std::function<void(const LLMResponse& response, bool any_event)> settle;
};

} // namespace ymh
```

`settle` is invoked once per **dispatched** attempt. A `prepare_call` failure
that produced no dispatch does not settle (34 §7.3 :490-511;
`src/agent/agent_loop.cpp:966-968`; RE-I5, RE-F10).

### 6.4 `RetryExecutor`

```cpp
namespace ymh {

// The durable retry executor (26-I3). A step-boundary collaborator, not an
// adapter concern and not a provider. PINNED, not activated (RE-D2): while
// pinned this class is not constructed by any production path.
class RetryExecutor {
public:
    RetryExecutor(LlmRuntime&   runtime,
                  Session&      session,
                  JitterSource  jitter,
                  RetrySleeper  sleeper);

    // Runs one logical step to a terminal result. Retries stay inside the
    // caller's open (turn, step). Each dispatched attempt is settled by
    // `hooks.settle` exactly once. Before each wait `llm/retry` is appended;
    // after the wait and before the next attempt `llm/retry_started` is
    // appended (26-I3).
    [[nodiscard]] Task<StepCallResult> run(FrozenRequest      frozen,
                                           const AttemptHooks& hooks,
                                           TurnId              turn,
                                           StepId              step,
                                           CancellationToken   cancel);

private:
    LlmRuntime&  runtime_;
    Session&     session_;
    JitterSource jitter_;
    RetrySleeper sleeper_;
    RetryId      next_retry_id_ = 1;   // re-seeded from the log on resume
};

} // namespace ymh
```

Seams:

```cpp
// Deterministic in tests; a real source in production (RE-D8).
using JitterSource = std::function<double()>;

// Cancellable wait. Returns false iff cancelled (08-L6). The default sleeps on
// a steady_clock in bounded slices; tests supply a fake that returns at once.
using RetrySleeper = std::function<bool(std::chrono::milliseconds,
                                        CancellationToken)>;
```

### 6.5 `StepCallResult`

```cpp
struct StepCallResult {
    LLMResponse   response;    // the terminal attempt's response
    std::uint32_t attempts = 0; // 1 + retries
    std::uint32_t retries  = 0;
};
```

### 6.6 Pinned target wiring (for the activation wave)

Today the loop calls `prepare_call` then one `stream`
(`src/agent/agent_loop.cpp:938-942`). The pinned target, **when activated**, is:

```
runStep(runtime, session, frozen, hooks, turn, step, cancel):
    retries := 0
    loop:
        call := runtime->prepare_call(frozen.config(), cancel).get()
        sink := hooks.make_sink()
        any_event := false            # set by the sink wrapper
        response := call.stream(frozen, sink, cancel).get()   # one attempt
        hooks.settle(response, any_event)                     # 34-D6
        decision := decide_retry(response, call.retry_policy(),
                                 any_event, retries, next_retry_id_, jitter())
        if !decision.retry:
            return StepCallResult{response, retries + 1, retries}
        session.append(payload::LlmRetry{... decision ...})   # before wait
        if !sleeper(decision.delay, cancel):
            return StepCallResult{cancelled_response, retries + 1, retries}
        session.append(payload::LlmRetryStarted{...})          # after wait
        retries += 1
```

`frozen` is passed by value and copied per attempt; `PreparedCall` is re-created
per attempt because it is one-shot (`28-L24`). The loop's compaction re-attempt
stays outside this loop (`31 §5.4`).

---

## 7. Interaction with the per-attempt settlement predicate (34)

`34 §7.1` :441-462 pins: settlement is per provider attempt, evaluated when
`call.stream(...)` returns; `Completed → AssistantMessage`, else
`AssistantAttempt`; exactly one durable event per settled attempt.

The executor preserves this unchanged:

- Each provider attempt gets a **fresh** assembler/accumulator/coalescer
  (34-D5, `34 §6` :363-436) via `hooks.make_sink`.
- Each dispatched attempt is settled exactly once via `hooks.settle`.
- A retried attempt settles as `AssistantAttempt` **before** `llm/retry` is
  appended. This is the "retried" case named in `26 §4.3.4 :604-606` and
  `34 §7.2` :472-488.
- The final attempt settles normally: `AssistantMessage` iff it completed.
- A step therefore emits one settlement per attempt; exactly one of them is an
  `AssistantMessage` iff the final attempt completed.

The executor does **not** own settlement and does **not** call `deriveMessages`.
The retry events are invisible to the projection. This keeps the 34 seam intact
and is the reason `AttemptHooks` exists rather than the executor owning the
assembler.

---

## 8. Interaction with the `LlmRuntime` boundary (28)

`28 §3.1` pins `LlmRuntime`/`PreparedCall`/`FrozenRequest`; `28-L18` pins the
single service boundary; `28-L23` pins a terminal `LLMResponse` per call;
`28-L24` pins `PreparedCall` as one-shot.

The executor consumes this boundary and changes none of it:

- It holds `LlmRuntime&`, never an `LLMProvider*` (L18).
- It dispatches only through `prepare_call` + `PreparedCall::stream`. It never
  reaches an adapter directly.
- It reads the policy from `PreparedCall::retry_policy()` (`llm_runtime.hpp:108`),
  which `register_adapter` captured from `adapter->retry_policy()` (`28 §3.1`
  :280-301). No new `RetryPolicy` parameter is added to any seam.
- It creates a **new** `PreparedCall` per attempt, honoring one-shot (L24). It
  never re-dispatches a consumed `PreparedCall`.
- It treats the returned `LLMResponse` as terminal (L23). Adapter throws and
  non-terminal returns are already normalized by `PreparedCall::stream`.
- It does not mutate the `FrozenRequest` (L19). The request is copied per
  attempt; freeze stays before dispatch.

`NoProviderRouteError` (`llm_runtime.hpp:91-95`) is raised by `prepare_call`
before any `PreparedCall` exists and before any dispatch. It is pre-dispatch and
non-retryable: the executor emits no retry event for it (RE-F10). A
`PreparedCallError` (`llm_runtime.hpp:77-86`) is a misuse, not a transient
failure, and is likewise not retried.

---

## 9. Idempotency and no-double-call guarantees

1. **Exactly one retry owner.** While pinned, the adapter's pre-first-event loop
   is the only owner. On activation, removing the adapter loop and enabling the
   executor are the same change (`28-L22`, `28 §6.2` :660-674). Both active is
   the `L-F23` double-retry defect (RE-I1, RE-F1).
2. **One-shot dispatch.** Each attempt gets a fresh `PreparedCall`; no
   `PreparedCall` is dispatched twice (`28-L24`; RE-I7, RE-F9).
3. **One retry record per scheduled retry.** `llm/retry` is appended exactly
   once for each `decide_retry` that returns `retry == true` (RE-I6, RE-F5).
4. **One started record per completed wait.** `llm/retry_started` is appended
   exactly once after a wait that returns success, before the next attempt
   (RE-I4, RE-F14).
5. **No retry after a dispatched event.** The L7 barrier is checked before any
   retry record is written (`08-L7`; RE-I3, RE-F2).
6. **No partial durable output.** The pre-first-event barrier means a retried
   attempt produced no durable `AssistantChunk`/`AssistantMessage`; its only
   durable trace is the `AssistantAttempt` settlement. Re-issuing the attempt is
   therefore safe (RE-I15, RE-F4).
7. **Crash between `llm/retry` and `llm/retry_started`.** The retry is recorded
   as scheduled but not started. On resume the step may re-issue the attempt; the
   log already shows the intent, and no started marker claims it ran. This is
   at-least-once attempt semantics with no double durable output (RE-I15).
8. **Replay never re-runs.** A replay reads the retry events from the log; it
   never constructs the executor (RE-I16, RE-F20).
9. **RetryId uniqueness.** Session-scoped monotonic, never reused; on resume it
   is seeded from the log (RE-I10, RE-F7).

---

## 10. Failure modes (RE-F1-RE-F20)

| ID | Failure | Guard |
|---|---|---|
| **RE-F1** | Double retry: adapter loop and executor both active | `28-L22`; activation removes the adapter loop atomically; a startup assertion fails if both are enabled (RE-I1) |
| **RE-F2** | Retry after the first dispatched event | L7 barrier in `decide_retry`; the failure is terminal regardless of code (`08-L7`, 26-F4) |
| **RE-F3** | Non-retryable code retried | `is_retryable_code` (`stream.hpp:119-131`) gates the decision |
| **RE-F4** | `llm/retry` without `llm/retry_started` on a non-cancel path | The wait is cancellable and the only non-started path is cancel; a throw after `llm/retry` is normalized; resume treats an unmatched `llm/retry` as scheduled-not-started |
| **RE-F5** | Duplicate `llm/retry` for one retry | Exactly one append per `retry == true` decision; a counting test asserts it |
| **RE-F6** | Unbounded retry budget | `max_attempts` bound in `decide_retry`; the terminal failure is returned when exhausted |
| **RE-F7** | `RetryId` collision or reuse | Session-scoped monotonic allocation seeded from the log (RE-I10) |
| **RE-F8** | Unbounded or uncancellable wait | `RetrySleeper` is cancellable; cancel aborts the wait (`08-L6`) |
| **RE-F9** | Re-dispatch on a consumed `PreparedCall` | Fresh `PreparedCall` per attempt; one-shot (`28-L24`) |
| **RE-F10** | Retry event for a pre-dispatch route failure | `NoProviderRouteError` is pre-dispatch and non-retryable; no retry record |
| **RE-F11** | Retry crosses a step boundary | The executor is step-scoped; `(turn, step)` is fixed for the whole run |
| **RE-F12** | Settlement skipped or doubled per attempt | `hooks.settle` exactly once per dispatched attempt; retry events are not settlements (RE-I5) |
| **RE-F13** | Non-deterministic backoff in tests | Injectable `JitterSource` and `RetrySleeper` (RE-D8) |
| **RE-F14** | `llm/retry_started` written before the wait completes | Append order is pinned: after the wait, before the next attempt |
| **RE-F15** | `failure` is not the triggering error | Payload built from the settled attempt's `LLMResponse::error` (RE-I17) |
| **RE-F16** | Activation without a codec | `29 §3.1` six-point checklist; `all_event_types()` round-trip (`29-I1`) |
| **RE-F17** | Policy changes between attempts | The policy is re-read from the fresh `PreparedCall`; the change is reflected, never cached |
| **RE-F18** | `Retry-After` parsed when absent or invalid | Falls back to the backoff (`openai_adapter.cpp:986-989` pattern) |
| **RE-F19** | Delay exceeds `max_delay` | `capped = min(base, max_delay)` before jitter |
| **RE-F20** | Executor runs during replay | Replay reads events; the executor is never constructed on the replay path (RE-I16) |

---

## 11. Invariants (RE-I1-RE-I20)

- **RE-I1: One retry owner.** At any time exactly one of {adapter loop,
  executor} is the retry owner. While pinned the adapter is; on activation the
  adapter loop is removed in the same change (`28-L22`).
- **RE-I2: Retry is step-scoped.** All attempts of one executor run carry the
  same `(turn, step)`; retries open no new step. Contrast the compaction
  re-attempt (`31 §5.4`).
- **RE-I3: L7 barrier.** No retry after any `StreamEvent` is dispatched,
  including `UsageEvent` (`08-L7`, 26-F4).
- **RE-I4: Durable before wait, started after wait.** `llm/retry` precedes the
  wait; `llm/retry_started` follows a successful wait and precedes the next
  attempt.
- **RE-I5: One settlement per attempt.** 34-D6 is unchanged; the executor never
  settles. The retry events are metadata.
- **RE-I6: Exactly one record each.** One `llm/retry` per scheduled retry; one
  `llm/retry_started` per completed wait.
- **RE-I7: One-shot dispatch.** A fresh `PreparedCall` per attempt; no
  re-dispatch (`28-L24`).
- **RE-I8: Bounded budget.** Retries are bounded by `policy.max_attempts`.
- **RE-I9: Cancellable wait.** A cancel during the wait aborts immediately and
  writes no `llm/retry_started`.
- **RE-I10: `RetryId` uniqueness.** Session-scoped, monotonic, never reused,
  log-seeded on resume.
- **RE-I11: Non-retryable codes never retried.** `is_retryable_code` is the
  filter.
- **RE-I12: `ContextLengthExceeded` is not an executor retry.** It is the
  loop's new-step compaction re-attempt (`31 §5.4`).
- **RE-I13: Metadata only.** Both events are `deriveMessages`-ignored and
  token-excluded; the UI shows at most an optional status-only notice.
- **RE-I14: Version neutrality.** Neither `kSchemaVersion` nor
  `kProtocolVersion` changes (`29-I6`).
- **RE-I15: Pinned means inert.** While pinned, the executor is not constructed
  by any production path and no retry event is emitted; retries stay unlogged.
  This resolves `29 §1.3`/§8 by ownership.
- **RE-I16: Replay reads, never re-runs.** The executor is absent from the
  replay path.
- **RE-I17: `failure` is the triggering error.** The payload carries the
  settled attempt's terminal `LLMError`.
- **RE-I18: Deterministic `policy_key`.** The `policy_key` is a deterministic
  function of the policy fields, stable across a resume.
- **RE-I19: No new config key.** The policy flows from the existing
  `llm.default.retry` chain; no config surface is added.
- **RE-I20: Frozen request immutability.** The executor copies the
  `FrozenRequest`; it never mutates it (`28-L19`).

---

## 12. dsh mapping

| dsh concept | ymh after this spec | Reference |
|---|---|---|
| `@deepseek-ai/dsh-llm-retry` executes the policy at durable step boundaries | `ymh::RetryExecutor`, step-scoped (pinned) | `26-dsh-alignment.md:400-406`; §3 |
| `RetryPolicyConfig` (`normal`/`always`) | `RetryPolicy` + `payload::LlmRetry::Mode`; `normal` only today | §4.4; RE-D11 |
| `retryPolicy` owned by provider registration, not the executor | `PreparedCall::retry_policy()` captured at `register_adapter` | `28 §3.1` :280-301 |
| `llm/retry` ("Durable, non-surface record of one provider-routed retry scheduled after a failed request attempt") | `EventType::LlmRetry` / `llm/retry` | `26-dsh-alignment.md:418-421`; §5.2 |
| `llm/retry-started` ("written after a retry wait succeeds and before the next request attempt starts") | `EventType::LlmRetryStarted` / `llm/retry_started` | `26-dsh-alignment.md:420-422`; §5.3 |
| `LlmRetryEventData` mode-discriminated union | `payload::LlmRetry` | `26-dsh-alignment-part2.md:856-870` |
| `RetryId` | session-scoped monotonic `RetryId` | §6.1; RE-I10 |
| one provider attempt per stream; no retry after the first event | `PreparedCall::stream` one attempt; L7 barrier retained | `26-I3`; `08-L7` |
| "has no config; providers own `retryPolicy`" | the executor adds no config; the policy is bound to the call | `26-dsh-alignment.md:436`; RE-I19 |

---

## 13. Dependency list

**Hard dependencies (must be verified before activation).**

- `28-llm-service-boundary-errata.md` (verified): the `LlmRuntime`/`PreparedCall`/
  `FrozenRequest` boundary, `PreparedCall::retry_policy()`, L18-L26.
- `29-event-family-errata.md` (verified): the codec checklist (§3.1), the
  two-axis compatibility rule (§3.3), the consumer matrix (§4.2/§4.4).
- `34-assembler-replay-errata.md` (Rev 3 verified; Rev 4 pending re-gate, §7
  unchanged): the settlement predicate (§7) and the per-attempt assembler
  lifetime (§6).
- `08-llm-provider.md` (verified): `RetryPolicy` (§3.7), L6, L7.
- `31-agent-loop-errata.md` (verified): the dispatch path (§5.2) and the
  compaction re-attempt (§5.4).

**Ownership hand-offs.**

- `29 §1.3`/§8: this spec takes ownership of `LlmRetry`/`LlmRetryStarted`.
- `30 §5.1` / `28 §13.2` / `31 §10`: this spec owns the executor design. The
  **wave** remains unassigned (RE-D4, RE-OQ1).

**Not dependencies.**

- The prompt registry (`36`). The executor consumes a `FrozenRequest`, not a
  prompt source.
- The output-retention spec. The executor does not own output retention.
- Config specs. No new key (RE-I19).

---

## 14. Decision register (RE-D1-RE-D12)

| ID | Decision | Class | Rationale |
|---|---|---|---|
| **RE-D1** | Spec 41 owns the executor design and the `LlmRetry`/`LlmRetryStarted` codec | design | resolves `29 §1.3`/§8 and the `30 §5.1` gap without inventing a wave |
| **RE-D2** | The executor is **pinned, not activated** | design | honors `26` OQ4 :1604-1607 |
| **RE-D3** | The adapter remains the single retry owner while pinned | design | `28-L22`; `28 §6.2` :660-674 |
| **RE-D4** | Activation timing is an **open user question** | open | OQ4 is a recommendation, not a decision; `31 §10` :615 requires the lead to assign a wave |
| **RE-D5** | While pinned, retries stay unlogged (no event emission) | design | the explicit branch `29 §8` :481-482 requires |
| **RE-D6** | Retry is step-scoped; `ContextLengthExceeded` stays a new step | design | `31 §5.4`; §3.3 |
| **RE-D7** | Policy source is `PreparedCall::retry_policy()`; no new config key | design | `28 §3.1` :280-301; RE-I19 |
| **RE-D8** | Backoff reuses `backoff_delay`; `JitterSource` and `RetrySleeper` are injectable | design | determinism (RE-F13); `08 §3.7` |
| **RE-D9** | `RetryId` is session-scoped monotonic, log-seeded on resume | design | correlation across `llm/retry` and `llm/retry_started` |
| **RE-D10** | Settlement stays 34-D6; the executor does not settle | design | preserves the 34 seam; `AttemptHooks` |
| **RE-D11** | Emit `Mode::Normal` with `max_retries = max_attempts - 1`; `Mode::Always` is reserved with no producer | design | ymh's `RetryPolicy` is `normal`-shaped |
| **RE-D12** | `failure` is the settled attempt's terminal `LLMError`; `policy_key` is deterministic | design | RE-I17, RE-I18 |

---

## 15. Open questions

- **RE-OQ1 (the OQ4 disposition).** Activation timing. `26` OQ4 :1604-1607
  leaves open whether to fully externalize retry or only add logging, with the
  recommendation "add logging in Wave 1; externalization is a later optional
  refactor". Wave 1 landed without either. The user must choose:
  (a) activate the executor in a later wave (remove the adapter loop in the same
  change, `28-L22`);
  (b) do the logging-only path (emit `llm/retry`/`llm/retry_started` while the
  adapter still owns retry, which requires an L7-safe logging tap and a decision
  on who emits it);
  (c) leave retries unlogged (the pinned state).
  This spec does not decide. It records the options and honors OQ4's recorded
  status.

- **RE-OQ2 (`Mode::Always`).** ymh's `RetryPolicy` has no "always" shape, so no
  producer emits `Mode::Always`. Whether to add an "always retry" policy is a
  product decision; until then the value is reserved and round-tripped only.
  Recommendation: do not add it; keep the reserved value.

- **RE-OQ3 (`policy_key` spelling).** The exact deterministic spelling of
  `policy_key` is deferred to the activation wave. Recommendation: a short
  canonical string over the policy fields, or a digest, pinned then.

---

## 16. Test plan

Deterministic and offline (`FakeLLM`, `include/ymh/llm/fake_llm.hpp`; spec 45).
This spec is a design artifact, so its **verification** is a gate review (§17).
The tests below are the acceptance criteria for the activation wave and, where
noted, for this spec's codec freeze.

**Codec and family (gate-now, can be written before activation).**

1. **Round-trip.** `LlmRetry` and `LlmRetryStarted` round-trip through
   `to_json`/`from_json` for every field, including both `mode` variants and the
   `max_retries` presence/absence rule.
2. **Family totality.** `all_event_types()` includes both new values;
   `parse_event_type(wire_name(t)) == t` for both (`29-I1`).
3. **Projection.** `deriveMessages` ignores both; the token estimate excludes
   both (`29-I5`).
4. **Version neutrality.** Neither `kSchemaVersion` nor `kProtocolVersion`
   changes (`29-I6`).
5. **Wire skip.** An old binary skips `llm/retry`/`llm/retry_started` with a
   cursor advance; the durable decode stays loud (`29 §3.3`).

**Decision and backoff (unit, activation).**

6. **Retryable table.** `RateLimited`, `ServerError`, `NetworkError`, `Timeout`,
   `MalformedResponse` retry; `Auth`, `ConfigError`, `BadRequest`,
   `ContextLengthExceeded`, `ContentFiltered`, `UnsupportedModel`,
   `MalformedToolCall`, `ProviderInternal`, `Cancelled` do not.
7. **L7 barrier.** With `any_event_dispatched == true`, no code retries.
8. **Budget bound.** `max_attempts = 3` yields at most two retries.
9. **Backoff math.** Delay equals the `backoff_delay` formula for a fixed
   `jitter_sample`; `max_delay` clamps.
10. **`Retry-After`.** `RateLimited` with a valid header overrides the backoff;
    an invalid/absent header falls back.

**Ordering and idempotency (integration with `FakeLLM`, activation).**

11. **Event order.** A scripted provider fails twice then succeeds; the durable
    order is `AssistantAttempt, llm/retry, llm/retry_started` twice, then
    `AssistantMessage`; one settlement per attempt; `attempts == 3`,
    `retries == 2`.
12. **No double retry.** With the adapter loop disabled and the executor
    enabled, exactly `attempts` provider calls occur. A static/startup assertion
    fails if both are enabled (`RE-F1`).
13. **Cancel during wait.** `llm/retry` present, `llm/retry_started` absent, the
    turn ends `Cancelled`.
14. **Pre-dispatch failure.** A `NoProviderRouteError` writes no retry event and
    no settlement.
15. **One-shot.** A second dispatch on a `PreparedCall` throws
    `InvalidPreparedCall` (`28-L24`).
16. **`RetryId`.** Monotonic and unique within a session; a resumed executor
    seeds from the log and does not reuse.
17. **Crash/resume.** A log ending at `llm/retry` without `llm/retry_started`
    resumes safely; re-issuing produces no duplicate durable output.
18. **Replay.** A log containing retry events replays without constructing the
    executor; derived messages are unchanged (`RE-I16`).

**Pinned-state (gate-now).**

19. **No emission while pinned.** A normal run that retries inside the adapter
    writes no `llm/retry`/`llm/retry_started` (retries stay unlogged, `RE-I15`).
20. **`Mode::Normal` only.** No producer emits `Mode::Always`; the value still
    round-trips (`RE-D11`).

**Property tests.**

21. **Ordering property.** For any scripted failure sequence, the retry events
    interleave with settlements as in §5.6, and exactly one final settlement is
    an `AssistantMessage` iff the last attempt completed.
22. **Fuzz the decision.** Random `(code, any_event, retries_so_far, max_attempts)`
    tuples never retry a non-retryable code or exceed the budget.

---

## 17. Gate review checklist

Because this is a design artifact, the gate is a review, not a test run:

1. **Gap closure.** §1.1 cites `30 §5.1` :295-302, `28 §13.2` :945-948, and
   `31 §10` :601-615; §1.2/§13 state how each half is closed.
2. **OQ4 honor.** §1.5 and §14/§15 do not promote OQ4; activation is RE-OQ1.
3. **Consistency with 26.** The payloads and keys match
   `26-dsh-alignment-part2.md:856-872`, `:950-951`; the step-boundary language
   matches `26-I3` :59-61.
4. **Consistency with 28.** No `LlmRuntime` interface changes; L18-L26 are
   respected; the policy source is `PreparedCall::retry_policy()`.
5. **Consistency with 29.** The codec checklist (§3.1), the two-axis rule
   (§3.3), the consumer matrix (§4.2/§4.4), and version neutrality (§5/§8) are
   all addressed; the 29 open item is closed by ownership.
6. **Consistency with 34.** Settlement is unchanged; the executor drives, 34
   settles.
7. **Consistency with 31.** The compaction re-attempt is untouched; no retry of
   the loop's own is added.
8. **Tree anchors.** Every `file:line` in §2/§5/§6 resolves to the named symbol
   at the freeze commit; drifted anchors from prior specs are corrected in §19.

---

## 18. Revision log

- **Rev 1 (2026-09-19).** Initial write. Owns the durable step-boundary retry
  executor and the `LlmRetry`/`LlmRetryStarted` codec, closing the `30 §5.1`
  named ownership gap and the `29 §1.3`/§8 event-ownership open item. Pins the
  executor **without activating it**, honoring `26` OQ4 :1604-1607; activation
  timing is RE-OQ1. Adds RE-I1-RE-I20, RE-F1-RE-F20, RE-D1-RE-D12, and the
  dependency list, dsh mapping, and test plan. No implementation code.

---

## 19. Reference and citation reconciliation

Every line anchor below was checked against the working tree at authoring time.
Several anchors cited by prior specs have drifted; the correct anchors are given
and the drift is recorded so the gate can reconcile.

| Cited by | Stale anchor | Current anchor | Note |
|---|---|---|---|
| `30 §5.1`, `28 §6.1`, `31 §10` | `26p2 §4.3.9.1 :947-948` | `26-dsh-alignment-part2.md:950-951` (JSON keys), `:840-841` (EventType/wire), `:856-872` (C++ payloads) | `:947-948` now points at the `LlmRequestHeader`/`LlmCallConfig` rows |
| `34 §7.2` | `26 §4.3.4 :603` | `26-dsh-alignment-part2.md:604-606` | `:603` is a blank line; the "failed, retried" text is at `:606` |
| `30 §5.1` | `26 §5 Waves 1-6 (:1397-1498)` | `26-dsh-alignment-part2.md:1386-1501` | Wave 0 Stage B table `:1386-1396`; Wave 1 `:1401`; Wave 6 `:1494-1501` |
| `28 §6.1` | `stream.hpp:111` (the L7 barrier) | `stream.hpp:119-131` (`is_retryable_code`); barrier enforced at `openai_adapter.cpp:983` | `:111` now points at `to_string(StreamOutcome)` |
| `28 §1.1` | `stream.hpp:112-123` | `stream.hpp:119-131` | `is_retryable_code` moved down 7 lines |
| `28 §1.1` | `agent_loop.cpp:781` (loop provider call) | `agent_loop.cpp:938-942` | the direct `provider->stream` call was replaced by `prepare_call` + `PreparedCall::stream` when Wave 1 landed |
| `28 §1.1` | `agent_loop.cpp:771-809` (compaction re-attempt) | `agent_loop.cpp:999-1004` | the re-attempt loop now starts at `:900`; the `ContextLengthExceeded` branch is `:999-1004` |
| `30 §5.1` | `28 §6.2/§13.2` | `28 :660-674` / `:945-948` | verified, no drift |

**Primary references.**

- `docs/design/26-dsh-alignment.md` §2.1.7 :400-436 (retry is a separate package
  at a durable boundary); :418-432 (the event payloads).
- `docs/design/26-dsh-alignment-part2.md` §4.3.9.1 :837-872 (EventType/wire/C++
  payloads), :946-951 (JSON keys); §5 :1386-1501 (the wave plan); §6 OQ4
  :1604-1607.
- `docs/design/28-llm-service-boundary-errata.md` §3.1 (the `LlmRuntime` seam),
  §6 (one attempt per stream), §13.2 (the gap), L18-L26.
- `docs/design/29-event-family-errata.md` §1.3 :82-87, §3.1 :115-146, §4.4
  :387-388, §8 :480-483.
- `docs/design/30-architecture-cascade-errata.md` §5.1 :295-302, §5.3 :315-328.
- `docs/design/31-agent-loop-errata.md` §5.2 :390-422, §5.4 :503-512, §10
  :601-615.
- `docs/design/34-assembler-replay-errata.md` §6 :363-436, §7 :437-488.
- `docs/design/08-llm-provider.md` §3.7 :451-492, L6 :1006, L7 :1011.
- Tree: `include/ymh/core/event.hpp` (`EventType`), `src/core/event.cpp`
  (`kWireNames`), `include/ymh/llm/llm_runtime.hpp`,
  `include/ymh/llm/llm_provider.hpp`, `include/ymh/llm/provider_registry.hpp`,
  `include/ymh/llm/stream.hpp`, `include/ymh/session/events.hpp`,
  `src/agent/agent_loop.cpp`, `src/llm/openai_adapter.cpp`,
  `include/ymh/config/config.hpp`.

# 84 - Retry Budget: Per-Attempt Deadline Decoupled from the Overall Call Budget Errata

```
Status: verified (Rev 2)
Verification status: adversarial reviewer (independent, Rev 2): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Revision: 2
Component: 84 (corrective errata) -- LLM provider transport/retry, config
Depends on: 08-llm-provider.md (verified) Sec. 3.7 (:451-496), Sec. 3.8 (:498-528),
            Sec. 5.3 (:729-758), Sec. 10 (L7 :1047-1050), Sec. 12 (:1152-1172),
            Sec. 14.1(v) :1399-1408 and Sec. 14.1(w) :1409-1419;
            00-architecture.md (verified) Sec. 54 D5 (:4756-4758), Sec. 55 (:4860-4894);
            21-config-jsonc-errata.md (verified) Sec. 6 (the config layer discipline);
            41-retry-executor.md (verified) Sec. 4.2 :267-275;
            56-dsh-fidelity-gap-closure-errata.md Sec. 3 (56-D6);
            AGENTS.md ("State lifetime is part of the spec"; "New symbols are normative").
Scope: one confirmed defect -- the retry budget is a SINGLE per-request deadline
       shared across every attempt, so a retryable transport timeout that consumes
       the deadline is never retried and `retry.max_attempts` is dead for any
       request that stalls to the deadline. This spec decouples the per-attempt
       deadline (`request_timeout_ms`) from a separate overall call budget and
       pins the budget algebra, invariants, failure modes, config surface,
       state lifetime, and test plan. Design only; no implementation code.
Supersedes: 08 Sec. 3.7 final bullet (:490-496) -- "a retry budget is bounded by
            `max_attempts` **and one per-request deadline shared across every
            attempt** (**08-D18**)" -- and decision **08-D18** itself (08 Sec. 14.1(w)
            :1409-1419). Each attempt now gets a fresh per-attempt deadline bounded
            by a separate overall budget (84-D1, 84-D2, 84-D6). See Sec. 1.4.
Amends: 08 Sec. 3.8 (:502-508) -- `request_timeout` changes meaning from "total
        wall time for the request" to "per-attempt wall time", and the call-level
        bound becomes the new overall budget; `LLMRequest.deadline` now caps both
        the per-attempt deadline and the overall budget.
        08 Sec. 5.3 (:737-753) -- the retry example gains `total_timeout_ms`.
        NOTE (L4): the 08 Sec. 5.3 block is a retired-TOML example (config is JSONC
        now; AGENTS.md, 21-config-jsonc-errata.md). The shipped JSONC surface is
        pinned in this spec Sec. 6.1 and in the scaffold at `src/config/config.cpp`
        :1511-1521; the 08 example is updated only for consistency, not as the
        normative surface.
        41 Sec. 4.2 :267-275 -- the sentence that names 08-D18's shared deadline is
        replaced by a reference to this spec; 41's executor is not otherwise changed
        (41 is not activated; its bounds are unaffected).
        Code anchors the implementing wave must touch: `src/llm/openai_adapter.cpp`
        :1233-1245, :1252-1255, :1285, :1349-1365, :1370; `include/ymh/config/config.hpp`
        :222-228, :244-247, :294-298, :530-542; `include/ymh/llm/llm_provider.hpp` :39-47;
        `include/ymh/llm/provider_registry.hpp` :29-48; `src/config/config.cpp`
        :175-177, :588-599, :601-606, :663-665, :913-928, :1511-1521;
        `src/cli/wiring.cpp` :104-122; **`include/ymh/llm/openai_adapter.hpp`** (new
        declarations for the budget helpers -- mirror `backoff_delay` at :130,
        defined after the anonymous namespace closes at
        `src/llm/openai_adapter.cpp:906`); **`tests/fixtures/spec_symbol_catalog.json`**
        :1170-1194 (add `config::llm.default.retry.total_timeout_ms`) and
        **`tests/unit/spec_catalog_test.cpp`** :293-338, :460-476 (the mechanical
        extractor/requirement this key trips); **`tests/unit/config_test.cpp`**
        :96-100 (retry-field equality enumeration).
Tracker: DESIGN_STATUS.md row 84 is orchestrator-owned and added only at promotion
         (AGENTS.md rule); it is not created or edited by this spec. Promotion must
         also update the `08` row's 08-D18 text (DESIGN_STATUS.md:16) and the review
         log entry (DESIGN_STATUS.md:181), which still record 08-D18 as current.
```

---

## 1. Purpose and root cause

### 1.1 The defect (confirmed from a durable session event log)

A real session stalled on a provider call and failed without retrying. The durable
event log recorded, at turn 4 / step 74:

- `assistant/attempt` (`EventType::AssistantAttempt`, wire `assistant/attempt`,
  `src/core/event.cpp:30`; payload `payload::AssistantAttempt`,
  `include/ymh/session/events.hpp:128-132`) carrying a `stream_error` whose
  `LLMError` was `{"code":"timeout","detail":"transport timeout",...,"retryable":true}`,
  with `time_ms: 120056`; then
- `turn/fail` (`EventType::TurnFailed`, wire `turn/fail`, `src/core/event.cpp:24`)
  with `{"code":"ProviderFailed","message":"transport timeout"}`.

The error is **retryable** (`LLMErrorCode::Timeout` is in `is_retryable_code`,
`include/ymh/llm/stream.hpp:119-131`) and it arrived **before the first dispatched
event**, so 08 L7 (`08-llm-provider.md:1047-1050`) explicitly permits a retry. Yet
no second attempt was made. The call failed after one 120 s attempt even though the
default `retry.max_attempts` is 3 (`src/config/config.cpp:1516`,
`src/config/config.cpp:590-591`; `include/ymh/llm/llm_provider.hpp:42`).

### 1.2 Correcting the record (why 08-D18 caused this)

08 Sec. 3.7 (:490-496) and decision 08-D18 (08 Sec. 14.1(w) :1409-1419) make the
retry budget **one per-request deadline shared across every attempt**. The shipped
code implements exactly that, in `OpenAICompatibleProvider::stream()`:

- `const std::chrono::milliseconds call_budget = effective_timeout(request.deadline, config_.request_timeout);`
  (`src/llm/openai_adapter.cpp:1236-1237`);
- each attempt's transport deadline is the remaining budget:
  `http.total_timeout = call_budget.count() > 0 ? budget : call_budget;`
  (`:1285`), where `budget = remaining_budget()` (`:1252`);
- after a retryable pre-event failure the retry is skipped once the budget is gone:
  `budget = remaining_budget(); if (call_budget.count() > 0 && (budget.count() <= 0 || delay >= budget)) return failed(error);`
  (`:1357-1359`).

Attempt 1 used the entire 120 s `request_timeout` (`src/config/config.cpp:1514`,
`:660-661`). `remaining_budget()` (`:1238-1245`) therefore went negative, and
`:1358` returned `failed(error)` instead of retrying. `max_attempts: 3` is
**dead** for any request that stalls to the deadline. The default per-attempt
deadline equals the overall budget: whenever the *first* attempt consumes it, no
retry can ever fit. 08-D18 traded retry capability for a tight wall-clock bound
and did not notice that, for the very failure mode it was protecting against
(a stall), it also disables the retry it assumed was available.

A contributing factor is that the mid-stream idle guard is off whenever a total
deadline is set: `context.guard_mid_stream = request.total_timeout.count() <= 0;`
(`src/llm/openai_adapter.cpp:1083`, rationale in the comment at `:784-792` and 08
Sec. 3.8 / 08-D17 :511-528). A stall *after* the first byte is therefore bounded
only by the per-attempt total deadline. Sec. 2.5 decides this explicitly and
**does not** change it.

### 1.3 What this spec changes, in one sentence

Each attempt gets its own fresh per-attempt deadline (`request_timeout_ms`), and the
all-attempts bound moves to a separate overall budget (`retry.total_timeout_ms`,
default `0` = derived), so a retryable timeout that consumes one attempt's deadline
is retried while the whole `stream()` call stays finitely bounded.

### 1.4 Supersession and amendment map

| # | Clause superseded / amended (file:line) | New text (this spec) |
|---|---|---|
| A1 | 08 Sec. 3.7 final bullet (`docs/design/08-llm-provider.md:490-496`): "a retry budget is bounded by `max_attempts` **and one per-request deadline shared across every attempt** (08-D18)" | **Superseded.** The retry budget is bounded by `max_attempts` **and a separate overall call budget**; each attempt is capped by a fresh per-attempt deadline `min(attempt_deadline, remaining_overall)`. See Sec. 2.2, RB1-RB7. |
| A2 | 08 Sec. 14.1(w) (`:1409-1419`), decision **08-D18** | **Superseded** by 84-D2/D3/D6. The guarantee is still a finite bound (now the overall budget), and `max_attempts` is now reachable, not dead. See Sec. 9. |
| A3 | 08 Sec. 3.8 timeout table (`:502-506`): `request_timeout` = "total wall time for the request, including an established stream" | **Amended.** `request_timeout` is the **per-attempt** wall-clock cap; the total-of-call bound is the overall budget. Default value (120 s) unchanged; meaning narrowed to one attempt. See Sec. 2.4. |
| A4 | 08 Sec. 3.8 (`:508`): "`LLMRequest.deadline`, when non-zero, caps `request_timeout` for that call" | **Amended.** `deadline` caps both the per-attempt deadline (the `effective_timeout` shape is amended only in its dormant `configured == 0 && deadline > 0` arm) and the overall budget (`min`). See Sec. 2.4, 84-D4. |
| A5 | 08 Sec. 5.3 config example (`:737-753`) -- a **retired-TOML** example | **Amended additively** for consistency only: the retry example gains `total_timeout_ms`. The **normative, shipped** surface is the JSONC `llm.default.retry` / `llm.endpoints.<name>.retry` table pinned in this spec Sec. 6.1 and the scaffold at `src/config/config.cpp:1511-1521`; config is JSONC and TOML is retired (AGENTS.md; 21-config-jsonc-errata.md). See Sec. 6 (L4). |
| A6 | 41 Sec. 4.2 :267-275 (the sentence "When the LLM adapter's **08-D18** shared per-request deadline is in force ... one `request_timeout`") | **Amended.** The reference is retargeted to this spec; 41's `decide_retry` and its own bounds are unchanged (41 is not activated, Sec. 1.5). |
| A7 | `src/llm/openai_adapter.cpp:1233-1245, :1252-1255, :1285, :1357-1359, :1370` (the 08-D18 budget implementation) | **Changed by the implementing wave** to the Sec. 2.2 algebra; the `08-D18` comments become stale and must cite 84-D1..84-D6. |
| A8 | `include/ymh/config/config.hpp:222-228, :244-247, :294-298, :530-542`; `include/ymh/llm/llm_provider.hpp:39-47`; `include/ymh/llm/provider_registry.hpp:29-48` | **Amended additively:** `RetrySettings`/`RetryPolicy` gain `total_timeout` (ms, default 0). See Sec. 6, Sec. 2.3. |
| A9 | `src/llm/openai_adapter.cpp:280-286` (`effective_timeout` in the anonymous namespace, `:31`..`:906`) and the new budget helpers | **Moved out of the anonymous namespace** into `namespace ymh`, declared in `include/ymh/llm/openai_adapter.hpp` (after `backoff_delay` at `:130`), defined after `:906`, so T5's direct calls compile/link (M1, 84-D13, Sec. 2.3). |
| A10 | `tests/fixtures/spec_symbol_catalog.json` (`:1170-1194`) and `tests/unit/config_test.cpp:96-100` | **Amended additively:** the new extracted symbol `config::llm.default.retry.total_timeout_ms` gains a catalog entry and the retry equality enumeration gains `total_timeout`, or the mechanical gate `tests/unit/spec_catalog_test.cpp:293-338, :460-476` fails (M2/L5, 84-D14, Sec. 6.5). |

Nothing else is superseded. 08 L7 (no retry after the first dispatched event,
`:1047-1050`), 08 L13/L17, the error taxonomy (`:167-196`), the `HttpTransport`
seam (`include/ymh/llm/openai_adapter.hpp:62-70`), `LLMRequest`/`LLMResponse`, and
08-D17 (Sec. 2.5) all stand unchanged. The `assistant/attempt` / `turn/fail`
durable event family is unchanged (Sec. 2.6).

### 1.5 Scope boundaries

- **In scope:** the adapter retry budget algebra; the `request_timeout` semantic
  narrowing; the new `retry.total_timeout_ms` config key; its validation, defaults,
  scaffold, and wiring; the state-lifetime table; the hermetic test plan.
- **Not in scope (explicit):** the durable step-boundary retry executor of spec 41
  (still not activated; 41 Sec. 1.5). This spec does not change `decide_retry`,
  `RetryExecutor`, or any durable event.
- **Not in scope:** the `LlmPool` concurrency bound (08 L13), the compaction
  re-attempt (41 Sec. 3.3, `src/agent/agent_loop.cpp:1394-1414`), and the
  `ContextLengthExceeded` handling.
- **Not in scope:** re-enabling the mid-stream idle guard. Sec. 2.5 keeps 08-D17.
- No new event type; no schema change; no new persisted state.

---

## 2. The fix: per-attempt deadline, separate overall budget

### 2.1 Chosen model and alternatives rejected

**Chosen.** Two independent quantities:

1. a **per-attempt deadline** `attempt_deadline`, computed once from
   `request_timeout` (capped by `LLMRequest.deadline`), applied fresh to **every**
   attempt (84-D1); and
2. an **overall budget** `overall_budget` for the entire `stream()` call, covering
   every attempt plus every backoff sleep (84-D2), with a new
   `retry.total_timeout_ms` key whose default `0` means **derived** (84-D3).

An attempt's transport timeout is `min(attempt_deadline, remaining_overall)` so no
attempt can outlive the overall budget (84-D5). A retryable pre-event failure
retries iff the overall budget still has room for the backoff; it no longer depends
on how much the attempt itself consumed (84-D6). This is the minimal change that
lets `max_attempts` actually retry a timeout while keeping a finite total bound.

**Alternatives rejected:**

- *Keep 08-D18 and raise `request_timeout`*: does not fix the defect (the first
  attempt still consumes the whole budget) and worsens stall latency.
- *Drop the budget entirely; bound only by `max_attempts`*: a stall could then
  defer the terminal `turn/fail` by `max_attempts x request_timeout` with no fixed
  wall-clock bound; rejected as the **default** (but see Sec. 2.2: this is exactly
  what happens when the per-attempt deadline is itself `0`, where no better bound
  exists).
- *Per-attempt deadline only, no overall budget*: unbounded deferral whenever
  attempts stall; rejected.
- *Re-enable the mid-stream idle guard (supersede 08-D17)*: rejected; Sec. 2.5.

### 2.2 The budget algebra (normative)

All arithmetic is on `std::chrono::milliseconds`; `0` means unbounded at the
quantity where it appears. Saturing arithmetic is used (RB-F1). `max_attempts` is
`std::max<std::uint32_t>(1, retry.max_attempts)` as at `openai_adapter.cpp:1223`.

```text
// (a) per-attempt deadline -- fresh every attempt (84-D1)
attempt_deadline = effective_timeout(request.deadline, cfg.request_timeout)
    // effective_timeout is the total function in Sec. 2.4 (84-D4)

// (b) backoff envelope -- the maximum total sleep the schedule can emit
backoff_envelope(retry, n):
    total = 0
    for i in 1 .. (n - 1):
        capped = min(retry.base_delay * 2^(i-1), retry.max_delay)
        total += ceil(capped * (1 + retry.jitter))
    return total

// (c) derived overall budget (default; total_timeout == 0) (84-D3)
derive_total(attempt_deadline, retry, n):
    if attempt_deadline <= 0: return 0        // per-attempt unbounded => overall unbounded
    if n <= 1:               return attempt_deadline
    return attempt_deadline * n + backoff_envelope(retry, n)

// (d) overall budget (84-D2, 84-D4)
base          = (retry.total_timeout > 0) ? retry.total_timeout
                                          : derive_total(attempt_deadline, retry, n)
overall_budget = (request.deadline > 0)
                   ? ((base > 0) ? min(base, request.deadline) : request.deadline)
                   :  base

// (e) per-attempt transport deadline (84-D5); called once per attempt.
// remaining is clamped to >= 0 so per_attempt_timeout is total (L1).
remaining = (overall_budget > 0) ? max(overall_budget - elapsed, 0) : 0
attempt_total = (overall_budget > 0)
                  ? ((attempt_deadline > 0) ? min(attempt_deadline, remaining) : remaining)
                  : attempt_deadline

// (f) loop guard before attempt 1 (kept; now measured against the OVERALL budget)
if overall_budget > 0 and remaining <= 0: return failed(Timeout, "request deadline exceeded")

// (g) retry decision after a retryable pre-event failure (THE FIX; 84-D6)
if is_retryable_code(error.code) and not decoder.any_event() and attempt < n:
    delay = backoff_delay(retry, attempt, jitter_sample)     // Retry-After may override
    remaining = remaining_overall()
    if overall_budget > 0 and (remaining <= 0 or delay >= remaining):
        return failed(error)          // overall budget cannot fit the backoff
    sleep_with_cancel(delay, cancel)
    continue
```

Mapping to shipped code (the implementing wave edits these in place):

| Algebra step | Current anchor | Change |
|---|---|---|
| (a) | `openai_adapter.cpp:1236-1237` (`call_budget`) | rename to `attempt_deadline`; keep `effective_timeout` (Sec. 2.4) |
| (d) | `openai_adapter.cpp:1238-1245` (`remaining_budget`) | compute `overall_budget` from `retry.total_timeout`/derived; `remaining_budget` measures elapsed against it |
| (e) | `openai_adapter.cpp:1285` (`http.total_timeout`) | `min(attempt_deadline, remaining)` when the overall budget is finite, else `attempt_deadline` |
| (f) | `openai_adapter.cpp:1252-1255` | keep; compare against the overall budget |
| (g) | `openai_adapter.cpp:1349-1365` | keep the shape; the `:1357-1359` guard now checks the overall budget (see (g)) |
| unreachable-fallthrough | `openai_adapter.cpp:1370` | unchanged (single attempt still exhausts the loop) |

### 2.3 Interface sketches (C++)

Signatures only; no bodies. The four budget helpers are **public `namespace ymh`
symbols declared in `include/ymh/llm/openai_adapter.hpp`** and defined in
`src/llm/openai_adapter.cpp` *after* the anonymous namespace closes (the anon
block is `:31`..`:906`), exactly mirroring `backoff_delay` -- declared at
`include/ymh/llm/openai_adapter.hpp:130` and defined at
`src/llm/openai_adapter.cpp:927`. This is required so the T5 helper unit tests
(Sec. 7) can call them from a separate translation unit as
`ymh::derive_total_budget(...)` etc. (M1). They are `[[nodiscard]]`, pure (no I/O,
no clock, deterministic), and total for every input. `now_`
(`include/ymh/llm/openai_adapter.hpp:89,110`) remains the injectable steady clock;
no new time source is introduced.

```cpp
namespace ymh {

// include/ymh/config/config.hpp:222 (RetrySettings) and
// include/ymh/llm/llm_provider.hpp:41 (RetryPolicy) -- one additive field each:
struct RetrySettings {
    std::uint32_t             max_attempts = 3;
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{30'000};
    double                    jitter = 0.25;
    bool                      honor_retry_after = true;
    // NEW (84-D2): overall budget for one stream() call across all attempts and
    // backoffs. 0 => derived (84-D3). Non-negative (read_int64 rejects < 0).
    std::chrono::milliseconds total_timeout{0};
};

struct RetryPolicy {                 // include/ymh/llm/llm_provider.hpp:41
    std::uint32_t             max_attempts = 3;
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{30'000};
    double                    jitter = 0.25;
    bool                      honor_retry_after = true;
    std::chrono::milliseconds total_timeout{0};   // NEW (84-D2)
};

// include/ymh/llm/openai_adapter.hpp -- the four helpers are declared here
// (append after backoff_delay at :130, beside parse_retry_after at :136) and
// defined in src/llm/openai_adapter.cpp after the anonymous namespace closes:

// (amended; 84-D4) total function. configured<=0 => max(deadline,0);
// deadline<=0 => configured; else min(deadline, configured).
[[nodiscard]] std::chrono::milliseconds effective_timeout(
    std::chrono::milliseconds deadline,
    std::chrono::milliseconds configured);          // openai_adapter.cpp:280

// (new) derived overall budget (84-D3); 0 == unbounded; saturating.
[[nodiscard]] std::chrono::milliseconds derive_total_budget(
    std::chrono::milliseconds attempt_deadline,
    const RetryPolicy&        retry,
    std::uint32_t             max_attempts);

// (new) overall budget from config + caller deadline (84-D2/D4); 0 == unbounded.
[[nodiscard]] std::chrono::milliseconds resolve_overall_budget(
    std::chrono::milliseconds retry_total_timeout,
    std::chrono::milliseconds request_deadline,
    std::chrono::milliseconds attempt_deadline,
    const RetryPolicy&        retry,
    std::uint32_t             max_attempts);

// (new) per-attempt transport deadline (84-D5); see algebra (e). Total (L1):
//   overall_budget == 0        -> attempt_deadline        (0 == unbounded)
//   overall_budget  > 0:
//       remaining = max(overall_budget - elapsed, 0)      (clamped to 0)
//       attempt_deadline == 0  -> remaining
//       else                   -> min(attempt_deadline, remaining)
// The clamped `elapsed >= overall` arm returns 0; the caller must not issue a
// transport call with this 0. That arm is unreachable from stream() because the
// loop guard (f) runs first and returns failed(Timeout) when the overall budget
// is exhausted; the clamp makes the helper well-defined for T5's direct call.
[[nodiscard]] std::chrono::milliseconds per_attempt_timeout(
    std::chrono::milliseconds attempt_deadline,
    std::chrono::milliseconds overall_budget,
    std::chrono::milliseconds elapsed);

// OpenAICompatibleProvider::stream() locals (unchanged shape, renamed):
//   attempt_deadline, overall_budget, call_start, remaining_overall();
//   http.total_timeout = per_attempt_timeout(attempt_deadline, overall_budget, elapsed());

} // namespace ymh
```

Callers (AGENTS.md "New symbols are normative"): `derive_total_budget` and
`resolve_overall_budget` are called by `OpenAICompatibleProvider::stream()` when it
enters the attempt loop (`openai_adapter.cpp:1233-1247`); `per_attempt_timeout` is
called once per attempt just before `transport_->postStream(...)` at `:1285`;
`effective_timeout` keeps its single existing caller at `:1237` and gains a header
declaration. The T5 test TU (`tests/unit/openai_adapter_test.cpp`) is a second,
compile-time caller of all four via the `ymh::` declarations in
`include/ymh/llm/openai_adapter.hpp`. No other caller is introduced.

### 2.4 `effective_timeout` and `LLMRequest.deadline` interaction

`LLMRequest.deadline` is a duration (`std::chrono::milliseconds deadline{0}`,
`include/ymh/llm/llm_request.hpp:62`; `0 => provider default`). It is currently
**never set by any caller** (grep: no `LLMRequest` field assignment; only the
unrelated tool-side `ToolRequest` deadlines in `src/tools/builtin_tools.cpp:424-426`
exist). It is therefore a dormant seam and this spec may pin its semantics without
a live behavior change.

Pinned interaction (84-D4):

1. `attempt_deadline = effective_timeout(request.deadline, cfg.request_timeout)`.
   `effective_timeout` (`openai_adapter.cpp:280-286`) is amended to the total
   function: `configured <= 0` returns `max(deadline, 0)`; `deadline <= 0` returns
   `configured`; otherwise `min(deadline, configured)`. The only behavioral
   difference from the shipped body is the previously-buggy arm
   `configured == 0 && deadline > 0`, which returned `0` (unbounded) and now
   returns `deadline`. That arm is dormant (no caller sets `deadline`), so no
   shipped call changes.
2. `request.deadline`, when non-zero, **also caps the overall budget**
   (algebra (d)). A caller that sets a hard per-call deadline therefore bounds the
   whole call; if that deadline is `<= request_timeout` only one attempt fits and
   the retry is suppressed. This is correct: the caller asked for that bound.
3. With the default `request.deadline == 0` and `retry.total_timeout == 0`, the
   overall budget is the derived value (84-D3), so retries are possible with no
   config change.

Documented consequence (RB-F3/RB-F4): `retry.total_timeout_ms > 0` set below
`request_timeout_ms`, or `request.deadline` set below `request_timeout_ms`, yields
a single attempt. That is a deliberate bound, not a defect; if an operator wants
retries they must give the overall budget room for `max_attempts` attempts.

### 2.5 The mid-stream idle guard (08-D17): unchanged

**Decision (84-D7): keep 08-D17 exactly as shipped.** When a total deadline is set
(the normal case), the mid-stream idle guard stays off
(`openai_adapter.cpp:1083`; comment `:784-792`; 08 Sec. 3.8 :511-528). This spec
does **not** supersede 08-D17.

Why keeping it does not reopen this failure:

1. **The failure is fully explained by the shared budget.** The attempt failed with
   a retryable `Timeout`; the only reason no retry happened is algebra (g) of the
   old code (`:1357-1359`). Fixing the budget makes attempt 2 run, independent of
   the idle guard. The guard's state is not on the failure path's decision.
2. **08-D17 fixes a real false abort.** dsh-style reasoning models are legitimately
   silent mid-stream (reasoning / long tool-argument assembly); the guard's
   sliding-window speed check cannot tell a hung socket from a thinking model
   08 Sec. 3.8 :511-520. Re-enabling it would regress the exact bug 08-D17 removed.
3. **The overall budget bounds the deferred failure.** A stalled attempt can now
   burn at most `min(attempt_deadline, remaining_overall)` per attempt, and the
   whole call is capped by `overall_budget` (RB2). The multiplicative, unbounded
   deferral 08-D18 feared is still prevented; only the retry is restored.
4. **The pre-first-byte stall is still guarded.** `idle_timeout` (default 60 s,
   `config.cpp:1513`) still bounds the response-await window via the progress
   callback (`openai_adapter.cpp:854-861`); only the post-first-byte gap is
   governed by the (now per-attempt) total deadline. An operator who wants faster
   post-first-byte stall detection now has a per-attempt knob:
   lowering `request_timeout_ms` lowers each attempt's bound without disabling
   retries (contrast with 08-D18, where lowering it also killed the retry).

Recorded rejected alternative (would-be 84-D8): re-enable the mid-stream guard and
supersede 08-D17. Rejected for reason (2); it is not needed for the fix.

### 2.6 Observability (durable event unchanged; transcript covers attempts)

No new event type. The `assistant/attempt` event
(`include/ymh/session/events.hpp:128-132`; appended once per settled provider
attempt at `src/agent/agent_loop.cpp:1387-1391`) is unchanged: the adapter's
internal retries are not separately durable events, exactly as before. Retries
remain observable through the opt-in LLM wire transcript (08 74-D7), which records
one `record_request`/`record_response` pair per attempt with the shared
`request_id` and the 1-based `attempt`:
`openai_adapter.cpp:1287-1290`, `:1328-1334`, `:1339-1342`; consumer precedent
`tests/unit/openai_adapter_test.cpp:1094-1129`. The terminal durable record after
the fix is a completed `AssistantMessage` (success) or one `turn/fail` (all
attempts exhausted), never both (08 L17).

---

## 3. State lifetime table (new and changed state)

Every row is normative. "Restart" means daemon process restart; "switch" means
session switch within a live daemon. This spec introduces **no durable state**; the
rows below are config values and per-call stack locals only.

| State | Type | Created | Owned by | Destroyed / evicted | Survives restart? | Survives switch? | Crash path |
|---|---|---|---|---|---|---|---|
| `RetrySettings.total_timeout` (config) | `std::chrono::milliseconds` | JSONC parse (`config.cpp:588-599` via `read_retry`) | the `Config` value for the process | when `Config` is destroyed | yes (re-read from the config file on next run if the file is unchanged) | n/a (process-wide) | a malformed value fails config load (Sec. 6); no partial config escapes |
| `RetryPolicy.total_timeout` (provider config) | `std::chrono::milliseconds` | `to_provider_config` copy (`wiring.cpp:104-122`) | `LLMProviderConfig` held by the daemon | with the provider/daemon | no (rebuilt from config on restart) | n/a | discarded with the process |
| `attempt_deadline` | `std::chrono::milliseconds` | computed once at the top of `stream()` (`openai_adapter.cpp:1236-1237`) | the `stream()` stack frame | when `stream()` returns | no (live-only) | no | unwinds with the frame; the response is already built |
| `overall_budget` (was `call_budget`) | `std::chrono::milliseconds` | computed once per `stream()` (algebra (d)) | the `stream()` stack frame | when `stream()` returns | no | no | discarded |
| `call_start` / `remaining_overall()` closure | `steady_clock::time_point` / lambda capturing `overall_budget` | `stream()` (`:1235-1245`) | the `stream()` stack frame | when `stream()` returns | no | no | discarded |
| per-attempt `HttpRequest.total_timeout` | `std::chrono::milliseconds` | each attempt, just before `postStream` (`:1278-1285`) | the `HttpRequest` value owned by the attempt's transport call | when `postStream(...)` returns (`:1292-1303`) | no | no | the transport call is bounded by its own `total_timeout`/cancel; a crash loses only that attempt |
| retry attempt counter | `std::uint32_t` loop local | `stream()` (`:1247`) | the `stream()` stack frame | when `stream()` returns | no | no | discarded |
| `assistant/attempt` record (unchanged) | `payload::AssistantAttempt` | agent loop after the settled attempt (`agent_loop.cpp:1387-1391`) | the session store | durable (append-only; never evicted by this spec) | yes | yes (per session) | the append is atomic at the store layer; a crash mid-append is the store's existing recovery, unchanged |

No cache, map, registry row, or file is added. The new config field is the only
state with a lifetime beyond one `stream()` call, and it is config, re-derived on
every start.

---

## 4. Invariants

Numbered `RB1`..`RB10`; disjoint from 08's `L#`/`L-F#`, 41's `RE#`/`RE-F#`, and the
shared F1-F12. Any code that can violate one is a defect.

| ID | Invariant | Where |
|---|---|---|
| **RB1** | **Fresh per-attempt deadline.** Each attempt's transport deadline is derived from `attempt_deadline = effective_timeout(request.deadline, retry.request_timeout)`, recomputed/applied for every attempt, not decremented from a shared pool. | Sec. 2.2(a), 84-D1 |
| **RB2** | **Bounded overall call.** The wall clock of one `stream()` call, including every attempt and every backoff sleep, never exceeds `overall_budget` when `overall_budget > 0`. | Sec. 2.2(d), 84-D2 |
| **RB3** | **Retry after a deadline-consuming timeout.** A retryable pre-first-event failure whose attempt consumed the entire `attempt_deadline` is retried iff `overall_budget` is unbounded or has room for the backoff (`remaining > delay >= 0`). | Sec. 2.2(g), 84-D6 |
| **RB4** | **`max_attempts` is reachable.** With `retry.total_timeout == 0` and `attempt_deadline > 0`, the derived budget schedulably holds `max_attempts` attempts: `derive_total >= attempt_deadline * max_attempts`. Scoped to the **plain backoff schedule**: a `RateLimited` `Retry-After` override that exceeds the remaining budget stops retries early (RB-F6, `openai_adapter.cpp:1352-1355`), so RB4 does not claim `max_attempts` attempts under an arbitrary `Retry-After`. | Sec. 2.2(c), 84-D3 |
| **RB5** | **Zero semantics.** `retry.total_timeout == 0` => derived (or unbounded iff `attempt_deadline == 0`); `request_timeout == 0` => per-attempt unbounded; `request.deadline > 0` caps both the per-attempt deadline and the overall budget. | Sec. 2.2(d), Sec. 2.4, 84-D4 |
| **RB6** | **Attempt never outlives the overall budget.** `http.total_timeout == min(attempt_deadline, remaining_overall)`, or `attempt_deadline` when the overall budget is unbounded. | Sec. 2.2(e), 84-D5 |
| **RB7** | **L7 preserved.** Retries occur only before the first dispatched `StreamEvent` (including `UsageEvent`); no change to 08 L7. | 08 :1047-1050, Sec. 1.4 |
| **RB8** | **Determinism / testability.** The budget helpers are pure functions of their arguments; elapsed time is read only through the injected `SteadyNow` (`openai_adapter.hpp:89,110`); no wall clock, random, or I/O in the helpers. | Sec. 2.3, Sec. 7 |
| **RB9** | **No new durable state / event / schema.** No event type, `assistant/attempt` field, DB schema, or persisted artifact is added or changed; the retry count is not durable. | Sec. 2.6, Sec. 3 |
| **RB10** | **Config totality + catalog.** `total_timeout_ms` is non-negative (rejected below 0) and, when > 0, a finite `std::chrono::milliseconds`; unknown keys are still rejected; the new extracted symbol `config::llm.default.retry.total_timeout_ms` has a `tests/fixtures/spec_symbol_catalog.json` entry so the mechanical gate passes. | Sec. 6, Sec. 6.5, 84-D9/84-D14 |

---

## 5. Failure modes

Component-local; disjoint from the shared F1-F12, 08's `L-F#`, and 41's `RE-F#`.
`RB-F#` is used only here.

| ID | Failure mode | Handling | Test |
|---|---|---|---|
| **RB-F1** | `derive_total` overflows `std::chrono::milliseconds::rep` for a huge `max_attempts` x `attempt_deadline` + envelope. | Satuating arithmetic: clamp to `numeric_limits<rep>::max()`; treat as a finite (very large) budget; RB2 still holds. | Sec. 7 T5 (unit on the helper) |
| **RB-F2** | `total_timeout_ms` is negative or non-numeric. | `read_int64` (`config.cpp:244-266`) already fails negatives ("value must be non-negative") and non-numbers; no code change beyond the schema key (RB10). | Sec. 7 T6 (config) |
| **RB-F3** | `total_timeout_ms > 0` but smaller than `request_timeout_ms` (or than the backoff envelope) suppresses retries. | Documented, deliberate: the overall bound wins; at most one attempt. Logged only as the ordinary terminal error (`:1359`). | Sec. 7 T4 |
| **RB-F4** | `request.deadline > 0` and `<= request_timeout_ms` suppresses retries. | Documented, deliberate (Sec. 2.4): a caller hard deadline may leave room for only one attempt. | Sec. 7 T4 |
| **RB-F5** | `retry.total_timeout == 0` and `request_timeout == 0` => unbounded overall budget. | Documented (RB5): bounded only by `max_attempts`; with `max_attempts` finite the worst case is `max_attempts x <unbounded per attempt>` (one hung attempt may never return). Operator responsibility; the same as the pre-existing `request_timeout = 0` semantics. | Sec. 7 T5 (semantics) |
| **RB-F6** | The backoff `delay >= remaining_overall`; retrying would exceed the bound. | Terminal `failed(error)` (algebra (g)); no sleep is started (bounded, RB2). | Sec. 7 T3 |
| **RB-F7** | A non-retryable error (e.g. `Auth`, `BadRequest`) is retried. | `is_retryable_code` gate unchanged (`:1349`); non-retryable codes are terminal. | Sec. 7 T7 |
| **RB-F8** | A retry after the first dispatched event duplicates `AssistantChunk`/`Usage`. | 08 L7 gate `!decoder.any_event()` unchanged (RB7). | Sec. 7 T8 (existing `NoRetryAfterFirstDispatchedEvent`, `openai_adapter_test.cpp:427-446`) |
| **RB-F9** | The derived budget is computed from the wrong per-attempt value (e.g. from `retry.total_timeout` when it was 0). | Algebra (c)/(d): derived uses `attempt_deadline`, not `total_timeout`; unit-tested as a pure function (RB8). | Sec. 7 T5 |
| **RB-F10** | Per-attempt `http.total_timeout` regresses to the full `attempt_deadline` on a later attempt and overruns the overall budget. | Algebra (e): `min(attempt_deadline, remaining_overall)`; RB6. | Sec. 7 T3 |

---

## 6. Config surface

### 6.1 Keys and location

New key (one, in the existing retry table):

| JSONC path | Type | Default | Meaning |
|---|---|---|---|
| `llm.default.retry.total_timeout_ms` | non-negative integer (ms) | `0` | Overall budget for one `stream()` call across all attempts and backoffs. `0` = derived (Sec. 2.2(c)). |
| `llm.endpoints.<name>.retry.total_timeout_ms` | non-negative integer (ms) | `0` | Same, per named endpoint (52-D1). |

The retry sub-table is shared by `llm.default` and every `llm.endpoints.<name>`
entry. The read path is already shared: `read_retry` (`config.cpp:588-599`) is
called for `llm.default` via `apply_retry` (`:601-606`) and for endpoints via
`apply_endpoint_entry` (`:663-665`). Add the field once in `read_retry` and it
loads for both.

### 6.2 Schema and validation

- Key allow-lists (both must gain `"total_timeout_ms"`):
  - the centralized retry-object key set in `reject_unknown_named_map`
    (`config.cpp:175-177`), which covers every named-map `retry` object; and
  - `apply_retry`'s explicit `reject_unknown` for `llm.default.retry`
    (`config.cpp:601-604`).
- Endpoint-entry key list (`config.cpp:742-746`) lists `"retry"` only; the retry
  object's own keys are checked by the centralized set above, so no change is
  needed there beyond `:175-177`.
- Validation: `read_int64` (`config.cpp:244-266`) rejects a negative value
  ("value must be non-negative for '...'") and a non-integer; no cross-field
  constraint is imposed (RB-F3/RB-F4 are documented consequences, not config
  errors). RB10.
- `llm.default`'s outer key list (`config.cpp:818-823`) lists `"retry"` only; the
  retry object's keys are covered by `:601-604`. No change there.

### 6.3 Defaults and first-run scaffold

- Built-in defaults: `include/ymh/config/config.hpp:222-228` (`RetrySettings`) and
  `include/ymh/llm/llm_provider.hpp:39-47` (`RetryPolicy`) each add
  `std::chrono::milliseconds total_timeout{0};`. Wiring copies it in
  `to_provider_config` (`src/cli/wiring.cpp:104-122`): add
  `provider.retry.total_timeout = endpoint.retry.total_timeout;` beside
  `:115-119`. `ResolvedEndpoint.retry` is already a `RetrySettings`
  (`config.hpp:540`), populated at `src/agent/model_selection.cpp:109`.
- First-run scaffold (`config.cpp:1515-1521`, the `ensure_global_config`/scaffold
  JSONC string): add the key inside the `"retry"` object:
  `"total_timeout_ms": 0,` with a trailing comment
  `// 0 = derived from max_attempts x request_timeout_ms + backoff`.
  The scaffold is emitted only at the conventional global path and is never
  auto-created for an explicit `--config` (21-config-jsonc-errata Sec. 6); no
  workspace-layer key is scaffolded.
- **Backward-sensible default justification (84-D3).** `0` = derived means:
  (i) existing configs and the scaffold get correctly retrying behavior with no
  edit; (ii) the bound stays finite and deterministic (`derive_total`), so 08-D18's
  "bounded, not multiplicative-forever" property is preserved; (iii) an operator
  who wants a tighter total sets a positive `total_timeout_ms` (e.g. `180000`);
  (iv) derived is unbounded only when the per-attempt deadline is itself `0`, where
  no tighter finite bound exists to derive. With shipped defaults
  (`max_attempts = 3`, `request_timeout_ms = 120000`, `base_delay_ms = 500`,
  `max_delay_ms = 30000`, `jitter = 0.25`) the derived budget is
  `3 x 120000 + ceil(500 x 1.25) + ceil(1000 x 1.25) = 360000 + 625 + 1250 = 361875 ms`.
- **User-visible worst case (L3).** This default deliberately raises the worst-case
  wall clock before the terminal `turn/fail` from one `request_timeout`
  (~120 s under 08-D18) to `max_attempts x request_timeout + envelope`
  (~361875 ms -- about 6 minutes -- with shipped defaults). That growth is the
  price of making `max_attempts` actually retry a full-length stall: to retry after
  an attempt that consumed its whole deadline, the overall budget must hold at
  least `max_attempts x attempt_deadline`. Operators who need a tighter stall
  ceiling set `retry.total_timeout_ms` explicitly (e.g. `180000` => at most one
  retry after a stalled first attempt) or lower `request_timeout_ms` (now a
  per-attempt knob that, unlike under 08-D18, does not disable retries). The
  supersession of 08-D18 is conscious and recorded (A1/A2, 84-D2/D6).

### 6.4 Environment and CLI

There is no `YMH_*` env override or CLI flag for retry fields today (env overrides
exist for other settings via `apply_env_overrides`, `config.hpp:524`). This spec
adds none; the key is config-file only, consistent with the other retry keys.

### 6.5 Mechanical gates triggered by the new key (M2, L5)

Adding `"total_timeout_ms"` to the `reject_unknown` allowlists makes it an
**extracted symbol** for the repo's mechanical spec-symbol catalog gate. The
implementing wave must, in the same change set:

- Add `config::llm.default.retry.total_timeout_ms` to
  `tests/fixtures/spec_symbol_catalog.json`, beside the existing retry keys at
  `:1170-1194` (category `config_key`, spec `84-retry-budget-per-attempt-deadline-errata.md`,
  section `6.1`). `tests/unit/spec_catalog_test.cpp:293-338` extracts every key from
  the `reject_unknown(...)` allowlists in `src/config/config.cpp`; the requirement
  test `EveryCodeSymbolHasACatalogEntry` (`:455-474`) fails the suite, naming the
  symbol, until the fixture entry exists. (Endpoint retry keys resolve to the same
  `config::llm.default.retry.<key>` symbol because `apply_retry` names the label
  `llm.default.retry`, so one entry covers both.)
- Update the retry-field equality enumeration in `tests/unit/config_test.cpp:96-100`
  to include `a.llm.retry.total_timeout == b.llm.retry.total_timeout`; T6 asserts
  it. Without this, a config round-trip test silently ignores the new field.

Both files join the "Code anchors the implementing wave must touch" list in the
header and are covered by T6.

---

## 7. Test plan

Strategy is 00 Sec. 44. The deterministic layers run offline; live tests are opt-in
(`YMH_LIVE_LLM=1`). No test uses sleep-based synchronization for the budget
arithmetic: the fake clock is injected through
`OpenAICompatibleProvider::SteadyNow` (`include/ymh/llm/openai_adapter.hpp:89,95`)
and advanced by `FakeTransport` (`scripts[i].delay`,
`tests/unit/openai_adapter_test.cpp:146-169`).

**Test seam (named, implementable).** `FakeTransport`
(`tests/unit/openai_adapter_test.cpp:134-192`) implements `HttpTransport`
(`include/ymh/llm/openai_adapter.hpp:62-70`) and scripts one `HttpResponse` per
call, including an arbitrary `transport_error` (the retryable `Timeout` at `:820-821`
is the exact shape from the incident). `make_provider` (`:210-229`) builds an
`OpenAICompatibleProvider` from `Options` (`:194-208`); `Options` must gain a
`total_timeout` field mapped at `:225`-adjacent into
`config.retry.total_timeout`. The provider's injected `now` (`:204-206, :831-834`)
reads the shared fake clock. This is the same seam the existing
`RetryBudgetIsBoundedByRequestDeadline` test (`:816-843`) already uses.

New/updated tests land in `tests/unit/openai_adapter_test.cpp` (the `OpenAiAdapterTest`
fixture) unless a new file is preferred; adding a file requires a `tests/CMakeLists.txt`
edit.

| Test | Covers |
|---|---|
| **T1 (regression, the fix) -- retryable deadline-consuming timeout retries then succeeds.** Script 1: `transport_error = {Timeout, "transport timeout", retryable}` and `delay = request_timeout` (or `delay = 500` with `request_timeout = 500`), consuming the whole per-attempt deadline. Script 2: a success SSE body. With `max_attempts = 3`, `request_timeout = 500`, `retry.total_timeout = 0` (derived). Assert `outcome == Completed`, `transport->requests.size() == 2`, and -- the old-code killer -- that the call did **not** fail after attempt 1. | RB1, RB3, RB4, 84-D1/D6 |
| **T2 -- `max_attempts` is reachable under the derived budget.** All 5 scripts retryable `Timeout` with `delay = request_timeout = 500`, `max_attempts = 5`, `retry.total_timeout = 0`. Assert `failed(Timeout)` and `requests.size() == 5` (the derived budget schedules every attempt). | RB4, RB2, 84-D3 |
| **T3 (budget exhausted) -- explicit overall bound stops retries.** Scripts: all retryable `Timeout` with `delay = 300`; `request_timeout = 500`, `retry.total_timeout = 500`, `max_attempts = 5`. Attempt 1 carries `total_timeout = min(500, 500) = 500`; after it (`elapsed 300`, `remaining 200`, backoff 1 ms fits) attempt 2 carries `total_timeout = min(500, 200) = 200`; after it (`elapsed 600 > 500`) the next retry is refused. Assert `failed(Timeout)`, `requests.size() == 2`, `requests[1].total_timeout < requests[0].total_timeout`, and `fake_now == 600`. This is the superseded `RetryBudgetIsBoundedByRequestDeadline` (`:816-843`) re-pinned to the **overall** key. | RB2, RB6, RB-F6/F10, 84-D2 |
| **T4 -- bound narrower than one attempt yields one attempt.** `request_timeout = 2000`, `retry.total_timeout = 500`, script retryable `Timeout` with `delay = 800`. Assert `requests.size() == 1` and `failed(Timeout)`. Mirrors `request.deadline = 500` in a companion case. | RB-F3, RB-F4, RB5 |
| **T5 -- helper unit tests (pure).** Table-drive the four header-declared helpers as `ymh::derive_total_budget` / `ymh::resolve_overall_budget` / `ymh::per_attempt_timeout` / `ymh::effective_timeout` (declared in `include/ymh/llm/openai_adapter.hpp`, so this TU compiles and links -- M1): `attempt_deadline = 0` => derived `0`; `max_attempts = 1` => `attempt_deadline`; the envelope arithmetic; `request.deadline > 0` clamping the overall; saturation (RB-F1); `configured == 0 && deadline > 0` returns `deadline` (the dormant-arm fix); `per_attempt_timeout` with `elapsed >= overall` returns `0` (the clamped arm, L1). No provider, no transport. | RB5, RB8, RB-F1/F5/F9 |
| **T6 -- config parse + catalog.** `retry.total_timeout_ms` accepted for `llm.default` and an endpoint entry; negative value is a `ConfigError`; unknown retry key still rejected; absent key => `0`; the scaffold string parses and contains the key; the retry-field equality helper compares the new field. Additionally the implementing wave **must** add `config::llm.default.retry.total_timeout_ms` to `tests/fixtures/spec_symbol_catalog.json` (beside the existing retry keys at `:1170-1194`) so the mechanical gate `tests/unit/spec_catalog_test.cpp:293-338, :460-476` (`EveryCodeSymbolHasACatalogEntry`) passes; the entry names spec `84...md` Sec. 6.1 (M2). (`tests/unit/config_test.cpp:96-100` for the equality helper; Sec. 6.5.) | RB10, RB-F2, Sec. 6 |
| **T7 -- non-retryable path.** Script a `BadRequest`/400; assert `requests.size() == 1` and `failed(BadRequest)` (no retry). | RB-F7, 08 Sec. 3.7 non-retryable list |
| **T8 -- L7 preserved (existing).** `NoRetryAfterFirstDispatchedEvent` (`:427-446`) and the 429-retry-before-first-event test (`RetriesBeforeFirstEventThenSucceeds`, `:448+`) stay green; a retry after a dispatched event must remain terminal. | RB7, RB-F8 |

Existing tests that must be **updated** (they encode 08-D18):
`RetryBudgetIsBoundedByRequestDeadline` (`:816-843`) is superseded by T3. As
shipped it sets `request_timeout = 500` with no `total_timeout`, so under the new
semantics `500` is the per-attempt deadline and the derived budget (2507 ms)
schedules all 5 scripted attempts: its `requests.size() == 2` expectation (:840)
and `fake_now == 600` (:842) no longer hold. Re-pin the test to
`retry.total_timeout = 500` (T3) to keep the same bounded-two-attempt shape; with
that move the `EXPECT_LT(requests[1].total_timeout, requests[0].total_timeout)`
ordering assertion (:841) holds again (200 < 500). All other `OpenAiAdapterTest`
retry/timeout tests (curl pre-first-byte/idle tests at `:760-814`, `:845-860`) are
unaffected. `tests/unit/config_test.cpp` gains T6 and its retry-field equality
enumeration (`:96-100`) gains `total_timeout` (L5). `tests/fixtures/spec_symbol_catalog.json`
gains the `config::llm.default.retry.total_timeout_ms` entry (M2, Sec. 6.5).

Live layer (opt-in, real DeepSeek, PTY-driven): reuse the existing live wiring;
no new live test is required by this spec, but a live smoke that a transient
timeout retries is acceptable and stays opt-in.

---

## 8. dsh (DeepSeek Harness) mapping

dsh is the architectural reference (00 Sec. 55; 08 Sec. 12). This errata is
implementation-internal: it changes how ymh bounds one provider call's retries,
not a dsh capability boundary. Per 56-D6, every non-mirror cell below carries a
concrete anchor.

| dsh concept | ymh equivalent | Disposition |
|---|---|---|
| LLM seam / model plugin (00 Sec. 55 :4878; 08 Sec. 12 :1159) | `LLMProvider`, `OpenAICompatibleProvider` | **Mirrored, unchanged.** The retry budget stays behind the same seam; no new provider-facing type. Anchor: `include/ymh/llm/llm_provider.hpp:61-74`; 00 Sec. 55 :4866-4884. |
| streaming model events (08 Sec. 12 :1160) | `StreamEvent` algebra | **Mirrored, unchanged.** No event added or changed; retries stay pre-first-event (L7). Anchor: 08 Sec. 10 :1047-1050. |
| request cancellation (08 Sec. 12 :1163) | `CancellationToken` into `postStream`; `sleep_with_cancel` | **Mirrored, unchanged.** The new budget does not alter cancellation checkpoints. Anchor: `openai_adapter.cpp:1288-1302, :1361-1363, :288-300`. |
| append-only trace (08 Sec. 12 :1167; 00 Sec. 55 :4864) | session event log; wire transcript (74) | **Mirrored, unchanged.** Retries remain non-durable; only the transcript records attempts. Anchor: `src/agent/agent_loop.cpp:1387-1391`; `openai_adapter.cpp:1287-1290`. |
| provider config / profiles (08 Sec. 12 :1166) | `LLMProviderConfig.retry` via config layers | **Mirrored, tightened additively.** One new field, same layering. Anchor: `include/ymh/llm/provider_registry.hpp:29-48`; 21-config-jsonc-errata Sec. 6. |
| per-attempt vs total retry budget | `attempt_deadline` + `overall_budget` | **ymh-internal scheduling policy; not a dsh-specified boundary.** dsh's comparison table (00 Sec. 55 :4866-4884) has no retry-budget row and its omitted list (00 Sec. 55 :4886-4894) does not mention retries; ymh owns this internally behind the `LLMProvider` seam, so there is no dsh capability to mirror. Anchor: 08 Sec. 12 :1169-1172 ("a small, stateless, headless seam -- not a plugin runtime"); 00 Sec. 55 :4886-4894. |
| capability negotiation / provider purity (08 L1, L13) | provider reads only config + request, owns no thread | **Mirrored, unchanged.** The budget is computed from `config_.retry` and the request; no cross-session state. Anchor: 08 Sec. 10 :1021-1024, :1071-1073. |

**Deliberate omission, with reason.** ymh does not model a dsh-style provider-owned
retry scheduler; retries remain adapter-local and pre-first-event. Reason: the seam
is deliberately stateless and headless, and the durable step-boundary retry owner
(spec 41) is a *different, unactivated* layer. Anchor: 08 Sec. 12 :1169-1172;
41 Sec. 1.5.

---

## 9. Decisions (84-D#)

- **84-D1 -- The per-attempt deadline is `request_timeout_ms`, applied fresh to
  every attempt.** `attempt_deadline = effective_timeout(request.deadline,
  retry.request_timeout)`; each attempt's transport deadline is derived from it,
  not from a shared decremented pool. Default value (120 s) unchanged; **meaning
  narrowed from "total wall time for the request" to "per-attempt wall time"**
  (amends 08 Sec. 3.8 :502-506). (RB1)
- **84-D2 -- A new, separate overall budget bounds the whole call.**
  `RetrySettings`/`RetryPolicy` gain `total_timeout` (ms); the overall budget covers
  every attempt plus every backoff sleep. Supersedes 08 Sec. 3.7 :490-496 and
  08-D18 (08 Sec. 14.1(w) :1409-1419). (RB2)
- **84-D3 -- Default `total_timeout_ms = 0` means derived.** Derived =
  `max_attempts x attempt_deadline + backoff_envelope`, or `0` (unbounded) when
  `attempt_deadline == 0`. Rationale in Sec. 6.3. With shipped defaults the derived
  budget is 361875 ms. (RB4)
- **84-D4 -- `LLMRequest.deadline` caps both levels.** It caps the per-attempt
  deadline (via the amended `effective_timeout`, whose only behavioral change is the
  dormant `configured == 0 && deadline > 0` arm) and, when non-zero, `min`-caps the
  overall budget. `deadline == 0` leaves both governed by config/derived. (RB5,
  Sec. 2.4)
- **84-D5 -- An attempt never outlives the overall budget.**
  `http.total_timeout = overall_budget > 0 ? min(attempt_deadline, remaining_overall)
  : attempt_deadline`. (RB6)
- **84-D6 -- The retry decision checks the OVERALL budget, not the consumed
  attempt.** A retryable pre-first-event failure retries iff `overall_budget == 0`
  or `remaining_overall > delay`; the old `delay >= budget`-after-attempt-1 kill is
  removed. This is the fix. (RB3)
- **84-D7 -- 08-D17 (mid-stream idle guard) is unchanged.** No supersession.
  Justification in Sec. 2.5: the defect is the budget, re-enabling the guard would
  regress the false-abort 08-D17 removed, and the new overall budget still bounds
  the deferred failure.
- **84-D8 -- (recorded, not adopted) Re-enable the mid-stream idle guard as a
  belt-and-braces bound.** Rejected; would supersede 08-D17 and re-create the
  false abort (Sec. 2.5).
- **84-D9 -- Config key and validation.** `llm.default.retry.total_timeout_ms` and
  `llm.endpoints.<name>.retry.total_timeout_ms`, non-negative integer ms, default
  `0`; added to the retry key allow-lists; negative rejected by `read_int64`; no
  cross-field constraint. (RB10, Sec. 6)
- **84-D10 -- Scaffold.** The global first-run scaffold's `retry` object gains
  `"total_timeout_ms": 0` with a comment. No workspace-layer scaffold. (Sec. 6.3)
- **84-D11 -- No durable/event/schema change.** No new event, field, schema, or
  persisted artifact; retries stay transcript-observable and non-durable.
  (RB9, Sec. 2.6)
- **84-D12 -- Supersession/amendment set.** A1-A8 (Sec. 1.4): 08 Sec. 3.7 :490-496
  and 08-D18 superseded; 08 Sec. 3.8 :502-508, 08 Sec. 5.3 :737-753, and 41 Sec. 4.2
  :267-275 amended; the implementing-wave code anchors and the `08` DESIGN_STATUS
  row text are updated at promotion.
- **84-D13 -- Helper placement and linkage.** The four budget helpers are declared
  in `include/ymh/llm/openai_adapter.hpp` (`namespace ymh`, after `backoff_delay`
  at `:130`) and defined in `src/llm/openai_adapter.cpp` after the anonymous
  namespace closes (`:906`), mirroring `backoff_delay` (`openai_adapter.hpp:130` /
  `openai_adapter.cpp:927`). They are **not** in the anonymous namespace, so T5's
  direct test calls compile and link. Each helper's contract (including the
  `per_attempt_timeout` `elapsed >= overall` clamp to `0`) is pinned in Sec. 2.3.
  (M1, RB8)
- **84-D14 -- Mechanical catalog gate.** The implementing wave adds
  `config::llm.default.retry.total_timeout_ms` to `tests/fixtures/spec_symbol_catalog.json`
  (`:1170-1194`) and `total_timeout` to the retry equality enumeration at
  `tests/unit/config_test.cpp:96-100`, or the suite fails
  (`tests/unit/spec_catalog_test.cpp:293-338, :460-476`). (M2/L5, Sec. 6.5)

---

## 10. Open questions

- **84-OQ-1.** Should `total_timeout_ms` also be exposed as a `YMH_*` env override
  or CLI flag, for symmetry with the other settings? Deferred: no other retry key
  has one (Sec. 6.4); add only if a caller needs it.
- **84-OQ-2.** Should the derived budget include the backoff envelope, or only
  `max_attempts x attempt_deadline`? This spec includes the envelope (RB4 gives the
  schedulability guarantee); revisit only if the bound proves too loose for a
  user-visible stall.

Neither blocks the fix.

---

## 11. References

- `docs/design/08-llm-provider.md` Sec. 3.7 :451-496, Sec. 3.8 :498-528, Sec. 5.3
  :729-758, Sec. 10 :1017-1093, Sec. 12 :1152-1172, Sec. 14.1 :1338-1419.
- `docs/design/00-architecture.md` Sec. 54 D5 :4756-4758, Sec. 55 :4860-4894.
- `docs/design/41-retry-executor.md` Sec. 1.5, Sec. 4.2 :252-279.
- `docs/design/21-config-jsonc-errata.md` Sec. 6.
- `docs/design/56-dsh-fidelity-gap-closure-errata.md` Sec. 3 (56-D6).
- Code: `src/llm/openai_adapter.cpp` :280-286, :784-796, :1040-1112, :1233-1371;
  `include/ymh/llm/openai_adapter.hpp` :42-117;
  `include/ymh/llm/llm_provider.hpp` :39-85;
  `include/ymh/llm/provider_registry.hpp` :29-48;
  `include/ymh/llm/llm_request.hpp` :56-65;
  `include/ymh/config/config.hpp` :222-228, :244-247, :269-308, :530-542;
  `src/config/config.cpp` :143-182, :244-266, :588-606, :640-665, :740-746,
  :790-928, :1496-1525;
  `src/cli/wiring.cpp` :104-129;
  `src/agent/model_selection.cpp` :93-112;
  `src/agent/agent_loop.cpp` :1387-1391;
  `src/core/event.cpp` :24, :30;
  `include/ymh/session/events.hpp` :126-132;
  `tests/unit/openai_adapter_test.cpp` :134-250, :427-460, :792-843, :1094-1129.

---

## 12. Revision log

- **Rev 1 (2026-10-16).** Initial draft. Decouples the per-attempt deadline from
  the overall retry budget (new `retry.total_timeout_ms`, default 0 = derived),
  supersedes 08 Sec. 3.7 :490-496 and decision 08-D18, amends 08 Sec. 3.8/5.3 and
  41 Sec. 4.2, keeps 08-D17 unchanged, and pins the config surface, invariants
  `RB1`-`RB10`, failure modes `RB-F1`-`RB-F10`, state lifetime, dsh mapping, and the
  hermetic `FakeTransport` test plan.
- **Rev 2 (2026-10-16).** Closes the gate report (`/tmp/opencode/gate-84.md`,
  0 HIGH / 2 MEDIUM / 5 LOW). M1: the four budget helpers are moved out of the
  anonymous namespace and declared in `include/ymh/llm/openai_adapter.hpp` (mirror
  `backoff_delay`), so T5 compiles and links (84-D13, Sec. 2.3); the header is added
  to the anchors list. M2: pinned the spec-symbol catalog entry for
  `config::llm.default.retry.total_timeout_ms` and the `config_test.cpp` equality
  field, with both test files added to the anchors list (84-D14, Sec. 6.5). L1:
  `per_attempt_timeout` clamps `elapsed >= overall` to `0`, pinned in algebra (e)
  and the contract. L2: RB4 scoped to the schedule without a `Retry-After`
  override. L3: worst-case stall growth (120 s -> ~361875 ms) stated explicitly
  with operator guidance. L4: A5 points at the JSONC surface, not the retired-TOML
  example. L5: config equality enumeration covered. Revision 1 -> 2.

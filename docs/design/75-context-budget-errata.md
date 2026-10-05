# 75 — Context Budget Errata: The Default Window, Reserve Sizing, and a Conservative Estimator (spec-13 / spec-18 amendment)

Status: **verified (Rev 1)** · reviewer: independent review team, round 3 (2026-10-05): PASS · gate: 0 HIGH / 0 MEDIUM (6 LOW, recorded in `DESIGN_STATUS.md` row 75)

```
Component: 75 (errata) — amends `13-context-compaction.md` §3.2/§5.6 and
           `18-context-errata.md` §3.3/§5.6 **by reference**; it does not edit
           `13` or `18` in place (the `39` convention). It also amends the
           `CompactionSettings` → `CompactionPolicy` wiring (`src/cli/wiring.cpp`)
           and the `DefaultTokenEstimator` / tool-schema arithmetic.
Depends on: `13-context-compaction.md` (verified) §3.2 :219-302 (`effective_
            threshold_tokens` :289-298, the "disabled rather than guessed" pin
            :245-248), §5.6 :823-843 (:835);
            `18-context-errata.md` (verified) §3.3 :416-533 (the documented
            estimator :444, the pinned tool-schema formula :507-527), §5.6
            :1339-1360;
            `32-compaction-errata.md` (verified Rev 1) §2.1, §4.1 (the
            `Pressure`/`ContextOverflow` taxonomy and `compact_if_needed`/
            `compact_now`);
            `73-non-action-guard-and-replay-dedup-errata.md` (draft Rev 3) — the
            non-adherence guard 75-D6 defers to;
            `00-architecture.md` §37 (config layering), §40 (logging);
            `08-llm-provider.md` §3.1;
            the working tree at authoring time:
            `include/ymh/agent/compactor.hpp` :34-65, :69-72 (doc 13 pins the
            same struct at 13 §3.2 :230-302);
            `src/agent/context_assembler.cpp` :29-33 (the estimator);
            `src/agent/context_snapshot.cpp` :16-17, :88-92;
            `src/agent/compactor.cpp` :445-447, :271-282;
            `src/agent/agent_loop.cpp` :1250-1265 (the proactive `Pressure`
            trigger), :1384-1398 (the reactive `ContextOverflow` retry);
            `src/cli/wiring.cpp` :49, :259-261, :289-312;
            `include/ymh/cli/wiring.hpp` :46;
            `include/ymh/config/config.hpp` :92-105, :244-257, :537-552;
            `src/config/config.cpp` :343-360, :651-663, :2364-2430;
            `src/host/host_runtime.cpp` :605-613;
            `src/ui/ui_render.cpp` :1820-1842;
            `src/ui/supervisor.cpp` :3587-3593;
            `include/ymh/llm/llm_provider.hpp` :36;
            `src/llm/provider_registry.cpp` :91-100.
Scope: default the compaction window, size the output reserve from the model's
       own generation cap, raise the token estimator's divisor (4 → 3) to narrow
       its under-count (the safety margin is carried by the threshold ratio), and
       make the startup validation loud — so compaction fires before an
       OpenAI-compatible provider silently truncates the request head. Budgeting
       only; detecting a provider that answers `200` while ignoring the prompt is
       explicitly out of scope (75-D6) and owned by `73` Rev 3.
Supersedes: `13-context-compaction.md` §3.2 (:245-248, decision C-D8), "when the
            window is unknown and `threshold_tokens == 0`, compaction is disabled
            rather than guessed"; the `DefaultTokenEstimator` divisor documented
            in `18` §3.3 (:444) and `13` §5.6 (:835); and the default
            `reserve_output_tokens = 4096` in `13` §3.2 (:251) /
            `config.hpp` :97 as the *effective* reserve whenever the resolved
            model declares a larger `max_tokens`. It does **not** supersede `32`'s
            trigger taxonomy (`Pressure` / `ContextOverflow`), which 75-D5 makes
            reachable again.
```

---

## 1. Purpose and the incident

This errata is written because a live deployment silently stopped respecting its
system prompt, and ymh's own budget arithmetic guaranteed it could not notice.

The incident (captured in a spec-74 wire transcript): an OpenAI-compatible
endpoint `DSA-Flash-CODE` at `105.140.238.68:4000` advertised **max input 320,000
tokens / output 32,000**. Deep into a session the model answered every user
`yes` with its base identity ("I'm DSA LLM, an AI assistant powered by Samsung
DSA."), 16 times. ymh's request body contained the correct system prompt
(`You are ymh, a terminal coding agent.` — `default_system_prompt()` at
`src/cli/wiring.cpp:48-52`, 178 bytes), so the model had stopped *seeing* it: the
provider silently truncated the request **head** when the payload crossed its
real window. Every response was `status=200, finish=stop, tool_calls=0` — there
was no provider error, so the reactive `ContextOverflow` path could not fire.

The measured transition: the fallback begins when the payload crosses ~860 KB
(`ordinal 7`: 857,534 chars answered normally; `ordinal 8`: 860,882 chars ⇒
DSA), and stops after a shrink to 714,728 chars. At ~2.7 bytes/token, 860 KB
≈ 318,500 ≈ 320 k tokens — the model's real input ceiling.

ymh believed it had ample room. `/context` printed:

```text
used 213,801 / window 350,000 (61.0%)  ...  threshold 276,723  reserve 4,096
```

The window `350,000` came from `agent.compaction.context_window_tokens` in the
deployment's config, not from ymh source (`350000` appears nowhere in the
repository — verified by `rg`). It is larger than the model's real 320,000 input
limit, but even that is not the core defect: **the estimator and the reserve were
wrong too, so compaction would not have fired in time even with the correct
window.** §2 shows the arithmetic.

## 2. Root cause (file:line anchored)

Three defects compound; each alone is survivable, together they make the
compaction window unreachable.

**(a) The estimator under-counts.** `DefaultTokenEstimator::estimate` is
`bytes/4 + 4·messages` (`src/agent/context_assembler.cpp:29-33`). The tool-schema
estimator uses the same divisor, documented at `18 §3.3 :511` and implemented at
`src/agent/context_snapshot.cpp:16-17` (`kSchemaBytesPerToken = 4`). Real
OpenAI-compatible traffic runs ~2.7 bytes/token on dense code/JSON/tool output,
i.e. the estimate is low by a factor of ~1.3-1.5×. An estimator used to trigger a
**safety** action must not under-count; this one under-counts by ~1.5×.

**(b) The output reserve is too small.** `CompactionPolicy::reserve_output_tokens`
defaults to `4'096` (`include/ymh/agent/compactor.hpp:42`,
`include/ymh/config/config.hpp:97`), while a model's generation cap is
`resolved.max_tokens` (`include/ymh/config/config.hpp:541`), wired to
`GenerationParameters::max_output_tokens` at `src/cli/wiring.cpp:259-261`. In the
incident that cap was `32'768` — 8× the reserve. On a shared input+output window
the provider's real input budget is `window − max_tokens`; reserving 4,096 while
the model may emit 32,768 leaves 28,672 tokens unaccounted for.

**(c) An unknown / wrong window silently disables or inflates the trigger.** The
ratio trigger is `0.80·(context_window − reserve_output)`
(`include/ymh/agent/compactor.hpp:55-64`). `to_compaction_policy` sets the window
from `resolved.context_window.value_or(settings.context_window_tokens)`
(`src/cli/wiring.cpp:296`); when both are unset it is `0`, so
`effective_threshold_tokens() == 0` and `is_enabled()` is false
(`compactor.hpp:51-53`). `ContextCompactor::compact_if_needed` then no-ops **for
every trigger** (`src/agent/compactor.cpp:445-447`), killing `ContextOverflow`
recovery as well. The one truthful provider-side source,
`ProviderCapabilities::max_context_tokens` (`include/ymh/llm/llm_provider.hpp:36`),
is never populated by the built-in provider
(`src/llm/provider_registry.cpp:91-100`), so an unconfigured session has no
budget at all.

**The arithmetic that makes truncation unavoidable.** Take the incident's real
window 320,000 and the *correct* reserve 32,768:

| Configuration | threshold (tokens) | threshold (bytes at estimator divisor) | vs 860 KB truncation point |
|---|---|---|---|
| incident: window 350,000, reserve 4,096, divisor 4 | 276,723 | 1,106,892 B (≈ 1.08 MB) | **above** — never fires |
| window 320,000, reserve 32,768, divisor 4 | 229,785 | 919,140 B (≈ 919 KB) | **above** — never fires |
| window 320,000, reserve 32,768, divisor 3 (75) | 229,785 | 689,355 B (≈ 689 KB) | **below** — fires in time |

`350,000` yields `floor(0.80·(350000−4096)) = 276,723` — exactly the threshold
`/context` printed, confirming the accounting. The middle row is the key point:
the incident report's "even with a correctly configured 320,000 window and the
bytes/4 estimator, the threshold (~919 KB) sits above the truncation point
(~860 KB)" is reproduced. Fixing the window alone is insufficient; the divisor
and the reserve must move together (75-D2, 75-D3).

## 3. Design decisions

### 75-D1 — Default the window, once, loudly

When **both** the resolved model `context_window` and
`[agent.compaction].context_window_tokens` are unset, the policy window becomes a
conservative built-in default:

```cpp
// include/ymh/agent/compactor.hpp
// 75-D1: the window assumed when neither the resolved model nor
// [agent.compaction] names one. A policy choice set at the smallest context
// class ymh targets for code, NOT at the largest common window: a default above
// a model's real ceiling would reintroduce the silent truncation 75 fixes.
inline constexpr std::size_t kDefaultContextWindowTokens = 32'768;
```

and `to_compaction_policy` emits a **one-time** `WARN` (via
`category_logger(LogCategory::Agent)`; see `src/host/host_runtime.cpp:1002-1003`
for the existing call shape) naming the assumed value and **both** override
sites:

```text
context window unknown; assuming 32768 tokens
  (override via llm.models.<name>.context_window or
   agent.compaction.context_window_tokens)
```

`32'768` is a policy choice, not a model fact. It is deliberately the
**smallest** context class ymh targets for code, not the largest (128k): a
default above a model's real ceiling would recreate exactly the head-truncation
75 exists to stop, and that risk is unbounded (any sub-32k model), whereas a
default that is too small only makes an unconfigured session compact early
(~22,937 estimated tokens; see §9) — a benign cost, never a truncation. The
honest trade-off: a deployment running a model with a context window **below**
32k must set `llm.models.<name>.context_window` or
`agent.compaction.context_window_tokens`; the one-time WARN names both sites.
This **supersedes** `13 §3.2 :245-248` ("compaction is disabled rather than
guessed"): a documented, warn-once conservative guess is strictly safer than
silently disabling the only guard.

**Consequence for the master switch.** `to_compaction_policy` already computes
`enabled = settings.enabled || threshold_tokens > 0 || context_window_tokens > 0`
(`src/cli/wiring.cpp:305-306`). Because 75-D1 makes `context_window_tokens > 0`
in the normal path, compaction is now **on by default**. `13`'s C7
(`enabled == false ⇒ no compaction, ever`) remains true of the raw
`CompactionPolicy` struct; it is the config-level off-switch that loses its old
force. This is a deliberate safety direction (75-D5) and is logged when the
window is assumed; see OQ-75-1.

### 75-D2 — Size the reserve from the model's own generation cap

In `to_compaction_policy`:

```cpp
// 75-D2: the reserve must cover the generation cap the request advertises, or
// a shared input+output window is over-committed by (max_tokens - reserve).
if (resolved.max_tokens.has_value() && *resolved.max_tokens > 0) {
    policy.reserve_output_tokens =
        std::max(policy.reserve_output_tokens,
                 static_cast<std::size_t>(*resolved.max_tokens));
}
```

`resolved.max_tokens` is the value placed on the wire as
`max_output_tokens` (`src/cli/wiring.cpp:259-261`), so it is the binding output
reservation. Justification for the mismatch: the shipped default reserve is
`4'096` (`13 §3.2 :251`, `config.hpp:97`) while the incident's model advertised
`32'768`; the 28,672-token gap is exactly the un-reserved headroom the provider
consumed by truncating the input instead. When `resolved.max_tokens` is unset or
zero, the settings value is kept unchanged (no inference from the window). A
reserve that would swallow the window is bounded by 75-D4, not accepted.

### 75-D3 — Raise the estimator divisor (4 → 3) to narrow the under-count

`DefaultTokenEstimator::estimate` changes its divisor to `kBytesPerToken = 3`,
the tool-schema estimator changes `kSchemaBytesPerToken` to `3`, and the
summary-bounding helper in `ContextCompactor::bound_summary`
(`src/agent/compactor.cpp:271-282`, also `kBytesPerToken = 4`) must move in
lockstep because it converts the same token budget (`max_summary_tokens`) into a
byte cap. This is a **narrowing of the under-count, not a strict over-estimate**:
the incident measured ~2.7 bytes/token, so divisor 3 is still ~11% low
(`3/2.7 = 1.111`). The safety margin is therefore carried by the threshold
ratio, not by the estimator. The trigger fires at
`E = ratio·(window − reserve)`; for `b = 2.7` the real tokens there are
`R = (3/2.7)·E = 1.111·E`, so with the default `ratio = 0.8`,
`R = 1.111·0.8·window = 0.889·window < window`. The combined design is safe for
content at or above `ratio·divisor = 2.4 B/token`, which the incident's 2.7
satisfies; content denser than that is a residual risk tracked as OQ-75-5.

Concretely: 97 bytes of schema now estimate `97/3 + 8 = 40` tokens (was
`97/4 + 8 = 32`); a 400,000-byte conversation now estimates ≈133,333 tokens (was
100,000), i.e. ~1.33× — enough to cross the threshold before the ~860 KB hard
ceiling when combined with a correct window and reserve.

**Amendments this forces.** This errata **supersedes** the documented formula in
`18 §3.3 :511` and `18 §3.3 :444` (`floor(bytes/4) + 4·count`) and `13 §5.6
:835`; those texts must be restated as `floor(bytes/3) + 4·count`. Existing tests
and fixtures that encode divisor 4 must be updated:

- `tests/unit/context_snapshot_test.cpp:97-98` — `32u` → `40u` and
  `bytes / 4 + 8` → `bytes / 3 + 8`; also the `32u` assertion at `:116`.
- `tests/unit/compaction_test.cpp:317-337`
  (`ContextCompactorTest.TruncatesSummaryAtTokenBound`) — `bound_summary`
  (`src/agent/compactor.cpp:271-282`) caps the summarizer output at
  `(max_summary_tokens − 4)·kBytesPerToken`. With `max_summary_tokens = 5` the cap
  drops from `(5−4)·4 = 4` bytes to `(5−4)·3 = 3`, so the assertion changes from
  `summary.size() == 4u` / `summary == "abcd"` (`:335-336`) to `3u` / `"abc"`.
- `18 §5.6 :1339-1360` — the worked example's `3,980` / `2,240` builtin/MCP
  schema sums are documented as derived from the formula; the derivation claim
  must be restated (a changed divisor changes those sums) or the constants
  re-derived. The golden/snapshot test fixtures that *inject* `3980` / `2240` /
  `33540` as constants (`tests/unit/context_snapshot_test.cpp:290-299`,
  `tests/unit/ui_render_golden_test.cpp:1276-1286`) are inputs, not outputs, and
  therefore do **not** have to change; only the doc's "produced by summing this
  formula" sentence becomes false. (No `tests/unit/errata18_*` target exists; the
  `18` coverage lives in `context_snapshot_test.cpp` and
  `ui_render_golden_test.cpp`.)

Verified by inspection (the full `tests/` tree was grepped for the divisor and
for byte-derived constants): the injected golden fixtures are
`context_snapshot_test.cpp:290-299`, `ui_render_golden_test.cpp:1276-1286`, and
the second injected set at `ui_render_golden_test.cpp:1432-1435` (same
`3980`/`2240`/`1128` constants); the byte-*derived* pins are
`context_snapshot_test.cpp:97-98`/`:116` and
`compaction_test.cpp:317-337`, both listed above. Every other
`DefaultTokenEstimator` use in `compaction_test.cpp` and
`compaction_trigger_test.cpp` is a relative `estimator.estimate(expected)`
comparison or against fixed thresholds (`1`, `1'000'000`), so those are
divisor-agnostic; the golden fixtures inject their token counts directly and are
unaffected.

### 75-D4 — Startup validation must be loud and must never disable compaction

`to_compaction_policy` normalizes the policy in this exact order:

1. **Assumed window** (75-D1): if the window is `0`, set it to
   `kDefaultContextWindowTokens` and emit the one-time `WARN`.
2. **Reserve covers the cap** (75-D2): raise the reserve to `resolved.max_tokens`
   when that is set and positive.
3. **Validate `threshold_ratio`**: reject a non-finite, `<= 0`, or `> 1` value
   with `ConfigError` naming the value. `threshold_ratio` is read by `read_double`
   (`src/config/config.cpp:228-241`, applied at `:353-354`) with no range check,
   and `effective_threshold_tokens()` (`include/ymh/agent/compactor.hpp:55-64`)
   multiplies it unguarded, so a `NaN`/negative ratio would produce a garbage
   `std::size_t` and `ratio == 1.0` would let the threshold reach the window.
   This is one of two loud fail paths.
4. **Hard-minimum window**: if `context_window_tokens <
   kMinContextWindowTokens`, **throw `ConfigError`** naming the value and the
   minimum. This is the second loud fail path; it is unreachable in a normal run
   because 75-D1 supplies `32'768`. Define `kMinContextWindowTokens = 4'096` — a
   window that cannot even hold the default reserve plus a turn is not a usable
   coding context.
5. **Bounded reserve clamp (never disable)**: if `context_window_tokens <=
   reserve_output_tokens` after steps 1–2, emit a `WARN` and clamp
   `reserve_output_tokens = min(reserve_output_tokens,
   context_window_tokens / 2)`, leaving at least half the window for input. When
   the overflow is caused by `resolved.max_tokens >= context_window_tokens`, the
   `WARN` additionally states that the clamped reserve no longer covers the
   advertised generation cap (`max_tokens`), so the operator must raise
   `context_window_tokens` or lower `max_tokens` (75-F3). Compaction stays
   **enabled** with a positive threshold; the window is never zeroed.
6. **Absolute threshold below the window**: if `threshold_tokens > 0` and
   `threshold_tokens >= context_window_tokens`, emit a `WARN` and clamp
   `threshold_tokens = context_window_tokens - 1`.
7. **Materialize the ratio trigger (clamp by construction)**: if
   `threshold_tokens == 0`, compute `candidate = static_cast<std::size_t>(
   threshold_ratio · (context_window_tokens − reserve_output_tokens))` from the
   now-validated ratio, then set `threshold_tokens = clamp(candidate,
   kMinEffectiveThresholdTokens, context_window_tokens − 1)` with
   `kMinEffectiveThresholdTokens = 1`, emitting a `WARN` when the clamp changed
   the value. After this step every wired policy carries an absolute
   `threshold_tokens ∈ [1, window − 1]`, so `effective_threshold_tokens()` returns
   exactly that value and cannot be driven to `0` or `>= window` by any ratio.
8. **Enable**: `enabled = settings.enabled || threshold_tokens > 0 ||
   context_window_tokens > 0`.

The existing `max_summary_bytes <= PersistenceConfig::max_payload_bytes`
validation (`src/cli/wiring.cpp:307-310`) runs **first**, before any return or
throw, so the oversized-summary error is unchanged. The rule "never silently run
with `context_window_tokens == 0`" holds structurally: a normal run gets
`32'768`; a window below the hard minimum aborts loudly; a window `<= reserve` is
clamped and kept enabled. There is no path that turns compaction off.

### 75-D5 — Invariants of a wired policy

For every `CompactionPolicy` returned by `to_compaction_policy` with
`context_window_tokens > 0`:

- `context_window_tokens >= kMinContextWindowTokens`;
- `context_window_tokens > reserve_output_tokens`;
- `effective_threshold_tokens() > 0` and
  `effective_threshold_tokens() < context_window_tokens` **strictly**; and
- `is_enabled()` is true (construction sets `enabled = true` whenever the window
  is positive — the existing `wiring.cpp:305-306` OR).

Construction. 75-D1 makes the window positive and the hard minimum (75-D4
step 4) makes it at least 4,096. Then:
- **`window > reserve_output_tokens`.** Step 5 fires exactly when `window <=
  reserve` and then sets `reserve = window/2 < window`; when `window > reserve`
  already holds it is untouched. The relation holds on both branches.
- **`effective_threshold_tokens() > 0` and `< context_window_tokens`.** Step 7
  materializes the ratio path: from a ratio validated into `(0, 1]` by step 3,
  `candidate = floor(ratio·(window − reserve))` is finite with
  `0 <= candidate <= window` (because `reserve >= 0`), and the explicit
  `clamp(candidate, 1, window − 1)` lands `threshold_tokens ∈ [1, window − 1]`.
  On the absolute path, step 6 clamps any `threshold_tokens >= window` to
  `window − 1` (positive and already `< window`). In both cases
  `effective_threshold_tokens()` returns that absolute value, so positivity and
  the strict upper bound hold **by construction** — they do not depend on the
  literal `0.8` nor on a `reserve <= window/2` assumption. The
  `window = 10,000, reserve = 9,999` case (where step 5 does **not** fire) is
  handled by step 7's floor.
`13`'s C7 remains
authoritative for a hand-built struct: `enabled == false` still disables it, so
the raw struct can be `window > 0 && !is_enabled()` outside the wiring seam;
75-D5 binds the wired policy, which is the only one the daemon injects. The
predicate `context_budget_consistent()` (§8) is called in `to_compaction_policy`
immediately before `return` as the production witness, and is also exercised by
`75-T5`.

Note the previously-unsound edge the predicate must reject: a struct with
`context_window_tokens = 1, reserve_output_tokens = 0` satisfies `window > 0` and
`window > reserve`, but `effective_threshold_tokens() = floor(0.8·1) = 0`, so
`is_enabled()` is false. The predicate therefore also requires
`effective_threshold_tokens() > 0`, and the wired path cannot produce that case:
the 4,096 hard minimum rules out `window = 1`, and step 7's `[1, window − 1]`
clamp rules out `effective == 0`. A user `ratio == 1.0` cannot defeat the witness
either: step 3 rejects `ratio > 1`, and `ratio == 1.0` is itself clamped by step 7
to `window − 1`, so the "internal inconsistent" `ConfigError` (75-D4/§8) remains
unreachable.

### 75-D6 — No silent-truncation detection here (deferred to `73` Rev 3)

A provider returning `200` + `finish=stop` with a non-adhering
system-prompt-free answer is **not** detectable from ymh's budgeting arithmetic;
the response is a well-formed `LLMResponse` with no error code, so
`agent_loop`'s `ContextLengthExceeded` retry
(`src/agent/agent_loop.cpp:1384-1398`; the proactive `Pressure` check is the
separate site at `:1250-1265`, `06 §5.1`) cannot fire. 75 stays strictly scoped
to *budgeting*: it makes the threshold reachable so the head is not dropped in
the first place. The behavioural guard — noticing that the model stopped acting
as the agent — is `73-non-action-guard-and-replay-dedup-errata.md` Rev 3, not
this spec. 75 must not grow a heuristic "did the model ignore the prompt?" check;
that is 73's contract.

### 75-D7 — `/context` keeps rendering the (now defaulted) window

The daemon builds the snapshot budget from the wired policy
(`src/host/host_runtime.cpp:605-613`), so after 75-D1 the overlay title and
totals (`src/ui/ui_render.cpp:1820-1842`) render the defaulted window and the
recomputed threshold. Example, no model `max_tokens`:

```text
used 0 / window 32,768 (0.0%)  threshold 22,937  reserve 4,096
```

(`floor(0.80·(32768 − 4096)) = 22,937`). With a model declaring
`max_tokens = 32'768`, 75-D2 raises the reserve to 32,768 and 75-D4 step 5 clamps
it to `32'768/2 = 16,384`, yielding
`threshold 13,107  reserve 16,384`
(`floor(0.80·(32768 − 16384)) = 13,107`). The "budget unknown" branches
(`ui_render.cpp:1825-1826`, `:1833-1835`) remain reachable only through the raw
no-budget path (e.g. a null policy); **`window 0` can no longer occur in a
normal run** — an unconfigured window is defaulted, and a sub-minimum window
aborts startup. The supervisor status fallback (`src/ui/supervisor.cpp:3587-3593`)
reads the *raw* config window only when the snapshot's window is 0; post-75 that
branch is not taken in a normal run, and 75 leaves the fallback unchanged.

### 75-D8 — dsh mapping

This is a local bug fix, not a dsh capability divergence: the writer-visible
seam (`CompactionPolicy` + `deriveMessages` projection) is unchanged. The mapping
below therefore records one genuine non-mirror — the absent provider-advertised
window — with its anchor.

| dsh concept | ymh mirror | Non-mirror justification / anchor |
|---|---|---|
| context-window management | `CompactionPolicy` window/threshold/reserve (`13 §3.2`); 75 defaults the window (75-D1) and sizes the reserve (75-D2) | Mirrored. Local arithmetic fix; the projection seam is untouched. |
| provider-advertised context limit | `ProviderCapabilities::max_context_tokens` (`include/ymh/llm/llm_provider.hpp:36`) is the seam | **Non-mirror (unbuilt, with seam).** The built-in provider never populates it (`src/llm/provider_registry.cpp:91-100`; `18 §2.2 :205-214`), so ymh has no model-aware window. Reason: no model-metadata fetch is in scope; the field exists with **no consumer**, and 75-D1 substitutes a conservative default instead of a provider query. Anchor: `75-D1`, `18 §2.2 :205-214`. |
| estimate as advisory arithmetic | `DefaultTokenEstimator` (`06 §8`, `13 §5.6`) | Mirrored/adjusted: dsh keeps context management explicit and replayable; 75 keeps the estimate local and advisory while raising its divisor (4 → 3) to narrow the under-count — the safety margin is carried by the threshold ratio, not by a strict over-estimate (75-D3, OQ-75-5). |
| per-run traceability | `/context` snapshot + threshold (`18 CTX6`) | Mirrored; values updated by 75-D7. |

No other non-mirror cells exist: 75 adds only constants and comparisons, no
capability, no wire field, no durable state.

## 4. Invariants

- `75-I1` **Default window.** With neither the resolved `context_window` nor
  `[agent.compaction].context_window_tokens` set, a wired policy has
  `context_window_tokens == kDefaultContextWindowTokens` (`32'768`) and a
  one-time `WARN` was emitted (75-D1; `75-T1`).
- `75-I2` **Reserve covers the cap.** Whenever `resolved.max_tokens` is set and
  positive, a wired policy has
  `reserve_output_tokens >= resolved.max_tokens` unless that would meet or
  exceed the window, in which case it is clamped to `window/2` and the policy
  stays enabled (75-D2/D4; `75-T2`).
- `75-I3` **Divisor coherent, margin explicit.** `DefaultTokenEstimator`,
  `estimate_tool_schema_tokens`, and `ContextCompactor::bound_summary` all use
  divisor 3 (no divisor-4 estimator or token→byte bound remains). This narrows
  the under-count but does **not** strictly over-estimate: the net real-token
  headroom is `ratio·(divisor/b)`, i.e. safe for content at `b >= 2.4 B/token`
  (the incident's 2.7 qualifies; denser content is OQ-75-5) (75-D3; `75-T4`).
- `75-I4` **Loud, never silent; never disabled for a positive window.** Every
  assumed window, clamp, or floor emits a `WARN` naming the concrete numbers; a
  window below `kMinContextWindowTokens` throws `ConfigError`; no path turns
  compaction off while the window is positive (75-D1/D4; `75-T1`/`75-T3`).
- `75-I5` **Wired-policy consistency.** Every policy with
  `context_window_tokens > 0` returned by `to_compaction_policy` satisfies
  `context_window_tokens > reserve_output_tokens`,
  `effective_threshold_tokens() > 0`,
  `effective_threshold_tokens() < context_window_tokens`, and `is_enabled()`
  (75-D5; `75-T5`).
- `75-I6` **Budgeting only.** 75 adds no provider-adherence detection, no new
  event, and no persistent state; a `200`+`finish=stop` non-adherent answer is
  unchanged by this spec (75-D6; `75-T6`).

## 5. Failure modes

| ID | Condition | Behaviour |
|---|---|---|
| `75-F1` | Neither window source set in a normal run | Window = `32'768`, one-time `WARN`, compaction enabled; the turn proceeds (75-D1, 75-I1) |
| `75-F2` | `resolved.max_tokens` unset or `0` | Reserve keeps `settings.reserve_output_tokens`; no inference; no warning (75-D2) |
| `75-F3` | Reserve would meet or exceed the window (`window <= reserve`) | `WARN` + clamp `reserve = window/2`; when caused by `resolved.max_tokens >= window`, an additional `WARN` states the clamped reserve no longer covers the advertised generation cap (raise `context_window_tokens` or lower `max_tokens`); compaction stays enabled with a positive threshold; `/context` still shows a positive budget. If `window < kMinContextWindowTokens` (4,096), startup throws `ConfigError` instead (75-D4) |
| `75-F4` | Explicit `threshold_tokens >= context_window_tokens` | `WARN` + clamp to `window-1`; absolute-trigger behaviour preserved, D5 holds (75-D4/D5) |
| `75-F5` | Operator explicitly sets `[agent.compaction].enabled=false` with no window | Compaction still enabled via 75-D1's window (the pre-existing `wiring.cpp:305-306` OR). Documented, not a crash; see OQ-75-1 |
| `75-F6` | Provider answers `200` with a non-adhering, system-prompt-free reply | Unchanged by 75; budgeting has already fired compaction if the threshold was crossed. Detection is `73` Rev 3 (75-D6) |

## 6. C++ interface sketches (before → after)

### 6.1 `include/ymh/agent/compactor.hpp` — `CompactionPolicy`

Before (`:34-65`): the struct as shipped; `context_window_tokens = 0` with the
comment "0 => unknown"; `is_enabled()` and `effective_threshold_tokens()`
unchanged.

After: three new namespace-scope constants above the struct, one new predicate,
and updated comments. The method bodies are unchanged (`13` C7 preserved).

```cpp
// 75-D1 / 75-D4
inline constexpr std::size_t kDefaultContextWindowTokens = 32'768;
inline constexpr std::size_t kMinContextWindowTokens     = 4'096;
inline constexpr std::size_t kMinEffectiveThresholdTokens = 1;

struct CompactionPolicy {
    // ... fields unchanged ...
    // The model's context window, in tokens. Still 0 => unknown **in the raw
    // struct**; 75-D1 makes `to_compaction_policy` never emit 0 on the normal
    // path (kDefaultContextWindowTokens instead).
    std::size_t context_window_tokens = 0;

    // Headroom reserved for the response. 75-D2 raises this at wiring time to
    // at least the resolved model's `max_tokens` when that is set; 75-D4 clamps
    // it to at most `context_window_tokens / 2`.
    std::size_t reserve_output_tokens = 4'096;

    // (existing bodies of is_enabled() / effective_threshold_tokens() unchanged)

    // 75-D5: executable witness of the wired-policy invariant. A zero window is
    // "unknown" and vacuously consistent; a positive window must exceed the
    // reserve and have a strictly-positive threshold below the window.
    [[nodiscard]] bool context_budget_consistent() const noexcept {
        return context_window_tokens == 0 ||
               (context_window_tokens > reserve_output_tokens &&
                effective_threshold_tokens() > 0 &&
                effective_threshold_tokens() < context_window_tokens);
    }
};
```

### 6.2 `src/agent/context_assembler.cpp` — the estimator

Before (`:29-33`):

```cpp
std::size_t DefaultTokenEstimator::estimate(const std::vector<Message>& messages) const {
    constexpr std::size_t kBytesPerToken = 4;
    constexpr std::size_t kPerMessageOverhead = 4;
    return text_bytes(messages) / kBytesPerToken + messages.size() * kPerMessageOverhead;
}
```

After:

```cpp
std::size_t DefaultTokenEstimator::estimate(const std::vector<Message>& messages) const {
    // 75-D3: 3 bytes/token. OpenAI-compatible dense code/JSON/tool output runs
    // ~2.7 B/token, so this narrows (does not eliminate) the old divisor-4
    // under-count; the threshold ratio carries the remaining safety margin.
    constexpr std::size_t kBytesPerToken = 3;
    constexpr std::size_t kPerMessageOverhead = 4;
    return text_bytes(messages) / kBytesPerToken + messages.size() * kPerMessageOverhead;
}
```

`src/agent/context_snapshot.cpp:16` changes `kSchemaBytesPerToken = 4` → `3`
(comment cites 75-D3 / supersedes `18 §3.3 :511`); `src/agent/compactor.cpp:273`
changes `kBytesPerToken = 4` → `3` in `bound_summary`. No signature changes.

### 6.3 `src/cli/wiring.cpp` — `to_compaction_policy` and the wiring

Before (`:289-312`): copies settings; `context_window_tokens =
resolved.context_window.value_or(settings.context_window_tokens)`; reserve from
settings; `enabled` OR; `max_summary_bytes` check.

After: same copy, then the 75-D1/D2/D4/D5 normalization. Exact shape (order
matters: the existing throw stays first, and the D5 witness is the last
statement before `return`):

```cpp
#include <mutex>   // 75-D1: std::once_flag / std::call_once

namespace {
// 75-D1: emit the assumed-window WARN exactly once per PROCESS. `warned` is a
// function-local static (static storage duration, initialized on first call,
// destroyed at process exit); std::call_once makes it thread-safe, so concurrent
// callers still emit exactly one WARN. Scope is per-process — each daemon is its
// own process (so one WARN per daemon), and a config reload in the same process
// does not re-warn.
void warn_assumed_window_once(std::size_t assumed) {
    static std::once_flag warned;
    std::call_once(warned, [assumed]() {
        category_logger(LogCategory::Agent)
            .warn("context window unknown; assuming " + std::to_string(assumed) +
                  " tokens (override via llm.models.<name>.context_window or "
                  "agent.compaction.context_window_tokens)");
    });
}
} // namespace

CompactionPolicy to_compaction_policy(const Config& config) {
    const ResolvedModel       resolved = resolve_model(config);
    const CompactionSettings& settings = config.agent.compaction;
    CompactionPolicy          policy;
    policy.provider                 = resolved.endpoint.provider;
    policy.threshold_tokens         = settings.threshold_tokens;
    policy.threshold_ratio          = settings.threshold_ratio;
    policy.context_window_tokens    = resolved.context_window.value_or(settings.context_window_tokens);
    policy.reserve_output_tokens    = settings.reserve_output_tokens;
    // ... remaining field copies unchanged ...

    if (policy.max_summary_bytes > PersistenceConfig{}.max_payload_bytes) {
        throw ConfigError(
            "[agent.compaction].max_summary_bytes must be <= PersistenceConfig::max_payload_bytes");
    }

    // 75-D1: default the window, warn once.
    if (policy.context_window_tokens == 0) {
        policy.context_window_tokens = kDefaultContextWindowTokens;
        warn_assumed_window_once(policy.context_window_tokens);
    }
    // 75-D2: reserve the generation cap.
    if (resolved.max_tokens.has_value() && *resolved.max_tokens > 0) {
        policy.reserve_output_tokens = std::max(
            policy.reserve_output_tokens, static_cast<std::size_t>(*resolved.max_tokens));
    }
    // 75-D4 (3): threshold_ratio is read by read_double with no range check; a
    // NaN/<=0/>1 value must not reach effective_threshold_tokens(). <cmath>.
    if (!std::isfinite(policy.threshold_ratio) || policy.threshold_ratio <= 0.0 ||
        policy.threshold_ratio > 1.0) {
        throw ConfigError("agent.compaction.threshold_ratio (" +
                          std::to_string(policy.threshold_ratio) +
                          ") must be finite and in (0, 1]");
    }
    // 75-D4 (4): a window below the hard minimum fails loudly, never silently.
    if (policy.context_window_tokens < kMinContextWindowTokens) {
        throw ConfigError("agent.compaction.context_window_tokens (" +
                          std::to_string(policy.context_window_tokens) +
                          ") is below the minimum " +
                          std::to_string(kMinContextWindowTokens));
    }
    // 75-D4 (5): bound the reserve to half the window; KEEP compaction enabled.
    if (policy.context_window_tokens <= policy.reserve_output_tokens) {
        std::string warning = "compaction reserve clamped: reserve_output_tokens (" +
                              std::to_string(policy.reserve_output_tokens) +
                              ") >= context_window_tokens (" +
                              std::to_string(policy.context_window_tokens) + "); using " +
                              std::to_string(policy.context_window_tokens / 2);
        if (resolved.max_tokens.has_value() &&
            static_cast<std::size_t>(*resolved.max_tokens) >= policy.context_window_tokens) {
            warning += "; the clamped reserve no longer covers max_tokens (" +
                       std::to_string(*resolved.max_tokens) +
                       "): raise context_window_tokens or lower max_tokens";
        }
        category_logger(LogCategory::Agent).warn(warning);
        policy.reserve_output_tokens = policy.context_window_tokens / 2;
    }
    // 75-D4 (6): an absolute threshold stays strictly below the window.
    if (policy.threshold_tokens >= policy.context_window_tokens) {
        category_logger(LogCategory::Agent)
            .warn("compaction threshold_tokens (" + std::to_string(policy.threshold_tokens) +
                  ") clamped below context_window_tokens (" +
                  std::to_string(policy.context_window_tokens) + ")");
        policy.threshold_tokens = policy.context_window_tokens - 1;
    }
    // 75-D4 (7) / 75-D5: materialize the ratio trigger and clamp it into
    // [kMinEffectiveThresholdTokens, window - 1] BY CONSTRUCTION. After this,
    // effective_threshold_tokens() returns this absolute value, so the D5
    // witness cannot be defeated by any ratio.
    if (policy.threshold_tokens == 0) {
        const std::size_t span = policy.context_window_tokens - policy.reserve_output_tokens;
        const std::size_t candidate =
            static_cast<std::size_t>(policy.threshold_ratio * static_cast<double>(span));
        const std::size_t bounded =
            std::min(std::max(candidate, kMinEffectiveThresholdTokens),
                     policy.context_window_tokens - 1);
        if (bounded != candidate) {
            category_logger(LogCategory::Agent)
                .warn("compaction threshold_ratio produced " + std::to_string(candidate) +
                      "; clamped to " + std::to_string(bounded));
        }
        policy.threshold_tokens = bounded;
    }
    policy.enabled = settings.enabled || policy.threshold_tokens > 0 ||
                     policy.context_window_tokens > 0;

    // 75-D5: production witness. Step 7 guarantees threshold_tokens in
    // [1, window-1] and step 5 guarantees window > reserve, so this is
    // unreachable; it is a defensive construction check, not a config error path.
    if (policy.context_window_tokens > 0 && !policy.context_budget_consistent()) {
        throw ConfigError("internal: compaction budget policy is inconsistent");
    }
    return policy;
}
```

`to_compaction_policy`'s signature is unchanged (`include/ymh/cli/wiring.hpp:46`),
so its existing callers (`src/agent/workspace_runtime.cpp:414`,
`tests/unit/config_test.cpp:376`, `:385`) are unaffected.

## 7. State and lifetime

| State | Created | Destroyed / evicted | Owner | Survives restart? |
|---|---|---|---|---|
| `kDefaultContextWindowTokens`, `kMinContextWindowTokens`, `kMinEffectiveThresholdTokens` | compile time (`inline constexpr`) | n/a (program image) | TU / binary | n/a |
| assumed-window `std::once_flag` (inside `warn_assumed_window_once`) | first `to_compaction_policy` call in a process | process exit (never reset) | process — function-local `static` (static storage duration), thread-safe via `std::call_once` | no — re-created and re-emits once per **process** (one per daemon; not per config load) |
| `policy.context_window_tokens` (defaulted value) | `to_compaction_policy` | with the policy, owned by `ContextCompactor` / `WorkspaceRuntime` | daemon (`WorkspaceRuntime::create`) | no — rebuilt from config each daemon start |
| `policy.reserve_output_tokens` (raised/clamped value) | `to_compaction_policy` | same as above | same as above | no |
| `WARN` lines (assumed / clamped / floored) | emission time | spdlog sink policy | process | no |
| `threshold_tokens` (clamped/floored value) | `to_compaction_policy` | with the policy | daemon | no |

75 introduces **no durable state**: no new event, no registry row, no session-DB
field, no wire message. Every value above is derived from config at daemon start.

## 8. Normative new symbols

1. **`inline constexpr std::size_t ymh::kDefaultContextWindowTokens = 32'768;`**
   (`include/ymh/agent/compactor.hpp`). Contract: the window assumed when both
   window sources are unset; set at the smallest context class ymh targets.
   Concrete caller: `to_compaction_policy` (`src/cli/wiring.cpp`), plus `75-T1`.
2. **`inline constexpr std::size_t ymh::kMinContextWindowTokens = 4'096;`**
   (`include/ymh/agent/compactor.hpp`). Contract: the smallest window a wired
   policy may carry; a smaller configured value aborts startup. Concrete caller:
   the hard-minimum guard in `to_compaction_policy` (`src/cli/wiring.cpp`), plus
   `75-T3`.
3. **`inline constexpr std::size_t ymh::kMinEffectiveThresholdTokens = 1;`**
   (`include/ymh/agent/compactor.hpp`). Contract: the lower bound of the
   materialized ratio trigger; step 7 clamps the computed candidate into
   `[kMinEffectiveThresholdTokens, window − 1]`, so a wired policy's
   `effective_threshold_tokens()` is always strictly positive and strictly below
   the window. Concrete caller: the materialization/clamp in
   `to_compaction_policy` (`src/cli/wiring.cpp`), plus `75-T5`.
4. **`bool CompactionPolicy::context_budget_consistent() const noexcept;`**
   (`include/ymh/agent/compactor.hpp`). Contract: `true` iff the window is 0
   (unknown) or (`window > reserve`, `effective_threshold_tokens() > 0`, and
   `effective_threshold_tokens() < window`); pure, `noexcept`. Concrete
   production caller: `to_compaction_policy`
   (`src/cli/wiring.cpp`, immediately before `return policy`), which throws
   `ConfigError` if it is violated; also exercised by `75-T5`.
5. **`void warn_assumed_window_once(std::size_t assumed);`** (file-local in
   `src/cli/wiring.cpp`). Contract: emit the 75-D1 `WARN` exactly once per
   process (function-local `static std::once_flag`, thread-safe), naming
   `assumed` and both override sites; never throws. Concrete caller:
   `to_compaction_policy` (same file), invoked when 75-D1 fires.

No existing signature changes. The direct `category_logger(...).warn(...)`
calls in 75-D4 are expressions of the existing `Logger` seam
(`include/ymh/core/logger.hpp:29`), anchored to the existing call shape at
`src/host/host_runtime.cpp:1002-1003`.

## 9. `/context` impact

- **Budget source.** The daemon snapshot reads the *wired* policy
  (`src/host/host_runtime.cpp:605-613`), so 75-D1/D2/D4 flow straight into
  `window_tokens`, `reserve_output_tokens`, and `effective_threshold_tokens`.
- **Default display.** For an unconfigured session with no model `max_tokens`:
  title `used N / window 32,768 (P%)` and totals
  `threshold 22,937  reserve 4,096`. With `max_tokens = 32'768` the reserve is
  raised then clamped to `16,384`:
  `threshold 13,107  reserve 16,384`. Both derive from `ui_render.cpp:1820-1842`
  and the `floor(0.8·(window−reserve))` rule (`18 §5.6 :1341-1345`).
- **`window 0` is gone from normal runs.** The two "budget unknown" branches
  (`src/ui/ui_render.cpp:1825-1826`, `:1833-1835`) remain reachable only for a
  raw no-budget snapshot (e.g. a null policy); a sub-minimum window aborts before
  a budget is ever built. The supervisor status fallback
  (`src/ui/supervisor.cpp:3587-3593`) still reads raw config only when
  `snapshot.budget.window_tokens == 0`; post-75 that branch is not taken in a
  normal run.
- **Segment estimates move.** The estimator divisor change raises every
  `/context` segment's token count (~1.33×) and, because the legend renders
  `tokens·100/window` truncated (`18 §5.3`, C5), every displayed percentage
  changes. The injected golden fixtures
  (`tests/unit/context_snapshot_test.cpp:290-299`,
  `tests/unit/ui_render_golden_test.cpp:1276-1286`) set segment tokens as
  constants, so the renderer goldens stay valid; only tests that *derive* a
  segment from bytes (75-D3 list) change.

## 10. dsh mapping

See 75-D8. Summary: the change is a **local arrow-fix** — no dsh capability is
added, removed, or renamed. The single genuine non-mirror is the absent
provider-advertised context limit (`max_context_tokens` seam present at
`llm_provider.hpp:36`, never populated at `provider_registry.cpp:91-100`), which
75-D1 papers over with a conservative default rather than a provider query; every
other cell is mirrored and anchored at `13 §3.2` / `18 §3.3`.

## 11. Test plan

Hermetic; no network. Deterministic under the existing fake layers.

- `75-T1` **Assumed window (`tests/unit/config_test.cpp`).** A default `Config`
  (no `context_window`, no `[agent.compaction]`) yields a policy with
  `context_window_tokens == kDefaultContextWindowTokens` (`32'768`),
  `reserve_output_tokens == 4'096`, `effective_threshold_tokens() == 22'937`, and
  `is_enabled()`. Asserts 75-D1/75-I1.
- `75-T2` **Reserve covers and is bounded by the window
  (`tests/unit/config_test.cpp`).** A model with `max_tokens = 32'768` yields
  `reserve_output_tokens == 16'384` (raised to 32,768, then clamped to half the
  defaulted window) and `effective_threshold_tokens() == 13'107`; a model without
  `max_tokens` keeps the settings reserve `4'096`. Asserts 75-D2/D4/75-I2.
- `75-T3` **Unusable window is clamped, sub-minimum fails
  (`tests/unit/config_test.cpp`).** Config window `4096` with reserve `8192`
  returns a policy with `reserve_output_tokens == 2'048`,
  `effective_threshold_tokens() == 1'638`, and `is_enabled()` (compaction stays
  on, never turned off). Config window `2048` (`< kMinContextWindowTokens`) throws
  `ConfigError`. The existing `CompactionOversizedSummaryBytesRejected`
  (`:382-386`) still throws. Asserts 75-D4/75-F3/75-I4.
- `75-T4` **Conservative arithmetic (`tests/unit/context_snapshot_test.cpp`).**
  Update `SchemaEstimateMatchesPinnedFixture` (`:91-100`) to `40u` and
  `bytes / 3 + 8`; update `:116` to `40u`; add a
  `DefaultTokenEstimator` assertion that a known byte payload estimates
  `bytes/3 + 4·count`. Asserts 75-D3/75-I3.
- `75-T5` **Wired-policy consistency and ratio validation
  (`tests/unit/config_test.cpp` + `tests/unit/compaction_test.cpp`).**
  (a) `ContextBudgetConsistency` (compaction_test): a ratio policy
  (`window=10000`, `reserve=1000`, `ratio=0.80`) is consistent
  (`effective = 7'200 < 10'000`); the unsound edge `window=1, reserve=0` is
  **inconsistent** (`effective == 0`, so `is_enabled()` is false); a window
  `== reserve` policy is inconsistent. Exercises `context_budget_consistent()`
  (75-D5/75-I5) and pins the method as reachable. (b)
  `CompactionRatioValidation` (config_test): `threshold_ratio` of `NaN`, `0`,
  `-1`, and `1.5` each throw `ConfigError`; `ratio=1.0` with
  `reserve_output_tokens=0` materializes to `window − 1` (the upper clamp), never
  `window`; the default `ratio=0.80` materializes a positive `threshold_tokens`
  strictly below the window.
- `75-T6` **Enabled-path regression (`tests/unit/compaction_trigger_test.cpp`).**
  With a positive-window policy, `compact_if_needed(CompactionTrigger::ContextOverflow,
  …)` reaches `compact()`; the existing `ContextOverflowFiresBelowThreshold`
  (`:128-149`) pins the fired direction and
  `DisabledPolicyReturnsNoCompactionForBothTriggers` (`:172-197`) pins the
  disabled direction. No new provider-adherence assertion is added
  (75-D6/75-I6).
- `75-T7` **Overlay renders the default window
  (`tests/unit/ui_render_golden_test.cpp`).** New goldens with
  `ContextBudget{32'768, 4'096, 22'937}` and
  `ContextBudget{32'768, 16'384, 13'107}` assert the totals lines
  `used ... / 32,768 (...)  threshold 22,937  reserve 4,096` and
  `... threshold 13,107  reserve 16,384`; the existing "budget unknown" goldens
  (`:1378-1422`) are retained for the raw no-budget path. Asserts 75-D7.
- `75-T8` **Catalog + drift.** Add `agent.compaction`
  `kDefaultContextWindowTokens` / `kMinContextWindowTokens` /
  `context_budget_consistent` catalog entries pointing at this spec where the
  catalog tracks new symbols, and add the `DESIGN_STATUS.md` row 75 (lead-owned).
  Asserts the 75-D8 mapping and the tracker/header agreement.

Pre-fix, `75-T1`–`75-T5` fail (window 0 / divisor 4 / reserve 4096); `75-T6` is
the regression that 75 fixes; `75-T7`/`75-T8` are assertions on new surfaces.

## 12. Open questions

- `75-OQ-1` **Config-level off-switch.** After 75-D1, an operator cannot disable
  compaction through `[agent.compaction].enabled = false` alone, because
  `to_compaction_policy` ORs a positive window into `enabled`
  (`wiring.cpp:305-306`) and the window is now always defaulted. A future
  revision could add an explicit presence-tracked `disable` sentinel. 75 keeps
  the fail-safe direction and logs the assumption.
- `75-OQ-2` **The default value.** `32'768` is a deliberate policy floor, not a
  model fact. It cannot protect a model whose real window is below 32k (that
  deployment must configure explicitly); a provider-advertised or probed window
  (`max_context_tokens`) would remove the guess. Belongs to `73`'s follow-on.
- `75-OQ-3` **Hard minimum vs. clamp.** A window below `kMinContextWindowTokens`
  (4,096) aborts startup with `ConfigError`; a window `<= reserve` is clamped to
  `window/2` and kept enabled. Whether 4,096 is the right floor, and whether a
  sub-minimum window should instead be accepted with a clamped reserve, is worth
  a second opinion.
- `75-OQ-4` **Divisor is still an average.** 3 B/token is a global constant; a
  byte-composition-aware estimate (ASCII vs. multi-byte UTF-8) or a tokenizer
  would be tighter. `13`'s OQ-C5 (estimator calibration) remains open and is not
  closed by 75.
- `75-OQ-5` **Estimator residual under-count.** Divisor 3 is still ~11% below
  the incident's measured 2.7 B/token; the 0.8 ratio absorbs that, so the design
  is safe for content at `>= 2.4 B/token`. Content denser than that (e.g. long
  minified numeric JSON or base64 blobs, where BPE can fall below 2 B/token) can
  still cross the real ceiling before the trigger. A divisor `<= 2` would
  over-estimate that class too, at the cost of ~1.5× earlier compaction; 75 keeps
  divisor 3 and documents the trade-off, to be revisited with real tokenizer
  counts.

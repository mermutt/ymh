# 32 — Compaction Errata: The Wave-1 Re-seam and the D12/D13 Output-Processing Contract (spec-13 amendment)

```
Status: written · verified: — · reviewer: — · Rev 1 (Wave-1 prerequisite)
Component: 32 (errata) — amends `13-context-compaction.md` by reference. It owns
           the 13 side of the `26-dsh-alignment-part2.md` (verified Rev 7, GATE
           PASS) decisions `26-D12` (the tool-result pruner) and `26-D13`
           (compaction trigger taxonomy, `compact_if_needed`/`compact_now`,
           shadowed-seq accounting, summary `provider`/`model` logging), and it
           applies the `ContextCompactor` constructor re-seam that
           `28-llm-service-boundary-errata.md` §8 pins. It does not edit
           `13-context-compaction.md` in place.
Depends on: `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS) §4.2
            (:150-151), §4.3.7 (:700-734), §4.3.9 (:840, :880-885),
            §4.3.9.1 (:945-958), §4.3.9.2 (:986, :994), §4.4 (:1079, :1097),
            §4.6 (:1197-1202), §5 Wave 1 (:1412-1413) and Wave 4 (:1466-1479);
            `28-llm-service-boundary-errata.md` (verified Rev 2) §3, §8
            (:639-664) and the pending Rev 3 `LlmCallConfig::provider` amendment
            (`AgentConfig::provider`; runtime default-route fallback; loud typed
            no-route failure); `13-context-compaction.md` (verified); `07-tools-
            execution.md` (verified) §5.2 (:601-612) and X10; `06-agent-loop.md`
            (verified) §5.3/§5.9; `30-architecture-cascade-errata.md` Rev 2
            (:5-12); the working tree at authoring time.
Scope: pin, for spec 13, (1) the `ContextCompactor(LLMProvider& → LlmRuntime&)`
       re-seam as a Wave-1 obligation and its callers, (2) the `ContextCompaction`
       payload fields `26-D13` adds — superseding 13 §4.1's "does not add fields
       to the payload" pin and classified **breaking at the specification level**
       while additive on the wire, (3) the tool-result pruner and the
       `CompactionTrigger` taxonomy, and (4) the Wave-1 / Wave-4 split that makes
       the staging coherent. Design only — no code, no behavior change.
Supersedes: `26-dsh-alignment-part2.md` §5 Stage B (:1389), which placed the `13`
            errata at Wave 4. `30-architecture-cascade-errata.md` Rev 2 already
            pulled the `13` errata forward to **Wave 0 Stage A** as a Wave-1
            blocking prerequisite because Wave 1 re-seams `ContextCompactor`;
            this errata is that pulled-forward artifact. Claims number **32**;
            `31` is the sibling `06` agent-loop errata, and the Wave-3+ names
            `27`/`28`/`29`/`30` are renumbered to `31+` per `28` §14 / `29` §1.1.
Amends: `13-context-compaction.md` §4.1 (the "does not add fields to the payload"
        pin, :489), §5.2 (the constructor and member, :604-637), §5.5 (the
        summarization call, :748-783), §11.3 (the errata ledger, :1802-1812) — all
        by reference, not in place.
```

This document is a **pin**, not a proposal. Every `file:line` below was
re-derived against the working tree before it was written. The re-seam target
type `LlmRuntime` and the entry points `compact_if_needed`/`compact_now`, the
pruner types, and the `context/prune` event **do not exist in the tree yet** —
they are pinned by the verified `26`/`28` errata and implemented by Waves 1 and
4. This errata records the spec-13 obligations; it does not claim any of them is
built.

---

## 1. Purpose, numbering, and the staging conflict this closes

`AGENTS.md`'s two-gate rule requires a component's spec to be `verified` before
any code for that component. `13-context-compaction.md` is verified, but the
verified `26` program changes two things it pins:

1. **The constructor seam.** `13` §5.2 (:604-610) and the tree
   (`include/ymh/agent/compactor.hpp:111-117`,
   `src/agent/compactor.cpp:117-126`) pin `ContextCompactor(LLMProvider&, …)`.
   `26` §5 Wave 1 (:1412-1413) re-seams it to `LlmRuntime&` so that Wave 1 can
   remove raw providers from the loop; `28` §8 (:639-664) pins the exact target
   signature and the call-site rewrite. Because the re-seam is a **Wave-1**
   change, the `13` errata that authorizes the 13 §5.2 text change must exist
   **before Wave 1 code** — but `26` §5 Stage B (:1389) originally scheduled the
   `13` errata for Wave 4. `30` Rev 2 (:5-12) recorded the correction: the `06`
   and `13` errata are pulled forward into Wave 0 Stage A as **Wave-1 blocking
   prerequisites**. This file is that pulled-forward `13` errata.

2. **The payload pin.** `13` §4.1 (:489) states: "This spec does **not** add
   fields to the payload." `26-D13` (:151) adds fields to
   `payload::ContextCompaction`. `26` §4.4 (:1097) classifies this `Brk. (D13)`.
   `13`'s text must therefore be amended — by this errata, not in place.

`13` §11.3 (:1764-1812) already carries an errata ledger `A1`–`A11` (the table
runs `:1802-1812`; `A6` is at `:1807`, `A7` at `:1808`) for the manual-`/compact`
path and the M3 re-review fixes. This errata extends that ledger (`A12`–`A13`)
and does not disturb `A1`–`A11`.

### 1.1 Verified baseline (tree facts)

| Fact | Evidence |
|---|---|
| `ContextCompactor` ctor takes `LLMProvider&` | `include/ymh/agent/compactor.hpp:113`; `src/agent/compactor.cpp:117` |
| member is `LLMProvider& provider_` | `include/ymh/agent/compactor.hpp:140`; `src/agent/compactor.cpp:122` |
| the summarizer call is `provider_.stream(...)` | `src/agent/compactor.cpp:252` |
| the daemon constructs it with `*provider_` | `src/agent/workspace_runtime.cpp:170-172` |
| test callers pass a `FakeLLM`/`LLMProvider` | `tests/support/agent_test_env.hpp:92-96`; `tests/unit/compaction_test.cpp:162,186,207,226,246,270,291,327,350,372,394,411,428,450,472` (15 direct constructions) |
| payload struct has 5 fields | `include/ymh/session/events.hpp:159-165` |
| payload is constructed with 5 fields | `src/agent/compactor.cpp:294-299` |
| `LlmRuntime` is **not** in the tree | `grep class LlmRuntime` → none |
| `PreparedCall`/`CallPurpose` are **not** in the tree | `grep` → none |
| `ToolResultPruner`/`ContextPrune`/`compact_if_needed`/`compact_now` are **not** in the tree | `grep` → none |
| `EventType` has no `ContextPrune` | `include/ymh/core/event.hpp:51-76` |
| `clamp_tool_result` is the durable byte cap | `include/ymh/tools/tool.hpp:82`; `src/tools/tool.cpp:253`; called at `src/tools/tool_registry.cpp:327` |
| `payload::ToolResult` has no citation field | `include/ymh/session/events.hpp:127-135` |

---

## 2. The `ContextCompactor` re-seam (26-D13, T-M9; Wave 1)

### 2.1 The pinned change

`28` §8 (:645-651) owns the target signature; this errata applies it to
`13` §5.2. The change is a **parameter type and member type change**, nothing
else:

```cpp
// 13 §5.2 :604-610 (current) → target (28 §8 :646-650)
ContextCompactor(LlmRuntime&           runtime,   // was: LLMProvider& provider
                 LLMPool&              pool,
                 const TokenEstimator& estimator,
                 CompactionPolicy      policy,
                 WallClock             clock = std::chrono::system_clock::now);
```

- The member `LLMProvider& provider_` (`include/ymh/agent/compactor.hpp:140`,
  `src/agent/compactor.cpp:122`) becomes `LlmRuntime& runtime_`.
- The summarizer call `provider_.stream(request, collect, cancel).get()`
  (`src/agent/compactor.cpp:252`; mirrored in `13` §5.5 :783) becomes
  `prepare_call(config, cancel).get()` → `PreparedCall::stream(frozen, collect,
  cancel).get()` with `purpose = CallPurpose::Compaction` (`28` §8 :653-656).
  Both `.get()`s are required: `compact()` is synchronous and `Task<T>` is eager
  (`13` §5.5 :751-754), exactly as the existing `pool_.acquire(...).get()` is.
- The compactor still brackets exactly one provider call with exactly one
  `LLMPool` slot (`06` §5.9, A12; `13` §5.5 :750-751; `28` §8 :657-658).
- The summarizer-model resolution (`src/agent/compactor.cpp:103-113`) is
  **unchanged** (`28` §8 :658-659); the resolved model remains the value that
  populates the payload's `model` (`13` §5.5 :803).
- **The summarizer provider id (closes gate M2).** `prepare_call` routes on
  `LlmCallConfig::provider`, a required field (`28` §3 :163-176, :252). Its
  source is pinned by the pending `28` Rev 3 amendment: the effective
  `AgentConfig::provider` (a new field on `include/ymh/agent/agent.hpp:94`), with
  the runtime's registered **default route** when it is unset, and a **loud typed
  failure** at `prepare_call` when no registered route matches (never a silent
  fallback). Because the `28` §8 constructor signature is applied verbatim
  (`32-D2`), the id is **not** a new constructor parameter: the 13-owned
  `CompactionPolicy` (`include/ymh/agent/compactor.hpp:30-58`) gains
  `ProviderId provider`, populated from the same `config.llm.provider`
  (`include/ymh/config/config.hpp:101`) that populates `AgentConfig::provider`
  (`to_agent_config`/`to_compaction_policy`, `src/cli/wiring.cpp:174-212`) — one
  source, not two. The daemon already holds the resolved `AgentConfig`
  (`agent_config_`, `src/agent/workspace_runtime.cpp:195`) at the construction
  site (`:171-172`). The compactor sets `config.provider = policy_.provider`; an
  empty value resolves to the runtime default route. `CompactionError::Code`
  gains `NoProviderRoute`, so the loud failure maps onto the existing compaction
  outcome (`13` §5.3).
- The `Compactor::run` frozen seam (`13` §5.1 :556-581; `06` §5.3) is
  **unchanged**: it is `optional`-returning and synchronous, and the re-seam is
  below it.

### 2.2 The callers (all Wave-1 obligations)

| Caller | Change |
|---|---|
| `src/agent/workspace_runtime.cpp:170-172` | pass the daemon's `LlmRuntime&` instead of `*provider_` (`28` §8 :660-661). The `provider_` member (`:200`) and the `AgentServices::provider` assignment (`:167`) are re-seamed by the `06`/`08` errata, not here. |
| `tests/support/agent_test_env.hpp:92-96` | construct the runtime around the test provider and pass it. |
| `tests/unit/compaction_test.cpp:162,186,207,226,246,270,291,327,350,372,394,411,428,450,472` (15 direct constructions; each passes a `FakeLLM` as the first argument) | same. |

This errata pins the **13-owned** constructor text and the callers' obligation;
the runtime type itself is owned by `28` §3/§8.

### 2.3 What is Wave 1 and what is not

The re-seam is the **only** `13` change that lands in Wave 1 (`26` §5
:1412-1413: "Re-seam `ContextCompactor`'s constructor … so Wave 4 can route the
summarizer"). The D12/D13 behavior — payload fields, trigger entry points, the
pruner — lands in **Wave 4** (`26` §5 :1466-1479). §6 below makes the split
explicit.

---

## 3. The `ContextCompaction` payload additions (26-D13) — BREAKING against 13 §4.1

### 3.1 The superseded pin

`13` §4.1 (:475-492) pins the payload and then states, at `:489`:

> This spec does **not** add fields to the payload.

`26-D13` (:151) adds fields. `26` §4.4 (:1097) classifies the decision
`Brk. (D13)` precisely because of that sentence. **This errata supersedes
`:489`.** The pin now reads: *13 adds the D13 fields below; the payload's struct
text and codec are owned by the `01` errata, not by 13.*

### 3.2 The added fields

`26` §4.3.9.1 (:958) pins the field set and JSON keys:

| C++ member (camelCase, matching `tokenEstimate`/`createdAt`) | JSON key | Meaning (pinned by this errata) |
|---|---|---|
| `provider` | `provider` | the provider id of the adapter that served the summarization call (from the `PreparedCall` bound by `LlmRuntime::prepare_call`; `28` §3; `26` §4.3.9.1 :946 `LlmCallConfig.provider`). Distinct from `model`, which 13 already records (`13` §5.5 :803). |
| `shadowedStart` | `shadowed_start` | inclusive first `Sequence` of the shadowed (summarized) prefix. |
| `shadowedEnd` | `shadowed_end` | inclusive last shadowed `Sequence`; for the v1 boundary model this equals `boundary` (`13` §3.3, §4.2). |
| `shadowedSeqs` | `shadowed_seqs` | the exact `Sequence` list shadowed, mirroring `ContextPrune.shadowed_seqs` (`26` §4.3.9 :883). |
| `shadowedTokenCount` | `shadowed_token_count` | the estimator's token count of the shadowed content (same `TokenEstimator` as `tokenEstimate`; `13` §5.6), consistent with `ContextPrune.shadowed_token_count` (`26` §4.3.9 :884). |

The existing five members (`boundary`, `summary`, `tokenEstimate`, `model`,
`createdAt`; `include/ymh/session/events.hpp:159-165`) are **retained unchanged**.
`boundary` remains the projection key (`13` §4.2 :496-517); the `shadowed*`
fields are additive provenance. `tokenEstimate` keeps its C-D10 meaning (the
estimate of the *resulting* compacted context, `13` §11.1 :1735-1738) and is
**not** redefined to the shadowed count.

### 3.3 Reconciliation: breaking in spec text, additive on the wire

The two statements are not in conflict once the axes are separated:

- **Specification axis (breaking).** `13` §4.1 said the payload is frozen. It is
  not. `26` §4.4 (:1097) classes it `Brk.`; the field set changes and the struct
  text (`01` §4.5) must be amended.
- **Wire/durable axis (additive).** `26` §4.6 (:1197-1202): new fields on a
  *known* event are additive JSON — each is optional or defaulted and
  `from_json` ignores unknown keys, so an older reader degrades gracefully
  instead of rejecting the row. All five D13 fields are optional/defaulted;
  `boundary` is not removed. No new event type and no `kSchemaVersion` /
  `kProtocolVersion` bump is required (`26` §4.6 :1166-1167, :1221).

An older binary reading a session DB whose `context/compaction` rows carry the
new keys must not fail; it reads the five known keys and ignores the rest. A
field that could not be made optional would have to ride a **new event type**
(`26` §4.6 :1201-1202) — none of the D13 fields is in that class.

### 3.4 Ownership boundary

The `payload::ContextCompaction` struct and its `to_json`/`from_json` live in
`include/ymh/session/events.hpp` / `src/session/events.cpp` and are pinned by
`01` §4.5. This errata pins the **field set and semantics** (13-owned) and hands
the **struct/codec text** to the `01` errata, per `26` §4.3.9.1's preamble
(:933-941: "an implementing errata may not rename one without amending this
table"). The `ContextCompaction` projection row (`26` §4.3.9.2 :994) is
unchanged: the new fields are metadata and do not alter `deriveMessages`.

---

## 4. Trigger taxonomy and entry points (26-D13)

### 4.1 The taxonomy

`26` §4.3.7 (:725) pins:

```cpp
enum class CompactionTrigger : std::uint8_t { Pressure, ContextOverflow };
```

This **names** the two triggers `13` §3.1 (:196-217) already specifies:

| `CompactionTrigger` | `13` §3.1 name | Semantics |
|---|---|---|
| `Pressure` | threshold trigger (proactive) | the pre-call estimate exceeds `policy.effective_threshold_tokens()`. |
| `ContextOverflow` | overflow retry (reactive) | the provider returned `LLMErrorCode::ContextLengthExceeded`; one compaction + one retry. |

The **manual** `/compact` path (`13` §3.1 :212, §6.7) is not a third trigger
value; it maps to `compact_now` (§4.2). Adding a trigger value later is a
spec-13 change, not an event-vocabulary change.

### 4.2 The entry points

`26` §4.3.7 (:727-733) adds two entry points to `ContextCompactor`:

```cpp
Task<std::optional<CompactionResult>> compact_if_needed(CompactionTrigger, CancellationToken);
Task<CompactionResult>                compact_now(CancellationToken);
```

- `compact_if_needed` checks `policy_.is_enabled()` and the trigger and returns
  `std::nullopt` when no compaction is warranted; it is the proactive/overflow
  path.
- `compact_now` always attempts a compaction; it is the manual `/compact` path
  (`13` §6.7).
- Both are **thin wrappers over the existing synchronous `compact()`**
  (`13` §5.3, §5.5; `src/agent/compactor.cpp:204-310`). `Task<T>` is an *eager*
  value type (`13` §5.5 :751-754), so the loop consumes them with `.get()`,
  exactly as it consumes `pool_.acquire(...)`/`stream(...)` today. The frozen
  `run()` (`13` §5.1) and `compact()` are unchanged.
- `CompactionResult` (`13` §5.3 :694-699) is unchanged; `usage` still returns to
  the loop, which owns the `TokenUsage` append (`13` §5.7, C10).

---

## 5. The tool-result pruner (26-D12) and its interaction with spec 07

### 5.1 What is pinned

`26` §4.3.7 (:700-723) pins the pruner surface:

```cpp
struct ToolResultPruneConfig {          // code points, NOT bytes
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

class ToolResultPruner {
public:
    [[nodiscard]] PruneResult prune_session(Session&);
};
```

Invariants pinned here (13-owned):

- **Replay-safe and model-free.** `prune_session` makes no LLM call; it is a
  deterministic function of the log (`26` §4.3.7 :717-719).
- **Never deletes.** It appends a replacement tool-result event and, immediately
  before it, a `context/prune` event carrying the shadowed set
  (`payload::ContextPrune`: `shadowed_start`, `shadowed_end`, `shadowed_seqs`,
  `shadowed_token_count`; `26` §4.3.9 :880-885, §4.3.9.1 :950). The original
  event stays in the log (`26-F8` :1135-1136). This is the same no-deletion rule
  as `13` C1 / §4.5.
- **Code-point budget.** The thresholds count Unicode code points, not bytes
  (`26` §4.3.7 :703). This is deliberately different from the byte-oriented
  durable cap in `07` (§5.2 below).
- **Projection/accounting — replace-by-id (closes gate M1).** `context/prune`
  contributes no message of its own (`26` §4.3.9.2 :986), but the replacement
  must **replace**, not append. The replacement `ToolResult` reuses the shadowed
  original's `payload::ToolResult::id`, and `deriveMessages`'s `ToolResult` case
  becomes **replace-by-id**: a `Role::Tool` message whose `tool_call_id` already
  exists has its content replaced in place instead of a second message being
  added. This is chosen over "consume `shadowed_seqs` in the projection" because
  `26` §4.3.9.2 :986 pins `ContextPrune` as projection-ignore (the shadowed set is
  audit provenance), and because a same-`id` replacement keeps `01` §6.3's pure
  fold shape. Without it, the original full result and the replacement would both
  project for the same `tool_call_id` — `src/session/session.cpp:441-455` folds
  every `ToolResult` unconditionally — growing the context and double-counting
  tokens. With it, exactly one Tool message per call is projected and counted
  (the **replacement**); the prune event is never counted; and the original event
  stays durable-but-unprojected (`26-F8`). `13` §4.2's `ContextCompaction` fold
  is unaffected.
- **Citation.** The pair is the citation: the `context/prune` event carries
  `shadowed_seqs`, the replacement immediately follows and reuses the shadowed
  original's `id`. `payload::ToolResult`
  (`include/ymh/session/events.hpp:127-135`) gains **no** citation field. If a
  direct citation field were later required, it is a `01`/`07` change owned
  elsewhere — not pinned here.
- **Placement.** New header `include/ymh/agent/tool_result_pruner.hpp` (32-D6),
  agent-adjacent to the compactor.

### 5.2 Interaction with spec 07's `ToolResult` / `clamp_tool_result`

`clamp_tool_result(ToolResult&, std::size_t max_bytes)`
(`include/ymh/tools/tool.hpp:82`; `src/tools/tool.cpp:253`; called at
`src/tools/tool_registry.cpp:327` with `ToolConfig::tool_result_max_bytes`) is
the **durable** cap: it truncates `output` until the *serialized*
`payload::ToolResult` fits `tool_result_max_bytes` (default 1 MiB), which must
be ≤ `PersistenceConfig::max_payload_bytes`, so a tool result can never trigger
`PayloadTooLarge` (`07` §5.2 :601-612, X10, E-F11). The pruner is a **context**
transform:

| | `clamp_tool_result` (07) | `ToolResultPruner` (D12, this errata) |
|---|---|---|
| Axis | durable payload | projected context |
| Unit | bytes (serialized JSON) | Unicode code points |
| When | at append time (registry/loop) | at context-build time |
| Effect | truncates `output` in place | appends `context/prune` + a same-`id` replacement `ToolResult` (replace-by-id projection) |
| Model | n/a | model-free |

They are **orthogonal and both apply**: the durable cap bounds what is stored;
the pruner bounds what is re-sent to the model. The pruner must **not** call or
depend on `clamp_tool_result`, because `26-D11` (retention library, `26` §4.3.6)
retires `clamp_tool_result` in favour of the retainer once all call sites
migrate (`26` §5 :1477-1479); the pruner must survive that retirement. The
replacement's own serialized payload is smaller than the original by
construction, so it still satisfies the durable cap.

`payload::ToolResult` gains omission metadata under `26-D11` (`26` §4.2 :149,
classified `Brk. (D11)` and owned by `07` + a new `28-output-retention.md`).
That `07`/retention errata is a **separate** dependency from this one; the
pruner does not require it to land first.

---

## 6. Staging: the Wave-1 / Wave-4 split made coherent

`26` §5 originally split the two obligations across waves: the constructor
re-seam in Wave 1 (:1412-1413), the D12/D13 behavior in Wave 4 (:1466-1479),
with the `13` errata itself scheduled at Wave 4 (:1389). That is incoherent —
Wave 1 cannot change the `13`-pinned constructor without the `13` errata. This
errata resolves it:

| Obligation | Wave | Gate before code |
|---|---|---|
| `13` §5.2 constructor text → `LlmRuntime&`; member; call site; provider source (`32-D8`) | **1** | this errata + `28` §8 + the pending `28` Rev 3 provider amendment |
| caller updates: daemon (`workspace_runtime.cpp:170-172`), fixture (`agent_test_env.hpp:92-96`), 15 `compaction_test.cpp` sites | **1** | this errata + the `06`/`08` errata (`31`/`28`) |
| `ContextCompaction` D13 fields (semantics) | **4** | this errata |
| `ContextCompaction` struct/codec text | **4** | future `01` errata (`26` §4.3.9.1) |
| `CompactionTrigger`, `compact_if_needed`/`compact_now` | **4** | this errata |
| `ToolResultPruner` + `ContextPrune` event/codec + replace-by-id projection row (`32-D9`) | **4** | this errata + future `01` errata |
| `clamp_tool_result` retirement (D11) | **4** | `07` + `28-output-retention.md` (separate) |

Wave 4 additionally **depends on Wave 3** (`26` §5 :1473-1479): `ContextAcceptor`
consumes `ContextMessage` (`include/ymh/agent/agent.hpp:88`) and D13/D14 both
reshape the spec-13 message model, so Wave 4 must land after Wave 3. This errata
does not change that ordering; it only makes the Wave-1 piece available now.

This errata is design-only: it adds no code and no version bump. `kSchemaVersion`
stays `1` and `kProtocolVersion` stays `1` (`26` §4.6 :1166-1167, :1221).

---

## 7. Invariants (extending 13 §7; `C1`–`C19` are unchanged)

**C20 — Single service boundary.** `ContextCompactor` reaches the LLM only
through `LlmRuntime&`; no `LLMProvider*` appears in its members or constructor.
(`28` L18; `26-D1`)

**C21 — Payload additive-only.** Every D13 field is optional/defaulted; the five
original members are retained; `from_json` ignores unknown keys, so an older
reader degrades gracefully. (`26` §4.6 :1197-1202)

**C22 — No deletion (pruner).** `prune_session` never deletes, updates, or
rewrites a session event; it appends `context/prune` then a replacement
`ToolResult`, and the original remains. (`13` C1, §4.5; `26-F8`)

**C23 — Model-free pruning.** The pruner performs no LLM call; its output is a
deterministic function of the log. (`26` §4.3.7 :717-719)

**C24 — Trigger totality.** `compact_if_needed` is driven only by
`CompactionTrigger::{Pressure, ContextOverflow}`; the manual path uses
`compact_now`. (`13` §3.1; `26-D13`)

**C25 — Code-point budget.** Prune thresholds count Unicode code points, never
bytes. (`26` §4.3.7 :703)

**C26 — Prune/replacement replace-by-id.** A replacement `ToolResult` is
immediately preceded by its `context/prune` and reuses the shadowed original's
`payload::ToolResult::id`; `deriveMessages` replaces the projected Tool message
for that `tool_call_id` in place, so exactly one (pruned) Tool message is
projected and counted and the prune event is never counted. (`26` §4.3.9.2 :986)

---

## 8. Failure modes (extending 13 §8.2; `C-F1`–`C-F17b` are unchanged)

| ID | Failure | Guard |
|---|---|---|
| **C-F18** | Re-seam leak: a `ContextCompactor` still holding a raw `LLMProvider*`. | C20; the constructor/member types; compile-time. |
| **C-F19** | A D13 field is made required, so an older reader rejects the row. | C21; all fields optional/defaulted; round-trip test with a legacy 5-field payload. |
| **C-F20** | The pruner deletes or rewrites the original event. | C22; append-only; replay round-trip. |
| **C-F21** | A replacement re-overflows the durable byte cap. | The replacement is smaller than the original; the durable cap still applies. |
| **C-F22** | `context/prune` and its replacement are reordered or split across turns. | C26; immediate adjacency; replay round-trip. |
| **C-F23** | Trigger misattribution: an overflow compaction recorded as pressure (or vice versa). | C24; the trigger is an explicit argument; the caller may log it in the turn context. |
| **C-F24** | No provider route for the summarizer call. | The pending `28` amendment's loud typed failure at `prepare_call` ("no route for config.provider"); `CompactionError::Code::NoProviderRoute`; never a silent fallback. |
| **C-F25** | A replacement `ToolResult` uses a fresh `id`, so replace-by-id misses and both results project. | C26; the replacement reuses the shadowed original's `id`; replay round-trip asserts exactly one Tool message per call. |

---

## 9. Test plan (additions to 13 §10)

1. **Re-seam shape (unit/compile).** `ContextCompactor`'s constructor takes
   `LlmRuntime&`; the daemon, the test fixture, and **all fifteen**
   `compaction_test.cpp` construction sites (`:162,186,207,226,246,270,291,327,
   350,372,394,411,428,450,472`) construct it that way; no `LLMProvider*` remains
   in the class. The compactor still acquires exactly one `LLMPool` slot per call
   (A12).
2. **Payload evolution (unit, golden).** A legacy 5-field `context/compaction`
   payload decodes with defaults; a full D13 payload round-trips byte-identically;
   an unknown key is ignored (`26` §4.6).
3. **Pruner (unit, golden).** Code-point boundaries at 8192/4096/1024; the
   `kPruneMarker` string verbatim; a replay round-trip
   (`prune_session` → replacement → `deriveMessages`) yields the same messages as
   the live path; the original event is still present; **replace-by-id**: exactly
   one `Role::Tool` message exists for the pruned `tool_call_id`, its text is the
   replacement's (the original's full text is absent), and the token estimate
   counts the replacement and not the prune event.
4. **Triggers (unit).** `compact_if_needed(Pressure)` fires only above the
   effective threshold; `compact_if_needed(ContextOverflow)` fires only after a
   `ContextLengthExceeded`; `compact_now` always attempts; each maps to the
   `13` §3.1 path.
5. **Live (opt-in, `YMH_LIVE_LLM=1`).** The existing compaction live path now
   exercises the `LlmRuntime`-routed summarizer; unchanged otherwise.
6. **Provider resolution (unit).** The summarizer's `LlmCallConfig.provider` is
   the effective `AgentConfig::provider`; when unset it resolves to the runtime's
   registered default route; an unroutable provider fails loud and typed at
   `prepare_call` and maps to `CompactionError::Code::NoProviderRoute` (C-F24).

---

## 10. Errata ledger additions (extends 13 §11.3; `A1`–`A11` unchanged)

| # | Target | Required amendment | Why |
|---|---|---|---|
| **A12** | `include/ymh/agent/compactor.hpp` (`:30-58`, `:69-86`, `:113`, `:140`), `src/agent/compactor.cpp` (`:117-126`, `:252`), `src/agent/workspace_runtime.cpp` (`:170-172`), `tests/support/agent_test_env.hpp` (`:92-96`), `tests/unit/compaction_test.cpp` (`:162,:186,:207,:226,:246,:270,:291,:327,:350,:372,:394,:411,:428,:450,:472`) | Re-seam the constructor and member from `LLMProvider&` to `LlmRuntime&`; rewrite the summarizer call to `prepare_call(config, cancel).get()` → `PreparedCall::stream(frozen, collect, cancel).get()` with `purpose = CallPurpose::Compaction`; add `CompactionPolicy::provider` (from `AgentConfig::provider`) and `CompactionError::Code::NoProviderRoute`; update **every** caller site — 1 daemon, 1 fixture, and all 15 `compaction_test.cpp` constructions. | `26` §5 Wave 1 :1412-1413; `28` §8 :639-664; the pending `28` Rev 3 provider amendment. |
| **A13** | `include/ymh/session/events.hpp` / `src/session/events.cpp` / `01` §4.5; new `include/ymh/agent/tool_result_pruner.hpp`; `include/ymh/core/event.hpp` / `src/session/session.cpp` | Add the five D13 fields to `payload::ContextCompaction`; add `payload::ContextPrune`, `EventType::ContextPrune` / wire `context/prune`, its codec, and its `deriveMessages` no-op case; add `CompactionTrigger` and the two entry points; add `ToolResultPruner`; make the `ToolResult` projection **replace-by-id** so a same-`id` replacement replaces the shadowed original (C26). | `26-D12`/`26-D13`; `26` §4.3.7, §4.3.9.1, §4.3.9.2; gate M1. |

`A1`–`A11` (the manual-`/compact` path and the M3 re-review fixes) remain in
force; this errata does not touch them. `A12`/`A13` are the next free labels
(`A6`/`A7` already exist at `13-context-compaction.md:1807-1808`).

---

## 11. Decisions (32-D1–32-D9)

- **32-D1 — This errata is a Wave-1 prerequisite.** The constructor text is
  amended now; the D12/D13 behavior lands in Wave 4. (`30` Rev 2 :5-12)
- **32-D2 — The target signature is `28` §8's, verbatim.** `28` owns it; this
  errata applies it to `13` §5.2. `28` §8 :662-664 records the 13-owned text
  change; this file performs it.
- **32-D3 — D13 supersedes `13` §4.1 :489.** The payload pin is amended: 13
  adds the five fields. Breaking at the specification level, additive on the wire
  (`26` §4.6).
- **32-D4 — Ownership boundary.** 13 owns the field set and semantics; the
  struct/codec text is owned by the `01` errata (`26` §4.3.9.1).
- **32-D5 — Entry points and taxonomy per `26` §4.3.7.** `CompactionTrigger`
  names the two existing triggers; the manual path maps to `compact_now`.
- **32-D6 — The pruner is separate and independent.** `ToolResultPruner` is
  additive to 13; it does not use or replace `clamp_tool_result`, so it survives
  the D11 retirement. New header `include/ymh/agent/tool_result_pruner.hpp`.
- **32-D7 — `boundary` is retained.** The `shadowed*` fields are additive
  provenance; `tokenEstimate` keeps its C-D10 meaning.
- **32-D8 — The summarizer provider is `AgentConfig::provider`.** The pending
  `28` Rev 3 amendment pins the `LlmCallConfig::provider` source (config-driven;
  the runtime's registered default route when unset; a loud typed failure when no
  route matches). The compactor applies it via the 13-owned
  `CompactionPolicy::provider`, preserving the `28` §8 constructor signature
  (`32-D2`). `CompactionError::Code::NoProviderRoute` carries the failure.
- **32-D9 — The pruner replacement is replace-by-id.** The replacement
  `ToolResult` reuses the shadowed original's `payload::ToolResult::id`, and
  `deriveMessages` replaces the projected Tool message in place (C26);
  `context/prune` stays projection-ignore (`26` §4.3.9.2 :986).

---

## 12. Revision log

| Rev | Change |
|---|---|
| 0 | Initial Wave-1-prerequisite errata. Applies the `28` §8 `ContextCompactor` re-seam (`LLMProvider& → LlmRuntime&`) to `13` §5.2; supersedes `13` §4.1 :489 with the `26-D13` payload fields (breaking in spec text, additive on the wire); pins the `26-D12` tool-result pruner and the `26-D13` trigger taxonomy/entry points; reconciles the pruner with `07`'s `clamp_tool_result`/`ToolResult`; makes the Wave-1/Wave-4 split coherent; adds `C20`–`C26`, `C-F18`–`C-F23`, the `A6`/`A7` ledger rows, and decisions `32-D1`–`32-D7`. Claims number 32. No code, no version bump. |
| 1 | Gate remediation (gate32, `regate32.md`). **H1:** enumerates all fifteen `compaction_test.cpp` constructions (`:162,186,207,226,246,270,291,327,350,372,394,411,428,450,472`) and drops "all three callers". **H2:** corrects the ledger inventory to `A1`–`A11` (`13` :1802-1812) and renumbers the additions `A6`/`A7` → **`A12`/`A13`**. **M1:** pins the pruner projection as **replace-by-id** (C26/C-F25, `32-D9`) instead of "already folds". **M2:** pins the summarizer provider source via the pending `28` Rev 3 `AgentConfig::provider` amendment (C-F24, `32-D8`). **LOWs:** `Brk. (13)` → `Brk. (D13)`; adds the missing `.get()`s; rewrites the "adds no event type" sentence. Adds `32-D8`/`32-D9`. No code, no version bump. |

---

## 13. References

- `26-dsh-alignment-part2.md` (verified Rev 7): §4.2 :150-151; §4.3.7 :700-734;
  §4.3.9 :840, :880-885; §4.3.9.1 :933-958; §4.3.9.2 :986, :994; §4.4 :1079,
  :1097; §4.6 :1166-1167, :1197-1202, :1221; §5 :1389, :1412-1413, :1466-1479.
- `28-llm-service-boundary-errata.md` (verified Rev 2): §3, §8 :639-664; the
  pending Rev 3 `LlmCallConfig::provider` amendment (`AgentConfig::provider`).
- `13-context-compaction.md` (verified): §3.1 :196-217; §4.1 :469-492; §4.2
  :496-517; §4.5; §5.1 :556-581; §5.2 :604-637; §5.3 :661-716; §5.5 :748-821;
  §5.6; §5.7 :845-876; §6.7; §11.1 :1661-1738; §11.3 :1764-1812.
- `30-architecture-cascade-errata.md` (Rev 2): :5-12.
- `07-tools-execution.md` (verified): §5.2 :601-612; X10; `ToolConfig`.
- `06-agent-loop.md` (verified): §5.3 (`Compactor`); §5.9 (`LLMPool`, A12).
- Tree: `include/ymh/agent/compactor.hpp:111-145`;
  `src/agent/compactor.cpp:103-113,117-126,204-321`;
  `src/agent/workspace_runtime.cpp:158-202`;
  `tests/support/agent_test_env.hpp:80-109`;
  `tests/unit/compaction_test.cpp:162,186,207,226,246,270,291,327,350,372,394,411,428,450,472`;
  `include/ymh/session/events.hpp:127-135,159-165`;
  `include/ymh/core/event.hpp:51-76`;
  `include/ymh/tools/tool.hpp:82`; `src/tools/tool.cpp:253`;
  `src/tools/tool_registry.cpp:327`.

# 40 — Output Retention (Wave 4)

```
Status: Rev 1 written · verified: — · reviewer: —
Authority: owning spec for `26-dsh-alignment-part2.md` §5 Wave 4 (decisions
           `26-D10`, `26-D11`, `26-D12`, `26-D13`, `26-D21`). The reserved
           filename `28-output-retention.md` named in `26-dsh-alignment-part2.md`
           §5 Stage B (line 1393) is **unavailable**: specs 27–39 are taken.
           This file is that spec, numbered **40**.
Component: 40 (owning spec) — owns the Wave-4 output-processing component. It
           amends `07-tools-execution.md` §8.1 (the `ToolResult` struct + the
           clamp contract), `06-agent-loop.md` (the serial tool loop → bounded
           scheduler; repeat-tool reminders), and it *consumes* the verified
           `32-compaction-errata.md` D12/D13 contract without restating it as
           new. It does not edit any of those files in place.
Depends on: `26-dsh-alignment.md` (design source; §2.2.2–§2.2.5, §3.2 item 4),
            `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS) §4.3.5–§4.3.7,
            §4.3.9, §4.3.9.1, §4.3.9.2, §4.4, §5 Wave 4;
            `36-prompt-registry.md` (Wave 3; `ContextMessage`/`ContextAcceptor`
            consumer model) — **must be verified before Wave-4 code**;
            `32-compaction-errata.md` (verified Rev 1; the D12/D13 authority);
            `13-context-compaction.md`, `07-tools-execution.md`,
            `06-agent-loop.md` (all verified); `01-session.md`;
            `08-llm-provider.md` / `28-llm-service-boundary-errata.md`
            (`LlmRuntime`); `21-config-jsonc-errata.md`; `24-agent-lifetime-errata.md`;
            `34-assembler-replay-errata.md` (replay harness); `30` (gate pass).
Scope: pin, for Wave 4, (1) the bounded-parallel tool scheduler (`26-D10`);
       (2) the retention library and the `ToolResult` omission-metadata contract
       (`26-D11`, a spec-07 break); (3) the Wave-4 wiring of the tool-result
       pruner (`26-D12`) and the compaction trigger taxonomy (`26-D13`) whose
       spec-13 text is owned by `32`; (4) repeat-tool reminders (`26-D21`); and
       (5) the retirement of `clamp_tool_result` once every call site migrates.
       Design only — no code.
Supersedes: `26-dsh-alignment-part2.md` §5 Stage B (line 1393), which reserved
            the name `28-output-retention.md`. That number is taken by the
            LLM-service-boundary errata; the reserved artifact is this file.
```

This document is a **pin**, not a proposal. Every `file:line` below was
re-derived against the working tree at authoring time. The scheduler, the
retention library, the pruner, `context/prune`, and the omission metadata **do
not exist in the tree yet**; they are pinned here and by the verified
`26`/`32` errata. This spec records the Wave-4 obligations; it does not claim
any of them is built. Where this spec and `32-compaction-errata.md` both speak
to the pruner or the compaction trigger, **`32` is authoritative for the
spec-13 text** and this spec only wires it.

---

## 1. Purpose, numbering, and the staging constraint

### 1.1 Why this spec exists

`26-dsh-alignment.md` §3.2 (:1290-1293) names gap 4 as one of the five
highest-leverage deltas:

> **G13/G15/G16 — no bounded-parallel tool scheduling, retention, or replay-safe
> pruning.** ymh is serial and caps results with a blunt byte clamp; dsh
> schedules a bounded pool, reports exact omissions, and prunes the surface with
> shadow-price accounting.

The gap analysis rows are explicit: `G13` (`26-dsh-alignment.md:1228`) records
that `AgentLoop::executeToolCall` is *singular* and executes one call at a time
(`include/ymh/agent/agent_loop.hpp:162`; the serial dispatch is
`src/agent/agent_loop.cpp:1062-1066`), and `G15` (:1230) records that only
`clamp_tool_result` truncates a serialized result. `26-dsh-alignment-part2.md`
§5 Wave 4 (:1470-1483) is where that delta lands. Wave 4 has **no owning spec**:
the reserved name at :1393 was `28-output-retention.md`, but `28` is the
LLM-service-boundary errata (`DESIGN_STATUS.md:44`) and `31`–`39` are also
taken (`DESIGN_STATUS.md:46-59`). The correct number is **40**.

### 1.2 Authority and relationship to the other specs

- `26-dsh-alignment.md` §2.2.2 (:519-529) is the **design source** for the
  scheduler, §2.2.3 (:540-562) for the retention library, §2.2.4 (:564-598) for
  the pruner/compaction, and §2.2.5 (:600-609) for repeat-tool reminders. The
  verbatim dsh texts are quoted there and are the copy target.
- `26-dsh-alignment-part2.md` §4.3.5 (:614-639), §4.3.6 (:641-693) and §4.3.7
  (:703-737) pin the C++ shapes; §4.3.9 (:802-935) and §4.3.9.1 (:936-975) pin
  the events; §4.4 (:1073-1100) classifies the breaks; §5 (:1470-1483) is the
  wave scope.
- `36-prompt-registry.md` (Wave 3, verified) owns `ContextMessage` and the
  message-provenance model Wave 4 consumes. `ContextMessage` already exists at
  `include/ymh/agent/agent.hpp:88-94`; the loop's inbox/`ContextInjected` seam
  is `06` §5.2 (`:648-674`). Wave 4 must not land before Wave 3 is verified
  (§1.5).
- `32-compaction-errata.md` (verified Rev 1) **owns** the spec-13 text for the
  tool-result pruner (`26-D12`) and the compaction trigger taxonomy (`26-D13`):
  the payload fields (`32` §3), `CompactionTrigger`/`compact_if_needed`/
  `compact_now` (`32` §4), the `ToolResultPruner` (`32` §5), and invariants
  `C20`–`C26` / failures `C-F18`–`C-F24`. This spec references those pins and
  pins only their **Wave-4 call sites and ordering**.
- `07-tools-execution.md` owns `ToolResult`, `ToolConfig`, and the durable cap.
  This spec is an **errata to `07`** for the omission-metadata reshape (§3.3)
  and the `clamp_tool_result` retirement (§3.4).
- `06-agent-loop.md` owns the turn/step cycle and the tool pipeline. This spec
  is an **errata to `06`** for the scheduler (§2) and repeat-tool reminders
  (§6).
- `08`/`28` own `LlmRuntime`; `21` owns the config keys this spec needs;
  `24` owns the loop's co-ownership invariant (`24-D1`,
  `include/ymh/agent/agent_loop.hpp:92`; `26` §4.4 :1089 cites the pre-Wave-1
  `:71-73`), which the scheduler preserves.

### 1.3 In scope

1. The bounded-parallel tool scheduler (`26-D10`): exclusive barriers, a bounded
   rolling pool (default 10), model-ordered results/context, and synthetic
   results on abort.
2. The retention library (`26-D11`): `Deque`, `ChunkedList`,
   `ItemRetainer`/`TextRetainer`, `RetentionNotice`, and the `ToolResult`
   omission metadata — the exact representation `26` §4.3.6 left to "spec 28",
   which is now spec 40.
3. The Wave-4 wiring of the tool-result pruner (`26-D12`) and the compaction
   trigger taxonomy (`26-D13`), by reference to `32`.
4. Repeat-tool reminders (`26-D21`) with thresholds `[3,5,8]`.
5. The retirement of `clamp_tool_result` (`26` §5 :1481-1483) once all call
   sites migrate.

### 1.4 Out of scope

- The spec-13 payload text and the pruner/trigger interfaces themselves: owned
  by `32` and `13` (referenced, not restated).
- The event-family codec mechanics (`01` errata) and the five consumer switches
  (`26` §4.3.9 :811-830): owned by the `01` errata.
- Presets/subagents (`26-D16`/`D17`, Wave 5) and goals/jobs/commands
  (`26-D18`–`D20`, Wave 6).
- PTC `run_code` and the SDK generator (`26-D22`, deferred).
- Remote SSH/TCP transport (out of scope for the project, `AGENTS.md`).

### 1.5 The serialization constraint (Wave 3 → Wave 4)

`26-dsh-alignment-part2.md` §5 (:1477-1483) pins the dependency:

> **Dependency: serialize Wave 3 → Wave 4.** This wave is *not* independent of
> the prompt system: `ContextAcceptor` (§4.3.5) consumes `ContextMessage`
> (`agent.hpp:88`) and D13/D14 both reshape the spec-13 message model, so Wave 4
> must land after Wave 3.

`ContextAcceptor` is a Wave-4 type (`26` §4.3.5 :629) but it is typed over
`ContextMessage`, which is Wave 3 (`36` §3; `agent.hpp:88-94`). The scheduler
accepts context contributions through `ContextAcceptor`; the reminder path
(§6) appends `ContextMessage`s through the same seam. Therefore **no Wave-4 code
may land until `36` is `verified`** in `DESIGN_STATUS.md`. This is invariant
`40-I16`.

---

## 2. The bounded-parallel tool scheduler (26-D10)

### 2.1 Model

`06` §5.1 (`:579`) pins "zero or more tool calls: policy → execute →
`ToolResult`" as one step; the shipped loop dispatches them **serially**
(`src/agent/agent_loop.cpp:1062-1066`, one `executeToolCall` per call in
`response.tool_calls` order). Wave 4 replaces that loop with
`execute_tool_calls`, which schedules one assistant step's calls by their **live
concurrency mode** (`26-dsh-alignment.md:521-529`):

- **Exclusive calls form barriers.** No exclusive call overlaps any other call,
  before or after it in model order. A barrier drains every in-flight
  parallel-safe call before the exclusive call dispatches, and no later call
  dispatches until the exclusive call commits.
- **Parallel-safe calls use a bounded rolling pool.** At most
  `max_parallel_tool_calls` (default 10, dsh `DEFAULT_MAX_PARALLEL_TOOL_CALLS`)
  are in flight at once.
- **Policy, results, and result context remain model-ordered.** Concurrency
  changes completion timing only; the committed `results` vector and every
  context contribution are in the assistant's original call order.
- **Abort drains started calls and synthesizes results for unstarted calls**
  (`26-D10` :148; `26-dsh-alignment.md:522-525`), so the durable log keeps
  `01` I12's totality property.
- **An internal scheduler failure** stops new dispatches, drains already-started
  dispatches, and rejects with the first failure *without fabricating tool
  results* (`26-dsh-alignment.md:524-525`). The loop maps the rejection to a
  terminal event; unstarted calls are then synthesized by the turn-cancel/
  failure path (`06` §5.6 :801-805).

### 2.2 Concurrency classification (a spec-07 additive field)

The scheduler needs to know each call's concurrency mode. `ToolSchema`
(`include/ymh/tools/tool.hpp:39-45`) carries `destructive` but no concurrency
attribute. This spec adds one:

```cpp
namespace ymh {

// dsh's "live concurrency mode" (26 §2.2.2:521). Default is the SAFE value:
// a tool that has not declared itself parallel-safe is an exclusive barrier.
enum class ToolConcurrencyMode : std::uint8_t { Exclusive, ParallelSafe };

struct ToolSchema {
    ToolName            name;
    ToolVersion         version;
    std::string         description;
    nlohmann::json      input_schema;
    bool                destructive{false};
    ToolConcurrencyMode concurrency{ToolConcurrencyMode::Exclusive};   // NEW
};

} // namespace ymh
```

`Add.` to `07` §3.2 (`07-tools-execution.md:250-292`): a new defaulted field.
The **per-tool classification** (which of the shipped tools are `ParallelSafe`)
is a `07` tool-set obligation (`07` §4.3); this spec pins only the mechanism and
the conservative default. A `deny` policy result and a `Cancelled` token do not
change the mode; a tool whose policy is `Ask` is treated as a barrier (§2.4).

### 2.3 C++ interfaces (pinned)

These are `26` §4.3.5's shapes (:616-638), restated as the Wave-4 contract:

```cpp
namespace ymh {

struct ToolScheduleConfig {
    std::size_t max_parallel_tool_calls = 10;   // dsh DEFAULT_MAX_PARALLEL_TOOL_CALLS
};

// One assistant step's committed result set. `results` is model-ordered.
struct ToolScheduleOutcome {
    std::vector<ToolResult> results;
    bool                    aborted = false;    // cancellation/abort drained the step
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

} // namespace ymh
```

`ToolCallAssembled` is `include/ymh/llm/stream.hpp:228`. `execute_tool_calls`
replaces the `for` loop at `src/agent/agent_loop.cpp:1062-1066`; the per-call
body stays `AgentLoop::executeToolCall` (`:582`), which already appends
`payload::ToolCall` then the durable `payload::ToolResult` (`:737`) and returns
`bool`.

### 2.4 Dispatch algorithm (normative)

```text
execute_tool_calls(loop, turn, step, calls, cancel, accept):
  results := [pending] * calls.size()          # model-indexed
  in_flight := {}                              # bounded by max_parallel_tool_calls
  for i in model_order(calls):
    if cancel.requested(): break
    mode := schema(calls[i]).concurrency
    if mode == Exclusive:
      drain_all(in_flight)                     # barrier
      commit(i, run_exclusive(calls[i]))       # alone
    else:
      wait_for_slot(in_flight, max_parallel)   # rolling pool
      dispatch(i, calls[i])                    # overlaps prior ParallelSafe calls
  drain_or_abort(in_flight)                    # see below
  aborted := cancel.requested()
  if aborted:
    for i where results[i] is pending:
      results[i] := synthetic(Cancelled)       # turn cancel
  return { results, aborted }
```

- **Policy order (A18).** Permission evaluation (`§19`) and the durable
  `payload::ToolCall` append happen in **model order**, before dispatch of that
  call. A call whose policy returns `Ask` enters `WaitingForPermission`
  (`06` §5.5 :775-779) and blocks **new dispatches** (it is a barrier);
  already-started parallel-safe calls are allowed to drain. This keeps the
  one-driver rule (`A1`) and the "never execute an unvalidated call" invariant
  (`A18`, `06` §10 :1064-1066).
- **Drain.** Normal completion commits started results in model order. On
  cancel, started calls are drained (their real results commit) and unstarted
  calls receive `ToolResult{outcome = Cancelled}` (matching `06` §5.6
  :803-805). On an internal scheduler failure, no synthetic results are
  fabricated; the rejection propagates and the terminal-event path synthesizes
  (`06` §5.6). A-F2/A-F1 apply unchanged.
- **Append serialization.** Only the loop thread appends; the pool returns
  values, it never touches the `Session`. `A3` (durable before live) and `A5`
  (barrier before non-chunk appends) are preserved: results are appended at
  commit points, and a `StepEnded` is not appended until all commits complete.
- **Lifetime.** In-flight calls hold the agent alive; this is additive to
  `24-D1` (`26` §4.4 :1089 — "no lifetime change"). `dispose()` cancels the
  turn token, which drains the pool (`A14`, `A-F12`).

### 2.5 Scheduler invariants and failure modes

Covered by `40-I1`–`40-I4` (§8) and `40-F1`–`40-F3` (§9).

---

## 3. The retention library and the ToolResult contract (26-D11)

### 3.1 Model

`26-dsh-alignment.md` §2.2.3 (:540-562) pins three dependency-light libraries
that implement bounded, replay-safe output. `26-dsh-alignment-part2.md` §4.3.6
(:641-693) pins the C++ shapes and states that "Spec 28 owns the exact
representation; this is the pinned shape." Since `28` is taken, **spec 40 owns
the exact representation**. The library is byte- and code-point-oriented; it
answers only the mechanical question *what did we keep, what did we omit?* Tool
domain states (permission failures, provider partial failures) stay in
tool-domain fields and are **never** folded into retention metadata
(`26-dsh-alignment.md:557-560`).

### 3.2 C++ interfaces (pinned)

```cpp
namespace ymh {

// dsh dsh-deque: circular, amortized O(1) push/pop at both ends; removed
// entries are cleared immediately and storage shrinks at a quarter live
// capacity. `capacity()` exposes that shrink for the §13.1 probe; it is the
// only capacity observable. (Spec 40 owns the exact representation.)
template <class T> class Deque {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;  // shrink-at-quarter probe
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
    // `max_bytes` is the retained-text budget in serialized bytes. The caller
    // derives it from `ToolConfig::tool_result_max_bytes` by subtracting the
    // envelope and retention-notice headroom (40-D6). The retainer is
    // escaping-aware: each retained byte is charged at its worst-case JSON
    // escaped width, so `finish()` never exceeds `max_bytes` (40-I6, 40-I14).
    explicit TextRetainer(TextRetentionStrategy, std::size_t max_bytes);
    PushDecision push(std::span<const std::byte>);   // UTF-8-boundary safe at finish
    [[nodiscard]] RetainedText finish() const;
};
[[nodiscard]] std::string format_retention_notice(const RetentionNotice&, RecoveryTextFn);

} // namespace ymh
```

- `Deque` backs the rolling pool's ready queue (§2.4) and the bounded
  pending-result buffer. `ChunkedList` backs the durable context-message list
  where copy-on-append matters (`36` §3).
- `TextRetainer` is the **only** output cap at append time after Wave 4. Its
  strategy for tool output defaults to `HeadTail` (the head gives the command
  context, the tail gives the failing lines); `Head`/`Tail` remain available.
  Its `max_bytes` budget is **escaping-aware**: the retainer charges each
  retained byte at its worst-case JSON-escaped width, so a cap that survives
  escaping cannot be exceeded by construction (40-I6). `finish()` never splits
  a UTF-8 sequence (`26-dsh-alignment.md:557`).
- `format_retention_notice` joins a library-owned omission clause with a
  tool-owned recovery sentence (`26-dsh-alignment.md:560-562`).

### 3.3 The `ToolResult` omission-metadata contract (spec-07 break)

`26` §4.3.6 (:695-701) pins the mapping, classified `Brk. (D11)` (`26-D11`
:149):

- The retained text becomes `ToolResult.output`.
- The notice is appended to `output`.
- `ToolResult` gains `OmittedKind omitted_kind` and `std::size_t omitted_count`
  so replay can recover exact omission facts.
- `ToolResult.truncated` becomes `omitted.kind != OmittedKind::None`.

The shipped struct (`include/ymh/session/events.hpp:135-146`; the `07` contract
text is `07-tools-execution.md:1127-1135`) currently has `truncated` and the
Wave-3 provenance fields (`source`, `context`). Wave 4 amends `07` §8.1:

```cpp
struct ToolResult {
    ToolCallId                 id;
    std::string                name;
    ToolOutcome                outcome = ToolOutcome::Ok;
    std::string                output;      // retained text + retention notice
    bool                       truncated = false;   // == (omitted_kind != None)
    OmittedKind                omitted_kind = OmittedKind::None;   // NEW
    std::size_t                omitted_count = 0;                  // NEW
    std::optional<std::string> error;
    std::chrono::milliseconds  duration{0};
    MessageSource              source = tool_message_source(
        id.empty() ? std::optional<ToolCallId>{} : std::optional<ToolCallId>{id});
    ContextFormed              context{};
};
```

**Ring loss maps to `Unknown`.** `07` §5.2 (`:545-565`) defines
`OutputSink::truncated()` as "ring wrapped or UTF-8 loss (F5)". The ring is
live-only and its eviction is a *different* fact from a retainer's exact
omission. To keep one durable signal without losing the F5 fact, this spec pins:
a ring wrap or UTF-8 loss sets `omitted_kind = OmittedKind::Unknown` (the count
is not known) unless the retainer also reports `Exact` for the same result, in
which case `Exact` wins and its count is recorded. `truncated` remains as a
derived, wire-compatible boolean equal to `omitted_kind != None`. See §10
Open Question 1.

### 3.4 `clamp_tool_result` retirement (D11 / `26` §5 :1481-1483)

The shipped clamp is `clamp_tool_result(ToolResult&, std::size_t)`
(`include/ymh/tools/tool.hpp:79-82`; implementation
`src/tools/tool.cpp:253`) with exactly three call sites:

| Call site | Line | Context |
|---|---|---|
| `src/tools/tool_registry.cpp` | `:327` | after the tool returns, with `config_.tool_result_max_bytes` |
| `src/mcp/mcp_tool.cpp` | `:36` | MCP result path |
| `src/mcp/mcp_tool.cpp` | `:69` | MCP result path (`result.truncated = …`) |

Wave 4 replaces all three with the retainer, then **deletes**
`clamp_tool_result` and its tests (`tests/unit/output_test.cpp:75,90,103`). The
retirement is complete only when `40-I8` holds: no call site, no declaration,
no test reference remains. `ToolConfig::tool_result_max_bytes`
(`include/ymh/execution/config.hpp:14-17`, default 1 MiB) stays the **budget
source**: the caller sets `TextRetainer::max_bytes` (the retained-text budget)
so the serialized `payload::ToolResult` (retained `output` + notice + envelope)
still fits `tool_result_max_bytes`, which must remain ≤
`PersistenceConfig::max_payload_bytes`
(`include/ymh/session/session_persistence.hpp:62`, 4 MiB) minus envelope
headroom (`07` §5.2 :601-612, `X10`, `E-F11`). Because JSON escaping can inflate
text ~6×, `max_bytes` is derived from the cap minus notice/envelope headroom,
never equal to the cap.

---

## 4. The tool-result pruner — Wave-4 wiring (26-D12)

### 4.1 What `32` owns (referenced, not restated)

The pruner surface is owned by `32` §5 (`:283-378`) and `13`:

- `ToolResultPruneConfig{ threshold_code_points = 8192, head_code_points = 4096,
  tail_code_points = 1024 }` — **code points, not bytes** (`32` §5.1 :289-294;
  `26` §4.3.7 :707-711; `26-dsh-alignment.md:589-592`).
- `inline constexpr std::string_view kPruneMarker =
  "\n\n[... tool result middle pruned ...]\n\n"` (`32` §5.1 :295-296;
  dsh `PRUNE_MARKER`, `26-dsh-alignment.md:593`).
- `class ToolResultPruner { PruneResult prune_session(Session&); }` (`32` §5.1
  :303-306).
- Never deletes: it appends `payload::ContextPrune`
  (`shadowed_start`, `shadowed_end`, `shadowed_seqs`, `shadowed_token_count`;
  `26` §4.3.9 :881-888; §4.3.9.1 :953) immediately before a same-`id`
  replacement `ToolResult` (`32` C22/C26 :419-437; `26` §4.3.9.2 :986).
- Model-free and deterministic (`32` C23 :423-424).
- New header `include/ymh/agent/tool_result_pruner.hpp` (`32-D6` :514-516).

### 4.2 Wave-4 wiring obligations (this spec)

`32` §5.2 (:347-377) states the pruner is a **context** transform that "must not
call or depend on `clamp_tool_result`" and "survives the D11 retirement." This
spec pins *where* it runs:

1. **Call site.** The loop invokes `pruner.prune_session(session_)` immediately
   **before** each `services_.context->assemble(...)`
   (`src/agent/agent_loop.cpp:768, :832`) and before each `buildRequest(...)`
   call (`:888`, `:1020`). It runs on the agent executor thread only (A1). It is
   **idempotent**: a replacement is under budget, so a second pass in the same
   turn prunes nothing.
2. **Ordering vs retention.** Retention (§3) runs at **append time** (durable
   payload); pruning runs at **context-build time** (projected surface). They
   are orthogonal and both apply (`32` §5.2 table :358-364). The pruner operates
   on the *retained* `output`, and the replacement's serialized payload is
   smaller by construction (`32` C-F21 :448).
3. **Gating.** Pruning is skipped when no projected `ToolResult` exceeds
   `threshold_code_points`; the scan is O(projected tool results) and appends
   nothing when clean.
4. **Trigger selection (`26-D13`).** The loop's two existing compaction sites —
   the proactive threshold check (`src/agent/agent_loop.cpp:870-877`) and the
   overflow retry (`:1000-1013`) — pass `CompactionTrigger::Pressure` and
   `CompactionTrigger::ContextOverflow` respectively to `compact_if_needed`
   (`32` §4 :238-280). The manual `/compact` path calls `compact_now`. This spec
   does not change `13`'s trigger semantics; it only names the call sites.
5. **Provider/model logging.** The compaction call routes through `LlmRuntime`
   (already re-seamed: `include/ymh/agent/compactor.hpp:116-127` takes
   `LlmRuntime&`; `32` C20 :411-413). The emitted `payload::ContextCompaction`
   records `provider` from the bound `PreparedCall` and `model` from the
   summarizer config (`26` §4.3.9.1 :962; `32` §3.2 :192-198). `LlmRuntime` is
   `include/ymh/llm/llm_runtime.hpp:185`; `prepare_call` is
   `src/llm/llm_runtime.cpp:294`.

Invariants `40-I9`–`40-I11`; failures `40-F8`–`40-F10`.

---

## 5. Compaction trigger taxonomy — Wave-4 wiring (26-D13)

The taxonomy itself is `32` §4 (`:238-280`): `CompactionTrigger::Pressure` is
the proactive pre-call threshold check and `CompactionTrigger::ContextOverflow`
is the reactive one-shot retry after `LLMErrorCode::ContextLengthExceeded`
(`13` §3.1 :196-217). The manual `/compact` path is `compact_now`, not a third
value. `compact_if_needed`/`compact_now` are thin wrappers over the existing
`ContextCompactor::compact` (`include/ymh/agent/compactor.hpp:127`;
`src/agent/compactor.cpp:204`). The `shadowed*` fields and the summary
`provider`/`model` are pinned by `32` §3 (`:188-205`).

This spec's only D13 obligations are the four wiring points in §4.2 items 4–5
plus `40-I10`/`40-I11`. `13` §4.1's "does not add fields to the payload"
sentence is superseded by `32` §3.1 (`:177-186`); this spec does not re-open it.

---

## 6. Repeat-tool reminders (26-D21)

### 6.1 Model

`26-dsh-alignment.md` §2.2.5 (:600-609) pins the dsh behavior: the
`dsh-repeat-tool-reminder` plugin watches consecutive identical tool calls and
injects an append-only plugin-sourced context message. Thresholds default
`[3,5,8]`; `argumentsPreviewChars` default `500`. This is **injected plugin
context, not a system prompt section** (`36` §3.2: the message is a
`ContextInjected`, not a `render()` input).

### 6.2 C++ interface and rule (pinned)

```cpp
namespace ymh {

struct RepeatToolReminderConfig {
    std::vector<std::size_t> thresholds = {3, 5, 8};   // dsh default
    std::size_t              arguments_preview_chars = 500;
};

// One per AgentLoop (executor-thread only). Observes committed calls in model
// order; returns a ContextMessage to inject when a threshold is crossed.
class RepeatToolReminder {
public:
    explicit RepeatToolReminder(RepeatToolReminderConfig = {});
    [[nodiscard]] std::optional<ContextMessage>
    observe(const ToolCallAssembled& committed);
};

} // namespace ymh
```

- **Identity.** Two calls are identical iff `name` is equal and their canonical
  arguments JSON is equal. Canonicalization is a deterministic key-sorted dump;
  the preview is `canonical_args.substr(0, arguments_preview_chars)`.
- **Counter.** The loop maintains the *consecutive* count of identical calls;
  a call with a different `(name, canonical_args)` resets it to 1. The window is
  the **session** (append-only history), not a single turn, because "not making
  progress" is a cross-step property. See §10 Open Question 4.
- **Thresholds.** At a count equal to `3` the **gentle** text is injected; at
  `5` and `8` the **detailed** text is injected. Each threshold fires at most
  once per run of identical calls.
- **Message shape.** The reminder is a `ContextMessage` with
  `role = Role::User`, `MessageSource::Kind::Plugin`,
  `source.plugin = "repeat-tool-reminder"`, and
  `ContextFormed{form = ContextForm::Notice, summary = <short reason>}`
  (`36` §3.1; `36-I8` requires a non-empty summary for `Notice`). It is
  appended through the loop's `appendContextInjected`
  (`src/agent/agent_loop.cpp:380`) as a durable `ContextInjected`, i.e.
  **append-only**.
- **Text (verbatim, dsh).** Gentle:
  > You are repeating the exact same tool call with identical arguments.
  > Carefully analyze the previous result before calling again: if the task is
  > not complete, try a different approach or different arguments instead of
  > repeating the call.

  Detailed:
  > Repeated tool call detected:
  > - tool: `<toolName>`
  > - consecutive_calls: `<count>`
  > - arguments: `<canonicalArguments>`
  > The repeated calls are not making progress. Do not call this tool with these
  > exact arguments again. Inspect the latest result and choose a different
  > action, different arguments, or finish the task if enough evidence has been
  > gathered.

- **Placement.** `observe` runs after each committed call (after the scheduler
  commits, §2.4) and before the next `assemble()`, so the reminder is visible to
  the next step. It never blocks or fails a turn; a rejected injection (inbox
  full, `A-F8`) is dropped with a logged warning, never silently.

Invariants `40-I12`; failure `40-F11`.

---

## 7. Event and payload changes (summary)

| Change | Payload / type | Owner | Classification |
|---|---|---|---|
| Scheduler config/outcome/acceptor | `ToolScheduleConfig`, `ToolScheduleOutcome`, `ContextAcceptor` | 40 | Add. (`06` errata) |
| Tool concurrency mode | `ToolSchema::concurrency` | 40 | Add. (`07` §3.2) |
| Retention library | `Deque`, `ChunkedList`, `ItemRetainer`, `TextRetainer`, `RetentionNotice`, `Omitted`/`OmittedKind` | 40 | New (`26-D11`) |
| `ToolResult` omission metadata | `ToolResult::omitted_kind`, `::omitted_count`; `truncated` derived | 40 | **Brk.** (`07` §8.1; `26-D11`) |
| Pruner + `context/prune` | `payload::ContextPrune`; replacement `ToolResult` | `32` (spec-13) | Add. (`26-D12`) |
| Compaction additions | `payload::ContextCompaction` `provider`/`shadowed*` | `32` (spec-13) | Brk. in spec text, additive on wire (`26-D13`) |
| Trigger entry points | `CompactionTrigger`, `compact_if_needed`, `compact_now` | `32` (spec-13) | Add. (`26-D13`) |
| Repeat-tool reminders | `ContextInjected` (existing) | 40 | Add. (`06` errata; `26-D21`) |
| `clamp_tool_result` removed | `include/ymh/tools/tool.hpp:82` | 40 | **Brk.** (retirement) |

No new `EventType` enumerator is introduced by this spec. `context/prune` is
pinned by `26` §4.3.9 (:843) and `32`; reminders reuse `ContextInjected`; the
scheduler and retention change payload fields and loop behavior only.

---

## 8. Invariants

These extend `26-I1`–`26-I12`, `06` A1–A18, `13` C1–C19, and `32` C20–C26.
`40-I*` are local to this spec. `06`/`07`/`13` IDs are unchanged.

- **40-I1 (model-order).** `ToolScheduleOutcome::results` and every context
  contribution accepted through `ContextAcceptor` are in the assistant's
  original call order, regardless of completion order. Guard: the model-indexed
  result vector; a shuffled-completion test.
- **40-I2 (barrier + bound).** An exclusive call never overlaps any other call;
  at most `max_parallel_tool_calls` parallel-safe calls are in flight. Guard:
  the dispatch algorithm; a concurrency-observation test and TSan.
- **40-I3 (abort totality).** On abort, every committed tool call has exactly
  one durable `ToolResult`: started calls commit real results; unstarted calls
  receive a synthetic `Cancelled` result. An internal scheduler failure
  fabricates none and rejects. Guard: `01` I12; replay round-trip.
- **40-I4 (single writer).** Only the loop thread appends; the pool returns
  values and never touches the `Session`. Guard: A1; a fake-tool append
  assertion.
- **40-I5 (retention exactness).** A retainer reports `OmittedKind::Exact` with
  the true omitted count whenever it dropped content; it never silently drops.
  Guard: exact-omission goldens.
- **40-I6 (durable cap preserved).** After the retainer replaces the clamp, the
  serialized `payload::ToolResult` still fits `tool_result_max_bytes`
  (escaping-aware), and that cap is ≤ `max_payload_bytes` minus headroom. Guard:
  the `TextRetainer` budget (§3.2, §3.4); `X10`/`E-F11`; an escaping-heavy
  fixture.
- **40-I7 (omission consistency).** `truncated == (omitted_kind != None)`; a
  ring/UTF-8 loss yields `Unknown` unless the retainer yields `Exact`, which
  wins. Guard: round-trip of both omission sources.
- **40-I8 (clamp retired).** Once Wave 4 lands, no call site, declaration, or
  test references `clamp_tool_result`. Guard: a tree grep in the test suite.
- **40-I9 (prune/replacement adjacency — by reference).** `context/prune`
  immediately precedes its same-`id` replacement; `deriveMessages` replaces the
  projected Tool message in place. (`32` C22/C26; `13` C1.) Guard: replay
  round-trip.
- **40-I10 (trigger totality — by reference).** Compaction is driven only by
  `Pressure`/`ContextOverflow`; the manual path is `compact_now`. (`32` C24.)
  Guard: trigger-selection test.
- **40-I11 (summarizer service + provenance).** The summarizer reaches the LLM
  only through `LlmRuntime&`; the `ContextCompaction` records the serving
  `provider` and `model`. (`32` C20, D8; `28` §3.5.) Guard: a fake-runtime
  assertion on the emitted payload.
- **40-I12 (reminder determinism).** Identical call sequences produce identical
  reminders; thresholds `[3,5,8]`; each fires at most once per run; the message
  is plugin-stamped and append-only. Guard: reminder goldens; a replay test.
- **40-I13 (no lifetime change).** The scheduler holds the agent alive only for
  the duration of in-flight calls; `dispose()` still cancels and drains.
  (`24-D1`; `A14`.) Guard: a dispose-mid-step test.
- **40-I14 (retention never grows).** The retained text never exceeds the
  `TextRetainer` budget (escaping-aware); the notice fits its reserved headroom
  and is never double-counted. Guard: a bound assertion in the golden.
- **40-I15 (no new UI dependency).** The scheduler, retainers, pruner, and
  reminders depend on no FTXUI type (`A17`). Guard: a link-level check.
- **40-I16 (Wave 3 → Wave 4 serialization).** No Wave-4 code lands before `36`
  is `verified` in `DESIGN_STATUS.md`, because `ContextAcceptor` is typed over
  Wave-3 `ContextMessage`. Guard: the gate process.

---

## 9. Failure modes

### 9.1 Shared findings (`F1`–`F12`, `§54`)

- **F5 (output ring buffers).** The live ring stays bounded (`07` §5.2); its
  eviction now surfaces as `OmittedKind::Unknown` on the durable result
  (`40-I7`). The retainer bounds the projected surface.
- **F8 (resource caps).** The rolling pool is bounded by
  `max_parallel_tool_calls`; every provider call still takes one `LLMPool` slot
  (`A12`).
- **F9 (cancellation scoping).** The scheduler's token is the turn token; abort
  is scoped to the turn and drains only its own calls (`A9`).
- **F1/F3/F10/F11/F12** apply by reference (`06` §11.1 :1074-1092); the
  scheduler, pruner, and reminders add no exception. **F2/F4/F6/F7** are
  supervisor/TUI concerns owned by `04`/`10`.

### 9.2 Component-local failure modes (`40-F#`)

| ID | Failure | Guard |
|---|---|---|
| **40-F1** | Concurrency bound or barrier violated (an exclusive call overlaps another, or > N parallel-safe calls run). | `40-I2`; the dispatch algorithm; TSan + a concurrency-observation test. |
| **40-F2** | Result reordering: completion order leaks into `results` or the projected context. | `40-I1`; model-indexed vector; a shuffled-completion test. |
| **40-F3** | Abort leaves an unstarted call without a result, invalidating replay. | `40-I3`; synthetic `Cancelled`; replay round-trip. |
| **40-F4** | A permission `Ask` call is dispatched before its decision (A18 violation). | §2.4 policy order; `A18`; an `Ask`-barrier test. |
| **40-F5** | Retainer silent drop: content is lost with `OmittedKind::None`. | `40-I5`; exact-omission goldens. |
| **40-F6** | Retained output still overflows the durable cap via JSON escaping. | `40-I6`; an escaping-heavy fixture (`07` §5.2). |
| **40-F7** | Omission metadata disagrees with the notice or with `truncated`. | `40-I7`; round-trip of both sources. |
| **40-F8** | A `clamp_tool_result` call site survives the retirement. | `40-I8`; a tree-grep test. |
| **40-F9** | Pruner double-projection (original + replacement both fold). | `32` C26/`C-F22`; replace-by-id; replay round-trip. |
| **40-F10** | Trigger misattribution (overflow recorded as pressure). | `32` C24/`C-F23`; explicit trigger argument; a selection test. |
| **40-F11** | Summarizer bypasses `LlmRuntime`, or `provider`/`model` missing from the event. | `40-I11`; `32` C20; a fake-runtime assertion. |
| **40-F12** | Reminder spam / non-determinism / missing plugin stamp. | `40-I12`; thresholds fire once; goldens. |
| **40-F13** | TSan race on the rolling pool or on a session append. | `40-I4`; single-writer append; TSan. |
| **40-F14** | `dispose()` mid-step leaks an in-flight call or blocks quiescence. | `40-I13`; `A14`; a dispose-mid-step test. |

---

## 10. dsh mapping

| dsh concept | ymh type / site | Reference |
|---|---|---|
| `executeToolCalls` (live concurrency mode, barriers, bounded pool) | `execute_tool_calls` | `26 §2.2.2:519-529`; `26 part2 §4.3.5:614-639` |
| `DEFAULT_MAX_PARALLEL_TOOL_CALLS = 10` | `ToolScheduleConfig::max_parallel_tool_calls` | `26 part2 §4.3.5:618` |
| tool "live concurrency mode" | `ToolConcurrencyMode` on `ToolSchema` | `26 §2.2.2:521`; §2.2 |
| `dsh-deque` | `Deque<T>` | `26 §2.2.3:544-548`; `26 part2 §4.3.6:647-654` |
| `dsh-chunked-list` (`appendChunkedList`, 64-value chunks) | `ChunkedList<T>`, `append_chunked_list` | `26 §2.2.3:549-552`; `26 part2 §4.3.6:658-666` |
| `dsh-output-retention` `ItemRetainer`/`TextRetainer` | `ItemRetainer`/`TextRetainer` | `26 §2.2.3:553-562`; `26 part2 §4.3.6:680-691` |
| `formatRetentionNotice` | `format_retention_notice` | `26 §2.2.3:560-562`; `26 part2 §4.3.6:692` |
| `Omitted` discriminated union | `Omitted` + `OmittedKind` | `26 part2 §4.3.6:668-671` |
| `dsh-compaction-tool-result-pruner`, `PRUNE_MARKER` | `ToolResultPruner`, `kPruneMarker` | `32 §5`; `26 §2.2.4:587-598` |
| `compaction/prune` shadow price | `context/prune` (`payload::ContextPrune`) | `26 §4.3.9:843,881-888`; `32` §5 |
| `compactIfNeeded('pressure'｜'context-overflow')` / `compactNow` | `compact_if_needed(CompactionTrigger,…)` / `compact_now` | `26 §2.2.4:578-585`; `26 part2 §4.3.7:728-736`; `32 §4` |
| `dsh-repeat-tool-reminder` (thresholds `[3,5,8]`, `argumentsPreviewChars=500`) | `RepeatToolReminder`, `RepeatToolReminderConfig` | `26 §2.2.5:600-609` |
| plugin `source: {kind:"plugin", plugin:"repeat-tool-reminder"}` | `ContextMessage.source` + `ContextFormed{Notice}` | `26 §2.2.5:608-609`; `36 §3.1` |

**Deliberate divergences (stated).** (a) ymh keeps the discriminant explicit
(`OmittedKind`) where dsh uses a TS union (`26 part2` T-L3). (b) ymh names the
event `context/prune` (slash/underscore convention) where dsh uses
`compaction/prune` (`26 §4.3.9:832-835`). (c) ymh defaults a tool to
`Exclusive`; dsh tools declare their mode. (d) The reminder window is the
session, not an unspecified dsh scope (§10 Open Question 4).

---

## 11. Dependencies

**Upstream (must be verified before Wave-4 code, per `AGENTS.md`):**

| Spec | What Wave 4 needs | Status |
|---|---|---|
| `26-dsh-alignment.md` | design source, verbatim texts | reference (not a component spec) |
| `26-dsh-alignment-part2.md` | pinned interfaces, wave scope | verified (Rev 7, GATE PASS) |
| `36-prompt-registry.md` | `ContextMessage` / provenance / `ContextAcceptor` consumer model | **required** — Wave 3; gate must pass first (`40-I16`) |
| `32-compaction-errata.md` | D12/D13 spec-13 contract | verified (Rev 1) |
| `13-context-compaction.md` | compaction model, `compact()` | verified |
| `07-tools-execution.md` | `ToolResult`, `ToolConfig`, durable cap | verified (this spec errata's it) |
| `06-agent-loop.md` | turn/step cycle, tool pipeline | verified (this spec errata's it) |
| `01-session.md` | event payloads, `deriveMessages` | verified |
| `08` / `28-llm-service-boundary-errata.md` | `LlmRuntime`, `PreparedCall` | verified (Wave 1) |
| `21-config-jsonc-errata.md` | new optional keys (§12 `40-D12`) | errata required |
| `24-agent-lifetime-errata.md` | loop co-ownership (`24-D1`) | verified |
| `34-assembler-replay-errata.md` | replay harness | Wave 2 |
| `30-architecture-cascade-errata.md` | gate pass | verified |

**Downstream:** `17` transcript rows (retention notices, reminder rows;
`26` §4.4 :1084), `01` errata (event codecs), `21` errata (keys), and the `07`
tool-set classification of `ParallelSafe`.

---

## 12. Decision register (40-D1–40-D14)

| ID | Decision | Add./Brk. | Owning spec |
|---|---|---|---|
| **40-D1** | This spec is the Wave-4 owning spec; it is numbered **40** because the reserved `28-output-retention.md` (`26` §5 Stage B :1393) is taken by the LLM-service-boundary errata. | New | 40 |
| **40-D2** | The scheduler is `execute_tool_calls` with model-ordered results, exclusive barriers, and a rolling pool bounded by `max_parallel_tool_calls` (default 10). | Add. | 40, 06 |
| **40-D3** | Concurrency is a `ToolSchema` field (`ToolConcurrencyMode`), default `Exclusive`; per-tool classification is a `07` §4.3 obligation. | Add. | 40, 07 |
| **40-D4** | Abort synthesis: turn cancel → `ToolResult{Cancelled}` for unstarted calls; internal scheduler failure → no synthetic results, reject. | Add. | 40, 06 |
| **40-D5** | Spec 40 owns the retention library's exact representation (the "spec 28" of `26` §4.3.6). | New | 40 |
| **40-D6** | The retainer replaces `clamp_tool_result` at every call site; `TextRetainer::max_bytes` derives from `ToolConfig::tool_result_max_bytes` minus notice/envelope headroom, escaping-aware (`X10`). | Brk. | 40, 07 |
| **40-D7** | `ToolResult` gains `omitted_kind`/`omitted_count`; `truncated` is derived (`omitted_kind != None`); ring/UTF-8 loss maps to `Unknown`, retainer exactness wins. | Brk. | 40, 07, 01 |
| **40-D8** | `clamp_tool_result` is deleted once `40-I8` holds (no call site/declaration/test reference). | Brk. | 40, 07 |
| **40-D9** | The pruner and the trigger taxonomy are owned by `32`; spec 40 pins only their Wave-4 call sites and ordering. | None | 32, 40 |
| **40-D10** | The pruner runs before every `assemble()` on the executor thread and is idempotent. | Add. | 40, 06 |
| **40-D11** | Reminders use thresholds `[3,5,8]` (gentle at 3, detailed at 5/8), consecutive-identical-call counting, plugin `Notice` messages, `arguments_preview_chars = 500`. | Add. | 40, 06 |
| **40-D12** | New optional config keys (retention strategy, prune thresholds, reminder thresholds) are a `21` errata; the retention byte budget stays derived from `ToolConfig::tool_result_max_bytes` (§3.4), not a new key; defaults are the pinned values in this spec. | Add. | 21, 40 |
| **40-D13** | The Wave-3 → Wave-4 serialization is mandatory (`40-I16`); no Wave-4 code before `36` is verified. | New | 40 |
| **40-D14** | No new `EventType` is introduced; reminders reuse `ContextInjected` and the pruner's event is `32`'s `context/prune`. | None | 40, 01 |

---

## 13. Test plan

Additions to `13` §10 and `32` §9; all hermetic unless marked live.

### 13.1 Unit tests

1. `Deque`: push/pop at both ends, `pop_front` on empty → `nullopt`, clear,
   shrink-at-quarter behavior (observable via `capacity()`).
2. `ChunkedList`: append copies at most one 64-value chunk; older chunks are
   shared (pointer identity); `iterate` yields insertion order; empty → empty.
3. `ItemRetainer` (head-only): exact `Omitted{Exact,count}` for overflow; no
   omission under budget.
4. `TextRetainer`: `Head`/`Tail`/`HeadTail`; UTF-8 boundary safety (a multi-byte
   code point is never split); exact omitted count; the byte budget is enforced
   (an over-budget push reports `Exact` and `finish()` never exceeds `max_bytes`
   even for an escaping-heavy fixture); `format_retention_notice` joins library +
   recovery text verbatim.
5. `ToolResult` omission metadata: `truncated == (omitted_kind != None)`; ring
   loss → `Unknown`; retainer `Exact` wins; JSON round-trip.
6. Scheduler: model-order preservation under shuffled completion; barrier
   (exclusive never overlaps); pool bound (`max_parallel_tool_calls`); abort
   synthetics (`Cancelled`); internal-failure rejection with no synthetic
   results; `Ask` barrier.
7. `RepeatToolReminder`: counters reset on a different call; thresholds
   `[3,5,8]` fire once each; gentle vs detailed text; canonical args + 500-char
   preview; plugin stamp and `ContextFormed{Notice}`.
8. Pruner (reference `32` §9): thresholds in code points (not bytes); marker
   verbatim; `context/prune` precedes the same-`id` replacement; model-free.
9. Trigger selection: pressure vs overflow argument at the two call sites.
10. Clamp retirement: a tree grep asserts no `clamp_tool_result` reference.

### 13.2 Integration tests (`FakeLLM`, `§45`)

11. A step with `[read, read, read, shell]`: the three reads overlap (observed
    via a fake tool's concurrency counter), the shell is a barrier, and results
    are model-ordered.
12. Cancel mid-step: started calls commit real results; unstarted calls commit
    synthetic `Cancelled`; the turn ends `TurnCancelled`.
13. Permission `Ask` in the middle of a step: no new dispatch until the durable
    `PermissionDecision`; already-started parallel-safe calls drain.
14. Overflow: FakeLLM returns `ContextLengthExceeded`; one
    `compact_if_needed(ContextOverflow,…)` + retry; the emitted
    `ContextCompaction` carries `provider`/`model`/`shadowed*`.
15. A reminder at count 3 is visible to the next step's assembled context.

### 13.3 Golden tests

16. Exact-omission goldens: retainer output + notice for head/tail/headTail and
    for a ring-wrapped result (`Unknown`).
17. Reminder message goldens (gentle and detailed) including the plugin source
    and `Notice` form.
18. Golden render of a pruned tool row (transcript, `17`) — by reference to
    `32` §9.

### 13.4 Replay tests

19. Prune round-trip: `context/prune` + same-`id` replacement reconstruct the
    same projected context (`32` C22/C26).
20. Reminder replay: a session with reminders replays byte-identically.
21. Scheduler abort replay: synthetic results reproduce the same projection
    (`40-I3`).

### 13.5 Concurrency / sanitizer

22. TSan over the scheduler with a mix of parallel-safe and exclusive fake tools
    (`40-I2`, `40-F13`).
23. A fake tool that attempts a `Session` append from the pool thread fails the
    single-writer assertion (`40-I4`).

### 13.6 Live end-to-end (opt-in: `YMH_LIVE_LLM=1`, real DeepSeek)

24. A multi-read task exercises the pool against the real provider; a repeated
    call triggers a reminder; a large tool output is retained and its notice is
    shown. Live tests drive the real binary under a PTY (`§44`).

### 13.7 Failure-mode coverage matrix

| Failure | Test |
|---|---|
| `40-F1`/`F13` | 6, 22 |
| `40-F2` | 6, 11 |
| `40-F3` | 6, 12, 21 |
| `40-F4` | 6, 13 |
| `40-F5` | 3, 4, 16 |
| `40-F6` | 5, 16 |
| `40-F7` | 5, 16 |
| `40-F8` | 10 |
| `40-F9` | 8, 19 |
| `40-F10` | 9, 14 |
| `40-F11` | 14 |
| `40-F12` | 7, 17, 20 |
| `40-F14` | 12 |

---

## 14. Open questions

1. **Ring truncation vs omission semantics.** `26` §4.3.6 (:695-698) says
   `ToolResult.truncated` *becomes* `omitted.kind != None`, while `07` §5.2
   (:560) defines `truncated` from `OutputSink` ring wrap / UTF-8 loss (F5) — a
   different fact from a retainer's exact omission. This spec proposes folding
   ring loss into `OmittedKind::Unknown` with retainer `Exact` winning
   (`40-D7`/`40-I7`). **Confirm with the `07` owner** whether `truncated` is
   retained as a derived field or removed outright; if the latter, the `01`
   codec errata must drop the key.
2. **Exclusivity source.** `40-D3` adds `ToolSchema::concurrency`. The
   alternative is a `07` §4.3 static table with no struct change. The struct
   field is preferred (per-tool, inspectable, survives third-party/MCP tools);
   confirm the `07` owner's preference. For MCP tools (spec 15), the default
   `Exclusive` is safe but pessimistic.
3. **MCP concurrency.** `src/mcp/mcp_tool.cpp` is a clamp call site (§3.4).
   Should an MCP tool ever be `ParallelSafe`? Default `Exclusive` unless the
   MCP server declares otherwise; deferred to the `07`/`15` owners.
4. **Reminder scope.** `40-D11` counts consecutive identical calls over the
   **session**. dsh's scope is not restated in the sources we have. If a
   turn-scoped counter is preferred, it changes `RepeatToolReminder`'s lifetime
   (per-turn reset) but not its message shape.
5. **Config key names.** `40-D12` leaves the exact `21` key names open
   (e.g. `[context] prune_threshold_code_points`, `[tools]
   retention_strategy`, `[agent] repeat_tool_reminders`). The defaults are
   pinned; only the spelling is deferred to the `21` errata.
6. **`07` §5.3 summarizer text.** `06` §5.3 (:700-702) still says the
   summarization call goes through "the same `LLMProvider` seam"; Wave 1's
   `LlmRuntime` re-seam (`28` §8; `32` C20) supersedes that sentence. This is
   already resolved by `28`/`31`/`32` and is **not** a contradiction, but the
   `06` text should be amended by the `31` errata for clarity.
7. **Synthetic-result outcome on abort.** `26-D10` (:148) says the scheduler
   records "synthetic **error** results on abort", while `06` §5.6 (:803-805)
   says a cancelled turn's unmatched `tool_use` blocks project to
   `ToolResult{outcome = Cancelled}` (`01` I12). This spec resolves it by layer:
   the scheduler's in-step abort synthesis uses `Cancelled` when the turn token
   fired (so the durable log matches `06` §5.6) and fabricates nothing on an
   internal scheduler failure (the terminal-event path synthesizes). **Confirm
   with the `06` owner** whether `26-D10`'s "error" wording intends a distinct
   `Error` outcome for a non-cancellation abort; if so, `40-D4` and `40-I3`
   gain an `Error` branch.

---

## 15. References

- `docs/design/26-dsh-alignment.md` — design source: §2.2.2 (:519-529),
  §2.2.3 (:540-562), §2.2.4 (:564-598), §2.2.5 (:600-609), §3.2 (:1290-1293),
  G13 (:1228), G15 (:1230).
- `docs/design/26-dsh-alignment-part2.md` — decisions `26-D10` (:148), `D11`
  (:149), `D12` (:150), `D13` (:151), `D21` (:159); §4.3.5 (:614-639), §4.3.6
  (:641-693), §4.3.7 (:703-737), §4.3.9 (:802-935), §4.3.9.1 (:936-975),
  §4.3.9.2 (:976-1022), §4.4 (:1073-1100), §5 (:1356-1503), Stage B (:1386-1396).
- `docs/design/32-compaction-errata.md` — §3 (:175-224), §4 (:238-280), §5
  (:283-378), §7 (:409-437), §8 (:441-455), §9 (:456-486), §11 (:500-528).
- `docs/design/13-context-compaction.md` — §3.1 (:196-217), §4.1 (:469-493),
  §5.2 (:585-660), §5.3, §5.5 (:748-822), §10.
- `docs/design/07-tools-execution.md` — §3.2 (:250-292), §4.3, §5.2 (:545-612),
  §5.5 `ToolConfig` (:672-715), §8.1 (:1096-1149).
- `docs/design/06-agent-loop.md` — §5.1 (:545-647), §5.2 (:648-679), §5.3
  (:681-707), §5.5 (:769-789), §5.6 (:791-805), §10 A1–A18 (:1000-1066),
  §11 A-F1–A-F18 (:1099-1118).
- `docs/design/36-prompt-registry.md` — §3 provenance, §6 invariants, §8 dsh
  mapping, §9 dependencies, §10 decisions, §11 test plan.
- `docs/design/34-assembler-replay-errata.md` — replay harness and status-header
  style.
- `docs/design/21-config-jsonc-errata.md`, `24-agent-lifetime-errata.md`,
  `28-llm-service-boundary-errata.md`, `01-session.md`, `02-persistence.md`.
- Code (tree facts): `include/ymh/tools/tool.hpp:24,39-45,79-82`;
  `src/tools/tool.cpp:253`; `src/tools/tool_registry.cpp:327`;
  `src/mcp/mcp_tool.cpp:36,69`; `include/ymh/session/events.hpp:135-146,164-177`;
  `include/ymh/execution/config.hpp:14-17`;
  `include/ymh/session/session_persistence.hpp:62`;
  `src/session/session.cpp:450-458`; `include/ymh/agent/agent.hpp:88-94,106`;
  `include/ymh/agent/agent_loop.hpp:71-73,162`;
  `src/agent/agent_loop.cpp:380,456-484,486,582,737,768,870-877,1000-1013,1062-1066`;
  `include/ymh/agent/compactor.hpp:31-62,116-127`; `src/agent/compactor.cpp:204`;
  `include/ymh/llm/llm_runtime.hpp:185`; `src/llm/llm_runtime.cpp:294`;
  `include/ymh/llm/stream.hpp:228`; `tests/unit/output_test.cpp:75,90,103`.

---

## 16. Revision log

| Rev | Change |
|---|---|
| 1 | Initial write. Pins the Wave-4 owning spec as number **40** (the reserved `28-output-retention.md` is taken). Owns: the bounded-parallel scheduler (`26-D10`), the retention library and the `07` `ToolResult` omission-metadata break (`26-D11`), repeat-tool reminders (`26-D21`), the `clamp_tool_result` retirement, and the Wave-4 wiring of the `32`-owned pruner (`26-D12`) and trigger taxonomy (`26-D13`). Adds `ToolConcurrencyMode`, `40-I1`–`40-I16`, `40-F1`–`40-F14`, `40-D1`–`40-D14`, and the test plan. Design only — no code, no other file changed. |

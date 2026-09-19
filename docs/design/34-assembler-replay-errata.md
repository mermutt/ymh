# 34 — Assembler & Replay Errata (dsh alignment, Wave 2 prerequisite)

```
Status: Rev 3 written · verified: — · reviewer: — (tracked in DESIGN_STATUS.md)
Revision: Rev 3 (2026-09-19). Pins the shipped `AssistantStreamPrinter`
          contract (§15 item 1, decision 34-D13, invariants 34-I11–34-I16,
          failure mode 34-F9), closing the Rev 2 open risk for the two `ymh run`
          paths. Back-fills the owning spec for code already implemented and
          green; no production surface changes. See §16.
          Rev 2 — fixes the Rev 1 gate's MEDIUM-1 (the §9.2 replay message
          prefix was bounded by the header sequence; it is now bounded by the
          attempt's dispatch position, with a harness assertion that detects a
          wrong prefix) and its six LOWs. See §16.
          Rev 1 — initial write. Closes GAP 2 and GAP 3 of the Wave-2
          design-first audit (/tmp/opencode/wave2-gaps.md). Pins the canonical
          block-assembly algorithm, `TimedStreamEvent.at`'s epoch/source, the
          sink→assembler/accumulator wiring, `interrupted_blocks()`'s contract,
          the `AssistantMessage`-vs-`AssistantAttempt` settlement predicate
          (including the `agent_loop.cpp:~938-963` reorder and the tests it
          affects), the accumulator packing rule, and the replay-harness shape.
Component: 34 (errata) — amends 08-llm-provider.md by reference; also pins
           changes owned by 06-agent-loop.md / 31-agent-loop-errata.md
           (`AgentServices`) and 26-dsh-alignment-part2.md §4.3.4 (the
           assembler types), each owned by its own errata.
Depends on: docs/design/26-dsh-alignment-part2.md (Rev 7, GATE PASS) §4.3.4,
            §4.3.9, §4.3.9.1, §5.2;
            docs/design/28-llm-service-boundary-errata.md (Rev 3, GATE PASS)
            §4.1–§4.4, §5.3, §12;
            docs/design/29-event-family-errata.md (GATE PASS) §3.2, §4.2;
            docs/design/30-architecture-cascade-errata.md (Rev 2, independent
            review PASS — the top-level gate is blocked only on the user's
            §6 P3 no-downgrade sign-off) §5.3;
            docs/design/31-agent-loop-errata.md (Rev 2, GATE PASS) §3.2, §5.2;
            docs/design/08-llm-provider.md (verified) §2–§5, §10–§11
Scope: (1) the block-assembly algorithm; (2) `TimedStreamEvent.at`'s epoch,
       source, unit, and stamping site; (3) the per-attempt
       sink→assembler/accumulator/coalescer wiring; (4) `interrupted_blocks()`'s
       contract; (5) the per-attempt `AssistantMessage`-vs-`AssistantAttempt`
       settlement predicate and its test effects; (6) the
       `AssistantStreamAccumulator` packing rule and `expand()` validation;
       (7) the test-only replay harness's rebuild/reconstruct API, its
       reconstruction rule, and its error surface; (8) the `ReplayEnvelope`
       nested JSON keys. Claims spec number **34**; sibling `33` owns the
       `StreamEvent` JSON codec (GAP 1) and is not touched here.
```

## 1. Purpose, authority, and what this errata is for

### 1.1 The problem this closes

`30-architecture-cascade-errata.md` §5.3 (`:320`) lists, for Stage-B Wave 2:

> | 2 | `01` assistant-stream payloads; `08` assembler/replay errata |

The `08` assembler/replay errata did not exist. Wave 2 — the canonical assembler
plus the assistant stream/replay (`26-dsh-alignment-part2.md` §5.2 Wave 2 list,
`:1424-1449`) — therefore had **signatures but no algorithms**: `26 §4.3.4`
(`:518-609`) pins the C++ shapes and says "Single canonical chunk→message
algorithm (mirrors dsh `BlockAssembler`)" (`:541`), which is a pointer to
TypeScript source, not a pinned rule. `28 §11` (`:875`) explicitly **defers**
the work:

> | `ReplayEnvelope` / assistant stream | Wave 2 (`BlockAssembler`, D8/D9) | 26 §4.3.4 |

This errata supplies the missing rules. It is an **errata**: it amends `08`
(and, where noted, `06`/`31` and `26 §4.3.4`) **by reference only**. It edits no
other file. It does not restate what `28`/`29`/`31` already pin; those are cited.

### 1.2 What is already pinned (cited, not restated)

| Thing | Pinned by | This errata's relationship |
|---|---|---|
| `ReplayEnvelope`, `TimedStreamEvent`, `TextRun`, `ReasoningRun`, `ToolCallRun`, `ChunkRecord`, `AssistantStreamRecord`, `BlockAssembler`, `AssistantStreamAccumulator`, `expand()` signatures | `26 §4.3.4` `:529-583` | **unchanged**; this errata pins the algorithms behind them |
| `Finished` gains `std::optional<ReplayEnvelope> replay_state`; `payload::AssistantMessage` gains `stream` + `replay_state` | `26 §4.3.4` `:586-599` | **unchanged**; the assembler's accessor contract is pinned here |
| `EventType::AssistantAttempt` / `assistant/attempt` / `payload::AssistantAttempt`; `AssistantChunk` becomes live-only; codec checklist | `29 §3.1-§3.2` `:115-187`, §4.2 `:319-354` | **unchanged**; §7 consumes the settlement event |
| `FrozenRequest` / `canonical_template()` / `canonical_json()` / `template_digest()`; the reconstruction guarantee | `28 §4.1-§4.4` `:441-516`, §12 `:880-931` | **unchanged**; §9 gives it a callable harness |
| The loop dispatch path (`prepare_call` → `PreparedCall::stream`), one attempt per stream, the compaction re-attempt is a new request | `31 §5.2-§5.3` `:386-453`; `28 §6` | **unchanged**; §6 wires the sink into it |
| The `StreamEvent` JSON codec (`{"type":"chunk",…}` and the run records) | sibling `33` (GAP 1) | **referenced, not defined** |

### 1.3 Verified-against-the-tree note

Every claim about shipped code below was checked against the tree at the cited
line. The relevant shipped surfaces are: `include/ymh/llm/stream.hpp`
(`:129-192`), `include/ymh/llm/tool_call_assembler.{hpp,cpp}`,
`include/ymh/llm/llm_provider.hpp` (`LLMResponse` `:49-59`),
`include/ymh/llm/llm_runtime.hpp` (`FrozenRequest` `:52-72`,
`PreparedCall` `:100-124`), `include/ymh/agent/message.hpp` (`:111-138`),
`include/ymh/session/events.hpp` (`AssistantMessage` `:104-108`,
`LlmRequestHeader` `:221-233`), `include/ymh/session/session.hpp`
(`append` `:256-264`, `emit` `:282-285`), `src/agent/agent_loop.cpp`
(`:785-998`), `src/agent/context_assembler.cpp` (`:41-65`),
`src/agent/chunk_coalescer.cpp` (`:52-71`), `src/session/session.cpp`
(`deriveMessages` `:362-...`, `emit` `:651-659`), `src/session/events.cpp`
(`:319-334`), `src/cli/headless.cpp` (`:197-208`),
`src/ui/ui_event_adapter.cpp` (`:90-110`), `src/llm/fake_llm.cpp` (`:93-218`).

## 2. Amendment register

| ID | Amendment | Kind | Owning spec |
|---|---|---|---|
| **34-D1** | Pin the header layout: `ReplayEnvelope` and `TimedStreamEvent` live in `include/ymh/llm/stream.hpp`; `BlockAssembler` / `AssistantStreamAccumulator` / the four record types / `expand()` live in a new `include/ymh/llm/assistant_stream.hpp`. | Add. | 08 |
| **34-D2** | Pin the `BlockAssembler` block-assembly algorithm (§4). | Add. | 08 |
| **34-D3** | Pin `interrupted_blocks()`'s contract: reasoning+text only, all tool-use blocks dropped, always a prefix-subsequence of `blocks()`; **not** consumed by the Wave-2 durable path. | Add. | 08 |
| **34-D4** | Pin `TimedStreamEvent.at`: `steady_clock`, per-attempt epoch, milliseconds; add the `AgentServices::stream_clock` injection seam. | Add. | 08 / 06 / 31 |
| **34-D5** | Pin per-attempt assembler/accumulator/coalescer lifetime and the sink body. | Brk. | 08 / 06 |
| **34-D6** | Pin per-attempt settlement: `Completed` → `AssistantMessage`; `Cancelled`/`Failed` → `AssistantAttempt`; the retried attempt is settled before compaction; the `agent_loop.cpp:~938-963` reorder. | Brk. | 08 / 06 |
| **34-D7** | Pin the accumulator packing rule, run ordering, and the orphan-delta rule. | Add. | 08 |
| **34-D8** | Pin `expand()`'s validation and its loud `CorruptionError` surface. | Add. | 08 |
| **34-D9** | Declare the replay harness **test-only** and pin its rebuild/reconstruct API + `ReplayMismatch` surface. | Add. | 06 (test) |
| **34-D10** | Pin the `ReplayEnvelope` nested JSON keys (`provider`, `version`, `state`), closing the `26 §4.3.9.1` omission. | Add. | 08 / 29 |
| **34-D11** | Pin `BlockAssembler::usage()` (Finished-authoritative) as the **single** durable usage source, superseding the loop's `response.usage`/`streamUsage` preference. | Brk. | 08 / 06 |

**Build-gate note.** `deriveMessages`'s projection switch (`session.cpp:375`) has
no `default:` and is gated by `-Werror=switch`. The `AssistantAttempt` case is
`29`'s obligation (`29 §4.2 :323`), not restated here.

## 3. Types and header layout (34-D1)

### 3.1 Why `ReplayEnvelope` and `TimedStreamEvent` sit in `stream.hpp`

`26 §4.3.4` pins `Finished` to gain `std::optional<ReplayEnvelope> replay_state`
(`:586-589`). `std::optional<T>` requires `T` to be a **complete type** at the
point of instantiation, so `ReplayEnvelope` cannot live in a header that
`#include`s `stream.hpp` — it must be defined **before** `Finished`. The shipped
`stream.hpp` (`:129-192`) is the stream algebra's home and is already included by
every adapter. Therefore:

- **`include/ymh/llm/stream.hpp`** gains, in this order:
  1. `ReplayEnvelope` (before `Finished`; no dependency on `StreamEvent`),
  2. `Finished::replay_state`,
  3. `TimedStreamEvent` (after the `StreamEvent` variant; it needs `StreamEvent`).

  `ReplayEnvelope` has no `StreamEvent` dependency, and `TimedStreamEvent` is the
  trivial `{at, event}` pairing that both the sink and the accumulator speak
  (`26 §4.3.4 :536-539`), so keeping both in the algebra header adds no cycle.

- **`include/ymh/llm/assistant_stream.hpp`** (new) holds the records,
  `BlockAssembler`, `AssistantStreamAccumulator`, and `expand()`. It includes
  `stream.hpp`. No adapter includes it; only the agent loop and tests do.

### 3.2 Pinned shapes (unchanged from `26 §4.3.4` `:529-583`)

```cpp
// stream.hpp (34-D1)
struct ReplayEnvelope {
    std::string    provider;
    std::uint32_t  version = 1;
    nlohmann::json state;              // adapter-private; JSON-serializable
};

struct Finished {
    FinishReason                       reason = FinishReason::Other;
    std::optional<Usage>               usage;
    std::optional<ReplayEnvelope>      replay_state;   // NEW (26 :586-589)
};

// stream.hpp (34-D1): stamped by the loop's sink, never by a provider (L1).
struct TimedStreamEvent {
    std::chrono::milliseconds at{0};
    StreamEvent               event;
};
```

```cpp
// assistant_stream.hpp (34-D1), verbatim from 26 §4.3.4 :559-581
struct TextRun      { std::size_t index; std::int64_t time0_ms; std::vector<std::int64_t> dt_ms; std::vector<std::string> texts; };
struct ReasoningRun { std::size_t index; std::int64_t time0_ms; std::vector<std::int64_t> dt_ms; std::vector<std::string> texts; };
struct ToolCallRun  { std::size_t index; std::int64_t time0_ms; std::vector<std::int64_t> dt_ms;
                      ToolCallId id; std::optional<std::string> name; std::vector<std::string> args; };
struct ChunkRecord  { std::int64_t time_ms; StreamEvent event; };

using AssistantStreamRecord = std::variant<TextRun, ReasoningRun, ToolCallRun, ChunkRecord>;

class BlockAssembler { /* §4 */ };
class AssistantStreamAccumulator { /* §6, §8 */ };
[[nodiscard]] std::vector<TimedStreamEvent> expand(const std::vector<AssistantStreamRecord>&);
```

**No `BlockAssembler` constructor parameters are added.** It is default-
constructible, has no clock, no error surface, and no thread affinity beyond its
owning attempt (§6). This matches the pinned `push`/accessor-only shape.

## 4. The block-assembly algorithm (34-D2, GAP 2 item 1)

### 4.1 State

`BlockAssembler` holds, per attempt:

| Field | Type | Meaning |
|---|---|---|
| `text_` | `std::string` | concatenation of every `TextDelta.text` pushed |
| `reasoning_` | `std::string` | concatenation of every `ReasoningDelta.text` pushed |
| `calls_` | `std::vector<CallState>` | one entry per tool-call `index` seen, in first-seen order |
| `finished_usage_` | `std::optional<Usage>` | the last `Finished.usage` that was set |
| `advisory_usage_` | `std::optional<Usage>` | the last `UsageEvent.usage` |
| `finish_` | `FinishReason` = `FinishReason::Stop` | last `Finished.reason` |
| `saw_finished_` | `bool` = `false` | whether any `Finished` was pushed |
| `replay_state_` | `std::optional<ReplayEnvelope>` | last `Finished.replay_state` |

`CallState` is `{std::uint32_t index; ToolCallId id; std::string name;
std::optional<ToolCallAssembled> finished;}`. `finished` is set by
`ToolCallFinished` and is **authoritative** (`26 §4.3.4 :597-599`). There is
**no** raw-fragment mirror: `blocks()` emits only `finished.call` (§4.3 step 3),
and raw deltas are the accumulator's concern, not the assembler's (§8.4).

### 4.2 `push(const StreamEvent&)` — the transition table

`push` is total, never throws, and has no error channel (the pinned signature
returns `void`; the adapter's `ToolCallAssembler` is where protocol violations
become a terminal `StreamError`, L10/L-F7/L-F14).

| Event | Effect |
|---|---|
| `TextDelta{text}` | `text_ += text` |
| `ReasoningDelta{text}` | `reasoning_ += text` |
| `ToolCallStarted{index,id,name}` | open a `CallState{index,id,name}` if none exists for `index`; if one exists, **first-wins** on `id`/`name` (the adapter rejects a duplicate start as `ProviderInternal`; the assembler stays total) |
| `ToolCallDelta{index,fragment}` | **no effect** on `blocks()`. The assembler keeps no raw-fragment mirror: the authoritative arguments are `ToolCallFinished.call.arguments` (`26 §4.3.4 :597-599`), and a delta for an `index` with no open call cannot form a `tool_use` block. (The accumulator records the raw delta itself — §8.4.) |
| `ToolCallFinished{index,call}` | set/replace the `CallState` for `index` and store `finished = call` (authoritative `id`/`name`/`arguments`) |
| `UsageEvent{usage}` | `advisory_usage_ = usage` |
| `Finished{reason,usage,replay_state}` | `finish_ = reason`; if `usage` has a value, `finished_usage_ = usage`; `replay_state_ = replay_state`; `saw_finished_ = true`. A second `Finished` overwrites (defensive; never emitted) |
| `StreamError{error}` | **no effect** on blocks/usage/finish/replay_state; the terminal disposition is `LLMResponse`'s, not the assembler's |

### 4.3 `blocks()` — the canonical message content

`blocks()` returns, in this exact order:

1. **one** `ContentBlock{kind = Reasoning, text = reasoning_}` iff `reasoning_`
   is non-empty;
2. **one** `ContentBlock{kind = Text, text = text_}` iff `text_` is non-empty;
3. **one** `ContentBlock{kind = ToolUse, tool_call_id = call.id,
   tool_name = call.name, arguments = call.arguments}` for every `CallState`
   whose `finished` is set, **ascending by `index`**.

This reproduces the shipped loop exactly (`agent_loop.cpp:927-936`: reasoning,
then text, then `response.tool_calls` in index order). It is the pinned
`ContentBlock` shape (`message.hpp:111-119`).

### 4.4 `interrupted_blocks()` — the interrupted prefix (34-D3, GAP 2 item 4)

**Contract.** `interrupted_blocks()` returns steps 1 and 2 of `blocks()` **and
nothing else**:

- one reasoning block iff `reasoning_` non-empty, then one text block iff
  `text_` non-empty;
- **all tool-use blocks are dropped**, whether or not a `ToolCallFinished`
  arrived.

**Why.** `26 §4.3.4 :546-548` pins the rationale verbatim: "Prefix an interrupted
stream can safely finalize; tool calls are dropped because interruption precedes
dispatch." In ymh the loop dispatches tools only after a `Completed` outcome
(§7), so an interrupted attempt never dispatched any tool call; emitting a
`tool_use` block with no `ToolResult` would leave `deriveMessages`'s pending
set (`session.cpp:430-435`, `:395-413`) to synthesize a result for a call that
never ran.

**Ordering guarantee.** `interrupted_blocks()` is exactly the reasoning+text
prefix of `blocks()`: same blocks, same order, same text; formally
`interrupted_blocks() == { b ∈ blocks() : b.kind != ToolUse }`. The two agree on
every block they share, byte-for-byte.

**Consumption (recorded, not silent).** The Wave-2 durable settlement does
**not** call `interrupted_blocks()`: an interrupted attempt commits
`AssistantAttempt`, whose payload is `{turn, step, stream}` and carries **no**
content blocks (`26 §4.3.9.1 :949`; `29 §4.2 :323`). `interrupted_blocks()`
exists for a consumer that must finalize a partial assistant message without
committing model-visible history (a future resumed-stream path, or a UI-only
finalize). This errata pins the contract; it does **not** claim a Wave-2
consumer. That is deliberate and recorded here rather than left implicit.

### 4.5 `usage()` — the authority order (34-D11)

`usage()` returns `finished_usage_` if it has a value, else `advisory_usage_`, else
`std::nullopt`. `26 §4.3.4 :549` pins this: "`Finished.usage` authoritative;
`UsageEvent` advisory."

The loop must use `BlockAssembler::usage()` — **not** `LLMResponse::usage` and
**not** the sink-captured `streamUsage` — as the single durable usage source
(§6.4, §7). Today the loop prefers `response.usage` over `streamUsage`
(`agent_loop.cpp:941-943`). That preference is superseded for durable output;
`LLMResponse::usage` (`llm_provider.hpp:54`) remains a diagnostic mirror only.

### 4.6 `finish()` and `replay_state()`

- `finish()` returns `finish_`, which defaults to `FinishReason::Stop` when no
  `Finished` was pushed (`26 §4.3.4 :550`). It is only meaningful when
  `saw_finished_`. **Wave-2 consumption: none** — the terminal reason remains
  `LLMResponse::finish` (`31 §5.2 :414-415`, "the loop maps it to the terminal
  event exactly as today"). The accessor is pinned for replay/UI consumers.
- `replay_state()` returns `replay_state_` (the last `Finished.replay_state`).
  It is **consumed**: a settled `AssistantMessage.replay_state` is this value
  (§7). It is the single durable home for replay state (`26 §4.3.4 :594-596`).

### 4.7 Invariants (B)

- **34-B1.** `blocks()`, `interrupted_blocks()`, `usage()`, `finish()`, and
  `replay_state()` are pure functions of the pushed events: no clock, no global,
  no randomness.
- **34-B2.** Block order is always reasoning → text → tool-use.
- **34-B3.** There is at most one reasoning block and at most one text block; all
  deltas of a kind coalesce into that single block regardless of interleaving.
- **34-B4.** Tool-use blocks are ascending by provider `index` and carry the
  `ToolCallFinished.call` object (the authoritative `id`/`name`/`arguments`);
  the assembler keeps no raw-fragment state.
- **34-B5.** `interrupted_blocks()` is the reasoning+text prefix of `blocks()`.
- **34-B6.** `usage()` prefers `Finished.usage`; `finish()` defaults to `Stop`.

## 5. `TimedStreamEvent.at` (34-D4, GAP 2 item 2)

### 5.1 Epoch, source, unit, stamping site

| Property | Pinned value |
|---|---|
| **Clock** | `std::chrono::steady_clock` — **monotonic**, never `system_clock` |
| **Epoch** | the **stream start of the current provider attempt**: the instant captured immediately before that attempt's `PreparedCall::stream(...)` call |
| **Value** | `at = clock.now() - stream_start`, a non-negative `std::chrono::milliseconds` |
| **Stamping site** | the loop's `StreamSink`, on the same serial stream order the provider invokes it (`stream.hpp:189-192`, L3). Providers never stamp: they emit bare `StreamEvent`s (L1) |
| **Lifetime** | one `stream_start` per provider call. The compaction re-attempt is a **new** attempt and captures a **new** `stream_start` |

Rationale for monotonic over wall: the offset is a *duration within one stream*,
so a wall-clock step (NTP, DST) must not corrupt it. The codebase already uses
`steady_clock` for in-process timing (`chunk_coalescer.hpp:46`,
`permission_broker.hpp:42`, `mcp_client.hpp:31`).

### 5.2 The injection seam (amends `06`/`31` by reference)

To make golden assembly deterministic, `AgentServices` (`agent_loop.hpp:43-69`)
gains:

```cpp
// 34-D4: monotonic source for TimedStreamEvent.at. Production default is
// steady_clock::now; tests inject a deterministic reader.
using StreamClock       = std::chrono::steady_clock;
using StreamClockReader = std::function<StreamClock::time_point()>;
// ...
StreamClockReader stream_clock = std::chrono::steady_clock::now;
```

This mirrors the existing `ClockReader`/`WallClock` injection pattern
(`permission_broker.hpp:49,65`; `compactor.hpp:111,122`; `mcp_client.hpp:32,101`).
The loop calls `services_.stream_clock()` once per attempt for `stream_start`
before `call.stream(...)`, and once per pushed event for `at`. The default keeps
production construction unchanged; no other `AgentServices` member changes.

### 5.3 Reproducibility limit (stated, not overclaimed)

`at` values are **not** reproducible across real runs (they measure real
elapsed time). The replay harness (`26 §5.2 :1526-1540`) asserts byte-identity of
`canonical_json()` — the **request**, which contains no stream timings — so the
timing non-determinism does not weaken the reconstruction contract. The
accumulator codec's `time0_ms`/`dt_ms` are made deterministic in tests by
injecting a manual `stream_clock` (§5.2); they are not compared across process
runs. This errata does not claim timing reproducibility.

## 6. The sink → assembler/accumulator wiring (34-D5, GAP 2 item 3)

### 6.1 Who owns what

| Object | Lifetime | Owner | Speaks |
|---|---|---|---|
| `BlockAssembler` | one per **provider attempt** | the loop's executor thread | `StreamEvent` |
| `AssistantStreamAccumulator` | one per **provider attempt** | the loop's executor thread | `TimedStreamEvent` |
| `ChunkCoalescer` | one per **provider attempt** | the loop's executor thread | text/reasoning strings → live `AssistantChunk` |
| `messageId` | one per **step** | the loop | `MessageId` (the durable message id) |

A fresh trio is constructed **inside** the attempt loop, immediately before
`call.stream(...)` (`agent_loop.cpp:856-868`), not once per step as
`ChunkCoalescer` is today (`:829-831`). This is required by §7's per-attempt
settlement: a retried attempt's stream must not leak into the retry's durable
message. The `messageId` stays per step, so a retried-then-successful step still
produces one `AssistantMessage` with that id.

### 6.2 The sink body (pinned shape)

```cpp
const auto stream_start = services_.stream_clock();
StreamSink sink = [&](const StreamEvent& event) -> SinkFlow {
    const TimedStreamEvent timed{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            services_.stream_clock() - stream_start),
        event};
    assembler.push(event);                                  // durable content
    const TimedStreamEvent live = accumulator.push(timed);  // durable stream + live copy
    // Live path only (34-D5): non-delta events are not live-published in Wave 2.
    if (const auto* delta = std::get_if<TextDelta>(&live.event)) {
        coalescer.onText(delta->text);
    } else if (const auto* delta = std::get_if<ReasoningDelta>(&live.event)) {
        coalescer.onReasoning(delta->text);
    }
    return SinkFlow::Continue;
};
```

The sink stays cheap, non-blocking, non-throwing, non-reentrant (`stream.hpp:189-192`).
It no longer reads `response.usage` or accumulates `text`/`reasoning`/`streamUsage`
by hand — those three locals (`agent_loop.cpp:832-834`) are **deleted**; the
assembler and accumulator own them.

### 6.3 Live publication

`ChunkCoalescer` is retained and is the live delta publisher. `29 §4.2 :353`
pins the required change: `ChunkCoalescer::flush` (`chunk_coalescer.cpp:52-71`)
switches from `session_.appendBatch(events)` (`:68`) to `Session::emit`
(`session.hpp:282-285`; `session.cpp:651-659`), so `AssistantChunk` is published
live-only and never durably appended by a new binary. The sink feeds the
coalescer from the **accumulator's returned detached event** (not the raw
event), which is the purpose of `push`'s return value (`26 §4.3.4 :571-575`).
The A5 barrier (`chunk_coalescer.hpp:31-32`) is retained: `coalescer.flush()` is
called once per attempt, before that attempt's settlement (§7.3), so live deltas
precede the durable finish row in bus order.

**Flush error handling moves with the call.** Shipped `ChunkCoalescer::flush()`
can throw `LeaseLost`/`StoreError` (`chunk_coalescer.cpp:52-71`); the loop today
catches both once per step (`agent_loop.cpp:917-925`) and terminates via
`appendTurnFailed`. Per-attempt flushing moves that `try`/`catch` **inside** the
attempt loop, wrapping that attempt's single `flush()`: on a throw the loop
appends `TurnFailed` (`appendTurnFailed`) and returns **without settling the
attempt** — no `AssistantMessage`/`AssistantAttempt` follows a failed flush. The
settlement order is therefore `stream → flush → settle`, per attempt.

### 6.4 Snapshot ownership

`AssistantStreamAccumulator::snapshot()` returns a **detached immutable copy** of
the record list (`26 §4.3.4 :576-577`). The loop stores it once per settled
attempt, in the `AssistantMessage.stream` / `AssistantAttempt.stream` field. The
loop must not mutate the returned vector afterward; `expand()` (`§8.5`) is the
only reader that reinterprets it.

## 7. The settlement predicate (34-D6, GAP 2 item 5)

### 7.1 The rule

**Settlement is per provider attempt**, evaluated when `call.stream(...)`
returns. Exactly one durable event is committed per settled attempt
(`26 §4.3.4 :601-609`: "dsh commits exactly one durable event per settled
attempt").

```
settle(attempt, response, assembler, accumulator, messageId, turn, step):
    if response.outcome == StreamOutcome::Completed:
        append payload::AssistantMessage{
            id          = messageId,
            content     = assembler.blocks(),
            usage       = assembler.usage(),
            stream      = accumulator.snapshot(),
            replay_state= assembler.replay_state(),
        }
    else:  // Cancelled or Failed
        append payload::AssistantAttempt{
            turn   = turn,
            step   = step,
            stream = accumulator.snapshot(),
        }
```

- `AssistantMessage` is appended **before** any tool dispatch and before the
  terminal event, preserving the current commit order (`agent_loop.cpp:944`
  precedes `:976`).
- `AssistantAttempt` is appended **before** the terminal event and, for a
  retried attempt, **before** compaction runs (§7.2). It is ignored by
  `deriveMessages` (`29 §4.2 :323`), so it adds no model-visible history.
- `interrupted_blocks()` is **not** consulted (§4.4).

### 7.2 The retried attempt

`26 §4.3.4 :603` names "a failed, **retried**, cancelled, or stream-error
attempt". The loop's only retry is the `ContextLengthExceeded` compaction
re-attempt (`agent_loop.cpp:888-909`; `31 §5.2 :393-405`). When attempt 0
returns `Failed` with `ContextLengthExceeded` and a retry will follow:

1. settle attempt 0 as `AssistantAttempt` (its own accumulator snapshot);
2. run compaction (`runCompaction`), re-assemble, build a **new** `FrozenRequest`;
3. start attempt 1 with a **fresh** assembler/accumulator/coalescer and a fresh
   `stream_start`.

The alternative — settle once per step using only the final attempt — is
**rejected**: it would silently drop the retried attempt's trace, contradicting
the explicit word "retried" in `26 §4.3.4 :603`. A step therefore emits one
attempt event per provider call; exactly one of them is an `AssistantMessage`
iff the final attempt completed.

### 7.3 The `agent_loop.cpp:~938-963` reorder (required change)

Today the loop appends `AssistantMessage` **unconditionally** at `:938-944`,
*before* checking `response.outcome` at `:952-963`; a failed or cancelled attempt
therefore commits partial content as a model-visible message. Required change:

| Current (`agent_loop.cpp`) | Target |
|---|---|
| `:927-936` hand-rolled content from `text`/`reasoning`/`response.tool_calls` | delete; use `assembler.blocks()` |
| `:938-944` unconditional `AssistantMessage` | **replace** with the `settle(...)` block above, called inside the attempt loop once `call.stream(...)` has returned **and** the attempt's `flush()` has run (§6.3 order: `stream → flush → settle`) |
| `:941-943` `response.usage` / `streamUsage` preference | `assembler.usage()` (34-D11) |
| `:952-963` `Cancelled`/`Failed` terminal handling | **unchanged** |
| `:965-996` `Completed` terminal/tool-dispatch handling | **unchanged**, except `usage` is the settled attempt's `assembler.usage()` captured at settlement |

The `slotCancelled` synthesis (`:911-915`) sets `outcome = Cancelled` **without a
provider call**, so it does **not** settle an attempt: settlement happens only
at the return of a real `call.stream(...)`, and a failed pool acquire never
reaches it. The turn still terminates with `TurnCancelled` (`:952-956`). Early
returns **before** the attempt loop (`runtime == nullptr` `:851-854`; context
assembly failure `:795-806`, `:816-823`, `:903-908`) likewise do not settle: no
provider dispatch occurred. This is the recorded boundary of the predicate.

### 7.4 Test effects (enumerated)

**Group A — settlement-predicate effects** (failed/cancelled no longer commits
an `AssistantMessage`):

| Test | Current behavior | Effect of 34-D6 |
|---|---|---|
| `tests/unit/agent_loop_test.cpp` `CancelMidStreamYieldsOneTurnCancelled` (`:344-368`) | partial `AssistantMessage` appended on cancel | replaced by one `AssistantAttempt`; test does not count messages, stays green; **extend** to assert `AssistantAttempt == 1`, `AssistantMessage == 0` |
| `tests/unit/agent_loop_test.cpp` `ProviderFailureYieldsOneTurnFailed` (`:370-394`) | empty `AssistantMessage` appended before `TurnFailed` | replaced by one `AssistantAttempt`; **extend** to assert `AssistantMessage == 0` |
| `tests/unit/agent_loop_test.cpp` `ContextLengthExceededRetriesOnce` (`:396-417`) | one `AssistantMessage`; no attempt record | one `AssistantAttempt` (attempt 0) + one `AssistantMessage` (attempt 1); **extend** |
| `tests/unit/agent_loop_test.cpp` `SecondContextOverflowFailsCompaction` (`:419-445`) | one `AssistantMessage`; `TurnFailed{CompactionFailed}` | two `AssistantAttempt` (both overflow attempts), zero `AssistantMessage`; **extend** |
| `tests/unit/headless_test.cpp` `ProviderFailureSetsNonZeroExit` (`:120-145`) | asserts terminal only | unaffected by the predicate; the durable-text path changes (Group B) |
| `tests/unit/headless_test.cpp` `CancellationProducesTurnCancelled` (`:147-165`) | asserts terminal only | unaffected by the predicate |
| `tests/unit/host_runtime_test.cpp` `AgentPromptRunsOnExecutor` (`:1171`) | `AssistantMessage >= 1` on success | unaffected (success path) |

**Group B — durable-stream effects** (`AssistantChunk` is no longer appended by
new binaries; 29 §4.2 `:353`):

| Test | Effect |
|---|---|
| `tests/unit/agent_loop_test.cpp:53-61` `chunk_text()` reads durable `AssistantChunk` | returns empty after Wave 2; `:275` assertion breaks — must read the durable text from `AssistantMessage.content` / embedded `stream` |
| `tests/unit/agent_coalescer_test.cpp:15-19` `chunks()` reads `session.events()` | returns empty; the whole test must observe the **live** publication (bus subscription) instead of the durable log |
| `tests/unit/host_runtime_test.cpp:1171` | counts `AssistantMessage`, not chunks → unaffected |
| `tests/unit/session_export_test.cpp:68`, `tests/unit/ui_render_golden_test.cpp:125`, `tests/unit/transport_server_test.cpp:552,577`, `tests/integration_ownership_two_supervisor_test.cpp:311-342` | build/consume **synthetic** `AssistantMessage`/`AssistantChunk` events → unaffected |

**Group C — usage-source effect** (34-D11): loop tests that drive a provider
which sets `LLMResponse::usage` but never emits `Finished{usage}` would lose
`TokenUsage`. In the shipped tree the loop tests use `FakeLLM`, which emits
`Finished{step.finish, step.usage}` (`fake_llm.cpp:214`) and sets
`response.usage` (`:212`), so no shipped loop test regresses. New stubs must
emit `Finished` with usage.

## 8. Accumulator packing and `expand()` (34-D7, 34-D8)

### 8.1 Packing rule

The accumulator walks the timed events in push order and groups **maximal runs
of same-kind delta events with no intervening non-delta event**
(`26 §4.3.4 :554-558`):

- a run of `TextDelta` → one `TextRun`;
- a run of `ReasoningDelta` → one `ReasoningRun`;
- a `ToolCallStarted{index,id,name}` opens a `ToolCallRun` for that `index`;
  subsequent `ToolCallDelta{index,·}` append to it; a delta for a **different**
  index closes the open run; `ToolCallFinished` closes the run;
- every other event (`ToolCallFinished`, `UsageEvent`, `Finished`,
  `StreamError`) → one `ChunkRecord{time_ms = at, event}` verbatim.

### 8.2 `index` semantics

- `ToolCallRun.index` = the provider-assigned `ToolCallStarted.index` (needed by
  `expand()` to reconstruct the delta indices).
- `TextRun.index` / `ReasoningRun.index` = the 0-based ordinal of that run
  among runs **of the same kind** within the snapshot (first `TextRun` = 0,
  second = 1). The in-memory `StreamEvent` has no provider index for
  text/reasoning, so the ordinal is the only deterministic value; it is
  informational and does not affect `expand()`.

### 8.3 Timestamps

For every run, `time0_ms` is the `at` of the run's **first** chunk and
`dt_ms[k]` is `at[k+1] - at[k]` (all non-negative). Array-size invariants:

| Record | Invariant |
|---|---|
| `TextRun` / `ReasoningRun` (N deltas, all chunks are deltas) | `texts.size() == N`, `dt_ms.size() == N-1` |
| `ToolCallRun` (one `Started` anchor + N deltas) | `args.size() == N`, `dt_ms.size() == N` (`dt_ms[0]` is `Started`→first delta); `time0_ms` is the `Started`'s `at`; `id`/`name` are the `Started`'s |

### 8.4 Orphan delta rule

A `ToolCallDelta` whose `index` has no open `ToolCallStarted` is recorded as a
`ChunkRecord{event = ToolCallDelta}` **verbatim**; it never fabricates a
`ToolCallRun` (which would have an empty `id` and could not be expanded
faithfully). The shipped adapter's `ToolCallAssembler` rejects a delta for an
unknown index as `ProviderInternal` (`tool_call_assembler.hpp:33-40`), so this
path is unreachable in production; the rule keeps the accumulator total and
lossless.

### 8.5 `expand()` validation (34-D8)

`expand(snapshot)` walks the records in order and reconstructs the exact
`TimedStreamEvent` sequence that was pushed. The per-delta timestamp is
reconstructed from `time0_ms`/`dt_ms` (§8.3), stated explicitly here:

- `TextRun` → N `TimedStreamEvent{at_k, TextDelta{texts[k]}}` with
  `at_k = time0_ms + Σ_{j<k} dt_ms[j]`;
- `ReasoningRun` → N `TimedStreamEvent{at_k, ReasoningDelta{texts[k]}}` with the
  same `at_k = time0_ms + Σ_{j<k} dt_ms[j]`;
- `ToolCallRun` → one `TimedStreamEvent{time0_ms, ToolCallStarted{index,id,name}}`
  then N `TimedStreamEvent{at_k, ToolCallDelta{index, args[k]}}` with
  `at_k = time0_ms + Σ_{j≤k} dt_ms[j]` (the extra `j=k` term is `dt_ms[0]`'s
  `Started`→first-delta gap);
- `ChunkRecord` → `TimedStreamEvent{time_ms, event}` verbatim.

`expand()` **never synthesizes** an event that was not pushed: a partial
`ToolCallRun` (no `ToolCallFinished`) expands to its `Started` + deltas, and the
`ToolCallFinished` is expanded from its own `ChunkRecord` (or is absent). It
throws **`CorruptionError`** (`session/errors.hpp:48-51`) on a semantically
invalid record: a negative `dt_ms`, a negative `time0_ms`, a violated array-size
invariant (§8.3), a `TextRun`/`ReasoningRun` with `texts.empty()` (a zero-length
delta run is never packed, and `dt_ms.size() == N-1` is unsatisfiable at `N=0`),
or a `ToolCallRun` with an empty `id`. A `ToolCallRun` with `args.empty()` **is**
valid: it expands to its single `Started` event (the anchor exists and
`dt_ms.size() == N == 0`). A malformed durable record is corruption on the strict
on-disk axis (`26 §4.6`), so the loud `CorruptionError` is the correct surface,
not a recoverable `expected`.

### 8.6 Accumulator invariants (A)

- **34-A1.** `snapshot()` order equals push order.
- **34-A2.** `expand(snapshot)` reproduces the pushed `StreamEvent` sequence and
  timestamps exactly (round-trip), with no synthesis and no drop.
- **34-A3.** Runs are maximal: no two adjacent records are same-kind runs that
  could be merged.
- **34-A4.** `time0_ms`/`dt_ms` are non-negative and satisfy §8.3.

## 9. The replay harness (34-D9, GAP 3)

### 9.1 Test-only declaration

**The replay harness is test-only.** It lives in
`tests/support/replay_harness.hpp` (+ `.cpp`), is not installed, and is included
by no production translation unit. Rationale:

1. No production consumer exists — no `ymh` verb needs it; `ymh replay`
   re-runs a session rather than reconstructing requests.
2. It needs live registries (`ToolRegistry`) **and** the rendered system prompt,
   neither of which a production replay path would have without re-deriving the
   prompt system (Wave 3).
3. `28 §12 :913-915` calls it "`06`-owned", but `26 §5.2` presents it as test
   strategy; declaring it test-only resolves the ownership ambiguity without
   adding production surface (consistent with `AGENTS.md`'s "do not build a giant
   umbrella dependency").

If a future production replay verb needs it, the API below is promotable
verbatim.

### 9.2 Reconstruction rule

An attempt is identified by its **settlement record** — the durable event the
attempt commits under 34-D6: a `payload::AssistantMessage` for a `Completed`
attempt, a `payload::AssistantAttempt` otherwise (`26 §4.3.9.1 :949`, `:957`).
The caller passes its `Sequence settlement_seq` (`EventRecord.seq`,
`core/event.hpp:113-116`); a test that recorded the session knows it.

1. **Bound the prefix by the attempt's dispatch, not the header.** The request's
   messages at dispatch are exactly the records that precede the attempt, because
   a stream's output is accumulated in memory and the live delta path is
   `Session::emit` — never a durable append (§6.3) — so **no durable
   message-affecting record is appended between a dispatch and its own
   settlement**:

   > `prefix = { r ∈ log : r.seq < settlement_seq }`; `log` is the full session
   > log ascending by `seq` (`EventRange`, `core/event.hpp:113-118`). Per-session
   > sequences are **not** contiguous — `sequence` is a DB-wide AUTOINCREMENT
   > shared by every session in the workspace store (`session_persistence.cpp:54`),
   > and the integrity check only requires strict monotonicity (`:589-598`) — so
   > the prefix is defined by `seq` comparison and `log` order, **never** by
   > `settlement_seq - 1` arithmetic.

   The `LlmRequestHeader` is **not** used to bound the prefix. This is the Rev 2
   correction of the Rev 1 rule (`seq <= header_seq`):
   `AgentLoop::buildRequest` logs a header only when the template changed
   (`agent_loop.cpp:468-496`, with `held_*` updated at `:498-501`), so the
   `ContextLengthExceeded` re-attempt at `:909` logs **no** header; a
   header-bounded prefix reconstructs the request as of the **series start**,
   not the attempt (`28 §4.4 :501-512`, "the attempt's messages").
2. **Find the header (template/digests only).** The last `LlmRequestHeader` with
   `(turn, step)` no later than the attempt's, positionally preceding the
   dispatch (`28 §4.4 :507`). None → `ReplayMismatch{MissingHeader}` (the
   `26-F15` legacy / unreconstructable case; `28 §4.4 :514-516`). The located
   header supplies **only** `config`/`purpose`/`session_id`/
   `system_prompt_digest`/`template_digest`/`tool_names`/`tool_schema_digests`
   and the optional `system_prompt` — never the message prefix.
3. **Messages.** `messages = deriveMessages(session_header, prefix)`
   (`session.cpp:362`). Then prepend the system message **exactly as
   `SessionContextAssembler::assemble` does** (`context_assembler.cpp:41-65`): a
   single `Role::System` message with one `Text` block, present iff the system
   text is non-empty. The system text is `header.system_prompt` when the opt-in
   stored it (`28 §5.4`), else `ReplayEnv.rendered_system_prompt` when the caller
   supplies it, else empty.
4. **Tools.** `tools = registry.schemas()`, in the registry's canonical order
   (`28 §4.2 :477-479`).
5. **Envelope.** `LLMRequest{session_id = header.session_id,
   purpose = header.purpose, messages, tools, parameters = from(header.config)}`,
   then `FrozenRequest::freeze(request, header.config)` (`llm_runtime.hpp:54`).

**Prefix validation (makes the assertion detect a wrong prefix).** The harness
asserts the candidate `prefix` is exactly the dispatch prefix, and throws
`ReplayMismatch{PrefixMismatch}` otherwise:

- `log` contains a record with `seq == settlement_seq`, and that record is an
  `AssistantMessage` or an `AssistantAttempt` (the attempt's own settlement —
  never a header or a mid-stream record);
- `prefix` equals `log[0 .. k)` for some `k` — same first record, same order, no
  skipped or reordered record (this is "contiguous in log order", not `seq + 1`);
- the next record in `log` after `prefix` is the attempt's settlement
  (`log[k].seq == settlement_seq`), so the prefix reaches the dispatch boundary
  and is not truncated at the header. A `seq <= header_seq` prefix leaves the
  intervening records (compaction, the retry's `buildRequest` header, message
  events) after its end, so `log[k].seq != settlement_seq` and the check fails —
  exactly the Rev 1 defect.

The two-rebuild determinism check (§9.3) then runs over the validated prefix; it
is no longer the only guard.

### 9.3 The API

```cpp
// tests/support/replay_harness.hpp — TEST-ONLY (34-D9). Not installed.
namespace ymh::test {

enum class ReplayMismatchKind : std::uint8_t {
    MissingHeader,          // no LlmRequestHeader precedes the attempt (26-F15)
    ToolNameSetMismatch,    // registry names != header.tool_names
    ToolSchemaMismatch,     // re-derived schema digest != header.tool_schema_digests[i]
    PromptDigestMismatch,   // sha256(rendered) != header.system_prompt_digest
    TemplateDigestMismatch, // rebuilt.template_digest() != header.template_digest
    PrefixMismatch,         // candidate prefix != the attempt's dispatch prefix (§9.2)
    MessageDivergence,      // two rebuilds of canonical_json() differ
};

class ReplayMismatch : public std::runtime_error {
public:
    ReplayMismatch(ReplayMismatchKind kind, std::string detail);
    [[nodiscard]] ReplayMismatchKind  kind() const noexcept;
    [[nodiscard]] const std::string&  detail() const noexcept;
private:
    ReplayMismatchKind kind_;
    std::string        detail_;
};

struct ReplayEnv {
    const ToolRegistry*        registry = nullptr;   // live registry (26 §5.2)
    // The live-rendered system prompt captured at dispatch time (record mode).
    // nullopt => disk-only replay; the prompt text is unavailable when the
    // session.persist_prompt_text opt-in was off.
    std::optional<std::string> rendered_system_prompt;
};

enum class ReplayStatus : std::uint8_t {
    Verified,           // full rebuild + digest checks ran
    PromptUnavailable,  // header omitted system_prompt and no rendered prompt was supplied
};

struct ReplayReport {
    ReplayStatus status = ReplayStatus::Verified;
    FrozenRequest rebuilt;                 // the rebuilt envelope
    std::string   canonical_json;          // byte-identical across both rebuilds
};

// (1) Rebuild the template from the header + live registry; verifies tool_names,
//     per-tool schema digests, and — only when the prompt text is known (§9.4) —
//     the prompt digest and the template digest. Throws ReplayMismatch on any
//     mismatch.
[[nodiscard]] FrozenRequest rebuild_template(const payload::LlmRequestHeader& header,
                                             const ReplayEnv& env);

// (2) Reconstruct the attempt's ordered messages per §9.2. `log` is the full
//     session log; `prefix` is the candidate dispatch prefix; `settlement_seq`
//     is the attempt's settlement record. Validates the prefix (§9.2) and throws
//     ReplayMismatch{PrefixMismatch} if it is not the dispatch prefix.
[[nodiscard]] std::vector<Message> reconstruct_messages(const SessionHeader& session_header,
                                                        const EventRange& log,
                                                        const EventRange& prefix,
                                                        Sequence settlement_seq,
                                                        const payload::LlmRequestHeader& header,
                                                        const ReplayEnv& env);

// (3) The full assertion: validate the prefix, rebuild twice, and assert
//     canonical_json() byte-identity. Throws ReplayMismatch{PrefixMismatch} for a
//     wrong prefix or {MessageDivergence} for a non-deterministic rebuild. Returns
//     PromptUnavailable (without throwing) when the prompt text cannot be known
//     (§9.4).
[[nodiscard]] ReplayReport assert_reconstructable(const SessionHeader& session_header,
                                                  const EventRange& log,
                                                  const EventRange& prefix,
                                                  Sequence settlement_seq,
                                                  const payload::LlmRequestHeader& header,
                                                  const ReplayEnv& env);

} // namespace ymh::test
```

### 9.4 Error surface and the honest limit

- `ToolNameSetMismatch` / `ToolSchemaMismatch` implement `28 §4.4 :508-509,
  515-516` ("re-deriving the tool schemas … verifying each against
  `header.tool_schema_digests`"; "fails loud rather than guessing") and the
  negative test `26 §5.2 :1537` ("fails loud as a registry mismatch").
- `PromptDigestMismatch` implements `28 §4.4` and the `26 §5.2 :1538` prompt-
  perturbation negative test.
- `TemplateDigestMismatch` implements `28 §4.4 :510`.
- `PrefixMismatch` implements the §9.2 dispatch bound: the candidate prefix must
  be the attempt's own pre-settlement records, never a header-bounded truncation.
- `MessageDivergence` implements `28 §4.4 :511-512` ("two rebuilds … byte-
  identical").
- **Limit (stated, not overclaimed).** When the header omits `system_prompt`
  (the default: `session.persist_prompt_text` off, `28 §5.4`) **and** the caller
  supplies no `rendered_system_prompt`, the system message text is unknowable
  from the log, so neither `template_digest` nor `canonical_json()` can be
  verified. The harness returns `ReplayStatus::PromptUnavailable` and asserts
  nothing about the prompt-dependent fields. It does **not** fabricate a prompt.
  This mirrors `26-F15`'s "legacy / unreconstructable" handling. Full
  verification requires record mode (the rendered prompt is available) or the
  opt-in (the text is stored).

### 9.5 Negative tests (pinned)

1. Mutate one tool schema → `ToolSchemaMismatch`.
2. Rename/remove a tool → `ToolNameSetMismatch`.
3. Perturb the rendered prompt → `PromptDigestMismatch`.
4. Two rebuilds with a non-deterministic `canonical_json()` → `MessageDivergence`
   (regression guard for `28 §4.2`'s no-timestamps/no-ids rule).
5. **Wrong prefix (MEDIUM-1 regression guard).** For a step whose attempt is a
   `ContextLengthExceeded` retry (or any step after the series' first dispatch),
   pass `prefix = {seq <= header_seq}` — the Rev 1 bound — → `PrefixMismatch`.
   The same test with `prefix = {seq < settlement_seq}` verifies.
6. A session with no header → `MissingHeader`.
7. A workspace-layer `session.persist_prompt_text` → `ConfigError` (21-owned;
   asserted by the config test, cited here as the `26 §5.2 :1539` companion).

## 10. Minor-gap resolutions

### 10.1 `ReplayEnvelope` nested JSON keys (34-D10)

`26 §4.3.9.1`'s key table (`:943-958`) pins every payload's keys but omits the
nested `ReplayEnvelope`. This errata pins them (adding one row to that table by
reference; a `26` re-gate is required to record it):

> `ReplayEnvelope` object: `provider` string, `version` int, `state` **any JSON
> (including null)** — adapter-private, JSON-serializable. NOTE: this corrects an
> earlier "object" wording; spec 33 §3.3 and the shipped codec (`stream.hpp:270`,
> the codec) pin `state` as any JSON including null, and they win.

This matches the struct at `26 §4.3.4 :529-533`; `state` is opaque to ymh. The
sibling `33` codec consumes this key set; it is pinned here because
`ReplayEnvelope` is introduced in `26 §4.3.4`, which this errata owns.

**Second `26` re-gate item: the `at` clock.** `26 §4.3.4 :535` describes
`TimedStreamEvent.at` as "the **wall-clock** offset at which the sink received
it". This errata pins the opposite source (34-D4, §5.1): a **`steady_clock`**
delta from the per-attempt `stream_start`. The verified `26` text therefore
contradicts `34` and must be corrected at the same re-gate; both items are
recorded in §15 item 4.

### 10.2 `ToolCallRun.id`/`name` capture

Pinned in §8.1-§8.2: `id`/`name` come from `ToolCallStarted`; a `ToolCallDelta`
before its `ToolCallStarted` is a `ChunkRecord` (§8.4), so a `ToolCallRun` always
has a non-empty `id`.

### 10.3 `time0_ms`/`dt_ms` reproducibility

Pinned in §5.3: deterministic only under an injected `stream_clock`; not
compared across real process runs.

## 11. Invariants (extending `08 §10`)

- **34-I1.** Exactly one durable event is committed per settled provider
  attempt (`26 §4.3.4 :601-609`).
- **34-I2.** A settled `Completed` attempt commits an `AssistantMessage` whose
  `content == assembler.blocks()` and whose `stream == accumulator.snapshot()`.
- **34-I3.** A settled non-`Completed` attempt commits an `AssistantAttempt`
  and **no** `AssistantMessage`; its `stream == accumulator.snapshot()`.
- **34-I4.** A fresh assembler/accumulator/coalescer and `stream_start` exist per
  provider attempt; no attempt's stream leaks into another's.
- **34-I5.** `TimedStreamEvent.at` is a non-negative `steady_clock` delta from
  the current attempt's stream start (§5.1).
- **34-I6.** `AssistantMessage.replay_state` is `assembler.replay_state()`; the
  event is the single durable home for replay state (`26 §4.3.4 :594-596`).
- **34-I7.** Durable usage is `assembler.usage()`; `LLMResponse::usage` is not a
  durable source (34-D11).
- **34-I8.** `expand(snapshot)` round-trips the pushed stream exactly
  (34-A2).
- **34-I9.** The replay harness is test-only and adds no production surface
  (34-D9).
- **34-I10.** The replay prefix is the attempt's pre-settlement records
  (`prefix == log[0..k)` and `log[k]` is the attempt's settlement), never a
  header-bounded truncation; the harness rejects a wrong prefix with
  `ReplayMismatch{PrefixMismatch}` (§9.2).
- **34-I11.** `AssistantStreamPrinter::feed` is the sole writer to `out`/`err`
  for the three assistant stream/settlement events; `Commit::handled` is true iff
  `event.type` is `AssistantChunk`, `AssistantMessage`, or `AssistantAttempt`,
  and `Commit::text` is non-empty only for a committed `AssistantMessage`
  (§15 item 1).
- **34-I12.** Live `AssistantChunk` deltas are buffered by `chunk.message` and
  are never printed before settlement; an id change clears the id and both
  buffers first; `Text` deltas append to the text buffer, `Reasoning` deltas to
  the reasoning buffer (§15 item 1 R1).
- **34-I13.** On `AssistantMessage` the committed text is the durable settlement
  text whenever that is non-empty and the live buffer only otherwise
  (`durable.empty() ? text_ : durable`); the durable is the recovery path for a
  missed live chunk (`26-D9`, `35 §3.6`; §15 item 1 R2).
- **34-I14.** On `AssistantAttempt` the id and both buffers are discarded and
  nothing is printed; because only a non-`Completed` settlement emits
  `AssistantAttempt` (34-I3, 34-D6), a retried attempt's text cannot concatenate
  into the retry under the per-step `messageId` (§6.1; §15 item 1 R3).
- **34-I15.** Each live buffer is independently capped at `kMaxBufferedBytes`
  (1 MiB); a crossing delta is truncated to the remaining room and later deltas
  are dropped once full. The cap bounds only the live fallback; the durable
  settlement path prints the full text (§15 item 1 R4).
- **34-I16.** Buffered reasoning is written to `err` on commit only when
  `print_reasoning` is set; text is written to `out` on commit; both buffers are
  cleared after every commit or discard (§15 item 1 R5).

## 12. Failure modes (extending `08 §11.2`)

| ID | Failure | Detection | Handling |
|---|---|---|---|
| **34-F1** | A delta arrives for a tool-call `index` with no `ToolCallStarted` | accumulator orphan rule (§8.4) | record verbatim as `ChunkRecord`; `BlockAssembler` ignores it; never fabricate a `ToolCallRun` |
| **34-F2** | A duplicate `ToolCallStarted` for one `index` | first-wins (§4.2) | keep the first `id`/`name`; no throw (the adapter already raises `ProviderInternal`) |
| **34-F3** | A malformed `AssistantStreamRecord` on `expand()` | §8.5 validation | throw `CorruptionError` (loud on-disk axis) |
| **34-F4** | A replay rebuild's tool schema digest differs | `rebuild_template` | throw `ReplayMismatch{ToolSchemaMismatch}` (`26-F6`, `26-F13`) |
| **34-F5** | A replay attempt has no header | `rebuild_template` / `assert_reconstructable` | throw `ReplayMismatch{MissingHeader}`; mark legacy, never fabricate (`26-F15`) |
| **34-F6** | The prompt text is unknowable on disk-only replay | §9.4 | return `PromptUnavailable`; assert nothing prompt-dependent |
| **34-F7** | A settled non-`Completed` attempt would commit a message | 34-D6 predicate | it cannot: only `Completed` reaches the `AssistantMessage` branch |
| **34-F8** | A replay prefix is bounded at the header instead of the attempt's settlement | §9.2 prefix validation | throw `ReplayMismatch{PrefixMismatch}`; never rebuild over a truncated prefix |
| **34-F9** | A live `AssistantChunk` is dropped, or a retried attempt re-streams under the same `messageId` | `AssistantStreamPrinter::feed` (§15 item 1) | the durable `AssistantMessage` prints the full text and wins (`DurableWinsOverLiveBuffer`); the failed attempt's buffer is discarded (`DiscardsBufferOnFailedAttempt`); a live-only stream falls back to the capped buffer (`FallsBackToLiveBufferWhenDurableEmpty`) |

## 13. Test plan

**Unit — `BlockAssembler`.**
1. Golden assembly: a recorded fixture of `StreamEvent`s (reasoning + text +
   tool calls) → `blocks()` equals the reasoning/text/tool-use sequence of
   `agent_loop.cpp:927-936`.
2. Coalescing: N interleaved `TextDelta`s → one text block; N `ReasoningDelta`s
   → one reasoning block.
3. Tool calls: `Started`+`Delta`+`Finished` → one tool_use block carrying the
   `Finished.call` object; ascending index across multiple calls.
4. `interrupted_blocks()`: drops every tool_use block, keeps reasoning+text, is
   the prefix of `blocks()` (34-B5).
5. Usage authority: `Finished.usage` beats `UsageEvent`; `UsageEvent` alone is
   the fallback; neither → `nullopt`.
6. `finish()` defaults to `Stop` with no `Finished`.
7. `replay_state()` returns the `Finished.replay_state`.

**Unit — `AssistantStreamAccumulator` / `expand()`.**
8. Round-trip: push a mixed stream (with an injected manual `stream_clock`),
   `expand(snapshot())` reproduces it exactly (34-A2).
9. Packing: adjacent same-kind deltas pack; a non-delta flushes the run;
   `ToolCallRun` anchors at `Started` (§8.3).
10. Orphan `ToolCallDelta` → `ChunkRecord`; round-trips.
11. `expand()` throws `CorruptionError` on a negative `dt_ms`, a bad array size,
    and an empty `ToolCallRun.id`.

**Integration — loop settlement.**
12. Success → exactly one `AssistantMessage`, zero `AssistantAttempt`;
    `content == blocks()`, `stream` non-empty, `replay_state` propagated.
13. Failure → one `AssistantAttempt`, zero `AssistantMessage`.
14. Cancel → one `AssistantAttempt`, zero `AssistantMessage`.
15. Compaction retry → one `AssistantAttempt` (attempt 0) + one
    `AssistantMessage` (attempt 1); attempt 0's text is absent from the message.
16. No durable `AssistantChunk` is written by a new binary (29 §4.2 `:353`); the
    live delta path still publishes (bus subscriber sees chunks).
17. Headless output: `ymh run` text derived from `AssistantMessage` with no
    `AssistantChunk` (26 §5.2 Wave 2 `:1445`).

**Replay harness.**
18. `assert_reconstructable` passes for a recorded FakeLLM session (record mode),
    including the `ContextLengthExceeded` retry's attempt 1 (settlement-bounded
    prefix).
19. The seven negative cases of §9.5.
20. Prefix regression: the retry attempt of §9.5 item 5 verifies with
    `prefix = {seq < settlement_seq}` and throws `PrefixMismatch` with the
    header-bounded `{seq <= header_seq}`.

**Unit — `AssistantStreamPrinter` (34-D13).**
21. `DurableWinsOverLiveBuffer` — a live delta prints nothing; the durable
    settlement prints its own text and wins over the buffer.
22. `FallsBackToLiveBufferWhenDurableEmpty` — an empty durable settlement
    commits the live buffer.
23. `DiscardsBufferOnFailedAttempt` — an `AssistantAttempt` discards the failed
    text; only the retry's text is printed.
24. `PrintsBufferedReasoningWhenEnabled` /
    `SuppressesBufferedReasoningWhenDisabled` — reasoning reaches `err` iff
    `print_reasoning`.
25. `LiveBufferStaysBoundedWithoutSettlement` — with no settlement the buffer
    stops at `kMaxBufferedBytes`.
26. `FullDurableTextAfterBufferCapExceeded` — the durable settlement prints in
    full past the cap.

## 14. Cross-spec reconciliation

- **`26 §4.3.4`** — signatures unchanged; algorithms pinned here. The
  `ReplayEnvelope` key table addition (§10.1) and the `TimedStreamEvent.at`
  clock correction (§5.1, §10.1) both require a `26` re-gate.
- **`28 §4.4`/§12** — the reconstruction guarantee (`:499-516`) and the
  `06`-owned harness (`:913-915`) now have a callable test-only API (§9). No
  competing source is introduced. (The assembler's C++ shapes are `26 §4.3.4`,
  not `28`; the `28 §11` dsh-mapping row `:875` defers to it.)
- **`29 §3.2`/§4.2** — `AssistantAttempt` / `AssistantChunk` live-only are
  consumed, not redefined. The `AssistantAttempt` `deriveMessages` case remains
  `29`'s obligation.
- **`30 §5.3`** — this errata satisfies Stage-B Wave 2's `08` prerequisite
  (`:320`). Because `30` is currently "written — pending re-gate"
  (`DESIGN_STATUS.md:42`), its Stage-B row 2 must be **re-gated** now that this
  file exists; the lead owns the tracker edit.
- **`31 §5.2`/§5.3** — the dispatch path is unchanged; this errata adds the
  per-attempt sink wiring (§6) and the `AgentServices::stream_clock` seam (§5.2),
  which amend `31`/`06` by reference.
- **`08 §10`/§11.2** — invariants `34-I*` and failure modes `34-F*` extend them.
- **Sibling `33`** — the `StreamEvent` JSON codec (GAP 1) is owned by `33`; this
  errata references it (`<existing StreamEvent codec>`) and does not define it.

## 15. Open items and the pinned `AssistantStreamPrinter` contract (34-D13)

1. **`AssistantStreamPrinter` contract (pinned in Rev 3; formerly open).**
   `headless.cpp:192-200` (in-process) and `cli.cpp:61-77, 585`
   (daemon-connected) feed every session event through one shared
   `AssistantStreamPrinter`, which owns the per-message live-delta buffer so live
   deltas and the durable `assistant/message` cannot double-print in `ymh run`
   (`26 §5.2 :1434-1442`). The contract is pinned as decision **34-D13** and
   invariants **34-I11–34-I16**; `tests/unit/assistant_stream_printer_test.cpp`
   enforces it (test plan §13 items 21-26).

   *Interface* (`include/ymh/cli/assistant_stream_printer.hpp`):

   ```cpp
   struct Commit {
       bool        handled = false;  // true iff event.type is one of
                                     //   AssistantChunk, AssistantMessage,
                                     //   AssistantAttempt
       std::string text;             // assistant text committed to `out`;
                                     //   non-empty only on AssistantMessage
   };
   Commit feed(const Event& event, std::ostream& out, std::ostream& err,
               bool print_reasoning);
   static constexpr std::size_t kMaxBufferedBytes = 1u << 20;  // 1 MiB / buffer
   ```

   Private state: `message_` (current live message id), `text_`, `reasoning_`.

   *Rules.*
   - **R1. Per-message-id buffering (34-I12).** A live `AssistantChunk` is
     buffered and never printed. If `chunk.message != message_`, the id and both
     buffers reset first. `kind == Text` appends to `text_`; `kind == Reasoning`
     appends to `reasoning_`. Buffering-before-settlement is enforced by
     `DurableWinsOverLiveBuffer`; the id-change reset is
     `assistant_stream_printer.cpp:42-46` and has no dedicated test among the
     seven.
   - **R2. Durable wins (34-I13).** On `AssistantMessage`, `durable` is the
     in-order concatenation of the settlement's `Text`-kind `content` blocks, and
     `commit.text = durable.empty() ? text_ : durable`. `durable` wins whenever
     non-empty; the live buffer is only the fallback for a settlement with no
     durable text (resume/replay). Rationale (`26-D9` / `35 §3.6`): the durable
     settlement is the recovery path for a missed live chunk, so the live channel
     is never the source of truth. Enforced by `DurableWinsOverLiveBuffer` and
     `FallsBackToLiveBufferWhenDurableEmpty`.
   - **R3. Discard on `AssistantAttempt` (34-I14).** An `AssistantAttempt`
     clears the id and both buffers and prints nothing. The event type is the
     discriminator: only a non-`Completed` settlement emits `AssistantAttempt`
     (34-I3, 34-D6), so no attempt status is inspected. Why: the per-step
     `messageId` (§6.1) is unchanged across a retry, so without the discard the
     failed attempt's streamed text would concatenate with the retry's under the
     same id. Enforced by `DiscardsBufferOnFailedAttempt`.
   - **R4. Bounded buffer (34-I15, F8).** Each buffer independently holds at
     most `kMaxBufferedBytes`. A delta appends only up to the remaining room (a
     crossing delta is truncated); once a buffer is full, further deltas for it
     are dropped. The cap bounds only the live-only fallback: the durable path
     prints the full settlement text regardless of the cap. Enforced by
     `LiveBufferStaysBoundedWithoutSettlement` and
     `FullDurableTextAfterBufferCapExceeded`.
   - **R5. Reasoning output (34-I16).** On commit, text is written to `out` (and
     flushed) always, and buffered reasoning is written to `err` (and flushed)
     only when `print_reasoning` is set. Both buffers clear after every commit or
     discard. Enforced by `PrintsBufferedReasoningWhenEnabled` and
     `SuppressesBufferedReasoningWhenDisabled`.

   *Remaining consumer item (TUI, not this class).* `ui_event_adapter.cpp:92-105`
   is the separate TUI path: it opens `AssistantMessageStarted` only when the
   incoming `AssistantChunk.message` changes, so a retried attempt's live deltas
   append to the failed attempt's live text under the same id; the TUI's
   replace-on-finish (`ui_model.cpp`) hides it. That is `26` Wave 2's consumer
   migration, not the run-path printer contract; it stays named here, not
   dropped.
2. **The `06`/`31` amendment for `AgentServices::stream_clock`** (§5.2) is pinned
   here by reference; `31`'s and `06`'s own errata must record the new member.
3. **`finish()` and `interrupted_blocks()` have no Wave-2 consumer** (§4.4, §4.6).
   They are pinned contracts with reserved consumers; this is recorded, not
   silently dropped.
4. **Two `26` re-gate items.** The `ReplayEnvelope` key-table row (§10.1) and
   the `TimedStreamEvent.at` clock correction (`26 §4.3.4 :535` says
   "wall-clock"; 34-D4 pins `steady_clock`, §5.1) both require a `26` re-gate.
5. **No `33` dependency is resolved here** — the `StreamEvent` codec remains the
   sibling's GAP 1.

## 16. Revision log

- **Rev 3 (2026-09-19).** Closes the Rev 2 §15 item 1 open risk by pinning the
  shipped `AssistantStreamPrinter` contract: decision 34-D13, invariants
  34-I11–34-I16, failure mode 34-F9, and test plan §13 items 21-26. The pin
  records the per-message-id live buffer, the durable-wins precedence
  (`26-D9`/`35 §3.6`), the discard-on-`AssistantAttempt` rule, the
  `kMaxBufferedBytes` cap, and the reasoning-output rule, and cites
  `tests/unit/assistant_stream_printer_test.cpp`. The TUI
  `ui_event_adapter.cpp` cross-attempt case remains a `26` Wave 2 consumer item
  (§15 item 1). No production surface changes: the code was already implemented
  and green, and this revision back-fills its owning spec.
- **Rev 2 (2026-09-19).** Repairs the Rev 1 independent gate
  (`/tmp/opencode/regate34.md`, GATE FAIL: 0 HIGH / 1 MEDIUM / 6 LOW).
  **MEDIUM-1:** §9.2 now bounds the replay message prefix by the **attempt's
  dispatch position** (`prefix = {seq < settlement_seq}`, anchored on the
  attempt's own settlement record) instead of `seq <= header_seq`; the header is
  used only for the template/digests, and §9.2/§9.3 pin a `PrefixMismatch`
  validation that fails on a header-bounded prefix (regression test §9.5 item 5,
  test plan §13 items 18/20, invariant 34-I10, failure mode 34-F8, decision
  34-D12). **LOWs:** §4.1/§4.2 drop the phantom `CallState` raw-fragment mirror;
  §8.5 states the per-delta `at_k` formulas and the zero-length-run rule;
  §10.1/§14/§15 record the `TimedStreamEvent.at` clock correction as a second
  `26` re-gate item; §9.3 scopes the template-digest check to the prompt-known
  case; §15 item 1 names the cross-attempt live-delta `messageId` case; §6.3
  re-pins the per-attempt `flush()` `LeaseLost`/`StoreError` handling. No
  production surface changes.
- **Rev 1 (2026-09-19).** Initial write. Closes GAP 2 and GAP 3 of
  `/tmp/opencode/wave2-gaps.md`. Pins 34-D1–34-D11, invariants 34-I1–34-I9,
  failure modes 34-F1–34-F7, and the test plan §13. Claims spec number 34.

## 17. Decisions (34-D1–34-D13)

| ID | Decision | Rationale |
|---|---|---|
| **34-D1** | `ReplayEnvelope`/`TimedStreamEvent` in `stream.hpp`; assembler in `assistant_stream.hpp` | `std::optional<ReplayEnvelope>` in `Finished` requires completeness before `stream.hpp`; avoids an include cycle |
| **34-D2** | Blocks are reasoning → text → tool-use; one block per kind; tool-use ascending by index | Reproduces shipped `agent_loop.cpp:927-936` exactly; no model-visible change |
| **34-D3** | `interrupted_blocks()` = reasoning+text only; all tool-use dropped | `26 §4.3.4 :546-548`; an interrupted attempt dispatched no tool |
| **34-D4** | `at` = `steady_clock` delta from the per-attempt stream start; injectable reader | Monotonic within a stream; existing `ClockReader` idiom; deterministic tests |
| **34-D5** | Assembler/accumulator/coalescer are per attempt; the sink feeds all three | Per-attempt settlement requires per-attempt streams; prevents cross-attempt text concatenation |
| **34-D6** | Per-attempt settlement; `Completed` → message, else attempt; retried attempt settled before compaction | `26 §4.3.4 :601-609` ("one durable event per settled attempt"; "retried") |
| **34-D7** | Maximal same-kind delta runs; `ToolCallRun` anchored at `Started`; orphan delta → `ChunkRecord` | `26 §4.3.4 :554-558`; keeps the accumulator total and lossless |
| **34-D8** | `expand()` throws `CorruptionError` on an invalid record | Malformed durable record = strict on-disk corruption (`26 §4.6`) |
| **34-D9** | Replay harness is test-only in `tests/support/replay_harness.hpp` | No production consumer; resolves the `06`-vs-test ownership ambiguity (`26 §5.2` vs `28 §12`) |
| **34-D10** | `ReplayEnvelope` keys = `provider`/`version`/`state` | Matches the `26 §4.3.4 :529-533` struct; fills the `26 §4.3.9.1` omission |
| **34-D11** | Durable usage = `BlockAssembler::usage()` | `26 §4.3.4 :549`; single authority |
| **34-D12** | The replay prefix is bounded by the attempt's settlement (`prefix = {seq < settlement_seq}`), not the header; the harness validates it and throws `PrefixMismatch` | The header is a changed snapshot logged only on template change (`agent_loop.cpp:468-496`), so it cannot bound the attempt's messages (`28 §4.4 :501-512`) |
| **34-D13** | The run-path de-duplication is `AssistantStreamPrinter`: per-message-id live buffer, durable-wins on `AssistantMessage`, discard on `AssistantAttempt`, `kMaxBufferedBytes` cap | `26-D9` / `35 §3.6` make the durable settlement the recovery path for a missed live chunk; one shared printer keeps both `ymh run` paths identical; closes the Rev 2 §15 item 1 open risk |

## 18. References

- `docs/design/26-dsh-alignment-part2.md` (Rev 7, GATE PASS): §4.3.4 `:518-609`;
  §4.3.9 `:820-1013`; §4.3.9.1 `:935-958`; §5.2 `:1521-1540`; Wave 2 `:1424-1449`.
- `docs/design/28-llm-service-boundary-errata.md` (Rev 3, GATE PASS): §4.1-§4.4
  `:441-516`; §5.2-§5.4 `:547-634`; §11 `:875`; §12 `:880-931`; §13 `:935-980`.
- `docs/design/29-event-family-errata.md` (GATE PASS): §3.1-§3.2 `:115-187`;
  §4.2 `:298-354`.
- `docs/design/30-architecture-cascade-errata.md` (Rev 2, pending re-gate): §5.3
  `:315-328`.
- `docs/design/31-agent-loop-errata.md` (Rev 2, GATE PASS): §3.2 `:216-262`;
  §5.2-§5.3 `:386-453`.
- `docs/design/08-llm-provider.md` (verified): §2-§5, §10-§11.
- `/tmp/opencode/wave2-gaps.md`: GAP 2 `:114-204`; GAP 3 `:208-260`; minor gaps
  `:264-277`.
- Shipped code: `include/ymh/llm/stream.hpp`; `include/ymh/llm/llm_provider.hpp`;
  `include/ymh/llm/llm_runtime.hpp`; `include/ymh/agent/message.hpp`;
  `include/ymh/session/events.hpp`; `include/ymh/session/session.hpp`;
  `include/ymh/session/errors.hpp`; `src/agent/agent_loop.cpp`;
  `src/agent/context_assembler.cpp`; `src/agent/chunk_coalescer.cpp`;
  `src/session/session.cpp`; `src/session/events.cpp`; `src/llm/fake_llm.cpp`;
  `src/cli/headless.cpp`; `src/ui/ui_event_adapter.cpp`.

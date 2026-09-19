# 33 — Stream-Event Codec Errata: The `StreamEvent` JSON Codec (spec-08/29 amendment)

```
Status: written · verified: — · reviewer: — (tracked in DESIGN_STATUS.md)
Revision: Rev 1 — initial authoring. Pins the missing `StreamEvent` JSON codec
          (the `<existing StreamEvent codec>` dangling reference at
          `26-dsh-alignment-part2.md:964` / `29-event-family-errata.md:186`):
          the `type` discriminator for each of the eight shipped alternatives,
          the exact key set and omission rules for each, and the nested
          `ToolCallFinished.call` (`ToolCallAssembled`), `StreamError.error`
          (`LLMError`), `UsageEvent.usage`/`Finished.usage` (`Usage`), and
          `Finished.replay_state`/`AssistantMessage.replay_state`
          (`ReplayEnvelope`) shapes. Closes GAP 1 of the Wave-2 design-first
          audit.
Component: 33 (errata) — amends `29-event-family-errata.md` §3.1/§3.2 and
           `26-dsh-alignment-part2.md` §4.3.9.1 **by reference**; it does not
           edit either file in place. It does not amend `01-session.md`, `08`'s
           in-memory stream algebra beyond the `Finished.replay_state` field that
           `26-part2` §4.3.4 already pins, or the assembler/replay contract
           (that is the missing `08` assembler errata, Wave-2 GAP 2).
Depends on: `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS) §4.3.4
            :518-609 (the assembler/stream types; `ReplayEnvelope` :529-533;
            `TimedStreamEvent` :536-539; the runs + `ChunkRecord` :554-563;
            `AssistantStreamRecord` :567; the stream-algebra change :586-593;
            the settlement model :601-609), §4.3.9.1 :933-966 (the pinned key
            table and the dangling phrase at :964), §5 Wave 2 :1424-1449;
            `29-event-family-errata.md` (verified) §3.1 :115-146 (the six-point
            codec checklist and the codec conventions), §3.2 :148-187 (the
            `AssistantAttempt`/`AssistantMessage` keys and the repeated dangling
            phrase at :186), §5 (29-I1) :400-405, §7 :442-468 (the test plan);
            `28-llm-service-boundary-errata.md` (verified Rev 3) §7 :685-744
            (the `LLMErrorCode` wire/diagnostic strings and the exhaustive-switch
            audit);
            `08-llm-provider.md` (verified) §2.1/§2.2/§3.2 (the stream algebra);
            the working tree at authoring time: `include/ymh/llm/stream.hpp`,
            `include/ymh/agent/message.hpp`, `include/ymh/session/ids.hpp`,
            `include/ymh/llm/llm_call_config.hpp`, `src/session/events.cpp`,
            `src/core/event.cpp`, `tests/unit/event_test.cpp`,
            `tests/unit/llm_types_test.cpp` (all `file:line` re-derived).
Scope: pin, for the durable half of Wave 2, the one codec the specs reference
       but never define. Design only — no code, no behavior change, no version
       bump. Every alternative, key, discriminator, omission rule, enum string,
       and round-trip obligation below is derived from the shipped C++ types and
       the existing codec conventions; nothing is invented.
Supersedes: nothing. Claims number **33** (see the numbering note below).
Amends: `29-event-family-errata.md` §3.2 :176-187 — the dangling
        `<existing StreamEvent codec>` is replaced **by reference** with §3
        below; the four `AssistantStreamRecord` `type` strings are restated
        (§4). `26-dsh-alignment-part2.md` §4.3.9.1 :949-964 — the dangling
        phrase at :964 and the `payload::LlmRetry` `failure` "(existing spec-08
        codec)" at :947 are replaced **by reference** with §3.3/§3.4 below.
```

> **Numbering note (authoritative).** This document **claims number 33**. The
> Wave-0 allocation already recorded by `28` §0 / `29` §1.4 is **27 = session
> locking**, **28 = LLM service boundary**, **29 = event family**, **30 =
> architecture cascade**; the Wave-1 prerequisites then claimed **31 =
> agent-loop** (`31-agent-loop-errata.md`) and **32 = compaction**
> (`32-compaction-errata.md`). The Wave-3+ specs that `26-dsh-alignment-part2.md`
> §5 reserved as `27-system-prompt.md`, `28-output-retention.md`,
> `29-agent-presets.md`, `30-goals-jobs-commands.md` are therefore **renumbered
> to 35+** when their errata are written (31, 32, 33, and 34 are taken). This
> errata does not reserve a number for them.

> **Status of the tree (truthful).** Wave 1 has shipped: `EventType::
> LlmRequestHeader` and its payload codec exist (`src/core/event.cpp:40`,
> `include/ymh/core/event.hpp:73`, `src/session/events.cpp:493-535`), so
> `all_event_types()` now returns **23** values (`kWireNames` is
> `std::array<WireEntry, 23>`, `src/core/event.cpp:18-42`), not the 22 that
> `29` §3.2 :160 recorded at authoring time. Wave 2 adds exactly **one** enum
> value (`AssistantAttempt`), reaching the **24** that `29` §3.2 :160 already
> pins. Nothing in this errata exists in the tree: there is **no**
> `StreamEvent`/`LLMError` codec, and **no** `AssistantAttempt`,
> `AssistantStreamRecord`, `ReplayEnvelope`, `TimedStreamEvent`,
> `BlockAssembler`, or `AssistantStreamAccumulator` (verified by grep).

---

## 1. Purpose, numbering, scope

### 1.1 The gap this closes

Wave 2 embeds a compact assistant stream in two durable payloads:
`payload::AssistantAttempt` (`29` §3.2 :176-177) and the `stream` field of
`payload::AssistantMessage` (`29` §3.2 :179-187). The record union
`AssistantStreamRecord` is pinned at `26-part2` :567, and its `ChunkRecord`
alternative carries a **serialized `StreamEvent`**:

> `{"type":"chunk","time_ms":int,"event":<existing StreamEvent codec>}`
> — `26-part2` :964, repeated at `29` :186.

No such codec exists, and no verified spec pins it. The tree defines only the
in-memory algebra:

```cpp
using StreamEvent = std::variant<TextDelta, ReasoningDelta, ToolCallStarted,
                                 ToolCallDelta, ToolCallFinished, UsageEvent,
                                 Finished, StreamError>;   // stream.hpp:173-180
```

`grep` over `include/ymh/llm/` and `src/llm/` finds **no** `to_json`/`from_json`
for `StreamEvent`, for any of its eight alternatives, or for `LLMError`; the
only LLM-side codecs are `LlmCallConfig` (`llm_call_config.hpp:83-133`) and the
`agent/message.hpp` codecs (`ContentBlock`, `Message`, `Usage`). An implementer
must otherwise guess the discriminator strings (`"text_delta"` vs `"text"`), the
emitted-vs-omitted keys, and the nested `call`/`error` shapes. This errata pins
all of it from the shipped types.

### 1.2 What this pins

One codec, `ymh::StreamEvent` (and the nested `ToolCallAssembled`, `LLMError`,
`Usage`, `ReplayEnvelope` objects it contains): the `type` discriminator string
per alternative; the exact JSON key set; the omission rules; the enum string
maps; the decode-rejection behavior; the round-trip obligation; and the test
plan additions. It replaces the dangling phrase in both specs by reference.

### 1.3 In scope / out of scope

**In scope (frozen here).** The JSON codec for the eight `StreamEvent`
alternatives and the four nested shapes, plus the four `AssistantStreamRecord`
record-level `type` strings restated for reconciliation.

**Out of scope (owned elsewhere).** The block-assembly algorithm, the
`TimedStreamEvent.at` epoch/source, the sink wiring, the
`interrupted_blocks()` contract, and the `AssistantMessage` vs
`AssistantAttempt` settlement predicate — all Wave-2 GAP 2, owned by the missing
`08` assembler/replay errata that `30-architecture-cascade-errata.md` §5.3 :320
requires. Also out of scope: `TimedStreamEvent`'s own JSON shape (it is not a
durable payload; only the record list is), and any renumbering performed in
place.

---

## 2. Verified tree facts (the basis for §3)

All counts and line anchors below were re-derived from the working tree.

### 2.1 The eight shipped alternatives and their payloads

`include/ymh/llm/stream.hpp`:

| # | Alternative | Members (`file:line`) |
|---|---|---|
| 1 | `TextDelta` | `std::string text` (:129-131) |
| 2 | `ReasoningDelta` | `std::string text` (:133-135) |
| 3 | `ToolCallStarted` | `std::uint32_t index`; `ToolCallId id`; `std::string name` (:144-148) |
| 4 | `ToolCallDelta` | `std::uint32_t index`; `std::string arguments_fragment` (:150-153) |
| 5 | `ToolCallFinished` | `std::uint32_t index`; `ToolCallAssembled call` (:155-158) |
| 6 | `UsageEvent` | `Usage usage` (:160-162) |
| 7 | `Finished` | `FinishReason reason`; `std::optional<Usage> usage` (:164-167) |
| 8 | `StreamError` | `LLMError error` (:169-171) |

The variant declaration is at :173-180. `ToolCallId` is a `std::string` alias
(`include/ymh/session/ids.hpp:18`), so it serializes as a JSON string with no
wrapper. The nested types:

- `ToolCallAssembled { ToolCallId id; std::string name; nlohmann::json arguments
  = nlohmann::json::object(); }` (:137-142). The comment at :137 pins
  "`arguments` is always a JSON object".
- `LLMError { LLMErrorCode code = None; int http_status = 0; std::string
  provider_message; std::string detail; bool retryable = false; }` (:63-69).
  `provider_message`/`detail` are always redacted before they are surfaced or
  logged (L12, :61-62), so persisting them is safe.
- `Usage { std::int64_t input_tokens; output_tokens; cached_tokens;
  reasoning_tokens; }` (`include/ymh/agent/message.hpp:131-138`), already
  carrying an inline codec (:190-204).

### 2.2 The enum string maps (already shipped; do not reinvent)

- `to_string(LLMErrorCode)` (`stream.hpp:71-92`) returns exactly: `none`,
  `auth`, `config_error`, `bad_request`, `context_length_exceeded`,
  `rate_limited`, `server_error`, `network_error`, `timeout`,
  `malformed_response`, `malformed_tool_call`, `content_filtered`,
  `unsupported_model`, `provider_internal`, `cancelled`,
  `invalid_prepared_call`, `no_provider_route` (17 values; the last two are the
  28 Rev 3 additions, pinned at `28` §7 :734-737).
- `to_string(FinishReason)` (`stream.hpp:94-104`) returns exactly: `stop`,
  `length`, `tool_calls`, `content_filter`, `error`, `other` (6 values).

These functions are the **only** spelling authority for the two enums. The
codec must reuse them rather than open-code a second map.

### 2.3 The codec conventions this errata inherits (cite)

From `src/session/events.cpp` and `src/core/event.cpp`, verified:

1. **`snake_case` keys.** `server_profile`, `token_estimate`, `created_at`,
   `tool_schema_digests`, `starts_series`, `session_id` (`events.cpp:213-216`,
   `:420`, `:507-509`, `:78-79`).
2. **Enums as lowercase strings** via a `*_name` function and a total parser
   that rejects unknown values (`events.cpp:20-22`, `:125-207`, `:230`, `:242`,
   `:316`; `core/event.cpp:55-62`).
3. **`optional` fields omitted when unset**, read with a default or a
   `contains`-guard (`events.cpp:321-323`, `:436-438`, `:498-500`, `:503-505`,
   `:517-524`; `llm_call_config.hpp:87-105`).
4. **Non-optional strings are emitted even when empty** (`AssistantChunk.text`
   at `events.cpp:307`; `ToolCall.name` at `:351`; `ContentBlock.text` at
   `message.hpp:147`).
5. **Arrays are emitted even when empty** (`tool_names`, `stop`,
   `tool_schema_digests`; `events.cpp:506-507`, `llm_call_config.hpp:96`).
6. **`nlohmann::json` object fields are emitted as objects**, defaulting to
   `json::object()` (`ToolCall.arguments` at `events.cpp:342`, `:352`;
   `ContentBlock.arguments` at `message.hpp:150`, `:166`).
7. **Inline ADL codecs in the owning header** are the precedent for
   dependency-light nested types: `LlmCallConfig` (`llm_call_config.hpp:83-133`)
   is inline "so the session codec can use it without a link dependency on
   `ymh::llm`" (:80-82).

---

## 3. The pinned `StreamEvent` codec (frozen)

### 3.1 The discriminator rule (33-D1)

A serialized `StreamEvent` is a JSON **object** whose `type` key is a required
string and whose remaining keys are the alternative's payload. The `type` value
is the **exact `snake_case` of the C++ alternative type name** — a mechanical,
collision-free derivation:

| # | Alternative | `type` |
|---|---|---|
| 1 | `TextDelta` | `"text_delta"` |
| 2 | `ReasoningDelta` | `"reasoning_delta"` |
| 3 | `ToolCallStarted` | `"tool_call_started"` |
| 4 | `ToolCallDelta` | `"tool_call_delta"` |
| 5 | `ToolCallFinished` | `"tool_call_finished"` |
| 6 | `UsageEvent` | `"usage_event"` |
| 7 | `Finished` | `"finished"` |
| 8 | `StreamError` | `"stream_error"` |

**Why the type name, not a shortened label.** The tempting alternatives
collide: `"text"`/`"reasoning"` are already the `ContentBlockKind` and
`AssistantChunkKind` strings (`message.hpp:77-106`, `events.cpp:189-197`), and
`"usage"` is already the `EventType::TokenUsage` wire name (`core/event.cpp:35`);
`"error"` is the `FinishReason::Error` string (`stream.hpp:100`). The
type-name derivation also makes the discriminator testable as a pure function of
the alternative index and leaves room for a future alternative without a naming
debate. `"usage_event"` (not `"usage"`) is deliberate for the same reason.
Decode rejects any unknown `type` loudly (33-I5).

### 3.2 The per-alternative key schemas (33-D2)

In addition to `type`, which is **always present and always first-class**:

| `type` | Keys | Required / omitted |
|---|---|---|
| `text_delta` | `text` string | always present (empty string allowed) |
| `reasoning_delta` | `text` string | always present |
| `tool_call_started` | `index` int, `id` string, `name` string | all always present |
| `tool_call_delta` | `index` int, `arguments_fragment` string | both always present |
| `tool_call_finished` | `index` int, `call` object | both always present |
| `usage_event` | `usage` object | always present |
| `finished` | `reason` string; `usage` object; `replay_state` object | `reason` always; `usage` and `replay_state` omitted when `nullopt` |
| `stream_error` | `error` object | always present |

`index` is the `std::uint32_t` value as a JSON integer; `time0_ms`/`dt_ms`/
`time_ms` (record level, §4) are JSON integers. `Finished.reason` is the
`to_string(FinishReason)` string (§2.2). `Finished.replay_state` is the
`26-part2` §4.3.4 :586-589 addition (`std::optional<ReplayEnvelope>`); the
codec supports it, but whether a recorded `ChunkRecord` retains it is an
assembler-side decision owned by the GAP-2 errata, not by this codec.

### 3.3 The nested shapes (33-D3)

**`ToolCallFinished.call` — `ToolCallAssembled`** (`stream.hpp:137-142`):

```json
{"id": <string>, "name": <string>, "arguments": <object>}
```

`id`/`name` are strings emitted even when empty (convention §2.3.4); `arguments`
is **always** a JSON object (`stream.hpp:137`), emitted even when empty, and
decoded with `json.value("arguments", nlohmann::json::object())` (the
`ToolCall`/`ContentBlock` precedent, `events.cpp:352`, `message.hpp:166`).

**`StreamError.error` — `LLMError`** (`stream.hpp:63-69`):

```json
{"code": <string>, "http_status": <int>, "provider_message": <string>,
 "detail": <string>, "retryable": <bool>}
```

All five keys are always present (none is `std::optional`). `code` is the
`to_string(LLMErrorCode)` string (§2.2; includes `"none"`); `http_status` is
`0` when not HTTP; `provider_message`/`detail` are the already-redacted strings
(empty string when absent); `retryable` is a bool. This object is also the shape
referenced by `payload::LlmRetry.failure` at `26-part2` :947 ("existing spec-08
codec"), which was likewise dangling; pinning it here closes both references.

**`usage_event.usage` / `finished.usage` — `Usage`** (`message.hpp:190-204`,
already shipped): `{"input_tokens": int, "output_tokens": int,
"cached_tokens": int, "reasoning_tokens": int}`, all four always present. The
codec reuses the existing `Usage` `to_json`/`from_json`; it does not redefine
it.

**`replay_state` — `ReplayEnvelope`** (`26-part2` :529-533):

```json
{"provider": <string>, "version": <int>, "state": <any JSON>}
```

All three keys always present. `provider` identifies the producer (empty string
allowed); `version` is `std::uint32_t` defaulting to `1`; `state` is the
adapter-private, versioned, JSON-serializable payload and is emitted even when
`null` (the default-constructed `nlohmann::json`). This closes the Wave-2 audit's
"`ReplayEnvelope` nested JSON keys" minor gap; `26` §4.3.9.1 :957 only said
"`replay_state` object (omitted when none)". The **object itself** is omitted
when the `optional` is unset; when present, all three keys are emitted.

### 3.4 The enum parsers (33-D4)

Decode needs the inverse of `to_string(FinishReason)` and
`to_string(LLMErrorCode)`. This errata pins two total parsers, inline in
`stream.hpp`, mirroring the `parse_role`/`parse_chunk_kind` convention
(`message.hpp:53-67`, `events.cpp:103-111`):

```cpp
[[nodiscard]] std::optional<FinishReason> parse_finish_reason(std::string_view) noexcept;
[[nodiscard]] std::optional<LLMErrorCode> parse_llm_error_code(std::string_view) noexcept;
```

Each accepts exactly the strings `to_string` emits and returns `std::nullopt`
otherwise. The codec throws on an unknown string (33-I5); it never silently
defaults a `code` or a `reason`. The two parsers are public so the round-trip
test can assert the inverse property directly.

### 3.5 Location and ownership (33-D5)

The `StreamEvent` and `LLMError` codecs are **inline ADL `to_json`/`from_json`
in `include/ymh/llm/stream.hpp`, namespace `ymh`** — the `LlmCallConfig`
precedent (`llm_call_config.hpp:80-82`) — so `src/session/events.cpp` can encode
`AssistantStreamRecord`/`ChunkRecord` without a link dependency on `ymh::llm`
and without a second definition. `Usage`'s codec stays where it is
(`message.hpp`). The `AssistantStreamRecord`/`ChunkRecord` codec lives with
those types in the Wave-2 header that declares them (the GAP-2 errata owns that
header's name); its `chunk` alternative embeds the `event` object produced by
the codec pinned here. There must be **exactly one** `StreamEvent` codec in the
tree (33-F6).

### 3.6 Omission rules (33-D2, continued)

1. `type` is always present.
2. Non-optional scalars (string/int/bool) are always present, including empty
   strings, `0`, and `false` (convention §2.3.4).
3. `std::optional` fields are **omitted** when `nullopt` and are read with a
   `contains`-guard or a default (convention §2.3.3). In this codec those are
   exactly `Finished.usage` and `Finished.replay_state`.
4. Arrays and non-optional nested objects are always present, even when empty
   (conventions §2.3.5-6).
5. Decode **ignores unknown keys** on a known `type` (the Axis-A graceful
   degradation that makes `29-D3` additive-safe, `29` §3.3 :206-210); decode
   **throws** on an unknown `type` or an unknown enum string (33-I5).

---

## 4. Reconciliation with `29` §3.2 and `26-part2` §4.3.9.1 (33-D7)

This section is the by-reference replacement for the two dangling phrases. The
record-level schemas are **unchanged**; only the `event` object is now defined.

**Record union.** `AssistantStreamRecord = std::variant<TextRun, ReasoningRun,
ToolCallRun, ChunkRecord>` (`26-part2` :567). Its four `type` strings, restated
verbatim from `26-part2` :960-964 / `29` :181-187:

| Record | `type` | Keys |
|---|---|---|
| `TextRun` | `text_chunks` | `index` int, `time0_ms` int, `dt_ms` int[], `texts` string[] |
| `ReasoningRun` | `reasoning_chunks` | `index` int, `time0_ms` int, `dt_ms` int[], `texts` string[] |
| `ToolCallRun` | `tool_call_chunks` | `index` int, `time0_ms` int, `dt_ms` int[], `id` string, `name` string (**omitted when `nullopt`**), `args` string[] |
| `ChunkRecord` | `chunk` | `time_ms` int, `event` object (**the §3 codec**) |

The three run schemas are owned by `29`/`26`; this errata only confirms they are
not redefined here. `ToolCallRun.name` is `std::optional<std::string>`
(`26-part2` :562), hence the one record-level omission; `id` is `ToolCallId`
(string), `dt_ms`/`texts`/`args` are always-present arrays, and `index` is the
`std::size_t` value as a JSON integer. The `type` strings keep ymh's underscore
convention (`26-part2` :965-966).

**The `event` object.** The dangling `<existing StreamEvent codec>` at
`26-part2` :964 / `29` :186 reads, by this amendment, as §3 above. The codec is
**created here** (it does not pre-exist); after Wave 2 the phrase is no longer a
reference to something that was never written.

**`payload::AssistantMessage` / `payload::AssistantAttempt`.** Their keys are
already pinned (`29` §3.2 :176-187; `26` §4.3.9.1 :949/:957) and are unchanged:
`AssistantAttempt` = `turn` int, `step` int, `stream` `AssistantStreamRecord[]`;
`AssistantMessage` = existing `id`/`content`/`usage` (omitted) **plus** `stream`
`AssistantStreamRecord[]` and `replay_state` object (omitted when none). The
`replay_state` object's internal keys are now §3.3.

---

## 5. Round-trip requirement and test plan (33-D6)

### 5.1 The requirement (33-I1)

For every one of the eight alternatives `e`:
`from_json(to_json(e)) == e` — **encode∘decode is the identity** over the
alternative, including all nested objects and all enum values. Concretely:

- every alternative round-trips, including empty strings, `0`/`false`, empty
  arrays, empty `arguments` objects, and `FinishReason::Other`;
- every `FinishReason` (6) and `LLMErrorCode` (17) string round-trips through
  `to_string`/`parse_*`;
- `Finished` round-trips with `usage` present and absent, and with
  `replay_state` present and absent, preserving the omission;
- `ToolCallFinished.call.arguments` round-trips as an object (not a string);
- `ReplayEnvelope.state` round-trips arbitrary JSON, including `null`.

To make the identity mechanically expressible, this errata pins defaulted
`operator==` (additive; `Usage` already carries `operator<=>`,
`message.hpp:137`) on `TextDelta`, `ReasoningDelta`, `ToolCallStarted`,
`ToolCallDelta`, `ToolCallFinished`, `UsageEvent`, `Finished`, `StreamError`,
`ToolCallAssembled`, `LLMError`, and `ReplayEnvelope`. No behavior changes; the
operator is a test affordance.

### 5.2 Test plan additions (extending `29` §7)

The `29` §7 :446-450 obligations are extended, not replaced.

1. **`StreamEvent` codec round-trip.** New coverage in
   `tests/unit/llm_types_test.cpp` (which already includes `stream.hpp` and
   tests the algebra, `:40-44`) or a new
   `tests/unit/stream_event_codec_test.cpp`: a table over all eight
   alternatives asserting `type`, the exact key set, and the §5.1 identity.
2. **Discriminator assertions.** One `EXPECT_EQ(json.at("type"), "<value>")`
   per alternative, matching the §3.1 table (the same shape as the
   `wire_name`/`parse_event_type` assertions at `tests/unit/event_test.cpp:50-63`).
3. **Nested round-trips.** `ToolCallFinished.call` with a non-empty
   `arguments` object; `StreamError.error` with `http_status != 0`,
   non-empty redacted strings, and `retryable` true/false; `UsageEvent.usage`
   with all four counters non-zero; `ReplayEnvelope` with `state` an object and
   with `state` null.
4. **Omission assertions.** Encoding a `Finished` with no `usage`/`replay_state`
   emits neither key; decoding a `Finished` without them yields `nullopt`; a
   present key is never `null` (the `29` §3.3 Axis-A rule).
5. **Loud decode.** An unknown `type` and an unknown `code`/`reason` string both
   throw; an unknown extra key on a known `type` is ignored.
6. **Payload-level round-trip.** In `tests/unit/event_test.cpp` (extending
   `:50-63`): `all_event_types().size()` grows by **exactly one** (the shipped
   23 → the pinned 24; `29` §3.2 :160), and
   `wire_name(EventType::AssistantAttempt) == "assistant/attempt"`; an
   `AssistantAttempt` and an `AssistantMessage` carrying a mixed
   `AssistantStreamRecord[]` (all four record types, with a `ChunkRecord` for
   each of the eight `StreamEvent` alternatives) survive a full
   `Event` encode/decode.
7. **Version neutrality.** `kProtocolVersion == 1` and `kSchemaVersion == 1`
   are unchanged (re-asserted from `29` §7 :465-466; no version bump here).

---

## 6. Invariants

- **33-I1 — Codec round-trip.** §5.1 holds for all eight alternatives and all
  nested shapes.
- **33-I2 — Discriminator totality.** Every alternative has exactly one `type`
  string (§3.1); the decode dispatch is total over the eight and rejects
  everything else.
- **33-I3 — Key-set exactness.** `to_json` emits exactly the keys in §3.2/§3.3
  — no extra key, no missing non-optional key.
- **33-I4 — Omission fidelity.** `optional` fields are omitted when unset and
  preserved as `nullopt` on decode; non-optional fields are always present
  (§3.6).
- **33-I5 — Loud decode, tolerant keys.** Unknown `type`/enum strings throw
  (the `parse_event_type`/`parse_role` convention, `core/event.cpp:55-62`,
  `message.hpp:156-161`); unknown keys on a known `type` are ignored
  (`29` §3.3 :206-210).
- **33-I6 — Reuse, do not redefine.** The enum spellings come only from
  `to_string(FinishReason)`/`to_string(LLMErrorCode)`; `Usage` uses the shipped
  codec; there is exactly one `StreamEvent` codec (§3.5).
- **33-I7 — Version neutrality.** No `kProtocolVersion`/`kSchemaVersion` change
  (`29` §7 :465-466).
- **33-I8 — No in-memory shape change beyond the pinned addition.** The codec
  tracks the shipped algebra; the only field change is
  `Finished.replay_state`, already pinned at `26-part2` :586-589. This errata
  adds no `StreamEvent` member.

---

## 7. Failure modes

| ID | Trigger | Consequence | Guard |
|---|---|---|---|
| **33-F1** | Each call site invents its own `type` string | Two encoders disagree; `expand()` cannot reconstruct the timed sequence; replay round-trip fails | 33-I2 + the §5.2 table test |
| **33-F2** | An `optional` is encoded as `null` instead of omitted (or omitted when set) | A round-trip drops or fabricates `usage`/`replay_state`; `26` §4.6 additivity is violated | 33-I4 + §5.2 item 4 |
| **33-F3** | A second enum map (e.g. `"toolCalls"`, `"rate-limited"`) is open-coded | A valid row fails decode or two binaries spell the same value differently | 33-I6 + `28` §7 :734-744 |
| **33-F4** | An unknown `StreamEvent.type` appears inside a known `assistant/message` payload | Decode throws; on the durable axis this is the intended forward-fence (the DB is refused), on the wire it propagates rather than being skipped (the envelope-level skip of `29` §3.3 Axis B is unchanged) | 33-I5; recorded here so the behavior is not a surprise |
| **33-F5** | `arguments` is emitted as a string instead of an object | `ToolCallFinished` round-trips to a different value; the assembler sees a type error | 33-I3/33-D3 + §5.2 item 3 |
| **33-F6** | The codec is defined in two translation units (e.g. header + `events.cpp`) | ODR divergence or link error; the two copies drift | 33-I6 + 33-D5 (inline, single definition) |

---

## 8. dsh mapping

| dsh concept | ymh after this errata | Reference |
|---|---|---|
| `ChunkRecord` (raw non-delta chunk, `assistant-stream.d.ts:16-49`) | `ChunkRecord { time_ms, event }`, `event` = the §3 codec | `26-part2` :554-563 |
| dsh chunk discriminator (hyphenated) | ymh `snake_case` `type` (§3.1) | `26-part2` :965-966; `29` :187 |
| dsh `LLMError` wire fields | `code`/`http_status`/`provider_message`/`detail`/`retryable` (§3.3) | `08` §2.2 :188-193; `26-part2` :947 |
| dsh `ReplayEnvelope` | `{provider, version, state}` (§3.3) | `26-part2` :529-533 |

---

## 9. Explicit non-goals

- This errata **implements nothing**; it is a Wave-2 design freeze. No file
  other than this one is edited.
- It does **not** pin the assembler/accumulator algorithm, the
  `TimedStreamEvent.at` epoch/source, the sink wiring, `interrupted_blocks()`,
  or the `AssistantMessage` vs `AssistantAttempt` settlement predicate — those
  are Wave-2 GAP 2, owned by the `08` assembler/replay errata.
- It does **not** pin the record packing/merging rule (`26-part2` :554-558);
  only the record `type` strings are restated, unchanged.
- It does **not** redefine `Usage`, `ContentBlock`, `Message`, or the
  `AssistantStreamRecord` run schemas.
- It does **not** renumber any spec in place; it records the renumbering to
  34+ for the Wave-3+ reserved names.
- It does **not** claim Wave 2 is unblocked: GAP 2 (the assembler/replay
  contract) remains a blocker for the loop wiring and the settlement change, and
  `30` itself is still "written — pending re-gate" (`DESIGN_STATUS.md:42`).

---

## 10. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-19 | Initial authoring (Wave-2 GAP 1 close). Pins the missing `StreamEvent` JSON codec: the type-name-derived `type` discriminator for all eight shipped alternatives (`text_delta`, `reasoning_delta`, `tool_call_started`, `tool_call_delta`, `tool_call_finished`, `usage_event`, `finished`, `stream_error`); the exact key set and omission rules per alternative; the nested `ToolCallFinished.call` (`ToolCallAssembled`), `StreamError.error` (`LLMError`), `Usage`, and `ReplayEnvelope` shapes; the `parse_finish_reason`/`parse_llm_error_code` inverses of the shipped `to_string`s; the inline-ADL location; the round-trip identity (33-I1) and its test-plan additions. Replaces the dangling `<existing StreamEvent codec>` at `26-part2` :964 / `29` :186 and the `payload::LlmRetry` "(existing spec-08 codec)" at `26-part2` :947 by reference. Claims number 33; Wave-3+ reserved names renumber to 34+. No code, no version bump. |

---

## 11. References

- `26-dsh-alignment-part2.md` (verified Rev 7, GATE PASS): §4.3.4 :518-609;
  `ReplayEnvelope` :529-533; `TimedStreamEvent` :536-539; runs + `ChunkRecord`
  :554-563; `AssistantStreamRecord` :567; stream-algebra change :586-593;
  settlement :601-609; §4.3.9.1 :933-966 (dangling phrase :964);
  §5 Wave 2 :1424-1449.
- `29-event-family-errata.md` (verified): §3.1 :115-146; §3.2 :148-187
  (dangling phrase :186); §3.3 :189-242; §5 (29-I1) :400-405; §7 :442-468.
- `28-llm-service-boundary-errata.md` (verified Rev 3): §7 :685-744.
- `08-llm-provider.md` (verified): §2.1 :145-160; §2.2 :164-193; §3.2 :255-292.
- `30-architecture-cascade-errata.md`: §5.3 :320 (the required `08` assembler
  errata, Wave-2 GAP 2).
- `DESIGN_STATUS.md`: :42 (spec 30 pending re-gate), :41 (spec 29 verified),
  :44 (spec 28 Rev 3 verified).
- Tree: `include/ymh/llm/stream.hpp:23-30,40-58,63-69,71-104,129-180`;
  `include/ymh/agent/message.hpp:131-138,190-204`; `include/ymh/session/ids.hpp:18`;
  `include/ymh/llm/llm_call_config.hpp:80-133`;
  `include/ymh/core/event.hpp:51-77`; `src/core/event.cpp:18-42,46-62,76-87`;
  `src/session/events.cpp:20-22,125-207,211-535`; `tests/unit/event_test.cpp:50-63`;
  `tests/unit/llm_types_test.cpp:40-44`.
- Wave-2 design-first audit, GAP 1 (the `StreamEvent` codec dangling reference)
  and the "Minor gaps" `ReplayEnvelope` nested-keys finding, closed here.

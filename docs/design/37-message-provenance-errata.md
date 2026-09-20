# 37 - Message Provenance Errata (amends 01-session.md, Wave-3 slice 3)

```
Status: Rev 2 written · verified: - · reviewer: -
Component: 37 (errata) - amends 01-session.md by reference only
Depends on: 01-session.md (verified), 36-prompt-registry.md (verified, Rev 1),
            06-agent-loop.md (verified) + 31-agent-loop-errata.md (verified, Rev 2)
            for the pinned producer sites, 34-assembler-replay-errata.md,
            28-llm-service-boundary-errata.md,
            18-context-errata.md (the ContextSnapshot carrier)
Scope: the MessageSource / ContextForm provenance reshape of the message model,
       the additive provenance fields on the durable payloads, the
       ContextInjected.role default, the deriveMessages() projection, the
       durable JSON codecs (event payloads plus the Message snapshot carrier),
       the pinned producer sites that derive each provenance value, the
       per-payload default member initializers, and the durable nested JSON
       schema + enum strings
```

## 1. Purpose, authority, precedence

### 1.1 The problem this closes

`36-prompt-registry.md` §3.2/§3.3 (decisions `36-D7`, `36-D8`) reshapes the
spec-`01` message model: the LLM-facing `Message` and four durable payloads gain
`MessageSource` / `ContextFormed` provenance, and `payload::ContextInjected.role`
defaults to `User` instead of `System`. Spec 36 declares itself an errata to
`01` for that reshape (`36-prompt-registry.md:40-42`) and lists the `01` errata
as **required** before Wave-3 code (`36-prompt-registry.md:1004`). That errata
did not exist. This file is it.

Wave-3 slices 1 and 2 are already implemented and green
(`e0bb6a84f`, `f66f2a8ff`); the provenance reshape (slice 3) is the remaining
piece, and per `AGENTS.md` it needs an owning spec before code.

### 1.2 Authority and relationship to the other specs

- `36-prompt-registry.md` owns the provenance vocabulary and the exact types.
  This errata **amends `01` by reference** and does not restate spec 36. Where
  this file and `36` disagree, `36` wins and the disagreement is an open
  question (§14).
- `01-session.md` owns the message model, the durable payloads,
  `deriveMessages()`, and the JSON codecs. This file pins only the deltas.
- `34-assembler-replay-errata.md` owns the replay harness and the settlement
  contract. Provenance is carried through its prefix reconstruction unchanged.
- `28-llm-service-boundary-errata.md` owns `LlmRequestHeader`. This errata does
  not change the header; `system_prompt_digest` remains the digest of the
  rendered prompt.
- `18-context-errata.md` owns the read-only `ContextSnapshot`. That snapshot
  serializes `std::vector<Message>` (`src/session/session_persistence.cpp:1041`),
  so the `Message` codec is a durable obligation of this errata (§7).

### 1.3 Verified-against-the-tree note

Every `file:line` below was re-read against the working tree at the time of
writing. The relevant facts:

- `Message` is `include/ymh/agent/message.hpp:123-127`, with `tool_call_id` at
  `:126`; its JSON codec is `:171-188`.
- `payload::UserMessage` is `include/ymh/session/events.hpp:88-91`;
  `AssistantMessage` `:105-112`; `ToolResult` `:140-148`; `ContextInjected`
  `:166-170` with the role default at `:168`.
- The payload codecs are `src/session/events.cpp:294-301` (UserMessage),
  `:319-344` (AssistantMessage), `:380-406` (ToolResult), `:422-438`
  (ContextInjected).
- `deriveMessages()` is `src/session/session.cpp:362-...`; the four message
  projection cases are `:414-421`, `:426-439`, `:447-459`, `:462-466`. The
  synthetic helpers are `:129-149`.
- The producer sites are all in `src/agent/agent_loop.cpp`:
  `appendUserMessage` `:373-378`; `appendContextInjected` `:380-386`, called from
  `materializeInstructions` `:388-402`, `materializeContexts` `:404-429`, the
  folded inbox `:559-565`, and the inject trigger `:793-794`; the
  `AssistantMessage` settlement `:963-971`; and the four `ToolResult`
  constructions `:584-590` (rejected), `:684-690` (denied), `:694-719`
  (executed), and `:698-704` (runtime unavailable). `buildRequest` `:472-492`
  exposes the effective `config_.provider` / `config_.model`.
- `ContextMessage` (`include/ymh/agent/agent.hpp:88-92`) carries no provenance
  today; §5.4 pins the additive fields the producer needs.
- The provider wire mapping is `src/llm/openai_adapter.cpp:235-293`; it builds
  each provider JSON object field-by-field and never reads provenance. It reads
  `tool_name` as a member of a `ContentBlock` inside `content` (`:279-280`), not
  as a `Message` field.
- The persisted snapshot writes `nlohmann::json(messages).dump()`
  (`src/session/session_persistence.cpp:1041`) and reads it back at `:1067-1068`.
  `SessionPersistence::checkpoint` (`:1018-1050`) and `loadSnapshot` (`:1052`)
  have no production caller today, and `snapshotIsCurrent` (`:1076-1088`)
  compares only `at_sequence`, not the stored messages. The round-trip
  obligation is nonetheless real because the codec is the durable carrier; §7.3
  and OQ-7 reconcile the invalidation claim with this code.

One line-drift instance was found and is recorded as OQ-5 (§14).

## 2. Amendment register

| ID | Amended clause (01) | Verified code anchors | New behaviour |
|---|---|---|---|
| **37-A1** | §4.5 `UserMessage` (`:424-427`) | `events.hpp:88-91`; `events.cpp:294-301` | add `MessageSource source` (structural/canonical default `{Kind::User}`) |
| **37-A2** | §4.5 `AssistantMessage` (`:440-444`) | `events.hpp:105-112`; `events.cpp:319-344` | add `MessageSource source` (structural default `{Kind::Model}`; a real row carries `provider`/`model`, §5.1/§5.4) |
| **37-A3** | §4.5 `ToolResult` (`:459-467`) | `events.hpp:140-148`; `events.cpp:380-406` | add `MessageSource source` (structural default `{Kind::Tool}`; a real row carries `call == id`) and `ContextFormed context` |
| **37-A4** | §4.5 `ContextInjected` (`:481-485`) | `events.hpp:166-170`; `events.cpp:422-438` | add `MessageSource source` (structural default `{Kind::Plugin}`) and `ContextFormed context`; change `role` default `System` -> `User` (`36-D8`) |
| **37-A5** | §4.5 `Message` (via `include/ymh/agent/message.hpp:123-127`) | `message.hpp:171-188` | add optional `source` and `context`; no existing field renamed or reordered |
| **37-A6** | §6.3 `deriveMessages` (`:757-773`) | `session.cpp:414-466`; `:129-149` | carry `source`/`context` from the payload onto the projected `Message`; synthetic tool messages get `Kind::Tool` |
| **37-A7** | §6.3 projection algorithm (`:733-805`) | `session.cpp:362-...` | no new projected event types; only the four message bodies change |
| **37-A8** | §12 I7, I21 | `message.hpp:123-127`; `session_persistence.cpp:1067-1068` | add defaulted `operator==` so purity and snapshot equivalence include provenance |
| **37-A9** | §13.2 S3 | `events.cpp` codec helpers | unknown provenance enum strings fail loud, same class as an undecodable payload |

The amendment is **additive to the pinned member layout**: no field is renamed,
reordered, or removed. It is a **Brk.** change to the durable JSON shape and to
the default of one field (`36-D7`, `36-D8`).

## 3. Provenance types (pinned by 36 §3.1, not restated)

The provenance vocabulary is pinned in `36-prompt-registry.md` §3.1
(`:560-595`): `ContextForm`, `ContextSnapshotSection`, `ContextFormed`, and
`MessageSource`, in a new header `include/ymh/agent/provenance.hpp` (`36-D1`).
This errata does **not** repeat those definitions. Two obligations attach to
them:

- **Construction coupling (`36-I8`).** `ContextFormed{form == Snapshot}`
  requires non-empty `sections`; `form == Notice` requires non-empty `summary`.
  A violation fails at construction. `MessageSource.kind` gates its fields:
  `plugin`/`context` only for `Kind::Plugin`, `call` only for `Kind::Tool`,
  `provider`/`model` only for `Kind::Model`.
- **Equality (37-D2).** `MessageSource`, `ContextFormed`, and
  `ContextSnapshotSection` gain a defaulted `operator==`. The codecs and the
  snapshot-equivalence check need it; without it "at its default" is not
  expressible and `01` I21 cannot see a provenance difference.

`SystemMessage` and `ToolResultMessage` (36 §3.3, `:647-670`) are
**projection-level** types in the same header. They are not durable payloads
and are not part of this `01` amendment; spec 36 owns them (OQ-6).

## 4. The `Message` reshape (37-A5, `36-D7`)

`Message` is the LLM-facing type pinned by `00-architecture.md` §12 and reused
by `01` §4.5. Wave 3 adds two optional, defaulted fields. The exact shape is
pinned by `36-prompt-registry.md` §3.2 (`:606-616`):

```cpp
// include/ymh/agent/message.hpp (amended)
struct Message {
    Role                      role = Role::User;
    std::vector<ContentBlock> content;
    std::string               tool_call_id;   // pinned type, unchanged (:126)
    std::optional<MessageSource> source;      // 37-A5: who produced it
    std::optional<ContextFormed> context;     // 37-A5: what kind of thing it is
};
```

Both fields are `std::optional` because the system message and the compaction
summary have no meaningful producer at the `Message` level (OQ-3), and because
old persisted snapshots decode to `nullopt`.

### 4.1 The `Message` codec obligation (durable)

`Message` is JSON-persisted. The persisted `ContextSnapshot` path serializes
`std::vector<Message>` with the ADL codec (`session_persistence.cpp:1041`) and
decodes it at `:1067-1068`. Therefore `to_json`/`from_json`
(`message.hpp:171-188`) must:

1. Emit `"source"` only when `message.source.has_value()`, and `"context"` only
   when `message.context.has_value()`. Absent keys decode to `nullopt`.
2. Decode an unknown `ContextForm` or `MessageSource::Kind` string as a **loud**
   failure, matching the existing unknown-role/unknown-kind behaviour
   (`message.hpp:156-169`, `:179-188`; `01` S3).
3. Keep the existing keys (`role`, `content`, `tool_call_id`) unchanged so an
   old reader tolerates the new keys through its unknown-key handling.

## 5. Durable payload provenance (37-A1..37-A4, `36 §3.3`)

The four durable payloads gain provenance. The exact field list is pinned by
`36-prompt-registry.md` §3.3 (`:672-686`). The amended shapes:

```cpp
namespace ymh::payload {

struct UserMessage {
    MessageId                 id;
    std::vector<ContentBlock> content;
    MessageSource             source{};  // 37-A1; Kind::User (the MessageSource default)
};

struct AssistantMessage {
    MessageId                          id;
    std::vector<ContentBlock>          content;
    std::optional<Usage>               usage;
    std::vector<AssistantStreamRecord> stream;        // 29-D3, unchanged
    std::optional<ReplayEnvelope>      replay_state;  // 29-D3, unchanged
    // 37-A2 / 37-D11: the structural default (and canonical codec default) is
    // `{Kind::Model}`. The settlement producer fills `provider`/`model` from
    // `config_.provider`/`config_.model` (§5.4), which makes the row differ from
    // the default and emit the `source` key (§5.1, OQ-2).
    MessageSource source{.kind = MessageSource::Kind::Model};
};

struct ToolResult {
    ToolCallId                 id;
    std::string                name;
    ToolOutcome                outcome = ToolOutcome::Ok;
    std::string                output;
    bool                       truncated = false;
    std::optional<std::string> error;
    std::chrono::milliseconds  duration{0};
    // 37-A3 / 37-D11: `call` mirrors `id` (37-I4). The conditional keeps the
    // structural default at `{Kind::Tool, call == nullopt}` when `id` is empty,
    // so the canonical codec default is `{Kind::Tool}` (§5.1).
    MessageSource source{.kind = MessageSource::Kind::Tool,
                         .call = id.empty() ? std::optional<ToolCallId>{}
                                            : std::optional<ToolCallId>{id}};
    ContextFormed context{};  // 37-A3; form == None
};

struct ContextInjected {
    MessageId      id;
    Role           role = Role::User;  // 37-A4: was Role::System (events.hpp:168)
    std::string    text;
    MessageSource  source{.kind = MessageSource::Kind::Plugin};  // 37-A4 / 37-D11
    ContextFormed  context{};  // 37-A4; form == None
};

} // namespace ymh::payload
```

### 5.1 Canonical defaults and the omit-at-default rule (37-D3)

Spec 36 pins the wire keys as `source` and `context`, "both omitted when at
their default" (`36-prompt-registry.md:684-686`). This errata pins the canonical
default the codec compares against as the **structural default** of the
payload's `source` member (37-D11): the value a default-constructed payload
holds.

| Payload | Canonical default source | Canonical default context |
|---|---|---|
| `UserMessage` | `{Kind::User}` | n/a (no field) |
| `AssistantMessage` | `{Kind::Model}` | n/a (no field) |
| `ToolResult` | `{Kind::Tool, call == nullopt}` | `{form = None}` |
| `ContextInjected` | `{Kind::Plugin}` | `{form = None}` |

The codec emits `source` iff `value.source != canonical_default`; it emits
`context` iff `value.context != ContextFormed{}`. This is why `MessageSource`
needs defaulted equality (37-D2). A row whose source carries nothing beyond its
kind (an `AssistantMessage` with an empty `provider`/`model`, a `ToolResult`
whose `call` is `nullopt`) omits the key; decode restores the kind from the
same structural default, so the round-trip is exact (37-I6) and the kind is
never lost. A row that carries `provider`/`model`/`call`/`plugin`/nested
`context` differs from the default and emits the key. OQ-2 records the
alternative reading (a single `MessageSource{}` default) and why it would drop
the `Model`/`Tool`/`Plugin` rows' kind.

The structural default is pinned in the member initializers (§5, 37-D11):
`AssistantMessage` -> `{Kind::Model}`, `ToolResult` -> `{Kind::Tool}` (with
`call` mirroring a non-empty `id`), `ContextInjected` -> `{Kind::Plugin}`. A
bare `MessageSource{}` is `Kind::User`; it is correct only for `UserMessage`.
A payload default that used `MessageSource{}` elsewhere would drop the kind
(37-F1, 37-F11).

### 5.2 Codec anchors and the role change (37-A4)

- `UserMessage`: `events.cpp:294-301`.
- `AssistantMessage`: `events.cpp:319-344`.
- `ToolResult`: `events.cpp:380-406`.
- `ContextInjected`: `events.cpp:422-438`. The codec already writes `role`
  explicitly (`:425`), so the default change is a no-op for stored rows and only
  affects new default-constructed payloads (`36-I9`).

`payload::AssistantChunk` (`events.hpp:98-103`) and `payload::ToolCall`
(`events.hpp:124-131`) are unchanged (`36 §3.3`, `:681-682`): a chunk is a live
delta and a tool call already lives inside the assistant content.

### 5.3 Durable nested JSON schema and enum strings (37-D12)

Spec 36 §3.3 delegates the `to_json`/`from_json` bodies to this errata
(`36-prompt-registry.md:683-686`). The wire keys are `source` and `context`;
their nested shape and the enum spellings are pinned here. The spelling
convention matches the existing codecs: lowercase `snake_case` for multiword
names (`content_block_kind_name`, `tool_outcome_name`,
`include/ymh/agent/message.hpp:39-104`).

**Enum strings.** Total and unique:

| Enum | Member | String |
|---|---|---|
| `ContextForm` | `None` / `Instructions` / `Catalog` / `Snapshot` / `Notice` / `Relay` / `Recall` | `none` / `instructions` / `catalog` / `snapshot` / `notice` / `relay` / `recall` |
| `MessageSource::Kind` | `User` / `Plugin` / `Model` / `Tool` | `user` / `plugin` / `model` / `tool` |

**Nested objects.**

```jsonc
// ContextSnapshotSection
{ "name": "...", "text": "..." }            // both keys always present

// ContextFormed: `form` always; the form-gated body iff non-empty
{ "form": "snapshot", "sections": [ ... ] } // sections iff form == snapshot
{ "form": "notice",   "summary": "..." }    // summary iff form == notice
{ "form": "instructions" }                  // other forms: form only

// MessageSource: `kind` always; the kind-gated members iff set
{ "kind": "user" }
{ "kind": "plugin", "plugin": "runtime-context", "context": { ... } }
{ "kind": "model",  "provider": "deepseek", "model": "deepseek-flash" }
{ "kind": "tool",   "call": "<tool-call-id>" }
```

The gated members are omitted when empty/absent (`plugin`, `provider`, `model`
iff non-empty; `call` iff `has_value()`; the nested `context` iff
`!= ContextFormed{}`). `ContextFormed`'s `sections`/`summary` follow the
construction coupling of `36-I8`: a `Snapshot` carries a non-empty `sections`,
a `Notice` a non-empty `summary`, and neither is emitted otherwise.

**Decode rules.** A missing `source`/`context` key decodes to the payload's
canonical default (§5.1). A missing nested member decodes to its structural
default (`form == None`, empty `sections`/`summary`, empty strings,
`call == nullopt`). An unknown `form` or `kind` string **fails loud** (37-A9,
`01` S3), as does a key present but not gated for the decoded `kind` (for
example a `plugin` key on `Kind::Model`), which is the decode-side expression of
the `36-I8` construction coupling. The codec emits a member iff it is non-empty
and gated, so a real row never carries a non-gated key.

### 5.4 Producer sites and value derivation (37-D13)

37-I1 asserts that every durable payload carries provenance; this clause pins
**where** each value is produced and **how** it is derived, so no other code
path can invent provenance. The sites are all in `src/agent/agent_loop.cpp`:

| Payload | Append site | `source` derivation | `context` derivation |
|---|---|---|---|
| `UserMessage` | `appendUserMessage` `:373-378` | `{Kind::User}` (the NSDMI) | n/a (no field) |
| `ContextInjected` | `appendContextInjected` `:380-386`, reached from `materializeInstructions` `:401`, `materializeContexts` `:429`, the folded inbox `:563`, and the inject trigger `:793-794` | `{Kind::Plugin, plugin, context}` copied from the extended `ContextMessage` | copied from `ContextMessage.context` |
| `AssistantMessage` | the settlement `:963-971` | `{Kind::Model, provider = config_.provider, model = config_.model}` (`buildRequest` `:472-492`) | n/a (no field) |
| `ToolResult` | `:584-590` (rejected), `:684-690` (denied), `:694-719` (executed), `:698-704` (runtime unavailable) | `{Kind::Tool, call = call.id}` | `{form = None}` today; a future notice producer (Wave 4) is the only writer of a non-`None` form |

`ContextMessage` (`include/ymh/agent/agent.hpp:88-92`) currently carries only
`role`/`text`/`startsTurn`. It gains the two provenance fields so the
`ContextInjected` producer has a value to copy:

```cpp
// include/ymh/agent/agent.hpp (amended)
struct ContextMessage {
    Role          role = Role::System;
    std::string   text;
    bool          startsTurn = false;
    MessageSource source{.kind = MessageSource::Kind::Plugin};  // 37-D13
    ContextFormed context{};                                    // 37-D13
};
```

`appendContextInjected` copies `context.source` and `context.context` verbatim
onto the payload; for `Kind::Plugin` the top-level `context` and the nested
`source.context` are the same value (OQ-1). The plugin ids and forms are pinned:

| Producer | `source.plugin` | `context.form` |
|---|---|---|
| instruction loader (`materializeInstructions` `:388-402`) | `agent-instructions` | `Instructions` |
| runtime context (`materializeContexts` `:404-429`) | `runtime-context` | `Snapshot` (with `sections`) |
| skill catalog | `skill-catalog` | `Catalog` |
| caller-supplied `inject()` (`:178-184`) | caller-provided, defaults to `agent` | caller-provided, defaults to `None` |
| compaction summary (OQ-3, not yet assigned) | `context-compaction` | `None` |

The `runtime-context` and `skill-catalog` ids are the canonical registry names
of `36 §2.1` (`36-prompt-registry.md:107-110`); `agent-instructions` mirrors
`dsh-agent-instructions` (`36 §9`, `:981`). No other producer writes a
provenance value.

## 6. Projection changes in `deriveMessages()` (37-A6, `36 §3.5`)

`deriveMessages()` stays a pure function of `(header, events())` (`01` I7,
`01-session.md:1121-1123`). Three bodies change:

1. **`UserMessage`** (`session.cpp:414-421`): copy `value.source` onto the
   projected message.
2. **`AssistantMessage`** (`session.cpp:426-439`): copy `value.source`. The
   `tool_use` pending-set logic is untouched.
3. **`ToolResult`** (`session.cpp:447-459`): copy `value.source` and
   `value.context`; the `tool_call_id` pairing is untouched.
4. **`ContextInjected`** (`session.cpp:462-466`): copy `value.source` and
   `value.context` in addition to `role`/`text`.

The system message is still **not** projected by `deriveMessages()`; it is
prepended by `SessionContextAssembler::assemble` (`src/agent/context_assembler.cpp:45-74`),
so the replay reconstruction rule (`34 §9.2`) is unchanged.

### 6.1 Synthetic tool messages (37-D6)

`make_synthetic_tool_message` (`session.cpp:139-149`) synthesizes a `Role::Tool`
message for an unmatched `tool_use` when a turn is cancelled or fails (`01`
I12, `session.cpp:395-413`). It must set
`MessageSource{kind = Kind::Tool, call = id}`. Without this, the default
`std::optional` is empty and the message has no provenance, which would let a
synthetic message be mistaken for unlabeled content. `context` stays default
(no Notice); OQ-4 records the question of whether the synthetic case should
carry a `Notice`.

## 7. Compatibility, codecs, and snapshot invalidation

### 7.1 Old rows decode

A row written before this amendment has no `source`/`context` keys. Each codec
decodes the missing key to the payload's canonical default (37-D3). A missing
`role` on `ContextInjected` is impossible for stored rows because the codec
always wrote it (`events.cpp:425`).

### 7.2 New rows are forward-fenced

A new row carries the two keys only when non-default. An old reader that does
not know them drops them through its existing unknown-key tolerance; the
message still decodes. This matches `36 §3.3` (`:684-686`).

### 7.3 Snapshot invalidation (37-D7)

`SessionSnapshot.messages` is `deriveMessages()` output, persisted as
`nlohmann::json(messages).dump()` (`session_persistence.cpp:1041`). A snapshot
written before this amendment decodes with `source == nullopt` on every
message. `01` I21 (`01-session.md:1182-1184`) requires that when
`snapshot.at` equals the head, `snapshot.messages == deriveMessages()`.

With a defaulted `operator==` on `Message` (37-D2), a pre-amendment snapshot
compares unequal to a freshly derived list and is therefore **stale**: it is
discarded and recomputed, never patched (`01` I21, S12). This errata pins that
`Message`, `ContentBlock`, `MessageSource`, `ContextFormed`, and
`ContextSnapshotSection` all gain a defaulted `operator==`, and that the
snapshot-equivalence check includes provenance. No schema or format bump is
needed; the existing equivalence rule does the work.

**Reconciliation with the shipped code.** The equivalence check this clause
relies on is `01` I21, and the shipped code does not yet enforce it on the
durable path:

- `Session::snapshot()` (`src/session/session.cpp:663-671`) recomputes
  `messages = deriveMessages()` in memory, so the in-memory snapshot is
  equivalent by construction once `Message::operator==` includes provenance.
- The durable path is `SessionPersistence::checkpoint` (`:1018-1050`), which
  writes `messages_json`, and `loadSnapshot` (`:1052-1074`), which reads it
  back. Neither has a production caller today; both are exercised only by
  `tests/unit/persistence_test.cpp:604,679,710-720`.
- `snapshotIsCurrent` (`:1076-1088`) compares only `at_sequence` against the log
  head (`return at == head;`). It does **not** compare `messages`, so it cannot
  observe a provenance-only difference.

The codec and `operator==` obligations (37-D2, 37-I6, 37-I10) are therefore
required for `loadSnapshot` correctness and for any future production
checkpoint/load, but the **durable** invalidation claim is not yet exercised by
a production equivalence check. This is recorded as OQ-7, not silently assumed.
It is not a licence to skip the codec: the moment a production caller loads a
snapshot, 37-I10 is what makes a pre-amendment row detectable.

### 7.4 No provider leakage (37-D8)

`Message` is also the provider request type, but the OpenAI-compatible wire
mapping is a bespoke field-by-field builder
(`src/llm/openai_adapter.cpp:235-293`) that reads only `role`, `content`,
`tool_call_id`, and the `tool_name` member of a `ContentBlock` inside `content`
(`:279-280`). `tool_name` is not a `Message` field. Provenance is never
serialized to the model. This errata pins that `map_message` must not gain a
provenance key.

## 8. Invariants

These extend `01` §12. `37-I*` are local to this errata.

- **37-I1 (durable provenance).** Every durable message-bearing payload carries
  its `MessageSource`; the log is the source of truth for who produced a
  message. The payload field is never absent by design on a new write (the wire
  key may be omitted at the canonical default, §5.1, which decodes back to the
  same value), and every value is derived at a pinned producer site (§5.4,
  37-I11).
- **37-I2 (projection fidelity).** `deriveMessages()` copies `source`/`context`
  from the payload to the projected `Message` unchanged. It never invents
  provenance except for the documented synthetic case (37-I3).
- **37-I3 (synthetic provenance).** A synthesized tool message (cancel or fail
  pairing, `01` I12) carries `MessageSource{Kind::Tool, call = id}`. It never
  claims `Kind::Model` and never carries an empty source.
- **37-I4 (tool source identity).** For a `ToolResult`, `source.call == id`; for
  a projected tool `Message`, `tool_call_id == source.call` when `source` is
  present. A mismatch fails loud.
- **37-I5 (role default).** A default-constructed `ContextInjected` is
  user-role (`36-I9`). An explicit stored role is preserved verbatim; existing
  rows are unaffected.
- **37-I6 (codec round-trip).** `from_json(to_json(x)) == x` for every amended
  payload and for `Message`. A missing key decodes to the canonical default; an
  unknown enum string fails loud (`01` S3).
- **37-I7 (additive layout).** No existing member is renamed, reordered, or
  removed. The amendment is additive only.
- **37-I8 (no provider leakage).** Provenance never reaches the model wire
  (`openai_adapter.cpp:235-293`).
- **37-I9 (provenance is inert for logic).** Provenance never affects tool
  pairing, compaction boundaries, turn/step nesting, or any other `01`
  projection rule. It is carried, not interpreted.
- **37-I10 (snapshot equivalence includes provenance).** `Message`'s defaulted
  equality includes `source`/`context`, so `01` I21 rejects a pre-amendment
  snapshot. The shipped durable path does not yet perform the message
  comparison (OQ-7); the invariant is the contract a production loader must
  satisfy.
- **37-I11 (producer origin).** Each payload's provenance is derived only at
  the append sites pinned in §5.4; no other code path constructs a
  provenance-bearing payload. `AssistantMessage.source.provider`/`.model` come
  from `config_.provider`/`config_.model`; `ToolResult.source.call` equals the
  tool call id; `ContextInjected.source` is the plugin identity and form the
  `ContextMessage` carried.
- **37-I12 (structural defaults).** A default-constructed payload's `source`
  member initializer has the canonical `Kind`: `User` for `UserMessage`,
  `Model` for `AssistantMessage`, `Tool` for `ToolResult`, `Plugin` for
  `ContextInjected` (§5). A bare `MessageSource{}` is `Kind::User` and is
  therefore never used as a payload default except for `UserMessage`.

## 9. Failure modes

### 9.1 Shared findings (`F1`-`F12`, §54)

- **F1 (path/process isolation).** Not applicable: no path is resolved here.
- **F3 (late event after close).** A provenance-carrying append is still a
  durable append and is rejected once the session is terminal; this errata adds
  no write path.
- **F10 (resume-suspended).** A resumed session re-derives messages from the log;
  provenance is recomputed from the payloads, never fabricated.

The remaining shared findings are out of scope for this component. The session's
event-sourcing invariant (`01` §12) applies by reference.

### 9.2 Component-local failure modes

- **37-F1 (silent wrong default).** A payload with a missing `source` decodes to
  the wrong kind instead of its canonical default. Guard: 37-I6; per-payload
  default table (37-D3).
- **37-F2 (projection drops provenance).** `deriveMessages()` builds a fresh
  `Message` and forgets `source`/`context`. Guard: 37-I2; round-trip test.
- **37-F3 (synthetic mislabeled).** A cancel/fail synthetic tool message gets an
  empty or `Kind::User` source. Guard: 37-I3.
- **37-F4 (unknown enum accepted).** A `ContextForm`/`Kind` string outside the
  enum decodes to a fallback. Guard: 37-I6; fail loud (`01` S3).
- **37-F5 (default always emitted).** The codec writes `source`/`context`
  unconditionally, bloating rows and defeating the forward fence. Guard: the
  omit-at-default rule (37-D3).
- **37-F6 (role default regression).** A new `ContextInjected` silently writes
  system-role and the model reads injected context as instructions. Guard:
  37-I5; the default is user-role and the codec is explicit.
- **37-F7 (stale snapshot served).** A pre-amendment snapshot decodes without
  provenance and is served as current. Guard: 37-I10; `01` I21 discards it. The
  shipped `snapshotIsCurrent` is sequence-only, so no production path currently
  performs the message comparison (OQ-7); a production loader must use
  `Message` equality.
- **37-F8 (provenance leaks to the model).** `map_message` serializes
  `source`/`context`. Guard: 37-I8.
- **37-F9 (tool source mismatch).** `source.call != id`, so a projection pairs
  the wrong tool result. Guard: 37-I4.
- **37-F10 (producer omits provenance).** An append site builds a payload
  without setting `source` (for example a `ToolResult` written by a path other
  than the four pinned in §5.4), so the row is unlabeled. Guard: 37-I1, 37-I11;
  the producer-site test (13.1).
- **37-F11 (wrong structural default).** A payload's `source` member
  initializer is left as `MessageSource{}`, so a default-constructed
  `AssistantMessage`/`ToolResult`/`ContextInjected` claims `Kind::User`. The
  round-trip test (37-I6) cannot catch this because the wrong kind round-trips
  faithfully. Guard: 37-I12; the default-construction test (13.1).
- **37-F12 (non-gated nested key).** `MessageSource` carries a member that its
  `kind` does not gate (for example `plugin` on `Kind::Model`), so the decode is
  ambiguous. Guard: §5.3 decode rules; fail loud (`01` S3).

## 10. dsh mapping

| dsh concept | ymh type / site | Reference |
|---|---|---|
| `MessageSourceMap` (`message.d.ts:94-104`) | `MessageSource` | `36 §3.1:581-592` |
| `ContextForm` (`message.d.ts:42-54`) | `ContextForm` | `36 §3.1:564-568` |
| `ContextFormed` | `ContextFormed` | `36 §3.1:572-579` |
| `ModelMessage` source/context | `Message.source` / `Message.context` | `36 §3.2:606-616` |
| `SystemMessage` / `ToolResultMessage` | projection structs | `36 §3.3:647-670`; `26-D14:152` |
| `ModelMessageSource.replayState` | kept on `AssistantMessage.replay_state` (34-I6), not on `MessageSource` | `36 §3.1:581-583` |

**Deliberate divergence (stated).** dsh's `ContextFormed` is a discriminated
union; ymh flattens it to one defaulted struct and enforces the coupling at
construction (`36 §8`, `:987-992`). dsh's `ModelMessageSource` carries
`replayState`; ymh keeps replay state once on the assistant settlement event
(`34-I6`), not here.

## 11. Dependencies

**Upstream (must be verified before slice-3 code):**

| Spec | What this errata needs | Status |
|---|---|---|
| `01-session.md` | the message model, payloads, projection, codecs being amended | verified (this errata amends it) |
| `36-prompt-registry.md` | the provenance vocabulary and the `36-D7`/`36-D8` decisions | verified Rev 1 |
| `06-agent-loop.md` + `31-agent-loop-errata.md` | the pinned producer sites (§5.4) and the `ContextMessage` extension | 06 verified; 31 verified Rev 2 |
| `34-assembler-replay-errata.md` | the replay prefix rule provenance must not disturb | Rev 4 written (Rev 3 verified) |
| `28-llm-service-boundary-errata.md` | the header/digest contract, unchanged | verified (Wave 1) |
| `18-context-errata.md` | the `ContextSnapshot` carrier that persists `Message` | verified |

**Downstream (consumers of this errata):**

- `38-transcript-provenance-errata.md` (the `17` errata) renders `source` and
  `context` rows; it depends on the `Message` fields pinned here.
- Wave 4 (retention/pruning) consumes `ToolResultMessage.context` notices.

**Code sites this errata touches (for the implementer; no code here):**
`include/ymh/agent/message.hpp:123-127,171-188`;
`include/ymh/agent/provenance.hpp` (new, `36-D1`);
`include/ymh/agent/agent.hpp:88-92` (`ContextMessage` gains provenance, §5.4);
`include/ymh/session/events.hpp:88-91,105-112,140-148,166-170`;
`src/session/events.cpp:294-301,319-344,380-406,422-438`;
`src/session/session.cpp:129-149,362-...,414-466`;
`src/agent/agent_loop.cpp:373-378,380-386,472-492,584-590,684-690,694-719,963-971`
(producer sites, §5.4);
`src/session/session_persistence.cpp:1018-1050,1052-1074,1076-1088` (the durable
snapshot path, §7.3);
`src/llm/openai_adapter.cpp:235-293` (must stay provenance-free).

## 12. Decision register

| ID | Decision | Add./Brk. | Owning spec |
|---|---|---|---|
| **37-D1** | The provenance types live in `include/ymh/agent/provenance.hpp` (`36-D1`); `message.hpp` includes it. This errata does not move them. | New | 36, 37 |
| **37-D2** | `MessageSource`, `ContextFormed`, `ContextSnapshotSection`, `ContentBlock`, and `Message` gain a defaulted `operator==`, required by the omit-at-default codec and the `01` I21 equivalence check. | Add. | 37 |
| **37-D3** | Payload provenance fields are non-optional (`36 §3.3`); each codec omits `source`/`context` iff it equals the payload's canonical default (§5.1). | Brk. (codec) | 01, 37 |
| **37-D4** | `ContextInjected.role` defaults to `Role::User`; the codec keeps writing `role` explicitly, so stored rows are unaffected. | Brk. (default) | 01, 36 |
| **37-D5** | `ToolResult.source.call == id`, and a projected tool message's `tool_call_id` equals `source.call` when present; a mismatch fails loud. | New | 37 |
| **37-D6** | `deriveMessages()` carries provenance verbatim; synthetic cancel/fail tool messages carry `Kind::Tool` with `call` and leave `context` default. | New | 01, 37 |
| **37-D7** | `Message` equality includes provenance, so `01` I21 discards a pre-amendment snapshot; no schema or format bump. The shipped durable path is test-only and sequence-only (OQ-7). | New | 01, 37 |
| **37-D8** | The provider wire mapping never serializes provenance (`openai_adapter.cpp:235-293`). | New | 37 |
| **37-D9** | `payload::AssistantChunk` and `payload::ToolCall` are unchanged (`36 §3.3`). | None | 36, 37 |
| **37-D10** | No new durable event type, no `kSchemaVersion` or `kProtocolVersion` bump (`36-D16`). | None | 01, 36 |
| **37-D11** | Each payload's `source` has a structural default member initializer with the canonical `Kind` (`Model`/`Tool`/`Plugin`; `User` for `UserMessage`), and that same structural default is the codec's canonical default (§5.1, §5). | Add. | 37 |
| **37-D12** | The durable nested JSON schema and the `ContextForm`/`MessageSource::Kind` enum strings are pinned (§5.3); unknown strings and non-gated keys fail loud. | New | 01, 37 |
| **37-D13** | The producer sites and value derivations are pinned (§5.4); `ContextMessage` gains `source`/`context` to carry the plugin identity and form to `appendContextInjected`. | Add. | 06, 37 |
| **37-D14** | `AssistantMessage.source.provider`/`.model` are `config_.provider`/`config_.model` at the settlement site; no second provider source. | New | 06, 28, 37 |

## 13. Test plan

Strategy follows `§44`; hermetic fixtures come from `FakeLLM` runs. No live
tests are required for provenance.

### 13.1 Unit tests

- **Type construction.** `ContextFormed{Snapshot}` without `sections` throws;
  `Notice` without `summary` throws; `MessageSource` field gating
  (`plugin`/`context` only for `Plugin`, `call` only for `Tool`,
  `provider`/`model` only for `Model`) (`36-I8`).
- **Codec round-trip per payload.** For each of `UserMessage`,
  `AssistantMessage`, `ToolResult`, `ContextInjected`: default in, default out;
  a non-default `source`/`context` survives; an unknown enum string throws.
- **Omit-at-default.** A default-constructed payload of each kind omits
  `source` (its value equals the structural default: `{Kind::User}`,
  `{Kind::Model}`, `{Kind::Tool, call == nullopt}`, `{Kind::Plugin}`). A row
  that carries data (`AssistantMessage` with `provider`/`model`, `ToolResult`
  with `call`, `ContextInjected` with `plugin`/`context`) emits it.
- **Structural defaults (37-I12).** A default-constructed `UserMessage` /
  `AssistantMessage` / `ToolResult` / `ContextInjected` has `source.kind`
  `User` / `Model` / `Tool` / `Plugin`; `ToolResult.source.call` is `nullopt`
  until `id` is set. This is the regression the round-trip test cannot catch
  (37-F11).
- **Nested schema.** The payload JSON matches §5.3: enum strings use the pinned
  spellings; a `Snapshot` emits `sections`, a `Notice` emits `summary`; a
  non-gated key on a `MessageSource` kind throws.
- **Producer sites.** A hermetic `FakeLLM` run asserts the §5.4 append sites
  produce the pinned values: `AssistantMessage.source == {Kind::Model,
  config.provider, config.model}`; every `ToolResult.source == {Kind::Tool,
  call = id}`; the instructions/snapshot/catalog `ContextInjected` rows carry
  `agent-instructions`/`runtime-context`/`skill-catalog` and the matching form.
- **`Message` codec.** `source`/`context` round-trip; absent keys decode to
  `nullopt`; an old two-key JSON (role/content/tool_call_id) decodes.
- **Projection.** `deriveMessages()` copies source/context for the four cases;
  a cancelled turn's synthetic tool message has `Kind::Tool` and the right
  `call` (37-I3); the system message is still absent from the projection.
- **Role default.** A default-constructed `ContextInjected` projects as
  user-role; an explicit system-role payload still projects as system (`36-I9`).

### 13.2 Integration tests

- **Provenance round-trip through the DB.** Append `UserMessage`,
  `ContextInjected` (instructions/snapshot/catalog), `ToolResult`, and
  `AssistantMessage` with provenance; reopen the DB; assert the projection and
  the source/context fields survive.
- **Snapshot equivalence.** Write a snapshot with a pre-amendment JSON body
  (no provenance) at the current head; load it with `loadSnapshot`, assert its
  `messages` compare unequal to a fresh `deriveMessages()` (37-I10), and assert
  the JSON decodes with the canonical defaults. This exercises the codec and
  equality directly; the shipped `snapshotIsCurrent` is sequence-only and does
  not perform the message comparison (OQ-7).
- **Replay.** A recorded session replays with identical provenance on every
  projected message.
- **No provider leakage.** Capture the request JSON for a session with
  provenance-bearing messages; assert no `source`/`context` key appears.

### 13.3 Failure-mode coverage matrix

| Failure | Test |
|---|---|
| 37-F1 | 13.1 codec round-trip (missing key default) |
| 37-F2 | 13.1 projection |
| 37-F3 | 13.1 projection (cancelled turn) |
| 37-F4 | 13.1 codec (unknown enum) |
| 37-F5 | 13.1 omit-at-default |
| 37-F6 | 13.1 role default |
| 37-F7 | 13.2 snapshot equivalence |
| 37-F8 | 13.2 no provider leakage |
| 37-F9 | 13.1 projection (`source.call` mismatch) |
| 37-F10 | 13.1 producer sites |
| 37-F11 | 13.1 structural defaults |
| 37-F12 | 13.1 nested schema (non-gated key) |

## 14. Open questions

- **OQ-1 (provenance context duplication).** `36 §3.1` gives `MessageSource` a
  `context` member for `Kind::Plugin` (`:588`), while `36 §3.2`/§3.3 give
  `Message` and `ContextInjected` a separate top-level `context` (`:614`,
  `:677`). For a plugin source both would carry the same `ContextFormed`. This
  errata pins that, when `source.kind == Plugin` and both are present, they are
  equal, and the top-level `context` is authoritative for rendering. If spec 36
  intends only one, one field should be dropped; recorded rather than resolved.
- **OQ-2 (default with row data).** `36 §3.3` says the keys are omitted "at
  their default" (`:684-686`) but does not say whether "default" means the
  payload's structural default or a bare `MessageSource{}`. This errata reads
  it as the structural default (kind-only: `{Kind::Model}`, `{Kind::Tool}`,
  `{Kind::Plugin}`), so a row carrying `provider`/`model`/`call`/`plugin` emits
  the key and the round-trip is exact (§5.1, 37-D3, 37-D11). The alternative
  reading (a single `MessageSource{}` = `Kind::User`) would omit `source` on
  every `AssistantMessage`/`ToolResult`/`ContextInjected` row and lose the
  kind. If spec 36 intends the alternative, this errata's §5.1 table and
  37-D11 change. Recorded, not resolved.
- **OQ-3 (compaction summary provenance).** `01` §6.3 prepends a synthetic
  summary `Message{Role::System, payload.summary}` (`:779`), but `36 §3.3`/§3.5
  does not list `ContextCompaction` among the provenance-carrying payloads.
  This errata leaves that message's `source`/`context` as `nullopt`. Confirm or
  assign a `MessageSource` (for example `Kind::Plugin`, plugin
  `"context-compaction"`).
- **OQ-4 (synthetic tool notice).** This errata gives the cancel/fail synthetic
  tool message a `Kind::Tool` source but no `ContextFormed` notice (37-D6).
  `36 §3.3` describes `ToolResultMessage.context` notices as Wave-4 retention or
  truncation notices, so leaving the synthetic case default is conservative.
  Confirm that a synthetic message needs no `Notice`.
- **OQ-5 (line drift).** `36 §3.3` cites `src/agent/context_assembler.cpp:41-65`
  and `:55-63` for the system-message build; the tree has `assemble` at
  `:45-74` and the system-message construction at `:64-72`. This is citation
  drift of the same class as `36 §12 Q1`, not a semantic conflict. Likewise,
  `36 §2.5` (`:478`) cites `AgentLoop::appendContextInjected` at
  `agent_loop.cpp:373-379`, but the tree has `appendUserMessage` at `:373-378`
  and `appendContextInjected` at `:380-386`; the citation points one function
  too early. This errata cites the real lines (§5.4).
- **OQ-6 (projection-type ownership).** `SystemMessage` / `ToolResultMessage`
  are pinned by `36 §3.3` and live in `provenance.hpp`. They are not durable
  payloads and are not part of this `01` amendment. Ownership stays with spec 36
  (its Q2 already records the interpretation). Confirm no `01` change is
  expected for them.
- **OQ-7 (durable snapshot path is test-only).** The snapshot-invalidation claim
  (37-D7, 37-I10) rests on `01` I21's message equivalence, but the shipped
  durable path does not enforce it: `SessionPersistence::checkpoint` and
  `loadSnapshot` have no production caller, and `snapshotIsCurrent`
  (`session_persistence.cpp:1076-1088`) compares only `at_sequence`, not
  `messages`. The in-memory `Session::snapshot()` (`session.cpp:663-671`)
  recomputes `deriveMessages()` and is equivalent by construction. This errata
  keeps the codec/equality obligations (needed by `loadSnapshot` and any future
  production checkpoint) but records that no production equivalence check
  currently detects a provenance-only stale row. Confirm the intended production
  snapshot policy (sequence-only vs message-equivalence) before relying on
  invalidation in production.

## 15. References

- `docs/design/01-session.md` §4.5, §6.3, §12 (I7, I12, I21), §13.2 (S3, S12).
- `docs/design/36-prompt-registry.md` §3, §6 (`36-I8`, `36-I9`), §7 (`36-F10`,
  `36-F11`), §10 (`36-D7`, `36-D8`, `36-D16`), §12.
- `docs/design/34-assembler-replay-errata.md` §9.2, §11 (34-I6).
- `docs/design/28-llm-service-boundary-errata.md` §4.
- `docs/design/18-context-errata.md` (the `ContextSnapshot` carrier).
- `include/ymh/agent/message.hpp`; `include/ymh/agent/agent.hpp`;
  `include/ymh/session/events.hpp`; `src/session/events.cpp`;
  `src/session/session.cpp`; `src/agent/agent_loop.cpp`;
  `src/session/session_persistence.cpp`; `src/llm/openai_adapter.cpp`.

## 16. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 1 | 2026-09-19 | Initial write. Pins the `01` provenance amendment that `36 §3.2`/§3.3 and `36-D7`/`36-D8` require: the `Message` fields, the four payload fields, the codecs, the `ContextInjected.role` default, the `deriveMessages()` projection, the snapshot-equivalence rule, invariants `37-I1`-`37-I10`, failure modes `37-F1`-`37-F9`, and decisions `37-D1`-`37-D10`. No implementation code. |
| Rev 2 | 2026-09-19 | Gate fix-pass. Pins the producer sites and value derivations (§5.4, 37-D13, 37-I11, 37-F10, `agent_loop.cpp` added to §1.3/§11); pins the structural default member initializers and makes the canonical codec default the structural default (§5, §5.1, 37-D11, 37-I12, 37-F11); pins the durable nested JSON schema and enum strings (§5.3, 37-D12, 37-F12); reconciles the snapshot-invalidation claim with the test-only sequence-only durable path (§7.3, OQ-7); fixes the `tool_name` citation (it is a `ContentBlock` field). Invariants now `37-I1`-`37-I12`, failure modes `37-F1`-`37-F12`, decisions `37-D1`-`37-D14`. |

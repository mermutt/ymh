# 29 — Event-Family Errata: The dsh New-Event Contract (spec-01 amendment)

```
Status: written · verified: — · reviewer: — · Rev 1 (Wave-0 Stage A1)
Component: 29 (errata) — amends `01-session.md` §4.3/§4.4/§4.5/§15.1 by
           reference. It owns the `26-dsh-alignment-part2.md` §5 Stage-A **A1**
           freeze: the `EventType`/wire-codec extension rule, the two-axis
           compatibility rule (§4.6), the live-only vs durable split, the
           consumer matrix (§4.3.9.2), and the `llm/request_header` event +
           codec. It does not edit `01-session.md` in place.
Depends on: `26-dsh-alignment.md` (verified) §2.1.7/§3; `26-dsh-alignment-part2.md`
            (verified Rev 7, GATE PASS) §4.2/§4.3.4/§4.3.9/§4.3.9.1/§4.3.9.2,
            §4.6, §5 Stage A; `01-session.md`; `27-session-locking-errata.md`
            §3.3/27-I2; the working tree at authoring time.
Scope: pin the shared new-event-family contract for the dsh migration so that
       Wave-1/Wave-2 code can add `llm/request_header`, `assistant/attempt`, and
       the `assistant/message` stream/replay payload without re-freezing the
       family. Design only — no code, no behavior change.
Supersedes: `26-dsh-alignment-part2.md` §5's reserved numbers 27–30. Actual
            allocation (lead, 2026-09-19): **27** = session locking, **28** =
            LLM service boundary, **29** = event family (this file), **30** =
            architecture cascade. The Wave-3+ specs named
            `27-system-prompt.md`, `28-output-retention.md`,
            `29-agent-presets.md`, `30-goals-jobs-commands.md` must therefore be
            **renumbered to 31+** when their errata are written.
Amends: `01-session.md` §4.3 (`EventType` set), §4.4 (`SessionEventMap`),
        §4.5 (payload definitions), §15.1 (round-trip test), and `01` S3/S14 —
        all by reference, not in place.
```

This document is a **pin**, not a proposal. Every count, `file:line`, and
behavior below was verified against the working tree before it was written. The
tree already carries 22 `EventType` values, 70 `case EventType::` labels across
five translation units, and a codec that is total over those 22 values. The
family is **non-exhaustive by design** (`01` §4.3): component specs extend it.
This errata freezes *how* an extension is made and *who* must react, for the two
Wave-1/Wave-2 additions only.

---

## 1. Purpose, numbering, scope

### 1.1 The problem

`26-dsh-alignment-part2.md` §4.3.9 adds ten new session events. The verified spec
records that adding one touches **two** layers — the codec and the consumers —
and that one consumer switch has **no `default:`**, so a missing case is a hard
`-Werror=switch` build break while the other four silently drop the event. The
dsh migration then splits the family across six waves, so without a single
family contract each wave would re-derive (and risk contradicting) the same
rules. The panel's #1 Wave-0 finding was to freeze this spine before any Wave-1
code.

### 1.2 What this pins

One document that (a) fixes the codec extension checklist and the concrete
`LlmRequestHeader`/`AssistantAttempt`/`AssistantMessage` additions, (b) states
the two-axis compatibility rule verbatim and anchors both axes in code, (c) gives
every new type an explicit obligation at every exhaustive `case EventType::`
site, (d) records the no-downgrade one-way door as a product sign-off item, and
(e) makes spec 27's `27-I2` re-entrancy restriction apply to every new
projection/consumer.

### 1.3 In scope / out of scope

**In scope (frozen here).** The Wave-1/Wave-2 event surface:

| Type | Kind | Wave | Source |
|---|---|---|---|
| `LlmRequestHeader` (`llm/request_header`) | **new** durable `EventType` | 1 | 26-D2/D3 |
| `AssistantAttempt` (`assistant/attempt`) | **new** durable `EventType` | 2 | 26-D9 |
| `AssistantMessage` (`assistant/message`) | **changed payload** (existing `EventType`) | 2 | 26-D9 |
| `AssistantChunk` (`assistant/chunk`) | **role change**: live-only publication, legacy durable rows read-only | 2 | 26-D9 |

**Out of scope (owned by their own wave's errata).** Every other type named in
26 §4.3.9: `LlmRetry`, `LlmRetryStarted`, `ContextPrune`,
`AgentPresetSelected`, `CommandRun`, `CommandDone`, `GoalChange`,
`JobChanged`. They are recorded as **reserved rows** in §4.4 and must be added
by the errata that owns their wave, using the §3.1 checklist. Later waves
**amend this errata by reference**; they do not re-freeze the family.

> **Open item (flagged, not resolved here).** `LlmRetry`/`LlmRetryStarted` are
> in 26 §4.3.9 and 26-D1 is Wave 1, but the Stage-A A1 scope (task, 2026-09-19)
> names only `LlmRequestHeader` + the Wave-2 assistant events. No Wave in 26 §5
> explicitly owns the retry events. The Wave-1 owning errata must either add
> them under §3.1 or state that retries stay unlogged. This errata does not
> silently claim them. See §8.

### 1.4 Numbering

This file **claims number 29**. It supersedes the number reservations in
`26-dsh-alignment-part2.md` §5 (which assumed 27–30 were free; 27 was already
taken by `27-session-locking-errata.md`). The Wave-3+ specs listed there are
renumbered to 31+ (header block, `Supersedes`).

---

## 2. Amendment register

| ID | Amendment | Add./Brk. | Anchors |
|---|---|---|---|
| **29-D1** | Add durable `EventType::LlmRequestHeader` / `llm/request_header` / `payload::LlmRequestHeader`, with `SessionEventMap` + `EventTraits` + JSON codec. | Add. to `01` | 26-D2/D3; 26 §4.3.9/§4.3.9.1 |
| **29-D2** | Add durable `EventType::AssistantAttempt` / `assistant/attempt` / `payload::AssistantAttempt`, with `SessionEventMap` + `EventTraits` + JSON codec. | Add. to `01` | 26-D9; 26 §4.3.9/§4.3.9.1 |
| **29-D3** | Extend the **existing** `payload::AssistantMessage` with `stream` and `replay_state` (additive, optional/defaulted JSON). The `EventType` value does not change. | Add. to `01` | 26-D9; 26 §4.3.4/§4.3.9.1 |
| **29-D4** | Make `AssistantChunk` a **live-only** publication. Keep the enum value, wire name, and payload codec so legacy durable rows stay decodable; **remove** `SessionEventMap<EventType::AssistantChunk>` (mirroring live-only `McpServerStatusChanged`); the only durable writer (`ChunkCoalescer::flush`) moves to `Session::emit`. | Brk. (`01` §16.1(c)) | 26-D9; `core/event.hpp:73-75`; `mcp_types.hpp:218`; `session.hpp:282-285` |
| **29-D5** | Pin the **two-axis** compatibility rule: durable decode is a loud forward-fence; wire decode skips an unknown event type and advances the cursor. | Add. to `01` S3 | 26-I12/26-D24; 26 §4.6 |
| **29-D6** | Pin the consumer matrix (§4) and the rule that the `01` errata may not ship a new type without filling its row. | Add. to `01` §15 | 26 §4.3.9.2 |
| **29-D7** | Record the **one-way door**: a DB containing a new type is unreadable by an older binary; downgrade is unsupported; acceptance is a product sign-off item. | Add. | 26 §4.6 |
| **29-D8** | Apply `27-I2` (no re-entry from an inline handler/projection/`AppendFn`) to every new event's projection and consumer, and to the `27-D8` audit artifact. | Add. to `01`/`27` | `27-session-locking-errata.md:503`, `27-F1` |

---

## 3. The new-event contract (pinned)

### 3.1 The codec extension checklist (six touch points)

Adding one durable event is a codec change, not a free extension. All six points
live in `include/ymh/core/event.hpp` / `include/ymh/session/events.hpp` /
`src/core/event.cpp` / `src/session/events.cpp`:

1. **`EventType` enum** (`core/event.hpp:51-76`). Append the new enumerator.
   The enum ordinal is **not** a persisted surface: the DB column is
   `events.type TEXT` and the JSON `type` is the slash string
   (`session_persistence.cpp:53-58`; `core/event.cpp:75-86`). Ordinal stability
   is therefore not a compatibility requirement; declaration order is only used
   by `all_event_types()` and its round-trip test.
2. **`kWireNames`** (`core/event.cpp:18-41`, currently 22 entries) — one
   `{type, "slash/name"}` entry. `wire_name()` (`:45-52`) and
   `parse_event_type()` (`:54-61`) are total over this array.
3. **`all_event_types()`** (`core/event.cpp:63-73`) — derived from `kWireNames`,
   so it extends automatically; the round-trip test
   (`tests/unit/event_test.cpp:50-63`) iterates it and asserts
   `parse_event_type(wire_name(t)) == t` for every value.
4. **`SessionEventMap<EventType::X>`** (`events.hpp:218-306`) — the type→payload
   map (`core/event.hpp:118-121`). Durable types only.
5. **`EventTraits<payload::X>`** (`events.hpp:308+`) — the payload→type map used
   by `encode()` (`core/event.hpp:145-153`). Live-only types also need this
   (the `McpServerStatusChanged` precedent, `mcp_types.hpp:218`), because live
   publication goes through `encode()`.
6. **Payload `to_json`/`from_json`** (`src/session/events.cpp`, ADL) — pinned by
   the §3.2 key schema. Conventions: `snake_case` keys; enums as lowercase
   strings; `optional` fields omitted when unset and read with a default; no
   ids/timestamps beyond the envelope's `id`/`timestamp`.

`parse_event_type()` rejects an unknown string (`S3`); that rejection is the
durable forward-fence (§3.3), not an invitation to relax.

### 3.2 The in-scope additions (frozen)

Wire names follow ymh's slash/underscore convention (`01` §4.3), deliberately
diverging from dsh's hyphenated names (26 §4.3.9).

| EventType | `wire_name` | Payload | `SessionEventMap` | `EventTraits` |
|---|---|---|---|---|
| `LlmRequestHeader` | `llm/request_header` | `payload::LlmRequestHeader` | **add** | **add** |
| `AssistantAttempt` | `assistant/attempt` | `payload::AssistantAttempt` | **add** | **add** |
| `AssistantMessage` | `assistant/message` (unchanged) | `payload::AssistantMessage` (extended) | unchanged | unchanged |
| `AssistantChunk` | `assistant/chunk` (unchanged) | `payload::AssistantChunk` (unchanged) | **remove** | keep (live publish) |

After the in-scope freeze `all_event_types()` returns **24** values
(22 today + 2 new). The Wave-2 `AssistantMessage`/`AssistantChunk` changes add
**no** enum value.

**`payload::LlmRequestHeader` JSON keys** (26 §4.3.9.1; `LlmCallConfig`,
`ProviderId`, `CallPurpose` are owned by `28-llm-service-boundary-errata.md`):
`turn` int, `step` int, `session_id` string, `purpose` string
(`"compaction"｜"session_title"`, omitted when unset), `config` object,
`system_prompt_digest` string, `system_prompt` string (**omitted unless
`session.persist_prompt_text`; default off, global layer only**), `tool_names`
string[], `tool_schema_digests` string[], `template_digest` string,
`starts_series` bool. Nested `LlmCallConfig`: `provider` string, `model` string,
`reasoning_effort` string (omitted), `temperature` number (omitted), `max_tokens`
int (omitted), `stop` string[], `top_p` number (omitted), `seed` int (omitted),
`tool_choice` string (omitted).

**`payload::AssistantAttempt` JSON keys:** `turn` int, `step` int, `stream`
`AssistantStreamRecord[]`.

**`payload::AssistantMessage` additions** (existing `id`, `content`, `usage`
omitted when unset, **plus**): `stream` `AssistantStreamRecord[]` and
`replay_state` object (omitted when none). `AssistantStreamRecord[]` entries are
discriminated by `type`: `{"type":"text_chunks","index":int,"time0_ms":int,
"dt_ms":int[],"texts":string[]}`; `{"type":"reasoning_chunks",…}`;
`{"type":"tool_call_chunks","index":int,"time0_ms":int,"dt_ms":int[],"id":…,
"name":string(omitted),"args":string[]}`;
`{"type":"chunk","time_ms":int,"event":<existing StreamEvent codec>}`. The
`type` strings use ymh's underscore convention.

### 3.3 The two-axis compatibility rule (verbatim from 26 §4.6)

> Compatibility is **two-axis**. The **durable/on-disk axis** is strict: an older
> binary must refuse a newer session DB loudly rather than misread it. The
> **wire axis** is tolerant: the socket is a live projection of the log, so a
> receiver skips an event type it does not know and stays connected.
> `kProtocolVersion` fences the wire **envelope** shape and handshake only; it is
> not an event-vocabulary counter (`26-I12`/`26-D24`).

**Axis A — durable / on-disk (strict, loud).**

- Unknown event **type**: `SessionPersistence::decode_event` calls
  `parse_event_type` and throws `CorruptionError("undecodable event type: …")`
  at `src/session/session_persistence.cpp:228-232`. An older binary refuses a
  newer DB at the first unknown row. `Event::from_json` independently throws
  `std::runtime_error{"unknown event type: …"}` at `src/core/event.cpp:88-107`
  (throw at `:98`).
- Unknown **keys** on a **known** event: degrade gracefully. The payload codecs
  read required keys with `json.at(...)` and optional keys with
  `json.value(..., default)`; an unrecognized key is simply never read, so an
  older reader accepts a newer row that only *adds* fields (26 §4.6). This is
  what makes 29-D3 (additive `AssistantMessage` fields) safe.
- A field that cannot be made optional is a **breaking** payload change and must
  be carried by a **new event type**, not added to an existing one.
- **No structural bump.** `kSchemaVersion` stays `1`
  (`include/ymh/session/session_persistence.hpp:68`). New events are rows in the
  existing `events(sequence, session_id, event_id, timestamp, type, payload)`
  table (`session_persistence.cpp:53-58`); `migrate_fresh`
  (`:569-580`) is unchanged. There is **no** `migrate_v1_to_v2` and no DDL
  delta. The version checks at `:677-682` are untouched.

**Axis B — wire (tolerant, skip).**

- Unknown event **type**: the receiver **skips** the `SessionEnvelope`, does not
  call `on_envelope`, logs it, and **still advances** the per-session cursor to
  the notification's `cursor` so a reconnect does not replay a poison pill. The
  pinned site is `SupervisorConnection::dispatch`
  (`src/ui/supervisor_connection.cpp:310-323`); the envelope decode is
  `protocol::SessionEnvelope::from_json` (`src/transport/protocol.cpp:266-268`)
  and `protocol::StreamNotification::from_json` (`:321-326`).
- **Current state (truthful):** the skip is **not yet implemented**. Today an
  unknown wire type throws from `Event::from_json` during the notification
  decode (`protocol.cpp:268`), i.e. the wire currently inherits the durable
  throw. This errata pins the required tolerant behavior; the Wave-1/Wave-2 code
  must make the wire decode tolerant while `decode_event`
  (`session_persistence.cpp:228-232`) stays loud. The two paths must **diverge**
  and must not share one throwing `Event` decode.
- `kProtocolVersion` stays `1` (`include/ymh/transport/protocol.hpp:153`). The
  handshake's exact-match check (`src/transport/protocol_server.cpp:285-293`)
  is therefore unchanged. This is what lets a new supervisor attach to an old
  daemon that outlives a non-last supervisor (`24` `AL23`/`AL25`); a bump would
  reject the attach with `UnsupportedProtocol`. A `kProtocolVersion` bump is
  reserved for an **envelope-shape** change (`SessionEnvelope = {session, event}`,
  `05` T5) or handshake semantics — never for a vocabulary addition (`26-I12`).

### 3.4 The one-way door (no downgrade)

Once a new binary appends a new event type, an older binary **cannot open that
DB**: its `decode_event` hits the unknown type and throws `CorruptionError`
(§3.3 Axis A). There is **no migration and no dual-read window**; the
alternative was considered and explicitly not proposed (26 §4.6). Downgrades are
unsupported.

This forward-incompatibility is a **product decision and a sign-off item** for
the Wave-0 freeze. It is not a bug and must not be "fixed" by weakening the
fence. The wire axis is unaffected: the same new type is *skipped* by an older
receiver, so it does not by itself break a live connection.

### 3.5 The `27-I2` re-entrancy restriction applies

`27-session-locking-errata.md:503` (`27-I2`) forbids any handler invoked by
`EventBus::publishCommitted` (global, session, or committed), and any injected
`ProjectionFn` or `AppendFn`, from re-entering
`PlanModeController`, `SessionManager` (from the `deleteSession` publish path),
or any of
`header`/`events`/`ownEvents`/`deriveMessages`/`snapshot`/`append`/`appendEvent`/
`appendAutoRename`/`appendBatch`. Violation ⇒ recursive self-deadlock on the
mutex the trigger class re-acquires (a silent hang; TSan cannot detect it). It
is **audit-only** (`27-D8`).

Any new event's projection (`deriveMessages`, `session.cpp:375-484`) and any new
consumer registered as an inline handler/projection falls under `27-I2`; the
`27` §7.1 audit artifact must be re-scanned for every new or moved subscriber,
projection, or `AppendFn` site. In particular, the new `LlmRequestHeader` and
`AssistantAttempt` projections are **pure reads of the event argument** — they
must not call any session read accessor or append.

---

## 4. Consumer matrix

### 4.1 Verified switch inventory (the 70-label surface)

Exactly **70** `case EventType::` labels exist across **five** translation
units / **seven** `switch (event.type)` statements. Counts verified by `grep -c`
on the working tree:

| Consumer | Switch(es) | Cases | `default:` | Failure if a new type is not handled |
|---|---|---|---|---|
| `src/session/session.cpp` (`deriveMessages`, `:375-484`) | 1 | **22** | **none** | **hard build break** (`-Werror=switch`) |
| `src/ui/ui_event_adapter.cpp` (`project_state :60-77`, `adapt :84-165`, `adapt_maintenance :205-255`) | 3 | **25** (9+11+5) | yes (3×) | **silent drop** of live/transcript rows or state |
| `src/ui/session_export.cpp` (`:200-274`) | 1 | **9** | yes | silent drop in export |
| `src/cli/session_cli.cpp` (`event_detail :91-143`) | 1 | **8** | yes | silent drop in `ymh show`/`replay` output |
| `src/cli/headless.cpp` (`:196-235`) | 1 | **6** | yes | silent drop in `ymh run` output |

22 + 25 + 9 + 8 + 6 = **70**. The projection switch has no `default:` and
therefore gates the build; the other four have a `default:` and therefore drop a
new type **silently**.

### 4.2 Per-new-type obligations

Every in-scope type gets an explicit obligation at every site. "Ignore" means an
**explicit** `case …: break;` in the no-`default:` projection switch and, in the
four `default:`-bearing switches, either an explicit no-op case or a
**recorded** fall-through to `default` — the choice is recorded here, never
implicit.

#### `LlmRequestHeader` (new, durable, metadata)

| Site | Obligation |
|---|---|
| `deriveMessages` (`session.cpp:375`) | **ignore** — explicit `case LlmRequestHeader: break;` (metadata; adds no `Message`) |
| `ui_event_adapter::project_state` (`:60`) | recorded fall-through to `default:` → state unchanged |
| `ui_event_adapter::adapt` (`:84`) | recorded fall-through to `default:` → no UI row, never rendered |
| `ui_event_adapter::adapt_maintenance` (`:205`) | recorded fall-through to `default:` |
| `session_export` (`:200`) | recorded fall-through to `default:` → no markdown row |
| `session_cli::event_detail` (`:91`) | recorded fall-through to `default:` → empty detail |
| `headless` (`:196`) | recorded fall-through to `default:` → no stdout/stderr output |
| compaction / token accounting | **excluded** — not model-visible content; never counted |

#### `AssistantAttempt` (new, durable, non-message attempt)

| Site | Obligation |
|---|---|
| `deriveMessages` (`session.cpp:375`) | **ignore** — explicit case; the attempt committed no message |
| `ui_event_adapter::project_state` (`:60`) | recorded fall-through to `default:` |
| `ui_event_adapter::adapt` (`:84`) | **ignore** (no row) in the required contract; a debug row is permitted only behind an explicit opt-in and is **not** required |
| `ui_event_adapter::adapt_maintenance` (`:205`) | recorded fall-through to `default:` |
| `session_export` (`:200`) | recorded fall-through to `default:` → no markdown row |
| `session_cli::event_detail` (`:91`) | recorded fall-through to `default:` |
| `headless` (`:196`) | recorded fall-through to `default:` |
| compaction / token accounting | **excluded** — no committed message, no tokens |

#### `AssistantMessage` (existing type, changed payload)

| Site | Obligation |
|---|---|
| `deriveMessages` (`session.cpp:423-436`) | **unchanged** — still projects the assistant `Message`; counts as assistant output |
| `ui_event_adapter::adapt` (`:105-110`) | existing case unchanged (durable finish row); live deltas continue to come from the **live-only** `AssistantChunk` |
| `ui_event_adapter::project_state` / `adapt_maintenance` | unchanged (already `default:`/existing) |
| `session_export` (`:213-218`) | existing case; renders `content` (unchanged). The embedded `stream` is **not** rendered as a row |
| `session_cli::event_detail` (`:106-109`) | existing case; `first_text(content)` unchanged |
| `headless` (`:196`) | **must add a durable `AssistantMessage` case** that prints the assembled `content` text. Today `headless` has **no** `AssistantMessage` case (`:197-232`); without it `ymh run` prints nothing and returns empty text on a resumed/replayed session where no live chunks are re-emitted (26 §5 Wave 2) |
| compaction / token accounting | **counted** as assistant output (existing behavior) |

#### `AssistantChunk` (existing type, live-only role change)

| Site | Obligation |
|---|---|
| `deriveMessages` (`session.cpp:421-422`) | existing **ignore** — explicit case (I14) |
| `ui_event_adapter::adapt` (`:90-104`) | **live delta feed (required)** — reads the live-only publication; `AssistantMessageStarted`/`AssistantTextDelta` stay |
| `session_export` (`:200`) / `session_cli` (`:100-105`) | fallback to **legacy durable** `assistant/chunk` rows when no embedded/compact record exists; new binaries never append new ones |
| `headless` (`:197-208`) | live delta path stays on the live-only event |
| compaction / token accounting | **excluded** — the durable stream lives in `assistant/message`; the chunk is not a durable message |
| **publisher** | `ChunkCoalescer::flush` (`src/agent/chunk_coalescer.cpp:52-71`) currently calls `session_.appendBatch(events)` (`:68`), i.e. it **durably appends** `AssistantChunk`. Wave 2 must switch it to `Session::emit` (`include/ymh/session/session.hpp:282-285`; `src/session/session.cpp:650-657`), the live-only destination. A test must assert no durable `AssistantChunk` is written by a new binary |
| **codec** | `SessionEventMap<EventType::AssistantChunk>` (`events.hpp:258-261`) is **removed** (live-only precedent: `McpServerStatusChanged` has `EventTraits` in `mcp_types.hpp:218` but no `SessionEventMap` entry). `EventTraits<payload::AssistantChunk>` is **kept** so the live `encode()` path works. The enum value, wire name, `parse_event_type`, `all_event_types`, and payload codec are **kept** so legacy durable rows stay decodable |

### 4.3 Matrix rules

1. **Build gate.** The projection switch (`session.cpp:375`) has no `default:`;
   every new **enumerator** — durable or live-only (the live-only
   `McpServerStatusChanged` already has one at `:380`) — gets an explicit case.
   Checked by `-Werror=switch`; `all_event_types()` round-trip tests (`01`
   §15.1) must cover every new type.
2. **No silent drops for user-visible events.** `AgentPresetSelected`,
   `CommandRun`/`CommandDone`, `GoalChange`, and `JobChanged` are user-visible;
   their waves must render them explicitly (or record why not). Internal
   metadata may fall through, but only with the recorded reason in this table.
3. **Token accounting.** No metadata event is counted toward the context/token
   estimate. The estimate is computed over the **projection** output
   (`src/agent/compactor.cpp:297`, `estimator_.estimate(compacted)`), so a
   projection-invisible event is naturally excluded. A projection case must
   never push a `Message` for a metadata event.
4. **Live vs durable.** `AssistantChunk` stays in the codec vocabulary (legacy
   durable rows remain decodable) but new binaries do not append it; it is
   published live-only. Consumers that need live deltas read the live event;
   consumers that need history read the durable `assistant/message`.
5. **`default:` is not a plan.** A recorded fall-through is allowed only because
   §4.2 records it; an unrecorded new type in a `default:`-bearing switch is a
   defect even though the build stays green.

### 4.4 Reserved rows for later waves (not frozen here)

These are recorded so the family is complete; each is owned by the errata that
gates its wave and must satisfy §3.1 + §4.3 before code emits it.

| EventType | `deriveMessages` | transcript / UI | token | Wave / owning errata |
|---|---|---|---|---|
| `LlmRetry` | ignore | optional retry notice (status only) | excluded | Wave 1 (see §8 open item) |
| `LlmRetryStarted` | ignore | no row | excluded | Wave 1 (see §8) |
| `ContextPrune` | ignore — the replacement `ToolResult` carries the text | prune notice row | replacement counted; prune event not | Wave 4 (`13`/`01` errata) |
| `AgentPresetSelected` | ignore | preset row | excluded | Wave 5 (`29-agent-presets` → renumber 31+) |
| `CommandRun` | ignore (log-only) | command row | excluded | Wave 6 (`30-goals-jobs-commands` → renumber) |
| `CommandDone` | ignore (log-only) | command row | excluded | Wave 6 |
| `GoalChange` | ignore | goal row | excluded | Wave 6 |
| `JobChanged` | ignore | job row | excluded | Wave 6 |

---

## 5. Invariants

- **29-I1 — Codec totality and round-trip.** For every `EventType` value,
  `wire_name(t)` is non-empty and `parse_event_type(wire_name(t)) == t`;
  `all_event_types()` enumerates every value. The test at
  `tests/unit/event_test.cpp:50-63` (and `plan_mode_test.cpp:74`) iterates
  `all_event_types()`, so adding a value extends the test automatically and a
  missing `kWireNames` entry fails it.
- **29-I2 — Build-gated projection.** Every `EventType` value (durable or
  live-only) has an explicit `case` in the no-`default:` projection switch
  (`session.cpp:375-484`).
- **29-I3 — Two axes never share a decode.** The durable decode stays loud
  (`CorruptionError`, `session_persistence.cpp:228-232`); the wire decode skips
  an unknown type. A single throwing `Event::from_json` must not serve both.
- **29-I4 — Additive payloads only.** New fields on a known event are optional
  or defaulted and unknown keys are ignored; a required new field is a new
  event type.
- **29-I5 — Metadata is not context.** No metadata event enters
  `deriveMessages` output or the token estimate.
- **29-I6 — Version neutrality.** A vocabulary addition changes neither
  `kProtocolVersion` (`protocol.hpp:153`) nor `kSchemaVersion`
  (`session_persistence.hpp:68`).
- **29-I7 — `AssistantChunk` is live-only.** New binaries never durably append
  it; `SessionEventMap<AssistantChunk>` is absent; `EventTraits` is kept for the
  live publish.
- **29-I8 — `27-I2`.** No new subscriber/projection/`AppendFn` re-enters the
  `27` §3.3 forbidden set; the `27` §7.1 audit is re-scanned.

---

## 6. Failure modes

| ID | Trigger | Consequence | Guard |
|---|---|---|---|
| **29-F1** | A new durable enum value ships without a projection case | **Hard build break** (`-Werror=switch`) at `session.cpp:375` — loud, cannot ship | 29-I2 |
| **29-F2** | A new user-visible type is omitted from a `default:`-bearing switch | **Silent drop**: no UI row / no export row / no `ymh run` text | §4.2 recorded obligations + per-type consumer tests |
| **29-F3** | An older receiver meets an unknown wire type before the skip is implemented | The throw from `Event::from_json` (`core/event.cpp:98`) propagates out of the notification decode; connection failure / poison-pill reconnect instead of a skip | §3.3 Axis B (Wave-1/2 implementation) + the wire-skip test |
| **29-F4** | An older binary opens a DB containing a new type | `CorruptionError` at the first unknown row — **intended**; the DB is refused, not misread | §3.4 one-way door; product sign-off |
| **29-F5** | A new consumer re-enters a session accessor from an inline handler | Recursive self-deadlock (silent hang; TSan cannot detect it) | 29-I8 / `27-I2` audit |
| **29-F6** | A new binary still durably appends `AssistantChunk` | Legacy-split durable rows the plan says are read-only; replay home split across two events | §4.2 publisher row + no-durable-chunk test |
| **29-F7** | A projection case pushes a `Message` for a metadata event | Inflated context/token estimate; phantom model-visible content | 29-I5 + projection-output test |

---

## 7. Test plan

Design-gate tests are named here; they land with the code in their wave.

1. **Codec round-trip (extended automatically).** `event_test.cpp:50-63` already
   iterates `all_event_types()`; the two new values must round-trip. Add explicit
   assertions that `all_event_types().size()` grew by exactly the number of new
   values, and that `wire_name(LlmRequestHeader) == "llm/request_header"` /
   `wire_name(AssistantAttempt) == "assistant/attempt"`.
2. **Durable loud fence.** A row with an unknown `type` makes `decode_event`
   throw `CorruptionError` (`session_persistence.cpp:228-232`). Extend
   `event_test.cpp:85`-style coverage if needed.
3. **Wire skip.** A `StreamNotification` carrying an unknown event `type` does
   **not** call `on_envelope`, does **not** throw, and advances the per-session
   cursor to the notification's `cursor` (`supervisor_connection.cpp:310-323`).
4. **Unknown keys degrade.** Decoding a known event with extra JSON keys
   succeeds and ignores them (`AssistantMessage` with an unknown key).
5. **Consumer coverage per in-scope type.** `LlmRequestHeader` and
   `AssistantAttempt` produce no projection message and no row in
   `session_export`/`session_cli`/`headless`; `AssistantMessage` yields a
   headless durable-text output (the Wave-2 regression test: `ymh run` text with
   **no** `AssistantChunk`); `AssistantChunk` is published live and **not**
   durably appended.
6. **Version neutrality.** Assert `kProtocolVersion == 1` and
   `kSchemaVersion == 1` are unchanged by the additions.
7. **`27-I2` audit.** The `27` §7.1 artifact lists every new subscriber /
   projection / `AppendFn` site and confirms no forbidden re-entry.

---

## 8. Explicit non-goals and the retry open item

- This errata does **not** implement anything; it is a Wave-0 design freeze.
- It does **not** renumber the Wave-3+ specs; it records that they must be
  renumbered to 31+ (header `Supersedes`).
- It does **not** decide the transport-decode mechanism for the wire skip
  (owned by the Wave-1/Wave-2 implementation under §3.3 Axis B); it pins only
  the required behavior and the "do not share the durable decode" constraint.
- It does **not** freeze `LlmRetry`/`LlmRetryStarted` (§1.3 open item). The
  Wave-1 owning errata must add them under §3.1 or explicitly state that retries
  stay unlogged; leaving them silently unowned is a defect this errata refuses
  to introduce.

---

## 9. Revision log

| Rev | Date | Change |
|---|---|---|
| Rev 1 | 2026-09-19 | Initial authoring (Wave-0 Stage A1). Pins 29-D1–29-D8: the six-point codec checklist; the in-scope `llm/request_header` and `assistant/attempt` event additions; the additive `assistant/message` `stream`/`replay_state` fields; the `AssistantChunk` live-only role change (`SessionEventMap` removed, `EventTraits` kept, `ChunkCoalescer::flush` → `Session::emit`); the verbatim two-axis compatibility rule (durable `CorruptionError` fence vs wire skip with cursor advance, `kProtocolVersion`/`kSchemaVersion` unchanged); the verified 70-label consumer matrix (session.cpp 22 no-default / ui_event_adapter 25 / session_export 9 / session_cli 8 / headless 6); the one-way no-downgrade door as a product sign-off item; and the `27-I2` re-entrancy restriction. Flags the `LlmRetry`/`LlmRetryStarted` ownership gap (§1.3/§8). Claims number 29 and supersedes 26 §5's 27–30 reservations (Wave-3+ specs renumber to 31+). Status **written** — pending independent review. |

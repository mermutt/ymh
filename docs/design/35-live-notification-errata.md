# 35 — Live-Notification Errata: The `event.live` Wire Notification (spec-05 amendment)

```
Status: written · verified: — · reviewer: — (tracked in DESIGN_STATUS.md)
Revision: Rev 1 — initial authoring. Pins the unpinned wire notification
          `event.live` (`LiveNotification` / `kEventLive` / `onLiveEvent` /
          `LiveEventForwarder`) that Wave 2 (commit `d979ec007`) added to carry
          the live-only `AssistantChunk` delta from the daemon to a remote
          supervisor. Closes NEW-M1 of the Wave-2 fix-pass re-check
          (`/tmp/opencode/review-wave2-recheck.md` §7).
Component: 35 (errata) — amends `05-transport.md` §5.1 :574-577 and §7.7
           :997-1004 **by reference**; it does not edit `05` in place. It
           reconciles with `11-m2-errata.md` (the frozen M2 interfaces),
           `29-event-family-errata.md` (29-D4 / 29-I7, the live-only
           `AssistantChunk`), and `26-dsh-alignment-part2.md` §5 Wave 2 (26-D9,
           the required live delta path). It does not amend `01`, `04`, `06`,
           `08`, `10`, or the assembler/replay contract (`34`).
Depends on: `05-transport.md` (verified) §5.1 :574-577, §5.2 :579-603, §5.4
            :639-665, §7.1 :748-752, §7.7 :997-1073, §8.1 :1079-1093, §8.2
            :1095-1108, §8.3 :1110-1125, §8.5 :1148-1166, T6/T7/T8/T11/T21/T23/T24
            :1232-1295, T-F5 :1335, T-F17 :1347;
            `11-m2-errata.md` (verified) §7 (frozen M2 interfaces), §8.2;
            `26-dsh-alignment-part2.md` (verified Rev 7) §4.3.9.2 :996, §4.6
            :1166-1235, 26-D9 :147, 26-D24 :162, 26-I12 :111-124, §5 Wave 2
            :1428-1451;
            `29-event-family-errata.md` (verified) 29-D4 :105, §4.2 :344-354,
            29-I7 :420-421;
            `34-assembler-replay-errata.md` (Rev 2) §15 item 1 :964-976;
            the working tree at authoring time: `include/ymh/transport/protocol.hpp`,
            `src/transport/protocol.cpp`, `include/ymh/transport/protocol_server.hpp`,
            `src/transport/protocol_server.cpp`, `include/ymh/transport/host_connection.hpp`,
            `src/transport/host_connection.cpp`, `src/transport/json_rpc.cpp`,
            `src/transport/transport_server.cpp`, `include/ymh/host/host_runtime.hpp`,
            `src/host/host_runtime.cpp`, `src/host/workspace_host.cpp`,
            `src/ui/supervisor_connection.cpp`, `src/cli/cli.cpp`,
            `include/ymh/cli/stream_receiver.hpp`, `src/core/event_bus.cpp`,
            `src/session/session.cpp`, `src/agent/chunk_coalescer.cpp`,
            `src/agent/agent_loop.cpp`, `tests/integration_host_harness_test.cpp`,
            `tests/unit/host_runtime_test.cpp`, `tests/unit/headless_test.cpp`
            (all `file:line` re-derived at authoring time).
Scope: pin, for the Wave-2 live half, the one wire notification the fix added but
       the verified specs never defined. Design only — no code, no behavior
       change, no version bump. Every name, key, direction, predicate, ordering
       rule, and compatibility claim below is derived from the shipped C++ types
       and the existing transport conventions; nothing is invented.
Supersedes: nothing. Claims number **35** (see the numbering note below).
Amends: `05-transport.md` §5.1 :574-577 and §7.7 :997-1004 — the live-signal
        enumeration and the notification catalog are corrected **by reference**
        to §3 below; §4 restates the exact required edits.
```

> **Numbering note (authoritative).** This document **claims number 35**. The
> Wave-0/Wave-1 allocations are `27` = session locking, `28` = LLM service
> boundary, `29` = event family, `30` = architecture cascade, `31` = agent-loop,
> `32` = compaction, `33` = stream-event codec, `34` = assembler & replay. `33`
> §"Numbering note" already recorded that the Wave-3+ names `26` §5 reserved
> (`system-prompt`, `output-retention`, `agent-presets`, `goals-jobs-commands`)
> were renumbered to **35+**; this errata takes **35**, so those reserved names
> shift one further, to **36+**. This errata does not reserve a number for them.

> **Status of the tree (Rev 1 assessment — historical; the `05` denial below
> was corrected in Rev 2, see §11).** The notification is **implemented and
> green**; what was missing is the design pin. `LiveNotification`
> (`include/ymh/transport/protocol.hpp:345-347`), its codec
> (`src/transport/protocol.cpp:339-345`), `notify::kEventLive = "event.live"`
> (`protocol.hpp:549`), `ProtocolServer::onLiveEvent`
> (`include/ymh/transport/protocol_server.hpp:69`;
> `src/transport/protocol_server.cpp:801-823`), and the daemon-side forwarder
> (`HostRuntime::LiveEventForwarder`, `include/ymh/host/host_runtime.hpp:85,242`;
> `src/host/workspace_host.cpp:771-779`) all exist. **No verified design doc
> mentions `event.live`, `kEventLive`, or `LiveNotification`** (`grep -rn` over
> `docs/` returns nothing), and `05` §5.1 :574-577 explicitly denies a fourth
> live signal while §7.7 :997-1004 lists only `event.stream` /
> `event.unsubscribed`. There is **no** dedicated codec unit test and **no**
> wire-name golden test for `event.live` (§8 records the gap). The consumer
> double-print defect in the in-process `ymh run` path is **out of scope here**
> and is being handled separately (Wave-2 re-check OPEN-MEDIUM M3).

---

## 1. Purpose, numbering, scope

### 1.1 The gap this closes

Wave 2 (commit `d979ec007`) fixed the critical HIGH of both prior reviews: the
live-only `AssistantChunk` (29-D4) never reached a remote supervisor, because it
is published with `Session::emit` (live bus only) and never committed, so the
`event.stream` subscription could never carry it. The fix introduced a **second
wire channel** for live-only events: the `event.live` notification. It works
end-to-end — the re-check independently verified the path and its integration
test — but the verified transport spec `05` was never amended. `05` §5.1
:574-577 states that the only live signals on the wire are `event.stream`,
`permission.request`, and `host.event`, and §7.7 :997-1004 pins the notification
catalog as `event.stream` / `event.unsubscribed`. `event.live` is therefore a
**fourth, unlisted** server→client notification carrying a `SessionEnvelope` —
an unpinned extension of a frozen interface, i.e. a design-first gate violation
(the re-check's NEW-M1).

This errata closes the gap by pinning the shipped surface. It does not change
behaviour and does not re-open the H1 fix: the re-check found **no functional
defect** in the live path, only the missing contract. The pin makes the live
channel auditable and gives the `05` amendments a normative home.

### 1.2 What this pins

1. The notification's **wire name**, **JSON-RPC framing**, **direction**, and
   **payload shape** (§3.2/§3.3), derived from the shipped DTO and codec.
2. Its **audience predicate** — which connections receive it (§3.4).
3. Its **ordering** relative to `event.stream` and to itself (§3.5).
4. Its **cursor/resume/replay** semantics — explicitly **none** (§3.6).
5. Its **reconnect/loss** and **backpressure** behaviour (§3.7/§3.8).
6. Its **version/forward-compatibility** status (§3.9) and the **producer scope
   today** (§3.10).
7. The **decision** to keep it a distinct notification rather than route live
   deltas through `event.stream`, with the alternative analysed and rejected
   (§3.1, 35-D1).
8. The **required `05` edits** (§4, 35-D11), quoted current-vs-required.

### 1.3 In scope / out of scope

**In scope.** The `event.live` wire surface, its relationship to `event.stream`,
and the two `05` sentences that currently contradict it.

**Out of scope (named, not dropped).**

- **The `AssistantChunk` live-only role itself** — owned by `29` (29-D4, 29-I7)
  and already verified; cited, not restated.
- **The `assistant/message` settlement and the consumer de-duplication** — owned
  by `34` §6/§7 and `34` §15 item 1; the cross-attempt double-print in
  in-process `ymh run` is Wave-2 re-check OPEN-MEDIUM M3, tracked separately.
- **The assembler/replay contract** — `34`.
- **`McpServerStatusChanged` forwarding** — it is handled inline by
  `HostRuntime::startForwarding` (`host_runtime.cpp:305-311`) and is **not**
  carried by `event.live` today (§3.10). Whether MCP status should also use the
  live channel is not pinned here.
- **Per-subscription live delivery** — `event.live` is per-session (§3.4); a
  future need for per-subscription live routing would be a new errata.

---

## 2. Verified tree facts (the basis for §3)

### 2.1 The shipped surface (types, constants, call sites)

| Fact | Location |
|---|---|
| `struct LiveNotification { SessionEnvelope envelope; };` | `include/ymh/transport/protocol.hpp:345-347` |
| codec declarations | `include/ymh/transport/protocol.hpp:450-451` |
| `to_json` / `from_json` — exactly `{"envelope": <SessionEnvelope>}` | `src/transport/protocol.cpp:339-345` |
| `inline constexpr std::string_view kEventLive = "event.live";` | `include/ymh/transport/protocol.hpp:549` |
| notification namespace (`kEventStream`, `kEventLive`, `kEventUnsubscribed`, `kPermissionRequest`, `kHostEvent`) | `include/ymh/transport/protocol.hpp:547-553` |
| `ProtocolServer::onLiveEvent(const Event&)` declaration | `include/ymh/transport/protocol_server.hpp:69` |
| `ProtocolServer::onLiveEvent` fan-out body | `src/transport/protocol_server.cpp:801-823` |
| `HostRuntime::LiveEventForwarder = std::function<void(const Event&)>` | `include/ymh/host/host_runtime.hpp:85` |
| the immutable `live_forwarder_` member, written once in the ctor | `include/ymh/host/host_runtime.hpp:242`; `src/host/host_runtime.cpp:242-269` |
| the live branch in `HostRuntime::startForwarding` | `src/host/host_runtime.cpp:305-320` (branch `:312-319`) |
| the single-threaded test fallback `server_->onLiveEvent(event)` | `src/host/host_runtime.cpp:318` |
| daemon wiring: `forwardLiveEvent` → `transport_->post` → `protocol_->onLiveEvent` | `src/host/workspace_host.cpp:544-547`, `:771-779` |
| supervisor dispatch of `kEventLive` → `sink_.on_envelope` | `src/ui/supervisor_connection.cpp:338-348` |
| CLI daemon path: `kEventLive` → `handle_live_notification` | `src/cli/cli.cpp:617-620`; `src/cli/cli.cpp:109-116`; declaration `include/ymh/cli/stream_receiver.hpp:41-42` |

`SessionEnvelope` is the same wire DTO as `event.stream`
(`protocol.hpp:225-232`; `05` §5.2 :579-603): `{session, event}`, no `Sequence`
(T5, `05` :1229-1230). The decode is the tolerant `29-D5` codec
(`protocol.cpp:266-280`): an unknown event `type` sets `event_skipped = true`
and leaves `event` default-constructed; a known type with a malformed payload
still throws.

### 2.2 The shipped fan-out and payload

`ProtocolServer::onLiveEvent` (`protocol_server.cpp:801-823`) iterates every
connection, skips dropped connections and connections that have not finished
`host.hello`, and sends the notification if **any** of that connection's
subscriptions has `session == event.session_id` (`std::any_of`, `:808-815`).
The payload is `to_json_value(LiveNotification{SessionEnvelope{session, event,
false}})` (`:816-818`). Two consequences follow directly from the code:

- **Per (connection, session), not per subscription.** The predicate is a
  boolean over the connection's subscriptions, so a connection holding two
  subscriptions to the same session receives **one** `event.live` per published
  live event (whereas `onEventCommitted` sends one `event.stream` **per
  subscription**, `:782-799`).
- **No `SubscriptionId`, no `replay`, no `cursor`.** The payload is exactly
  `{envelope}` — the notification carries strictly less than `event.stream`.

`HostRuntime::startForwarding` (`host_runtime.cpp:294-323`) subscribes a live
bus handler and forwards **only** `EventType::AssistantChunk` through the live
channel (`:312-319`); `McpServerStatusChanged` is consumed inline (`:305-311`).
When no `live_forwarder_` was injected (the unit-test bridge), it calls
`server_->onLiveEvent` directly (`:318`). In the daemon, the forwarder is
`WorkspaceHost::Impl::forwardLiveEvent`, which posts to the transport io thread
only while `transport_->running()` (`workspace_host.cpp:771-779`).

### 2.3 The contradiction with verified `05`

`05-transport.md:574-577` currently reads:

> Live events (§8.1) are **not** durable and therefore are **not** carried as
> `Event` frames. The only live signals on the wire are the transport-level
> notifications this spec defines (`event.stream`, `permission.request`,
> `host.event`), none of which is a `UiEvent`.

`05-transport.md:997-1004` currently reads:

```text
client -> server   event.subscribe    params: { session, from? }   result: { subscription, cursor }
client -> server   event.unsubscribe  params: { subscription }     result: {}
server -> client   event.stream       notification { subscription, replay, envelope, cursor }
server -> client   event.unsubscribed notification { subscription, reason }
```

Both predate the Wave-2 live channel. `event.live` is neither listed in §5.1's
"only live signals" nor in §7.7's catalog, and §5.1's literal claim ("not carried
as `Event` frames") is imprecise: `event.live` **does** carry a core `Event`
value inside a `SessionEnvelope`; what it is not carried on is the **durable
`event.stream` subscription**. §4 pins the required corrections.

---

## 3. The pinned `event.live` notification (frozen)

### 3.1 Decision 35-D1 — a distinct notification, not `event.stream`

The re-check offered two resolutions: pin the shipped `event.live`, or route the
live deltas through `event.stream` with a documented flag. This errata chooses
the **distinct notification** (the shipped design) and rejects the `event.stream`
route. The decision is not merely "keep what shipped": the `event.stream` route
is a worse design against `05`'s own invariants.

**Alternative A — `event.stream` with a live discriminator (rejected).** Reusing
`StreamNotification` for a live-only event requires either (a) fabricating a
`cursor` for an event that has no durable position, or (b) making `cursor`
optional and adding a `live` flag. Both are defects:

1. **T21 forbids it.** "Every stream notification carries a post-event cursor."
   A live-only event has **no** `cursor(E)` — it is never committed, so there is
   no position after it. Emitting a token anyway would hand the client a resume
   point that cannot resolve; a later `event.subscribe{from: cursor}` would be
   `CursorInvalid` (T16) at best, and a fabricated valid-looking token at worst.
2. **T7/T8 become ambiguous.** The durable stream promises "durable-before-
   observable" and "no loss, no duplicate across reconnect". A live chunk cannot
   satisfy either: it is observable without durability, and it is lost on
   disconnect. Mixing it into the same notification makes the channel's contract
   conditional on the event type, which is exactly what the live/durable split
   exists to avoid.
3. **Identity mismatch.** `event.stream` is keyed by `SubscriptionId` and
   carries `replay`; the live signal is a per-session broadcast that is never
   replayed. Reusing the subscription-keyed DTO would force a meaningless
   `SubscriptionId` onto the live signal and invite a consumer to treat it as
   resumable.
4. **Envelope-shape churn.** `StreamNotification` is pinned at `05` §7.7
   :1024-1029. Adding a discriminator/nullable cursor is a change to a frozen,
   verified wire DTO, which under 26-I12/26-D24 is the class of change that
   **does** warrant a `kProtocolVersion` bump — the opposite of the desired
   additive outcome.

**Alternative B — a distinct `event.live` notification (chosen).** It carries
strictly less than `event.stream` (`{envelope}` only), so a receiver cannot
mistake it for a resumable position; it makes "no cursor / no replay / no
subscription identity" explicit in the type system and on the wire; and it
mirrors the bus's own live/durable split (`Session::emit` vs
`Session::append`, `session.cpp:653-661` vs `:632-650`) and the naming family
`event.stream` / `event.live` / `event.unsubscribed` (`05` §7.1 :748-752,
`namespace.verb`).

**Accepted cost (recorded, not hidden).** Because the payload omits
`SubscriptionId`, a consumer cannot correlate a live notification to a specific
subscription — only to a session. This is accepted: the live signal is
per-session by construction (§3.4), the shipped consumers (`UiEventAdapter` via
`SupervisorConnection`, the headless stream receiver) key by session, and a
per-subscription live routing would be a new errata, not a silent extension of
this one.

### 3.2 Wire name, framing, direction (35-D2)

- **Method name:** `"event.live"` (`notify::kEventLive`,
  `protocol.hpp:549`). It conforms to the `namespace.verb` rule (`05` §7.1
  :750-752); the `event.` namespace is the subscription subsystem's.
- **Framing:** a JSON-RPC 2.0 **notification** — `{"jsonrpc":"2.0",
  "method":"event.live","params":{...}}`, no `id` (`json_rpc.cpp:116-120`;
  `05` §5.4 :647-650). It is framed by the same length-prefixed Unix-socket
  framing as every other message (`05` §3); it adds no framing rule.
- **Direction:** **server → client only** (daemon → supervisor/automation). It
  is not a request and has no response. `event.live` is **not** in the request
  catalog `kMethodCatalog` (`protocol.cpp:633-645`, 34 entries), so a client
  request naming it is `MethodNotFound` (T-F5, `05` :1335). A client must not
  send it; the daemon never expects it.
- **Profile:** delivered to both `Interactive` and `Automation` connections.
  `onLiveEvent` gates only on `hello_done` and subscription (`protocol_server.cpp
  :803-815`), unlike `host.event`, which skips non-`Interactive` clients
  (`onSessionCreated`, `:825-834`).

### 3.3 Payload shape (35-D3)

Exactly the shipped `LiveNotification` (`protocol.hpp:345-347`), JSON key set
`{envelope}` (`protocol.cpp:339-345`):

```cpp
namespace ymh::protocol {

// 05 §5.2 SessionEnvelope; no Sequence (T5). Live-only: no subscription,
// no replay, no cursor.
struct LiveNotification {
    SessionEnvelope envelope;
};

} // namespace ymh::protocol
```

```json
{
  "jsonrpc": "2.0",
  "method": "event.live",
  "params": {
    "envelope": {
      "session": "1a7b...uuid",
      "event": { "id": "9f2c...uuid", "session_id": "1a7b...uuid",
                 "timestamp": 1789430822832, "type": "assistant/chunk",
                 "payload": { "message": "m-1", "index": 3, "text": "hi",
                              "kind": "Text" } }
    }
  }
}
```

Rules:

- `params` has exactly one key, `envelope`; `envelope` has exactly `session`
  and `event` (T5). There is **no** `subscription`, `replay`, or `cursor` key.
- The embedded `event` is the same core `Event` JSON form as in `event.stream`
  (`05` §5.1 :566-572): slash `type` via `wire_name()`, `payload` the typed
  struct's JSON.
- The event type carried today is one whose **publication** is live-only
  (29-D4/29-I7): it is never appended to the store, so it has no store
  `Sequence`, and no `Sequence` appears anywhere in this DTO (T5).
- Decode uses the tolerant `SessionEnvelope` codec (`protocol.cpp:266-280`): a
  known `type` decodes (malformed payload still throws — the durable decode stays
  loud); an unknown `type` yields `event_skipped = true` and the receiver skips
  the envelope. Both the supervisor (`supervisor_connection.cpp:341-343`) and the
  CLI (`cli.cpp:112-114`) implement that skip.

### 3.4 Audience predicate (35-D4)

For a live `Event` `E` with session `S`, the daemon sends exactly one
`event.live` to a connection `C` iff:

```text
C is not dropped
AND C has completed host.hello
AND C holds at least one subscription whose session == S
```

It is **not** gated by `SubscriptionId`, by `replay` phase, by the
subscription's `from` kind, by profile, or by whether the subscription is
mid-replay. A connection with `N` subscriptions to `S` receives **one**
notification, not `N` (the `std::any_of` boolean,
`protocol_server.cpp:808-815`). A connection with **no** subscription to `S`
receives nothing — live delivery begins only after `event.subscribe` completes,
and prior live events are never delivered retroactively (§3.7).

This is a deliberately weaker addressing rule than `event.stream` (which is
per-`SubscriptionId`): the live signal is a per-session broadcast to interested
observers.

### 3.5 Ordering (35-D5)

- **Within `event.live` for a session:** delivered in the order the daemon's
  live bus published the events. `ProtocolServer::onLiveEvent` enqueues to each
  connection's single outbound queue in iteration order; the queue is FIFO.
- **Across `event.live` and `event.stream` for a session:** the relative wire
  order follows the producer's bus-publish order. Both the live handler
  (`bus().subscribe`, `host_runtime.cpp:305`) and the committed handler
  (`bus().subscribeCommitted`, `:321`) are invoked **synchronously** on the
  publishing thread (`event_bus.cpp:199-238`, `:240-288`), and both post to the
  transport io thread through the same FIFO `post` (`transport_server.cpp
  :193-200`, `asio::post` under `post_mutex_`). So if the producer publishes
  live event `L` before committed record `R`, `event.live(L)` is enqueued before
  `event.stream(R)`.
- **The load-bearing instance of that rule.** For an attempt, the live
  `assistant/chunk` deltas precede the durable `assistant/message` (or
  `assistant/attempt`) that settles the attempt: `coalescer.flush()` emits the
  chunks live (`agent_loop.cpp:899`; `chunk_coalescer.cpp:52-68`, emit at `:63`)
  **before** `session_.append(assistant)` commits the settlement
  (`agent_loop.cpp:905-920`, append at `:912`). A consumer may therefore append
  deltas and then settle on the durable message.
- **Cross-session interleaving is unconstrained** (T6, `05` :1232-1234); global
  cross-session ordering is not guaranteed.
- **Honest limit.** This is a *delivery-order* guarantee, not a causal barrier:
  the transport does not hold `event.stream` back waiting for live posts. It
  holds for the shipped producer because the live emit and the settlement append
  happen on one thread in that order. A future producer that publishes a live
  event **after** committing the corresponding durable record would break the
  "deltas before settlement" expectation; such a producer must be pinned by its
  own errata. This errata records the rule for the shipped `AssistantChunk`
  producer only.

### 3.6 Cursor, resume, and replay (35-D6)

- `event.live` carries **no cursor** and a receiver **MUST NOT** advance its
  per-session resume cursor from it. The shipped supervisor obeys this: the
  `kEventLive` branch (`supervisor_connection.cpp:338-348`) does not touch
  `cursors_`, unlike the `kEventStream` branch (`:313-336`, which stores
  `stream.cursor`).
- Live events are **not** part of the durable subscription and are **never**
  replayed. `event.subscribe{from: beginning}` and `from: cursor(c)` stream
  committed events only (`05` §7.7 :1040-1052); a live event is not in the store
  (29-D4/29-I7), so no replay can reproduce it. This is the substantive reason
  the `event.stream` route is rejected (§3.1).
- The durable settlement (`assistant/message`, 34 §6.1) is the recovery path for
  a missed live chunk. The live channel is an optimization for latency, never the
  source of truth.

### 3.7 Reconnect and loss (35-D7)

- A live event published while a client is disconnected is **lost** and is not
  recoverable from the live channel. This does not violate T8, which is scoped to
  committed events (`05` :1239-1240).
- On reconnect (supersede, T12), the client re-subscribes and resumes committed
  events from its cursor; it then receives subsequent live events. There is no
  live backfill and no "resume from live cursor".
- Live events published before the subscription completes are not delivered
  (§3.4); this is the live-channel analogue of T17 (no implicit replay).

### 3.8 Backpressure (35-D8)

`event.live` is enqueued on the **same** bounded per-connection outbound queue as
every other message and counts against `max_outbound_bytes` (`05` §8.3
:1110-1125; T11 :1248-1249). There is no separate live queue and no preferential
drop. On overflow the **client** is dropped, never the session; the client
reconnects and resumes durable events via cursor (live deltas lost, §3.7).

### 3.9 Version and forward compatibility (35-D9)

- **No `kProtocolVersion` bump.** It stays `1` (`protocol.hpp:153`). `event.live`
  is an additive notification method, not a `SessionEnvelope`-shape or handshake
  change, which is the class that reserves a bump (26-I12 :111-124; 26-D24 :162;
  `05` §4.1).
- **Forward compatibility.** A receiver that does not know `event.live` ignores
  the method: `SupervisorConnection::dispatch` is a chain of `if`s with no
  `else` (`supervisor_connection.cpp:312-362`), and the CLI loop explicitly
  `continue`s on an unrecognised method (`cli.cpp:621-622`). `parse_message`
  accepts any method string as a `Notification` (`json_rpc.cpp:155-177`). So a
  new daemon emitting `event.live` cannot break an old supervisor's connection;
  the old supervisor simply renders without live deltas and settles on the
  durable `assistant/message`.
- **Nested unknown event type.** Because the payload embeds a `SessionEnvelope`,
  the `29-D5` tolerant-skip rule applies: if a future live-only event `type` is
  unknown to the receiver, `event_skipped = true` and the envelope is skipped
  without erroring the connection.

### 3.10 Producer scope today (35-D10)

The only live event the daemon forwards today is `AssistantChunk`
(`host_runtime.cpp:312-319`). `McpServerStatusChanged` is handled inline and is
**not** sent as `event.live`. The notification shape is generic enough to carry
any future live-only event, but this errata pins only the shipped producer;
a consumer must skip unknown event types (§3.9) rather than assume
`assistant/chunk`.

---

## 4. Required `05` amendments (35-D11)

These are the only normative edits this errata requests to `05`. They are stated
here **by reference**; this errata does not edit `05` in place.

### 4.1 `05` §5.1 :574-577 — the live-signal enumeration

**Current (verbatim):**

> Live events (§8.1) are **not** durable and therefore are **not** carried as
> `Event` frames. The only live signals on the wire are the transport-level
> notifications this spec defines (`event.stream`, `permission.request`,
> `host.event`), none of which is a `UiEvent`.

**Required (replacement):**

> Live events (§8.1) are **not** durable and are therefore **not** carried on the
> durable `event.stream` subscription: they have no cursor, no replay, and no
> `SubscriptionId`, and a client must never advance its resume cursor from them
> (§7.7, T21). The server→client transport-level notifications this spec defines
> are `event.stream`, `event.live`, `event.unsubscribed`, `permission.request`,
> and `host.event`. `event.live` is the one that carries a live-only core `Event`
> inside a `SessionEnvelope`; the others are control notifications. None of them
> is a `UiEvent`.

Rationale: the current sentence denies a fourth live signal and conflates "live
signal" with "notification"; the replacement enumerates the full notification
set, scopes the durable-only claim to `event.stream`, and names `event.live`'s
unique payload. The `event.unsubscribed` omission is also closed here (§4.3).

### 4.2 `05` §7.7 :997-1004 — the notification catalog

**Current (verbatim):**

```text
client -> server   event.subscribe    params: { session, from? }   result: { subscription, cursor }
client -> server   event.unsubscribe  params: { subscription }     result: {}
server -> client   event.stream       notification { subscription, replay, envelope, cursor }
server -> client   event.unsubscribed notification { subscription, reason }
```

**Required (add one line to the catalog):**

```text
server -> client   event.live         notification { envelope }
```

**Required (add the DTO to the §7.7 code block, after `StreamNotification`):**

```cpp
// 29-D4/29-I7: a live-only session event. No subscription, no replay, no
// cursor — a receiver must not advance its resume cursor from it. Delivered
// to every connection subscribed to `envelope.session` (per session, not per
// subscription).
struct LiveNotification {
    SessionEnvelope envelope;
};
```

**Required (one semantics bullet in the §7.7 list):**

> `event.live` (35-D2–35-D10) carries a live-only event (today
> `assistant/chunk`) that is never committed, so it is not replayed and carries
> no `cursor`; it is delivered once per subscribed session regardless of
> `SubscriptionId`, and a receiver must not use it to advance its resume cursor.
> See `35-live-notification-errata.md` §3.

### 4.3 The `event.unsubscribed` omission (recorded)

`05` §5.1's "only live signals" list omits `event.unsubscribed`, which §7.7
:1003 does define. The §4.1 replacement enumerates it. This is a pre-existing
LOW, recorded here because the same sentence is being amended.

---

## 5. Reconciliation with `26` / `29` / `34` (35-D12)

- **`26` §5 Wave 2 (26-D9, :1435-1451)** requires that live deltas survive and
  that consumers read `assistant/message` for durable text. It pins the
  **behaviour** but not the wire mechanism. This errata supplies the missing
  mechanism and does not alter the mandate. The consumer migration itself
  (including the cross-attempt de-duplication, `34` §15 item 1) is unchanged and
  out of scope.
- **`29` 29-D4 / 29-I7 (:105, :420-421)** make `AssistantChunk` live-only. This
  errata is the transport consequence: a live-only event must cross the daemon
  boundary on a channel that carries no cursor, which is `event.live`.
- **`26-I12` / `26-D24` (:111-124, :162)** decouple the wire envelope version
  from the event vocabulary. `event.live` extends the **notification** vocabulary
  additively and does not touch `SessionEnvelope` or the handshake, so the
  version stays `1` (35-D9). The `29-D5` tolerant-skip rule continues to apply to
  the embedded `event.type`.
- **`34` §15 item 1 (:964-976)** records the consumer de-duplication and the
  cross-attempt live case as Wave-2 implementation risks. This errata confirms
  the transport half (ordering, §3.5) but does not resolve the consumer half;
  OPEN-MEDIUM M3 remains tracked separately.
- **`11-m2-errata.md`** froze the M2 interfaces before Wave 2; `event.live` is a
  post-M2 addition, so `11` needs no edit. This errata is the record that the
  M2 notification surface gained one member.

---

## 6. Invariants

- **35-I1 — Method identity.** The notification method is exactly `"event.live"`;
  it is never emitted under another name, and no other notification reuses it.
- **35-I2 — Payload exactness.** `params` is exactly `{envelope}` and `envelope`
  is exactly `{session, event}`; no `subscription`, `replay`, or `cursor` key is
  ever emitted (T5; §3.3).
- **35-I3 — Server→client only.** The daemon never treats `event.live` as a
  request; the client never sends it (§3.2).
- **35-I4 — Audience predicate.** One `event.live` per (connection, session) per
  published live event, iff the connection is hello-complete, not dropped, and
  subscribed to that session (§3.4).
- **35-I5 — No cursor.** A receiver never advances a resume cursor from
  `event.live`; the daemon never emits one (§3.6).
- **35-I6 — No replay.** `event.live` is never replayed by `beginning` or
  `cursor` subscriptions; a missed live event is recovered from the durable
  settlement, not the live channel (§3.6/§3.7).
- **35-I7 — Ordering.** Within a session, `event.live` follows bus-publish order,
  and live/committed wire order follows producer publish order; the shipped
  `AssistantChunk` producer emits deltas before its settling `assistant/message`
  (§3.5).
- **35-I8 — Version neutrality and tolerant decode.** No `kProtocolVersion` bump;
  an unknown `event.live` method is ignored by an old receiver, and an unknown
  embedded event `type` is skipped via `event_skipped` (§3.9).
- **35-I9 — Bounded queue.** `event.live` uses the same bounded outbound queue
  and drop-the-client rule as every other message (T11; §3.8).

---

## 7. Failure modes

| ID | Trigger | Consequence | Guard |
|---|---|---|---|
| **35-F1** | A second live notification name is invented (e.g. `event.delta`) | Two encoders disagree; a receiver silently drops one channel | 35-I1 + the §8 golden-name test |
| **35-F2** | A `cursor`/`subscription` key is added to `LiveNotification` | A receiver resumes from a non-durable position; T8/T21 violated | 35-I2/35-I5 + the §8 key-set test |
| **35-F3** | A consumer advances its cursor from `event.live` | Reconnect resumes from a position that does not exist → `CursorInvalid` or a gap/dup | 35-I5; the shipped supervisor already obeys (`supervisor_connection.cpp:338-348`) |
| **35-F4** | A consumer expects live deltas to be replayed | `from=beginning` produces no deltas; text appears missing until settlement | 35-I6; the durable `assistant/message` is the recovery path (34 §6.1) |
| **35-F5** | A producer publishes a live event after committing its settlement record | The consumer sees settlement before deltas; ordering expectation breaks | 35-I7's honest limit (§3.5); a new producer needs its own errata |
| **35-F6** | The daemon emits `event.live` to an unsubscribed connection | Live deltas leak to a client that never asked for the session | 35-I4 + the §8 fan-out test |
| **35-F7** | An old supervisor errors on the unknown `event.live` method | Connection drops on every turn with live output | 35-I8; `dispatch` falls through, CLI `continue`s (§3.9) |

---

## 8. Test plan additions (extending `05` §13 and the Wave-2 tests)

The tree already has: the end-to-end live assertion in
`tests/integration_host_harness_test.cpp:245-263` (a live-only `AssistantChunk`
reaches `sink.on_envelope`), the committed-path ordering/drop test in
`tests/unit/host_runtime_test.cpp:1029-1043`, and the CLI live-delta suppression
test `StreamReceiver.LiveChunkSuppressesDurableDuplicateText`
(`tests/unit/headless_test.cpp`). **Missing (required here):**

1. **Wire-name golden.** Encode a `LiveNotification` and assert
   `json["method"] == "event.live"`, `params` has exactly the key `envelope`, and
   `envelope` has exactly `session` and `event` — and no `subscription`/`replay`/
   `cursor` (35-I1/35-I2).
2. **Round-trip.** `to_json` → `from_json` preserves `session` and `event`; an
   unknown `type` decodes to `event_skipped == true` (35-I2/35-I8).
3. **Audience predicate.** A subscribed connection receives `event.live`; an
   unsubscribed connection and a pre-`hello` connection receive nothing; a
   connection with two subscriptions to the same session receives exactly one
   (35-I4).
4. **No cursor.** Assert the emitted notification has no `cursor` key and that a
   supervisor receiving it does not change `cursors_` (35-I5).
5. **Ordering.** Publish a live chunk then commit a settlement record; assert the
   `event.live` frame precedes the `event.stream` frame on the connection
   (35-I7).
6. **Profile.** An `Automation` connection subscribed to the session receives
   `event.live` (35-D4).
7. **Forward-compat.** A receiver that does not handle `event.live` keeps its
   connection and still processes `event.stream` (35-I8).

These extend the `05` §13.2 integration list and the Wave-2 live test; they do
not replace them.

---

## 9. dsh mapping

The dsh session protocol streams live assistant deltas on a side channel
separate from the durable event log, precisely so that a dropped side channel
cannot corrupt durable replay. `event.live` is ymh's faithful expression of that
split: the durable `event.stream` remains the cursor-bearing source of truth, and
the live channel is a transient, unreplayable optimization. The distinct
notification (rather than a flag on the durable stream) preserves the property
that no wire message can be both resumable and live-only.

---

## 10. Explicit non-goals

1. No behaviour change; no code. The shipped path is pinned as-is.
2. No `kProtocolVersion` / `kSchemaVersion` bump.
3. No change to `event.stream`, `event.unsubscribed`, `permission.request`,
   `host.event`, `SessionEnvelope`, or the handshake.
4. No change to the `AssistantChunk` live-only role (`29`) or the
   assembler/replay contract (`34`).
5. No resolution of the consumer double-print (OPEN-MEDIUM M3) — tracked
   separately.
6. No per-subscription live routing, and no decision on whether
   `McpServerStatusChanged` should join the live channel.

---

## 11. Revision log

- **Rev 1 (2026-09-19).** Initial authoring. Pins the `event.live` wire
  notification added unpinned by Wave 2 (`d979ec007`): method `"event.live"`,
  payload `{envelope}` (`LiveNotification`), server→client, per-session audience,
  no cursor/replay/subscription, same bounded queue, no version bump. Chooses the
  distinct notification over the `event.stream` route with the alternative
  analysed (35-D1). Requests four `05` amendments (§4.1 §5.1 :574-577, §4.2
  §7.7 :997-1004 — the catalog row, the `LiveNotification` DTO, and the
  semantics bullet) and records the `event.unsubscribed` omission (§4.3). Closes
  NEW-M1 of `/tmp/opencode/review-wave2-recheck.md`. No code, no git.
- **Rev 2 (2026-09-19).** All four required `05` amendments landed (§5.1's full
  no-cursor sentence, the §7.7 catalog row, the `LiveNotification` DTO, the §7.7
  semantics bullet); the gate's MEDIUM-1 is closed. The §2 "Status of the tree"
  block below is Rev 1's assessment and is retained as the historical record —
  the denial it cites no longer holds. Independently re-gated PASS (0 HIGH /
  0 MEDIUM / 6 LOW).

---

## 12. References

- `docs/design/05-transport.md` §5.1 :574-577; §5.2 :579-603; §5.4 :639-665;
  §7.1 :748-752; §7.7 :997-1073; §8.1 :1079-1093; §8.2 :1095-1108; §8.3
  :1110-1125; §8.5 :1148-1166; T6 :1232-1234; T7 :1236-1237; T8 :1239-1240; T11
  :1248-1249; T21 :1279-1282; T23 :1288-1291; T24 :1293-1295; T-F5 :1335; T-F17
  :1347.
- `docs/design/26-dsh-alignment-part2.md` 26-D9 :147; 26-I12 :111-124; 26-D24
  :162; §4.3.9.2 :996; §4.6 :1166-1235; §5 Wave 2 :1428-1451.
- `docs/design/29-event-family-errata.md` 29-D4 :105; §4.2 :344-354; 29-I7
  :420-421.
- `docs/design/34-assembler-replay-errata.md` §15 item 1 :964-976.
- `docs/design/33-stream-event-codec-errata.md` "Numbering note" (the 35+
  reservation).
- `docs/design/11-m2-errata.md` §7 (frozen M2 interfaces).
- `/tmp/opencode/review-wave2-recheck.md` §7 NEW-M1 (the finding this closes).
- Working tree: `include/ymh/transport/protocol.hpp` :225-232, :345-347, :450-451,
  :547-553; `src/transport/protocol.cpp` :262-280, :339-345, :633-662;
  `include/ymh/transport/protocol_server.hpp` :69; `src/transport/protocol_server.cpp`
  :557-627, :768-776, :782-799, :801-823, :825-834; `src/transport/json_rpc.cpp`
  :116-120, :155-177; `src/transport/transport_server.cpp` :193-200;
  `include/ymh/host/host_runtime.hpp` :85, :242; `src/host/host_runtime.cpp`
  :242-269, :294-323; `src/host/workspace_host.cpp` :544-547, :761-779;
  `src/ui/supervisor_connection.cpp` :312-362; `src/cli/cli.cpp` :109-116,
  :604-630; `include/ymh/cli/stream_receiver.hpp` :41-42; `src/core/event_bus.cpp`
  :199-238, :240-288; `src/session/session.cpp` :632-661;
  `src/agent/chunk_coalescer.cpp` :52-68; `src/agent/agent_loop.cpp` :899-920;
  `tests/integration_host_harness_test.cpp` :245-263;
  `tests/unit/host_runtime_test.cpp` :1029-1043; `tests/unit/headless_test.cpp`
  (`StreamReceiver.LiveChunkSuppressesDurableDuplicateText`).

# 01 — Session & Event Log

**Component 01 of 10.** The session is the spine of the runtime: an append-only
typed event log whose projection is the conversation and whose replay is the
debugging surface. This document pins the session/event-log interfaces, its
invariants, its failure modes, its DeepSeek Harness (dsh) mapping, and its test
plan. It follows `00-architecture.md` (cited inline as `§n`); where it cannot,
it records the conflict under §16 *Decisions and open questions* rather than
choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
                       Agent Loop (§11, spec 06)
                            │  append / deriveMessages
                            ▼
   ┌──────────────────────────────────────────────────────┐
   │  Session  (this spec)                                 │
   │    SessionHeader + append-only Event log              │
   │    deriveMessages() : log → LLM messages (§9.1)       │
   └───────────────┬───────────────────────┬──────────────┘
                   │ persist (lease-checked)│ publish
                   ▼                        ▼
        SessionStore / SessionPersistence   EventBus  (§8.3)
        (§9.2, §9.7; spec 02)               (spec 01 contract)
                   │                        │
                   ▼                        ├── global subscribers
        <workspace>/.ymh/sessions.db        └── per-session mailboxes
        sessions · events · session_leases      (§8.3)
```

The session is the durable source of truth (D2, §54). The TUI, CLI, and RPC
frontends are consumers of the same event stream, never owners of the agent
(D1, §54; §4.2, §56). The session layer is headless: it must be usable with no
terminal (G1, §4.1).

### 1.2 Owned responsibilities (HANDOFF §6, row 01)

This spec pins:

- `Session` as an append-only typed event log (§9.1).
- `SessionEventMap` and the turn/step event taxonomy (durable vs live, §8.1,
  §9.1), including user turn, assistant turn, tool call, tool result,
  permission decision, cancellation, compaction, and subagent spawn/fan-in.
- `deriveMessages()` — the pure, deterministic projection from the event log to
  the LLM message list (§9.1, §9.3).
- create / resume / fork / replay semantics (§9.3, §9.4, §9.5).
- `SessionHeader` in the final shape fixed by §9.2 / §9.10 (see §3).
- The `SessionId`-routed `EventBus` delivery contract (§8.3).
- The `SessionStore` / `SessionPersistence` / `SessionHandle` seams as consumed
  by the session layer (their SQLite implementation is spec 02).

### 1.3 Boundaries — deferred to other specs

| Concern | Owner | Why |
|---|---|---|
| SQLite DDL, pragmas, open/close policy, WAL | 02 | §9.2 says values belong to the persistence spec |
| Lease TTL, boot nonce, steal, `flock` sidecar, crash recovery | 02 | §9.7, HANDOFF §6 row 02 |
| Which agent-loop step emits which event, and when | 06 | §11; this spec defines the event *shape*, not the loop |
| Wire framing of `SessionEnvelope`, JSON-RPC methods | 05 | §9.6, §20.22 |
| Registry open-set, `ordinal`, `archived`, host liveness | 03 | §9.10, D21 |
| Attention, flash, waiting count | 10 | §9.9, §20.23, D19 |
| `ExecutionEnvironment::resolve()` implementation | 07 | §9.7, §18 |

The session layer never `chdir()`s and never resolves a path from `getcwd()`;
every path it stores is canonicalized with `std::filesystem::canonical()` and
every path it hands to a tool goes through `ExecutionEnvironment` (§9.7, §18).
The environment-root parameter is baked into the §18 interface before any tool
is written (§9.7).

---

## 2. Terminology and identities

### 2.1 Identifiers

All identifiers are strong value types so they cannot be silently interchanged
(§9.2). Each has `std::hash` and `std::formatter` specializations; the
`std::hash` specializations are required because `SessionId` keys
`UiModel::sessions` and `WorkspaceId` keys `UiModel::workspaces` (§20.22).

```cpp
namespace ymh {

// Store-assigned position in the event log. 0 means "not yet appended".
// Strictly increasing; assigned only by SessionStore (see Invariant I2).
using Sequence = std::int64_t;

struct SessionId {                       // UUIDv4; stable; never a path (§9.2)
    std::string value;
    auto operator<=>(const SessionId&) const = default;
};

struct EventId {                         // UUIDv4; globally unique (§9.2)
    std::string value;
    auto operator<=>(const EventId&) const = default;
};

struct WorkspaceId {                     // UUIDv4; never the path (§9.7)
    std::string value;
    auto operator<=>(const WorkspaceId&) const = default;
};

// Session-local monotonic counters, assigned by the Session on append.
// Deterministic: identical logs produce identical ids (needed for replay, §9.5).
using TurnId = std::uint64_t;
using StepId = std::uint64_t;

// Provider-supplied correlation ids (must round-trip verbatim).
using ToolCallId = std::string;          // from the LLM tool_use block (§12)
using MessageId  = std::string;          // UUIDv4

} // namespace ymh
```

`SessionId` and `WorkspaceId` are UUIDv4 values, never paths (§9.2, §9.7).
Canonical paths are compared only as `std::filesystem::canonical()` strings;
uniqueness of a workspace is string equality of its canonical path (§9.7,
DIV-8). `TurnId`/`StepId` are assigned by the session (not the provider) so
that a replayed log reconstructs identical turn/step identities; `ToolCallId`
is provider-supplied and must be preserved exactly for tool_use ↔ tool_result
pairing (§12).

### 2.2 Session kinds

```cpp
enum class SessionKind : std::uint8_t {
    Root,       // top-level conversation; no parent, no seed
    Fork,       // prefix copy of a parent at a boundary; has parent + seedLength
    Subagent,   // child agent with its OWN independent log; has parent, seed = null|0
};
```

`SessionKind` mirrors the SQL `CHECK (kind IN ('root','fork','subagent'))` and
the parent/seed constraint in §9.2. Subagents are ordinary `Agent` instances
(D9, §30) and keep their own `SessionId` so their durable log stays replayable
and inspectable (F11, §20.25).

---

## 3. `SessionHeader` (pinned)

This is the final field list fixed by §9.2 / §9.10 and `HANDOFF.md` §5 item 3.
Types are aligned **exactly** with the verified §9.10 sketch: timestamps are
epoch-ms `int64_t` (not `time_point`), `seedLength` is `std::optional<size_t>`
(not `uint32_t`), and `metadata` is an opaque `std::optional<std::string>` JSON
blob (not a decoded `nlohmann::json`). It deliberately contains **no boot
nonce** (liveness lives in the lease and the host registration, §9.7, §9.10) and
**no `ordinal` / `archived`** (those live in the registry junction
`workspace_sessions`, §9.10).

```cpp
struct SessionHeader {
    SessionId                     id;
    std::filesystem::path         cwd;           // canonical workspace root; immutable (§9.2)
    int64_t                       createdAt;     // epoch ms
    int64_t                       updatedAt;     // epoch ms of last appended event (§9.2)
    std::string                   title;         // '' => derive for display (§9.2)
    std::string                   model;         // default model id
    std::string                   serverProfile; // 'interactive' | 'automation' (§9.2, §38)
    SessionKind                   kind = SessionKind::Root;
    std::optional<SessionId>      parentSession; // fork/subagent parent; nullopt if root
    std::optional<size_t>         seedLength;    // # parent events shared; only fork
    std::optional<std::string>    metadata;      // opaque JSON extension bag; no schema
};
```

Consistency rules (enforced by `validateHeader()`, mirroring the SQL CHECK in
§9.2 — see Invariant I9):

```text
kind == Root      => !parentSession && !seedLength
kind == Fork      =>  parentSession &&  seedLength (>= 0)
kind == Subagent  =>  parentSession && (!seedLength || *seedLength == 0)
```

`cwd` is immutable for the lifetime of the session (§9.2, §9.7). Changing
workspace means creating a session in another daemon, not mutating this header.
`updatedAt` is advanced by the store on every successful append (§9.2).
`metadata` is an opaque JSON string with no schema commitments (§9.10); it is
not part of the projection and never influences `deriveMessages()` (it is not a
message source).

---

## 4. Event model

### 4.1 Event categories

§8.1 splits events into two categories, and the split is load-bearing:

- **Durable session events** survive process restart and are appended to the
  session log.
- **Live events** are runtime notifications; they are **never** persisted
  (§8.1 "Persistence rule").

Only durable events are in `SessionEventMap`; live events (`AgentStarted`,
`AgentStopped`, `AgentWaiting`, `LLMRequestStarted`, `LLMChunkReceived`,
`ToolExecutionStarted`, `ToolExecutionFinished`, `PermissionRequested`,
`TerminalOutput`, `Progress`, `Error`; §8.1) travel on the same `EventBus` but
are not written by the store. This keeps the log the authoritative trace while
allowing high-frequency UI notifications (§4.2, §8.1, §9.11 F5).

### 4.2 The erased core `Event` (§8.2)

The wire and persistence representation is the core, frontend-agnostic event
from §8.2. It carries no frontend type (D15, §20.22). Note it carries **no
`Sequence`**: sequence is store-local and is represented by `EventRecord`
(§4.6), so the wire `SessionEnvelope` is exactly `{session, event}` (§20.22).

```cpp
struct Event {
    EventId                                  id;
    SessionId                                session_id;
    std::chrono::system_clock::time_point    timestamp;
    EventType                                type;
    nlohmann::json                           payload;
};
```

Internally the code uses strongly typed payload structs (§4.4, §4.5); JSON is
only the persistence/wire representation (§8.2). `encode()` / `decode()` are
the only places that cross that boundary.

### 4.3 `EventType` and wire names

The C++ enum is CamelCase; the JSON `type` string is the slash form (§8.1).
Per the §8.1 errata, the durable event set is **non-exhaustive**: component
specs extend it, and this section is one such extension. The values below are
therefore **not a contradiction** of §8.1 but this component's additions —
`turn/cancel` (§34), `turn/fail` (§34, spec 08), `context/compaction` (§32),
`usage` (§33), and `subagent/spawn` / `subagent/fan_in` (§30). `wire_name()`
and
`parse_event_type()` are total for this extended set and reject anything else
(S3).

```cpp
enum class EventType : std::uint16_t {
    SessionStarted,       // wire: session/start          (§4.2, §8.1)
    SessionEnded,         // wire: session/end            (§8.1, §9.8)
    TurnStarted,          // wire: turn/start             (§8.1, §11)
    TurnEnded,            // wire: turn/end               (§8.1, §11)
    TurnCancelled,        // wire: turn/cancel            (§34) — component extension
    TurnFailed,           // wire: turn/fail              (§34, spec 08) — component extension
    StepStarted,          // wire: step/start             (§8.1, §11)
    StepEnded,            // wire: step/end               (§8.1, §11)
    UserMessage,          // wire: user/message           (§8.1)
    AssistantChunk,       // wire: assistant/chunk        (§8.1)
    AssistantMessage,     // wire: assistant/message      (§8.1)
    ToolCall,             // wire: tool/call              (§8.1, §11)
    ToolResult,           // wire: tool/result            (§8.1, §11)
    PermissionDecision,   // wire: permission/decision    (§8.1, §19)
    ContextInjected,      // wire: context/injected       (§8.1)
    ContextCompaction,    // wire: context/compaction     (§32)
    TokenUsage,           // wire: usage                  (§33)
    SubagentSpawned,      // wire: subagent/spawned       (§30, §20.25)
    SubagentFanIn,        // wire: subagent/fan_in        (§30, §20.25)
};

std::string_view wire_name(EventType);                       // CamelCase -> slash form
std::optional<EventType> parse_event_type(std::string_view); // slash form -> enum
```

`TurnCancelled` and `TurnFailed` are **component-local extensions**: §34
requires cancellation to be first-class, and spec 08 requires a provider/step
failure to be distinguishable from a user cancel. A turn terminates with
exactly one of `TurnEnded`, `TurnCancelled`, or `TurnFailed` (never more than
one). `ContextCompaction` (§32) and `TokenUsage` (§33)
are durable so that replay reproduces both the compacted context and the cost
accounting. `SubagentSpawned` / `SubagentFanIn` record the parent-side edges of
§30; the subagent's own turn/step events live in its own log (F11, §20.25).

### 4.4 `SessionEventMap` (type ↔ payload mapping)

`SessionEventMap` is the compile-time map from `EventType` to its payload type,
matching the dsh `SessionEventMap` naming (§8.1). Payloads live in
`namespace ymh::payload` so that the payload type name does not collide with
the enum value.

```cpp
namespace ymh::payload {

struct SessionStarted;
struct SessionEnded;
struct TurnStarted;
struct TurnEnded;
struct TurnCancelled;
struct TurnFailed;
struct StepStarted;
struct StepEnded;
struct UserMessage;
struct AssistantChunk;
struct AssistantMessage;
struct ToolCall;
struct ToolResult;
struct PermissionDecision;
struct ContextInjected;
struct ContextCompaction;
struct TokenUsage;
struct SubagentSpawned;
struct SubagentFanIn;

} // namespace ymh::payload

namespace ymh {

// EventType -> payload type. One specialization per durable event type.
template <EventType T> struct SessionEventMap;

template <> struct SessionEventMap<EventType::SessionStarted>     { using type = payload::SessionStarted;     };
template <> struct SessionEventMap<EventType::SessionEnded>       { using type = payload::SessionEnded;       };
template <> struct SessionEventMap<EventType::TurnStarted>        { using type = payload::TurnStarted;        };
template <> struct SessionEventMap<EventType::TurnEnded>          { using type = payload::TurnEnded;          };
template <> struct SessionEventMap<EventType::TurnCancelled>      { using type = payload::TurnCancelled;      };
template <> struct SessionEventMap<EventType::TurnFailed>         { using type = payload::TurnFailed;         };
template <> struct SessionEventMap<EventType::StepStarted>        { using type = payload::StepStarted;        };
template <> struct SessionEventMap<EventType::StepEnded>          { using type = payload::StepEnded;          };
template <> struct SessionEventMap<EventType::UserMessage>        { using type = payload::UserMessage;        };
template <> struct SessionEventMap<EventType::AssistantChunk>     { using type = payload::AssistantChunk;     };
template <> struct SessionEventMap<EventType::AssistantMessage>   { using type = payload::AssistantMessage;   };
template <> struct SessionEventMap<EventType::ToolCall>           { using type = payload::ToolCall;           };
template <> struct SessionEventMap<EventType::ToolResult>         { using type = payload::ToolResult;         };
template <> struct SessionEventMap<EventType::PermissionDecision> { using type = payload::PermissionDecision; };
template <> struct SessionEventMap<EventType::ContextInjected>    { using type = payload::ContextInjected;    };
template <> struct SessionEventMap<EventType::ContextCompaction>  { using type = payload::ContextCompaction;  };
template <> struct SessionEventMap<EventType::TokenUsage>         { using type = payload::TokenUsage;         };
template <> struct SessionEventMap<EventType::SubagentSpawned>    { using type = payload::SubagentSpawned;    };
template <> struct SessionEventMap<EventType::SubagentFanIn>      { using type = payload::SubagentFanIn;      };

template <EventType T>
using session_payload_t = typename SessionEventMap<T>::type;

// Payload type -> EventType + wire name. One specialization per payload.
template <class P> struct EventTraits;

template <class P>
inline constexpr EventType event_type_v = EventTraits<P>::type;

} // namespace ymh
```

`SessionEventMap` is exhaustive by construction: the `encode`/`decode` unit
tests iterate every `EventType` and assert a round-trip (Test plan §15.1).

### 4.5 Durable payload definitions

All payloads are plain structs, value-copyable, and JSON-serializable
(`to_json` / `from_json` via ADL). `Message`, `Role`, `ContentBlock`, and
`Usage` are the types defined by §12 (`include/ymh/agent/message.hpp`) and §33;
they are reused rather than redefined.

```cpp
namespace ymh::payload {

// ---- session lifecycle (§9.2, §9.8) ---------------------------------------

struct SessionStarted {
    std::string model;            // effective model at start
    std::string serverProfile;    // 'interactive' | 'automation'
    std::string title;            // may be '' (derive for display)
};

enum class SessionEndReason : std::uint8_t {
    Deleted,   // /session delete — the only mandated emitter (§9.8)
    Faulted,   // crash/corruption recovery marks the session terminal (02)
};

struct SessionEnded {
    SessionEndReason reason;
};

// ---- turn / step taxonomy (§8.1, §11) -------------------------------------

enum class TurnOrigin : std::uint8_t {
    User,       // a user prompt
    Steer,      // Agent::steer during an active turn (§10.1)
    FollowUp,   // queued input after the current turn (§10.1, inbox)
    Injection,  // ContextInjected materialised as a turn (§31)
};

struct TurnStarted {
    TurnId     turn;      // session-local monotonic
    TurnOrigin origin;
};

struct TurnEnded {
    TurnId turn;
};

struct TurnCancelled {
    TurnId      turn;
    std::string reason;   // human-readable; e.g. "user", "superseded", "shutdown"
};

// Provider/step failure (spec 08 §4.5). Distinct from TurnCancelled: a failed
// turn is not a user cancel, and its projection pairs unmatched tool_use
// blocks with ToolResult{outcome = Error} (I11, I12).
struct TurnFailed {
    TurnId      turn;
    std::string code;     // stable machine-readable code, e.g. "RateLimited"
    std::string message;  // redacted, human-readable diagnostic
};

struct StepStarted {
    TurnId turn;
    StepId step;          // session-local monotonic
};

struct StepEnded {
    TurnId turn;
    StepId step;
};

// ---- messages --------------------------------------------------------------

struct UserMessage {
    MessageId                 id;
    std::vector<ContentBlock> content;   // text / attachments (§12)
};

// Streaming deltas. Durable per §8.1 so a streamed UI can be replayed; NOT
// projected into LLM messages when a matching AssistantMessage exists (I14).
// The producer coalesces deltas into bounded batches before append; the exact
// cadence defers to specs 02/06 (§16.1 decision (c)).
struct AssistantChunk {
    MessageId   message;   // the AssistantMessage this chunk belongs to
    std::size_t index;     // monotonic within `message`
    std::string text;      // raw delta (may be reasoning; see `kind`)
    enum class Kind : std::uint8_t { Text, Reasoning } kind;
};

struct AssistantMessage {
    MessageId                 id;
    std::vector<ContentBlock> content;   // text + reasoning + tool_use blocks (§12)
    std::optional<Usage>      usage;     // convenience; TokenUsage is canonical (§33)
};

// ---- tool pipeline (§11, §14, §19) ----------------------------------------

struct ToolCall {
    ToolCallId      id;          // must equal the tool_use block id in the assistant message
    TurnId          turn;
    StepId          step;
    std::string     name;
    nlohmann::json  arguments;
    std::chrono::system_clock::time_point requestedAt;
};

enum class ToolOutcome : std::uint8_t { Ok, Error, Denied, Cancelled };

struct ToolResult {
    ToolCallId                 id;         // pairs with ToolCall::id
    std::string                name;
    ToolOutcome                outcome;
    std::string                output;     // possibly ring-truncated (F5, §9.11)
    bool                       truncated = false;
    std::optional<std::string> error;
    std::chrono::milliseconds  duration{0};
};

// ---- permissions (§8.1, §19) ----------------------------------------------

enum class PermissionDecisionKind : std::uint8_t { Allow, Deny, AllowAlways };

struct PermissionDecision {
    ToolCallId              call;
    PermissionDecisionKind  decision;
    std::string             reason;    // policy rule / user note
};

// ---- context (§8.1, §31, §32, §33) ----------------------------------------

struct ContextInjected {
    MessageId id;
    Role      role;      // role the injected content assumes when projected
    std::string text;
};

struct ContextCompaction {                 // §32: never deletes original events
    Sequence               boundary;       // events with seq <= boundary are summarised
    std::string            summary;
    std::size_t            tokenEstimate;
    std::string            model;          // model that produced the summary
    std::chrono::system_clock::time_point createdAt;
};

struct TokenUsage {
    Usage                  usage;          // §33: input/output/cached/reasoning tokens
    std::optional<TurnId>  turn;           // nullopt for out-of-turn accounting
};

// ---- subagents (§30, §20.25) ----------------------------------------------

struct SubagentSpawned {
    SessionId   subagent;      // the child's own SessionId (F11: not the parent's)
    std::string task;          // the delegated task, for display and replay
};

enum class SubagentOutcome : std::uint8_t { Completed, Failed, Cancelled };

struct SubagentFanIn {
    SessionId       subagent;
    SubagentOutcome outcome;
    std::string     summary;   // final result surfaced to the parent turn
};

} // namespace ymh::payload
```

`AssistantMessage::content` carries the assistant text, reasoning, and
`tool_use` blocks (§12). `ToolCall` is the durable execution record and carries
the same `ToolCallId`; it is not projected as a separate assistant message
(I12), it exists to drive and audit tool execution and to correlate
`PermissionDecision` and `ToolResult`. A denied or cancelled call still
produces a `ToolResult` (with `outcome == Denied` / `Cancelled`) so the model
always sees a result for every `tool_use` block (I12, §11).

**Cancelled/failed-turn pairing (projection policy).** When a turn is
cancelled (`TurnCancelled`), any `tool_use` block in that turn's
`AssistantMessage` that has no matching `ToolResult` is *unmatched*.
`deriveMessages()` MUST synthesize a terminal `ToolResult{outcome = Cancelled}`
for every unmatched `tool_use` in a cancelled turn, and
`ToolResult{outcome = Error}` for every unmatched `tool_use` in a failed turn
(`TurnFailed`), so the projected message list pairs every `tool_use` with a
result (provider-safe; I12, §6.3). Failure and cancellation must remain
distinguishable in replay. This is a projection-time synthesis only: it never
appends to the log and never mutates the durable event stream.

### 4.6 `EventRecord` and `EventRange`

Because §8.2's `Event` has no `Sequence`, the read side wraps it:

```cpp
struct EventRecord {
    Sequence seq;      // store-assigned, strictly increasing (I2)
    Event    event;    // core erased event (§8.2); carries no sequence
};

using EventRange = std::vector<EventRecord>;   // ascending by seq
```

`Sequence` never crosses the transport boundary; `SessionEnvelope` carries the
bare `Event` (§20.22). This keeps the wire shape pinned by §20.22 unchanged.

---

## 5. `EventBus`: `SessionId`-routed delivery (pinned contract)

§8.3 mandates **one shared `EventBus`** plus a per-session mailbox between the
bus and each consumer; it explicitly forbids N child buses (D12). The session
layer's contract with the bus:

```cpp
class EventBus {
public:
    // Global subscribers: logging, telemetry, and the host event forwarder.
    // A durable-only subscriber (e.g. SessionStore as observer, §8.3) filters
    // to durable EventTypes and ignores live ones (§8.1 persistence rule).
    template <class E>
    Subscription subscribe(std::function<void(const E&)> handler);

    // Per-session subscribers: backed by that session's ordered mailbox.
    // The handler sees only events whose session_id == session.
    template <class E>
    Subscription subscribe(SessionId session, std::function<void(const E&)> handler);

    // The single publish entry point. Durable events reach the bus only after
    // they are committed (I4); live events are published directly.
    void publish(Event event);

    template <class P>
    void publish(const TypedEvent<P>& event);   // erases to Event via encode()
};

class Subscription {                 // RAII; unsubscribes on destruction (§8.3)
public:
    ~Subscription();
    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;
    Subscription(const Subscription&) = delete;
};

class SessionMailbox {               // one per session, owned by the bus (§8.3)
public:
    void push(Event event);                          // preserves publish order
    bool drain(const std::function<bool(Event&&)>& sink);  // true when empty
    bool empty() const noexcept;
};
```

**Refinement note.** §8.3 sketches only the global `subscribe(handler)` and
`publish(Event)` entry points; that sketch is illustrative and not exhaustive.
The session-routed `subscribe(SessionId, …)` overload and the typed
`publish(const TypedEvent<P>&)` overload are **spec-level refinements** of that
sketch. They add no new delivery semantics and leave the §8.3 contract (promoted
here to I6) unchanged.

Delivery contract (verbatim from §8.3, promoted to invariants I6 and I20):

```text
events for session X are delivered in publish order
events for different sessions may interleave arbitrarily
global cross-session ordering is not guaranteed and is not required
```

The mailbox also resolves F3: before a closed session's `SessionUiState` is
freed, its mailbox must drain and its `AgentHandle` must report a terminal
state (§8.3, §9.8). The session layer exposes `SessionMailbox::empty()` so the
teardown path can enforce this; the tombstone policy itself is spec 10.

The **waterfall/interception** events (`tool/pre_execute`, `agent/pre_step`,
`permission/request`; §8.4) are live control events, not durable, and are
published on the same bus. They are out of scope for the log (they never enter
`SessionEventMap`), but the session's `TurnCancelled` / `PermissionDecision`
records are their durable outcomes.

---

## 6. The `Session` interface

```cpp
class Session {
public:
    Session(SessionHeader header,
            SessionStore& store,
            EventBus& bus);

    // ---- identity / header -------------------------------------------------
    const SessionHeader& header() const noexcept;
    SessionId            id() const noexcept;
    SessionKind          kind() const noexcept;

    // ---- read side ---------------------------------------------------------
    // Resolved logical view in ascending Sequence order. For a Root/Subagent
    // session this is the physical log; for a Fork it is the parent prefix
    // [0, seedLength) followed by this session's own events (I10, §9.4).
    EventRange events() const;

    // This session's physical events only (excludes any inherited prefix).
    EventRange ownEvents() const;

    // Pure, deterministic projection (I7). Reads only header() and events();
    // performs no I/O, no clock read, and no randomness (§9.1, §9.3).
    std::vector<Message> deriveMessages() const;

    // ---- write side (only the lease holder may call; I5, §9.7) -------------
    // Erases the typed payload, assigns EventId/timestamp, persists, then
    // publishes. Returns the store-assigned Sequence.
    template <class P>
    Sequence append(const P& payload) {
        return appendEvent(encode(payload));
    }

    // The one true write path: lease-check -> persist -> advance updatedAt ->
    // publish (I4). Throws LeaseLost if this process is not the lease holder.
    Sequence appendEvent(Event event);

    // ---- derived cache (optional; must be equivalent to deriveMessages) ----
    SessionSnapshot snapshot() const;   // I21: snapshot().messages == deriveMessages()

    // ---- factories ---------------------------------------------------------
    static Session resume(const SessionHeader&, SessionStore&, EventBus&);   // §9.3
    static Session fork(const Session& parent,
                        size_t seedLength,
                        SessionStore&, EventBus&);                            // §9.4
    static Session replay(const SessionHeader&, SessionStore&, EventBus&);    // §9.5

private:
    SessionHeader  header_;
    SessionStore*  store_;
    EventBus*      bus_;
    EventRange     log_;             // loaded on construction/resume
    TurnId         nextTurn_  = 1;   // deterministic id assignment
    StepId         nextStep_  = 1;
    mutable std::mutex appendMutex_; // serialises appendEvent within the process (I18)
};
```

`append<P>` is the ergonomic entry point; `appendEvent` is the only path that
touches the store and the bus, so the lease check and the durability ordering
exist in exactly one place. The agent loop (spec 06) is the primary caller, but
tools also emit through `ToolContext::emit(Event)` (§14.2), which routes to the
owning session.

### 6.1 Append protocol

```text
append<P>(payload)
  └─ encode(payload) -> Event { EventId = uuid4(), session_id = id(),
                                timestamp = system_clock::now(),
                                type = event_type_v<P>, payload = json }
       └─ appendEvent(event)
            ├─ 1. lease check: store.isLeaseHolder(id())          (I5, §9.7)
            ├─ 2. store.append(id(), event) -> Sequence           (commit first)
            ├─ 3. header_.updatedAt = event.timestamp             (§9.2)
            ├─ 4. log_.push_back(EventRecord{seq, event})
            └─ 5. bus_->publish(event)                            (I4: after commit)
```

Persist-then-publish is deliberate: a durable event must never be observable by
a consumer that could not later replay it (I4). If step 2 throws, nothing is
published and the in-memory log is unchanged. `encode` is total over the
`SessionEventMap` domain; an unknown payload type is a compile error.

### 6.2 Read side

`events()` returns the **resolved logical view**. The session layer does not
decide the physical fork strategy: `SessionStore` (spec 02) may implement a
fork as a shared-prefix reference or as physically copied rows; either way the
logical view is the same and `Sequence` ordering is preserved. Because
`events.sequence` is DB-global `AUTOINCREMENT` (§9.2), a fork's inherited
prefix sequences are always smaller than the child's own later sequences, so
the concatenation is ascending under both strategies (see §16.1 decision (b)).

`ownEvents()` is needed by `Session::fork` (to compute the prefix boundary) and
by the store; it is not part of the projection contract.

### 6.3 `deriveMessages()` — pure projection

`deriveMessages()` turns the log into the LLM message list (§9.1, §9.3). It is
a **pure function of `(header, events())`**: no I/O, no clock, no randomness,
no bus access. This is what makes resume, fork, and replay deterministic
(§44 replay tests) and is a precondition for the Fake-LLM test layer (§45).

Algorithm (normative):

```text
messages := []
openTurn : optional<TurnId> := null
openStep : optional<StepId> := null

for rec in events():                       # ascending Sequence
    e := rec.event
    switch e.type:

    case SessionStarted:                   # not projected
    case SessionEnded:
    case TurnStarted:      openTurn = payload.turn; openStep = null
    case StepStarted:      openStep = payload.step
    case StepEnded:        openStep = null
    case TurnEnded:        openTurn = null; openStep = null
    case TurnCancelled:    openTurn = null; openStep = null      # terminal for the turn
        # I12: synthesize ToolResult{outcome = Cancelled} for every unmatched
        # tool_use in this turn so the projection pairs all tool_use blocks
    case TurnFailed:       openTurn = null; openStep = null      # terminal for the turn
        # I12: synthesize ToolResult{outcome = Error} for every unmatched
        # tool_use in this turn; failure ≠ cancel

    case UserMessage:
        messages += Message{ Role::User, payload.content }

    case AssistantChunk:                   # not projected (I14)
        # streaming deltas are for UI replay; the assembled AssistantMessage is canonical

    case AssistantMessage:
        messages += Message{ Role::Assistant, payload.content }   # includes tool_use blocks

    case ToolCall:                         # not projected (I12): already in the assistant content
    case ToolResult:
        messages += Message{ Role::Tool,
                             tool_call_id = payload.id,
                             content      = payload.output }      # truncated per F5

    case ContextInjected:
        messages += Message{ payload.role, payload.text }

    case ContextCompaction:
        # §32: do not destroy events; drop projected messages derived from
        # events with seq <= boundary and prepend one synthetic summary message.
        messages.remove_where(derived_seq <= payload.boundary)
        messages.prepend(Message{ Role::System, payload.summary })

    case PermissionDecision:               # not projected
    case TokenUsage:                       # not projected
    case SubagentSpawned:                  # not projected
    case SubagentFanIn:                    # not projected

return messages
```

Determinism requirements:

- Iteration is strictly by ascending `Sequence`, never by timestamp (wall
  clocks can tie or move backwards).
- `TurnId` / `StepId` are assigned by the session at append time, so replay
  yields identical ids; `deriveMessages` never re-derives them.
- Compaction is a fold over the log in sequence order; overlapping compactions
  compose deterministically (the latest boundary wins for overlapping ranges).
- A partial/open turn (crash or live session, S8) projects the events seen so
  far, deterministically, rather than failing. This is required for live UI and
  for crash recovery; the same log always yields the same partial projection.

`deriveMessages` is intentionally the *only* place that defines LLM-visible
semantics. The UI projection (§20.8 `ConversationModel`) is a separate
projection over the same events; it must not be reused here.

### 6.4 `snapshot` / checkpoint

For long logs, re-deriving on every resume is wasteful. A snapshot is a derived
cache persisted by spec 02; the session layer only pins its shape and its
equivalence invariant.

```cpp
struct SessionSnapshot {
    SessionId            session;
    Sequence             at;          // log head covered by this snapshot
    SessionHeader        header;      // header at `at`
    std::vector<Message> messages;    // == deriveMessages() at `at` (I21)
    std::size_t          eventCount;  // resolved event count at `at`
};
```

A snapshot is valid only while `snapshot.at` equals the current log head. If
the log has advanced, the snapshot is stale and must be discarded and
recomputed (S12) — it is never patched incrementally in place. A snapshot is
**never** the source of truth: `deriveMessages()` over the log always wins, and
the replay tests assert the two are equal (I21).

---

## 7. `SessionStore` / `SessionPersistence` / `SessionHandle` seams

Spec 01 depends on these seams; spec 02 owns their implementation (§9.2, §9.7,
`HANDOFF.md` §6 row 02).

```cpp
class SessionStore {                    // dsh "Session event log" seam (§55)
public:
    virtual ~SessionStore() = default;

    // ---- lifecycle ---------------------------------------------------------
    virtual SessionHeader                create(SessionHeader) = 0;   // §9.2
    virtual std::optional<SessionHeader> load(SessionId) const = 0;   // §9.3
    virtual std::vector<SessionHeader>   list() const = 0;
    virtual void                         erase(SessionId) = 0;        // §9.8 delete

    // ---- read --------------------------------------------------------------
    virtual EventRange read(SessionId, Sequence after = 0) const = 0;
    virtual EventRange readRange(SessionId, Sequence from, Sequence to) const = 0;

    // ---- write (lease-checked) --------------------------------------------
    // Persists and returns the store-assigned Sequence. Throws LeaseLost when
    // this process does not hold the session's lease (§9.7, I5).
    virtual Sequence append(SessionId, Event) = 0;

    // ---- lease query (ownership lives in spec 02) --------------------------
    virtual bool isLeaseHolder(SessionId) const = 0;
};

// The SQLite-backed implementation (§9.2); opened with
// PRAGMA foreign_keys=ON, journal_mode=WAL, busy_timeout (§9.2).
class SessionPersistence : public SessionStore {
public:
    // 02 pins: DB path, pragmas, open/close policy, flush/checkpoint,
    // lease TTL/boot-nonce/steal, crash recovery. The lease is checked
    // before each COMMIT (§9.7).
};

// IPC-addressable teardown capability that owns the lease (§9.7, §9.8).
class SessionHandle {
public:
    virtual ~SessionHandle() = default;
    virtual SessionId session() const = 0;
    virtual bool      holdsLease() const noexcept = 0;
    virtual void      release() = 0;   // release lease; daemon keeps or reaps
};
```

The session layer treats `SessionStore` as an abstract seam: unit tests inject
an in-memory fake store, and integration tests use the real
`SessionPersistence` (Test plan §15.1–§15.2).

---

## 8. `SessionManager`

`SessionManager` is one instance per `WorkspaceHost` daemon (§9.6). It owns the
sessions of its own daemon; the focused/active session is supervisor-local
state, not manager state (§9.6, §20.22). The shape is §9.6, extended with the
session-first entry points this spec defines:

```cpp
struct SessionOptions {                 // §9.6
    std::filesystem::path cwd;
    std::string           serverProfile;   // Interactive | Automation
    std::string           model;
    std::string           title;
};

class SessionManager {                  // one instance per WorkspaceHost (§9.6)
public:
    // ---- create / resume / fork / replay ----------------------------------
    SessionId              createSession(SessionOptions);              // §9.3
    SessionId              resumeSession(SessionId);                   // §9.3
    SessionId              forkSession(SessionId, size_t seedLength);   // §9.4
    SessionId              replaySession(SessionId);                   // §9.5

    // ---- close / delete ----------------------------------------------------
    void                   closeSession(SessionId);   // detach semantics (§9.8)
    void                   deleteSession(SessionId);  // destructive; emits SessionEnded (§9.8)

    std::vector<SessionId> list() const;

    Session&               session(SessionId);
    Agent&                 agent(SessionId);           // §9.6
    ExecutionEnvironment&  env(SessionId);             // §9.6
};
```

`replaySession` returns a handle whose `Session` was constructed read-only; it
never acquires the write lease and never appends (I19, §9.5). `closeSession`
implements detach, which does **not** emit `SessionEnded`; `deleteSession` does
(§9.8, I16).

---

## 9. Lifecycle semantics

### 9.1 Create (§9.2, §9.3)

```text
createSession(options)
  ├─ canonicalize options.cwd with std::filesystem::canonical()   (§9.7, S14)
  ├─ allocate SessionId = UUIDv4
  ├─ header := SessionHeader{ id, cwd, now, now, title, model,
  │                           serverProfile, kind = Root,
  │                           parentSession = nullopt, seedLength = nullopt,
  │                           metadata = nullopt }
  ├─ validateHeader(header)                                        (I9)
  ├─ store.create(header)              # acquires the session's lease; open failure -> S11 (02)
  └─ session.append(payload::SessionStarted{ model, serverProfile, title })
        └─ first durable event; becomes this session's first own event
           (global sequence > 0)                                        (I2)
```

`SessionStarted` is the first event of every session and is never re-emitted.

### 9.2 Resume (§9.3)

```text
resumeSession(id)
  ├─ header := store.load(id)                 # open failure -> S11; !header -> UnknownSession (S6)
  ├─ log    := store.read(id)                 # ascending Sequence
  ├─ validateHeader(header); validateLog(log)                       (I9, S2, S3)
  ├─ projection := deriveMessages(header, log)                      (I7)
  ├─ restore agent state from the projection (§9.3)
  └─ return session handle in AgentStatus::Idle
```

Resume appends nothing (I18). Per F10, a resumed session opens **suspended /
`Idle`** and rehydrates on activation (not on UI focus); background auto-resume is opt-in only, so a
restart does not spawn a burst of LLM calls (§9.9). The lease is acquired by
the store (02) before the session becomes writable.

### 9.3 Fork (§9.4)

A fork creates a new session whose logical view is the parent's first
`seedLength` events followed by its own (I10):

```text
forkSession(parentId, seedLength)
  ├─ parent := store.load(parentId)          # must be a live or durable session
  ├─ parentView := resolve(parent)
  ├─ require 0 <= seedLength <= parentView.size()    else InvalidForkBoundary (S5)
  ├─ allocate childId = UUIDv4
  ├─ header := SessionHeader{ childId, parent.cwd, now, now, '', parent.model,
  │                           parent.serverProfile, kind = Fork,
  │                           parentSession = parentId,
  │                           seedLength   = seedLength,
  │                           metadata     = nullopt }
  ├─ store.create(header)                    # child acquires its OWN lease (02)
  └─ return childId
```

The fork copies no lease from the parent; the child has an independent write
lease and an independent future. The default physical strategy is
**copy-on-write / shared-prefix with prefix-transparent reads**; physically
copied rows are reserved for export and cross-DB copies. The exact physical
choice still defers to spec 02; the logical prefix contract is fixed here
(§16.1 decision (d)). `seedLength` counts parent events, so fork-of-fork
composes: the parent's resolved view already includes its own inherited prefix.

The fork emits no `SessionStarted` of its own? It does: a fork is a session and
must emit `SessionStarted` as its first own event. The inherited prefix is
logically before it; `deriveMessages` projects the prefix, then the child's own
`SessionStarted` (non-projected), then the child's later events.

### 9.4 Replay (§9.5)

```text
replaySession(id)
  ├─ header := store.load(id)
  ├─ log    := store.read(id)
  ├─ projection := deriveMessages(header, log)
  └─ publish projection/events to the requesting consumer (headless, CLI, or TUI)
```

Replay is **read-only**: it never acquires the write lease, never appends, and
opens a WAL read transaction so it observes a consistent snapshot even while a
live holder writes (I19, §9.5). Replay is the substrate of the §44 replay tests
and of `ymh replay SESSION` (§9.5). It must be deterministic: identical input
log ⇒ identical projected state (§44).

### 9.5 Close / detach / delete (§9.8)

These are four orthogonal operations and must not be overloaded (§9.8):

| Operation | Emits `SessionEnded`? | Session layer action |
|---|---|---|
| Detach (`Ctrl+W`) | No | TUI unsubscribes; the daemon keeps appending (§9.8, D23) |
| Cancel (`Ctrl+C` / `/cancel`) | No | Emits `TurnCancelled`; history intact (§34, §9.8) |
| Delete (`/session delete`) | Yes (`reason = Deleted`) | `store.erase(id)` after appending `SessionEnded` |
| Archive (deferred) | No | Registry junction only (§9.10); not a session-layer concern |

Detach must not emit `SessionEnded`; delete must (§9.8). Overloading them would
corrupt resume: "did the session end, or was it only closed here?" Late events
after close (F3) are retained by a tombstone until the mailbox drains and the
`AgentHandle` is terminal (§8.3, §9.8); the session layer exposes the log and
mailbox state the tombstone needs, but the tombstone itself is spec 10.

---

## 10. Write lease and path safety

### 10.1 Only the lease holder may append (§9.7)

`session_leases` (§9.2) grants exclusive write access to one holder per
session, keyed by `(holder_pid, holder_boot_id)`:

```text
acquire  INSERT (session_id, holder_pid, holder_boot_id, now, now+TTL)
renew    UPDATE expires_at WHERE holder_pid = me AND holder_boot_id = my_boot
steal    REPLACE when the row is expired, or when (holder_pid, holder_boot_id)
         no longer names a live process
release  DELETE WHERE holder_pid = me AND holder_boot_id = my_boot
```

Liveness is established primarily by a `flock` on the sidecar
`<workspace>/.ymh/sessions.lock`; `kill(pid, 0)` is only a secondary hint (§9.7,
§9.10). The lease mechanics (TTL, boot nonce, steal, the sidecar) are spec 02.
The session layer's obligations are:

1. `appendEvent` asks `SessionStore::isLeaseHolder(id())` **before** persisting;
   a non-holder gets `LeaseLost` and performs no write (I5).
2. `SessionPersistence` re-checks the lease before each `COMMIT` (§9.7).
3. A holder that fails to renew stops writing, degrades to **read-only**, and
   notifies its supervisor (§9.7); `deriveMessages` and replay keep working
   read-only.

### 10.2 Path safety (§9.7, §18)

- `SessionHeader.cwd` is canonicalized once with
  `std::filesystem::canonical()` at create and is immutable (§9.7).
- The session layer never `chdir()`s; the daemon chdirs to its workspace root
  once at startup, which is safe because one daemon owns one workspace (D18,
  F1, §9.7).
- Every path the session hands to a tool, LSP, git, or subprocess is resolved
  through `ExecutionEnvironment::resolve()`, root-relative; `getcwd()` is never
  a resolution base in tool code (§9.7, §18).
- A create whose `cwd` does not exist or is not a directory is rejected (S14).

---

## 11. Concurrency and threading

The session is written from one daemon process, but a daemon runs multiple
sessions and may append from more than one thread (agent loop, tool threads).
Rules:

- `appendEvent` is serialized by the session's `appendMutex_`; a single session
  never has two in-flight appends within the process (I18).
- Cross-process single-writer is the lease (I5, §9.7); the store enforces it.
- Reads (`events`, `deriveMessages`) are taken over an immutable snapshot of the
  in-memory log; a reader never observes a torn append (S13).
- The `EventBus` delivers per session in publish order; the mailbox is the
  ordering authority for a session (§8.3). Cross-session interleaving is
  unconstrained and must not be relied upon (§8.3, I6).
- `Session::append` is not reentrant: an event handler that runs on `publish`
  must not call back into `append` for the same session synchronously. If a
  handler needs to append, it enqueues (the agent loop owns this policy, §11).

---

## 12. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**I1 — Append-only.** Durable events are never updated or deleted in place; the
only removal is a whole-session delete (§9.8), which itself appends
`SessionEnded` first.

**I2 — Sequence monotonicity.** `SessionStore::append` is the sole assigner of
`Sequence`; every new sequence is strictly greater than every sequence already
committed in the database. Within a session, sequences are strictly increasing;
`events()` is ascending by sequence. (§9.2)

**I3 — EventId uniqueness and immutability.** `EventId` is a UUIDv4, globally
unique (`events.event_id UNIQUE`, §9.2), and never reused.

**I4 — Durable-before-observable.** A durable event is published on the
`EventBus` only after it is committed by the store. A consumer never observes
an event that replay could not reproduce.

**I5 — Only the lease holder may append.** `appendEvent` fails with
`LeaseLost` for a non-holder; `SessionPersistence` re-checks before each
`COMMIT`. (§9.7)

**I6 — `SessionId`-routed, per-session ordered delivery.** For a given
`SessionId`, subscribers receive events in publish order; different sessions
may interleave arbitrarily; global cross-session ordering is not guaranteed and
is not required. (§8.3)

**I7 — `deriveMessages` is pure and deterministic.** It reads only
`header()` and `events()`; no I/O, no clock, no randomness. Identical
`(header, events())` ⇒ identical message list.

**I8 — `cwd` is immutable and canonical.** A session's `cwd` is the
realpath-canonical workspace root and never changes for the session's life.
(§9.2, §9.7)

**I9 — Header kind/parent/seed consistency.** The `validateHeader` matrix in §3
holds for every persisted and loaded header, mirroring the SQL CHECK (§9.2).

**I10 — Fork is a prefix.** For `kind == Fork`, the resolved view is
`parent.events()[0, seedLength) ++ ownEvents()`, and the child's own log begins
with its own `SessionStarted`. (§9.4)

**I11 — Turn/step nesting.** `StepStarted`/`StepEnded` occur only inside an
open turn (`TurnStarted` not yet closed); `AssistantMessage`, `ToolCall`,
`ToolResult`, `PermissionDecision`, `TokenUsage`, and `ContextCompaction`
occur inside an open turn. A turn closes with exactly one of `TurnEnded`,
`TurnCancelled`, or `TurnFailed`.

**I12 — Tool pairing.** Every `ToolResult` references a prior `ToolCall` by
`ToolCallId`; the `ToolCallId` equals the `tool_use` block id in the step's
`AssistantMessage`. A denied or cancelled call yields a `ToolResult` with
`outcome ∈ {Denied, Cancelled}`; a failed call yields `outcome = Error`. Every
`ToolCall` in a turn that ends with `TurnEnded` has exactly one `ToolResult`.
When a turn is cancelled, `deriveMessages()` synthesizes a
`ToolResult{outcome = Cancelled}` for every unmatched `tool_use` in that turn;
when a turn fails (`TurnFailed`), it synthesizes
`ToolResult{outcome = Error}` instead. This pairs every `tool_use` in either
terminal case (provider-safe; §4.5) and keeps failure distinguishable from
cancellation in replay. (§11, §12)

**I13 — Durable/live separation.** Live events (§8.1) are never written to the
session log and never appear in `SessionEventMap`. (§8.1)

**I14 — Chunk/message precedence.** `AssistantChunk` is not projected when a
matching `AssistantMessage` exists for the message id; the assembled
`AssistantMessage` is canonical for the LLM projection.

**I15 — No ambient cwd.** Session and tool code never resolve a path from
`getcwd()`; resolution goes through `ExecutionEnvironment::resolve()`.
(§9.7, §18)

**I16 — Detach ≠ end; delete = end.** Detach emits no `SessionEnded`; delete
emits exactly one with `reason == Deleted`. (§9.8)

**I17 — Subagent identity.** A subagent has its own `SessionId` and its own
durable log; parent-side routing must not overwrite the subagent id. (F11,
§20.25)

**I18 — Single in-process appender.** Concurrent `append` calls to one
`Session` are serialized; a reader never observes a torn append (S13).

**I19 — Replay is read-only.** Replay never acquires the lease and never
appends; it observes a consistent snapshot. (§9.5)

**I20 — Mailbox drain before teardown.** A closed/detached session's mailbox is
drained and its `AgentHandle` is terminal before its UI state is freed. (F3,
§8.3, §9.8)

**I21 — Snapshot equivalence.** If `snapshot.at` equals the current log head,
then `snapshot.messages == deriveMessages()`; otherwise the snapshot is stale
and must be discarded and recomputed, never patched (S12).

**I22 — `updatedAt` tracks the log.** `header.updatedAt` equals the timestamp
of the most recently appended event. (§9.2)

**I23 — Metadata is inert.** `metadata` never influences `deriveMessages()` or
any projection; it is an extension bag only.

---

## 13. Failure modes

### 13.1 Shared findings (F1–F12, §54)

The session layer's responsibilities for the existing findings:

| F# | Finding | Session-layer handling |
|---|---|---|
| **F1** | path/process isolation | `cwd` canonicalized and immutable; no `chdir()`; resolution via `ExecutionEnvironment` (§9.7, §10.2, I8, I15) |
| **F2** | background permission | `PermissionDecision` is durable; a background session that hits `ASK` still records the decision, so it cannot deadlock invisibly (the policy is spec 09, §9.9) |
| **F3** | late event after close | tombstone until mailbox drains + handle terminal; session exposes log/mailbox state (I20, §8.3, §9.8) |
| **F4** | edge-triggered attention | out of scope here; durable turn/step/`ToolCall`/`ToolResult` events are the source the UI projects into `AgentState` (§9.9, §20.23) |
| **F5** | output ring buffers | `ToolResult.output` may be ring-truncated; `truncated` is recorded so `deriveMessages` and replay are honest about what the model saw (§9.11) |
| **F6** | input/keybinding focus | out of scope here; per-session draft lives in `InputModel`, not the log (§20.24, §20.26) |
| **F7** | per-session dirty flags | out of scope here; per-session mailbox is the delivery unit that makes per-session dirtying possible (§20.23) |
| **F8** | resource caps | session layer does not execute; caps are per host (§9.11) |
| **F9** | cancellation scoping | `TurnCancelled` names a single `TurnId`; cancelling one session cannot cancel another (§34, §20.24) |
| **F10** | resume-suspended | resume opens `Idle` and appends nothing; no auto-resume burst (I18, §9.9) |
| **F11** | subagent ID duality | subagent keeps its own `SessionId` and log; parent id is display routing only (I17, §20.25) |
| **F12** | flash clock in model | out of scope here; no timers in the session layer (§9.9, §20.23) |

**Explicitly out of scope for this component.** F4, F6, F7, F8, and F12 are
listed above only to fix the boundary; their owning components are:

- **F4** edge-triggered attention — UI projection (§9.9, §20.23).
- **F6** input/keybinding focus — `InputModel` (§20.24, §20.26).
- **F7** per-session dirty flags — per-session mailbox/UI model (§20.23).
- **F8** resource caps — per-host scheduler (§9.11).
- **F12** flash clock in model — UI timer (§9.9, §20.23).

The session layer supplies the durable events these components project, but
owns none of their logic.

### 13.2 Component-local failure modes (S1–S14)

These are **component-local** to the session/event-log layer and are not part
of the top-level F1–F12 set. They are numbered `S#` and must be covered by
tests (§15.6).

| S# | Failure | Detection | Required behavior |
|---|---|---|---|
| **S1** | Duplicate `EventId` on append | store returns a uniqueness violation (`events.event_id UNIQUE`, §9.2) | Reject the append; fail loud; do not publish; surface as corruption |
| **S2** | Non-monotonic sequence | store returns a sequence ≤ the session's current head, or `read()` yields out-of-order rows | Treat as store corruption; fail loud; refuse to project (I2) |
| **S3** | Unknown / undecodable event type or payload | `parse_event_type` returns `nullopt`, or JSON `from_json` fails | Replay must be faithful: fail loud on the offending record rather than silently skipping (no lossy replay) |
| **S4** | Lease lost mid-session | `isLeaseHolder` false, or `COMMIT` rejected | Degrade the session to read-only; stop appending; notify supervisor (§9.7, I5) |
| **S5** | Invalid fork boundary | `seedLength > parentView.size()` | Reject with `InvalidForkBoundary`; create nothing (§9.4) |
| **S6** | Dangling parent / unknown session | `store.load(id)` empty, or `parentSession` does not resolve | Reject resume/fork/replay; report the orphan; do not fabricate a header |
| **S7** | Compaction boundary not found | `boundary` does not match any committed `Sequence` in the resolved view | Reject the compaction event at append/validate time; fail loud (§32) |
| **S8** | Open turn/step at projection time | live session, or crash recovery with an unclosed turn | Project deterministically up to the last event; never throw (I7) |
| **S9** | Malformed identity / header | `SessionId` not UUIDv4, `cwd` not absolute, or `validateHeader` fails | Reject at create/load; do not persist (I9) |
| **S10** | Oversized payload / metadata | payload JSON exceeds the configured cap, or `metadata` is present but is not valid JSON / does not parse to an object | Reject the append; do not commit; surface the cap |
| **S11** | Store unavailable at open | `SessionStore` create/open fails: DB file missing, locked, or otherwise unopenable | Reject the open with a store error; do not fabricate a session; surface it to the caller (02 owns the store error) |
| **S12** | Stale snapshot | `snapshot.at` ≠ the current log head | Discard the snapshot and recompute by replaying forward; never patch it in place (I21, §6.4) |
| **S13** | Torn read | a reader observes a partially-written event (or a sequence whose row is not fully visible) | Never project the partial event; re-read to a consistent snapshot before projecting (I18, §11) |
| **S14** | Missing / non-directory `cwd` | `cwd` does not exist or is not a directory at open/append | Reject create/open (and refuse the append) with a path error; never `chdir` (§9.7, §10.2) |

`S4` and `S12` degrade gracefully (read-only / recompute); `S1`, `S2`, `S3`,
`S7`, `S9`, `S13`, and `S14` are corruption- or consistency-class and must fail
loud rather than guess; `S11` rejects the open and surfaces the store error.

---

## 14. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (§55). The session layer maps onto
it as follows.

| dsh concept | ymh session layer | Reference |
|---|---|---|
| Session event log | `Session` + `SessionStore` (the `SessionEventMap`-typed append-only log) | §9.1, §55 |
| `SessionEventMap` | `ymh::SessionEventMap<EventType>` (type ↔ payload) | §4.4, §8.1 |
| `turn/start`, `turn/end` | `EventType::TurnStarted`, `EventType::TurnEnded` (wire `turn/start`, `turn/end`) | §8.1, §11 |
| `step/start`, `step/end` | `EventType::StepStarted`, `EventType::StepEnded` | §8.1, §11 |
| history derived from the session log | `Session::deriveMessages()` (pure projection) | §9.1, §9.3 |
| session store (SQLite) | `SessionPersistence` over `<workspace>/.ymh/sessions.db` | §9.2, §55 |
| resume / fork / replay | `Session::resume` / `Session::fork` / `Session::replay` | §9.3–§9.5 |
| Cordis typed events | `EventBus` + `Subscription` | §8.3, §55 |
| Cordis services | `SessionStore` as a capability seam | §4.3, §55 |
| Agent vs. concrete loop | `Agent` handle vs. private loop (the session is owned by neither) | §10.1, §55 |

**Naming alignment.** §8.1 requires matching dsh's `SessionEventMap` names so
that replay and cross-harness tooling line up; the C++ enum is CamelCase and
the JSON `type` is the slash form. This spec keeps that contract in
`wire_name()` / `parse_event_type()`.

**Deliberate omissions** (accepted for v1, §55): no Cordis-compatible
configuration, no plugin dependency graph, no hot module replacement, no
browser client, no large-scale plugin ecosystem. The session layer is
headless and provider-agnostic (G1, G3; §2.1).

---

## 15. Test plan

Strategy is §44: unit, integration (fake LLM / fake FS / fake shell), golden,
replay, and a separate live PTY/real-LLM layer (§44, §45). The deterministic
layers run offline against the Fake LLM (§45); the live layer is opt-in and
API-key gated.

### 15.1 Unit tests

Against an in-memory fake `SessionStore` and a synchronous `EventBus`:

- **Identity / header**
  - `validateHeader` matrix: every `SessionKind` × parent/seed combination
    (I9); malformed id and non-absolute `cwd` rejected (S9); missing or
    non-directory `cwd` rejected (S14).
  - Store open failure (DB missing/locked/unopenable) rejects the open (S11).
  - `SessionHeader` JSON round-trip preserves every field; no boot nonce, no
    `ordinal`, no `archived` (field-set pin).
- **Event encoding**
  - `encode`/`decode` round-trip for **every** `EventType` (exhaustive over the
    `SessionEventMap`), including `ToolResult` truncation and `ContextCompaction`
    boundaries.
  - `wire_name` / `parse_event_type` total and inverse; unknown strings rejected
    (S3).
  - `Event` carries no `Sequence`; `EventRecord` does.
- **Append**
  - Sequence strictly increases across appends (I2); `append` returns the
    assigned sequence.
  - Duplicate `EventId` rejected (S1); non-holder append throws `LeaseLost`
    (I5, S4).
  - Persist-before-publish: a subscriber observes an event only after the fake
    store's commit flag is set (I4).
  - `updatedAt` advances to the last event timestamp (I22).
  - Concurrent appends from N threads produce a strictly increasing, gaps
    allowed (DB-global `AUTOINCREMENT`, §6.2/I2) log (I18).
- **`deriveMessages` purity**
  - Called twice on the same log yields byte-identical results; it performs no
    store/bus/clock access (inject a store that traps on read during the call)
    (I7).
  - Per-type projection rules: user, assistant, tool result, injected,
    compaction folding, `TurnCancelled` closing a turn, chunk non-projection
    (I14), tool-call non-projection (I12), permission/usage/subagent
    non-projection.
  - A cancelled turn with an unmatched `tool_use` synthesizes
    `ToolResult{outcome = Cancelled}` at projection, so every `tool_use` is
    paired (I12); nothing is appended to the log.
  - A failed turn (`TurnFailed`) with an unmatched `tool_use` synthesizes
    `ToolResult{outcome = Error}` at projection, distinguishable from the
    cancelled case (I11, I12); nothing is appended to the log.
  - Open-turn projection is deterministic and does not throw (S8).
  - Compaction chains compose deterministically (latest boundary wins for
    overlaps).
- **EventBus**
  - Per-session delivery preserves publish order; two sessions interleave
    without cross-contamination (I6).
  - `Subscription` RAII unsubscribes on destruction; moving a subscription
    transfers ownership.
  - A torn append is never observable: a reader re-reads to a consistent
    snapshot before projecting (S13, I18).
  - `SessionMailbox::drain` empties in order and reports empty (I20).
- **Snapshot**
  - `snapshot().messages == deriveMessages()` at the same head (I21); a stale
    snapshot is detected and discarded (S12 is exercised here).
- **Fork / replay**
  - Fork view is exactly the parent prefix plus own events (I10); fork boundary
    beyond the parent view rejected (S5); fork-of-fork composes.
  - Replay never calls `append` and never acquires the lease (I19).
- **Failure modes S1–S14** each have a dedicated unit test (§15.6).

### 15.2 Integration tests (Fake LLM, §45)

Driven by the deterministic `FakeLLM` (§45) so the whole loop is offline:

- **create → append → project**: user → assistant → tool call → tool result →
  assistant, then assert `deriveMessages()` equals the expected message list
  (§57 Step 3).
- **Persistence round-trip**: append against the real `SessionPersistence`,
  close, reopen, `read`, and assert identical `EventRange` and projection
  (§44 "session persistence").
- **Resume**: reconstruct after process restart; assert the projection matches
  and the agent rehydrates `Idle` (F10, I18).
- **Fork then diverge**: both sessions append independently; the child's
  projection begins with the parent prefix and the child's own turns.
- **Replay**: record a Fake-LLM stream, replay it, assert the same projected
  state (§44 replay).
- **Cancellation**: `TurnCancelled` closes the in-flight turn; the projection is
  deterministic and the session remains resumable (§34, F9).
- **Subagents**: spawn two subagents (distinct `SessionId`s), fan in; assert
  each has its own replayable log and the parent records `SubagentSpawned` /
  `SubagentFanIn` (F11, §20.25, §30).
- **Lease**: a second process cannot append; after expiry a steal succeeds and
  the original degrades to read-only (S4, §9.7) — shared with spec 02.
- **Path safety**: a create with a symlinked `cwd` stores the canonical path;
  a tool path attempt that would escape the root is rejected at the environment
  seam (§9.7, §18).

### 15.3 Golden tests

Given a deterministic event stream, render through the UI projection and
compare against the expected terminal representation (§44 "given event stream →
render → expected terminal representation"). The session layer supplies the
fixture logs; the renderers are spec 10. Golden fixtures must use only durable
events so they are replayable.

### 15.4 Replay tests

Same input log ⇒ same projected state (§44), including:

- long logs with multiple compactions,
- a cancelled turn,
- a failed turn (`TurnFailed`),
- a fork with an inherited prefix,
- a log ending in an open step (S8).

Replay tests assert `deriveMessages()` and, where a snapshot exists, that the
snapshot equals the recomputation (I21).

### 15.5 Live end-to-end tests (real LLM, PTY-driven, §44)

The live layer exercises the real product as a human would:

```text
spawn the real ymh binary under a PTY (forkpty / posix_openpt)
        ↓
write a prompt to the pty master (as typed keystrokes)
        ↓
read and parse the rendered terminal output (ANSI) from the pty master
        ↓
assert observable behavior: session created, assistant text streamed,
    tool call rendered, permission prompt handled, session resumes
```

Session-layer assertions:

- A `SessionStarted` is persisted as the first event; `updatedAt` advances.
- `UserMessage`, `AssistantMessage`, `ToolCall`, `ToolResult` appear in order
  in the session DB for the observed flow.
- Resume after killing and relaunching the process reconstructs the same
  conversation.
- The test is gated by an API key plus an explicit opt-in flag and is
  **skipped, not failed**, when absent, so the default suite stays hermetic
  (§44). It is tolerant of model nondeterminism: assert structure and
  invariants (events emitted, tools invoked, final state), never exact prose.
- It runs as a separate CI stage and is never part of the fast default test
  command (§44).

Milestone gating: MVP live tests cover the single-process flow; multi-workspace
PTY tests target Milestone 2 (§44, §57 Step 13, §58).

### 15.6 Failure-mode coverage matrix

| Failure | Test layer | Asserts |
|---|---|---|
| F1 | unit + integration | canonical `cwd`, no `chdir`, root-relative resolve (I8, I15) |
| F2 | integration | background `ASK` records a durable `PermissionDecision` |
| F3 | unit | mailbox drains and handle terminal before teardown (I20) |
| F5 | unit | `truncated` flag preserved through encode/decode and projection |
| F9 | integration | cancel targets one `TurnId` only |
| F10 | integration | resume opens `Idle`, appends nothing (I18) |
| F11 | integration | subagent id survives fan-in (I17) |
| S1 | unit | duplicate `EventId` rejected, not published |
| S2 | unit | non-monotonic sequence fails loud (I2) |
| S3 | unit | unknown type / bad JSON fails replay loud |
| S4 | integration | lease loss degrades to read-only |
| S5 | unit | fork boundary out of range rejected |
| S6 | unit | dangling parent rejected |
| S7 | unit | invalid compaction boundary rejected |
| S8 | unit + replay | open turn projects deterministically without throwing |
| S9 | unit | malformed id / header rejected |
| S10 | unit | oversized payload / invalid (non-object) metadata JSON rejected |
| S11 | unit | store open failure (missing/locked/unopenable DB) rejects the open |
| S12 | unit | stale snapshot detected and recomputed, never patched (I21) |
| S13 | unit + replay | a torn append is never projected; reader re-reads consistent (I18) |
| S14 | unit | missing / non-directory `cwd` rejects create/open and append |

#### Invariant coverage

Every invariant I1–I23 maps to at least one test, or is marked
covered-by-construction (CBC) with the reason it cannot be violated.

| Invariant | Test layer | Coverage |
|---|---|---|
| I1 append-only | unit | no mutating API; delete emits `SessionEnded` first |
| I2 sequence monotonicity | unit | appends strictly increase; non-monotonic rejected (S2) |
| I3 EventId uniqueness | unit | duplicate `EventId` rejected (S1) |
| I4 durable-before-observable | unit | persist-before-publish commit-flag test |
| I5 only lease holder appends | unit + integration | non-holder throws `LeaseLost`; re-check before `COMMIT` (S4) |
| I6 per-session ordered delivery | unit | two sessions interleave without cross-contamination |
| I7 deriveMessages purity | unit | byte-identical on repeat; trapping store proves no I/O |
| I8 cwd immutable/canonical | unit | canonical `cwd`; header round-trip; F1 test |
| I9 header consistency | unit | `validateHeader` matrix over every `SessionKind` |
| I10 fork is a prefix | unit | fork view == parent prefix ++ own events |
| I11 turn/step nesting | unit | `validateLog` rejects events outside an open turn |
| I12 tool pairing | unit | pairing rules + cancelled-turn synthesis (§4.5) |
| I13 durable/live separation | unit | live types absent from `SessionEventMap`; never written |
| I14 chunk/message precedence | unit | chunk non-projection with a matching `AssistantMessage` |
| I15 no ambient cwd | CBC + unit | session/tool code resolves only via `ExecutionEnvironment`; F1 test |
| I16 detach ≠ end; delete = end | unit | detach emits no `SessionEnded`; delete emits one |
| I17 subagent identity | integration | subagent id survives fan-in (F11) |
| I18 single in-process appender | unit | concurrent appends serialize; no torn read (S13) |
| I19 replay is read-only | unit | replay never calls `append` or acquires the lease |
| I20 mailbox drain before teardown | unit | `drain` empties in order and reports empty |
| I21 snapshot equivalence | unit | `snapshot().messages == deriveMessages()`; stale → recompute (S12) |
| I22 updatedAt tracks the log | unit | `updatedAt` advances to the last event timestamp |
| I23 metadata is inert | unit | `metadata` never influences `deriveMessages()` |

---

## 16. Decisions and open questions

### 16.1 Decisions (adjudicated by review)

These were previously open questions; independent review has adjudicated them,
so they are **pinned** and are no longer open.

- **(a) Persistence is service-driven.** `Session::append` calls
  `SessionStore::append`, which returns the store-assigned `Sequence`; the lease
  is re-checked **before** `COMMIT`; the `EventBus` subscription is
  **observer-only** and must not perform a second write. This is the pinned
  write path (§6.1, §10.1, I4, I5).
- **(b) `events.sequence` stays DB-global `AUTOINCREMENT` with gaps.** §9.2's
  single database-global counter is canonical. Within a session `Sequence` is
  strictly increasing; it is not dense, and gaps are expected and allowed
  (§6.2, I2).
- **(c) `AssistantChunk` stays durable; the producer coalesces.** Chunks remain
  durable so a streamed UI is replayable, but the producer coalesces deltas into
  **bounded batches** before append. The exact cadence (batch size / flush
  policy) defers to specs 02 and 06 (§4.5, I14).
- **(d) Fork defaults to copy-on-write / shared-prefix.** The default physical
  strategy is COW / shared-prefix with prefix-transparent reads; physically
  copied rows are reserved for export and cross-DB copies. The exact physical
  choice still defers to spec 02 (§9.3).
- **(e) `TurnCancelled` stays distinct.** Cancellation is a distinct durable
  event (wire `turn/cancel`), not folded into `TurnEnded{reason}` (§34, §4.3).
- **(f) `TurnFailed` is distinct from `TurnCancelled`.** A provider/step
  failure is a durable `turn/fail` event (`TurnFailed{turn, code, message}`),
  never a `TurnCancelled{reason}` (spec 08 §4.5). A failed turn pairs unmatched
  `tool_use` with `ToolResult{outcome = Error}`, while a cancelled turn uses
  `ToolResult{outcome = Cancelled}` (I11, I12, §6.3).

### 16.2 Open questions

None remain for this component; every prior open question is resolved in
§16.1. A new question reopens this subsection.

---

## 17. References

- `00-architecture.md` §4.2 (event stream is the runtime spine), §4.3 (services
  vs. events), §8.1–§8.4 (event system), §9.1–§9.11 (session architecture),
  §10.1 (`AgentHandle`), §11 (agent loop turn/step), §12 (LLM interface),
  §14 (tools), §18 (execution environment), §19 (permissions), §20.22
  (`SessionEnvelope`), §20.24–§20.26 (cancellation/focus/subagent duality),
  §30 (subagents), §31–§33 (context/compaction/usage), §34 (cancellation),
  §44 (testing strategy), §45 (Fake LLM), §49 (source tree), §54 (D1–D23,
  F1–F12), §55–§56 (dsh comparison), §57 Steps 1–3, §58 (milestones).
- `HANDOFF.md` §2 (the rule), §5 item 3 (`SessionHeader` finalization), §6 row
  01 (component plan), §7 (definition of verified).
- `DESIGN_STATUS.md` (written/verified tracker).

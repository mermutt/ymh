# 06 — Agent & Loop

**Component 06 of 10.** The agent is the runtime driver between the durable
session (`01`) and the replaceable LLM seam (`08`): an `Agent` handle with an
inbox, an `AgentRegistry` that owns the create/resume transaction, and an
`AgentLoop` that runs the turn/step cycle, coalesces streamed deltas into
durable `AssistantChunk`s, drives tools through permission policy, and closes
every turn with exactly one terminal event. The loop is **daemon-driven and
never focus-gated** (`§9.9`, `04` decision (m)): UI focus selects what a
supervisor shows, never what the daemon runs.

This document pins the `Agent` handle and its state machine, the inbox
(`send`/`followup`/`steer`/`inject`), the `AgentRegistry` create/resume
transaction, the `AgentLoop` step cycle and its durable-event mapping, the
single-active-session policy, the `LLMPool` acquire/release contract, subagent
spawn/fan-in, the context/compaction/token seams, invariants (`A1`–`A18`),
failure modes (`F1`–`F12` plus local `A-F#`), the DeepSeek Harness (dsh)
mapping, and the test plan.

It follows `00-architecture.md` (cited inline as `§n`), `01-session.md`
(`01 §n`), `02-persistence.md` (`02 §n`), `04-workspace-host-daemon.md`
(`04 §n`), `05-transport.md` (`05 §n`), and `08-llm-provider.md` (`08 §n`).
Where it cannot follow them it records the conflict under §14 rather than
choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** The invariants in this spec are numbered `A1`, `A2`, … (for
> "agent"), so they cannot collide with the `D1`–`D23` decisions in
> `00-architecture.md` §54 or with the `I#`/`P#`/`R#`/`H#`/`L#` namespaces of
> specs 01–05 and 08. Architecture decisions are always written with the `§54`
> prefix (`§54 D9`) and agent invariants bare (`A4`). Component-local failure
> modes are `A-F#` and cannot collide with the shared `F1`–`F12`.

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   Supervisor TUI (spec 10)          CLI (ymh run / ymh resume)       Live PTY test (§44)
        │ prompt/cancel/steer (05)          │ create/resume              │ drives TUI
        ▼                                    ▼                          ▼
   ┌──────────────────────────────────────────────────────────────────────────┐
   │  WorkspaceHost daemon  (spec 04)                                          │
   │    SessionManager (01 §8) · ResourceGovernor/LLMPool (04 §8)              │
   │    ┌────────────────────────────────────────────────────────────────┐    │
   │    │  AgentRegistry  (this spec)   one Agent per session             │    │
   │    │    ┌──────────────────────────────────────────────────────┐    │    │
   │    │    │  AgentLoop (this spec)                                │    │    │
   │    │    │    ContextAssembler (§31) · Compactor (§32)           │    │    │
   │    │    │    ToolRegistry (§14) · PermissionPolicy (§19)        │    │    │
   │    │    │    LLMProvider seam (§12, spec 08)                    │    │    │
   │    │    └──────────────────────────────────────────────────────┘    │    │
   │    └────────────────────────────────────────────────────────────────┘    │
   └──────────────────────────────────────────────────────────────────────────┘
             │ append / deriveMessages                │ durable events
             ▼                                         ▼
   Session event log (01/02)                    EventBus (01 §5, §8.3)
```

The agent never owns the session, the store, the transport, or the daemon: it
consumes a writable `Session` handle and an `LLMProvider`, and it is the
**producer** of the durable events the rest of the system projects (`§4.2`,
`01 §4.5`). It is headless (no FTXUI dependency, `§54 D14`, `§4.1`) and
daemon-resident (`§9.6`, `04 §3.7`).

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 06)

This spec pins:

- The `Agent` handle (`§10.1`): `id`/`session`, the inbox
  (`send`/`followup`/`steer`/`inject`), `cancel`, `dispose`, `whenIdle`, and
  the state machine (`AgentState` `§20.15`, `AgentStatus` `§9.8`).
- `AgentRegistry`: the create/resume transaction (session + agent + lease
  ordering), one agent per session, and disposal.
- `AgentLoop`: the turn/step cycle (`§11`), context assembly (`§31`),
  compaction (`§32`), the provider call, delta→`AssistantChunk` coalescing
  (`01 §16.1(c)`), the tool pipeline (`§14`, `§19`), and terminal events
  (`01` I11).
- The daemon-side **single-active-session** policy (`§9.9`, `04` (m)) and the
  activation predicate.
- The `LLMPool` acquire/release contract (`§5.9`, `08` (r), `04 §8`, F8).
- Subagent spawn/fan-in at the loop level (`§30`, `§20.25`, `01 §4.5`).
- Token accounting (`§33`) and the context-token estimator seam.
- Cancellation scoping (`§34`, F9) and failure-vs-cancel (`01`, `08` (k)/(p)).

### 1.3 Boundaries — deferred to other specs

- **Session/event-log semantics.** `Session`, `append`, `deriveMessages`,
  event payloads, and `SessionManager` are owned by 01; this spec calls them.
- **Store, lease, chunk bounds.** `SessionPersistence`, lease
  acquire/steal/release, and `max_chunk_batch`/`chunk_flush_interval` are
  owned by 02 (`02 §4.4`, `§5`, `§6.2`).
- **Daemon lifecycle, activation controls, caps.** Process topology, the
  `ResourceGovernor`, and `session.activate`/`session.suspend` are owned by
  04/05 (`04 §8`, `04` (m)).
- **Provider adapters, streaming algebra, retry.** Owned by 08; this spec is a
  consumer of `LLMProvider::stream` (`08 §3`).
- **Tool implementations and validation.** Owned by 07 (`§14`); this spec
  drives the registry and the permission gate.
- **Permission policy rules.** Owned by 09 (`§19`); this spec calls
  `evaluate()` and records `PermissionDecision`.
- **Rendering and attention.** The UI projection of `AgentState`, the waiting
  count, and the flash are owned by 10 (`§9.9`, `§20.23`).

### 1.4 Seam ownership relative to 01/02/04/05/07/08/09/10

| Concern | Owner | This spec's role |
|---|---|---|
| `Session` append/derive | 01 | the sole loop caller |
| `AssistantChunk` durability | 01 | **coalescing policy** lives here (`01 §16.1(c)`) |
| Lease acquire/steal/release | 02 | triggers acquire at create/resume; stops on lease loss |
| `max_chunk_batch` / flush interval bounds | 02 | picks the policy inside the bounds (`02 §6.2`) |
| Daemon activation / one-active-session | 04 | provides the predicate and `activate`/`suspend` |
| `agent.*` / `session.activate` RPC surface | 05 | maps the RPCs onto the `Agent` methods (`05 §7.4`/`§7.5`) |
| `LLMPool` handle | 06 (`§5.9`, `04 §8`) | pins the pool interface; acquires/releases a slot around each provider call |
| `ToolRegistry` / `Tool` | 07 | drives execution and records `ToolResult` |
| `PermissionPolicy` | 09 | calls `evaluate()`; records `PermissionDecision` |
| `LLMProvider` | 08 | calls `stream()`; maps failure to `TurnFailed` |
| `AgentState` UI projection / waiting count | 10 | owns the state machine; emits state changes |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId`, `TurnId`, `StepId`, `MessageId`, `ToolCallId`, `EventId`, and
`Usage` are frozen by `01 §2.1` / `01 §4.5`; they are reproduced for reference
and **not** redefined. Agent-local types:

```cpp
namespace ymh {

// Distinct from SessionId even though the registry holds a 1:1 mapping; a
// handle can outlive a session view and must not be silently interchangeable
// with it (decision (a)).
struct AgentId {
    std::string value;   // UUIDv4
    auto operator<=>(const AgentId&) const = default;
};

// Coarse lifecycle observable, exactly the two values §9.8 allows. Disposal
// is not a status (it removes the agent from the registry).
enum class AgentStatus : std::uint8_t {
    Idle,      // no driver active
    Running,   // cancellable work in flight
};

// Fine-grained loop state (§20.15). This is the state §9.9 sums for the
// waiting count; spec 10 projects it into the UI.
enum class AgentState : std::uint8_t {
    Idle,                  // no turn in flight, no pending work
    Thinking,              // awaiting the model (provider.stream in flight)
    CallingTool,           // executing one or more tools
    WaitingForPermission,  // a tool call is blocked on PermissionPolicy::Ask
    WaitingForInput,       // blocked on an explicit user input request
    Cancelling,            // cancel requested; turn not yet terminal
    Error,                 // terminal failure surfaced; awaits user action
};

// Inbox capacity is bounded (F5, §9.11); overflow is rejected, never dropped.
enum class InboxResult : std::uint8_t {
    Accepted,
    InboxFull,
    AgentDisposed,
};

} // namespace ymh
```

### 2.2 Agent error taxonomy

Agent operations do not throw across the handle boundary; failures are
returned as typed values or surfaced as durable/live events (L16-analogous
total typing). The loop's failures are **events**, not return codes, because
the session log is the authoritative trace (`§4.2`, D2).

```cpp
namespace ymh {

enum class AgentErrorCode : std::uint8_t {
    None,
    UnknownSession,        // resume/create target does not resolve (01 S6)
    LeaseHeldByOther,      // another live daemon holds the session lease (02 §5.3)
    LeaseLost,             // this process lost the lease mid-turn (01 S4, 02 §5.7)
    StoreUnavailable,      // store open/append failure (01 S11, 02 P-F#)
    InboxFull,             // bounded inbox overflow (§3.2)
    AgentDisposed,         // operation after dispose() (§3.4)
    StepLimitExceeded,     // turn exceeded max_steps (§5.8)
    ContextAssemblyFailed, // ContextAssembler could not build a request (§31)
    CompactionFailed,      // summarization failed and the context cannot fit (§32)
    ProviderFailed,        // provider terminal error (mapped from 08 §2.2)
    Cancelled,             // cooperative cancellation (§34)
    Internal,              // invariant violation
};

struct AgentError {
    AgentErrorCode code = AgentErrorCode::None;
    std::string    detail;   // redacted, short diagnostic
};

} // namespace ymh
```

`TurnFailed.code` is an `AgentErrorCode` (`01 §4.5` serializes it as the stable
`code` string). Provider terminal failures are mapped into this taxonomy by
`mapAgentError(LLMErrorCode)`: every provider code except `Cancelled` maps to
`ProviderFailed`, except `ContextLengthExceeded`, which first triggers the
one-shot compaction retry (§5.7) and, if the re-assembled context still
overflows, maps to `CompactionFailed` (`A-F10`). `Cancelled` is not a failure
and is never mapped to `TurnFailed` (A10). The provider's own `LLMErrorCode` and
message are preserved in the redacted `TurnFailed.message`; `08 §4.5` pins the
provider-side error and delegates the producer-side mapping to this spec.

---

## 3. The `Agent` handle (pinned)

### 3.1 Class shape

This is the `§10.1` surface, extended additively with status/state accessors
and explicit result types (decision (a)).

```cpp
namespace ymh {

// Context injected into the projection (01 §4.5 ContextInjected). By default it
// does not start a turn; `startsTurn` materialises it as a turn with origin
// Injection (01 §4.5, §31).
struct ContextMessage {
    Role        role = Role::System;  // role assumed when projected
    std::string text;
    bool        startsTurn = false;   // false => context-only
};

class Agent {
public:
    virtual ~Agent() = default;

    // ---- identity ----------------------------------------------------------
    virtual AgentId   id() const = 0;
    virtual SessionId session() const = 0;

    // ---- state -------------------------------------------------------------
    virtual AgentStatus status() const noexcept = 0;   // Idle | Running (§9.8)
    virtual AgentState  state()  const noexcept = 0;   // §20.15
    virtual bool        disposed() const noexcept = 0;
    virtual bool        hasPendingWork() const noexcept = 0;  // inbox non-empty or turn in flight

    // ---- inbox (§3.2) ------------------------------------------------------
    virtual InboxResult send(Message) = 0;               // start-or-queue
    virtual InboxResult followup(Message) = 0;           // always queue
    virtual InboxResult steer(Message) = 0;              // fold into the current turn
    virtual InboxResult inject(ContextMessage) = 0;      // context, not a turn

    // ---- control -----------------------------------------------------------
    virtual void cancel() = 0;                           // §34
    virtual void dispose() = 0;                          // teardown; removes the handle (§9.8)
    virtual void whenIdle(std::function<void()>) = 0;    // await quiescence (§9.8)
};

} // namespace ymh
```

`dispose()` and `whenIdle()` mirror the `AgentHandle` lifecycle in `§9.8`:
disposal removes the agent from the registry (it is not a status), and
`whenIdle()` lets callers await terminal quiescence before freeing a session.
The `status`/`state`/`disposed`/`hasPendingWork` accessors and the `followup`
inbox op are **additive** to the `§10.1` sketch (the `followup` op is listed as
a named durable extension in `00 §8.1`). `§10.1`'s method names and argument
lists are preserved; `send`/`steer`/`inject` return `InboxResult` instead of
`void` so that admission rejection is total and typed (decision (a)).
`id`/`session`/`cancel`/`dispose`/`whenIdle` are unchanged from `§10.1`.

### 3.2 Inbox semantics

Four operations, four distinct meanings. All are **posted to the loop's
executor** (`§35`) and are serialized against the turn driver; none mutates the
log directly (A1, A6).

| Op | Meaning | Durable event when consumed | If a turn is in flight | If idle |
|---|---|---|---|---|
| `send(Message)` | Submit a user message; start a turn if idle, else queue it | `UserMessage` + `TurnStarted{origin=User}` | queued as a follow-up (`TurnOrigin::FollowUp`) | starts a turn now |
| `followup(Message)` | Always queue; never interrupt | `UserMessage` + `TurnStarted{origin=FollowUp}` | queued | starts a turn now |
| `steer(Message)` | Fold into the **current** turn at the next step boundary | `UserMessage`, attributed to the current turn (`TurnOrigin::Steer`) | folded before the next step | starts a turn now (`origin=Steer`) |
| `inject(ContextMessage)` | Add context to the **next** assembly; `startsTurn=false` is not a turn, `startsTurn=true` starts one | `ContextInjected` (+ `TurnStarted{origin=Injection}` when `startsTurn`) | folded at the next assembly | folded at the next assembly (or starts a turn when `startsTurn`) |

Normative rules:

- **FIFO.** Follow-ups and steers are consumed in submission order; a `steer`
  never overtakes an earlier follow-up (A6). Multiple steers before a step
  boundary are concatenated in order.
- **Exactly once.** An accepted op is consumed exactly once and produces
  exactly one durable event; a rejected op (`InboxFull`, `AgentDisposed`)
  produces none (A6).
- **Bounded.** The inbox is a bounded queue (`maxInbox`, `§9.11` F5). Overflow
  returns `InboxFull`; it never silently drops (`A-F8`).
- **`send` vs `followup`.** `send` is the ergonomic "type here" entry
  (start-or-queue); `followup` is the explicit "queue after this turn" entry.
  They differ only when the agent is idle (decision (b)).
- **`send` origin is state-dependent.** The turn `send` starts uses
  `TurnOrigin::User`; when `send` is queued behind an in-flight turn it is
  consumed as `TurnOrigin::FollowUp` (`01 §4.5`). `agent.prompt` in `05 §7.5`
  is this operation, so its origin is likewise state-dependent.
- **`steer` timing.** A steer is visible to the **next** provider call, never
  mid-stream: the current streamed response is not mutated (`§10.1`,
  `01 §4.5` `TurnOrigin::Steer`).
- **`inject` scope.** Injected context is a projection input
  (`01 §6.3`), not a turn; with the default `startsTurn == false` it does not by
  itself create pending work, so it does not wake a suspended session (A7).
- **`inject` turn-starting form.** `ContextMessage::startsTurn == true` makes
  the injection turn-starting: it is recorded as `ContextInjected` and begins a
  turn with `TurnOrigin::Injection` (`01 §4.5`, `§31`). The default
  `startsTurn == false` is context-only and never starts a turn, so an injection
  cannot cause an unexpected LLM burst. The producer that sets `startsTurn`
  (background/scheduled producers, `§9.9` Phase B) is out of scope here.
- **After `dispose()`.** Every inbox op returns `AgentDisposed` (A14).

`followup`/`steer`/`send` require the session to be writable: if the lease is
lost, the loop refuses to start a turn and surfaces `LeaseLost` (A11).

### 3.3 State machine

Two views of one machine:

- `AgentStatus` (`§9.8`) is the **coarse** observable: `Running` iff a turn
  driver is active, i.e. `state ∈ {Thinking, CallingTool,
  WaitingForPermission, WaitingForInput, Cancelling}`; `Idle` otherwise.
  `WaitingForInput` holds a blocked in-flight turn, so it is `Running`
  (**blocked**) and remains cancellable via `cancel()` (§3.5, decision (c)).
- `AgentState` (`§20.15`) is the **fine-grained** state the loop drives and
  spec 10 projects. It is the value §9.9 sums for the waiting count.

```text
        ┌──────────────────────────────────────────────────────────────┐
        │                                                              │
        ▼                                                              │
      Idle ──send/followup/activate──► Thinking ──tool calls──► CallingTool
        ▲                                │  ▲                        │
        │                                │  └──── tool results ──────┘
        │                                │
        │                                ├── Ask ──► WaitingForPermission
        │                                │              │ allow ─► CallingTool
        │                                │              │ deny  ─► Thinking (ToolResult Denied)
        │                                │
        │                                ├── no tool calls ──► TurnEnded ──► Idle
        │                                ├── provider error ──► Error
        │                                └── step limit ──────► Error
        │
        ├── cancel ──► Cancelling ──► TurnCancelled ──► Idle
        └── WaitingForInput ──user input──► Thinking
```

Transitions are driven by durable/live loop events (A3):

| From | Trigger | To | Durable side effect |
|---|---|---|---|
| `Idle` | inbox has work / activation | `Thinking` | `TurnStarted` |
| `Thinking` | model returned tool calls | `CallingTool` | `AssistantMessage`, `ToolCall`(s) |
| `Thinking` | model ended the turn | `Idle` | `AssistantMessage`, `StepEnded`, `TurnEnded` |
| `Thinking` | provider terminal error | `Error` | `TurnFailed` (08 (k)) |
| `Thinking` | step limit | `Error` | `TurnFailed{StepLimitExceeded}` |
| `CallingTool` | all results appended | `Thinking` | `ToolResult`(s), `StepEnded` |
| `CallingTool` | policy says `Ask` | `WaitingForPermission` | (live `PermissionRequested`) |
| `WaitingForPermission` | decision `Allow`/`AllowAlways` | `CallingTool` | `PermissionDecision` |
| `WaitingForPermission` | decision `Deny` | `Thinking` | `PermissionDecision`, `ToolResult{Denied}` |
| `WaitingForInput` | user answers | `Thinking` | `ContextInjected`/`UserMessage` per answer |
| any `Running` | `cancel()` | `Cancelling` | (flush pending chunks) |
| `Cancelling` | cancellation observed | `Idle` | `TurnCancelled` |
| `Error` | user acknowledges / retries | `Idle` | (none) |

**Waiting set (for attention, `§9.9`).** `WaitingForPermission` and
`WaitingForInput` are the "needs input" states; `Error` also needs attention.
The exact count/flash arithmetic is spec 10 (`§9.9`, `§20.23`); the loop owns
only the transitions that produce the edges.

`WaitingForInput` is entered **only** when work is pending but cannot proceed
without a user answer (e.g. an explicit agent question); a permission prompt does
**not** enter it — the agent stays in `WaitingForPermission` (09 decision (q)).
A session with no pending work is `Idle`. The precise
`Idle`/`WaitingForInput` boundary is pinned by decision (c).

### 3.4 `dispose()` and `whenIdle()`

- `dispose()` is the teardown call (`§9.8`). It: (1) marks the agent disposed
  so every inbox op returns `AgentDisposed`; (2) requests cancellation of any
  in-flight turn; (3) flushes the pending chunk batch before the closing event
  (`02 §6.2`); (4) drops queued follow-ups/steers (injected context already
  consumed is unaffected); (5) releases the session lease via `SessionHandle`
  (`01 §7`); (6) removes the agent from the `AgentRegistry`. It is not a third
  status (`§9.8`).
- `whenIdle(fn)` registers a continuation invoked when the agent reaches a
  terminal quiescent state: no turn in flight, inbox empty, and no pending
  chunk flush. If already quiescent, `fn` is invoked immediately on the
  caller's executor. Callbacks fire in registration order; each fires once.
- **Terminal for F3.** The F3 tombstone (`§8.3`, `§9.8`) waits for
  `disposed() && status() == Idle` **and** the per-session mailbox to drain
  (`01 §5`); `whenIdle` is the loop's half of that contract.
- Disposal during a turn is always safe: the in-flight provider call is
  cancelled (`§34`), its chunks are flushed, and the turn closes with
  `TurnCancelled` (A14, `A-F12`).

### 3.5 Cancellation

- `cancel()` requests cancellation of the **in-flight turn only**, scoped to
  this session (F9, `§34`, `§20.24`). It transitions the agent to `Cancelling`
  and fires the turn's `CancellationToken`; the provider aborts (`08 §3.6`).
- On cancellation the loop flushes the pending chunk batch, appends
  `TurnCancelled{turn, reason="user"}`, and returns to `Idle`. Cancellation is
  **not** an error: no `TurnFailed`, no live `Error` (`01 §16.1(e)`,
  `08` decision (k)).
- Queued follow-ups **survive** a cancel; `dispose()` drops them (A9).
- A second cancel while `Cancelling` is a no-op (idempotent). Force
  termination (second Ctrl-C) is deferred (`§34`).
- The daemon may cancel a turn when it moves the active slot (`§9.9`); that
  uses `reason="superseded"` and is initiated by `suspend()` (§6), not by UI
  focus.

---

## 4. `AgentRegistry`: create / resume transaction

### 4.1 Class shape

```cpp
namespace ymh {

class AgentRegistry {                 // one instance per WorkspaceHost (decision (l))
public:
    AgentRegistry(SessionManager&, ResourceGovernor&, ToolRegistry&,
                  PermissionPolicy&, ProviderRegistry&, ContextAssembler&,
                  AgentConfig);

    // ---- lifecycle ---------------------------------------------------------
    // Atomic from the caller's perspective: on success the session is
    // writable, the agent is registered, and the lease is held; on failure no
    // agent is registered (see §4.2/§4.3).
    std::expected<AgentId, AgentError> create(SessionOptions);
    std::expected<AgentId, AgentError> resume(SessionId);

    // ---- lookup / teardown -------------------------------------------------
    Agent&                 get(AgentId);
    Agent*                 find(SessionId) noexcept;   // at most one (A1)
    void                   dispose(AgentId);
    std::vector<AgentId>   list() const;
    std::size_t            activeCount() const;        // status == Running

private:
    std::unordered_map<SessionId, AgentId> bySession_;  // 1:1 (A1)
};

} // namespace ymh
```

`AgentRegistry` is the daemon-level **agent-lifetime registry**: it owns agent
construction, the create/resume transaction, and disposal, and it composes
`SessionManager` (01 §8), the lease (02), and the loop construction. It never
appends events itself; `SessionManager::createSession` and the loop do
(01 §9.1). `SessionManager::agent(SessionId)` (01 §8) is a **thin delegate** to
`AgentRegistry::find(SessionId)`/`get()`: the registry remains the single owner
of agent lifetime, and the manager method only forwards (decision (l)).

### 4.2 `create` transaction (pinned order)

```text
create(options) -> AgentId
  1. canonicalize options.cwd                      (01 §9.1, §9.7, S14)
  2. SessionId id := SessionManager::createSession(options)
       └─ store.create(header)  # acquires the session lease (02 §5.3 acquire)
       └─ session.append(SessionStarted)           (01 §9.1)
  3. construct the writable Session bound to (id, lease)
  4. construct the AgentLoop bound to the Session, provider, tools, policy
  5. registry.bySession_[id] := agent.id; store the agent
  6. daemon adds the open-set junction row               (03 R16; LAST)
       registry.addSession(workspace, id) under the pending_mutation marker
  7. return AgentId        # Idle; no turn starts until there is pending work
```

Ordering rationale: the lease must be held **before** any append (01 I5), and
the agent must not start a turn before the session is writable. So the order
is session+lease → loop construction → registration → work (A11). The junction
row is last so the durable session exists before it is advertised in the open
set (`03 R16`).

Failure behavior:

- **Steps 1–2 fail** (bad cwd, store open failure, lease held by another live
  daemon): return the mapped `AgentError`; **no session row is created** and no
  agent is registered.
- **Steps 3–5 fail** (loop construction error): the session already exists and
  is durable (its `SessionStarted` is committed). The registry **releases the
  lease**, registers **no** agent, and returns the error. The session is left
  intact and resumable; create never deletes a session it did not fully
  establish (decision (e)).
- **Step 6 fails** (junction insert error): the session and agent remain valid
  and usable; the create still returns success (`std::expected` holds the
  `AgentId`), and the failure is surfaced as a **live `registry_pending` event**
  (`§8.1`), not as a return value. The daemon reconciles the junction at startup
  (`03 §8`, R-F12). The durable log is the source of truth (D21); the open set is
  derivable.
- Create/resume for a `SessionId` that already has an agent is **idempotent**:
  it returns the existing `AgentId` (A1).

### 4.3 `resume` transaction (pinned order, F10)

```text
resume(id) -> AgentId
  1. header := store.load(id)                 # !header -> UnknownSession (01 S6)
  2. log    := store.read(id); validate header+log (01 I9, S2, S3)
  3. store acquires the lease (02 §5.3 acquire; may steal a dead holder)
       └─ HeldByOther (live) -> LeaseHeldByOther (A-F15)
  4. Session::resume(header, store, bus)      # writable, projection restored (01 §9.2)
  5. construct AgentLoop; register; state := Idle
  6. return AgentId        # F10: no turn is started by resume
```

- **F10 — resume is `Idle`.** Resume appends nothing (01 I18) and starts **no**
  turn, so a restart cannot spawn an auto-resume LLM burst (`§9.9`, `01 §9.2`).
  Rehydration is by construction (`deriveMessages`, `01 §6.3`); the first turn
  begins only when the session is activated with pending work (§6).
- **Open turn at crash.** If the log ends mid-turn (crash), `deriveMessages`
  projects deterministically up to the last event (01 S8, `01 §6.3`); the
  resumed agent is `Idle` and does **not** auto-continue the orphaned turn
  (`A-F16`). A new turn may be started by a new inbox message/activation.
- **Lease held by a live daemon.** `resume` returns `LeaseHeldByOther`; no
  agent is registered (`A-F15`).

### 4.4 One agent per session

- `bySession_` is 1:1 (A1): a session has at most one driver at a time.
- `dispose(AgentId)` removes the mapping and releases the lease (§3.4); the
  session remains durable and can be resumed later.
- `find(session)` returns `nullptr` after disposal; `list()` never contains
  disposed agents.

---

## 5. `AgentLoop` (pinned)

### 5.1 Turn/step cycle

```cpp
namespace ymh {

class AgentLoop {
public:
    // Runs until the inbox is drained or the agent is suspended/disposed.
    // Daemon-driven: called by the activation policy (§6), never by UI focus.
    Task<void> run(CancellationToken sessionCancel);

    // Activation controls (04 (m), spec 05 RPC).
    bool hasPendingWork() const noexcept;
    void activate();     // begin running the next pending turn
    void suspend();      // gracefully cancel the in-flight turn; keep the queue

    AgentState state() const noexcept;
    AgentStatus status() const noexcept;
};

} // namespace ymh
```

The fundamental unit is the turn/step taxonomy of `§11`:

```text
Turn
 ├── Step
 │    ├── ContextAssembler.assemble()          (§31)
 │    ├── optional Compactor.run()             (§32)
 │    ├── LLMPool.acquire()                    (§5.9, 04 §8, 08 (r))
 │    ├── LLMProvider.stream(request, sink, cancel)   (08 §3)
 │    ├── sink: deltas -> coalesced AssistantChunk batches   (01 §16.1(c))
 │    ├── append AssistantMessage (assembled, canonical)
 │    ├── zero or more tool calls: policy -> execute -> ToolResult
 │    └── StepEnded
 ├── Step ...
 └── Turn end (exactly one terminal event)
```

Pinned per-turn algorithm (normative):

```text
runTurn(turn, origin):
  append TurnStarted{turn, origin}
  loop:
    step := nextStep
    append StepStarted{turn, step}

    # 1. context (§31/§32)
    messages := contextAssembler.assemble(session, turn, step)   # inbox folds in
    if estimateTokens(messages) > compactionThreshold:
        compactor.run(messages)          # may append ContextCompaction; best-effort
        messages := contextAssembler.assemble(...)   # re-assemble after compaction

    # 2. provider call, one pool slot (08 (r), §5.9); §5.7 one-shot retry
    request := buildRequest(messages, model, options)   # LLMRequest (§12, 08 §3.1)
    for attempt in {First, CompactionRetry}:
        slot := llmPool.acquire(cancel)  # cancellable; bounded by F8, §5.9
        response := provider.stream(request, sink, cancel)
        llmPool.release(slot)            # always, on every path (A12)
        if response.outcome != Failed or response.code != ContextLengthExceeded:
            break
        if attempt == CompactionRetry:
            break                        # still overflowing -> §5.7 CompactionFailed
        compactor.run(messages)          # one-shot compaction (§5.3, §5.7)
        messages := contextAssembler.assemble(session, turn, step)  # re-assemble
        request  := buildRequest(messages, model, options)

    # 3. flush chunks; append the canonical assistant message
    flushChunks()                        # barrier: before AssistantMessage
    append AssistantMessage{messageId, content, usage}

    # 4. terminal provider outcomes (branch on the code before the generic return)
    if response.outcome == Cancelled:
        flushChunks(); append TurnCancelled{turn, "user"}; return
    if response.outcome == Failed:
        flushChunks(); append TurnFailed{turn, mapAgentError(response.code), message}
        emit live Error; return

    # 5. tool pipeline (§14, §19)
    if response.tool_calls is empty:
        append StepEnded; append TurnEnded{turn}; return
    for call in response.tool_calls:
        append ToolCall{call}
        decision := policy.evaluate(call, session)
        if decision == Ask:
            state := WaitingForPermission
            await user decision          # wait only; the append below is the single write
        append PermissionDecision
        if denied:
            append ToolResult{call, outcome = Denied}; continue
        state := CallingTool
        result := toolRegistry.execute(call, ctx)    # §14
        append ToolResult{call, result}
    append StepEnded
    if step >= maxSteps:
        flushChunks(); append TurnFailed{turn, "StepLimitExceeded"}; emit live Error; return
```

The agent **never** depends on provider-specific streaming formats (`§12`);
it consumes the `StreamEvent` algebra of `08 §3.2`.

### 5.2 Context assembly (`§31`) — seam

```cpp
namespace ymh {

struct TurnContext {
    TurnId                    turn;
    StepId                    step;
    std::vector<Message>      inbox;       // steers/follow-ups folded for this step
    std::vector<ContextMessage> injected;  // ContextInjected pending
};

class ContextAssembler {
public:
    // Pure w.r.t. durable state: reads the session projection and the turn
    // context, returns the LLM message list. No I/O, no clock, no randomness.
    // Responsibilities (§31): history selection, system sections, workspace
    // context, tool schemas, injected context, current user message.
    virtual std::vector<Message> assemble(const Session&, const TurnContext&) const = 0;

    // Tool schemas are assembled from the ToolRegistry (§14); the assembler
    // does not execute tools.
    virtual std::vector<ToolSchema> tools() const = 0;
};

} // namespace ymh
```

- The assembler is the **only** place history is selected/trimmed; the provider
  receives a complete message list (`08 §3.1`).
- Assembly failure (e.g. an unresolvable tool schema) is `A-F9`:
  `TurnFailed{ContextAssemblyFailed}` **before** any provider call.

### 5.3 Compaction (`§32`) — seam

```cpp
namespace ymh {

class Compactor {
public:
    // When the estimate exceeds the threshold, summarize the projected prefix
    // and return a ContextCompaction payload; original events are untouched.
    virtual std::optional<payload::ContextCompaction>
    run(const Session&, const std::vector<Message>&, CancellationToken) = 0;
};

} // namespace ymh
```

- Compaction **never deletes** original events (`§32`, D2); it appends
  `payload::ContextCompaction{boundary, summary, tokenEstimate, model,
  createdAt}` and `deriveMessages` folds it (`01 §4.5`, `01 §6.3`).
- The summarization call goes through the same `LLMProvider` seam and the same
  `LLMPool` discipline as a normal request (`08` (r), A12); its model is
  resolved via the per-request override (`08 §5.2`).
- Compaction is **best-effort** (decision (h)): if it fails and the context
  still fits the model, the turn proceeds uncompacted; if it cannot fit,
  `TurnFailed{CompactionFailed}` (`A-F10`).
- After a compaction, `checkpoint(session)` is refreshed per `02 §6.4`.

### 5.4 Delta → durable `AssistantChunk` coalescing (owned here)

The provider emits fine-grained deltas; **this loop is the producer** that
coalesces them into bounded `AssistantChunk` batches before append
(`01 §16.1(c)`; bounds from `02 §6.2`).

```cpp
namespace ymh {

class ChunkCoalescer {          // one per in-flight step
public:
    ChunkCoalescer(Session&, MessageId, AgentConfig);

    // Called serially from the provider's StreamSink (08 §3.3).
    // Never blocks, never throws, never touches the store directly.
    void onText(std::string_view);
    void onReasoning(std::string_view);

    // Barrier: flush before any non-chunk append (AssistantMessage, ToolCall,
    // StepEnded, TurnEnded/Cancelled/Failed) and before closing a turn.
    void flush();

private:
    std::vector<payload::AssistantChunk> pending_;   // bounded by max_chunk_batch
};

} // namespace ymh
```

Pinned policy (inside the `02 §6.2` bounds, decision (d)):

- **Flush when any of:** the pending batch reaches `max_chunk_batch`
  (default 32), `chunk_flush_interval` elapses (default 100 ms), or a barrier
  event is about to be appended.
- A flush is one `Session::appendBatch` (`02 §4.4`): one transaction; the
  committed chunks are published in order (01 I4/I6).
- Each chunk is `payload::AssistantChunk{message, index, text, kind}` where
  `message` is the step's `MessageId` (UUIDv4) and `index` is monotonic within
  that message; `kind ∈ {Text, Reasoning}` (`01 §4.5`).
- Coalescing **never reorders**, never merges distinct message ids, and never
  drops a delta (A4, `02 §6.2`). Chunks are durable so a streamed UI is
  replayable (01 I14).
- The flush timer runs on the daemon's main loop (`§35`, `04 §9`), not on a
  thread per session.
- On cancel/close, `flushChunks()` runs **before** the closing event so the log
  is ordered (A5, `02 §6.2`).
- A store error during flush is `A-F11`: the turn fails with
  `TurnFailed{StoreUnavailable}`; nothing is silently dropped.

The coalescer's sink adapter maps `08 §3.2` events:

```text
TextDelta        -> coalescer.onText(text)
ReasoningDelta   -> coalescer.onReasoning(text)      # only if capability (08 §5.4)
ToolCallStarted  -> (barrier) flush; assembler.onStarted
ToolCallDelta    -> assembler.onDelta
ToolCallFinished -> assembler.onFinished             # parsed object (08 §4.2)
UsageEvent       -> pending TokenUsage for the step (appended before StepEnded)
Finished/Error   -> (barrier) flush; terminal handling
```

### 5.5 Tool pipeline

- For each assembled tool call (`08 §4.2`), the loop appends a durable
  `payload::ToolCall` and then runs the `§46` chain:
  **model output → tool parser → permission policy → execution environment**.
  It never executes an unvalidated call (A18).
- `PermissionPolicy::evaluate` (`§19`) returns `Allow`/`Deny`/`Ask`. `Ask`
  transitions to `WaitingForPermission` and awaits a durable
  `PermissionDecision`; the decision is appended in all cases (F2: a background
  session that hits `ASK` records the decision and is surfaced via attention,
  never deadlocking invisibly).
- `Deny` yields `ToolResult{outcome = Denied}` and the step continues; the
  model sees the denial (`01` I12).
- Execution goes through `ToolRegistry`/`Tool::execute` (`§14`) with the
  session's `ToolContext` (`§14.2`); the result is appended as
  `payload::ToolResult` with `outcome ∈ {Ok, Error, Denied, Cancelled}`
  (`01 §4.5`).
- A tool that throws/fails yields `ToolResult{outcome = Error}`; the turn
  continues (`A-F5`), so the model can recover.
- Tools emit through `ToolContext::emit(Event)` (`§14.2`), which routes to the
  owning session; the loop remains the only turn driver (A1).

### 5.6 Terminal events

Every turn ends with **exactly one** terminal event (`01` I11):

```text
TurnEnded{turn}                       # natural completion
TurnCancelled{turn, reason}           # cancellation (user/superseded/shutdown)
TurnFailed{turn, code, message}       # provider/loop failure
```

The loop is the sole appender of these for its session (A2). Before any
terminal event, `flushChunks()` runs (A5). Failure and cancellation are
distinct (`01 §16.1(e)(f)`): a failed turn's unmatched `tool_use` blocks
project to `ToolResult{outcome = Error}`; a cancelled turn's project to
`ToolResult{outcome = Cancelled}` (`01` I12).

### 5.7 Provider failure → `TurnFailed`

- A terminal provider `StreamError`/`Failed` response (`08 §2.2`, `08 §4.5`)
  maps to `TurnFailed{turn, mapAgentError(code), message}`, where `code` is the
  provider's `LLMErrorCode` and the emitted field is the `AgentErrorCode` of
  §2.2; `message` is the redacted provider message (`08` decisions (k)/(p)).
  The mapping is `mapAgentError`: `ContextLengthExceeded` is handled by the
  one-shot compaction retry below; every other provider code except `Cancelled`
  maps to `ProviderFailed`. A live `Error` event is emitted for the UI (`§8.1`).
  Failure is never recorded as `TurnCancelled` (`08` (p), `01` §16.1(f)).
- The loop does **not** retry a mid-stream failure; retry is the provider's
  pre-first-event concern (`08` decision (d), L7). The one exception is the
  single `ContextLengthExceeded` compaction retry below; any other new attempt
  is a new turn, started explicitly.
- `ContextLengthExceeded` from the provider (`08 §2.2`) triggers **one**
  compaction attempt and a re-assembly (§5.1); if the re-assembled context
  still overflows, the turn fails with `TurnFailed{CompactionFailed}` (`A-F10`,
  `§5.3`).
- `Cancelled` is never mapped to `TurnFailed` (§3.5, A10).

### 5.8 Step limit and termination

- `maxSteps` (config `[agent] max_steps`, default 100, `§37`) bounds the steps
  per turn. Exceeding it is a **loop-policy failure**: flush, emit a live
  `Error`, append `TurnFailed{turn, "StepLimitExceeded"}` (decision (g)).
- A turn also terminates when the model returns no tool calls (natural
  `TurnEnded`) or on cancel/failure as above.

### 5.9 `LLMPool` (pinned)

`04 §8` exposes `ResourceGovernor::llm()` as an opaque `LLMPool&` and `08`
decision (r) defers the concrete type to this spec; this subsection pins it.
The pool owns the bounded set of LLM worker slots for one daemon (`§9.11`,
F8); the provider owns no executor or threads (`08 §9`, L13), and the **caller**
brackets every provider call (A12).

```cpp
namespace ymh {

// One pool per WorkspaceHost; held by the daemon's ResourceGovernor (04 §8)
// and injected into the loop. maxConcurrency comes from ResourceCaps (§9.11).
class LLMPool {
public:
    // RAII slot: at most one in-flight provider call per held slot. Moving
    // transfers the slot; destruction returns it to the pool (release).
    class Slot {
    public:
        Slot() noexcept;                       // empty slot
        Slot(Slot&&) noexcept;
        Slot& operator=(Slot&&) noexcept;
        ~Slot();                               // releases if held (A12)
        bool held() const noexcept;
    };

    explicit LLMPool(std::size_t maxConcurrency);

    // Cancellable acquire. Completes with a slot when one is free, or with
    // nullopt when `cancel` fires first (never blocks an aborted turn).
    Task<std::optional<Slot>> acquire(CancellationToken cancel);

    std::size_t capacity() const noexcept;
};

} // namespace ymh
```

- **Slot type.** A `Slot` is move-only and RAII; there is no public manual
  `release()` — dropping the `Slot` (or its scope) returns the slot. Releasing
  twice is impossible by construction.
- **Acquire/release discipline.** `acquire(cancel)` is called immediately
  before `provider.stream(...)` (and each compaction call), and the returned
  `Slot` is destroyed immediately after, on **every** path — completion,
  cancellation, failure (A12, §5.1).
- **Exhaustion.** When all `maxConcurrency` slots are held, `acquire` suspends
  the calling coroutine on a FIFO waiter queue and resumes it when a slot
  frees; it never spawns a thread and never busy-waits. Waiters are bounded by
  the number of live turns (one active session per workspace, §6, plus capped
  subagents, §7), so the queue is bounded by the host caps (F8).
- **Cancellable acquire.** If `cancel` fires while waiting, `acquire` completes
  with `std::nullopt` and removes the waiter from the queue; the turn then
  closes as cancellation (A10). A slot is never leaked to a cancelled waiter.
- **Placement.** The concrete pool is constructed by the daemon from
  `ResourceCaps` and reached through `ResourceGovernor::llm()` (`04 §8`); the
  loop holds only the injected reference.

---

## 6. Single-active-session policy (daemon-driven)

The daemon keeps exactly one active session per workspace; activation is
serialized by the daemon (`§9.9`, `04` (m)).

```text
a session is ACTIVATABLE  iff  hasPendingWork()  and  not blocked
                             (inbox non-empty or a turn already in flight)
the daemon activates the next activatable session when the active one
  ends (Idle) or blocks (WaitingForPermission | WaitingForInput)
moving the active slot GRACEFULLY CANCELS the current turn (reason "superseded")
attachment, detach, and UI focus NEVER gate, start, or stop work
```

- `hasPendingWork()` is the loop's predicate: inbox non-empty **or** a turn in
  flight. `inject` alone does not create pending work (A7).
- `activate()` begins the next pending turn; it is **idempotent** (a second
  call while running is a no-op, `A-F13`).
- `suspend()` gracefully cancels the in-flight turn (`TurnCancelled{reason =
  "superseded"}`), flushes chunks, returns to `Idle`, and **preserves** the
  follow-up queue (`§9.9`).
- One active session per workspace is enforced by the daemon's activation
  serializer, not by a global lock in the loop (A8).
- `session.activate`/`session.suspend` are explicit RPC controls (spec 05,
  `04` (m)); they are not a mirror of UI focus.

---

## 7. Subagents

A subagent is an ordinary `Agent` instance (`§30`, `§54 D9`): it gets its own
session, context, tool set, execution environment, and policy. No special loop
architecture is needed.

- **Spawn.** A tool spawns a subagent via the registry; the parent
  appends `payload::SubagentSpawned{subagent, task}` (`01 §4.5`) and the child
  gets its own `SessionStarted` in its own log (F11, `01` I17).
- **Fan-in.** On the child's terminal state, the parent appends
  `payload::SubagentFanIn{subagent, outcome, summary}` with
  `outcome ∈ {Completed, Failed, Cancelled}` (`01 §4.5`).
- **Identity duality (F11).** The parent id routes display; the child keeps its
  own `SessionId`/`AgentId` for durability (`§20.25`, `01` I17). The parent log
  holds only the spawn/fan-in edges.
- **Synchronous v1 (decision (i)).** The spawning tool awaits the child's
  terminal state via `whenIdle()`; the parent stays in `CallingTool` until
  fan-in. Detached/async subagents and the §20.25 delta coalescer are deferred.
- **Failure.** A failed/cancelled child records `SubagentFanIn` with the
  matching outcome; the tool result reflects it and the parent turn continues
  (`A-F17`).
- **Caps.** Concurrent subagents are bounded by the host's caps (`§9.11`, F8);
  each child's provider calls take their own `LLMPool` slots (A12).

---

## 8. Token accounting

- Provider-reported usage (`08 §3.5`, `§33`) is appended by the loop as
  `payload::TokenUsage{usage, turn}` **before `StepEnded`** (`01 §4.5`). Usage is never
  fabricated; absent usage ⇒ no event (`08` L8).
- **Context-token estimation** for the compaction threshold is owned here (08
  §3.5): a `TokenEstimator` seam estimates the assembled message list.

```cpp
namespace ymh {

class TokenEstimator {
public:
    // Estimate only; the provider's reported Usage is canonical (§33).
    virtual std::size_t estimate(const std::vector<Message>&) const = 0;
};

} // namespace ymh
```

- The estimator never replaces reported usage; it only decides whether to
  compact before a call (`§31`, `§32`).

---

## 9. Concurrency and threading

- **One main Asio loop per daemon** (`§35`, `04 §9`). The agent loop runs as
  coroutine tasks on that loop; there is no thread per session and no thread
  per turn.
- **One driver per session** (A1). The loop is the session's only turn writer;
  inbox ops and activation are posted onto its executor, so they serialize
  against the driver.
- **`LLMPool` discipline** (A12, `08` (r), `04 §8`, §5.9): the caller acquires
  one slot around each `provider.stream` (and each compaction call) and releases
  it on **every** path — completion, cancellation, failure. The provider is
  synchronous and never acquires/releases (`08 §9`, L13).
- **In-process writes** are serialized by 01 I18 (`appendMutex_`) and 02 §8
  (writer mutex); the loop adds no second write path. `appendBatch` flushes are
  one transaction (`02 §4.4`).
- **Chunk flush timer** is a loop timer, not a thread (`02 §6.2`, `04 §9`).
- **Cross-process** serialization is the lease (02); a lease loss stops the
  loop (`02 §5.7`, A11).
- **No UI dependency**: the loop depends on no FTXUI type; state changes are
  core events/enums projected by spec 10 (A17, `§54 D14`).

---

## 10. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**A1 — One driver per session.** At most one turn runs per session at a time;
the loop is the session's sole turn appender; `AgentRegistry` holds a 1:1
`SessionId ↔ AgentId` mapping. (`§10.1`, `§11`, `01` I18)

**A2 — Exactly one terminal event per turn.** Every turn ends with exactly one
of `TurnEnded`/`TurnCancelled`/`TurnFailed`, appended by the loop. (`§11`,
`01` I11)

**A3 — Durable before live.** The loop appends durable events before emitting
the corresponding live state change; a consumer never observes a live
transition replay could not reproduce. (`§8.1`, `01` I4)

**A4 — Chunk ordering and coalescing.** `AssistantChunk`s are appended in
provider delta order, never reordered, never merged across message ids; a
batch is bounded by `max_chunk_batch`. (`01 §16.1(c)`, `02 §6.2`)

**A5 — Barrier before non-chunk appends.** The pending chunk batch is flushed
before `AssistantMessage`, `ToolCall`, `StepEnded`, and any terminal event.
(`02 §6.2`)

**A6 — Inbox durability and order.** Every accepted inbox op is consumed
exactly once, in FIFO order, and produces exactly one durable event; rejected
ops produce none. (`§10.1`, `01 §4.5`)

**A7 — No focus gating.** The loop runs iff the session is active and has
pending work; attachment, detach, and UI focus never start, stop, or gate it;
`inject` alone is not pending work. (`§9.9`, `04` (m))

**A8 — One active session per workspace.** At most one session's loop runs at a
time; the daemon serializes activation. (`§9.9`, `04` (m))

**A9 — Cancellation scoping.** `cancel()` cancels only this session's in-flight
turn; queued follow-ups survive; no other session is affected. (F9, `§34`,
`§20.24`)

**A10 — Failure ≠ cancel.** Provider/loop failure closes a turn with
`TurnFailed`; cancellation closes it with `TurnCancelled`; `Cancelled` is never
mapped to `TurnFailed`. (`01` I11/I12, `08` (k)/(p))

**A11 — Lease before work.** No turn starts before the session's write lease is
held; a lost lease stops the loop and degrades to read-only. (01 I5, `02 §5.7`)

**A12 — `LLMPool` discipline.** Every provider call (including compaction) is
bracketed by one slot acquire/release; the slot is released on completion,
cancel, and failure. (`04 §8`, `08` (r), F8)

**A13 — Bounded memory.** Pending chunk batches are bounded by
`max_chunk_batch`; the inbox is bounded; no unbounded per-turn buffer exists.
(F5, `§9.11`)

**A14 — `dispose()` is terminal.** After `dispose()` the agent accepts no new
work, cancels in-flight work, flushes chunks, releases the lease, leaves the
registry, and fires `whenIdle` when quiescent. (`§9.8`)

**A15 — Resume is `Idle`.** Resume starts no turn and auto-continues no
orphaned turn; activation with pending work is required. (F10, `01 §9.2`)

**A16 — Subagent identity.** A subagent has its own `SessionId`/`AgentId` and
its own log; the parent records only spawn/fan-in edges. (F11, `§30`,
`01` I17)

**A17 — No UI dependency.** The loop depends on no FTXUI/UI type; state is
exposed as core enums/events. (`§54 D14`, `§4.1`)

**A18 — Tool pipeline order.** Model output → tool parser → permission policy →
execution environment; the loop never executes an unvalidated call. (`§46`,
`§19`)

---

## 11. Failure modes

### 11.1 Shared findings (F1–F12, §54)

The loop's responsibilities for the existing findings:

| F# | Finding | Agent-loop handling |
|---|---|---|
| **F1** | path/process isolation | loop performs no path resolution; tools go through `ExecutionEnvironment` (§9.7, §18); one workspace per daemon (`04` H1) |
| **F2** | background permission | an `ASK` on a background session records `PermissionDecision` and enters `WaitingForPermission`; it is surfaced via attention, never an invisible deadlock (§9.9) |
| **F3** | late event after close | `dispose()` stops new work and flushes; `whenIdle()` is the loop's half of the tombstone contract (`§9.8`, `01 §5`) |
| **F4** | edge-triggered attention | loop owns the `AgentState` transitions; spec 10 projects them into the waiting count/flash (`§9.9`, `§20.23`) |
| **F5** | output ring buffers | chunk batches and the inbox are bounded (A13); tool output truncation is spec 07/01 (`§9.11`) |
| **F6** | input/keybinding focus | out of scope; focus never reaches the loop (A7) |
| **F7** | per-session dirty flags | out of scope; per-session mailbox/UI projection (`§20.23`) |
| **F8** | resource caps | every provider call takes one `LLMPool` slot (A12); subprocess/PTY caps are spec 07 (`§9.11`) |
| **F9** | cancellation scoping | `cancel()` is per-session/per-turn (A9, `§34`) |
| **F10** | resume-suspended | resume is `Idle`, no auto-turn (A15, `§9.9`) |
| **F11** | subagent ID duality | subagent keeps its own ids; parent records edges (A16, `§20.25`) |
| **F12** | flash clock in model | out of scope; the loop emits state edges only (`§9.9`, `§20.23`) |

**Explicitly out of scope:** F6, F7, F12 (UI projection), F8's subprocess/PTY
half (spec 07), and the flash arithmetic (spec 10).

### 11.2 Component-local failure modes (`A-F#`)

These are component-local to the agent/loop layer and must be covered by tests
(§13.6).

| A-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **A-F1** | Provider terminal failure | `StreamError`/`Failed` (`08 §2.2`) | flush; live `Error`; `TurnFailed{code,message}`; unmatched `tool_use` → `ToolResult{Error}` |
| **A-F2** | Cancel mid-turn | `cancel()`/token fired | flush; `TurnCancelled`; queued follow-ups preserved (A9) |
| **A-F3** | Lease lost mid-turn | `LeaseLost` on append (`02 §5.7`) | stop appending; degrade read-only; surface; no partial commit |
| **A-F4** | Step limit exceeded | `step >= maxSteps` | flush; live `Error`; `TurnFailed{StepLimitExceeded}` |
| **A-F5** | Tool execution fails | tool returns error/throws | `ToolResult{outcome = Error}`; turn continues |
| **A-F6** | Permission denied | policy `Deny`/user deny | `PermissionDecision`; `ToolResult{Denied}`; turn continues |
| **A-F7** | `ASK` on a background session | policy `Ask`, session not active | enter `WaitingForPermission`; record decision; surface via attention; never deadlock invisibly (F2) |
| **A-F8** | Inbox overflow | `maxInbox` reached | return `InboxFull`; never silently drop |
| **A-F9** | Context assembly fails | assembler error | `TurnFailed{ContextAssemblyFailed}` before any provider call |
| **A-F10** | Compaction fails | summarizer error | best-effort: proceed if it fits; else `TurnFailed{CompactionFailed}` |
| **A-F11** | Chunk flush fails | store error on `appendBatch` | `TurnFailed{StoreUnavailable}`; no silent drop (`01` S11/S13) |
| **A-F12** | `dispose()` during a turn | dispose while `Running` | cancel + flush + release lease; `whenIdle` fires (A14) |
| **A-F13** | Duplicate activation | `activate()` while running | idempotent no-op |
| **A-F14** | Op after `dispose()` | `send`/`steer`/… post-dispose | `AgentDisposed`; no event |
| **A-F15** | Resume under a live lease | `acquire` → `HeldByOther` | `LeaseHeldByOther`; no agent registered (`02 §5.3`) |
| **A-F16** | Open turn at resume (crash) | log ends mid-turn (`01` S8) | project to last event; start `Idle`; no auto-continue (A15) |
| **A-F17** | Subagent fails/cancels | child terminal `Failed`/`Cancelled` | `SubagentFanIn{outcome}`; tool result reflects it; parent continues |
| **A-F18** | Re-entrant same-session turn | a tool tries to drive its own session | forbidden; one driver per session (A1); reject with `Internal` |

---

## 12. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`§55`). The agent/loop maps onto
it as follows.

| dsh concept | ymh agent/loop | Reference |
|---|---|---|
| Agent (public surface) | `Agent` handle (`§10.1`) | `§10.1`, `§55` |
| Concrete agent-loop | `AgentLoop` (this spec) | `§11`, `§55` |
| Agent handle lifecycle | `dispose()` / `whenIdle()` | `§9.8` |
| turn/step taxonomy | `TurnStarted`/`StepStarted`/`StepEnded`/`TurnEnded` | `§8.1`, `§11`, `01 §4.5` |
| inbox / steer / follow-up | `send`/`followup`/`steer`/`inject` | `§10.1`, `01 §4.5` `TurnOrigin` |
| Session manager | `SessionManager` (01) + `AgentRegistry` (06) | `§9.6`, `01 §8` |
| Subagent | ordinary `Agent` instance | `§30`, `§54 D9` |
| Scheduling | daemon activation policy | `§9.9`, `04` (m) |
| Cancellation | `CancellationToken` propagation | `§34` |
| Approvals | `PermissionPolicy` between loop and tools | `§19`, `§46` |
| Append-only trace | loop appends; the log is the trace | D2, `§9.1` |

**Deliberate omissions** (accepted for v1, `§55`): no detached/async subagent
scheduler (synchronous v1, decision (i)); no hot loop reload; no distributed
driver. The loop is a small, headless, single-workspace turn driver.

---

## 13. Test plan

Strategy is `§44`: unit, integration (fake LLM / fake FS / fake shell), golden,
replay, and a separate live PTY/real-LLM layer. The deterministic layers run
offline against `FakeLLM` (`§45`, `08 §7`); the live layer is opt-in and
API-key gated.

### 13.1 Unit tests

- **State machine**
  - every transition of §3.3 is reachable and legal; illegal transitions
    rejected (`Internal`).
  - `AgentStatus` derivation from `AgentState`; `Running` iff a driver is
    active (§9.8).
- **Inbox**
  - `send` start-or-queue; `followup` always queues; `steer` folds into the
    current turn and is visible only to the next provider call; `inject`
    produces `ContextInjected`, not a turn (A6).
  - FIFO ordering; multiple steers concatenated in order; overflow →
    `InboxFull` (`A-F8`); post-dispose → `AgentDisposed` (`A-F14`).
- **`dispose` / `whenIdle`**
  - dispose stops new work, cancels in-flight, flushes, releases the lease,
    removes the handle (A14); `whenIdle` fires once, in order, and immediately
    when already quiescent.
- **Cancellation**
  - `cancel()` is per-session (A9); `Cancelling → Idle` with `TurnCancelled`;
    queued follow-ups survive; double-cancel is idempotent.
- **Coalescing**
  - flush on `max_chunk_batch`, on `chunk_flush_interval`, and on a barrier
    (A4/A5); ordering preserved; no merge across message ids; bounded batch.
- **Step limit / termination**
  - `maxSteps` → `TurnFailed{StepLimitExceeded}`; no tool calls → `TurnEnded`.
- **Estimator / compaction seams**
  - estimator is deterministic and never overrides reported usage; compaction
    produces a valid `ContextCompaction` boundary and never deletes events.
- **Registry**
  - create ordering (session+lease → loop → registration) (A11); resume is
    `Idle` and starts no turn (A15); idempotent re-create/resume (A1).

### 13.2 Integration tests (`FakeLLM`, `§45`)

- **Full turn.** `FakeLLM` drives user → assistant text → tool call → tool
  result → assistant → `TurnEnded`; the durable event sequence matches the
  golden (§13.3) and `deriveMessages` reproduces it (`01 §6.3`).
- **Provider failure.** Scripted mid-stream failure → `TurnFailed`; unmatched
  `tool_use` → `ToolResult{Error}`; live `Error` (`A-F1`).
- **`ContextLengthExceeded` retry.** A first `ContextLengthExceeded` triggers
  exactly one compaction + re-assembly and a second provider call; a second
  overflow closes the turn with `TurnFailed{CompactionFailed}` (`§5.1`, `§5.7`,
  `A-F10`).
- **Cancel.** Cancel mid-stream → chunks flushed → `TurnCancelled`; follow-ups
  survive (`A-F2`).
- **Lease loss.** Simulated `LeaseLost` → loop stops appending; read-only
  degrade (`A-F3`, `02 §5.7`).
- **Resume (F10).** Resume an `Idle` session → no provider call until
  activation; an orphaned open turn is not auto-continued (`A-F16`).
- **Single active session.** Two sessions with pending work; the daemon
  activates one; `suspend()` cancels the current turn and the next is
  activated; focus does not gate (A7/A8).
- **`LLMPool` accounting.** Concurrent turns never exceed the pool bound;
  slots released on completion/cancel/failure (A12); a full pool suspends
  `acquire` on the FIFO waiter queue, and a cancelled waiter gets `nullopt`
  without leaking a slot (`§5.9`).
- **Subagent.** Spawn → child turn → `SubagentFanIn`; a failed child records
  `Failed` and the parent continues (`A-F17`, A16).
- **Permission.** `Allow`/`Deny`/`Ask` paths; a denied call yields
  `ToolResult{Denied}` and the turn continues (`A-F6`, `§19`).

### 13.3 Golden tests

- **Loop event stream.** A scripted `FakeLLM` turn yields a byte-stable golden
  event sequence: `TurnStarted`, `StepStarted`, `AssistantChunk`(s),
  `AssistantMessage`, `ToolCall`, `PermissionDecision`, `ToolResult`,
  `TokenUsage`, `StepEnded`, `TurnEnded`.
- **Failure/cancel fixtures.** Golden sequences for `TurnFailed` and
  `TurnCancelled`, proving failure ≠ cancel and the `ToolResult` outcome
  difference (`01` I12).
- **Coalescing fixtures.** A known delta stream yields a golden chunk batch
  sequence at a fixed cadence.

### 13.4 Replay tests

- Record a `FakeLLM`-driven session; `deriveMessages` over the log reproduces
  the message list exactly, with **no provider construction or call** (`§44`,
  `01 §6.3`). This is the substrate of `ymh replay SESSION` (`§9.5`).
- Replay after crash (open turn) is deterministic and partial (`01` S8,
  `A-F16`).

### 13.5 Live end-to-end tests (real LLM, PTY-driven, `§44`)

- Spawn the real `ymh` binary under a PTY (`forkpty`/`posix_openpt`), write a
  prompt as keystrokes, read/parse rendered ANSI output, and assert observable
  behavior: session created, assistant text streamed, a tool call rendered, a
  permission prompt handled, cancellation, and resume (`§44`).
- Uses a **real LLM API through the same `LLMProvider` seam** as production
  (`§44`, `§45`).
- **Gated and opt-in:** requires an API key plus an explicit flag
  (`YMH_LIVE_LLM=1`); skipped — never failed — when absent, so the default
  suite stays hermetic and offline.
- **Nondeterminism-tolerant:** assert on structure and invariants (events
  emitted, tools invoked, final state), not exact prose; any retry/quorum is
  explicit, never silent.
- **Milestone-aware:** MVP live tests cover the single-process flow
  (Milestone 1, `§57`/`§58`); multi-workspace PTY tests target Milestone 2.
- Runs as a separate CI stage; never part of the fast default command (`§44`).

### 13.6 Failure-mode coverage matrix

| A-F# | Unit | Integration | Golden | Live |
|---|---|---|---|---|
| A-F1 | state machine | provider failure | failure fixture | — |
| A-F2 | cancellation | cancel mid-stream | cancel fixture | cancel in TUI |
| A-F3 | — | lease loss | — | — |
| A-F4 | step limit | — | — | — |
| A-F5 | tool pipeline | tool error | — | — |
| A-F6 | inbox/permission | deny path | — | permission prompt |
| A-F7 | permission | background `ASK` | — | — |
| A-F8 | inbox | overflow | — | — |
| A-F9 | assembler | assembly error | — | — |
| A-F10 | compactor | compaction failure | — | — |
| A-F11 | coalescer | flush store error | — | — |
| A-F12 | dispose | dispose mid-turn | — | — |
| A-F13 | activation | duplicate activate | — | — |
| A-F14 | inbox | post-dispose | — | — |
| A-F15 | registry | live-lease resume | — | — |
| A-F16 | registry | crash resume | — | — |
| A-F17 | — | subagent failure | — | — |
| A-F18 | — | re-entrant turn | — | — |

### 13.7 Invariant coverage

| Invariant | Covered by |
|---|---|
| A1 | registry unit; concurrency test |
| A2 | terminal-event unit + golden |
| A3 | persist-before-publish integration |
| A4/A5 | coalescer unit + golden |
| A6 | inbox unit + integration |
| A7 | focus-independence integration (no focus calls) |
| A8 | single-active-session integration |
| A9 | cancellation unit + integration |
| A10 | failure/cancel golden |
| A11 | registry ordering unit; lease-loss integration |
| A12 | pool-accounting integration |
| A13 | bounded-buffer unit |
| A14 | dispose unit + integration |
| A15 | resume unit + integration (F10) |
| A16 | subagent integration |
| A17 | static check: no UI include in the loop target |
| A18 | tool-pipeline unit + integration |

---

## 14. Decisions and open questions

### 14.1 Decisions (pinned by this spec)

- **(a) The `§10.1` handle is extended additively, and `AgentId` is a distinct
  UUIDv4 strong type 1:1 with `SessionId`.** `status`/`state`/`disposed`/
  `hasPendingWork` are added, and `send`/`steer`/`inject` return `InboxResult`
  (instead of `void`) so admission rejection is total and typed; the handle is
  not silently interchangeable with the session id even though the registry maps
  them 1:1 (`§10.1`, A1).
- **(b) `send` starts-or-queues; `followup` always queues; `steer` folds into
  the current turn (or starts one if idle); `inject` adds context, not a turn.**
  `send` and `followup` differ only when the agent is idle (§3.2, A6); `send`'s
  origin is state-dependent (`User` when it starts a turn, `FollowUp` when
  queued, §3.2). The `followup` inbox op is an additive extension to the `§10.1`
  handle, listed among the named durable extensions in `00 §8.1` (it queues a
  `TurnStarted{origin=FollowUp}`). An injection starts a turn only when
  `ContextMessage::startsTurn == true`, with `origin=Injection` (`01 §4.5`).
- **(c) `AgentState` (`§20.15`) is the core loop state; `AgentStatus` (`§9.8`)
  is the coarse derived view.** The waiting set is `{WaitingForPermission,
  WaitingForInput}` plus `Error` for attention; the count/flash arithmetic is
  spec 10 (`§9.9`, `§20.23`). `WaitingForInput` means blocked on an explicit
  user answer; no pending work ⇒ `Idle`. `WaitingForInput` holds a blocked
  in-flight turn, so it is `AgentStatus::Running` (**blocked**) and remains
  cancellable via `cancel()` (§3.3, §3.5).
- **(d) Coalescing policy: flush on `max_chunk_batch` (32) OR
  `chunk_flush_interval` (100 ms) OR a barrier event.** The bounds come from
  `02 §6.2`; this spec chooses the policy inside them (`01 §16.1(c)`).
- **(e) Create/resume transaction order is session+lease → loop → register.**
  On failure after the session is created, the session stays durable/resumable,
  the lease is released, and no agent is registered; create never deletes a
  partially established session (`§4.2`).
- **(f) Resume is `Idle` and starts no turn** (F10); an open turn at crash is
  projected to the last event and not auto-continued (`§4.3`, A15).
- **(g) Step limit is a loop-policy failure:** `TurnFailed{turn,
  "StepLimitExceeded"}` (`§5.8`).
- **(h) Compaction is best-effort.** It fails the turn only when the context
  cannot fit after a compaction attempt (`§5.3`, `A-F10`). A provider
  `ContextLengthExceeded` triggers exactly **one** compaction retry (§5.7); if
  the re-assembled context still overflows, the turn fails with
  `CompactionFailed` (`§5.1`, `§5.7`).
- **(i) Subagents are spawned by a tool and awaited synchronously in v1.**
  Detached/async subagents and the `§20.25` delta coalescer are deferred
  (`§7`).
- **(j) The loop is daemon-driven; focus never gates.** `suspend()` gracefully
  cancels the in-flight turn with `reason="superseded"` and preserves the
  queue (`§6`, A7).
- **(k) `LLMPool` acquire/release brackets every provider call** (including
  compaction); the provider is synchronous (`08` (r), A12). This spec pins the
  concrete `LLMPool` interface that `08` decision (r) defers: move-only RAII
  `Slot`, FIFO-bounded exhaustion, and a cancellable `acquire` (`§5.9`).
- **(l) `AgentRegistry` is one per `WorkspaceHost` daemon** and is the
  daemon-level owner of agent lifetime; it composes `SessionManager`, whose
  `agent(SessionId)` is a thin delegate to it. It is not supervisor/global
  state (`§9.6`, `§4.1`).
- **(m) `TurnFailed.code` is an `AgentErrorCode`.** The loop maps provider
  failures via `mapAgentError` (every provider code except `Cancelled` →
  `ProviderFailed`; `ContextLengthExceeded` → one-shot compaction retry → on
  re-overflow `CompactionFailed`); `01 §4.5` serializes it as the stable `code`
  string and `08 §4.5`'s provider-side code is preserved in `TurnFailed.message`
  (`§2.2`, `§5.7`).

### 14.2 Open questions

None remain for this component. The items previously listed here are now
pinned as decisions (§14.1) or explicitly deferred to their owning components:

- Registry placement is pinned by decision (l): one `AgentRegistry` per daemon,
  the daemon-level owner of agent lifetime over `SessionManager` (whose
  `agent(SessionId)` is a thin delegate). `04 §3.7` hosts agents but does not
  define registry ownership, so this spec does not claim a `04 §3.7`
  cross-check.
- Step-limit semantics are pinned by decision (g); a soft `TurnEnded`
  alternative is rejected because `01` I11 requires one terminal event.
- Detached/async subagents and their `§20.25` delta coalescer are deferred by
  decision (i); they are not required for the v1 loop.
- The background-permission answer (F2) is owned by spec 09; this spec pins
  only that the turn enters `WaitingForPermission`, records the durable
  `PermissionDecision`, and never deadlocks invisibly (`A-F7`).
- `steer` on an idle agent starts a turn with `origin=Steer` (decision (b)).
- The `WaitingForInput`/`Idle` boundary is pinned by decision (c); spec 10 owns
  the exact UI projection (`§20.15`).
- The compaction threshold and estimator are configuration (`§37`), not a
  design question.

A genuine cross-document contradiction, if review finds one, reopens this
subsection rather than being resolved silently.

---

## 15. References

- `00-architecture.md` §4.1 (agent ≠ UI), §4.2 (event stream is the runtime
  spine), §4.3 (services vs events), §8.1 (durable vs live events), §9.6
  (daemon/session model), §9.7 (write lease), §9.8 (session lifecycle,
  `AgentHandle`), §9.9 (background execution, attention, one active session),
  §9.11 (resource caps, F5/F8), §10.1 (`Agent` handle), §11 (agent loop
  turn/step), §12 (LLM interface), §14 (tool system), §19 (permissions), §30
  (subagents), §31 (context management), §32 (compaction), §33 (token
  accounting), §34 (cancellation), §35 (concurrency), §37 (configuration),
  §44 (testing strategy), §45 (Fake LLM), §46 (security model), §54 (D1–D23,
  F1–F12), §55 (dsh comparison), §57 Steps 3/8, §58 (milestones).
- `01-session.md` §2.1 (identifiers), §3 (`SessionHeader`), §4.3 (`turn/fail`),
  §4.5 (payloads: `AssistantChunk`, `AssistantMessage`, `ToolCall`,
  `ToolResult`, `PermissionDecision`, `TokenUsage`, `ContextInjected`,
  `ContextCompaction`, `SubagentSpawned`/`SubagentFanIn`,
  `TurnCancelled`/`TurnFailed`), §5 (`EventBus`), §6 (`Session`,
  `deriveMessages`), §7–§8 (`SessionStore`/`SessionManager`), §9 (lifecycle),
  §10 (lease), §12 (I11/I12/I18), §16.1(c)(e)(f).
- `02-persistence.md` §4.4 (`append`/`appendBatch`), §5 (write lease), §6.2
  (chunk coalescing bounds), §8 (concurrency).
- `03-workspace-registry.md` §8 (pending-mutation protocol), R16 (row-domain
  write ownership; the daemon writes the session junction), R-F12 (reconcile).
- `04-workspace-host-daemon.md` §8 (`ResourceGovernor`/`LLMPool`), §9
  (concurrency), §14 decision (m) (daemon-driven activation).
- `05-transport.md` §7.4 (session methods), §7.5 (agent input: `agent.prompt`/
  `followup`/`steer`/`inject`/`cancel`/`status`), §7.6 (permissions), §7.7
  (event streaming).
- `08-llm-provider.md` §1.4 (seam ownership), §2.2 (error taxonomy), §3 (seam),
  §4 (streaming→durable mapping), §5 (model selection), §7 (`FakeLLM`), §9
  (concurrency), §10 (L1–L17), decisions (k)/(p)/(r) (`TurnFailed`, pool
  contract).
- `HANDOFF.md` §2 (the rule), §6 row 06 (component plan), §7 (definition of
  verified); `DESIGN_STATUS.md` (written/verified tracker).

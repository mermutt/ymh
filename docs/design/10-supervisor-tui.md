# 10 — Supervisor / TUI

**Component 10 of 10.** This spec owns the **supervisor** — the FTXUI frontend
process that attaches to N workspace daemons, projects their session event
streams into a pure presentation model, and drives the terminal. It pins the
`WorkspaceModel` / `SessionUiState` / `SessionEnvelope` shapes, the
workspace→session tree switcher, the aggregate status line with its ~1s
edge-triggered flash, the FTXUI renderers and `TerminalLayer` (including the
`IXON` fix), the input-focus and keybinding namespace, subagent fan-in
coalescing, the permission/attention UI, and the live PTY testing hook.

This document follows `00-architecture.md` (cited inline as `§n`),
`01-session.md` (`01 §n`), `02-persistence.md` (`02 §n`),
`03-workspace-registry.md` (`03 §n`), `04-workspace-host-daemon.md`
(`04 §n`), `05-transport.md` (`05 §n`), `06-agent-loop.md` (`06 §n`),
`07-tools-execution.md` (`07 §n`), `08-llm-provider.md` (`08 §n`), and
`09-permissions.md` (`09 §n`); where it cannot follow them it records the
conflict under §17.1 (decisions) / §17.2 (open questions) rather than choosing
silently.

Status: **written · verified** · reviewer: Oracle component gate (GATE PASS) (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** The invariants in this spec are numbered `U1`, `U2`, … (for
> "UI"), so they cannot collide with the `D1`–`D23` design decisions in
> `00-architecture.md` §54 nor with any other component's invariant namespace
> (`S#`/`I#` in 01, `P#` in 02, `R#` in 03, `H#` in 04, `T#` in 05, `A#` in 06,
> `X#` in 07, `L#` in 08, `Q#` in 09). Component-local failure modes are `U-F#`,
> disjoint from `P-F#` (02), `R-F#` (03), `D-F#` (04), `T-F#` (05), `A-F#` (06),
> `E-F#` (07), `L-F#` (08), and `Q-F#` (09). Architecture decisions are always
> written with the `§54` prefix (`§54 D16`); this spec's invariants are written
> bare (`U3`). **Caveat:** `00-architecture.md` uses bare `Q3`/`Q5`/`Q6`/`Q7`/`Q8`
> for its *own* internal open questions (§9.9, §20.22, §20.25); this spec's `U#`
> never collides with those, and architecture references always carry the `§`
> prefix (`§20.23`).

---

## 1. Purpose and scope

### 1.1 Position in the component graph

The supervisor is a **frontend**, not a runtime. It owns no agent, session, tool,
or policy state; it is one consumer of the event stream (`§1`, `§54 D1`/`D10`).

```text
   WorkspaceHost A (04)   WorkspaceHost B (04)   WorkspaceHost C (04)
        │  Unix-socket JSON-RPC 2.0 (05)  │             │
        └───────────────┬─────────────────┴─────────────┘
                        ▼
             ┌──────────────────────────────────────────────┐
             │        Supervisor (this spec, §20)            │
             │                                              │
             │  HostConnection[] (05)                       │
             │        │  SessionEnvelope (core Event)       │
             │        ▼                                     │
             │  UiEventAdapter (10)                         │
             │        │  UiEvent                            │
             │        ▼                                     │
             │  UiModel (WorkspaceModel[], SessionUiState[])│
             │        │                                     │
             │  UiController ──► HostConnection RPC         │
             │        │                                     │
             │  FTXUI Views / Renderers                     │
             │        │                                     │
             │  TerminalLayer (raw mode, IXON off)          │
             └──────────────────────────────────────────────┘
                        │
                   PTY / SSH
```

`UiEvent` is produced **only** inside the supervisor by the adapter and never
crosses the wire (`§20.6`, `05 §5.2`, `§54 D15`, T4). The wire carries the core,
durable `Event` (§8.2, `05 §5.1`).

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 10)

- The multi-workspace presentation model: `UiModel`, `WorkspaceModel`,
  `SessionUiState`, `SessionCell`, `AttentionState` (§20.22).
- The wire-to-frontend envelope boundary: `SessionEnvelope` / `WorkspaceEvent`
  and the `UiEventAdapter` (§20.6, §20.22).
- The tree switcher overlay (workspaces → sessions) and its focus blocking
  (§20.24).
- The aggregate status line (counts across **all** attached workspaces) and the
  ~1s edge-triggered flash (§20.23, §9.9).
- FTXUI renderers, the presentation boundary, `TerminalLayer` (`IXON` off), and
  adaptive layout (§20.19, §21–§25).
- Input focus and the keybinding namespace, per-session draft/history, and
  cancellation scoping (§20.26, §25, F6/F9).
- Subagent fan-in display and per-frame coalescing (§20.25).
- The permission dialog and attention UI (§19, `09 §4`–§5).
- The live PTY testing hook (a scripted PTY driver, §44).

### 1.3 Boundaries — deferred to other specs

- Agent state transitions are **owned** by 06 (§3.3); this spec only *projects*
  them.
- Permission classification, the broker, timeouts, and the background policy are
  **owned** by 09; this spec renders the request and sends the decision.
- The transport, cursors, profiles, and the RPC catalog are **owned** by 05;
  this spec is a client.
- The daemon's lifecycle, activation serializer, and attach semantics are
  **owned** by 04; this spec observes `DaemonStatus` and sends explicit controls.
- The session log, replay, and durable projection are **owned** by 01; this spec
  adapts the streamed `Event`s.
- The subagent delta coalescer (`SubagentCoalescer`, §4.8) is producer-side: its
  daemon implementation is **owned by 04** and deferred to Phase B (`06` (i),
  `§20.25`); this spec owns only the consumed `SubagentDelta`/`SubagentViewModel`
  shapes and the per-tick drain contract.
- The headless/RPC frontends' *equivalent* attention signal (D19) is deferred to
  the headless/CLI component (§42); it is not specified here.

### 1.4 Seam ownership relative to 01/02/03/04/05/06/07/08/09

| Seam | Owner | This spec's use |
|---|---|---|
| Core `Event` / `SessionEnvelope` | 01 / 05 | consume `envelope.event`; never re-serialize `UiEvent` |
| Cursors / `event.subscribe` | 05 | resume after reconnect (`05 §8.5`) |
| `HostConnection` | 05 | one per attached daemon; lifecycle → `WorkspaceModel.daemonStatus` |
| `AgentState` / edges | 06 | projected into `SessionUiState.attention` and `AggregateStatusModel` |
| `permission.request` / `permission.decide` | 05 / 09 | dialog + `resolvePermission` |
| `session.activate` / `session.suspend` | 04 / 05 | explicit controls only; never bound to focus |
| `WorkspaceRegistry` reads | 03 | populate the switcher's workspace list |
| `SubagentDelta` / coalescer | 04 (producer) | consumes at most one aggregated batch per tick (§10) |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

The supervisor reuses the core identities verbatim; it introduces **no** new
durable identity (`§20.22`, `05 §2.1`).

```cpp
namespace ymh {

// Reproduced verbatim from 01 §2.1 / 03 §2.1; never redefined here.
struct SessionId {                       // UUIDv4; stable; never a path
    std::string value;
    auto operator<=>(const SessionId&) const = default;
};
struct WorkspaceId {                     // UUIDv4; never the path
    std::string value;
    auto operator<=>(const WorkspaceId&) const = default;
};

enum class DaemonStatus : std::uint8_t {
    Connecting,   // handshake in flight (05 §4.1)
    Attached,     // host.hello accepted; stream live (05 §7.2)
    Detached,     // host.detach / connection closed; daemon survives (§54 D23)
    Dead,         // process gone / heartbeat lost (04 §3.6)
};

enum class UiMode : std::uint8_t { Conversation, Switcher, Dialog };

} // namespace ymh
```

`DaemonStatus` is the supervisor's view; it is **supervisor-local** and never
written to the shared registry (03 §2.1, §9.10). `WorkspaceModel.activeSessionId`
is likewise supervisor-local (`§20.22`): two TUI clients may focus different
sessions of the same workspace.

### 2.2 UI state and error taxonomy

The UI has no wire error namespace of its own; transport errors surface through
05's codes and map to user-visible status, never to a crash.

```text
transport error (05 §2.2)     supervisor reaction
────────────────────────      ───────────────────────────────────────────────
HostUnreachable               daemonStatus := Dead; banner; keep UI responsive
NotServing / ShutdownInProgress  daemonStatus := Detached; sessions stay listed
AuthFailed                    fatal attach error for that workspace; banner
CursorInvalid (T16)           re-subscribe { from: beginning } (never silent now)
UnknownSession (01 S6)        drop the stale SessionUiState; refresh via session.list
PayloadTooLarge (01 S10)      reject the send locally; show an input error
LeaseLost (02 §5.7)           read-only badge; disable send for that session
```

The taxonomy is deliberately small: the supervisor is a **projection**, and any
state it cannot project is shown as an explicit degraded badge rather than
silently hidden.

### 2.3 Two milestones (`§57` Step 13 / `§58`)

The model shapes below are milestone-independent; the **transport** differs.

- **Milestone 1 (single-process MVP, `§58`).** `UiModel` holds exactly one
  `WorkspaceModel` (the process cwd) and its sessions; there is no
  `HostConnection`, no registry, and no daemon. `UiEventAdapter` reads the local
  `EventBus` instead of a socket. The `WorkspaceModel`/`SessionUiState` split is
  kept so the fork to Milestone 2 is mechanical.
- **Milestone 2 (supervisor + per-workspace daemons, `§57` Step 13).** The
  supervisor holds one `HostConnection` and one `WorkspaceModel` per attached
  daemon (`§9.6`, `§58`). Nothing else changes.

The two milestones are **not** a topology choice (`AGENTS.md`): Milestone 2 is
the target and Milestone 1 is written so the runtime core forks without
redesign.

---

## 3. Presentation boundary, threading, and invalidation

### 3.1 Dependency rule (`§20.1`, `§54 D14`)

The boundary is enforced by directory and by the link graph:

```text
core/       MUST NOT depend on FTXUI (or on ui/ or terminal/)
ui/         depends on core interfaces + FTXUI
terminal/   thin low-level terminal handling beneath FTXUI
supervisor/ process wiring (HostConnection[] + UiApplication)
```

Agent, Tool, and Session never reference FTXUI. The agent must run with no
terminal at all (`§42`). A static/link check enforces that `core` targets do not
link FTXUI (U1).

### 3.2 Layered architecture (`§20.2`)

```text
Agent Runtime (remote, in WorkspaceHost daemons)
        │  core Event (wire, 05)
        ▼
  UiEventAdapter            (frontend-only)
        │  UiEvent
        ▼
      UiModel               (pure presentation state)
        │
   UiController             (actions → RPC)
        │
   FTXUI Widgets            (own no application state, §54 D13)
        │
   TerminalLayer            (raw mode / IXON / caps / cursor)
        │
      PTY / SSH
```

### 3.3 Threading model (`§20.13`, `§35`)

```text
UI thread
  ├── terminal input
  ├── UiModel updates (adapter + tick)
  └── rendering

Worker threads (per HostConnection, in the supervisor process)
  ├── socket read / decode
  └── socket write
```

Workers never touch FTXUI and never mutate `UiModel` directly: a decoded
`SessionEnvelope` is posted to the UI thread, which runs the adapter and
`apply()` (`§20.13`). The UI thread never blocks on a non-active session or
daemon (`§54 D20`, U14); an in-flight request is tracked by `RequestId` and a
pending-state flag, not a blocking wait (`05 §8.6`).

### 3.4 Rendering is pure (`§20.12`, `§54 D16`)

`Render()` reads the model and returns an `Element`. It performs no I/O, no
network, no database access, no allocation of long-lived state, and **never**
advances a timer (U3). The flash clock is advanced by the model-level tick
(§5.2), never in a widget.

```cpp
ftxui::Element ConversationView::Render() {
    return renderConversation(model_.activeSession().conversation);
}
```

### 3.5 Invalidation: per-session dirty flags + `aggregateDirty` (F7)

The single `UiDirtyFlag` set of `§20.14` is split **per session** so a burst from
a background session cannot invalidate the whole screen and jitter the active
view (F7, `§20.23`).

```cpp
enum class UiDirtyFlag : std::uint32_t {
    None          = 0,
    Conversation  = 1u << 0,
    Tools         = 1u << 1,
    Diff          = 1u << 2,
    Input         = 1u << 3,
    Status        = 1u << 4,
    Layout        = 1u << 5,
    Subagents     = 1u << 6,
    Attention     = 1u << 7,   // per-session badge / glyph
    SessionBar    = 1u << 8,   // the current workspace's session strip
    Aggregate     = 1u << 9,   // the cross-workspace counts line only (F7)
};
```

Each `SessionUiState` owns one `UiDirtyFlag` (its conversation/tools/diff/input/
status/subagent/attention bits); `UiModel` owns one `aggregateDirty` bit. The
`SessionBar` bit is per-session: a session marks it when its cell changes, and
the strip re-renders only if a session of the current workspace is dirty (F7).
`UiController::flush()` folds the dirty bits into at most one FTXUI invalidation
per render tick, targeting ~20–60 FPS (`§20.14`).

```cpp
class DirtySet {
public:
    void mark(SessionId, UiDirtyFlag);
    void markAggregate();          // Aggregate bit only (F7)
    bool takeAggregate();          // consume, never per-token
    std::vector<SessionId> takeDirtySessions();
private:
    std::map<SessionId, UiDirtyFlag> perSession_;
    UiDirtyFlag aggregate_ = UiDirtyFlag::None;
};
```

Only the **active** session renders its conversation body; every other session
in the current workspace renders a bounded, collapsed summary in the `SessionBar`
strip (`§20.23`, F7). A background session's token stream therefore marks only
its own bits plus `Aggregate` (if its counts changed), never `Conversation`.

---

## 4. The presentation model (pinned)

### 4.1 `UiModel` (`§20.22`)

One `UiModel` for the whole supervisor (preserves D13/D16); the workspace
dimension is a map keyed by `WorkspaceId`.

```cpp
namespace ymh {

class UiModel {
public:
    std::map<WorkspaceId, WorkspaceModel> workspaces;      // one per attached daemon
    WorkspaceId                           activeWorkspaceId;

    // Per-session UI state, keyed by SessionId (cross-workspace, §20.22)
    std::map<SessionId, SessionUiState>   sessions;

    AggregateStatusModel                  aggregate;       // §4.7
    DialogModel                           dialogs;         // global modal stack (§20.16)
    SwitcherOverlayModel                  switcher;        // §4.6
    UiMode                                mode = UiMode::Conversation;
    bool                                  shouldExit = false;

    DirtySet                              dirty;           // §3.5 (F7)

    WorkspaceModel&  activeWorkspace();
    SessionUiState&  activeSession();
    const SessionUiState& session(SessionId) const;

    void apply(const UiEvent& event);          // adapted, frontend-facing (§20.6)
    void apply(const WorkspaceEvent& event);   // workspace-level events
};

} // namespace ymh
```

`apply()` takes the frontend-adapted `UiEvent`; the model never sees core or wire
types (U4). `activeWorkspace()` is `workspaces.at(activeWorkspaceId)`;
`activeSession()` is `sessions.at(activeWorkspace().activeSessionId)`.

### 4.2 `WorkspaceModel` (`§20.22`)

```cpp
namespace ymh {

struct WorkspaceModel {
    WorkspaceId              id;             // 03 §2.1
    std::string              cwd;            // daemon host working directory (display only)
    DaemonStatus             daemonStatus = DaemonStatus::Connecting;

    SessionId                activeSessionId;   // supervisor-local focus (§9.10)
    std::vector<SessionCell> sessions;          // ordered by the registry junction (03 R8)
    OverlayStack             overlays;          // switcher, dialogs, palettes (§20.16)

    bool                     hasDaemon() const { return daemonStatus == DaemonStatus::Attached; }
};

} // namespace ymh
```

`cwd` is **display only**: the supervisor never resolves tool paths and never
`chdir()`s (`§9.7`, `§18`, F1). The daemon owns the real cwd.

### 4.3 `SessionUiState` (`§20.22`)

```cpp
namespace ymh {

struct SessionUiState {
    SessionId         id;                 // never an AgentHandle or ExecutionEnvironment
    WorkspaceId       workspace;          // reverse index for routing

    ConversationModel conversation;
    ToolModel         tools;
    DiffModel         diffs;
    InputModel        input;              // per-session draft + history (§25, F6)
    StatusModel       status;             // model, tokens, context, cost (§33)

    AttentionState    attention;          // derived/cached: level + edge flash (D19, F4)
    SubagentModel     subagents;          // §10
    UiDirtyFlag       dirty = UiDirtyFlag::None;   // per-session (F7)
};

} // namespace ymh
```

D1 constraint: `SessionUiState` holds a `SessionId`, never a core object pointer.
No `AgentHandle`, no `ExecutionEnvironment`, no `LLMProvider`. The supervisor
talks to a daemon host only through session IDs and the event stream (`§20.22`).
`workspace`, `subagents`, and `dirty` extend the `§20.22` shape (reverse routing
index, `§10` fan-in, and per-session F7 flags); the extension is recorded in
decision (p).

### 4.4 `SessionCell` and `AttentionState` (pinned here)

`§20.22` references both types without defining them; this spec pins them.

```cpp
namespace ymh {

// A bounded summary cell for the SessionBar strip and the switcher leaf.
// It is a projection, not a second copy of the session.
struct SessionCell {
    SessionId   id;
    std::string title;                 // 01 §3 SessionHeader.title
    AgentState  state = AgentState::Idle;
    bool        attention = false;     // needs input: Waiting* | Error (D19)
    bool        unread = false;        // turn completed while not focused
    bool        readOnly = false;      // LeaseLost (02 §5.7)
};

// Derived/cached attention for one session. Never advanced in Render() (F12).
struct AttentionState {
    AgentState  lastState = AgentState::Idle;   // edge source
    bool        needsInput = false;             // lastState ∈ waiting set
    bool        completed = false;              // lastState == Idle after a turn
};

} // namespace ymh
```

### 4.5 `SessionEnvelope` and `WorkspaceEvent` (wire = core `Event`)

Exactly `§20.22`, reproduced (`05 §5.2`, T5). The wire carries the core, durable
`Event` (§8.2); `UiEvent` is never serialized (T4, U4).

```cpp
namespace ymh::protocol {

struct SessionEnvelope {
    SessionId session;   // routing key; 00 §20.22 retains it although Event carries session_id
    Event     event;     // core, durable; the client adapts via UiEventAdapter
};

} // namespace ymh::protocol
```

The adapter asserts `envelope.session == envelope.event.session_id` and treats a
mismatch as protocol corruption (`05 §5.2`, T-F20).

```cpp
namespace ymh {

struct WorkspaceEvent {
    WorkspaceId        workspace;
    WorkspaceEventKind kind;
};

enum class WorkspaceEventKind : std::uint8_t {
    DaemonAttached, DaemonDetached, DaemonDied,
    SessionOpened,  SessionClosed,
};

} // namespace ymh
```

`WorkspaceEvent` is produced by the supervisor's `HostConnection` layer from 05's
`HostNotice` (`05 §5.3`) and connection state — it is **supervisor-local** and
never on the wire.

### 4.6 `SwitcherOverlayModel` (`§20.24`)

```cpp
namespace ymh {

struct SwitcherCursor {
    WorkspaceId              workspace;
    std::optional<SessionId> session;   // nullopt = workspace node
};

struct SessionNode {
    SessionId  id;
    std::string title;
    AgentState state;
    bool       attention;               // badge
};

struct WorkspaceNode {
    WorkspaceId              id;
    std::string              title;      // display_title (03 §4.2)
    DaemonStatus             status;
    std::vector<SessionNode> sessions;
};

class SwitcherOverlayModel {
public:
    std::vector<WorkspaceNode> workspaces;   // ordered, from WorkspaceRegistry (03 §4.2)
    SwitcherCursor             cursor;
    std::optional<std::string> filter;       // "/" prefix filters by name
    std::set<WorkspaceId>      collapsed;    // collapsed nodes hide sessions

    void moveDown();
    void moveUp();
    void toggleExpand();   // Tab — expand/collapse workspace node
    void open();           // builds nodes from UiModel + registry reads
    void close();
};

} // namespace ymh
```

### 4.7 `AggregateStatusModel` and `FlashState` (`§20.23`)

```cpp
namespace ymh {

struct AggregateStatus {
    std::uint16_t activeCount  = 0;   // Thinking | CallingTool
    std::uint16_t waitingCount = 0;   // WaitingForInput | WaitingForPermission | Error
    bool operator==(const AggregateStatus&) const = default;
};

enum class FlashPhase : std::uint8_t { Idle, Flashing, Done };

struct FlashState {
    FlashPhase                phase = FlashPhase::Idle;
    std::chrono::milliseconds elapsed{};
    bool enabled = false;             // Phase A: false; Phase B: true (§9.9)

    bool isFlashing() const { return phase == FlashPhase::Flashing; }
    void arm();                              // → Flashing, elapsed = 0
    void tick(std::chrono::milliseconds d);  // Flashing → Done at ~1s
    Decorator flashColor() const;            // pure read for Render() (§20.12)
};

class AggregateStatusModel {
public:
    AggregateStatus current;
    FlashState      flash;   // Idle → Flashing → Done

    // Level snapshot across ALL attached workspaces (§20.23). Runs in the
    // adapter on each AgentState edge; never per token; never in Render().
    void recompute(const std::map<WorkspaceId, WorkspaceModel>&,
                   const std::map<SessionId, SessionUiState>&);
    void armOnEdge(SessionId, AgentState oldState, AgentState newState);
};

} // namespace ymh
```

`recompute()` takes the cross-workspace session map in addition to the workspace
map so it can sum each session's projected `AgentState`; this extends the
one-argument `§20.23` signature (recorded in decision (p)).

### 4.8 Subagent types (`§20.25`)

```cpp
namespace ymh::protocol {

struct SubagentEnvelope {
    SessionId parent;      // UI display routing
    SessionId subagent;    // durability / replay (§30, §9.5)
    Event     event;       // core, durable; frontend adapts via UiEventAdapter
};

// One coalesced fan-in unit (§20.25 step 1): a bounded chunk from one child,
// never a raw per-token delta. Produced in the daemon, consumed per render tick.
struct SubagentDelta {
    SessionId   subagent;  // child id (durability)
    std::string chunk;     // coalesced text/tool fragment
};

} // namespace ymh::protocol
```

```cpp
namespace ymh {

struct SubagentViewModel {
    SessionId    id;
    std::string  task;
    AgentState   state = AgentState::Idle;
    std::string  summary;
    std::vector<std::string> tail;   // capped at K lines (§20.25 step 6)
    bool         expanded = false;
    std::chrono::steady_clock::time_point started;
    std::chrono::milliseconds duration{};
};

struct SubagentModel {
    std::vector<SubagentViewModel> agents;   // collapsed by default
};

// Producer-side in the daemon (04); the supervisor receives at most one
// aggregated batch per render tick. Deferred to Phase B (06 (i)).
class SubagentCoalescer {
public:
    explicit SubagentCoalescer(std::uint16_t coalesceHz = 30);
    void push(protocol::SubagentDelta delta);          // may buffer until next tick
    std::vector<protocol::SubagentDelta> drain();      // called by the render tick
private:
    std::chrono::microseconds coalesceInterval_;
    std::chrono::steady_clock::time_point lastFlush_;
    std::vector<protocol::SubagentDelta> buffer_;
};

} // namespace ymh
```

The coalescer lives in the **daemon host** (producer side), so raw per-token
deltas never cross the IPC boundary; the supervisor receives at most one
aggregated delta batch per tick (`§20.25`). See §10.

---

## 5. Event adaptation

### 5.1 `UiEvent` (extended from `§20.5`)

The adapter emits a strongly typed, frontend-only event. `AgentStateChanged` is
the single source of agent-state truth (`§20.5`, `§20.22`).

```cpp
namespace ymh {

struct UiEvent {
    std::variant<
        UserMessage,
        AssistantMessageStarted,
        AssistantTextDelta,
        AssistantMessageFinished,

        ToolStarted,
        ToolOutput,
        ToolFinished,

        FileChanged,
        DiffUpdated,

        PermissionRequested,
        PermissionResolved,

        AgentStateChanged,     // {session, oldState, newState} — ONLY state carrier
        SubagentUpdated,       // coalesced fan-in (§20.25)

        ErrorOccurred,
        TokenUsageUpdated,
        StatusChanged
    > value;
};

struct AgentStateChanged {   // carries old + new for edge-triggered attention
    SessionId  session;
    AgentState oldState;
    AgentState newState;
};

} // namespace ymh
```

The former `AgentThinking`/`AgentWaiting`/`AgentFinished` variants do not exist
(`§20.5`): keeping them alongside `AgentStateChanged` would allow two encodings
of the same state.

### 5.2 `UiEventAdapter` (`§20.6`, D15)

```cpp
namespace ymh {

class UiEventAdapter {
public:
    UiEventAdapter(UiModel& model, UiController& controller);

    // Decoded wire frames, posted to the UI thread (§3.3).
    void onSessionEnvelope(const protocol::SessionEnvelope&);
    void onSubagentEnvelope(const protocol::SubagentEnvelope&);
    void onHostNotice(const protocol::HostNotice&);          // 05 §5.3
    void onPermissionRequest(const protocol::PermissionRequest&);  // 05 §7.6
    void onWorkspaceEvent(const WorkspaceEvent&);

    // Model-level clock; the ONLY place the flash advances (F12, D16).
    void onTick(std::chrono::milliseconds delta);

private:
    UiEvent adapt(const Event&) const;                       // core → UiEvent
    void applyAndMark(const UiEvent&, SessionId);

    UiModel&      model_;
    UiController& controller_;
    std::map<SessionId, AgentState> lastState_;              // edge detection (D19/F4)
};

} // namespace ymh
```

Core events are frontend-agnostic (`§54 D15`): the adapter maps
`ToolExecutionStarted` → `ToolStarted`, `AssistantChunk` → `AssistantTextDelta`,
and derives `AgentStateChanged` by comparing the projected `AgentState` against
`lastState_[session]` (`06 §3.3`, A3). Because core state is exposed as a core
enum (`06` A17), the adapter owns the edge synthesis; the loop does not emit a
UI-shaped event. `lastState_[session]` is updated in `applyAndMark()`, after the
edge has been applied and after `aggregate.recompute()`/`armOnEdge()` have run,
so the next event compares against the previously projected state; `adapt()` only
reads the cache and never mutates it.

### 5.3 `apply()` ordering and the aggregate hook

`apply()` is the single mutation point (besides the tick). The adapter calls
`apply()` for each `UiEvent`, then, for an `AgentStateChanged`, calls
`aggregate.recompute(...)` and `aggregate.armOnEdge(...)`, updates
`lastState_[session]` to `newState`, and only then marks dirty. Ordering is
fixed:

```text
SessionEnvelope (core Event)
      │  adapt
      ▼
UiEvent ──► UiModel::apply()          (per-session state)
      │
      ├── AgentStateChanged ──► aggregate.recompute()   (level snapshot, §6.1)
      │                      ├► aggregate.armOnEdge()    (edge flash, §6.2)
      │                      └► lastState_[session] = newState  (edge cache, §5.2)
      ▼
DirtySet.mark(session, bit) / markAggregate()
      ▼
UiController::flush() ──► one FTXUI invalidation
```

Because `recompute()` is a full sum, it is self-healing: a missed edge is
corrected by the next recompute (`§9.9`, U5).

### 5.4 Cursor and reconnect integration (`05 §8.5`)

`HostConnection` tracks the cursor carried on every `event.stream`
(`05 §7.7`, T21). On reconnect the supervisor re-subscribes with
`from: cursor(E)` and receives exactly the committed events after `E`, in order,
then live (T8). A `CursorInvalid` (T16) forces a re-subscribe with
`from: beginning`; the supervisor **never** silently resets to `now` (U-F11).
While catching up, `replay == true`; the UI may render a "catching up" badge but
must not reorder or drop.

---

## 6. Aggregate status line and flash

### 6.1 Counts are a level snapshot (`§20.23`, `§9.9`)

The bottom region shows an active-session status line and an aggregate counts
line. The aggregate line shows **counts only** — it never lists workspaces.

```text
──────────────────────────────────────────────
 main  ● thinking…               2 active · 3 waiting
 [main ●] [api ✓] [docs >] [tests !]      ← current workspace, per-session
──────────────────────────────────────────────

active   Thinking | CallingTool
waiting  WaitingForInput | WaitingForPermission | Error
```

`recompute()` iterates **all** attached workspaces and sums each session's
`AgentState` into the two counters. It runs in the adapter whenever a state edge
arrives — **never** in `Render()` (D16) and **never** per token. The counts are
a LEVEL SNAPSHOT, not per-edge increments and not frame-sampled: they are
self-healing, so a missed edge is corrected by the next recompute (`§20.23`,
`§9.9`, U5).

Focusing a session does **not** remove it from `waitingCount`; the count drops
only when its `AgentState` actually leaves the waiting set (`§20.23`, `09 §5.3`,
U7). The waiting set is `{WaitingForInput, WaitingForPermission, Error}` per
`§20.23` and `06 (c)`.

### 6.2 Flash is an edge notification (`§20.23`, F4/F12)

When **any** session transitions
`Thinking|CallingTool → Waiting*|Error` (needs input) or
`Thinking|CallingTool → Idle` (a turn completed), the aggregate line flashes for
~1s, then stops. The edge is armed regardless of UI focus: `armOnEdge()` takes no
focus argument and is called for every edge (`§9.9`).

```cpp
void AggregateStatusModel::armOnEdge(SessionId s, AgentState oldS, AgentState newS) {
    if (!flash.enabled) return;                     // Phase A: no flash (§9.9)
    const bool running = (oldS == AgentState::Thinking || oldS == AgentState::CallingTool);
    const bool needsInput = (newS == AgentState::WaitingForPermission ||
                             newS == AgentState::WaitingForInput ||
                             newS == AgentState::Error);
    const bool done = (newS == AgentState::Idle);
    if (running && (needsInput || done)) flash.arm();
}
```

The clock is advanced by `UiController::onTick(deltaMs)` → adapter
`onTick` (model-level, `§20.13`), **not** inside any widget's `Render()`
(`§20.23`, F12, D16). `Render()` only reads `flash.phase` to pick a color and is
otherwise side-effect free. `tick()` moves `Flashing → Done` at ~1s; a `Done`
phase stops flashing until the next `arm()`.

Attention therefore signals **both** "needs input" and "done"; neither the count
nor the flash is suppressed by UI focus (`§9.9`).

### 6.3 Focus independence (`04` (m), A7/A8)

- A workspace runs its active session whenever it has pending work, regardless
  of whether any supervisor is attached or focused; attachment and focus never
  gate work (`§9.9`, `06 §6`).
- The daemon keeps exactly **one active session per workspace** and activates
  the next session with pending work when the active one finishes or blocks
  (`04` (m), `06 §6`).
- `session.activate` / `session.suspend` are explicit operator controls
  (Interactive profile only, `05 §6.1`), **not** a mirror of UI focus: attach,
  detach, and focus never call them (`05 §7.4`, `04` (m)).
- The supervisor's focus change updates `WorkspaceModel.activeSessionId` and the
  display only; it does **not** send `session.activate` (U7, U-F7).

### 6.4 Phase A / Phase B gating (`§9.9`)

- **Phase A** (single-process MVP and the daemon baseline): static glyphs plus
  the aggregate waiting count; `flash.enabled = false`, so `arm()` is never
  called. The `FlashState` shape is pinned now so Phase B needs no redesign.
- **Phase B** (background execution): `flash.enabled = true`; the flash is armed
  by the edges above. Phase B layers on the Phase A base (`§57` Step 12).

### 6.5 Capability degradation (`§20.19`, `§9.9`)

The flash color degrades against `TerminalCapabilities`: bright/inverted where
supported, bold/gray otherwise (`§20.23`). With `trueColor == false` the badge
falls back to `color256`, then to attributes only. The count text is always
plain ASCII and readable without color.

---

## 7. Tree switcher overlay (`§20.24`)

### 7.1 Model and levels

`Ctrl+S` (fallback `Ctrl+P`) opens ONE tree overlay (an `OverlayManager` layer,
`§20.16`) with two levels: workspaces at the top, sessions as leaves. It shows
ALL workspaces, including detached and background ones (`§20.24`).

```text
Switcher
▸ coding-agent          ● thinking
    [main ●] [api ✓] [tests !]
▸ homelab-docs           ✓ idle
    [readme ✓] [network-config !]
▸ scratch                > waiting
    [exp1 >]

j/k or ↓/↑   navigate (crosses workspace boundaries)
Tab          expand/collapse the selected workspace node
Enter        on a workspace: expand-or-focus; on a session: focus it
Esc          close the overlay
Ctrl+W       context-sensitive detach of the selected session (§9.8)
```

The node list is built from `UiModel.workspaces` (status/badges) joined with
`workspace.list` / `session.list` reads (`05 §7.3`) for detached workspaces not
yet attached. Ordering follows the registry junction (`03 R8`).

### 7.2 Keybindings

`moveDown()`/`moveUp()` cross workspace boundaries; `toggleExpand()` is bound to
Tab; Enter on a workspace expands-or-focuses, on a session focuses it; Esc
closes; `Ctrl+W` detaches the selected session (`session.close`, detach
semantics — `05 §7.4`, `01 §8`). `/` starts filter mode (`filter`).

### 7.3 Focus blocking (F6)

While the overlay is open, the underlying `InputView` must not receive keys
(`§20.24`). Exactly one component owns focus (U8): the overlay is the focus owner
when active, and the input pipeline routes `KeyEvent`s to it exclusively.
`Ctrl+C` while the overlay is open cancels the switcher, not the session
(`§20.24`, F9).

### 7.4 XOFF caveat and the `IXON` fix

`Ctrl+S` is the terminal XOFF flow-control character. `TerminalLayer` disables
`IXON` on the tty (`§20.19`, §8.3) so the byte reaches the application. Fallback:
`Ctrl+P` opens the same switcher in filter mode, and `/switch` exists for
terminals that ignore the `IXON` change (`§20.24`, U17, U-F5).

### 7.5 Activation vs. focus

Selecting a session in the switcher sets `activeSessionId` (focus). It does not
call `session.activate`; activation remains daemon-driven (`04` (m), §6.3). The
switcher displays the daemon's active-session marker independently of the
supervisor's focus, so the two never disagree about who is *running*.

---

## 8. Renderers and `TerminalLayer`

### 8.1 Component hierarchy (`§20.17`)

```text
RootComponent
│
├── Header                (workspace title, model, context %)
│
├── MainArea
│   ├── ConversationView  (active session only)
│   │   └── MessageView[]
│   │       ├── Markdown / CodeBlock / Diff
│   │       └── ToolView[]            (collapsed by default, §20.10)
│   ├── SubagentPanel     (collapsed summary lines, §10)
│   └── SessionBar        (current workspace, bounded cells, F7)
│
├── InputView             (per-session draft/history, §9.3)
│
├── StatusBar             (active session status + aggregate counts, §6)
│
└── OverlayManager        (§20.16)
    ├── SwitcherOverlay   (§7)
    ├── PermissionDialog  (§11)
    ├── CommandPalette    (§26)
    └── HelpDialog
```

The root stays small and only composes children; it owns no application state
(`§20.17`, D13).

### 8.2 Renderers (`§22`–§25`)

```cpp
namespace ymh {

class MarkdownRenderer {
public:
    ftxui::Element render(const MarkdownBlock&, const RenderContext&);
};

class CodeBlockRenderer {
public:
    ftxui::Element render(std::string_view code, std::string_view language,
                          const RenderContext&);
};

class DiffView : public ftxui::ComponentBase {
public:
    explicit DiffView(DiffModel& model);
    ftxui::Element Render() override;
    bool OnEvent(ftxui::Event) override;
private:
    DiffModel& model_;
    int scroll_ = 0;
    int selectedFile_ = 0;
};

} // namespace ymh
```

Markdown goes through a parser (cmark-gfm, §22) and tree-sitter for code blocks
(§23); diffs are a first-class component (§24). Streaming text uses the
`StreamingMessage` incremental buffer of `§20.9` so a token never reparses the
whole conversation. Tool output is collapsed by default and expandable
(`§20.10`); the full buffer is retained only for the active/expanded session
(F5, U13).

### 8.3 `TerminalLayer` (`§20.19`)

```cpp
namespace ymh {

struct TerminalCapabilities {
    bool trueColor;
    bool color256;
    bool mouse;
    bool bracketedPaste;
    bool modifyOtherKeys;
    bool kittyKeyboard;
    bool unicode;
};

class Terminal {
public:
    void enterRawMode();          // also disables IXON (see below)
    void leaveRawMode();
    void enterAlternateScreen();
    void leaveAlternateScreen();
    TerminalSize size() const;
    TerminalCapabilities capabilities() const;
    void hideCursor();
    void showCursor();
    void setCursorPosition(int row, int col);
    void flush();
};

} // namespace ymh
```

`enterRawMode()` clears `IXON` (and `ICRNL`/`ISIG` as required for FTXUI) so
`Ctrl+S`/`Ctrl+Q` are delivered as key bytes (`§20.24`, §25). The terminal is
restored by an RAII guard on every exit path, including exceptions and `SIGSEGV`
handling (`§20.19`, U-F19). `Terminal` is not an emulator: it abstracts only the
capabilities the application cares about.

### 8.4 Adaptive layout (`§21`)

```cpp
namespace ymh {

enum class LayoutMode : std::uint8_t { Narrow, Normal, Wide };
LayoutMode calculateLayout(int width);

} // namespace ymh
```

```text
< 90 columns     conversation + inline tool activity + input
90–130 columns   conversation + tool activity + input
> 130 columns    conversation | activity/files side panel + input
```

The width comes from `Terminal::size()`; a `SIGWINCH` marks `Layout` dirty and
recomputes `LayoutMode`. The side panel (Files/Changes/Tools/Context) is
pinned for the Wide mode and hidden below 130 columns (`§21`).

---

## 9. Input focus, keybindings, draft/history, cancellation

### 9.1 Focus rule (F6, `§20.26`)

Exactly one component owns keyboard focus (U8). The `OverlayManager` top layer,
when active, is the focus owner; the `InputView` receives nothing (`§20.26`).
This prevents dropped keystrokes and accidental session closes. The input
pipeline (`§20.20`) carries a focus token, not a global key handler.

### 9.2 Keybinding namespace (`§20.26`, `§25`)

```text
Ctrl+N          new conversation in the active workspace (global)
Ctrl+Shift+N    new workspace (new daemon host, pick directory)
                fallback: /workspace new
Ctrl+S          workspace/session switcher (global; XOFF caveat, §7.4)
Ctrl+P          command palette / switcher in filter mode
Ctrl+W          context-sensitive detach: detach session when the switcher is
                open or no word is edited; delete-word inside the input editor
Ctrl+C          cancel the active session only; when the switcher is open,
                cancels the switcher
```

Input-editor bindings (`§25`): `Enter` send, `Shift+Enter` newline, `Ctrl+D`
exit/EOF, `Ctrl+L` redraw, `Up`/`Down` history, `PageUp`/`PageDown` scroll,
`Ctrl+R` history search, `Tab` completion, `Esc` cancel popup. Slash commands
are detected by a leading `/` and delegated to `CommandRegistry` (`§26`);
`InputView` never hard-codes them.

### 9.3 Per-session draft and history (F6)

`InputModel` is one instance per `SessionId`, not one per active view
(`§20.22`, `§25`). Draft text, cursor, and history are preserved across both
session and workspace switches (U9, U-F9). Switching focus writes the outgoing
draft into its `SessionUiState.input` before the new one is bound to the view;
`InputView` holds no draft of its own.

### 9.4 Cancellation (F9, `§34`)

`Ctrl+C` cancels **only** the active session (`§20.24`, `§20.26`, F9):

```text
Ctrl+C
  │
  ▼
UiController::cancelActive()
  │  agent.cancel { session: activeSessionId }   (05 §7.5)
  ▼
HostConnection ──► daemon ──► Agent::cancel()      (06 §3.5)
```

When the switcher (or any overlay) is open, `Ctrl+C` cancels the overlay, not
the session (`§20.24`). Cancelling resolves a pending permission fail-closed
(`09` Q12) and leaves other sessions untouched (`06` A9, U10).

---

## 10. Subagent fan-in and coalescing (`§20.25`)

M concurrent subagents each streaming tokens into the parent `SessionUiState` is
O(N × M) churn. The pinned mitigations:

```text
1. batch at the source: aggregated SubagentDelta(subagentId, chunk) at ~30–60 Hz,
   not raw per-token deltas per subagent
2. give each subagent a SubagentViewModel under the parent session
3. coalesce per-frame into one parent update; the render tick folds all dirty
   sub-streams into one update
4. collapse subagents by default (one summary line), expanded on demand;
   reuse the §20.10 collapse pattern
5. render token-level deltas only for the active session and its expanded subagent;
   background sessions get summary-only
6. cap the visible subagent log to the last K lines
```

Result: O(1) re-render per frame for background updates plus O(M) collapsed
lines. The `SubagentCoalescer` (§4.8) lives in the daemon (producer side), so
raw per-token deltas never cross IPC; the supervisor receives at most one
aggregated delta batch per tick.

F11 subagent ID duality: an envelope for a subagent carries two IDs
(`SubagentEnvelope.parent` routes display; `SubagentEnvelope.subagent` keeps the
durable log replayable). Do **not** collapse them (`§20.25`, `06` A16, U12).

**v1 status.** `06` decision (i) pins subagents as synchronous in v1: the
spawning tool awaits the child's terminal state via `whenIdle()`. Detached/async
subagents and the §20.25 delta coalescer are **deferred to Phase B**; the model
shapes and the envelope are pinned now so no redesign is needed.

---

## 11. Permission dialog and attention UI (`§19`, `09`)

### 11.1 Request path (`09 §4.1`, `05 §7.6`)

```text
daemon: live PermissionRequested + permission.request (all subscribers)
   │
   ▼
HostConnection ──► UiEventAdapter::onPermissionRequest
   │
   ▼
PermissionRequested ──► UiModel: open PermissionDialog; session → WaitingForPermission
   │
   ▼
user answers ──► UiController::resolvePermission(PermissionId, PermissionDecision)
   │
   ▼
permission.decide { request_id, decision, scope }   (05 §7.6)
```

The dialog shows `summary` (bounded, redacted by 09), the tool name, and the
scope choices `{Once, Session, Always}`. `arguments` are shown only where 09
permits (e.g. the shell command being approved); file bodies are never shown
(`09` Q11, §4.3). The dialog is an `OverlayManager` layer and takes focus (§9.1).

### 11.2 Resolution and first-wins (`09 §4.4`)

The supervisor sends exactly one `permission.decide` per request. Multi-
supervisor fan-out is broadcast-first-decision-wins (`09` (h)); a late/duplicate
decision returns `InvalidParams` and the dialog dismisses idempotently (U-F14).
A pending request re-broadcast to a newly attached supervisor (`09` (o)) opens
the dialog there too; answering it resolves for all.

### 11.3 Background policy → attention (`09 §5`, F2)

`09` pins **surface-via-attention with a fail-closed timeout auto-deny** as the
single v1 background mechanism. The supervisor's obligations:

- `WaitingForPermission` is in the waiting set, so a blocked background session
  contributes to `waitingCount` and, in Phase B, arms the flash (`§20.23`,
  `09 §5.3`).
- The session's `SessionCell.attention` badge is set; it is **not** cleared by
  focusing the session (`09 §5.3`, U7).
- A detached daemon's pending request is re-broadcast on attach; the badge and
  count appear immediately (attachment never gates work, `09 §5.2`).

### 11.4 Headless/RPC parity (D19)

Attention is UI-only and derived (`§54 D19`). The core stays headless; the
equivalent session-state-change signal for headless/RPC frontends is deferred to
the headless/CLI component (§42) and is not specified here.

---

## 12. Live PTY testing hook (`§44`)

The supervisor must be **100% automatically testable**: a scripted PTY driver
stands in for the human, so no manual keystrokes are required to cover
multi-workspace attach, session switching, and permission handling (`§44`).

### 12.1 Seams (pinned)

```cpp
namespace ymh {

// Input source is an fd, not "stdin" hard-coded, so a PTY master can drive it.
class TerminalInput {
public:
    explicit TerminalInput(int fd);
    std::optional<KeyEvent> poll(std::chrono::milliseconds timeout);
};

// Deterministic render for golden tests AND PTY assertions.
// Pure: reads the model, writes ANSI to a caller buffer. Never a timer (D16).
std::string renderToAnsi(const UiModel&, TerminalSize, const Theme&);

} // namespace ymh
```

`UiApplication` takes its input fd and output sink from `TerminalConfig`; the
production path uses `STDIN_FILENO`/`STDOUT_FILENO`, and the test path uses the
pty slave. `renderToAnsi` is the same pure renderer the golden tests use, so a
PTY assertion and a golden fixture share one code path.

### 12.2 Scripted PTY driver

```text
spawn the real ymh binary under a PTY (forkpty / posix_openpt)
        ↓
write a prompt to the pty master (as typed keystrokes)
        ↓
read and parse the rendered ANSI from the pty master
        ↓
assert observable behavior: session created, assistant text streamed,
    tool call rendered, permission prompt handled, session resumes
```

The driver asserts on **structure and invariants** (events emitted, tools
invoked, final state), not exact prose, and tolerates model nondeterminism
(`§44`). A `tmux send-keys` + `capture-pane` variant is an accepted alternative.

### 12.3 Opt-in gating (`§44`)

The live layer uses a **real LLM** through the same `LLMProvider` seam as
production. It is gated by an API key plus an explicit opt-in flag and is
**skipped, not failed**, when absent, so the default suite stays hermetic and
offline. It runs as a separate CI stage, never in the fast default command.
Milestone gating: live multi-workspace PTY tests target Milestone 2; MVP live
tests cover only the single-process flow (`§44`).

---

## 13. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**U1 — Core never depends on FTXUI.** No target under `core/` links or includes
FTXUI; the presentation boundary is enforced by the link graph (`§20.1`,
`§54 D14`).

**U2 — Widgets own no application state.** FTXUI components render `UiModel` and
hold only view-local ephemera (scroll offsets); they never own the conversation,
tools, or sessions (`§20.4`, `§54 D13`).

**U3 — `Render()` is pure.** No I/O, network, database, long-lived allocation, or
timer advance inside any `Render()` (`§20.12`, `§54 D16`).

**U4 — Core/wire types stay out of the model; `UiEvent` never on the wire.** The
model sees only `UiEvent`/`WorkspaceEvent`; the wire carries the core `Event` in
a `SessionEnvelope` (`§20.6`, `05` T4/T5, `§54 D15`).

**U5 — Counts are a level snapshot.** `recompute()` sums per-session
`AgentState` across all attached workspaces on each edge; never per-edge
increment, never per token, never frame-sampled (`§20.23`, `§9.9`).

**U6 — Flash is an edge notification ticked in the model.** The flash phase
lives in `AggregateStatusModel`, is armed on the pinned edges for any session
(focused or not), and is advanced only by `onTick` (`§20.23`, F12, `§54 D16`).

**U7 — Focus is display-only.** Focus/attach/detach never gates, starts,
suspends, or cancels work; one active session per workspace; activation is
daemon-driven (`04` (m), `06` A7/A8, `§9.9`). Focusing never removes a session
from the waiting count (`§20.23`).

**U8 — Exactly one keyboard focus owner.** The active overlay owns focus; the
`InputView` receives nothing while it is open (`§20.26`, F6).

**U9 — Per-session draft/history.** `InputModel` is keyed by `SessionId` and
survives session and workspace switches (`§25`, F6).

**U10 — Cancellation is scoped to the active session.** `Ctrl+C` cancels only
`activeSessionId`; with an overlay open it cancels the overlay (`§20.24`,
`§20.26`, F9, `06` A9).

**U11 — Per-session dirty flags plus `aggregateDirty`.** A background burst marks
only its own bits (and `Aggregate` when counts change), never the active view's
conversation (F7, `§20.23`).

**U12 — Subagent ID duality is preserved.** `parent` routes display; `subagent`
keeps durability/replay; they are never collapsed (`§20.25`, F11, `06` A16).

**U13 — Bounded UI buffers.** Conversation, tool, and subagent buffers are
bounded; full output is retained only for active/expanded sessions (F5, `§9.11`).

**U14 — The UI thread never blocks on a non-active session or daemon.** All
awaits are asynchronous; no `Render()` or input handler blocks (`§54 D20`).

**U15 — No sensitive content in logs.** Prompts, file bodies, and tool output are
never written to the `tui` log category; the session event log is the
authoritative trace (`§40`, `§46`).

**U16 — Attention is UI-only and derived.** The core stays headless; the waiting
count and flash are computed in the frontend (`§54 D19`, `§9.9`).

**U17 — `IXON` is disabled so `Ctrl+S` reaches the app.** The fallback binding
`Ctrl+P` and `/switch` exist for terminals that ignore the change (`§20.24`,
`§25`).

**U18 — The TUI is fully PTY-drivable.** Input is an fd and the renderer is pure,
so a scripted PTY driver can start a session, type a prompt, and observe rendered
output with no manual keystrokes (`§44`).

---

## 14. Failure modes

### 14.1 Shared findings (F1–F12, `§54`)

The supervisor's responsibilities for the existing findings:

| F# | Finding | Supervisor handling |
|---|---|---|
| **F1** | path/process isolation | never resolves paths, never `chdir()`s; `cwd` is display-only (§4.2, `§9.7`) |
| **F2** | background permission | surface-via-attention badge + waiting count; the dialog is shown for any session (§11.3, `09 §5`) |
| **F3** | late event after close | tombstone the session until the stream ends (`event.unsubscribed`), then drop (`05 §8.4`, `01 §9.5`, `§9.8`, U-F1) |
| **F4** | edge-triggered attention | **owned here**: `AgentStateChanged` → count + flash (§6, `06 §3.3`) |
| **F5** | output ring buffers | bounded conversation/tool/subagent buffers; full output only active/expanded (U13) |
| **F6** | input/keybinding focus | **owned here**: one focus owner; per-session draft/history (§9, U8/U9) |
| **F7** | per-session dirty flags | **owned here**: per-session bits + `aggregateDirty` (§3.5, U11) |
| **F8** | resource caps | out of scope; the UI allocates nothing unbounded and owns no executor (`§9.11`) |
| **F9** | cancellation scoping | **owned here**: `Ctrl+C` cancels only the active session (§9.4, U10) |
| **F10** | resume-suspended | sessions reopen `Idle`; the UI never auto-starts a turn on focus (`06` A15, U7) |
| **F11** | subagent ID duality | **owned here for display**: keep both IDs, never collapse (§10, U12) |
| **F12** | flash clock in model | **owned here**: phase in the model, ticked by `onTick`, never in `Render()` (§6.2, U6) |

**Explicitly out of scope:** F1's resolution arithmetic (07), F2's policy and
timeout (09), F3's durable tombstone (01), F5's store-side rings (04/07), F8's
caps (04/07), F10's resume transaction (01/06), and F11's durable edges
(01/06).

### 14.2 Component-local failure modes (`U-F#`)

These are component-local to the supervisor and must be covered by tests (§16.6).
The `U-F#` namespace is disjoint from every other component's failure namespace.

| U-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **U-F1** | Late event after close (F3) | envelope after `event.unsubscribed` | tombstone until the stream ends; never render a closed session; never drop a durable event silently |
| **U-F2** | Daemon died while attached | heartbeat loss / socket EOF | `WorkspaceEvent{DaemonDied}` → `daemonStatus := Dead`; mark sessions stale; keep the UI responsive (U14) |
| **U-F3** | Missed `AgentState` edge | count diverges from stream | `recompute()` self-heals on the next edge; the count is never increment-only (U5) |
| **U-F4** | Flash never ticked / ticked in `Render()` | `onTick` not called; timer in a widget | flash stays `Flashing` only if `onTick` is absent; `Render()` must not advance it; test asserts purity (U6) |
| **U-F5** | `Ctrl+S` swallowed by XOFF | `IXON` not cleared | disable `IXON`; fall back to `Ctrl+P` and `/switch` (U17) |
| **U-F6** | Overlay steals keys | `InputView` receives a key while overlay open | exactly one focus owner; input gets nothing (U8) |
| **U-F7** | Focus change gates/cancels work | `session.activate` sent on focus | focus is display-only; never send activate/suspend on focus (U7) |
| **U-F8** | `Ctrl+C` cancels the wrong session | cancel with a non-active target | cancel only `activeSessionId`; overlay-open cancels the overlay (U10) |
| **U-F9** | Per-session draft lost on switch | switch then switch back | draft/history preserved per `SessionId` (U9) |
| **U-F10** | Background burst jitters the active view | background tokens mark `Conversation` | per-session bits only; `aggregateDirty` for counts (U11) |
| **U-F11** | Cursor gap / silent reset on reconnect | `CursorInvalid` handled as `now` | resume with the cursor; on `CursorInvalid` re-subscribe `beginning`, never silent `now` (`05` T16) |
| **U-F12** | Subagent token flood | per-token deltas across IPC | producer-side coalescer drains per tick; collapse by default; cap K lines (§10) |
| **U-F13** | Permission request invisible (background) | `WaitingForPermission` with no dialog | badge + waiting count; dialog on attach; fail-closed timeout (`09 §5`, §11.3) |
| **U-F14** | Duplicate/late permission decision | two decisions for one `request_id` | first wins; later returns `InvalidParams`; dismiss idempotently (`09 §4.4`) |
| **U-F15** | Side effect in `Render()` | static + runtime check | forbidden; `Render()` is pure (U3) |
| **U-F16** | FTXUI leaks into core | link/static check | forbidden; `core/` does not link FTXUI (U1) |
| **U-F17** | Unbounded output buffer | memory growth under `grep -R`/`cmake` | ring-buffer cap; full output only active/expanded (U13, F5) |
| **U-F18** | PTY driver hangs | no API key / no output | bounded read/expect timeouts; skip (not fail) when gated off (`§44`) |
| **U-F19** | Terminal not restored on crash | exception/`SIGSEGV` while raw | RAII restore of raw mode/alternate screen on every exit path (`§20.19`) |
| **U-F20** | `UiEvent` serialized on the wire | encode/decode inspection | forbidden; the wire carries only core `Event` (U4, `05` T4) |

---

## 15. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`§55`). The supervisor maps onto it
as follows.

| dsh concept | ymh supervisor | Reference |
|---|---|---|
| UI plugin / frontend | `UiApplication` + `UiModel` | `§20.2`, `§54 D10` |
| Event consumer | `UiEventAdapter` (core → `UiEvent`) | `§20.6`, `§54 D15` |
| Session/workspace manager view | tree switcher (`SwitcherOverlayModel`) | `§20.24` |
| Agent state / attention | `SessionUiState.attention` + `AggregateStatusModel` | `§20.15`, `§20.23` |
| Append-only trace | the session event log, not the TUI | `§54 D2`, `§40` |
| Approval prompt | `PermissionDialog` over `permission.request` | `09 §4`, `05 §7.6` |
| Raw terminal handling | `TerminalLayer` (raw mode, `IXON`, caps) | `§20.19` |
| Process/session topology | supervisor + N workspace daemons | `§9.6`, `§54 D17` |

**Deliberate omissions** (accepted for v1, `§55`): no offline session cache (a
live host is required), no hot plugin reload, and no GUI/RPC *renderer* — the
`UiModel` is frontend-agnostic so a future GUI/RPC frontend reuses the model but
is out of scope here (`§59`).

---

## 16. Test plan

Strategy is `§44`: unit, integration (fake host / `FakeLLM`), golden, replay, and
a separate live PTY/real-LLM layer. The deterministic layers run offline; the
live layer is opt-in and API-key gated (`§44`, `§45`).

### 16.1 Unit tests (models)

- **`AggregateStatusModel` (U5/U6)**
  - `recompute()` over a synthetic map: N sessions in each state → exact counts;
    `WaitingForPermission`/`WaitingForInput`/`Error` count as waiting,
    `Thinking`/`CallingTool` as active, `Idle`/`Cancelling` as neither.
  - a missed edge then a later edge yields the correct snapshot (self-healing).
  - `armOnEdge()` arms on `Thinking→Waiting*`, `Thinking→Error`,
    `CallingTool→Idle`; does **not** arm on `Waiting→Thinking`, `Idle→Thinking`,
    or `Thinking→CallingTool`.
  - `flash.enabled == false` (Phase A) never arms; `tick()` moves
    `Flashing→Done` at ~1s; `Render()`-purity is asserted by a side-effect probe.
  - arming is independent of focus: `armOnEdge()` takes no focus argument, so the
    adapter arms for every `AgentStateChanged`, focused or not.
- **`DirtySet` (U11)**
  - a background session's mark does not set `Conversation` for the active
    session; `markAggregate()` sets only `Aggregate`.
- **`SwitcherOverlayModel` (U8)**
  - `moveUp/Down` cross workspace boundaries; `toggleExpand` hides/shows
    sessions; `/` filter narrows nodes.
- **`InputModel` per session (U9)**
  - draft/cursor/history round-trip across a switch; two sessions keep separate
    histories.
- **`SubagentModel` (U12/U13)**
  - `tail` is capped at K; collapse default; expanded toggling.
- **`renderToAnsi` (U18)**
  - the same model + size yields byte-identical output (pure).

### 16.2 Integration tests (fake host, `FakeLLM`, `§45`)

- A fake `HostConnection` injects scripted `SessionEnvelope`s; assert the model
  projection, the counts, and the dirty bits.
- **Focus independence (U7):** changing `activeSessionId` sends **no** RPC and
  changes no daemon-visible state; a background session keeps advancing.
- **Cancellation (U10):** `Ctrl+C` emits exactly one `agent.cancel` for the
  active session; with the overlay open it closes the overlay and sends nothing.
- **Reconnect (U-F11):** drop + reconnect resumes with the cursor; the projected
  state has no gap and no duplicate (`05` T8).
- **Late event after close (U-F1):** a stray envelope after
  `event.unsubscribed` is tombstoned, not rendered.
- **Daemon death (U-F2):** EOF flips `daemonStatus` to `Dead` and the UI stays
  responsive.
- **Permission round-trip (U-F13/U-F14):** `permission.request` → dialog →
  `permission.decide`; a duplicate decision returns `InvalidParams` and does not
  re-close the dialog.

### 16.3 Golden TUI render tests (`§44`)

```text
given event stream
        ↓
renderToAnsi (pure)
        ↓
expected terminal representation
```

Fixtures cover: narrow/normal/wide layouts; the tree switcher; the aggregate line
with counts and (Phase B) the flash color; the permission dialog; a collapsed
subagent panel; and per-session drafts across a switch. The flash fixture pins
`Flashing` vs `Done` by setting `FlashState.phase` directly (no timer).

### 16.4 Replay tests (`§44`)

Given a recorded event stream, `same input → same projected state`: replay into
`UiEventAdapter` + `UiModel` and assert the conversation, tools, counts, and
switcher nodes are identical run to run. This is the event-sourcing payoff
(`§9.5`, `§54 D2`).

### 16.5 Live end-to-end tests (real LLM, PTY-driven, opt-in, `§44`)

- Spawn the real `ymh` binary under a PTY; type a prompt; parse the rendered ANSI;
  assert session creation, streamed assistant text, a rendered tool call, a
  handled permission prompt, and resume (§12).
- Multi-workspace: attach to two workspaces, switch focus via the switcher, and
  confirm the aggregate count updates without gating work (Milestone 2).
- Gated by API key + explicit opt-in; skipped when absent; structure/invariant
  assertions only; separate CI stage (`§44`).

### 16.6 Failure-mode coverage matrix

| U-F# | Unit | Integration | Golden | Replay | Live |
|---|---|---|---|---|---|
| U-F1 | — | late-after-close | — | replay + stray | — |
| U-F2 | — | daemon death | dead badge | — | kill daemon |
| U-F3 | recompute table | missed edge | — | — | — |
| U-F4 | tick/purity probe | — | flash phases | — | — |
| U-F5 | — | keymap | — | — | `Ctrl+S` over PTY |
| U-F6 | switcher model | focus routing | overlay keys | — | switcher |
| U-F7 | — | no-RPC-on-focus | — | — | switch focus |
| U-F8 | — | cancel scope | — | — | cancel in TUI |
| U-F9 | per-session draft | switch round-trip | drafts golden | — | — |
| U-F10 | dirty-set table | background burst | — | — | — |
| U-F11 | — | reconnect cursor | — | replay cursor | — |
| U-F12 | cap/collapse | coalescer drain | subagent panel | — | — |
| U-F13 | — | background request | badge golden | — | permission |
| U-F14 | — | duplicate decision | — | — | multi-supervisor |
| U-F15 | static probe | — | — | — | — |
| U-F16 | link check | — | — | — | — |
| U-F17 | ring cap | large output | bounded view | — | — |
| U-F18 | — | — | — | — | gated skip |
| U-F19 | — | restore-on-throw | — | — | crash restore |
| U-F20 | encode check | wire inspection | — | — | — |

### 16.7 Invariant coverage

| Invariant | Covered by |
|---|---|
| U1 | link/static check (`core/` does not link FTXUI) |
| U2 | component review + golden render (widgets read-only) |
| U3 | `Render()` purity probe; static scan for I/O in `Render` |
| U4 | encode check (no `UiEvent` on the wire) + model type test |
| U5 | `recompute()` table + missed-edge self-heal |
| U6 | flash arm/tick unit + golden phases |
| U7 | focus-independence integration (no RPC on focus) |
| U8 | focus-routing integration + switcher unit |
| U9 | per-session draft unit + switch round-trip |
| U10 | cancel-scope integration + live `Ctrl+C` |
| U11 | `DirtySet` unit + background-burst integration |
| U12 | subagent unit + fan-in replay |
| U13 | cap unit + large-output integration |
| U14 | async-await review + responsive-UI test |
| U15 | log-inspection test (`tui` category) |
| U16 | review: attention only in the frontend |
| U17 | `IXON` unit (termios) + PTY `Ctrl+S` |
| U18 | `renderToAnsi` purity + live PTY driver |

---

## 17. Decisions and open questions

### 17.1 Decisions (pinned by this spec)

- **(a) The wire carries the core `Event`; `UiEvent` is frontend-only.**
  `SessionEnvelope` is exactly `{session, event}` and `UiEvent` never appears on
  the wire (`§20.22`, `05` T4/T5, `§54 D15`, U4).
- **(b) One `UiModel`, a workspace map, and a cross-workspace session map.**
  The `§20.4` `UiModel` is superseded by the `§20.22` shape; `apply()` takes the
  adapted `UiEvent` (`§20.22`).
- **(c) `SessionCell` and `AttentionState` are pinned here.** `§20.22` references
  them without defining them; both are bounded projections and hold no core
  pointer (§4.4).
- **(d) The waiting count is a level snapshot recomputed on every `AgentState`
  edge across all attached workspaces.** Never a per-edge increment, never per
  token, never frame-sampled; self-healing (`§20.23`, `§9.9`, U5).
- **(e) The flash is an edge notification armed on `Thinking|CallingTool →
  Waiting*|Error` (needs input) or `→ Idle` (done), for any session (focused or
  not; OQ-1 resolved fix-now).** The phase lives in `AggregateStatusModel` and is
  ticked at model level, never in `Render()` (`§20.23`, `§9.9`, F12, `§54 D16`,
  U6).
- **(f) Focusing a session does not remove it from the waiting count; focus is
  display-only.** The count drops only when the session's `AgentState` leaves the
  waiting set (`§20.23`, `09 §5.3`, U7).
- **(g) Activation is daemon-driven; the supervisor never sends
  `session.activate` on focus.** `session.activate`/`session.suspend` are
  explicit Interactive-only controls (`04` (m), `05 §6.1`/§7.4, U7).
- **(h) Per-session dirty flags plus `aggregateDirty`.** A background burst marks
  only its own bits; only the active session renders its conversation body (F7,
  U11).
- **(i) The `IXON` fix is mandatory; `Ctrl+P` and `/switch` are the fallback.**
  `TerminalLayer::enterRawMode()` clears `IXON` (`§20.24`, `§25`, U17).
- **(j) `Ctrl+C` cancels only the active session; an open overlay cancels
  first.** Cancellation is scoped per session (`§20.24`, F9, U10).
- **(k) Per-session draft/history is keyed by `SessionId`, not by view.** Drafts
  survive session and workspace switches (`§25`, F6, U9).
- **(l) Subagent fan-in keeps both IDs and coalesces producer-side.** The
  coalescer is owned by 04 (daemon producer side) and deferred to Phase B; the
  supervisor receives at most one aggregated batch per tick (`§20.25`, `06` (i),
  U12, §1.3).
- **(m) Permission UI is a focus-owning overlay; decisions are first-wins.**
  Background requests surface via attention and fail closed (`09 §4`/§5, §11).
- **(n) Phase A ships counts without the flash; Phase B enables it.** The
  `FlashState` shape is pinned now; `flash.enabled` gates arming (`§9.9`,
  `§57` Steps 11–12).
- **(o) Input is an fd and the renderer is pure, so the TUI is PTY-drivable.**
  The live layer is opt-in and skipped when gated off (`§44`, U18).
- **(p) The `§20.22`/`§20.23` shapes are extended for the pinned multi-workspace
  model.** Relative to `§20.22`: `SessionUiState` adds `workspace` (reverse
  routing index), `subagents` (`§10`), and `dirty` (per-session F7 flags), and
  `UiModel` adds `switcher`/`dirty`. Relative to `§20.23`: `recompute()` takes
  `(workspaces, sessions)` so it can sum the cross-workspace session map, and the
  model adds `armOnEdge()` plus `FlashState.enabled`. These extensions are
  additive and weaken no architecture invariant (`§20.22`, `§20.23`, §4.3, §4.7).

### 17.2 Open questions (adjudicated)

Each item records its disposition; none blocks the component gate. OQ-1 is
resolved (fix-now) and folded into decision (e) and §6.2, so it is no longer
open.

- **OQ-2 — Headless/RPC attention parity (D19). [deferred]** The equivalent
  session-state-change signal for headless/RPC frontends is deferred to the
  headless/CLI component (`§42`), which is outside the ten-spec plan. Until it is
  specified, D19's "headless frontends receive an equivalent signal" is only
  partially discharged (the core `AgentState` enum is available, but no
  event/notification shape is pinned).
- **OQ-3 — Explicit `session.activate` affordance. [deferred]** `04` (m)/`05`
  §7.4 permit `session.activate` as an operator control, but no TUI binding or
  slash command is pinned. This spec deliberately binds none (focus ≠ activation).
  A future `/session activate` command is possible without redesign.
- **OQ-4 — `Ctrl+Shift+N` reachability over SSH. [deferred]** Many terminals
  cannot distinguish `Ctrl+Shift+N` from `Ctrl+N`. The pinned fallback is
  `/workspace new` (`§20.26`); a terminal-capability probe
  (`modifyOtherKeys`/`kittyKeyboard`, `§20.19`) could refine this later.
- **OQ-5 — Wide-layout side panel default. [deferred]** `§21` lists
  Files/Changes/Tools/Context; this spec pins the panel for `> 130` columns but
  not which tab is default. A minor UX decision, recorded for implementation.
- **OQ-6 — `WorkspaceEventKind` vs. `HostNotice` overlap. [deferred]**
  `WorkspaceEventKind` (`§20.22`) and 05's `HostNoticeKind` (`05 §5.3`) overlap
  on session open/close. This spec derives `WorkspaceEvent` from `HostNotice`
  plus connection state; if 05 later adds a distinct notice, the mapping is
  additive.

---

## 18. References

- `00-architecture.md` — §1, §4.1–§4.4, §8.1–§8.4, §9.6–§9.11, §20.1–§20.26,
  §21–§25, §26, §30, §33–§35, §40–§44, §45, §46, §47, §49–§50, §54 (D1–D23,
  F1–F12), §55, §57–§59.
- `01-session.md` — §2.1, §3, §4.5, §4.6, §5, §8, §9, I4/I6/I11/I16–I20.
- `02-persistence.md` — §5.7, §6.1–§6.2, P-F13, P-F15.
- `03-workspace-registry.md` — §2.1, §3.3, §4.1–§4.2, R8, R16.
- `04-workspace-host-daemon.md` — §2.1, §3.6, §4.2, §6.3–§6.6, §9, (m),
  H8/H9/H17, D-F9/D-F22.
- `05-transport.md` — §2.2, §3.2–§3.3, §4.1, §5.1–§5.5, §6.1–§6.3,
  §7.2–§7.7, §8.1–§8.6, T4–T24, T-F13–T-F20.
- `06-agent-loop.md` — §3.1–§3.5, §4.1–§4.4, §5.6–§5.8, §6, §7, A1–A17, (c),
  (i), (j), A-F1–A-F17.
- `07-tools-execution.md` — §4, §5.4, §6.8, §8.1, §8.3, X7, E-F17.
- `08-llm-provider.md` — §2.2, (k), (r).
- `09-permissions.md` — §2.2–§2.3, §3.1–§3.7, §4.1–§4.7, §5.1–§5.4, Q3/Q4/Q8/
  Q9/Q10/Q11/Q12/Q13, (g), (h), (n), (o).
- `HANDOFF.md` — §6 row 10, §7.

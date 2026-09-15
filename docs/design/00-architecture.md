# C++ Coding Harness — Architecture & Implementation Design

**Status:** Architecture baseline / implementation-ready design  
**Target:** Linux terminal over SSH  
**Primary language:** Modern C++ (C++23 recommended)  
**UI:** Native terminal UI  
**Initial deployment:** Single Linux executable (one binary, two run modes: supervisor TUI and workspace daemon)  
**Design inspiration:** Claude Code, OpenCode, and especially DeepSeek Harness/Cordis

---

## 1. Executive Summary

This project is a personal, native C++ coding-agent harness intended to provide the capabilities of Claude Code / OpenCode while remaining:

- terminal-first,
- excellent over SSH,
- lightweight,
- highly responsive,
- modular,
- model-provider agnostic,
- extensible through plugins/capability providers,
- capable of local or remote execution,
- persistent and replayable,
- suitable for future MCP/LSP/subagent integration.

The central architectural decision is:

> **The agent is not the TUI. The TUI is one consumer of an event-driven agent runtime.**

A second major decision is:

> **Sessions are event-sourced. The append-only session event stream is the durable source of truth.**

A third is:

> **Capabilities are replaceable seams.**

The runtime should therefore be organized around:

```text
                         ┌──────────────────────┐
                         │      Frontends       │
                         │                      │
                         │  TUI / CLI / RPC     │
                         └──────────┬───────────┘
                                    │
                              Event Stream
                                    │
                         ┌──────────▼───────────┐
                         │      Agent Runtime    │
                         │                       │
                         │  Agent / Loop         │
                         │  Context              │
                         │  Session              │
                         │  Policy               │
                         └──────────┬────────────┘
                                    │
                ┌───────────────────┼───────────────────┐
                │                   │                   │
                ▼                   ▼                   ▼
              LLM                Tools             Execution
          provider seam       tool registry       environment
                │                   │                   │
       OpenAI/Anthropic       FS/Shell/Git       Local/SSH/Container
       vLLM/Ollama/etc.       MCP/LSP/etc.
```

DeepSeek Harness's current architecture strongly validates this direction: its capabilities—including model adapters, tools, sessions, storage, sandboxes, loops and UI—are plugins, with services and typed events connecting them. It also treats the append-only session log as the basis for traceability, resume, fork, search and replay. [DeepSeek Harness Architecture] citeturn0search0turn0search1

This project should **adopt those architectural ideas without attempting to reproduce the entire DeepSeek/Cordis framework**.

---

# 2. Goals

## 2.1 Primary goals

### G1 — Excellent SSH experience

The application must work exceptionally well through:

```text
ssh user@server
```

It must tolerate:

- narrow terminals,
- wide terminals,
- resizing,
- high latency,
- dropped/reconnected SSH sessions,
- UTF-8 terminals,
- tmux,
- screen,
- ordinary xterm-compatible terminals.

The UI must never depend on a browser.

### G2 — Native, low-overhead runtime

The application should be a normal Linux executable with:

- fast startup,
- modest memory usage,
- no Node.js runtime requirement,
- no browser,
- no Python runtime requirement.

### G3 — Model independence

The agent core must not know whether the model is:

- Anthropic,
- OpenAI,
- DeepSeek,
- local vLLM,
- Ollama,
- another OpenAI-compatible endpoint,
- a future provider.

### G4 — Tool independence

Tools must be registered capabilities rather than hard-coded agent-loop branches.

Initial tools:

```text
read_file
write_file
edit_file
grep
glob
shell
git
```

Later:

```text
lsp
mcp
web
terminal
background_job
subagent
```

### G5 — Durable sessions

A session must survive process termination and allow:

- resume,
- search,
- replay,
- inspection,
- fork,
- context reconstruction.

### G6 — Safe execution

Tool execution must pass through an explicit policy layer.

Examples:

```text
read_file       auto
grep            auto
git diff        auto
write_file      ask
git commit      ask
shell command   ask
```

### G7 — Extensibility

A new capability should normally be added by registering a provider/plugin rather than editing the agent loop.

### G8 — Future remote execution

The same agent should eventually be able to operate against:

```text
LocalEnvironment
SSHEnvironment
ContainerEnvironment
SandboxEnvironment
```

without changing agent logic.

---

# 3. Non-Goals

The first implementation should NOT attempt to reproduce every feature of DeepSeek Harness.

Do not initially implement:

- hot plugin replacement,
- browser UI,
- distributed agent orchestration,
- elaborate dependency injection,
- full Cordis compatibility,
- arbitrary third-party dynamic binary plugins,
- cloud account management,
- complex workflow authoring,
- multi-user authentication.

These can be added later if the architecture proves useful.

---

# 4. Architectural Principles

## 4.1 Agent != UI

Never allow the agent core to depend on FTXUI.

Bad:

```text
Agent -> TUI
```

Good:

```text
Agent -> EventBus
TUI   -> EventBus
```

The agent should be executable without a terminal UI.

---

## 4.2 Event stream is the runtime spine

The durable session event stream is the canonical representation of what happened.

For example:

```text
session/start
turn/start
user/message
step/start
assistant/chunk
assistant/message
tool/call
tool/result
step/end
turn/end
```

Live control events can coexist with durable events:

```text
agent/request
agent/cancel
agent/interrupt
tool/pre_execute
tool/post_execute
permission/request
```

These slash names (`agent/*`, `tool/*`, `permission/*`) are the **wire forms** of
the CamelCase live events enumerated in §8.1; §8.1 is the canonical home for
live-event naming, and §4.2 keeps only the wire spelling.

DeepSeek Harness uses this distinction between durable session events and live extension events; its turn/step lifecycle also derives subsequent model history from the session log. citeturn0search0turn0search4

---

## 4.3 Services for capabilities, events for interception

Use a service interface when a consumer wants to directly perform an operation:

```cpp
llm->stream(request);
fs->read(path);
sessions->append(event);
```

Use events when a component wants to observe, modify, authorize, or intercept behavior:

```text
tool/pre_execute
agent/pre_step
llm/chunk
permission/request
session/event
```

This follows the same useful separation described in Cordis: services expose stable capability seams, while events provide extension/interception points. citeturn0search6

---

## 4.4 Execution environment is a capability boundary

Filesystem, subprocesses, PTYs, Git and LSP should conceptually belong to one execution world.

```text
ExecutionEnvironment
 ├── Filesystem
 ├── Process
 ├── PTY
 ├── Git
 └── LSP
```

This allows:

```text
LocalEnvironment
SSHEnvironment
ContainerEnvironment
SandboxEnvironment
```

without duplicating every tool.

This is directly inspired by DeepSeek Harness's capability-seam design, where filesystem and subprocess providers share an execution world so moving to a remote sandbox can move Bash, PTY and LSP together. citeturn0search0

---

# 5. High-Level Components

```text
src/
├── app/
├── core/
│   ├── agent/
│   ├── events/
│   ├── session/
│   ├── context/
│   ├── plugins/
│   └── policy/
├── llm/
├── tools/
├── execution/
├── registry/
│   └── workspace_registry.hpp
├── daemon/
│   ├── workspace_host.hpp
│   └── session_manager.hpp
├── supervisor/
│   └── supervisor.hpp
├── ui/
│   ├── UiApplication.hpp
│   ├── UiController.hpp
│   ├── UiModel.hpp
│   ├── UiEvent.hpp
│   ├── model/
│   ├── components/
│   ├── render/
│   ├── terminal/
│   └── theme/
├── storage/
├── protocol/
├── mcp/
├── lsp/
├── git/
└── main.cpp
```

Logical components:

```text
Supervisor process (TUI)
 │
 ├── Frontend
 │    └── TUI
 ├── WorkspaceRegistry (shared registry.db, §9.10)
 └── HostClient ── Unix-socket JSON-RPC 2.0 ──┐
                                             │
WorkspaceHost daemon (one per workspace) ◄───┘
 │
 ├── Runtime
 │    ├── PluginRegistry
 │    ├── ServiceRegistry
 │    ├── EventBus
 │    └── Configuration
 │
 ├── SessionManager
 │    └── Session[1..N]
 │
 ├── Agent
 │    ├── AgentHandle
 │    ├── AgentLoop
 │    ├── ContextAssembler
 │    └── PermissionPolicy
 │
 ├── Session
 │    ├── EventStore
 │    ├── Replay
 │    ├── Fork
 │    └── Search
 │
 ├── LLM
 │    ├── ProviderRegistry
 │    └── Streaming
 │
 ├── Tools
 │    ├── ToolRegistry
 │    └── ToolPipeline
 │
 └── Execution
      ├── Filesystem
      ├── Process
      ├── PTY
      └── Environment
```

---

# 6. Runtime Context

A lightweight `Context` is the dependency-access object.

Conceptually:

```cpp
class Context {
public:
    template<class T>
    T& service();

    EventBus& events();
    PluginRegistry& plugins();

    Context child();
};
```

The context should not become a giant global singleton.

Instead:

```text
Root Context
   │
   ├── runtime services
   │
   └── Agent Context
         │
         ├── agent-local tools
         ├── agent-local policy
         └── agent-local state
```

DeepSeek's Cordis uses a context as the shared service/event access surface and supports child/scoped contexts. The C++ version should use the same conceptual model but with substantially less machinery. citeturn0search11

---

# 7. Plugin Model

## 7.1 Minimal interface

```cpp
class Plugin {
public:
    virtual ~Plugin() = default;

    virtual std::string name() const = 0;

    virtual void load(Context&) = 0;

    virtual void unload(Context&) {}
};
```

Registration:

```cpp
class CorePlugin : public Plugin {
public:
    void load(Context& ctx) override;
};
```

The first version should use statically compiled plugins:

```text
core
llm
tools
filesystem
shell
git
tui
```

Do not begin with `dlopen()`.

---

## 7.2 Plugin responsibilities

A plugin can:

- register a service,
- register tools,
- subscribe to events,
- register commands,
- contribute prompt sections,
- register configuration,
- create UI components.

Example:

```cpp
void GitPlugin::load(Context& ctx)
{
    ctx.services().provide<GitService>(
        std::make_shared<GitService>(...));

    ctx.tools().register_tool(make_git_tool(...));

    ctx.events().subscribe<ToolResultEvent>(
        [](const auto& event) {
            ...
        });
}
```

---

## 7.3 Lifecycle

Eventually:

```text
DECLARED
   ↓
LOADING
   ↓
ACTIVE
   ↓
UNLOADING
   ↓
DISPOSED
```

A plugin owns all registrations it creates.

Unload must remove:

- services,
- event handlers,
- tools,
- commands,
- timers,
- background tasks.

DeepSeek/Cordis explicitly treats plugin registrations as reversible effects and gives plugins lifecycle states and cleanup guarantees. citeturn0search5

---

# 8. Event System

## 8.1 Event categories

### Durable session events

These must survive process restart.

```text
SessionStarted
SessionEnded
TurnStarted        (dsh turn/start)
TurnEnded          (dsh turn/end)
UserMessage
AssistantChunk
AssistantMessage
ToolCall
ToolResult
StepStarted        (dsh step/start)
StepEnded          (dsh step/end)
ContextInjected
PermissionDecision
```

The turn/step pair is dsh's session/turn/step taxonomy: a turn opens with
`turn/start`, each model step inside it with `step/start`, and both close
symmetrically with `step/end` and `turn/end`. Match dsh's `SessionEventMap`
names so replay and cross-harness tooling line up. The C++ enum uses CamelCase
(`TurnStarted`); the wire/JSON `type` string uses the slash form
(`turn/start`, `turn/end`, `step/start`, `step/end`).

**Errata — the durable set is non-exhaustive.** The list above is the baseline,
not a closed set: component specs extend it. Named extensions include
`turn/cancel` (§34), `context/compaction` (§32), `usage` (§33),
`subagent/spawn` / `subagent/fan_in` (§30), and the `followup` inbox op whose
queued turn records `TurnStarted{origin=FollowUp}` (§10.1, spec 06). A component
spec that adds a durable event extends this set; it does not contradict it.

### Live events

These are runtime notifications.

```text
AgentStarted
AgentStopped
AgentWaiting
LLMRequestStarted
LLMChunkReceived
ToolExecutionStarted
ToolExecutionFinished
PermissionRequested
TerminalOutput
Progress
Error
```

**Persistence rule.** Only DURABLE session events are appended to the session
log. LIVE events are runtime-only notifications and are NOT persisted. A
`SessionStore` therefore subscribes to durable events only (§8.3).

---

## 8.2 Event interface

```cpp
struct Event {
    EventId id;
    SessionId session_id;
    std::chrono::system_clock::time_point timestamp;
    EventType type;
    nlohmann::json payload;
};
```

Use strongly typed C++ event structures internally.

JSON can be the persistence/wire representation.

---

## 8.3 EventBus

```cpp
class EventBus {
public:
    template<typename Event>
    Subscription subscribe(
        std::function<void(const Event&)> handler);

    void publish(Event event);
};
```

Subscriptions must return RAII handles:

```cpp
class Subscription {
public:
    ~Subscription();
};
```

This prevents event-handler leaks.

### SessionId-routed delivery

**Q2.** "SessionId-routed" does not mean "tagged and best-effort". It means
per-session ordered delivery.

```text
EventBus (one, shared)
   │
   ├── global subscribers (logging, telemetry; SessionStore = durable events only)
   │
   └── per-session mailbox (SessionId → ordered FIFO)
            │
            ▼
       UiEventAdapter
            │
            ▼
         UiModel
```

Contract:

```text
events for session X are delivered in publish order
events for different sessions may interleave arbitrarily
global cross-session ordering is not guaranteed and is not required
```

Implement one shared `EventBus` plus a per-session mailbox between the bus
and the UI adapter, and between the bus and each session's agent loop. Do not
create N child buses: global concerns would then need a parent fan-out bus,
which re-derives the shared bus plus N subscriptions for strictly more
machinery (D12).

The mailbox also resolves F3. Before a closed session's `SessionUiState` is
freed, its mailbox must drain and its `AgentHandle` must report a terminal
state. See 9.8 and 20.22.

---

## 8.4 Waterfall/interception events

Some events should support middleware semantics:

```text
request
  ↓
policy A
  ↓
policy B
  ↓
agent
```

For example:

```cpp
ToolDecision on_pre_execute(
    ToolRequest,
    Next<ToolDecision>);
```

Possible outcomes:

```text
next()
allow
deny
modify request
```

This is analogous to Cordis waterfall events and is particularly useful for permissions and policy. citeturn0search6

---

# 9. Session Architecture

## 9.1 Session as event log

The session database is not merely a chat-history table.

It is an event log.

```text
Session
  |
  +-- Event 1
  +-- Event 2
  +-- Event 3
  +-- ...
```

The current conversation is a projection of the events.

---

## 9.2 SQLite schema

Initial schema:

```sql
CREATE TABLE sessions (
    id              TEXT PRIMARY KEY,       -- SessionId (UUIDv4, stable, never a path)
    cwd             TEXT NOT NULL,          -- canonical workspace root, immutable
    created_at      INTEGER NOT NULL,       -- epoch ms
    updated_at      INTEGER NOT NULL,       -- epoch ms of last appended event
    title           TEXT NOT NULL DEFAULT '',   -- display title ('' => derive)
    model           TEXT NOT NULL DEFAULT '',   -- default model id
    server_profile  TEXT NOT NULL DEFAULT 'interactive',
    kind            TEXT NOT NULL DEFAULT 'root',   -- root | fork | subagent
    parent_session  TEXT,                   -- fork/subagent parent, NULL if root
    seed_length     INTEGER,                -- # parent events copied; NULL for root/subagent; required for fork
    metadata        JSON,
    FOREIGN KEY(parent_session) REFERENCES sessions(id),
    CHECK (kind IN ('root','fork','subagent')),
    CHECK ((kind='root' AND parent_session IS NULL AND seed_length IS NULL) OR (kind='fork' AND parent_session IS NOT NULL AND seed_length IS NOT NULL) OR (kind='subagent' AND parent_session IS NOT NULL AND (seed_length IS NULL OR seed_length = 0)))
);

CREATE TABLE events (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    event_id TEXT NOT NULL UNIQUE,
    timestamp INTEGER NOT NULL,
    type TEXT NOT NULL,
    payload JSON NOT NULL,

    FOREIGN KEY(session_id) REFERENCES sessions(id)
);

CREATE INDEX idx_events_session
ON events(session_id, sequence);

CREATE TABLE session_leases (
    session_id     TEXT PRIMARY KEY,
    holder_pid     INTEGER NOT NULL,  -- host daemon that owns writes
    holder_boot_id TEXT NOT NULL,     -- holder's per-boot UUID (PID-reuse guard)
    acquired_at    INTEGER NOT NULL,
    expires_at     INTEGER NOT NULL,  -- acquired_at + TTL
    FOREIGN KEY(session_id) REFERENCES sessions(id)
);
```

This is the **per-workspace** database, at `<workspace>/.ymh/sessions.db`,
owned by that workspace's daemon host. It holds three **source-of-truth**
concerns: `sessions` (headers), `events` (the append-only log), and
`session_leases` (the cross-process write lease, §9.7), plus a derived,
discardable snapshot cache (`session_snapshots`, spec 02 §6). The snapshot cache
is never a source of truth and may be deleted and rebuilt from `events` at any
time (spec 02 §3.5).

The old single-row `workspace` table is removed. The open-set is cross-workspace
state and lives in the shared workspace registry (§9.10), not in this database.
The focused/active session is **supervisor-local** state (held in `WorkspaceModel`
/ `UiModel`, §20.22), not in the registry either. Keeping both out of the event
log preserves D2/D21.

**Connection pragmas (required).** SQLite has foreign keys OFF by default, so
every connection that opens this database MUST set `PRAGMA foreign_keys=ON`,
`PRAGMA journal_mode=WAL`, and a busy timeout (`PRAGMA busy_timeout=<ms>`) as
part of opening it. These are required, not optional; the concrete values and
open/close policy belong in the persistence spec (02).

Do not prematurely normalize every event type into separate SQL tables. The
event payload can evolve independently.

---

## 9.3 Resume

On startup:

```text
open session
    ↓
load event stream
    ↓
rebuild projection
    ↓
restore agent state
    ↓
continue
```

---

## 9.4 Fork

A fork should create a new session referencing a boundary:

```text
Session A

1
2
3
4  <── fork boundary
5
6
```

creates:

```text
Session B
1
2
3
4
```

Prefer copy-on-write or shared-prefix semantics rather than physically duplicating large payloads.

---

## 9.5 Replay

Replay is valuable for debugging the agent itself.

```text
session
   ↓
event replay
   ↓
agent state reconstruction
   ↓
TUI reconstruction
```

This should eventually permit:

```text
ymh replay SESSION
```

---

## 9.6 Multi-workspace / supervisor model

The process topology is **one daemon host process per workspace**, aggregated
by a **supervisor** TUI process.

```text
Supervisor (TUI process)
   │  attaches to many hosts simultaneously
   ├───────────────┬───────────────┐
   ▼               ▼               ▼
WorkspaceHost A  WorkspaceHost B  WorkspaceHost C
 cwd=/proj/api    cwd=/proj/docs   cwd=/proj/tests
 SessionManager   SessionManager   SessionManager
 Session[1..N]    Session[1..M]    Session[1..K]
```

Three roles:

```text
WorkspaceRegistry  shared durable store (§9.10): canonical path ↔ workspace id
                   ↔ ordered sessions, host pid/socket/heartbeat
WorkspaceHost      one daemon per workspace; owns its cwd and hosts an
                   in-process SessionManager with N conversations
Supervisor         the TUI; attaches to N hosts at once (watch P2 while P1
                   flashes), routes UI events, enforces supervisor caps
```

Each daemon runs its own in-process `SessionManager` (the dsh model, scoped to
one workspace). Each session owns:

```text
SessionId
child Context (from the root Context, §6)
ExecutionEnvironment rooted at its own CWD
AgentHandle (§10.1)
policy (§19)
```

Shared spine *inside a daemon*:

```text
                 EventBus (SessionId-routed, §8.3)
                    │
      ┌─────────────┼─────────────┐
      ▼             ▼             ▼
 SessionStore   SessionManager   HostEventForwarder
 (SQLite)       (owns sessions)  (IPC to supervisor)
```

```cpp
struct SessionOptions {
    std::filesystem::path cwd;
    std::string serverProfile;   // Interactive | Automation
    std::string model;
    std::string title;
};

class SessionManager {           // one instance per WorkspaceHost
public:
    SessionId              createSession(SessionOptions);
    void                   closeSession(SessionId);
    void                   focusSession(SessionId);
    std::vector<SessionId> list() const;

    Agent&                 agent(SessionId);
    ExecutionEnvironment&  env(SessionId);
};
```

`SessionManager` owns the sessions **of its own daemon**. The focused/active
session is **supervisor-local** state — held in `WorkspaceModel` / `UiModel`
(§20.22), NOT in the shared registry and not a property of `SessionManager` —
because multiple TUI clients may each focus a different session. Cross-process
ownership is enforced by the write lease (§9.7).

The supervisor↔host wire is JSON-RPC 2.0 over a length-prefixed Unix domain
socket. `event.stream` notifications carry `SessionEnvelope{session, event}`
where `event` is the core, durable `Event` (§8.2) — never a frontend type; the
frontend adapts it through `UiEventAdapter` (§20.6, §20.22). Two server profiles
mirror dsh: Interactive (full event stream, the API-gateway equivalent) and
Automation (prompt/cancel/permission, the ACP equivalent). The protocol stays
transport-agnostic; TCP for remote runtimes is deferred. The supervisor attaches
to several hosts at once; v1 requires a live host (no offline session cache).

---

## 9.7 Session isolation, path safety, and the write lease

**F1 is solved structurally by process-per-workspace.** A `WorkspaceHost`
daemon owns exactly one workspace, so `chdir()` to that workspace at daemon
startup is safe: no other workspace shares the process. Every session in the
daemon is rooted at the same workspace cwd, and cross-project reads and writes
cannot follow.

Path safety is still enforced per session through the single
`ExecutionEnvironment` interface (§18); its `root()` / `resolve()` members are
the rooting contract (root-relative, never `getcwd()`).

Rules:

```text
chdir() to the workspace root once, at daemon startup (safe: one workspace/process)
use openat / openat2 with a dirfd rooted at the session root as optional
  defense-in-depth, or an absolute-path-join against root()
every tool, LSP, git, and subprocess builder resolves via resolve()
getcwd() is never a resolution base in tool code
Context, policy, and environment are per-session; only EventBus and store are shared
```

Canonicalization (DIV-8): every workspace path is normalized with
`std::filesystem::canonical()` (realpath: trailing slashes, `.`/`..`, and
symlinks resolved) before it is stored or compared. Uniqueness is string
equality of canonical paths, so a symlink to an owned directory collides with
it. Attach-time session-cwd checks go through the same canon. `WorkspaceId` is
a stable UUID, never the path.

**Cross-process write lease (DIV-3 / RISK-1).** With N daemons, concurrent
writes to one session's event log would corrupt it. Each session row carries a
`session_leases` entry (§9.2) that grants exclusive write access. Each row also
records the holder's **boot nonce** (`holder_boot_id`, a per-boot UUID) so a
reused PID is never mistaken for the original holder:

```text
acquire  INSERT (session_id, holder_pid, holder_boot_id, now, now+TTL)
renew    UPDATE expires_at WHERE holder_pid = me AND holder_boot_id = my_boot
steal    REPLACE when the row is expired, or when (holder_pid, holder_boot_id)
         no longer names a live process
release  DELETE WHERE holder_pid = me AND holder_boot_id = my_boot
```

Lease/heartbeat liveness is established primarily by a `flock` on a **dedicated
sidecar lock file**, `<workspace>/.ymh/sessions.lock` — not on the SQLite DB
file, whose own fcntl locks are unrelated and released per-connection. The
kernel releases the sidecar flock automatically on process death; the lease row
is tied to that lock and cross-checked against the holder's boot nonce.
`kill(pid, 0)` is only a **secondary hint, never the sole liveness test**: PID
reuse would otherwise block a session forever, and a SIGSTOP/suspended process
would look dead while it is merely frozen (split-brain).

Only the lease holder may append events. A holder that fails to renew stops
writing, degrades to read-only, and notifies its supervisor. `SessionHandle`
is the IPC-addressable teardown capability that owns the lease; the daemon's
`SessionPersistence` checks the lease before each `COMMIT`.

The environment-root parameter is baked into the §18 interface before any tool
is written. Retrofitting it later is a breaking change across every tool.

---

## 9.8 Session lifecycle

**Q4.** Close is four orthogonal operations. Do not overload one key.

| Op | Trigger | Semantics | Emits `SessionEnded`? |
|---|---|---|---|
| Detach | `Ctrl+W` | TUI detaches from the daemon. The daemon SURVIVES; the session keeps running headless. TUI frees `SessionUiState` and re-attaches later. | No |
| Cancel | `Ctrl+C` / `/cancel` | Stop the in-flight turn, keep session open and history intact. | No |
| Delete | `/session delete` | Remove from `SessionStore`; destructive, requires confirmation. | Yes |
| Archive | (deferred) | `archived` column in the `workspace_sessions` junction (§9.10); list/search concern, not a tab concern. | No |

Detach must not emit `SessionEnded`; Delete must. Overloading them corrupts
§9.3 resume: did the session end, or was it only closed here?

Detach means "the TUI disconnects", not "the session stops" (D23). The
`WorkspaceHost` daemon keeps running and keeps appending to the session log;
the supervisor simply stops observing that host. This supersedes the earlier
"detach pauses in v1" stance. `host.detach` is distinct from `host.shutdown`:
only shutdown reaps the daemon.

`AgentHandle` lifecycle (DIV-7): a handle is created for a session and disposed
when the session is closed/deleted. `AgentStatus` has exactly two observable
values — `Idle` (no driver active) and `Running` (cancellable pre-step work).
Disposal removes the agent from the registry; it is not a third status. Callers
use `dispose()` for teardown and `whenIdle()` to await quiescence.

Late-event-after-close (F3): on detach or close, keep a tombstone for the
session until its `AgentHandle` reports a terminal state AND its per-session
mailbox drains. Do not silently drop late events; do not leave a zombie
`SessionUiState` or a phantom SessionBar cell. On TUI-side detach no tombstone
is needed: the daemon survives and re-attach returns the current state.

---

## 9.9 Background execution, attention, and notification

Ship in two phases (Q7/Q8).

Phase A (ship first; single-process MVP): one active session per workspace.

```text
only one session per workspace is active; activation is serialized by the daemon
a workspace runs its active session whenever it has pending work, regardless of
  whether any Supervisor is attached or focused; attachment and focus never gate work
when the active session finishes or blocks awaiting input, the daemon activates
  the next session with pending work; switching gracefully cancels the current
  turn (§34) when the daemon itself moves the active slot
static glyphs plus an aggregate waiting count; no flash timer
reuses the existing per-session UiModel / SessionUiState shape unchanged
```

**A workspace runs its active session whenever it has pending work, regardless
of whether any Supervisor is attached or focused; attachment and focus never
gate work.** UI focus is display-only: it selects what a Supervisor shows, never
what the daemon runs. The daemon itself never suspends: it survives TUI detach
and keeps any in-flight session running (D23). "Suspended" is a per-session state
inside the daemon, not a process state, and has explicit causes only (awaiting
input, user action, resource caps). With multiple supervisors attached to one
daemon, the daemon serializes activation and only one session is active per
workspace at a time.

Phase B (later; daemon-model baseline): background execution, edge-triggered
attention, flash, and LLM/tool caps, layered on the Phase A base.

Resume-suspended (F10): sessions resume SUSPENDED (`Idle`) so a restart spawns no
auto-resume LLM burst. The daemon activates a session when it has pending work,
independent of UI focus; auto-resume is opt-in only.

Attention (F4) is UI-only and derived (D19), with exactly TWO mechanisms:

```text
(a) persistent signal   the WAITING COUNT — a LEVEL SNAPSHOT, recomputed as the
                        sum of per-session AgentState across ALL attached
                        workspaces whenever a state edge arrives; self-healing
                        (a missed edge is corrected by the next recompute)
(b) transient signal    the ~1s FLASH — an EDGE NOTIFICATION armed when a
                        session transitions Thinking|CallingTool →
                        Waiting*|Error (needs input), or Thinking|CallingTool →
                        Idle (done) (§20.15, §20.23)
```

Attention therefore signals **both** "needs input" and "done"; neither is
suppressed by UI focus.

`recompute()` (§20.23) is the single algorithm for the counts: sum per-session
`AgentState` on each edge arrival — never increment per edge, never per token,
never sampled at the coalescing frame rate. Focusing a session does NOT remove it
from the waiting count; it leaves the count when its `AgentState` actually leaves
the waiting set.

Background permission policy (F2): a background session that hits
`write_file ASK` must not deadlock invisibly. Decide before background
execution ships:

```text
auto-deny
auto-allow per tool
surface via attention (badge + waiting count)
```

Flash clock (F12): the flash phase lives in the model
(`SessionUiState.AttentionState` / `AggregateStatusModel`), driven by a
model-level tick. Never advance a timer inside a widget's `Render()` (D16).
Degrade the attention color against `TerminalCapabilities` (§20.19). See
§20.23 for the flash phase machine.

Decisions required before implementation (Q8):

```text
close semantics              (§9.8)       resolved: detach survives, delete ends
suspended vs background      (§9.9)
path isolation mechanism     (§9.7, §18)  resolved: process-per-workspace + realpath
background permission policy (§9.9)
workspace storage location   (§9.10)      resolved: shared registry.db
event ordering contract      (§8.3)       resolved: per-session ordered
subagent ID duality          (§20.25)
cancellation scoping         (§20.24, §20.26)
resource caps                (§9.11)
keyboard namespace           (§20.26)
```

---

## 9.10 Shared workspace registry

Workspace = the open set of sessions plus its host registration. It lives OUTSIDE
the event log (D21) and OUTSIDE any single daemon, in a **shared registry
database** that every process reads and a single writer mutates. The
focused/active session is NOT here: it is supervisor-local state (§20.22).

```text
${XDG_STATE_HOME:-~/.local/state}/ymh/registry.db
```

This is separate from the per-workspace session DBs
(`<workspace>/.ymh/sessions.db`, §9.2). It is small — workspace records,
the session junction, the `pending_mutation` crash-recovery marker, and a
one-row bookkeeping table — and is the one
database multiple processes touch.

```sql
CREATE TABLE workspaces (
    id              TEXT PRIMARY KEY,      -- WorkspaceId (UUIDv4, stable)
    canonical_path  TEXT NOT NULL UNIQUE,  -- realpath() of the workspace dir
    display_title   TEXT NOT NULL,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL,
    host_pid        INTEGER,               -- WorkspaceHost daemon; NULL if none, never 0
    host_boot_id    TEXT,                  -- daemon's per-boot UUID (PID-reuse guard)
    host_socket     TEXT,                  -- Unix socket path for IPC
    host_heartbeat  INTEGER,               -- last heartbeat (epoch ms)
    metadata        JSON,
    CHECK (host_pid IS NULL OR host_pid > 0),
    CHECK ((host_pid IS NULL) = (host_boot_id IS NULL)),
    CHECK ((host_pid IS NULL) = (host_socket IS NULL)),
    CHECK ((host_pid IS NULL) = (host_heartbeat IS NULL))
);

CREATE TABLE workspace_sessions (
    workspace_id    TEXT NOT NULL,
    session_id      TEXT NOT NULL,
    ordinal         INTEGER NOT NULL,      -- ordering key; strictly increasing, not dense
    archived        INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    PRIMARY KEY (workspace_id, session_id),
    FOREIGN KEY (workspace_id) REFERENCES workspaces(id),
    CHECK (archived IN (0, 1))
);

CREATE INDEX idx_ws_sessions ON workspace_sessions(workspace_id, ordinal);

CREATE TABLE pending_mutation (            -- dsh-style crash recovery marker (§8)
    workspace_id    TEXT PRIMARY KEY,      -- at most one marker per workspace
    mutation_type   TEXT NOT NULL,         -- 'create' | 'delete' | 'reorder'
    payload         JSON NOT NULL,
    timestamp       INTEGER NOT NULL,
    CHECK (mutation_type IN ('create','delete','reorder'))
);

CREATE TABLE registry_meta (               -- schema/bootstrap bookkeeping only
    key             TEXT PRIMARY KEY,      -- 'initialized' | 'legacy_migrated' | ...
    value           TEXT NOT NULL
);
```

**`registry_meta` is bookkeeping, not workspace state.** It holds
schema/bootstrap markers only (`initialized`, `legacy_migrated`; the schema
version itself is tracked by `PRAGMA user_version`, not a row) and carries no
open-set payload. The DDL above is the canonical form that component spec 03
reproduces.

**Single writer (D22).** Readers (supervisor TUI, CLI) open WAL read
transactions and never block. Exactly one process holds `flock(LOCK_EX)` on
`~/.local/state/ymh/registry.lock`; only the lock holder writes. The `flock` is
taken **for each mutation** (a per-write critical section), so exactly one
process holds it at any instant. The `WorkspaceHost` daemon is the canonical
writer for its own workspace's session list; the supervisor writes only host
claim/heartbeat rows.

**Pending mutations (DIV-3).** A mutation writes its `pending_mutation` marker
before the two writes (record + order) can diverge. On startup the registry
resolves exactly the marked mutation: a pending `create` rolls back, a pending
`delete` completes, an unmarked order/table mismatch fails loud as corruption.

**Heartbeat registration.** On startup a daemon canonicalizes its directory,
claims `host_pid`/`host_boot_id`/`host_socket`, and refreshes `host_heartbeat`
every 5s. Liveness is checked primarily by an `flock` the daemon holds on its
workspace lock file, `<workspace>/.ymh/sessions.lock` (kernel-released on
process death; §9.7), plus the `host_boot_id` match; `kill(pid, 0)` is only a
secondary hint (PID reuse, SIGSTOP). A claim is cleared lazily on next access
**only when the daemon's lock is absent AND its `host_boot_id` no longer
matches the holder's boot nonce**; heartbeat staleness is only a hint and is never
sufficient on its own to clear a claim. `claimHost`/`releaseHost` are the only
supervisor writes.

**Bootstrap (one-time).** On first run (no `initialized` marker) the registry
has no canonical-path index to walk, so the first-time discovery source is the
configured `workspace_roots` list (§37). Each configured root is walked to a
bounded depth (default 4), skipping a denylist (`.git`, `node_modules`,
`.cache`, `Library`, `target`, `build`, `.venv`); every directory containing
`<dir>/.ymh/sessions.db` is canonicalized and its `sessions` rows imported,
grouped by canonical `cwd` into per-directory workspaces (newest first). If
`workspace_roots` is unset it defaults to `["$HOME/prjs"]`; if that path does
not exist there is no auto-discovery and the user runs
`ymh workspace add <path>`. Optionally a legacy central store may be imported
once, after which it is marked migrated. The `initialized` marker is written
last so an interrupted bootstrap resumes safely. A process scan is **NOT** a
discovery source: it only sees currently-running daemons, never the durable
ordered open-set.

**Discovery vs. liveness (closes the §5 bootstrap item).** These are two
different questions and must never be conflated:

- *Open-set discovery* — which workspaces and sessions exist, and in what order
  — is **durable state, not process state**. The `workspaces` rows plus the
  `workspace_sessions.ordinal` junction are authoritative, seeded by the one-time
  bootstrap above. Process inspection cannot reconstruct the ordered junction, so
  it is never the source of truth.
- *Liveness* — is a host still alive? — is established by the `flock` the daemon
  holds on its workspace lock file (`<workspace>/.ymh/sessions.lock`, §9.7;
  kernel-released on process death), cross-checked against `host_boot_id`.
  `kill(pid, 0)` is only a hint (PID reuse, SIGSTOP). A `ps`/procfs scan is no
  stronger than `kill(pid, 0)` and is therefore **never the sole liveness
  test**.

**Process-scan fallback (best-effort only).** When the registry is missing,
corrupt, or predates `initialized`, the supervisor MAY enumerate candidate
daemons with an exact-match process scan: match the executable name on `argv[0]`
**exactly** (never a substring `grep ymh`). The scan uses `argv` only to seed
candidate PIDs; it never trusts the self-reported `--workspace`/`--socket`
values. The DB/socket path is derived **independently** — from the registry row
or from the workspace root — and never from the scanned argv. Every candidate
MUST be confirmed before it is trusted: by connecting to its independently
derived Unix socket and completing the authenticated JSON-RPC handshake, or by
probing the workspace `flock` (§9.7). A process scan **NEVER** kills a daemon on
`ps` evidence alone. Reaping an orphan requires lock-absence on the
independently derived DB path **and** a `host_boot_id` mismatch, and the
supervisor MUST take the D22 registry write lock before mutating anything. `ps`
output is self-reported, spoofable, and racy; it is used only to seed candidate
PIDs, never as authoritative membership or liveness.

```cpp
enum class SessionKind { Root, Fork, Subagent };

struct SessionHeader {                  // one row of the per-workspace `sessions` table
    SessionId                  id;              // UUIDv4; never the path
    std::filesystem::path      cwd;             // canonical workspace root; immutable
    int64_t                    createdAt;       // epoch ms
    int64_t                    updatedAt;       // epoch ms of last appended event
    std::string                title;           // display title ("" => derive)
    std::string                model;           // default model id
    std::string                serverProfile;   // Interactive | Automation
    SessionKind                kind;            // root | fork | subagent discriminator
    std::optional<SessionId>   parentSession;   // fork/subagent parent
    std::optional<size_t>      seedLength;      // # parent events copied; set iff kind==Fork
    std::optional<std::string> metadata;        // opaque JSON, no schema commitments
};
```

Invariants:

- `kind` is the root/fork/subagent discriminator. A `Root` has no
  `parentSession` and no `seedLength`; a `Fork` has both a `parentSession` and a
  `seedLength` (the parent events copied at fork time, ≤ the parent's event
  count then); a `Subagent` has a `parentSession` while `seedLength` may be
  NULL or 0 (no copied prefix).
- `cwd` is realpath-canonicalized at creation and immutable thereafter.
- `id` is a UUID, never derived from `cwd`.
- **No liveness token in the header.** The boot nonce lives in
  `session_leases.holder_boot_id` (§9.7) and `workspaces.host_boot_id` (§9.10) —
  mutable runtime state, not durable header state. Only `updatedAt`/`title`/
  `metadata` change after creation.
- `ordinal` and `archived` are **not** header fields: they are cross-workspace
  open-set state in the `workspace_sessions` junction (§9.10).

**Open-set and resume.** The junction's `ordinal` is the ordered open-set. The
supervisor's focused session per workspace is **supervisor-local** state
(`WorkspaceModel.activeSessionId`, §20.22), not registry state and not an event —
different TUI clients may focus different sessions. On attach/restart:

```text
load workspaces from the registry
    ↓
attach to each live host (or spawn one)
    ↓
reopen sessions suspended (Idle)
    ↓
focus this supervisor's last active session (local)
    ↓
rehydrate that session only
```

Non-active sessions stay suspended until the daemon activates them (activation
is daemon-driven, never focus-driven). This is what keeps restart from firing N
LLM calls (F10). v1 requires a live host for a workspace's sessions; there is no
offline session cache.

---

## 9.11 Resource limits and scheduling

Caps are **per host** (per workspace daemon), because each daemon owns its own
LLM pool, subprocesses, PTYs, and buffers. The supervisor adds a thin layer of
cross-host caps.

Per-host `ResourceCaps`:

```text
LLM pool                 bounded worker pool for LLM calls (§35)
global subprocess cap    total tool subprocesses in the daemon
per-session subprocess cap
global PTY cap           total PTYs in the daemon
per-session PTY cap
output ring buffers      per-session + larger active-session ring
subagent coalescer       batch subagent deltas (~30–60 Hz), §20.25
fd / memory soft limits
```

**F5.** Per-session output ring buffers with caps. Full tool output is
materialized only for the active session and for expanded subagents. N sessions
times M subagents times huge tool output is otherwise unbounded (`grep -R`,
`cmake`, `ninja`, `pytest`; §20.10).

**F8.** Bound tool subprocesses and PTYs, not just LLM calls:

```text
global semaphore on tool execution
per-session subprocess cap
per-session PTY cap
global subprocess cap
global PTY cap
```

Supervisor-level caps (across all attached hosts):

```text
maxAttachedHosts     max workspaces the supervisor tracks
maxTotalSessions     max sessions across all hosts
TUI memory cap       supervisor process only
```

The supervisor does not enforce LLM/subprocess/PTY caps — those stay per host.
A shared semaphore is unnecessary because each daemon's caps are independent;
to bound total system load, cap the number of live daemons in the registry.

Subagent fan-in: see 20.25 for the Q5 coalescing design.

---

# 10. Agent Model

## 10.1 Agent handle

External components should see:

```cpp
class Agent {
public:
    virtual ~Agent() = default;

    virtual AgentId id() const = 0;
    virtual SessionId session() const = 0;

    virtual void send(Message) = 0;
    virtual void cancel() = 0;
    virtual void steer(Message) = 0;
    virtual void inject(ContextMessage) = 0;

    virtual void dispose() = 0;                          // teardown; removes the handle (§9.8)
    virtual void whenIdle(std::function<void()>) = 0;    // await quiescence
};
```

`dispose()` and `whenIdle()` mirror the `AgentHandle` lifecycle in §9.8:
disposal removes the agent from the registry (it is not a status), and
`whenIdle()` lets callers await terminal quiescence before freeing a session.

The actual loop implementation remains private.

This is deliberately similar to DeepSeek Harness's separation between the public `Agent` surface and the concrete `agent-loop` implementation. citeturn0search4

---

# 11. Agent Loop

The fundamental unit is:

```text
Turn
 ├── Step
 │    ├── LLM request
 │    ├── streamed response
 │    ├── zero or more tool calls
 │    └── tool results
 │
 ├── Step
 │    └── ...
 │
 └── Turn end
```

A typical flow:

```text
turn/start

  ↓

claim next user input

  ↓

assemble context + tool schemas

  ↓

agent/pre-step

  ↓

step/start

  ↓

LLM request

  ↓

stream:
    text chunks
    tool calls

  ↓

assistant/message

  ↓

tool calls

  ↓

permission policy

  ↓

tool execution

  ↓

tool results

  ↓

step/end

  ↓

if more work required:
    next step
else:
    turn/end
```

This is intentionally close to DeepSeek Harness's documented turn/step lifecycle. citeturn0search0

---

# 12. LLM Interface

The LLM abstraction should be streaming-first.

```cpp
struct Message {
    Role role;
    std::vector<ContentBlock> content;
};

struct LLMRequest {
    std::string model;
    std::vector<Message> messages;
    std::vector<ToolSchema> tools;
    GenerationParameters parameters;
};

class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    virtual Task<LLMResponse> stream(
        const LLMRequest& request,
        StreamSink sink,
        CancellationToken cancel) = 0;
};
```

Stream events:

```text
TextDelta
ReasoningDelta        // if provider exposes it
ToolCallStarted
ToolCallDelta
ToolCallFinished
Usage
Finished
Error
```

The agent should never depend on provider-specific streaming formats.

---

# 13. Provider Adapters

Initial providers:

```text
AnthropicProvider
OpenAIProvider
OpenAICompatibleProvider
OllamaProvider
VLLMProvider
```

Because many local inference servers expose OpenAI-compatible APIs, the generic provider should be a priority.

Configuration:

```toml
[llm.default]
provider = "openai-compatible"
base_url = "http://localhost:8000/v1"
model = "..."
```

---

# 14. Tool System

## 14.1 Tool interface

```cpp
class Tool {
public:
    virtual ~Tool() = default;

    virtual ToolSchema schema() const = 0;

    virtual Task<ToolResult> execute(
        const ToolContext&,
        const ToolArguments&) = 0;
};
```

Tool schema is exposed to the model.

---

## 14.2 ToolContext

```cpp
class ToolContext {
public:
    ExecutionEnvironment& execution();
    Session& session();
    Logger& logger();
    CancellationToken cancellation();

    void emit(Event);
};
```

Tools should not directly access the TUI.

---

## 14.3 Initial tool set

### Filesystem

```text
read_file
write_file
edit_file
```

### Search

```text
glob
grep
```

### Process

```text
shell
```

### Git

```text
git_status
git_diff
git_log
git_show
git_branch
git_checkout
```

Avoid giving the model a separate tool for every trivial Git operation initially. A small Git abstraction plus shell can be enough.

---

# 15. Filesystem

The filesystem service should provide:

```cpp
class Filesystem {
public:
    Task<FileData> read(Path);
    Task<void> write(Path, Data);
    Task<void> remove(Path);
    Task<FileInfo> stat(Path);
    Task<std::vector<Path>> glob(...);
};
```

Policy can restrict:

```text
workspace root
allowed paths
denied paths
read-only paths
```

Path handling must prevent accidental traversal outside the configured workspace.

---

# 16. Process / Shell

Do not implement shell execution directly inside the shell tool.

Use:

```text
ShellTool
   ↓
ExecutionEnvironment
   ↓
ProcessService
```

Example:

```cpp
struct ProcessRequest {
    std::string executable;
    std::vector<std::string> argv;
    Path cwd;
    Environment environment;
    Timeout timeout;
};
```

Avoid constructing shell command strings whenever an argv representation is sufficient.

For commands that intentionally require a shell:

```text
/bin/bash -lc "..."
```

must still pass through the permission policy.

---

# 17. PTY

PTY should be a separate capability from one-shot process execution.

```cpp
class PtySession {
public:
    void write(std::string_view);
    Stream<std::string> output();
    void resize(int rows, int cols);
    void terminate();
};
```

This enables:

- interactive shells,
- curses applications,
- build systems with progress,
- long-running commands,
- future persistent terminal tool.

---

# 18. Execution Environment

Central interface:

```cpp
class ExecutionEnvironment {
public:
    virtual const std::filesystem::path& root() const = 0;
    virtual std::filesystem::path resolve(std::string_view) const = 0;
    virtual Filesystem& fs() = 0;
    virtual ProcessService& process() = 0;
    virtual PtyService& pty() = 0;
    virtual GitService& git() = 0;
    virtual LspService* lsp() = 0;
};
```

Implementations:

```text
LocalEnvironment
SSHEnvironment
ContainerEnvironment
SandboxEnvironment
```

Initial implementation:

```text
LocalEnvironment
```

Later:

```text
SSHEnvironment
```

can execute on another host while the TUI remains local.

Every environment is rooted (F1): `root()` and `resolve()` on the canonical
interface above are the rooting contract — root-relative resolution that never
uses `getcwd()`.

Path rules:

```text
a WorkspaceHost daemon chdir()s to its workspace root once, at startup
  (one workspace per process, so chdir() is safe)
resolve relative paths against root()
use openat / openat2 with a dirfd rooted at root() as optional
  defense-in-depth, or absolute-join
every tool / LSP / git / subprocess builder goes through resolve()
getcwd() is never a resolution base in tool code
```

Canonicalize workspace paths with `std::filesystem::canonical()` (realpath) at
registration and attach time; `WorkspaceId` is a stable UUID, never the path.
See 9.7.

The environment-root parameter is part of this interface before any tool is
written.

---

# 19. Permissions and Policy

Policy is a first-class subsystem.

```cpp
enum class PermissionDecision {
    Allow,
    Deny,
    Ask
};

class PermissionPolicy {
public:
    PermissionDecision evaluate(
        const ToolRequest&,
        const ExecutionContext&);
};
```

> **Errata (09).** The two-argument `evaluate(const ToolRequest&, const
> ExecutionContext&)` sketch above is superseded by spec 09's
> `evaluate(const PermissionRequest&) const`; `PermissionRequest` carries the
> execution context.

Example rules:

```text
read_file       ALLOW
grep            ALLOW
glob            ALLOW
git diff        ALLOW

write_file      ASK
edit_file       ASK

git commit      ASK
git push        ASK

shell           ASK
```

The policy must be independent of the TUI.

The TUI simply handles:

```text
PermissionRequested
```

and submits:

```text
PermissionDecision
```

This makes headless operation possible.

---

# 20. TUI Architecture

Recommended library:

**FTXUI**

The TUI should be a plugin/frontend.

Conceptual structure:

```text
TuiApp
 ├── Header
 ├── MainView
 │    ├── Conversation
 │    ├── ToolActivity
 │    └── Diff
 ├── InputEditor
 ├── StatusBar
 └── ModalLayer
```

The UI observes session/runtime events.

## 20.1 Presentation boundary and dependency rule

The UI is a pure presentation subsystem. The boundary is enforced by directory:

```text
core/       <-- MUST NOT depend on FTXUI
ui/         <-- depends on core interfaces + FTXUI
terminal/   <-- thin low-level terminal handling beneath FTXUI
```

Agent, Tool, and Session never reference FTXUI. The agent must be executable with no terminal at all.

Bad:

```text
Agent -> FTXUI
```

Good:

```text
Agent -> EventBus
ui    -> EventBus
ui    -> core interfaces
```

## 20.2 Layered architecture

```text
┌─────────────────────────────┐
│        Agent Runtime        │
│                             │
│ Agent / AgentLoop / Tools   │
│ LLM / Sessions / MCP / LSP  │
└──────────────┬──────────────┘
               │
          UI Events
               │
┌──────────────▼──────────────┐
│          UiModel            │
│                             │
│ Conversation                │
│ Tool executions             │
│ Streaming responses         │
│ Diffs / files               │
│ Status / permissions        │
│ Input / dialogs             │
└──────────────┬──────────────┘
               │
          UiController
               │
┌──────────────▼──────────────┐
│        FTXUI Widgets        │
│                             │
│ Header / Chat / ToolPanel   │
│ Input / Diff / Status       │
│ Dialogs / CommandPalette    │
└──────────────┬──────────────┘
               │
          FTXUI DOM
               │
┌──────────────▼──────────────┐
│       TerminalLayer         │
│                             │
│ raw mode / resize / input   │
│ capabilities / cursor       │
│ alternate screen            │
└──────────────┬──────────────┘
               │
           PTY / SSH
```

## 20.3 UiApplication

Top-level owner of the UI.

```cpp
class UiApplication {
public:
    UiApplication(
        std::shared_ptr<AgentRuntime> runtime,
        std::shared_ptr<EventBus> events);

    int run();
    void requestExit();

private:
    std::shared_ptr<AgentRuntime> runtime_;
    std::shared_ptr<EventBus> events_;

    UiModel model_;
    UiController controller_;
    Terminal terminal_;
    RootComponent root_;
};
```

Lifecycle:

```text
initialize
   ↓
initialize terminal
   ↓
subscribe to runtime events
   ↓
construct FTXUI component tree
   ↓
run event loop
   ↓
shutdown
```

It contains no agent logic.

## 20.4 UiModel

**Superseded (early sketch).** This is the original single-workspace `UiModel`.
§20.22 is authoritative for the multi-workspace shape and the `apply()`
signatures; this block is kept only as history.

```cpp
class UiModel {
public:
    ConversationModel conversation;
    ToolModel         tools;
    DiffModel         diffs;
    InputModel        input;
    StatusModel       status;
    DialogModel       dialogs;

    UiMode mode = UiMode::Conversation;

    bool shouldExit = false;

    void apply(const UiEvent& event);
};
```

Invariant: FTXUI components never own application state. They render `UiModel`.

```cpp
class ConversationView : public ftxui::ComponentBase {
public:
    explicit ConversationView(UiModel& model);
    ftxui::Element Render() override;

private:
    UiModel& model_;
};
```

The component does not decide what the conversation is. It only renders it.

## 20.5 UI events

UI events are strongly typed.

```cpp
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

        AgentStateChanged,   // single source of agent-state truth: {old, new}

        ErrorOccurred,

        TokenUsageUpdated,

        StatusChanged
    > value;
};
```

Agent-state changes are carried ONLY by `AgentStateChanged{old,new}` — the single
source of agent-state truth (§20.22). The former `AgentThinking` / `AgentWaiting`
/ `AgentFinished` variants are removed: they overlapped and could disagree with
`AgentStateChanged`, so keeping both would allow two encodings of the same state.

Examples:

```cpp
struct AssistantTextDelta {
    MessageId message;
    std::string text;
};

struct ToolStarted {
    ToolCallId id;
    std::string name;
    std::string description;
};
```

The agent publishes events instead of calling UI mutators. Do not do:

```cpp
ui->appendText(...);
ui->showTool(...);
ui->showSpinner(...);
```

Do:

```cpp
events.publish(
    AssistantTextDelta{
        .message = message_id,
        .text = delta
    });
```

The UI reacts.

## 20.6 UiEventAdapter

The core does not publish UI-shaped events. It publishes core events; the UI adapter maps them into UI events and applies them to the model.

```text
Agent Runtime
      │
      ▼
   EventBus
      │
      ▼
 UiEventAdapter
      │
      ▼
   UiModel
      │
      ▼
 FTXUI Views
```

The adapter is valuable because core events are not designed around the TUI. For example, the core emits:

```text
ToolExecutionStarted
```

and the adapter produces:

```cpp
ToolViewModel{
    .state = ToolState::Running,
    .summary = ...
};
```

The same core event feeds every frontend:

```text
                     EventBus
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
    FTXUI TUI      Headless CLI       RPC/GUI
        │
    UiEventAdapter
        │
    UiModel
```

## 20.7 Event flow

Streaming LLM response:

```text
LLM
 │
 ├── token
 ├── token
 ├── token
 ▼
AgentLoop
 │
 ▼
EventBus
 │
 ▼
UiEventAdapter
 │
 ▼
UiModel::apply()
 │
 ▼
FTXUI invalidation
 │
 ▼
Render()
```

Tool execution:

```text
AgentLoop
    │
    ├── ToolStarted
    ├── ToolOutput
    ├── ToolOutput
    └── ToolFinished
             │
             ▼
         UiModel
             │
             ▼
        ToolView
```

## 20.8 ConversationModel

Conversation messages are separate from durable session history.

```cpp
struct MessageViewModel {
    MessageId id;

    enum class Role {
        User,
        Assistant,
        System
    };

    Role role;

    std::string text;

    bool streaming = false;

    std::vector<ContentBlock> blocks;
};
```

```cpp
class ConversationModel {
public:
    const std::vector<MessageViewModel>& messages() const;

    void addMessage(MessageViewModel);
    void appendText(MessageId, std::string_view);
    void finishMessage(MessageId);

    void scrollUp();
    void scrollDown();
    void pageUp();
    void pageDown();

    bool atBottom() const;

private:
    std::vector<MessageViewModel> messages_;

    int scrollOffset_ = 0;
    bool followOutput_ = true;
};
```

Follow-output behavior matters for a coding agent. While the assistant streams, auto-scroll only when the viewport is already at the bottom. If the user scrolls up to read history, never force the viewport back down.

```cpp
if (model_.conversation.atBottom()) {
    auto_scroll();
}
```

## 20.9 Streaming markdown

Do not reparse the entire conversation on every token.

```text
LLM delta
   │
   ▼
MessageBuffer
   │
   ├── stable blocks
   │
   └── streaming block
```

```cpp
struct StreamingMessage {
    std::string rawText;

    std::vector<ContentBlock> committedBlocks;

    std::string pendingBlock;
};
```

Render the unfinished portion conservatively. When the pending block becomes syntactically complete, commit it to `committedBlocks`. This complements Section 22.

## 20.10 ToolModel and tool output streaming

```cpp
enum class ToolState {
    Pending,
    Running,
    Success,
    Failed,
    Cancelled
};

struct ToolViewModel {
    ToolCallId id;

    std::string name;
    std::string summary;

    ToolState state;

    std::chrono::steady_clock::time_point started;
    std::chrono::milliseconds duration{};

    std::vector<std::string> output;

    bool expanded = false;
};
```

```text
╭─ tools ───────────────────────────────╮
│ ✓ read_file src/main.cpp              │
│ ✓ grep "AgentLoop" src/               │
│ ● bash cmake --build build            │
│   └─ compiling...                     │
╰───────────────────────────────────────╯
```

Tool output is collapsed by default. The conversation shows the summary inline; the full output buffer is expandable.

```text
● bash: git diff

  43 lines changed
```

```text
● bash: git diff
──────────────────────────────
diff --git a/src/foo.cpp
...
```

This matters for `grep -R`, `find`, `cmake`, `ninja`, `git diff`, and `pytest`, which can produce enormous output.

## 20.11 UiController

The bridge between UI actions and the core.

```cpp
class UiController {
public:
    void sendMessage(std::string text);

    void cancelAgent();

    void executeCommand(std::string command);

    void selectTool(ToolCallId);
    void expandTool(ToolCallId);

    void showDiff();

    void resolvePermission(
        PermissionId,
        PermissionDecision);

    void requestExit();

private:
    AgentRuntime& runtime_;
    CommandRegistry& commands_;
};
```

FTXUI widgets call the controller. They never directly manipulate `AgentLoop`.

## 20.12 Rendering must be pure

```cpp
Element ConversationView::Render() {
    return renderConversation(model_);
}
```

Avoid:

```cpp
Element Render() {
    agent_->getCurrentResponse();
    database_->query(...);
    tools_->refresh();
    ...
}
```

`Render()` must have no side effects. This gives deterministic tests, easier debugging, no accidental blocking, no database calls during rendering, and an easier path to future GUI/RPC frontends.

## 20.13 Threading model

```text
UI thread
   │
   ├── input
   ├── model updates
   └── rendering

Worker threads:
LLM streaming
tool execution
filesystem
git
MCP
LSP
```

Workers communicate through the EventBus. Never call FTXUI from a worker. See Section 35.

## 20.14 UI invalidation

```cpp
enum class UiDirtyFlag {
    None          = 0,
    Conversation  = 1 << 0,
    Tools         = 1 << 1,
    Diff          = 1 << 2,
    Input         = 1 << 3,
    Status        = 1 << 4,
    Layout        = 1 << 5
};
```

The controller coalesces updates:

```text
50 LLM tokens
      ↓
coalesce
      ↓
one UI update
      ↓
render
```

A target of roughly 20 to 60 FPS is more than enough.

## 20.15 Agent state machine

```cpp
enum class AgentState {
    Idle,
    Thinking,
    CallingTool,
    WaitingForPermission,
    WaitingForInput,
    Cancelling,
    Error
};
```

```text
Idle
 > message

Thinking
 ● thinking...

CallingTool
 ● bash: ninja

WaitingForPermission
 [permission dialog]

WaitingForInput
 >

Error
 ! error
```

## 20.16 Modal layers

Dialogs are overlays, not whole-tree replacements.

```cpp
class OverlayManager {
public:
    void push(Component);
    void pop();

    Component top() const;
    bool active() const;
};
```

```text
Root
 ├── normal UI
 └── overlay
       ├── permission
       ├── command palette
       ├── model selector
       └── help
```

This keeps keyboard handling manageable.

## 20.17 Component hierarchy

```text
RootComponent
│
├── Header
│
├── MainArea
│   │
│   ├── ConversationView
│   │   └── MessageView[]
│   │       ├── Markdown
│   │       ├── CodeBlock
│   │       └── Diff
│   │
│   └── ToolPanel
│       └── ToolView[]
│
├── InputView
│
├── StatusBar
│
└── OverlayManager
    │
    ├── PermissionDialog
    ├── CommandPalette
    ├── ModelSelector
    └── HelpDialog
```

The root component stays small.

```cpp
class RootComponent : public ftxui::ComponentBase {
public:
    RootComponent(
        UiModel& model,
        UiController& controller);

    ftxui::Element Render() override;
    bool OnEvent(ftxui::Event event) override;

private:
    UiModel& model_;
    UiController& controller_;

    Component header_;
    Component conversation_;
    Component toolPanel_;
    Component input_;
    Component statusBar_;
    Component dialog_;
};
```

```cpp
Element RootComponent::Render() {
    return vbox({
        header_->Render(),
        separator(),
        conversation_->Render() | flex,
        toolPanel_->Render(),
        input_->Render(),
        statusBar_->Render(),
    });
}
```

## 20.18 Cancellation flow (UI side)

```text
Ctrl+C
  │
  ▼
UiController
  │
  ▼
AgentRuntime.cancel()
  │
  ▼
CancellationToken
  │
  ├── LLM request
  ├── tool
  ├── subprocess
  └── MCP request
```

Events flow back:

```text
ToolCancelled
AgentCancelled
```

so the UI updates naturally. See Section 34.

## 20.19 TerminalLayer

FTXUI owns the application UI. A small `Terminal` abstraction sits beneath it, inspired by Neovim.

```cpp
class Terminal {
public:
    void enterRawMode();
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
```

```cpp
struct TerminalCapabilities {
    bool trueColor;
    bool color256;

    bool mouse;
    bool bracketedPaste;

    bool modifyOtherKeys;
    bool kittyKeyboard;

    bool unicode;
};
```

This is not a terminal emulator. It abstracts only the capabilities the application cares about.

## 20.20 Input architecture

Terminal input is separated from FTXUI event translation.

```text
stdin
 │
 ▼
TerminalInput
 │
 ▼
KeyEvent
 │
 ▼
InputTranslator
 │
 ▼
FTXUI Event
```

```cpp
struct KeyEvent {
    Key key;
    Modifier modifiers;
};

enum class Key {
    Character,
    Enter,
    Escape,
    Backspace,
    Tab,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    Delete,
    F1,
    ...
};
```

This isolates odd SSH terminal differences from the UI.

## 20.21 UI implementation order

Phase 1:

```text
UiApplication
UiModel
UiController
RootComponent
ConversationView
InputView
StatusBar
Terminal
```

Support user messages, streamed assistant messages, scrolling, Ctrl+C, resize, and clean terminal restoration.

Phase 2:

```text
ToolModel
ToolView
MarkdownRenderer
CodeBlockRenderer
CommandRegistry
CommandPalette
```

Phase 3:

```text
DiffModel
DiffView
PermissionDialog
file references
syntax highlighting
```

Phase 4:

```text
LSP UI
MCP UI
subagent panels
session browser
model selector
context/token visualization
```

Phase A — multi-session, suspended sessions (see 9.9):

```text
SessionManager
WorkspaceModel
SessionBar
session switcher overlay (Ctrl+S)
Ctrl+N new session
per-session draft preservation
workspace persistence + resume
```

Only the active session's agent runs. Switching gracefully cancels the current
turn and resumes the target. Static glyphs, no flash, no attention engine.

Phase B — multi-session, background execution (later, see 9.9):

```text
background execution
edge-triggered attention + flash
background permission policy
LLM / subprocess / PTY caps
subagent fan-in coalescing
```

---

## 20.22 Multi-workspace presentation model

**Q3.** Keep ONE `UiModel` (preserves D13/D16). Add a workspace dimension.

```cpp
class WorkspaceModel {
public:
    WorkspaceId              id;
    std::string              cwd;          // daemon host working directory
    DaemonStatus             daemonStatus; // Attached/Connecting/Detached/Dead

    SessionId                activeSessionId;
    std::vector<SessionCell> sessions;     // ordered; one cell per open session
    OverlayStack             overlays;     // switcher, dialogs, palettes
};

class UiModel {
public:
    std::map<WorkspaceId, WorkspaceModel> workspaces;
    WorkspaceId                           activeWorkspaceId;

    // Per-session UI state, keyed by SessionId (cross-workspace)
    std::map<SessionId, SessionUiState>   sessions;

    AggregateStatusModel                  aggregate;   // §20.23

    DialogModel                           dialogs;     // global modal stack (§20.16)
    UiMode                                mode = UiMode::Conversation;
    bool                                  shouldExit = false;

    WorkspaceModel& activeWorkspace();
    SessionUiState& activeSession();

    void apply(const UiEvent& event);          // adapted, frontend-facing (§20.6)
    void apply(const WorkspaceEvent& event);   // workspace-level events
};
```

`apply()` takes the frontend-adapted `UiEvent` (reconciling §20.4 with this
authoritative shape): `UiEventAdapter` (§20.6) unwraps a wire `SessionEnvelope`
(core `Event`, below) into a `UiEvent` before calling `apply`. The model never
sees core or wire types.

`SessionUiState` holds only per-session projections plus derived attention:

```cpp
struct SessionUiState {
    SessionId id;                 // never an AgentHandle or ExecutionEnvironment

    ConversationModel conversation;
    ToolModel         tools;
    DiffModel         diffs;
    InputModel        input;      // per-session draft + history (§25, F6)
    StatusModel       status;

    AttentionState    attention;  // derived/cached: level snapshot + edge flash (D19, F4)
};
```

D1 constraint: `SessionUiState` holds a `SessionId`, never a core object
pointer. No `AgentHandle`, no `ExecutionEnvironment`. The TUI talks to a daemon
host only through session IDs and the event stream.

`WorkspaceModel.activeSessionId` is this supervisor's focused session —
**supervisor-local** state, never written to the shared registry (§9.10), because
multiple TUI clients may focus different sessions.

**Q6.** This shape does not violate D1–D16. Flash-in-`Render()` is fixed by F12
(§9.9, §20.23); complexity is contained by the Phase A / Phase B split (§9.9);
D15/D10 require headless and RPC frontends to get an equivalent
session-state-change signal (D19). D2/D9 stay clean because the open-set lives
outside the event log (D21) and subagents keep their own `SessionId` (§20.25).

Envelope: the wire carries the core, durable `Event` (§8.2), never a frontend
type. `UiEvent` is produced only by `UiEventAdapter` in the frontend (§20.6,
D15). Core `Event` already carries `session_id`, so `SessionEnvelope` does not
duplicate the id; `session` is retained for routing.

```cpp
struct SessionEnvelope {
    SessionId session;
    Event     event;   // core, durable; frontend adapts via UiEventAdapter
};

struct WorkspaceEvent {
    WorkspaceId        workspace;
    WorkspaceEventKind kind;
};

enum class WorkspaceEventKind {
    DaemonAttached, DaemonDetached, DaemonDied,
    SessionOpened,  SessionClosed,
};
```

Add one UI event variant for the edge trigger:

```cpp
struct AgentStateChanged {   // NEW: carries old + new for edge-triggered attention
    SessionId  session;
    AgentState oldState;
    AgentState newState;
};
```

`WorkspaceModel` is not per-session. Attention is derived and cached in
`SessionUiState`, computed by the adapter from `AgentState` transitions (D19,
F4). The `UiEvent` variant gains `AgentStateChanged` alongside the existing
cases (§20.5). The supervisor holds one `WorkspaceModel` per attached daemon,
so it can watch workspace P2 while P1 flashes (D20).

---

## 20.23 Aggregate status line and flash

The bottom region has an active-session status line and an **aggregate counts
line**. The aggregate line shows COUNTS ONLY — it never lists workspaces.

```text
──────────────────────────────────────────────
 main  ● thinking…               2 active · 3 waiting
 [main ●] [api ✓] [docs >] [tests !]      ← current workspace, per-session
──────────────────────────────────────────────
```

The aggregate line spans ALL attached workspaces:

```text
active   Thinking | CallingTool
waiting  WaitingForInput | WaitingForPermission | Error
```

```cpp
struct AggregateStatus {
    uint16_t activeCount  = 0;
    uint16_t waitingCount = 0;
    bool operator==(const AggregateStatus&) const = default;
};

class AggregateStatusModel {
public:
    AggregateStatus current;
    FlashState      flash;   // Idle → Flashing → Done

    void recompute(const std::map<WorkspaceId, WorkspaceModel>& workspaces);
};
```

`recompute()` iterates all workspaces and sums per-session `AgentState` into the
two counters. It runs in the adapter whenever a state edge arrives — never in
`Render()` (D16) and never per token. The counts are a LEVEL SNAPSHOT, not
per-edge increments and not frame-sampled: they are self-healing, so a missed
edge is corrected by the next recompute. The transient ~1s flash below is the
separate EDGE NOTIFICATION. Focusing a session does not remove it from
`waitingCount`; the count drops only when its `AgentState` leaves the waiting set.

**Flash (~1s).** When any session (focused or not; §9.9) transitions
`Thinking|CallingTool → Waiting*|Error` (needs input) or `Thinking|CallingTool →
Idle` (a turn completed), the aggregate line flashes for ~1s, then stops:

```cpp
enum class FlashPhase { Idle, Flashing, Done };

struct FlashState {
    FlashPhase phase = FlashPhase::Idle;
    std::chrono::milliseconds elapsed{};

    bool isFlashing() const { return phase == FlashPhase::Flashing; }
    void arm();                              // → Flashing, elapsed = 0
    void tick(std::chrono::milliseconds d);  // Flashing → Done at ~1s
    Decorator flashColor() const;            // pure read for Render()
};
```

The clock is advanced by `UiController::onTick(deltaMs)` (model-level, §20.13),
NOT inside any widget's `Render()`. `Render()` only reads `flash.phase` to pick
a color and is otherwise side-effect free (D16). Degrade the flash color against
`TerminalCapabilities` (§20.19): use bright/inverted where supported, bold/gray
otherwise. Phase A ships counts without the flash; Phase B arms it (§9.9).

F7: make `UiDirtyFlag` per-session plus an `aggregateDirty` bit, so a burst from
a background session does not invalidate the whole screen and jitter the active
view. Only the active session renders the conversation body; the current
workspace's SessionBar strip renders a bounded, collapsed summary.

---

## 20.24 Workspace/session switcher overlay

`Ctrl+S` (fallback `Ctrl+P`) opens ONE tree overlay (an `OverlayManager` layer,
§20.16) with two levels: workspaces at the top, sessions as leaves. It shows
ALL workspaces, including detached and background ones.

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

```cpp
enum class SwitcherLevel { Workspace, Session };

struct SwitcherCursor {
    WorkspaceId              workspace;
    std::optional<SessionId> session;   // nullopt = workspace node
};

class SwitcherOverlayModel {
public:
    std::vector<WorkspaceNode> workspaces;  // ordered, from WorkspaceRegistry
    SwitcherCursor             cursor;
    std::optional<std::string> filter;      // "/" prefix filters by name
    std::set<WorkspaceId>      collapsed;   // collapsed nodes hide sessions

    void moveDown();
    void moveUp();
    void toggleExpand();   // Tab — expand/collapse workspace node
};
```

Focus blocking (F6): while the overlay is open, the underlying `InputView` must
not receive keys. The input pipeline (§20.20) has no focus concept today; add
one. Exactly one component owns focus.

Per-session draft (F6): draft text and history are preserved per session across
a switch.

XOFF caveat: `Ctrl+S` is the terminal XOFF flow-control character. Disable
`IXON` in `TerminalLayer` (§20.19). Fallback: `Ctrl+P` opens the same switcher
in filter mode (and the `/switch` slash command exists for terminals that
ignore the IXON change).

F9: `Ctrl+C` cancels only the ACTIVE session. When the switcher overlay is open,
`Ctrl+C` cancels the switcher, not the session.

---

## 20.25 Subagent fan-in and coalescing

Q5. M concurrent subagents each streaming tokens and tool output into the
parent `SessionUiState` is O(N × M) churn.

```text
1. batch at the source: aggregated SubagentDelta(subagentId, chunk) at ~30 to 60 Hz,
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
lines.

**SubagentCoalescer.** The batching in step 1 is implemented by a coalescer
that buffers deltas and drains them on the render tick:

```cpp
class SubagentCoalescer {
public:
    explicit SubagentCoalescer(uint16_t coalesceHz = 30);

    void push(SubagentDelta delta);          // may buffer until next tick
    std::vector<SubagentDelta> drain();      // called by the render tick

private:
    std::chrono::microseconds coalesceInterval_;
    std::chrono::steady_clock::time_point lastFlush_;
    std::vector<SubagentDelta> buffer_;
};
```

One coalescer per parent session. It lives in the daemon host (producer side),
so raw per-token deltas never cross the IPC boundary; the supervisor receives
at most one aggregated delta batch per tick.

F11 subagent ID duality: an envelope for a subagent carries two IDs.

```cpp
struct SubagentEnvelope {
    SessionId parent;      // UI display routing
    SessionId subagent;    // durability / replay (§30, §9.5)
    Event     event;       // core, durable; frontend adapts via UiEventAdapter
};
```

Do not collapse them. The parent ID routes display; the subagent's own
`SessionId` keeps its durable log replayable and inspectable.

---

## 20.26 Input focus and keybinding namespace

F6 resolution:

```text
Ctrl+N          new conversation in the active workspace (global)
Ctrl+Shift+N    new workspace (new daemon host, pick directory)
                fallback: /workspace new
Ctrl+S          workspace/session switcher (global; XOFF caveat, §20.24)
Ctrl+P          command palette / switcher in filter mode
Ctrl+W          context-sensitive detach: detach session when the switcher is
                open or no word is edited; delete-word inside the input editor
Ctrl+C          cancel the active session only; when the switcher is open,
                cancels the switcher
```

Focus rule: exactly one component owns keyboard focus. The overlay, when
active, is the focus owner; the `InputView` receives nothing. This prevents
dropped keystrokes and accidental session closes.

---

# 21. Adaptive Layout

The terminal width should control layout.

```cpp
enum class LayoutMode {
    Narrow,
    Normal,
    Wide
};

LayoutMode calculateLayout(int width);
```

### < 90 columns

```text
┌─────────────────────────┐
│ conversation             │
│                          │
│ tools inline             │
│                          │
├─────────────────────────┤
│ input                    │
└─────────────────────────┘
```

### 90–130 columns

```text
┌──────────────────────────────────────┐
│ conversation                         │
│                                      │
│ tool activity                        │
├──────────────────────────────────────┤
│ input                                │
└──────────────────────────────────────┘
```

### > 130 columns

```text
┌──────────────────────────────┬──────────────┐
│ conversation                 │ activity     │
│                              │              │
│                              │ files/diff   │
├──────────────────────────────┴──────────────┤
│ input                                       │
└─────────────────────────────────────────────┘
```

Possible side panel:

```text
Files
Changes
Tools
Context
```

Wide terminal (conversation plus right side panel):

```text
┌──────────────────────────────────────────────────────────────┐
│ coding-agent                             main.cpp  72%       │
├──────────────────────────────────────────────┬───────────────┤
│                                              │ TOOLS         │
│ User                                         │               │
│ Fix the race in AgentLoop                    │ ✓ grep        │
│                                              │ ✓ read        │
│ Assistant                                    │ ● build       │
│ I'll inspect the execution path...           │               │
│                                              │               │
│ ```cpp                                       │               │
│ ...                                          │               │
│ ```                                          │               │
│                                              │               │
├──────────────────────────────────────────────┴───────────────┤
│ > Type a message...                                          │
├──────────────────────────────────────────────────────────────┤
│ main  • 3 tools • 12.4k tokens • 128k context                │
└──────────────────────────────────────────────────────────────┘
```

Medium terminal (inline tool activity):

```text
┌──────────────────────────────────────────┐
│ coding-agent              main.cpp       │
├──────────────────────────────────────────┤
│ User                                     │
│ Fix the race...                          │
│                                          │
│ Assistant                                │
│ I'll inspect...                          │
│                                          │
│ ● bash: ninja                            │
│ ✓ read_file                              │
│                                          │
├──────────────────────────────────────────┤
│ > Type a message...                      │
├──────────────────────────────────────────┤
│ 12.4k tokens • build running             │
└──────────────────────────────────────────┘
```

Narrow terminal (minimal):

```text
┌──────────────────────────────┐
│ agent • main.cpp             │
├──────────────────────────────┤
│ Assistant                    │
│ I'll inspect...              │
│                              │
│ ● ninja                      │
│                              │
├──────────────────────────────┤
│ > message                    │
├──────────────────────────────┤
│ 12k tok • running            │
└──────────────────────────────┘
```

---

# 22. Markdown Rendering

Use a Markdown parser rather than writing a full parser.

Recommended:

```text
cmark-gfm
```

Rendering pipeline:

```text
LLM markdown
   ↓
Markdown AST
   ↓
FTXUI elements
   ↓
terminal
```

Markdown rendering is explicitly isolated so it can be optimized independently of the rest of the UI.

```cpp
class MarkdownRenderer {
public:
    ftxui::Element render(
        const MarkdownBlock& block,
        const RenderContext&);
};

struct RenderContext {
    int width;
    Theme theme;
    bool compact;
};
```

Streaming text should not force complete reparsing of the entire conversation on every token.

Use incremental buffering (Section 20.9).

---

# 23. Syntax Highlighting

Use:

```text
tree-sitter
```

Code blocks go through a dedicated renderer:

```cpp
class CodeBlockRenderer {
public:
    ftxui::Element render(
        std::string_view code,
        std::string_view language,
        const RenderContext&);
};
```

Pipeline:

```text
code block
   ↓
language detection
   ↓
tree-sitter parser
   ↓
highlight spans
   ↓
FTXUI rendering
```

Initially support:

```text
C++
C
Python
Rust
Go
JavaScript/TypeScript
JSON
YAML
Shell
```

Add more later.

The full rendering path is:

```text
Markdown
   │
   ▼
code block
   │
   ▼
tree-sitter
   │
   ▼
syntax tokens
   │
   ▼
FTXUI Elements
```

Tree-sitter therefore does not need to know anything about FTXUI.

---

# 24. Diff Rendering

Diffs should be a first-class UI component.

Sources:

```text
git diff
tool edit preview
generated patch
```

Display:

```text
src/foo.cpp

@@ -120,7 +120,11 @@

- old code
+ new code
+ another line
```

```cpp
class DiffView : public ftxui::ComponentBase {
public:
    ftxui::Element Render() override;
    bool OnEvent(ftxui::Event) override;

private:
    DiffModel& model_;

    int scroll_ = 0;
    int selectedFile_ = 0;
};
```

Model:

```cpp
struct DiffHunk {
    std::string header;

    std::vector<DiffLine> lines;
};

struct DiffFile {
    std::filesystem::path path;

    std::vector<DiffHunk> hunks;

    int additions;
    int deletions;
};

class DiffModel {
    std::vector<DiffFile> files_;
};
```

Layout:

```text
┌─ Changes ───────────────────────────────┐
│ M src/agent/AgentLoop.cpp       +12 -4  │
│ M src/ui/ConversationView.cpp   +31 -8  │
│ A src/ui/DiffView.cpp            +84    │
├─────────────────────────────────────────┤
│ @@ -42,7 +42,15 @@                     │
│                                         │
│   auto result = execute(...);           │
│                                         │
│ - return result;                        │
│ + if (!result) {                        │
│ +     ...                               │
│ + }                                     │
└─────────────────────────────────────────┘
```

Eventually support:

```text
accept
reject
edit
```

for proposed changes.

---

# 25. Input Editor

The input area should support:

- multiline input,
- history,
- paste,
- cursor navigation,
- Ctrl-C cancellation,
- command completion,
- slash commands.

```cpp
class InputModel {
public:
    std::string text;

    size_t cursor = 0;

    std::vector<std::string> history;
    int historyIndex = -1;

    bool multiline = false;

    void insert(std::string_view);
    void backspace();
    void deleteForward();

    void moveLeft();
    void moveRight();

    void historyUp();
    void historyDown();
};
```

Key bindings:

```text
Enter           send
Shift+Enter     newline
Ctrl+C          cancel generation
Ctrl+D          exit / EOF
Ctrl+L          redraw
Up/Down         history
PageUp/PageDown scroll
Ctrl+R          search history
Tab             completion
Esc             cancel popup
```

Later:

```text
Ctrl+P          command palette
Ctrl+O          tool/output view
Ctrl+G          diff view
```

Global session bindings (see 20.26):

```text
Ctrl+N          new conversation in the active workspace
Ctrl+Shift+N    new workspace (fallback: /workspace new)
Ctrl+S          workspace/session switcher overlay
Ctrl+P          command palette / switcher in filter mode
Ctrl+W          context-sensitive detach (switcher) / delete word (editor)
Ctrl+C          cancel the active session only
```

`Ctrl+S` is the terminal XOFF character. Disable `IXON` in `TerminalLayer` and
provide a fallback binding (`Ctrl+P`).

Draft text and history are per-session and preserved across a session switch.
The `InputModel` above becomes one instance per `SessionUiState` (§20.22). The
per-session draft survives workspace switches too: the supervisor keeps one
`InputModel` per `SessionId`, not one per active view.

Potential commands:

```text
/help
/model
/session
/workspace new [path]   create a workspace daemon (fallback for Ctrl+Shift+N)
/workspace list         list workspaces
/workspace switch <id>  switch to workspace
/workspace close <id>   detach all sessions and close the workspace
/fork
/replay
/diff
/status
/clear
/compact
/quit
```

Human commands should bypass the model when appropriate.

---

# 26. Slash Command Architecture

Commands are another registry.

```cpp
class Command {
public:
    virtual CommandSchema schema() const = 0;
    virtual Task<void> execute(CommandContext&, Args) = 0;
};
```

Examples:

```text
/help
/model
/tools
/permissions
/session
/fork
```

The command system should not be hard-coded into the TUI. Slash commands must not be hardcoded in `InputView`.

```cpp
class CommandRegistry {
public:
    void registerCommand(Command);

    std::optional<Command>
    find(std::string_view name) const;

    std::vector<Command>
    complete(std::string_view prefix) const;
};
```

`InputView` detects a leading `/` and delegates to `CommandRegistry`.

CommandPalette:

```text
┌──────────────────────────────────────┐
│ Command                              │
│ > /mo                                │
├──────────────────────────────────────┤
│   /model       Select model          │
│   /models      List models           │
│   /mode        Change agent mode     │
└──────────────────────────────────────┘
```

The same mechanism later provides `@file`, `@symbol`, `/tool`, and `/model` completion.

---

# 27. MCP

MCP should be implemented as an adapter into the same tool registry.

```text
MCP Server
    ↓
MCP Client
    ↓
ToolSchema
    ↓
ToolRegistry
    ↓
Agent
```

The agent should not know that a tool came from MCP.

Example:

```text
local.read_file
mcp.github.search
mcp.database.query
```

all appear as normal tools.

---

# 28. LSP

LSP should live behind the execution environment.

```text
LspService
 ├── definition
 ├── references
 ├── hover
 ├── diagnostics
 ├── symbols
 └── completion
```

The model-facing tools can then be:

```text
lsp_definition
lsp_references
lsp_diagnostics
lsp_symbols
```

The LSP server itself runs inside the execution environment.

---

# 29. Git

Use a native Git library where it provides meaningful advantages, but do not hesitate to invoke the `git` CLI for operations where it is simpler or more compatible.

Recommended split:

```text
libgit2
    status
    diff
    history
    object inspection

git CLI
    unusual commands
    advanced operations
    user-configured workflows
```

Git worktrees should eventually be supported.

---

# 30. Subagents

A subagent is another `Agent` instance.

```text
Parent Agent
     │
     ├── Child Agent A
     │
     ├── Child Agent B
     │
     └── Child Agent C
```

Each child gets:

```text
session
context
tool set
execution environment
policy
```

A subagent can therefore be implemented without a special agent-loop architecture.

---

# 31. Context Management

Context assembly should be explicit.

```text
System Prompt
    +
Workspace Context
    +
Tool Schemas
    +
Session History
    +
Injected Context
    +
Current User Message
```

Use a dedicated:

```cpp
class ContextAssembler
```

Responsibilities:

- select history,
- summarize old content,
- add system sections,
- add tool schemas,
- add workspace information,
- add injected messages.

---

# 32. Compaction

When context approaches a configured threshold:

```text
current history
       ↓
summarization
       ↓
compact context
       ↓
continue session
```

Important:

**Do not destroy the original session events.**

Compaction is a projection/context-generation operation, not deletion.

Store:

```text
ContextCompaction event
```

with:

```text
boundary
summary
token estimate
model
timestamp
```

This preserves replayability.

---

# 33. Token Accounting

LLM providers should emit usage events:

```text
input_tokens
output_tokens
cached_tokens
reasoning_tokens
```

Store usage in session metadata/events.

This enables:

```text
session cost
turn cost
model comparison
context growth
```

---

# 34. Cancellation

Every asynchronous operation must accept a cancellation token.

```cpp
CancellationToken
```

Cancellation must propagate:

```text
Ctrl-C
 ↓
TUI
 ↓
Agent.cancel()
 ↓
LLM request
 ↓
tool
 ↓
process
```

The second Ctrl-C may eventually be defined as force termination.

---

# 35. Concurrency Model

Prefer one main event loop plus asynchronous tasks.

Possible implementation:

```text
Main thread
 └── TUI/event loop

Worker execution
 ├── HTTP/LLM
 ├── filesystem
 ├── subprocess
 ├── PTY
 └── background jobs
```

Use:

```text
asio
```

as the asynchronous foundation.

Avoid creating a thread for every tool call.

CPU-heavy tasks can use a bounded worker pool.

Process topology: the TUI is the **supervisor process**; agent runtimes live in
**per-workspace daemon processes** (`WorkspaceHost`), which communicate with the
supervisor over the Unix-socket JSON-RPC 2.0 transport (§9.6). The thread model
above applies inside each daemon; the supervisor's main thread stays responsive
to UI and never blocks on a non-active session or daemon (D20).

Multi-session additions (see 9.11):

```text
per-session ordered mailboxes   (§8.3): events for a session deliver in order
LLM pool cap                    bounded worker pool for LLM calls
global subprocess semaphore     bounds tool execution across sessions
per-session subprocess cap
per-session PTY cap
global PTY cap
```

A bounded LLM cap does not bound tool subprocesses. N sessions running `ninja`,
`pytest`, or PTYs simultaneously will exhaust fds, PTYs, and CPU without the
caps above (F8).

---

# 36. Networking

Recommended initial stack:

```text
asio
OpenSSL
HTTP library
nlohmann/json
```

Possible choices:

- Boost.Asio
- standalone Asio
- libcurl for straightforward HTTP
- Boost.Beast for native async HTTP/WebSocket

For the first version, **do not over-engineer HTTP**.

A provider adapter should expose a clean interface regardless of transport.

---

# 37. Configuration

Use TOML.

Example:

```toml
[ui]
theme = "default"
show_activity = true
side_panel = "auto"

[agent]
model = "..."
max_steps = 100

[workspace]
root = "."
workspace_roots = ["~/prjs"]   # first-run bootstrap discovery roots (§9.10)

[permissions]
shell = "ask"
write = "ask"
read = "allow"

[llm.default]
provider = "openai-compatible"
base_url = "http://localhost:8000/v1"
model = "..."
```

Configuration should be layered:

```text
built-in defaults
      ↓
global config
      ↓
project config
      ↓
profile config
      ↓
command-line overrides
```

---

# 38. Profiles

**Disambiguation.** A composition profile/bundle here is distinct from a
session's `server_profile` (Interactive/Automation, §9.6).

Eventually support:

```text
profiles/
    default
    local
    remote
    minimal
    coding
    research
```

A profile selects:

```text
LLM
tools
policy
execution environment
UI
storage
```

Example:

```text
minimal:
    shell
    edit_file
    local LLM

full:
    filesystem
    shell
    git
    LSP
    MCP
    subagents
```

This follows the useful composition idea behind DeepSeek Harness profiles and bundles, while keeping the C++ implementation much simpler. citeturn0search0

---

# 39. Build System

Use:

```text
CMake
Ninja
```

Suggested structure:

```text
CMakeLists.txt

src/
include/

tests/

third_party/
```

Dependencies should be discovered through CMake.

Potential libraries:

```text
FTXUI
Asio / Boost.Asio
nlohmann/json
toml++
SQLite3
spdlog
CLI11
cmark-gfm
tree-sitter
libgit2
```

Do not create a giant umbrella library dependency.

---

# 40. Logging

Use:

```text
spdlog
```

Log categories:

```text
agent
llm
tool
process
filesystem
session
tui
mcp
lsp
plugin
network
```

Important:

**Never dump full prompts or sensitive tool output into normal logs by default.**

Session persistence is the authoritative trace.

---

# 41. CLI

Initial executable:

```text
ymh
```

Commands:

```text
ymh
ymh --resume SESSION
ymh --new
ymh list
ymh show SESSION
ymh replay SESSION
ymh fork SESSION
ymh run "task"
ymh workspace add PATH
ymh workspace list
```

Eventually:

```text
ymh serve
ymh rpc
```

---

# 42. Headless Mode

The agent runtime must run without FTXUI.

Example:

```bash
ymh run "Find the bug in src/foo.cpp"
```

Output:

```text
assistant text
tool activity
final result
```

This is essential for testing.

The headless state-change signal required by D19 (an equivalent of the UI's
edge-triggered attention signal) is explicitly **deferred to the headless/CLI
component spec**; it is not specified here.

---

# 43. RPC Mode

The TUI-to-runtime transport is **JSON-RPC 2.0 over a length-prefixed Unix domain
socket** — the same wire specified in §9.6 for supervisor↔host. This is the
primary transport, not a future option:

```text
TUI (supervisor)
  │
  │ JSON-RPC 2.0 over length-prefixed Unix socket
  ▼
WorkspaceHost daemon (agent runtime)
```

This allows the runtime to run independently of the SSH terminal and lets one
supervisor attach to many workspace daemons at once.

Other transports:

```text
stdio    testing/debug convenience only (not the deployment transport)
TCP      remote runtimes, deferred (§9.6)
```

stdio JSON-RPC remains useful for tests and local debugging, but the Unix-socket
transport is the primary path (cross-ref §9.6).

---

# 44. Testing Strategy

## Unit tests

Test:

```text
EventBus
SessionStore
ContextAssembler
PermissionPolicy
ToolRegistry
PathPolicy
LLM message conversion
configuration
```

## Integration tests

Test:

```text
agent + fake LLM
agent + fake filesystem
agent + fake shell
session persistence
resume
fork
tool permissions
cancellation
```

## Golden tests

Very useful for the TUI:

```text
given event stream
        ↓
render
        ↓
expected terminal representation
```

## Replay tests

Given a recorded event stream:

```text
same input
   ↓
same projected state
```

This is one of the biggest benefits of event sourcing.

## Live end-to-end tests (real LLM, PTY-driven)

The deterministic layers above run with the Fake LLM (§45) and need no network.
A separate **live** layer exercises the real product end to end, exactly as a
human would:

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

- Uses a **real LLM API** through the same `LLMProvider` seam as production.
- Gated by an API key plus an explicit opt-in flag; skipped (not failed) when
  absent, so the default suite stays hermetic and offline.
- Tolerant of model nondeterminism: assert on structure and invariants (events
  emitted, tools invoked, final state), not exact prose. Any retry/quorum is
  explicit, never silent.
- The top-level **supervisor/manager** flow must be **100% automatically
  testable**: a scripted PTY driver (or `tmux send-keys` + `capture-pane`) stands
  in for the human, so no manual keystrokes are required to cover multi-workspace
  attach, session switching, and permission handling.
- Runs as a separate CI stage from the hermetic unit/integration/golden/replay
  suite; it is never part of the fast default test command.

**Milestone gating.** The live multi-workspace PTY tests above target Milestone
2 (daemon + supervisor, §57 Step 13 / §58); MVP live tests cover only the
single-process flow.

**Fake vs. live.** §45's Fake LLM is the substrate for unit, integration, golden,
and replay tests (deterministic, hermetic, fully automatable). The live layer is
the only place a real provider is used, and it is the acceptance test for the
end-user experience.

---

# 45. Fake LLM

Create a deterministic fake model:

```cpp
class FakeLLM : public LLMProvider {
public:
    ...
};
```

Script:

```text
user:
  "fix foo"

model:
  tool_call(read_file, foo.cpp)

tool:
  file contents

model:
  tool_call(edit_file, ...)

tool:
  success

model:
  "Fixed the problem."
```

This lets the complete agent loop be tested without network access.

---

# 46. Security Model

The agent can execute arbitrary commands, so the architecture must assume that model-generated actions are untrusted.

Important boundaries:

```text
LLM output
    ↓
Tool parser
    ↓
Permission policy
    ↓
Execution environment
    ↓
OS
```

Never:

```text
LLM → shell → OS
```

without the policy layer.

Workspace restrictions should be explicit.

---

# 47. SSH Strategy

There are two useful deployment modes.

## Mode A — Everything on remote machine

```text
Laptop
   │
   │ SSH
   ▼
ymh
   │
   ├── TUI
   ├── Agent
   ├── Tools
   └── Code
```

This should be the initial target.

## Mode B — Local TUI + remote runtime

Later:

```text
Laptop
 └── TUI
       │
       │ SSH / RPC
       ▼
Server
 └── Agent Runtime
       ├── model
       ├── tools
       └── code
```

The architecture should make this possible without redesign.

Mode B maps onto the D17 topology directly: the local TUI is the supervisor and
the remote runtime is one or more workspace daemons reached over the §9.6
Unix-socket JSON-RPC transport (TCP for remote, deferred).

---

# 48. Recommended Initial Dependency Set

## Core

```text
C++23
CMake
Ninja
```

## TUI

```text
FTXUI
```

## Async

```text
Asio
```

## JSON

```text
nlohmann/json
```

## Configuration

```text
toml++
```

## Persistence

```text
SQLite3
```

## Logging

```text
spdlog
```

## CLI

```text
CLI11
```

## Markdown

```text
cmark-gfm
```

## Syntax

```text
tree-sitter
```

## Git

```text
libgit2
```

Do not add all of these before they are needed. The first executable can start with:

```text
FTXUI
Asio
nlohmann/json
SQLite3
spdlog
CLI11
toml++
```

and add tree-sitter/cmark/libgit2 as features arrive.

---

# 49. Proposed Source Tree

```text
ymh/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── cmake/
│
├── include/ymh/
│   ├── core/
│   │   ├── context.hpp
│   │   ├── event.hpp
│   │   ├── event_bus.hpp
│   │   ├── plugin.hpp
│   │   ├── service_registry.hpp
│   │   └── cancellation.hpp
│   │
│   ├── agent/
│   │   ├── agent.hpp
│   │   ├── agent_loop.hpp
│   │   ├── context_assembler.hpp
│   │   └── message.hpp
│   │
│   ├── session/
│   │   ├── session.hpp
│   │   ├── session_manager.hpp
│   │   ├── session_store.hpp
│   │   └── event_store.hpp
│   │
│   ├── llm/
│   │   ├── llm_provider.hpp
│   │   ├── llm_request.hpp
│   │   └── stream.hpp
│   │
│   ├── tools/
│   │   ├── tool.hpp
│   │   ├── tool_registry.hpp
│   │   └── tool_context.hpp
│   │
│   ├── execution/
│   │   ├── environment.hpp
│   │   ├── filesystem.hpp
│   │   ├── process.hpp
│   │   └── pty.hpp
│   │
│   ├── policy/
│   │   └── permission_policy.hpp
│   │
│   ├── registry/
│   │   └── workspace_registry.hpp
│   │
│   ├── daemon/
│   │   └── workspace_host.hpp
│   │
│   ├── supervisor/
│   │   └── supervisor.hpp
│   │
│   └── ui/
│       ├── ui_application.hpp
│       ├── ui_controller.hpp
│       ├── ui_model.hpp
│       ├── ui_event.hpp
│       │
│       ├── model/
│       │   ├── conversation_model.hpp
│       │   ├── tool_model.hpp
│       │   ├── diff_model.hpp
│       │   ├── input_model.hpp
│       │   ├── status_model.hpp
│       │   └── dialog_model.hpp
│       │
│       ├── components/
│       │   ├── root_component.hpp
│       │   ├── header.hpp
│       │   ├── conversation_view.hpp
│       │   ├── message_view.hpp
│       │   ├── tool_view.hpp
│       │   ├── input_view.hpp
│       │   ├── status_bar.hpp
│       │   ├── diff_view.hpp
│       │   ├── file_view.hpp
│       │   ├── dialog.hpp
│       │   └── command_palette.hpp
│       │
│       ├── render/
│       │   ├── markdown_renderer.hpp
│       │   ├── code_block_renderer.hpp
│       │   ├── diff_renderer.hpp
│       │   └── syntax_renderer.hpp
│       │
│       ├── terminal/
│       │   ├── terminal.hpp
│       │   ├── terminal_capabilities.hpp
│       │   ├── terminal_input.hpp
│       │   └── terminal_size.hpp
│       │
│       └── theme/
│           ├── theme.hpp
│           └── themes.hpp
│
├── src/
│   ├── core/
│   ├── agent/
│   ├── session/
│   ├── llm/
│   ├── tools/
│   ├── execution/
│   ├── policy/
│   ├── registry/
│   ├── daemon/
│   ├── supervisor/
│   ├── ui/
│   └── main.cpp
│
├── plugins/
│   ├── core/
│   ├── llm-openai/
│   ├── llm-anthropic/
│   ├── filesystem/
│   ├── shell/
│   ├── git/
│   ├── mcp/
│   ├── lsp/
│   └── ui/
│
└── tests/
    ├── unit/
    ├── integration/
    ├── replay/
    └── golden/
```

---

# 50. MVP Definition

The first useful release should NOT attempt to implement everything.

MVP:

```text
✓ C++23
✓ FTXUI
✓ SQLite
✓ EventBus
✓ Session event log
✓ Agent loop
✓ OpenAI-compatible LLM
✓ Streaming
✓ read_file
✓ edit_file
✓ grep
✓ glob
✓ shell
✓ permission policy
✓ Git status/diff
✓ resume
✓ cancellation
✓ Markdown rendering
✓ basic syntax highlighting
```

User experience:

```text
$ ymh

> inspect this C++ project and fix the failing test

Assistant:
  I'll inspect the project.

  ● glob("**/*.cpp")
  ● grep("TODO", ...)
  ● shell("cmake --build build")
  ...

  I found the failure...
```

This is already a genuinely useful coding agent.

**Scope note.** This MVP is the **single-process** milestone (§58, Milestone 1).
The supervisor + per-workspace-daemon split (§9.6, D17/D18) is the post-MVP
milestone; the MVP is written so the runtime core can be forked into a daemon
without redesign (§57 Step 13).

---

# 51. Phase 2

Add:

```text
MCP
LSP
PTY
background jobs
session fork
session replay
context compaction
worktrees
subagents
```

---

# 52. Phase 3

Add:

```text
remote ExecutionEnvironment
RPC
profiles
plugin loading
sandbox providers
advanced permission policies
agent orchestration
```

---

# 53. Phase 4 — Experimental

Potentially:

```text
dynamic plugins
hot reload
agent teams
workflow engine
web UI
distributed execution
```

Only pursue these if actual use demonstrates the need.

---

# 54. Key Design Decisions

These decisions should be treated as architectural invariants.

### D1

**TUI does not own the agent.**

### D2

**Session event log is the durable source of truth.**

### D3

**Agent loop consumes and produces events.**

### D4

**Tools are registered capabilities.**

### D5

**LLM is a replaceable provider.**

### D6

**Execution environment is a replaceable provider.**

### D7

**Permission policy sits between agent and execution.**

### D8

**MCP tools enter through the same ToolRegistry.**

### D9

**Subagents are ordinary Agent instances.**

### D10

**Frontends consume the same runtime.**

### D11

**Do not make dynamic plugins a prerequisite for modularity.**

### D12

**Prefer simple C++ interfaces over reproducing Cordis wholesale.**

### D13

**The UI renders a pure presentation model (UiModel); FTXUI widgets own no application state.**

### D14

**The core never depends on FTXUI; UI concerns stay behind a presentation boundary.**

### D15

**Each frontend adapts core events through its own adapter (UiEventAdapter); core events are frontend-agnostic.**

### D16

**Rendering is side-effect free.**

### D17

**A supervisor hosts many workspace daemons; each daemon hosts many independent sessions.**

### D18

**Isolation is per-workspace-process (one daemon, one cwd). Only the shared workspace registry and the transport are cross-process; `chdir()` is safe at daemon startup.**

### D19

**Attention is a UI-only, edge-triggered concern driving the aggregate counters and a transient ~1s flash. The core stays headless; headless/RPC frontends receive an equivalent session-state-change signal.**

### D20

**The UI thread never blocks on a non-active session or daemon.**

### D21

**Workspace/open-set state lives in the shared registry, outside the event log.**

### D22

**The shared registry has a single writer (`flock(LOCK_EX)`); all other access is read-only (WAL readers).**

### D23

**A workspace daemon survives TUI detach.**

### Findings reference (F1–F12)

F1–F12 are review findings cited throughout this document.

```text
F1   path/process isolation        F7   per-session dirty flags
F2   background permission         F8   resource caps
F3   late event after close        F9   cancellation scoping
F4   edge-triggered attention      F10  resume-suspended
F5   output ring buffers           F11  subagent ID duality
F6   input/keybinding focus        F12  flash clock in model
```

- **F1** — path/process isolation: one workspace per daemon; `chdir()` safe at startup (§9.7).
- **F2** — background permission: a background session hitting `ASK` must not deadlock invisibly (§9.9).
- **F3** — late event after close: tombstone until the mailbox drains and the handle is terminal (§8.3, §9.8).
- **F4** — edge-triggered attention: `AgentState` transitions drive the waiting count and the flash (§9.9, §20.23).
- **F5** — output ring buffers: per-session caps; full output only for active/expanded sessions (§9.11).
- **F6** — input/keybinding focus: exactly one focus owner; per-session draft/history (§20.24, §20.26).
- **F7** — per-session dirty flags: plus `aggregateDirty`, so background bursts do not jitter the active view (§20.23).
- **F8** — resource caps: bound tool subprocesses and PTYs, not just LLM calls (§9.11).
- **F9** — cancellation scoping: `Ctrl+C` cancels only the active session (§20.24).
- **F10** — resume-suspended: sessions reopen `Idle` and rehydrate on activation (not on UI focus); no auto-resume burst (§9.9, §9.10).
- **F11** — subagent ID duality: parent ID routes display, subagent ID keeps durability (§20.25).
- **F12** — flash clock in model: flash phase in `AggregateStatusModel`, ticked at model level (§9.9, §20.23).

---

# 55. DeepSeek Harness Comparison

DeepSeek Harness is the strongest architectural reference for this project.

Its current design explicitly says that models, tools, skills, sessions, sandboxes, storage, loops, scheduling and UI are plugins. It also treats every run as traceable through an append-only session log. citeturn0search1

The C++ project should adopt:

```text
DeepSeek concept              C++ project

Cordis context            →   Context
Cordis services           →   ServiceRegistry
Typed events              →   EventBus
Plugin lifecycle          →   Plugin
Agent                     →   Agent
Agent loop                →   AgentLoop
Session event log         →   SessionStore
LLM seam                  →   LLMProvider
Tool registry             →   ToolRegistry
Capability seams          →   Service interfaces
Execution world           →   ExecutionEnvironment
Profiles                  →   Profiles
Approvals                 →   PermissionPolicy
```

But deliberately omit initially:

```text
Cordis-compatible configuration
complex plugin dependency graph
hot module replacement
browser client
large-scale plugin ecosystem
```

DeepSeek's documentation also explicitly separates the public Agent interface from its concrete loop and recommends attaching new behavior to documented service/event seams instead of modifying the loop. That is an important pattern to preserve. citeturn0search4turn0search0

---

# 56. Most Important Architectural Insight

The project should not be thought of as:

```text
"Claude Code written in C++"
```

Instead:

```text
"C++ agent runtime with a Claude-Code-like terminal frontend"
```

The distinction matters.

The former tends toward:

```text
TUI
 └── giant agent implementation
```

The latter becomes:

```text
                 ┌── Supervisor (TUI)
                 ├── CLI
                 └── RPC
                     │
        WorkspaceHost daemons   (one per workspace, own cwd)
                     │
        ┌────────────┼────────────┐
        ▼            ▼            ▼
  SessionManager SessionManager SessionManager   (in-process, per daemon)
        │            │            │
      Agent        Agent        Agent
        │
   ┌────┴─────┬──────────┐
   │          │          │
  LLM       Tools     Execution
   │          │          │
providers  registry  environments
        │
      Events
        │
      Session
```

That second architecture can evolve into something much more powerful.

The FTXUI frontend is not the agent. It adapts core events into a presentation model:

```text
EventBus
   │
   ▼
UiEventAdapter
   │
   ▼
UiModel
   │
   ▼
FTXUI Views
```

---

# 57. First Implementation Order

Do the work in this order.

## Step 1 — Core event system

Implement:

```text
Event
EventBus
Subscription
CancellationToken
```

No LLM yet.

## Step 2 — SQLite session store

Implement:

```text
create session
append event
read events
resume
```

## Step 3 — Fake agent

Create a deterministic agent that emits:

```text
user
assistant
tool call
tool result
assistant
```

Then prove that the session can be replayed.

## Step 4 — TUI

Render the event stream.

At this point the TUI should already work without an LLM.

## Step 5 — Tool registry

Implement:

```text
read_file
grep
glob
edit_file
```

## Step 6 — Shell + permissions

Add:

```text
shell
permission policy
permission dialog
```

## Step 7 — Real LLM

Implement one provider first:

```text
OpenAI-compatible HTTP
```

Do not implement five providers simultaneously.

## Step 8 — Agent loop

Connect:

```text
LLM
 ↕
Agent
 ↕
Tools
 ↕
Session
 ↕
TUI
```

## Step 9 — Git

Add Git status/diff.

## Step 10 — Markdown and syntax highlighting

Make the output pleasant.

At this point the MVP is already usable.

## Step 11 — Multi-session, Phase A (suspended sessions)

Implement:

```text
SessionManager (§9.6)
WorkspaceModel (§20.22)
per-session SessionUiState
SessionBar (§20.23)
session switcher overlay, Ctrl+S (§20.24)
Ctrl+N new session
per-session draft preservation
workspace persistence and resume (§9.10)
```

Only the active session's agent runs. Switching gracefully cancels the current
turn and resumes the target. Static glyphs, no flash. Bake the environment-root
parameter into the §18 interface before writing any tool.

## Step 12 — Multi-session, Phase B (background execution, later)

Only if concurrent watching plus mid-flight alerts are actually needed:

```text
background execution
edge-triggered attention + flash (§9.9, F4/F12)
background permission policy (§9.9, F2)
LLM / subprocess / PTY caps (§9.11, F8)
subagent fan-in coalescing (§20.25, Q5)
```

The Phase A base does not go to waste; Phase B layers on top of it.

## Step 13 — Fork into daemon + supervisor processes

Split the monolith: the agent runtime moves into `WorkspaceHost` daemon processes
(one per workspace) and the TUI becomes the `Supervisor` process, connected over
the §9.6 Unix-socket JSON-RPC 2.0 transport. Add the shared `WorkspaceRegistry`
(§9.10) and the write lease (§9.7). This is the migration point D17/D18 define;
it is not optional and not "either way".

---

# 58. First Milestone Architecture

## Milestone 1 — single-process MVP (pre-D17)

The first serious milestone is a single-process monolith; this is what §50's MVP
and §57 Steps 1–10 build:

```text
                           ┌───────────────┐
                           │    FTXUI      │
                           └───────▲───────┘
                                   │
                                UiModel
                                   ▲
                                   │
                            UiEventAdapter
                                   ▲
                                   │
                              EventBus
                                   │
             ┌─────────────────────┼───────────────────┐
             │                     │                   │
       ┌─────▼─────┐        ┌─────▼─────┐       ┌─────▼─────┐
       │  Agent    │        │ Session   │       │  Policy   │
       │   Loop    │        │  Store    │       │           │
       └─────┬─────┘        └───────────┘       └─────┬─────┘
             │                                        │
             ├───────────────┐                        │
             │               │                        │
       ┌─────▼─────┐   ┌─────▼────────┐               │
       │    LLM    │   │ ToolRegistry │◄──────────────┘
       └───────────┘   └──────┬───────┘
                              │
                       ┌──────▼──────┐
                       │ Execution   │
                       │ Environment │
                       └─────────────┘
```

## Milestone 2 — supervisor + per-workspace daemons

Once the MVP is stable, fork it into the D17/D18 topology (§9.6): the TUI becomes
the supervisor and the runtime moves into one `WorkspaceHost` daemon per
workspace. Each daemon keeps an in-process `SessionManager` and its sessions'
agents; the shared `WorkspaceRegistry` (`registry.db`) holds only the open-set
and host registration.

```text
                ┌────────────────────────────┐
                │      Supervisor TUI        │
                └──────────────┬─────────────┘
             Unix-socket JSON-RPC 2.0 (§9.6)
        ┌──────────────┼──────────────┐
        ▼              ▼              ▼
 WorkspaceHost A  WorkspaceHost B  WorkspaceHost C
  (daemon)         (daemon)         (daemon)
  SessionManager   SessionManager   SessionManager
  Session[1..N]    Session[1..M]    Session[1..K]

  all processes read/write ──► shared registry.db (WorkspaceRegistry, §9.10)
```

Milestone 1 is the first thing to implement, not the final target. The migration
point is §57 Step 13.

---

# 59. Future Evolution

Once the MVP is stable, the architecture naturally permits:

```text
                     Agent Runtime
                           │
          ┌────────────────┼────────────────┐
          │                │                │
       Terminal          RPC              SDK
          │                │                │
          ▼                ▼                ▼
       FTXUI            JSON-RPC        application
                           │
                           ▼
                    remote runtime
```

and:

```text
ExecutionEnvironment
       │
       ├── Local
       ├── SSH
       ├── Docker
       ├── VM
       └── Sandbox
```

and:

```text
Agent
 ├── Main
 ├── Explorer
 ├── Coder
 ├── Tester
 └── Reviewer
```

without changing the fundamental model.

---

# 60. Final Recommendation

Build this in C++.

Do not attempt to beat Go or Rust at general-purpose web development. The reason for C++ here is different:

- native Linux,
- terminal-first,
- low overhead,
- excellent integration with systems tooling,
- direct access to PTYs/processes/filesystems,
- strong fit with an existing C++ development environment,
- opportunity to build a very responsive SSH application.

Use **FTXUI** for the terminal frontend and keep it completely separate from the agent runtime.

Use **Asio** as the asynchronous foundation.

Use **SQLite + an append-only event log** for sessions.

Use **nlohmann/json** for model/tool/wire representations.

Use **tree-sitter** and **cmark-gfm** for rich code/Markdown presentation.

Use **libgit2** selectively.

Most importantly, borrow the architectural insight from DeepSeek Harness rather than trying to clone its implementation:

> **Capabilities should be replaceable, events should connect the system, and the session log should make every agent run inspectable and replayable.**

That gives this project a very strong foundation for becoming a personal coding environment rather than merely another Claude-Code clone.

---

## Reference Architecture

```text
┌───────────────────────────────────────────────────────────────────────────┐
│                              FRONTENDS                                    │
│                                                                           │
│                    FTXUI TUI       CLI       JSON-RPC                     │
└─────────────────────────────────┬─────────────────────────────────────────┘
                                  │
                                  │ events / commands
                                  ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                           RUNTIME / CONTEXT                               │
│                                                                           │
│ PluginRegistry │ ServiceRegistry │ EventBus │ Configuration │ Cancellation│
└──────────────┬──────────────────────┬───────────────────────┬─────────────┘
               │                      │                       │
               ▼                      ▼                       ▼
        ┌────────────┐        ┌─────────────┐        ┌───────────────┐
        │   Agent    │        │   Session   │        │    Policy     │
        │    Loop    │        │    Store    │        │               │
        └─────┬──────┘        └─────────────┘        └───────┬───────┘
              │                                               │
              ▼                                               │
        ┌────────────┐                                        │
        │    LLM     │                                        │
        │  Provider  │                                        │
        └────────────┘                                        │
                                                              │
              ┌───────────────────────────────────────────────┘
              │
              ▼
        ┌─────────────┐
        │    Tools    │
        │  Registry   │
        └──────┬──────┘
               │
       ┌───────┼────────┬────────┬────────┐
       ▼       ▼        ▼        ▼        ▼
      FS     Shell     Git      MCP      LSP
       │       │        │        │        │
       └───────┴────────┴────────┴────────┘
                       │
                       ▼
              ExecutionEnvironment
                       │
          ┌────────────┼────────────┐
          ▼            ▼            ▼
        Local         SSH       Container
```

The FTXUI frontend is reached through the adapter path, not through direct core coupling:

```text
EventBus
   │
   ▼
UiEventAdapter
   │
   ▼
UiModel
   │
   ▼
FTXUI TUI
```

**This document is the architectural baseline. Future implementation decisions should preserve the invariants in Section 54 unless there is a deliberate architectural reason to change them.**

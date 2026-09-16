# 15 — MCP Adapter

The Model Context Protocol (MCP) is the first **external capability source** for
`ymh`: a remote (or local) MCP server advertises tools over JSON-RPC 2.0, and
this component adapts those tools into the existing `ToolRegistry` so the agent,
the loop, the permission layer, and the durable event log all see them as
**ordinary tools** (`00 §27`, `§54 D8`).

`00 §27` is one paragraph and a diagram; it states the requirement but pins no
interface. This spec is that interface. It is deliberately shaped like `07`
(Tools & Execution): pinned C++ sketches, numbered invariants `M1…`, component
failure modes, a dsh mapping, and a test plan.

This is a **design-only** artifact. It is `written`, not `verified`; no code may
be written for this component until the gate in `HANDOFF.md` §7 passes
(`DESIGN_STATUS.md` row 15). The design-first rule (`AGENTS.md`) applies: the
interfaces here are **pinned** and must not churn after verification.

Amendments to already-verified specs are **required** for this design to be
implementable as written. They are called out at their point of use and
enumerated with exact diffs in the **amendment ledger (§12.3)**:

- **AM-0a / AM-0b** — the two additive `07` seams: a post-`freeze()` adapter
  namespace on `ToolRegistry` (§4.8) and a streaming `ProcessService::spawn`
  (§5.1). Additive, no existing signature changes.
- **AM-1** — MCP live status and its supervisor projection: a live event added
  to the `00 §8.1` live set and the `01` live-event taxonomy, a
  `HostNoticeKind::McpServerStatus` added to `05 §5.3`/`§2.1`, and its handling
  in `10 §5.2` (§4.7).
- **AM-2** — the `04 §3.3` startup order gains the `ToolRegistry` construction
  and `freeze()` step MCP needs (§5.2). `04` currently has neither.
- **AM-3** — the `09 §3.3`/`§3.4` NO-MATCH default tier: `PermissionConfig`
  gains `tool_defaults` so a per-server MCP `default_verdict` is a true fallback
  below every operator rule (§6.1, §12.3). Additive.

If AM-0a/AM-0b are rejected, the conservative fallback in §12.1(ad) applies; if
AM-1 is rejected, the fallback in §12.1(p) applies; AM-2 is load-bearing (no
registry ⇒ no tools ⇒ the component is unimplementable as written); if AM-3 is
rejected, `McpServerConfig::default_verdict` is dropped and operators must write
explicit `[[permissions.rule]]` entries for `mcp.*` (the built-in `[permissions]
default` still applies).

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
                     MCP server (external process / remote endpoint)
                              │  JSON-RPC 2.0
                              ▼
                     McpTransport (stdio | HTTP+SSE)
                              │  framed messages
                              ▼
                     McpClient (initialize → tools/list → tools/call)
                              │  McpToolInfo
                              ▼
              McpManager  ── translate ──►  ToolSchema
                  │                              │
                  │  AdapterScope::replace()     │
                  ▼                              ▼
             ToolRegistry  ◄──────────────────────┘
                  │  schemas()
                  ▼
          ContextAssembler (06 §5.2)  ──►  LLM request
                  │
                  ▼
          AgentLoop (06 §5.1)  ── ToolCall → PermissionDecision → ToolResult
                  │
                  ▼
          McpTool::execute(ctx, args)  ──►  McpClient::callTool()
```

The loop (spec 06) drives the pipeline and is the **only** appender of durable
events (`06` A1/A2); the adapter produces `ToolResult`s and live events, never
log records — exactly like `07`.

The MCP adapter introduces **no new loop, no new agent, no new event type on
the durable path, and no new permission gate**. It is a tool *producer*: it
manufactures `Tool` implementations and registers them. That is the whole
design (`00 §27`: "The agent should not know that a tool came from MCP").

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 15)

- The `McpClient` contract: transport-agnostic MCP session lifecycle
  (initialize handshake, capability/version negotiation, `tools/list`,
  `tools/call`, notifications, cancellation, shutdown).
- The MCP **wire vocabulary** this component speaks: the method names,
  notification names, pagination cursors, and result shapes it consumes
  (§2.3), pinned to a supported protocol-revision set.
- `McpTransport`: the stdio (v1) and HTTP+SSE (deferred) transports, including
  the MCP framing rules (which differ from `05`'s length-prefixed framing).
- **Schema translation** both ways (§4.4): MCP `inputSchema` → `ToolSchema`;
  and the inverse identity used only for validation/echo.
- **Result translation** (§4.5): MCP `CallToolResult` / JSON-RPC error →
  `payload::ToolResult` (`01 §4.5`) and `ToolErrorCode` (`07 §2.1`).
- `McpTool : Tool` (§4.6): the per-remote-tool adapter object.
- `McpManager` (§4.7): config ingestion, per-server `McpClient` lifetime,
  registration/unregistration, reconnect, status, and shutdown.
- `McpConfig` / `McpServerConfig` (§4.1): the `[mcp]` config schema and its
  bounds.
- The additive `ToolRegistry::AdapterScope` seam (§4.8) that lets a tool
  *producer* replace its namespace after `freeze()`.

### 1.3 Boundaries — deferred to other specs

- **`Tool`, `ToolSchema`, `ToolRegistry`, `ToolContext`, `ToolConfig`,
  `ToolErrorCode`, `ToolResult`, `ExecutionEnvironment`, `ProcessService`,
  `ResourceGovernor`.** Owned by `07` / `04`; this spec consumes them and pins
  only the additive seams it needs (§4.8, §5.1).
- **`payload::ToolCall` / `payload::ToolResult` / `ToolOutcome`.** Owned by
  `01 §4.5`; this spec never redefines them.
- **Durable append and turn lifecycle.** Owned by `06`; the adapter never
  appends and never drives a turn.
- **Permission policy rules and the decision flow.** Owned by `09` (`§19`,
  `§3.2`); the adapter references the `PermissionPolicy` seam and never decides
  policy. It *does* own the **default verdict applied to MCP tools** as a
  config-provided **NO-MATCH fallback** input (AM-3, §6.1).
- **Daemon process model, `ResourceCaps`, `ResourceGovernor`, and child
  reaping.** Owned by `04` (`§3`, `§8`); the adapter owns MCP server children
  *inside* the daemon's tree, per `07 §9`.
- **Transport to the supervisor (JSON-RPC over Unix socket).** Owned by `05`;
  MCP is a *client* of an external protocol and does not reuse `05`'s frame
  codec (§2.3).
- **LLM/provider behavior.** Owned by `08`; the adapter exposes schemas, not
  messages.
- **UI projection of MCP status.** Owned by `10`; the adapter emits the bounded
  live status event defined in §4.7 (AM-1) and never `UiEvent`s.
- **MCP `resources/*`, `prompts/*`, `sampling/*`, `roots/*`, and elicitation.**
  Out of scope for v1 (§12.2 OQ-5); only `tools/*` is adapted.

### 1.4 Seam ownership relative to 04/05/06/07/09/10

| Concern | Owner | This spec's role |
|---|---|---|
| `ToolCall` / `ToolResult` payloads | 01 (`01 §4.5`) | produces `ToolResult`; never appends |
| `ToolOutcome` enum | 01 (`01 §4.5`) | reuses `{Ok, Error, Denied, Cancelled}` verbatim |
| `ToolRegistry` / `ToolSchema` | 07 (`07 §3`, `§4`) | registers adapter tools; adds `AdapterScope` (§4.8) |
| `ToolContext` / `ToolConfig` | 07 (`07 §5`) | `ToolConfig` injected into `McpManager` by ctor; no `ToolContext::config()` (§4.1/§4.6) |
| `ProcessService` | 07 (`07 §6.4`) | consumes; adds streaming `spawn` (§5.1) |
| `ResourceGovernor` / `ResourceCaps` | 04 (`04 §8`, `§2.1`) | **not** used for MCP call slots; MCP caps live in `McpConfig` (§6.6) |
| Daemon lifecycle / startup order | 04 (`04 §3.3`) | requires AM-2: add `ToolRegistry` + `McpManager` + `freeze()` (§5.2, §12.3) |
| Permission decision flow | 09 (`09 §3`, `§4`) | supplies per-server default verdict as a NO-MATCH fallback (AM-3); never decides (§6.1) |
| Transport conventions (`namespace.verb`, `data.kind`) | 05 (`05 §2.1`, `§2.2`) | MCP has its own method namespace; `05` frame codec untouched |
| Live status events | 01 (live set), 05 (`HostNoticeKind`), 10 (projection) | defines `payload::McpServerStatusChanged`; requires AM-1 (§4.7, §12.3) |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId`, `TurnId`, `StepId`, `ToolCallId`, `ToolOutcome` are frozen by
`01 §2.1` / `01 §4.5`. `ToolName`, `ToolVersion`, `ToolSchema`, `ToolErrorCode`
are frozen by `07 §2.1` / `§3.2`. They are **reproduced for reference only** and
never redefined here.

MCP-local types:

```cpp
namespace ymh {

// A configured MCP server's stable, human-authored id. It is the middle
// segment of every tool name it contributes and MUST match `[a-z][a-z0-9_]{0,31}`
// (the ToolName sub-segment grammar, 07 §2.1). Validated at config load; a
// violation is a startup ConfigError, never a runtime surprise (M3, MCP-F14).
struct McpServerId {
    std::string value;
    auto operator<=>(const McpServerId&) const = default;
};

// The raw tool name as advertised by the server. MCP allows a broader charset
// than ToolName (`[A-Za-z0-9_-]`); translation sanitizes it (§3.2).
struct McpRemoteToolName {
    std::string value;
    auto operator<=>(const McpRemoteToolName&) const = default;
};

// JSON-RPC request id used on the MCP wire. We only ever mint integers, but the
// peer may answer with the same type; a response whose id is not one of ours is
// dropped (MCP-F11).
struct McpRequestId {
    std::int64_t value{0};
    auto operator<=>(const McpRequestId&) const = default;
};

// Lifecycle state of one configured MCP server, as seen by the daemon.
enum class McpServerState : std::uint8_t {
    Disabled,      // config present, enabled=false; no client constructed
    Starting,      // transport starting / handshake in progress
    Ready,         // initialize done; tools registered; calls allowed
    Degraded,      // handshake done, zero usable tools (none advertised, or all
                   // skipped); non-terminal but NOT callable — no tool is
                   // registered. A partial skip stays Ready (§5.2).
    Disconnected,  // transport closed; reconnect scheduled
    Failed,        // terminal for this daemon lifetime (config/spawn/backoff)
    Stopped,       // shutdown completed
};

enum class McpTransportKind : std::uint8_t {
    Stdio,      // local child process, newline-delimited JSON-RPC (v1)
    HttpSse,    // remote endpoint, Streamable HTTP + SSE (deferred, OQ-2)
};

} // namespace ymh
```

### 2.2 Error taxonomy

MCP failures are classified once, at the adapter boundary, and mapped onto
`07`'s `ToolErrorCode` so no MCP-specific code ever reaches the durable
`payload::ToolResult` (`01 §4.5`, `07 §2.2`).

```cpp
namespace ymh {

// Adapter-local failure vocabulary. `McpError` is thrown by McpClient /
// McpTransport and caught by McpTool::execute, which maps it to a ToolErrorCode.
enum class McpErrorCode : std::uint8_t {
    ConfigInvalid,        // bad [mcp] config (startup; never execution)
    SpawnFailed,          // stdio child could not be spawned
    TransportClosed,      // EOF / socket closed unexpectedly
    HandshakeTimeout,     // no initialize response within handshake_timeout
    HandshakeRejected,    // initialize returned an error / unsupported revision
    ProtocolViolation,    // malformed frame, bad id, missing result+error
    SchemaIncompatible,   // remote inputSchema outside the ToolSchema subset
    UnknownTool,          // tools/call for a name the server no longer advertises
    CallTimeout,          // tools/call exceeded call_timeout
    ServerError,          // tools/call returned isError:true
    RpcError,             // tools/call returned a JSON-RPC error object
    ResultTooLarge,       // response exceeded max_frame_bytes / max_result_bytes
    Cancelled,            // turn cancellation / notifications/cancelled
    CapExhausted,         // max_servers / max_inflight_calls reached
    Internal,             // adapter invariant violated / unexpected exception
};

class McpError final : public std::runtime_error {
public:
    McpError(McpErrorCode code, std::string message);
    McpErrorCode code() const noexcept;
};

} // namespace ymh
```

Mapping to the durable vocabulary (`07 §2.2`):

| `McpErrorCode` | `ToolErrorCode` | `ToolOutcome` |
|---|---|---|
| `UnknownTool` | `UnknownTool` | `Error` |
| `SchemaIncompatible` | `InvalidArguments` | `Error` |
| `CallTimeout` | `Timeout` | `Error` |
| `SpawnFailed` / `TransportClosed` / `ProtocolViolation` / `ResultTooLarge` | `Io` | `Error` |
| `ServerError` / `RpcError` | `Internal` | `Error` |
| `CapExhausted` | `ResourceExhausted` | `Error` |
| `Cancelled` | — | `Cancelled` |
| `ConfigInvalid` / `HandshakeTimeout` / `HandshakeRejected` | `Internal` | `Error` |
| *(permission `Deny`)* | — | `Denied` |

`Denied` and `Cancelled` are never produced by `McpTool` itself: `Denied` is the
loop's (`07 §8.3`, `09 §3.7`) and `Cancelled` is only produced when the token
fires (`McpErrorCode::Cancelled` maps to `ToolOutcome::Cancelled`, never
`Error`).

### 2.3 MCP wire vocabulary (pinned subset)

MCP is JSON-RPC 2.0, but it is **not** `05`'s protocol: it is a different method
namespace on a different transport, and its framing differs. This component
speaks only the subset below; anything else is ignored (notifications) or is a
`ProtocolViolation` (requests we did not send).

**Client → server requests**

| Method | Params | Result (consumed fields) |
|---|---|---|
| `initialize` | `protocolVersion`, `capabilities`, `clientInfo` | `protocolVersion`, `capabilities`, `serverInfo`, `instructions` |
| `tools/list` | `cursor?` | `tools[]`, `nextCursor?` |
| `tools/call` | `name`, `arguments` | `content[]`, `isError?`, `structuredContent?` |
| `ping` | — | `{}` |

**Client → server notifications**

| Notification | Params | When |
|---|---|---|
| `notifications/initialized` | — | once, after a successful `initialize` |
| `notifications/cancelled` | `requestId`, `reason?` | turn cancel / call abort |

**Server → client notifications (consumed)**

| Notification | Params | Handling |
|---|---|---|
| `notifications/tools/list_changed` | — | re-`tools/list`, atomic namespace replace (§5.5) |
| `notifications/message` | `level`, `logger?`, `data` | logged at `mcp` category, redacted (§6.5) |
| `notifications/progress` | `progressToken`, `progress`, `total?` | live progress event, best-effort (§4.7) |

Server → client notifications that are **not** consumed (`resources/*`,
`prompts/*`, `sampling/*`, `elicitation/*`) are logged at debug and dropped;
they never reach the model or the loop.

**Framing.** The stdio transport frames messages **one JSON value per line**
(newline-delimited, UTF-8, no embedded raw newlines — JSON escapes them). This
is MCP's framing and is **not** `05`'s 4-byte big-endian length prefix
(`05 §3.3`). The adapter reuses `05`'s JSON-RPC *message* conventions
(`namespace.verb`-style method strings, `data.kind`-style error data) but not
its frame codec. A frame over `McpConfig::max_frame_bytes` is
`McpErrorCode::ResultTooLarge` (M11, MCP-F10).

**Protocol revision.** `initialize` negotiates a revision string. v1 supports
the revisions `2025-06-18` and `2025-03-26`; the client advertises its newest
supported revision, accepts the server's echoed revision if it is in the set,
and otherwise fails the handshake (`HandshakeRejected`, MCP-F3). The supported
set is pinned at implementation time (OQ-1). Capabilities are negotiated but
only `tools` is required: a server that does not advertise `tools` is `Degraded`
with zero tools (or `StartupRejected` when `required`; §5.2), not `Failed`.

---

## 3. Adapter model

### 3.1 The adapter pipeline

`00 §27` pins the shape:

```text
MCP Server → MCP Client → ToolSchema → ToolRegistry → Agent
```

This spec refines each arrow:

1. **MCP Server → McpClient.** `McpManager` constructs one `McpClient` per
   enabled server, starts its `McpTransport`, and performs the handshake.
2. **McpClient → McpToolInfo.** `tools/list` yields `McpToolInfo` records
   (remote name, description, `inputSchema`, annotations).
3. **McpToolInfo → ToolSchema.** Translation (§4.4) sanitizes the name,
   bounds the description, downcasts `inputSchema` to the pinned subset, and
   derives `destructive` from `annotations.destructiveHint`.
4. **ToolSchema → ToolRegistry.** `McpTool` instances are wrapped and installed
   through an `AdapterScope` (§4.8).
5. **ToolRegistry → Agent.** `ContextAssembler::tools()` returns
   `ToolRegistry::schemas()` (`06 §5.2`); the agent sees MCP tools with **no
   marker** distinguishing them from local tools.

### 3.2 Namespacing and name translation

The provider-visible name is:

```text
mcp.<server>.<tool>
```

- `<server>` is the `McpServerId` (`[a-z][a-z0-9_]{0,31}`).
- `<tool>` is the remote tool name **sanitized** to `[a-z0-9_]+`:
  lowercase ASCII; every character outside `[a-z0-9_]` becomes `_`; a leading
  digit gets an `m_` prefix; consecutive `_` collapse; trailing `_` is trimmed;
  empty result becomes `tool`.
- The full name must satisfy `07 §2.1`'s `ToolName` grammar
  (`[a-z][a-z0-9_]* ("." [a-z0-9_]+)*`). The `mcp.` prefix guarantees the
  leading character is valid.

Examples:

```text
mcp.github.search
mcp.database.query
mcp.fs.read_file
```

**Collisions.** Within one server, two remote names may sanitize to the same
`<tool>`. The adapter does **not** silently rename (that would make the model's
view depend on iteration order). The first in `tools/list` order wins; each
later colliding tool is **skipped** and recorded in
`McpServerStatus::skipped_tools` (MCP-F8), which is surfaced as a live status
event. Across servers, collisions are impossible because `<server>` is unique.

**Collision with a local tool.** A translated name that already exists in the
registry as a non-adapter tool (e.g. a local plugin registered `mcp.foo.bar`)
**fails loud** (`ConfigInvalid`): the colliding tool is skipped, the server is
`Failed` (`StartupRejected` when `required`), and it is never silently degraded
to `Ready`. Local tools always win; an adapter may never shadow a non-adapter
tool (M3, MCP-F8).

### 3.3 Transparency rules

- `ToolSchema` gains **no** `origin`/`provider` field. The only signal that a
  tool came from MCP is the `mcp.` name prefix, which is a naming convention,
  not a type distinction. Nothing in `Tool`, `ToolRegistry`, `ToolContext`,
  `ContextAssembler`, `AgentLoop`, or the provider adapters branches on it
  (M1).
- `McpTool` is a `Tool` like any other (`07 §3.1`): pure `execute`, no append,
  no UI, no policy decision, observes `ctx.cancellation()`.
- The permission layer classifies MCP tools with the **same** `evaluate()`
  call (`09 §3.1`). There is no separate MCP approval path (M2, §6.1).
- The durable event stream contains only `ToolCall` / `PermissionDecision` /
  `ToolResult`; there is no `McpCall` event (`01 §4.5`). Replay of a session
  that used MCP tools needs **no MCP server** (`§11.4`).

---

## 4. C++ interfaces (pinned)

### 4.1 `McpConfig` and `McpServerConfig`

`07 §5.5` pins `ToolConfig`; `04 §4.1` pins `HostConfig`; `02 §4.1` pins
`PersistenceConfig`. MCP bounds have no owning type elsewhere, so this spec pins
them in `McpConfig` (one per daemon), plus one `McpServerConfig` per configured
server. The daemon constructs `McpConfig` from the layered `Config` (§5.6) and
injects it; the adapter reads it, never global state.

```cpp
namespace ymh {

struct McpServerConfig {
    McpServerId            id;

    // ---- enablement / failure policy --------------------------------------
    bool                   enabled{true};
    // required=true: the server MUST reach Ready with >=1 translated tool by
    // startup_deadline. Any other outcome — spawn/handshake/list failure,
    // deadline exceeded, zero tools advertised, or every tool skipped — fails
    // daemon startup with StartupRejected (04 §2.1, M6). required=false: the
    // same outcomes leave the server Failed (on failure) or Degraded (zero
    // usable tools) and the daemon proceeds. The full condition table is §5.2.
    bool                   required{false};

    // ---- transport ---------------------------------------------------------
    McpTransportKind       transport{McpTransportKind::Stdio};

    // Stdio only. argv-first (07 §16): no shell string is ever constructed.
    std::string            command;                 // absolute or PATH-resolved
    std::vector<std::string> args;
    // Each entry is "KEY=VALUE"; VALUE may contain ${ENV} references resolved
    // from the daemon's environment at spawn time. Values are NEVER logged.
    std::vector<std::string> env;
    // Root-relative (resolve()d under root()) or empty => root() itself.
    std::filesystem::path  cwd;

    // HttpSse only (deferred, OQ-2).
    std::string            url;
    std::vector<std::string> header_env;            // "Name=${ENV}"

    // Optional pin; empty => negotiate (advertise the newest supported).
    std::string            protocol_version;

    // ---- tool filtering ----------------------------------------------------
    // Glob dialect of 09 §3.3 (* / ** / ?), matched against the RAW remote name.
    // Empty allow list => all tools allowed. Deny wins over allow.
    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;

    // ---- policy / bounds ---------------------------------------------------
    // Default verdict the policy layer applies to this server's tools when no
    // explicit rule matches (09 §3.3). Never Allow unless configured (M7, §6.1).
    PolicyVerdict          default_verdict{PolicyVerdict::Ask};
    std::chrono::milliseconds call_timeout{60'000};
    std::size_t            max_result_bytes{1u * 1024u * 1024u};
};

struct McpConfig {
    bool                   enabled{true};

    std::vector<McpServerConfig> servers;

    // Bounds (per daemon).
    std::size_t            max_servers{8};                   // MCP-F12, §6.6
    std::size_t            max_inflight_calls_per_server{4}; // MCP-F12, §6.6

    // Startup is bounded: the sum of per-server startup work may not exceed
    // this; a server not settled by then is Failed, and a server that settles
    // with zero usable tools is Degraded (M6, MCP-F2, §5.2).
    std::chrono::milliseconds startup_deadline{5'000};
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds list_timeout{5'000};
    std::size_t            list_max_pages{64};               // pagination bound

    // Reconnect (stdio). Bounded attempts; 0 attempts => no reconnect (Failed).
    std::uint32_t          reconnect_max_attempts{5};
    std::chrono::milliseconds reconnect_initial_backoff{500};
    std::chrono::milliseconds reconnect_max_backoff{30'000};
    double                 reconnect_jitter{0.25};
    // A connection is "stable" once it has stayed Ready this long; only then
    // does a successful ping reset reconnect_attempt to zero (§4.3/§5.4).
    std::chrono::milliseconds reconnect_stable_window{30'000};
    // Liveness probe cadence while Ready; ping is never on the call path.
    std::chrono::milliseconds ping_interval{15'000};

    // Shutdown: stdin close + SIGTERM, then SIGKILL after grace (M15).
    std::chrono::milliseconds shutdown_grace{2'000};

    // Wire bound; must be <= protocol::TransportLimits::max_frame_bytes (05 §3.3).
    std::size_t            max_frame_bytes{8u * 1024u * 1024u};

    // HttpSse is off unless explicitly enabled (egress policy, §6.7).
    bool                   allow_network_servers{false};
};

} // namespace ymh
```

Rules:

- `McpConfig` is validated **once at startup** by `McpManager`, which receives
  the process `ToolConfig` by constructor injection (there is no
  `ToolContext::config()`; `07 §5.1`): an unknown transport, a missing `command`
  for `Stdio`, a malformed `McpServerId`, a duplicate server id, a
  `max_result_bytes` above `tools.tool_result_max_bytes`, or an enabled
  `HttpSse` when `allow_network_servers == false` is a `ConfigError` (startup),
  never a runtime surprise (MCP-F14). This matches the strict loader in
  `config.hpp` (unknown TOML keys already throw).
- `McpServerConfig::max_result_bytes` is clamped at runtime to
  `tools.tool_result_max_bytes` (`07 §5.5`, injected into `McpManager`) minus
  envelope headroom, so an MCP result can never trigger `PayloadTooLarge` (M11).
  The effective clamp is passed to each `McpTool` at construction (§4.6).
- `McpServerConfig::default_verdict` is **input** to the policy layer; the
  adapter never evaluates it. `Allow` is only reachable when the operator writes
  it explicitly (M7). The daemon's config assembly lowers each enabled server's
  `default_verdict` into one `PermissionConfig::ToolDefault{prefix =
  "mcp.<server>.", verdict = default_verdict, id = "mcp.<server>.default"}`
  (AM-3, §6.1, §12.3); `McpManager`'s constructor is unchanged and holds no
  policy reference. Because the value is a NO-MATCH fallback (not a
  specificity-ranked rule), an operator's `[[permissions.rule]]` for `mcp.*`
  always outranks it.

### 4.2 `McpTransport`

One transport **at a time** per `McpClient`: a reconnect replaces the transport
object inside the *same* client (§5.4). It owns byte-level framing and connection
lifetime; it knows nothing about MCP methods.

```cpp
namespace ymh {

// Why a transport closed. Drives reconnect vs. terminal state (MCP-F7).
enum class McpDisconnectReason : std::uint8_t {
    ClientClose,     // we closed it (shutdown)
    ServerEof,       // peer closed stdin/stdout or the socket
    SpawnFailed,     // process could not be started
    ProtocolError,   // framing violation; connection is untrusted
    TransportError,  // I/O error
};

class McpTransport {
public:
    virtual ~McpTransport() = default;

    // Start the underlying connection (spawn the child / open the endpoint).
    virtual Task<void> start(CancellationToken) = 0;

    // Send one JSON-RPC message. Framing is transport-specific (§2.3).
    virtual Task<void> send(const nlohmann::json& message, CancellationToken) = 0;

    // Exactly one handler; invoked on the daemon io_context, in order (04 §9).
    virtual void setMessageHandler(std::function<void(nlohmann::json)>) = 0;
    virtual void setCloseHandler(std::function<void(McpDisconnectReason)>) = 0;

    // Close the connection. For Stdio: close stdin, SIGTERM the group, wait
    // `grace`, SIGKILL; idempotent (M15).
    virtual Task<void> close(std::chrono::milliseconds grace) = 0;

    // The child pid for a Stdio transport; 0 for HttpSse.
    virtual std::uint64_t childPid() const noexcept = 0;
};

} // namespace ymh
```

Implementations:

- **`StdioMcpTransport` (v1, pinned).** Spawns the configured command
  **argv-first** through the additive `ProcessService::spawn` seam (§5.1), with
  `cwd = ExecutionEnvironment::resolve(cwd)` (empty ⇒ `root()`), a sanitized
  environment, and its own process group. Reads newline-delimited JSON from the
  child's stdout as an async descriptor on the daemon's one `io_context`, and
  writes newline-delimited JSON to its stdin serialized on that same loop
  (`04 §9`, `07 §10`). It is a long-lived child, not a one-shot
  `ProcessService::run` (`07 §6.4`), which is why §5.1 is required.
- **`HttpSseMcpTransport` (deferred).** Streamable HTTP with an SSE response
  stream and `Mcp-Session-Id`; disabled unless `allow_network_servers` is set.
  Its exact shape is OQ-2. The `McpClient` contract is transport-agnostic so
  adding it is not a redesign.

### 4.3 `McpClient`

`McpClient` is the MCP session state machine. It owns exactly one transport at a
time (a reconnect recreates the transport inside the same client; the client
object is stable for the server's daemon lifetime, §5.4) and serializes
request/response pairing, notifications, cancellation, and reconnect.

```cpp
namespace ymh {

struct McpToolInfo {
    McpRemoteToolName      remote_name;
    std::string            description;       // raw; bounded at translation
    nlohmann::json         input_schema;      // raw JSON Schema
    bool                   destructive{false}; // from annotations.destructiveHint
    std::optional<std::string> title;         // annotations.title, optional
};

struct McpCallResult {
    nlohmann::json         content;           // raw content[] (never logged)
    bool                   is_error{false};
    std::optional<nlohmann::json> structured_content;
};

struct McpCallOptions {
    ToolCallId             call_id;           // for notifications/cancelled
    std::chrono::milliseconds timeout;        // 0 => server default
    std::size_t            max_bytes;         // result bound
};

// Live-only status snapshot (never durable; §4.7).
struct McpServerStatus {
    McpServerId            id;
    McpServerState         state{McpServerState::Disabled};
    std::string            server_name;       // serverInfo.name, if known
    std::string            server_version;    // serverInfo.version, if known
    std::string            protocol_version;  // negotiated revision
    std::size_t            tool_count{0};
    std::vector<std::string> skipped_tools;   // sanitization/filter skips (MCP-F8)
    std::string            last_error;        // bounded, redacted (MCP-F14)
};

class McpClient {
public:
    virtual ~McpClient() = default;

    virtual const McpServerId& id() const noexcept = 0;
    virtual McpServerState     state() const noexcept = 0;

    // Start transport + initialize handshake + notifications/initialized.
    // Idempotent while Ready. Throws McpError on handshake failure.
    virtual Task<void> start(CancellationToken) = 0;

    // tools/list with cursor pagination, bounded by list_max_pages.
    virtual Task<std::vector<McpToolInfo>> listTools(CancellationToken) = 0;

    // tools/call. Observes the token; on fire sends notifications/cancelled
    // and rejects with McpErrorCode::Cancelled (M10).
    virtual Task<McpCallResult> callTool(std::string_view remote_tool,
                                         const nlohmann::json& arguments,
                                         const McpCallOptions&,
                                         CancellationToken) = 0;

    // Per-server in-flight `tools/call` slot (M11, §6.6). The client owns the
    // counter, bounded by max_inflight_calls_per_server. RAII: releasing on
    // destruction is the only release path, so no exit path can leak (MCP-F17).
    class CallSlot {
    public:
        CallSlot(CallSlot&&) noexcept;
        CallSlot& operator=(CallSlot&&) noexcept;
        ~CallSlot();                                  // releases the slot
    };

    // nullopt => saturated; McpTool::execute maps it to Error{ResourceExhausted}
    // (MCP-F12). Never blocks.
    [[nodiscard]] virtual std::optional<CallSlot> tryAcquireCallSlot() = 0;

    // Liveness probe. Returns true iff the server answered `ping` within
    // `list_timeout`; false on timeout / transport error (the caller treats a
    // false as a disconnect, §5.4). It never changes state() and is never on
    // the tools/call path — a call never waits for a ping ("never gates work").
    virtual Task<bool> ping() = 0;

    // Close the transport and stop the child. Idempotent.
    virtual Task<void> shutdown(std::chrono::milliseconds grace) = 0;

    // Called on the daemon io_context for every server notification (MCP-F11).
    virtual void setNotificationHandler(
        std::function<void(std::string_view method, const nlohmann::json& params)>,
        std::function<void()> on_tools_changed) = 0;

    virtual McpServerStatus status() const = 0;
};

// Factory seam for tests: production builds StdioMcpTransport; tests inject a
// scripted transport or a fake client (07 pattern, §11). Called only at startup;
// a reconnect never constructs a new client (§5.4).
using McpClientFactory =
    std::function<std::unique_ptr<McpClient>(const McpServerConfig&,
                                             McpConfig&,
                                             ExecutionEnvironment&,
                                             ResourceGovernor&,
                                             Logger&)>;

} // namespace ymh
```

Rules:

- **Handshake before use (M5).** No `tools/list` or `tools/call` is issued
  before `initialize` succeeds and `notifications/initialized` is sent.
  **`Ready` is the only callable state.** A call reaching a client in any other
  state throws `McpError` immediately (mapped to `Error`, M9) — it never blocks
  waiting for readiness. `Degraded` registers zero tools, so no `McpTool` exists
  to call; `Failed` keeps its previously-registered tools in the schema
  (§12.1(o)) but every call fails closed with `Error`.
- **Request/response pairing.** Every request gets a monotonic `McpRequestId`.
  A response whose `id` is unknown, or that carries neither `result` nor
  `error`, is a `ProtocolViolation` and is dropped (the pending call times out
  normally) (MCP-F11).
- **Server-initiated requests.** MCP servers may send requests (e.g.
  `sampling/createMessage`, `roots/list`). v1 advertises **no** client
  capabilities that would solicit them; an inbound request is answered with a
  JSON-RPC `MethodNotFound` error and logged (it is never routed to the model).
- **Cancellation.** `callTool` observes its token. On fire it sends
  `notifications/cancelled{requestId, reason:"cancelled"}`, drops the pending
  entry, and throws `McpErrorCode::Cancelled` (M10).
- **`status()` is a snapshot** safe to read from any thread; it is never a
  control path.
- **In-flight slots are client-owned (M11).** The per-server
  `max_inflight_calls_per_server` counter lives in `McpClient`;
  `McpTool::execute` acquires via `tryAcquireCallSlot()` and releases by RAII.
  `ResourceGovernor` has no MCP slot API and is not consulted (§6.6).
- **`ping()` is a probe, not a gate.** A `false` result is a disconnect; a
  `true` result after a stable `Ready` window resets the reconnect backoff
  (§5.4). It is never required before `tools/call` and never changes `state()`.

### 4.4 Schema translation (both directions)

**MCP → ymh (primary).** MCP `inputSchema` is arbitrary JSON Schema; `ymh`'s
`ToolSchema.input_schema` is a deliberately small, lossless-for-providers
subset (`07 §3.2`). Translation is a **downcast with an explicit allowlist**:

```text
translate(inputSchema, remote_name) -> Result<ToolSchema, McpErrorCode>
  1. require an object with "type" == "object"; else SchemaIncompatible
  2. copy "description" (bounded to the injected ToolConfig::
     tool_description_max_bytes; §4.1)
  3. for each property:
       copy "description", "default", "enum" (scalar only)
       map "type" ∈ {object,string,integer,number,boolean,array}
       recurse for "object"/"array" ("items", "properties")
       a "type" union/array, "$ref", "anyOf"/"oneOf"/"allOf", "not",
         "patternProperties", "if/then/else", "const" -> SchemaIncompatible
  4. "required" copied verbatim (names not in properties are dropped + noted)
  5. "additionalProperties":
       false or absent  -> false        (tighten; noted in status)
       true             -> SchemaIncompatible (open object; cannot map losslessly)
  6. unknown top-level keywords are dropped and recorded in status
  7. name := "mcp." + server + "." + sanitize(remote_name)   (§3.2)
```

- A `SchemaIncompatible` result **skips that tool** (recorded in
  `skipped_tools`), not the server: one exotic schema must not disable a server
  (MCP-F4).
- Tightening `additionalProperties` from absent to `false` is a deliberate,
  documented loss: it is the only way to satisfy the pinned subset. It can
  cause the model's extra keys to be rejected by validation (`07 §3.3`), which
  is a visible `InvalidArguments` error, never a silent drop.
- `destructive` is taken from `annotations.destructiveHint` when the server
  advertises the tool-annotations capability; otherwise it defaults `false`.
  It is a **hint** to the permission layer (`07 §3.2`), never an enforcement.

**ymh → MCP (inverse).** There is no reverse adapter in v1 (we never expose
`ymh` tools *to* an MCP server). The only inverse use is **argument echo**:
`McpTool::execute` forwards `ToolArguments.value` to the server verbatim after
`ToolRegistry` validated it against the translated schema (`07 §3.3`). No
re-serialization or key rewriting occurs; the arguments the server sees are
exactly the arguments the model produced, modulo JSON normalization.

### 4.5 Result and error mapping → `ToolResult`

MCP `tools/call` has three outcomes: a result with `content[]`, a result with
`isError:true`, or a JSON-RPC error object. Mapping is pinned:

```text
callTool(remote, args):
  ├─ JSON-RPC error object      -> McpError{RpcError}      -> Error{Internal}
  ├─ result{isError:true}       -> McpError{ServerError}   -> Error{Internal}
  └─ result{content[], ...}     -> text projection         -> Ok
```

**Text projection.** `content[]` is an array of typed blocks. `ymh`'s durable
`ToolResult.output` is a single string, so:

| MCP content block | Projection into `output` |
|---|---|
| `{type:"text", text}` | `text`, in order, joined by `\n` |
| `{type:"image", mimeType, data}` | `[image <mimeType> <decoded-bytes> bytes]` (base64 never dumped) |
| `{type:"audio", ...}` | `[audio <mimeType> <bytes> bytes]` |
| `{type:"resource", resource:{uri, mimeType, text?}}` | `[resource <uri>]` + `text` when inline and textual |
| `structuredContent` present | appended as a fenced `json` block, bounded |
| unknown block type | `[<type> <n bytes>]` |

- **Images/audio/binary are never materialized into `output`** — only a typed
  placeholder. This is a §40 redaction rule as much as a size rule (M12).
- **Size.** The projection is clamped on the **serialized** payload size below
  the `ToolConfig::tool_result_max_bytes` value injected into `McpManager` and
  passed to each `McpTool` (§4.1/§4.6; `07 §5.2`, X10). Clamping sets
  `truncated = true`. A response whose raw frame exceeds
  `McpConfig::max_frame_bytes` is rejected as `ResultTooLarge` (M11, MCP-F10)
  rather than buffered unbounded.
- **`isError:true` is `Error`, not `Ok`.** This differs from `07`'s subprocess
  decision (h) because MCP's `isError` is an explicit *tool failure* signal, not
  a process exit code. `output` still carries the server's error text so the
  model can recover (the loop continues, `06` A-F5).
- **Empty content** is `Ok` with `output = ""`.

### 4.6 `McpTool : Tool`

`McpTool` is the per-remote-tool adapter. It is the **only** MCP type the
registry and the loop ever touch.

```cpp
namespace ymh {

class McpTool final : public Tool {
public:
    // `client` is shared: one McpClient serves every tool of its server.
    // `schema` is pre-translated and immutable for the tool's lifetime; a
    // reconnect that changes the schema produces a NEW McpTool via
    // AdapterScope::replace (§5.4/§5.5), never a mutation in place.
    McpTool(std::shared_ptr<McpClient> client,
            McpServerConfig           server,
            ToolSchema                schema,
            McpRemoteToolName         remote_name,
            std::size_t               result_max_bytes);   // injected (§4.1)

    ToolSchema schema() const override;                    // cached, immutable

    // Builds McpCallOptions from ctx, acquires the per-server in-flight slot
    // from client_, calls the client, maps the result. Observes
    // ctx.cancellation(); the RAII slot releases on every exit path (M10/M11).
    // Never appends, never touches the UI, never decides policy (07 §3.1).
    Task<ToolResult> execute(const ToolContext&,
                             const ToolArguments&) override;

private:
    std::shared_ptr<McpClient> client_;
    McpServerConfig            server_;
    ToolSchema                 schema_;
    McpRemoteToolName          remote_name_;
    std::size_t                result_max_bytes_{0};  // injected (§4.1)
};

} // namespace ymh
```

Rules:

- `execute` maps `McpError` to `ToolResult` per §2.2; any other exception maps
  to `Error{Internal}` at the registry boundary (`07` E-F16). The loop never
  sees an MCP exception.
- `execute` must not retry a `tools/call`: MCP calls are not idempotent in
  general (`07 §3.1` rule). A retry is a new model turn, not an adapter action.
- `execute` may `ctx.emit(...)` progress live events derived from
  `notifications/progress`; those are best-effort and coalesced (`07 §8.2`).
- `execute` acquires its in-flight slot from `client_` (`McpClient::
  tryAcquireCallSlot()`) and reads `ctx.cancellation()` for the token. It never
  calls `ctx.governor()`: `ResourceGovernor` has no MCP slot API (§6.6, M11).
  Saturation (`nullopt`) is `Error{ResourceExhausted}` (MCP-F12). It reads
  nothing else from the daemon, and it never reads a `ToolConfig` from
  `ToolContext` (`07 §5.1` has no `config()`): `result_max_bytes_` is injected at
  construction.

### 4.7 `McpManager`

`McpManager` is the daemon-owned orchestrator: it turns `McpConfig` into
registered tools and owns every `McpClient`.

```cpp
namespace ymh {

class McpManager {
public:
    McpManager(McpConfig               config,
               const ToolConfig&       tools,
               ExecutionEnvironment&   environment,
               ResourceGovernor&       governor,
               ToolRegistry&           registry,
               EventBus&               bus,
               Logger&                 logger,
               ClockReader             now = Clock::now);

    // Start every enabled server within startup_deadline, handshake, list,
    // translate, and install tools via AdapterScope. Non-throwing for
    // non-required servers (they become Failed, or Degraded when zero tools
    // translated); throws ConfigError only for invalid config and McpError for
    // a failed required server (M6, §5.2).
    Task<void> start(CancellationToken);

    // Re-list and atomically replace one server's tool namespace. Called on
    // notifications/tools/list_changed and after a reconnect (M14).
    Task<void> refresh(McpServerId, CancellationToken);

    // Close every client (stdin close, SIGTERM, SIGKILL after grace), release
    // scopes, and drop registrations. Idempotent; called from daemon drain.
    Task<void> shutdown(std::chrono::milliseconds grace);

    std::vector<McpServerStatus> statuses() const;   // stable order by id

    // Test seam: replace client construction (07 pattern, §11.2).
    void setClientFactory(McpClientFactory);

private:
    // one entry per configured server
    struct ServerSlot {
        McpServerConfig                       config;
        std::shared_ptr<McpClient>            client;
        std::optional<ToolRegistry::AdapterScope> scope;
        McpServerState                        state{McpServerState::Disabled};
        std::uint32_t                         reconnect_attempt{0};
        std::chrono::milliseconds             next_backoff{0};
        std::vector<std::string>              skipped_tools;
    };
    std::vector<ServerSlot> slots_;
    // ...
};

} // namespace ymh
```

**Live status events.** `McpManager` emits a **live-only** event on every state
transition (start, ready, degraded, disconnected, failed, stopped). The payload
has no home in the existing event model, so it is defined here and its required
cross-spec amendments are recorded as **AM-1** (§12.3):

```cpp
namespace ymh::payload {

// Live-only; never appended to the session log (00 §8.1 persistence rule).
// Carries no arguments, results, or secrets (M12).
struct McpServerStatusChanged {
    McpServerId     server;
    McpServerState  state;
    std::size_t     tool_count{0};
    std::string     reason;      // bounded, redacted (MCP-F14)
};

} // namespace ymh::payload
```

- **Delivery (AM-1).** The event travels on the shared `EventBus` to **global
  subscribers only** (`00 §8.3`): MCP status is daemon/workspace-scoped, has no
  `SessionId`, and is therefore **not** routed through a per-session mailbox.
  The daemon's host-event forwarder projects it to the supervisor as
  `protocol::HostNotice{kind = HostNoticeKind::McpServerStatus}` (`05 §5.3`,
  `10 §5.2`). The projection reuses the existing `HostNotice.detail` for a
  bounded, redacted status token (`id`, `state`, `tool_count`); the full
  structured status stays in `McpManager::statuses()` and is not carried on the
  wire, so no new `HostNotice` field is required. Both the live-set entry
  (`00 §8.1`, `01 §4.1`) and the
  `HostNoticeKind` (`05 §5.3`/`§2.1`) are **amendments, not existing homes**;
  see §12.3 (AM-1).
- **Fallback.** If AM-1 is rejected, `McpManager` emits the existing live
  `Progress` event with a bounded status token, and `statuses()` remains the
  only structured snapshot (§12.1(p)).
- It is never durable and carries no arguments or results (M12); the UI
  projection is `10`'s concern.

**Constructor injection.** `tools` (the process `ToolConfig`) supplies the
description/result bounds used by translation and result clamping
(§4.1/§4.4/§4.5); it is passed to each `McpTool` as `result_max_bytes`.
`governor` is retained only because `McpClientFactory` (`§4.3`) requires it; v1
acquires **no** `ResourceGovernor` cap for MCP — server processes are bounded by
`max_servers` and call slots by the client (§6.6).

### 4.8 Additive seam: `ToolRegistry::AdapterScope` (requires a 07 errata)

`07` freezes the registry after startup: `add`/`remove` after `freeze()` throws
`RegistryFrozen` (`07 §4.1`, X5, decision (c)). MCP cannot live with that as-is,
for two reasons:

1. **Initial discovery is asynchronous.** `tools/list` requires a completed
   handshake, so tools are not known at static-wiring time.
2. **`notifications/tools/list_changed` and reconnects change the set at
   runtime.** The task explicitly requires register *and unregister*.

The pinned additive seam (no existing signature changes):

```cpp
namespace ymh {

class ToolRegistry {
public:
    // ... existing 07 §4.1 surface unchanged ...

    // Additive (15). A namespaced lease held by a tool PRODUCER (the MCP
    // adapter). Acquired BEFORE freeze(); afterwards the scope's membership may
    // be atomically replaced, which bumps generation() and invalidates the
    // cached schema snapshot (07 E-F15). Ordinary add/remove stay frozen (X5).
    class AdapterScope {
    public:
        AdapterScope(AdapterScope&&) noexcept;
        AdapterScope& operator=(AdapterScope&&) noexcept;
        ~AdapterScope();                              // removes its tools
        [[nodiscard]] bool held() const noexcept;

        // Atomic: on success the scope holds exactly `tools`, all names share
        // the scope prefix, no name shadows a non-adapter tool, and every
        // schema is valid (07 §3.2). On any violation the previous set is
        // retained (strong guarantee) and ToolRegistryError is thrown.
        std::size_t replace(std::vector<std::unique_ptr<Tool>> tools);

        std::size_t size() const noexcept;
        std::string_view prefix() const noexcept;     // e.g. "mcp.github."
    };

    // One scope per adapter namespace. `prefix` must be a valid ToolName
    // prefix ending in '.', unique among scopes, and disjoint from non-adapter
    // tool names. Callable only before freeze(); after freeze() it throws
    // RegistryFrozen. Names under a scope do NOT participate in the ordinary
    // DuplicateName/DuplicateVersion rules; the scope owns them exclusively.
    [[nodiscard]] AdapterScope openAdapter(std::string_view prefix);
};

} // namespace ymh
```

Rules:

- `openAdapter` is called during daemon startup wiring, **before**
  `ToolRegistry::freeze()` (§5.2). `replace` is callable after freeze.
- `replace` is **atomic and generation-bumping**: a concurrent `schemas()`
  reader sees either the old or the new set, never a mix (M14). `schemas()`
  remains byte-stable between replacements (X14, M13).
- The scope may only produce names with its prefix. This is what keeps MCP
  tools disjoint from local tools and from other adapters (M3).
- `~AdapterScope` removes its tools and bumps the generation; it is the
  unregister path (§5.3).
- **This is a `07` amendment**, recorded as **AM-0a** (OQ-3) in §12.3. If the
  reviewer rejects it, the conservative fallback is §12.1(ad): tools are
  discovered once before `freeze()`, `tools/list_changed` is ignored, and a
  reconnect that changes the set is deferred to a daemon restart.

---

## 5. Lifecycle and ownership

### 5.1 Process ownership (the workspace daemon)

**Decision (pinned): the `WorkspaceHost` daemon owns every `McpClient`.** The
supervisor/TUI owns none (`04 §3.7`: the daemon owns the process, cwd, caps, and
lifecycle; `11 §11.2`: runtime ⊃ adapter ⊃ protocol ⊃ transport). Consequences:

- MCP servers are **daemon-lifetime**, not session-lifetime: they start when the
  daemon starts and stop when it drains. A session that never uses a server
  still pays for its process; this is bounded by `max_servers` and is the price
  of MCP's long-lived handshake. (Per-session lazy start is OQ-4.)
- The supervisor never spawns, sees, or signals an MCP server. It observes only
  live status events and (optionally) `host.event` notices.
- A TUI detach does not stop MCP servers (`04 §6.5`, `D23`); a daemon crash
  leaves them to the daemon's process-tree reaping (`07 §9.4`).
- `ymh run` (headless, single-process) constructs a `McpManager` too, so
  headless mode has the same MCP capability as the daemon path (M4).

**Required additive `07` seam.** `07 §6.4`'s `ProcessService::run` is a
one-shot request/response. A stdio MCP server needs a long-lived bidirectional
child. The pinned additive method (no existing signature changes):

```cpp
namespace ymh {

class ProcessService {
public:
    // ... existing 07 §6.4 run() unchanged ...

    // Additive (15). Spawn a long-lived child with piped stdin/stdout. The
    // child is placed in its own process group and is reaped by the same
    // specific-pid path as run() (07 §9.2, 11 E8). `sink` is ignored; the
    // caller reads via the handle. On failure throws ToolError{SpawnFailed}.
    virtual Task<std::unique_ptr<ChildProcessHandle>>
    spawn(const ProcessRequest&) = 0;
};

class ChildProcessHandle {
public:
    virtual ~ChildProcessHandle() = default;
    virtual std::uint64_t pid() const noexcept = 0;

    // Bounded write to the child's stdin. Backpressure is the caller's
    // (McpTransport) concern; a closed pipe is ToolError{Io}.
    virtual Task<void> writeStdin(std::string_view bytes, CancellationToken) = 0;
    virtual void closeStdin() = 0;

    // One read chunk; empty result => EOF.
    virtual Task<std::size_t> readStdout(std::span<char>, CancellationToken) = 0;

    virtual void signal(int sig) noexcept = 0;   // signals the process group
    virtual Task<ProcessResult> wait(CancellationToken) = 0;
};

} // namespace ymh
```

This seam is what makes M15 (process ownership) and §6.3 (timeout/kill)
implementable without bypassing the daemon's process layer. It is recorded as
**AM-0b** (OQ-3) alongside the registry seam (§12.3).

### 5.2 Startup order (requires the `04 §3.3` amendment AM-2)

`04 §3.3` step 6 builds the in-process runtime, but as written it contains **no
`ToolRegistry` construction and no `freeze()`** (the string `ToolRegistry` does
not appear in `04` at all). MCP therefore requires step 6 to be extended; the
added lines are marked `+` and the exact diff is recorded as **AM-2** in §12.3.

```text
 6. build the in-process runtime:
      env      := LocalEnvironment(root = workspace_root)      (§18)
      bus      := EventBus()                                   (§8.3)
      governor := ResourceGovernor(caps)                       (§9.11)
      sessions := SessionManager(store, bus, env, governor)    (01 §8)
+     registry := ToolRegistry(); register local tools         (07 §4.3)
+     mcp      := McpManager(mcp_config, tool_config, env, governor,
+                            registry, bus, log)               (§4.7)
 6a. (async) mcp.start(...)   # bounded by startup_deadline (M6)
      -> openAdapter("mcp.<id>.") per enabled server, handshake, tools/list,
         translate, replace()
+6b. registry.freeze()        # only after 6a settles; M5/X5
 7. bind + listen ...
```

- **`openAdapter` must precede `freeze()`; `replace()` may follow it.** The
  scopes are opened in 6a, `freeze()` runs in 6b, and a later reconnect or
  `list_changed` uses `replace()` (M14).
- **`freeze()` waits for MCP discovery, but only up to `startup_deadline`.** A
  slow/unresponsive server cannot hold the daemon hostage: a server not settled
  by the deadline is marked `Failed`, a server that settles with zero usable
  tools is `Degraded`, their tools are simply absent, and the daemon proceeds
  (M6, MCP-F2). This is the MCP instance of the "bootstrap must not block" rule
  (`11 §12.6`).
- **`required` is defined by this table** (evaluated once, at the end of 6a):

  | Outcome by `startup_deadline` | `required = false` | `required = true` |
  |---|---|---|
  | handshake done, ≥ 1 tool translated | `Ready` | `Ready` |
  | handshake done, ≥ 1 tool translated, ≥ 1 skipped | `Ready` (`skipped_tools` non-empty) | `Ready` |
  | handshake done, 0 tools translated (none advertised, or all skipped) | `Degraded` | `StartupRejected` |
  | spawn / handshake / `tools/list` failure | `Failed` (terminal) | `StartupRejected` |
  | not settled by the deadline | `Failed` (terminal) | `StartupRejected` |

  A **skipped** tool is one dropped by translation, filtering, or collision
  (§4.4, MCP-F4/MCP-F8); it is recorded in `status().skipped_tools` but does
  **not** demote the server as long as ≥ 1 tool translated. `Degraded` is the
  **zero-usable-tools** state: it is non-terminal (a later reconnect or
  `list_changed` can promote it to `Ready`) but it registers no tools, so it is
  **not callable** (M9). `Failed` is terminal for the daemon lifetime; it keeps
  its previously-registered tools in the schema for the running turn's sake but
  every call fails closed with `Error` (§12.1(o)). `StartupRejected` (`04
  §2.1`) is the only outcome that aborts the daemon; a `required` server that
  fails is never a silently degraded daemon.
- MCP never `chdir()`s. The daemon's single `chdir(root)` (`04 §3.3` step 2)
  already happened; a stdio server's cwd is `resolve(cwd)` under that root
  (M8, `§6.2`).

### 5.3 Shutdown order (amends `11 §11.2`)

`11 §11.2` pins destruction order: **runtime ⊃ adapter ⊃ protocol ⊃ transport**.
MCP sits inside the runtime, so it drains **before** the transport stops:

```text
host.shutdown / SIGTERM
 1. stop accepting; drain TurnExecutor (11 §3.2)
 2. mcp.shutdown(grace):        # this spec
      for each client (stable order):
        send notifications/cancelled for in-flight calls
        close transport: close stdin, SIGTERM process group, wait grace, SIGKILL
        release AdapterScope (unregisters tools; generation bumps)
        state := Stopped
 3. close sessions; flush store; release lease (04 §3.5)
 4. stop transport; unlink socket (05)
```

- `McpManager::shutdown` is **idempotent** and **bounded**: `shutdown_grace`
  per server, not per call, and a server that ignores SIGTERM is SIGKILLed
  (M15, MCP-F13).
- Scopes are released even if a client hangs; unregistration does not depend on
  a clean MCP shutdown (M9).
- A crash of the daemon (no graceful path) leaves MCP children to the daemon's
  process-tree teardown (`07 §9.4`); they must not outlive it (M15).

### 5.4 Reconnect on server crash

A stdio server's death is detected as stdout EOF; an HTTP server's as a stream
close. The `McpClient` transitions `Disconnected` and `McpManager` schedules a
reconnect with exponential backoff + jitter:

```text
on disconnect:
  fail every in-flight call with McpError{TransportClosed}  -> Error{Io}
  state := Disconnected; emit live status
  if reconnect_max_attempts == 0 or attempts exhausted:
     state := Failed (terminal until daemon restart); emit live status
  else:
     sleep(base * 2^attempt, capped at max_backoff, ± jitter)   [timer, not a thread]
     state := Starting; recreate transport in the SAME client; handshake
     on success: listTools; AdapterScope::replace (atomic);
                 state := Ready, or Degraded if zero tools translated (§5.2)
     on failure: attempts++; schedule again
```

- **In-flight calls never survive a crash.** They fail fast with `Error{Io}`
  (MCP-F7); they are not retried (MCP-§4.6). The model sees the error and
  decides.
- **Client identity is stable across reconnect.** A reconnect recreates the
  **transport inside the existing `McpClient`**; it never constructs a new
  client (`McpClientFactory` is a startup-only seam). Every retained `McpTool`
  holds a `shared_ptr` to that same client, so a retained tool can never point
  at a replaced or dead client; `AdapterScope::replace` installs new `McpTool`s
  that share it. A new client object is built only at daemon startup (or after a
  config change, which requires a restart; §5.6).
- **Backoff is timer-driven** on the daemon's loop, never a dedicated thread
  (`04 §9`). `reconnect_attempt` resets to zero only after the connection has
  been continuously `Ready` for `reconnect_stable_window` **and** a `ping()` in
  that window returned `true`; a `false` ping is a disconnect and schedules the
  next backoff. A `ping()` never gates a `tools/call` (§4.3).
- **A reconnect that changes the tool set is atomic** (§5.5); a reconnect that
  yields the same set still bumps the generation (the snapshot is rebuilt).
- **A `Failed` server keeps its tools registered but non-callable** (state
  `Failed`), so the model does not silently lose a tool from its schema
  mid-turn; calls return `Error`. Unregistering a failed server's tools would
  change the schema under a running turn. (If the reviewer prefers
  unregistration, that is OQ-6.)

### 5.5 `notifications/tools/list_changed`

On `notifications/tools/list_changed` the manager runs `refresh(server)`:

1. `listTools` (bounded pagination), translate, filter (`allowed_tools` /
   `denied_tools`), skip collisions (`skipped_tools`).
2. Build the new `McpTool` set.
3. `AdapterScope::replace(new_tools)` — **atomic**, generation-bumping (M14).
4. Emit a live status event with the new tool count; if the new set is empty,
   the server becomes `Degraded` (non-callable, §5.2) and the empty replacement
   still bumps the generation.

The schema change is observed by the loop only at its next
`ContextAssembler::tools()` call (`06 §5.2`); a turn already in flight keeps the
schemas it started with. There is no attempt to mutate an in-flight turn
(`07 E-F15` invalidates the cache; the assembler re-reads).

### 5.6 Configuration and layering

MCP configuration follows the existing layered loader (`config.hpp`,
`00 §37`): built-in defaults → global `~/.config/ymh/config.toml` → project
`<root>/.ymh/config.toml` → `YMH_*` env → CLI. `Config` gains one section,
`McpSettings mcp;` (mapped onto `McpConfig` by the daemon wiring). Because the
loader is strict, the `[mcp]` keys are part of the known schema; a typo is a
`ConfigError`.

```toml
[mcp]
enabled = true
max_servers = 8
startup_deadline_ms = 5000

[[mcp.server]]
id = "github"
transport = "stdio"
command = "npx"
args = ["-y", "@modelcontextprotocol/server-github"]
env = ["GITHUB_PERSONAL_ACCESS_TOKEN=${GITHUB_TOKEN}"]
required = false
default_verdict = "ask"
call_timeout_ms = 60000

[[mcp.server]]
id = "fs"
transport = "stdio"
command = "mcp-server-filesystem"
args = ["."]
cwd = "."
allowed_tools = ["read_*", "list_*"]
denied_tools = ["write_*", "delete_*"]
default_verdict = "ask"

# Deferred (OQ-2); rejected unless allow_network_servers = true.
# [[mcp.server]]
# id = "remote"
# transport = "http_sse"
# url = "https://example.invalid/mcp"
# header_env = ["Authorization=Bearer ${REMOTE_MCP_TOKEN}"]
```

Rules:

- **Secrets live in environment variables, never in `Config`** (`config.hpp`,
  `08 §6.4`). `env`/`header_env` values are `${VAR}` references resolved at
  spawn time; the resolved values are never logged (M12, §6.5).
- **Layering follows `§37` and `09 §3.4` exactly — no MCP-specific merge.**
  MCP settings are ordinary layered config. On a tie a later layer wins, and
  `09 §3.4` orders `... Project > Global > Builtin`, so a project layer **can**
  set `default_verdict = "allow"` where the global layer said `ask` (`09 §3.4`
  property: "a workspace-local `allow` ... can override a global ... = ask").
  An earlier draft of this spec claimed the opposite; that claim is
  **withdrawn** because it contradicts `09 §3.4`/`§3.5`. MCP does not add a
  stricter merge rule — doing so would change `09`'s precedence fold. The
  residual malicious-workspace risk is real and is recorded as **OQ-13** for
  the `09`/`config.hpp` owners; if a cross-layer guard is wanted it must be
  specified in `09`, and this spec will then reference it.
- **Per-server default lowering (AM-3).** After the layered `Config` is
  resolved, the daemon config assembly lowers each enabled
  `McpServerConfig::default_verdict` into one `PermissionConfig::ToolDefault`
  (§6.1, §12.3); the fold and the broker are otherwise untouched.
- **Array semantics are pinned here (replace, not merge).** `§37` does not state
  array-merge behavior, so MCP pins it: `servers`, `args`, `env`, `header_env`,
  `allowed_tools`, and `denied_tools` are **replaced wholesale** by a later
  layer (no concatenation), matching the scalar last-wins rule. Consequence: a
  project layer that lists `allowed_tools` replaces the global list entirely
  (see OQ-13).
- **Config changes require a daemon restart** (`§54 D11`: no hot reload). There
  is no `mcp.reload` RPC in v1 (OQ-7).

---

## 6. Safety and policy

### 6.1 Permission gating per MCP tool

MCP tools are ordinary tools and therefore pass through the **same** mandatory
chain (`09 §6.1`, `§46`):

```text
LLM output → Tool parser → Permission policy → ToolRegistry → McpTool::execute → MCP server
```

- `09 §3.1`'s `PermissionPolicy::evaluate` classifies a `PermissionRequest`
  whose `tool` is `mcp.<server>.<tool>` and whose `arguments` are the model's
  arguments verbatim. The per-server `McpServerConfig::default_verdict` (default
  `Ask`) is lowered **once, at config assembly, by the daemon** — not by
  `McpManager` — into one `PermissionConfig::ToolDefault` (AM-3, §12.3):
  `prefix = "mcp.<server>."`, `verdict = default_verdict`,
  `id = "mcp.<server>.default"`. `evaluate` consults `tool_defaults` only at
  step 3 (NO MATCH) of the `09 §3.4` fold, so an operator's ordinary
  `[[permissions.rule]]` entry for `mcp.*` **always outranks the server
  default**; the default is a fallback, never a specificity-ranked rule. The MCP
  component adds no rule syntax, and `McpManager` never sees the policy.
- **MCP tools default to `Ask`.** There is no implicit `Allow` for a server
  (`09 §3.3`: "never `allow` by default"). A `trust = "allow"` server is an
  explicit operator decision.
- **No MCP-specific approval path.** The `PermissionBroker` (`11 §7.2`), the
  attention/background policy (`09 §5`, F2), first-wins, and fail-closed timeout
  all apply unchanged (M7).
- **`destructiveHint` is a hint only.** It may be surfaced in the request
  summary, but it never changes a verdict (`07 §3.2`).
- **Server-initiated sampling is not a bypass.** v1 advertises no client
  capabilities, so an MCP server cannot ask the model to do anything (§4.3).
  This closes the obvious "MCP server drives the agent" hole.

### 6.2 Path safety and the `resolve()` model

This is the sharpest safety question in the spec, and the answer is
**honesty**, mirroring `07` X16 (sandbox honesty):

- **`ExecutionEnvironment::resolve()` governs the daemon and its in-process
  tools; it does NOT constrain an MCP server's filesystem calls.** An MCP
  server is a separate process (or a remote endpoint). `ymh` cannot intercept
  its `open()`/`write()`. Claiming containment would be false.
- What `ymh` **does** enforce:
  1. **cwd rooting.** A stdio server is spawned with
     `cwd = ExecutionEnvironment::resolve(cwd)` (empty ⇒ `root()`), so a
     relative-path server starts rooted at the workspace. This is a *default*,
     not a jail.
  2. **argv-first spawn.** `command`/`args` are never concatenated into a shell
     string (`07 §16`), so a malicious tool argument cannot inject shell syntax
     into the spawn.
  3. **Sanitized environment.** The child inherits only the environment the
     config names (plus a minimal `PATH`), not the daemon's full environment.
  4. **Process-group ownership.** The child is in its own process group, so
     timeout/cancel/shutdown can kill its whole tree (`07 §9`, M15).
  5. **Permission gating.** The operator must approve (or pre-allow) every MCP
     tool call (§6.1), and can deny a server's mutating tools by glob.
  6. **Explicit opt-in.** No server is enabled by default; HTTP egress requires
     `allow_network_servers`.
- **MCP tools carry no resolved `path` in `PermissionRequest`.** The policy
  cannot match a `path` dimension for an arbitrary MCP argument, because the
  adapter has no generic way to know which JSON field is a path. `09 §3.2`'s
  `path` field is therefore empty for MCP tools; rules match on `tool` (and
  `command` is not applicable). An operator who wants path-scoped MCP rules must
  configure them on the *server* (`allowed_tools`/`denied_tools`) or use a
  server that exposes narrow tools. This is a documented limitation, not a
  silent one (M8, OQ-8).
- **`Unrestricted` mode does not make an MCP server safer or less safe.** It
  affects only `resolve()` for in-process tools; the server's confinement is
  whatever the server itself implements. The docs and the config schema must say
  so (`07` X16 style).

### 6.3 Timeouts

Every MCP operation is bounded; there is no unbounded wait anywhere:

| Operation | Bound | On expiry |
|---|---|---|
| transport start / spawn | `handshake_timeout` | `SpawnFailed`/`HandshakeTimeout` |
| `initialize` | `handshake_timeout` | `HandshakeTimeout` (MCP-F2) |
| `tools/list` (all pages) | `list_timeout` | `McpError{CallTimeout}` → `Failed` at startup; previous set retained on refresh (§6.3) |
| `tools/call` | `server.call_timeout` | `notifications/cancelled`; `Error{Timeout}` (MCP-F6) |
| whole startup (all servers) | `startup_deadline` | unsettled servers `Failed`, zero-tool servers `Degraded`; daemon proceeds (M6) |
| reconnect backoff | `reconnect_max_backoff` | bounded by attempts |
| shutdown per server | `shutdown_grace` | SIGKILL the group |

- Timeouts use the injected `ClockReader` (`11 §7.2`, E21), never
  `Clock::now()` directly, so tests advance a `ManualClock` instead of sleeping.
- A `tools/call` timeout sends `notifications/cancelled` **and** fails the call;
  it does not leave a zombie pending entry (MCP-F6).
- A `tools/list` timeout during `list_changed` leaves the previous tool set in
  place (the scope is only replaced on a fully successful list) (M14).

### 6.4 Cancellation (`turn/cancel`)

- `turn/cancel` (`06 §3.5`) fires the turn's `CancellationToken`. The in-flight
  `McpTool::execute` observes `ctx.cancellation()`:
  1. send `notifications/cancelled{requestId, reason:"cancelled"}`,
  2. drop the pending entry,
  3. throw `McpErrorCode::Cancelled` → `ToolOutcome::Cancelled` (M10).
- **A stdio server that ignores cancellation is not killed.** Cancellation
  abandons the call; the server process stays (it is daemon-lifetime). The
  manager does not SIGKILL a server for a per-call cancel — that would destroy
  unrelated calls and the handshake. Only shutdown and reconnect-kill touch the
  process group. (If a server is known to leak on cancel, the operator can
  shorten `call_timeout`.)
- **Cancellation is session-scoped.** Cancelling session A's turn never
  cancels session B's MCP calls, even on the same server (M10, `07` X12/F9).
  In-flight calls are tracked per `(server, ToolCallId)`; `notifications/
  cancelled` is sent only for the cancelled call's request id.
- Cancellation is **not** an error (`06 §3.5`): no `TurnFailed`, no live error.

### 6.5 Output truncation and redaction (`§40`)

- **Never log MCP arguments, results, prompts, or tool output** (`§40`,
  `09 §6.2`). The `mcp` log category may carry: server id, state transitions,
  method names, request ids, byte counts, durations, and bounded error tokens.
  It may **not** carry `arguments`, `content`, or `structuredContent`.
- **Error messages are redacted.** `McpServerStatus::last_error` is a bounded,
  machine-token-plus-short-human string; a server that echoes request data in
  its error text does not get that text into a log unredacted (`09 §6.2`
  no-log invariant).
- **Durable results are bounded** (§4.5) and clamped on the serialized payload
  size (`07 §5.2`, X10); images/audio/binary become placeholders, never base64.
- **Live progress** carries a progress token and numeric counts only; it never
  carries the tool's content.
- **`notifications/message`** from a server is logged at `mcp` category with
  its `data` redacted to a byte count unless the operator explicitly opts into
  verbose MCP logging (mirroring `LoggingSettings::log_prompts`, off by
  default).

### 6.6 Resource caps

MCP resources are bounded by `McpConfig` (proposed; see OQ-4 for whether any
should move to `04`'s `ResourceCaps`):

| Resource | Cap | Release |
|---|---|---|
| concurrent MCP servers | `max_servers` | server stops |
| in-flight `tools/call` per server | `max_inflight_calls_per_server` | call returns/cancels/times out |
| inbound frame | `max_frame_bytes` | per frame |
| projected result | `min(server.max_result_bytes, tools.tool_result_max_bytes)` | per call |
| `tools/list` pages | `list_max_pages` | per list |
| reconnect attempts | `reconnect_max_attempts` | per disconnect |

- **MCP server processes do NOT consume the tool-subprocess cap**
  (`ResourceCaps::max_global_subprocesses`, `04 §2.1`). They are long-lived
  daemon services, not per-call tools; charging them to the tool cap would
  starve tools whenever MCP is enabled. They are bounded by `max_servers`
  instead. This is a deliberate split (OQ-4).
- In-flight call slots are owned by each server's `McpClient` (a per-server
  counter, bounded by `max_inflight_calls_per_server`). `McpTool::execute`
  acquires one via `McpClient::tryAcquireCallSlot()` and releases it by RAII on
  **every** exit path — success, `McpError`, cancellation, exception (M11). The
  `ResourceGovernor` is **not** used: it has no MCP slot API (`04 §8`, `07 §7.1`),
  and `McpTool` holds no `McpManager` reference. A slot leak is a defect
  (MCP-F17).
- Cap exhaustion (`tryAcquireCallSlot()` returns `nullopt`) is a typed error
  (`Error{ResourceExhausted}`), never a silent queue (`07` E-F8 style, MCP-F12).

### 6.7 Security model (`§46`)

- **MCP servers are untrusted capability providers.** Enabling a server is a
  trust decision by the operator; the config is the trust boundary.
- The `§46` chain is not bypassed: the model cannot reach a server except
  through a `ToolCall` that passed the policy layer. There is no MCP tool that
  is pre-approved by construction.
- **HTTP servers are off by default** and require `allow_network_servers`;
  remote transport authentication (OAuth, mTLS) is deferred (OQ-2). v1 does not
  claim to secure remote MCP beyond the configured header env.
- **No dynamic server loading.** Servers come only from config; there is no
  `mcp.add` RPC, no discovery protocol, no plugin dlopen (`§54 D11`).

---

## 7. Concurrency and threading

MCP follows the **single-`io_context` model** (`04 §9`: "one main Asio loop per
daemon … no thread per session, no thread per client"; `07 §10`; `11 §3.2`). It
adds **no thread per server and no thread per call**.

- **Transports are async streams on the daemon's one loop.** A stdio child's
  stdout/stdin are registered as async descriptors (`ChildProcessHandle::
  readStdout`/`writeStdin`, §5.1) on the daemon `io_context`; the HTTP+SSE
  transport uses the same loop. Reads, writes, and timers are loop-affine; a
  blocking syscall is offloaded (`07 §10`), never run on the loop thread.
- **Responses and notifications are dispatched on the daemon loop**, in arrival
  order; the pending-request map is guarded by a mutex.
- **`tools/call` runs on the turn executor** (the `TurnExecutor` worker,
  `11 §3.2`), never on the io loop. It posts the request and awaits a `Task`
  resolved on the loop. This mirrors the `PermissionBroker::resolve` pattern
  (`11 §7.2`): never block the io loop.
- **The transport path never runs tool logic or permission logic.** It only
  frames, pairs, and enqueues.
- **`McpManager` state** (`slots_`, states, reconnect timers) is owned by the
  daemon loop; `statuses()` is a snapshot safe for the UI/transport paths to
  read. State transitions are posted to the daemon loop; no lock is held across
  a `replace()`.
- **`AdapterScope::replace`** is serialized by the registry's own write
  discipline; a concurrent `schemas()` sees a whole generation (M14).
- **No thread per server and no thread per call:** transport I/O is async on the
  shared loop and calls are tasks on the existing turn executor. `max_servers`
  bounds concurrent transports, not threads.

---

## 8. Invariants

Numbered, testable, cited. Any code that can violate one is a defect.

**M1 — Adapter transparency.** MCP tools are ordinary `Tool`s reached only
through `ToolRegistry`; no component in the agent/loop/provider path branches on
an MCP origin, and `ToolSchema` carries no origin field. (`00 §27`, D8)

**M2 — One registry, one path.** Every MCP tool is registered in the single
`ToolRegistry`; there is no side channel from the model to a server. All calls
flow `ToolCall → PermissionDecision → ToolResult`. (`§46`, `06 §5.5`, M7)

**M3 — Namespace uniqueness.** Every MCP tool name is `mcp.<server>.<tool>`
with `server` unique and `tool` sanitized; an adapter may never shadow a
non-adapter tool, and two adapters may not share a prefix. Collisions are
skipped or fail loud, never silently renamed. (`§3.2`, MCP-F8)

**M4 — Daemon ownership.** Exactly one `McpManager` per daemon (or per
single-process headless run); the supervisor/TUI owns no MCP client. MCP
servers are daemon-lifetime and survive TUI detach. (`04 §3.7`, `11 §11.2`,
D23)

**M5 — Handshake before use.** No `tools/list` or `tools/call` is issued before
`initialize` succeeds and `notifications/initialized` is sent; no tool is
registered before `tools/list`. (`§4.3`, `§5.2`)

**M6 — Bounded startup.** MCP discovery cannot block daemon startup beyond
`startup_deadline`. For `required = false`, a server with zero usable tools is
`Degraded` (or `Failed` on failure) and the daemon starts; for `required = true`,
any outcome other than `Ready` with ≥ 1 translated tool — including zero usable
tools (`Degraded`) — is `StartupRejected` (§5.2 table). (`04 §3.3` + AM-2,
`11 §12.6`, MCP-F2)

**M7 — Permission before execution.** Every MCP call records a
`PermissionDecision` before `McpTool::execute`; the default verdict for MCP
tools is never `Allow` unless configured, and it enters the policy only as the
NO-MATCH fallback AM-3. (`§6.1`, `07` X7, `09 §3.1`, `§3.4` + AM-3)

**M8 — Path-safety honesty.** `resolve()` does not constrain an MCP server's
filesystem access; stdio servers are rooted by `cwd` and argv-first spawn, and
MCP tools carry no resolved `path` in `PermissionRequest`. No doc, schema, or
log claims containment. (`§6.2`, `07` X16)

**M9 — Fail-closed unless `Ready`.** `Ready` is the only callable state. A call
reaching any other state (`Degraded`/`Disconnected`/`Failed`/`Stopped`) returns
`Error` immediately; it never blocks waiting for readiness and never hangs.
`Degraded` holds zero tools, so no `McpTool` can reach it; `Failed` retains its
schema entry but fails closed. (`§4.3`, `§5.2`, `§5.4`, MCP-F7)

**M10 — Cancellation propagation.** Cancelling a turn sends
`notifications/cancelled` for exactly that call and yields `Cancelled`; no
other session or call is affected. A server that ignores cancel is not killed.
(`§6.4`, `07` X12, F9)

**M11 — Bounded I/O and slots.** Inbound frames, results, and pagination are
bounded; a call holds an in-flight slot released on every exit path; oversized
input is truncated or rejected, never buffered unbounded. (`§6.5`, `§6.6`,
MCP-F10)

**M12 — Redaction.** MCP arguments, content, and structured results are never
written to normal logs; only names, ids, byte counts, and durations are.
(`§6.5`, `§40`, `09 §6.2`)

**M13 — Deterministic registration.** For a fixed config and a fixed server
`tools/list`, the registered names and schemas are ordered and byte-stable for a
generation. (`§4.8`, `07` X14, `§44`)

**M14 — Atomic namespace replacement.** A reconnect or `list_changed` replaces a
server's tool set atomically and bumps the registry generation; a concurrent
`schema()` reader sees old or new, never a mix. A failed list leaves the old set
in place. (`§4.8`, `§5.5`, `07` E-F15)

**M15 — Process ownership.** Every stdio MCP server is a child of the daemon in
its own process group; it is reaped by the daemon's process layer and cannot
outlive the daemon. (`07 §9`, `11` E8, MCP-F13)

**M16 — No UI dependency.** The `mcp/` targets never include or reference UI
types. (`§4.1`, `§20.1`, `07` X15, D15)

---

## 9. Failure modes

### 9.1 Shared findings (F1–F12, `§54`)

| F# | Finding | MCP handling |
|---|---|---|
| **F1** | path/process isolation | stdio servers are daemon children, own group, `cwd=resolve(root)`; containment is **not** claimed (§6.2, M8, M15) |
| **F2** | background permission | MCP `Ask` is surfaced by attention and fail-closed on timeout via the unchanged broker (`09 §5`, M7) |
| **F3** | late event after close | MCP emits live events best-effort; the durable result is appended by the loop, so no server-written late durable event (M2) |
| **F4** | edge-triggered attention | the manager emits live status events; attention stays loop/UI-owned (`§4.7`) |
| **F5** | output ring buffers | MCP results are bounded and clamped; no unbounded materialization (§6.5, M11) |
| **F6** | input/keybinding focus | out of scope; MCP never sees focus |
| **F7** | per-session dirty flags | out of scope; status is per-server, not per-session |
| **F8** | resource caps | `max_servers` / `max_inflight_calls_per_server` (§6.6); server processes are not tool subprocesses |
| **F9** | cancellation scoping | per-call `notifications/cancelled`; session-scoped (M10) |
| **F10** | resume-suspended | MCP clients are daemon-lifetime; resuming a session never starts a server (`§5.1`) |
| **F11** | subagent ID duality | a subagent shares the daemon's clients; its MCP calls carry its own `ToolCallId` (M10) |
| **F12** | flash clock in model | out of scope; MCP emits no flash state |

**Explicitly out of scope:** F6, F7, F12 (UI projection), F4's flash arithmetic
(spec 10), F2's policy rules (spec 09), F10 (spec 06).

### 9.2 Component-local failure modes (`MCP-F#`)

These are component-local to the MCP adapter and must be covered by tests
(§11.6). **Namespace note:** `11 §15` already owns `M-F1`–`M-F12` (the M2
errata failure modes). To avoid a collision, this spec uses the
component-prefixed namespace **`MCP-F#`**, consistent with `07`'s `E-F#`, `05`'s
`T-F#`, `04`'s `D-F#`, `09`'s `Q-F#`, and `06`'s `A-F#`. The task brief's
shorthand "`M-F#`" is read as `MCP-F#`; this is recorded as OQ-1.

| MCP-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **MCP-F1** | Server spawn failure | `spawn()` throws / exec fails | `McpError{SpawnFailed}`; server `Failed`; tools absent or non-callable; `required` ⇒ startup failure (M6) |
| **MCP-F2** | Handshake timeout | no `initialize` response within `handshake_timeout` | `HandshakeTimeout`; server `Failed` (terminal); daemon proceeds; live status (M6, §5.2) |
| **MCP-F3** | Protocol/revision mismatch | echoed `protocolVersion` unsupported / `initialize` error | `HandshakeRejected`; server `Failed`; never proceed on a guess |
| **MCP-F4** | Schema incompatibility | `translate()` returns `SchemaIncompatible` | skip that tool; record in `skipped_tools`; server stays `Ready` while ≥ 1 tool translated, else `Degraded` (§5.2) |
| **MCP-F5** | Tool call JSON-RPC error | response has `error` | `McpError{RpcError}` → `Error{Internal}`; message redacted (§6.5) |
| **MCP-F6** | Tool call timeout | `call_timeout` fires | send `notifications/cancelled`; `Error{Timeout}`; slot released (M11) |
| **MCP-F7** | Server crash mid-call | stdout EOF / stream close | in-flight calls `Error{Io}`; `Disconnected`; backoff reconnect (M9, §5.4) |
| **MCP-F8** | Duplicate tool name | sanitization collision / local shadow | skip + `skipped_tools`; local shadow fails loud (M3) |
| **MCP-F9** | Cancellation mid-call | token fired | `notifications/cancelled`; `Cancelled`; server not killed (M10) |
| **MCP-F10** | Oversized output | frame > `max_frame_bytes` or result > `max_result_bytes` | reject frame (`ResultTooLarge`) or clamp + `truncated`; never OOM (M11) |
| **MCP-F11** | Malformed / unknown message | bad JSON, unknown id, missing result+error | drop; log at `mcp` (redacted); pending call times out normally (§4.3) |
| **MCP-F12** | Cap exhaustion | `max_servers` / in-flight cap | `Error{ResourceExhausted}` / server refused at startup; typed, never silent (§6.6) |
| **MCP-F13** | Orphaned / zombie server | process-tree teardown / `waitpid` | process-group kill + specific-pid reap; no orphan outlives the daemon (M15) |
| **MCP-F14** | Config error | strict loader / validation | `ConfigError` at startup; never a runtime bypass (§4.1) |
| **MCP-F15** | Non-text result content | content block type unknown | typed placeholder in `output`; base64 never dumped (§4.5) |
| **MCP-F16** | Adapter internal exception | `catch (...)` at `McpTool::execute` | `Error{Internal}`; the loop continues (`07` E-F16, `06` A-F5) |
| **MCP-F17** | Slot leak | in-flight counter mismatch | defect; assert in debug; release on every path (M11) |
| **MCP-F18** | Status event storm | repeated state flapping | coalesce status transitions per server (like `07 §8.2`); never a live flood |

---

## 10. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (`§55`). The MCP adapter maps onto
it as follows.

| dsh concept | ymh MCP adapter | Reference |
|---|---|---|
| MCP tool integration | `McpManager` → `McpTool` → `ToolRegistry` | `00 §27`, D8 |
| Tool registry | `ToolRegistry` (+ additive `AdapterScope`) | `07 §4`, §4.8 |
| Tool | `McpTool : Tool` | `07 §3.1`, §4.6 |
| Tool invocation context | `ToolContext` (unchanged) | `07 §5`, §4.6 |
| Execution world / capability seam | `ExecutionEnvironment` (stdio spawn root) | `§18`, §5.1/§6.2 |
| Subprocess provider | `ProcessService` (+ additive `spawn`) | `07 §6.4`, §5.1 |
| Approvals | `PermissionPolicy` / `PermissionBroker` (+AM-3 NO-MATCH tier) | `09 §3`, §6.1 |
| Append-only trace | loop appends `ToolCall`/`ToolResult`; adapter never appends | D2, `01 §4.5`, M2 |
| Plugin lifecycle | static config only; no dynamic server loading | `§7.1`, D11, §6.7 |

**Deliberate omissions** (accepted for v1): no MCP resources/prompts/sampling
(§1.3, OQ-5), no HTTP+SSE transport (OQ-2), no reverse adapter (ymh as an MCP
server), no dynamic server discovery, no per-session lazy server start (OQ-4),
no MCP-specific approval UI, no tool-level path confinement for server I/O
(§6.2).

---

## 11. Test plan

Strategy is `§44`: unit, integration (fake MCP server over stdio / `FakeLLM`),
golden, replay, and a separate live opt-in layer. The deterministic layers run
offline; the live layer is opt-in and gated (`§44`, `§45`).

### 11.1 Unit tests

- **Name translation**
  - `sanitize()` table: uppercase → lower; `-`/`.`/space → `_`; leading digit
    → `m_`; collapse/trim `_`; empty → `tool`; unicode → `_`.
  - full name matches the `ToolName` grammar for a fuzz corpus of remote names.
  - collision within a server → first wins, second in `skipped_tools` (MCP-F8).
  - local shadow (`mcp.foo.bar` already registered) → fail-loud (M3).
- **Schema translation**
  - a supported schema maps byte-stable; `required`, `enum`, `default`,
    `items`, nested objects preserved.
  - `additionalProperties: true` → `SchemaIncompatible`; absent/false → false.
  - `$ref`/`anyOf`/`oneOf`/union `type`/`patternProperties`/`const` → skipped
    (MCP-F4).
  - description over `tool_description_max_bytes` is truncated and recorded.
  - `annotations.destructiveHint` → `ToolSchema.destructive`.
- **Result mapping**
  - text-only; multi-block join order; image/audio/resource → typed placeholder;
    `structuredContent` fenced; unknown block → placeholder (MCP-F15).
  - `isError:true` → `Error{Internal}` with text preserved (MCP-F5).
  - JSON-RPC error object → `Error{Internal}` (MCP-F5).
  - oversized result → clamp + `truncated`; oversized frame → `ResultTooLarge`
    (M11, MCP-F10).
- **Config**
  - strict loader rejects unknown `[mcp]` keys, bad `McpServerId`, duplicate
    ids, missing `command`, `HttpSse` without `allow_network_servers`,
    `max_result_bytes > tool_result_max_bytes` (MCP-F14).
  - project-layer override follows `09 §3.4` (later layer wins); array values
    replace wholesale, not concatenated (§5.6).
  - `${ENV}` resolution; a missing variable is a startup error; values never
    logged (M12).
- **Client state machine (scripted transport)**
  - handshake before list/call (M5); call before `Ready` → `Error` (M9).
  - request/response pairing; unknown id dropped (MCP-F11).
  - cancellation sends `notifications/cancelled` exactly once (M10).
  - timeout uses the injected `ClockReader` (`11` E21); no sleep.
  - reconnect backoff schedule with a `ManualClock`: attempts, cap, jitter
    bounds, reset after `Ready` (§5.4).
- **`AdapterScope`**
  - `openAdapter` before freeze; `replace` after freeze; ordinary `add` after
    freeze still throws `RegistryFrozen` (M14).
  - `replace` is atomic (a concurrent reader sees old or new) and bumps
    `generation()`; a failed `replace` retains the old set (strong guarantee).
  - prefix violation / schema violation / local shadow throws and does not
    mutate.
  - `~AdapterScope` unregisters and bumps the generation.

### 11.2 Fake MCP server over stdio (integration)

Two fakes, mirroring `§45`'s Fake LLM discipline:

- **`FakeMcpTransport` (in-process).** A scripted `McpTransport` that replays a
  list of server messages and records client messages. Backs the unit state
  machine tests above; no process, no I/O.
- **`fake_mcp_server` (real subprocess).** A small test binary that speaks
  newline-delimited JSON-RPC over stdio, driven by a scenario file. Scenarios:
  - `happy`: initialize → `tools/list` (2 pages) → `tools/call` text success.
  - `is_error`: `tools/call` returns `isError:true`.
  - `rpc_error`: `tools/call` returns a JSON-RPC error.
  - `hang`: never answers `initialize` (handshake timeout).
  - `slow_call`: answers `tools/call` only after the timeout (cancellation).
  - `crash_mid_call`: exits after receiving `tools/call`.
  - `huge`: returns a frame over `max_frame_bytes`.
  - `list_changed`: sends `notifications/tools/list_changed` mid-session.
  - `bad_schema`: advertises an unsupported schema (skipped tool).
  - `flood`: emits many `notifications/message`/`progress` (coalescing).
  - `ignore_cancel`: ignores `notifications/cancelled` (timeout fallback).
- **Integration assertions**
  - **Full pipeline.** `FakeLLM` scripts a call to `mcp.echo.echo`; the durable
    sequence is `ToolCall → PermissionDecision → ToolResult` with id pairing
    preserved (M2/M7).
  - **Permission paths.** `Ask` resolves via the broker; `Deny` yields
    `ToolResult{Denied}` and the server never receives `tools/call` (M7).
  - **Cancellation.** Cancel mid-call sends `notifications/cancelled` and yields
    `Cancelled`; a second session's concurrent call is untouched (M10).
  - **Crash + reconnect.** Kill the server mid-call; the call is `Error{Io}`;
    the manager reconnects and `replace`s the namespace; a later call succeeds
    (M9/M14).
  - **`list_changed`.** After the notification, a newly advertised tool appears
    in `schemas()` and a removed one disappears (M14).
  - **Startup degradation.** A `hang` server does not block startup beyond the
    deadline; a `required` `hang` server fails startup with `StartupRejected`
    (M6).
  - **Cap exhaustion.** Saturate `max_inflight_calls_per_server`; the next call
    is `Error{ResourceExhausted}` (MCP-F12).
  - **Shutdown.** Daemon drain closes stdin, SIGTERMs, and SIGKILLs a
    `ignore_sigterm` scenario within grace; no child survives (M15, MCP-F13).

### 11.3 Golden tests

- **Registered schema set.** A byte-stable golden of `ToolRegistry::schemas()`
  for a fixture server (ordering, translated names, translated schemas) (M13).
- **Event stream.** Golden `ToolCall`/`PermissionDecision`/`ToolResult` for a
  scripted MCP turn, including `Error` and `Cancelled` fixtures.
- **Status transitions.** Golden sequence of `payload::McpServerStatusChanged`
  live events and their `HostNoticeKind::McpServerStatus` projections (AM-1)
  for a start → ready → disconnect → reconnect scenario.

### 11.4 Replay tests

- Record a `FakeLLM`-driven session containing MCP tool calls; `deriveMessages`
  over the log reproduces the message list exactly with **no MCP server
  constructed and no `McpClient`** (§3.3, `§44`).
- A replayed MCP tool call is never re-executed; the projection synthesizes
  results for unmatched `tool_use` blocks on cancel/failure (`01` I12).

### 11.5 Live end-to-end tests (real LLM, real MCP server, opt-in, `§44`)

- **Gated** by `YMH_LIVE_MCP=1` plus a configured server; skipped (not failed)
  when absent, so the default suite stays hermetic.
- Spawn the real `ymh` binary under a PTY; drive a prompt that must call an MCP
  tool (e.g. a filesystem server's `read_file`); assert observable behavior:
  session created, tool call rendered, permission prompt handled, durable events
  emitted, session resumes.
- **Tolerant of model nondeterminism**: assert structure/invariants (the MCP
  tool was invoked, a `ToolResult` was appended, the server state is `Ready`),
  never exact prose.
- Runs as a separate CI stage, never part of the fast default command.

### 11.6 Failure-mode coverage matrix

| MCP-F# | Covered by |
|---|---|
| MCP-F1 | integration `spawn` failure (bad command) |
| MCP-F2 | integration `hang` scenario + unit ManualClock |
| MCP-F3 | unit revision negotiation table; integration `bad_revision` |
| MCP-F4 | unit schema table; integration `bad_schema` |
| MCP-F5 | unit result mapping; integration `rpc_error` |
| MCP-F6 | integration `slow_call` + unit ManualClock timeout |
| MCP-F7 | integration `crash_mid_call` |
| MCP-F8 | unit name-collision + local-shadow |
| MCP-F9 | integration cancellation; unit `notifications/cancelled` |
| MCP-F10 | unit oversized result/frame |
| MCP-F11 | unit malformed/unknown-id drop |
| MCP-F12 | integration cap exhaustion |
| MCP-F13 | integration shutdown `ignore_sigterm`; crash/orphan tests |
| MCP-F14 | unit config table |
| MCP-F15 | unit non-text content |
| MCP-F16 | unit exception mapping |
| MCP-F17 | unit slot release on every path (assert counter) |
| MCP-F18 | unit status coalescing at a fixed clock |

### 11.7 Invariant coverage

| Invariant | Covered by |
|---|---|
| M1 | static check: no `mcp`/origin branch in agent/loop/provider; integration schema set |
| M2 | integration full pipeline; static check: no direct server call path |
| M3 | unit name translation + shadow; integration duplicate |
| M4 | integration daemon ownership; headless parity test |
| M5 | unit state machine; integration call-before-ready |
| M6 | integration `hang` + `required`; startup deadline |
| M7 | integration permission paths (Allow/Deny/Ask) |
| M8 | unit spawn `cwd` resolution; docs check (no containment claim) |
| M9 | unit call-before-ready; integration crash |
| M10 | integration cancellation scoping (two sessions) |
| M11 | unit caps/slots/oversize; integration cap exhaustion |
| M12 | unit redaction; log-capture assertion |
| M13 | golden schema ordering |
| M14 | unit `AdapterScope::replace` atomicity; integration `list_changed` |
| M15 | integration shutdown/reap; crash/orphan tests |
| M16 | static check: no UI include |

---

## 12. Decisions and open questions

### 12.1 Decisions (pinned by this spec)

- **(a) MCP is a tool producer, not a runtime.** It manufactures `Tool`s and
  registers them; it adds no loop, no agent, no durable event, no approval path
  (`§1.1`, `§3.3`, `00 §27`, D8).
- **(b) The provider-visible name is `mcp.<server>.<tool>`.** Server ids are
  config-authored and validated; tool names are sanitized deterministically;
  collisions are skipped (never renamed) and local shadows fail loud (`§3.2`,
  M3).
- **(c) The workspace daemon owns MCP client lifetime.** Servers are
  daemon-lifetime, survive TUI detach, and are stopped only on daemon drain
  (`§5.1`, M4, `04 §3.7`, D23).
- **(d) stdio is the v1 transport; HTTP+SSE is deferred.** The `McpClient`
  contract is transport-agnostic so the HTTP transport is additive (OQ-2).
- **(e) MCP stdio framing is newline-delimited JSON, not `05`'s length-prefixed
  framing.** `05` is a different protocol on a different wire; only the
  JSON-RPC message conventions are shared (`§2.3`).
- **(f) Schema translation is a downcast with an explicit allowlist.** An
  unsupported remote schema skips that tool, never the server; tightening
  `additionalProperties` from absent to `false` is a documented loss (`§4.4`).
- **(g) `isError:true` maps to `Error`, unlike a subprocess non-zero exit.**
  MCP's `isError` is an explicit tool-failure signal, not a process exit code
  (`§4.5`, contrast `07` decision (h)).
- **(h) Images/audio/binary become typed placeholders; base64 is never
  materialized or logged** (`§4.5`, `§6.5`, M12).
- **(i) MCP tools default to `Ask`.** There is no implicit allow; `destructive`
  is a hint only; the existing policy/broker flow is unchanged apart from the
  additive NO-MATCH default tier AM-3 (`§6.1`, M7, §12.3).
- **(j) Path containment is not claimed for MCP servers.** `resolve()` roots the
  spawn cwd only; the server's own I/O is outside `ymh`'s confinement. This is
  documented, not hidden (`§6.2`, M8, `07` X16 style).
- **(k) Server processes are capped by `max_servers`, not by the tool
  subprocess cap.** Long-lived services must not starve per-call tools
  (`§6.6`, OQ-4).
- **(l) Timeouts and backoff use the injected `ClockReader`** (`11` E21), so
  timeout/reconnect tests are deterministic (`§6.3`, §5.4).
- **(m) Cancellation abandons a call but does not kill the server.**
  Session-scoped `notifications/cancelled`; only shutdown/reconnect touch the
  process group (`§6.4`, M10).
- **(n) A reconnect or `list_changed` replaces a server's namespace
  atomically.** A failed list leaves the old set; a generation bump invalidates
  the schema cache (`§4.8`, §5.5, M14).
- **(o) A `Failed` server keeps its tools registered but non-callable** so a
  running turn's schema does not change under it; calls return `Error`
  (`§5.4`, OQ-6).
- **(p) MCP status is live-only and daemon-scoped.** The payload
  `payload::McpServerStatusChanged` is delivered as a live event and projected
  as `HostNoticeKind::McpServerStatus` (AM-1); it is never durable, so replay
  needs no server (`§4.7`, `§3.3`, M2). Fallback if AM-1 is rejected: emit the
  existing live `Progress` event with a bounded status token and keep
  `statuses()` as the only structured snapshot.
- **(q) Configuration is layered and strict.** `[mcp]` is part of the known
  schema; layering follows `§37`/`09 §3.4` (later layer wins; OQ-13); arrays
  replace wholesale; secrets are env references, never logged (`§5.6`, M12,
  `config.hpp`).
- **(r) There is no hot reload and no dynamic server loading.** Config changes
  require a daemon restart (`§54 D11`, §6.7, OQ-7).
- **(s) Two additive `07` seams are required:** `ToolRegistry::AdapterScope`
  (`§4.8`) and `ProcessService::spawn` (`§5.1`). Both are additive; both are
  OQ-3 (AM-0a/AM-0b in §12.3).
- **(t) The `M-F#` namespace is taken by `11 §15`; this spec uses `MCP-F#`.**
  Invariants use `M1…` (free) (`§9.2`).
- **(u) `initialize.instructions` is stored in status but not injected into the
  model context in v1.** Injecting server-authored text into the system prompt
  is a context-assembly change owned by `06`; it is deferred (OQ-9).
- **(v) `allowed_tools`/`denied_tools` match the RAW remote name**, not the
  sanitized `ymh` name, so config authors reason about the server's own
  vocabulary (`§4.1`).
- **(w) Server-initiated requests are answered `MethodNotFound`.** v1 advertises
  no client capabilities; an MCP server cannot drive the model (`§4.3`, §6.1).
- **(x) MCP adds no thread.** Transports are async on the daemon's single
  `io_context`; calls run on the turn executor. Never block the daemon io loop
  (`§7`, `04 §9`, `07 §10`, `11 §3.2`, `11 §7.2` pattern).
- **(y) MCP results are clamped on the serialized payload size**, so an MCP
  result can never raise `PayloadTooLarge` (`§4.5`, `07` X10, M11).
- **(z) `ymh run` (headless) gets the same `McpManager`** as the daemon path;
  there is no MCP-only-in-daemon behavior (`§5.1`, M4).
- **(aa) A `tools/list` timeout during `list_changed` is non-destructive**: the
  previous namespace is retained (`§6.3`, M14).
- **(ab) Backoff is timer-driven on the daemon loop, never a thread per
  reconnect** (`§5.4`, `04 §9`).
- **(ac) Reconnect attempts reset after a stable `Ready` window**
  (`reconnect_stable_window`) **and** a successful `ping()`; a `false` ping is a
  disconnect and does not reset (`§5.4`, §4.3).
- **(ad) Conservative fallback if either `07` amendment is rejected:**
  discovery happens once before `freeze()`, `tools/list_changed` is ignored, and
  a reconnect that changes the set is deferred to a daemon restart. This keeps
  v1 implementable without touching `07`, at the cost of runtime tool-set
  changes (OQ-3).
- **(ae) MCP config layering follows `§37`/`09 §3.4` exactly; there is no
  MCP-specific tightening rule.** A project layer can override a global
  `default_verdict` (09 §3.4); the malicious-workspace risk is OQ-13, not a
  silent divergence (`§5.6`).

### 12.2 Open questions

The required cross-spec amendments are enumerated separately in **§12.3**.

- **OQ-1 — Failure-mode namespace.** `11 §15` already uses `M-F1`–`M-F12`. This
  spec uses `MCP-F#` to avoid the collision; the task brief's `M-F#` shorthand
  is read as `MCP-F#`. Confirm the convention at the next re-verification.
- **OQ-2 — HTTP+SSE shape and remote auth.** Streamable HTTP vs. the older
  HTTP+SSE; `Mcp-Session-Id` handling; OAuth/mTLS. v1 ships stdio only and
  rejects `http_sse` unless `allow_network_servers`. The exact HTTP contract is
  deferred.
- **OQ-3 — The two additive `07` seams (AM-0a/AM-0b).** `ToolRegistry::
  AdapterScope` (§4.8) and `ProcessService::spawn` (§5.1) are required for the
  design as written. They are additive (no existing signature changes) but amend
  verified `07`. Approve, or apply fallback (ad). See §12.3.
- **OQ-4 — Where do MCP caps live?** Proposed in `McpConfig` (`max_servers`,
  `max_inflight_calls_per_server`) rather than `04`'s `ResourceCaps`, because
  they are not tool subprocesses. Confirm with `04`/`07` owners.
- **OQ-5 — Non-tool MCP primitives.** `resources/*`, `prompts/*`, `sampling/*`,
  `roots/*`, and elicitation are out of scope. If `resources` is later exposed
  as a tool (`mcp.<server>.read_resource`), it reuses this adapter's translation
  and result mapping.
- **OQ-6 — Failed-server tool visibility.** This spec keeps a `Failed` server's
  tools registered but non-callable (o) to keep a running turn's schema stable.
  The alternative — unregister on terminal failure — would change `schemas()`
  mid-turn. Confirm.
- **OQ-7 — MCP config reload.** No hot reload in v1 (`§54 D11`). A future
  `mcp.reload` RPC would have to reconcile scopes, in-flight calls, and the
  registry generation; deferred.
- **OQ-8 — Path-scoped MCP permission rules.** MCP tools carry no resolved
  `path`, so `09 §3.2`'s `path` dimension is unusable for them. A future
  per-server argument-to-path mapping (`path_args = ["path"]`) could restore it;
  deferred.
- **OQ-9 — `initialize.instructions` injection.** Stored but not injected (u).
  Whether/how to add it as a bounded context section is a `06` context-assembly
  question.
- **OQ-10 — Server-initiated progress token plumbing.** Progress notifications
  require a `_meta.progressToken` on `tools/call`; v1 emits them as live
  progress best-effort. Whether progress should be durable is a `01`/`10`
  question.
- **OQ-11 — Protocol revision set.** The supported `protocolVersion` set is
  pinned at implementation time; this spec names `2025-06-18` / `2025-03-26` as
  the initial set. New revisions must be added deliberately, with a test.
- **OQ-12 — Lazy per-session server start.** v1 starts all enabled servers at
  daemon startup (M4). A daemon hosting many workspaces pays `max_servers`
  processes each; lazy start/stop is a future optimization, deferred.
- **OQ-13 — Cross-layer MCP policy tightening.** §5.6 follows `09 §3.4`
  (Project > Global), so a project config can weaken a global MCP
  `default_verdict` (e.g. set `allow` over a global `ask`). An earlier draft
  claimed MCP forbids this; that claim is withdrawn because it contradicts
  `09 §3.4`/`§3.5`. Decide with the `09`/`config.hpp` owners whether a
  cross-layer guard is wanted; if so it belongs in `09`, not here. The same
  question applies to array replace-vs-concatenate semantics (§5.6).

### 12.3 Amendment ledger (required cross-spec changes)

This spec cannot be implemented as written without the following changes to
already-verified specs. None may be silently assumed: each is an explicit gate
item for the next re-verification pass. (OQ-3 covers AM-0a/AM-0b.)

**AM-0a — `07` `ToolRegistry::AdapterScope` (§4.8).** Additive: a new nested
`AdapterScope` type plus `openAdapter()` on `ToolRegistry` (`07 §4.1`). No
existing signature changes; `add`/`remove` stay frozen after `freeze()`.
Fallback if rejected: §12.1(ad).

**AM-0b — `07` streaming `ProcessService::spawn` (§5.1).** Additive: a new
`spawn()` method plus `ChildProcessHandle` (`07 §6.4`). No existing signature
changes. Fallback if rejected: §12.1(ad) (no long-lived stdio child ⇒ no stdio
transport).

**AM-1 — live MCP status + supervisor projection (§4.7).** Four coordinated
edits; the spec's pinned choice is to add a new live event *and* a host notice:
  - `00 §8.1` — add `McpServerStatusChanged` to the **live** set. `00 §8.1`'s
    existing errata explicitly covers only the **durable** set (its lines
    592–597); the live set has no errata, so this amendment must also state the
    rule for extending the live set (mirroring the durable errata).
  - `01 §4.1`/`§4.3` — add the live event to the live-event list and pin its
    `EventType`/routing. **Routing caveat:** MCP status is daemon/workspace-
    scoped and has no `SessionId`, so it must be delivered to **global
    subscribers only**, not through a per-session mailbox (`00 §8.3`); the
    amendment must say so explicitly (a session-less live event, or a
    host-scoped live-event variant).
  - `05 §5.3` + `§2.1` — add `HostNoticeKind::McpServerStatus` and its wire
    string (`"mcp_server_status"`). `05 §2.1`'s `wire_name()`/`parse_*()` are
    total-and-rejecting, so the enum and the wire-string table must be extended
    together or the string is rejected as `InvalidParams`. No new `HostNotice`
    field is needed: the notice reuses `HostNotice.detail` for a bounded status
    token (§4.7).
  - `10 §5.2` — `UiEventAdapter::onHostNotice` must handle the new kind and
    project it into the status model (no signature change; a new branch).
  Fallback if rejected: §12.1(p) — reuse the existing live `Progress` event with
  a bounded status token, and drop the new `HostNoticeKind` (status is then
  readable only via `McpManager::statuses()`).

**AM-2 — `04 §3.3` startup order (§5.2).** `04 §3.3` step 6 has **no**
`ToolRegistry` construction and **no** `freeze()` (the string `ToolRegistry`
does not appear in `04` at all). Add: construct `ToolRegistry`, register local
tools (`07 §4.3`), construct `McpManager`, run async discovery (6a), and
`freeze()` after discovery settles (6b). Without this amendment MCP cannot
register a single tool and the component is unimplementable as written.

**AM-3 — `09 §3.3`/`§3.4` NO-MATCH default tier for per-server verdicts
(§6.1).** Additive. `PermissionConfig` (`09 §3.3`) gains one field:
  - `std::vector<ToolDefault> tool_defaults;` where
    `struct ToolDefault { std::string prefix; PolicyVerdict verdict;
    std::string id; };` — `prefix` is a **literal** tool-name prefix (e.g.
    `"mcp.github."`), `verdict` is `Allow | Ask | Deny`, `id` is provenance
    (e.g. `"mcp.github.default"`).
  - `09 §3.4` step 3 (NO MATCH) becomes: if some `tool_defaults.prefix` is a
    literal prefix of `request.tool`, return the verdict of the **longest**
    such prefix; otherwise return `config.default_verdict`.
  **Why not a synthesized `PolicyRule`** with `tool = "mcp.<server>.*"`: the
  `§3.4` fold is **specificity-first**, so that glob would outrank an operator's
  broader `mcp.*` rule and could never be relaxed — violating "applies when no
  explicit rule matches". A dedicated NO-MATCH tier keeps `evaluate` pure and
  total and leaves the rule fold and `PermissionPolicy`'s interface untouched;
  only `PermissionConfig` gains the field and §3.4 gains the step.
  Fallback if rejected: drop `McpServerConfig::default_verdict` and require
  explicit `[[permissions.rule]]` entries for `mcp.*`.

**Not amended (deliberately).** `00 §8.3`'s per-session mailbox contract and
`01`'s durable `EventType` are **not** changed: no durable MCP event exists and
MCP status is not session-scoped. `09`'s precedence fold is **not** changed
(§5.6, OQ-13) beyond the additive NO-MATCH default tier AM-3 above; the
rule-match fold and its total order are untouched. `07 §5.1`'s `ToolContext` is
**not** changed: no `config()` accessor is added; bounds are injected into
`McpManager`/`McpTool` instead (§4.1/§4.6).

**Threading — reconciled, not amended.** §7 is aligned with the single
`io_context` model (`04 §9`: "no thread per client"; `07 §10`; `11 §3.2`): MCP
transports are async streams on the daemon's one loop and add no thread per
server or per call. No amendment to `04`/`07`/`11` is required.

---

## 13. References

- `00-architecture.md` §4.1 (agent ≠ UI), §4.2 (event stream is the runtime
  spine), §4.4 (execution environment as a capability boundary), §5 (components),
  §7.1–§7.3 (plugin model), §8.1–§8.3 (events, `EventBus`), §9.6 (supervisor
  model), §9.7 (path safety), §9.9 (attention), §9.10 (registry), §9.11
  (resource limits), §10.1 (agent handle), §11 (agent loop), §14.1–§14.3 (tool
  system), §15 (filesystem), §16 (process/shell), §18 (execution environment),
  §19 (permissions), §27 (MCP), §28 (LSP), §30 (subagents), §34 (cancellation),
  §35 (concurrency), §37 (configuration), §40 (logging), §43 (RPC mode), §44
  (testing), §45 (Fake LLM), §46 (security model), §49 (source tree), §50 (MVP),
  §51–§53 (phases), §54 (D1–D23, F1–F12), §55 (dsh comparison), §57
  (implementation order), §58 (milestones).
- `01-session.md` §2.1 (identifiers), §4.1 (live vs. durable event split), §4.5
  (`ToolCall`/`ToolResult`, `ToolOutcome`, `PermissionDecisionKind`), §5
  (`EventBus` routing), §6.3 (`deriveMessages`), I4/I11/I12 (append/pairing
  invariants), §13.2 (S-failure modes), §15 (test plan).
- `02-persistence.md` §4.1 (`max_payload_bytes`), §4.4 (`append`), §6.2 (chunk
  coalescing).
- `03-workspace-registry.md` (workspace identity and the canonical root the
  environment is built from).
- `04-workspace-host-daemon.md` §2.1 (`ResourceCaps`, `HostState`,
  `HostExitCode`), §3.2 (`setsid`), §3.3 (startup order; AM-2), §3.5 (shutdown), §3.6
  (signals), §3.7 (what the daemon hosts), §4.1 (`HostConfig`), §4.4 (state
  machine), §8 (`ResourceGovernor`), §9 (concurrency), §10 (invariants), §11
  (failure modes).
- `05-transport.md` §2.1 (wire names; total-and-rejecting `parse_*`), §2.2
  (`RpcCode`/`AppCode`, `data.kind`), §3.1 (socket convention), §3.3
  (length-prefixed framing — deliberately **not** reused), §4.1 (`host.hello`),
  §5.3 (`HostNoticeKind`/`host.event`; AM-1), §7.6 (`permission.request`).
- `06-agent-loop.md` §1.4 (seam ownership), §3.5 (cancellation), §5.1
  (turn/step cycle), §5.2 (`ContextAssembler::tools`), §5.5 (tool pipeline),
  §5.6 (terminal events), §7 (subagents), §10 (invariants), §11 (failure modes).
- `07-tools-execution.md` §2.1 (`ToolName`/`ToolVersion`/`ToolErrorCode`), §2.2
  (error taxonomy), §3.1 (`Tool`), §3.2 (`ToolSchema` subset), §3.3
  (validation), §4.1 (`ToolRegistry`), §4.4 (schema exposure), §5.1
  (`ToolContext`), §5.2 (output ring), §5.4 (`PermissionHandle`), §5.5
  (`ToolConfig`), §6.1 (`ExecutionEnvironment`), §6.2 (path resolution), §6.4
  (`ProcessService`), §6.8 (sandbox modes), §8.1 (`ToolResult` mapping), §9
  (process ownership), §11 (invariants X1–X16), §12 (failure modes E-F#), §13
  (dsh mapping), §14 (test plan), §15 (decisions).
- `08-llm-provider.md` §3.1 (`ToolSchema` hand-off), §4.2 (tool-call assembly),
  §6.4 (secret handling).
- `09-permissions.md` §2.2 (verdict vs. kind), §2.3 (scope model), §3.1
  (`PermissionPolicy`), §3.2 (`PermissionRequest`), §3.3 (rule model and
  config; AM-3), §3.4 (precedence fold; AM-3), §3.7 (`PermissionHandle`), §4.1
  (decision flow), §5 (background policy), §6.1 (mandatory chain), §6.2
  (redaction), §8 (invariants).
- `10-supervisor-tui.md` §2.3 (entry), §4.1 (browse), §5.2 (`UiEventAdapter::
  onHostNotice`; AM-1), §5.3 (idempotent apply), §11.1 (permission
  re-broadcast) — MCP status projection is its concern.
- `11-m2-errata.md` §2.2 (`TransportServer`), §3.2 (`TurnExecutor`), §3.3 (child
  status), §7.2 (`PermissionBroker`, `ClockReader`), §7.3 (frozen permission
  rules), §11.2 (destruction order), §14 (E1–E21), §15 (M-F1–M-F12).
- `12-m1-drift-errata.md` (M1 drift register).
- `HANDOFF.md` §6 (component design plan), §7 (verification gate).
- `AGENTS.md` (design-first rule, no warning suppression, naming).

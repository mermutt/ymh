# 05 — Transport & Protocol

**Component 05 of 10.** This spec owns the wire between a **supervisor TUI**
process (spec 10) and one or more **`WorkspaceHost` daemons** (spec 04): the
JSON-RPC 2.0 protocol over a **length-prefixed Unix domain socket** (§9.6, §43),
the **Interactive** and **Automation** server profiles, the full RPC method
catalog, the `SessionEnvelope` DTO, event multiplexing, per-session ordered
delivery, the authenticated handshake, cursors/reconnect, and the final socket
convention. The daemon owns the socket file and the accept loop (04 §5); this
spec owns what travels over it.

This document follows `00-architecture.md` (cited inline as `§n`),
`01-session.md` (`01 §n`), `02-persistence.md` (`02 §n`),
`03-workspace-registry.md` (`03 §n`), and `04-workspace-host-daemon.md`
(`04 §n`); where it cannot follow them it records the conflict under §14 rather
than choosing silently.

Status: **written** · verified: — · reviewer: — (tracked in `DESIGN_STATUS.md`,
`HANDOFF.md` §6–§7). No code may be written for this component until it is
`verified`.

> **Naming note.** The invariants in this spec are numbered `T1`, `T2`, … (for
> "transport"), so they cannot collide with the `D1`–`D23` design decisions in
> `00-architecture.md` §54. Component-local failure modes are `T-F#` (not `D-F#`,
> which spec 04 owns). The two namespaces are disjoint: this spec always writes
> architecture decisions with the `§54` prefix (`§54 D15`) and transport
> invariants bare (`T6`).

---

## 1. Purpose and scope

### 1.1 Position in the component graph

```text
   Supervisor TUI (spec 10)        CLI (ymh list / ymh show)      Automation client
        │                                │                              │
        │  HostConnection                │  read-only WAL               │  JSON-RPC
        │  (this spec's client seam)     │                              │
        └───────────────┬────────────────┴──────────────┬───────────────┘
                        │   JSON-RPC 2.0 over length-prefixed Unix socket
                        │   (Interactive | Automation profile)
                        ▼
   ┌──────────────────────────────────────────────────────────────────────────┐
   │  WorkspaceHost daemon (spec 04)                                            │
   │    owns <workspace>/.ymh/host.sock · accept loop · per-client fan-out      │
   │    ┌────────────────────────────────────────────────────────────────┐    │
   │    │ TransportServer (this spec)  ← framing · handshake · dispatch   │    │
   │    │   → SessionManager (01 §8, §9.6) · Agent (§10.1) · Policy (§19) │    │
   │    └────────────────────────────────────────────────────────────────┘    │
   └──────────────────────────────────────────────────────────────────────────┘
```

One socket = one daemon = one workspace. A supervisor opens **N** connections,
one per attached daemon, and multiplexes their streams into one `UiModel`
(§20.22). A daemon serves **N** concurrent connections (04 §6.3, H14).

### 1.2 Owned responsibilities (`HANDOFF.md` §6, row 05)

This spec pins:

- **Framing** — a 4-byte big-endian length prefix plus a UTF-8 JSON body, the
  maximum frame size, and malformed-frame handling (§3.3).
- **Handshake and authentication** — the mandatory `host.hello` first frame,
  protocol-version negotiation, profile negotiation, `ClientId` assignment, and
  the same-UID peer check on an owner-only socket (§4).
- **The wire DTOs** — the core, durable `Event` (§8.2, 01 §4.2) and
  `SessionEnvelope{session, event}` (§20.22); `UiEvent` is **never** on the wire
  (§20.6, §54 D15).
- **Server profiles** — Interactive (full event stream) and Automation
  (prompt/cancel/permission) (§6).
- **The RPC method catalog** — host, workspace/registry, session lifecycle,
  agent input, permissions, and event streaming (§7).
- **Event multiplexing and per-session ordered delivery** — subscriptions,
  fan-out to N clients, cursors, backpressure, and the late-event-after-close
  contract (§8).
- **Cursors and reconnect** — the opaque `EventCursor`, no-loss/no-dup resume,
  and reconnect supersede (§4.4, §8.5).
- **The final socket convention** — resolving 04 OQ-3 (§3.1).

### 1.3 Boundaries — deferred to other specs

- **Socket bind/unlink, accept loop, spawn, attach/detach lifecycle, crash/orphan
  policy (spec 04).** This spec frames each accepted connection; 04 owns the
  acceptor and the socket file (04 §1.3, §5).
- **Session semantics (spec 01).** `Session`, `EventBus`, ordering,
  `deriveMessages`, create/resume/fork/replay, `SessionHeader`. This spec
  transports them; it does not redefine them.
- **Persistence and the write lease (spec 02).** `SessionPersistence`,
  `read(after)`, lease loss. The transport surfaces lease loss as an error and
  a notification; it never writes.
- **Registry (spec 03).** `WorkspaceRegistry`, `claimHost`/`heartbeat`,
  liveness. The transport may expose read-only registry views (`workspace.list`,
  `session.list`); it never mutates the registry.
- **Agent/loop, tools, LLM, permissions (specs 06–09).** The transport invokes
  the `Agent` surface (§10.1) and forwards permission requests; the policy that
  answers them is spec 09.
- **Supervisor/TUI (spec 10).** `WorkspaceModel`, `DaemonStatus`, the tombstone,
  focus, and rendering. This spec supplies `HostConnection`; spec 10 consumes it.

### 1.4 Seam ownership relative to 01/02/03/04/06/09/10

| Concern | Owner | Transport's role |
|---|---|---|
| Socket file, bind/replace/unlink, accept loop | **04** | frames each connection |
| Framing, handshake, method catalog, profiles, cursors | **05 (this)** | the protocol |
| Per-session event ordering | 01 (mailbox contract, I6) | preserves it on the wire (T6) |
| `Event` / `SessionEnvelope` shape | 01 §4.2 / §20.22 | transports it verbatim (T4, T5) |
| Lease loss, store errors | 02 | maps to JSON-RPC errors + notification |
| Host claim/liveness | 03 | not a wire concern; discovery is 04 §6.2 |
| `Agent` methods (`send`/`cancel`/`steer`/`inject`) | §10.1, 06 | invokes them via `agent.*` |
| Permission decision flow | 09 | `permission.request` / `permission.decide` |
| Reconnect/tombstone policy | **05** (wire) / 10 (UI) | cursor resume + drain contract |
| Focus/active-session display | 10 (supervisor-local) | never gates work; `session.activate` is explicit (04 §6.6) |

---

## 2. Terminology and identities

### 2.1 Identifiers and value types

`SessionId`/`EventId`/`WorkspaceId` are frozen by 01 §2.1; `HostPid`/`HostBootId`
by 03 §2.1 and 04 §2.1. They are reproduced for reference and **not** redefined.
Transport-local types:

```cpp
namespace ymh::protocol {

// Opaque stream position. The wire never carries a numeric Sequence (01 §4.6);
// the daemon maps the token to a store Sequence internally (decision (c)).
struct EventCursor {
    std::string value;                 // opaque; the daemon-issued token
    auto operator<=>(const EventCursor&) const = default;
};

// Supervisor-process identity, minted once per TUI process (UUIDv4). Used to
// supersede a stale connection on reconnect (04 §6.3, D-F22, T12).
struct ClientInstanceId {
    std::string value;
    auto operator<=>(const ClientInstanceId&) const = default;
};

// Daemon-assigned, connection-scoped handle for fan-out bookkeeping (04 §2.1).
struct ClientId {
    std::uint64_t value;
    auto operator<=>(const ClientId&) const = default;
};

// Server-assigned handle for one (session, client) stream subscription.
struct SubscriptionId {
    std::uint64_t value;
    auto operator<=>(const SubscriptionId&) const = default;
};

// JSON-RPC 2.0 request id: integer or string, echoed verbatim (T9).
// `std::monostate` encodes JSON `null`, used only for error responses produced
// before a request id exists (framing-level errors: §3.3, §5.4).
struct RequestId {
    std::variant<std::monostate, std::int64_t, std::string> value;
};

enum class ServerProfile : std::uint8_t {
    Interactive,   // wire "interactive"; full event stream (§9.6)
    Automation,    // wire "automation"; prompt/cancel/permission (§9.6)
};

inline constexpr std::uint32_t kProtocolVersion = 1;

} // namespace ymh::protocol
```

`EventCursor` is deliberately opaque: it is not `Sequence`, it is not parsed by
clients, and it names a **durable position** in a session's committed log (T23).
It is daemon-instance-independent: any daemon instance serving that session
resolves it, including one started after a daemon crash/restart (§5.5, §8.5).
`ClientId` is connection-scoped and dies with the connection;
`ClientInstanceId` outlives it and is what makes reconnect idempotent.

**Wire enum encodings (pinned, T22).** Every transport enum has one canonical
lower-case JSON string; clients and golden fixtures (§13.3) depend on them. The
profile strings align with `01 §3`/`01 §4.5`'s `server_profile` values
(`'interactive' | 'automation'`).

| C++ value | JSON string | Appears in |
|---|---|---|
| `ServerProfile::Interactive` | `"interactive"` | `HelloParams.profile`, `HelloResult.profiles` |
| `ServerProfile::Automation` | `"automation"` | idem |
| `StreamFrom::Kind::Now` | `"now"` | `event.subscribe.from` |
| `StreamFrom::Kind::Beginning` | `"beginning"` | idem |
| `StreamFrom::Kind::Cursor` | `"cursor"` | idem |
| `HostNoticeKind::SessionClosed` | `"session_closed"` | `host.event` |
| `HostNoticeKind::SessionCreated` | `"session_created"` | `host.event` |
| `HostNoticeKind::LeaseLost` | `"lease_lost"` | `host.event` |
| `HostNoticeKind::DaemonShuttingDown` | `"daemon_shutting_down"` | `host.event` |
| `PermissionAnswer::Allow` | `"allow"` | `permission.decide` |
| `PermissionAnswer::Deny` | `"deny"` | idem |
| `PermissionScope::Once` | `"once"` | idem |
| `PermissionScope::Session` | `"session"` | idem |
| `PermissionScope::Always` | `"always"` | idem |

`wire_name()` / `parse_*()` are total over these sets and reject anything else.
The server rejects an unknown string with `InvalidParams`; the client fails loud
on an unknown string in a notification (mirrors 01 S3).

### 2.2 Protocol error taxonomy

Every fallible transport operation surfaces a JSON-RPC error code so clients and
tests share names. Standard codes are used where JSON-RPC 2.0 defines them;
`ymh` application errors live in the reserved server range `-32000…-32099`.

```cpp
namespace ymh::protocol {

// JSON-RPC 2.0 standard codes.
enum class RpcCode : int {
    ParseError      = -32700,
    InvalidRequest  = -32600,
    MethodNotFound  = -32601,
    InvalidParams   = -32602,
    InternalError   = -32603,
};

// ymh application codes (server range).
enum class AppCode : int {
    HandshakeRequired        = -32000,
    UnsupportedProtocol      = -32001,
    AuthFailed               = -32002,
    NotServing               = -32003,  // 04 HostErrorCode::NotServing
    ShutdownInProgress       = -32004,  // 04 HostErrorCode::ShutdownInProgress
    UnknownWorkspace         = -32005,
    UnknownSession           = -32006,  // 01 S6
    LeaseLost                = -32007,  // 02 §5.7, 01 S4
    InvalidForkBoundary      = -32008,  // 01 S5
    DependentSession         = -32009,  // 02 P-F13
    FrameTooLarge            = -32010,
    CursorInvalid            = -32011,
    MethodNotAllowedForProfile = -32012,
    SubscriptionLimit        = -32013,
    PayloadTooLarge          = -32014,  // 01 S10, 02 P-F15
    StoreUnavailable         = -32015,  // 02 P-F8/P-F10
    RegistryUnavailable      = -32016,  // 03 R-F2/R-F15
    PermissionDenied         = -32017,
    SessionNotActive         = -32018,
};

struct RpcError {
    RequestId       id;
    int             code;      // RpcCode | AppCode
    std::string     message;
    nlohmann::json  data;      // { "kind": "UnknownSession", "session": "..." }
};

} // namespace ymh::protocol
```

The mapping from daemon-side `HostError` (04 §2.2) and store/registry errors
(02 §2.2, 03 §2.2) to these codes is pinned in §7.1. A daemon never leaks a raw
errno or SQLite error string as `message`; `data.kind` carries the stable
machine token and `message` is human-readable.

---

## 3. Transport substrate

### 3.1 Socket convention (resolves 04 OQ-3)

The v1 deployment transport is a **Unix domain socket** at:

```text
<workspace>/.ymh/host.sock
```

This spec **adopts** the v1 default that 03 §6.5 and 04 §5.1 already use; it
does not change it. Adopting it keeps three independently-derived values in
agreement: the daemon's bind path (04 §5.1), the registry's `host_socket` column
(03 §3.3, §9.10), and the process-scan fallback's derivation (03 §6.5). Because
this spec owns the final convention (04 decision (p)), it pins the default as
the **transport convention**, not merely a daemon default.

Rules:

```text
socket type         SOCK_STREAM, SOCK_CLOEXEC
socket mode         0600, bound under umask(0o077) (04 H7)
parent dir          <workspace>/.ymh/ is 0700 (02 §4.3)
path length         must be <= sizeof(sockaddr_un::sun_path) - 1 (~108 bytes on
                    Linux); the daemon fails loud with SocketPathTooLong before
                    bind and never truncates (04 §5.4, D-F6)
abstract namespace  deferred; a future shortening/hashing scheme changes 04 §5,
                    03 §6.5, and this section together (OQ-1)
```

`sun_path` length is the one real portability constraint: a deep workspace root
can exceed it. The daemon's pre-bind check is authoritative (04 §5.4); this spec
adds no alternate encoding. If a future convention is chosen, it must be applied
in all three places simultaneously and is an OQ here (OQ-1).

### 3.2 Connection lifecycle

```text
client (supervisor / CLI / automation)
  socket(AF_UNIX, SOCK_STREAM)
  connect(host_sock)                      # 04 already bound + listening
  ── host.hello (mandatory first request; performs the handshake) ──►
  ◄── hello result { client_id, workspace, boot_id, profiles, limits } ──
  ── host.attach (optional; idempotent status re-confirm, NOT a re-handshake) ──►
  ── event.subscribe / session.* / agent.* ... ──►
  ◄── event.stream notifications (per subscription) ──
  ── host.ping ──►  ◄── pong ──              # liveness
  ── host.detach ──►  ◄── result ── then the daemon closes the socket
```

- The daemon accepts, registers a connection with `onClientAttached(ClientId)`
  (04 §4.2), and waits for `host.hello` within `handshake_timeout` (default 5 s).
- A connection that sends any other request first, or that times out before
  hello, receives `HandshakeRequired` and is closed (T3, T-F7).
- The daemon never blocks its accept loop on a client: reads and writes are
  asynchronous on the daemon's single `io_context` (04 §3.4, §9).
- `host.hello` is the handshake: it authenticates, negotiates the protocol
  version and profile, and assigns the `ClientId`. `host.attach` is a separate,
  idempotent request that re-confirms attachment and returns `HostStatus`; it
  does **not** re-authenticate and cannot bypass the handshake.
- `host.detach` is a graceful detach: the daemon replies, removes the `ClientId`
  and its subscriptions, then **closes the connection**. The client must
  re-handshake to attach again. An abrupt `close(fd)` is equivalent to detach.
- `close(fd)` by either side is a detach: the daemon calls
  `onClientDetached(ClientId)` and drops that client's subscriptions. It never
  emits `SessionEnded` and never stops the daemon (04 H8/H9, 01 I16, T13).

### 3.3 Framing (pinned)

Every frame is:

```text
┌────────────────────┬─────────────────────────────────────────────┐
│ length (4 bytes)   │ body (length bytes)                         │
│ unsigned, big-     │ UTF-8 encoded JSON (RFC 8259)               │
│ endian (network)   │ one JSON-RPC message per frame              │
└────────────────────┴─────────────────────────────────────────────┘
```

- **Endianness is pinned to big-endian (network byte order).** This is the only
  endianness the protocol uses; a little-endian reader is a defect.
- `length` counts **body bytes only**, never the header. It must satisfy
  `0 < length <= max_frame_bytes`.
- The body is exactly one JSON value: a JSON-RPC request, response, error, or
  notification object. Leading/trailing whitespace is allowed, but **non-
  whitespace bytes after the value are `InvalidRequest` and the connection is
  closed**: the frame's declared length already bounded the message, so trailing
  bytes mean the sender's framing is wrong and cannot be trusted (T18, T-F4).
- The transport is **message-framed, not newline-delimited**; JSON bodies may
  contain newlines.
- There is no compression and no encryption on the Unix-socket path; both are
  out of scope for v1 (a future TCP/SSH transport may add them, OQ-1).

Limits:

```cpp
namespace ymh::protocol {

struct TransportLimits {
    std::size_t max_frame_bytes{8u * 1024u * 1024u};      // 8 MiB (see below)
    std::size_t max_outbound_bytes{8u * 1024u * 1024u};   // per client, §8.3
    std::size_t max_subscriptions_per_client{64};
    std::chrono::milliseconds handshake_timeout{5'000};
    std::chrono::milliseconds idle_timeout{30'000};       // no frame -> close
};

} // namespace ymh::protocol
```

`max_frame_bytes` MUST be at least the store's `max_payload_bytes` plus envelope
overhead (02 §4.1 defaults `max_payload_bytes = 4 MiB`; 8 MiB leaves room). A
single `Event` frame is therefore always representable. Large histories are
**streamed** as many small `event.stream` frames, never packed into one frame
(§8.5, T2). The exact default is provisional pending spec 07's tool-output caps
(OQ-2).

Malformed-frame handling (all covered by T-F1…T-F4):

```text
length == 0                         -> InvalidRequest, then close (no body to parse)
length >  max_frame_bytes           -> FrameTooLarge error frame, then close
body not valid UTF-8                -> ParseError, then close (cannot trust framing)
body not valid JSON                 -> ParseError (-32700); connection may continue
JSON is not an object / array batch -> InvalidRequest (-32600); batches unsupported
trailing bytes after the JSON value  -> InvalidRequest (-32600), then close
read EOF mid-frame / mid-header     -> close; no partial message delivered
```

A frame whose length prefix is intact but whose body fails JSON parsing is
recoverable: the connection stays open and the client receives a `ParseError`.
A frame whose **length prefix** is implausible (zero or over the cap) means the
byte stream can no longer be trusted, so the connection is closed.

Framing-level errors (zero/oversized length, invalid UTF-8, malformed JSON, a
non-object body, trailing bytes) are produced before any request id is known, so
their error response carries `"id": null` (JSON-RPC 2.0 error object, §5.4,
T-F1…T-F4).

```cpp
namespace ymh::protocol {

class FrameCodec {
public:
    static constexpr std::size_t kHeaderBytes = 4;

    // Prepends the 4-byte big-endian length. Throws PayloadTooLarge if the
    // encoded body exceeds the cap.
    static std::string encode(std::string_view json_body, std::size_t max);

    // Consumes zero or more frames from `buf`. Returns the frames decoded and
    // the number of bytes consumed; leaves a partial tail in `buf`. Throws
    // ProtocolError{FrameTooLarge|ParseError} per the rules above.
    static std::vector<std::string> decode(std::string& buf, std::size_t max);
};

} // namespace ymh::protocol
```

### 3.4 stdio transport (test/debug only)

JSON-RPC over stdin/stdout uses the **same** framing and the **same** method
catalog, so a test harness can drive the daemon without a socket. It is a
debugging convenience only and is **not** the deployment transport (§43). The
daemon's production entry point always binds the Unix socket; a `--stdio`
debug flag is opt-in and rejected in the live PTY layer (T20).

### 3.5 TCP / SSH (deferred)

§47 Mode B (local TUI + remote runtime) maps onto this protocol unchanged: only
the accept/connect layer is TCP, and the framing and method catalog are
transport-agnostic (§9.6, §47). TCP is **deferred** (HANDOFF §5 item 4); when it
arrives it must add authentication (same-UID does not exist across hosts) and is
recorded as OQ-1.

---

## 4. Handshake, authentication, and identity

### 4.1 `host.hello` (mandatory first request)

The first frame on a connection MUST be a `host.hello` request. Before a
successful hello, the daemon dispatches no other method (T3).

```cpp
namespace ymh::protocol {

struct HelloParams {
    std::uint32_t                 protocol_version;   // client's version
    ServerProfile                 profile;            // requested profile
    ClientInstanceId              client_instance;    // supervisor-process UUID
    std::optional<EventCursor>    resume_hint;        // unused by v1 daemon
};

struct HelloResult {
    std::uint32_t                 protocol_version;   // server's version (== 1)
    WorkspaceId                   workspace;          // this daemon's workspace
    HostBootId                    boot_id;            // PID-reuse guard (04 §2.1)
    HostPid                       pid;                // daemon pid
    std::vector<ServerProfile>    profiles;           // supported profiles
    ClientId                      client_id;          // assigned handle
    TransportLimits               limits;             // server limits
    std::int64_t                  server_time_ms;     // epoch ms
};

} // namespace ymh::protocol
```

Negotiation rules:

```text
client protocol_version != 1        -> UnsupportedProtocol (-32001), close
profile not in profiles             -> InvalidParams (-32602), close
client_instance empty/not UUIDv4    -> InvalidParams (-32602), close
peer uid != daemon uid              -> AuthFailed (-32002), close   (§4.2)
daemon state != Serving             -> NotServing / ShutdownInProgress, close
otherwise                           -> assign ClientId, reply HelloResult
```

The daemon does not silently substitute a profile. In v1 both profiles are
always supported, so a profile rejection means the client sent an unknown value.

### 4.2 Authentication

The Unix socket is the authentication boundary:

```text
socket file mode        0600, under a 0700 .ymh/ directory (04 H7, 02 §4.3)
peer credential         SO_PEERCRED: the peer's uid MUST equal the daemon's uid
```

A same-machine, same-user process is the trust domain. This matches the owner-
only socket 04 binds and is what 03 §6.3 rule 5 calls "an authenticated
handshake (spec 05)". A uid mismatch yields `AuthFailed` and an immediate close
(T15, T-F9). There is **no** bearer token and no password in v1; a token-file
scheme is deferred with TCP (OQ-1). The daemon never logs the peer's command
line (04 H18).

**Trust domain (explicit).** Same-UID is the *entire* trust boundary in v1: any
process running as the daemon's user may connect, and `ClientInstanceId` is
client-supplied, not authenticated. Consequently a same-UID peer that presents
another supervisor's `ClientInstanceId` can supersede that supervisor's
connection (§4.4). This is acceptable in the single-user, single-machine model;
a credential scheme is required before multi-user or TCP (OQ-1).

### 4.3 `ClientId` assignment

On a successful hello the daemon assigns a `ClientId` (04 §4.2
`onClientAttached`) and registers the connection for fan-out. `ClientId` is
connection-scoped: it dies with the connection and is never persisted (R1: the
registry holds no transport state).

### 4.4 Reconnect supersede

A supervisor reconnecting (e.g. the TUI restarted, or a dropped socket) presents
the **same** `ClientInstanceId`. The daemon treats that as the same logical
client and **supersedes** the prior connection (04 §6.3, D-F22, T12):

```text
on hello:
  if an existing connection has the same client_instance:
      drop its subscriptions and outbound queue
      close its socket (the old stream is superseded)
      assign the new connection the same logical identity, a fresh ClientId
  else:
      normal attach
```

This makes reconnect idempotent without leaking subscriptions. The client
resumes its streams with `event.subscribe{ from: cursor }` using the last
`EventCursor` it processed (§8.5), so no event is lost and none is duplicated.

Supersede is authorized by same-UID only (§4.2): a same-UID process presenting
another supervisor's `ClientInstanceId` will supersede it. That is inside the
single-user trust domain; the mechanism is an idempotency aid, not a security
boundary.

### 4.5 Liveness: ping/pong and idle timeout

Because a half-closed Unix socket may not surface promptly, the client sends
`host.ping` every `ping_interval` (default 10 s) and the daemon replies. The
daemon closes a connection that has sent no frame for `idle_timeout` (default
30 s, T-F21). Ping is a normal request/response and participates in the same
frame budget; it is never used to gate session work (D20).

---

## 5. Wire DTOs (pinned)

### 5.1 The core durable `Event`

The wire carries the core, frontend-agnostic `Event` of §8.2 / 01 §4.2. It
carries **no** `Sequence` (01 §4.6) and no frontend type (T4, §54 D15):

```json
{
  "id": "9f2c...uuid",
  "session_id": "1a7b...uuid",
  "timestamp": 1789430822832,
  "type": "assistant/chunk",
  "payload": { "message": "m-1", "index": 3, "text": "hi", "kind": "Text" }
}
```

- `id` / `session_id`: UUIDv4 strings (01 §2.1).
- `timestamp`: epoch milliseconds (matching the store, 01 §9.2), so replay is
  exact and no timezone formatting is on the wire.
- `type`: the **slash form** from `wire_name()` (01 §4.3), e.g. `turn/start`,
  `tool/result`, `permission/decision`. Unknown types are a decode error at the
  client (mirroring 01 S3) and are never silently skipped.
- `payload`: the JSON encoding of the typed payload struct (01 §4.5).

Live events (§8.1) are **not** durable and therefore are **not** carried as
`Event` frames. The only live signals on the wire are the transport-level
notifications this spec defines (`event.stream`, `permission.request`,
`host.event`), none of which is a `UiEvent`.

### 5.2 `SessionEnvelope`

Exactly §20.22, reproduced (T5):

```cpp
namespace ymh::protocol {

struct SessionEnvelope {
    SessionId session;   // routing key (§20.22 retains it although Event carries session_id)
    Event     event;     // core, durable; the client adapts via UiEventAdapter
};

} // namespace ymh::protocol
```

JSON:

```json
{ "session": "1a7b...uuid", "event": { "id": "...", "type": "turn/start", ... } }
```

`session` is retained for routing (§20.22); the client asserts
`envelope.session == envelope.event.session_id` and treats a mismatch as a
protocol corruption (T-F20). `UiEvent` is produced only by the frontend's
`UiEventAdapter` (§20.6, §54 D15) and **never** appears on the wire.

### 5.3 Host and workspace notifications

Daemon→client control notifications are distinct from session events:

```cpp
namespace ymh::protocol {

enum class HostNoticeKind : std::uint8_t {
    SessionClosed,      // a session was deleted/closed; subscription ends
    SessionCreated,     // the open set changed (daemon-driven)
    LeaseLost,          // 02 §5.7: a session degraded to read-only
    DaemonShuttingDown, // host.shutdown accepted; drain begins
};

struct HostNotice {
    HostNoticeKind kind;
    WorkspaceId    workspace;
    std::optional<SessionId> session;
    std::string    detail;
};

} // namespace ymh::protocol
```

These are **supervisor-side hints**; the durable truth stays in the session log
(§54 D2). `LeaseLost` mirrors 02 §5.7 step 2 (the daemon "notifies its
supervisor"), and `SessionClosed` lets the client finish its tombstone (F3,
§8.4). They are notifications, not `Event`s.

The notification method is **`host.event`** (server→client), with
`params = HostNotice` and `kind` encoded as its pinned wire string (§2.1, T24).
The `host.` namespace is the daemon's, so `host.event` is never confused with
`event.stream`, which carries `SessionEnvelope` session events.

### 5.4 JSON-RPC envelope shapes

```cpp
namespace ymh::protocol {

struct Request       { RequestId id; std::string method; nlohmann::json params; };
struct Response      { RequestId id; nlohmann::json result; };
struct ErrorResponse { RequestId id; int code; std::string message; nlohmann::json data; };
struct Notification  { std::string method; nlohmann::json params; };

} // namespace ymh::protocol
```

Rules (T9, T19):

- `id` is echoed **verbatim** (string or integer); the client correlates on it.
- A client MUST NOT reuse an in-flight `id`; the daemon rejects a duplicate
  in-flight id with `InvalidRequest` (T-F14).
- An error produced before a request `id` is known (framing-level errors, §3.3)
  carries `"id": null`; `RequestId`'s `std::monostate` encodes that `null`
  (§2.1). A response to a well-formed request always echoes a non-null id.
- A request without `id` is a notification: the daemon executes it but sends no
  response and no error (fire-and-forget). `agent.prompt` is a **request**, not
  a notification, so the client learns of acceptance.
- **Batch requests are not supported**: a top-level JSON array is rejected with
  `InvalidRequest` (T-F15). This keeps per-session ordering and error
  attribution unambiguous.

### 5.5 Cursors

```cpp
namespace ymh::protocol {

struct EventCursor { std::string value; };   // opaque; daemon-issued

} // namespace ymh::protocol
```

- The cursor identifies a position in a session's **committed** log.
- The wire never exposes a numeric `Sequence`; the daemon maps the opaque token
  to a store `Sequence` internally (decision (c), 01 §4.6).
- `event.subscribe` accepts `from ∈ { now, beginning, cursor(c) }` (§7.7).
- **Durability across daemon restart (pinned, T23).** A cursor names a durable
  position, not a daemon-instance-scoped offset, so any daemon instance serving
  that session resolves it — including a daemon started after the original one
  crashed or was restarted. The `host_boot_id` (04 §2.1) is irrelevant to a
  cursor. A cursor is **not** tied to the `ClientId` or the `SubscriptionId`.
- A cursor is valid only for the session that produced it. A cursor from a
  different session, or one whose underlying position no longer exists (session
  deleted/erased, or a retention prune), yields `CursorInvalid` (T16, T-F13).
  The client must then re-subscribe with `beginning`; the transport never
  silently resets to `now`. Erasure is the only normal cause of invalidity; a
  daemon restart alone never invalidates a cursor.

---

## 6. Server profiles

Profiles mirror dsh (§9.6): **Interactive** is the full-event-stream
(API-gateway) profile; **Automation** is the prompt/cancel/permission (ACP)
profile. The profile is negotiated once, in `host.hello`, and is immutable for
the connection (T10).

### 6.1 Interactive

The default for the TUI. Exposes the full method catalog (§7) and, on
`event.subscribe`, streams every committed durable event for the subscribed
sessions in publish order. It is the only profile that may call
`session.activate` / `session.suspend` (they are explicit controls, 04 decision
(m)) and the only profile that receives `host.event` and full `event.stream`
traffic by default.

### 6.2 Automation

A headless client (CI, editor integration, scripted driver) that submits a
prompt and waits for the outcome. It exposes:

```text
session.create / session.resume / session.delete
session.list / session.show / session.replay (read-only)
agent.prompt / agent.followup / agent.cancel
permission.decide
event.subscribe / event.unsubscribe   (universal: any profile may subscribe)
host.detach / host.ping / host.status
```

`event.subscribe` and `event.unsubscribe` are **universal**: every profile may
subscribe to and unsubscribe from any session it can see. The profile changes
only the *default filter* and the gated methods, never the right to subscribe.

It **does not** expose `session.activate`, `session.suspend`, or
`host.shutdown` (T10); those are operator controls. Automation's default
subscription filter is turn-boundary and terminal events
(`turn/start|end|cancel`, `assistant/message`, `tool/result`,
`permission/decision`, `session/end`); it may opt into the full stream
explicitly, but the profile's default is quieter to bound headless traffic. The
exact ACP-parity subset is OQ-3.

### 6.3 Profile gating

A method not permitted by the negotiated profile returns
`MethodNotAllowedForProfile` (T10, T-F18). Gating is enforced **before** any
side effect: a rejected method never creates a session, never appends, and
never touches the store.

---

## 7. RPC method catalog

### 7.1 Naming and error conventions

- Method names are `namespace.verb` (`host.hello`, `session.create`,
  `agent.prompt`, `event.subscribe`). The namespace names the subsystem, not the
  profile.
- Errors use the codes of §2.2. The daemon maps its own `HostError` (04 §2.2)
  and the store/registry errors (02 §2.2, 03 §2.2) as follows:

```text
HostErrorCode::AlreadyRunning      -> InvalidRequest (data.kind="AlreadyRunning")
HostErrorCode::WorkspaceMissing    -> UnknownWorkspace
HostErrorCode::StoreUnavailable    -> StoreUnavailable
HostErrorCode::RegistryUnavailable -> RegistryUnavailable
HostErrorCode::NotServing          -> NotServing
HostErrorCode::ShutdownInProgress  -> ShutdownInProgress
HostErrorCode::AttachRejected      -> AuthFailed
HostErrorCode::HostUnreachable     -> (client-side; never a wire error)
LeaseLost (02 §5.7)                -> LeaseLost + HostNotice{LeaseLost}
UnknownSession (01 S6)             -> UnknownSession
InvalidForkBoundary (01 S5)        -> InvalidForkBoundary
DependentSessionError (02 P-F13)   -> DependentSession
PayloadTooLarge (02 P-F15)         -> PayloadTooLarge
CorruptionError (02 P-F8)          -> StoreUnavailable
```

`HostErrorCode::SocketUnavailable`, `SocketPathTooLong`, `LogSinkUnwritable`,
`StoreOpenError`, and `SchemaVersionError` are **startup-only**: the daemon exits
before it serves (04 §3.3, §3.5), so they never appear as runtime JSON-RPC
errors. If a connection somehow exists while the daemon is `Starting`/`Failed`,
the request is answered with `NotServing`.

- A method that mutates (create/resume/fork/delete/activate/prompt/…)
  serializes through the daemon's single loop and the session's own
  `appendMutex_` (01 I18, 02 §8); the transport adds no second write path.

### 7.2 Host methods

```text
host.hello       params: HelloParams         result: HelloResult      (mandatory first; handshake)
host.attach      params: {}                  result: HostStatus       (idempotent; NOT a re-handshake)
host.detach      params: {}                  result: {}               (removes ClientId, then closes the socket)
host.status      params: {}                  result: HostStatus
host.ping        params: {}                  result: { "server_time_ms": n }
host.shutdown    params: { reason }          result: {}               (Interactive only)
```

`host.hello` and `host.attach` are distinct: hello authenticates, negotiates,
and assigns `ClientId`; attach only re-confirms attachment and returns
`HostStatus` (it cannot bypass the handshake). `host.detach` replies and then
closes the connection (§3.2); `host.shutdown` triggers the 04 §3.5 graceful
path.

`HostStatus`:

```cpp
namespace ymh::protocol {

struct HostStatus {
    HostState                 state;             // 04 §4.4
    WorkspaceId               workspace;
    HostBootId                boot_id;
    HostPid                   pid;
    std::size_t               attached_clients;  // 04 §4.2
    std::optional<SessionId>  active_session;    // 04 decision (m)
    ServerProfile             profile;
};

} // namespace ymh::protocol
```

`host.shutdown` requests the 04 §3.5 graceful path (stop accepting → drain →
flush → `releaseHost` → unlink). The daemon replies **before** it stops
accepting, then sends `HostNotice{DaemonShuttingDown}` and closes (T-F19).
`host.detach` and `close(fd)` are detach, never shutdown (T13).

### 7.3 Workspace and registry reads

Read-only views, served from the daemon's `WorkspaceRegistry` handle (03 §4.2)
and `SessionManager` (01 §8). They never mutate the registry (R1, R2).

```text
workspace.list   params: {}                  result: [WorkspaceSummary]   (registry read)
workspace.show   params: { workspace }       result: WorkspaceDetail      (registry read)
session.list     params: {}                  result: [SessionSummary]     (OWN workspace only)
session.show     params: { session }         result: SessionDetail        (OWN workspace only)
```

```cpp
namespace ymh::protocol {

struct WorkspaceSummary {
    WorkspaceId   id;
    std::string   canonical_path;
    std::string   display_title;
    std::optional<HostPid>   host_pid;      // NULL/absent = no daemon (R3)
    std::optional<HostBootId> host_boot_id;
    std::optional<std::string> host_socket;
    std::optional<std::int64_t> host_heartbeat_ms;
};

struct SessionSummary {
    SessionId   id;            // junction + store key
    std::int64_t ordinal;      // registry junction (03 R8): order, not dense
    bool        archived;      // registry junction
    std::string title;         // own store SessionHeader (01 §3)
    std::string kind;          // own store: root | fork | subagent
    std::int64_t updated_at_ms; // own store SessionHeader.updated_at
};

struct SessionDetail {
    SessionSummary summary;
    SessionHeader  header;     // 01 §3
    std::size_t    event_count;
};

} // namespace ymh::protocol
```

`workspace.list` / `workspace.show` are read-only registry views (03 §4.2
`listWorkspaces` / `findById`) and may name any workspace because the registry is
shared.

`session.list` / `session.show` are scoped to the daemon's **own workspace**: a
daemon has exactly one `sessions.db` (04 H1, H6), so it cannot serve another
workspace's sessions. There is **no** `workspace` parameter; a request naming a
different workspace is `UnknownWorkspace`. `SessionSummary` is **enriched from
the daemon's own store**, not built from the junction alone: `title`/`kind`/
`updated_at_ms` come from `SessionStore::list()`/`load()` (02 §4.6, 01 §3);
`SessionDetail.event_count` likewise comes from the store, while only
`ordinal`/`archived` come from the
registry junction (03 §3.3), joined by `SessionId`. The junction supplies order
and archive state; the store supplies identity and content metadata.
`session.show` returns the `SessionHeader` (01 §3) plus the event count; it does
**not** return the full log (use `session.replay` or `event.subscribe`).

### 7.4 Session lifecycle

```text
session.create    params: { cwd?, model?, title?, server_profile? }  result: { session, header }
session.resume    params: { session }                               result: { session, status }
session.fork      params: { session, seed_length }                  result: { session, header }
session.replay    params: { session, from? }   result: { subscription, cursor }  (streams on it)
session.activate  params: { session }                               result: {}          (Interactive)
session.suspend   params: { session }                               result: {}          (Interactive)
session.close     params: { session }                               result: {}          (detach semantics)
session.delete    params: { session, confirm: true }                result: {}
```

- `session.create` maps to `SessionManager::createSession(SessionOptions)`
  (01 §8). `cwd` MUST canonicalize to the daemon's workspace root; any other
  value is `InvalidParams` (04 H1, 01 I8). A create appends `SessionStarted`
  (01 §9.1) and adds the registry junction row (03 R16, daemon-written).
- `session.resume` maps to `resumeSession`; it appends nothing and returns
  `AgentStatus::Idle` (01 §9.2, F10).
- `session.fork` maps to `forkSession(parent, seedLength)`; a boundary beyond the
  parent's resolved view is `InvalidForkBoundary` (01 S5).
- `session.replay` maps to `replaySession` (01 §9.4). It is **read-only**: it
  never acquires the lease and never appends (01 I19). It establishes a
  subscription exactly like `event.subscribe` and streams the session's resolved
  events on that `SubscriptionId` with `replay: true`; its result is
  `{ subscription, cursor }`, and when the committed view is exhausted the daemon
  sends `event.unsubscribed{ reason: "replay_complete" }` (§7.7). Unlike
  `event.subscribe{ from: beginning }` it does **not** go live. This is the
  `ymh replay SESSION` substrate (§9.5).
- `session.activate` / `session.suspend` map to the daemon's
  `activateSession`/`suspendSession` (04 §4.2, decision (m)). They are explicit
  controls, **not** a mirror of UI focus: attach, detach, and focus never call
  them, and focus never gates work (04 §6.6, H17). One session per workspace is
  active at a time.
- `session.close` maps to `SessionManager::closeSession(SessionId)` (01 §8),
  which implements detach semantics: it does **not** emit `SessionEnded`
  (01 I16) and the daemon keeps appending. It also ends the calling client's
  subscription to that session; it is not `host.detach` (which ends the whole
  connection, 04 §6.4).
- `session.delete` is destructive and requires `confirm: true`. It appends
  `SessionEnded{reason=Deleted}` then erases (01 §9.5, 02 §4.7); a delete with
  dependents fails with `DependentSession` (02 P-F13). It emits `SessionEnded`
  and **must** emit it (01 I16).

### 7.5 Agent input

Mapped to the `Agent` surface of §10.1 (the inbox is owned by spec 06):

```text
agent.prompt    params: { session, message }        result: { accepted: true }   -> Agent::send (new turn, origin User)
agent.followup  params: { session, message }        result: { accepted: true }   -> queued after current turn (origin FollowUp)
agent.steer     params: { session, message }        result: { accepted: true }   -> Agent::steer (origin Steer)
agent.inject    params: { session, context }        result: { accepted: true }   -> Agent::inject (context/injected)
agent.cancel    params: { session, reason? }        result: { cancelled: bool }  -> Agent::cancel (F9)
agent.status    params: { session }                 result: { status }           -> AgentStatus
```

- `message` is the §12 `Message` shape (text/attachments); `context` is a
  `ContextMessage` (01 §4.5, §31). The transport validates shape and size
  (bounded by `max_payload_bytes`, 02 §4.1) and rejects oversized input with
  `PayloadTooLarge` (01 S10).
- `agent.cancel` names exactly one session; it cancels only that session's
  in-flight turn (§34, F9, 04 §11.1) and never touches another.
- `agent.prompt` is a request, not a notification, so the client learns whether
  the turn was accepted; the turn's progress arrives as events (§8).
- `agent.prompt`'s `TurnOrigin` is state-dependent: `User` when it starts a turn, `FollowUp` when queued behind a running turn (06 §3.2).
- `agent.status` returns `Idle` or `Running` only (01 §9.8, DIV-7).

### 7.6 Permissions

Permissions are a **notification + request/response** pair, never an `Event` on
the wire. The durable outcome is `permission/decision` (01 §4.3, §19).

```text
server -> client   permission.request  params: { request_id, session, tool,
                                                 arguments, summary, expires_at_ms }
client -> server   permission.decide   params: { request_id, decision, scope }
```

```cpp
namespace ymh::protocol {

enum class PermissionAnswer : std::uint8_t { Allow, Deny };
enum class PermissionScope  : std::uint8_t { Once, Session, Always };

struct PermissionRequest {
    std::string     request_id;     // UUIDv4, correlation only
    SessionId       session;
    std::string     tool;
    nlohmann::json  arguments;
    std::string     summary;
    std::int64_t    expires_at_ms;  // server-side deadline
};

struct PermissionDecisionParams {
    std::string      request_id;
    PermissionAnswer decision;
    PermissionScope  scope;
};

} // namespace ymh::protocol
```

- The daemon forwards a request only to clients that are attached and (for the
  Interactive profile) subscribed to the session. A detached daemon must not
  block: per F2/§9.9 the policy (spec 09) applies auto-deny / auto-allow /
  surface-via-attention. The transport never waits on an absent client
  (04 D-F8).
- `permission.decide` for an unknown or expired `request_id` returns
  `InvalidParams` (idempotent at the policy layer, 09). The decision is recorded
  as a durable `PermissionDecision` event (01 §4.5) and the turn resumes.
- With multiple supervisors attached, the request is broadcast; the first
  decision wins and later ones are ignored. The exact fan-out policy is OQ-4.

### 7.7 Event streaming

```text
client -> server   event.subscribe    params: { session, from? }   result: { subscription, cursor }
client -> server   event.unsubscribe  params: { subscription }     result: {}
server -> client   event.stream       notification { subscription, replay, envelope, cursor }
server -> client   event.unsubscribed notification { subscription, reason }
```

```cpp
namespace ymh::protocol {

struct StreamFrom {
    enum class Kind : std::uint8_t { Now, Beginning, Cursor } kind{Kind::Now};
    std::optional<EventCursor> cursor;   // required iff kind == Cursor
};

struct SubscribeParams {
    SessionId  session;
    StreamFrom from;
};

struct SubscribeResult {
    SubscriptionId subscription;
    EventCursor    cursor;     // position at the moment of subscribe (after replay)
};

struct StreamNotification {
    SubscriptionId  subscription;
    bool            replay;      // true while catching up from a cursor/beginning
    SessionEnvelope envelope;
    EventCursor     cursor;      // position AFTER `envelope.event` (T21)
};

struct UnsubscribedNotice {
    SubscriptionId subscription;
    std::string    reason;   // "replay_complete" | "session_closed"
                             // | "unsubscribed" | "superseded"
};

} // namespace ymh::protocol
```

Semantics (T6, T7, T8, T17):

- `from = now` (default): the client receives only events committed **after**
  the subscribe completes. This is the "attach replays nothing implicitly" rule
  of 04 §6.3.
- `from = beginning`: the daemon streams the session's **entire committed**
  resolved view in order (`replay: true`), then switches to live
  (`replay: false`) with no gap and no duplicate. It is atomic: subscription
  registration and the head capture happen under the session's append
  serialization (01 I18, 02 §8).
- `from = cursor(c)`: the daemon streams exactly the committed events **after**
  the event identified by `c`, in order, then live. An unresolvable cursor is
  `CursorInvalid` (T16).
- The daemon streams an event only **after** it is committed (01 I4, 02 §6.1):
  durable-before-observable holds on the wire (T7).
- `event.unsubscribe` (or detach, or `session.close`/`session.delete`) ends the
  subscription. Every teardown is signalled by `event.unsubscribed{reason}`
  after the mailbox drains, so the client never has to infer why a stream
  stopped: `"unsubscribed"` (explicit), `"session_closed"` (delete/close),
  `"replay_complete"` (a `session.replay` finished), `"superseded"` (reconnect
  replaced the connection). On a delete the daemon drains the mailbox, delivers
  `SessionEnded`, then sends `event.unsubscribed{reason:"session_closed"}`
  followed by `HostNotice{SessionClosed}` (§8.4, F3).

**`cursor(E)` (pinned, T21).** For a delivered event `E`, `cursor(E)` is the
opaque token for the position **immediately after** `E` in the session's
committed log. It is carried in every `StreamNotification` as `cursor`, so a
client always holds a resume token for the last event it processed without an
extra round trip. A client that has received `E` resumes with
`event.subscribe{ from: cursor(E) }`; the daemon then delivers exactly the
events after `E`. `SubscribeResult.cursor` is `cursor(head)` at subscribe time,
so a client that stores it before processing any live event can resume from
there. The implementation may derive the token from `EventId` (or an encoded
position); clients treat it as opaque (§5.5).

---

## 8. Event multiplexing and ordered delivery

### 8.1 Per-session ordered delivery

The delivery contract is 01 I6 / §8.3, promoted to the wire (T6):

```text
events for session X are delivered in publish order
events for different sessions may interleave arbitrarily
global cross-session ordering is not guaranteed and is not required
```

The daemon backs each subscription with a per-session mailbox (01 §5,
`SessionMailbox`). A client subscribed to sessions A and B may receive A's and
B's events interleaved, but each session's subsequence is strictly in publish
order. The transport never reorders within a session and never merges distinct
message ids.

### 8.2 Fan-out to N clients

Multiple supervisors may attach to one daemon (04 §6.3, §6.6, T14). Each
connection has:

```text
one outbound frame queue (bounded)
one set of subscriptions (bounded by max_subscriptions_per_client)
one ClientId; its own ClientInstanceId for reconnect supersede
```

The daemon fans out each committed event to every subscribed client. One
client's slow or dropped connection **never** affects another client and never
affects the session (04 §9, D-F9, T19).

### 8.3 Backpressure and ring buffers

The daemon's loop never blocks on a slow client (04 H9, D-F9). Each client's
outbound queue is bounded by `max_outbound_bytes` (default 8 MiB). On overflow:

```text
queue full -> drop the client (detach)
           -> session keeps running; other clients unaffected
           -> the client reconnects and resumes via cursor (no loss)
```

This is the transport's expression of F5: the transport keeps **no** unbounded
replay buffer per client. Catch-up is served from the **durable log**
(02 §4.6 `read(after)`), not from an in-memory ring. The daemon's per-session
output rings (04 §2.1 `session_output_ring_bytes`, §9.11) are a separate,
tool-output concern; the transport does not rely on them for event delivery.

### 8.4 Late-event-after-close (F3)

Detach never closes a session (04 H9, 01 I16). When a session is **deleted** or
the client's subscription ends, the transport guarantees the mailbox is drained
before the stream ends (01 I20, §8.3):

```text
session.delete:
  1. daemon appends SessionEnded{Deleted}  (01 I16)  -> streamed (durable, ordered)
  2. drain the subscription mailbox       (no late event silently dropped)
  3. send event.unsubscribed{reason:"session_closed"}  (ends the stream explicitly)
  4. send HostNotice{SessionClosed}        (lets the client finish its tombstone)
  5. remove the subscription; optional registry junction removal (03 R16)
```

The supervisor-side tombstone that retains late events until the handle is
terminal is spec 10 (§9.8); the transport's obligation is only to deliver in
order and to signal closure explicitly, never to drop silently (T-F17). On a
plain detach no tombstone is needed: the daemon survives and re-attach returns
the current state (04 §6.4).

### 8.5 Cursors, reconnect, and no loss / no dup

The cursor model (T8, T16):

```text
client processes envelope E, remembers cursor(E)
connection drops
client reconnects (same ClientInstanceId -> supersede)
client calls event.subscribe{ session, from: cursor(E) }
daemon streams exactly the committed events after E, then live
=> no gap, no duplicate
```

- A cursor is **daemon-instance-independent**: a daemon started after a crash or
  restart resolves the same cursor (T23, §5.5). A cursor is only valid for the
  session that produced it and only while the underlying log position exists.
  After a `session.delete` (log erased) or an implementation-defined retention
  prune, it is `CursorInvalid` and the client must re-subscribe with
  `beginning` (T16). Retaining erased positions is deferred (OQ-5).
- `event.subscribe{beginning}` streams the committed view and then goes live;
  `session.replay` streams the committed view only and ends with
  `event.unsubscribed{ reason: "replay_complete" }` (it is the read-only CLI
  path, 01 I19). Both set `replay: true` while catching up.
- Every `event.stream` notification carries `cursor(E)` (§7.7), so the token a
  client resumes from is always the position after the last event it actually
  processed — not a token it must separately track.
- The transport never buffers "the last N events" for a client; it replays from
  the store. This is what makes a slow/dropped client recoverable without an
  unbounded daemon-side buffer (F5, D-F9).

### 8.6 Request/response correlation

Requests and responses share the connection's byte stream with notifications.
The `id` is the sole correlation key (T9): a response or error is matched to its
request by exact `id` equality, and notifications (no `id`) are never matched.
The daemon may interleave responses and notifications on one connection; a
client MUST NOT assume a response arrives before or after any notification. A
request's side effects are not ordered relative to another connection's stream
(D20).

---

## 9. Concurrency and threading

- **One connection per supervisor daemon-attach; one Asio `io_context` in the
  daemon** (04 §3.4, §9, §35). The transport adds no threads: reads, writes,
  dispatch, and fan-out run on the daemon's main loop.
- **Dispatch is serialized per connection.** Two requests on one connection are
  handled in arrival order; a mutating method serializes through the session's
  `appendMutex_` (01 I18) and the store's writer mutex (02 §8).
- **Ordering is per session, not per connection.** A slow handler for session A
  must not delay session B's stream; the per-session mailbox isolates them
  (01 §5, T6).
- **No blocking waits on clients.** Permission requests and responses are
  asynchronous; a missing client is handled by policy (09, F2), never by
  blocking the loop (T19, 04 D-F8).
- **Shutdown** is ordered: stop accepting → drain in-flight frames → close
  connections → (04 §3.5 continues). Frames already accepted are answered or
  rejected with `ShutdownInProgress`; none is silently dropped (T-F19).

---

## 10. Invariants

Numbered `T1`–`T24` (see the naming note at the top). Any code that can violate
one is a defect.

**T1 — Framing is exact.** Every frame is a 4-byte **big-endian** unsigned
length followed by exactly that many UTF-8 JSON bytes. No other framing exists
(§3.3).

**T2 — Frame bounds.** `0 < length <= max_frame_bytes`; oversized and zero-length
frames are rejected, never truncated. Histories are streamed, not packed into one
frame (§3.3, §8.5).

**T3 — Handshake first.** No method except `host.hello` is dispatched before a
successful hello; a violation is `HandshakeRequired` and a close (§4.1).

**T4 — Core events only on the wire.** The wire carries the core `Event` (§8.2);
`UiEvent` and every frontend type are never serialized (§20.6, §54 D15).

**T5 — Envelope is exactly `{session, event}`.** `SessionEnvelope` duplicates no
`Sequence`; no numeric `Sequence` appears in any wire DTO (01 §4.6, §20.22).

**T6 — Per-session ordered delivery.** For a session, notifications are
delivered in publish order; cross-session interleaving is unconstrained; global
ordering is not guaranteed (01 I6, §8.3).

**T7 — Durable-before-observable.** An event is streamed only after the store has
committed it (01 I4, 02 P5/§6.1).

**T8 — No loss, no duplicate across reconnect.** A cursor resume delivers exactly
the committed events after the cursor, in order, then live (§8.5).

**T9 — Request/response id correlation.** Every response/error echoes its
request's `id` verbatim; a duplicate in-flight id is `InvalidRequest` (§5.4).

**T10 — Profile gating.** A method outside the negotiated profile returns
`MethodNotAllowedForProfile` before any side effect (§6.3).

**T11 — Bounded outbound per client.** Each connection has a bounded queue; on
overflow the **client** is dropped, never the session (04 D-F9, §8.3).

**T12 — Reconnect supersedes.** A hello with a known `ClientInstanceId` replaces
the prior connection and its subscriptions (04 §6.3, D-F22, §4.4).

**T13 — Detach ≠ shutdown.** Closing a connection or `host.detach` never emits
`SessionEnded` and never stops the daemon (04 H8/H9, 01 I16).

**T14 — Fan-out isolation.** One client's slowness/drop does not affect another
client or any session (04 §9, T19).

**T15 — Auth boundary.** Only a same-UID peer on the owner-only socket may
complete a handshake; otherwise `AuthFailed` and close (§4.2).

**T16 — Cursor validity.** An unresolvable cursor is `CursorInvalid`; the
transport never silently resets to `now` (§8.5).

**T17 — No implicit replay.** `event.subscribe` defaults to `now`; a client must
explicitly request `beginning`/`cursor` to receive history (04 §6.3, §7.7).

**T18 — Recoverable parse errors.** A malformed JSON body yields `ParseError`
without necessarily closing; a malformed **length prefix** closes the connection
(§3.3).

**T19 — The loop never blocks on a client.** No transport operation blocks the
daemon's accept loop or another session's work (04 H9, D20, §9).

**T20 — Unix socket is the deployment transport.** stdio is debug/test only; TCP
is deferred (§43, §47).

**T21 — Every stream notification carries a post-event cursor.** Each
`event.stream` carries `cursor(E)`, the opaque position immediately after the
delivered event, so a client always has a resume token for the last event it
processed (§7.7, §8.5).

**T22 — Wire enum strings are pinned.** Every transport enum serializes to
exactly one lower-case string; profile strings match 01 §3's `server_profile`
values (§2.1). Golden fixtures depend on this.

**T23 — Cursors are durable and daemon-instance-independent.** A cursor names a
committed log position and is resolvable by any daemon instance serving that
session; it is not tied to a `ClientId`, `SubscriptionId`, or `host_boot_id`.
Only erasure/prune invalidates it (§5.5, §8.5).

**T24 — `HostNotice` method is `host.event`.** Control notices are delivered as
the `host.event` notification with `params = HostNotice`; they are never mixed
with `event.stream` session events (§5.3).

---

## 11. Failure modes

### 11.1 Shared findings (F1–F12, §54)

The transport's responsibilities for the existing findings:

| F# | Finding | Transport-layer handling |
|---|---|---|
| **F1** | path/process isolation | the socket lives under the daemon's canonical `<workspace>/.ymh/`; the transport never resolves a path from `getcwd()` and never crosses workspaces on one socket (T20, §3.1) |
| **F2** | background permission | a permission request for a detached/background session is never a blocking wait; policy (09) decides, transport surfaces (T19, §7.6) |
| **F3** | late event after close | the mailbox is drained and `SessionEnded` delivered before the stream closes; no silent drop (T-F17, §8.4) |
| **F4** | edge-triggered attention | out of scope; attention is derived in the UI from core events (spec 10, §20.23) |
| **F5** | output ring buffers | the transport keeps no unbounded per-client replay buffer; catch-up reads the durable log (T11, §8.3) |
| **F6** | input/keybinding focus | out of scope; focus is supervisor-local and never sent on the wire (04 §6.6, §20.22) |
| **F7** | per-session dirty flags | out of scope; dirty flags are the UI model's (§20.23) |
| **F8** | resource caps | the transport bounds connections, subscriptions, and outbound bytes; LLM/subprocess/PTY caps stay per-host in the daemon (04 H16, §9.11) |
| **F9** | cancellation scoping | `agent.cancel` names one session and cancels only its turn (T-F18, §7.5) |
| **F10** | resume-suspended | `session.resume` returns `Idle`; the transport never auto-runs a resumed session (01 §9.2) |
| **F11** | subagent ID duality | subagents are ordinary sessions with their own `SessionId`; the wire never rewrites a subagent id to its parent (01 I17, §20.25) |
| **F12** | flash clock in model | out of scope; no timers in the transport beyond ping/idle (spec 10) |

**Explicitly out of scope for this component:** F4, F6, F7, F12 (UI), and the
policy halves of F2/F8/F10/F11. The transport supplies the ordered, bounded,
authenticated pipe those components use.

### 11.2 Component-local failure modes (`T-F#`)

Component-local to the transport; not part of the top-level F1–F12 set. Covered
by §13.6.

| T-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **T-F1** | Corrupt/implausible length prefix | `length == 0` or `> max_frame_bytes` | `InvalidRequest`/`FrameTooLarge` then close; never parse a partial body (§3.3) |
| **T-F2** | Body not valid UTF-8 | UTF-8 validation fails | `ParseError`, close (framing cannot be trusted) (§3.3) |
| **T-F3** | Body not valid JSON | JSON parse fails | `ParseError` (-32700); connection may continue (§3.3) |
| **T-F4** | Non-object / batch JSON | top-level value is array or scalar | `InvalidRequest` (-32600); batches unsupported (§5.4) |
| **T-F5** | Unknown method | method not in catalog | `MethodNotFound` (-32601) (§7) |
| **T-F6** | Invalid params | schema/type/size validation fails | `InvalidParams` (-32602); no side effect (§7.1) |
| **T-F7** | Handshake missing / out of order | non-hello before hello, or hello timeout | `HandshakeRequired` + close (T3) (§4.1) |
| **T-F8** | Protocol version mismatch | `protocol_version != kProtocolVersion` | `UnsupportedProtocol` + close (§4.1) |
| **T-F9** | Auth failure | peer uid ≠ daemon uid | `AuthFailed` + close (T15) (§4.2) |
| **T-F10** | Client disconnect mid-request | read EOF / socket error | drop `ClientId`; daemon and sessions continue (T13) (§3.2) |
| **T-F11** | Slow/stuck client | outbound queue full | drop the client (detach); session continues (T11, 04 D-F9) (§8.3) |
| **T-F12** | Reconnect supersede | hello with a known `ClientInstanceId` | replace the old connection/subscriptions (T12, 04 D-F22) (§4.4) |
| **T-F13** | Cursor not found | cursor session/position unresolvable | `CursorInvalid`; client re-subscribes `beginning` (T16) (§8.5) |
| **T-F14** | Duplicate in-flight request id | id already pending on the connection | `InvalidRequest` (-32600) (§5.4) |
| **T-F15** | Batch request | top-level array | `InvalidRequest`; batches unsupported (§5.4) |
| **T-F16** | Subscription limit exceeded | count > `max_subscriptions_per_client` | `SubscriptionLimit` (-32013); existing subscriptions unaffected (§8.2) |
| **T-F17** | Notification for unknown subscription | `subscription` id unknown | ignore and log; never error the connection, never drop another stream (§8.4) |
| **T-F18** | Profile gating violation | method not allowed by profile | `MethodNotAllowedForProfile` before any side effect (T10) (§6.3) |
| **T-F19** | Shutdown with in-flight frames | `host.shutdown` / SIGTERM while frames pending | answer or reject with `ShutdownInProgress`, then drain and close (04 §3.5) (§9) |
| **T-F20** | Envelope inconsistency | `envelope.session != envelope.event.session_id`, or undecodable payload | client fails loud (mirrors 01 S3); the daemon never emits an inconsistent envelope (§5.2) |
| **T-F21** | Half-open connection | no frame for `idle_timeout` | close the connection (detach); daemon continues (§4.5) |
| **T-F22** | Lease lost mid-stream | 02 `isLeaseHolder` false / `LeaseLost` | reply `LeaseLost` for writes; send `HostNotice{LeaseLost}`; session degrades read-only; daemon keeps serving (02 §5.7, 04 D-F10) |

---

## 12. dsh (DeepSeek Harness) mapping

dsh is the strongest architectural reference (§55). The transport maps onto it
as follows.

| dsh concept | ymh transport | Reference |
|---|---|---|
| Core IPC (JSON-RPC-style service bus) | JSON-RPC 2.0 over length-prefixed Unix socket | §9.6, §43 |
| API-gateway profile (full event stream) | `ServerProfile::Interactive` | §9.6, §6.1 |
| ACP profile (prompt/cancel/permission) | `ServerProfile::Automation` | §9.6, §6.2 |
| Session event multiplexing | `event.subscribe` + `event.stream` with `SessionEnvelope` | §9.6, §20.22 |
| Service discovery | registry row + socket + authenticated handshake | 03 §6, 04 §6, §4 |
| Resume / cursor | `EventCursor` + `event.subscribe{from: cursor}` | §8.5 |
| `turn/start`, `step/start` taxonomy | `Event.type` slash form on the wire | 01 §4.3, §8.1 |
| Permission request/response | `permission.request` / `permission.decide` notifications | §7.6, §19 |

**Deliberate omissions** (accepted for v1, §55): no Cordis-compatible
configuration transport, no WebSocket/browser client, no remote TCP, no
compression/encryption, no batch requests, no offline session cache. The
protocol is transport-agnostic so TCP/SSH can be added without a redesign
(§47, OQ-1).

---

## 13. Test plan

Strategy is §44: unit, integration (fake LLM / fake FS / fake shell), golden,
replay, and a separate live PTY/real-LLM layer (§44, §45). The deterministic
layers run offline against the Fake LLM (§45); the live layer is opt-in and
API-key gated. The transport's unit layer needs no daemon: it exercises the
codec and dispatcher directly.

### 13.1 Unit tests (framing and codec)

Against `FrameCodec` and the JSON-RPC dispatcher, with no socket:

- **Length prefix.** Big-endian round-trip for lengths 1, 255, 256, 65535,
  16 MiB-1; a little-endian-prefixed frame is rejected (T1).
- **Boundaries.** `length == 0` → `InvalidRequest`; `length == max+1` →
  `FrameTooLarge`; `length == max` decodes (T2).
- **Partial frames.** A split header, a split body, and two frames in one buffer
  decode correctly and leave only the incomplete tail (T1).
- **UTF-8.** Invalid UTF-8 body → `ParseError` + close (T-F2).
- **JSON.** Malformed JSON → `ParseError`, connection may continue; non-object
  and array bodies → `InvalidRequest` (T-F3, T-F4).
- **Envelope.** `Request`/`Response`/`ErrorResponse`/`Notification` encode/decode
  round-trip; string and integer ids are echoed verbatim; a duplicate in-flight
  id is rejected (T9, T-F14).
- **Catalog dispatch.** Every catalog method maps to exactly one handler; an
  unknown method → `MethodNotFound` (T-F5).
- **Params validation.** Missing/extra/wrong-type params → `InvalidParams` with
  no side effect (T-F6).
- **Profile gating.** An Automation connection calling `session.activate` or
  `host.shutdown` → `MethodNotAllowedForProfile` before any effect (T10, T-F18).
- **Cursor mapping.** `now`/`beginning`/`cursor` resolve to the right store
  read; an unknown cursor → `CursorInvalid` (T16, T-F13).
- **Error mapping.** Each `HostErrorCode`, store error, and registry error maps
  to the pinned `AppCode` (§7.1).
- **DTO shapes.** `Event` and `SessionEnvelope` JSON fixtures round-trip;
  `UiEvent` has no encoder on the wire path (T4, T5).
- **Trailing bytes.** A complete JSON value followed by a non-whitespace byte →
  `InvalidRequest` + close (T18, T-F4).
- **Null id.** A framing-level error response serializes `"id": null` (§3.3,
  §5.4).
- **Enum encodings.** `wire_name`/`parse_*` round-trip for every transport enum
  and reject unknown strings; profile strings are exactly `"interactive"` /
  `"automation"` (T22).

### 13.2 Integration tests (in-process host, fake client, §45)

Against a real daemon in `foreground == true` (04 §4.3) with a temp store and
registry, driven by a scripted fake client:

- **Handshake.** A non-hello first frame is rejected with `HandshakeRequired`
  and closed; a valid hello returns `ClientId`, workspace, boot id, limits (T3).
- **Version/profile negotiation.** Version mismatch → `UnsupportedProtocol`;
  unknown profile → `InvalidParams` (T-F8).
- **Subscribe ordering.** `from: beginning` on a session with a fixed event
  script yields envelopes in publish order; two sessions interleave but each
  subsequence is ordered (T6, T7).
- **Fan-out.** Two clients subscribe to the same session; both receive the same
  ordered stream; dropping one does not disturb the other (T14).
- **Reconnect supersede.** A second hello with the same `ClientInstanceId`
  replaces the first; the old stream ends; the new one resumes from a cursor
  with no loss/dup (T8, T12).
- **Backpressure.** Fill a fake client's queue; the daemon drops that client and
  the session keeps appending; a reconnect from a cursor catches up (T11).
- **Detach ≠ stop.** Close the client; the daemon is still `Serving`, the
  session keeps appending, and no `SessionEnded` is emitted (T13).
- **Permissions.** A tool hitting `ASK` emits `permission.request`;
  `permission.decide` records a durable `permission/decision` and resumes; a
  detached client does not block (F2, §7.6).
- **Lifecycle.** `session.create/resume/fork/replay/close/delete` map to the
  session manager; `delete` emits `SessionEnded` then `HostNotice{SessionClosed}`
  after drain (01 I16, §8.4).
- **Lease loss.** Force `isLeaseHolder` false; writes fail `LeaseLost` and a
  `HostNotice{LeaseLost}` is sent; reads/replay keep working (T-F22, 02 §5.7).
- **Activation is explicit.** `session.activate`/`suspend` change the active
  slot; attach/detach/focus never call them (04 decision (m)).
- **Post-event cursor resume.** Every `event.stream` carries `cursor(E)`; the
  client resumes from it and receives exactly the subsequent events (T21, T8).
- **Replay subscription.** `session.replay` returns `{subscription, cursor}`,
  streams with `replay:true`, ends with
  `event.unsubscribed{reason:"replay_complete"}`, and does not go live (§7.4).
- **Own-workspace scoping.** `session.list`/`session.show` reject another
  workspace with `UnknownWorkspace` and enrich summaries from the store (§7.3).
- **`host.event`.** A `HostNotice` arrives as `host.event` with the pinned
  `kind` string (T24).
- **`host.detach` closes.** After `host.detach`, the daemon closes the socket
  (§3.2).

### 13.3 Golden tests

- **Wire fixtures.** Canonical byte fixtures for: a hello request/response, a
  `SessionEnvelope` notification, an error response, a permission request, and a
  `host.event` notice are byte-identical across runs (T1, T4).
- **Framing golden.** A recorded session event stream re-framed through
  `FrameCodec` produces a stable byte sequence (T1).
- **Ordering golden.** A fixed multi-session event script produces the same
  per-session notification order on every run (T6).

### 13.4 Replay tests

- **`from: beginning`.** Subscribing to a recorded session streams exactly the
  store's resolved view, then goes live; the projected state matches `replay`
  (§44, 01 §9.4).
- **Cursor resume.** Record a stream, drop the client mid-stream, reconnect
  using the `cursor` of the last received notification, and assert the delivered
  remainder is exactly the events after it — no gap, no duplicate (T8, T21).
- **Cursor survives a daemon restart.** Restart the daemon between the drop and
  the reconnect; the same cursor still resolves and resumes with no gap (T23).
- **Determinism.** The same committed log replayed through the transport yields
  the same envelope sequence every time (01 I7).

### 13.5 Live end-to-end tests (real LLM, PTY-driven, §44)

Milestone-2 gated (§57 Step 13, §58). The live layer starts the **real** `ymh`
binary under a PTY and drives it as a human would:

- Start the TUI under a PTY (`forkpty`/`posix_openpt` or `tmux send-keys` +
  `capture-pane`), create two workspaces, and assert two daemon sockets accept
  Interactive connections (04 §13.5, §43).
- Type a prompt; assert `turn/start`, streamed `assistant/chunk`, `tool/call`,
  `tool/result`, and `turn/end` arrive as ordered `event.stream` notifications
  and render (§44).
- Drive a permission prompt end to end: the request renders, the decision is
  submitted, the durable `permission/decision` appears, and the turn resumes.
- Kill and restart the TUI while the daemon survives; assert the daemon is still
  alive and the reconnecting supervisor resumes each session via cursor with no
  visible gap (04 §6.5, T8).
- Skipped (not failed) without an API key plus an explicit opt-in flag; tolerant
  of model nondeterminism — assert structure and invariants, never prose (§44).
- **Daemonization must not break the harness.** The live PTY driver attaches to
  the supervisor's terminal; the daemon is `setsid`'d and never reads the PTY,
  so `capture-pane` output is unaffected (04 H3, H19).

### 13.6 Failure-mode coverage matrix

| Failure | Test layer | Asserts |
|---|---|---|
| F1 | unit + integration | socket path rooted in the workspace; no `getcwd()` resolution |
| F2 | integration | a detached client never blocks a permission request |
| F3 | integration | drain before stream close; `SessionEnded` delivered; no silent drop |
| F5 | integration | no unbounded per-client buffer; cursor catch-up from the log |
| F8 | unit | connection/subscription/outbound caps enforced |
| F9 | integration | `agent.cancel` scopes to one session |
| F10 | integration | `session.resume` returns `Idle`; no auto-run |
| F11 | integration | subagent id preserved on the wire |
| T-F1 | unit | zero/oversized length → reject + close |
| T-F2 | unit | invalid UTF-8 → `ParseError` + close |
| T-F3 | unit | malformed JSON → `ParseError`, connection continues |
| T-F4 | unit | array/scalar body → `InvalidRequest` |
| T-F5 | unit | unknown method → `MethodNotFound` |
| T-F6 | unit | bad params → `InvalidParams`, no side effect |
| T-F7 | integration | non-hello first frame → `HandshakeRequired` + close |
| T-F8 | unit | version mismatch → `UnsupportedProtocol` |
| T-F9 | unit | uid mismatch → `AuthFailed` (fake peer cred) |
| T-F10 | integration | disconnect mid-request → detach; daemon continues |
| T-F11 | integration | queue overflow → client dropped, session continues |
| T-F12 | integration | reconnect supersede replaces the old stream |
| T-F13 | unit | unknown cursor → `CursorInvalid` |
| T-F14 | unit | duplicate in-flight id → `InvalidRequest` |
| T-F15 | unit | batch → `InvalidRequest` |
| T-F16 | unit | subscription cap → `SubscriptionLimit` |
| T-F17 | integration | unknown-subscription notification ignored |
| T-F18 | unit | profile gating rejects before side effect |
| T-F19 | integration | shutdown drains/rejects in-flight frames |
| T-F20 | unit | inconsistent envelope / bad payload fails loud |
| T-F21 | integration | idle timeout closes the connection |
| T-F22 | integration | lease loss → `LeaseLost` + notice; read-only degrade |

### 13.7 Invariant coverage

| Invariant | Test layer |
|---|---|
| T1 framing exact / endianness | unit |
| T2 frame bounds | unit |
| T3 handshake first | integration |
| T4 core events only | unit |
| T5 envelope shape / no sequence | unit |
| T6 per-session ordering | integration + golden |
| T7 durable-before-observable | integration |
| T8 no loss/dup on resume | replay |
| T9 id correlation | unit |
| T10 profile gating | unit |
| T11 bounded outbound | integration |
| T12 reconnect supersede | integration |
| T13 detach ≠ shutdown | integration |
| T14 fan-out isolation | integration |
| T15 auth boundary | unit |
| T16 cursor validity | unit |
| T17 no implicit replay | integration |
| T18 recoverable parse errors | unit |
| T19 loop never blocks | integration |
| T20 unix socket is the transport | unit |
| T21 post-event cursor in every stream | integration + replay |
| T22 wire enum strings pinned | unit + golden |
| T23 cursor durable / daemon-instance-independent | replay |
| T24 `HostNotice` method is `host.event` | integration + golden |

---

## 14. Decisions and open questions

### 14.1 Decisions (pinned by this spec)

- **(a) Framing is a 4-byte big-endian length prefix plus a UTF-8 JSON body.**
  The endianness is network byte order; a little-endian reader is a defect
  (§3.3, T1).
- **(b) The wire DTO is the core, durable `Event` (§8.2); `UiEvent` is never on
  the wire.** Frontends adapt through `UiEventAdapter` (§20.6, §54 D15, T4).
- **(c) `SessionEnvelope` is exactly `{session, event}`, and the cursor is
  opaque.** No numeric `Sequence` appears in any wire DTO; the daemon maps
  `EventCursor` to a store `Sequence` internally (01 §4.6, §20.22, T5).
- **(d) `event.subscribe` defaults to `now`; history requires an explicit
  `beginning`/`cursor`.** Attach replays nothing implicitly (04 §6.3, T17).
- **(e) Profiles are negotiated once in `host.hello` and are immutable.**
  Interactive exposes the full catalog and stream; Automation is restricted
  (§6, T10).
- **(f) Authentication is same-UID `SO_PEERCRED` on the owner-only socket, plus
  the mandatory hello handshake.** No bearer token in v1 (§4.2, T15).
- **(g) Reconnect supersedes via `ClientInstanceId`.** A hello with a known
  instance replaces the prior connection and its subscriptions (04 §6.3,
  D-F22, T12).
- **(h) Each client has a bounded outbound queue; overflow drops the client,
  never the session.** Catch-up is from the durable log, not an in-memory ring
  (F5, 04 D-F9, T11).
- **(i) Batch requests are not supported.** A top-level array is
  `InvalidRequest` (§5.4, T-F15).
- **(j) The final socket convention is `<workspace>/.ymh/host.sock`.** This
  adopts the v1 default of 03 §6.5 / 04 §5.1 and resolves 04 OQ-3; a future
  shortening/abstract scheme is OQ-1 (§3.1).
- **(k) stdio is a debug/test transport only; the Unix socket is the deployment
  transport.** TCP/SSH is deferred (§43, §47, T20).
- **(l) Permissions are a `permission.request` notification plus a
  `permission.decide` request; the durable outcome is `permission/decision`.**
  The transport never blocks on a client (F2, §7.6).
- **(m) `agent.*` maps directly to the §10.1 `Agent` surface; the inbox
  (`followup`) is owned by spec 06.** `prompt`/`steer`/`inject` are the §10.1
  `send`/`steer`/`inject` (§7.5).
- **(n) `session.activate`/`session.suspend` are explicit controls, not a mirror
  of UI focus; attach/detach/focus never call them.** One active session per
  workspace (04 decision (m), §7.4).
- **(o) The daemon never emits an inconsistent `SessionEnvelope`; a client
  fails loud on one** (mirrors 01 S3, T-F20).
- **(p) Every `event.stream` carries a post-event cursor.** `cursor(E)` is the
  opaque position immediately after the delivered event; a client resumes from
  the last received notification's cursor. `SubscribeResult.cursor` is
  `cursor(head)` at subscribe time (§7.7, T21).
- **(q) `session.replay` establishes a subscription and streams on it.** Its
  result is `{subscription, cursor}`; it streams the committed view with
  `replay: true`, ends with `event.unsubscribed{reason:"replay_complete"}`, and
  does not go live (§7.4).
- **(r) Cursors are durable and daemon-instance-independent.** A cursor names a
  committed log position and is resolvable by any daemon instance serving that
  session; only erasure/prune invalidates it (§5.5, §8.5, T23).
- **(s) Wire enum strings are pinned and aligned with 01 §3.** `ServerProfile`
  is `"interactive"`/`"automation"`; every other transport enum has one
  canonical lower-case string (§2.1, T22).
- **(t) `session.list`/`session.show` are scoped to the daemon's own
  workspace.** A daemon has one store (04 H1/H6); summaries are enriched from
  that store, and only `ordinal`/`archived` come from the registry junction
  (§7.3).
- **(u) Framing-level errors carry `"id": null`.** `RequestId` includes
  `std::monostate` so an error produced before an id is known can be encoded
  (§2.1, §3.3, §5.4).
- **(v) `host.hello` is the handshake; `host.attach` is an idempotent status
  re-confirm; `host.detach` removes the `ClientId` and then closes the
  connection.** The client must re-handshake after detach (§3.2, §7.2).

### 14.2 Open questions

- **OQ-1 — TCP/SSH transport and its authentication (deferred).** §47 Mode B
  needs a network transport with authentication (same-UID does not cross hosts)
  and possibly compression/encryption. The framing and method catalog are
  transport-agnostic, so only the accept/connect layer and auth change (§3.5,
  §47, HANDOFF §5 item 4). A changed socket convention (abstract namespace or
  hashed short path) must be applied in 04 §5, 03 §6.5, and §3.1 together.
- **OQ-2 — `max_frame_bytes` numeric default (value only; the invariant is
  pinned).** The invariant `max_frame_bytes >= max_payload_bytes + overhead` is
  pinned in §3.3. Only the numeric value is open: 8 MiB is provisional and
  should be finalized with spec 07's tool-output caps so the largest legal event
  is always representable (§3.3).
- **OQ-3 — Automation profile method subset (ACP parity).** The exact set is
  provisional pending spec 09 (permissions) and spec 06 (inbox); the Interactive
  catalog is authoritative and Automation is a strict subset (§6.2).
- **OQ-4 — Permission fan-out with multiple supervisors.** Pinned provisionally
  as "broadcast; first decision wins"; the authoritative policy is spec 09
  (§7.6).
- **OQ-5 — Tombstone retention of erased cursor positions (durability
  resolved).** Cursor durability across daemon restart is pinned (decision (r),
  T23). What remains open is only whether the daemon should retain a bounded
  tombstone of **erased** positions (after a delete/prune) so a late resume is
  distinguishable from corruption, or always force `beginning`. The store keeps
  no per-client retention (§8.5).
- **OQ-6 — Connection-level rate limiting.** The transport bounds bytes and
  subscriptions but has no per-client request-rate limit; whether one is needed
  to protect the daemon from a misbehaving local client is deferred (§8.3).

---

## 15. References

- `00-architecture.md` §4.1 (agent ≠ UI), §4.2 (event stream is the runtime
  spine), §8.1–§8.3 (event system, `SessionId`-routed delivery), §9.6
  (supervisor/daemon topology, the wire), §9.7 (isolation, write lease), §9.8
  (detach/delete/archive), §9.9 (background execution, permission policy), §9.10
  (shared registry, host socket), §9.11 (resource limits), §10.1 (`AgentHandle`),
  §19 (permissions), §20.5–§20.6 (UI events, `UiEventAdapter`), §20.22
  (`SessionEnvelope`, `WorkspaceModel`), §20.23 (attention), §34 (cancellation),
  §35 (concurrency), §43 (RPC mode), §44 (testing strategy), §45 (Fake LLM),
  §47 (SSH strategy), §54 (D1–D23, F1–F12), §55 (dsh comparison), §57 Step 13,
  §58 (milestones).
- `01-session.md` §2.1 (identifiers), §4.1–§4.6 (event model, `Event`,
  `EventType`, `EventRecord`, no sequence on the wire), §5 (`EventBus` mailbox),
  §6 (`Session`), §8 (`SessionManager`), §9.1–§9.5 (create/resume/fork/replay/
  close), §12 (I1–I23), §13.2 (S1–S14).
- `02-persistence.md` §4.1 (config, `max_payload_bytes`), §4.2 (`SessionStore`
  seam), §4.4 (`append`), §4.6 (`read(after)`), §5 (write lease), §5.7
  (lease-loss), §8 (concurrency), §10.2 (P-F#).
- `03-workspace-registry.md` §4.2 (`WorkspaceRegistry` interface), §6.1–§6.5
  (host claim, heartbeat, liveness, socket derivation), §11 (R1–R16), §12.2
  (R-F#).
- `04-workspace-host-daemon.md` §2.2 (`HostErrorCode`), §3.2–§3.5 (spawn/serve/
  shutdown), §4.2 (`WorkspaceHost`, activate/suspend, `onClientAttached`), §5
  (socket bind, path length), §6.3–§6.6 (attach/detach, multiple supervisors),
  §9 (concurrency), §10 (H1–H21), §11.2 (D-F#), §14.2 (OQ-3).
- `HANDOFF.md` §2 (the rule), §6 row 05 (component plan), §7 (definition of
  verified).
- `DESIGN_STATUS.md` (written/verified tracker).

#pragma once

// Transport wire types pinned by docs/design/05-transport.md (component 05).
//
// Everything here lives in `ymh::protocol` exactly as the spec sketches it.
// The wire DTO is the core, durable `Event` (`ymh/core/event.hpp`, 00 §8.2 /
// 01 §4.2); `UiEvent` is never serialized (05 §5.1, T4). No numeric `Sequence`
// appears in any wire DTO (T5): positions travel as the opaque `EventCursor`.
//
// The file also mirrors the small set of identifiers the spec reproduces from
// 03/04 (`WorkspaceId`, `HostPid`, `HostBootId`, `HostState`, `HostErrorCode`)
// inside this namespace, so the transport compiles before those components
// land. When 03/04 ship their canonical `ymh::` versions, the DTO field types
// stay wire-identical; only the C++ spelling would change.

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"

namespace ymh::protocol {

// ---------------------------------------------------------------------------
// Identifiers and value types (05 §2.1)
// ---------------------------------------------------------------------------

// Opaque stream position. The wire never carries a numeric Sequence; the
// daemon maps the token to a store Sequence internally (decision (c), T23).
struct EventCursor {
    std::string value;

    auto operator<=>(const EventCursor&) const = default;
};

// Supervisor-process identity, minted once per TUI process (UUIDv4). Used to
// supersede a stale connection on reconnect (05 §4.4, T12).
struct ClientInstanceId {
    std::string value;

    auto operator<=>(const ClientInstanceId&) const = default;
};

// Daemon-assigned, connection-scoped handle for fan-out bookkeeping.
struct ClientId {
    std::uint64_t value{0};

    auto operator<=>(const ClientId&) const = default;
};

// Server-assigned handle for one (session, client) stream subscription.
struct SubscriptionId {
    std::uint64_t value{0};

    auto operator<=>(const SubscriptionId&) const = default;
};

// Mirrors 03 §2.1. UUIDv4; stable; never a path.
struct WorkspaceId {
    std::string value;

    auto operator<=>(const WorkspaceId&) const = default;
};

// Mirrors 03 §2.1. > 0 always; 0 is never stored.
using HostPid = std::int32_t;

// Mirrors 03 §2.1 / 04 §2.1. UUIDv4 minted at daemon startup.
struct HostBootId {
    std::string value;

    auto operator<=>(const HostBootId&) const = default;
};

// Mirrors 04 §4.4.
enum class HostState : std::uint8_t {
    Starting,
    Serving,
    Draining,
    Stopped,
    Failed,
};

// Mirrors 04 §2.2. Mapped to AppCode by §7.1 (see `app_code_for_host_error`).
enum class HostErrorCode : std::uint8_t {
    AlreadyRunning,
    WorkspaceMissing,
    StoreUnavailable,
    RegistryUnavailable,
    SocketUnavailable,
    SocketPathTooLong,
    LogSinkUnwritable,
    NotServing,
    ShutdownInProgress,
    AttachRejected,
    HostUnreachable,
};

// 16 §7.4 (16-D3). A client's declared role, asserted at hello. Self-asserted
// and advisory: ownership liveness trusts same-UID peers (§7.4 O-L8).
enum class ClientRole : std::uint8_t {
    Supervisor,   // interactive TUI; registers, owns the daemon set
    Automation,   // `ymh run`; attaches, never registers (§4.5)
    Observer,     // `ymh workspace stop`; owns nothing (§4.6)
};

// Transport mirror of ymh::ShutdownReason (workspace_host.hpp), alongside the
// existing HostState / HostErrorCode mirrors. The daemon maps it one-to-one
// (16 §7.4). Total and 1:1 with the six daemon reasons.
enum class ShutdownReason : std::uint8_t {
    ClientRequest,    // host.shutdown with no recognized reason (admission-checked)
    Signal,           // SIGTERM / SIGINT
    StartupFailure,   // a startup step failed after partial state was created
    LastSupervisor,   // the last owner's confirmed exit (§4.4)
    NoOwners,         // owner watchdog: K(d) false for owner_grace (§5.1)
    WorkspaceStop,    // `ymh workspace stop` administrative override (§4.6)
};

// JSON-RPC 2.0 request id: integer or string, echoed verbatim (T9).
// `std::monostate` encodes JSON `null`, used only for error responses produced
// before a request id exists (framing-level errors: 05 §3.3, §5.4).
struct RequestId {
    std::variant<std::monostate, std::int64_t, std::string> value;

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::monostate>(value);
    }
    [[nodiscard]] bool is_integer() const noexcept {
        return std::holds_alternative<std::int64_t>(value);
    }
    [[nodiscard]] bool is_string() const noexcept {
        return std::holds_alternative<std::string>(value);
    }
};

// Stable machine-readable key for duplicate-in-flight detection.
[[nodiscard]] std::string request_id_key(const RequestId& id);

enum class ServerProfile : std::uint8_t {
    Interactive,   // wire "interactive"; full event stream (§9.6)
    Automation,    // wire "automation"; prompt/cancel/permission (§9.6)
};

inline constexpr std::uint32_t kProtocolVersion = 1;

// ---------------------------------------------------------------------------
// Protocol error taxonomy (05 §2.2)
// ---------------------------------------------------------------------------

// JSON-RPC 2.0 standard codes.
enum class RpcCode : int {
    ParseError     = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603,
};

// ymh application codes (server range -32000…-32099).
enum class AppCode : int {
    HandshakeRequired          = -32000,
    UnsupportedProtocol        = -32001,
    AuthFailed                 = -32002,
    NotServing                 = -32003,
    ShutdownInProgress         = -32004,
    UnknownWorkspace           = -32005,
    UnknownSession             = -32006,
    LeaseLost                  = -32007,
    InvalidForkBoundary        = -32008,
    DependentSession           = -32009,
    FrameTooLarge              = -32010,
    CursorInvalid              = -32011,
    MethodNotAllowedForProfile = -32012,
    SubscriptionLimit          = -32013,
    PayloadTooLarge            = -32014,
    StoreUnavailable           = -32015,
    RegistryUnavailable        = -32016,
    PermissionDenied           = -32017,
    SessionNotActive           = -32018,
    NotLastOwner               = -32019,  // any non-override host.shutdown refused (§4.4)
};

[[nodiscard]] constexpr int code_value(RpcCode code) noexcept {
    return static_cast<int>(code);
}
[[nodiscard]] constexpr int code_value(AppCode code) noexcept {
    return static_cast<int>(code);
}

// 05 §7.1: the daemon's HostErrorCode maps to exactly one RpcCode|AppCode.
// Startup-only codes never reach the wire; they are mapped for completeness.
[[nodiscard]] int rpc_code_for_host_error(HostErrorCode code) noexcept;

// ---------------------------------------------------------------------------
// Limits (05 §3.3)
// ---------------------------------------------------------------------------

struct TransportLimits {
    std::size_t             max_frame_bytes{8u * 1024u * 1024u};
    std::size_t             max_outbound_bytes{8u * 1024u * 1024u};
    std::size_t             max_subscriptions_per_client{64};
    std::chrono::milliseconds handshake_timeout{5'000};
    std::chrono::milliseconds idle_timeout{30'000};
};

// D5: the supervisor's `host.ping` cadence. Three missed pings fit inside the
// daemon's 30 s `idle_timeout`, matching the registry heartbeat ratio.
inline constexpr std::chrono::milliseconds kPingInterval{10'000};

// ---------------------------------------------------------------------------
// Wire DTOs (05 §5)
// ---------------------------------------------------------------------------

// Exactly 05 §20.22 / §5.2: `{session, event}`. `session` is retained for
// routing even though `Event` carries `session_id`.
struct SessionEnvelope {
    SessionId session;
    Event     event;
};

enum class HostNoticeKind : std::uint8_t {
    SessionClosed,       // a session was deleted/closed; subscription ends
    SessionCreated,      // the open set changed (daemon-driven)
    LeaseLost,           // 02 §5.7: a session degraded to read-only
    DaemonShuttingDown,  // host.shutdown accepted; drain begins
    McpServerStatus,     // 15 §4.7 (AM-1): bounded MCP status token in `detail`
};

struct HostNotice {
    HostNoticeKind           kind{HostNoticeKind::SessionClosed};
    WorkspaceId              workspace;
    std::optional<SessionId> session;
    std::string              detail;
};

// 05 §6 / §9.6.
enum class PermissionAnswer : std::uint8_t { Allow, Deny };
enum class PermissionScope  : std::uint8_t { Once, Session, Always };

struct PermissionRequest {
    std::string     request_id;     // UUIDv4, correlation only
    SessionId       session;
    std::string     tool;
    nlohmann::json  arguments;
    std::string     summary;
    std::int64_t    expires_at_ms{0};
};

struct PermissionDecisionParams {
    std::string      request_id;
    PermissionAnswer decision{PermissionAnswer::Deny};
    PermissionScope  scope{PermissionScope::Once};
};

struct HostStatus {
    HostState                 state{HostState::Starting};
    WorkspaceId               workspace;
    HostBootId                boot_id;
    HostPid                   pid{0};
    std::size_t               attached_clients{0};
    std::optional<SessionId>  active_session;
    ServerProfile             profile{ServerProfile::Interactive};
};

struct HelloParams {
    std::uint32_t              protocol_version{kProtocolVersion};
    ServerProfile              profile{ServerProfile::Interactive};
    ClientInstanceId           client_instance;
    std::optional<EventCursor> resume_hint;
    ClientRole                 role{ClientRole::Supervisor};  // additive; default preserves v1
};

struct HelloResult {
    std::uint32_t              protocol_version{kProtocolVersion};
    WorkspaceId                workspace;
    HostBootId                 boot_id;
    HostPid                    pid{0};
    std::vector<ServerProfile> profiles;
    ClientId                   client_id;
    TransportLimits            limits;
    std::int64_t               server_time_ms{0};
};

// `host.ownership` (16 §7.4, new read-only method). No request params: the
// daemon uses the calling connection's ClientInstanceId (set at hello), so it
// cannot be spoofed (O-L1).
struct OwnershipView {
    struct ClientInfo {
        std::string client_instance;
        ClientRole  role{ClientRole::Supervisor};
        std::int32_t pid{0};
    };

    std::vector<ClientInfo> clients;             // live, hello-complete, owner roles
    std::size_t live_supervisors{0};             // RAW count INCLUDING the caller (R-M3)
    std::size_t live_automation{0};              // RAW count INCLUDING the caller (R-M3)
    std::size_t other_fresh_owners{0};           // |snapshot − caller|, the only exclusion (R-H1)
    bool        shutting_down{false};            // HostState::Draining
};

struct StreamFrom {
    enum class Kind : std::uint8_t { Now, Beginning, Cursor };
    Kind                       kind{Kind::Now};
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
    bool            replay{false};   // true while catching up from cursor/beginning
    SessionEnvelope envelope;
    EventCursor     cursor;          // position AFTER `envelope.event` (T21)
};

struct UnsubscribedNotice {
    SubscriptionId subscription;
    std::string    reason;   // replay_complete | session_closed | unsubscribed | superseded
};

// 05 §7.3. Enriched from the daemon's own store; only `ordinal`/`archived` come
// from the registry junction (decision (t)). `SessionHeader` (01 §3) is carried
// as JSON here because spec 01 owns its concrete shape.
struct WorkspaceSummary {
    WorkspaceId              id;
    std::string              canonical_path;
    std::string              display_title;
    std::optional<HostPid>   host_pid;
    std::optional<HostBootId> boot_id;
};

struct WorkspaceDetail {
    WorkspaceSummary summary;
    std::int64_t     created_at_ms{0};
    std::int64_t     updated_at_ms{0};
};

struct SessionSummary {
    SessionId    id;
    std::int64_t ordinal{0};
    bool         archived{false};
    std::string  title;
    std::string  kind;            // root | fork | subagent
    std::int64_t updated_at_ms{0};
};

struct SessionDetail {
    SessionSummary  summary;
    nlohmann::json  header;       // SessionHeader (01 §3)
    std::size_t     event_count{0};
};

// ---------------------------------------------------------------------------
// Wire enum encodings (05 §2.1, T22)
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view wire_name(ServerProfile profile) noexcept;
[[nodiscard]] std::optional<ServerProfile> parse_server_profile(std::string_view name) noexcept;

[[nodiscard]] std::string_view wire_name(StreamFrom::Kind kind) noexcept;
[[nodiscard]] std::optional<StreamFrom::Kind> parse_stream_kind(std::string_view name) noexcept;

[[nodiscard]] std::string_view wire_name(HostNoticeKind kind) noexcept;
[[nodiscard]] std::optional<HostNoticeKind> parse_host_notice_kind(std::string_view name) noexcept;

[[nodiscard]] std::string_view wire_name(PermissionAnswer answer) noexcept;
[[nodiscard]] std::optional<PermissionAnswer> parse_permission_answer(std::string_view name) noexcept;

[[nodiscard]] std::string_view wire_name(PermissionScope scope) noexcept;
[[nodiscard]] std::optional<PermissionScope> parse_permission_scope(std::string_view name) noexcept;

[[nodiscard]] std::string_view wire_name(HostState state) noexcept;
[[nodiscard]] std::optional<HostState> parse_host_state(std::string_view name) noexcept;

[[nodiscard]] std::string_view to_string(ClientRole role) noexcept;
[[nodiscard]] std::optional<ClientRole> parse_client_role(std::string_view name) noexcept;

// 16 §7.4 C-M6. Role ↔ profile is pinned: `Automation ⇒ Automation`, and
// `Supervisor`/`Observer ⇒ Interactive`. `profile_for_role` is the derivation
// the attach path uses; `role_matches_profile` rejects a mismatched pair (the
// daemon derives the profile from the role at hello; clients derive the profile
// they send from their role).
[[nodiscard]] ServerProfile profile_for_role(ClientRole role) noexcept;
[[nodiscard]] bool role_matches_profile(ClientRole role, ServerProfile profile) noexcept;

// Total parser with a fail-safe fallback (16 §7.4): only `"last_supervisor"`
// and `"workspace_stop"` are recognized; anything else, including a missing
// reason, maps to ClientRequest (which admission still checks).
[[nodiscard]] ShutdownReason parse_shutdown_reason(std::string_view reason) noexcept;
[[nodiscard]] std::string_view to_string(ShutdownReason reason) noexcept;

// ---------------------------------------------------------------------------
// JSON codecs
// ---------------------------------------------------------------------------

void to_json(nlohmann::json& json, const RequestId& id);
void from_json(const nlohmann::json& json, RequestId& id);

void to_json(nlohmann::json& json, const TransportLimits& limits);
void from_json(const nlohmann::json& json, TransportLimits& limits);

void to_json(nlohmann::json& json, const SessionEnvelope& envelope);
void from_json(const nlohmann::json& json, SessionEnvelope& envelope);

void to_json(nlohmann::json& json, const StreamFrom& from);
void from_json(const nlohmann::json& json, StreamFrom& from);

void to_json(nlohmann::json& json, const SubscribeParams& params);
void from_json(const nlohmann::json& json, SubscribeParams& params);

void to_json(nlohmann::json& json, const SubscribeResult& result);
void from_json(const nlohmann::json& json, SubscribeResult& result);

void to_json(nlohmann::json& json, const StreamNotification& notification);
void from_json(const nlohmann::json& json, StreamNotification& notification);

void to_json(nlohmann::json& json, const UnsubscribedNotice& notice);
void from_json(const nlohmann::json& json, UnsubscribedNotice& notice);

void to_json(nlohmann::json& json, const HelloParams& params);
void from_json(const nlohmann::json& json, HelloParams& params);

void to_json(nlohmann::json& json, const HelloResult& result);
void from_json(const nlohmann::json& json, HelloResult& result);

void to_json(nlohmann::json& json, const HostNotice& notice);
void from_json(const nlohmann::json& json, HostNotice& notice);

void to_json(nlohmann::json& json, const HostStatus& status);
void from_json(const nlohmann::json& json, HostStatus& status);

// 16 §7.4: `host.ownership` result. Raw live counts include the caller; only
// `other_fresh_owners` excludes it.
void to_json(nlohmann::json& json, const OwnershipView& view);
void from_json(const nlohmann::json& json, OwnershipView& view);

void to_json(nlohmann::json& json, const PermissionRequest& request);
void from_json(const nlohmann::json& json, PermissionRequest& request);

void to_json(nlohmann::json& json, const PermissionDecisionParams& params);
void from_json(const nlohmann::json& json, PermissionDecisionParams& params);

void to_json(nlohmann::json& json, const WorkspaceSummary& summary);
void from_json(const nlohmann::json& json, WorkspaceSummary& summary);

void to_json(nlohmann::json& json, const WorkspaceDetail& detail);
void from_json(const nlohmann::json& json, WorkspaceDetail& detail);

void to_json(nlohmann::json& json, const SessionSummary& summary);
void from_json(const nlohmann::json& json, SessionSummary& summary);

void to_json(nlohmann::json& json, const SessionDetail& detail);
void from_json(const nlohmann::json& json, SessionDetail& detail);

// ---------------------------------------------------------------------------
// Method catalog (05 §7)
// ---------------------------------------------------------------------------

namespace method {
inline constexpr std::string_view kHostHello        = "host.hello";
inline constexpr std::string_view kHostAttach       = "host.attach";
inline constexpr std::string_view kHostDetach       = "host.detach";
inline constexpr std::string_view kHostStatus       = "host.status";
inline constexpr std::string_view kHostPing         = "host.ping";
inline constexpr std::string_view kHostShutdown     = "host.shutdown";
inline constexpr std::string_view kHostOwnership    = "host.ownership";
inline constexpr std::string_view kWorkspaceList    = "workspace.list";
inline constexpr std::string_view kWorkspaceShow    = "workspace.show";
inline constexpr std::string_view kSessionList      = "session.list";
inline constexpr std::string_view kSessionShow      = "session.show";
inline constexpr std::string_view kSessionCreate    = "session.create";
inline constexpr std::string_view kSessionResume    = "session.resume";
inline constexpr std::string_view kSessionFork      = "session.fork";
inline constexpr std::string_view kSessionReplay    = "session.replay";
inline constexpr std::string_view kSessionActivate  = "session.activate";
inline constexpr std::string_view kSessionSuspend   = "session.suspend";
inline constexpr std::string_view kSessionCompact   = "session.compact";
inline constexpr std::string_view kSessionClose     = "session.close";
inline constexpr std::string_view kSessionDelete    = "session.delete";
inline constexpr std::string_view kAgentPrompt      = "agent.prompt";
inline constexpr std::string_view kAgentFollowup    = "agent.followup";
inline constexpr std::string_view kAgentSteer       = "agent.steer";
inline constexpr std::string_view kAgentInject      = "agent.inject";
inline constexpr std::string_view kAgentCancel      = "agent.cancel";
inline constexpr std::string_view kAgentStatus      = "agent.status";
inline constexpr std::string_view kPermissionDecide = "permission.decide";
inline constexpr std::string_view kEventSubscribe   = "event.subscribe";
inline constexpr std::string_view kEventUnsubscribe = "event.unsubscribe";
inline constexpr std::string_view kSessionRename    = "session.rename";
inline constexpr std::string_view kSkillsList       = "skills.list";
inline constexpr std::string_view kSkillsShow       = "skills.show";
inline constexpr std::string_view kContextShow      = "context.show";
} // namespace method

// The full catalog, in the order of 05 §7. `host.hello` is first.
[[nodiscard]] std::span<const std::string_view> all_methods() noexcept;

[[nodiscard]] bool is_known_method(std::string_view name) noexcept;

// Profile gating (05 §6.3, T10). Automation denies the operator controls
// (`session.activate`, `session.suspend`, `session.compact`, `host.shutdown`);
// every other catalog method is permitted (see the ambiguity note in the
// implementation report).
[[nodiscard]] bool is_method_allowed(ServerProfile profile, std::string_view method) noexcept;

// ---------------------------------------------------------------------------
// Outbound notification method names
// ---------------------------------------------------------------------------

namespace notify {
inline constexpr std::string_view kEventStream   = "event.stream";
inline constexpr std::string_view kEventUnsubscribed = "event.unsubscribed";
inline constexpr std::string_view kPermissionRequest = "permission.request";
inline constexpr std::string_view kHostEvent     = "host.event";
} // namespace notify

} // namespace ymh::protocol

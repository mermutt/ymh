#include "ymh/transport/protocol.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace ymh::protocol {
namespace {

template <class Enum>
struct WireEnumEntry {
    Enum             value;
    std::string_view name;
};

template <class Enum, std::size_t N>
[[nodiscard]] std::string_view wire_name_of(const std::array<WireEnumEntry<Enum>, N>& table,
                                            Enum value) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> parse_wire_enum(
    const std::array<WireEnumEntry<Enum>, N>& table, std::string_view name) noexcept {
    for (const auto& entry : table) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

constexpr std::array<WireEnumEntry<ServerProfile>, 2> kProfiles{{
    {ServerProfile::Interactive, "interactive"},
    {ServerProfile::Automation, "automation"},
}};

constexpr std::array<WireEnumEntry<StreamFrom::Kind>, 3> kStreamKinds{{
    {StreamFrom::Kind::Now, "now"},
    {StreamFrom::Kind::Beginning, "beginning"},
    {StreamFrom::Kind::Cursor, "cursor"},
}};

constexpr std::array<WireEnumEntry<HostNoticeKind>, 4> kHostNoticeKinds{{
    {HostNoticeKind::SessionClosed, "session_closed"},
    {HostNoticeKind::SessionCreated, "session_created"},
    {HostNoticeKind::LeaseLost, "lease_lost"},
    {HostNoticeKind::DaemonShuttingDown, "daemon_shutting_down"},
}};

constexpr std::array<WireEnumEntry<PermissionAnswer>, 2> kPermissionAnswers{{
    {PermissionAnswer::Allow, "allow"},
    {PermissionAnswer::Deny, "deny"},
}};

constexpr std::array<WireEnumEntry<PermissionScope>, 3> kPermissionScopes{{
    {PermissionScope::Once, "once"},
    {PermissionScope::Session, "session"},
    {PermissionScope::Always, "always"},
}};

constexpr std::array<WireEnumEntry<HostState>, 5> kHostStates{{
    {HostState::Starting, "starting"},
    {HostState::Serving, "serving"},
    {HostState::Draining, "draining"},
    {HostState::Stopped, "stopped"},
    {HostState::Failed, "failed"},
}};

[[nodiscard]] std::int64_t to_ms(std::chrono::milliseconds value) noexcept {
    return value.count();
}

[[nodiscard]] std::chrono::milliseconds from_ms(std::int64_t value) noexcept {
    return std::chrono::milliseconds{value};
}

} // namespace

std::string request_id_key(const RequestId& id) {
    if (id.is_integer()) {
        return "i:" + std::to_string(std::get<std::int64_t>(id.value));
    }
    if (id.is_string()) {
        return "s:" + std::get<std::string>(id.value);
    }
    return "n:";
}

int rpc_code_for_host_error(HostErrorCode code) noexcept {
    switch (code) {
        case HostErrorCode::AlreadyRunning:
            return code_value(RpcCode::InvalidRequest);
        case HostErrorCode::WorkspaceMissing:
            return code_value(AppCode::UnknownWorkspace);
        case HostErrorCode::StoreUnavailable:
            return code_value(AppCode::StoreUnavailable);
        case HostErrorCode::RegistryUnavailable:
            return code_value(AppCode::RegistryUnavailable);
        case HostErrorCode::SocketUnavailable:
        case HostErrorCode::SocketPathTooLong:
        case HostErrorCode::LogSinkUnwritable:
        case HostErrorCode::NotServing:
        case HostErrorCode::HostUnreachable:
            return code_value(AppCode::NotServing);
        case HostErrorCode::ShutdownInProgress:
            return code_value(AppCode::ShutdownInProgress);
        case HostErrorCode::AttachRejected:
            return code_value(AppCode::AuthFailed);
    }
    return code_value(AppCode::NotServing);
}

std::string_view wire_name(ServerProfile profile) noexcept {
    return wire_name_of(kProfiles, profile);
}

std::optional<ServerProfile> parse_server_profile(std::string_view name) noexcept {
    return parse_wire_enum(kProfiles, name);
}

std::string_view wire_name(StreamFrom::Kind kind) noexcept {
    return wire_name_of(kStreamKinds, kind);
}

std::optional<StreamFrom::Kind> parse_stream_kind(std::string_view name) noexcept {
    return parse_wire_enum(kStreamKinds, name);
}

std::string_view wire_name(HostNoticeKind kind) noexcept {
    return wire_name_of(kHostNoticeKinds, kind);
}

std::optional<HostNoticeKind> parse_host_notice_kind(std::string_view name) noexcept {
    return parse_wire_enum(kHostNoticeKinds, name);
}

std::string_view wire_name(PermissionAnswer answer) noexcept {
    return wire_name_of(kPermissionAnswers, answer);
}

std::optional<PermissionAnswer> parse_permission_answer(std::string_view name) noexcept {
    return parse_wire_enum(kPermissionAnswers, name);
}

std::string_view wire_name(PermissionScope scope) noexcept {
    return wire_name_of(kPermissionScopes, scope);
}

std::optional<PermissionScope> parse_permission_scope(std::string_view name) noexcept {
    return parse_wire_enum(kPermissionScopes, name);
}

std::string_view wire_name(HostState state) noexcept {
    return wire_name_of(kHostStates, state);
}

std::optional<HostState> parse_host_state(std::string_view name) noexcept {
    return parse_wire_enum(kHostStates, name);
}

void to_json(nlohmann::json& json, const RequestId& id) {
    std::visit(
        [&json](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                json = nullptr;
            } else {
                json = value;
            }
        },
        id.value);
}

void from_json(const nlohmann::json& json, RequestId& id) {
    if (json.is_null()) {
        id.value = std::monostate{};
    } else if (json.is_number_integer()) {
        id.value = json.get<std::int64_t>();
    } else if (json.is_string()) {
        id.value = json.get<std::string>();
    } else {
        throw std::invalid_argument("request id must be an integer, string, or null");
    }
}

void to_json(nlohmann::json& json, const TransportLimits& limits) {
    json = nlohmann::json{
        {"max_frame_bytes", limits.max_frame_bytes},
        {"max_outbound_bytes", limits.max_outbound_bytes},
        {"max_subscriptions_per_client", limits.max_subscriptions_per_client},
        {"handshake_timeout_ms", to_ms(limits.handshake_timeout)},
        {"idle_timeout_ms", to_ms(limits.idle_timeout)},
    };
}

void from_json(const nlohmann::json& json, TransportLimits& limits) {
    limits.max_frame_bytes = json.at("max_frame_bytes").get<std::size_t>();
    limits.max_outbound_bytes = json.at("max_outbound_bytes").get<std::size_t>();
    limits.max_subscriptions_per_client =
        json.at("max_subscriptions_per_client").get<std::size_t>();
    limits.handshake_timeout = from_ms(json.at("handshake_timeout_ms").get<std::int64_t>());
    limits.idle_timeout = from_ms(json.at("idle_timeout_ms").get<std::int64_t>());
}

void to_json(nlohmann::json& json, const SessionEnvelope& envelope) {
    json = nlohmann::json{{"session", envelope.session.value}, {"event", envelope.event}};
}

void from_json(const nlohmann::json& json, SessionEnvelope& envelope) {
    envelope.session.value = json.at("session").get<std::string>();
    envelope.event = json.at("event").get<Event>();
}

void to_json(nlohmann::json& json, const StreamFrom& from) {
    json = nlohmann::json{{"kind", std::string{wire_name(from.kind)}}};
    if (from.cursor.has_value()) {
        json["cursor"] = from.cursor->value;
    }
}

void from_json(const nlohmann::json& json, StreamFrom& from) {
    const auto kind = parse_stream_kind(json.at("kind").get<std::string>());
    if (!kind.has_value()) {
        throw std::invalid_argument("unknown stream-from kind");
    }
    from.kind = *kind;
    if (json.contains("cursor") && !json.at("cursor").is_null()) {
        from.cursor = EventCursor{json.at("cursor").get<std::string>()};
    } else {
        from.cursor.reset();
    }
}

void to_json(nlohmann::json& json, const SubscribeParams& params) {
    json = nlohmann::json{{"session", params.session.value}, {"from", params.from}};
}

void from_json(const nlohmann::json& json, SubscribeParams& params) {
    params.session.value = json.at("session").get<std::string>();
    if (json.contains("from")) {
        params.from = json.at("from").get<StreamFrom>();
    } else {
        params.from = StreamFrom{};
    }
}

void to_json(nlohmann::json& json, const SubscribeResult& result) {
    json = nlohmann::json{{"subscription", result.subscription.value},
                          {"cursor", result.cursor.value}};
}

void from_json(const nlohmann::json& json, SubscribeResult& result) {
    result.subscription.value = json.at("subscription").get<std::uint64_t>();
    result.cursor.value = json.at("cursor").get<std::string>();
}

void to_json(nlohmann::json& json, const StreamNotification& notification) {
    json = nlohmann::json{{"subscription", notification.subscription.value},
                          {"replay", notification.replay},
                          {"envelope", notification.envelope},
                          {"cursor", notification.cursor.value}};
}

void from_json(const nlohmann::json& json, StreamNotification& notification) {
    notification.subscription.value = json.at("subscription").get<std::uint64_t>();
    notification.replay = json.at("replay").get<bool>();
    notification.envelope = json.at("envelope").get<SessionEnvelope>();
    notification.cursor.value = json.at("cursor").get<std::string>();
}

void to_json(nlohmann::json& json, const UnsubscribedNotice& notice) {
    json = nlohmann::json{{"subscription", notice.subscription.value},
                          {"reason", notice.reason}};
}

void from_json(const nlohmann::json& json, UnsubscribedNotice& notice) {
    notice.subscription.value = json.at("subscription").get<std::uint64_t>();
    notice.reason = json.at("reason").get<std::string>();
}

void to_json(nlohmann::json& json, const HelloParams& params) {
    json = nlohmann::json{{"protocol_version", params.protocol_version},
                          {"profile", std::string{wire_name(params.profile)}},
                          {"client_instance", params.client_instance.value}};
    if (params.resume_hint.has_value()) {
        json["resume_hint"] = params.resume_hint->value;
    }
}

void from_json(const nlohmann::json& json, HelloParams& params) {
    params.protocol_version = json.at("protocol_version").get<std::uint32_t>();
    const auto profile = parse_server_profile(json.at("profile").get<std::string>());
    if (!profile.has_value()) {
        throw std::invalid_argument("unknown server profile");
    }
    params.profile = *profile;
    params.client_instance.value = json.at("client_instance").get<std::string>();
    if (json.contains("resume_hint") && !json.at("resume_hint").is_null()) {
        params.resume_hint = EventCursor{json.at("resume_hint").get<std::string>()};
    } else {
        params.resume_hint.reset();
    }
}

void to_json(nlohmann::json& json, const HelloResult& result) {
    nlohmann::json profiles = nlohmann::json::array();
    for (const ServerProfile profile : result.profiles) {
        profiles.push_back(std::string{wire_name(profile)});
    }
    json = nlohmann::json{
        {"protocol_version", result.protocol_version},
        {"workspace", result.workspace.value},
        {"boot_id", result.boot_id.value},
        {"pid", result.pid},
        {"profiles", std::move(profiles)},
        {"client_id", result.client_id.value},
        {"limits", result.limits},
        {"server_time_ms", result.server_time_ms},
    };
}

void from_json(const nlohmann::json& json, HelloResult& result) {
    result.protocol_version = json.at("protocol_version").get<std::uint32_t>();
    result.workspace.value = json.at("workspace").get<std::string>();
    result.boot_id.value = json.at("boot_id").get<std::string>();
    result.pid = json.at("pid").get<HostPid>();
    result.profiles.clear();
    for (const auto& entry : json.at("profiles")) {
        const auto profile = parse_server_profile(entry.get<std::string>());
        if (!profile.has_value()) {
            throw std::invalid_argument("unknown server profile in hello result");
        }
        result.profiles.push_back(*profile);
    }
    result.client_id.value = json.at("client_id").get<std::uint64_t>();
    result.limits = json.at("limits").get<TransportLimits>();
    result.server_time_ms = json.at("server_time_ms").get<std::int64_t>();
}

void to_json(nlohmann::json& json, const HostNotice& notice) {
    json = nlohmann::json{{"kind", std::string{wire_name(notice.kind)}},
                          {"workspace", notice.workspace.value},
                          {"detail", notice.detail}};
    if (notice.session.has_value()) {
        json["session"] = notice.session->value;
    } else {
        json["session"] = nullptr;
    }
}

void from_json(const nlohmann::json& json, HostNotice& notice) {
    const auto kind = parse_host_notice_kind(json.at("kind").get<std::string>());
    if (!kind.has_value()) {
        throw std::invalid_argument("unknown host notice kind");
    }
    notice.kind = *kind;
    notice.workspace.value = json.at("workspace").get<std::string>();
    if (json.contains("session") && !json.at("session").is_null()) {
        notice.session = SessionId{json.at("session").get<std::string>()};
    } else {
        notice.session.reset();
    }
    notice.detail = json.value("detail", std::string{});
}

void to_json(nlohmann::json& json, const HostStatus& status) {
    json = nlohmann::json{
        {"state", std::string{wire_name(status.state)}},
        {"workspace", status.workspace.value},
        {"boot_id", status.boot_id.value},
        {"pid", status.pid},
        {"attached_clients", status.attached_clients},
        {"profile", std::string{wire_name(status.profile)}},
    };
    if (status.active_session.has_value()) {
        json["active_session"] = status.active_session->value;
    } else {
        json["active_session"] = nullptr;
    }
}

void from_json(const nlohmann::json& json, HostStatus& status) {
    const auto state = parse_host_state(json.at("state").get<std::string>());
    if (!state.has_value()) {
        throw std::invalid_argument("unknown host state");
    }
    status.state = *state;
    status.workspace.value = json.at("workspace").get<std::string>();
    status.boot_id.value = json.at("boot_id").get<std::string>();
    status.pid = json.at("pid").get<HostPid>();
    status.attached_clients = json.at("attached_clients").get<std::size_t>();
    if (json.contains("active_session") && !json.at("active_session").is_null()) {
        status.active_session = SessionId{json.at("active_session").get<std::string>()};
    } else {
        status.active_session.reset();
    }
    const auto profile = parse_server_profile(json.at("profile").get<std::string>());
    if (!profile.has_value()) {
        throw std::invalid_argument("unknown server profile");
    }
    status.profile = *profile;
}

void to_json(nlohmann::json& json, const PermissionRequest& request) {
    json = nlohmann::json{
        {"request_id", request.request_id},
        {"session", request.session.value},
        {"tool", request.tool},
        {"arguments", request.arguments},
        {"summary", request.summary},
        {"expires_at_ms", request.expires_at_ms},
    };
}

void from_json(const nlohmann::json& json, PermissionRequest& request) {
    request.request_id = json.at("request_id").get<std::string>();
    request.session.value = json.at("session").get<std::string>();
    request.tool = json.at("tool").get<std::string>();
    request.arguments = json.value("arguments", nlohmann::json::object());
    request.summary = json.value("summary", std::string{});
    request.expires_at_ms = json.at("expires_at_ms").get<std::int64_t>();
}

void to_json(nlohmann::json& json, const PermissionDecisionParams& params) {
    json = nlohmann::json{{"request_id", params.request_id},
                          {"decision", std::string{wire_name(params.decision)}},
                          {"scope", std::string{wire_name(params.scope)}}};
}

void from_json(const nlohmann::json& json, PermissionDecisionParams& params) {
    params.request_id = json.at("request_id").get<std::string>();
    const auto decision = parse_permission_answer(json.at("decision").get<std::string>());
    if (!decision.has_value()) {
        throw std::invalid_argument("unknown permission decision");
    }
    params.decision = *decision;
    const auto scope = parse_permission_scope(json.at("scope").get<std::string>());
    if (!scope.has_value()) {
        throw std::invalid_argument("unknown permission scope");
    }
    params.scope = *scope;
}

void to_json(nlohmann::json& json, const WorkspaceSummary& summary) {
    json = nlohmann::json{{"id", summary.id.value},
                          {"canonical_path", summary.canonical_path},
                          {"display_title", summary.display_title}};
    json["host_pid"] = summary.host_pid.has_value() ? nlohmann::json(*summary.host_pid)
                                                    : nlohmann::json(nullptr);
    json["boot_id"] = summary.boot_id.has_value() ? nlohmann::json(summary.boot_id->value)
                                                  : nlohmann::json(nullptr);
}

void from_json(const nlohmann::json& json, WorkspaceSummary& summary) {
    summary.id.value = json.at("id").get<std::string>();
    summary.canonical_path = json.at("canonical_path").get<std::string>();
    summary.display_title = json.at("display_title").get<std::string>();
    if (json.contains("host_pid") && !json.at("host_pid").is_null()) {
        summary.host_pid = json.at("host_pid").get<HostPid>();
    } else {
        summary.host_pid.reset();
    }
    if (json.contains("boot_id") && !json.at("boot_id").is_null()) {
        summary.boot_id = HostBootId{json.at("boot_id").get<std::string>()};
    } else {
        summary.boot_id.reset();
    }
}

void to_json(nlohmann::json& json, const WorkspaceDetail& detail) {
    json = detail.summary;
    json["created_at_ms"] = detail.created_at_ms;
    json["updated_at_ms"] = detail.updated_at_ms;
}

void from_json(const nlohmann::json& json, WorkspaceDetail& detail) {
    detail.summary = json.get<WorkspaceSummary>();
    detail.created_at_ms = json.at("created_at_ms").get<std::int64_t>();
    detail.updated_at_ms = json.at("updated_at_ms").get<std::int64_t>();
}

void to_json(nlohmann::json& json, const SessionSummary& summary) {
    json = nlohmann::json{{"id", summary.id.value},
                          {"ordinal", summary.ordinal},
                          {"archived", summary.archived},
                          {"title", summary.title},
                          {"kind", summary.kind},
                          {"updated_at_ms", summary.updated_at_ms}};
}

void from_json(const nlohmann::json& json, SessionSummary& summary) {
    summary.id.value = json.at("id").get<std::string>();
    summary.ordinal = json.at("ordinal").get<std::int64_t>();
    summary.archived = json.at("archived").get<bool>();
    summary.title = json.at("title").get<std::string>();
    summary.kind = json.at("kind").get<std::string>();
    summary.updated_at_ms = json.at("updated_at_ms").get<std::int64_t>();
}

void to_json(nlohmann::json& json, const SessionDetail& detail) {
    json = nlohmann::json{{"summary", detail.summary},
                          {"header", detail.header},
                          {"event_count", detail.event_count}};
}

void from_json(const nlohmann::json& json, SessionDetail& detail) {
    detail.summary = json.at("summary").get<SessionSummary>();
    detail.header = json.at("header");
    detail.event_count = json.at("event_count").get<std::size_t>();
}

namespace {

constexpr std::array<std::string_view, 27> kMethodCatalog{{
    method::kHostHello,        method::kHostAttach,       method::kHostDetach,
    method::kHostStatus,       method::kHostPing,         method::kHostShutdown,
    method::kWorkspaceList,    method::kWorkspaceShow,    method::kSessionList,
    method::kSessionShow,      method::kSessionCreate,    method::kSessionResume,
    method::kSessionFork,      method::kSessionReplay,    method::kSessionActivate,
    method::kSessionSuspend,   method::kSessionClose,     method::kSessionDelete,
    method::kAgentPrompt,      method::kAgentFollowup,    method::kAgentSteer,
    method::kAgentInject,      method::kAgentCancel,      method::kAgentStatus,
    method::kPermissionDecide, method::kEventSubscribe,   method::kEventUnsubscribe,
}};

} // namespace

std::span<const std::string_view> all_methods() noexcept {
    return kMethodCatalog;
}

bool is_known_method(std::string_view name) noexcept {
    for (const std::string_view entry : kMethodCatalog) {
        if (entry == name) {
            return true;
        }
    }
    return false;
}

bool is_method_allowed(ServerProfile profile, std::string_view method_name) noexcept {
    if (profile == ServerProfile::Interactive) {
        return true;
    }
    return method_name != method::kSessionActivate && method_name != method::kSessionSuspend &&
           method_name != method::kHostShutdown;
}

} // namespace ymh::protocol

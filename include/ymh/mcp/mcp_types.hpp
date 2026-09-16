#pragma once

// MCP adapter types, configuration, and error taxonomy (15-mcp-adapter.md §2,
// §4.1). This header pins the provider-visible identities, the lifecycle state
// machine, the wire error vocabulary, and the `[mcp]` bounds. It depends only on
// core/session/execution/policy; it never includes a UI or transport type (M16).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {

struct McpServerId {
    std::string value;
    auto operator<=>(const McpServerId&) const = default;
};

struct McpRemoteToolName {
    std::string value;
    auto operator<=>(const McpRemoteToolName&) const = default;
};

struct McpRequestId {
    std::int64_t value{0};
    auto operator<=>(const McpRequestId&) const = default;
};

enum class McpServerState : std::uint8_t {
    Disabled,
    Starting,
    Ready,
    Degraded,
    Disconnected,
    Failed,
    Stopped,
};

enum class McpTransportKind : std::uint8_t {
    Stdio,
    HttpSse,
};

[[nodiscard]] std::string_view mcp_state_token(McpServerState state) noexcept;
[[nodiscard]] std::optional<McpServerState> parse_mcp_state(std::string_view name) noexcept;
[[nodiscard]] std::string_view mcp_transport_token(McpTransportKind kind) noexcept;
[[nodiscard]] std::optional<McpTransportKind> parse_mcp_transport(std::string_view name) noexcept;

// `[a-z][a-z0-9_]{0,31}` (07 §2.1 ToolName sub-segment grammar, M3).
[[nodiscard]] bool is_valid_mcp_server_id(std::string_view value) noexcept;

enum class McpErrorCode : std::uint8_t {
    ConfigInvalid,
    SpawnFailed,
    TransportClosed,
    HandshakeTimeout,
    HandshakeRejected,
    ProtocolViolation,
    SchemaIncompatible,
    UnknownTool,
    CallTimeout,
    ServerError,
    RpcError,
    ResultTooLarge,
    Cancelled,
    CapExhausted,
    Internal,
};

[[nodiscard]] std::string_view to_string(McpErrorCode code) noexcept;

// 15 §2.2: the single classification boundary. Maps an adapter failure onto the
// frozen `07 §2.1` vocabulary so no MCP-specific code reaches a durable result.
[[nodiscard]] ToolErrorCode to_tool_error_code(McpErrorCode code) noexcept;

class McpError final : public std::runtime_error {
public:
    McpError(McpErrorCode code, std::string message);
    [[nodiscard]] McpErrorCode code() const noexcept { return code_; }

private:
    McpErrorCode code_;
};

enum class McpDisconnectReason : std::uint8_t {
    ClientClose,
    ServerEof,
    SpawnFailed,
    ProtocolError,
    TransportError,
};

struct McpToolInfo {
    McpRemoteToolName          remote_name;
    std::string                description;
    nlohmann::json             input_schema;
    bool                       destructive{false};
    std::optional<std::string> title;
};

struct McpCallResult {
    nlohmann::json                content;
    bool                          is_error{false};
    std::optional<nlohmann::json> structured_content;
};

struct McpCallOptions {
    ToolCallId                call_id;
    std::chrono::milliseconds timeout{0};
    std::size_t               max_bytes{0};
};

struct McpServerStatus {
    McpServerId              id;
    McpServerState           state{McpServerState::Disabled};
    std::string              server_name;
    std::string              server_version;
    std::string              protocol_version;
    std::size_t              tool_count{0};
    std::vector<std::string> skipped_tools;
    std::string              last_error;
};

struct McpServerConfig {
    McpServerId              id;

    bool                     enabled{true};
    bool                     required{false};

    McpTransportKind         transport{McpTransportKind::Stdio};

    std::string              command;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::filesystem::path    cwd;

    std::string              url;
    std::vector<std::string> header_env;

    std::string              protocol_version;

    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;

    PolicyVerdict            default_verdict{PolicyVerdict::Ask};
    std::chrono::milliseconds call_timeout{60'000};
    std::size_t              max_result_bytes{1u * 1024u * 1024u};
};

struct McpConfig {
    bool                         enabled{true};

    std::vector<McpServerConfig> servers;

    std::size_t                  max_servers{8};
    std::size_t                  max_inflight_calls_per_server{4};

    std::chrono::milliseconds    startup_deadline{5'000};
    std::chrono::milliseconds    handshake_timeout{10'000};
    std::chrono::milliseconds    list_timeout{5'000};
    std::size_t                  list_max_pages{64};

    std::uint32_t                reconnect_max_attempts{5};
    std::chrono::milliseconds    reconnect_initial_backoff{500};
    std::chrono::milliseconds    reconnect_max_backoff{30'000};
    double                       reconnect_jitter{0.25};
    std::chrono::milliseconds    reconnect_stable_window{30'000};
    std::chrono::milliseconds    ping_interval{15'000};

    std::chrono::milliseconds    shutdown_grace{2'000};

    std::size_t                  max_frame_bytes{8u * 1024u * 1024u};

    bool                         allow_network_servers{false};
};

namespace payload {

struct McpServerStatusChanged {
    McpServerId     server;
    McpServerState  state;
    std::size_t     tool_count{0};
    std::string     reason;
};

void to_json(nlohmann::json& json, const McpServerStatusChanged& status);
void from_json(const nlohmann::json& json, McpServerStatusChanged& status);

} // namespace payload

template <>
struct EventTraits<payload::McpServerStatusChanged> {
    static constexpr EventType type = EventType::McpServerStatusChanged;
};

} // namespace ymh

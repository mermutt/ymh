#include "ymh/mcp/mcp_types.hpp"

#include <array>
#include <string>
#include <utility>

namespace ymh {
namespace {

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> parse_token(
    const std::array<std::pair<Enum, std::string_view>, N>& table,
    std::string_view name) noexcept {
    for (const auto& [value, token] : table) {
        if (token == name) {
            return value;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::pair<McpServerState, std::string_view>, 7> kStates{{
    {McpServerState::Disabled, "disabled"},
    {McpServerState::Starting, "starting"},
    {McpServerState::Ready, "ready"},
    {McpServerState::Degraded, "degraded"},
    {McpServerState::Disconnected, "disconnected"},
    {McpServerState::Failed, "failed"},
    {McpServerState::Stopped, "stopped"},
}};

constexpr std::array<std::pair<McpTransportKind, std::string_view>, 2> kTransports{{
    {McpTransportKind::Stdio, "stdio"},
    {McpTransportKind::HttpSse, "http_sse"},
}};

} // namespace

std::string_view mcp_state_token(McpServerState state) noexcept {
    for (const auto& [value, token] : kStates) {
        if (value == state) {
            return token;
        }
    }
    return {};
}

std::optional<McpServerState> parse_mcp_state(std::string_view name) noexcept {
    return parse_token(kStates, name);
}

std::string_view mcp_transport_token(McpTransportKind kind) noexcept {
    for (const auto& [value, token] : kTransports) {
        if (value == kind) {
            return token;
        }
    }
    return {};
}

std::optional<McpTransportKind> parse_mcp_transport(std::string_view name) noexcept {
    return parse_token(kTransports, name);
}

bool is_valid_mcp_server_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32) {
        return false;
    }
    if (value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    for (const char c : value.substr(1)) {
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !digit && c != '_') {
            return false;
        }
    }
    return true;
}

std::string_view to_string(McpErrorCode code) noexcept {
    switch (code) {
        case McpErrorCode::ConfigInvalid:      return "ConfigInvalid";
        case McpErrorCode::SpawnFailed:        return "SpawnFailed";
        case McpErrorCode::TransportClosed:    return "TransportClosed";
        case McpErrorCode::HandshakeTimeout:   return "HandshakeTimeout";
        case McpErrorCode::HandshakeRejected:  return "HandshakeRejected";
        case McpErrorCode::ProtocolViolation:  return "ProtocolViolation";
        case McpErrorCode::SchemaIncompatible: return "SchemaIncompatible";
        case McpErrorCode::UnknownTool:        return "UnknownTool";
        case McpErrorCode::CallTimeout:        return "CallTimeout";
        case McpErrorCode::ServerError:        return "ServerError";
        case McpErrorCode::RpcError:           return "RpcError";
        case McpErrorCode::ResultTooLarge:     return "ResultTooLarge";
        case McpErrorCode::Cancelled:          return "Cancelled";
        case McpErrorCode::CapExhausted:       return "CapExhausted";
        case McpErrorCode::Internal:           return "Internal";
    }
    return "Internal";
}

ToolErrorCode to_tool_error_code(McpErrorCode code) noexcept {
    switch (code) {
        case McpErrorCode::UnknownTool:
            return ToolErrorCode::UnknownTool;
        case McpErrorCode::SchemaIncompatible:
            return ToolErrorCode::InvalidArguments;
        case McpErrorCode::CallTimeout:
            return ToolErrorCode::Timeout;
        case McpErrorCode::SpawnFailed:
        case McpErrorCode::TransportClosed:
        case McpErrorCode::ProtocolViolation:
        case McpErrorCode::ResultTooLarge:
            return ToolErrorCode::Io;
        case McpErrorCode::CapExhausted:
            return ToolErrorCode::ResourceExhausted;
        case McpErrorCode::ServerError:
        case McpErrorCode::RpcError:
        case McpErrorCode::ConfigInvalid:
        case McpErrorCode::HandshakeTimeout:
        case McpErrorCode::HandshakeRejected:
        case McpErrorCode::Internal:
            return ToolErrorCode::Internal;
        case McpErrorCode::Cancelled:
            return ToolErrorCode::Internal;
    }
    return ToolErrorCode::Internal;
}

McpError::McpError(McpErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

namespace payload {

void to_json(nlohmann::json& json, const McpServerStatusChanged& status) {
    json = nlohmann::json{
        {"server", status.server.value},
        {"state", std::string{mcp_state_token(status.state)}},
        {"tool_count", status.tool_count},
        {"reason", status.reason},
    };
}

void from_json(const nlohmann::json& json, McpServerStatusChanged& status) {
    status.server.value = json.at("server").get<std::string>();
    const auto state = parse_mcp_state(json.at("state").get<std::string>());
    if (!state.has_value()) {
        throw std::invalid_argument("unknown MCP server state");
    }
    status.state = *state;
    status.tool_count = json.value("tool_count", static_cast<std::size_t>(0));
    status.reason = json.value("reason", std::string{});
}

} // namespace payload

} // namespace ymh

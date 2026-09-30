#include "ymh/mcp/mcp_types.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

bool is_utf8_continuation(unsigned char byte) noexcept {
    return (byte & 0xC0) == 0x80;
}

// 70-D7: the byte length (1..4) of the valid UTF-8 sequence at `text[index]`, or 0
// when the byte starts no valid sequence (invalid, overlong, surrogate, or out of
// RFC 3629 range). An invalid byte counts as one byte to the caller.
std::size_t utf8_sequence_length(std::string_view text, std::size_t index) noexcept {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        codepoint = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        codepoint = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        codepoint = first & 0x07u;
    } else {
        return 0;
    }
    if (index + length > text.size()) {
        return 0;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto byte = static_cast<unsigned char>(text[index + offset]);
        if (!is_utf8_continuation(byte)) {
            return 0;
        }
        codepoint = (codepoint << 6) | (byte & 0x3Fu);
    }
    static constexpr std::uint32_t kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (codepoint < kMinimum[length]) {
        return 0;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        return 0;
    }
    if (codepoint > 0x10FFFF) {
        return 0;
    }
    return length;
}

// 70-D7: the same conservative marker set as 69-D6; a line carrying a
// credential-shaped key is dropped rather than surfaced.
bool reason_line_has_secret(const std::string& line) {
    static constexpr std::string_view kMarkers[] = {
        "api_key", "api-key", "apikey", "authorization", "bearer ",
        "secret",  "password", "passwd", "credential",   "token="};
    std::string lowered;
    lowered.reserve(line.size());
    for (const char c : line) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const std::string_view marker : kMarkers) {
        if (lowered.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string normalize_line_endings(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '\r') {
            out.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string coerce_valid_utf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const std::size_t length = utf8_sequence_length(text, index);
        if (length == 0) {
            out.push_back('?');
            ++index;
            continue;
        }
        if (length == 1) {
            const auto byte = static_cast<unsigned char>(text[index]);
            if (byte == '\n' || byte == '\t') {
                out.push_back(static_cast<char>(byte));
            } else if (byte < 0x20 || byte == 0x7F) {
                out.push_back(' ');
            } else {
                out.push_back(static_cast<char>(byte));
            }
        } else {
            out.append(text, index, length);
        }
        index += length;
    }
    return out;
}

constexpr std::size_t kMaxReasonLines = 4;
constexpr std::size_t kMaxReasonBytes = 512;
constexpr std::size_t kMaxReasonLineBytes = 240;

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

std::string normalize_mcp_server_id(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const char c : name) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '_') {
            normalized.push_back(lower);
        } else if (lower == '-' || lower == '.' || lower == ' ') {
            normalized.push_back('_');
        }
    }
    if (normalized.empty() || normalized.front() < 'a' || normalized.front() > 'z') {
        normalized.insert(normalized.begin(), 's');
    }
    if (normalized.size() > 32) {
        normalized.resize(32);
    }
    if (normalized.empty()) {
        return "server";
    }
    return normalized;
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

std::string_view mcp_disconnect_token(McpDisconnectReason reason) noexcept {
    switch (reason) {
        case McpDisconnectReason::ClientClose:    return "client closed";
        case McpDisconnectReason::ServerEof:      return "server closed the connection (EOF)";
        case McpDisconnectReason::SpawnFailed:    return "spawn failed";
        case McpDisconnectReason::ProtocolError:  return "protocol error";
        case McpDisconnectReason::TransportError: return "transport I/O error";
    }
    return "unknown";
}

std::string mcp_truncate_utf8(std::string_view text, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(text.size(), max_bytes));
    std::size_t index = 0;
    while (index < text.size() && out.size() < max_bytes) {
        const std::size_t length = utf8_sequence_length(text, index);
        const std::size_t step = length == 0 ? 1 : length;
        if (out.size() + step > max_bytes) {
            break;
        }
        out.append(text.substr(index, step));
        index += step;
    }
    return out;
}

std::string bound_mcp_reason(std::string_view reason) {
    if (reason.empty()) {
        return {};
    }
    const std::string clean = coerce_valid_utf8(normalize_line_endings(reason));
    std::vector<std::string> lines;
    std::size_t              total = 0;
    std::size_t              start = 0;
    for (std::size_t index = 0; index <= clean.size(); ++index) {
        if (index != clean.size() && clean[index] != '\n') {
            continue;
        }
        std::string line = clean.substr(start, index - start);
        start = index + 1;
        if (line.empty() || reason_line_has_secret(line)) {
            continue;
        }
        line = mcp_truncate_utf8(line, kMaxReasonLineBytes);
        if (line.empty()) {
            continue;
        }
        if (lines.size() >= kMaxReasonLines ||
            total + line.size() + (lines.empty() ? 0u : 1u) > kMaxReasonBytes) {
            break;
        }
        total += line.size();
        lines.push_back(std::move(line));
    }
    if (lines.empty()) {
        return "failure reason withheld";
    }
    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            out.push_back('\n');
        }
        out += lines[index];
    }
    return out;
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

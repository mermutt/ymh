#include "ymh/ui/status_format.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "ymh/mcp/mcp_types.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::ui {
namespace {

constexpr std::size_t kMaxStatusToolRows = 20;
constexpr std::size_t kMaxErrorChars = 120;
constexpr std::size_t kMaxSkippedNames = 8;

std::string daemon_token(DaemonStatus status) {
    switch (status) {
        case DaemonStatus::Connecting:
            return "connecting";
        case DaemonStatus::Attached:
            return "attached";
        case DaemonStatus::Detached:
            return "detached";
        case DaemonStatus::Dead:
            return "dead";
        case DaemonStatus::Stopping:
            return "stopping";
        case DaemonStatus::NotRunning:
            return "not running";
    }
    return "unknown";
}

std::string bounded_error(std::string text) {
    for (char& character : text) {
        if (character == '\n' || character == '\r' || character == '\t') {
            character = ' ';
        }
    }
    if (text.size() > kMaxErrorChars) {
        text.resize(kMaxErrorChars);
        text += "...";
    }
    return text;
}

std::size_t json_size(const nlohmann::json& value) {
    return value.is_number_unsigned() || value.is_number_integer()
               ? value.get<std::size_t>()
               : 0;
}

std::string join_skipped(const nlohmann::json& skipped_tools) {
    std::string names;
    std::size_t shown = 0;
    for (const nlohmann::json& entry : skipped_tools) {
        if (shown == kMaxSkippedNames) {
            names += ", … and " + std::to_string(skipped_tools.size() - shown) + " more";
            break;
        }
        if (shown != 0) {
            names += ", ";
        }
        names += entry.is_string() ? entry.get<std::string>() : entry.dump();
        ++shown;
    }
    return names;
}

} // namespace

std::optional<std::string> mcp_unavailable_notice(int error_code) {
    if (error_code == static_cast<int>(protocol::RpcCode::MethodNotFound) ||
        error_code == static_cast<int>(protocol::AppCode::MethodNotAllowedForProfile)) {
        return std::string{"mcp: unavailable (daemon does not support mcp.status)"};
    }
    return std::nullopt;
}

std::string agent_no_agents_notice() {
    return "no agents configured (add <config>/presets/<id>/preset.jsonc)";
}

std::string agent_no_others_notice() { return "no other agents available"; }

std::string agent_composition_fixed_notice() {
    return "agent composition is fixed for this session";
}

std::optional<std::string> agent_unavailable_notice(int error_code) {
    if (error_code == static_cast<int>(protocol::RpcCode::MethodNotFound) ||
        error_code == static_cast<int>(protocol::AppCode::MethodNotAllowedForProfile)) {
        return agent_no_agents_notice();
    }
    return std::nullopt;
}

std::string format_mcp_block(const nlohmann::json& result) {
    const nlohmann::json servers =
        result.is_object() && result.contains("servers") && result.at("servers").is_array()
            ? result.at("servers")
            : nlohmann::json::array();
    std::size_t connected = 0;
    std::size_t tool_total = 0;
    for (const nlohmann::json& server : servers) {
        if (server.value("connected", false)) {
            ++connected;
        }
        tool_total += json_size(server.value("tool_count", nlohmann::json{0}));
    }

    std::string block = "mcp servers: " + std::to_string(servers.size()) + " configured, " +
                        std::to_string(connected) + " connected, " + std::to_string(tool_total) +
                        " tools";
    for (const nlohmann::json& server : servers) {
        const std::string id = server.value("id", std::string{});
        const std::string state = server.value("state", std::string{});
        const bool is_connected = server.value("connected", false);
        block += "\n  " + id + "  ";
        if (is_connected) {
            block += "connected  " +
                     std::to_string(json_size(server.value("tool_count", nlohmann::json{0}))) +
                     " tools";
        } else if (state == "disabled") {
            block += "disabled";
        } else {
            block += "not connected  " + state;
        }
        if (server.contains("skipped_tools") && server.at("skipped_tools").is_array() &&
            !server.at("skipped_tools").empty()) {
            const std::size_t skipped = json_size(server.value("skipped", nlohmann::json{0}));
            block += "\n    skipped: " + join_skipped(server.at("skipped_tools")) +
                     " (skipped=" + std::to_string(skipped) + ")";
        }
    }
    return block;
}

std::string format_status_block(const StatusBlockInputs& inputs) {
    std::string block = "ymh status";
    block += "\n  version:  " + (inputs.version.empty() ? std::string{"unknown"} : inputs.version);
    block += "\n  model:    " + (inputs.model.empty() ? std::string{"unknown"} : inputs.model);
    switch (inputs.api_state) {
        case ApiConnectivity::Ok:
            block += "\n  api:      ok";
            break;
        case ApiConnectivity::Error:
            block += "\n  api:      error: " + bounded_error(inputs.last_error);
            break;
        case ApiConnectivity::Unknown:
            block += "\n  api:      unknown";
            break;
    }
    std::string daemon = daemon_token(inputs.daemon);
    if (inputs.daemon == DaemonStatus::Attached && !inputs.workspace_title.empty()) {
        daemon += " (workspace: " + inputs.workspace_title + ")";
    }
    block += "\n  daemon:   " + daemon;

    if (!inputs.has_session) {
        block += "\n  tools:    (no session)";
        block += "\n  mcp:      (no session)";
        return block;
    }
    if (inputs.snapshot == nullptr) {
        block += "\n  tools:    (unavailable)";
        block += "\n  mcp:      (unavailable)";
        return block;
    }

    const ContextSnapshot& snapshot = *inputs.snapshot;
    std::size_t builtin = 0;
    std::size_t mcp = 0;
    for (const ContextToolEntry& tool : snapshot.tools) {
        if (tool.provenance == "mcp") {
            ++mcp;
        } else {
            ++builtin;
        }
    }
    block += "\n  tools:    " + std::to_string(snapshot.tools.size()) + " available (" +
             std::to_string(builtin) + " builtin, " + std::to_string(mcp) + " mcp)";
    const std::size_t rows = std::min(kMaxStatusToolRows, snapshot.tools.size());
    for (std::size_t index = 0; index < rows; ++index) {
        const ContextToolEntry& tool = snapshot.tools[index];
        block += "\n    " + tool.name + " (" + tool.provenance + ") ~" +
                 std::to_string(tool.schema_tokens) + " tok";
    }
    if (snapshot.tools.size() > rows) {
        block += "\n    … and " + std::to_string(snapshot.tools.size() - rows) + " more";
    }

    std::size_t connected = 0;
    for (const ContextServerEntry& server : snapshot.mcp_servers) {
        if (server.state == "ready") {
            ++connected;
        }
    }
    block += "\n  mcp:      " + std::to_string(connected) + " connected / " +
             std::to_string(snapshot.mcp_servers.size()) + " configured";
    for (const ContextServerEntry& server : snapshot.mcp_servers) {
        block += "\n    " + server.id + "  " + server.state;
        if (server.has_error) {
            block += " (failed)";
        }
    }
    return block;
}

} // namespace ymh::ui

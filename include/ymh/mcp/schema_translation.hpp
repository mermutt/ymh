#pragma once

// MCP schema/name/result translation (15-mcp-adapter.md §3.2, §4.4, §4.5).
// Pure functions: no I/O, no state, shared by McpManager and the test suite.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/mcp/mcp_types.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

// `mcp.<server>.<sanitized>` (§3.2): lowercase ASCII; every character outside
// `[a-z0-9_]` becomes `_`; a leading digit gets an `m_` prefix; consecutive `_`
// collapse; trailing `_` is trimmed; empty becomes `tool`.
[[nodiscard]] std::string sanitize_mcp_tool_segment(std::string_view remote_name);

// Downcast an MCP `inputSchema` to the pinned `ToolSchema` subset (§4.4). A
// violation is `McpErrorCode::SchemaIncompatible`; `notes` collects the
// documented losses (tightened additionalProperties, dropped keywords).
[[nodiscard]] std::expected<ToolSchema, McpErrorCode> translate_mcp_schema(
    const McpServerId& server,
    const McpRemoteToolName& remote_name,
    const nlohmann::json& input_schema,
    std::size_t description_max_bytes,
    std::vector<std::string>* notes);

// Project `content[]`/`structuredContent` into a single bounded `output` (§4.5).
// Binary blocks become typed placeholders; base64 is never materialized.
[[nodiscard]] std::string project_mcp_content(const nlohmann::json& content,
                                              const std::optional<nlohmann::json>& structured);

// `allowed_tools`/`denied_tools` filtering against the RAW remote name (§4.1).
// Deny wins over allow; an empty allow list admits everything.
[[nodiscard]] bool mcp_tool_allowed(const McpServerConfig& server,
                                    std::string_view remote_name);

} // namespace ymh

#include "ymh/mcp/schema_translation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/execution/glob.hpp"

namespace ymh {
namespace {

constexpr std::string_view kUnsupportedKeywords[] = {
    "$ref",      "anyOf",       "oneOf",   "allOf",   "not",
    "patternProperties",        "if",      "then",    "else",    "const",
};

bool is_unsupported_keyword(std::string_view key) {
    return std::find(std::begin(kUnsupportedKeywords), std::end(kUnsupportedKeywords),
                     key) != std::end(kUnsupportedKeywords);
}

bool is_allowed_scalar(const nlohmann::json& value) {
    return value.is_string() || value.is_number() || value.is_boolean();
}

std::string bounded(std::string value, std::size_t max_bytes) {
    if (value.size() > max_bytes) {
        value.resize(max_bytes);
    }
    return value;
}

void note(std::vector<std::string>* notes, std::string text) {
    if (notes != nullptr) {
        notes->push_back(std::move(text));
    }
}

bool translate_node(const nlohmann::json& in,
                    nlohmann::json& out,
                    std::size_t description_max_bytes,
                    std::vector<std::string>* notes) {
    if (!in.is_object()) {
        return false;
    }
    for (const auto& [key, value] : in.items()) {
        (void)value;
        if (is_unsupported_keyword(key)) {
            return false;
        }
    }
    if (!in.contains("type") || !in["type"].is_string()) {
        return false;
    }
    const std::string type = in["type"].get<std::string>();
    static constexpr std::string_view kTypes[] = {"object", "string", "integer",
                                                  "number", "boolean", "array"};
    if (std::find(std::begin(kTypes), std::end(kTypes), type) == std::end(kTypes)) {
        return false;
    }
    out = nlohmann::json::object();
    out["type"] = type;

    if (in.contains("description") && in["description"].is_string()) {
        out["description"] =
            bounded(in["description"].get<std::string>(), description_max_bytes);
    }
    if (in.contains("enum")) {
        if (!in["enum"].is_array()) {
            return false;
        }
        for (const auto& element : in["enum"]) {
            if (!is_allowed_scalar(element)) {
                return false;
            }
        }
        out["enum"] = in["enum"];
    }
    if (in.contains("default")) {
        if (!is_allowed_scalar(in["default"])) {
            return false;
        }
        out["default"] = in["default"];
    }

    if (type == "object") {
        if (in.contains("additionalProperties") && in["additionalProperties"] == true) {
            return false;
        }
        if (!in.contains("additionalProperties")) {
            note(notes, "additionalProperties tightened to false");
        }
        out["additionalProperties"] = false;
        out["properties"] = nlohmann::json::object();
        if (in.contains("properties")) {
            if (!in["properties"].is_object()) {
                return false;
            }
            for (const auto& [name, sub] : in["properties"].items()) {
                nlohmann::json translated;
                if (!translate_node(sub, translated, description_max_bytes, notes)) {
                    return false;
                }
                out["properties"][name] = std::move(translated);
            }
        }
        if (in.contains("required")) {
            if (!in["required"].is_array()) {
                return false;
            }
            nlohmann::json required = nlohmann::json::array();
            for (const auto& entry : in["required"]) {
                if (!entry.is_string()) {
                    return false;
                }
                const std::string name = entry.get<std::string>();
                if (out["properties"].contains(name)) {
                    required.push_back(name);
                } else {
                    note(notes, "required name not in properties: " + name);
                }
            }
            out["required"] = std::move(required);
        }
    } else if (type == "array") {
        if (!in.contains("items")) {
            return false;
        }
        nlohmann::json items;
        if (!translate_node(in["items"], items, description_max_bytes, notes)) {
            return false;
        }
        out["items"] = std::move(items);
    }
    return true;
}

std::size_t decoded_base64_bytes(const nlohmann::json& block) {
    if (!block.contains("data") || !block["data"].is_string()) {
        return 0;
    }
    const std::string data = block["data"].get<std::string>();
    if (data.empty()) {
        return 0;
    }
    std::size_t padding = 0;
    if (data.ends_with("==")) {
        padding = 2;
    } else if (data.ends_with("=")) {
        padding = 1;
    }
    return (data.size() / 4) * 3 - padding;
}

std::string block_placeholder(const nlohmann::json& block) {
    const std::string type = block.value("type", std::string{"unknown"});
    const std::string mime = block.value("mimeType", std::string{"application/octet-stream"});
    if (type == "image") {
        return "[image " + mime + " " + std::to_string(decoded_base64_bytes(block)) +
               " bytes]";
    }
    if (type == "audio") {
        return "[audio " + mime + " " + std::to_string(decoded_base64_bytes(block)) +
               " bytes]";
    }
    if (type == "resource") {
        std::string uri;
        std::string text;
        if (block.contains("resource") && block["resource"].is_object()) {
            uri = block["resource"].value("uri", std::string{});
            if (block["resource"].contains("text") && block["resource"]["text"].is_string()) {
                text = block["resource"]["text"].get<std::string>();
            }
        }
        std::string projected = "[resource " + uri + "]";
        if (!text.empty()) {
            projected += "\n" + text;
        }
        return projected;
    }
    return "[" + type + " " + std::to_string(block.dump().size()) + " bytes]";
}

} // namespace

std::string sanitize_mcp_tool_segment(std::string_view remote_name) {
    std::string sanitized;
    sanitized.reserve(remote_name.size());
    for (const char raw : remote_name) {
        const unsigned char c = static_cast<unsigned char>(raw);
        char mapped = '_';
        if (c >= 'A' && c <= 'Z') {
            mapped = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            mapped = static_cast<char>(c);
        } else if (c == '_') {
            mapped = '_';
        }
        if (mapped == '_' && !sanitized.empty() && sanitized.back() == '_') {
            continue;
        }
        sanitized.push_back(mapped);
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }
    if (sanitized.empty()) {
        return "tool";
    }
    if (sanitized.front() >= '0' && sanitized.front() <= '9') {
        sanitized.insert(0, "m_");
    }
    return sanitized;
}

std::expected<ToolSchema, McpErrorCode> translate_mcp_schema(
    const McpServerId& server,
    const McpRemoteToolName& remote_name,
    const nlohmann::json& input_schema,
    std::size_t description_max_bytes,
    std::vector<std::string>* notes) {
    if (!input_schema.is_object()) {
        return std::unexpected(McpErrorCode::SchemaIncompatible);
    }
    nlohmann::json translated;
    if (!translate_node(input_schema, translated, description_max_bytes, notes)) {
        return std::unexpected(McpErrorCode::SchemaIncompatible);
    }
    if (!translated.contains("type") || translated["type"] != "object") {
        return std::unexpected(McpErrorCode::SchemaIncompatible);
    }

    ToolSchema schema;
    schema.name.value = "mcp." + server.value + "." +
                        sanitize_mcp_tool_segment(remote_name.value);
    schema.version = ToolVersion{1, 0};
    schema.description = translated.value("description", std::string{});
    translated.erase("description");
    schema.input_schema = std::move(translated);
    return schema;
}

std::string project_mcp_content(const nlohmann::json& content,
                                const std::optional<nlohmann::json>& structured) {
    std::string output;
    if (content.is_array()) {
        for (const nlohmann::json& block : content) {
            if (!block.is_object()) {
                continue;
            }
            const std::string type = block.value("type", std::string{"unknown"});
            std::string piece;
            if (type == "text") {
                piece = block.value("text", std::string{});
            } else {
                piece = block_placeholder(block);
            }
            if (!output.empty()) {
                output += "\n";
            }
            output += piece;
        }
    }
    if (structured.has_value()) {
        if (!output.empty()) {
            output += "\n";
        }
        output += "```json\n";
        output += structured->dump();
        output += "\n```";
    }
    return output;
}

bool mcp_tool_allowed(const McpServerConfig& server, std::string_view remote_name) {
    for (const std::string& pattern : server.denied_tools) {
        if (glob_match(pattern, remote_name)) {
            return false;
        }
    }
    if (server.allowed_tools.empty()) {
        return true;
    }
    for (const std::string& pattern : server.allowed_tools) {
        if (glob_match(pattern, remote_name)) {
            return true;
        }
    }
    return false;
}

} // namespace ymh

#pragma once

// LLM-facing message model, pinned by 00-architecture.md §12 and reused by
// 01-session.md §4.5 ("Message, Role, ContentBlock, and Usage are the types
// defined by §12 (`include/ymh/agent/message.hpp`) and §33; they are reused
// rather than redefined").
//
// DESIGN AMBIGUITY: neither §12 nor §33 pins the concrete member layout of
// `ContentBlock` / `Usage`; only their existence and their role in the
// projection are fixed. The shapes below are the minimal faithful
// materialisation needed by `Session::deriveMessages()` (01 §6.3) and the
// `AssistantMessage` / `UserMessage` payloads (01 §4.5). They are additive: a
// later agent/LLM wave may extend them, but must not rename the pinned types.
//
// `Message` deliberately carries an optional `tool_call_id` so that a projected
// `Role::Tool` message can pair with the `tool_use` block id it answers
// (01 I12); `deriveMessages` synthesizes such messages for cancelled/failed
// turns. No frontend/UI type appears here (D15).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {

// The role a projected message assumes (01 §6.3, §12).
enum class Role : std::uint8_t {
    System,
    User,
    Assistant,
    Tool,
};

// Total; returns an empty view for a value outside the enum domain.
[[nodiscard]] inline std::string_view role_name(Role role) noexcept {
    switch (role) {
        case Role::System:
            return "system";
        case Role::User:
            return "user";
        case Role::Assistant:
            return "assistant";
        case Role::Tool:
            return "tool";
    }
    return {};
}

[[nodiscard]] inline std::optional<Role> parse_role(std::string_view name) noexcept {
    if (name == "system") {
        return Role::System;
    }
    if (name == "user") {
        return Role::User;
    }
    if (name == "assistant") {
        return Role::Assistant;
    }
    if (name == "tool") {
        return Role::Tool;
    }
    return std::nullopt;
}

// Content block discriminator (01 §4.5: text / reasoning / tool_use / image).
enum class ContentBlockKind : std::uint8_t {
    Text,
    Reasoning,
    ToolUse,
    Image,
};

[[nodiscard]] inline std::string_view content_block_kind_name(ContentBlockKind kind) noexcept {
    switch (kind) {
        case ContentBlockKind::Text:
            return "text";
        case ContentBlockKind::Reasoning:
            return "reasoning";
        case ContentBlockKind::ToolUse:
            return "tool_use";
        case ContentBlockKind::Image:
            return "image";
    }
    return {};
}

[[nodiscard]] inline std::optional<ContentBlockKind> parse_content_block_kind(
    std::string_view name) noexcept {
    if (name == "text") {
        return ContentBlockKind::Text;
    }
    if (name == "reasoning") {
        return ContentBlockKind::Reasoning;
    }
    if (name == "tool_use") {
        return ContentBlockKind::ToolUse;
    }
    if (name == "image") {
        return ContentBlockKind::Image;
    }
    return std::nullopt;
}

// A single block of message content. Only the fields relevant to `kind` are
// meaningful; the others stay at their defaults (kept flat so the struct is a
// plain value type with no variant visitation in the projection).
struct ContentBlock {
    ContentBlockKind kind = ContentBlockKind::Text;
    std::string      text;                         // Text / Reasoning
    std::string      tool_call_id;                 // ToolUse: pairs with ToolResult::id
    std::string      tool_name;                    // ToolUse
    nlohmann::json   arguments = nlohmann::json::object();  // ToolUse
    std::string      media_type;                   // Image
    std::string      data;                         // Image (opaque, provider-encoded)
};

// The LLM message (00 §12). `tool_call_id` is set only for Role::Tool and is the
// `tool_use` block id the message answers (01 I12).
struct Message {
    Role                     role = Role::User;
    std::vector<ContentBlock> content;
    std::string              tool_call_id;
};

// Token accounting (00 §33). `TokenUsage` is the canonical durable carrier
// (01 §4.5); `AssistantMessage.usage` is a convenience copy.
struct Usage {
    std::int64_t input_tokens     = 0;
    std::int64_t output_tokens    = 0;
    std::int64_t cached_tokens    = 0;
    std::int64_t reasoning_tokens = 0;

    auto operator<=>(const Usage&) const = default;
};

// ---------------------------------------------------------------------------
// JSON (persistence / wire). Unknown enum strings fail loudly (01 S3).
// ---------------------------------------------------------------------------

inline void to_json(nlohmann::json& json, const ContentBlock& block) {
    json = nlohmann::json{
        {"kind", std::string{content_block_kind_name(block.kind)}},
        {"text", block.text},
        {"tool_call_id", block.tool_call_id},
        {"tool_name", block.tool_name},
        {"arguments", block.arguments},
        {"media_type", block.media_type},
        {"data", block.data},
    };
}

inline void from_json(const nlohmann::json& json, ContentBlock& block) {
    const auto kind = parse_content_block_kind(json.at("kind").get<std::string>());
    if (!kind) {
        throw nlohmann::json::other_error::create(
            501, "unknown content block kind: " + json.at("kind").get<std::string>(), &json);
    }
    block.kind         = *kind;
    block.text         = json.value("text", std::string{});
    block.tool_call_id = json.value("tool_call_id", std::string{});
    block.tool_name    = json.value("tool_name", std::string{});
    block.arguments    = json.value("arguments", nlohmann::json::object());
    block.media_type   = json.value("media_type", std::string{});
    block.data         = json.value("data", std::string{});
}

inline void to_json(nlohmann::json& json, const Message& message) {
    json = nlohmann::json{
        {"role", std::string{role_name(message.role)}},
        {"content", message.content},
        {"tool_call_id", message.tool_call_id},
    };
}

inline void from_json(const nlohmann::json& json, Message& message) {
    const auto role = parse_role(json.at("role").get<std::string>());
    if (!role) {
        throw nlohmann::json::other_error::create(
            501, "unknown message role: " + json.at("role").get<std::string>(), &json);
    }
    message.role         = *role;
    message.content      = json.value("content", std::vector<ContentBlock>{});
    message.tool_call_id = json.value("tool_call_id", std::string{});
}

inline void to_json(nlohmann::json& json, const Usage& usage) {
    json = nlohmann::json{
        {"input_tokens", usage.input_tokens},
        {"output_tokens", usage.output_tokens},
        {"cached_tokens", usage.cached_tokens},
        {"reasoning_tokens", usage.reasoning_tokens},
    };
}

inline void from_json(const nlohmann::json& json, Usage& usage) {
    usage.input_tokens     = json.value("input_tokens", std::int64_t{0});
    usage.output_tokens    = json.value("output_tokens", std::int64_t{0});
    usage.cached_tokens    = json.value("cached_tokens", std::int64_t{0});
    usage.reasoning_tokens = json.value("reasoning_tokens", std::int64_t{0});
}

} // namespace ymh

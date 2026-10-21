#include "ymh/agent/context_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/session/events.hpp"

namespace ymh {
namespace {

constexpr std::size_t kSchemaBytesPerToken = 4;
constexpr std::size_t kSchemaOverhead = 8;

Message system_message(std::string_view text) {
    Message message;
    message.role = Role::System;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::string(text);
    message.content.push_back(std::move(block));
    return message;
}

void append_note(std::string& note, std::string_view fragment) {
    if (!note.empty()) {
        note += "; ";
    }
    note.append(fragment);
}

std::optional<std::string> last_compaction_summary(const EventRange& events) {
    std::optional<std::string> summary;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ContextCompaction) {
            summary = record.event.payload.get<payload::ContextCompaction>().summary;
        }
    }
    return summary;
}

} // namespace

std::string_view context_segment_token(ContextSegmentKind kind) noexcept {
    switch (kind) {
        case ContextSegmentKind::SystemPrompt:
            return "system_prompt";
        case ContextSegmentKind::ToolSchemas:
            return "tool_schemas";
        case ContextSegmentKind::McpToolSchemas:
            return "mcp_tool_schemas";
        case ContextSegmentKind::Conversation:
            return "conversation";
        case ContextSegmentKind::CompactionSummary:
            return "compaction_summary";
        case ContextSegmentKind::FreeSpace:
            return "free_space";
    }
    return "conversation";
}

std::optional<ContextSegmentKind> parse_context_segment(std::string_view token) noexcept {
    if (token == "system_prompt") {
        return ContextSegmentKind::SystemPrompt;
    }
    if (token == "tool_schemas") {
        return ContextSegmentKind::ToolSchemas;
    }
    if (token == "mcp_tool_schemas") {
        return ContextSegmentKind::McpToolSchemas;
    }
    if (token == "conversation") {
        return ContextSegmentKind::Conversation;
    }
    if (token == "compaction_summary") {
        return ContextSegmentKind::CompactionSummary;
    }
    if (token == "free_space") {
        return ContextSegmentKind::FreeSpace;
    }
    return std::nullopt;
}

std::uint64_t estimate_tool_schema_tokens(const ToolSchema& schema) {
    const std::size_t bytes = schema.name.value.size() + schema.description.size() +
                              schema.input_schema.dump().size();
    return static_cast<std::uint64_t>(bytes / kSchemaBytesPerToken + kSchemaOverhead);
}

ContextSnapshot build_context_snapshot(const ContextSnapshotInputs& inputs) {
    ContextSnapshot snapshot;
    snapshot.session = inputs.header.id;
    snapshot.budget = inputs.budget;

    const std::vector<Message> messages = deriveMessages(inputs.header, inputs.events);
    const std::optional<std::string> summary = last_compaction_summary(inputs.events);

    ContextSegment system;
    system.kind = ContextSegmentKind::SystemPrompt;
    system.provenance = "agent.system_prompt";
    if (!inputs.system_prompt.empty()) {
        system.tokens = inputs.estimator.estimate({system_message(inputs.system_prompt)});
        system.items = 1;
    }
    snapshot.segments.push_back(std::move(system));

    ContextSegment builtin;
    builtin.kind = ContextSegmentKind::ToolSchemas;
    builtin.provenance = "ToolRegistry::schemas()";
    ContextSegment mcp;
    mcp.kind = ContextSegmentKind::McpToolSchemas;
    mcp.provenance = "ToolRegistry::schemas() (mcp.)";
    for (const ToolSchema& schema : inputs.tools) {
        const std::uint64_t tokens = estimate_tool_schema_tokens(schema);
        if (schema.name.value.starts_with("mcp.")) {
            mcp.tokens += tokens;
            mcp.items += 1;
        } else {
            builtin.tokens += tokens;
            builtin.items += 1;
        }
    }
    snapshot.segments.push_back(std::move(builtin));
    snapshot.segments.push_back(std::move(mcp));

    ContextSegment conversation;
    conversation.kind = ContextSegmentKind::Conversation;
    conversation.provenance = "deriveMessages(header, log)";
    if (summary.has_value() && !messages.empty()) {
        const std::vector<Message> tail(messages.begin() + 1, messages.end());
        conversation.tokens = inputs.estimator.estimate(tail);
        conversation.items = messages.size() - 1;
    } else {
        conversation.tokens = inputs.estimator.estimate(messages);
        conversation.items = messages.size();
    }
    snapshot.segments.push_back(std::move(conversation));

    ContextSegment compaction;
    compaction.kind = ContextSegmentKind::CompactionSummary;
    compaction.provenance = "payload::ContextCompaction.summary";
    if (summary.has_value()) {
        compaction.tokens = inputs.estimator.estimate({system_message(*summary)});
        compaction.items = 1;
    }
    snapshot.segments.push_back(std::move(compaction));

    std::uint64_t used = 0;
    for (const ContextSegment& segment : snapshot.segments) {
        used += segment.tokens;
    }
    snapshot.used_tokens = used;

    ContextSegment free;
    free.kind = ContextSegmentKind::FreeSpace;
    free.provenance = "window − used";
    free.tokens = inputs.budget.window_tokens > used ? inputs.budget.window_tokens - used : 0;
    snapshot.segments.push_back(std::move(free));

    snapshot.captured_sequence = inputs.events.empty() ? 0 : inputs.events.back().seq;

    snapshot.tools.reserve(inputs.tools.size());
    for (const ToolSchema& schema : inputs.tools) {
        ContextToolEntry entry;
        entry.name = schema.name.value;
        entry.provenance = schema.name.value.starts_with("mcp.") ? "mcp" : "builtin";
        entry.schema_tokens = estimate_tool_schema_tokens(schema);
        snapshot.tools.push_back(std::move(entry));
    }

    if (inputs.mcp_available) {
        snapshot.mcp_servers.reserve(inputs.mcp_servers.size());
        for (const McpServerStatus& status : inputs.mcp_servers) {
            ContextServerEntry entry;
            entry.id = status.id.value;
            entry.state = std::string{mcp_state_token(status.state)};
            entry.tool_count = status.tool_count;
            entry.skipped = status.skipped_tools.size();
            entry.has_error = !status.last_error.empty();
            snapshot.mcp_servers.push_back(std::move(entry));
        }
    }

    if (inputs.budget.window_tokens == 0) {
        append_note(snapshot.note, "budget unknown");
        snapshot.segments.back().tokens = 0;
    }
    if (snapshot.tools.size() > inputs.max_tools) {
        snapshot.tools.resize(inputs.max_tools);
        snapshot.truncated = true;
        append_note(snapshot.note, "tools truncated");
    }
    if (!inputs.mcp_available) {
        snapshot.mcp_servers.clear();
        append_note(snapshot.note, "mcp status unavailable");
    }

    return snapshot;
}

void to_json(nlohmann::json& json, const ContextSnapshot& snapshot) {
    nlohmann::json segments = nlohmann::json::array();
    for (const ContextSegment& segment : snapshot.segments) {
        segments.push_back(nlohmann::json{{"kind", context_segment_token(segment.kind)},
                                          {"provenance", segment.provenance},
                                          {"tokens", segment.tokens},
                                          {"items", segment.items}});
    }
    nlohmann::json tools = nlohmann::json::array();
    for (const ContextToolEntry& tool : snapshot.tools) {
        tools.push_back(nlohmann::json{{"name", tool.name},
                                       {"provenance", tool.provenance},
                                       {"schema_tokens", tool.schema_tokens}});
    }
    nlohmann::json servers = nlohmann::json::array();
    for (const ContextServerEntry& server : snapshot.mcp_servers) {
        servers.push_back(nlohmann::json{{"id", server.id},
                                         {"state", server.state},
                                         {"tool_count", server.tool_count},
                                         {"skipped", server.skipped},
                                         {"has_error", server.has_error}});
    }
    json = nlohmann::json{
        {"session", snapshot.session.value},
        {"captured_sequence", snapshot.captured_sequence},
        {"used_tokens", snapshot.used_tokens},
        {"budget",
         {{"window_tokens", snapshot.budget.window_tokens},
          {"reserve_output_tokens", snapshot.budget.reserve_output_tokens},
          {"effective_threshold_tokens", snapshot.budget.effective_threshold_tokens}}},
        {"segments", std::move(segments)},
        {"tools", std::move(tools)},
        {"mcp_servers", std::move(servers)},
        {"truncated", snapshot.truncated},
        {"note", snapshot.note},
    };
}

void from_json(const nlohmann::json& json, ContextSnapshot& snapshot) {
    snapshot.session = SessionId{json.at("session").get<std::string>()};
    snapshot.captured_sequence = json.at("captured_sequence").get<std::uint64_t>();
    snapshot.used_tokens = json.at("used_tokens").get<std::uint64_t>();
    const nlohmann::json& budget = json.at("budget");
    snapshot.budget.window_tokens = budget.at("window_tokens").get<std::uint64_t>();
    snapshot.budget.reserve_output_tokens =
        budget.at("reserve_output_tokens").get<std::uint64_t>();
    snapshot.budget.effective_threshold_tokens =
        budget.at("effective_threshold_tokens").get<std::uint64_t>();
    snapshot.segments.clear();
    for (const nlohmann::json& entry : json.at("segments")) {
        ContextSegment segment;
        const std::optional<ContextSegmentKind> kind =
            parse_context_segment(entry.at("kind").get<std::string>());
        segment.kind = kind.value_or(ContextSegmentKind::Conversation);
        segment.provenance = entry.at("provenance").get<std::string>();
        segment.tokens = entry.at("tokens").get<std::uint64_t>();
        segment.items = entry.at("items").get<std::size_t>();
        snapshot.segments.push_back(std::move(segment));
    }
    snapshot.tools.clear();
    for (const nlohmann::json& entry : json.at("tools")) {
        ContextToolEntry tool;
        tool.name = entry.at("name").get<std::string>();
        tool.provenance = entry.at("provenance").get<std::string>();
        tool.schema_tokens = entry.at("schema_tokens").get<std::uint64_t>();
        snapshot.tools.push_back(std::move(tool));
    }
    snapshot.mcp_servers.clear();
    for (const nlohmann::json& entry : json.at("mcp_servers")) {
        ContextServerEntry server;
        server.id = entry.at("id").get<std::string>();
        server.state = entry.at("state").get<std::string>();
        server.tool_count = entry.at("tool_count").get<std::size_t>();
        server.skipped = entry.at("skipped").get<std::size_t>();
        server.has_error = entry.at("has_error").get<bool>();
        snapshot.mcp_servers.push_back(std::move(server));
    }
    snapshot.truncated = json.at("truncated").get<bool>();
    snapshot.note = json.at("note").get<std::string>();
}

} // namespace ymh

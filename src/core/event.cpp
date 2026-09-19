#include "ymh/core/event.hpp"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

namespace ymh {
namespace {

using enum EventType;

struct WireEntry {
    EventType        type;
    std::string_view name;
};

constexpr std::array<WireEntry, 23> kWireNames{{
    {SessionStarted, "session/start"},
    {SessionEnded, "session/end"},
    {TurnStarted, "turn/start"},
    {TurnEnded, "turn/end"},
    {TurnCancelled, "turn/cancel"},
    {TurnFailed, "turn/fail"},
    {StepStarted, "step/start"},
    {StepEnded, "step/end"},
    {UserMessage, "user/message"},
    {AssistantChunk, "assistant/chunk"},
    {AssistantMessage, "assistant/message"},
    {ToolCall, "tool/call"},
    {ToolResult, "tool/result"},
    {PermissionDecision, "permission/decision"},
    {ContextInjected, "context/injected"},
    {ContextCompaction, "context/compaction"},
    {TokenUsage, "usage"},
    {SubagentSpawned, "subagent/spawned"},
    {SubagentFanIn, "subagent/fan_in"},
    {SessionRenamed, "session/renamed"},
    {PlanMode, "plan/mode"},
    {LlmRequestHeader, "llm/request_header"},
    {McpServerStatusChanged, "mcp/server_status_changed"},
}};

} // namespace

std::string_view wire_name(EventType type) noexcept {
    for (const WireEntry& entry : kWireNames) {
        if (entry.type == type) {
            return entry.name;
        }
    }
    return {};
}

std::optional<EventType> parse_event_type(std::string_view name) noexcept {
    for (const WireEntry& entry : kWireNames) {
        if (entry.name == name) {
            return entry.type;
        }
    }
    return std::nullopt;
}

std::span<const EventType> all_event_types() noexcept {
    static constexpr std::array<EventType, kWireNames.size()> kAll = [] {
        std::array<EventType, kWireNames.size()> types{};
        for (std::size_t index = 0; index < kWireNames.size(); ++index) {
            types[index] = kWireNames[index].type;
        }
        return types;
    }();

    return kAll;
}

void to_json(nlohmann::json& json, const Event& event) {
    json = nlohmann::json{
        {"id", event.id.value},
        {"session_id", event.session_id.value},
        {"timestamp",
         std::chrono::duration_cast<std::chrono::milliseconds>(
             event.timestamp.time_since_epoch())
             .count()},
        {"type", std::string{wire_name(event.type)}},
        {"payload", event.payload},
    };
}

void from_json(const nlohmann::json& json, Event& event) {
    event.id.value         = json.at("id").get<std::string>();
    event.session_id.value = json.at("session_id").get<std::string>();
    event.timestamp        = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{json.at("timestamp").get<std::int64_t>()}};
    event.payload = json.at("payload");

    const std::string wire = json.at("type").get<std::string>();
    const std::optional<EventType> type = parse_event_type(wire);
    if (!type.has_value()) {
        throw std::runtime_error{"unknown event type: " + wire};
    }
    event.type = *type;
}

std::optional<Event> try_decode_event(const nlohmann::json& json) {
    const std::string wire = json.at("type").get<std::string>();
    if (!parse_event_type(wire).has_value()) {
        return std::nullopt;
    }
    Event event;
    from_json(json, event);
    return event;
}

} // namespace ymh

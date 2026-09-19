#include "ymh/session/events.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ymh {
namespace {

using payload::AssistantChunkKind;
using payload::PermissionDecisionKind;
using payload::RenameOrigin;
using payload::SessionEndReason;
using payload::SubagentOutcome;
using payload::ToolOutcome;
using payload::TurnOrigin;

[[noreturn]] void reject_enum(std::string_view field, std::string_view value) {
    throw std::runtime_error("unknown " + std::string{field} + ": " + std::string{value});
}

std::int64_t to_epoch_ms(std::chrono::system_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_epoch_ms(std::int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

SessionEndReason parse_session_end_reason(std::string_view value) {
    if (value == "deleted") {
        return SessionEndReason::Deleted;
    }
    if (value == "faulted") {
        return SessionEndReason::Faulted;
    }
    reject_enum("session end reason", value);
}

TurnOrigin parse_turn_origin(std::string_view value) {
    if (value == "user") {
        return TurnOrigin::User;
    }
    if (value == "steer") {
        return TurnOrigin::Steer;
    }
    if (value == "follow_up") {
        return TurnOrigin::FollowUp;
    }
    if (value == "injection") {
        return TurnOrigin::Injection;
    }
    if (value == "maintenance") {
        return TurnOrigin::Maintenance;
    }
    reject_enum("turn origin", value);
}

ToolOutcome parse_tool_outcome(std::string_view value) {
    if (value == "ok") {
        return ToolOutcome::Ok;
    }
    if (value == "error") {
        return ToolOutcome::Error;
    }
    if (value == "denied") {
        return ToolOutcome::Denied;
    }
    if (value == "cancelled") {
        return ToolOutcome::Cancelled;
    }
    reject_enum("tool outcome", value);
}

PermissionDecisionKind parse_permission_decision(std::string_view value) {
    if (value == "allow") {
        return PermissionDecisionKind::Allow;
    }
    if (value == "deny") {
        return PermissionDecisionKind::Deny;
    }
    if (value == "allow_always") {
        return PermissionDecisionKind::AllowAlways;
    }
    reject_enum("permission decision", value);
}

SubagentOutcome parse_subagent_outcome(std::string_view value) {
    if (value == "completed") {
        return SubagentOutcome::Completed;
    }
    if (value == "failed") {
        return SubagentOutcome::Failed;
    }
    if (value == "cancelled") {
        return SubagentOutcome::Cancelled;
    }
    reject_enum("subagent outcome", value);
}

AssistantChunkKind parse_chunk_kind(std::string_view value) {
    if (value == "text") {
        return AssistantChunkKind::Text;
    }
    if (value == "reasoning") {
        return AssistantChunkKind::Reasoning;
    }
    reject_enum("assistant chunk kind", value);
}

RenameOrigin parse_rename_origin(std::string_view value) {
    if (value == "user") {
        return RenameOrigin::User;
    }
    if (value == "auto") {
        return RenameOrigin::Auto;
    }
    reject_enum("rename origin", value);
}

} // namespace

std::string_view session_end_reason_name(payload::SessionEndReason reason) noexcept {
    switch (reason) {
        case payload::SessionEndReason::Deleted:
            return "deleted";
        case payload::SessionEndReason::Faulted:
            return "faulted";
    }
    return {};
}

std::string_view turn_origin_name(payload::TurnOrigin origin) noexcept {
    switch (origin) {
        case payload::TurnOrigin::User:
            return "user";
        case payload::TurnOrigin::Steer:
            return "steer";
        case payload::TurnOrigin::FollowUp:
            return "follow_up";
        case payload::TurnOrigin::Injection:
            return "injection";
        case payload::TurnOrigin::Maintenance:
            return "maintenance";
    }
    return {};
}

std::string_view tool_outcome_name(payload::ToolOutcome outcome) noexcept {
    switch (outcome) {
        case payload::ToolOutcome::Ok:
            return "ok";
        case payload::ToolOutcome::Error:
            return "error";
        case payload::ToolOutcome::Denied:
            return "denied";
        case payload::ToolOutcome::Cancelled:
            return "cancelled";
    }
    return {};
}

std::string_view permission_decision_name(payload::PermissionDecisionKind decision) noexcept {
    switch (decision) {
        case payload::PermissionDecisionKind::Allow:
            return "allow";
        case payload::PermissionDecisionKind::Deny:
            return "deny";
        case payload::PermissionDecisionKind::AllowAlways:
            return "allow_always";
    }
    return {};
}

std::string_view subagent_outcome_name(payload::SubagentOutcome outcome) noexcept {
    switch (outcome) {
        case payload::SubagentOutcome::Completed:
            return "completed";
        case payload::SubagentOutcome::Failed:
            return "failed";
        case payload::SubagentOutcome::Cancelled:
            return "cancelled";
    }
    return {};
}

std::string_view assistant_chunk_kind_name(payload::AssistantChunkKind kind) noexcept {
    switch (kind) {
        case payload::AssistantChunkKind::Text:
            return "text";
        case payload::AssistantChunkKind::Reasoning:
            return "reasoning";
    }
    return {};
}

std::string_view rename_origin_name(payload::RenameOrigin origin) noexcept {
    switch (origin) {
        case payload::RenameOrigin::User:
            return "user";
        case payload::RenameOrigin::Auto:
            return "auto";
    }
    return {};
}

namespace payload {

void to_json(nlohmann::json& json, const SessionStarted& value) {
    json = nlohmann::json{
        {"model", value.model},
        {"server_profile", value.serverProfile},
        {"title", value.title},
    };
}

void from_json(const nlohmann::json& json, SessionStarted& value) {
    value.model         = json.at("model").get<std::string>();
    value.serverProfile = json.at("server_profile").get<std::string>();
    value.title         = json.value("title", std::string{});
}

void to_json(nlohmann::json& json, const SessionEnded& value) {
    json = nlohmann::json{{"reason", std::string{session_end_reason_name(value.reason)}}};
}

void from_json(const nlohmann::json& json, SessionEnded& value) {
    value.reason = parse_session_end_reason(json.at("reason").get<std::string>());
}

void to_json(nlohmann::json& json, const TurnStarted& value) {
    json = nlohmann::json{
        {"turn", value.turn},
        {"origin", std::string{turn_origin_name(value.origin)}},
    };
}

void from_json(const nlohmann::json& json, TurnStarted& value) {
    value.turn   = json.at("turn").get<TurnId>();
    value.origin = parse_turn_origin(json.at("origin").get<std::string>());
}

void to_json(nlohmann::json& json, const TurnEnded& value) {
    json = nlohmann::json{{"turn", value.turn}};
}

void from_json(const nlohmann::json& json, TurnEnded& value) {
    value.turn = json.at("turn").get<TurnId>();
}

void to_json(nlohmann::json& json, const TurnCancelled& value) {
    json = nlohmann::json{{"turn", value.turn}, {"reason", value.reason}};
}

void from_json(const nlohmann::json& json, TurnCancelled& value) {
    value.turn   = json.at("turn").get<TurnId>();
    value.reason = json.value("reason", std::string{});
}

void to_json(nlohmann::json& json, const TurnFailed& value) {
    json = nlohmann::json{
        {"turn", value.turn},
        {"code", value.code},
        {"message", value.message},
    };
}

void from_json(const nlohmann::json& json, TurnFailed& value) {
    value.turn    = json.at("turn").get<TurnId>();
    value.code    = json.value("code", std::string{});
    value.message = json.value("message", std::string{});
}

void to_json(nlohmann::json& json, const StepStarted& value) {
    json = nlohmann::json{{"turn", value.turn}, {"step", value.step}};
}

void from_json(const nlohmann::json& json, StepStarted& value) {
    value.turn = json.at("turn").get<TurnId>();
    value.step = json.at("step").get<StepId>();
}

void to_json(nlohmann::json& json, const StepEnded& value) {
    json = nlohmann::json{{"turn", value.turn}, {"step", value.step}};
}

void from_json(const nlohmann::json& json, StepEnded& value) {
    value.turn = json.at("turn").get<TurnId>();
    value.step = json.at("step").get<StepId>();
}

void to_json(nlohmann::json& json, const UserMessage& value) {
    json = nlohmann::json{{"id", value.id}, {"content", value.content}};
}

void from_json(const nlohmann::json& json, UserMessage& value) {
    value.id      = json.at("id").get<MessageId>();
    value.content = json.value("content", std::vector<ContentBlock>{});
}

void to_json(nlohmann::json& json, const AssistantChunk& value) {
    json = nlohmann::json{
        {"message", value.message},
        {"index", value.index},
        {"text", value.text},
        {"kind", std::string{assistant_chunk_kind_name(value.kind)}},
    };
}

void from_json(const nlohmann::json& json, AssistantChunk& value) {
    value.message = json.at("message").get<MessageId>();
    value.index   = json.at("index").get<std::size_t>();
    value.text    = json.value("text", std::string{});
    value.kind    = parse_chunk_kind(json.at("kind").get<std::string>());
}

void to_json(nlohmann::json& json, const AssistantMessage& value) {
    json = nlohmann::json{{"id", value.id}, {"content", value.content}};
    if (value.usage.has_value()) {
        json["usage"] = *value.usage;
    }
}

void from_json(const nlohmann::json& json, AssistantMessage& value) {
    value.id      = json.at("id").get<MessageId>();
    value.content = json.value("content", std::vector<ContentBlock>{});
    if (json.contains("usage") && !json.at("usage").is_null()) {
        value.usage = json.at("usage").get<Usage>();
    } else {
        value.usage = std::nullopt;
    }
}

void to_json(nlohmann::json& json, const ToolCall& value) {
    json = nlohmann::json{
        {"id", value.id},
        {"turn", value.turn},
        {"step", value.step},
        {"name", value.name},
        {"arguments", value.arguments},
        {"requested_at", to_epoch_ms(value.requestedAt)},
    };
}

void from_json(const nlohmann::json& json, ToolCall& value) {
    value.id          = json.at("id").get<ToolCallId>();
    value.turn        = json.at("turn").get<TurnId>();
    value.step        = json.at("step").get<StepId>();
    value.name        = json.value("name", std::string{});
    value.arguments   = json.value("arguments", nlohmann::json::object());
    value.requestedAt = from_epoch_ms(json.value("requested_at", std::int64_t{0}));
}

void to_json(nlohmann::json& json, const ToolResult& value) {
    json = nlohmann::json{
        {"id", value.id},
        {"name", value.name},
        {"outcome", std::string{tool_outcome_name(value.outcome)}},
        {"output", value.output},
        {"truncated", value.truncated},
        {"duration_ms", value.duration.count()},
    };
    if (value.error.has_value()) {
        json["error"] = *value.error;
    }
}

void from_json(const nlohmann::json& json, ToolResult& value) {
    value.id        = json.at("id").get<ToolCallId>();
    value.name      = json.value("name", std::string{});
    value.outcome   = parse_tool_outcome(json.at("outcome").get<std::string>());
    value.output    = json.value("output", std::string{});
    value.truncated = json.value("truncated", false);
    value.duration  = std::chrono::milliseconds{json.value("duration_ms", std::int64_t{0})};
    if (json.contains("error") && !json.at("error").is_null()) {
        value.error = json.at("error").get<std::string>();
    } else {
        value.error = std::nullopt;
    }
}

void to_json(nlohmann::json& json, const PermissionDecision& value) {
    json = nlohmann::json{
        {"call", value.call},
        {"decision", std::string{permission_decision_name(value.decision)}},
        {"reason", value.reason},
    };
}

void from_json(const nlohmann::json& json, PermissionDecision& value) {
    value.call     = json.at("call").get<ToolCallId>();
    value.decision = parse_permission_decision(json.at("decision").get<std::string>());
    value.reason   = json.value("reason", std::string{});
}

void to_json(nlohmann::json& json, const ContextInjected& value) {
    json = nlohmann::json{
        {"id", value.id},
        {"role", std::string{role_name(value.role)}},
        {"text", value.text},
    };
}

void from_json(const nlohmann::json& json, ContextInjected& value) {
    value.id   = json.at("id").get<MessageId>();
    const auto role = parse_role(json.at("role").get<std::string>());
    if (!role) {
        reject_enum("role", json.at("role").get<std::string>());
    }
    value.role = *role;
    value.text = json.value("text", std::string{});
}

void to_json(nlohmann::json& json, const ContextCompaction& value) {
    json = nlohmann::json{
        {"boundary", value.boundary},
        {"summary", value.summary},
        {"token_estimate", value.tokenEstimate},
        {"model", value.model},
        {"created_at", to_epoch_ms(value.createdAt)},
    };
}

void from_json(const nlohmann::json& json, ContextCompaction& value) {
    value.boundary      = json.at("boundary").get<Sequence>();
    value.summary       = json.value("summary", std::string{});
    value.tokenEstimate = json.value("token_estimate", std::size_t{0});
    value.model         = json.value("model", std::string{});
    value.createdAt     = from_epoch_ms(json.value("created_at", std::int64_t{0}));
}

void to_json(nlohmann::json& json, const TokenUsage& value) {
    json = nlohmann::json{{"usage", value.usage}};
    if (value.turn.has_value()) {
        json["turn"] = *value.turn;
    }
}

void from_json(const nlohmann::json& json, TokenUsage& value) {
    value.usage = json.at("usage").get<Usage>();
    if (json.contains("turn") && !json.at("turn").is_null()) {
        value.turn = json.at("turn").get<TurnId>();
    } else {
        value.turn = std::nullopt;
    }
}

void to_json(nlohmann::json& json, const SubagentSpawned& value) {
    json = nlohmann::json{{"subagent", value.subagent.value}, {"task", value.task}};
}

void from_json(const nlohmann::json& json, SubagentSpawned& value) {
    value.subagent.value = json.at("subagent").get<std::string>();
    value.task           = json.value("task", std::string{});
}

void to_json(nlohmann::json& json, const SubagentFanIn& value) {
    json = nlohmann::json{
        {"subagent", value.subagent.value},
        {"outcome", std::string{subagent_outcome_name(value.outcome)}},
        {"summary", value.summary},
    };
}

void from_json(const nlohmann::json& json, SubagentFanIn& value) {
    value.subagent.value = json.at("subagent").get<std::string>();
    value.outcome        = parse_subagent_outcome(json.at("outcome").get<std::string>());
    value.summary        = json.value("summary", std::string{});
}

void to_json(nlohmann::json& json, const SessionRenamed& value) {
    json = nlohmann::json{
        {"title", value.title},
        {"origin", std::string{rename_origin_name(value.origin)}},
    };
}

void from_json(const nlohmann::json& json, SessionRenamed& value) {
    value.title  = json.at("title").get<std::string>();
    value.origin = parse_rename_origin(json.at("origin").get<std::string>());
}

void to_json(nlohmann::json& json, const PlanMode& value) {
    json = nlohmann::json{{"active", value.active}};
}

void from_json(const nlohmann::json& json, PlanMode& value) {
    value.active = json.value("active", false);
}

} // namespace payload

} // namespace ymh

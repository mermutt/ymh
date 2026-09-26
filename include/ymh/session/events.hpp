#pragma once

// Durable session event payloads and the `SessionEventMap` type<->payload map,
// pinned by 01-session.md §4.4-§4.5. Payloads live in `namespace ymh::payload`
// so a payload name never collides with the `EventType` enumerator of the same
// name. Only durable events appear here; live events (00 §8.1) never do (I13).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/ids.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/commands/command_types.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/omission.hpp"
#include "ymh/goal/goal.hpp"
#include "ymh/jobs/job_types.hpp"
#include "ymh/llm/assistant_stream.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/stream.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {
namespace payload {

// ---- session lifecycle (01 §4.5, §9.1-§9.2) --------------------------------

struct SessionStarted {
    std::string model;
    std::string serverProfile;
    std::string title;
    std::string model_name;
};

enum class SessionEndReason : std::uint8_t {
    Deleted,
    Faulted,
};

struct SessionEnded {
    SessionEndReason reason = SessionEndReason::Deleted;
};

// ---- turn / step taxonomy (01 §4.5, §11) -----------------------------------

enum class TurnOrigin : std::uint8_t {
    User,
    Steer,
    FollowUp,
    Injection,
    // The dedicated maintenance turn that services a manual `/compact`
    // (13-context-compaction.md §6.7, errata A1). Distinct from `Injection`,
    // which is reserved for a `ContextInjected` that materializes as a turn.
    Maintenance,
};

struct TurnStarted {
    TurnId     turn = 0;
    TurnOrigin origin = TurnOrigin::User;
};

struct TurnEnded {
    TurnId turn = 0;
    // 55-A13: additive persisted finish reason (omitted when Stop); lets the
    // 55-D9 token-limit/refusal stop-reason rows be derived from the log.
    std::optional<FinishReason> finish_reason = std::nullopt;
};

struct TurnCancelled {
    TurnId      turn = 0;
    std::string reason;
};

struct TurnFailed {
    TurnId      turn = 0;
    std::string code;
    std::string message;
};

struct StepStarted {
    TurnId turn = 0;
    StepId step = 0;
};

struct StepEnded {
    TurnId turn = 0;
    StepId step = 0;
};

// ---- messages (01 §4.5) ----------------------------------------------------

struct UserMessage {
    MessageId                 id;
    std::vector<ContentBlock> content;
    MessageSource             source{};
};

enum class AssistantChunkKind : std::uint8_t {
    Text,
    Reasoning,
};

struct AssistantChunk {
    MessageId          message;
    std::size_t        index = 0;
    std::string        text;
    AssistantChunkKind kind = AssistantChunkKind::Text;
};

struct AssistantMessage {
    MessageId                          id;
    std::vector<ContentBlock>          content;
    std::optional<Usage>               usage;
    // 29-D3 / 26 §4.3.9.1 :949: additive, defaulted stream + replay state.
    std::vector<AssistantStreamRecord> stream;
    std::optional<ReplayEnvelope>      replay_state;
    MessageSource                      source = message_source(MessageSource::Kind::Model);
};

// 29-D2 / 26 §4.3.9.1 :957: a settled non-Completed provider attempt. Durable,
// projection-invisible (deriveMessages ignores it).
struct AssistantAttempt {
    TurnId                             turn = 0;
    StepId                             step = 0;
    std::vector<AssistantStreamRecord> stream;
};

// ---- tool pipeline (01 §4.5, §11, §14) -------------------------------------

struct ToolCall {
    ToolCallId                            id;
    TurnId                                turn = 0;
    StepId                                step = 0;
    std::string                           name;
    nlohmann::json                        arguments = nlohmann::json::object();
    std::chrono::system_clock::time_point requestedAt{};
};

struct ToolResult {
    ToolCallId                 id;
    std::string                name;
    ToolOutcome                outcome = ToolOutcome::Ok;
    std::string                output;      // retained text + retention notice
    bool                       truncated = false;   // == (omitted_kind != None)
    OmittedKind                omitted_kind = OmittedKind::None;
    std::size_t                omitted_count = 0;
    std::optional<std::string> error;
    std::chrono::milliseconds  duration{0};
    MessageSource              source = tool_message_source(
        id.empty() ? std::optional<ToolCallId>{} : std::optional<ToolCallId>{id});
    ContextFormed              context{};
};

// ---- permissions (01 §4.5, §19) --------------------------------------------

enum class PermissionDecisionKind : std::uint8_t {
    Allow,
    Deny,
    AllowAlways,
};

struct PermissionDecision {
    ToolCallId             call;
    PermissionDecisionKind decision = PermissionDecisionKind::Allow;
    std::string            reason;
};

// ---- context (01 §4.5, §31-§33) --------------------------------------------

struct ContextInjected {
    MessageId     id;
    Role          role = Role::User;
    std::string   text;
    MessageSource source = message_source(MessageSource::Kind::Plugin);
    ContextFormed context{};
};

struct ContextCompaction {
    Sequence                              boundary = 0;
    std::string                           summary;
    std::size_t                           tokenEstimate = 0;
    std::string                           model;
    std::chrono::system_clock::time_point createdAt{};
    // D13 additions (32-compaction-errata.md §3.2; 26 §4.3.9.1). Additive on
    // the wire: every field is optional/defaulted so an older reader ignores
    // them (C21, C-F19).
    std::string           provider;
    Sequence              shadowedStart = 0;
    Sequence              shadowedEnd = 0;
    std::vector<Sequence> shadowedSeqs;
    std::uint64_t         shadowedTokenCount = 0;
};

// The tool-result pruner's shadow-price event (32-compaction-errata.md §5.1;
// 26 §4.3.9). It never deletes: it precedes a same-`id` replacement
// `ToolResult` and carries the shadowed set for audit (C22, C26).
struct ContextPrune {
    Sequence              shadowedStart = 0;
    Sequence              shadowedEnd = 0;
    std::vector<Sequence> shadowedSeqs;
    std::uint64_t         shadowedTokenCount = 0;
};

struct TokenUsage {
    Usage                 usage;
    std::optional<TurnId> turn;
};

// ---- subagents (01 §4.5, §30) ----------------------------------------------

struct SubagentSpawned {
    SessionId   subagent;
    std::string task;
};

enum class SubagentOutcome : std::uint8_t {
    Completed,
    Failed,
    Cancelled,
};

struct SubagentFanIn {
    SessionId       subagent;
    SubagentOutcome outcome = SubagentOutcome::Completed;
    std::string     summary;
    // 55-A11: true iff the epoch was background-registered (any non-foreground
    // epoch, including one opened by send_message); the replay rule filters on it.
    bool            notice_expected = false;
};

// ---- rename (19 §5.1) ------------------------------------------------------

// 19 §5.1: who produced a rename. `User` is an explicit /rename or
// session.rename; `Auto` is the daemon's first-turn heuristic (19 §4.3).
enum class RenameOrigin : std::uint8_t {
    User,
    Auto,
};

// 19 §5.1: the only way a title changes after creation. Append-only; the
// latest SessionRenamed in the session's own log is authoritative (RN2).
struct SessionRenamed {
    std::string  title;
    RenameOrigin origin = RenameOrigin::User;
};

// ---- plan mode (25-D2) -----------------------------------------------------

// Whole-value-replace collaboration state. The last `plan/mode` in a session
// log wins; a log with none folds to inactive (25-D2, UX11).
struct PlanMode {
    bool active = false;
};

// ---- session model (53-D6) -------------------------------------------------

// Whole-value-replace session model selection. The last `session/model` in a
// session log wins; a log with none keeps `SessionStarted.model`. `model` is the
// wire id; `model_name` is the `llm.models` entry name ("" for a literal id).
struct SessionModelChanged {
    std::string model;
    std::string model_name;
};

// ---- LLM request header (28 §5.2, 29 §3.2) ---------------------------------

// A changed snapshot, not a per-dispatch record: logged at a request-series
// start and whenever config/prompt/tools/purpose change (26-D2, L21). It is
// projection-invisible (28 §4.2): `deriveMessages` ignores it.
struct LlmRequestHeader {
    TurnId                     turn = 0;
    StepId                     step = 0;
    SessionId                  session_id;
    std::optional<CallPurpose> purpose;
    LlmCallConfig              config;
    std::string                system_prompt_digest;
    std::optional<std::string> system_prompt;
    std::vector<std::string>   tool_names;
    std::vector<std::string>   tool_schema_digests;
    std::string                template_digest;
    bool                       starts_series = true;
};

// ---- agent preset (42 §5.1, 43 §1.3) ---------------------------------------

// dsh agent-preset/selected. The payload carries only the preset id; the event
// envelope owns id/timestamp (42 §5.1).
struct AgentPresetSelected {
    std::string agent_preset;
};

// ---- goal (44 §5.7, 26 §4.3.9.1) -------------------------------------------

// dsh GoalChangeMeta (dsh-goal/lib/types/domain.d.ts:12-32), lower-snake ymh
// form. The envelope owns id/timestamp; the payload carries the operation, the
// whole snapshot (non-clear) or the tombstone ref (clear), and the admitted
// round count at the mutation (44 §6.2).
enum class GoalOperation : std::uint8_t {
    Create,
    Edit,
    Pause,
    Resume,
    Complete,
    Block,
    Clear,
};

struct GoalChange {
    GoalOperation               operation = GoalOperation::Create;
    std::optional<GoalSnapshot> goal;                 // present for non-Clear
    std::optional<GoalRef>      cleared;              // present for Clear
    std::uint32_t               rounds_started = 0;   // non-Clear
};

// ---- commands (44 §5.7, 26 §4.3.9.1) ---------------------------------------

// dsh command/run (dsh-commands types.d.ts:99-117). Log-only: ignored by
// deriveMessages and token accounting (44-I12). `args` is omitted when the
// spec set record_input=false (44-D18).
struct CommandRun {
    CommandId                  command_id = 0;
    std::string                name;
    std::optional<std::string> args;
    CommandSource              source = CommandSource::User;
};

enum class CommandDoneKind : std::uint8_t { Success, Error };

// dsh command/done, paired by `command_id`.
struct CommandDone {
    CommandId                  command_id = 0;
    CommandDoneKind            kind = CommandDoneKind::Success;
    std::optional<std::string> text;
    std::optional<Sequence>    source_event_seq;
};

// ---- jobs (44 §5.7) --------------------------------------------------------

// ymh-local job registry change (owner-scoped; no dsh session-event analogue).
// `owner` is absent when the job is unowned (44-D10, Rev 2).
struct JobChanged {
    std::optional<AgentId> owner;
    std::string            kind;
    std::uint64_t          ordinal = 0;
    JobStatus              status = JobStatus::Running;
    std::string            label;
};

} // namespace payload

// ---------------------------------------------------------------------------
// SessionEventMap: EventType -> payload type (01 §4.4)
// ---------------------------------------------------------------------------

template <>
struct SessionEventMap<EventType::SessionStarted> {
    using type = payload::SessionStarted;
};
template <>
struct SessionEventMap<EventType::SessionEnded> {
    using type = payload::SessionEnded;
};
template <>
struct SessionEventMap<EventType::TurnStarted> {
    using type = payload::TurnStarted;
};
template <>
struct SessionEventMap<EventType::TurnEnded> {
    using type = payload::TurnEnded;
};
template <>
struct SessionEventMap<EventType::TurnCancelled> {
    using type = payload::TurnCancelled;
};
template <>
struct SessionEventMap<EventType::TurnFailed> {
    using type = payload::TurnFailed;
};
template <>
struct SessionEventMap<EventType::StepStarted> {
    using type = payload::StepStarted;
};
template <>
struct SessionEventMap<EventType::StepEnded> {
    using type = payload::StepEnded;
};
template <>
struct SessionEventMap<EventType::UserMessage> {
    using type = payload::UserMessage;
};
template <>
struct SessionEventMap<EventType::AssistantMessage> {
    using type = payload::AssistantMessage;
};
template <>
struct SessionEventMap<EventType::AssistantAttempt> {
    using type = payload::AssistantAttempt;
};
template <>
struct SessionEventMap<EventType::ToolCall> {
    using type = payload::ToolCall;
};
template <>
struct SessionEventMap<EventType::ToolResult> {
    using type = payload::ToolResult;
};
template <>
struct SessionEventMap<EventType::PermissionDecision> {
    using type = payload::PermissionDecision;
};
template <>
struct SessionEventMap<EventType::ContextInjected> {
    using type = payload::ContextInjected;
};
template <>
struct SessionEventMap<EventType::ContextCompaction> {
    using type = payload::ContextCompaction;
};
template <>
struct SessionEventMap<EventType::ContextPrune> {
    using type = payload::ContextPrune;
};
template <>
struct SessionEventMap<EventType::TokenUsage> {
    using type = payload::TokenUsage;
};
template <>
struct SessionEventMap<EventType::SubagentSpawned> {
    using type = payload::SubagentSpawned;
};
template <>
struct SessionEventMap<EventType::SubagentFanIn> {
    using type = payload::SubagentFanIn;
};
template <>
struct SessionEventMap<EventType::SessionRenamed> {
    using type = payload::SessionRenamed;
};
template <>
struct SessionEventMap<EventType::PlanMode> {
    using type = payload::PlanMode;
};
template <>
struct SessionEventMap<EventType::LlmRequestHeader> {
    using type = payload::LlmRequestHeader;
};
template <>
struct SessionEventMap<EventType::AgentPresetSelected> {
    using type = payload::AgentPresetSelected;
};
template <>
struct SessionEventMap<EventType::GoalChange> {
    using type = payload::GoalChange;
};
template <>
struct SessionEventMap<EventType::CommandRun> {
    using type = payload::CommandRun;
};
template <>
struct SessionEventMap<EventType::CommandDone> {
    using type = payload::CommandDone;
};
template <>
struct SessionEventMap<EventType::JobChanged> {
    using type = payload::JobChanged;
};
template <>
struct SessionEventMap<EventType::SessionModelChanged> {
    using type = payload::SessionModelChanged;
};

// Payload type -> EventType (01 §4.4).
template <>
struct EventTraits<payload::SessionStarted> {
    static constexpr EventType type = EventType::SessionStarted;
};
template <>
struct EventTraits<payload::SessionEnded> {
    static constexpr EventType type = EventType::SessionEnded;
};
template <>
struct EventTraits<payload::TurnStarted> {
    static constexpr EventType type = EventType::TurnStarted;
};
template <>
struct EventTraits<payload::TurnEnded> {
    static constexpr EventType type = EventType::TurnEnded;
};
template <>
struct EventTraits<payload::TurnCancelled> {
    static constexpr EventType type = EventType::TurnCancelled;
};
template <>
struct EventTraits<payload::TurnFailed> {
    static constexpr EventType type = EventType::TurnFailed;
};
template <>
struct EventTraits<payload::StepStarted> {
    static constexpr EventType type = EventType::StepStarted;
};
template <>
struct EventTraits<payload::StepEnded> {
    static constexpr EventType type = EventType::StepEnded;
};
template <>
struct EventTraits<payload::UserMessage> {
    static constexpr EventType type = EventType::UserMessage;
};
template <>
struct EventTraits<payload::AssistantChunk> {
    static constexpr EventType type = EventType::AssistantChunk;
};
template <>
struct EventTraits<payload::AssistantMessage> {
    static constexpr EventType type = EventType::AssistantMessage;
};
template <>
struct EventTraits<payload::AssistantAttempt> {
    static constexpr EventType type = EventType::AssistantAttempt;
};
template <>
struct EventTraits<payload::ToolCall> {
    static constexpr EventType type = EventType::ToolCall;
};
template <>
struct EventTraits<payload::ToolResult> {
    static constexpr EventType type = EventType::ToolResult;
};
template <>
struct EventTraits<payload::PermissionDecision> {
    static constexpr EventType type = EventType::PermissionDecision;
};
template <>
struct EventTraits<payload::ContextInjected> {
    static constexpr EventType type = EventType::ContextInjected;
};
template <>
struct EventTraits<payload::ContextCompaction> {
    static constexpr EventType type = EventType::ContextCompaction;
};
template <>
struct EventTraits<payload::ContextPrune> {
    static constexpr EventType type = EventType::ContextPrune;
};
template <>
struct EventTraits<payload::TokenUsage> {
    static constexpr EventType type = EventType::TokenUsage;
};
template <>
struct EventTraits<payload::SubagentSpawned> {
    static constexpr EventType type = EventType::SubagentSpawned;
};
template <>
struct EventTraits<payload::SubagentFanIn> {
    static constexpr EventType type = EventType::SubagentFanIn;
};
template <>
struct EventTraits<payload::SessionRenamed> {
    static constexpr EventType type = EventType::SessionRenamed;
};
template <>
struct EventTraits<payload::PlanMode> {
    static constexpr EventType type = EventType::PlanMode;
};
template <>
struct EventTraits<payload::LlmRequestHeader> {
    static constexpr EventType type = EventType::LlmRequestHeader;
};
template <>
struct EventTraits<payload::AgentPresetSelected> {
    static constexpr EventType type = EventType::AgentPresetSelected;
};
template <>
struct EventTraits<payload::GoalChange> {
    static constexpr EventType type = EventType::GoalChange;
};
template <>
struct EventTraits<payload::CommandRun> {
    static constexpr EventType type = EventType::CommandRun;
};
template <>
struct EventTraits<payload::CommandDone> {
    static constexpr EventType type = EventType::CommandDone;
};
template <>
struct EventTraits<payload::JobChanged> {
    static constexpr EventType type = EventType::JobChanged;
};
template <>
struct EventTraits<payload::SessionModelChanged> {
    static constexpr EventType type = EventType::SessionModelChanged;
};

// Total payload-name mapping used by tests and diagnostics.
[[nodiscard]] std::string_view session_end_reason_name(payload::SessionEndReason reason) noexcept;
[[nodiscard]] std::string_view turn_origin_name(payload::TurnOrigin origin) noexcept;
[[nodiscard]] std::string_view tool_outcome_name(payload::ToolOutcome outcome) noexcept;
[[nodiscard]] std::string_view permission_decision_name(
    payload::PermissionDecisionKind decision) noexcept;
[[nodiscard]] std::string_view subagent_outcome_name(payload::SubagentOutcome outcome) noexcept;
[[nodiscard]] std::string_view assistant_chunk_kind_name(
    payload::AssistantChunkKind kind) noexcept;
[[nodiscard]] std::string_view rename_origin_name(payload::RenameOrigin origin) noexcept;

} // namespace ymh

// ---------------------------------------------------------------------------
// JSON (persistence / wire). ADL `to_json` / `from_json` per payload; unknown
// enum strings fail loudly (01 S3).
// ---------------------------------------------------------------------------

namespace ymh::payload {

void to_json(nlohmann::json& json, const SessionStarted& value);
void from_json(const nlohmann::json& json, SessionStarted& value);
void to_json(nlohmann::json& json, const SessionEnded& value);
void from_json(const nlohmann::json& json, SessionEnded& value);
void to_json(nlohmann::json& json, const TurnStarted& value);
void from_json(const nlohmann::json& json, TurnStarted& value);
void to_json(nlohmann::json& json, const TurnEnded& value);
void from_json(const nlohmann::json& json, TurnEnded& value);
void to_json(nlohmann::json& json, const TurnCancelled& value);
void from_json(const nlohmann::json& json, TurnCancelled& value);
void to_json(nlohmann::json& json, const TurnFailed& value);
void from_json(const nlohmann::json& json, TurnFailed& value);
void to_json(nlohmann::json& json, const StepStarted& value);
void from_json(const nlohmann::json& json, StepStarted& value);
void to_json(nlohmann::json& json, const StepEnded& value);
void from_json(const nlohmann::json& json, StepEnded& value);
void to_json(nlohmann::json& json, const UserMessage& value);
void from_json(const nlohmann::json& json, UserMessage& value);
void to_json(nlohmann::json& json, const AssistantChunk& value);
void from_json(const nlohmann::json& json, AssistantChunk& value);
void to_json(nlohmann::json& json, const AssistantMessage& value);
void from_json(const nlohmann::json& json, AssistantMessage& value);
void to_json(nlohmann::json& json, const AssistantAttempt& value);
void from_json(const nlohmann::json& json, AssistantAttempt& value);
void to_json(nlohmann::json& json, const ToolCall& value);
void from_json(const nlohmann::json& json, ToolCall& value);
void to_json(nlohmann::json& json, const ToolResult& value);
void from_json(const nlohmann::json& json, ToolResult& value);
void to_json(nlohmann::json& json, const PermissionDecision& value);
void from_json(const nlohmann::json& json, PermissionDecision& value);
void to_json(nlohmann::json& json, const ContextInjected& value);
void from_json(const nlohmann::json& json, ContextInjected& value);
void to_json(nlohmann::json& json, const ContextCompaction& value);
void from_json(const nlohmann::json& json, ContextCompaction& value);
void to_json(nlohmann::json& json, const ContextPrune& value);
void from_json(const nlohmann::json& json, ContextPrune& value);
void to_json(nlohmann::json& json, const TokenUsage& value);
void from_json(const nlohmann::json& json, TokenUsage& value);
void to_json(nlohmann::json& json, const SubagentSpawned& value);
void from_json(const nlohmann::json& json, SubagentSpawned& value);
void to_json(nlohmann::json& json, const SubagentFanIn& value);
void from_json(const nlohmann::json& json, SubagentFanIn& value);
void to_json(nlohmann::json& json, const SessionRenamed& value);
void from_json(const nlohmann::json& json, SessionRenamed& value);
void to_json(nlohmann::json& json, const PlanMode& value);
void from_json(const nlohmann::json& json, PlanMode& value);
void to_json(nlohmann::json& json, const LlmRequestHeader& value);
void from_json(const nlohmann::json& json, LlmRequestHeader& value);
void to_json(nlohmann::json& json, const AgentPresetSelected& value);
void from_json(const nlohmann::json& json, AgentPresetSelected& value);
void to_json(nlohmann::json& json, const GoalChange& value);
void from_json(const nlohmann::json& json, GoalChange& value);
void to_json(nlohmann::json& json, const CommandRun& value);
void from_json(const nlohmann::json& json, CommandRun& value);
void to_json(nlohmann::json& json, const CommandDone& value);
void from_json(const nlohmann::json& json, CommandDone& value);
void to_json(nlohmann::json& json, const JobChanged& value);
void from_json(const nlohmann::json& json, JobChanged& value);
void to_json(nlohmann::json& json, const SessionModelChanged& value);
void from_json(const nlohmann::json& json, SessionModelChanged& value);

} // namespace ymh::payload

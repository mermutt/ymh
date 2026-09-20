#include "ymh/ui/ui_event_adapter.hpp"

#include <utility>

#include "ymh/agent/message.hpp"
#include "ymh/session/events.hpp"

namespace ymh::ui {
namespace {

constexpr std::size_t kMaxArgumentsPreview = 512;
constexpr std::size_t kMaxAppliedEventIds = 8192;

std::string flatten_content(const std::vector<ContentBlock>& content) {
    std::string text;
    for (const ContentBlock& block : content) {
        if (block.kind != ContentBlockKind::Text || block.text.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += '\n';
        }
        text += block.text;
    }
    return text;
}

} // namespace

std::string summarize_tool_arguments(const std::string& name, const nlohmann::json& arguments) {
    (void)name;
    std::string dumped = arguments.dump();
    if (dumped.size() > kMaxArgumentsPreview) {
        dumped.resize(kMaxArgumentsPreview);
        dumped += "...";
    }
    return dumped;
}

UiEventAdapter::UiEventAdapter(UiModel& model) : model_(model) {}

UiEventAdapter::UiEventAdapter(UiModel& model, UiController& controller)
    : model_(model) {
    (void)controller;
}

UiEventAdapter::~UiEventAdapter() = default;

void UiEventAdapter::set_state_provider(
    std::function<AgentState(const SessionId&)> provider) {
    state_provider_ = std::move(provider);
}

AgentState UiEventAdapter::project_state(const SessionId& session, const Event& event) const {
    if (state_provider_) {
        return state_provider_(session);
    }
    const auto previous = lastState_.find(session);
    const AgentState old = previous == lastState_.end() ? AgentState::Idle : previous->second;
    switch (event.type) {
        case EventType::TurnStarted:
        case EventType::StepStarted:
        case EventType::UserMessage:
            return AgentState::Thinking;
        case EventType::ToolCall:
            return AgentState::CallingTool;
        case EventType::ToolResult:
            return AgentState::Thinking;
        case EventType::TurnEnded:
        case EventType::TurnCancelled:
        case EventType::SessionEnded:
            return AgentState::Idle;
        case EventType::TurnFailed:
            return AgentState::Error;
        case EventType::AssistantAttempt:
            return old;
        default:
            return old;
    }
}

std::vector<UiEvent> UiEventAdapter::adapt(const Event& event) const {
    std::vector<UiEvent> events;
    const SessionId session = event.session_id;

    switch (event.type) {
        case EventType::UserMessage: {
            const auto payload = event.payload.get<payload::UserMessage>();
            events.push_back(UiEvent{UserMessage{session, payload.id,
                                                 flatten_content(payload.content),
                                                 payload.source}});
            break;
        }
        case EventType::AssistantChunk: {
            const auto payload = event.payload.get<payload::AssistantChunk>();
            if (payload.text.empty()) {
                break;
            }
            if (startedMessages_.find(session) == startedMessages_.end() ||
                startedMessages_.at(session) != payload.message) {
                events.push_back(UiEvent{AssistantMessageStarted{session, payload.message}});
            }
            const bool reasoning =
                payload.kind == payload::AssistantChunkKind::Reasoning;
            events.push_back(
                UiEvent{AssistantTextDelta{session, payload.message, payload.text, reasoning}});
            break;
        }
        case EventType::AssistantMessage: {
            const auto payload = event.payload.get<payload::AssistantMessage>();
            events.push_back(UiEvent{AssistantMessageFinished{
                session, payload.id, flatten_content(payload.content), payload.usage,
                payload.source}});
            break;
        }
        case EventType::ToolCall: {
            const auto payload = event.payload.get<payload::ToolCall>();
            events.push_back(UiEvent{ToolStarted{session, payload.id, payload.name,
                                                 summarize_tool_arguments(payload.name,
                                                                          payload.arguments)}});
            break;
        }
        case EventType::ToolResult: {
            const auto payload = event.payload.get<payload::ToolResult>();
            events.push_back(UiEvent{ToolFinished{session, payload.id, payload.name,
                                                  payload.outcome, payload.output,
                                                  payload.truncated, payload.error,
                                                  payload.source, payload.context}});
            break;
        }
        case EventType::ContextInjected: {
            const auto payload = event.payload.get<payload::ContextInjected>();
            events.push_back(UiEvent{ContextInjected{session, payload.id, payload.role,
                                                     payload.text, payload.source,
                                                     payload.context}});
            break;
        }
        case EventType::TokenUsage: {
            const auto payload = event.payload.get<payload::TokenUsage>();
            events.push_back(UiEvent{TokenUsageUpdated{session, payload.usage}});
            break;
        }
        case EventType::TurnFailed: {
            const auto payload = event.payload.get<payload::TurnFailed>();
            events.push_back(UiEvent{ErrorOccurred{session, payload.message}});
            break;
        }
        case EventType::SubagentFanIn: {
            const auto payload = event.payload.get<payload::SubagentFanIn>();
            AgentState state = AgentState::Idle;
            if (payload.outcome == payload::SubagentOutcome::Failed) {
                state = AgentState::Error;
            } else if (payload.outcome == payload::SubagentOutcome::Cancelled) {
                state = AgentState::Idle;
            }
            events.push_back(UiEvent{SubagentUpdated{session, payload.subagent, payload.summary, state}});
            break;
        }
        case EventType::ContextCompaction: {
            const auto payload = event.payload.get<payload::ContextCompaction>();
            events.push_back(UiEvent{CompactionMarker{
                session, static_cast<std::uint64_t>(payload.boundary), payload.tokenEstimate,
                payload.model, payload.summary}});
            break;
        }
        case EventType::SessionRenamed: {
            const auto payload = event.payload.get<payload::SessionRenamed>();
            events.push_back(UiEvent{SessionTitleChanged{session, payload.title}});
            break;
        }
        case EventType::PlanMode: {
            const auto payload = event.payload.get<payload::PlanMode>();
            events.push_back(UiEvent{PlanModeChanged{session, payload.active}});
            break;
        }
        case EventType::AssistantAttempt:
        case EventType::GoalChange:
            break;
        default:
            break;
    }

    const auto previous = lastState_.find(session);
    const AgentState old = previous == lastState_.end() ? AgentState::Idle : previous->second;
    const AgentState next = project_state(session, event);
    if (next != old) {
        events.push_back(UiEvent{AgentStateChanged{session, old, next}});
    }
    return events;
}

void UiEventAdapter::applyAndMark(const UiEvent& event) {
    model_.apply(event);
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, AgentStateChanged>) {
                model_.aggregate.recompute(model_.workspaces, model_.sessions);
                model_.aggregate.armOnEdge(e.oldState, e.newState);
                lastState_[e.session] = e.newState;
                model_.dirty.markAggregate();
            } else if constexpr (std::is_same_v<T, AssistantMessageStarted>) {
                startedMessages_[e.session] = e.message;
            }
        },
        event.value);
}

void UiEventAdapter::onEvent(const Event& event) {
    for (const UiEvent& adapted : adapt(event)) {
        applyAndMark(adapted);
    }
    for (const UiEvent& notice : adapt_maintenance(event)) {
        applyAndMark(notice);
    }
}

std::vector<UiEvent> UiEventAdapter::adapt_maintenance(const Event& event) {
    const SessionId session = event.session_id;
    std::vector<UiEvent> events;
    switch (event.type) {
        case EventType::TurnStarted: {
            const auto payload = event.payload.get<payload::TurnStarted>();
            if (payload.origin == payload::TurnOrigin::Maintenance) {
                MaintenanceState state;
                state.active = true;
                maintenance_[session] = state;
            }
            break;
        }
        case EventType::ContextCompaction: {
            const auto state = maintenance_.find(session);
            if (state == maintenance_.end() || !state->second.active) {
                break;
            }
            const auto payload = event.payload.get<payload::ContextCompaction>();
            state->second.sawCompaction = true;
            state->second.boundary      = static_cast<std::uint64_t>(payload.boundary);
            state->second.tokenEstimate = payload.tokenEstimate;
            state->second.model         = payload.model;
            break;
        }
        case EventType::TurnEnded:
        case EventType::TurnCancelled:
        case EventType::TurnFailed: {
            const auto state = maintenance_.find(session);
            if (state == maintenance_.end() || !state->second.active) {
                break;
            }
            CompactionOutcomeNotice notice;
            notice.session       = session;
            notice.boundary      = state->second.boundary;
            notice.tokenEstimate = state->second.tokenEstimate;
            notice.model         = state->second.model;
            if (event.type == EventType::TurnEnded) {
                notice.outcome = state->second.sawCompaction ? CompactionOutcome::Compacted
                                                             : CompactionOutcome::NotNeeded;
            } else if (event.type == EventType::TurnCancelled) {
                notice.outcome = CompactionOutcome::Cancelled;
                notice.reason  = event.payload.get<payload::TurnCancelled>().reason;
            } else {
                notice.outcome = CompactionOutcome::Failed;
                notice.reason  = event.payload.get<payload::TurnFailed>().message;
            }
            maintenance_.erase(state);
            events.push_back(UiEvent{notice});
            break;
        }
        case EventType::AssistantAttempt:
        case EventType::GoalChange:
            break;
        default:
            break;
    }
    return events;
}

void UiEventAdapter::onSessionEnvelope(const protocol::SessionEnvelope& envelope) {
    onSessionEnvelope(model_.activeWorkspaceId, envelope);
}

void UiEventAdapter::onSessionEnvelope(const WorkspaceId& workspace,
                                       const protocol::SessionEnvelope& envelope) {
    if (envelope.session != envelope.event.session_id) {
        return;
    }
    if (!envelope.event.id.value.empty()) {
        if (applied_event_ids_.find(envelope.event.id.value) != applied_event_ids_.end()) {
            return;
        }
        applied_event_ids_.insert(envelope.event.id.value);
        applied_event_order_.push_back(envelope.event.id.value);
        if (applied_event_order_.size() > kMaxAppliedEventIds) {
            applied_event_ids_.erase(applied_event_order_.front());
            applied_event_order_.pop_front();
        }
    }
    if (model_.sessions.find(envelope.session) == model_.sessions.end()) {
        model_.ensureSessionIn(workspace, envelope.session);
    }
    onEvent(envelope.event);
}

void UiEventAdapter::onPermissionRequest(const SessionId& session,
                                         const PermissionRequestId& id,
                                         const PermissionRequest& request) {
    PermissionRequested requested;
    requested.session = session;
    requested.request = id;
    requested.tool = request.tool;
    requested.summary = summarize_tool_arguments(request.tool, request.arguments);
    requested.force_ask = request.force_ask;
    applyAndMark(UiEvent{std::move(requested)});

    const auto previous = lastState_.find(session);
    const AgentState old = previous == lastState_.end() ? AgentState::Idle : previous->second;
    if (old != AgentState::WaitingForPermission) {
        applyAndMark(UiEvent{AgentStateChanged{session, old, AgentState::WaitingForPermission}});
    }
}

void UiEventAdapter::onPermissionRequest(const WorkspaceId& workspace,
                                         const protocol::PermissionRequest& request) {
    if (model_.sessions.find(request.session) == model_.sessions.end()) {
        model_.ensureSessionIn(workspace, request.session);
    }
    const PermissionRequestId id{request.request_id};
    PermissionRequested requested;
    requested.session = request.session;
    requested.request = id;
    requested.tool = request.tool;
    requested.summary = request.summary.empty()
                            ? summarize_tool_arguments(request.tool, request.arguments)
                            : request.summary;
    requested.force_ask = request.force_ask;
    applyAndMark(UiEvent{std::move(requested)});

    const auto previous = lastState_.find(request.session);
    const AgentState old = previous == lastState_.end() ? AgentState::Idle : previous->second;
    if (old != AgentState::WaitingForPermission) {
        applyAndMark(
            UiEvent{AgentStateChanged{request.session, old, AgentState::WaitingForPermission}});
    }
}

void UiEventAdapter::onPermissionResolved(const SessionId& session,
                                          const PermissionRequestId& id,
                                          payload::PermissionDecisionKind decision) {
    applyAndMark(UiEvent{PermissionResolved{session, id, decision}});

    const auto previous = lastState_.find(session);
    const AgentState old = previous == lastState_.end() ? AgentState::Idle : previous->second;
    if (old != AgentState::Thinking) {
        applyAndMark(UiEvent{AgentStateChanged{session, old, AgentState::Thinking}});
    }
}

void UiEventAdapter::onWorkspaceEvent(const WorkspaceEvent& event) {
    model_.apply(event);
}

void UiEventAdapter::onHostNotice(const WorkspaceId& workspace,
                                  const protocol::HostNotice& notice) {
    switch (notice.kind) {
        case protocol::HostNoticeKind::SessionCreated: {
            WorkspaceEvent event;
            event.workspace = workspace;
            event.kind = WorkspaceEventKind::SessionOpened;
            event.session = notice.session;
            model_.apply(event);
            break;
        }
        case protocol::HostNoticeKind::SessionClosed: {
            WorkspaceEvent event;
            event.workspace = workspace;
            event.kind = WorkspaceEventKind::SessionClosed;
            event.session = notice.session;
            model_.apply(event);
            break;
        }
        case protocol::HostNoticeKind::DaemonShuttingDown: {
            WorkspaceEvent event;
            event.workspace = workspace;
            event.kind = WorkspaceEventKind::DaemonStopping;
            model_.apply(event);
            break;
        }
        case protocol::HostNoticeKind::LeaseLost: {
            if (notice.session.has_value()) {
                model_.setSessionReadOnly(*notice.session, true);
            }
            break;
        }
        case protocol::HostNoticeKind::McpServerStatus: {
            model_.setMcpStatus(notice.detail);
            break;
        }
    }
}

void UiEventAdapter::onTick(std::chrono::milliseconds delta) {
    const FlashPhase before = model_.aggregate.flash.phase;
    model_.aggregate.flash.tick(delta);
    if (model_.aggregate.flash.phase != before) {
        model_.dirty.markAggregate();
    }
}

} // namespace ymh::ui

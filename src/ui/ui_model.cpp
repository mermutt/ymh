#include "ymh/ui/ui_model.hpp"

#include <utility>

namespace ymh::ui {
namespace {

constexpr std::size_t kMaxToolOutput = 64u * 1024u;
constexpr std::size_t kMaxToolConversationOutput = 4u * 1024u;

void append_bounded(std::string& target, const std::string& chunk, std::size_t max_bytes) {
    if (target.size() >= max_bytes) {
        return;
    }
    const std::size_t room = max_bytes - target.size();
    target.append(chunk, 0, room);
    if (chunk.size() > room) {
        target += "\n... (truncated)";
    }
}

} // namespace

void DirtySet::mark(const SessionId& session, UiDirtyFlag flag) {
    perSession_[session] |= flag;
}

void DirtySet::markAggregate() {
    aggregate_ |= UiDirtyFlag::Aggregate;
}

bool DirtySet::takeAggregate() {
    const bool set = any_flag(aggregate_);
    aggregate_ = UiDirtyFlag::None;
    return set;
}

std::vector<SessionId> DirtySet::takeDirtySessions() {
    std::vector<SessionId> out;
    for (const auto& [id, flag] : perSession_) {
        if (any_flag(flag)) {
            out.push_back(id);
        }
    }
    perSession_.clear();
    return out;
}

UiDirtyFlag DirtySet::peek(const SessionId& session) const {
    const auto it = perSession_.find(session);
    return it == perSession_.end() ? UiDirtyFlag::None : it->second;
}

bool DirtySet::hasAny() const {
    if (any_flag(aggregate_)) {
        return true;
    }
    for (const auto& [id, flag] : perSession_) {
        if (any_flag(flag)) {
            return true;
        }
    }
    return false;
}

void DirtySet::clear() {
    perSession_.clear();
    aggregate_ = UiDirtyFlag::None;
}

std::size_t ConversationModel::find_message(const std::string& id) const {
    const auto it = by_message.find(id);
    return it == by_message.end() ? kNoEntry : it->second;
}

std::size_t ToolModel::find(const std::string& id) const {
    const auto it = by_id.find(id);
    return it == by_id.end() ? kNoEntry : it->second;
}

void InputModel::push_history(std::string line) {
    if (line.empty()) {
        return;
    }
    if (!history.empty() && history.back() == line) {
        history_pos = history.size();
        return;
    }
    history.push_back(std::move(line));
    history_pos = history.size();
}

void FlashState::arm() {
    phase = FlashPhase::Flashing;
    elapsed = std::chrono::milliseconds{0};
}

void FlashState::tick(std::chrono::milliseconds delta) {
    if (phase != FlashPhase::Flashing) {
        return;
    }
    elapsed += delta;
    if (elapsed >= std::chrono::milliseconds{1000}) {
        phase = FlashPhase::Done;
    }
}

bool is_active_state(AgentState state) noexcept {
    return state == AgentState::Thinking || state == AgentState::CallingTool;
}

bool is_waiting_state(AgentState state) noexcept {
    return state == AgentState::WaitingForInput ||
           state == AgentState::WaitingForPermission ||
           state == AgentState::Error;
}

void AggregateStatusModel::recompute(const std::map<WorkspaceId, WorkspaceModel>& workspaces,
                                     const std::map<SessionId, SessionUiState>& sessions) {
    AggregateStatus next;
    for (const auto& [id, session] : sessions) {
        const auto workspace = workspaces.find(session.workspace);
        if (workspace != workspaces.end() &&
            workspace->second.daemonStatus != DaemonStatus::Attached) {
            continue;
        }
        if (is_active_state(session.agent_state)) {
            ++next.activeCount;
        } else if (is_waiting_state(session.agent_state)) {
            ++next.waitingCount;
        }
    }
    current = next;
}

void AggregateStatusModel::armOnEdge(AgentState oldState, AgentState newState) {
    if (!flash.enabled) {
        return;
    }
    const bool running = oldState == AgentState::Thinking ||
                         oldState == AgentState::CallingTool;
    const bool needsInput = is_waiting_state(newState);
    const bool done = newState == AgentState::Idle;
    if (running && (needsInput || done)) {
        flash.arm();
    }
}

WorkspaceModel* UiModel::activeWorkspace() {
    auto it = workspaces.find(activeWorkspaceId);
    if (it == workspaces.end()) {
        return nullptr;
    }
    return &it->second;
}

SessionUiState* UiModel::activeSession() {
    WorkspaceModel* workspace = activeWorkspace();
    if (workspace == nullptr) {
        return nullptr;
    }
    return session(workspace->activeSessionId);
}

SessionUiState* UiModel::session(const SessionId& id) {
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : &it->second;
}

const SessionUiState* UiModel::session(const SessionId& id) const {
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : &it->second;
}

void UiModel::ensureCell(const SessionId& id) {
    WorkspaceModel& workspace = workspaces[activeWorkspaceId];
    for (const SessionCell& cell : workspace.sessions) {
        if (cell.id == id) {
            return;
        }
    }
    SessionCell cell;
    cell.id = id;
    workspace.sessions.push_back(std::move(cell));
}

SessionUiState& UiModel::ensureSession(const SessionId& id) {
    auto it = sessions.find(id);
    if (it == sessions.end()) {
        SessionUiState state;
        state.id = id;
        state.workspace = activeWorkspaceId;
        it = sessions.emplace(id, std::move(state)).first;
        ensureCell(id);
    }
    return it->second;
}

void UiModel::refreshCell(const SessionId& id) {
    const auto state = sessions.find(id);
    if (state == sessions.end()) {
        return;
    }
    WorkspaceModel& workspace = workspaces[activeWorkspaceId];
    for (SessionCell& cell : workspace.sessions) {
        if (cell.id != id) {
            continue;
        }
        cell.state = state->second.agent_state;
        cell.attention = state->second.attention.needsInput;
        cell.unread = state->second.attention.completed && id != workspace.activeSessionId;
        return;
    }
}

void UiModel::apply(const UiEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            SessionUiState& state = ensureSession(e.session);
            if constexpr (std::is_same_v<T, UserMessage>) {
                ConversationEntry entry;
                entry.role = ConversationRole::User;
                entry.text = e.text;
                state.conversation.entries.push_back(std::move(entry));
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, AssistantMessageStarted>) {
                if (state.conversation.find_message(e.message) == kNoEntry) {
                    ConversationEntry entry;
                    entry.role = ConversationRole::Assistant;
                    entry.streaming = true;
                    state.conversation.by_message[e.message] = state.conversation.entries.size();
                    state.conversation.entries.push_back(std::move(entry));
                }
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, AssistantTextDelta>) {
                std::size_t index = state.conversation.find_message(e.message);
                if (index == kNoEntry) {
                    ConversationEntry entry;
                    entry.role = ConversationRole::Assistant;
                    entry.streaming = true;
                    index = state.conversation.entries.size();
                    state.conversation.by_message[e.message] = index;
                    state.conversation.entries.push_back(std::move(entry));
                }
                state.conversation.entries[index].text += e.text;
                state.conversation.entries[index].streaming = true;
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, AssistantMessageFinished>) {
                std::size_t index = state.conversation.find_message(e.message);
                if (index == kNoEntry) {
                    ConversationEntry entry;
                    entry.role = ConversationRole::Assistant;
                    index = state.conversation.entries.size();
                    state.conversation.by_message[e.message] = index;
                    state.conversation.entries.push_back(std::move(entry));
                }
                if (!e.text.empty()) {
                    state.conversation.entries[index].text = e.text;
                }
                state.conversation.entries[index].streaming = false;
                if (e.usage.has_value()) {
                    state.status.input_tokens = e.usage->input_tokens;
                    state.status.output_tokens = e.usage->output_tokens;
                    state.status.cached_tokens = e.usage->cached_tokens;
                    dirty.mark(e.session, UiDirtyFlag::Status);
                }
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, ToolStarted>) {
                ToolCallView view;
                view.id = e.id;
                view.name = e.name;
                view.arguments = e.arguments;
                state.tools.by_id[e.id] = state.tools.calls.size();
                state.tools.calls.push_back(std::move(view));
                ConversationEntry entry;
                entry.role = ConversationRole::Tool;
                entry.tool_name = e.name;
                entry.tool_call_id = e.id;
                state.conversation.entries.push_back(std::move(entry));
                dirty.mark(e.session, UiDirtyFlag::Tools | UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, ToolOutput>) {
                const std::size_t index = state.tools.find(e.id);
                if (index != kNoEntry) {
                    append_bounded(state.tools.calls[index].output, e.chunk, kMaxToolOutput);
                }
                for (ConversationEntry& entry : state.conversation.entries) {
                    if (entry.role == ConversationRole::Tool && entry.tool_call_id == e.id) {
                        append_bounded(entry.text, e.chunk, kMaxToolConversationOutput);
                        break;
                    }
                }
                dirty.mark(e.session, UiDirtyFlag::Tools | UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, ToolFinished>) {
                const std::size_t index = state.tools.find(e.id);
                if (index != kNoEntry) {
                    ToolCallView& call = state.tools.calls[index];
                    call.finished = true;
                    call.outcome = e.outcome;
                    call.truncated = e.truncated;
                    if (!e.output.empty()) {
                        call.output = e.output;
                        if (call.output.size() > kMaxToolOutput) {
                            call.output.resize(kMaxToolOutput);
                            call.output += "\n... (truncated)";
                        }
                    }
                }
                for (ConversationEntry& entry : state.conversation.entries) {
                    if (entry.role == ConversationRole::Tool && entry.tool_call_id == e.id) {
                        if (!e.output.empty()) {
                            entry.text = e.output;
                            if (entry.text.size() > kMaxToolConversationOutput) {
                                entry.text.resize(kMaxToolConversationOutput);
                                entry.text += "\n... (truncated)";
                            }
                        }
                        break;
                    }
                }
                dirty.mark(e.session, UiDirtyFlag::Tools | UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, FileChanged>) {
                dirty.mark(e.session, UiDirtyFlag::Diff);
            } else if constexpr (std::is_same_v<T, DiffUpdated>) {
                dirty.mark(e.session, UiDirtyFlag::Diff);
            } else if constexpr (std::is_same_v<T, PermissionRequested>) {
                dialog.open = true;
                dialog.session = e.session;
                dialog.request = e.request;
                dialog.tool = e.tool;
                dialog.summary = e.summary;
                dialog.selected = 0;
                state.attention.needsInput = true;
                mode = UiMode::Dialog;
                dirty.mark(e.session, UiDirtyFlag::Attention);
            } else if constexpr (std::is_same_v<T, PermissionResolved>) {
                if (dialog.request == e.request) {
                    dialog.open = false;
                    mode = UiMode::Conversation;
                }
                state.attention.needsInput = false;
                dirty.mark(e.session, UiDirtyFlag::Attention);
            } else if constexpr (std::is_same_v<T, AgentStateChanged>) {
                state.agent_state = e.newState;
                state.status.agent_state = e.newState;
                state.attention.lastState = e.newState;
                state.attention.needsInput = is_waiting_state(e.newState);
                state.attention.completed = e.newState == AgentState::Idle;
                dirty.mark(e.session, UiDirtyFlag::Attention | UiDirtyFlag::Status |
                                           UiDirtyFlag::SessionBar);
            } else if constexpr (std::is_same_v<T, SubagentUpdated>) {
                bool found = false;
                for (SubagentView& agent : state.subagents.agents) {
                    if (agent.id == e.subagent) {
                        agent.summary = e.summary;
                        agent.state = e.state;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    state.subagents.agents.push_back(SubagentView{e.subagent, e.summary, e.state});
                }
                dirty.mark(e.session, UiDirtyFlag::Subagents);
            } else if constexpr (std::is_same_v<T, ErrorOccurred>) {
                state.status.last_error = e.message;
                ConversationEntry entry;
                entry.role = ConversationRole::System;
                entry.text = "error: " + e.message;
                state.conversation.entries.push_back(std::move(entry));
                dirty.mark(e.session, UiDirtyFlag::Conversation | UiDirtyFlag::Status |
                                           UiDirtyFlag::Attention);
            } else if constexpr (std::is_same_v<T, TokenUsageUpdated>) {
                state.status.input_tokens = e.usage.input_tokens;
                state.status.output_tokens = e.usage.output_tokens;
                state.status.cached_tokens = e.usage.cached_tokens;
                dirty.mark(e.session, UiDirtyFlag::Status);
            } else if constexpr (std::is_same_v<T, StatusChanged>) {
                state.status.note = e.text;
                dirty.mark(e.session, UiDirtyFlag::Status);
            }
            refreshCell(e.session);
        },
        event.value);
}

void UiModel::apply(const WorkspaceEvent& event) {
    const auto it = workspaces.find(event.workspace);
    if (it != workspaces.end()) {
        switch (event.kind) {
            case WorkspaceEventKind::DaemonAttached:
                it->second.daemonStatus = DaemonStatus::Attached;
                break;
            case WorkspaceEventKind::DaemonDetached:
                it->second.daemonStatus = DaemonStatus::Detached;
                break;
            case WorkspaceEventKind::DaemonDied:
                it->second.daemonStatus = DaemonStatus::Dead;
                break;
            case WorkspaceEventKind::SessionOpened:
            case WorkspaceEventKind::SessionClosed:
                break;
        }
    }
    dirty.markAggregate();
}

} // namespace ymh::ui

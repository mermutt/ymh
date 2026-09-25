#include "ymh/ui/ui_model.hpp"

#include <algorithm>
#include <cctype>
#include <type_traits>
#include <utility>

#include "ymh/config/config.hpp"
#include "ymh/llm/redaction.hpp"

namespace ymh::ui {

std::string display_model(const ymh::ResolvedModel& model) {
    return model.model_name.empty() ? model.model_id : model.model_name;
}

std::string display_model(const ymh::Config& config, const std::string& model_id) {
    if (model_id.empty()) {
        return {};
    }
    for (const auto& [name, settings] : config.llm.models) {
        if (settings.model == model_id) {
            return name;
        }
    }
    return model_id;
}

void ModelPickerModel::open(const ymh::Config& config, const std::string& current_display) {
    rows.clear();
    selected = 0;
    const ResolvedModel resolved = resolve_model(config);
    if (resolved.model_name.empty()) {
        rows.push_back(ModelPickerRow{"", resolved.model_id, resolved.endpoint.name});
    }
    for (const auto& [name, settings] : config.llm.models) {
        rows.push_back(ModelPickerRow{name, settings.model, settings.endpoint});
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const ModelPickerRow& row = rows[index];
        if ((!row.name.empty() && row.name == current_display) ||
            (!current_display.empty() && row.model_id == current_display)) {
            selected = index;
            break;
        }
    }
    visible = true;
}

void ModelPickerModel::close() {
    visible  = false;
    selected = 0;
    rows.clear();
}

void ModelPickerModel::moveDown() {
    if (!rows.empty()) {
        selected = (selected + 1) % rows.size();
    }
}

void ModelPickerModel::moveUp() {
    if (!rows.empty()) {
        selected = (selected + rows.size() - 1) % rows.size();
    }
}

namespace {

constexpr std::size_t kMaxToolOutput = 64u * 1024u;
constexpr std::size_t kMaxToolConversationOutput = 4u * 1024u;

// 25-D5: the transient plan notices are cleared on the next plan/mode change or
// when the turn ends, whichever is first.
void clear_plan_notices(UiModel& model) {
    const auto is_plan_notice = [](const UiNotice& notice) {
        return notice.text == "plan change queued" || notice.text == "plan exit queued";
    };
    model.notices.erase(
        std::remove_if(model.notices.begin(), model.notices.end(), is_plan_notice),
        model.notices.end());
}

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

// 48-D4.1: the pinned character-class word model. Continuation bytes inherit
// the class of the glyph's lead byte so a multi-byte glyph is never split.
enum class WordClass : std::uint8_t { Word, Space, Punct };

WordClass byte_class(std::string_view text, std::size_t index) {
    std::size_t lead = index;
    while (lead > 0 && (static_cast<unsigned char>(text[lead]) & 0xC0) == 0x80) {
        --lead;
    }
    const unsigned char byte = static_cast<unsigned char>(text[lead]);
    if (byte == ' ' || byte == '\t') {
        return WordClass::Space;
    }
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '_') {
        return WordClass::Word;
    }
    return WordClass::Punct;
}

// 17 §4 (RB-02, U-RB02-1): route a reasoning delta into one folded Reasoning
// entry per message, ordered immediately before that message's Assistant entry
// whether the Assistant entry already exists (empty or non-empty) or not.
void apply_reasoning_delta(ConversationModel& conversation, const AssistantTextDelta& delta) {
    const std::size_t existing = conversation.find_reasoning_message(delta.message);
    if (existing != kNoEntry) {
        conversation.entries[existing].text += delta.text;
        conversation.entries[existing].streaming = true;
        return;
    }
    ConversationEntry entry;
    entry.role = ConversationRole::Reasoning;
    entry.text = delta.text;
    entry.streaming = true;
    const std::size_t assistant = conversation.find_message(delta.message);
    if (assistant == kNoEntry) {
        conversation.by_reasoning_message[delta.message] = conversation.entries.size();
        conversation.entries.push_back(std::move(entry));
        return;
    }
    conversation.entries.insert(conversation.entries.begin() +
                                    static_cast<std::ptrdiff_t>(assistant),
                                std::move(entry));
    for (auto& pair : conversation.by_message) {
        if (pair.second >= assistant) {
            ++pair.second;
        }
    }
    for (auto& pair : conversation.by_reasoning_message) {
        if (pair.second >= assistant) {
            ++pair.second;
        }
    }
    conversation.by_reasoning_message[delta.message] = assistant;
}

std::string lower_ascii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

// 57-D5: effective root first, then `lastUsedAt` desc, then title asc
// (case-insensitive), then canonical path asc. `cwd_path` is the effective root
// (`UiModel::cwdWorkspacePath`); an empty path never matches.
[[nodiscard]] bool switcher_workspace_less(const WorkspaceNode& left,
                                           const WorkspaceNode& right,
                                           const std::string& cwd_path,
                                           const std::string& left_path,
                                           const std::string& right_path) {
    const bool left_root = !cwd_path.empty() && !left_path.empty() && left_path == cwd_path;
    const bool right_root = !cwd_path.empty() && !right_path.empty() && right_path == cwd_path;
    if (left_root != right_root) {
        return left_root;
    }
    if (left.lastUsedAt != right.lastUsedAt) {
        return left.lastUsedAt > right.lastUsedAt;
    }
    const std::string left_title = lower_ascii(left.title);
    const std::string right_title = lower_ascii(right.title);
    if (left_title != right_title) {
        return left_title < right_title;
    }
    return left_path < right_path;
}

// 22 §3.3 (L3): validate the switcher cursor against the node list it just
// built, not against `UiModel::workspaces`. Shared by `open` (Live) and
// `openHistory` (History) so a filtered-out workspace cannot leave a dangling
// cursor and a stale session cursor is cleared deterministically.
void revalidate_switcher_cursor(SwitcherOverlayModel& switcher, const UiModel& model) {
    const auto node_for = [&switcher](const WorkspaceId& id) -> const WorkspaceNode* {
        for (const WorkspaceNode& node : switcher.workspaces) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    };

    const WorkspaceNode* cursor_node = node_for(switcher.cursor.workspace);
    if (cursor_node == nullptr) {
        switcher.cursor.workspace = WorkspaceId{};
        switcher.cursor.session.reset();
        const WorkspaceNode* target = node_for(model.activeWorkspaceId);
        if (target == nullptr && !switcher.workspaces.empty()) {
            target = &switcher.workspaces.front();
        }
        if (target != nullptr) {
            switcher.cursor.workspace = target->id;
            const auto active = model.workspaces.find(target->id);
            if (active != model.workspaces.end() &&
                !active->second.activeSessionId().value.empty()) {
                for (const SessionNode& session : target->sessions) {
                    if (session.id == active->second.activeSessionId()) {
                        switcher.cursor.session = session.id;
                        break;
                    }
                }
            }
        }
    } else if (switcher.cursor.session.has_value()) {
        bool present = false;
        for (const SessionNode& session : cursor_node->sessions) {
            if (session.id == *switcher.cursor.session) {
                present = true;
                break;
            }
        }
        if (!present) {
            switcher.cursor.session.reset();
        }
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

std::size_t ConversationModel::find_reasoning_message(const std::string& id) const {
    const auto it = by_reasoning_message.find(id);
    return it == by_reasoning_message.end() ? kNoEntry : it->second;
}

std::size_t ToolModel::find(const std::string& id) const {
    const auto it = by_id.find(id);
    return it == by_id.end() ? kNoEntry : it->second;
}

void InputModel::push_history(std::string line) {
    if (line.empty()) {
        return;
    }
    saved_draft.clear();
    if (!history.empty() && history.back() == line) {
        history_pos = history.size();
        return;
    }
    history.push_back(std::move(line));
    history_pos = history.size();
}

bool InputModel::history_up() {
    if (history.empty() || history_pos == 0) {
        return false;
    }
    if (history_pos == history.size()) {
        saved_draft = draft;
    }
    --history_pos;
    draft = history[history_pos];
    cursor = draft.size();
    return true;
}

bool InputModel::history_down() {
    if (history.empty() || history_pos >= history.size()) {
        return false;
    }
    ++history_pos;
    draft = history_pos == history.size() ? saved_draft : history[history_pos];
    cursor = draft.size();
    return true;
}

bool InputModel::delete_forward() {
    if (cursor >= draft.size()) {
        return false;
    }
    draft.erase(cursor, 1);
    return true;
}

void InputModel::clear_line() {
    draft.clear();
    cursor = 0;
}

bool InputModel::delete_word() {
    if (cursor == 0) {
        return false;
    }
    std::size_t boundary = cursor;
    while (boundary > 0 && byte_class(draft, boundary - 1) == WordClass::Space) {
        --boundary;
    }
    if (boundary > 0) {
        const WordClass run = byte_class(draft, boundary - 1);
        while (boundary > 0 && byte_class(draft, boundary - 1) == run) {
            --boundary;
        }
    }
    if (boundary == cursor) {
        return false;
    }
    draft.erase(boundary, cursor - boundary);
    cursor = boundary;
    return true;
}

std::size_t InputModel::word_left_boundary(std::size_t position) const {
    if (position == 0 || draft.empty()) {
        return 0;
    }
    std::size_t boundary = position;
    while (boundary > 0 && byte_class(draft, boundary - 1) == WordClass::Space) {
        --boundary;
    }
    if (boundary == 0) {
        return 0;
    }
    const WordClass run = byte_class(draft, boundary - 1);
    while (boundary > 0 && byte_class(draft, boundary - 1) == run) {
        --boundary;
    }
    return boundary;
}

std::size_t InputModel::word_right_boundary(std::size_t position) const {
    const std::size_t size = draft.size();
    if (position >= size) {
        return size;
    }
    std::size_t boundary = position;
    while (boundary < size && byte_class(draft, boundary) == WordClass::Space) {
        ++boundary;
    }
    if (boundary >= size) {
        return size;
    }
    const WordClass run = byte_class(draft, boundary);
    while (boundary < size && byte_class(draft, boundary) == run) {
        ++boundary;
    }
    return boundary;
}

std::size_t InputModel::cursor_left(std::size_t position) const {
    if (position == 0) {
        return 0;
    }
    std::size_t index = position;
    if (index > draft.size()) {
        index = draft.size();
    }
    --index;
    while (index > 0 && (static_cast<unsigned char>(draft[index]) & 0xC0) == 0x80) {
        --index;
    }
    return index;
}

std::size_t InputModel::cursor_right(std::size_t position) const {
    const std::size_t size = draft.size();
    if (position >= size) {
        return size;
    }
    std::size_t index = position + 1;
    while (index < size && (static_cast<unsigned char>(draft[index]) & 0xC0) == 0x80) {
        ++index;
    }
    return index;
}

std::size_t glyph_floor(std::string_view draft, std::size_t cursor) noexcept {
    if (cursor >= draft.size()) {
        return draft.size();
    }
    std::size_t index = cursor;
    while (index < draft.size() &&
           (static_cast<unsigned char>(draft[index]) & 0xC0) == 0x80) {
        ++index;
    }
    return index;
}

std::size_t glyph_len(std::string_view text, std::size_t cursor) noexcept {
    if (cursor >= text.size()) {
        return 0;
    }
    const unsigned char lead = static_cast<unsigned char>(text[cursor]);
    std::size_t length = 1;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
    }
    if (cursor + length > text.size()) {
        length = 1;
    }
    return length;
}

std::string_view glyph_at(std::string_view text, std::size_t cursor) noexcept {
    if (cursor >= text.size()) {
        return {};
    }
    return text.substr(cursor, glyph_len(text, cursor));
}

void ConversationScroll::pageUp() {
    if (following) {
        following = false;
        fraction = 1.0f;
    }
    fraction = std::max(0.0f, fraction - kPageStep);
}

void ConversationScroll::pageDown() {
    if (following) {
        return;
    }
    fraction = std::min(1.0f, fraction + kPageStep);
    if (fraction >= 1.0f) {
        following = true;
        unseen = false;
    }
}

void ConversationScroll::lineUp() {
    if (following) {
        following = false;
        fraction = 1.0f;
    }
    fraction = std::max(0.0f, fraction - kLineStep);
}

void ConversationScroll::lineDown() {
    if (following) {
        return;
    }
    fraction = std::min(1.0f, fraction + kLineStep);
    if (fraction >= 1.0f) {
        following = true;
        unseen = false;
    }
}

void ConversationScroll::toTop() {
    following = false;
    fraction = 0.0f;
}

void ConversationScroll::toBottom() {
    following = true;
    fraction = 1.0f;
    unseen = false;
}

void ConversationScroll::onNewContent() {
    if (!following) {
        unseen = true;
    }
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

Presentation entry_presentation(const std::vector<ConversationEntry>& entries,
                                std::size_t index, bool turn_active) noexcept {
    if (index >= entries.size()) {
        return Presentation::FinalAnswer;
    }
    switch (entries[index].role) {
        case ConversationRole::User:
            return Presentation::UserAuthored;
        case ConversationRole::Reasoning:
        case ConversationRole::Tool:
            return Presentation::Intermediate;
        case ConversationRole::System:
        case ConversationRole::Context:
            return Presentation::Chrome;
        case ConversationRole::Assistant:
            break;
    }
    if (turn_active && index + 1 == entries.size()) {
        return Presentation::Intermediate;
    }
    for (std::size_t next = index + 1; next < entries.size(); ++next) {
        const ConversationRole role = entries[next].role;
        if (role == ConversationRole::User) {
            break;
        }
        if (role == ConversationRole::Tool || role == ConversationRole::Reasoning) {
            return Presentation::Intermediate;
        }
    }
    return Presentation::FinalAnswer;
}

OwnershipMark ownership_mark(DaemonStatus status) noexcept {
    switch (status) {
        case DaemonStatus::Attached:
            return OwnershipMark::Owned;
        case DaemonStatus::Stopping:
            return OwnershipMark::Stopping;
        case DaemonStatus::NotRunning:
            return OwnershipMark::NotRunning;
        case DaemonStatus::Connecting:
        case DaemonStatus::Detached:
        case DaemonStatus::Dead:
            return OwnershipMark::Unreachable;
    }
    return OwnershipMark::Unreachable;
}

bool live_switcher_renderable(const WorkspaceModel& workspace) noexcept {
    return workspace.live && (workspace.daemonStatus == DaemonStatus::Attached ||
                              workspace.daemonStatus == DaemonStatus::Stopping);
}

void apply_daemon_status_liveness(WorkspaceModel& workspace, DaemonStatus status) noexcept {
    switch (status) {
        case DaemonStatus::Attached:
            workspace.live = true;
            break;
        case DaemonStatus::Detached:
        case DaemonStatus::Dead:
        case DaemonStatus::NotRunning:
            workspace.live = false;
            break;
        case DaemonStatus::Connecting:
        case DaemonStatus::Stopping:
            break;
    }
}

std::vector<WorkspaceId> switcher_eviction_candidates(
    const std::map<WorkspaceId, WorkspaceModel>& workspaces,
    const std::set<WorkspaceId>& live_ids, const std::set<WorkspaceId>& connecting_ids) {
    std::vector<WorkspaceId> doomed;
    for (const auto& [id, workspace] : workspaces) {
        (void)workspace;
        if (live_ids.count(id) != 0) {
            continue;
        }
        if (connecting_ids.count(id) != 0) {
            continue;
        }
        doomed.push_back(id);
    }
    return doomed;
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
    return session(workspace->activeSessionId());
}

SessionUiState* UiModel::ensureActiveSession() {
    if (SessionUiState* modeled = activeSession(); modeled != nullptr) {
        return modeled;
    }
    WorkspaceModel* workspace = activeWorkspace();
    if (workspace == nullptr || workspace->activeSessionId().value.empty()) {
        return nullptr;
    }
    return &ensureSessionIn(workspace->id, workspace->activeSessionId());
}

bool UiModel::catalog_has_session(const WorkspaceId& workspace,
                                  const SessionId& session_id) const {
    for (const WorkspaceHistory& history : catalog.workspaces) {
        if (history.id != workspace) {
            continue;
        }
        for (const SessionHistoryEntry& entry : history.sessions) {
            if (entry.id == session_id) {
                return true;
            }
        }
        return false;
    }
    return false;
}

SessionUiState* UiModel::session(const SessionId& id) {
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : &it->second;
}

const SessionUiState* UiModel::session(const SessionId& id) const {
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : &it->second;
}

void UiModel::ensureCell(const SessionId& id) { ensureCellIn(activeWorkspaceId, id); }

void UiModel::ensureCellIn(const WorkspaceId& workspace, const SessionId& id) {
    WorkspaceModel& target = workspaces[workspace];
    if (target.id.value.empty()) {
        target.id = workspace;
    }
    for (const SessionCell& cell : target.sessions) {
        if (cell.id == id) {
            return;
        }
    }
    SessionCell cell;
    cell.id = id;
    target.sessions.push_back(std::move(cell));
}

SessionUiState& UiModel::ensureSession(const SessionId& id) {
    return ensureSessionIn(activeWorkspaceId, id);
}

SessionUiState& UiModel::ensureSessionIn(const WorkspaceId& workspace, const SessionId& id) {
    auto it = sessions.find(id);
    if (it == sessions.end()) {
        SessionUiState state;
        state.id = id;
        state.workspace = workspace;
        it = sessions.emplace(id, std::move(state)).first;
        ensureCellIn(workspace, id);
        return it->second;
    }
    if (it->second.workspace.value.empty()) {
        it->second.workspace = workspace;
        ensureCellIn(workspace, id);
    }
    return it->second;
}

void UiModel::refreshCell(const SessionId& id) {
    const auto state = sessions.find(id);
    if (state == sessions.end()) {
        return;
    }
    refreshCellIn(state->second.workspace, id);
}

void UiModel::refreshCellIn(const WorkspaceId& workspace, const SessionId& id) {
    const auto state = sessions.find(id);
    if (state == sessions.end()) {
        return;
    }
    const auto workspace_it = workspaces.find(workspace);
    if (workspace_it == workspaces.end()) {
        return;
    }
    WorkspaceModel& target = workspace_it->second;
    for (SessionCell& cell : target.sessions) {
        if (cell.id != id) {
            continue;
        }
        cell.state = state->second.agent_state;
        cell.attention = state->second.attention.needsInput;
        cell.unread = state->second.attention.completed && id != target.activeSessionId();
        return;
    }
}

void UiModel::setCellTitle(const WorkspaceId& workspace, const SessionId& id,
                           std::string title) {
    const auto workspace_it = workspaces.find(workspace);
    if (workspace_it == workspaces.end()) {
        return;
    }
    for (SessionCell& cell : workspace_it->second.sessions) {
        if (cell.id == id) {
            cell.title = std::move(title);
            dirty.mark(id, UiDirtyFlag::Layout | UiDirtyFlag::SessionBar);
            return;
        }
    }
}

void UiModel::setSessionReadOnly(const SessionId& id, bool read_only) {
    const auto state = sessions.find(id);
    if (state == sessions.end()) {
        return;
    }
    const auto workspace_it = workspaces.find(state->second.workspace);
    if (workspace_it == workspaces.end()) {
        return;
    }
    for (SessionCell& cell : workspace_it->second.sessions) {
        if (cell.id == id) {
            cell.readOnly = read_only;
            dirty.mark(id, UiDirtyFlag::SessionBar | UiDirtyFlag::Attention);
            return;
        }
    }
}

void UiModel::eraseSession(const WorkspaceId& workspace, const SessionId& id) {
    sessions.erase(id);
    const auto workspace_it = workspaces.find(workspace);
    if (workspace_it == workspaces.end()) {
        return;
    }
    WorkspaceModel& target = workspace_it->second;
    target.sessions.erase(
        std::remove_if(target.sessions.begin(), target.sessions.end(),
                       [&id](const SessionCell& cell) { return cell.id == id; }),
        target.sessions.end());
    if (target.activeSessionId() == id) {
        target.setActiveSessionId(SessionId{});
    }
    dirty.mark(id, UiDirtyFlag::Conversation | UiDirtyFlag::Layout | UiDirtyFlag::SessionBar);
}

void UiModel::setMcpStatus(std::string detail) {
    mcp_status = std::move(detail);
    dirty.markAggregate();
}

void UiModel::pushNotice(std::string text) {
    notices.push_back(UiNotice{redact_secrets(text), 0});
    while (notices.size() > kMaxNotices) {
        notices.pop_front();
    }
    dirty.markAggregate();
}

void UiModel::eraseWorkspace(const WorkspaceId& workspace) {
    const auto it = workspaces.find(workspace);
    if (it == workspaces.end()) {
        return;
    }
    for (auto session = sessions.begin(); session != sessions.end();) {
        if (session->second.workspace == workspace) {
            session = sessions.erase(session);
        } else {
            ++session;
        }
    }
    workspaces.erase(it);
    if (activeWorkspaceId == workspace) {
        WorkspaceId promoted;
        for (const auto& [id, candidate] : workspaces) {
            if (live_switcher_renderable(candidate)) {
                promoted = id;
                break;
            }
        }
        if (promoted.value.empty() && !workspaces.empty()) {
            promoted = workspaces.begin()->first;
        }
        activeWorkspaceId = promoted;
    }
    if (mode == UiMode::Switcher && switcher.cursor.workspace == workspace) {
        if (switcher.source == SwitcherSource::Live) {
            switcher.open(*this);
        } else {
            switcher.cursor = SwitcherCursor{};
        }
    }
    dirty.markAggregate();
}

void UiModel::apply(const UiEvent& event) {
    const bool conversation_event = std::visit(
        [](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            return std::is_same_v<T, UserMessage> ||
                   std::is_same_v<T, AssistantMessageStarted> ||
                   std::is_same_v<T, AssistantTextDelta> ||
                   std::is_same_v<T, AssistantMessageFinished> ||
                   std::is_same_v<T, ToolStarted> ||
                   std::is_same_v<T, ToolOutput> ||
                   std::is_same_v<T, ToolFinished> ||
                   std::is_same_v<T, ErrorOccurred> ||
                   std::is_same_v<T, CompactionMarker> ||
                   std::is_same_v<T, CompactionOutcomeNotice> ||
                   std::is_same_v<T, ContextInjected>;
        },
        event.value);
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            SessionUiState& state = ensureSession(e.session);
            if constexpr (std::is_same_v<T, UserMessage>) {
                ConversationEntry entry;
                entry.role   = ConversationRole::User;
                entry.text   = e.text;
                entry.source = e.source;
                state.conversation.entries.push_back(std::move(entry));
                if (e.source.kind == MessageSource::Kind::User) {
                    state.input.push_history(e.text);
                }
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, AssistantMessageStarted>) {
                if (state.conversation.find_message(e.message) == kNoEntry) {
                    ConversationEntry entry;
                    entry.role = ConversationRole::Assistant;
                    entry.streaming = true;
                    state.conversation.by_message[e.message] = state.conversation.entries.size();
                    state.conversation.entries.push_back(std::move(entry));
                }
                state.stream_started_at = now_reader();
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, AssistantTextDelta>) {
                if (e.reasoning) {
                    apply_reasoning_delta(state.conversation, e);
                } else {
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
                }
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
                state.conversation.entries[index].source    = e.source;
                state.conversation.entries[index].streaming = false;
                const std::size_t reasoning =
                    state.conversation.find_reasoning_message(e.message);
                if (reasoning != kNoEntry) {
                    state.conversation.entries[reasoning].streaming = false;
                }
                if (e.usage.has_value()) {
                    state.status.input_tokens = e.usage->input_tokens;
                    state.status.output_tokens = e.usage->output_tokens;
                    state.status.cached_tokens = e.usage->cached_tokens;
                    dirty.mark(e.session, UiDirtyFlag::Status);
                }
                if (state.stream_started_at.has_value()) {
                    if (e.usage.has_value() && e.usage->output_tokens > 0) {
                        const std::chrono::steady_clock::duration elapsed =
                            now_reader() - *state.stream_started_at;
                        if (elapsed >= std::chrono::milliseconds(250)) {
                            const double seconds =
                                std::chrono::duration<double>(elapsed).count();
                            if (seconds > 0.0) {
                                state.status.tps =
                                    static_cast<double>(e.usage->output_tokens) / seconds;
                                dirty.mark(e.session, UiDirtyFlag::Status);
                            }
                        }
                    }
                    state.stream_started_at.reset();
                }
                state.status.api_state = ApiConnectivity::Ok;
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
                    call.notice = e.context.form == ContextForm::Notice
                                      ? std::optional<ContextFormed>{e.context}
                                      : std::nullopt;
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
                        entry.source  = e.source;
                        entry.context = e.context.form == ContextForm::None
                                            ? std::nullopt
                                            : std::optional<ContextFormed>{e.context};
                        break;
                    }
                }
                dirty.mark(e.session, UiDirtyFlag::Tools | UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, ContextInjected>) {
                ConversationEntry entry;
                entry.role    = ConversationRole::Context;
                entry.text    = e.text;
                entry.source  = e.source;
                entry.context = e.context;
                state.conversation.entries.push_back(std::move(entry));
                dirty.mark(e.session, UiDirtyFlag::Conversation);
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
                dialog.force_ask = e.force_ask;
                dialog.selected = e.force_ask ? 1 : 0;
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
                if (e.newState == AgentState::Idle) {
                    clear_plan_notices(*this);
                }
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
                state.status.api_state = ApiConnectivity::Error;
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
                state.status.api_state = ApiConnectivity::Ok;
                dirty.mark(e.session, UiDirtyFlag::Status);
            } else if constexpr (std::is_same_v<T, StatusChanged>) {
                state.status.note = e.text;
                dirty.mark(e.session, UiDirtyFlag::Status);
            } else if constexpr (std::is_same_v<T, CompactionMarker>) {
                ConversationEntry entry;
                entry.role = ConversationRole::System;
                entry.text = "\u22ef compacted history up to #" + std::to_string(e.boundary) +
                             " \u00b7 summary ~" + std::to_string(e.tokenEstimate) +
                             " tokens \u00b7 model " + e.model;
                state.conversation.entries.push_back(std::move(entry));
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, CompactionOutcomeNotice>) {
                ConversationEntry entry;
                entry.role = ConversationRole::System;
                switch (e.outcome) {
                    case CompactionOutcome::Compacted:
                        entry.text = "compaction complete: ~" +
                                     std::to_string(e.tokenEstimate) + " tokens, model " + e.model;
                        break;
                    case CompactionOutcome::NotNeeded:
                        entry.text = "compaction skipped: nothing to compact";
                        break;
                    case CompactionOutcome::Cancelled:
                        entry.text = "compaction cancelled";
                        break;
                    case CompactionOutcome::Failed:
                        entry.text = "compaction failed" +
                                     (e.reason.empty() ? std::string{} : ": " + e.reason);
                        break;
                    case CompactionOutcome::Queued:
                        entry.text = "compaction queued";
                        break;
                }
                state.conversation.entries.push_back(std::move(entry));
                dirty.mark(e.session, UiDirtyFlag::Conversation);
            } else if constexpr (std::is_same_v<T, SessionTitleChanged>) {
                setCellTitle(state.workspace, e.session, e.title);
            } else if constexpr (std::is_same_v<T, PlanModeChanged>) {
                state.status.plan_active = e.active;
                clear_plan_notices(*this);
                dirty.mark(e.session, UiDirtyFlag::Status);
            } else if constexpr (std::is_same_v<T, ModelChanged>) {
                ResolvedModel resolved;
                resolved.model_name = e.model_name;
                resolved.model_id   = e.model;
                state.status.model  = display_model(resolved);
                dirty.mark(e.session, UiDirtyFlag::Status);
            }
            refreshCell(e.session);
        },
        event.value);
    if (conversation_event) {
        const SessionId session_id =
            std::visit([](const auto& e) { return e.session; }, event.value);
        if (SessionUiState* state = session(session_id); state != nullptr) {
            state->scroll.onNewContent();
        }
    }
}

void UiModel::apply(const WorkspaceEvent& event) {
    const auto it = workspaces.find(event.workspace);
    if (it != workspaces.end()) {
        switch (event.kind) {
            case WorkspaceEventKind::DaemonAttached:
                it->second.daemonStatus = DaemonStatus::Attached;
                apply_daemon_status_liveness(it->second, it->second.daemonStatus);
                break;
            case WorkspaceEventKind::DaemonDetached:
                it->second.daemonStatus = DaemonStatus::Detached;
                apply_daemon_status_liveness(it->second, it->second.daemonStatus);
                break;
            case WorkspaceEventKind::DaemonDied:
                it->second.daemonStatus = DaemonStatus::Dead;
                apply_daemon_status_liveness(it->second, it->second.daemonStatus);
                break;
            case WorkspaceEventKind::DaemonStopping:
                it->second.daemonStatus = DaemonStatus::Stopping;
                apply_daemon_status_liveness(it->second, it->second.daemonStatus);
                break;
            case WorkspaceEventKind::SessionOpened:
                if (event.session.has_value()) {
                    ensureSessionIn(event.workspace, *event.session);
                }
                break;
            case WorkspaceEventKind::SessionClosed:
                if (event.session.has_value()) {
                    eraseSession(event.workspace, *event.session);
                }
                break;
        }
    }
    dirty.markAggregate();
}

void UiModel::openSwitcher() {
    switcher.source = SwitcherSource::Live;
    switcher.open(*this);
    mode = UiMode::Switcher;
    dirty.markAggregate();
}

void UiModel::focusWorkspace(const WorkspaceId& workspace) {
    if (workspaces.find(workspace) == workspaces.end()) {
        return;
    }
    activeWorkspaceId = workspace;
    dirty.markAggregate();
}

void UiModel::focusSession(const SessionId& id) {
    if (id.value.empty()) {
        return;
    }
    const auto state = sessions.find(id);
    const WorkspaceId workspace =
        state != sessions.end() && !state->second.workspace.value.empty()
            ? state->second.workspace
            : activeWorkspaceId;
    focusSessionIn(workspace, id);
}

void UiModel::focusSessionIn(const WorkspaceId& workspace, const SessionId& id) {
    if (id.value.empty()) {
        return;
    }
    const auto workspace_it = workspaces.find(workspace);
    if (workspace_it == workspaces.end()) {
        return;
    }
    // 48-D2.4: a session switch clears the outgoing session's Esc arm.
    const auto previous_workspace = workspaces.find(activeWorkspaceId);
    if (previous_workspace != workspaces.end()) {
        const auto previous =
            sessions.find(previous_workspace->second.activeSessionId());
        if (previous != sessions.end()) {
            previous->second.esc_arm = EscArm::Disarmed;
            previous->second.esc_armed_at.reset();
        }
    }
    ensureSessionIn(workspace, id);
    activeWorkspaceId = workspace;
    workspace_it->second.setActiveSessionId(id);
    mode = UiMode::Conversation;
    dirty.mark(id, UiDirtyFlag::Conversation | UiDirtyFlag::Layout | UiDirtyFlag::Input);
    dirty.markAggregate();
}

void SwitcherOverlayModel::open(const UiModel& model) {
    workspaces.clear();
    disarm_delete();
    source = SwitcherSource::Live;
    const bool filtering = filter.has_value() && !filter->empty();
    const bool catalog_pending = !model.catalog.loaded || model.catalog.generation == 0;
    SessionId focused;
    if (!model.activeWorkspaceId.value.empty()) {
        const auto active = model.workspaces.find(model.activeWorkspaceId);
        if (active != model.workspaces.end()) {
            focused = active->second.activeSessionId();
        }
    }
    for (const auto& [workspace_id, workspace] : model.workspaces) {
        if (!live_switcher_renderable(workspace)) {
            continue;
        }
        WorkspaceNode node;
        node.id = workspace_id;
        node.title = workspace.title.empty() ? workspace.cwd : workspace.title;
        node.status = workspace.daemonStatus;
        node.mark = ownership_mark(workspace.daemonStatus);
        node.live = workspace.live;
        node.catalog_pending = catalog_pending;
        for (const WorkspaceHistory& history : model.catalog.workspaces) {
            if (history.id == workspace_id) {
                node.note = history.note;
                node.lastUsedAt = history.lastUsedAt;
                break;
            }
        }
        std::size_t hidden_by_focus = 0;
        for (const SessionCell& cell : workspace.sessions) {
            const bool matches =
                !filtering || cell.title.find(*filter) != std::string::npos ||
                cell.id.value.find(*filter) != std::string::npos;
            if (cell.id == focused && !focused.value.empty()) {
                if (matches) {
                    ++hidden_by_focus;
                }
                continue;
            }
            if (!matches) {
                continue;
            }
            if (!model.catalog_has_session(workspace_id, cell.id)) {
                continue;
            }
            SessionNode session;
            session.id = cell.id;
            session.title = cell.title;
            session.state = cell.state;
            session.attention = cell.attention;
            node.sessions.push_back(std::move(session));
        }
        node.sessions_hidden_by_focus = hidden_by_focus > 0 && node.sessions.empty();
        if (filtering && node.sessions.empty() && node.title.find(*filter) == std::string::npos) {
            continue;
        }
        workspaces.push_back(std::move(node));
    }

    std::sort(workspaces.begin(), workspaces.end(),
              [&model](const WorkspaceNode& left, const WorkspaceNode& right) {
                  const auto left_workspace = model.workspaces.find(left.id);
                  const auto right_workspace = model.workspaces.find(right.id);
                  const std::string left_path = left_workspace == model.workspaces.end()
                                                    ? left.title
                                                    : left_workspace->second.cwd;
                  const std::string right_path = right_workspace == model.workspaces.end()
                                                     ? right.title
                                                     : right_workspace->second.cwd;
                  return switcher_workspace_less(left, right, model.cwdWorkspacePath, left_path,
                                                 right_path);
              });

    revalidate_switcher_cursor(*this, model);
}

void SwitcherOverlayModel::openHistory(const UiModel& model) {
    workspaces.clear();
    disarm_delete();
    source = SwitcherSource::History;
    const bool filtering = filter.has_value() && !filter->empty();
    SessionId focused;
    if (!model.activeWorkspaceId.value.empty()) {
        const auto active = model.workspaces.find(model.activeWorkspaceId);
        if (active != model.workspaces.end()) {
            focused = active->second.activeSessionId();
        }
    }

    const auto path_of = [&model](const WorkspaceId& id) -> const std::string& {
        static const std::string empty;
        for (const WorkspaceHistory& history : model.catalog.workspaces) {
            if (history.id == id) {
                return history.canonicalPath;
            }
        }
        return empty;
    };

    for (const WorkspaceHistory& history : model.catalog.workspaces) {
        WorkspaceNode node;
        node.id = history.id;
        node.title = history.title.empty() ? history.canonicalPath : history.title;
        node.live = history.live;
        node.historyOnly = !history.live;
        node.note = history.note;
        node.lastUsedAt = history.lastUsedAt;
        if (history.live) {
            node.status = DaemonStatus::Attached;
            node.mark = OwnershipMark::Owned;
        } else {
            node.status = DaemonStatus::NotRunning;
            node.mark = OwnershipMark::NotRunning;
        }
        std::size_t hidden_by_focus = 0;
        for (const SessionHistoryEntry& entry : history.sessions) {
            const bool matches =
                !filtering || entry.title.find(*filter) != std::string::npos ||
                entry.id.value.find(*filter) != std::string::npos;
            if (entry.id == focused && !focused.value.empty()) {
                if (matches) {
                    ++hidden_by_focus;
                }
                continue;
            }
            if (!matches) {
                continue;
            }
            SessionNode session;
            session.id = entry.id;
            session.title = entry.title;
            session.fromDisk = true;
            session.kind = entry.kind;
            session.model = entry.model;
            session.updatedAt = entry.updatedAt;
            session.parent = entry.parent;
            node.sessions.push_back(std::move(session));
        }
        node.sessions_hidden_by_focus = hidden_by_focus > 0 && node.sessions.empty();
        if (filtering && node.sessions.empty() && node.title.find(*filter) == std::string::npos) {
            continue;
        }
        // 22 §3.7 (SW17): History sessions are `updated_at` desc, `id` asc.
        std::sort(node.sessions.begin(), node.sessions.end(),
                  [](const SessionNode& left, const SessionNode& right) {
                      if (left.updatedAt != right.updatedAt) {
                          return left.updatedAt > right.updatedAt;
                      }
                      return left.id.value < right.id.value;
                  });
        workspaces.push_back(std::move(node));
    }

    std::sort(workspaces.begin(), workspaces.end(),
              [&path_of, &model](const WorkspaceNode& left, const WorkspaceNode& right) {
                  return switcher_workspace_less(left, right, model.cwdWorkspacePath,
                                                 path_of(left.id), path_of(right.id));
              });

    revalidate_switcher_cursor(*this, model);
}

void SwitcherOverlayModel::close() {
    workspaces.clear();
    cursor = SwitcherCursor{};
    filter.reset();
    disarm_delete();
}

void SwitcherOverlayModel::disarm_delete() {
    delete_arm = EscArm::Disarmed;
    delete_armed_at.reset();
    delete_target.reset();
    delete_target_session_count = 0;
}

void SwitcherOverlayModel::clamp_cursor() {
    if (workspaces.empty()) {
        cursor = SwitcherCursor{};
        return;
    }
    const WorkspaceNode* node = nullptr;
    for (const WorkspaceNode& candidate : workspaces) {
        if (candidate.id == cursor.workspace) {
            node = &candidate;
            break;
        }
    }
    if (node == nullptr) {
        cursor.workspace = workspaces.front().id;
        cursor.session.reset();
        return;
    }
    if (!cursor.session.has_value()) {
        return;
    }
    for (const SessionNode& session : node->sessions) {
        if (session.id == *cursor.session) {
            return;
        }
    }
    cursor.session.reset();
}

void SwitcherOverlayModel::moveDown() {
    if (workspaces.empty()) {
        return;
    }
    for (std::size_t index = 0; index < workspaces.size(); ++index) {
        WorkspaceNode& workspace = workspaces[index];
        if (workspace.id != cursor.workspace) {
            continue;
        }
        if (!cursor.session.has_value()) {
            if (collapsed.find(workspace.id) == collapsed.end() && !workspace.sessions.empty()) {
                cursor.session = workspace.sessions.front().id;
                return;
            }
        } else {
            for (std::size_t leaf = 0; leaf + 1 < workspace.sessions.size(); ++leaf) {
                if (workspace.sessions[leaf].id == *cursor.session) {
                    cursor.session = workspace.sessions[leaf + 1].id;
                    return;
                }
            }
        }
        if (index + 1 < workspaces.size()) {
            cursor.workspace = workspaces[index + 1].id;
            cursor.session.reset();
        }
        return;
    }
    cursor.workspace = workspaces.front().id;
    cursor.session.reset();
}

void SwitcherOverlayModel::moveUp() {
    if (workspaces.empty()) {
        return;
    }
    for (std::size_t index = 0; index < workspaces.size(); ++index) {
        WorkspaceNode& workspace = workspaces[index];
        if (workspace.id != cursor.workspace) {
            continue;
        }
        if (cursor.session.has_value()) {
            for (std::size_t leaf = 0; leaf < workspace.sessions.size(); ++leaf) {
                if (workspace.sessions[leaf].id != *cursor.session) {
                    continue;
                }
                if (leaf > 0) {
                    cursor.session = workspace.sessions[leaf - 1].id;
                } else {
                    cursor.session.reset();
                }
                return;
            }
        }
        if (index > 0) {
            WorkspaceNode& previous = workspaces[index - 1];
            cursor.workspace = previous.id;
            cursor.session.reset();
            if (collapsed.find(previous.id) == collapsed.end() && !previous.sessions.empty()) {
                cursor.session = previous.sessions.back().id;
            }
        }
        return;
    }
    cursor.workspace = workspaces.front().id;
    cursor.session.reset();
}

void SwitcherOverlayModel::toggleExpand() {
    if (cursor.workspace.value.empty()) {
        return;
    }
    if (collapsed.find(cursor.workspace) != collapsed.end()) {
        collapsed.erase(cursor.workspace);
        return;
    }
    collapsed.insert(cursor.workspace);
    cursor.session.reset();
}

bool UiModel::has_streaming_reasoning() const {
    for (const auto& [id, state] : sessions) {
        (void)id;
        for (const ConversationEntry& entry : state.conversation.entries) {
            if (entry.role == ConversationRole::Reasoning && entry.streaming) {
                return true;
            }
        }
    }
    return false;
}

bool UiModel::advance_reasoning_spinner(std::chrono::milliseconds delta) {
    constexpr std::chrono::milliseconds kFrameStep{120};
    if (!has_streaming_reasoning()) {
        spinner.elapsed = std::chrono::milliseconds::zero();
        return false;
    }
    spinner.elapsed += delta;
    bool changed = false;
    while (spinner.elapsed >= kFrameStep) {
        spinner.elapsed -= kFrameStep;
        ++spinner.frame;
        changed = true;
    }
    return changed;
}

bool UiModel::has_active_turn() const {
    const auto workspace = workspaces.find(activeWorkspaceId);
    if (workspace == workspaces.end()) {
        return false;
    }
    const auto state = sessions.find(workspace->second.activeSessionId());
    if (state == sessions.end()) {
        return false;
    }
    return is_active_state(state->second.agent_state);
}

bool UiModel::active_has_streaming_reasoning() const {
    const auto workspace = workspaces.find(activeWorkspaceId);
    if (workspace == workspaces.end()) {
        return false;
    }
    const auto state = sessions.find(workspace->second.activeSessionId());
    if (state == sessions.end()) {
        return false;
    }
    for (const ConversationEntry& entry : state->second.conversation.entries) {
        if (entry.role == ConversationRole::Reasoning && entry.streaming) {
            return true;
        }
    }
    return false;
}

bool UiModel::advance_spinner(std::chrono::milliseconds delta) {
    constexpr std::chrono::milliseconds kFrameStep{120};
    if (!has_active_turn() && !active_has_streaming_reasoning()) {
        spinner.elapsed = std::chrono::milliseconds::zero();
        return false;
    }
    spinner.elapsed += delta;
    bool changed = false;
    while (spinner.elapsed >= kFrameStep) {
        spinner.elapsed -= kFrameStep;
        ++spinner.frame;
        changed = true;
    }
    return changed;
}

void UiModel::set_now_reader(ClockReader reader) {
    if (reader) {
        now_reader = std::move(reader);
    }
}

} // namespace ymh::ui

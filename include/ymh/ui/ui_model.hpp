#pragma once

// Pure presentation model (10-supervisor-tui.md §4). No FTXUI, no core object
// pointers: `SessionUiState` holds a `SessionId` only (D1, §4.3).

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/ui/ui_event.hpp"

namespace ymh::ui {

struct UiModel;

constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);

enum class UiDirtyFlag : std::uint32_t {
    None         = 0,
    Conversation = 1u << 0,
    Tools        = 1u << 1,
    Diff         = 1u << 2,
    Input        = 1u << 3,
    Status       = 1u << 4,
    Layout       = 1u << 5,
    Subagents    = 1u << 6,
    Attention    = 1u << 7,
    SessionBar   = 1u << 8,
    Aggregate    = 1u << 9,
};

constexpr UiDirtyFlag operator|(UiDirtyFlag a, UiDirtyFlag b) noexcept {
    return static_cast<UiDirtyFlag>(static_cast<std::uint32_t>(a) |
                                    static_cast<std::uint32_t>(b));
}

inline UiDirtyFlag& operator|=(UiDirtyFlag& a, UiDirtyFlag b) noexcept {
    a = a | b;
    return a;
}

constexpr UiDirtyFlag operator&(UiDirtyFlag a, UiDirtyFlag b) noexcept {
    return static_cast<UiDirtyFlag>(static_cast<std::uint32_t>(a) &
                                    static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr bool any_flag(UiDirtyFlag flags) noexcept {
    return static_cast<std::uint32_t>(flags) != 0;
}

// Per-session dirty bits + one aggregate bit (10 §3.5, U11/F7).
class DirtySet {
public:
    void mark(const SessionId& session, UiDirtyFlag flag);
    void markAggregate();
    [[nodiscard]] bool takeAggregate();
    [[nodiscard]] std::vector<SessionId> takeDirtySessions();
    [[nodiscard]] UiDirtyFlag peek(const SessionId& session) const;
    [[nodiscard]] bool hasAny() const;
    void clear();

private:
    std::map<SessionId, UiDirtyFlag> perSession_;
    UiDirtyFlag                      aggregate_ = UiDirtyFlag::None;
};

enum class ConversationRole : std::uint8_t {
    User,
    Assistant,
    Tool,
    System,
};

struct ConversationEntry {
    ConversationRole role = ConversationRole::Assistant;
    std::string      text;
    std::string      tool_name;
    std::string      tool_call_id;
    bool             streaming = false;
};

struct ConversationModel {
    std::vector<ConversationEntry>            entries;
    std::unordered_map<std::string, std::size_t> by_message;

    [[nodiscard]] std::size_t find_message(const std::string& id) const;
};

// Per-session conversation viewport (10 §8.2 refinement). Supervisor-local,
// purely presentational: scrolling never gates agent work and never touches the
// event log. `fraction` is the scroll position in [0,1] from the top; it is only
// meaningful while `following` is false. `unseen` is raised when content arrives
// while the view is scrolled up.
struct ConversationScroll {
    static constexpr float kPageStep = 0.20f;
    static constexpr float kLineStep = 0.04f;

    float fraction  = 0.0f;
    bool  following = true;
    bool  unseen    = false;

    [[nodiscard]] float position() const { return following ? 1.0f : fraction; }

    void pageUp();
    void pageDown();
    void lineUp();
    void lineDown();
    void toTop();
    void toBottom();
    void onNewContent();
};

struct ToolCallView {
    std::string  id;
    std::string  name;
    std::string  arguments;
    std::string  output;
    payload::ToolOutcome outcome = payload::ToolOutcome::Ok;
    bool         truncated = false;
    bool         finished = false;
    bool         expanded = false;
};

struct ToolModel {
    std::vector<ToolCallView>                    calls;
    std::unordered_map<std::string, std::size_t> by_id;

    [[nodiscard]] std::size_t find(const std::string& id) const;
};

struct InputModel {
    std::string              draft;
    std::size_t              cursor = 0;
    std::vector<std::string> history;
    std::size_t              history_pos = 0;
    std::string              saved_draft;

    void push_history(std::string line);
    bool history_up();
    bool history_down();
    bool delete_forward();
    void clear_line();
    bool delete_word();
};

// One entry of the slash-command completion list, snapshotted into the model so
// the renderer stays pure (10 §8.2 refinement).
struct CommandHint {
    std::string name;
    std::string description;
};

struct StatusModel {
    std::string model;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cached_tokens = 0;
    AgentState  agent_state = AgentState::Idle;
    std::string last_error;
    std::string note;
};

// Derived/cached attention for one session; never advanced in Render() (10 §4.4).
struct AttentionState {
    AgentState lastState = AgentState::Idle;
    bool       needsInput = false;
    bool       completed = false;
};

struct SubagentView {
    SessionId   id;
    std::string summary;
    AgentState  state = AgentState::Idle;
};

struct SubagentModel {
    std::vector<SubagentView> agents;
};

struct SessionUiState {
    SessionId id;
    WorkspaceId workspace;

    ConversationModel  conversation;
    ToolModel          tools;
    InputModel         input;
    StatusModel        status;
    AttentionState     attention;
    SubagentModel      subagents;
    ConversationScroll scroll;
    std::vector<CommandHint> command_hints;

    AgentState agent_state = AgentState::Idle;
};

struct SessionCell {
    SessionId   id;
    std::string title;
    AgentState  state = AgentState::Idle;
    bool        attention = false;
    bool        unread = false;
    bool        readOnly = false;
};

struct WorkspaceModel {
    WorkspaceId              id;
    std::string              title;
    std::string              cwd;
    std::string              boot_id;
    DaemonStatus             daemonStatus = DaemonStatus::Attached;
    SessionId                activeSessionId;
    std::vector<SessionCell> sessions;

    [[nodiscard]] bool hasDaemon() const { return daemonStatus == DaemonStatus::Attached; }
};

struct SessionNode {
    SessionId   id;
    std::string title;
    AgentState  state = AgentState::Idle;
    bool        attention = false;
};

struct WorkspaceNode {
    WorkspaceId              id;
    std::string              title;
    DaemonStatus             status = DaemonStatus::Connecting;
    std::vector<SessionNode> sessions;
};

struct SwitcherCursor {
    WorkspaceId              workspace;
    std::optional<SessionId> session;

    auto operator<=>(const SwitcherCursor&) const = default;
};

class SwitcherOverlayModel {
public:
    std::vector<WorkspaceNode> workspaces;
    SwitcherCursor             cursor;
    std::optional<std::string> filter;
    std::set<WorkspaceId>      collapsed;

    void open(const UiModel& model);
    void close();
    void moveDown();
    void moveUp();
    void toggleExpand();
};

struct AggregateStatus {
    std::uint16_t activeCount = 0;
    std::uint16_t waitingCount = 0;

    bool operator==(const AggregateStatus&) const = default;
};

enum class FlashPhase : std::uint8_t {
    Idle,
    Flashing,
    Done,
};

struct FlashState {
    FlashPhase                phase = FlashPhase::Idle;
    std::chrono::milliseconds elapsed{};
    bool                      enabled = false;

    [[nodiscard]] bool isFlashing() const { return phase == FlashPhase::Flashing; }
    void arm();
    void tick(std::chrono::milliseconds delta);
};

class AggregateStatusModel {
public:
    AggregateStatus current;
    FlashState      flash;

    void recompute(const std::map<WorkspaceId, WorkspaceModel>& workspaces,
                   const std::map<SessionId, SessionUiState>&  sessions);
    void armOnEdge(AgentState oldState, AgentState newState);
};

struct PermissionDialogModel {
    bool                open = false;
    SessionId           session;
    PermissionRequestId request;
    std::string         tool;
    std::string         summary;
    int                 selected = 0;   // 0=Once 1=Session 2=Always 3=Deny
};

struct UiModel {
    std::map<WorkspaceId, WorkspaceModel> workspaces;
    WorkspaceId                           activeWorkspaceId;
    std::map<SessionId, SessionUiState>   sessions;
    AggregateStatusModel                  aggregate;
    SwitcherOverlayModel                  switcher;
    PermissionDialogModel                 dialog;
    UiMode                                mode = UiMode::Conversation;
    bool                                  shouldExit = false;
    DirtySet                              dirty;

    [[nodiscard]] WorkspaceModel*  activeWorkspace();
    [[nodiscard]] SessionUiState*  activeSession();
    [[nodiscard]] SessionUiState*  session(const SessionId& id);
    [[nodiscard]] const SessionUiState* session(const SessionId& id) const;

    SessionUiState& ensureSession(const SessionId& id);
    SessionUiState& ensureSessionIn(const WorkspaceId& workspace, const SessionId& id);
    void            ensureCell(const SessionId& id);
    void            ensureCellIn(const WorkspaceId& workspace, const SessionId& id);
    void            refreshCell(const SessionId& id);
    void            refreshCellIn(const WorkspaceId& workspace, const SessionId& id);
    void            setSessionReadOnly(const SessionId& id, bool read_only);

    // Builds (or refreshes) the switcher node tree from the current workspaces
    // and sessions (10 §7.1). Pure model work; no registry/daemon access.
    void            openSwitcher();
    void            focusWorkspace(const WorkspaceId& workspace);
    void            focusSession(const SessionId& id);

    void apply(const UiEvent& event);
    void apply(const WorkspaceEvent& event);
};

[[nodiscard]] bool is_active_state(AgentState state) noexcept;
[[nodiscard]] bool is_waiting_state(AgentState state) noexcept;

} // namespace ymh::ui

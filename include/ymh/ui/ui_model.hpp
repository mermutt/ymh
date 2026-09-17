#pragma once

// Pure presentation model (10-supervisor-tui.md §4). No FTXUI, no core object
// pointers: `SessionUiState` holds a `SessionId` only (D1, §4.3).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/agent/context_snapshot.hpp"
#include "ymh/ui/session_catalog.hpp"
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
    Reasoning,
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
    // 17 §4 (RB-02): all reasoning deltas of one message coalesce into a single
    // folded Reasoning entry, indexed here independently of `by_message`.
    std::unordered_map<std::string, std::size_t> by_reasoning_message;

    [[nodiscard]] std::size_t find_message(const std::string& id) const;
    [[nodiscard]] std::size_t find_reasoning_message(const std::string& id) const;
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

// 17 §5 (RB-08): Tab-completion cycle state. Names (not Command pointers) keep
// the model pure and pointer-free; the field is per-session (U9).
struct CompletionCycle {
    std::string              draft;
    std::vector<std::string> names;
    std::size_t              index = 0;
};

struct InputModel {
    std::string              draft;
    std::size_t              cursor = 0;
    std::vector<std::string> history;
    std::size_t              history_pos = 0;
    std::string              saved_draft;
    std::optional<CompletionCycle> completion;

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
    // 17 §4 (RB-02): global expand-all / collapse-all for foldable entries.
    bool expand_all_folds = false;

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
    // 22 §3.1 (S1): true only once this supervisor's handshake is `Attached`.
    // The Live switcher renders a workspace iff `live && daemonStatus ∈
    // {Attached, Stopping}` (SW1/SW20). Default false so a workspace created by
    // a stray `ensureCellIn`/`ensureSessionIn` (default `daemonStatus ==
    // Attached`) is never rendered.
    bool                     live = false;

    [[nodiscard]] bool hasDaemon() const { return daemonStatus == DaemonStatus::Attached; }
};

// 22 §3.6 (H1): workspace-independent notice surface. Rendered in the status
// bar; never routed through `ensureSessionIn`/`ensureCellIn` for an unmodeled
// workspace.
struct UiNotice {
    std::string  text;
    std::int64_t atMs = 0;
};

constexpr std::size_t kMaxNotices = 8;

// 16 §3.6 (C4): display-only switcher marker, derived from DaemonStatus. It is
// supervisor-local and never written to the registry.
enum class OwnershipMark : std::uint8_t {
    Owned,
    NotRunning,
    Stopping,
    Unreachable,
};

struct SessionNode {
    SessionId   id;
    std::string title;
    AgentState  state = AgentState::Idle;
    bool        attention = false;
    // 22 §3.6 (S2): History-source fields; default-initialized for the Live
    // source (`fromDisk == false`).
    bool                     fromDisk = false;
    std::string              kind;                 // "root" | "fork" | "subagent"
    std::string              model;
    std::int64_t             updatedAt = 0;
    std::optional<SessionId> parent;
};

// 22 §3.6: the switcher's projection source. `History` is declared for S2 and
// is unused by S1.
enum class SwitcherSource : std::uint8_t {
    Live,
    History,
};

struct WorkspaceNode {
    WorkspaceId                id;
    std::string                title;
    DaemonStatus               status = DaemonStatus::Connecting;
    OwnershipMark              mark = OwnershipMark::Unreachable;
    bool                       live = true;
    // 22 §3.6 (S2): a registered workspace with no live daemon (History source
    // only). It renders `[history]` and, when `note` is set, its degradation
    // leaf; it is never rendered by the Live source.
    bool                       historyOnly = false;
    std::optional<std::string> note;
    std::vector<SessionNode>   sessions;
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
    SwitcherSource             source = SwitcherSource::Live;

    void open(const UiModel& model);
    // 22 §3.6/§4.3 (S2): History source — builds nodes from `model.catalog`
    // (every registered workspace, live or not). Sets `source = History`.
    void openHistory(const UiModel& model);
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

// 16 §7.6 / §4.2: the last-supervisor exit confirmation. Counts and workspace
// titles only; never a per-session list (C2). `selected` is 0=Terminate,
// 1=Cancel and defaults to Cancel so a stray Enter cannot tear daemons down.
struct ExitConfirmState {
    bool                     open = false;
    std::vector<WorkspaceId> orphaning;
    int                      sessions = 0;
    int                      running = 0;
    int                      selected = 1;
};

// 18 §4.2 (CX-04): the read-only `/context` overlay state. Purely
// presentational; the snapshot is a daemon RPC reply, never a UiEvent.
struct ContextOverlayModel {
    bool            open = false;
    bool            loaded = false;
    SessionId       session;
    ContextSnapshot snapshot;   // ymh::ContextSnapshot (agent value type, 18 §3)
    int             view = 0;   // 0 = grid+legend, 1 = tools/servers
    int             scroll = 0;
    // The note string the overlay renders verbatim in its bottom-line slot
    // (§5.1 CX-D12) when non-empty. On success it is the daemon's
    // `snapshot.note` (e.g. "budget unknown"); on a UI-local failure it is
    // "malformed snapshot" or the transport error. Empty means no note.
    std::string     note;
};

// 22 §4.6 (S2): the last delivered catalog snapshot, projected for `/sessions`.
// `nowMs` is additive (Wave C): the UI-thread wall clock at store time, so the
// pure renderer can derive `captured <relative> ago` and the per-session
// relative update without reading a clock (10 U3/D16).
struct SessionCatalogModel {
    std::vector<WorkspaceHistory> workspaces;
    bool                          loaded = false;
    bool                          complete = false;
    std::int64_t                  capturedAtMs = 0;
    std::uint64_t                 generation = 0;
    std::int64_t                  nowMs = 0;
};

struct UiModel {
    std::map<WorkspaceId, WorkspaceModel> workspaces;
    WorkspaceId                           activeWorkspaceId;
    std::map<SessionId, SessionUiState>   sessions;
    AggregateStatusModel                  aggregate;
    SwitcherOverlayModel                  switcher;
    PermissionDialogModel                 dialog;
    ExitConfirmState                      exitConfirm;
    ContextOverlayModel                   context;
    SessionCatalogModel                   catalog;
    UiMode                                mode = UiMode::Conversation;
    bool                                  shouldExit = false;
    std::string                           mcp_status;
    DirtySet                              dirty;
    std::deque<UiNotice>                  notices;

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
    void            setCellTitle(const WorkspaceId& workspace, const SessionId& id,
                                 std::string title);
    void            setSessionReadOnly(const SessionId& id, bool read_only);
    void            eraseSession(const WorkspaceId& workspace, const SessionId& id);

    // 15 §4.7 (AM-1): the bounded MCP status token projected from a
    // `HostNoticeKind::McpServerStatus` host notice.
    void            setMcpStatus(std::string detail);

    // 22 §3.6 (H1): appends a status-bar notice, dropping the oldest past
    // `kMaxNotices`.
    void            pushNotice(std::string text);

    // 22 §3.5 (S1): removes a workspace and its sessions, repairs
    // `activeWorkspaceId` by the §3.5 step 4 promotion rule, and re-snapshots a
    // Live switcher whose cursor targeted it.
    void            eraseWorkspace(const WorkspaceId& workspace);

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

// 16 §3.6: the switcher's display-only ownership mark for one daemon status.
[[nodiscard]] OwnershipMark ownership_mark(DaemonStatus status) noexcept;

// 22 §3.1 (SW1/SW20): the Live-source display predicate. A workspace is
// renderable iff `live` and its link is `Attached` or `Stopping`.
[[nodiscard]] bool live_switcher_renderable(const WorkspaceModel& workspace) noexcept;

// 22 §3.1: the liveness transition driven by a link state. `live` becomes true
// only for `Attached` and false for `Detached`/`Dead`/`NotRunning`; it is left
// unchanged for `Connecting` (attach in flight) and `Stopping` (daemon still
// holds its sidecar lock).
void apply_daemon_status_liveness(WorkspaceModel& workspace, DaemonStatus status) noexcept;

// 22 §3.2: the workspaces an `on_scan` tick must evict. Pure: a workspace is
// retained when it is in the live set or its link is still `Connecting`.
[[nodiscard]] std::vector<WorkspaceId> switcher_eviction_candidates(
    const std::map<WorkspaceId, WorkspaceModel>& workspaces,
    const std::set<WorkspaceId>& live_ids,
    const std::set<WorkspaceId>& connecting_ids);

} // namespace ymh::ui

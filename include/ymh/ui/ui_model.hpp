#pragma once

// Pure presentation model (10-supervisor-tui.md §4). No FTXUI, no core object
// pointers: `SessionUiState` holds a `SessionId` only (D1, §4.3).

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ymh/agent/context_snapshot.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/ui_event.hpp"

namespace ymh {
struct Config;
struct ResolvedModel;
} // namespace ymh

namespace ymh::ui {

struct UiModel;

// 53-D3.1: the canonical status-line model value: the `llm.models` entry name
// when named, else the wire id. Used at create, refresh, apply_create_reply,
// resume, and catalog hydration so the segment never flips between name and id.
[[nodiscard]] std::string display_model(const ymh::ResolvedModel& model);
[[nodiscard]] std::string display_model(const ymh::Config& config,
                                        const std::string& model_id);

constexpr std::size_t kNoEntry = static_cast<std::size_t>(-1);

// 25-D6: injectable monotonic clock reader for TPS.
using ClockReader = std::function<std::chrono::steady_clock::time_point()>;

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
    Context,
    // 62-D5: an actionable, non-error notice (e.g. the step hard ceiling). Not
    // dimmed like System, so a stopped-but-recoverable turn stands out.
    Notice,
};

struct ConversationEntry {
    ConversationRole role = ConversationRole::Assistant;
    std::string      text;
    std::string      tool_name;
    std::string      tool_call_id;
    bool             streaming = false;
    std::optional<MessageSource> source  = std::nullopt;
    std::optional<ContextFormed> context = std::nullopt;
};

struct ConversationModel {
    std::vector<ConversationEntry>            entries;
    std::unordered_map<std::string, std::size_t> by_message;
    // 17 §4 (RB-02): all reasoning deltas of one message coalesce into a single
    // folded Reasoning entry, indexed here independently of `by_message`.
    std::unordered_map<std::string, std::size_t> by_reasoning_message;

    [[nodiscard]] std::size_t find_message(const std::string& id) const;
    [[nodiscard]] std::size_t find_reasoning_message(const std::string& id) const;

    // 73-D7: idempotent append guard for `UserMessage`/`ContextInjected`.
    // Returns true iff `id` was not already applied, so a replayed stream
    // cannot double-render. The set is per-session and dies with this model.
    [[nodiscard]] bool mark_seen(const MessageId& id);
    // 73-D8/58-A10: the explicit "re-apply is intended" signal. Called by
    // `UiEventAdapter::forget_session` (the 58 HIGH-1 dedup reset), it drops the
    // seen-ids only; rendered entries are untouched.
    void reset_dedup();

  private:
    std::unordered_set<MessageId> seen_ids_;
};

// Per-session conversation viewport (10 §8.2 refinement; 60-D2). Supervisor-local,
// purely presentational: scrolling never gates agent work and never touches the
// event log. `top` is the row-exact index of the first visible content row (0 =
// first row); it is meaningful only while `following` is false, and
// `observeGeometry()` clamps it to [0, max_top()]. `unseen` is raised when content
// arrives while the view is scrolled up.
struct ConversationScroll {
    static constexpr float kPageStep = 0.20f;

    int  top           = 0;
    int  content_rows  = 0;
    int  viewport_rows = 0;
    bool following     = true;
    bool unseen        = false;

    [[nodiscard]] int max_top() const {
        return std::max(0, content_rows - std::max(1, viewport_rows));
    }

    [[nodiscard]] int page_rows() const {
        return std::max(1, static_cast<int>(static_cast<float>(content_rows) * kPageStep));
    }

    // Row index handed to the renderer's row-indexed focus (60-D2). `content` is
    // the just-measured height so the upper clamp is never stale; `following`
    // yields `content` (one past the last row) and the frame clamps to bottom.
    [[nodiscard]] int focus_row(int content) const {
        if (following) {
            return content;
        }
        const int vp = std::max(1, viewport_rows);
        return std::clamp(top + (vp - 1) / 2, 0, std::max(0, content - 1));
    }

    void lineUp();
    void lineDown();
    void pageUp();
    void pageDown();
    void toTop();
    void toBottom();
    void onNewContent();
    void observeGeometry(int content_rows, int viewport_rows);
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
    std::optional<ContextFormed> notice = std::nullopt;
};

struct ToolModel {
    std::vector<ToolCallView>                    calls;
    std::unordered_map<std::string, std::size_t> by_id;

    [[nodiscard]] std::size_t find(const std::string& id) const;
};

// 45-D2 (45-I4): `CompletionCycle` and `InputModel::completion` are retired.
// Tab no longer freezes a cycle; ArrowUp/ArrowDown move the highlight and Tab
// consumes it (45-D2).
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
    // 48-D4.5: class-model delete (skip trailing Space, then one Word/Punct run).
    bool delete_word();

    // 48-D4.1: pure word motion. Never edits the draft. Byte-aligned to a
    // character-class boundary (Word/Space/Punct; continuation bytes inherit
    // their lead byte's class). Both are total and clamp to [0, draft.size()].
    [[nodiscard]] std::size_t word_left_boundary(std::size_t cursor) const;
    [[nodiscard]] std::size_t word_right_boundary(std::size_t cursor) const;

    // 48-D5.1 (Rev 2): glyph-wise motion for the plain arrows. Both are pure,
    // total, and clamp to [0, draft.size()]; `cursor_left` never lands on a
    // UTF-8 continuation byte.
    [[nodiscard]] std::size_t cursor_left(std::size_t cursor) const;
    [[nodiscard]] std::size_t cursor_right(std::size_t cursor) const;
};

// 48-D5.1 (Rev 2): render-time glyph snap. Advances `cursor` off any UTF-8
// continuation byte to the next lead byte, clamped to `draft.size()`.
[[nodiscard]] std::size_t glyph_floor(std::string_view draft,
                                      std::size_t cursor) noexcept;

// 48-D5.1 (Rev 2, gate MEDIUM): the byte length of the UTF-8 glyph beginning at
// `cursor` (1..4), clamped to the end of `text`; 0 when `cursor >= text.size()`.
[[nodiscard]] std::size_t glyph_len(std::string_view text,
                                    std::size_t cursor) noexcept;

// 48-D5.1 (Rev 2, gate MEDIUM): the glyph beginning at `cursor`; empty when
// `cursor >= text.size()`. `glyph_at` is `text.substr(cursor, glyph_len(...))`.
[[nodiscard]] std::string_view glyph_at(std::string_view text,
                                        std::size_t cursor) noexcept;

// 48-D2.1: the Esc-Esc arm. Per-session, never persisted.
enum class EscArm : std::uint8_t { Disarmed, Armed };

// One entry of the slash-command completion list, snapshotted into the model so
// the renderer stays pure (10 §8.2 refinement). 45-D8: `display` is the rendered
// label (`name` + aliases, e.g. "exit(quit)"). 46-D6: `name` stays the canonical
// name (identity); `insert` is the matched spelling completion writes.
struct CommandHint {
    std::string name;
    std::string display;
    std::string insert;
    std::string description;
};

// 45-D7: derived last-outcome connectivity (never a probe). `Error` can be set
// by a non-LLM `ErrorOccurred` until the next successful assistant message.
enum class ApiConnectivity : std::uint8_t {
    Unknown,
    Ok,
    Error,
};

struct StatusModel {
    std::string model;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cached_tokens = 0;
    AgentState  agent_state = AgentState::Idle;
    std::string last_error;
    std::string note;
    // 45-D7: derived connectivity for `/status`.
    ApiConnectivity api_state = ApiConnectivity::Unknown;
    // 45-D9: the DAEMON-ACTIVE preset only; never the pending preference.
    std::string agent;
    // 45-D9.6: the pending preference for the next session.create, rendered
    // distinctly (e.g. "agent(next):<id>"); never written into `agent`.
    std::string pending_agent;
    // 25-D1/D2/D6/D7
    bool                  plan_active = false;
    std::optional<double> tps;
    std::uint64_t         context_used_tokens = 0;
    std::uint64_t         context_window_tokens = 0;
};

// Derived/cached attention for one session; never advanced in Render() (10 §4.4).
struct AttentionState {
    AgentState lastState = AgentState::Idle;
    bool       needsInput = false;
    bool       completed = false;
};

struct SubagentView {
    SessionId      id;
    std::string    summary;
    AgentState     state = AgentState::Idle;
    SubagentStatus status = SubagentStatus::Running;
};

struct SubagentModel {
    std::vector<SubagentView> agents;
};

// 77-D3: the optimistic open state. Set the instant a resume is submitted for a
// modeled workspace; cleared at replay completion (77-D4), on the first live
// (replay == false) envelope, on resume failure, on a link-state drop, on
// session eviction, or by the kOpeningTimeout escape. The derived turn is
// reconciled independently of this placeholder on every replay envelope, so
// clearing it early cannot strand a dangling turn. Never persisted.
struct OpeningState {
    std::string                           title;
    std::chrono::steady_clock::time_point since;
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
    // 45-D2: index into `command_hints` of the entry Tab completes. The renderer
    // paints it brighter; it is reset to 0 whenever the hint set is rebuilt and
    // moved by ArrowUp/ArrowDown.
    std::size_t command_hint_selected = 0;
    // 45-D5: true while the list is hidden by Esc; any command-prefix edit clears
    // it (45-I9).
    bool hints_dismissed = false;
    // 17 §4 (RB-02): global expand-all / collapse-all for foldable entries.
    bool expand_all_folds = false;
    // 25-D6: non-durable TPS clock start for the streaming assistant message.
    std::optional<std::chrono::steady_clock::time_point> stream_started_at;

    // 48-D2.1: the per-session Esc-Esc arm and its arming instant. UI-only;
    // never persisted and never written to the event log.
    EscArm esc_arm = EscArm::Disarmed;
    std::optional<std::chrono::steady_clock::time_point> esc_armed_at;

    // 62-D2 (Rev 3): the instant of the last edit to THIS session's composer.
    // Per-session (not model-global) so an edit in one workspace cannot force
    // the caret `Bar` in another; it is the same scope the renderer arms the
    // caret from (the rendered composer's session).
    std::optional<std::chrono::steady_clock::time_point> composer_input_at;

    // 58-D7: true for a materialized viewed-child state. Such a state is never a
    // `SessionCell` and never appears in the Live switcher or `/sessions`.
    bool subagent = false;

    AgentState agent_state = AgentState::Idle;

    // 77-D3: present iff this session is actively being opened/resumed.
    std::optional<OpeningState> opening;
    // 77-D2: the opaque event.subscribe cursor (EventCursor.value) of the last
    // event materialized into this state. Empty when no event has been applied.
    // Never parsed; dies with the state.
    std::string applied_through;
};

struct SessionCell {
    SessionId   id;
    std::string title;
    AgentState  state = AgentState::Idle;
    bool        attention = false;
    bool        unread = false;
    bool        readOnly = false;
};

// 81-D5: the dashboard's per-row status. `Stopped` is a vocabulary member only;
// `dashboard_status` never produces it (no durable per-session stop flag exists).
enum class DashboardStatus : std::uint8_t {
    Working,
    NeedsInput,
    Idle,
    Completed,
    Failed,
    Stopped,
};

// 81-D6: render order (NeedsInput > Working > Completed); enum order is normative.
enum class DashboardGroup : std::uint8_t {
    NeedsInput,
    Working,
    Completed,
};

// 81-D2/81-D7: one projected live session row. `status`/`group` are derived on
// every project and stored so the renderer never re-derives (81-I6).
struct DashboardRow {
    WorkspaceId                 workspace;
    SessionId                   session;
    std::string                 title;
    std::string                 workspace_title;
    std::int64_t                workspace_last_used = 0;   // WorkspaceNode::lastUsedAt
    DashboardStatus             status = DashboardStatus::Idle;
    DashboardGroup              group  = DashboardGroup::Completed;
    std::optional<std::int64_t> updatedAt;                 // catalog join; nullopt == unknown
    bool                        workspace_stopping = false;
};

// 81-D7: the summary counts; `Completed` folds Completed+Failed+Idle (81-D6).
struct DashboardCounts {
    std::size_t needs_input = 0;
    std::size_t working = 0;
    std::size_t completed = 0;
    std::size_t idle = 0;
    std::size_t failed = 0;
};

class WorkspaceModel {
public:
    WorkspaceId              id;
    std::string              title;
    std::string              cwd;
    std::string              boot_id;
    DaemonStatus             daemonStatus = DaemonStatus::Attached;
    std::vector<SessionCell> sessions;
    // 22 §3.1 (S1): true only once this supervisor's handshake is `Attached`.
    // The Live switcher renders a workspace iff `live && daemonStatus ∈
    // {Attached, Stopping}` (SW1/SW20). Default false so a workspace created by
    // a stray `ensureCellIn`/`ensureSessionIn` (default `daemonStatus ==
    // Attached`) is never rendered.
    bool                     live = false;

    [[nodiscard]] bool hasDaemon() const { return daemonStatus == DaemonStatus::Attached; }

    // 45-D10.7: the focus is PRIVATE with a single public writer,
    // `UiModel::focusSessionIn` (which models the target via `ensureSessionIn`).
    // `UiModel` is a friend so it can call the private setter; `eraseSession`
    // clears through the same setter. Any other translation unit that tries to
    // assign the focus fails to compile — 45-I10 is enforced by the compiler,
    // not by convention. Public read is via the getter.
    [[nodiscard]] const SessionId& activeSessionId() const noexcept { return activeSessionId_; }

private:
    friend struct UiModel;
    void setActiveSessionId(SessionId id) noexcept { activeSessionId_ = std::move(id); }
    SessionId activeSessionId_;   // written only by focusSessionIn / eraseSession
};

// 22 §3.6 (H1): workspace-independent notice surface. Rendered in the status
// bar; never routed through `ensureSessionIn`/`ensureCellIn` for an unmodeled
// workspace.
struct UiNotice {
    std::string  text;
    std::int64_t atMs = 0;
};

constexpr std::size_t kMaxNotices = 8;

// 77-D3/OL15: the bounded wall-clock lifetime of an `opening` placeholder (30 s).
// `UiModel::expireOpenings` ages it out; the supervisor timer schedules that call.
constexpr std::chrono::milliseconds kOpeningTimeout{30'000};

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
// is unused by S1. 58-D9 adds `Subagents` (the viewed session's child picker).
enum class SwitcherSource : std::uint8_t {
    Live,
    History,
    Subagents,
};

// 58-D9: what Enter does for a source. Data, consumed by E4.
enum class SwitcherEnter : std::uint8_t {
    Focus,
    Resume,
    EnterChild,
};

// 58-D9/P2: the per-source policy. The single place the nine per-source
// presentation/action divergences live; the shared widget/handler consult it.
struct SwitcherSourcePolicy {
    std::string_view window_title;
    std::string_view heading;
    std::string_view footer;
    std::string_view empty_state;
    SwitcherEnter    enter;
    bool             ctrl_d_enabled;
    bool             tab_expands;
    bool             ctrl_t_closes;
    bool             r_refreshes;
};

[[nodiscard]] const SwitcherSourcePolicy& switcher_policy(SwitcherSource source) noexcept;

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
    // 45-D4.3: true iff >= 1 leaf was removed by the focused-session exclusion
    // and the result is empty, so the renderer shows `(current session hidden)`
    // instead of a false empty-state. No node is suppressed.
    bool                       sessions_hidden_by_focus = false;
    // 45-D3.3: the Live node's catalog snapshot has not been delivered yet
    // (`!catalog.loaded || generation == 0`); the renderer shows
    // `(loading live sessions…)`.
    bool                       catalog_pending = false;
    // 57-D5/57-D6: epoch ms of the workspace's last usage (max session updatedAt);
    // 0 == unknown, sorts last.
    std::int64_t               lastUsedAt = 0;
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

    // 51-D4.3: the switcher's double-Ctrl+D delete arm. Reuses 48-D2's `EscArm`
    // and the `kEscArmTimeout` window; UI-only, never persisted.
    EscArm                                     delete_arm = EscArm::Disarmed;
    std::optional<std::chrono::steady_clock::time_point> delete_armed_at;
    // 51-D4.3 (51-M1/M2): the exact target the arm is bound to. The confirm
    // requires the live cursor == *delete_target; a mismatch disarms, so a cursor
    // move can never redirect a confirmed delete.
    std::optional<SwitcherCursor>              delete_target;
    // 51-D4.5: a workspace target's `workspace_sessions` junction count, captured
    // at arm time so the confirmation can state it (the renderer stays pure).
    std::size_t                                delete_target_session_count = 0;

    void open(const UiModel& model);
    // 81-D2: the Live projection with the focused-session exclusion made
    // optional. `open(model)` delegates with `include_focused == false` (the
    // popup behaviour); the dashboard calls it with `true` so the focused
    // session is a row (81-I8).
    void openLive(const UiModel& model, bool include_focused);
    // 22 §3.6/§4.3 (S2): History source — builds nodes from `model.catalog`
    // (every registered workspace, live or not). Sets `source = History`.
    void openHistory(const UiModel& model);
    // 58-D1/A7: Subagents source — one node whose leaves are exactly
    // `viewedSession()->subagents.agents` (the current view level). The caller
    // sets `source = Subagents`.
    void openSubagents(const UiModel& model);
    void close();
    void moveDown();
    void moveUp();
    void toggleExpand();

    // 51-D4.3 (51-M1/M2): reset delete_arm/delete_armed_at/delete_target/
    // delete_target_session_count. Called by open(), openHistory() and close().
    void disarm_delete();
    // 51-D4.10 (51-M4): re-clamp `cursor` to a still-valid node after a delete.
    void clamp_cursor();
};

// 79-D4/D6: one rewind-picker row. `prompt` is the full restore text written to
// the child's composer; `summary` is the one-line excerpt the renderer draws.
struct RewindTargetView {
    TurnId       turn;
    std::int64_t boundary_index{0};
    std::int64_t started_at_ms{0};
    std::string  prompt;
    std::string  summary;
    // 80-D8/80-D9: distinct files a restore to this checkpoint would revert;
    // 0 hides the code actions.
    std::int64_t file_change_count{0};
};

// 79-D6: the `/rewind` overlay state. `open` mirrors `mode == UiMode::Rewind`
// for the renderer; `cursor` opens on the last (most recent) row.
struct RewindOverlayModel {
    bool                          open = false;
    WorkspaceId                   workspace;
    SessionId                     session;
    std::int64_t                  view_length{0};   // 79-D10 no-op guard
    std::vector<RewindTargetView> targets;
    std::size_t                   cursor = 0;
    UiMode                        prev_mode = UiMode::Conversation;

    void open_with(WorkspaceId ws, SessionId session, std::int64_t view_length,
                   std::vector<RewindTargetView> targets);
    void close();
    void moveUp();
    void moveDown();
    [[nodiscard]] const RewindTargetView* selected() const;
};

// 80-D9: the second step of `/rewind`. The four table actions (1-4) gate on
// `file_change_count`: code actions appear only when it is > 0.
enum class RewindAction : std::uint8_t {
    RestoreCodeAndConversation,
    RestoreConversation,
    RestoreCode,
    Cancel,
};

struct RewindActionModel {
    bool                      open = false;
    WorkspaceId               workspace;
    SessionId                 session;
    TurnId                    turn{0};
    std::int64_t              file_change_count{0};
    std::vector<RewindAction> actions;
    std::size_t               cursor = 0;
    UiMode                    prev_mode = UiMode::Rewind;

    void open_with(WorkspaceId ws, SessionId session, TurnId turn,
                   std::int64_t file_change_count);
    void close();
    void moveUp();
    void moveDown();
    [[nodiscard]] const RewindAction* selected() const;
};

// 81-D2: the dashboard is a pure snapshot rebuilt on open and on every catalog
// delivery. All fields are UI-only and process-local (81-I18). `open()` saves
// `prev_mode` from the outgoing mode before the caller sets `UiMode::Dashboard`.
struct DashboardModel {
    bool                      open = false;
    std::vector<DashboardRow> rows;
    std::size_t               cursor = 0;
    UiMode                    prev_mode = UiMode::Conversation;
    EscArm                    exit_arm = EscArm::Disarmed;
    std::optional<std::chrono::steady_clock::time_point> exit_armed_at;
    std::set<DashboardGroup>  collapsed;

    // 81-D2: `open` is the public base-selector flag (chain R). The snapshot
    // initializer is `begin_snapshot` -- C++ forbids a data member and a member
    // function sharing the name `open`.
    void begin_snapshot(const UiModel& model);
    void rebuild(const UiModel& model);
    void close();
    void moveDown();
    void moveUp();
    void pageDown();
    void pageUp();
    void moveHome();
    void moveEnd();
    void toggle_collapse();
    [[nodiscard]] DashboardCounts counts() const;

private:
    void project(const UiModel& model);
    void clamp_cursor();
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

// 53-D4: the `/model` picker overlay. Rows are the `llm.models` entries plus a
// synthetic row for the current literal id when no named entry is active.
struct ModelPickerRow {
    std::string name;      // llm.models key; "" for the synthetic literal row
    std::string model_id;  // wire id
    std::string endpoint;  // endpoint name
};

struct ModelPickerModel {
    bool                        visible = false;
    std::vector<ModelPickerRow> rows;
    std::size_t                 selected = 0;

    void open(const ymh::Config& config, const std::string& current_display);
    void close();
    void moveUp();
    void moveDown();
};

struct PermissionDialogModel {
    bool                open = false;
    SessionId           session;
    PermissionRequestId request;
    std::string         tool;
    std::string         summary;
    int                 selected = 0;   // 0=Once 1=Session 2=Always 3=Deny
    // 25-D4 (NEW-3 Rev 5): true => only {Allow once, Deny}; default Deny.
    bool                force_ask = false;
    // 81-D13: the mode the dialog interrupted, restored on `PermissionResolved`
    // so a dialog raised over the dashboard returns to it. Defaults to
    // `Conversation`, the only value the conversation path ever records.
    UiMode              prev_mode = UiMode::Conversation;
};

// 16 §7.6 / §4.2: the last-supervisor exit confirmation. Counts and workspace
// titles only; never a per-session list (C2). `selected` is 0=Terminate,
// 1=Cancel. It defaults to Terminate (0) to match the intent that opened it:
// this popup is only reachable through an explicit exit action (Ctrl-Q /
// `/exit`), so Enter confirming the exit is the natural outcome. Trade-off:
// a stray Enter now tears daemons down instead of merely cancelling; Escape /
// Ctrl-C / `n` remain the safe cancel paths.
struct ExitConfirmState {
    bool                     open = false;
    std::vector<WorkspaceId> orphaning;
    int                      sessions = 0;
    int                      running = 0;
    int                      selected = 0;
    // 81-D13: the mode the confirm interrupted, restored on cancel so a confirm
    // raised from the dashboard returns to it. Defaults to `Conversation`.
    UiMode                   prev_mode = UiMode::Conversation;
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
// pure renderer can derive the per-session relative update without reading a
// clock (10 U3/D16). 57-D1 removed the footer's `captured <relative> ago` label
// and its `capturedAtMs` carrier; `nowMs` is retained for the per-session leaf
// (57-I2).
struct SessionCatalogModel {
    std::vector<WorkspaceHistory> workspaces;
    bool                          loaded = false;
    bool                          complete = false;
    std::uint64_t                 generation = 0;
    std::int64_t                  nowMs = 0;
};

// RB-17 / 64-D1: the animation spinner clock. The frame is a pure function of
// the total wall-clock time elapsed since the animation started, accumulated
// from exact monotonic durations, so the number of `advance_spinner` calls
// (drain frequency) can never change the pace. `elapsed` holds the sub-step
// remainder; `last_tick` anchors the next exact delta.
struct ReasoningSpinnerState {
    std::uint32_t             frame = 0;
    std::chrono::steady_clock::duration elapsed{};
    std::optional<std::chrono::steady_clock::time_point> last_tick;
};

struct UiModel {
    std::map<WorkspaceId, WorkspaceModel> workspaces;
    WorkspaceId                           activeWorkspaceId;
    // 57-D5/57-D6: canonical path of `options_.initial_workspace` (the effective
    // root, not getcwd); empty == no effective-root match. Assigned once in
    // `SupervisorApp::run()`; never focus-derived.
    std::string                           cwdWorkspacePath;
    std::map<SessionId, SessionUiState>   sessions;
    // 58-D2: the active session's subagent focus path (outermost first). Empty
    // == main agent. Scoped to the active workspace/session; cleared by
    // `focusWorkspace`/`focusSessionIn`/`focusSession` (E22).
    std::vector<SessionId> subagent_path;
    // 49-D1: the zero-workspace composer. Before the first prompt creates a
    // workspace/session there is no `SessionUiState` in `sessions`; the draft and
    // its command hints live here so the empty screen is typeable. Never a
    // session cell and never persisted. Read only while `workspaces` is empty.
    SessionUiState                        pendingComposer;
    AggregateStatusModel                  aggregate;
    SwitcherOverlayModel                  switcher;
    PermissionDialogModel                 dialog;
    ExitConfirmState                      exitConfirm;
    ContextOverlayModel                   context;
    SessionCatalogModel                   catalog;
    ReasoningSpinnerState                 spinner;
    ModelPickerModel                      model_picker;
    // 81-D2: the full-screen dashboard snapshot (81-D1).
    DashboardModel                        dashboard;
    // 79-D6: the `/rewind` turn-picker overlay.
    RewindOverlayModel                    rewind;
    // 80-D9: the second-step action menu.
    RewindActionModel                     rewind_action;
    UiMode                                mode = UiMode::Conversation;
    // 53-D3.1: the resolved model for the no-session fallback segment (entry
    // name when named, else the wire id). Never persisted.
    std::string                           resolved_model;
    bool                                  shouldExit = false;
    std::string                           mcp_status;
    DirtySet                              dirty;
    std::deque<UiNotice>                  notices;

    [[nodiscard]] WorkspaceModel*  activeWorkspace();
    [[nodiscard]] SessionUiState*  activeSession();
    [[nodiscard]] SessionUiState*  session(const SessionId& id);
    [[nodiscard]] const SessionUiState* session(const SessionId& id) const;

    // 58-D2/A4: the deepest path entry's state, or the active session when the
    // path is empty. Returns nullptr (never default-constructs) when the deepest
    // id has no materialized state; callers reconcile first (58-I14).
    [[nodiscard]] SessionUiState*       viewedSession();
    [[nodiscard]] const SessionUiState* viewedSession() const;

    SessionUiState& ensureSession(const SessionId& id);
    SessionUiState& ensureSessionIn(const WorkspaceId& workspace, const SessionId& id);
    // 58-D7/A4: materialize a SessionUiState (subagent == true) and erase any
    // existing SessionCell for it; called before `track`.
    SessionUiState& ensureSubagentState(const WorkspaceId& workspace, const SessionId& id);
    // 58-D7/A4: materialize a SessionUiState WITHOUT a SessionCell (used by
    // `apply(SessionOpened)` so a SessionCreated notice cannot leak a cell).
    SessionUiState& ensureSessionState(const WorkspaceId& workspace, const SessionId& id);
    // 58-D4/A4: clear the per-session Esc arm of a child leaving the view.
    void            disarm(const SessionId& id);
    void            ensureCell(const SessionId& id);
    void            ensureCellIn(const WorkspaceId& workspace, const SessionId& id);
    void            refreshCell(const SessionId& id);
    void            refreshCellIn(const WorkspaceId& workspace, const SessionId& id);
    void            setCellTitle(const WorkspaceId& workspace, const SessionId& id,
                                 std::string title);
    void            setSessionReadOnly(const SessionId& id, bool read_only);
    void            eraseSession(const WorkspaceId& workspace, const SessionId& id);

    // 77-D3: if `workspace` is modeled, ensure the session cell/state and mark it
    // opening with `title`, then focus it. No-op for an unmodeled workspace (no
    // phantom workspace; SW25). Returns true iff the state was marked.
    bool beginOpening(const WorkspaceId& workspace, const SessionId& session,
                      std::string title);
    // 77-D3: clear `session`'s opening state. Returns true iff it was set.
    bool endOpening(const SessionId& session);
    // 77-D3: clear every opening state in `workspace` (link-state drop backstop).
    void endOpeningsIn(const WorkspaceId& workspace);
    // 77-D3/OL-X2: clear every opening state older than `kOpeningTimeout` as of
    // `now` and return the ids whose opening it cleared, so the caller can
    // reconcile the derived state at the same escape.
    [[nodiscard]] std::vector<SessionId> expireOpenings(
        std::chrono::steady_clock::time_point now);
    // 77-D3: true iff any SessionUiState has an `opening`.
    [[nodiscard]] bool hasAnyOpening() const;

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

    // 81-D2: opens the full-screen dashboard over the available Live
    // projection. Saves the outgoing mode into `dashboard.prev_mode`, then sets
    // `UiMode::Dashboard`. Pure model work; no registry/daemon access.
    void            openDashboard();
    // 81-D2: closes the dashboard, restoring `dashboard.prev_mode`.
    void            closeDashboard();
    void            focusWorkspace(const WorkspaceId& workspace);
    void            focusSession(const SessionId& id);
    // 45-D10.7: THE single mutator of WorkspaceModel's private focus. Models the
    // target via `ensureSessionIn` before assigning, so `active() != nullptr`
    // after any successful selection (45-I10). `activate_session` delegates here.
    void            focusSessionIn(const WorkspaceId& workspace, const SessionId& id);
    // 45-D10.2: reconciles the focused workspace's activeSessionId into
    // `sessions`. Returns the state, or nullptr when there is no workspace / no
    // active id.
    [[nodiscard]] SessionUiState* ensureActiveSession();
    // 45-D3: the Live switcher's membership authority — true iff `session` is in
    // the latest catalog snapshot for `workspace`.
    [[nodiscard]] bool catalog_has_session(const WorkspaceId& workspace,
                                           const SessionId& session) const;

    void apply(const UiEvent& event);
    void apply(const WorkspaceEvent& event);

    // RB-17: true iff any session holds a streaming Reasoning entry. The spinner
    // clock and the supervisor's repaint timer must tick only in this state.
    [[nodiscard]] bool has_streaming_reasoning() const;
    // RB-17 / 64-D1: advances the spinner clock to `now` while reasoning
    // streams; returns true iff the frame changed. The frame derives from the
    // exact elapsed time, so call count is irrelevant. With no streaming
    // reasoning the clock is reset and the frame is left untouched, so an idle
    // TUI never animates.
    bool advance_reasoning_spinner(std::chrono::steady_clock::time_point now);

    // 46-D9: the ACTIVE session's turn is running (Thinking || CallingTool).
    [[nodiscard]] bool has_active_turn() const;
    // 65-D1: the ACTIVE session's turn subtree is working: its own turn is
    // active OR it has a direct subagent still `Running`. Drives the bottom
    // indicator, the spinner clock and `animation_active_`. 65-D3 keeps
    // `has_active_turn()` for the Esc-Esc interrupt so a running child alone
    // never cancels the parent.
    [[nodiscard]] bool active_session_working() const;
    // 46-D9: the ACTIVE session has a streaming reasoning block.
    [[nodiscard]] bool active_has_streaming_reasoning() const;
    // 62-D2 (Rev 3): true iff `state` holds a streaming Reasoning entry. Shared
    // by the caret predicate (scoped to the rendered composer's session) and the
    // spinner so the two cannot drift.
    [[nodiscard]] static bool session_has_streaming_reasoning(const SessionUiState& state);
    // 46-D9 / 64-D1: advances the shared frame clock to `now` while a turn is
    // active OR the active session streams reasoning; returns true when the
    // frame changed. Drain frequency cannot change the pace (64-D1).
    bool advance_spinner(std::chrono::steady_clock::time_point now);

    // 25-D6: injects the TPS clock; defaults to `std::chrono::steady_clock::now`.
    void set_now_reader(ClockReader reader);

    ClockReader now_reader = [] { return std::chrono::steady_clock::now(); };
};

[[nodiscard]] bool is_active_state(AgentState state) noexcept;
[[nodiscard]] bool is_waiting_state(AgentState state) noexcept;

// 81-D5/81-D6: the dashboard's status/group/glyph/label derivation. `status` is
// total over `(SessionCell, SessionUiState*)`; the renderer and model share it
// so they cannot disagree (81-I6).
[[nodiscard]] DashboardStatus dashboard_status(const SessionCell& cell,
                                               const SessionUiState* state) noexcept;
[[nodiscard]] DashboardGroup dashboard_group(DashboardStatus status) noexcept;
[[nodiscard]] const char* dashboard_status_glyph(DashboardStatus status) noexcept;
[[nodiscard]] const char* dashboard_status_label(DashboardStatus status) noexcept;
// 81-D6: the dashboard row order within a group. The two workspace paths are
// supplied by the caller so the rule is evaluable from its arguments.
[[nodiscard]] bool dashboard_row_less(const DashboardRow& left,
                                      const DashboardRow& right,
                                      const std::string& cwd_path,
                                      const std::string& left_ws_path,
                                      const std::string& right_ws_path);

// 48-D6.2: the derived styling level. Never stored on ConversationEntry.
enum class Presentation : std::uint8_t {
    UserAuthored,   // bright: White + bold
    Intermediate,   // dim
    FinalAnswer,    // normal
    Chrome,         // dim (System/Context)
};

// 48-D6.2 (Rev 2): `turn_active` is folded into the classifier so a streaming
// tail assistant is Intermediate. `turn_active` = `is_active_state(state)` for
// the rendered session. Pure; O(n) in the worst case.
[[nodiscard]] Presentation entry_presentation(
    const std::vector<ConversationEntry>& entries, std::size_t index,
    bool turn_active) noexcept;

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

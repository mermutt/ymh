#include "ymh/ui/ui_render.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/pixel.hpp>
#include <ftxui/screen/screen.hpp>

#include "ymh/ui/render/diff_renderer.hpp"
#include "ymh/ui/render/markdown_renderer.hpp"
#include "ymh/ui/render/render_context.hpp"

namespace ymh::ui {
namespace {

using ftxui::Element;
using ftxui::Elements;

Element paint(Element element, ftxui::Color color, const Theme& theme) {
    if (!theme.color) {
        return element;
    }
    return element | ftxui::color(color);
}

Element paint_bg(Element element, ftxui::Color color, const Theme& theme) {
    if (!theme.color || !theme.user_block) {
        return element;
    }
    return element | ftxui::bgcolor(color);
}

// 17 §3 (RB-01): FTXUI has no per-line/left-only border primitive, so the user
// gutter is a custom Node that reserves two columns and draws `│` in the first
// column of every row (U-RB01-2).
class LeftBar final : public ftxui::Node {
public:
    explicit LeftBar(Element child) : ftxui::Node(Elements{std::move(child)}) {}

    void ComputeRequirement() override {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
        requirement_.min_x += 2;
        requirement_.focused.box.Shift(2, 0);
    }

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        box.x_min += 2;
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        children_[0]->Render(screen);
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            ftxui::Pixel& pixel = screen.PixelAt(box_.x_min, y);
            pixel.character = "│";
            pixel.automerge = true;
        }
    }
};

Element with_left_bar(Element element) {
    return std::make_shared<LeftBar>(std::move(element));
}

const char* state_glyph(AgentState state) {
    switch (state) {
        case AgentState::Idle:
            return "o";
        case AgentState::Thinking:
            return "*";
        case AgentState::CallingTool:
            return ">";
        case AgentState::WaitingForInput:
        case AgentState::WaitingForPermission:
            return "!";
        case AgentState::Cancelling:
            return "-";
        case AgentState::Error:
            return "x";
    }
    return "?";
}

const char* state_name(AgentState state) {
    switch (state) {
        case AgentState::Idle:
            return "idle";
        case AgentState::Thinking:
            return "thinking";
        case AgentState::CallingTool:
            return "calling tool";
        case AgentState::WaitingForInput:
            return "waiting for input";
        case AgentState::WaitingForPermission:
            return "waiting for permission";
        case AgentState::Cancelling:
            return "cancelling";
        case AgentState::Error:
            return "error";
    }
    return "unknown";
}

const char* daemon_status_glyph(DaemonStatus status) {
    switch (status) {
        case DaemonStatus::Connecting:
            return "~";
        case DaemonStatus::Attached:
            return "o";
        case DaemonStatus::Detached:
            return "-";
        case DaemonStatus::Dead:
            return "x";
        case DaemonStatus::Stopping:
            return "s";
        case DaemonStatus::NotRunning:
            return ".";
    }
    return "?";
}

const char* ownership_mark_name(OwnershipMark mark) {
    switch (mark) {
        case OwnershipMark::Owned:
            return "owned";
        case OwnershipMark::NotRunning:
            return "not running";
        case OwnershipMark::Stopping:
            return "stopping";
        case OwnershipMark::Unreachable:
            return "unreachable";
    }
    return "unknown";
}

std::string short_id(const SessionId& id) {
    return id.value.size() > 8 ? id.value.substr(0, 8) : id.value;
}

bool looks_like_diff(const std::string& text) {
    return text.rfind("diff --git", 0) == 0 ||
           text.find("\ndiff --git") != std::string::npos ||
           text.rfind("@@ ", 0) == 0 || text.find("\n@@ ") != std::string::npos;
}

Element render_tool_entry(const ConversationEntry& entry, const ToolModel* tools,
                          bool expand_all_folds, const RenderContext& context) {
    const Theme& theme = context.theme;
    const ToolCallView* call = nullptr;
    if (tools != nullptr) {
        const std::size_t index = tools->find(entry.tool_call_id);
        if (index != kNoEntry) {
            call = &tools->calls[index];
        }
    }
    const bool expanded = expand_all_folds || (call != nullptr && call->expanded);
    Elements rows;
    std::string header = "tool: " + entry.tool_name;
    if (expanded) {
        header += " (expanded)";
    }
    rows.push_back(paint(ftxui::text(header), ftxui::Color::Yellow, theme));
    if (!expanded) {
        return ftxui::vbox(std::move(rows));
    }
    if (call != nullptr && !call->arguments.empty()) {
        rows.push_back(ftxui::text("args: " + call->arguments) | ftxui::dim);
    }
    const std::string& text =
        call != nullptr && !call->output.empty() ? call->output : entry.text;
    if (!text.empty()) {
        if (looks_like_diff(text)) {
            rows.push_back(DiffRenderer{}.render(DiffModel::parse(text), context));
        } else {
            rows.push_back(ftxui::paragraph(text));
        }
    }
    return ftxui::vbox(std::move(rows));
}

Element render_reasoning_entry(const ConversationEntry& entry, bool expand_all_folds,
                               const RenderContext& context) {
    const Theme& theme = context.theme;
    std::string header = "reasoning";
    if (expand_all_folds) {
        header += entry.streaming ? " (expanded · streaming)" : " (expanded)";
    } else {
        header += entry.streaming ? " (streaming · Ctrl+O to expand)"
                                  : " (Ctrl+O to expand)";
    }
    Elements rows;
    rows.push_back(paint(ftxui::text(header), ftxui::Color::Magenta, theme));
    if (expand_all_folds) {
        const MarkdownRenderer markdown;
        rows.push_back(markdown.render(MarkdownBlock{entry.text}, context));
    }
    return ftxui::vbox(std::move(rows));
}

Element render_entry(const ConversationEntry& entry, const ToolModel* tools,
                     bool expand_all_folds, const RenderContext& context) {
    const Theme& theme = context.theme;
    const MarkdownRenderer markdown;
    Elements rows;
    switch (entry.role) {
        case ConversationRole::User: {
            Element body =
                with_left_bar(markdown.render(MarkdownBlock{entry.text}, context));
            rows.push_back(
                paint_bg(std::move(body), ftxui::Color::RGB(40, 42, 54), theme));
            break;
        }
        case ConversationRole::Assistant:
            rows.push_back(markdown.render(MarkdownBlock{entry.text}, context));
            break;
        case ConversationRole::Reasoning:
            return render_reasoning_entry(entry, expand_all_folds, context);
        case ConversationRole::Tool:
            return render_tool_entry(entry, tools, expand_all_folds, context);
        case ConversationRole::System:
            rows.push_back(ftxui::paragraph(entry.text) | ftxui::dim);
            break;
    }
    return ftxui::vbox(std::move(rows));
}

Element render_conversation(const SessionUiState* active, const RenderContext& context) {
    Elements rows;
    if (active == nullptr) {
        rows.push_back(ftxui::text("(no active session)") | ftxui::dim);
        return ftxui::vbox(std::move(rows));
    }
    for (const ConversationEntry& entry : active->conversation.entries) {
        rows.push_back(
            render_entry(entry, &active->tools, active->expand_all_folds, context));
    }
    if (rows.empty()) {
        rows.push_back(ftxui::text("Type a message and press Enter. Ctrl+D or /exit quits.") |
                       ftxui::dim);
    }
    return ftxui::vbox(std::move(rows)) |
           ftxui::focusPositionRelative(0.f, active->scroll.position()) | ftxui::yframe |
           ftxui::vscroll_indicator;
}

Element render_scroll_hint(const SessionUiState* active, const Theme& theme) {
    if (active == nullptr || active->scroll.following) {
        return ftxui::text("");
    }
    std::string label = active->scroll.unseen ? "new output below (Ctrl+End to follow)"
                                              : "scrolled (Ctrl+End to follow)";
    return paint(ftxui::text(label), ftxui::Color::Yellow, theme);
}

Element render_subagents(const SessionUiState* active, const Theme& theme) {
    if (active == nullptr || active->subagents.agents.empty()) {
        return ftxui::text("");
    }
    Elements cells;
    cells.push_back(paint(ftxui::text("subagents:"), ftxui::Color::Magenta, theme));
    for (const SubagentView& agent : active->subagents.agents) {
        std::string label = " [" + short_id(agent.id) + " " + state_glyph(agent.state) + "]";
        if (!agent.summary.empty()) {
            std::string summary = agent.summary;
            if (summary.size() > 32) {
                summary.resize(32);
                summary += "...";
            }
            label += " " + summary;
        }
        cells.push_back(ftxui::text(label));
    }
    return ftxui::hbox(std::move(cells));
}

Element render_command_hints(const SessionUiState* active, const Theme& theme) {
    if (active == nullptr || active->command_hints.empty()) {
        return ftxui::text("");
    }
    Elements rows;
    for (const CommandHint& hint : active->command_hints) {
        rows.push_back(ftxui::hbox({
            paint(ftxui::text("/" + hint.name), ftxui::Color::Green, theme) | ftxui::bold,
            ftxui::text("  " + hint.description) | ftxui::dim,
        }));
    }
    return ftxui::vbox(std::move(rows));
}

Element render_input(const UiModel& model, const Theme& theme) {
    const SessionUiState* active = nullptr;
    const auto workspace = model.workspaces.find(model.activeWorkspaceId);
    if (workspace != model.workspaces.end()) {
        const auto session = model.sessions.find(workspace->second.activeSessionId);
        if (session != model.sessions.end()) {
            active = &session->second;
        }
    }
    std::string draft = active == nullptr ? std::string{} : active->input.draft;
    return ftxui::hbox({
        paint(ftxui::text("> "), ftxui::Color::Green, theme) | ftxui::bold,
        ftxui::text(draft),
        ftxui::text("_"),
    });
}

Element render_status(const UiModel& model, const SessionUiState* active, const Theme& theme) {
    const AggregateStatus& counts = model.aggregate.current;
    std::string state = active == nullptr ? "idle" : state_name(active->agent_state);
    std::string left = state;
    if (active != nullptr && !active->status.model.empty()) {
        left += " · " + active->status.model;
    }
    if (active != nullptr) {
        const StatusModel& status = active->status;
        if (status.input_tokens != 0 || status.output_tokens != 0 || status.cached_tokens != 0) {
            left += " · ↑" + std::to_string(status.input_tokens) + " ↓" +
                    std::to_string(status.output_tokens) + " ⚡" +
                    std::to_string(status.cached_tokens);
        }
    }
    if (active != nullptr && !active->status.note.empty()) {
        left += " · " + active->status.note;
    }
    std::string right = std::to_string(counts.activeCount) + " active · " +
                        std::to_string(counts.waitingCount) + " waiting";
    Element aggregate = ftxui::text(right);
    if (theme.color && model.aggregate.flash.isFlashing()) {
        aggregate = aggregate | ftxui::inverted;
    }
    return ftxui::hbox({
        ftxui::text(left),
        ftxui::filler(),
        aggregate,
    });
}

Element render_dialog(const UiModel& model, const Theme& theme) {
    const PermissionDialogModel& dialog = model.dialog;
    Elements rows;
    rows.push_back(ftxui::text("Permission required") | ftxui::bold);
    rows.push_back(ftxui::separator());
    rows.push_back(ftxui::text("tool: " + dialog.tool));
    if (!dialog.summary.empty()) {
        rows.push_back(ftxui::paragraph(dialog.summary) | ftxui::dim);
    }
    rows.push_back(ftxui::separator());
    const char* options[] = {
        "1) Allow once",
        "2) Allow for session",
        "3) Always allow",
        "0) Deny",
    };
    for (int index = 0; index < 4; ++index) {
        Element row = ftxui::text(options[index]);
        if (index == dialog.selected) {
            row = row | ftxui::inverted;
        }
        rows.push_back(row);
    }
    rows.push_back(ftxui::text("y allow · n deny · Esc cancel") | ftxui::dim);
    (void)theme;
    return ftxui::window(ftxui::text("permission"), ftxui::vbox(std::move(rows))) | ftxui::center;
}

Element render_exit_confirm(const UiModel& model, const Theme& theme) {
    const ExitConfirmState& confirm = model.exitConfirm;
    Elements rows;
    const std::size_t count = confirm.orphaning.size();
    rows.push_back(ftxui::text("Exiting will terminate " + std::to_string(count) +
                               (count == 1 ? " workspace daemon:" : " workspace daemons:")));
    for (const WorkspaceId& workspace : confirm.orphaning) {
        std::string title = workspace.value;
        const auto it = model.workspaces.find(workspace);
        if (it != model.workspaces.end() && !it->second.title.empty()) {
            title = it->second.title;
        }
        rows.push_back(ftxui::text("  • " + title));
    }
    rows.push_back(ftxui::text(std::to_string(confirm.sessions) + " session" +
                               (confirm.sessions == 1 ? "" : "s") + " · " +
                               std::to_string(confirm.running) + " running") |
                   ftxui::dim);
    rows.push_back(ftxui::separator());
    rows.push_back(ftxui::paragraph("In-flight turns will be cancelled. Sessions are "
                                    "saved; the daemons restart automatically the next "
                                    "time you use them.") |
                   ftxui::dim);
    rows.push_back(ftxui::separator());
    const char* options[] = {"[ Terminate and exit ]", "[ Cancel ]"};
    for (int index = 0; index < 2; ++index) {
        Element row = ftxui::text(options[index]);
        if (index == confirm.selected) {
            row = row | ftxui::inverted;
        }
        rows.push_back(row);
    }
    rows.push_back(ftxui::text("y terminate · n cancel · Esc cancel") | ftxui::dim);
    (void)theme;
    return ftxui::window(ftxui::text("Exiting"), ftxui::vbox(std::move(rows))) | ftxui::center;
}

Element render_switcher(const UiModel& model, const Theme& theme) {
    const SwitcherOverlayModel& switcher = model.switcher;
    Elements rows;
    rows.push_back(ftxui::text("Switcher") | ftxui::bold);
    rows.push_back(ftxui::separator());
    if (switcher.workspaces.empty()) {
        rows.push_back(ftxui::text("(no workspaces)") | ftxui::dim);
    }
    for (const WorkspaceNode& workspace : switcher.workspaces) {
        const bool on_workspace = switcher.cursor.workspace == workspace.id &&
                                  !switcher.cursor.session.has_value();
        const bool collapsed = switcher.collapsed.find(workspace.id) != switcher.collapsed.end();
        const std::string title =
            workspace.title.empty() ? workspace.id.value : workspace.title;
        Element row =
            ftxui::text(std::string(collapsed ? "+ " : "- ") + title + "  " +
                        daemon_status_glyph(workspace.status) + " [" +
                        ownership_mark_name(workspace.mark) + "]");
        if (on_workspace) {
            row = paint(row, ftxui::Color::Cyan, theme) | ftxui::bold;
        }
        rows.push_back(row);
        if (collapsed) {
            continue;
        }
        for (const SessionNode& session : workspace.sessions) {
            const bool on_session = switcher.cursor.workspace == workspace.id &&
                                    switcher.cursor.session.has_value() &&
                                    *switcher.cursor.session == session.id;
            const std::string leaf_title =
                session.title.empty() ? short_id(session.id) : session.title;
            std::string leaf = "    [" + leaf_title + " " + state_glyph(session.state);
            if (session.attention) {
                leaf += "!";
            }
            leaf += "]";
            Element leaf_element = ftxui::text(leaf);
            if (on_session) {
                leaf_element = leaf_element | ftxui::inverted;
            }
            if (session.attention) {
                leaf_element = paint(leaf_element, ftxui::Color::Red, theme);
            }
            rows.push_back(leaf_element);
        }
    }
    rows.push_back(ftxui::separator());
    rows.push_back(ftxui::text("j/k move · Tab expand · Enter focus · Esc close") | ftxui::dim);
    return ftxui::window(ftxui::text("workspaces"), ftxui::vbox(std::move(rows))) |
           ftxui::center;
}

Element render_header(const UiModel& model, const Theme& theme) {
    const auto workspace = model.workspaces.find(model.activeWorkspaceId);
    std::string title = "ymh";
    if (workspace != model.workspaces.end() && !workspace->second.cwd.empty()) {
        title += " · " + workspace->second.cwd;
    }
    std::string session_title;
    if (workspace != model.workspaces.end()) {
        const SessionId& active_id = workspace->second.activeSessionId;
        if (!active_id.value.empty()) {
            for (const SessionCell& cell : workspace->second.sessions) {
                if (cell.id == active_id && !cell.title.empty()) {
                    session_title = cell.title;
                    break;
                }
            }
            if (session_title.empty()) {
                session_title = short_id(active_id);
            }
        }
    }
    return ftxui::hbox({paint(ftxui::text(title), ftxui::Color::Cyan, theme) | ftxui::bold,
                        ftxui::filler(), ftxui::text(session_title)});
}

// 18 §5: the `/context` overlay. Chrome is exactly 10 rows in every view and
// every size (border 2 + title/status 1 + body 6 + bottom line 1).
constexpr int kContextGridMaxCols = 72;
constexpr int kContextGridMaxRows = 12;
constexpr int kContextGridMinCols = 40;
constexpr int kContextGridMinRows = 10;
constexpr int kContextOverlayChromeRows = 10;
constexpr int kContextOverlayBorderCols = 2;
constexpr int kContextInventoryRows = 6;
constexpr std::string_view kContextFallbackHint =
    "terminal too small for the grid (need >= 40x10)";

constexpr std::array<ContextSegmentKind, 6> kContextSegmentOrder = {
    ContextSegmentKind::SystemPrompt,
    ContextSegmentKind::ToolSchemas,
    ContextSegmentKind::McpToolSchemas,
    ContextSegmentKind::Conversation,
    ContextSegmentKind::CompactionSummary,
    ContextSegmentKind::FreeSpace,
};

ftxui::Color context_segment_color(ContextSegmentKind kind) {
    switch (kind) {
        case ContextSegmentKind::SystemPrompt:
            return ftxui::Color::Blue;
        case ContextSegmentKind::ToolSchemas:
            return ftxui::Color::Green;
        case ContextSegmentKind::McpToolSchemas:
            return ftxui::Color::Magenta;
        case ContextSegmentKind::Conversation:
            return ftxui::Color::Default;
        case ContextSegmentKind::CompactionSummary:
            return ftxui::Color::Cyan;
        case ContextSegmentKind::FreeSpace:
            return ftxui::Color::GrayDark;
    }
    return ftxui::Color::Default;
}

char context_segment_glyph(ContextSegmentKind kind) {
    switch (kind) {
        case ContextSegmentKind::SystemPrompt:
            return 'S';
        case ContextSegmentKind::ToolSchemas:
            return 'T';
        case ContextSegmentKind::McpToolSchemas:
            return 'M';
        case ContextSegmentKind::Conversation:
            return 'C';
        case ContextSegmentKind::CompactionSummary:
            return '~';
        case ContextSegmentKind::FreeSpace:
            return '.';
    }
    return '?';
}

std::string_view context_segment_label(ContextSegmentKind kind) {
    switch (kind) {
        case ContextSegmentKind::SystemPrompt:
            return "system prompt";
        case ContextSegmentKind::ToolSchemas:
            return "tool schemas";
        case ContextSegmentKind::McpToolSchemas:
            return "mcp tool schemas";
        case ContextSegmentKind::Conversation:
            return "conversation";
        case ContextSegmentKind::CompactionSummary:
            return "compaction summary";
        case ContextSegmentKind::FreeSpace:
            return "free";
    }
    return "";
}

std::string format_thousands(std::uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string       out;
    out.reserve(digits.size() + digits.size() / 3);
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index > 0 && (digits.size() - index) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[index]);
    }
    return out;
}

std::string pad_left(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return std::string(width - text.size(), ' ') + text;
}

std::string pad_right(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
}

// `tokens * 100 / window`, truncated (floor) to one decimal (18 §5.3 C5).
std::string format_percent(std::uint64_t tokens, std::uint64_t window) {
    if (window == 0) {
        return "—";
    }
    const std::uint64_t tenths = tokens * 1000 / window;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}

std::string context_title(const ContextSnapshot& snapshot) {
    std::string title;
    if (snapshot.budget.window_tokens > 0) {
        title = "used " + format_thousands(snapshot.used_tokens) + " / window " +
                format_thousands(snapshot.budget.window_tokens) + " (" +
                format_percent(snapshot.used_tokens, snapshot.budget.window_tokens) + ")";
    } else {
        title = "used tokens (budget unknown)";
    }
    title += "  @" + std::to_string(snapshot.captured_sequence);
    return title;
}

std::string context_totals(const ContextSnapshot& snapshot) {
    if (snapshot.budget.window_tokens == 0) {
        return "used " + format_thousands(snapshot.used_tokens) +
               " tokens (budget unknown)  threshold —  reserve —";
    }
    return "used " + format_thousands(snapshot.used_tokens) + " / " +
           format_thousands(snapshot.budget.window_tokens) + " (" +
           format_percent(snapshot.used_tokens, snapshot.budget.window_tokens) +
           ")  threshold " + format_thousands(snapshot.budget.effective_threshold_tokens) +
           "  reserve " + format_thousands(snapshot.budget.reserve_output_tokens);
}

Element render_context_grid(const ContextSnapshot& snapshot, int cols, int rows,
                            const Theme& theme) {
    const int cells_total = cols * rows;
    Elements  lines;
    lines.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        Elements cells;
        cells.reserve(static_cast<std::size_t>(cols));
        for (int col = 0; col < cols; ++col) {
            const ContextSegmentKind kind =
                context_cell_kind(snapshot, cells_total, row * cols + col);
            cells.push_back(paint(ftxui::text(std::string(1, context_segment_glyph(kind))),
                                  context_segment_color(kind), theme));
        }
        lines.push_back(ftxui::hbox(std::move(cells)));
    }
    return ftxui::vbox(std::move(lines));
}

Element render_context_legend(const ContextSnapshot& snapshot, const Theme& theme) {
    const std::uint64_t window = snapshot.budget.window_tokens;
    Elements            rows;
    rows.reserve(kContextSegmentOrder.size());
    for (const ContextSegmentKind kind : kContextSegmentOrder) {
        const ContextSegment* segment = nullptr;
        for (const ContextSegment& candidate : snapshot.segments) {
            if (candidate.kind == kind) {
                segment = &candidate;
                break;
            }
        }
        const std::uint64_t tokens = segment != nullptr ? segment->tokens : 0;
        const std::string   token_text = kind == ContextSegmentKind::FreeSpace && window == 0
                                             ? "—"
                                             : format_thousands(tokens);
        const std::string   pct = window == 0 ? "—" : format_percent(tokens, window);
        const std::string   rest = "  " + pad_right(std::string(context_segment_label(kind)), 20) +
                                   pad_left(token_text, 7) + " tok " + pad_left(pct, 6);
        rows.push_back(ftxui::hbox(
            {paint(ftxui::text(std::string(1, context_segment_glyph(kind))),
                   context_segment_color(kind), theme),
             ftxui::text(rest)}));
    }
    return ftxui::vbox(std::move(rows));
}

Element render_context_inventory(const ContextSnapshot& snapshot, int scroll,
                                 const Theme& theme) {
    (void)theme;
    std::vector<std::string> lines;
    lines.push_back("servers");
    for (const ContextServerEntry& server : snapshot.mcp_servers) {
        std::string line = server.id + "  " + server.state + "  tools=" +
                           std::to_string(server.tool_count);
        if (server.skipped > 0) {
            line += "  skipped=" + std::to_string(server.skipped);
        }
        if (server.has_error) {
            line += "  !";
        }
        lines.push_back(std::move(line));
    }
    lines.push_back("tools");
    for (const ContextToolEntry& tool : snapshot.tools) {
        lines.push_back(tool.name + "  " + tool.provenance + "  ~" +
                        std::to_string(tool.schema_tokens) + " tok");
    }

    const int max_scroll =
        std::max<int>(0, static_cast<int>(lines.size()) - kContextInventoryRows);
    const int first = std::clamp(scroll, 0, max_scroll);
    Elements  rows;
    rows.reserve(kContextInventoryRows);
    for (int index = 0; index < kContextInventoryRows; ++index) {
        const int line_index = first + index;
        if (line_index >= 0 && line_index < static_cast<int>(lines.size())) {
            rows.push_back(ftxui::text(lines[static_cast<std::size_t>(line_index)]));
        } else {
            rows.push_back(ftxui::text(" "));
        }
    }
    return ftxui::vbox(std::move(rows));
}

Element render_context_overlay(const UiModel& model, TerminalSize size, const Theme& theme) {
    const ContextOverlayModel& overlay = model.context;
    const ContextSnapshot&     snapshot = overlay.snapshot;
    const ContextGridGeometry  geometry = context_grid_geometry(size.width, size.height);
    const std::uint64_t        grid_total = snapshot.budget.window_tokens > 0
                                                ? snapshot.budget.window_tokens
                                                : snapshot.used_tokens;
    const bool grid_ok = geometry.cols >= kContextGridMinCols &&
                         geometry.rows >= kContextGridMinRows && geometry.cols > 0 &&
                         geometry.rows > 0 && grid_total > 0;

    Elements content;
    content.push_back(ftxui::text(context_title(snapshot)));
    if (grid_ok) {
        content.push_back(render_context_grid(snapshot, geometry.cols, geometry.rows, theme));
    }
    if (overlay.view == 1) {
        content.push_back(render_context_inventory(snapshot, overlay.scroll, theme));
    } else {
        content.push_back(render_context_legend(snapshot, theme));
    }
    if (!overlay.note.empty()) {
        content.push_back(ftxui::text(overlay.note));
    } else if (grid_ok) {
        content.push_back(ftxui::text(context_totals(snapshot)));
    } else {
        content.push_back(ftxui::text(std::string(kContextFallbackHint)));
    }
    return ftxui::window(ftxui::text(""), ftxui::vbox(std::move(content))) |
           ftxui::clear_under;
}

} // namespace

ContextGridGeometry context_grid_geometry(int width, int height) noexcept {
    ContextGridGeometry geometry;
    const int           inner_width = std::max(width - kContextOverlayBorderCols, 0);
    geometry.cols = std::clamp(inner_width, 0, kContextGridMaxCols);
    geometry.rows = std::clamp(height - kContextOverlayChromeRows, 0, kContextGridMaxRows);
    return geometry;
}

ContextSegmentKind context_cell_kind(const ContextSnapshot& snapshot, int cells_total,
                                     int cell_index) noexcept {
    if (cells_total <= 0 || cell_index < 0 || cell_index >= cells_total) {
        return ContextSegmentKind::FreeSpace;
    }
    const std::uint64_t grid_total = snapshot.budget.window_tokens > 0
                                         ? snapshot.budget.window_tokens
                                         : snapshot.used_tokens;
    if (grid_total == 0) {
        return ContextSegmentKind::FreeSpace;
    }
    const std::uint64_t lo = static_cast<std::uint64_t>(cell_index) * grid_total /
                             static_cast<std::uint64_t>(cells_total);
    std::uint64_t running = 0;
    for (const ContextSegment& segment : snapshot.segments) {
        if (lo >= running && lo < running + segment.tokens) {
            return segment.kind;
        }
        running += segment.tokens;
    }
    return ContextSegmentKind::FreeSpace;
}

LayoutMode calculate_layout(int width) {
    if (width < 90) {
        return LayoutMode::Narrow;
    }
    if (width <= 130) {
        return LayoutMode::Normal;
    }
    return LayoutMode::Wide;
}

Element build_ui(const UiModel& model, TerminalSize size, const Theme& theme) {
    const SessionUiState* active = nullptr;
    const auto workspace = model.workspaces.find(model.activeWorkspaceId);
    if (workspace != model.workspaces.end()) {
        const auto session = model.sessions.find(workspace->second.activeSessionId);
        if (session != model.sessions.end()) {
            active = &session->second;
        }
    }

    Elements rows;
    rows.push_back(render_header(model, theme));
    rows.push_back(ftxui::separator());
    rows.push_back(render_conversation(active, RenderContext{size.width, theme, false}) |
                   ftxui::flex);
    if (active != nullptr && !active->scroll.following) {
        rows.push_back(render_scroll_hint(active, theme));
    }
    rows.push_back(ftxui::separator());
    if (active != nullptr && !active->subagents.agents.empty()) {
        rows.push_back(render_subagents(active, theme));
    }
    if (active != nullptr && !active->command_hints.empty()) {
        rows.push_back(render_command_hints(active, theme));
    }
    rows.push_back(render_input(model, theme));
    rows.push_back(render_status(model, active, theme));

    Element main = ftxui::vbox(std::move(rows)) | ftxui::border;
    if (model.exitConfirm.open) {
        return ftxui::dbox({main, render_exit_confirm(model, theme)});
    }
    if (model.dialog.open) {
        return ftxui::dbox({main, render_dialog(model, theme)});
    }
    if (model.mode == UiMode::Context && model.context.open) {
        return ftxui::dbox({main, render_context_overlay(model, size, theme)});
    }
    if (model.mode == UiMode::Switcher) {
        return ftxui::dbox({main, render_switcher(model, theme)});
    }
    return main;
}

std::string render_to_ansi(const UiModel& model, TerminalSize size, const Theme& theme) {
    Element element = build_ui(model, size, theme);
    ftxui::Screen screen =
        ftxui::Screen::Create(ftxui::Dimensions{size.width, size.height});
    ftxui::Render(screen, element);
    return screen.ToString();
}

} // namespace ymh::ui

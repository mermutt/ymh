#include "ymh/ui/ui_render.hpp"

#include <memory>
#include <string>
#include <utility>

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
    }
    return "?";
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
        Element row = ftxui::text(std::string(collapsed ? "+ " : "- ") + title + "  " +
                                  daemon_status_glyph(workspace.status));
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

} // namespace

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
    if (model.dialog.open) {
        return ftxui::dbox({main, render_dialog(model, theme)});
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

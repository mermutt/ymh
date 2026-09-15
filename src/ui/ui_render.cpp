#include "ymh/ui/ui_render.hpp"

#include <string>
#include <utility>

#include <ftxui/dom/elements.hpp>
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

Element render_entry(const ConversationEntry& entry, const RenderContext& context) {
    const Theme& theme = context.theme;
    const MarkdownRenderer markdown;
    Elements rows;
    switch (entry.role) {
        case ConversationRole::User:
            rows.push_back(paint(ftxui::text("you:"), ftxui::Color::Cyan, theme) | ftxui::bold);
            rows.push_back(markdown.render(MarkdownBlock{entry.text}, context));
            break;
        case ConversationRole::Assistant:
            rows.push_back(paint(ftxui::text(entry.streaming ? "assistant (streaming)" : "assistant"),
                                 ftxui::Color::Green, theme) |
                           ftxui::bold);
            rows.push_back(markdown.render(MarkdownBlock{entry.text}, context));
            break;
        case ConversationRole::Tool:
            rows.push_back(paint(ftxui::text("tool: " + entry.tool_name),
                                 ftxui::Color::Yellow, theme));
            if (!entry.text.empty()) {
                if (looks_like_diff(entry.text)) {
                    rows.push_back(DiffRenderer{}.render(DiffModel::parse(entry.text), context));
                } else {
                    rows.push_back(ftxui::paragraph(entry.text) | ftxui::dim);
                }
            }
            break;
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
        rows.push_back(render_entry(entry, context));
    }
    if (rows.empty()) {
        rows.push_back(ftxui::text("Type a message and press Enter. Ctrl+D or /exit quits.") |
                       ftxui::dim);
    }
    return ftxui::vbox(std::move(rows));
}

Element render_session_bar(const UiModel& model, const Theme& theme) {
    const auto workspace = model.workspaces.find(model.activeWorkspaceId);
    if (workspace == model.workspaces.end()) {
        return ftxui::text("");
    }
    Elements cells;
    for (const SessionCell& cell : workspace->second.sessions) {
        const std::string title = cell.title.empty() ? short_id(cell.id) : cell.title;
        std::string label = "[" + title + " " + state_glyph(cell.state);
        if (cell.attention) {
            label += "!";
        }
        label += "]";
        Element element = ftxui::text(label);
        if (cell.id == workspace->second.activeSessionId) {
            element = paint(element, ftxui::Color::Cyan, theme) | ftxui::bold;
        }
        if (cell.attention) {
            element = paint(element, ftxui::Color::Red, theme);
        }
        cells.push_back(element);
        cells.push_back(ftxui::text(" "));
    }
    return ftxui::hbox(std::move(cells));
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
    return ftxui::hbox({paint(ftxui::text(title), ftxui::Color::Cyan, theme) | ftxui::bold,
                        ftxui::filler()});
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
    rows.push_back(ftxui::separator());
    rows.push_back(render_session_bar(model, theme));
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

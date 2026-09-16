#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"golden-session"};

std::string strip_ansi(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() &&
                   (input[index] == ';' || (input[index] >= '0' && input[index] <= '9'))) {
                ++index;
            }
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

std::string normalize(const std::string& input) {
    std::vector<std::string> lines;
    std::string current;
    for (const char character : strip_ansi(input)) {
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            while (!current.empty() && current.back() == ' ') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    while (!current.empty() && current.back() == ' ') {
        current.pop_back();
    }
    lines.push_back(current);
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            result += '\n';
        }
        result += lines[index];
    }
    return result;
}

Event make_event(EventType type, const nlohmann::json& payload) {
    Event event;
    event.id = EventId{"event"};
    event.session_id = kSession;
    event.timestamp = std::chrono::system_clock::now();
    event.type = type;
    event.payload = payload;
    return event;
}

template <class P>
Event typed_event(EventType type, const P& payload) {
    return make_event(type, nlohmann::json(payload));
}

UiModel build_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/work";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.activeSessionId = kSession;
    model.workspaces.emplace(workspace.id, workspace);
    model.ensureSession(kSession);
    model.session(kSession)->status.model = "test-model";

    UiEventAdapter adapter(model);
    adapter.onEvent(typed_event(EventType::UserMessage, [] {
        payload::UserMessage message;
        message.id = "m1";
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = "hello there";
        message.content.push_back(block);
        return message;
    }()));
    payload::AssistantChunk chunk;
    chunk.message = "a1";
    chunk.text = "Hello world";
    adapter.onEvent(typed_event(EventType::AssistantChunk, chunk));
    adapter.onEvent(typed_event(EventType::AssistantMessage, [] {
        payload::AssistantMessage message;
        message.id = "a1";
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = "Hello world";
        message.content.push_back(block);
        Usage usage;
        usage.input_tokens = 12;
        usage.output_tokens = 3;
        message.usage = usage;
        return message;
    }()));
    adapter.onEvent(typed_event(EventType::ToolCall, [] {
        payload::ToolCall call;
        call.id = "t1";
        call.name = "read_file";
        call.arguments = {{"path", "hello.txt"}};
        return call;
    }()));
    adapter.onEvent(typed_event(EventType::ToolResult, [] {
        payload::ToolResult result;
        result.id = "t1";
        result.name = "read_file";
        result.outcome = payload::ToolOutcome::Ok;
        result.output = "file-body";
        return result;
    }()));
    adapter.onEvent(typed_event(EventType::TurnEnded, payload::TurnEnded{}));
    return model;
}

const char* kGolden = R"GOLDEN(╭──────────────────────────────────────────────────────────────────────╮
│ymh · /work                                                   golden-s│
├┬─────────────────────────────────────────────────────────────────────┤
││ hello there                                                         │
│Hello world                                                           │
│tool: read_file                                                       │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
├──────────────────────────────────────────────────────────────────────┤
│> _                                                                   │
│idle · test-model · ↑12 ↓3 ⚡0                    0 active · 0 waiting│
╰──────────────────────────────────────────────────────────────────────╯)GOLDEN";

TEST(UiRenderGolden, ConversationSnapshot) {
    const UiModel model = build_model();
    const std::string rendered = normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    std::cerr << "\n---GOLDEN-BEGIN---\n" << rendered << "\n---GOLDEN-END---\n";
    SCOPED_TRACE(rendered);
    EXPECT_EQ(rendered, std::string(kGolden));
}

TEST(UiRenderGolden, RenderIsPure) {
    const UiModel model = build_model();
    const std::string first = render_to_ansi(model, TerminalSize{72, 20}, Theme{false});
    const std::string second = render_to_ansi(model, TerminalSize{72, 20}, Theme{false});
    EXPECT_EQ(first, second);
}

TEST(UiRenderGolden, ContainsConversationAndToolLines) {
    const UiModel model = build_model();
    const std::string rendered = render_to_ansi(model, TerminalSize{72, 20}, Theme{false});
    EXPECT_NE(rendered.find("hello there"), std::string::npos);
    EXPECT_NE(rendered.find("Hello world"), std::string::npos);
    EXPECT_NE(rendered.find("read_file"), std::string::npos);
    EXPECT_NE(rendered.find("0 active"), std::string::npos);
}

TEST(UiRenderGolden, PermissionDialogOverlay) {
    UiModel model = build_model();
    UiEventAdapter adapter(model);
    PermissionRequest request;
    request.session = kSession;
    request.tool = "shell";
    request.arguments = {{"command", "rm -rf build"}};
    adapter.onPermissionRequest(kSession, PermissionRequestId{"p1"}, request);
    const std::string rendered = render_to_ansi(model, TerminalSize{72, 24}, Theme{false});
    EXPECT_NE(rendered.find("Permission required"), std::string::npos);
    EXPECT_NE(rendered.find("shell"), std::string::npos);
}

TEST(UiRenderGolden, LayoutModes) {
    EXPECT_EQ(calculate_layout(60), LayoutMode::Narrow);
    EXPECT_EQ(calculate_layout(90), LayoutMode::Normal);
    EXPECT_EQ(calculate_layout(130), LayoutMode::Normal);
    EXPECT_EQ(calculate_layout(160), LayoutMode::Wide);
}

TEST(UiRenderGolden, MultiWorkspaceSwitcherTree) {
    UiModel model = build_model();
    model.workspaces[model.activeWorkspaceId].title = "alpha";
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-beta"};
    beta.title = "beta";
    beta.cwd = "/work/beta";
    beta.daemonStatus = DaemonStatus::Dead;
    beta.activeSessionId = SessionId{"beta-session"};
    SessionCell beta_cell;
    beta_cell.id = SessionId{"beta-session"};
    beta_cell.title = "notes";
    beta_cell.state = AgentState::WaitingForInput;
    beta_cell.attention = true;
    beta.sessions.push_back(beta_cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-beta"}, SessionId{"beta-session"});
    model.openSwitcher();

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{80, 24}, Theme{false}));
    EXPECT_NE(rendered.find("Switcher"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("alpha"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("beta"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("notes"), std::string::npos) << rendered;

    const std::string again =
        normalize(render_to_ansi(model, TerminalSize{80, 24}, Theme{false}));
    EXPECT_EQ(rendered, again);
}

TEST(UiRenderGolden, RenderIsPureAcrossCalls) {
    UiModel model = build_model();
    const std::string first = render_to_ansi(model, TerminalSize{100, 40}, Theme{false});
    const std::string second = render_to_ansi(model, TerminalSize{100, 40}, Theme{false});
    EXPECT_EQ(first, second);
}

void append_lines(UiModel& model, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        model.apply(UiEvent{UserMessage{kSession, "u" + std::to_string(index),
                                        "line-" + std::to_string(index)}});
    }
}

TEST(UiRenderGolden, ScrolledConversationClipsAndHints) {
    UiModel model = build_model();
    append_lines(model, 40);
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->scroll.toTop();

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("line-0"), std::string::npos);
    EXPECT_EQ(rendered.find("line-39"), std::string::npos);
    EXPECT_NE(rendered.find("Ctrl+End"), std::string::npos);
}

TEST(UiRenderGolden, NewOutputHintWhileScrolled) {
    UiModel model = build_model();
    append_lines(model, 40);
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->scroll.toTop();
    model.apply(UiEvent{UserMessage{kSession, "fresh", "fresh-line"}});

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("new output below"), std::string::npos);
}

TEST(UiRenderGolden, ExpandedToolCallShowsArguments) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->tools.calls.empty());
    state->tools.calls.back().expanded = true;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("(expanded)"), std::string::npos);
    EXPECT_NE(rendered.find("hello.txt"), std::string::npos);
}

TEST(UiRenderGolden, SlashCommandHintsRendered) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->command_hints = {CommandHint{"help", "list slash commands"},
                            CommandHint{"new", "create and activate a new session"}};

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("/help"), std::string::npos);
    EXPECT_NE(rendered.find("list slash commands"), std::string::npos);
    EXPECT_NE(rendered.find("/new"), std::string::npos);
}

TEST(UiRenderGolden, StatusShowsTokenUsage) {
    UiModel model = build_model();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("↑12"), std::string::npos);
    EXPECT_NE(rendered.find("↓3"), std::string::npos);
}

TEST(UiRenderGolden, SubagentPanelRendered) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->subagents.agents.push_back(
        SubagentView{SessionId{"sub-1"}, "exploring", AgentState::CallingTool});

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{80, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("subagents:"), std::string::npos);
    EXPECT_NE(rendered.find("sub-1"), std::string::npos);
    EXPECT_NE(rendered.find("exploring"), std::string::npos);
}

TEST(UiRenderGolden, CollapsedToolShowsOnlyHeaderNoBody) {
    const UiModel model = build_model();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("tool: read_file"), std::string::npos);
    EXPECT_EQ(rendered.find("(expanded)"), std::string::npos);
    EXPECT_EQ(rendered.find("file-body"), std::string::npos);
}

TEST(UiRenderGolden, ExpandAllFoldsShowsBodies) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->expand_all_folds = true;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("tool: read_file (expanded)"), std::string::npos);
    EXPECT_NE(rendered.find("file-body"), std::string::npos);
}

TEST(UiRenderGolden, ReasoningFoldSummaryAndExpandAll) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->conversation.entries.clear();
    state->conversation.by_message.clear();
    state->conversation.by_reasoning_message.clear();
    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "deep thought", true}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "the answer"}});
    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "the answer", std::nullopt}});

    const std::string collapsed =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(collapsed);
    EXPECT_NE(collapsed.find("reasoning (Ctrl+O to expand)"), std::string::npos);
    EXPECT_EQ(collapsed.find("deep thought"), std::string::npos);
    EXPECT_NE(collapsed.find("the answer"), std::string::npos);

    state->expand_all_folds = true;
    const std::string expanded =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(expanded);
    EXPECT_NE(expanded.find("reasoning (expanded)"), std::string::npos);
    EXPECT_NE(expanded.find("deep thought"), std::string::npos);
}

TEST(UiRenderGolden, ReasoningStreamingSummaryStrings) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->conversation.entries.clear();
    state->conversation.by_message.clear();
    state->conversation.by_reasoning_message.clear();
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "partial", true}});

    const std::string collapsed =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(collapsed);
    EXPECT_NE(collapsed.find("reasoning (streaming · Ctrl+O to expand)"),
              std::string::npos);
    EXPECT_EQ(collapsed.find("partial"), std::string::npos);

    state->expand_all_folds = true;
    const std::string expanded =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(expanded);
    EXPECT_NE(expanded.find("reasoning (expanded · streaming)"), std::string::npos);
    EXPECT_NE(expanded.find("partial"), std::string::npos);
}

TEST(UiRenderGolden, HeaderShowsSessionTitleRightAligned) {
    UiModel model = build_model();
    model.setCellTitle(WorkspaceId{"workspace"}, kSession, "golden-title");
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::size_t first = rendered.find('\n');
    const std::size_t second = rendered.find('\n', first + 1);
    const std::string header = rendered.substr(first + 1, second - first - 1);
    SCOPED_TRACE(header);
    EXPECT_NE(header.find("golden-title"), std::string::npos);
    const std::size_t at = header.rfind("golden-title");
    EXPECT_EQ(header.substr(at), "golden-title│");
}

TEST(UiRenderGolden, HeaderFallsBackToShortIdWhenTitleEmpty) {
    const UiModel model = build_model();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::size_t first = rendered.find('\n');
    const std::size_t second = rendered.find('\n', first + 1);
    const std::string header = rendered.substr(first + 1, second - first - 1);
    SCOPED_TRACE(header);
    EXPECT_NE(header.find("golden-s│"), std::string::npos);
}

TEST(UiRenderGolden, BottomLineCountsOnlyNoSessionList) {
    const UiModel model = build_model();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("0 active · 0 waiting"), std::string::npos);
    EXPECT_EQ(rendered.find("[golden-s o]"), std::string::npos);
}

TEST(UiRenderGolden, SwitcherAttentionBadgeStillRenders) {
    UiModel model = build_model();
    model.workspaces[model.activeWorkspaceId].title = "alpha";
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-beta"};
    beta.title = "beta";
    beta.cwd = "/work/beta";
    beta.activeSessionId = SessionId{"beta-session"};
    SessionCell beta_cell;
    beta_cell.id = SessionId{"beta-session"};
    beta_cell.title = "notes";
    beta_cell.state = AgentState::WaitingForInput;
    beta_cell.attention = true;
    beta.sessions.push_back(beta_cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-beta"}, SessionId{"beta-session"});
    model.openSwitcher();

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{80, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("notes !"), std::string::npos);
}

TEST(UiRenderGolden, UserBlockBackgroundGatedByTheme) {
    UiModel model = build_model();

    const std::string colored =
        render_to_ansi(model, TerminalSize{72, 20}, Theme{true, true});
    EXPECT_NE(colored.find("\x1b[48;2;"), std::string::npos);
    EXPECT_NE(colored.find("│"), std::string::npos);

    const std::string no_block =
        render_to_ansi(model, TerminalSize{72, 20}, Theme{true, false});
    EXPECT_EQ(no_block.find("\x1b[48;2;"), std::string::npos);
    EXPECT_NE(no_block.find("│"), std::string::npos);
}

UiModel exit_prompt_model(std::vector<WorkspaceId> orphaning, int sessions, int running) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/work";
    workspace.title = "alpha";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.activeSessionId = SessionId{"active-session"};
    SessionCell active;
    active.id = SessionId{"active-session"};
    active.title = "active-session";
    workspace.sessions.push_back(active);
    SessionCell other;
    other.id = SessionId{"other-session"};
    other.title = "other-session";
    workspace.sessions.push_back(other);
    model.workspaces.emplace(workspace.id, workspace);
    model.ensureSession(SessionId{"active-session"});

    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-beta"};
    beta.title = "beta";
    beta.cwd = "/work/beta";
    beta.daemonStatus = DaemonStatus::Attached;
    beta.activeSessionId = SessionId{"beta-session"};
    SessionCell beta_cell;
    beta_cell.id = SessionId{"beta-session"};
    beta_cell.title = "beta-session";
    beta.sessions.push_back(beta_cell);
    model.workspaces.emplace(beta.id, beta);
    model.ensureSessionIn(beta.id, SessionId{"beta-session"});

    model.exitConfirm.open = true;
    model.exitConfirm.orphaning = std::move(orphaning);
    model.exitConfirm.sessions = sessions;
    model.exitConfirm.running = running;
    model.mode = UiMode::ExitConfirm;
    return model;
}

TEST(UiRenderGolden, ExitConfirmPromptZeroOneTwoDaemons) {
    for (int count = 0; count <= 2; ++count) {
        std::vector<WorkspaceId> orphaning;
        if (count >= 1) {
            orphaning.push_back(WorkspaceId{"workspace"});
        }
        if (count >= 2) {
            orphaning.push_back(WorkspaceId{"workspace-beta"});
        }
        const UiModel model = exit_prompt_model(std::move(orphaning), count, 0);
        const std::string rendered =
            normalize(render_to_ansi(model, TerminalSize{90, 24}, Theme{false}));
        SCOPED_TRACE(rendered);
        EXPECT_NE(rendered.find("Exiting will terminate " + std::to_string(count)),
                  std::string::npos);
        EXPECT_NE(rendered.find(count == 1 ? "workspace daemon:" : "workspace daemons:"),
                  std::string::npos);
        EXPECT_NE(rendered.find(std::to_string(count) + " session"), std::string::npos);
        EXPECT_NE(rendered.find("Terminate and exit"), std::string::npos);
        EXPECT_NE(rendered.find("Cancel"), std::string::npos);
        EXPECT_EQ(rendered.find("other-session"), std::string::npos)
            << "the prompt must not render a per-session list";
        EXPECT_EQ(rendered.find("beta-session"), std::string::npos)
            << "the prompt must not render a per-session list";
        if (count >= 1) {
            EXPECT_NE(rendered.find("alpha"), std::string::npos);
        }
        if (count >= 2) {
            EXPECT_NE(rendered.find("beta"), std::string::npos);
        }
    }
}

TEST(UiRenderGolden, SwitcherShowsOwnershipMarks) {
    UiModel model = build_model();
    model.workspaces[model.activeWorkspaceId].title = "owned-ws";
    model.workspaces[model.activeWorkspaceId].daemonStatus = DaemonStatus::Attached;

    WorkspaceModel stopping;
    stopping.id = WorkspaceId{"ws-stopping"};
    stopping.title = "stopping-ws";
    stopping.daemonStatus = DaemonStatus::Stopping;
    model.workspaces.emplace(stopping.id, stopping);

    WorkspaceModel not_running;
    not_running.id = WorkspaceId{"ws-notrunning"};
    not_running.title = "notrunning-ws";
    not_running.daemonStatus = DaemonStatus::NotRunning;
    model.workspaces.emplace(not_running.id, not_running);

    WorkspaceModel unreachable;
    unreachable.id = WorkspaceId{"ws-unreachable"};
    unreachable.title = "unreachable-ws";
    unreachable.daemonStatus = DaemonStatus::Dead;
    model.workspaces.emplace(unreachable.id, unreachable);

    model.openSwitcher();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{100, 30}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("[owned]"), std::string::npos);
    EXPECT_NE(rendered.find("[stopping]"), std::string::npos);
    EXPECT_NE(rendered.find("[not running]"), std::string::npos);
    EXPECT_NE(rendered.find("[unreachable]"), std::string::npos);
}

} // namespace

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/session_catalog.hpp"
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
    workspace.live = true;
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, kSession);
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

void seed_catalog_session(UiModel& model, const WorkspaceId& workspace,
                          const SessionId& session) {
    WorkspaceHistory history;
    history.id = workspace;
    history.title = workspace.value;
    history.canonicalPath = "/" + workspace.value;
    SessionHistoryEntry entry;
    entry.id = session;
    entry.title = session.value;
    entry.kind = "root";
    entry.model = "m";
    entry.updatedAt = 1;
    history.sessions.push_back(std::move(entry));
    model.catalog.workspaces.push_back(std::move(history));
    model.catalog.loaded = true;
    model.catalog.generation = 1;
}

const char* kGolden = R"GOLDEN(╭──────────────────────────────────────────────────────────────────────╮
│ymh · /work                                                           │
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
│build · test-model · ↑12 ↓3 ⚡0 · [░░░░░░░░░░] —  0 active · 0 waiting│
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
    // RB-12 addendum (2026-09-17): the dialog is Enter-only, so the footer must
    // describe selection + Enter and the dead letter/number key hints must go.
    EXPECT_NE(rendered.find("↑/↓ select · Enter confirm · Esc cancel"), std::string::npos);
    EXPECT_EQ(rendered.find("y allow · n deny"), std::string::npos);
    EXPECT_EQ(rendered.find("1) Allow once"), std::string::npos);
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
    beta.daemonStatus = DaemonStatus::Attached;
    beta.live = true;
    SessionCell beta_cell;
    beta_cell.id = SessionId{"beta-session"};
    beta_cell.title = "notes";
    beta_cell.state = AgentState::WaitingForInput;
    beta_cell.attention = true;
    beta.sessions.push_back(beta_cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-beta"}, SessionId{"beta-session"});
    seed_catalog_session(model, WorkspaceId{"workspace-beta"}, SessionId{"beta-session"});
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
    state->command_hints = {CommandHint{"help", "help", "list slash commands"},
                            CommandHint{"new", "new", "create and activate a new session"}};

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("/help"), std::string::npos);
    EXPECT_NE(rendered.find("list slash commands"), std::string::npos);
    EXPECT_NE(rendered.find("/new"), std::string::npos);
}

// 45-G1 (45-D8.3): the command list row for exit renders the alias inside the
// displayed name, separated by " - ".
TEST(UiRenderGolden, UI45_G1_ExitRowRendersAlias) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->command_hints = {CommandHint{"exit", "exit(quit)", "quit the supervisor"}};

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("> /exit(quit) - quit the supervisor"), std::string::npos);
}

TEST(UiRenderGolden, StatusShowsTokenUsage) {
    UiModel model = build_model();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("↑12"), std::string::npos);
    EXPECT_NE(rendered.find("↓3"), std::string::npos);
}

TEST(UiRenderGolden, StatusWideShowsAllSegments) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->status.plan_active          = true;
    state->status.agent_state          = AgentState::Thinking;
    state->status.tps                  = 50.0;
    state->status.context_used_tokens  = 50;
    state->status.context_window_tokens = 100;
    state->status.note                 = "a note";
    model.pushNotice("a notice");

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{200, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("plan · thinking · test-model · ↑12 ↓3 ⚡0 · 50.0 tps · "
                            "[█████░░░░░] 50.0% · a note · a notice"),
              std::string::npos);
}

TEST(UiRenderGolden, StatusPlanModeGolden) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    state->status.plan_active = true;
    std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    EXPECT_NE(rendered.find("│plan · test-model"), std::string::npos);

    state->status.plan_active = false;
    rendered = normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    EXPECT_NE(rendered.find("│build · test-model"), std::string::npos);
}

TEST(UiRenderGolden, ContextBarGeometryAndUnknownWindow) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    state->status.context_used_tokens   = 0;
    state->status.context_window_tokens = 0;
    std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{120, 20}, Theme{false}));
    EXPECT_NE(rendered.find("[░░░░░░░░░░] —"), std::string::npos);

    state->status.context_used_tokens   = 1;
    state->status.context_window_tokens = 1000;
    rendered = normalize(render_to_ansi(model, TerminalSize{120, 20}, Theme{false}));
    EXPECT_NE(rendered.find("[█░░░░░░░░░] 0.1%"), std::string::npos);

    state->status.context_used_tokens   = 80;
    state->status.context_window_tokens = 100;
    rendered = normalize(render_to_ansi(model, TerminalSize{120, 20}, Theme{false}));
    EXPECT_NE(rendered.find("[████████░░] 80.0%"), std::string::npos);
}

TEST(UiRenderGolden, ContextBarRendersWithKnownWindow) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->status.context_used_tokens   = 50'000;
    state->status.context_window_tokens = 131'072;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{120, 20}, Theme{false}));
    EXPECT_NE(rendered.find("[███░░░░░░░] 38.1%"), std::string::npos);
}

TEST(UiRenderGolden, StatusNarrowDegradationDropsTpsBeforeNoteAndNotice) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->status.tps                   = 50.0;
    state->status.context_used_tokens   = 50;
    state->status.context_window_tokens = 100;
    state->status.note                  = "note";
    model.pushNotice("notice");

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{91, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("↑12 ↓3 ⚡0"), std::string::npos);
    EXPECT_NE(rendered.find("[█████░░░░░] 50.0%"), std::string::npos);
    EXPECT_NE(rendered.find("note"), std::string::npos);
    EXPECT_NE(rendered.find("notice"), std::string::npos);
    EXPECT_EQ(rendered.find("50.0 tps"), std::string::npos);
    EXPECT_NE(rendered.find("build · test-model"), std::string::npos);
}

// 25 review H3: the status row is inside the 2-column border, so the fit math
// must use the inner width; otherwise the right-aligned aggregate is clipped.
TEST(UiRenderGolden, StatusAggregateNotClippedByBorder) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->status.context_used_tokens   = 50;
    state->status.context_window_tokens = 100;

    for (const int width : {73, 80, 100}) {
        const std::string rendered =
            normalize(render_to_ansi(model, TerminalSize{width, 20}, Theme{false}));
        SCOPED_TRACE(width);
        EXPECT_NE(rendered.find("0 active · 0 waiting"), std::string::npos) << rendered;
    }
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
    EXPECT_NE(collapsed.find("• Thinking"), std::string::npos);
    EXPECT_NE(collapsed.find("ctrl+o to expand"), std::string::npos);
    EXPECT_EQ(collapsed.find("deep thought"), std::string::npos);
    EXPECT_NE(collapsed.find("the answer"), std::string::npos);

    state->expand_all_folds = true;
    const std::string expanded =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(expanded);
    EXPECT_NE(expanded.find("• Thinking"), std::string::npos);
    EXPECT_NE(expanded.find("expanded"), std::string::npos);
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
    EXPECT_NE(collapsed.find("⠋ Thinking"), std::string::npos);
    EXPECT_NE(collapsed.find("ctrl+o to expand"), std::string::npos);
    EXPECT_EQ(collapsed.find("partial"), std::string::npos);

    state->expand_all_folds = true;
    const std::string expanded =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(expanded);
    EXPECT_NE(expanded.find("⠋ Thinking"), std::string::npos);
    EXPECT_NE(expanded.find("expanded"), std::string::npos);
    EXPECT_NE(expanded.find("partial"), std::string::npos);
}

TEST(UiRenderGolden, ReasoningSpinnerFrameFromModel) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->conversation.entries.clear();
    state->conversation.by_message.clear();
    state->conversation.by_reasoning_message.clear();
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "partial", true}});
    model.spinner.frame = 3;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("⠸ Thinking"), std::string::npos);

    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "done", std::nullopt}});
    const std::string finished =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    SCOPED_TRACE(finished);
    EXPECT_NE(finished.find("• Thinking"), std::string::npos);
    EXPECT_EQ(finished.find("⠸ Thinking"), std::string::npos);
}

TEST(UiRenderGolden, ReasoningSpinnerOnlyAdvancesWhileStreaming) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    EXPECT_FALSE(model.has_streaming_reasoning());
    EXPECT_FALSE(model.advance_reasoning_spinner(std::chrono::milliseconds{1000}));
    EXPECT_EQ(model.spinner.frame, 0u);

    state->conversation.entries.clear();
    state->conversation.by_message.clear();
    state->conversation.by_reasoning_message.clear();
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "partial", true}});
    ASSERT_TRUE(model.has_streaming_reasoning());
    EXPECT_TRUE(model.advance_reasoning_spinner(std::chrono::milliseconds{120}));
    EXPECT_EQ(model.spinner.frame, 1u);

    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "done", std::nullopt}});
    EXPECT_FALSE(model.has_streaming_reasoning());
    EXPECT_FALSE(model.advance_reasoning_spinner(std::chrono::milliseconds{1000}));
    EXPECT_EQ(model.spinner.frame, 1u);
}

TEST(UiRenderGolden, ReasoningExpandHintIsDimmed) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->conversation.entries.clear();
    state->conversation.by_message.clear();
    state->conversation.by_reasoning_message.clear();
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "partial", true}});

    const std::string raw = render_to_ansi(model, TerminalSize{72, 20}, Theme{true});
    const std::size_t hint = raw.find("ctrl+o to expand");
    ASSERT_NE(hint, std::string::npos);
    const std::size_t start = hint > 24 ? hint - 24 : 0;
    const std::string window = raw.substr(start, hint - start);
    EXPECT_NE(window.find("\x1b[2m"), std::string::npos) << window;
}

TEST(UiRenderGolden, SlashCommandCompletionSelectionHighlighted) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->command_hints = {CommandHint{"help", "help", "list slash commands"},
                            CommandHint{"new", "new", "create and activate a new session"},
                            CommandHint{"model", "model", "show or set the model"}};
    state->command_hint_selected = 1;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("> /new"), std::string::npos);
    EXPECT_NE(rendered.find("  /help"), std::string::npos);
    EXPECT_NE(rendered.find("  /model"), std::string::npos);
    EXPECT_EQ(rendered.find("> /help"), std::string::npos);
}

// UX-G4 (25-D8): the highlighted `>` palette row and the composer draft are the
// same command. The live Tab->state sync is covered by
// SupervisorHarnessTest.UX_U14; this pins the render contract.
TEST(UiRenderGolden, PaletteHighlightAndDraftAgreeGolden) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->command_hints = {CommandHint{"help", "help", "list slash commands"},
                            CommandHint{"skills", "skills", "list available skills"},
                            CommandHint{"new", "new", "create and activate a new session"}};
    state->command_hint_selected = 1;
    state->input.draft = "/skills";
    state->input.cursor = state->input.draft.size();

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("> /skills"), std::string::npos);
    EXPECT_NE(rendered.find("  /help"), std::string::npos);
    EXPECT_NE(rendered.find("  /new"), std::string::npos);
    EXPECT_EQ(rendered.find("> /help"), std::string::npos);
    EXPECT_EQ(rendered.find("> /new"), std::string::npos);
}

TEST(UiRenderGolden, SlashCommandSelectionUsesThemeAccent) {
    UiModel model = build_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->command_hints = {CommandHint{"help", "help", "list slash commands"},
                            CommandHint{"new", "new", "create and activate a new session"}};

    const auto accent_precedes = [](const std::string& raw, const std::string& token) {
        const std::size_t pos = raw.find(token);
        if (pos == std::string::npos) {
            return false;
        }
        const std::size_t start = pos > 24 ? pos - 24 : 0;
        return raw.substr(start, pos - start).find("\x1b[96m") != std::string::npos;
    };

    state->command_hint_selected = 0;
    const std::string first = render_to_ansi(model, TerminalSize{72, 24}, Theme{true});
    EXPECT_TRUE(accent_precedes(first, "/help"));
    EXPECT_FALSE(accent_precedes(first, "/new"));

    state->command_hint_selected = 1;
    const std::string second = render_to_ansi(model, TerminalSize{72, 24}, Theme{true});
    EXPECT_TRUE(accent_precedes(second, "/new"));
    EXPECT_FALSE(accent_precedes(second, "/help"));
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

TEST(UiRenderGolden, HeaderHidesPlaceholderTitle) {
    const auto header_of = [](const UiModel& model) {
        const std::string rendered =
            normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
        const std::size_t first = rendered.find('\n');
        const std::size_t second = rendered.find('\n', first + 1);
        return rendered.substr(first + 1, second - first - 1);
    };

    // An active session with no cell/title at all renders an empty right slot
    // rather than the short id.
    const std::string no_title = header_of(build_model());
    SCOPED_TRACE(no_title);
    EXPECT_EQ(no_title.find("golden-s"), std::string::npos);

    // Every create-time placeholder must be suppressed in the header.
    for (const char* placeholder : {"tui", "headless", "main"}) {
        UiModel model = build_model();
        model.setCellTitle(model.activeWorkspaceId, kSession, placeholder);
        const std::string header = header_of(model);
        SCOPED_TRACE(placeholder);
        SCOPED_TRACE(header);
        EXPECT_EQ(header.find(placeholder), std::string::npos);
    }
}

TEST(UiRenderGolden, SwitcherShowsShortIdForPlaceholderTitle) {
    UiModel model = build_model();
    model.workspaces[model.activeWorkspaceId].title = "alpha";
    const SessionId other{"cafebabe-1234"};
    model.ensureSessionIn(model.activeWorkspaceId, other);
    model.setCellTitle(model.activeWorkspaceId, other, "tui");
    seed_catalog_session(model, model.activeWorkspaceId, other);
    model.openSwitcher();

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{80, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_EQ(rendered.find("tui"), std::string::npos);
    EXPECT_NE(rendered.find("[cafebabe "), std::string::npos);
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
    beta.live = true;
    SessionCell beta_cell;
    beta_cell.id = SessionId{"beta-session"};
    beta_cell.title = "notes";
    beta_cell.state = AgentState::WaitingForInput;
    beta_cell.attention = true;
    beta.sessions.push_back(beta_cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-beta"}, SessionId{"beta-session"});
    seed_catalog_session(model, WorkspaceId{"workspace-beta"}, SessionId{"beta-session"});
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

UiModel exit_prompt_model(std::vector<WorkspaceId> orphaning, int sessions, int running,
                          std::optional<int> selected = std::nullopt) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/work";
    workspace.title = "alpha";
    workspace.daemonStatus = DaemonStatus::Attached;
    SessionCell active;
    active.id = SessionId{"active-session"};
    active.title = "active-session";
    workspace.sessions.push_back(active);
    SessionCell other;
    other.id = SessionId{"other-session"};
    other.title = "other-session";
    workspace.sessions.push_back(other);
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, SessionId{"active-session"});

    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-beta"};
    beta.title = "beta";
    beta.cwd = "/work/beta";
    beta.daemonStatus = DaemonStatus::Attached;
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
    if (selected.has_value()) {
        model.exitConfirm.selected = *selected;
    }
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
        const std::string raw = render_to_ansi(model, TerminalSize{90, 24}, Theme{false});
        const std::string rendered = normalize(raw);
        SCOPED_TRACE(rendered);
        EXPECT_NE(rendered.find("Exiting will terminate " + std::to_string(count)),
                  std::string::npos);
        EXPECT_NE(rendered.find(count == 1 ? "workspace daemon:" : "workspace daemons:"),
                  std::string::npos);
        EXPECT_NE(rendered.find(std::to_string(count) + " session"), std::string::npos);
        EXPECT_NE(rendered.find("Terminate and exit"), std::string::npos);
        EXPECT_NE(rendered.find("Cancel"), std::string::npos);
        EXPECT_NE(rendered.find("↑/↓ select · Enter confirm · y terminate · n cancel · "
                                "Esc cancel"),
                  std::string::npos)
            << "the footer must advertise the keys that actually work";
        EXPECT_NE(raw.find("\x1b[7m[ Terminate and exit ]"), std::string::npos)
            << "the popup must open with Terminate highlighted";
        EXPECT_EQ(raw.find("\x1b[7m[ Cancel ]"), std::string::npos);
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

// User-reported (2026-09-17): the highlight tracks `selected` (0 = Terminate,
// 1 = Cancel). The inverted SGR (`\x1b[7m`) immediately precedes the highlighted
// option, so this pins which row is highlighted, not merely that both exist.
TEST(UiRenderGolden, ExitConfirmHighlightFollowsSelection) {
    const std::string terminate_highlight = "\x1b[7m[ Terminate and exit ]";
    const std::string cancel_highlight = "\x1b[7m[ Cancel ]";

    const UiModel defaulted = exit_prompt_model({WorkspaceId{"workspace"}}, 1, 0);
    const std::string default_raw =
        render_to_ansi(defaulted, TerminalSize{90, 24}, Theme{false});
    EXPECT_NE(default_raw.find(terminate_highlight), std::string::npos);
    EXPECT_EQ(default_raw.find(cancel_highlight), std::string::npos);

    const UiModel cancelled = exit_prompt_model({WorkspaceId{"workspace"}}, 1, 0, 1);
    const std::string cancelled_raw =
        render_to_ansi(cancelled, TerminalSize{90, 24}, Theme{false});
    EXPECT_NE(cancelled_raw.find(cancel_highlight), std::string::npos)
        << "selected=1 must highlight Cancel";
    EXPECT_EQ(cancelled_raw.find(terminate_highlight), std::string::npos);
}

// SW-G1 (22 §3.1/§3.3): the Live switcher renders only [owned]/[stopping];
// [not running]/[unreachable] are unreachable, including for a live Connecting
// workspace (the SW20 gate).
TEST(UiRenderGolden, SwitcherShowsOwnershipMarks) {
    UiModel model = build_model();
    model.workspaces[model.activeWorkspaceId].title = "owned-ws";
    model.workspaces[model.activeWorkspaceId].daemonStatus = DaemonStatus::Attached;
    model.workspaces[model.activeWorkspaceId].live = true;

    WorkspaceModel stopping;
    stopping.id = WorkspaceId{"ws-stopping"};
    stopping.title = "stopping-ws";
    stopping.daemonStatus = DaemonStatus::Stopping;
    stopping.live = true;
    model.workspaces.emplace(stopping.id, stopping);

    WorkspaceModel connecting;
    connecting.id = WorkspaceId{"ws-connecting"};
    connecting.title = "connecting-ws";
    connecting.daemonStatus = DaemonStatus::Connecting;
    connecting.live = true;
    model.workspaces.emplace(connecting.id, connecting);

    WorkspaceModel not_running;
    not_running.id = WorkspaceId{"ws-notrunning"};
    not_running.title = "notrunning-ws";
    not_running.daemonStatus = DaemonStatus::NotRunning;
    not_running.live = true;
    model.workspaces.emplace(not_running.id, not_running);

    WorkspaceModel unreachable;
    unreachable.id = WorkspaceId{"ws-unreachable"};
    unreachable.title = "unreachable-ws";
    unreachable.daemonStatus = DaemonStatus::Dead;
    unreachable.live = true;
    model.workspaces.emplace(unreachable.id, unreachable);

    model.openSwitcher();
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{100, 30}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("workspaces"), std::string::npos);
    EXPECT_NE(rendered.find("Switcher"), std::string::npos);
    EXPECT_NE(rendered.find("[owned]"), std::string::npos);
    EXPECT_NE(rendered.find("[stopping]"), std::string::npos);
    EXPECT_EQ(rendered.find("[not running]"), std::string::npos);
    EXPECT_EQ(rendered.find("[unreachable]"), std::string::npos);
    EXPECT_EQ(rendered.find("notrunning-ws"), std::string::npos);
    EXPECT_EQ(rendered.find("unreachable-ws"), std::string::npos);
    EXPECT_EQ(rendered.find("connecting-ws"), std::string::npos);
}

ContextSnapshot context_fixture() {
    ContextSnapshot snapshot;
    snapshot.session = kSession;
    snapshot.used_tokens = 41300;
    snapshot.budget = ContextBudget{64000, 4096, 47923};
    snapshot.captured_sequence = 1842;
    snapshot.segments = {
        ContextSegment{ContextSegmentKind::SystemPrompt, "agent.system_prompt", 412, 1},
        ContextSegment{ContextSegmentKind::ToolSchemas, "ToolRegistry::schemas()", 3980, 2},
        ContextSegment{ContextSegmentKind::McpToolSchemas, "ToolRegistry::schemas() (mcp.)", 2240,
                       2},
        ContextSegment{ContextSegmentKind::Conversation, "deriveMessages(header, log)", 33540, 42},
        ContextSegment{ContextSegmentKind::CompactionSummary, "payload::ContextCompaction.summary",
                       1128, 1},
        ContextSegment{ContextSegmentKind::FreeSpace, "window − used", 22700, 0},
    };
    snapshot.tools = {
        ContextToolEntry{"read_file", "builtin", 32},
        ContextToolEntry{"shell", "builtin", 30},
        ContextToolEntry{"mcp.alpha.search", "mcp", 48},
        ContextToolEntry{"mcp.beta.fetch", "mcp", 40},
    };
    snapshot.mcp_servers = {
        ContextServerEntry{"alpha", "ready", 2, 0, false},
        ContextServerEntry{"beta", "degraded", 1, 1, true},
    };
    return snapshot;
}

UiModel context_model(ContextSnapshot snapshot, int view = 0, std::string note = "") {
    UiModel model = build_model();
    model.mode = UiMode::Context;
    model.context.open = true;
    model.context.loaded = true;
    model.context.session = snapshot.session;
    model.context.snapshot = std::move(snapshot);
    model.context.view = view;
    model.context.note = std::move(note);
    return model;
}

std::string repeat(char value, int count) { return std::string(static_cast<std::size_t>(count), value); }

TEST(UiRenderGolden, ContextOverlayGridGolden) {
    const UiModel model = context_model(context_fixture());
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{66, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("used 41,300 / window 64,000 (64.5%)  @1842"), std::string::npos);
    EXPECT_NE(rendered.find("used 41,300 / 64,000 (64.5%)  threshold 47,923  reserve 4,096"),
              std::string::npos);
    EXPECT_NE(rendered.find(repeat('S', 5) + repeat('T', 39) + repeat('M', 20)), std::string::npos);
    EXPECT_NE(rendered.find(repeat('M', 3) + repeat('C', 61)), std::string::npos);
    EXPECT_NE(rendered.find(repeat('C', 18) + repeat('~', 11) + repeat('.', 35)), std::string::npos);
    EXPECT_NE(rendered.find("~  compaction summary    1,128 tok   1.7%"), std::string::npos);
    EXPECT_NE(rendered.find(".  free                 22,700 tok  35.4%"), std::string::npos);
    EXPECT_EQ(rendered.find("1.8%"), std::string::npos);
    EXPECT_EQ(rendered.find("35.5%"), std::string::npos);
    std::size_t rows = 1;
    for (const char character : rendered) {
        rows += character == '\n' ? 1 : 0;
    }
    EXPECT_EQ(rows, 20u);
    EXPECT_EQ(render_to_ansi(model, TerminalSize{66, 20}, Theme{false}),
              render_to_ansi(model, TerminalSize{66, 20}, Theme{false}));
}

TEST(UiRenderGolden, ContextOverlayMonochrome) {
    const UiModel model = context_model(context_fixture());
    const std::string raw = render_to_ansi(model, TerminalSize{66, 20}, Theme{false});
    SCOPED_TRACE(raw);
    EXPECT_NE(raw.find(repeat('S', 5) + repeat('T', 39) + repeat('M', 20)), std::string::npos);
    EXPECT_EQ(raw.find("\x1b[34m"), std::string::npos);
    EXPECT_EQ(raw.find("\x1b[32m"), std::string::npos);
    EXPECT_EQ(raw.find("\x1b[35m"), std::string::npos);
    EXPECT_EQ(raw.find("\x1b[36m"), std::string::npos);
}

TEST(UiRenderGolden, ContextOverlayColor) {
    const UiModel model = context_model(context_fixture());
    const std::string raw = render_to_ansi(model, TerminalSize{66, 20}, Theme{true});
    SCOPED_TRACE(raw);
    EXPECT_NE(raw.find("\x1b[34m"), std::string::npos);
    EXPECT_NE(raw.find("\x1b[32m"), std::string::npos);
    EXPECT_NE(raw.find("\x1b[35m"), std::string::npos);
    EXPECT_NE(raw.find("\x1b[36m"), std::string::npos);
}

TEST(UiRenderGolden, ContextOverlaySmallTerminal) {
    const UiModel model = context_model(context_fixture());
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{52, 10}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("terminal too small for the grid (need >= 40x10)"), std::string::npos);
    EXPECT_NE(rendered.find("used 41,300 / window 64,000 (64.5%)  @1842"), std::string::npos);
    EXPECT_NE(rendered.find("system prompt"), std::string::npos);
    EXPECT_EQ(rendered.find("threshold"), std::string::npos);
    std::size_t rows = 1;
    for (const char character : rendered) {
        rows += character == '\n' ? 1 : 0;
    }
    EXPECT_EQ(rows, 10u);
}

TEST(UiRenderGolden, ContextOverlaySmallTerminalNote) {
    const UiModel model = context_model(context_fixture(), 0, "budget unknown");
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{52, 10}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("budget unknown"), std::string::npos);
    EXPECT_EQ(rendered.find("terminal too small for the grid"), std::string::npos);
    EXPECT_EQ(rendered.find("threshold"), std::string::npos);
}

TEST(UiRenderGolden, ContextOverlaySmallTerminalInventory) {
    const UiModel model = context_model(context_fixture(), 1);
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{52, 10}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("servers"), std::string::npos);
    EXPECT_NE(rendered.find("alpha  ready  tools=2"), std::string::npos);
    EXPECT_EQ(rendered.find("system prompt"), std::string::npos);
    EXPECT_NE(rendered.find("terminal too small for the grid (need >= 40x10)"), std::string::npos);
}

TEST(UiRenderGolden, ContextOverlayInventoryView) {
    const UiModel model = context_model(context_fixture(), 1);
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{66, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("servers"), std::string::npos);
    EXPECT_NE(rendered.find("alpha  ready  tools=2"), std::string::npos);
    EXPECT_NE(rendered.find("beta  degraded  tools=1  skipped=1  !"), std::string::npos);
    EXPECT_NE(rendered.find("tools"), std::string::npos);
    EXPECT_NE(rendered.find("read_file  builtin  ~32 tok"), std::string::npos);
    EXPECT_NE(rendered.find("shell  builtin  ~30 tok"), std::string::npos);
    EXPECT_EQ(rendered.find("system prompt"), std::string::npos);
}

TEST(UiRenderGolden, ContextOverlayBudgetUnknown) {
    ContextSnapshot snapshot = context_fixture();
    snapshot.budget = ContextBudget{0, 4096, 0};
    snapshot.note = "budget unknown";
    snapshot.segments.back().tokens = 0;
    const UiModel model = context_model(std::move(snapshot), 0, "budget unknown");
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{66, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("used tokens (budget unknown)  @1842"), std::string::npos);
    EXPECT_NE(rendered.find("budget unknown"), std::string::npos);
    EXPECT_EQ(rendered.find("threshold"), std::string::npos);
    EXPECT_EQ(rendered.find("%"), std::string::npos);
}

TEST(UiRenderGolden, ContextOverlayOverBudget) {
    ContextSnapshot snapshot = context_fixture();
    snapshot.used_tokens = 70000;
    snapshot.segments = {
        ContextSegment{ContextSegmentKind::SystemPrompt, "agent.system_prompt", 412, 1},
        ContextSegment{ContextSegmentKind::ToolSchemas, "ToolRegistry::schemas()", 3980, 2},
        ContextSegment{ContextSegmentKind::McpToolSchemas, "ToolRegistry::schemas() (mcp.)", 2240,
                       2},
        ContextSegment{ContextSegmentKind::Conversation, "deriveMessages(header, log)", 62240, 42},
        ContextSegment{ContextSegmentKind::CompactionSummary, "payload::ContextCompaction.summary",
                       1128, 1},
        ContextSegment{ContextSegmentKind::FreeSpace, "window − used", 0, 0},
    };
    const UiModel model = context_model(std::move(snapshot));
    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{66, 20}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("used 70,000 / 64,000 (109.3%)  threshold 47,923  reserve 4,096"),
              std::string::npos);
    EXPECT_EQ(rendered.find(">100%"), std::string::npos);
}

class HistoryTempDir {
public:
    explicit HistoryTempDir(const std::string& prefix) {
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~HistoryTempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    HistoryTempDir(const HistoryTempDir&) = delete;
    HistoryTempDir& operator=(const HistoryTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

WorkspaceHistory history_workspace(const char* id, const char* title, const char* path, bool live) {
    WorkspaceHistory history;
    history.id = WorkspaceId{id};
    history.title = title;
    history.canonicalPath = path;
    history.live = live;
    return history;
}

SessionHistoryEntry history_session(const char* id, const char* title, std::int64_t updated,
                                    const char* kind, const char* model,
                                    std::optional<SessionId> parent = std::nullopt) {
    SessionHistoryEntry entry;
    entry.id = SessionId{id};
    entry.title = title;
    entry.kind = kind;
    entry.model = model;
    entry.updatedAt = updated;
    entry.parent = std::move(parent);
    return entry;
}

UiModel history_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws-beta"};
    WorkspaceModel beta;
    beta.id = WorkspaceId{"ws-beta"};
    beta.title = "beta";
    beta.cwd = "/beta";
    beta.daemonStatus = DaemonStatus::Attached;
    beta.live = true;
    model.workspaces.emplace(beta.id, beta);

    WorkspaceHistory zeta = history_workspace("ws-zeta", "Zeta", "/zeta", false);
    zeta.sessions.push_back(
        history_session("s2", "second", 2000, "fork", "model-z", SessionId{"parent-1234"}));
    zeta.sessions.push_back(history_session("s1", "first", 5000, "root", "model-a"));
    WorkspaceHistory alpha = history_workspace("ws-alpha", "alpha", "/alpha", true);
    alpha.sessions.push_back(history_session("s3", "session-one", 3000, "root", "model-b"));

    model.catalog.workspaces = {zeta, alpha};
    model.catalog.loaded = true;
    model.catalog.complete = false;
    model.catalog.capturedAtMs = 1'000'000;
    model.catalog.nowMs = 1'005'000;
    return model;
}

// SW-G2 (22 §3.7/§4.3): the History overlay orders workspace groups by title
// (case-insensitive, canonical_path tie-break), lists History sessions
// `updated_at` desc, marks a non-live group `[history]`/a live one `[owned]`,
// renders the full session leaf, and shows the `partial` footer.
TEST(UiRenderGolden, HistoryOverlayGroupsAndLeaves) {
    UiModel model = history_model();
    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{100, 30}, Theme{false}));
    SCOPED_TRACE(rendered);

    EXPECT_NE(rendered.find("sessions"), std::string::npos);
    const std::size_t alpha_pos = rendered.find("alpha");
    const std::size_t zeta_pos = rendered.find("Zeta");
    ASSERT_NE(alpha_pos, std::string::npos);
    ASSERT_NE(zeta_pos, std::string::npos);
    EXPECT_LT(alpha_pos, zeta_pos);

    EXPECT_NE(rendered.find("[owned]"), std::string::npos);
    EXPECT_NE(rendered.find("[history]"), std::string::npos);

    EXPECT_NE(rendered.find("first · root · model-a · 16m"), std::string::npos);
    EXPECT_NE(rendered.find("second · fork · model-z · 16m · fork←parent-1"), std::string::npos);
    EXPECT_NE(rendered.find("session-one · root · model-b · 16m"), std::string::npos);
    EXPECT_LT(rendered.find("first · root"), rendered.find("second · fork"));

    EXPECT_NE(rendered.find("stored sessions"), std::string::npos);
    EXPECT_NE(rendered.find("captured 5s ago"), std::string::npos);
    EXPECT_NE(rendered.find("partial"), std::string::npos);
}

// SW-G2 variant: a stored session whose title is still a creation placeholder
// is listed by its short id, never by the placeholder text.
TEST(UiRenderGolden, HistoryOverlayShowsShortIdForPlaceholderTitle) {
    UiModel model = history_model();
    model.catalog.workspaces[1].sessions.clear();
    model.catalog.workspaces[1].sessions.push_back(
        history_session("cafebabe-1234", "tui", 3000, "root", "model-b"));
    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{100, 30}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_EQ(rendered.find("tui ·"), std::string::npos);
    EXPECT_NE(rendered.find("cafebabe · root"), std::string::npos);
}

// SW-G3 (22 §4.2/§4.3): before the first snapshot the History overlay shows the
// loading placeholder; a loaded workspace with zero stored sessions renders the
// `(no stored sessions)` leaf.
TEST(UiRenderGolden, HistoryOverlayLoadingAndEmpty) {
    UiModel loading = build_model();
    loading.switcher.openHistory(loading);
    loading.mode = UiMode::Switcher;
    const std::string loading_rendered =
        normalize(render_to_ansi(loading, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(loading_rendered);
    EXPECT_NE(loading_rendered.find("loading stored sessions"), std::string::npos);

    UiModel empty = history_model();
    empty.catalog.workspaces[0].sessions.clear();
    empty.catalog.workspaces[1].sessions.clear();
    empty.switcher.openHistory(empty);
    empty.mode = UiMode::Switcher;
    const std::string empty_rendered =
        normalize(render_to_ansi(empty, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(empty_rendered);
    EXPECT_NE(empty_rendered.find("(no stored sessions)"), std::string::npos);
}

// SW-I4/SW-I7 UI half (22 §4.4/§10.2): the reader's pinned per-workspace notes
// flow through `openHistory` and render as exactly one marker each, with the
// `partial` footer, while a good group still lists. The reader half lives in
// `session_catalog_test.cpp`; here the real read helper is exercised for the
// uid-independent notes.
TEST(UiRenderGolden, HistoryOverlayDegradationNotesRender) {
    HistoryTempDir root("ymh_history_ui");
    const std::filesystem::path bad = root.path() / "bad";
    std::filesystem::create_directories(bad / ".ymh");
    {
        std::ofstream(bad / ".ymh" / "sessions.db") << "not sqlite";
    }
    WorkspaceRecord bad_record;
    bad_record.id = WorkspaceId{"ws-bad"};
    bad_record.canonicalPath = bad;
    bad_record.displayTitle = "bad";

    const std::filesystem::path nodb = root.path() / "nodb";
    std::filesystem::create_directories(nodb);
    WorkspaceRecord nodb_record;
    nodb_record.id = WorkspaceId{"ws-nodb"};
    nodb_record.canonicalPath = nodb;
    nodb_record.displayTitle = "nodb";

    WorkspaceRecord gone_record;
    gone_record.id = WorkspaceId{"ws-gone"};
    gone_record.canonicalPath = root.path() / "gone";
    gone_record.displayTitle = "gone";

    WorkspaceHistory bad_history = read_workspace_history(bad_record, false);
    ASSERT_TRUE(bad_history.note.has_value());
    EXPECT_EQ(*bad_history.note, "corrupt");
    WorkspaceHistory nodb_history = read_workspace_history(nodb_record, false);
    ASSERT_TRUE(nodb_history.note.has_value());
    EXPECT_EQ(*nodb_history.note, "no sessions.db");
    WorkspaceHistory gone_history = read_workspace_history(gone_record, false);
    ASSERT_TRUE(gone_history.note.has_value());
    EXPECT_EQ(*gone_history.note, "workspace missing");

    WorkspaceHistory read_only = history_workspace("ws-ro", "readonly", "/readonly", false);
    read_only.note = "read-only location";
    WorkspaceHistory good = history_workspace("ws-good", "good", "/good", true);
    good.sessions.push_back(history_session("good-session", "kept", 15000, "root", "m"));

    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws-good"};
    model.catalog.loaded = true;
    model.catalog.complete = false;
    model.catalog.capturedAtMs = 10'000;
    model.catalog.nowMs = 20'000;
    model.catalog.workspaces = {bad_history, nodb_history, gone_history, read_only, good};
    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{110, 40}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("(corrupt)"), std::string::npos);
    EXPECT_NE(rendered.find("(no stored sessions)"), std::string::npos);
    EXPECT_NE(rendered.find("(workspace missing)"), std::string::npos);
    EXPECT_NE(rendered.find("(read-only location)"), std::string::npos);
    EXPECT_NE(rendered.find("partial"), std::string::npos);
    EXPECT_NE(rendered.find("kept · root · m"), std::string::npos);
}

UiModel provenance_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id              = model.activeWorkspaceId;
    workspace.cwd             = "/work";
    workspace.daemonStatus    = DaemonStatus::Attached;
    workspace.live            = true;
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, kSession);
    return model;
}

payload::ContextInjected context_payload(ContextForm form, std::string plugin,
                                         std::string text,
                                         std::vector<ContextSnapshotSection> sections = {}) {
    payload::ContextInjected payload;
    payload.id   = "ctx-" + std::string{context_form_name(form)};
    payload.role = Role::User;
    payload.text = text;
    if (form == ContextForm::Snapshot) {
        payload.context = ContextFormed{ContextForm::Snapshot, std::move(sections)};
    } else if (form == ContextForm::Notice) {
        payload.context = ContextFormed{ContextForm::Notice, {}, text};
    } else {
        payload.context = ContextFormed{form};
    }
    payload.source.kind    = MessageSource::Kind::Plugin;
    payload.source.plugin  = std::move(plugin);
    payload.source.context = payload.context;
    return payload;
}

TEST(UiRenderGolden, ContextRowsUseFormLabels) {
    UiModel model = provenance_model();
    UiEventAdapter adapter(model);
    adapter.onEvent(typed_event(EventType::ContextInjected,
                                context_payload(ContextForm::Instructions, "agent-instructions",
                                                "instruction body")));
    adapter.onEvent(typed_event(EventType::ContextInjected,
                                context_payload(ContextForm::Catalog, "skill-catalog",
                                                "catalog body")));
    adapter.onEvent(typed_event(EventType::ContextInjected,
                                context_payload(ContextForm::Snapshot, "runtime-context",
                                                "snapshot body",
                                                {{"runtime-context", "snapshot section"}})));
    adapter.onEvent(typed_event(EventType::ContextInjected,
                                context_payload(ContextForm::Notice, "notice-plugin",
                                                "notice body")));
    adapter.onEvent(typed_event(EventType::ContextInjected,
                                context_payload(ContextForm::Relay, "relay-plugin",
                                                "relay body")));

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    EXPECT_NE(rendered.find("instructions (agent-instructions)"), std::string::npos);
    EXPECT_NE(rendered.find("catalog (skill-catalog)"), std::string::npos);
    EXPECT_NE(rendered.find("runtime context (runtime-context)"), std::string::npos);
    EXPECT_NE(rendered.find("notice (notice-plugin)"), std::string::npos);
    EXPECT_NE(rendered.find("context (relay-plugin)"), std::string::npos);
    EXPECT_EQ(rendered.find("instruction body"), std::string::npos);
}

TEST(UiRenderGolden, ExpandedContextRowRevealsBody) {
    UiModel model = provenance_model();
    UiEventAdapter adapter(model);
    adapter.onEvent(typed_event(EventType::ContextInjected,
                                context_payload(ContextForm::Snapshot, "runtime-context",
                                                "snapshot body",
                                                {{"runtime-context", "snapshot section"}})));
    model.session(kSession)->expand_all_folds = true;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 24}, Theme{false}));
    EXPECT_NE(rendered.find("runtime context (runtime-context)"), std::string::npos);
    EXPECT_NE(rendered.find("snapshot section"), std::string::npos);
}

TEST(UiRenderGolden, ToolNoticeSuffixRendered) {
    UiModel model = provenance_model();
    UiEventAdapter adapter(model);
    adapter.onEvent(typed_event(EventType::ToolCall, [] {
        payload::ToolCall call;
        call.id   = "t1";
        call.name = "read_file";
        return call;
    }()));
    adapter.onEvent(typed_event(EventType::ToolResult, [] {
        payload::ToolResult result;
        result.id      = "t1";
        result.name    = "read_file";
        result.output  = "body";
        result.source.kind = MessageSource::Kind::Tool;
        result.source.call = result.id;
        result.context = ContextFormed{ContextForm::Notice, {}, "retention notice"};
        return result;
    }()));

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    EXPECT_NE(rendered.find("tool: read_file"), std::string::npos);
    EXPECT_NE(rendered.find("notice: retention notice"), std::string::npos);
}

TEST(UiRenderGolden, UI45_G4_LiveNoSuppression) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws-alpha"};
    WorkspaceModel alpha;
    alpha.id           = WorkspaceId{"ws-alpha"};
    alpha.title        = "alpha";
    alpha.cwd          = "/alpha";
    alpha.daemonStatus = DaemonStatus::Attached;
    alpha.live         = true;
    SessionCell focused_cell;
    focused_cell.id    = SessionId{"focused"};
    focused_cell.title = "focused";
    SessionCell kept_cell;
    kept_cell.id    = SessionId{"kept"};
    kept_cell.title = "kept";
    alpha.sessions  = {focused_cell, kept_cell};
    model.workspaces.emplace(alpha.id, alpha);
    model.focusSessionIn(alpha.id, SessionId{"focused"});
    WorkspaceHistory alpha_history;
    alpha_history.id            = alpha.id;
    alpha_history.title         = "alpha";
    alpha_history.canonicalPath = "/alpha";
    alpha_history.live          = true;
    alpha_history.sessions.push_back(history_session("focused", "focused", 1, "root", "m"));
    alpha_history.sessions.push_back(history_session("kept", "kept", 2, "root", "m"));
    model.catalog.workspaces = {alpha_history};
    model.catalog.loaded     = true;
    model.catalog.generation = 1;
    model.openSwitcher();

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(rendered);
    EXPECT_NE(rendered.find("kept"), std::string::npos);
    EXPECT_EQ(rendered.find("[focused "), std::string::npos);
    EXPECT_EQ(rendered.find("(current session hidden)"), std::string::npos);

    UiModel only;
    only.activeWorkspaceId = WorkspaceId{"ws-only"};
    WorkspaceModel only_workspace;
    only_workspace.id           = WorkspaceId{"ws-only"};
    only_workspace.title        = "only";
    only_workspace.cwd          = "/only";
    only_workspace.daemonStatus = DaemonStatus::Attached;
    only_workspace.live         = true;
    SessionCell only_cell;
    only_cell.id    = SessionId{"only-session"};
    only_cell.title = "only-session";
    only_workspace.sessions = {only_cell};
    only.workspaces.emplace(only_workspace.id, only_workspace);
    only.focusSessionIn(only_workspace.id, SessionId{"only-session"});
    WorkspaceHistory only_history;
    only_history.id            = only_workspace.id;
    only_history.title         = "only";
    only_history.canonicalPath = "/only";
    only_history.live          = true;
    only_history.sessions.push_back(
        history_session("only-session", "only-session", 1, "root", "m"));
    only.catalog.workspaces = {only_history};
    only.catalog.loaded     = true;
    only.catalog.generation = 1;
    only.openSwitcher();

    const std::string hidden =
        normalize(render_to_ansi(only, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(hidden);
    EXPECT_NE(hidden.find("only"), std::string::npos);
    EXPECT_NE(hidden.find("(current session hidden)"), std::string::npos);

    UiModel pending = build_model();
    pending.openSwitcher();
    const std::string loading =
        normalize(render_to_ansi(pending, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(loading);
    EXPECT_NE(loading.find("(loading live sessions…"), std::string::npos);
    EXPECT_EQ(loading.find("(no live sessions)"), std::string::npos);

    UiModel noted = build_model();
    WorkspaceHistory noted_history;
    noted_history.id            = noted.activeWorkspaceId;
    noted_history.title         = "workspace";
    noted_history.canonicalPath = "/workspace";
    noted_history.live          = true;
    noted_history.note          = "corrupt";
    noted.catalog.workspaces    = {noted_history};
    noted.catalog.loaded        = true;
    noted.catalog.generation    = 1;
    noted.openSwitcher();
    const std::string failed =
        normalize(render_to_ansi(noted, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(failed);
    EXPECT_NE(failed.find("(corrupt)"), std::string::npos);
    EXPECT_EQ(failed.find("(no live sessions)"), std::string::npos);
}

TEST(UiRenderGolden, UI45_G5_HistoryDistinctHiddenLabel) {
    UiModel empty;
    empty.catalog.workspaces = {history_workspace("ws-empty", "empty", "/empty", false)};
    empty.catalog.loaded     = true;
    empty.catalog.generation = 1;
    empty.switcher.openHistory(empty);
    empty.mode = UiMode::Switcher;
    const std::string empty_rendered =
        normalize(render_to_ansi(empty, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(empty_rendered);
    EXPECT_NE(empty_rendered.find("(no stored sessions)"), std::string::npos);

    UiModel hidden;
    hidden.activeWorkspaceId = WorkspaceId{"ws-hidden"};
    WorkspaceModel hidden_workspace;
    hidden_workspace.id           = hidden.activeWorkspaceId;
    hidden_workspace.title        = "hidden";
    hidden_workspace.cwd          = "/hidden";
    hidden_workspace.daemonStatus = DaemonStatus::Attached;
    hidden_workspace.live         = true;
    hidden.workspaces.emplace(hidden_workspace.id, hidden_workspace);
    hidden.focusSessionIn(hidden_workspace.id, SessionId{"focused"});
    WorkspaceHistory hidden_history =
        history_workspace("ws-hidden", "hidden", "/hidden", true);
    hidden_history.sessions.push_back(history_session("focused", "focused", 1, "root", "m"));
    hidden.catalog.workspaces = {hidden_history};
    hidden.catalog.loaded     = true;
    hidden.catalog.generation = 1;
    hidden.switcher.openHistory(hidden);
    hidden.mode = UiMode::Switcher;
    const std::string hidden_rendered =
        normalize(render_to_ansi(hidden, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(hidden_rendered);
    EXPECT_NE(hidden_rendered.find("(current session hidden)"), std::string::npos);
    EXPECT_EQ(hidden_rendered.find("(no stored sessions)"), std::string::npos);

    UiModel noted;
    WorkspaceHistory noted_history = history_workspace("ws-note", "note", "/note", false);
    noted_history.note            = "corrupt";
    noted.catalog.workspaces      = {noted_history};
    noted.catalog.loaded          = true;
    noted.catalog.generation      = 1;
    noted.switcher.openHistory(noted);
    noted.mode = UiMode::Switcher;
    const std::string noted_rendered =
        normalize(render_to_ansi(noted, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(noted_rendered);
    EXPECT_NE(noted_rendered.find("(corrupt)"), std::string::npos);
    ASSERT_EQ(noted.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(noted.switcher.workspaces[0].historyOnly);
    EXPECT_NE(noted_rendered.find("[history]"), std::string::npos);
}

TEST(UiRenderGolden, UI45_D4_WholeListPlaceholder) {
    UiModel live;
    live.openSwitcher();
    const std::string live_rendered =
        normalize(render_to_ansi(live, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(live_rendered);
    EXPECT_NE(live_rendered.find("(no workspaces)"), std::string::npos);

    UiModel stored;
    stored.catalog.loaded     = true;
    stored.catalog.generation = 1;
    stored.switcher.openHistory(stored);
    stored.mode = UiMode::Switcher;
    const std::string stored_rendered =
        normalize(render_to_ansi(stored, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(stored_rendered);
    EXPECT_NE(stored_rendered.find("(no stored sessions)"), std::string::npos);

    UiModel loading;
    loading.switcher.openHistory(loading);
    loading.mode = UiMode::Switcher;
    const std::string loading_rendered =
        normalize(render_to_ansi(loading, TerminalSize{90, 24}, Theme{false}));
    SCOPED_TRACE(loading_rendered);
    EXPECT_NE(loading_rendered.find("loading stored sessions"), std::string::npos);
}

} // namespace

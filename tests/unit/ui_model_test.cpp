#include <gtest/gtest.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/command_registry.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_application.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"session-1"};

Event make_event(EventType type, const nlohmann::json& payload) {
    Event event;
    event.id = EventId{"event-1"};
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

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/tmp/workspace";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.activeSessionId = kSession;
    model.workspaces.emplace(workspace.id, workspace);
    model.ensureSession(kSession);
    return model;
}

TEST(UiModel, AppliesUserAndStreamingAssistant) {
    UiModel model = make_model();
    model.apply(UiEvent{UserMessage{kSession, "m1", "hello"}});
    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "wo"}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "rld"}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 2u);
    EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::User);
    EXPECT_EQ(state->conversation.entries[0].text, "hello");
    EXPECT_EQ(state->conversation.entries[1].role, ConversationRole::Assistant);
    EXPECT_EQ(state->conversation.entries[1].text, "world");
    EXPECT_TRUE(state->conversation.entries[1].streaming);
    EXPECT_TRUE(any_flag(model.dirty.peek(kSession) & UiDirtyFlag::Conversation));
}

TEST(UiModel, FinishesAssistantMessageWithUsage) {
    UiModel model = make_model();
    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "partial"}});
    Usage usage;
    usage.input_tokens = 10;
    usage.output_tokens = 3;
    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "final text", usage}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->conversation.entries.back().text, "final text");
    EXPECT_FALSE(state->conversation.entries.back().streaming);
    EXPECT_EQ(state->status.input_tokens, 10);
    EXPECT_EQ(state->status.output_tokens, 3);
}

TEST(UiModel, TracksToolCallAndResult) {
    UiModel model = make_model();
    model.apply(UiEvent{ToolStarted{kSession, "t1", "read_file", "{\"path\":\"a\"}"}});
    model.apply(UiEvent{ToolOutput{kSession, "t1", "file-"}});
    model.apply(UiEvent{ToolOutput{kSession, "t1", "body"}});
    model.apply(UiEvent{ToolFinished{kSession, "t1", "read_file", payload::ToolOutcome::Ok, "file-body",
                                     false, std::nullopt}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->tools.calls.size(), 1u);
    EXPECT_EQ(state->tools.calls[0].name, "read_file");
    EXPECT_EQ(state->tools.calls[0].output, "file-body");
    EXPECT_TRUE(state->tools.calls[0].finished);
    ASSERT_EQ(state->conversation.entries.size(), 1u);
    EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::Tool);
    EXPECT_EQ(state->conversation.entries[0].tool_name, "read_file");
    EXPECT_TRUE(any_flag(model.dirty.peek(kSession) & UiDirtyFlag::Tools));
}

TEST(UiModel, PermissionRequestAndResolution) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    PermissionRequest request;
    request.session = kSession;
    request.tool = "shell";
    request.arguments = {{"command", "ls"}};
    const PermissionRequestId id{"p1"};

    adapter.onPermissionRequest(kSession, id, request);
    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.tool, "shell");
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);
    ASSERT_NE(model.session(kSession), nullptr);
    EXPECT_TRUE(model.session(kSession)->attention.needsInput);

    adapter.onPermissionResolved(kSession, id, payload::PermissionDecisionKind::Allow);
    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.aggregate.current.waitingCount, 0u);
    EXPECT_FALSE(model.session(kSession)->attention.needsInput);
}

TEST(UiModel, AggregateCountsAreLevelSnapshot) {
    UiModel model = make_model();
    const SessionId other{"session-2"};
    model.ensureSession(other);
    model.session(kSession)->agent_state = AgentState::Thinking;
    model.session(other)->agent_state = AgentState::WaitingForPermission;

    model.aggregate.recompute(model.workspaces, model.sessions);
    EXPECT_EQ(model.aggregate.current.activeCount, 1u);
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);

    model.session(kSession)->agent_state = AgentState::Idle;
    model.aggregate.recompute(model.workspaces, model.sessions);
    EXPECT_EQ(model.aggregate.current.activeCount, 0u);
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);
}

TEST(UiModel, FlashArmsOnEdgesAndTicks) {
    AggregateStatusModel aggregate;
    aggregate.flash.enabled = true;
    aggregate.armOnEdge(AgentState::Thinking, AgentState::WaitingForPermission);
    EXPECT_TRUE(aggregate.flash.isFlashing());
    aggregate.flash.tick(std::chrono::milliseconds{999});
    EXPECT_TRUE(aggregate.flash.isFlashing());
    aggregate.flash.tick(std::chrono::milliseconds{1});
    EXPECT_EQ(aggregate.flash.phase, FlashPhase::Done);
    EXPECT_FALSE(aggregate.flash.isFlashing());

    aggregate.armOnEdge(AgentState::WaitingForPermission, AgentState::Thinking);
    EXPECT_EQ(aggregate.flash.phase, FlashPhase::Done);

    aggregate.flash.enabled = false;
    aggregate.flash.phase = FlashPhase::Idle;
    aggregate.armOnEdge(AgentState::Thinking, AgentState::Idle);
    EXPECT_EQ(aggregate.flash.phase, FlashPhase::Idle);
}

TEST(UiModel, DirtySetIsolatesSessions) {
    DirtySet dirty;
    const SessionId a{"a"};
    const SessionId b{"b"};
    dirty.mark(b, UiDirtyFlag::Conversation);
    EXPECT_EQ(dirty.peek(a), UiDirtyFlag::None);
    EXPECT_TRUE(any_flag(dirty.peek(b) & UiDirtyFlag::Conversation));
    dirty.markAggregate();
    EXPECT_TRUE(dirty.takeAggregate());
    EXPECT_FALSE(dirty.takeAggregate());
    const std::vector<SessionId> dirty_sessions = dirty.takeDirtySessions();
    ASSERT_EQ(dirty_sessions.size(), 1u);
    EXPECT_EQ(dirty_sessions[0], b);
    EXPECT_EQ(dirty.peek(b), UiDirtyFlag::None);
}

TEST(UiEventAdapter, SynthesizesStartThenDeltas) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    payload::AssistantChunk chunk;
    chunk.message = "a1";
    chunk.text = "hi";
    const Event event = typed_event(EventType::AssistantChunk, chunk);

    const std::vector<UiEvent> first = adapter.adapt(event);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<AssistantMessageStarted>(first[0].value));
    EXPECT_TRUE(std::holds_alternative<AssistantTextDelta>(first[1].value));

    adapter.onEvent(event);
    const std::vector<UiEvent> second = adapter.adapt(event);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<AssistantTextDelta>(second[0].value));
}

TEST(UiEventAdapter, DerivesAgentStateEdges) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    adapter.onEvent(typed_event(EventType::TurnStarted, payload::TurnStarted{}));
    ASSERT_NE(model.session(kSession), nullptr);
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::Thinking);
    EXPECT_EQ(model.aggregate.current.activeCount, 1u);

    adapter.onEvent(typed_event(EventType::ToolCall, [] {
        payload::ToolCall call;
        call.id = "t1";
        call.name = "read_file";
        call.arguments = {{"path", "a"}};
        return call;
    }()));
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::CallingTool);

    adapter.onEvent(typed_event(EventType::TurnEnded, payload::TurnEnded{}));
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::Idle);
    EXPECT_EQ(model.aggregate.current.activeCount, 0u);
}

TEST(UiEventAdapter, RejectsMismatchedEnvelope) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    protocol::SessionEnvelope envelope;
    envelope.session = SessionId{"other"};
    envelope.event = typed_event(EventType::TurnStarted, payload::TurnStarted{});
    adapter.onSessionEnvelope(envelope);
    EXPECT_EQ(model.session(kSession)->agent_state, AgentState::Idle);
}

TEST(TerminalLayer, DisablesIxonAndRestores) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_GE(master, 0);
    ASSERT_EQ(::grantpt(master), 0);
    ASSERT_EQ(::unlockpt(master), 0);
    const char* slave_name = ::ptsname(master);
    ASSERT_NE(slave_name, nullptr);
    const int slave = ::open(slave_name, O_RDWR | O_NOCTTY);
    ASSERT_GE(slave, 0);

    termios initial{};
    ASSERT_EQ(::tcgetattr(slave, &initial), 0);
    initial.c_iflag |= IXON;
    ASSERT_EQ(::tcsetattr(slave, TCSANOW, &initial), 0);
    ASSERT_FALSE(TerminalLayer::ixon_disabled(initial));

    {
        TerminalLayer layer(slave);
        layer.enterRawMode();
        const std::optional<termios> raw = TerminalLayer::snapshot(slave);
        ASSERT_TRUE(raw.has_value());
        EXPECT_TRUE(TerminalLayer::ixon_disabled(*raw));
    }

    const std::optional<termios> restored = TerminalLayer::snapshot(slave);
    ASSERT_TRUE(restored.has_value());
    EXPECT_FALSE(TerminalLayer::ixon_disabled(*restored));

    ::close(slave);
    ::close(master);
}

TEST(UiApplication, RejectsMissingWorkspace) {
    UiRunOptions options;
    options.workspace = "/definitely/not/a/workspace";
    EXPECT_EQ(run_tui(options), 2);
}

TEST(UiModel, MultiWorkspaceSessionsRouteAndCount) {
    UiModel model = make_model();
    const SessionId other{"session-2"};
    model.workspaces[WorkspaceId{"workspace-2"}] = WorkspaceModel{
        WorkspaceId{"workspace-2"}, "beta", "/tmp/beta", "", DaemonStatus::Attached, other, {}};
    model.ensureSessionIn(WorkspaceId{"workspace-2"}, other);

    model.session(kSession)->agent_state = AgentState::Thinking;
    model.session(other)->agent_state = AgentState::WaitingForPermission;
    model.aggregate.recompute(model.workspaces, model.sessions);

    EXPECT_EQ(model.session(other)->workspace, WorkspaceId{"workspace-2"});
    EXPECT_EQ(model.aggregate.current.activeCount, 1u);
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);

    model.aggregate.flash.enabled = true;
    model.aggregate.armOnEdge(AgentState::Thinking, AgentState::WaitingForPermission);
    EXPECT_TRUE(model.aggregate.flash.isFlashing());
}

TEST(UiModel, SwitcherNavigatesAcrossWorkspaces) {
    UiModel model = make_model();
    const SessionId second{"session-2"};
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-2"};
    beta.title = "beta";
    beta.daemonStatus = DaemonStatus::Attached;
    beta.activeSessionId = second;
    SessionCell cell;
    cell.id = second;
    cell.title = "second";
    beta.sessions.push_back(cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-2"}, second);

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 2u);
    EXPECT_EQ(model.switcher.cursor.workspace, model.activeWorkspaceId);

    model.switcher.moveDown();
    EXPECT_EQ(model.switcher.cursor.workspace, WorkspaceId{"workspace-2"});
    EXPECT_FALSE(model.switcher.cursor.session.has_value());

    model.switcher.moveDown();
    ASSERT_TRUE(model.switcher.cursor.session.has_value());
    EXPECT_EQ(*model.switcher.cursor.session, second);

    model.focusSession(second);
    EXPECT_EQ(model.activeWorkspaceId, WorkspaceId{"workspace-2"});
    EXPECT_EQ(model.activeWorkspace()->activeSessionId, second);
    EXPECT_EQ(model.mode, UiMode::Conversation);
}

TEST(UiModel, SwitcherCollapseHidesSessions) {
    UiModel model = make_model();
    SessionCell cell;
    cell.id = kSession;
    cell.title = "one";
    model.workspaces[model.activeWorkspaceId].sessions.push_back(cell);

    model.openSwitcher();
    model.switcher.toggleExpand();
    EXPECT_NE(model.switcher.collapsed.find(model.activeWorkspaceId),
              model.switcher.collapsed.end());
    model.switcher.moveDown();
    EXPECT_FALSE(model.switcher.cursor.session.has_value());
}

TEST(UiModel, AggregateWaitingCountIncludesPermission) {
    UiModel model = make_model();
    model.session(kSession)->agent_state = AgentState::WaitingForPermission;
    model.aggregate.recompute(model.workspaces, model.sessions);
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);
    model.focusSession(kSession);
    model.aggregate.recompute(model.workspaces, model.sessions);
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);
}

TEST(UiModel, InputHistoryPreservesDraft) {
    InputModel input;
    input.push_history("first");
    input.push_history("second");
    input.draft = "in-progress";
    input.cursor = input.draft.size();

    EXPECT_TRUE(input.history_up());
    EXPECT_EQ(input.draft, "second");
    EXPECT_EQ(input.cursor, input.draft.size());
    EXPECT_TRUE(input.history_up());
    EXPECT_EQ(input.draft, "first");
    EXPECT_FALSE(input.history_up());
    EXPECT_EQ(input.draft, "first");

    EXPECT_TRUE(input.history_down());
    EXPECT_EQ(input.draft, "second");
    EXPECT_TRUE(input.history_down());
    EXPECT_EQ(input.draft, "in-progress");
    EXPECT_FALSE(input.history_down());
}

TEST(UiModel, InputEditingHelpers) {
    InputModel input;
    input.draft = "hello world";
    input.cursor = 5;
    EXPECT_TRUE(input.delete_forward());
    EXPECT_EQ(input.draft, "helloworld");

    input.draft = "hello world";
    input.cursor = 11;
    EXPECT_TRUE(input.delete_word());
    EXPECT_EQ(input.draft, "hello ");
    EXPECT_EQ(input.cursor, 6u);
    EXPECT_TRUE(input.delete_word());
    EXPECT_EQ(input.draft, "");

    input.draft = "abc";
    input.cursor = 3;
    input.clear_line();
    EXPECT_TRUE(input.draft.empty());
    EXPECT_EQ(input.cursor, 0u);
}

TEST(UiModel, ConversationScrollFollowAndUnseen) {
    ConversationScroll scroll;
    EXPECT_TRUE(scroll.following);
    EXPECT_FALSE(scroll.unseen);
    EXPECT_FLOAT_EQ(scroll.position(), 1.0f);

    scroll.pageUp();
    EXPECT_FALSE(scroll.following);
    EXPECT_FLOAT_EQ(scroll.position(), 0.8f);
    scroll.onNewContent();
    EXPECT_TRUE(scroll.unseen);

    scroll.pageDown();
    EXPECT_TRUE(scroll.following);
    EXPECT_FALSE(scroll.unseen);
    EXPECT_FLOAT_EQ(scroll.position(), 1.0f);

    scroll.toTop();
    EXPECT_FALSE(scroll.following);
    EXPECT_FLOAT_EQ(scroll.position(), 0.0f);
    scroll.lineUp();
    EXPECT_FLOAT_EQ(scroll.position(), 0.0f);
    scroll.toBottom();
    EXPECT_TRUE(scroll.following);
}

TEST(UiModel, NewContentWhileScrolledRaisesUnseen) {
    UiModel model = make_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->scroll.pageUp();
    EXPECT_FALSE(state->scroll.following);

    model.apply(UiEvent{UserMessage{kSession, "m1", "hi"}});
    EXPECT_TRUE(state->scroll.unseen);

    state->scroll.toBottom();
    model.apply(UiEvent{UserMessage{kSession, "m2", "again"}});
    EXPECT_FALSE(state->scroll.unseen);
}

TEST(UiModel, CommandRegistryDispatchesBuiltins) {
    UiModel model = make_model();
    const CommandRegistry registry = CommandRegistry::builtin();
    bool exited = false;
    bool created = false;
    CommandContext context{model};
    context.session = model.session(kSession);
    context.request_exit = [&exited] { exited = true; };
    context.create_session = [&created] { created = true; };

    EXPECT_FALSE(registry.dispatch("plain text", context));
    EXPECT_TRUE(registry.dispatch("/help", context));
    EXPECT_FALSE(model.session(kSession)->conversation.entries.empty());

    EXPECT_TRUE(registry.dispatch("/clear", context));
    EXPECT_TRUE(model.session(kSession)->conversation.entries.empty());

    EXPECT_TRUE(registry.dispatch("/new", context));
    EXPECT_TRUE(created);

    EXPECT_TRUE(registry.dispatch("/model tiny", context));
    EXPECT_EQ(model.session(kSession)->status.model, "tiny");

    EXPECT_TRUE(registry.dispatch("/bogus", context));
    EXPECT_TRUE(registry.dispatch("/exit", context));
    EXPECT_TRUE(exited);

    const std::vector<const Command*> matches = registry.complete("he");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->name, "help");
}

TEST(UiModel, CommandRegistryDispatchesCompact) {
    UiModel model = make_model();
    const CommandRegistry registry = CommandRegistry::builtin();
    bool compacted = false;
    CommandContext context{model};
    context.session = model.session(kSession);
    context.compact = [&compacted] { compacted = true; };

    EXPECT_TRUE(registry.dispatch("/compact", context));
    EXPECT_TRUE(compacted);

    const std::vector<const Command*> matches = registry.complete("co");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->name, "compact");
    EXPECT_EQ(matches.front()->description, "Summarize history to reclaim context");
}

TEST(UiModel, CommandOutputWhileScrolledRaisesUnseen) {
    UiModel model = make_model();
    const CommandRegistry registry = CommandRegistry::builtin();
    CommandContext context{model};
    context.session = model.session(kSession);
    context.session->scroll.pageUp();
    ASSERT_FALSE(context.session->scroll.following);

    ASSERT_TRUE(registry.dispatch("/help", context));
    EXPECT_TRUE(context.session->scroll.unseen);
}

TEST(UiModel, ReasoningPrecedesAssistantForBothArrivalOrders) {
    UiModel reasoning_first = make_model();
    reasoning_first.apply(UiEvent{AssistantTextDelta{kSession, "a1", "think", true}});
    reasoning_first.apply(UiEvent{AssistantTextDelta{kSession, "a1", "answer"}});
    {
        const SessionUiState* state = reasoning_first.session(kSession);
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->conversation.entries.size(), 2u);
        EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::Reasoning);
        EXPECT_EQ(state->conversation.entries[0].text, "think");
        EXPECT_EQ(state->conversation.entries[1].role, ConversationRole::Assistant);
        EXPECT_EQ(state->conversation.entries[1].text, "answer");
        EXPECT_EQ(state->conversation.find_message("a1"), 1u);
        EXPECT_EQ(state->conversation.find_reasoning_message("a1"), 0u);
    }

    UiModel text_first = make_model();
    text_first.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    text_first.apply(UiEvent{AssistantTextDelta{kSession, "a1", "answer"}});
    text_first.apply(UiEvent{AssistantTextDelta{kSession, "a1", "think", true}});
    {
        const SessionUiState* state = text_first.session(kSession);
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->conversation.entries.size(), 2u);
        EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::Reasoning);
        EXPECT_EQ(state->conversation.entries[0].text, "think");
        EXPECT_EQ(state->conversation.entries[1].role, ConversationRole::Assistant);
        EXPECT_EQ(state->conversation.entries[1].text, "answer");
        EXPECT_EQ(state->conversation.find_message("a1"), 1u);
        EXPECT_EQ(state->conversation.find_reasoning_message("a1"), 0u);
    }
}

TEST(UiModel, ReasoningDeltasCoalesceAndFinishClearsBoth) {
    UiModel model = make_model();
    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "th", true}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "ink", true}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "answer"}});
    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "answer", std::nullopt}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 2u);
    EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::Reasoning);
    EXPECT_EQ(state->conversation.entries[0].text, "think");
    EXPECT_FALSE(state->conversation.entries[0].streaming);
    EXPECT_EQ(state->conversation.entries[1].role, ConversationRole::Assistant);
    EXPECT_EQ(state->conversation.entries[1].text, "answer");
    EXPECT_FALSE(state->conversation.entries[1].streaming);
}

TEST(UiModel, LongestCommonPrefixOverZeroOneAndManyMatches) {
    const CommandRegistry registry = CommandRegistry::builtin();
    EXPECT_EQ(CommandRegistry::longest_common_prefix({}), std::string{});

    const std::vector<const Command*> one = registry.complete("he");
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(CommandRegistry::longest_common_prefix(one), "help");

    const std::vector<const Command*> many = registry.complete("c");
    ASSERT_EQ(many.size(), 2u);
    EXPECT_EQ(CommandRegistry::longest_common_prefix(many), "c");
}

TEST(UiModel, CompletionCycleResetsOnDraftMutation) {
    InputModel input;
    input.draft = "/c";
    input.cursor = 2;
    input.completion = CompletionCycle{"/c", {"clear", "compact"}, 0};
    EXPECT_FALSE(input.delete_forward());
    EXPECT_TRUE(input.completion.has_value());

    input.history.push_back("/help");
    input.history_pos = 1;
    ASSERT_TRUE(input.history_up());
    EXPECT_FALSE(input.completion.has_value());
    EXPECT_EQ(input.draft, "/help");

    input.draft = "/c";
    input.cursor = 2;
    input.completion = CompletionCycle{"/c", {"clear", "compact"}, 0};
    input.clear_line();
    EXPECT_FALSE(input.completion.has_value());

    input.draft = "/clear now";
    input.cursor = input.draft.size();
    input.completion = CompletionCycle{"/clear now", {"clear"}, 0};
    ASSERT_TRUE(input.delete_word());
    EXPECT_FALSE(input.completion.has_value());
}

TEST(UiModel, ClearResetsReasoningIndex) {
    UiModel model = make_model();
    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "think", true}});

    CommandRegistry registry = CommandRegistry::builtin();
    CommandContext context{model};
    context.session = model.session(kSession);
    ASSERT_TRUE(registry.dispatch("/clear", context));

    model.apply(UiEvent{AssistantTextDelta{kSession, "a1", "again", true}});
    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 1u);
    EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::Reasoning);
    EXPECT_EQ(state->conversation.entries[0].text, "again");
}

TEST(UiModel, SetCellTitlePopulatesCell) {
    UiModel model = make_model();
    model.setCellTitle(WorkspaceId{"workspace"}, kSession, "my title");

    const auto workspace = model.workspaces.find(WorkspaceId{"workspace"});
    ASSERT_NE(workspace, model.workspaces.end());
    ASSERT_FALSE(workspace->second.sessions.empty());
    EXPECT_EQ(workspace->second.sessions.front().title, "my title");
}

} // namespace

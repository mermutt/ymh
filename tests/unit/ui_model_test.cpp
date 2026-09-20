#include <gtest/gtest.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/command_registry.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"session-1"};

template <typename T, typename = void>
struct HasCompletionMember : std::false_type {};
template <typename T>
struct HasCompletionMember<T, std::void_t<decltype(std::declval<T&>().completion)>>
    : std::true_type {};

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
    workspace.live = true;
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

TEST(UiModel, ComputesTpsFromInjectedClock) {
    UiModel model = make_model();
    auto    now   = std::chrono::steady_clock::time_point{};
    model.set_now_reader([&now] { return now; });

    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    now += std::chrono::seconds(2);
    Usage usage;
    usage.output_tokens = 100;
    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "done", usage}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->status.tps.has_value());
    EXPECT_DOUBLE_EQ(*state->status.tps, 50.0);
    EXPECT_FALSE(state->stream_started_at.has_value());
}

TEST(UiModel, TpsSkipsShortAndEmptyResponses) {
    UiModel model = make_model();
    auto    now   = std::chrono::steady_clock::time_point{};
    model.set_now_reader([&now] { return now; });

    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    now += std::chrono::milliseconds(100);
    Usage usage;
    usage.output_tokens = 100;
    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "done", usage}});
    EXPECT_FALSE(model.session(kSession)->status.tps.has_value());

    model.apply(UiEvent{AssistantMessageStarted{kSession, "a2"}});
    now += std::chrono::seconds(2);
    Usage empty;
    empty.output_tokens = 0;
    model.apply(UiEvent{AssistantMessageFinished{kSession, "a2", "done", empty}});
    EXPECT_FALSE(model.session(kSession)->status.tps.has_value());
}

TEST(UiModel, PlanModeChangedSetsStatusAndDirty) {
    UiModel model = make_model();
    static_cast<void>(model.dirty.takeDirtySessions());
    model.apply(UiEvent{PlanModeChanged{kSession, true}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->status.plan_active);
    EXPECT_TRUE(any_flag(model.dirty.peek(kSession) & UiDirtyFlag::Status));

    model.apply(UiEvent{PlanModeChanged{kSession, false}});
    EXPECT_FALSE(state->status.plan_active);
}

// UX-U32 (N6/L4): a `session.set_mode` reply is never authoritative. A pending
// notice must not change the status; only a log-derived PlanModeChanged flips it.
TEST(UiModel, UX_U32_SetModeReplyIsNotAuthoritative) {
    UiModel model = make_model();
    static_cast<void>(model.dirty.takeDirtySessions());

    model.pushNotice("plan change queued");
    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(state->status.plan_active);

    model.apply(UiEvent{PlanModeChanged{kSession, true}});
    EXPECT_TRUE(model.session(kSession)->status.plan_active);
}

TEST(UiModel, ForceAskDialogDefaultsToDenyWithTwoOptions) {
    UiModel model = make_model();
    PermissionRequested requested;
    requested.session   = kSession;
    requested.request   = PermissionRequestId{"r1"};
    requested.tool      = "exit_plan_mode";
    requested.summary   = "the plan";
    requested.force_ask = true;
    model.apply(UiEvent{std::move(requested)});

    EXPECT_TRUE(model.dialog.open);
    EXPECT_TRUE(model.dialog.force_ask);
    EXPECT_EQ(model.dialog.selected, 1);
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
    beta.live = true;
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

    // 18 §4.1 adds `/context`, so the "co" prefix now completes both commands.
    const std::vector<const Command*> matches = registry.complete("co");
    ASSERT_EQ(matches.size(), 2u);
    const Command* compact = registry.find("compact");
    ASSERT_NE(compact, nullptr);
    EXPECT_EQ(compact->description, "Summarize history to reclaim context");
    EXPECT_NE(registry.find("context"), nullptr);
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
    ASSERT_EQ(many.size(), 3u);
    EXPECT_EQ(CommandRegistry::longest_common_prefix(many), "c");
}

TEST(UiModel, UI45_D2_CompletionCycleRetired) {
    // 45-I4 (compile-level): `InputModel::completion` and `CompletionCycle` are
    // gone; the detection idiom below fails to compile if either returns.
    static_assert(!HasCompletionMember<InputModel>::value,
                  "InputModel::completion must be retired (45-D2/45-I4)");
    SUCCEED();
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

TEST(UiModel, SessionTitleChangedUpdatesCellAndMarks) {
    UiModel model = make_model();
    model.setCellTitle(WorkspaceId{"workspace"}, kSession, "tui");
    model.apply(UiEvent{SessionTitleChanged{kSession, "renamed"}});

    const auto workspace = model.workspaces.find(WorkspaceId{"workspace"});
    ASSERT_NE(workspace, model.workspaces.end());
    ASSERT_FALSE(workspace->second.sessions.empty());
    EXPECT_EQ(workspace->second.sessions.front().title, "renamed");
    const UiDirtyFlag flags = model.dirty.peek(kSession);
    EXPECT_TRUE(any_flag(flags & UiDirtyFlag::Layout));
    EXPECT_TRUE(any_flag(flags & UiDirtyFlag::SessionBar));
}

TEST(UiModel, ErrorOccurredAppendsSystemEntryAndLeavesTitle) {
    UiModel model = make_model();
    model.setCellTitle(WorkspaceId{"workspace"}, kSession, "keep me");
    model.apply(UiEvent{ErrorOccurred{kSession, "rename failed: InvalidParams"}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 1u);
    EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::System);
    EXPECT_EQ(state->conversation.entries[0].text, "error: rename failed: InvalidParams");
    EXPECT_EQ(state->status.last_error, "rename failed: InvalidParams");
    const UiDirtyFlag flags = model.dirty.peek(kSession);
    EXPECT_TRUE(any_flag(flags & UiDirtyFlag::Conversation));
    EXPECT_TRUE(any_flag(flags & UiDirtyFlag::Status));
    EXPECT_TRUE(any_flag(flags & UiDirtyFlag::Attention));

    const auto workspace = model.workspaces.find(WorkspaceId{"workspace"});
    ASSERT_NE(workspace, model.workspaces.end());
    EXPECT_EQ(workspace->second.sessions.front().title, "keep me");
}

TEST(UiModel, CommandRegistryDispatchesRename) {
    UiModel model = make_model();
    const CommandRegistry registry = CommandRegistry::builtin();
    std::string renamed;
    CommandContext context{model};
    context.session = model.session(kSession);
    context.rename_session = [&renamed](const std::string& title) { renamed = title; };

    EXPECT_TRUE(registry.dispatch("/rename", context));
    EXPECT_TRUE(renamed.empty());
    ASSERT_FALSE(model.session(kSession)->conversation.entries.empty());
    EXPECT_NE(model.session(kSession)->conversation.entries.back().text.find("usage"),
              std::string::npos);

    EXPECT_TRUE(registry.dispatch("/rename my title", context));
    EXPECT_EQ(renamed, "my title");

    const std::vector<const Command*> matches = registry.complete("re");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->name, "rename");
}

TEST(UiModel, CommandRegistryRenameWithoutSessionMakesNoCall) {
    UiModel model = make_model();
    const CommandRegistry registry = CommandRegistry::builtin();
    bool called = false;
    CommandContext context{model};
    context.session = nullptr;
    context.rename_session = [&called](const std::string&) { called = true; };

    EXPECT_TRUE(registry.dispatch("/rename title", context));
    EXPECT_FALSE(called);
}

TEST(UiModel, WorkspaceEventCarriesSessionId) {
    WorkspaceEvent event;
    event.workspace = WorkspaceId{"workspace"};
    event.kind = WorkspaceEventKind::SessionOpened;
    event.session = SessionId{"peer-session"};

    EXPECT_TRUE(event.session.has_value());
    EXPECT_EQ(*event.session, SessionId{"peer-session"});
}

TEST(UiModel, SessionOpenedInsertsCellAndIsIdempotent) {
    UiModel model = make_model();
    const SessionId peer{"peer-session"};
    WorkspaceEvent event;
    event.workspace = WorkspaceId{"workspace"};
    event.kind = WorkspaceEventKind::SessionOpened;
    event.session = peer;

    model.apply(event);
    model.apply(event);

    const auto workspace = model.workspaces.find(WorkspaceId{"workspace"});
    ASSERT_NE(workspace, model.workspaces.end());
    std::size_t matches = 0;
    for (const SessionCell& cell : workspace->second.sessions) {
        if (cell.id == peer) {
            ++matches;
        }
    }
    EXPECT_EQ(matches, 1u) << "double-applied SessionCreated must be idempotent";
    EXPECT_NE(model.session(peer), nullptr);
    EXPECT_EQ(model.session(peer)->workspace, WorkspaceId{"workspace"});
}

TEST(UiModel, SessionClosedErasesCellAndState) {
    UiModel model = make_model();
    const SessionId peer{"peer-session"};
    WorkspaceEvent opened;
    opened.workspace = WorkspaceId{"workspace"};
    opened.kind = WorkspaceEventKind::SessionOpened;
    opened.session = peer;
    model.apply(opened);

    WorkspaceEvent closed;
    closed.workspace = WorkspaceId{"workspace"};
    closed.kind = WorkspaceEventKind::SessionClosed;
    closed.session = peer;
    model.apply(closed);

    const auto workspace = model.workspaces.find(WorkspaceId{"workspace"});
    ASSERT_NE(workspace, model.workspaces.end());
    for (const SessionCell& cell : workspace->second.sessions) {
        EXPECT_NE(cell.id, peer);
    }
    EXPECT_EQ(model.session(peer), nullptr);
}

TEST(UiModel, SessionClosedClearsActiveSession) {
    UiModel model = make_model();
    WorkspaceEvent closed;
    closed.workspace = WorkspaceId{"workspace"};
    closed.kind = WorkspaceEventKind::SessionClosed;
    closed.session = kSession;
    model.apply(closed);

    EXPECT_TRUE(model.workspaces.at(WorkspaceId{"workspace"}).activeSessionId.value.empty());
    EXPECT_EQ(model.session(kSession), nullptr);
}

TEST(UiModel, OwnershipMarkDerivesFromDaemonStatus) {
    EXPECT_EQ(ownership_mark(DaemonStatus::Attached), OwnershipMark::Owned);
    EXPECT_EQ(ownership_mark(DaemonStatus::Stopping), OwnershipMark::Stopping);
    EXPECT_EQ(ownership_mark(DaemonStatus::NotRunning), OwnershipMark::NotRunning);
    EXPECT_EQ(ownership_mark(DaemonStatus::Connecting), OwnershipMark::Unreachable);
    EXPECT_EQ(ownership_mark(DaemonStatus::Detached), OwnershipMark::Unreachable);
    EXPECT_EQ(ownership_mark(DaemonStatus::Dead), OwnershipMark::Unreachable);
}

TEST(UiModel, SwitcherNodesCarryOwnershipMark) {
    UiModel model = make_model();
    const SessionId second{"session-2"};
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-2"};
    beta.title = "beta";
    beta.daemonStatus = DaemonStatus::Attached;
    beta.live = true;
    beta.activeSessionId = second;
    SessionCell cell;
    cell.id = second;
    cell.title = "second";
    beta.sessions.push_back(cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-2"}, second);

    WorkspaceModel dead;
    dead.id = WorkspaceId{"workspace-dead"};
    dead.title = "dead";
    dead.daemonStatus = DaemonStatus::Dead;
    dead.live = true;
    model.workspaces.emplace(dead.id, std::move(dead));

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 2u);
    for (const WorkspaceNode& node : model.switcher.workspaces) {
        EXPECT_EQ(node.mark, OwnershipMark::Owned);
        EXPECT_NE(node.id, WorkspaceId{"workspace-dead"});
    }
}

// SW-U1 (22 §3.1, SW1/SW20): the Live source copies only renderable workspaces.
TEST(UiModel, SwitcherOpenIsLiveOnly) {
    UiModel model = make_model();
    const auto add = [&model](const char* id, const char* title, DaemonStatus status, bool live) {
        WorkspaceModel workspace;
        workspace.id = WorkspaceId{id};
        workspace.title = title;
        workspace.cwd = std::string{"/work/"} + id;
        workspace.daemonStatus = status;
        workspace.live = live;
        model.workspaces.emplace(workspace.id, std::move(workspace));
    };
    add("ws-attached", "attached", DaemonStatus::Attached, true);
    add("ws-stopping", "stopping", DaemonStatus::Stopping, true);
    add("ws-connecting", "connecting", DaemonStatus::Connecting, true);
    add("ws-detached", "detached", DaemonStatus::Detached, true);
    add("ws-dead", "dead", DaemonStatus::Dead, true);
    add("ws-notrunning", "notrunning", DaemonStatus::NotRunning, true);
    add("ws-hidden", "hidden", DaemonStatus::Attached, false);

    model.openSwitcher();
    std::set<WorkspaceId> ids;
    for (const WorkspaceNode& node : model.switcher.workspaces) {
        ids.insert(node.id);
        EXPECT_NE(node.mark, OwnershipMark::Unreachable);
    }
    EXPECT_EQ(ids.count(WorkspaceId{"workspace"}), 1u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-attached"}), 1u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-stopping"}), 1u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-connecting"}), 0u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-detached"}), 0u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-dead"}), 0u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-notrunning"}), 0u);
    EXPECT_EQ(ids.count(WorkspaceId{"ws-hidden"}), 0u);
}

// SW-U2 (22 §3.5): eraseWorkspace removes the workspace + its sessions, repairs
// focus, and is a no-op for an unknown id.
TEST(UiModel, EraseWorkspaceRemovesSessionsAndRepairsFocus) {
    UiModel model = make_model();
    const SessionId other{"session-2"};
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-2"};
    beta.title = "beta";
    beta.daemonStatus = DaemonStatus::Attached;
    beta.live = true;
    beta.activeSessionId = other;
    model.workspaces.emplace(beta.id, beta);
    model.ensureSessionIn(beta.id, other);

    model.eraseWorkspace(WorkspaceId{"workspace"});
    EXPECT_EQ(model.workspaces.count(WorkspaceId{"workspace"}), 0u);
    EXPECT_EQ(model.session(kSession), nullptr);
    EXPECT_EQ(model.activeWorkspaceId, WorkspaceId{"workspace-2"});

    model.dirty.clear();
    model.eraseWorkspace(WorkspaceId{"nope"});
    EXPECT_FALSE(model.dirty.hasAny());

    model.eraseWorkspace(WorkspaceId{"workspace-2"});
    EXPECT_TRUE(model.dirty.hasAny());
    EXPECT_TRUE(model.activeWorkspaceId.value.empty());
}

// SW-U3 (22 §3.2): the pure eviction decision retains live and Connecting
// workspaces and selects the absent, non-Connecting ones.
TEST(UiModel, SwitcherEvictionCandidates) {
    UiModel model = make_model();
    WorkspaceModel dead;
    dead.id = WorkspaceId{"ws-dead"};
    dead.daemonStatus = DaemonStatus::Dead;
    model.workspaces.emplace(dead.id, dead);
    WorkspaceModel connecting;
    connecting.id = WorkspaceId{"ws-connecting"};
    connecting.daemonStatus = DaemonStatus::Connecting;
    model.workspaces.emplace(connecting.id, connecting);

    const std::set<WorkspaceId> live_ids{WorkspaceId{"workspace"}};
    const std::set<WorkspaceId> connecting_ids{WorkspaceId{"ws-connecting"}};
    const std::vector<WorkspaceId> doomed =
        switcher_eviction_candidates(model.workspaces, live_ids, connecting_ids);
    ASSERT_EQ(doomed.size(), 1u);
    EXPECT_EQ(doomed[0], WorkspaceId{"ws-dead"});
}

// SW-U9 (22 §3.1): a dead link clears `live` at once, hiding the workspace from
// a Live open before any eviction.
TEST(UiModel, DaemonDeathHidesWorkspace) {
    WorkspaceModel workspace;
    workspace.id = WorkspaceId{"ws"};
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    apply_daemon_status_liveness(workspace, DaemonStatus::Dead);
    EXPECT_FALSE(workspace.live);
    apply_daemon_status_liveness(workspace, DaemonStatus::Connecting);
    EXPECT_FALSE(workspace.live);
    apply_daemon_status_liveness(workspace, DaemonStatus::Attached);
    EXPECT_TRUE(workspace.live);
    apply_daemon_status_liveness(workspace, DaemonStatus::Stopping);
    EXPECT_TRUE(workspace.live);

    UiModel model = make_model();
    WorkspaceEvent died;
    died.workspace = model.activeWorkspaceId;
    died.kind = WorkspaceEventKind::DaemonDied;
    model.apply(died);
    model.openSwitcher();
    EXPECT_TRUE(model.switcher.workspaces.empty());
}

// SW-U11 (22 §3.6, H1): the notice ring is bounded and marks the model dirty.
TEST(UiModel, NoticeRingIsBounded) {
    UiModel model = make_model();
    model.dirty.clear();
    model.pushNotice("first");
    ASSERT_EQ(model.notices.size(), 1u);
    EXPECT_EQ(model.notices.back().text, "first");
    EXPECT_TRUE(model.dirty.hasAny());

    for (std::size_t index = 0; index < kMaxNotices + 3; ++index) {
        model.pushNotice("n" + std::to_string(index));
    }
    EXPECT_EQ(model.notices.size(), kMaxNotices);
    EXPECT_EQ(model.notices.front().text, "n3");
}

// SW-U15 (22 §3.7): case-insensitive title order with a cwd tie-break; the Live
// source preserves the daemon's session.list order.
TEST(UiModel, SwitcherOrderingAndSessionOrder) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws-b"};
    const auto add = [&model](const char* id, const char* title, const char* cwd) {
        WorkspaceModel workspace;
        workspace.id = WorkspaceId{id};
        workspace.title = title;
        workspace.cwd = cwd;
        workspace.daemonStatus = DaemonStatus::Attached;
        workspace.live = true;
        model.workspaces.emplace(workspace.id, std::move(workspace));
    };
    add("ws-b", "same", "/b");
    add("ws-a", "same", "/a");
    add("ws-g", "Gamma", "/g");
    add("ws-beta", "beta", "/beta");

    SessionCell second;
    second.id = SessionId{"s2"};
    second.title = "two";
    SessionCell first;
    first.id = SessionId{"s1"};
    first.title = "one";
    model.workspaces[WorkspaceId{"ws-b"}].sessions = {second, first};

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 4u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-g"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-a"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-b"});
    ASSERT_EQ(model.switcher.workspaces[3].sessions.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[3].sessions[0].id, SessionId{"s2"});
    EXPECT_EQ(model.switcher.workspaces[3].sessions[1].id, SessionId{"s1"});

    // History source (22 §3.7/SW17): the same workspace rule; sessions are
    // `updated_at` desc, `id` asc. The Live source above was NOT re-sorted.
    const auto add_history = [&model](const char* id, const char* title, const char* path) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.title = title;
        history.canonicalPath = path;
        history.live = false;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_history("ws-b", "same", "/b");
    add_history("ws-a", "same", "/a");
    add_history("ws-g", "Gamma", "/g");
    add_history("ws-beta", "beta", "/beta");
    model.catalog.loaded = true;

    SessionHistoryEntry older;
    older.id = SessionId{"s1"};
    older.updatedAt = 100;
    SessionHistoryEntry newer;
    newer.id = SessionId{"s2"};
    newer.updatedAt = 200;
    model.catalog.workspaces[0].sessions = {older, newer};

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 4u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-g"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-a"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-b"});
    ASSERT_EQ(model.switcher.workspaces[3].sessions.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[3].sessions[0].id, SessionId{"s2"});
    EXPECT_EQ(model.switcher.workspaces[3].sessions[1].id, SessionId{"s1"});
}

// SW-U16 (22 §3.6, L1): openSwitcher resets the source to Live; close leaves it.
TEST(UiModel, SwitcherSourceResetsToLive) {
    UiModel model = make_model();
    model.openSwitcher();
    EXPECT_EQ(model.switcher.source, SwitcherSource::Live);
    model.switcher.source = SwitcherSource::History;
    model.switcher.close();
    EXPECT_EQ(model.switcher.source, SwitcherSource::History);
    model.openSwitcher();
    EXPECT_EQ(model.switcher.source, SwitcherSource::Live);
}

// SW-U17 (22 §3.3, L3): the cursor is validated against the built node list.
TEST(UiModel, SwitcherCursorValidatedAgainstNodes) {
    UiModel model = make_model();
    WorkspaceModel hidden;
    hidden.id = WorkspaceId{"ws-hidden"};
    hidden.title = "hidden";
    hidden.cwd = "/hidden";
    hidden.daemonStatus = DaemonStatus::Dead;
    hidden.live = true;
    model.workspaces.emplace(hidden.id, hidden);
    model.ensureSessionIn(hidden.id, SessionId{"hidden-session"});

    model.switcher.cursor.workspace = WorkspaceId{"ws-hidden"};
    model.switcher.cursor.session = SessionId{"hidden-session"};
    model.switcher.open(model);
    EXPECT_EQ(model.switcher.cursor.workspace, model.activeWorkspaceId);
    EXPECT_EQ(model.switcher.cursor.session, std::optional<SessionId>{kSession});

    model.switcher.cursor.session = SessionId{"bogus"};
    model.switcher.open(model);
    EXPECT_EQ(model.switcher.cursor.workspace, model.activeWorkspaceId);
    EXPECT_FALSE(model.switcher.cursor.session.has_value());

    model.activeWorkspaceId = WorkspaceId{"ws-hidden"};
    model.switcher.cursor = SwitcherCursor{};
    model.switcher.open(model);
    ASSERT_FALSE(model.switcher.workspaces.empty());
    EXPECT_EQ(model.switcher.cursor.workspace, model.switcher.workspaces.front().id);
}

// SW-U6 (22 §3.7/§4.3): openHistory builds nodes from `model.catalog`; a
// non-live workspace is `historyOnly` and carries its note; History sessions
// are `updated_at` desc / `id` asc; workspace groups are title-ascending
// (case-insensitive) with a canonical_path tie-break.
TEST(UiModel, SwitcherOpenHistoryBuildsFromCatalog) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws-live"};

    WorkspaceHistory live;
    live.id = WorkspaceId{"ws-live"};
    live.title = "Live";
    live.canonicalPath = "/live";
    live.live = true;
    SessionHistoryEntry newest;
    newest.id = SessionId{"s-new"};
    newest.title = "new";
    newest.kind = "root";
    newest.model = "m1";
    newest.updatedAt = 5000;
    SessionHistoryEntry oldest;
    oldest.id = SessionId{"s-old"};
    oldest.title = "old";
    oldest.kind = "fork";
    oldest.model = "m2";
    oldest.updatedAt = 1000;
    SessionHistoryEntry middle;
    middle.id = SessionId{"s-mid"};
    middle.title = "mid";
    middle.kind = "subagent";
    middle.model = "m3";
    middle.updatedAt = 5000;
    live.sessions = {oldest, newest, middle};

    WorkspaceHistory stopped;
    stopped.id = WorkspaceId{"ws-stop"};
    stopped.title = "Stopped";
    stopped.canonicalPath = "/stopped";
    stopped.live = false;
    stopped.note = "corrupt";

    WorkspaceHistory tie_b;
    tie_b.id = WorkspaceId{"ws-tie-b"};
    tie_b.title = "Tie";
    tie_b.canonicalPath = "/b";
    tie_b.live = false;
    WorkspaceHistory tie_a;
    tie_a.id = WorkspaceId{"ws-tie-a"};
    tie_a.title = "Tie";
    tie_a.canonicalPath = "/a";
    tie_a.live = false;

    model.catalog.workspaces = {live, stopped, tie_b, tie_a};
    model.catalog.loaded = true;

    model.switcher.openHistory(model);
    EXPECT_EQ(model.switcher.source, SwitcherSource::History);

    ASSERT_EQ(model.switcher.workspaces.size(), 4u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-live"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-stop"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-tie-a"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-tie-b"});

    const WorkspaceNode& stopped_node = model.switcher.workspaces[1];
    EXPECT_TRUE(stopped_node.historyOnly);
    EXPECT_FALSE(stopped_node.live);
    ASSERT_TRUE(stopped_node.note.has_value());
    EXPECT_EQ(*stopped_node.note, "corrupt");
    EXPECT_TRUE(stopped_node.sessions.empty());

    const WorkspaceNode& live_node = model.switcher.workspaces[0];
    EXPECT_FALSE(live_node.historyOnly);
    EXPECT_TRUE(live_node.live);
    EXPECT_FALSE(live_node.note.has_value());
    ASSERT_EQ(live_node.sessions.size(), 3u);
    EXPECT_EQ(live_node.sessions[0].id, SessionId{"s-mid"});
    EXPECT_EQ(live_node.sessions[1].id, SessionId{"s-new"});
    EXPECT_EQ(live_node.sessions[2].id, SessionId{"s-old"});
    EXPECT_TRUE(live_node.sessions[0].fromDisk);
    EXPECT_EQ(live_node.sessions[2].kind, "fork");
    EXPECT_EQ(live_node.sessions[2].model, "m2");
    EXPECT_EQ(live_node.sessions[2].updatedAt, 1000);
}

// SW-U7 (22 §4.3/§4.6): `/sessions` dispatch opens the History source in
// Switcher mode; a context without the callback is a no-op; before the first
// snapshot the render shows the loading placeholder.
TEST(UiModel, SessionsCommandOpensHistoryAndLoadingPlaceholder) {
    CommandRegistry registry = CommandRegistry::builtin();
    UiModel model = make_model();

    CommandContext unset{model};
    EXPECT_TRUE(registry.dispatch("/sessions", unset));
    EXPECT_EQ(model.mode, UiMode::Conversation);
    EXPECT_EQ(model.switcher.source, SwitcherSource::Live);

    CommandContext context{model};
    bool called = false;
    context.sessions = [&] {
        called = true;
        model.switcher.source = SwitcherSource::History;
        model.switcher.openHistory(model);
        model.mode = UiMode::Switcher;
    };
    EXPECT_TRUE(registry.dispatch("/sessions", context));
    EXPECT_TRUE(called);
    EXPECT_EQ(model.switcher.source, SwitcherSource::History);
    EXPECT_EQ(model.mode, UiMode::Switcher);
    EXPECT_FALSE(model.catalog.loaded);

    const std::string rendered =
        render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("loading stored sessions"), std::string::npos);
}

} // namespace

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

} // namespace

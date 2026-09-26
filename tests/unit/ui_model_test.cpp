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
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, kSession);
    model.dirty.clear();
    return model;
}

void add_catalog_sessions(UiModel& model, const WorkspaceId& workspace,
                          std::initializer_list<SessionId> sessions) {
    WorkspaceHistory history;
    history.id = workspace;
    history.title = workspace.value;
    history.canonicalPath = "/" + workspace.value;
    for (const SessionId& session : sessions) {
        SessionHistoryEntry entry;
        entry.id = session;
        entry.title = session.value;
        entry.kind = "root";
        entry.model = "m";
        entry.updatedAt = 1;
        history.sessions.push_back(std::move(entry));
    }
    model.catalog.workspaces.push_back(std::move(history));
    model.catalog.loaded = true;
    model.catalog.generation = 1;
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
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-2"};
    beta.title = "beta";
    beta.cwd = "/tmp/beta";
    beta.daemonStatus = DaemonStatus::Attached;
    model.workspaces.emplace(beta.id, std::move(beta));
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
    SessionCell cell;
    cell.id = second;
    cell.title = "second";
    beta.sessions.push_back(cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-2"}, second);
    add_catalog_sessions(model, WorkspaceId{"workspace"}, {kSession});
    add_catalog_sessions(model, WorkspaceId{"workspace-2"}, {second});

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
    EXPECT_EQ(model.activeWorkspace()->activeSessionId(), second);
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

TEST(UiModel, InputWordBoundaries) {
    const auto left = [](const std::string& text, std::size_t cursor) {
        InputModel input;
        input.draft  = text;
        input.cursor = cursor;
        return input.word_left_boundary(cursor);
    };
    const auto right = [](const std::string& text, std::size_t cursor) {
        InputModel input;
        input.draft  = text;
        input.cursor = cursor;
        return input.word_right_boundary(cursor);
    };

    EXPECT_EQ(left("", 0), 0u);
    EXPECT_EQ(right("", 0), 0u);
    EXPECT_EQ(left("a", 1), 0u);
    EXPECT_EQ(right("a", 0), 1u);
    EXPECT_EQ(left("ab cd", 5), 3u);
    EXPECT_EQ(right("ab cd", 0), 2u);
    EXPECT_EQ(left("a.b", 3), 2u);
    EXPECT_EQ(right("a.b", 0), 1u);
    EXPECT_EQ(left("a  b", 4), 3u);
    EXPECT_EQ(right("a  b", 0), 1u);
    EXPECT_EQ(left("  ab", 4), 2u);
    EXPECT_EQ(right("  ab", 0), 4u);
    EXPECT_EQ(left("ab  ", 4), 0u);
    EXPECT_EQ(right("ab  ", 0), 2u);
    EXPECT_EQ(left("a_b-c", 5), 4u);
    EXPECT_EQ(right("a_b-c", 0), 3u);
    EXPECT_EQ(right("a_b-c", 3), 4u);
    EXPECT_EQ(left("foo.bar", 7), 4u);
    // Multi-byte glyphs are atomic Punct runs (48-F4).
    EXPECT_EQ(left("\xC3\xA9", 2), 0u);
    EXPECT_EQ(right("\xC3\xA9", 0), 2u);
    // Invariant 48-I6 for every cursor position.
    const std::string sample = "alpha.beta  gamma";
    for (std::size_t cursor = 0; cursor <= sample.size(); ++cursor) {
        EXPECT_LE(left(sample, cursor), cursor);
        EXPECT_LE(cursor, right(sample, cursor));
    }
}

TEST(UiModel, InputGlyphMotionAndSnap) {
    InputModel input;
    input.draft = "a\xC3\xA9";   // 'a' + U+00E9 (2 bytes)
    EXPECT_EQ(input.cursor_right(0), 1u);
    EXPECT_EQ(input.cursor_right(1), 3u);
    EXPECT_EQ(input.cursor_right(3), 3u);
    EXPECT_EQ(input.cursor_left(3), 1u);
    EXPECT_EQ(input.cursor_left(1), 0u);
    EXPECT_EQ(input.cursor_left(0), 0u);

    input.draft = "\xF0\x9F\x98\x80";   // U+1F600 (4 bytes)
    EXPECT_EQ(input.cursor_right(0), 4u);
    EXPECT_EQ(input.cursor_left(4), 0u);

    EXPECT_EQ(glyph_floor("a\xC3\xA9", 2), 3u);
    EXPECT_EQ(glyph_floor("a\xC3\xA9", 1), 1u);
    EXPECT_EQ(glyph_floor("a\xC3\xA9", 3), 3u);
    EXPECT_EQ(glyph_len("a\xC3\xA9", 0), 1u);
    EXPECT_EQ(glyph_len("a\xC3\xA9", 1), 2u);
    EXPECT_EQ(glyph_len("a\xC3\xA9", 3), 0u);
    EXPECT_EQ(glyph_at("a\xC3\xA9", 1), std::string_view("\xC3\xA9"));
    EXPECT_TRUE(glyph_at("a", 1).empty());
}

TEST(UiModel, EntryPresentationTable) {
    const auto entry = [](ConversationRole role) {
        ConversationEntry value;
        value.role = role;
        return value;
    };
    std::vector<ConversationEntry> entries = {
        entry(ConversationRole::User),
        entry(ConversationRole::Assistant),
        entry(ConversationRole::Tool),
        entry(ConversationRole::Assistant),
        entry(ConversationRole::Reasoning),
        entry(ConversationRole::System),
        entry(ConversationRole::Context),
        entry(ConversationRole::Assistant),
    };
    EXPECT_EQ(entry_presentation(entries, 0, false), Presentation::UserAuthored);
    EXPECT_EQ(entry_presentation(entries, 1, false), Presentation::Intermediate);
    EXPECT_EQ(entry_presentation(entries, 2, false), Presentation::Intermediate);
    EXPECT_EQ(entry_presentation(entries, 3, false), Presentation::Intermediate);
    EXPECT_EQ(entry_presentation(entries, 4, false), Presentation::Intermediate);
    EXPECT_EQ(entry_presentation(entries, 5, false), Presentation::Chrome);
    EXPECT_EQ(entry_presentation(entries, 6, false), Presentation::Chrome);
    EXPECT_EQ(entry_presentation(entries, 7, false), Presentation::FinalAnswer);

    std::vector<ConversationEntry> streaming = {
        entry(ConversationRole::User), entry(ConversationRole::Assistant)};
    EXPECT_EQ(entry_presentation(streaming, 1, true), Presentation::Intermediate);
    EXPECT_EQ(entry_presentation(streaming, 1, false), Presentation::FinalAnswer);
    EXPECT_EQ(entry_presentation(streaming, 9, false), Presentation::FinalAnswer);
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
    bool picker_opened = false;
    std::string applied_model;
    CommandContext context{model};
    context.session = model.session(kSession);
    context.request_exit = [&exited] { exited = true; };
    context.create_session = [&created] { created = true; };
    context.open_model_picker = [&picker_opened] { picker_opened = true; };
    context.apply_model = [&applied_model](const std::string& name) { applied_model = name; };

    EXPECT_FALSE(registry.dispatch("plain text", context));
    EXPECT_TRUE(registry.dispatch("/help", context));
    EXPECT_FALSE(model.session(kSession)->conversation.entries.empty());

    EXPECT_TRUE(registry.dispatch("/clear", context));
    EXPECT_TRUE(model.session(kSession)->conversation.entries.empty());

    EXPECT_TRUE(registry.dispatch("/new", context));
    EXPECT_TRUE(created);

    EXPECT_TRUE(registry.dispatch("/model", context));
    EXPECT_TRUE(picker_opened);
    EXPECT_TRUE(registry.dispatch("/model tiny", context));
    EXPECT_EQ(applied_model, "tiny");

    EXPECT_TRUE(registry.dispatch("/bogus", context));
    EXPECT_TRUE(registry.dispatch("/exit", context));
    EXPECT_TRUE(exited);

    const std::vector<CompletionCandidate> matches = registry.complete_candidates("he");
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_NE(matches.front().command, nullptr);
    EXPECT_EQ(matches.front().command->name, "help");
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
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("co");
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

    const auto commands_of = [](const std::vector<CompletionCandidate>& candidates) {
        std::vector<const Command*> commands;
        commands.reserve(candidates.size());
        for (const CompletionCandidate& candidate : candidates) {
            commands.push_back(candidate.command);
        }
        return commands;
    };

    const std::vector<const Command*> one = commands_of(registry.complete_candidates("he"));
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(CommandRegistry::longest_common_prefix(one), "help");

    const std::vector<const Command*> many = commands_of(registry.complete_candidates("c"));
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

    const std::vector<CompletionCandidate> matches = registry.complete_candidates("re");
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_NE(matches.front().command, nullptr);
    EXPECT_EQ(matches.front().command->name, "rename");
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

TEST(UiModel, SessionOpenedInsertsStateWithoutCell) {
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
    // 58-E25: SessionOpened materializes state WITHOUT a cell (cells come from
    // `refresh_sessions`), so a SessionCreated notice cannot leak one.
    std::size_t matches = 0;
    for (const SessionCell& cell : workspace->second.sessions) {
        if (cell.id == peer) {
            ++matches;
        }
    }
    EXPECT_EQ(matches, 0u) << "SessionOpened must not insert a cell (58-E25)";
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

    EXPECT_TRUE(model.workspaces.at(WorkspaceId{"workspace"}).activeSessionId().value.empty());
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

// SW-U15 (22 §3.7, extended by 57-D5): effective-root first, then `lastUsedAt`
// desc, then case-insensitive title order with a path tie-break; the Live source
// preserves the daemon's session.list order.
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
    add_catalog_sessions(model, WorkspaceId{"ws-b"}, {SessionId{"s2"}, SessionId{"s1"}});
    model.catalog.workspaces[0].lastUsedAt = 50;  // ws-b
    const auto add_live_history = [&model](const char* id, const char* path, std::int64_t used) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.canonicalPath = path;
        history.lastUsedAt = used;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_live_history("ws-a", "/a", 5000);
    add_live_history("ws-g", "/g", 3000);
    add_live_history("ws-beta", "/beta", 4000);
    model.cwdWorkspacePath = "/g";

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 4u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-g"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-a"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-b"});
    ASSERT_EQ(model.switcher.workspaces[3].sessions.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[3].sessions[0].id, SessionId{"s2"});
    EXPECT_EQ(model.switcher.workspaces[3].sessions[1].id, SessionId{"s1"});

    // History source (22 §3.7/SW17): the same workspace rule; sessions are
    // `updated_at` desc, `id` asc. The Live source above was NOT re-sorted.
    model.catalog.workspaces.clear();
    model.catalog.loaded = false;
    model.catalog.generation = 0;
    const auto add_history = [&model](const char* id, const char* title, const char* path,
                                      std::int64_t used) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.title = title;
        history.canonicalPath = path;
        history.live = false;
        history.lastUsedAt = used;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_history("ws-b", "same", "/b", 50);
    add_history("ws-a", "same", "/a", 5000);
    add_history("ws-g", "Gamma", "/g", 3000);
    add_history("ws-beta", "beta", "/beta", 4000);
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
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-g"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-a"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-b"});
    ASSERT_EQ(model.switcher.workspaces[3].sessions.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[3].sessions[0].id, SessionId{"s2"});
    EXPECT_EQ(model.switcher.workspaces[3].sessions[1].id, SessionId{"s1"});
}

// 57-U1 (57-D5/57-I11): the effective-root workspace sorts first; the rest by
// `lastUsedAt` desc, then title/path.
TEST(UiModel, SwitcherOrdersEffectiveRootFirstThenLastUsed) {
    UiModel model;
    const auto add_history = [&model](const char* id, const char* title, const char* path,
                                      std::int64_t used) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.title = title;
        history.canonicalPath = path;
        history.lastUsedAt = used;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_history("ws-beta", "beta", "/beta", 100);
    add_history("ws-alpha", "alpha", "/alpha", 5000);
    add_history("ws-gamma", "gamma", "/gamma", 3000);
    add_history("ws-delta", "delta", "/delta", 3000);
    model.catalog.loaded = true;
    model.cwdWorkspacePath = "/beta";

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 4u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-alpha"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-delta"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-gamma"});
}

// 57-U2 (57-D5/57-I12): equal `lastUsedAt` falls back to title asc
// (case-insensitive), then canonical path asc.
TEST(UiModel, SwitcherOrderTieBreak) {
    UiModel model;
    const auto add_history = [&model](const char* id, const char* title, const char* path) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.title = title;
        history.canonicalPath = path;
        history.lastUsedAt = 100;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_history("ws-b", "Tie", "/b");
    add_history("ws-a", "Tie", "/a");
    add_history("ws-z", "Aardvark", "/z");
    model.catalog.loaded = true;

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 3u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-z"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-a"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-b"});
}

// 57-U3 (57-D5/57-I12): a zero `lastUsedAt` (unknown) sorts after every dated
// node.
TEST(UiModel, SwitcherOrderZeroLastUsedLast) {
    UiModel model;
    const auto add_history = [&model](const char* id, const char* title, const char* path,
                                      std::int64_t used) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.title = title;
        history.canonicalPath = path;
        history.lastUsedAt = used;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_history("ws-old", "old", "/old", 0);
    add_history("ws-new", "new", "/new", 50);
    model.catalog.loaded = true;

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-new"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-old"});
}

// 57-U4 (57-D5/57-I11): an effective-root path matching no node never fires; a
// filter that excludes the effective-root node leaves the rule un-fired.
TEST(UiModel, SwitcherOrderEffectiveRootAbsentOrFiltered) {
    UiModel model;
    const auto add_history = [&model](const char* id, const char* title, const char* path,
                                      std::int64_t used) {
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.title = title;
        history.canonicalPath = path;
        history.lastUsedAt = used;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_history("ws-alpha", "alpha", "/alpha", 5000);
    add_history("ws-beta", "beta", "/beta", 100);
    model.catalog.loaded = true;
    model.cwdWorkspacePath = "/missing";

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-alpha"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-beta"});

    model.switcher.filter = "alpha";
    model.cwdWorkspacePath = "/beta";
    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-alpha"});
}

// 57-U5 (57-D5/57-D6): the same rule through the Live source, using
// `WorkspaceModel::cwd` for the effective-root match.
TEST(UiModel, SwitcherOrderLiveSource) {
    UiModel model;
    const auto add_live = [&model](const char* id, const char* title, const char* cwd,
                                   std::int64_t used) {
        WorkspaceModel workspace;
        workspace.id = WorkspaceId{id};
        workspace.title = title;
        workspace.cwd = cwd;
        workspace.daemonStatus = DaemonStatus::Attached;
        workspace.live = true;
        model.workspaces.emplace(workspace.id, std::move(workspace));
        WorkspaceHistory history;
        history.id = WorkspaceId{id};
        history.canonicalPath = cwd;
        history.lastUsedAt = used;
        model.catalog.workspaces.push_back(std::move(history));
    };
    add_live("ws-beta", "beta", "/beta", 100);
    add_live("ws-alpha", "alpha", "/alpha", 5000);
    model.catalog.loaded = true;
    model.cwdWorkspacePath = "/beta";

    model.switcher.open(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-alpha"});
}

// 57-U7 (57-I2): after 57-D1 the per-session `· <relative>` leaf still renders
// from `catalog.nowMs`.
TEST(UiModel, PerSessionLeafStillRendersAfterD1) {
    UiModel model;
    WorkspaceHistory history;
    history.id = WorkspaceId{"ws"};
    history.title = "ws";
    history.canonicalPath = "/ws";
    SessionHistoryEntry entry;
    entry.id = SessionId{"s1"};
    entry.title = "kept";
    entry.kind = "root";
    entry.model = "m";
    entry.updatedAt = 1000;
    history.sessions.push_back(entry);
    model.catalog.workspaces.push_back(history);
    model.catalog.loaded = true;
    model.catalog.nowMs = 1000 + 60'000;
    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;

    const std::string rendered =
        render_to_ansi(model, TerminalSize{100, 30}, Theme{false});
    EXPECT_NE(rendered.find("kept · root · m · 1m"), std::string::npos);
    EXPECT_EQ(rendered.find("captured"), std::string::npos);
}

// 57-U8 (57-I11/57-I19): Live `open` before any catalog snapshot still pins the
// effective-root node (via `WorkspaceModel::cwd`); the rest tie-break by title.
TEST(UiModel, SwitcherEffectiveRootRuleWhenCatalogUnloaded) {
    UiModel model;
    const auto add_live = [&model](const char* id, const char* title, const char* cwd) {
        WorkspaceModel workspace;
        workspace.id = WorkspaceId{id};
        workspace.title = title;
        workspace.cwd = cwd;
        workspace.daemonStatus = DaemonStatus::Attached;
        workspace.live = true;
        model.workspaces.emplace(workspace.id, std::move(workspace));
    };
    add_live("ws-beta", "beta", "/beta");
    add_live("ws-alpha", "alpha", "/alpha");
    model.catalog.loaded = false;
    model.cwdWorkspacePath = "/beta";

    model.switcher.open(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 2u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-beta"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-alpha"});
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
    // 45-D4: the focused session is excluded from the Live source, so the cursor
    // cannot point at it.
    EXPECT_FALSE(model.switcher.cursor.session.has_value());

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
    // 57-D5 extension: effective root `/stopped`, then lastUsedAt desc, then the
    // title/path tie-break.
    model.catalog.workspaces[0].lastUsedAt = 100;  // live
    model.catalog.workspaces[1].lastUsedAt = 500;  // stopped
    model.catalog.workspaces[2].lastUsedAt = 200;  // tie-b
    model.catalog.workspaces[3].lastUsedAt = 200;  // tie-a
    model.cwdWorkspacePath = "/stopped";

    model.switcher.openHistory(model);
    EXPECT_EQ(model.switcher.source, SwitcherSource::History);

    ASSERT_EQ(model.switcher.workspaces.size(), 4u);
    EXPECT_EQ(model.switcher.workspaces[0].id, WorkspaceId{"ws-stop"});
    EXPECT_EQ(model.switcher.workspaces[1].id, WorkspaceId{"ws-tie-a"});
    EXPECT_EQ(model.switcher.workspaces[2].id, WorkspaceId{"ws-tie-b"});
    EXPECT_EQ(model.switcher.workspaces[3].id, WorkspaceId{"ws-live"});

    const WorkspaceNode& stopped_node = model.switcher.workspaces[0];
    EXPECT_TRUE(stopped_node.historyOnly);
    EXPECT_FALSE(stopped_node.live);
    ASSERT_TRUE(stopped_node.note.has_value());
    EXPECT_EQ(*stopped_node.note, "corrupt");
    EXPECT_TRUE(stopped_node.sessions.empty());

    const WorkspaceNode& live_node = model.switcher.workspaces[3];
    EXPECT_FALSE(live_node.historyOnly);
    EXPECT_TRUE(live_node.live);
    EXPECT_FALSE(live_node.note.has_value());
    // 58-E19: `kind == "subagent"` rows are filtered out of the History source.
    ASSERT_EQ(live_node.sessions.size(), 2u);
    EXPECT_EQ(live_node.sessions[0].id, SessionId{"s-new"});
    EXPECT_EQ(live_node.sessions[1].id, SessionId{"s-old"});
    for (const SessionNode& node : live_node.sessions) {
        EXPECT_NE(node.id, SessionId{"s-mid"});
        EXPECT_NE(node.kind, "subagent");
    }
    EXPECT_TRUE(live_node.sessions[0].fromDisk);
    EXPECT_EQ(live_node.sessions[1].kind, "fork");
    EXPECT_EQ(live_node.sessions[1].model, "m2");
    EXPECT_EQ(live_node.sessions[1].updatedAt, 1000);
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

// ── 45-D3: the Live switcher is a strict subset of `/sessions` ───────────────

TEST(UiModel, UI45_D3_LiveIsSubsetOfHistory) {
    UiModel model = make_model();
    const SessionId second{"session-2"};
    WorkspaceModel beta;
    beta.id = WorkspaceId{"workspace-2"};
    beta.title = "beta";
    beta.daemonStatus = DaemonStatus::Attached;
    beta.live = true;
    SessionCell cell;
    cell.id = second;
    cell.title = "second";
    beta.sessions.push_back(cell);
    model.workspaces.emplace(beta.id, std::move(beta));
    model.ensureSessionIn(WorkspaceId{"workspace-2"}, second);
    add_catalog_sessions(model, WorkspaceId{"workspace"}, {kSession});
    add_catalog_sessions(model, WorkspaceId{"workspace-2"}, {second});

    model.switcher.open(model);
    std::set<std::pair<std::string, std::string>> live;
    for (const WorkspaceNode& node : model.switcher.workspaces) {
        for (const SessionNode& session : node.sessions) {
            live.emplace(node.id.value, session.id.value);
        }
    }

    model.switcher.openHistory(model);
    std::set<std::pair<std::string, std::string>> history;
    for (const WorkspaceNode& node : model.switcher.workspaces) {
        for (const SessionNode& session : node.sessions) {
            history.emplace(node.id.value, session.id.value);
        }
    }

    ASSERT_FALSE(live.empty());
    for (const auto& leaf : live) {
        EXPECT_EQ(history.count(leaf), 1u) << leaf.first << "/" << leaf.second;
    }
}

TEST(UiModel, UI45_D3_LiveHidesUncataloguedSession) {
    UiModel model = make_model();
    const SessionId catalogued{"s-cat"};
    const SessionId uncatalogued{"s-uncat"};
    model.ensureSessionIn(model.activeWorkspaceId, catalogued);
    model.ensureSessionIn(model.activeWorkspaceId, uncatalogued);
    add_catalog_sessions(model, model.activeWorkspaceId, {kSession, catalogued});

    model.switcher.open(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    ASSERT_EQ(model.switcher.workspaces[0].sessions.size(), 1u);
    EXPECT_EQ(model.switcher.workspaces[0].sessions[0].id, catalogued);
}

// ── 45-D4: hide the focused session from both lists ─────────────────────────

TEST(UiModel, UI45_D4_FocusedSessionExcludedLive) {
    UiModel model = make_model();
    const SessionId other{"other"};
    model.ensureSessionIn(model.activeWorkspaceId, other);
    add_catalog_sessions(model, model.activeWorkspaceId, {kSession, other});

    model.switcher.open(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    ASSERT_EQ(model.switcher.workspaces[0].sessions.size(), 1u);
    EXPECT_EQ(model.switcher.workspaces[0].sessions[0].id, other);
    EXPECT_FALSE(model.switcher.workspaces[0].sessions_hidden_by_focus);
}

TEST(UiModel, UI45_D4_FocusedSessionExcludedHistory) {
    UiModel model = make_model();
    const SessionId other{"other"};
    add_catalog_sessions(model, model.activeWorkspaceId, {kSession, other});

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    ASSERT_EQ(model.switcher.workspaces[0].sessions.size(), 1u);
    EXPECT_EQ(model.switcher.workspaces[0].sessions[0].id, other);
}

TEST(UiModel, UI45_D4_LiveEmptyNodeRendered) {
    UiModel model = make_model();
    add_catalog_sessions(model, model.activeWorkspaceId, {kSession});

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 1u) << "the node must not be suppressed";
    EXPECT_TRUE(model.switcher.workspaces[0].sessions.empty());
    EXPECT_TRUE(model.switcher.workspaces[0].sessions_hidden_by_focus);
    EXPECT_FALSE(model.switcher.workspaces[0].catalog_pending);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(current session hidden)"), std::string::npos);
}

TEST(UiModel, UI45_D4_LivePlaceholderLeaf) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/ws";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    model.workspaces.emplace(workspace.id, workspace);
    add_catalog_sessions(model, workspace.id, {});

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(model.switcher.workspaces[0].sessions.empty());
    EXPECT_FALSE(model.switcher.workspaces[0].sessions_hidden_by_focus);
    EXPECT_FALSE(model.switcher.workspaces[0].catalog_pending);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(no live sessions)"), std::string::npos);
}

TEST(UiModel, UI45_D4_LiveCatalogPendingPlaceholder) {
    UiModel model = make_model();
    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(model.switcher.workspaces[0].catalog_pending);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(loading live sessions…"), std::string::npos);
    EXPECT_EQ(rendered.find("(no live sessions)"), std::string::npos);
}

TEST(UiModel, UI45_D4_LiveCatalogNotePlaceholder) {
    UiModel model = make_model();
    WorkspaceHistory history;
    history.id = model.activeWorkspaceId;
    history.title = "workspace";
    history.canonicalPath = "/workspace";
    history.live = true;
    history.note = "corrupt";
    model.catalog.workspaces.push_back(std::move(history));
    model.catalog.loaded = true;
    model.catalog.generation = 1;

    model.openSwitcher();
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    ASSERT_TRUE(model.switcher.workspaces[0].note.has_value());
    EXPECT_FALSE(model.switcher.workspaces[0].catalog_pending);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(corrupt)"), std::string::npos);
    EXPECT_EQ(rendered.find("(no live sessions)"), std::string::npos);
}

TEST(UiModel, UI45_D4_HistoryGenuinelyEmptyKeepsEmptyState) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/ws";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    model.workspaces.emplace(workspace.id, workspace);
    add_catalog_sessions(model, workspace.id, {});

    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(model.switcher.workspaces[0].sessions.empty());
    EXPECT_FALSE(model.switcher.workspaces[0].sessions_hidden_by_focus);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(no stored sessions)"), std::string::npos);
    EXPECT_EQ(rendered.find("(current session hidden)"), std::string::npos);
}

TEST(UiModel, UI45_D4_HistoryAllHiddenDistinctLabel) {
    UiModel model = make_model();
    add_catalog_sessions(model, model.activeWorkspaceId, {kSession});

    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(model.switcher.workspaces[0].sessions.empty());
    EXPECT_TRUE(model.switcher.workspaces[0].sessions_hidden_by_focus);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(current session hidden)"), std::string::npos);
    EXPECT_EQ(rendered.find("(no stored sessions)"), std::string::npos);
}

TEST(UiModel, UI45_D4_HistoryNoteLeafRetained) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    WorkspaceHistory history;
    history.id = model.activeWorkspaceId;
    history.title = "ws";
    history.canonicalPath = "/ws";
    history.live = false;
    history.note = "corrupt";
    model.catalog.workspaces.push_back(std::move(history));
    model.catalog.loaded = true;
    model.catalog.generation = 1;

    model.switcher.openHistory(model);
    model.mode = UiMode::Switcher;
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(model.switcher.workspaces[0].sessions.empty());
    EXPECT_FALSE(model.switcher.workspaces[0].sessions_hidden_by_focus);

    const std::string rendered = render_to_ansi(model, TerminalSize{80, 24}, Theme{false});
    EXPECT_NE(rendered.find("(corrupt)"), std::string::npos);
}

// ── 45-D10: the unmodeled-session lockout ───────────────────────────────────

TEST(UiModel, UI45_D10_FocusModelsTarget) {
    UiModel model = make_model();
    const SessionId target{"unmodeled"};
    ASSERT_EQ(model.session(target), nullptr);

    model.focusSessionIn(model.activeWorkspaceId, target);
    ASSERT_NE(model.session(target), nullptr);
    EXPECT_EQ(model.activeWorkspace()->activeSessionId(), target);
    ASSERT_NE(model.activeSession(), nullptr);
    EXPECT_EQ(model.activeSession()->id, target);
}

TEST(UiModel, UI45_D10_EnsureActiveSessionRepairs) {
    UiModel model = make_model();
    const SessionId target{"repair"};
    model.focusSessionIn(model.activeWorkspaceId, target);
    ASSERT_NE(model.activeSession(), nullptr);

    model.sessions.erase(target);
    EXPECT_EQ(model.activeSession(), nullptr);
    EXPECT_FALSE(model.activeWorkspace()->activeSessionId().value.empty());

    SessionUiState* repaired = model.ensureActiveSession();
    ASSERT_NE(repaired, nullptr);
    EXPECT_EQ(repaired->id, target);
    EXPECT_NE(model.activeSession(), nullptr);
}

TEST(UiModel, UI45_D10_EnsureActiveSessionNoWorkspace) {
    UiModel model;
    EXPECT_EQ(model.ensureActiveSession(), nullptr);
}

TEST(UiModel, UI45_D10_SingleMutator) {
    static_assert(!std::is_aggregate_v<WorkspaceModel>,
                  "WorkspaceModel must not be an aggregate once the focus is private");
    EXPECT_TRUE(
        (std::is_member_function_pointer_v<decltype(&WorkspaceModel::activeSessionId)>));
}

// 45-D7.3/45-I18: connectivity is a derived last-outcome, never a probe.
// AssistantMessageStarted must not mark Ok; ErrorOccurred sets Error; a later
// AssistantMessageFinished or TokenUsageUpdated sets Ok.
TEST(UiModel, UI45_D7_ConnectivityTransitions) {
    UiModel model = make_model();
    ASSERT_NE(model.session(kSession), nullptr);
    EXPECT_EQ(model.session(kSession)->status.api_state, ApiConnectivity::Unknown);

    model.apply(UiEvent{AssistantMessageStarted{kSession, "a1"}});
    EXPECT_EQ(model.session(kSession)->status.api_state, ApiConnectivity::Unknown)
        << "a started message can still fail";

    model.apply(UiEvent{ErrorOccurred{kSession, "boom"}});
    EXPECT_EQ(model.session(kSession)->status.api_state, ApiConnectivity::Error);

    model.apply(UiEvent{AssistantMessageFinished{kSession, "a1", "ok", std::nullopt}});
    EXPECT_EQ(model.session(kSession)->status.api_state, ApiConnectivity::Ok);

    model.apply(UiEvent{ErrorOccurred{kSession, "again"}});
    EXPECT_EQ(model.session(kSession)->status.api_state, ApiConnectivity::Error);

    Usage usage;
    usage.input_tokens = 1;
    model.apply(UiEvent{TokenUsageUpdated{kSession, usage}});
    EXPECT_EQ(model.session(kSession)->status.api_state, ApiConnectivity::Ok);
}

TEST(UiModel, UI46_D12_ApiKeyNeverRendered) {
    UiModel model;
    model.pushNotice(R"({"api_key": "SECRET123"})");
    ASSERT_FALSE(model.notices.empty());
    EXPECT_EQ(model.notices.back().text.find("SECRET123"), std::string::npos);
    EXPECT_NE(model.notices.back().text.find("[REDACTED]"), std::string::npos);
}

// ---- spec 58 (subagent navigation) -----------------------------------------

const WorkspaceId kWs58{"workspace"};

void seed_child(UiModel& model, const SessionId& parent, const SessionId& child,
                const std::string& summary, SubagentStatus status) {
    SessionUiState* state = model.session(parent);
    ASSERT_NE(state, nullptr);
    state->subagents.agents.push_back(
        SubagentView{child, summary, AgentState::Idle, status});
}

bool has_cell(const UiModel& model, const WorkspaceId& workspace, const SessionId& id) {
    const auto it = model.workspaces.find(workspace);
    if (it == model.workspaces.end()) {
        return false;
    }
    for (const SessionCell& cell : it->second.sessions) {
        if (cell.id == id) {
            return true;
        }
    }
    return false;
}

// 58-U2 (58-I2): a session/workspace switch clears the subagent path.
TEST(UiModel, UI58_U2_SubagentPathClearedOnSessionSwitch) {
    UiModel model = make_model();
    const SessionId child{"child"};
    model.subagent_path.push_back(child);
    model.focusSessionIn(kWs58, kSession);
    EXPECT_TRUE(model.subagent_path.empty());

    model.subagent_path.push_back(child);
    model.focusWorkspace(kWs58);
    EXPECT_TRUE(model.subagent_path.empty());
}

// 58-U3 (58-I14): the deepest path entry is viewed; a missing id yields nullptr.
TEST(UiModel, UI58_U3_ViewedSessionReturnsDeepestChild) {
    UiModel model = make_model();
    const SessionId a{"a"};
    const SessionId b{"b"};
    model.ensureSubagentState(kWs58, a);
    model.ensureSubagentState(kWs58, b);

    model.subagent_path = {a, b};
    EXPECT_EQ(model.viewedSession(), model.session(b));

    model.subagent_path = {SessionId{"missing"}};
    EXPECT_EQ(model.viewedSession(), nullptr);

    model.subagent_path.clear();
    EXPECT_EQ(model.viewedSession(), model.activeSession());
}

// 58-U4 (58-I5): SubagentSpawned adds a Running entry, idempotently.
TEST(UiModel, UI58_U4_SubagentSpawnedAddsRunningEntry) {
    UiModel model = make_model();
    model.apply(UiEvent{SubagentSpawned{kSession, SessionId{"c1"}, "task one"}});
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->subagents.agents.size(), 1u);
    EXPECT_EQ(state->subagents.agents[0].id, SessionId{"c1"});
    EXPECT_EQ(state->subagents.agents[0].summary, "task one");
    EXPECT_EQ(state->subagents.agents[0].status, SubagentStatus::Running);

    model.apply(UiEvent{SubagentSpawned{kSession, SessionId{"c1"}, "task one"}});
    EXPECT_EQ(state->subagents.agents.size(), 1u);
}

// 58-U5 (58-I6): fan-in maps the four terminal statuses distinctly.
TEST(UiModel, UI58_U5_SubagentFanInMarksTerminal) {
    UiModel model = make_model();
    const SessionId a{"a"};
    const SessionId b{"b"};
    const SessionId c{"c"};
    model.apply(UiEvent{SubagentSpawned{kSession, a, "A"}});
    model.apply(UiEvent{SubagentSpawned{kSession, b, "B"}});
    model.apply(UiEvent{SubagentSpawned{kSession, c, "C"}});

    model.apply(UiEvent{
        SubagentUpdated{kSession, a, "done", AgentState::Idle, SubagentStatus::Completed}});
    model.apply(UiEvent{
        SubagentUpdated{kSession, b, "bad", AgentState::Error, SubagentStatus::Failed}});
    model.apply(UiEvent{
        SubagentUpdated{kSession, c, "stop", AgentState::Idle, SubagentStatus::Cancelled}});

    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->subagents.agents.size(), 3u);
    EXPECT_EQ(state->subagents.agents[0].status, SubagentStatus::Completed);
    EXPECT_EQ(state->subagents.agents[1].status, SubagentStatus::Failed);
    EXPECT_EQ(state->subagents.agents[2].status, SubagentStatus::Cancelled);
}

// 58-U6 (58-I13): ensureSubagentState materializes state and no cell.
TEST(UiModel, UI58_U6_EnsureSubagentStateHasNoCell) {
    UiModel model = make_model();
    const SessionId child{"child"};
    model.ensureCellIn(kWs58, child);
    ASSERT_TRUE(has_cell(model, kWs58, child));

    SessionUiState& state = model.ensureSubagentState(kWs58, child);
    EXPECT_TRUE(state.subagent);
    EXPECT_EQ(state.workspace, kWs58);
    EXPECT_FALSE(has_cell(model, kWs58, child));
}

// 58-U7 (58-I7): the picker lists the viewed level's direct children only.
TEST(UiModel, UI58_U7_PickerListsOnlyViewedChildren) {
    UiModel model = make_model();
    const SessionId a{"a"};
    const SessionId b{"b"};
    const SessionId c{"c"};
    model.apply(UiEvent{SubagentSpawned{kSession, a, "A"}});
    model.apply(UiEvent{SubagentSpawned{kSession, b, "B"}});
    model.ensureSubagentState(kWs58, a);
    seed_child(model, a, c, "C", SubagentStatus::Running);
    model.subagent_path = {a};

    model.switcher.openSubagents(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    const WorkspaceNode& node = model.switcher.workspaces[0];
    ASSERT_EQ(node.sessions.size(), 1u);
    EXPECT_EQ(node.sessions[0].id, c);
    EXPECT_EQ(node.sessions[0].kind, "subagent");
}

// 58-U9 (58-E25): a SessionOpened for a materialized child creates no cell.
TEST(UiModel, UI58_U9_SessionOpenedCreatesNoCellForSubagent) {
    UiModel model = make_model();
    const SessionId child{"child"};
    model.ensureSubagentState(kWs58, child);

    WorkspaceEvent opened;
    opened.workspace = kWs58;
    opened.kind = WorkspaceEventKind::SessionOpened;
    opened.session = child;
    model.apply(opened);

    EXPECT_FALSE(has_cell(model, kWs58, child));
    ASSERT_NE(model.session(child), nullptr);
    EXPECT_TRUE(model.session(child)->subagent);
}

// 58-U10 (58-I26): the Live leaf predicate excludes subagent entries.
TEST(UiModel, UI58_U10_CatalogPredicateExcludesSubagent) {
    UiModel model = make_model();
    const SessionId child{"child"};
    add_catalog_sessions(model, kWs58, {child});
    EXPECT_TRUE(model.catalog_has_session(kWs58, child));

    model.catalog.workspaces[0].sessions[0].kind = "subagent";
    EXPECT_FALSE(model.catalog_has_session(kWs58, child));
}

// 58-U11 (58-I16): a nested chain resolves level by level and never revisits.
TEST(UiModel, UI58_U11_SubagentPathCannotCycle) {
    UiModel model = make_model();
    const SessionId a{"a"};
    const SessionId b{"b"};
    model.apply(UiEvent{SubagentSpawned{kSession, a, "A"}});
    model.ensureSubagentState(kWs58, a);
    seed_child(model, a, b, "B", SubagentStatus::Running);

    model.ensureSubagentState(kWs58, b);
    model.subagent_path = {a};
    EXPECT_EQ(model.viewedSession(), model.session(a));
    model.subagent_path.push_back(b);
    EXPECT_EQ(model.viewedSession(), model.session(b));

    // At depth 1 the picker lists only b's children (none), never a or main.
    model.switcher.openSubagents(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_TRUE(model.switcher.workspaces[0].sessions.empty());
}

// 58-U12 (58-D8): a terminal status while viewed keeps the path and transcript.
TEST(UiModel, UI58_U12_ChildFinishWhileViewedStays) {
    UiModel model = make_model();
    const SessionId a{"a"};
    model.apply(UiEvent{SubagentSpawned{kSession, a, "A"}});
    model.ensureSubagentState(kWs58, a);
    model.subagent_path = {a};

    model.apply(UiEvent{
        SubagentUpdated{kSession, a, "done", AgentState::Idle, SubagentStatus::Completed}});

    ASSERT_EQ(model.subagent_path.size(), 1u);
    EXPECT_EQ(model.viewedSession(), model.session(a));
    ASSERT_EQ(model.session(kSession)->subagents.agents.size(), 1u);
    EXPECT_EQ(model.session(kSession)->subagents.agents[0].status, SubagentStatus::Completed);
}

// 58-U13/58-U14 (58-D8): clamp keeps a stale cursor valid after a resnapshot.
TEST(UiModel, UI58_U13_U14_PickerClampManyChildren) {
    UiModel model = make_model();
    for (int index = 0; index < 100; ++index) {
        const std::string id = "c" + std::to_string(index);
        model.apply(UiEvent{SubagentSpawned{kSession, SessionId{id}, id}});
    }
    model.switcher.openSubagents(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    EXPECT_EQ(model.switcher.cursor.workspace, kWs58);
    EXPECT_EQ(model.switcher.workspaces[0].sessions.size(), 100u);

    model.switcher.cursor.session = SessionId{"missing"};
    model.switcher.clamp_cursor();
    EXPECT_FALSE(model.switcher.cursor.session.has_value());
    EXPECT_EQ(model.switcher.cursor.workspace, kWs58);
}

// 58-U15 (58-I3): a spawn for the viewed child updates the viewed strip.
TEST(UiModel, UI58_U15_SpawnWhileViewingUpdatesStrip) {
    UiModel model = make_model();
    const SessionId a{"a"};
    const SessionId c{"c"};
    model.apply(UiEvent{SubagentSpawned{kSession, a, "A"}});
    model.ensureSubagentState(kWs58, a);
    model.subagent_path = {a};

    model.apply(UiEvent{SubagentSpawned{a, c, "C"}});
    ASSERT_NE(model.session(a), nullptr);
    ASSERT_EQ(model.session(a)->subagents.agents.size(), 1u);
    EXPECT_EQ(model.session(a)->subagents.agents[0].id, c);
}

// 58-U17 (58-I13): the History source excludes subagent rows.
TEST(UiModel, UI58_U17_HistoryFiltersSubagents) {
    UiModel model = make_model();
    const SessionId root{"root"};
    const SessionId child{"child"};
    add_catalog_sessions(model, kWs58, {root, child});
    model.catalog.workspaces[0].sessions[1].kind = "subagent";

    model.switcher.openHistory(model);
    ASSERT_EQ(model.switcher.workspaces.size(), 1u);
    const WorkspaceNode& node = model.switcher.workspaces[0];
    ASSERT_EQ(node.sessions.size(), 1u);
    EXPECT_EQ(node.sessions[0].id, root);
}

// 58-U21 (58-A3.1/P2): the pinned per-source policy values.
TEST(UiModel, UI58_U21_SwitcherPolicyTable) {
    const SwitcherSourcePolicy& live = switcher_policy(SwitcherSource::Live);
    EXPECT_EQ(live.window_title, "workspaces");
    EXPECT_EQ(live.heading, "Switcher");
    EXPECT_EQ(live.footer, "j/k move · Tab expand · Ctrl+D delete · Enter focus · Esc close");
    EXPECT_EQ(live.empty_state, "(no workspaces)");
    EXPECT_EQ(live.enter, SwitcherEnter::Focus);
    EXPECT_TRUE(live.ctrl_d_enabled);
    EXPECT_TRUE(live.tab_expands);
    EXPECT_FALSE(live.ctrl_t_closes);
    EXPECT_FALSE(live.r_refreshes);

    const SwitcherSourcePolicy& history = switcher_policy(SwitcherSource::History);
    EXPECT_EQ(history.window_title, "sessions");
    EXPECT_EQ(history.heading, "sessions");
    EXPECT_EQ(history.footer,
              "stored sessions · r refresh · Ctrl+D delete · Enter resume · Esc close");
    EXPECT_EQ(history.empty_state, "(no stored sessions)");
    EXPECT_EQ(history.enter, SwitcherEnter::Resume);
    EXPECT_TRUE(history.ctrl_d_enabled);
    EXPECT_TRUE(history.tab_expands);
    EXPECT_FALSE(history.ctrl_t_closes);
    EXPECT_TRUE(history.r_refreshes);

    const SwitcherSourcePolicy& subagents = switcher_policy(SwitcherSource::Subagents);
    EXPECT_EQ(subagents.window_title, "subagents");
    EXPECT_EQ(subagents.heading, "subagents");
    EXPECT_EQ(subagents.footer, "Enter enter · Esc close");
    EXPECT_EQ(subagents.empty_state, "(no subagents)");
    EXPECT_EQ(subagents.enter, SwitcherEnter::EnterChild);
    EXPECT_FALSE(subagents.ctrl_d_enabled);
    EXPECT_FALSE(subagents.tab_expands);
    EXPECT_TRUE(subagents.ctrl_t_closes);
    EXPECT_FALSE(subagents.r_refreshes);
}

} // namespace

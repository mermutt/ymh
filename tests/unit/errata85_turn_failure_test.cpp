// 85-U1..U5, U8 (spec 85): a non-StepLimitExceeded TurnFailed is attributed to
// its turn and rendered as a ConversationRole::TurnError banner, never a tool
// row; ErrorOccurred retains its System rendering for the non-turn sources.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"errata85-session"};

Event make_event(EventType type, const nlohmann::json& payload) {
    Event event;
    event.id = EventId{"event-85"};
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
    workspace.cwd = "/work";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, kSession);
    model.session(kSession)->status.model = "test-model";
    return model;
}

// 85-U1 (TF2/TF5): the model appends a TurnError entry carrying the bare message
// and the turn id, and raises the API-error surface.
TEST(Errata85, TurnFailedNoticeAppendsTurnErrorEntry) {
    UiModel model = make_model();
    model.apply(UiEvent{TurnFailedNotice{kSession, 7, "ProviderFailed", "transport timeout"}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->conversation.entries.empty());
    const ConversationEntry& last = state->conversation.entries.back();
    EXPECT_EQ(last.role, ConversationRole::TurnError);
    EXPECT_EQ(last.text, "transport timeout");
    ASSERT_TRUE(last.turn.has_value());
    EXPECT_EQ(*last.turn, 7u);
    EXPECT_TRUE(last.tool_name.empty());
    EXPECT_TRUE(last.tool_call_id.empty());
    EXPECT_EQ(state->status.last_error, "transport timeout");
    EXPECT_EQ(state->status.api_state, ApiConnectivity::Error);
}

// 85-U2 (TF1/TF8): the adapter routes by code -- a provider code becomes exactly
// one TurnFailedNotice and no ErrorOccurred; StepLimitExceeded stays a Notice.
TEST(Errata85, TurnFailedAdaptationRoutesByCode) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::TurnFailed provider;
    provider.turn = 3;
    provider.code = "ProviderFailed";
    provider.message = "transport timeout";
    const std::vector<UiEvent> routed =
        adapter.adapt(typed_event(EventType::TurnFailed, provider));

    std::size_t turn_failures = 0;
    std::size_t error_events = 0;
    for (const UiEvent& event : routed) {
        if (const auto* notice = std::get_if<TurnFailedNotice>(&event.value)) {
            ++turn_failures;
            EXPECT_EQ(notice->session, kSession);
            EXPECT_EQ(notice->turn, 3u);
            EXPECT_EQ(notice->code, "ProviderFailed");
            EXPECT_EQ(notice->message, "transport timeout");
        }
        if (std::holds_alternative<ErrorOccurred>(event.value)) {
            ++error_events;
        }
    }
    EXPECT_EQ(turn_failures, 1u);
    EXPECT_EQ(error_events, 0u);

    payload::TurnFailed step_limit;
    step_limit.turn = 4;
    step_limit.code = "StepLimitExceeded";
    step_limit.message = "step limit reached";
    const std::vector<UiEvent> limited =
        adapter.adapt(typed_event(EventType::TurnFailed, step_limit));
    ASSERT_EQ(limited.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<StepLimitReached>(limited.front().value));
    EXPECT_FALSE(std::holds_alternative<TurnFailedNotice>(limited.front().value));

    // TF11/TF-F9: a Cancelled/None code (never produced in normal operation) is
    // treated conservatively as a real failure, not silently dropped.
    payload::TurnFailed cancelled;
    cancelled.turn = 5;
    cancelled.code = "Cancelled";
    const std::vector<UiEvent> conservative =
        adapter.adapt(typed_event(EventType::TurnFailed, cancelled));
    std::size_t conservative_failures = 0;
    for (const UiEvent& event : conservative) {
        if (const auto* notice = std::get_if<TurnFailedNotice>(&event.value)) {
            ++conservative_failures;
            EXPECT_EQ(notice->code, "Cancelled");
        }
    }
    EXPECT_EQ(conservative_failures, 1u);
}

// 85-U3 (TF3): the TurnError entry is not a tool result -- the preceding tool
// entry is byte-identical, the new role is not Tool, and ToolModel is untouched.
TEST(Errata85, TurnFailureIsNotAToolResult) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ToolCall call;
    call.id = "t1";
    call.name = "edit_file";
    call.arguments = {{"file", "a.txt"}};
    adapter.onEvent(typed_event(EventType::ToolCall, call));

    payload::ToolResult result;
    result.id = "t1";
    result.name = "edit_file";
    result.outcome = payload::ToolOutcome::Ok;
    result.output = "ok";
    adapter.onEvent(typed_event(EventType::ToolResult, result));

    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 1u);
    const ConversationEntry tool_before = state->conversation.entries.front();
    const std::size_t calls_before = state->tools.calls.size();
    ASSERT_EQ(calls_before, 1u);
    EXPECT_EQ(tool_before.role, ConversationRole::Tool);

    model.apply(UiEvent{TurnFailedNotice{kSession, 7, "ProviderFailed", "transport timeout"}});

    ASSERT_EQ(state->conversation.entries.size(), 2u);
    const ConversationEntry& tool_after = state->conversation.entries.front();
    EXPECT_EQ(tool_after.role, tool_before.role);
    EXPECT_EQ(tool_after.text, tool_before.text);
    EXPECT_EQ(tool_after.tool_call_id, tool_before.tool_call_id);
    const ConversationEntry& banner = state->conversation.entries.back();
    EXPECT_EQ(banner.role, ConversationRole::TurnError);
    EXPECT_NE(banner.role, ConversationRole::Tool);
    EXPECT_TRUE(banner.tool_call_id.empty());
    EXPECT_EQ(state->tools.calls.size(), calls_before);
    EXPECT_TRUE(state->tools.by_id.count(banner.tool_call_id) == 0);
}

// 85-U4 (TF6/TF7): the banner is conversation content, so it raises `unseen`
// while the view is scrolled up, exactly like the step-ceiling Notice.
TEST(Errata85, TurnFailureRaisesUnseenWhileScrolled) {
    UiModel model = make_model();
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    state->scroll.pageUp();
    EXPECT_FALSE(state->scroll.following);

    model.apply(UiEvent{TurnFailedNotice{kSession, 7, "ProviderFailed", "transport timeout"}});

    EXPECT_TRUE(state->scroll.unseen);
    ASSERT_FALSE(state->conversation.entries.empty());
    EXPECT_EQ(state->conversation.entries.back().role, ConversationRole::TurnError);
}

// 85-U5 (TF4): ErrorOccurred keeps its System rendering and 45-D7 behaviour for
// the non-turn sources (supervisor/rename notices).
TEST(Errata85, ErrorOccurredStillAppendsSystemEntry) {
    UiModel model = make_model();
    model.apply(UiEvent{ErrorOccurred{kSession, "rename failed: InvalidParams"}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->conversation.entries.empty());
    const ConversationEntry& last = state->conversation.entries.back();
    EXPECT_EQ(last.role, ConversationRole::System);
    EXPECT_EQ(last.text, "error: rename failed: InvalidParams");
    EXPECT_FALSE(last.turn.has_value());
    EXPECT_EQ(state->status.api_state, ApiConnectivity::Error);
}

// 85-U8 (TF-F2): a replayed TurnFailed event re-derives the same TurnError entry
// as the direct live path.
TEST(Errata85, TurnFailureReplayReconstructsBanner) {
    payload::TurnFailed payload_failed;
    payload_failed.turn = 9;
    payload_failed.code = "ProviderFailed";
    payload_failed.message = "transport timeout";

    UiModel replayed = make_model();
    UiEventAdapter adapter(replayed);
    for (const UiEvent& event : adapter.adapt(typed_event(EventType::TurnFailed, payload_failed))) {
        replayed.apply(event);
    }

    UiModel live = make_model();
    live.apply(UiEvent{TurnFailedNotice{kSession, 9, "ProviderFailed", "transport timeout"}});

    const ConversationEntry& from_replay = replayed.session(kSession)->conversation.entries.back();
    const ConversationEntry& from_live = live.session(kSession)->conversation.entries.back();
    EXPECT_EQ(from_replay.role, from_live.role);
    EXPECT_EQ(from_replay.text, from_live.text);
    ASSERT_TRUE(from_replay.turn.has_value());
    ASSERT_TRUE(from_live.turn.has_value());
    EXPECT_EQ(*from_replay.turn, *from_live.turn);
}

} // namespace

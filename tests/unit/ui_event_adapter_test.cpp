#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const WorkspaceId kWorkspaceA{"workspace-a"};
const WorkspaceId kWorkspaceB{"workspace-b"};
const SessionId kSessionA{"session-a"};
const SessionId kSessionB{"session-b"};

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = kWorkspaceA;
    WorkspaceModel a;
    a.id = kWorkspaceA;
    a.cwd = "/work/a";
    a.title = "alpha";
    a.activeSessionId = kSessionA;
    model.workspaces.emplace(kWorkspaceA, std::move(a));
    WorkspaceModel b;
    b.id = kWorkspaceB;
    b.cwd = "/work/b";
    b.title = "beta";
    model.workspaces.emplace(kWorkspaceB, std::move(b));
    model.ensureSessionIn(kWorkspaceA, kSessionA);
    return model;
}

Event user_event(const SessionId& session, const std::string& id, const std::string& text) {
    Event event;
    event.id.value = id;
    event.session_id = session;
    event.timestamp = std::chrono::system_clock::now();
    event.type = EventType::UserMessage;
    payload::UserMessage message;
    message.id = "m-" + id;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content.push_back(std::move(block));
    event.payload = message;
    return event;
}

protocol::SessionEnvelope envelope(const Event& event) {
    protocol::SessionEnvelope result;
    result.session = event.session_id;
    result.event = event;
    return result;
}

TEST(UiEventAdapter, IdempotentApplyIgnoresDuplicateEventIds) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    const Event event = user_event(kSessionA, "dup-1", "hello");

    adapter.onSessionEnvelope(kWorkspaceA, envelope(event));
    adapter.onSessionEnvelope(kWorkspaceA, envelope(event));

    const SessionUiState* state = model.session(kSessionA);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->conversation.entries.size(), 1u);
}

TEST(UiEventAdapter, RoutesWireEnvelopeToBackgroundWorkspace) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    const Event event = user_event(kSessionB, "b-1", "from beta");

    adapter.onSessionEnvelope(kWorkspaceB, envelope(event));

    const SessionUiState* state = model.session(kSessionB);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->workspace, kWorkspaceB);
    EXPECT_EQ(state->conversation.entries.size(), 1u);

    const auto workspace = model.workspaces.find(kWorkspaceB);
    ASSERT_NE(workspace, model.workspaces.end());
    bool has_cell = false;
    for (const SessionCell& cell : workspace->second.sessions) {
        if (cell.id == kSessionB) {
            has_cell = true;
        }
    }
    EXPECT_TRUE(has_cell);
}

TEST(UiEventAdapter, MismatchedEnvelopeSessionIsIgnored) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    Event event = user_event(kSessionA, "mismatch-1", "hello");
    protocol::SessionEnvelope wire;
    wire.session = kSessionB;
    wire.event = event;

    adapter.onSessionEnvelope(kWorkspaceA, wire);

    EXPECT_EQ(model.session(kSessionB), nullptr);
    EXPECT_EQ(model.session(kSessionA)->conversation.entries.size(), 0u);
}

TEST(UiEventAdapter, PermissionOverWireOpensDialog) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    protocol::PermissionRequest request;
    request.request_id = "req-7";
    request.session = kSessionB;
    request.tool = "shell";
    request.summary = "run rm -rf build";
    request.arguments = nlohmann::json::object();

    adapter.onPermissionRequest(kWorkspaceB, request);

    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.request.value, "req-7");
    EXPECT_EQ(model.dialog.tool, "shell");
    const SessionUiState* state = model.session(kSessionB);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->agent_state, AgentState::WaitingForPermission);
    EXPECT_TRUE(state->attention.needsInput);
    EXPECT_EQ(model.aggregate.current.waitingCount, 1u);
}

TEST(UiEventAdapter, LeaseLostMarksCellReadOnly) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    protocol::HostNotice notice;
    notice.kind = protocol::HostNoticeKind::LeaseLost;
    notice.workspace = protocol::WorkspaceId{kWorkspaceB.value};
    notice.session = kSessionB;

    adapter.onHostNotice(kWorkspaceB, notice);

    const auto workspace = model.workspaces.find(kWorkspaceB);
    ASSERT_NE(workspace, model.workspaces.end());
    for (const SessionCell& cell : workspace->second.sessions) {
        if (cell.id == kSessionB) {
            EXPECT_TRUE(cell.readOnly);
        }
    }
}

TEST(UiEventAdapter, WorkspaceEventUpdatesDaemonStatus) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    WorkspaceEvent event;
    event.workspace = kWorkspaceB;
    event.kind = WorkspaceEventKind::DaemonDied;

    adapter.onWorkspaceEvent(event);

    EXPECT_EQ(model.workspaces.at(kWorkspaceB).daemonStatus, DaemonStatus::Dead);
}

TEST(UiEventAdapter, AggregateCountsAcrossWorkspaces) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    model.ensureSessionIn(kWorkspaceB, kSessionB);

    adapter.onEvent(user_event(kSessionA, "a-1", "one"));
    adapter.onEvent(user_event(kSessionB, "b-1", "two"));

    EXPECT_EQ(model.aggregate.current.activeCount, 2u);
    EXPECT_EQ(model.aggregate.current.waitingCount, 0u);
}

} // namespace

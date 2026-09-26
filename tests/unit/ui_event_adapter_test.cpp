#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    model.workspaces.emplace(kWorkspaceA, std::move(a));
    WorkspaceModel b;
    b.id = kWorkspaceB;
    b.cwd = "/work/b";
    b.title = "beta";
    model.workspaces.emplace(kWorkspaceB, std::move(b));
    model.focusSessionIn(kWorkspaceA, kSessionA);
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

TEST(UiEventAdapter, HostNoticeSessionLifecycleCarriesSessionId) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    const SessionId peer{"peer-session"};

    protocol::HostNotice created;
    created.kind = protocol::HostNoticeKind::SessionCreated;
    created.session = peer;
    adapter.onHostNotice(kWorkspaceB, created);
    adapter.onHostNotice(kWorkspaceB, created);

    const auto workspace = model.workspaces.find(kWorkspaceB);
    ASSERT_NE(workspace, model.workspaces.end());
    // 58-E25: a SessionCreated notice materializes state WITHOUT a cell.
    std::size_t matches = 0;
    for (const SessionCell& cell : workspace->second.sessions) {
        if (cell.id == peer) {
            ++matches;
        }
    }
    EXPECT_EQ(matches, 0u) << "SessionCreated must not insert a cell (58-E25)";
    ASSERT_NE(model.session(peer), nullptr);
    EXPECT_EQ(model.session(peer)->workspace, kWorkspaceB);

    protocol::HostNotice closed;
    closed.kind = protocol::HostNoticeKind::SessionClosed;
    closed.session = peer;
    adapter.onHostNotice(kWorkspaceB, closed);

    EXPECT_EQ(model.session(peer), nullptr) << "SessionClosed must erase the session";
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

Event turn_started_event(const SessionId& session, std::uint64_t turn,
                         payload::TurnOrigin origin) {
    Event event;
    event.id.value = "ts-" + std::to_string(turn);
    event.session_id = session;
    event.type = EventType::TurnStarted;
    event.payload = payload::TurnStarted{turn, origin};
    return event;
}

Event turn_ended_event(const SessionId& session, std::uint64_t turn) {
    Event event;
    event.id.value = "te-" + std::to_string(turn);
    event.session_id = session;
    event.type = EventType::TurnEnded;
    event.payload = payload::TurnEnded{turn};
    return event;
}

Event compaction_event(const SessionId& session, Sequence boundary, std::size_t estimate,
                       std::string model, std::string summary) {
    Event event;
    event.id.value = "cc-" + std::to_string(boundary);
    event.session_id = session;
    event.type = EventType::ContextCompaction;
    payload::ContextCompaction payload;
    payload.boundary      = boundary;
    payload.tokenEstimate = estimate;
    payload.model         = std::move(model);
    payload.summary       = std::move(summary);
    event.payload         = payload;
    return event;
}

TEST(UiEventAdapter, CompactionMarkerProjectsFromEvent) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    const std::vector<UiEvent> events =
        adapter.adapt(compaction_event(kSessionA, 8, 42, "fake-model", "the summary"));
    ASSERT_EQ(events.size(), 1u);
    const auto* marker = std::get_if<CompactionMarker>(&events[0].value);
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->session, kSessionA);
    EXPECT_EQ(marker->boundary, 8u);
    EXPECT_EQ(marker->tokenEstimate, 42u);
    EXPECT_EQ(marker->model, "fake-model");
    EXPECT_EQ(marker->summary, "the summary");
}

TEST(UiEventAdapter, MaintenanceTurnEmitsCompactedNotice) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    adapter.onEvent(turn_started_event(kSessionA, 1, payload::TurnOrigin::Maintenance));
    adapter.onEvent(compaction_event(kSessionA, 8, 42, "fake-model", "the summary"));
    adapter.onEvent(turn_ended_event(kSessionA, 1));

    const SessionUiState* state = model.session(kSessionA);
    ASSERT_NE(state, nullptr);
    bool marker = false;
    bool notice = false;
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.text.find("compacted history") != std::string::npos) {
            marker = true;
        }
        if (entry.text.find("compaction complete") != std::string::npos) {
            notice = true;
        }
    }
    EXPECT_TRUE(marker);
    EXPECT_TRUE(notice);
}

TEST(UiEventAdapter, MaintenanceTurnEmitsNotNeededNotice) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    adapter.onEvent(turn_started_event(kSessionA, 1, payload::TurnOrigin::Maintenance));
    adapter.onEvent(turn_ended_event(kSessionA, 1));

    const SessionUiState* state = model.session(kSessionA);
    ASSERT_NE(state, nullptr);
    bool notice = false;
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.text.find("compaction skipped") != std::string::npos) {
            notice = true;
        }
    }
    EXPECT_TRUE(notice);
}

TEST(UiEventAdapter, SessionRenamedAdaptsToTitleChangeOnly) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    Event event;
    event.id.value   = "rename-1";
    event.session_id = kSessionA;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::SessionRenamed;
    event.payload    = payload::SessionRenamed{"short-name", payload::RenameOrigin::User};

    adapter.onSessionEnvelope(kWorkspaceA, envelope(event));

    const SessionUiState* state = model.session(kSessionA);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->conversation.entries.empty());

    const auto workspace = model.workspaces.find(kWorkspaceA);
    ASSERT_NE(workspace, model.workspaces.end());
    ASSERT_FALSE(workspace->second.sessions.empty());
    EXPECT_EQ(workspace->second.sessions.front().title, "short-name");
}

// 58-U22 (58-E30): SubagentSpawned adapts to one UiEvent::SubagentSpawned.
TEST(UiEventAdapter, UI58_U22_SubagentSpawnedAdapted) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    Event event;
    event.id.value   = "spawn-1";
    event.session_id = kSessionA;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::SubagentSpawned;
    event.payload    = payload::SubagentSpawned{SessionId{"child-1"}, "task text"};

    const std::vector<UiEvent> adapted = adapter.adapt(event);
    ASSERT_EQ(adapted.size(), 1u);
    const auto* spawned = std::get_if<SubagentSpawned>(&adapted[0].value);
    ASSERT_NE(spawned, nullptr);
    EXPECT_EQ(spawned->session, kSessionA);
    EXPECT_EQ(spawned->subagent, SessionId{"child-1"});
    EXPECT_EQ(spawned->task, "task text");
}

// 58-U23 (58-E30): fan-in carries the status derived from SubagentOutcome.
TEST(UiEventAdapter, UI58_U23_SubagentFanInCarriesStatus) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    const auto fan_in = [&](payload::SubagentOutcome outcome) {
        Event event;
        event.id.value   = "fanin-" + std::to_string(static_cast<int>(outcome));
        event.session_id = kSessionA;
        event.timestamp  = std::chrono::system_clock::now();
        event.type       = EventType::SubagentFanIn;
        event.payload    = payload::SubagentFanIn{SessionId{"child-1"}, outcome, "summary", false};
        const std::vector<UiEvent> adapted = adapter.adapt(event);
        EXPECT_EQ(adapted.size(), 1u);
        const auto* updated = std::get_if<SubagentUpdated>(&adapted[0].value);
        EXPECT_NE(updated, nullptr);
        return updated == nullptr ? SubagentStatus::Running : updated->status;
    };

    EXPECT_EQ(fan_in(payload::SubagentOutcome::Completed), SubagentStatus::Completed);
    EXPECT_EQ(fan_in(payload::SubagentOutcome::Failed), SubagentStatus::Failed);
    EXPECT_EQ(fan_in(payload::SubagentOutcome::Cancelled), SubagentStatus::Cancelled);
}

// 58-U24 (58-A10/E46, HIGH-1): forget_session clears a session's dedup only.
TEST(UiEventAdapter, UI58_U24_ForgetSessionClearsDedup) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    const Event a1 = user_event(kSessionA, "a-1", "one");
    const Event a2 = user_event(kSessionA, "a-2", "two");
    const Event b1 = user_event(kSessionB, "b-1", "bee");

    adapter.onSessionEnvelope(kWorkspaceA, envelope(a1));
    adapter.onSessionEnvelope(kWorkspaceA, envelope(a2));
    adapter.onSessionEnvelope(kWorkspaceB, envelope(b1));
    EXPECT_EQ(model.session(kSessionA)->conversation.entries.size(), 2u);

    adapter.forget_session(kSessionA);
    adapter.forget_session(kSessionA);   // absent id after the first call: no-op

    adapter.onSessionEnvelope(kWorkspaceA, envelope(a1));
    adapter.onSessionEnvelope(kWorkspaceA, envelope(a2));
    EXPECT_EQ(model.session(kSessionA)->conversation.entries.size(), 4u)
        << "forgotten ids must be applied again";

    adapter.onSessionEnvelope(kWorkspaceB, envelope(b1));
    EXPECT_EQ(model.session(kSessionB)->conversation.entries.size(), 1u)
        << "another session's dedup is untouched";
}

} // namespace

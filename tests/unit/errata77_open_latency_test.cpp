// 77 §10.1: the session-open latency errata, driven through the real
// `SupervisorHarness` seams. OL1/OL4/OL5/OL14/OL16/OL7/OL-F1/OL15 exercise the
// real sink handlers (`feed_sink_envelope`/`deliver_sink_notice`) and the real
// `resume_after_attach`/`maybe_post_opening_expiry` production paths.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/short_temp.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

const SessionId kSession{"s1"};
const SessionId kChild{"a1b2c3d4-1111-4111-8111-111111111111"};

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata77-ui"},
                          protocol::ClientRole::Supervisor};
}

RegistryConfig registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = 200ms;
    config.lock_retry_interval = 5ms;
    return config;
}

WorkspaceModel live_workspace(const WorkspaceId& id, const std::string& title) {
    WorkspaceModel workspace;
    workspace.id           = id;
    workspace.title        = title;
    workspace.cwd          = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    return workspace;
}

protocol::SessionEnvelope envelope_for(const SessionId& session, const std::string& id,
                                       EventType type, nlohmann::json payload) {
    Event event;
    event.id.value   = id;
    event.session_id = session;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = type;
    event.payload    = std::move(payload);
    protocol::SessionEnvelope envelope;
    envelope.session = session;
    envelope.event   = std::move(event);
    return envelope;
}

protocol::SessionEnvelope assistant_envelope(const SessionId& session, const std::string& id,
                                             const std::string& text) {
    payload::AssistantMessage message;
    message.id = MessageId{"m-" + id};
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content.push_back(std::move(block));
    return envelope_for(session, id, EventType::AssistantMessage, std::move(message));
}

protocol::SessionEnvelope turn_started_envelope(const SessionId& session, const std::string& id) {
    return envelope_for(session, id, EventType::TurnStarted, payload::TurnStarted{});
}

protocol::SessionEnvelope turn_ended_envelope(const SessionId& session, const std::string& id) {
    return envelope_for(session, id, EventType::TurnEnded, payload::TurnEnded{});
}

struct Fixture {
    explicit Fixture(const std::string& name)
        : root(name), registry(WorkspaceRegistry::open(registry_config(root.path()))) {
        std::filesystem::create_directories(root.path() / "ws");
        row = registry->registerWorkspace(root.path() / "ws", "ws");
        workspace = row.id;

        SupervisorRunOptions options;
        options.registry = registry.get();
        options.identity = test_identity();
        harness          = make_supervisor_harness(std::move(options));

        WorkspaceModel model = live_workspace(workspace, "ws");
        SessionCell cell;
        cell.id = kSession;
        model.sessions.push_back(cell);
        harness->seed_active_workspace(model);
        harness->activate_session(workspace, kSession);
    }

    UiModel& model() { return harness->mutable_model(); }
    SessionUiState* state() { return model().session(kSession); }

    void install_idle_resume() {
        harness->install_method_reply(std::string(protocol::method::kSessionResume),
                                      nlohmann::json{{"status", "Idle"}}, 0);
    }
    void install_running_resume() {
        harness->install_method_reply(std::string(protocol::method::kSessionResume),
                                      nlohmann::json{{"status", "Running"}}, 0);
    }

    protocol::HostNotice replay_complete() const {
        protocol::HostNotice notice;
        notice.kind      = protocol::HostNoticeKind::ReplayComplete;
        notice.workspace = protocol::WorkspaceId{workspace.value};
        notice.session   = kSession;
        return notice;
    }

    void replay(const protocol::SessionEnvelope& envelope, const std::string& cursor) {
        harness->feed_sink_envelope(workspace, envelope, true,
                                    protocol::EventCursor{cursor});
    }
    void live(const protocol::SessionEnvelope& envelope) {
        harness->feed_sink_envelope(workspace, envelope, false, std::nullopt);
    }

    ShortTempRoot root;
    std::unique_ptr<WorkspaceRegistry> registry;
    WorkspaceRecord row;
    WorkspaceId workspace;
    std::unique_ptr<SupervisorHarness> harness;
};

TEST(Errata77OpenLatency, OL1OneContextShowPerResumeNotPerReplayedMessage) {
    Fixture fixture("errata77-ol1");
    fixture.install_idle_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();
    EXPECT_EQ(fixture.harness->submitted_count("context.show"), 1u);

    constexpr std::size_t kMessages = 1000;
    for (std::size_t index = 0; index < kMessages; ++index) {
        const std::string id = "a" + std::to_string(index);
        fixture.replay(assistant_envelope(kSession, id, "text-" + id), "c-" + id);
    }
    EXPECT_EQ(fixture.harness->submitted_count("context.show"), 1u);

    fixture.live(assistant_envelope(kSession, "live-1", "live text"));
    EXPECT_EQ(fixture.harness->submitted_count("context.show"), 2u);
}

TEST(Errata77OpenLatency, OL7ReplayedHistoryIsPreserved) {
    Fixture fixture("errata77-ol7");
    constexpr std::size_t kMessages = 1000;
    for (std::size_t index = 0; index < kMessages; ++index) {
        const std::string id = "a" + std::to_string(index);
        fixture.replay(assistant_envelope(kSession, id, "text-" + id), "c-" + id);
    }
    ASSERT_NE(fixture.state(), nullptr);
    EXPECT_EQ(fixture.state()->conversation.entries.size(), kMessages);
}

TEST(Errata77OpenLatency, OL4OL5InterruptedOpenIsIdleAfterReplayComplete) {
    Fixture fixture("errata77-ol45");
    ASSERT_TRUE(fixture.model().beginOpening(fixture.workspace, kSession, "opening"));

    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    // No status recorded yet, so the per-envelope reconcile is a no-op and the
    // derived turn is active; only the `opening` gate keeps the indicator idle.
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Thinking);
    EXPECT_FALSE(fixture.model().active_session_working());
    EXPECT_TRUE(fixture.state()->opening.has_value());

    fixture.harness->set_daemon_turn_status(kSession, "Idle");
    EXPECT_FALSE(fixture.model().active_session_working());

    fixture.harness->deliver_sink_notice(fixture.workspace, fixture.replay_complete());
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Idle);
    EXPECT_FALSE(fixture.model().active_session_working());
    EXPECT_FALSE(fixture.state()->opening.has_value());
    EXPECT_FALSE(fixture.model().aggregate.flash.isFlashing());
    EXPECT_FALSE(fixture.model().has_streaming_reasoning());
}

TEST(Errata77OpenLatency, OL14RunningTurnStaysWorking) {
    Fixture fixture("errata77-ol14");
    fixture.install_running_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();

    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Thinking);
    EXPECT_FALSE(fixture.model().active_session_working());

    fixture.harness->deliver_sink_notice(fixture.workspace, fixture.replay_complete());
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Thinking);
    EXPECT_TRUE(fixture.model().active_session_working());
    EXPECT_FALSE(fixture.state()->opening.has_value());
}

TEST(Errata77OpenLatency, OL16AutoTrackedLiveIsNeverFalseIdled) {
    Fixture fixture("errata77-ol16");
    ASSERT_TRUE(fixture.model().beginOpening(fixture.workspace, kSession, "opening"));
    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    fixture.state()->subagents.agents.push_back(
        SubagentView{kChild, "task", AgentState::Idle, SubagentStatus::Running});

    // No `set_daemon_turn_status` (auto-tracked `live`): the reconcile must be a
    // no-op, so a genuine live turn is not hidden.
    const protocol::HostNotice notice = fixture.replay_complete();
    fixture.harness->deliver_sink_notice(fixture.workspace, notice);
    EXPECT_TRUE(fixture.model().active_session_working());
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Thinking);
    ASSERT_EQ(fixture.state()->subagents.agents.size(), 1u);
    EXPECT_EQ(fixture.state()->subagents.agents[0].status, SubagentStatus::Running);
    EXPECT_FALSE(fixture.model().aggregate.flash.isFlashing());

    // A second ReplayComplete is likewise a no-op (OL-F8).
    fixture.harness->deliver_sink_notice(fixture.workspace, notice);
    EXPECT_TRUE(fixture.model().active_session_working());
    EXPECT_EQ(fixture.state()->subagents.agents[0].status, SubagentStatus::Running);
}

TEST(Errata77OpenLatency, OLX2TimeoutEscapeReconcilesDanglingReplayTurn) {
    Fixture fixture("errata77-olx2");
    fixture.install_idle_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();

    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    // Withhold ReplayComplete; backdate the placeholder past kOpeningTimeout.
    ASSERT_TRUE(fixture.state()->opening.has_value());
    fixture.state()->opening->since = std::chrono::steady_clock::now() - kOpeningTimeout - 1s;

    const auto now = std::chrono::steady_clock::now();
    EXPECT_TRUE(fixture.harness->opening_expiry_tick(now));
    EXPECT_FALSE(fixture.state()->opening.has_value());
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Idle);
    EXPECT_FALSE(fixture.model().active_session_working());
    EXPECT_FALSE(fixture.model().hasAnyOpening());
}

TEST(Errata77OpenLatency, OLX2RunningStatusIsRetainedAtTimeoutEscape) {
    Fixture fixture("errata77-olx2-running");
    fixture.install_running_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();

    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    ASSERT_TRUE(fixture.state()->opening.has_value());
    fixture.state()->opening->since = std::chrono::steady_clock::now() - kOpeningTimeout - 1s;

    const auto now = std::chrono::steady_clock::now();
    EXPECT_TRUE(fixture.harness->opening_expiry_tick(now));
    EXPECT_FALSE(fixture.state()->opening.has_value());
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Thinking);
    EXPECT_TRUE(fixture.model().active_session_working());
}

TEST(Errata77OpenLatency, OLX3LateDanglingTurnAfterEscapeIsStillReconciled) {
    Fixture fixture("errata77-olx3");
    fixture.install_idle_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();

    // A non-dangling prefix, then escape clears opening BEFORE the final turn.
    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    fixture.replay(turn_ended_envelope(kSession, "te-1"), "c-2");
    ASSERT_TRUE(fixture.state()->opening.has_value());
    fixture.state()->opening->since = std::chrono::steady_clock::now() - kOpeningTimeout - 1s;
    EXPECT_TRUE(fixture.harness->opening_expiry_tick(std::chrono::steady_clock::now()));
    ASSERT_FALSE(fixture.state()->opening.has_value());

    // The final dangling turn arrives after the escape and no ReplayComplete.
    fixture.replay(turn_started_envelope(kSession, "ts-2"), "c-3");
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Idle);
    EXPECT_FALSE(fixture.model().active_session_working());
}

TEST(Errata77OpenLatency, OLX3RunningStatusRetainsLateDanglingTurn) {
    Fixture fixture("errata77-olx3-running");
    fixture.install_running_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();

    fixture.replay(turn_started_envelope(kSession, "ts-1"), "c-1");
    fixture.replay(turn_ended_envelope(kSession, "te-1"), "c-2");
    ASSERT_TRUE(fixture.state()->opening.has_value());
    fixture.state()->opening->since = std::chrono::steady_clock::now() - kOpeningTimeout - 1s;
    EXPECT_TRUE(fixture.harness->opening_expiry_tick(std::chrono::steady_clock::now()));

    fixture.replay(turn_started_envelope(kSession, "ts-2"), "c-3");
    EXPECT_EQ(fixture.state()->agent_state, AgentState::Thinking);
    EXPECT_TRUE(fixture.model().active_session_working());
}

TEST(Errata77OpenLatency, OLF1FirstLiveEnvelopeClearsOpening) {
    Fixture fixture("errata77-olf1-live");
    ASSERT_TRUE(fixture.model().beginOpening(fixture.workspace, kSession, "opening"));
    fixture.live(assistant_envelope(kSession, "live-1", "live text"));
    EXPECT_FALSE(fixture.state()->opening.has_value());
}

TEST(Errata77OpenLatency, OL15OpeningIsBoundedByTheProductionSchedulingDecision) {
    Fixture fixture("errata77-ol15");
    fixture.install_idle_resume();
    fixture.harness->resume_after_attach(fixture.workspace, kSession);
    fixture.harness->drain_actions();
    ASSERT_TRUE(fixture.state()->opening.has_value());
    fixture.state()->opening->since = std::chrono::steady_clock::now() - kOpeningTimeout - 1s;

    // The first tick is due (last_opening_post starts at the epoch) and posts the
    // real expire_openings closure, which clears the placeholder synchronously.
    const auto first = std::chrono::steady_clock::now();
    EXPECT_TRUE(fixture.harness->opening_expiry_tick(first));
    EXPECT_FALSE(fixture.model().hasAnyOpening());

    // A later tick, past the rate limit, returns false because the recomputed
    // `opening_pending_` is false (distinguishable from the rate limit).
    EXPECT_FALSE(fixture.harness->opening_expiry_tick(first + 10s));

    // With no opening the decision returns false, falling through to presence.
    EXPECT_FALSE(fixture.harness->opening_expiry_tick(first + 20s));
}

} // namespace

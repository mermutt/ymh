#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/plan_mode_controller.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/plan_mode.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;

EventRecord plan_record(Sequence seq, const SessionId& session, bool active) {
    TypedEvent<payload::PlanMode> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{seq * 10}};
    typed.payload    = payload::PlanMode{active};
    return EventRecord{seq, encode(typed)};
}

EventRecord other_record(Sequence seq, const SessionId& session) {
    TypedEvent<payload::UserMessage> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{seq * 10}};
    typed.payload    = payload::UserMessage{};
    return EventRecord{seq, encode(typed)};
}

struct ControllerEnv {
    test::MemorySessionStore store;
    EventBus                 bus;
    SessionManager           manager{store, bus};
    SessionId                id;
    int                      appends = 0;

    ControllerEnv() {
        SessionOptions options;
        options.cwd           = std::filesystem::temp_directory_path();
        options.serverProfile = "interactive";
        options.model         = "test-model";
        options.title         = "plan";
        id                    = manager.createSession(options);
    }

    std::shared_ptr<Session> session() { return manager.sessionPtr(id); }
};

TEST(PlanModeEvent, RoundTripsThroughWireAndJson) {
    EXPECT_EQ(wire_name(EventType::PlanMode), "plan/mode");
    const std::optional<EventType> parsed = parse_event_type("plan/mode");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, EventType::PlanMode);
    const auto types = all_event_types();
    EXPECT_NE(std::find(types.begin(), types.end(), EventType::PlanMode), types.end());

    TypedEvent<payload::PlanMode> typed;
    typed.id         = make_event_id();
    typed.session_id = SessionId{"session"};
    typed.timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{1}};
    typed.payload    = payload::PlanMode{true};

    const Event encoded = encode(typed);
    EXPECT_EQ(encoded.type, EventType::PlanMode);
    EXPECT_TRUE(encoded.payload.value("active", false));
    const TypedEvent<payload::PlanMode> decoded = decode<payload::PlanMode>(encoded);
    EXPECT_TRUE(decoded.payload.active);

    nlohmann::json json = encoded;
    const Event     round = json.get<Event>();
    EXPECT_EQ(round.type, EventType::PlanMode);
    EXPECT_TRUE(round.payload.get<payload::PlanMode>().active);
}

TEST(PlanModeProjection, LastEventWins) {
    const SessionId session{"s"};
    EXPECT_FALSE(plan_mode_active(EventRange{}));

    EventRange one_true;
    one_true.push_back(plan_record(1, session, true));
    EXPECT_TRUE(plan_mode_active(one_true));

    EventRange true_then_false;
    true_then_false.push_back(plan_record(1, session, true));
    true_then_false.push_back(plan_record(2, session, false));
    EXPECT_FALSE(plan_mode_active(true_then_false));

    EventRange false_then_true;
    false_then_true.push_back(plan_record(1, session, false));
    false_then_true.push_back(plan_record(2, session, true));
    EXPECT_TRUE(plan_mode_active(false_then_true));

    EventRange with_other;
    with_other.push_back(other_record(1, session));
    EXPECT_FALSE(plan_mode_active(with_other));
}

TEST(PlanModeControllerTest, IdleSetCommitsOnce) {
    ControllerEnv env;
    PlanModeController controller([&env](const SessionId& id, payload::PlanMode mode) {
        ++env.appends;
        if (auto session = env.manager.sessionPtr(id)) {
            session->append(mode);
        }
    });

    EXPECT_EQ(controller.set(env.id, false, true), PlanModeSetResult::Committed);
    EXPECT_EQ(env.appends, 1);
    EXPECT_TRUE(controller.active(*env.session()));

    EXPECT_EQ(controller.set(env.id, false, true), PlanModeSetResult::Unchanged);
    EXPECT_EQ(env.appends, 1);
}

TEST(PlanModeControllerTest, OpenTurnQueuesAndFlushes) {
    ControllerEnv env;
    PlanModeController controller([&env](const SessionId& id, payload::PlanMode mode) {
        ++env.appends;
        if (auto session = env.manager.sessionPtr(id)) {
            session->append(mode);
        }
    });

    EXPECT_EQ(controller.set(env.id, true, true), PlanModeSetResult::Queued);
    EXPECT_EQ(env.appends, 0);
    EXPECT_TRUE(controller.apply_pending_at_step_start(*env.session()));
    EXPECT_EQ(env.appends, 1);

    EXPECT_EQ(controller.set(env.id, true, false), PlanModeSetResult::Queued);
    EXPECT_EQ(controller.set(env.id, true, true), PlanModeSetResult::Cancelled);
    EXPECT_EQ(env.appends, 1);

    EXPECT_EQ(controller.set(env.id, true, false), PlanModeSetResult::Queued);
    EXPECT_FALSE(controller.flush_pending_at_turn_end(*env.session()));
    EXPECT_EQ(env.appends, 2);
    EXPECT_FALSE(controller.flush_pending_at_turn_end(*env.session()));
    EXPECT_EQ(env.appends, 2);
}

TEST(PlanModeControllerTest, ProjectionMemoizedPerSession) {
    ControllerEnv env;
    int folds = 0;
    PlanModeController controller(
        [&env](const SessionId& id, payload::PlanMode mode) {
            if (auto session = env.manager.sessionPtr(id)) {
                session->append(mode);
            }
        },
        [&folds](const EventRange& events) {
            ++folds;
            return plan_mode_active(events);
        });

    EXPECT_FALSE(controller.active(*env.session()));
    EXPECT_FALSE(controller.active(*env.session()));
    EXPECT_EQ(folds, 1);

    EXPECT_EQ(controller.set(env.id, false, true), PlanModeSetResult::Committed);
    EXPECT_EQ(folds, 1);
    EXPECT_TRUE(controller.active(*env.session()));
    EXPECT_EQ(folds, 1);
}

TEST(PlanModeControllerTest, RequestExitCommitsAtStepBoundary) {
    ControllerEnv env;
    PlanModeController controller([&env](const SessionId& id, payload::PlanMode mode) {
        if (auto session = env.manager.sessionPtr(id)) {
            session->append(mode);
        }
    });

    EXPECT_EQ(controller.set(env.id, false, true), PlanModeSetResult::Committed);
    controller.request_exit(env.id);
    EXPECT_FALSE(controller.apply_pending_at_step_start(*env.session()));
    EXPECT_FALSE(plan_mode_active(env.session()->events()));
}

TEST(PlanPolicySection, AppendedOnlyWhileActive) {
    ToolRegistry             tools;
    SessionContextAssembler  assembler(tools, "SYS");
    ControllerEnv            env;
    bool                     active = false;
    assembler.set_plan_policy_provider(
        [&active](const Session&) -> std::string { return active ? "PLAN" : std::string{}; });

    const std::vector<Message> inactive = assembler.assemble(*env.session(), TurnContext{});
    ASSERT_FALSE(inactive.empty());
    EXPECT_EQ(inactive.front().content.front().text, "SYS");

    active = true;
    const std::vector<Message> enabled = assembler.assemble(*env.session(), TurnContext{});
    ASSERT_FALSE(enabled.empty());
    EXPECT_EQ(enabled.front().content.front().text, "SYS\n\nPLAN");
}

TEST(PlanPolicySection, SectionAloneFormsTheSystemMessage) {
    ToolRegistry            tools;
    SessionContextAssembler assembler(tools, "");
    ControllerEnv           env;
    assembler.set_plan_policy_provider(
        [](const Session&) -> std::string { return "PLAN"; });

    const std::vector<Message> messages = assembler.assemble(*env.session(), TurnContext{});
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.front().role, Role::System);
    EXPECT_EQ(messages.front().content.front().text, "PLAN");
}

TEST(PlanModePolicy, ReadOnlyDeniesMutatingToolsButNotTheForcedReview) {
    RulePermissionPolicy policy(PermissionConfig{});

    PermissionRequest review;
    review.tool        = "exit_plan_mode";
    review.sandbox     = SandboxMode::ReadOnly;
    review.force_ask   = true;
    review.destructive = false;
    EXPECT_FALSE(tool_is_mutating(review));
    EXPECT_EQ(policy.evaluate(review), PolicyVerdict::Ask);

    PermissionRequest write;
    write.tool        = "write_file";
    write.sandbox     = SandboxMode::ReadOnly;
    write.destructive = true;
    EXPECT_TRUE(tool_is_mutating(write));
    EXPECT_EQ(policy.evaluate(write), PolicyVerdict::Deny);
}

} // namespace

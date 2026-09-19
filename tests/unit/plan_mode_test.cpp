#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    EXPECT_EQ(controller.set(*env.session(), false, true), PlanModeSetResult::Committed);
    EXPECT_EQ(env.appends, 1);
    EXPECT_TRUE(controller.active(*env.session()));

    EXPECT_EQ(controller.set(*env.session(), false, true), PlanModeSetResult::Unchanged);
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

    EXPECT_EQ(controller.set(*env.session(), true, true), PlanModeSetResult::Queued);
    EXPECT_EQ(env.appends, 0);
    EXPECT_TRUE(controller.apply_pending_at_step_start(*env.session()));
    EXPECT_EQ(env.appends, 1);

    EXPECT_EQ(controller.set(*env.session(), true, false), PlanModeSetResult::Queued);
    EXPECT_EQ(controller.set(*env.session(), true, true), PlanModeSetResult::Cancelled);
    EXPECT_EQ(env.appends, 1);

    EXPECT_EQ(controller.set(*env.session(), true, false), PlanModeSetResult::Queued);
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

    EXPECT_EQ(controller.set(*env.session(), false, true), PlanModeSetResult::Committed);
    EXPECT_EQ(folds, 1);
    EXPECT_TRUE(controller.active(*env.session()));
    EXPECT_EQ(folds, 1);
}

// UX-U33: a fresh controller over a NON-empty log folds once, keeps the cache
// across unrelated appends, folds once per other session, and re-folds after
// `erase`. The cold-memo `set()` case is covered at the host layer (H1).
TEST(PlanModeControllerTest, ProjectionMemoColdNonEmptyLogEraseAndOtherSessions) {
    ControllerEnv env;
    // Populate the durable log before the controller exists, so its memo is
    // cold over a non-empty log.
    env.session()->append(payload::PlanMode{true});
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

    EXPECT_TRUE(controller.active(*env.session()));
    EXPECT_EQ(folds, 1);
    EXPECT_TRUE(controller.active(*env.session()));
    EXPECT_EQ(folds, 1);

    // A non-`plan/mode` event must not invalidate the memo.
    env.session()->append(payload::StepStarted{1, 1});
    EXPECT_TRUE(controller.active(*env.session()));
    EXPECT_EQ(folds, 1);

    SessionOptions options;
    options.cwd           = std::filesystem::temp_directory_path();
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "plan-2";
    const SessionId second = env.manager.createSession(options);
    const std::shared_ptr<Session> second_session = env.manager.sessionPtr(second);
    ASSERT_NE(second_session, nullptr);
    EXPECT_FALSE(controller.active(*second_session));
    EXPECT_EQ(folds, 2);

    controller.erase(env.id);
    EXPECT_TRUE(controller.active(*env.session()));
    EXPECT_EQ(folds, 3);
}

// 25 review H1 residual: `set()` must read the logged state and append under
// the same lock that guards `commit_`, so a commit's invalidation window cannot
// make it compare against a cold projection and silently drop the request. The
// blocking append hook parks the worker's `commit_` with the memo invalidated
// (`commit_` invalidates before `append_`), reproducing the exact window.
TEST(PlanModeControllerTest, SetIsAtomicAcrossCommitInvalidationWindow) {
    ControllerEnv            env;
    std::shared_ptr<Session> session = env.session();
    std::mutex               gate_mutex;
    std::condition_variable  gate;
    bool                     append_entered = false;
    bool                     release_append = false;
    std::atomic<bool>        io_done{false};
    int                      appends = 0;

    PlanModeController controller([&](const SessionId& id, payload::PlanMode mode) {
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            ++appends;
            if (appends == 1) {
                append_entered = true;
                gate.notify_all();
                gate.wait(lock, [&] { return release_append; });
            }
        }
        if (auto target = env.manager.sessionPtr(id)) {
            target->append(mode);
        }
    });

    // Log starts inactive; queue `plan on` and let a worker commit it.
    ASSERT_FALSE(controller.active(*session));
    ASSERT_EQ(controller.set(*session, /*turn_open=*/true, true), PlanModeSetResult::Queued);
    std::thread worker([&] { (void)controller.apply_pending_at_step_start(*session); });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate.wait(lock, [&] { return append_entered; });
    }

    // With the worker parked (memo invalid), race `/plan off`. The fixed `set()`
    // blocks on commit_mutex_ until the worker records, reads the committed
    // `true`, and appends `false`; a non-atomic `set()` reads the cold default
    // `false`, returns Unchanged, and loses the request.
    std::thread io([&] {
        (void)controller.set(*session, /*turn_open=*/false, false);
        io_done = true;
    });
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        (void)gate.wait_for(lock, std::chrono::milliseconds(200),
                            [&] { return io_done.load(); });
        release_append = true;
        gate.notify_all();
    }
    worker.join();
    io.join();

    EXPECT_FALSE(controller.active(*session));
    EXPECT_FALSE(plan_mode_active(session->events()));
}

// UX-U31 (controller half): `flush_pending_at_turn_end` is idempotent,
// null-safe, and `noexcept` — an append failure is absorbed and the pending
// selection dropped, leaving the durable log authoritative.
TEST(PlanModeControllerTest, TurnEndFlushIsNoexceptAndDropsOnAppendFailure) {
    ControllerEnv env;
    int  appends         = 0;
    bool append_failure  = false;
    PlanModeController controller([&](const SessionId& id, payload::PlanMode mode) {
        ++appends;
        if (append_failure) {
            throw LeaseLost("boom");
        }
        if (auto session = env.manager.sessionPtr(id)) {
            session->append(mode);
        }
    });

    EXPECT_EQ(controller.set(*env.session(), /*turn_open=*/true, true), PlanModeSetResult::Queued);
    append_failure = true;
    EXPECT_NO_THROW((void)controller.flush_pending_at_turn_end(*env.session()));
    EXPECT_EQ(appends, 1);
    EXPECT_NO_THROW((void)controller.flush_pending_at_turn_end(*env.session()));
    EXPECT_EQ(appends, 1);
    EXPECT_FALSE(plan_mode_active(env.session()->events()));
}

TEST(PlanModeControllerTest, RequestExitCommitsAtStepBoundary) {
    ControllerEnv env;
    PlanModeController controller([&env](const SessionId& id, payload::PlanMode mode) {
        if (auto session = env.manager.sessionPtr(id)) {
            session->append(mode);
        }
    });

    EXPECT_EQ(controller.set(*env.session(), false, true), PlanModeSetResult::Committed);
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

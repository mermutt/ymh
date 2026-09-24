#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/jobs/job_registry.hpp"
#include "ymh/jobs/job_types.hpp"
#include "ymh/jobs/job_wakeup.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/job_tools.hpp"
#include "ymh/tools/tool_context.hpp"

namespace {

using namespace ymh;

class FakeAgent final : public Agent {
public:
    FakeAgent(AgentId id, SessionId session) : id_(std::move(id)), session_(std::move(session)) {}

    AgentId id() const override { return id_; }
    SessionId session() const override { return session_; }
    AgentStatus status() const noexcept override { return agent_status; }
    AgentState state() const noexcept override { return AgentState::Idle; }
    bool disposed() const noexcept override { return false; }
    bool hasPendingWork() const noexcept override { return pending; }

    InboxResult send(Message) override { return InboxResult::Accepted; }
    InboxResult followup(Message message) override {
        ++followups;
        last = std::move(message);
        return followup_result;
    }
    InboxResult steer(Message) override { return InboxResult::Accepted; }
    InboxResult inject(ContextMessage message) override {
        ++injects;
        last_inject = std::move(message);
        return InboxResult::Accepted;
    }
    void cancel() override {}
    void dispose() override {}
    void whenIdle(std::function<void()>) override {}
    void onSettled(std::function<void()>) override {}

    AgentStatus   agent_status    = AgentStatus::Idle;
    bool          pending         = false;
    InboxResult   followup_result = InboxResult::Accepted;
    int           followups       = 0;
    int           injects         = 0;
    Message       last;
    ContextMessage last_inject;

private:
    AgentId   id_;
    SessionId session_;
};

JobHooks ready_hooks(JobOutcome outcome, std::function<std::string()> read = {}) {
    JobHooks hooks;
    hooks.done        = Task<JobOutcome>(std::move(outcome));
    hooks.read_output = std::move(read);
    return hooks;
}

JobHooks running_hooks(std::function<std::string()> read, std::function<void()> on_cancel = {}) {
    JobHooks hooks;
    hooks.read_output = std::move(read);
    hooks.cancel      = [on_cancel = std::move(on_cancel)](std::string_view) {
        if (on_cancel) {
            on_cancel();
        }
    };
    return hooks;
}

const AgentId kOwner{"agent-1"};
const AgentId kOther{"agent-2"};

TEST(JobTypes, StatusAndIdRoundTrip) {
    for (const JobStatus status : {JobStatus::Running, JobStatus::Stopping, JobStatus::Completed,
                                   JobStatus::Killed, JobStatus::Failed}) {
        const auto parsed = parse_job_status(job_status_name(status));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, status);
    }
    EXPECT_FALSE(parse_job_status("bogus").has_value());

    const JobId id{"terminal", 42};
    EXPECT_EQ(to_string(id), "terminal-42");
    const auto parsed = parse_job_id("terminal-42");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, id);
    EXPECT_FALSE(parse_job_id("terminal").has_value());
    EXPECT_FALSE(parse_job_id("terminal-x").has_value());
    EXPECT_FALSE(parse_job_id("-3").has_value());
}

TEST(JobCodec, ChangedRoundTripsWithOwner) {
    payload::JobChanged changed;
    changed.owner   = kOwner;
    changed.kind    = "terminal";
    changed.ordinal = 7;
    changed.status  = JobStatus::Killed;
    changed.label   = "pty 1";

    const nlohmann::json json = changed;
    EXPECT_EQ(json.at("owner").get<std::string>(), "agent-1");
    EXPECT_EQ(json.at("status").get<std::string>(), "killed");
    EXPECT_EQ(json.at("ordinal").get<std::uint64_t>(), 7u);
    EXPECT_EQ(json.get<payload::JobChanged>().owner, kOwner);
    EXPECT_EQ(json.get<payload::JobChanged>().label, "pty 1");
}

TEST(JobCodec, ChangedOmitsOwnerWhenUnowned) {
    payload::JobChanged changed;
    changed.kind    = "subagent";
    changed.ordinal = 1;
    changed.status  = JobStatus::Completed;
    changed.label   = "explore";

    const nlohmann::json json = changed;
    EXPECT_FALSE(json.contains("owner"));
    const payload::JobChanged back = json.get<payload::JobChanged>();
    EXPECT_FALSE(back.owner.has_value());
    EXPECT_EQ(back.kind, "subagent");
    EXPECT_EQ(back.status, JobStatus::Completed);
}

TEST(JobRegistry, StartSettlesReadyOutcomeAndNotifiesOnce) {
    JobRegistry registry;
    std::vector<JobStatus> notices;
    registry.on_job_done([&](const JobSnapshot& snapshot, const Agent*) {
        notices.push_back(snapshot.status);
    });

    const JobId id = registry.start(JobStart{"subagent", "one-shot", std::nullopt, kOwner,
                                             [&] {
                                                 JobOutcome outcome;
                                                 outcome.status = JobStatus::Completed;
                                                 outcome.output = "summary";
                                                 return ready_hooks(std::move(outcome));
                                             }});

    const JobSnapshot snapshot = registry.get(id, kOwner);
    EXPECT_EQ(snapshot.status, JobStatus::Completed);
    EXPECT_TRUE(snapshot.finished_at.has_value());
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices.front(), JobStatus::Completed);

    const JobRead read = registry.read(id, kOwner);
    EXPECT_EQ(read.output, "summary");
    EXPECT_TRUE(read.snapshot.reported);
    EXPECT_EQ(registry.read(id, kOwner).output, "summary");
}

TEST(JobRegistry, RunningJobReadDrainsThenKillSettles) {
    JobRegistry registry;
    int         drain  = 0;
    int         cancels = 0;
    const JobId id     = registry.start(JobStart{"terminal", "pty", std::nullopt, kOwner, [&] {
        return running_hooks([&] { return "chunk" + std::to_string(++drain); },
                             [&] { ++cancels; });
    }});

    EXPECT_EQ(registry.get(id, kOwner).status, JobStatus::Running);
    EXPECT_EQ(registry.read(id, kOwner).output, "chunk1");
    EXPECT_EQ(registry.read(id, kOwner).output, "chunk2");

    EXPECT_EQ(registry.kill(id, kOwner, "user"), KillResult::Requested);
    EXPECT_EQ(cancels, 1);
    EXPECT_EQ(registry.get(id, kOwner).status, JobStatus::Killed);
    EXPECT_EQ(registry.kill(id, kOwner, "again"), KillResult::AlreadyFinished);
    EXPECT_EQ(registry.read(id, kOwner).output, "chunk3");
}

TEST(JobRegistry, FirstWinsLateSettlementIgnored) {
    JobRegistry registry;
    int         notices = 0;
    registry.on_job_done([&](const JobSnapshot&, const Agent*) { ++notices; });
    const JobId id = registry.start(JobStart{"terminal", "pty", std::nullopt, kOwner, [&] {
        return running_hooks([] { return std::string{}; });
    }});

    JobOutcome first;
    first.status = JobStatus::Completed;
    registry.settle(id, first);

    JobOutcome late;
    late.status = JobStatus::Killed;
    registry.settle(id, late);

    EXPECT_EQ(registry.get(id, kOwner).status, JobStatus::Completed);
    EXPECT_EQ(notices, 1);
}

TEST(JobRegistry, StartThrowRegistersNothing) {
    JobRegistry registry;
    EXPECT_THROW(static_cast<void>(registry.start(JobStart{"terminal", "boom", std::nullopt,
                                                           kOwner,
                                                           []() -> JobHooks {
                                                               throw std::runtime_error("no");
                                                           }})),
                 std::runtime_error);
    EXPECT_TRUE(registry.list(std::nullopt).empty());

    const JobId next = registry.start(JobStart{"terminal", "ok", std::nullopt, kOwner, [] {
        return ready_hooks(JobOutcome{});
    }});
    EXPECT_EQ(next.ordinal, 2u);
}

TEST(JobRegistry, WaitReturnsSettledOrRunningSnapshot) {
    JobRegistry registry;
    const JobId settled = registry.start(JobStart{"subagent", "done", std::nullopt, kOwner, [] {
        return ready_hooks(JobOutcome{});
    }});
    const JobId running = registry.start(JobStart{"terminal", "live", std::nullopt, kOwner, [] {
        return running_hooks([] { return std::string{}; });
    }});

    const auto timeout = std::chrono::milliseconds{5};
    EXPECT_EQ(registry.wait(settled, timeout, kOwner, CancellationToken{}).get().status,
              JobStatus::Completed);
    EXPECT_EQ(registry.wait(running, timeout, kOwner, CancellationToken{}).get().status,
              JobStatus::Running);
}

TEST(JobRegistry, EmitsJobChangedOnStartAndSettle) {
    std::vector<JobStatus> published;
    JobRegistry            registry([&](const JobSnapshot& snapshot) {
        published.push_back(snapshot.status);
    });

    registry.start(JobStart{"subagent", "one", std::nullopt, kOwner, [] {
        JobOutcome outcome;
        outcome.status = JobStatus::Failed;
        outcome.detail = "boom";
        return ready_hooks(std::move(outcome));
    }});

    ASSERT_EQ(published.size(), 2u);
    EXPECT_EQ(published[0], JobStatus::Running);
    EXPECT_EQ(published[1], JobStatus::Failed);
}

TEST(JobRegistry, AttachControllerRecordsName) {
    JobRegistry registry;
    registry.attach_controller("jobs-tool");
    EXPECT_EQ(registry.controller(), "jobs-tool");
}

TEST(JobRegistry, OwnerFenceHidesForeignJobs) {
    JobRegistry registry;
    const JobId owned = registry.start(JobStart{"subagent", "mine", std::nullopt, kOwner, [] {
        return ready_hooks(JobOutcome{});
    }});
    const JobId unowned = registry.start(JobStart{"subagent", "open", std::nullopt, std::nullopt,
                                                  [] { return ready_hooks(JobOutcome{}); }});

    EXPECT_EQ(registry.get(owned, kOwner).id, owned);
    EXPECT_THROW(static_cast<void>(registry.get(owned, kOther)), JobError);
    EXPECT_THROW(static_cast<void>(registry.read(owned, kOther)), JobError);
    EXPECT_THROW(static_cast<void>(registry.kill(owned, kOther, "")), JobError);
    EXPECT_THROW(static_cast<void>(registry.wait(owned, std::chrono::milliseconds{1}, kOther,
                                                 CancellationToken{})),
                 JobError);
    EXPECT_THROW(static_cast<void>(registry.get(owned, std::nullopt)), JobError);

    EXPECT_EQ(registry.list(kOwner).size(), 2u);
    EXPECT_EQ(registry.list(kOther).size(), 1u);
    EXPECT_EQ(registry.list(kOther).front().id, unowned);
    EXPECT_EQ(registry.list(std::nullopt).size(), 1u);
    EXPECT_EQ(registry.list(std::nullopt).front().id, unowned);
    EXPECT_EQ(registry.get(unowned, kOther).id, unowned);
}

TEST(JobRegistry, UnknownJobThrows) {
    JobRegistry registry;
    EXPECT_THROW(static_cast<void>(registry.get(JobId{"nope", 9}, std::nullopt)), JobError);
}

TEST(JobTools, ListOutputAndKillRespectOwner) {
    JobRegistry registry;
    const JobId owned = registry.start(JobStart{"subagent", "mine", std::nullopt, kOwner, [] {
        JobOutcome outcome;
        outcome.output = "the summary";
        return ready_hooks(std::move(outcome));
    }});
    registry.start(JobStart{"subagent", "open", std::nullopt, std::nullopt,
                            [] { return ready_hooks(JobOutcome{}); }});

    test::ToolEnv env("job_tools");
    const JobOwnerResolver owner = [](const ToolContext&) {
        return std::optional<AgentId>{kOwner};
    };
    const JobOwnerResolver other = [](const ToolContext&) {
        return std::optional<AgentId>{kOther};
    };

    auto list_tool   = make_job_list_tool(registry, owner);
    auto output_tool = make_job_output_tool(registry, owner);
    auto kill_tool   = make_job_kill_tool(registry, owner);

    const ToolResult listed =
        list_tool->execute(env.context(), ToolArguments{nlohmann::json::object()}).get();
    EXPECT_EQ(listed.outcome, payload::ToolOutcome::Ok);
    const nlohmann::json list_json = nlohmann::json::parse(listed.output);
    ASSERT_TRUE(list_json.is_array());
    EXPECT_EQ(list_json.size(), 2u);

    const ToolResult output =
        output_tool->execute(env.context(), ToolArguments{{{"job_id", to_string(owned)}}}).get();
    EXPECT_EQ(nlohmann::json::parse(output.output).at("output").get<std::string>(), "the summary");

    auto foreign_output = make_job_output_tool(registry, other);
    EXPECT_THROW(static_cast<void>(foreign_output
                                       ->execute(env.context(),
                                                 ToolArguments{{{"job_id", to_string(owned)}}})
                                       .get()),
                 ToolError);

    const JobId running = registry.start(JobStart{"terminal", "live", std::nullopt, kOwner, [] {
        return running_hooks([] { return std::string{}; });
    }});
    const ToolResult killed =
        kill_tool->execute(env.context(), ToolArguments{{{"job_id", to_string(running)}}}).get();
    EXPECT_EQ(nlohmann::json::parse(killed.output).at("result").get<std::string>(), "requested");
}

struct WakeFixture {
    EventBus       bus;
    JobRegistry    registry;
    std::shared_ptr<FakeAgent> agent =
        std::make_shared<FakeAgent>(kOwner, SessionId{"s-1"});

    JobWakeupPolicy make_policy(JobWakeupConfig config) {
        return JobWakeupPolicy(
            registry, bus, config,
            [this](const AgentId& id) -> std::shared_ptr<Agent> {
                return id == kOwner ? agent : nullptr;
            },
            [this](const SessionId& session) -> std::shared_ptr<Agent> {
                return session.value == "s-1" ? agent : nullptr;
            });
    }
};

void start_ready(JobRegistry& registry, const AgentId& owner) {
    registry.start(JobStart{"subagent", "job", std::nullopt, owner,
                            [] { return ready_hooks(JobOutcome{}); }});
}

void publish_user_message(EventBus& bus, MessageSource::Kind kind) {
    Event event;
    event.id         = EventId{"evt-1"};
    event.session_id = SessionId{"s-1"};
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::UserMessage;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "hi";
    payload::UserMessage message;
    message.id      = "m-1";
    message.content = {std::move(block)};
    message.source  = message_source(kind);
    event.payload   = message;
    bus.publish(event);
}

TEST(JobWakeup, IdleOwnerWakesToBoundThenInjects) {
    WakeFixture fixture;
    JobWakeupPolicy policy = fixture.make_policy(JobWakeupConfig{CompletionDelivery::Wakeup, 2});
    policy.start();

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 1);
    EXPECT_EQ(policy.consecutive_wakes(kOwner), 1u);

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 2);

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 2);
    EXPECT_EQ(fixture.agent->injects, 1);
}

TEST(JobWakeup, UserInputResetsTheBound) {
    WakeFixture fixture;
    JobWakeupPolicy policy = fixture.make_policy(JobWakeupConfig{CompletionDelivery::Wakeup, 1});
    policy.start();

    start_ready(fixture.registry, kOwner);
    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 1);
    EXPECT_EQ(fixture.agent->injects, 1);

    publish_user_message(fixture.bus, MessageSource::Kind::User);
    EXPECT_EQ(policy.consecutive_wakes(kOwner), 0u);

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 2);
}

TEST(JobWakeup, GoalRoundDoesNotResetTheBound) {
    WakeFixture fixture;
    JobWakeupPolicy policy = fixture.make_policy(JobWakeupConfig{CompletionDelivery::Wakeup, 1});
    policy.start();

    start_ready(fixture.registry, kOwner);
    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(policy.consecutive_wakes(kOwner), 1u);

    publish_user_message(fixture.bus, MessageSource::Kind::Goal);
    EXPECT_EQ(policy.consecutive_wakes(kOwner), 1u);

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->injects, 2);
}

TEST(JobWakeup, QuietLeavesCompletionPending) {
    WakeFixture fixture;
    JobWakeupPolicy policy = fixture.make_policy(JobWakeupConfig{CompletionDelivery::Quiet, 3});
    policy.start();

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 0);
    EXPECT_EQ(fixture.agent->injects, 0);
}

TEST(JobWakeup, BusyOwnerIsInjected) {
    WakeFixture fixture;
    fixture.agent->agent_status = AgentStatus::Running;
    JobWakeupPolicy policy = fixture.make_policy(JobWakeupConfig{CompletionDelivery::Wakeup, 3});
    policy.start();

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 0);
    EXPECT_EQ(fixture.agent->injects, 1);
}

TEST(JobWakeup, ReportedCompletionIsSuppressed) {
    WakeFixture fixture;
    fixture.registry.on_job_done([&](const JobSnapshot& snapshot, const Agent*) {
        fixture.registry.read(snapshot.id, kOwner);
    });
    JobWakeupPolicy policy = fixture.make_policy(JobWakeupConfig{CompletionDelivery::Wakeup, 3});
    policy.start();

    start_ready(fixture.registry, kOwner);
    EXPECT_EQ(fixture.agent->followups, 0);
    EXPECT_EQ(fixture.agent->injects, 0);
}

} // namespace

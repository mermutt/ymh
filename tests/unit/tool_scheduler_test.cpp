#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/llm/fake_llm.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

struct ProbeState {
    std::mutex               mutex;
    std::condition_variable  cv;
    std::size_t              active = 0;
    std::size_t              max_active = 0;
    std::size_t              arrivals = 0;
    bool                     released = false;
    std::atomic<std::size_t> started{0};
    std::atomic<std::size_t> exclusive_parallel_active{0};
};

nlohmann::json probe_schema() {
    return nlohmann::json{{"type", "object"},
                          {"properties", {{"delay_ms", {{"type", "integer"}}}}},
                          {"additionalProperties", false}};
}

nlohmann::json empty_schema() {
    return nlohmann::json{{"type", "object"},
                          {"properties", nlohmann::json::object()},
                          {"additionalProperties", false}};
}

class ProbeTool final : public Tool {
public:
    ProbeTool(std::string name, ToolConcurrencyMode mode, std::shared_ptr<ProbeState> state,
              std::size_t release_at = 0)
        : name_(std::move(name)),
          mode_(mode),
          state_(std::move(state)),
          release_at_(release_at) {}

    ToolSchema schema() const override {
        ToolSchema schema;
        schema.name        = ToolName{name_};
        schema.version     = ToolVersion{1, 0};
        schema.description = name_;
        schema.input_schema = probe_schema();
        schema.concurrency  = mode_;
        return schema;
    }

    Task<ToolResult> execute(const ToolContext&, const ToolArguments& arguments) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            ++state_->active;
            ++state_->arrivals;
            state_->max_active = std::max(state_->max_active, state_->active);
            state_->started.fetch_add(1);
            if (mode_ == ToolConcurrencyMode::Exclusive) {
                state_->exclusive_parallel_active.store(state_->active - 1);
            }
            state_->cv.notify_all();
            if (release_at_ > 0) {
                state_->cv.wait_for(lock, 2s, [this] {
                    return state_->released || state_->arrivals >= release_at_;
                });
            } else {
                const auto delay = std::chrono::milliseconds{
                    arguments.value.value("delay_ms", std::int64_t{0})};
                if (delay.count() > 0) {
                    state_->cv.wait_for(lock, delay, [] { return false; });
                }
            }
            --state_->active;
        }
        ToolResult result;
        result.name   = name_;
        result.output = "ok";
        return Task<ToolResult>(std::move(result));
    }

private:
    std::string                 name_;
    ToolConcurrencyMode         mode_;
    std::shared_ptr<ProbeState> state_;
    std::size_t                 release_at_ = 0;
};

class FragileSchemaTool final : public Tool {
public:
    FragileSchemaTool(std::shared_ptr<std::atomic<std::size_t>> calls,
                      std::shared_ptr<std::atomic<std::size_t>> throw_at)
        : calls_(std::move(calls)), throw_at_(std::move(throw_at)) {}

    ToolSchema schema() const override {
        const std::size_t call = calls_->fetch_add(1) + 1;
        if (call >= throw_at_->load()) {
            throw std::runtime_error("schema unavailable");
        }
        ToolSchema schema;
        schema.name         = ToolName{"fragile"};
        schema.version      = ToolVersion{1, 0};
        schema.description  = "fragile";
        schema.input_schema = empty_schema();
        return schema;
    }

    Task<ToolResult> execute(const ToolContext&, const ToolArguments&) override {
        ToolResult result;
        result.name   = "fragile";
        result.output = "ok";
        return Task<ToolResult>(std::move(result));
    }

private:
    std::shared_ptr<std::atomic<std::size_t>> calls_;
    std::shared_ptr<std::atomic<std::size_t>> throw_at_;
};

class DeadlineProbeTool final : public Tool {
public:
    explicit DeadlineProbeTool(std::string name) : name_(std::move(name)) {}

    ToolSchema schema() const override {
        ToolSchema schema;
        schema.name         = ToolName{name_};
        schema.version      = ToolVersion{1, 0};
        schema.description  = name_;
        schema.input_schema = empty_schema();
        schema.concurrency  = ToolConcurrencyMode::ParallelSafe;
        return schema;
    }

    Task<ToolResult> execute(const ToolContext& context, const ToolArguments&) override {
        while (!context.expired()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        throw ToolError{ToolErrorCode::Timeout, "deadline expired"};
    }

private:
    std::string name_;
};

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

FakeToolCallStep call(std::string name, nlohmann::json arguments = nlohmann::json::object()) {
    FakeToolCallStep step;
    step.name      = std::move(name);
    step.arguments = std::move(arguments);
    return step;
}

FakeResponseStep tool_step(std::vector<FakeToolCallStep> calls) {
    FakeResponseStep step;
    step.tool_calls = std::move(calls);
    step.finish     = FinishReason::ToolCalls;
    return step;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

std::size_t count_type(const EventRange& events, EventType type) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type == type) {
            ++count;
        }
    }
    return count;
}

TEST(ToolScheduler, ModelOrderPreservedUnderShuffledCompletion) {
    auto state = std::make_shared<ProbeState>();
    AgentEnv env("sched_order",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step({call("probe", {{"delay_ms", 90}}),
                                 call("probe", {{"delay_ms", 45}}),
                                 call("probe", {{"delay_ms", 0}})}),
                      text_step("done")})),
                 AgentConfig{});
    env.keeper.add(std::make_unique<ProbeTool>("probe", ToolConcurrencyMode::ParallelSafe, state));
    auto agent = env.createAgent();
    ASSERT_EQ(agent->send(user_message("go")), InboxResult::Accepted);

    auto session = env.sessionOf(*agent);
    std::vector<std::string> result_ids;
    for (const EventRecord& record : session->events()) {
        if (record.event.type == EventType::ToolResult) {
            result_ids.push_back(record.event.payload.get<payload::ToolResult>().id);
        }
    }
    ASSERT_EQ(result_ids.size(), 3u);
    EXPECT_EQ(result_ids[0], "call_0_0");
    EXPECT_EQ(result_ids[1], "call_0_1");
    EXPECT_EQ(result_ids[2], "call_0_2");
}

TEST(ToolScheduler, ParallelSafeCallsOverlapOnlyUpToTheBound) {
    auto state = std::make_shared<ProbeState>();

    AgentConfig config;
    config.schedule.max_parallel_tool_calls = 2;
    AgentEnv env("sched_bound",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step({call("probe"), call("probe"), call("probe"), call("probe")}),
                      text_step("done")})),
                 config);
    env.keeper.add(std::make_unique<ProbeTool>("probe", ToolConcurrencyMode::ParallelSafe, state,
                                               2));
    auto agent = env.createAgent();
    ASSERT_EQ(agent->send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(state->max_active, 2u);
}

TEST(ToolScheduler, ExclusiveCallIsABarrier) {
    auto state = std::make_shared<ProbeState>();
    AgentEnv env("sched_barrier",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step({call("parallel", {{"delay_ms", 120}}), call("exclusive")}),
                      text_step("done")})),
                 AgentConfig{});
    env.keeper.add(
        std::make_unique<ProbeTool>("parallel", ToolConcurrencyMode::ParallelSafe, state));
    env.keeper.add(
        std::make_unique<ProbeTool>("exclusive", ToolConcurrencyMode::Exclusive, state));
    auto agent = env.createAgent();
    ASSERT_EQ(agent->send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(state->exclusive_parallel_active.load(), 0u);
}

TEST(ToolScheduler, AbortSynthesizesCancelledForUnstartedCalls) {
    auto state = std::make_shared<ProbeState>();
    AgentConfig config;
    config.schedule.max_parallel_tool_calls = 1;
    AgentEnv env("sched_abort",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step({call("probe"), call("probe"), call("probe")}),
                      text_step("done")})),
                 config);
    env.keeper.add(std::make_unique<ProbeTool>("probe", ToolConcurrencyMode::ParallelSafe, state,
                                               1000));
    auto agent = env.createAgent();

    std::thread turn([&] { (void)agent->send(user_message("go")); });
    for (int attempt = 0; attempt < 400 && state->started.load() == 0; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(state->started.load(), 1u);
    agent->cancel();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
    }
    state->cv.notify_all();
    turn.join();

    auto session = env.sessionOf(*agent);
    std::size_t real      = 0;
    std::size_t synthetic = 0;
    for (const EventRecord& record : session->events()) {
        if (record.event.type != EventType::ToolResult) {
            continue;
        }
        const auto& result = record.event.payload.get<payload::ToolResult>();
        if (result.output == "ok") {
            ++real;
        } else if (result.output == "tool call cancelled") {
            ++synthetic;
        }
    }
    EXPECT_EQ(state->started.load(), 1u);
    EXPECT_EQ(real, 1u);
    EXPECT_EQ(synthetic, 2u);
    EXPECT_EQ(count_type(session->events(), EventType::TurnCancelled), 1u);
}

TEST(ToolScheduler, InternalFailureRejectsWithoutSyntheticResults) {
    auto state    = std::make_shared<ProbeState>();
    auto calls    = std::make_shared<std::atomic<std::size_t>>(0);
    auto throw_at = std::make_shared<std::atomic<std::size_t>>(
        std::numeric_limits<std::size_t>::max());

    AgentConfig config;
    config.schedule.max_parallel_tool_calls = 1;
    AgentEnv env("sched_failure",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step({call("probe"), call("fragile")}), text_step("done")})),
                 config);
    env.keeper.add(std::make_unique<ProbeTool>("probe", ToolConcurrencyMode::ParallelSafe, state));
    env.keeper.add(std::make_unique<FragileSchemaTool>(calls, throw_at));
    auto agent = env.createAgent();

    throw_at->store(calls->load() + 2);
    ASSERT_EQ(agent->send(user_message("go")), InboxResult::Accepted);

    auto session = env.sessionOf(*agent);
    EXPECT_EQ(count_type(session->events(), EventType::TurnFailed), 1u);
    EXPECT_EQ(count_type(session->events(), EventType::ToolResult), 1u);
}

TEST(ToolScheduler, ReminderIsInjectedThroughTheAcceptor) {
    auto state = std::make_shared<ProbeState>();
    AgentEnv env("sched_reminder",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step({call("probe"), call("probe"), call("probe")}),
                      text_step("done")})),
                 AgentConfig{});
    env.keeper.add(std::make_unique<ProbeTool>("probe", ToolConcurrencyMode::ParallelSafe, state));
    auto agent = env.createAgent();
    ASSERT_EQ(agent->send(user_message("go")), InboxResult::Accepted);

    auto session = env.sessionOf(*agent);
    bool reminder = false;
    for (const EventRecord& record : session->events()) {
        if (record.event.type != EventType::ContextInjected) {
            continue;
        }
        const auto& injected = record.event.payload.get<payload::ContextInjected>();
        if (injected.source.plugin == "repeat-tool-reminder") {
            reminder = true;
        }
    }
    EXPECT_TRUE(reminder);
}

TEST(ToolScheduler, UI46_D8_ParallelWorkerJoinedAfterKill) {
    ToolConfig tool_config;
    tool_config.tool_timeout = std::chrono::milliseconds{150};
    AgentEnv env("sched_d8_join",
                 std::make_unique<FakeLLM>(
                     script_of({tool_step({call("deadline")}), text_step("done")})),
                 AgentConfig{},
                 allow_all_permission_config(),
                 {},
                 false,
                 4,
                 nullptr,
                 false,
                 std::nullopt,
                 std::chrono::system_clock::now,
                 false,
                 nullptr,
                 nullptr,
                 nullptr,
                 tool_config);
    env.keeper.add(std::make_unique<DeadlineProbeTool>("deadline"));
    auto agent = env.createAgent();
    ASSERT_EQ(agent->send(user_message("go")), InboxResult::Accepted);

    auto session = env.sessionOf(*agent);
    bool timed_out = false;
    for (const EventRecord& record : session->events()) {
        if (record.event.type != EventType::ToolResult) {
            continue;
        }
        const auto& result = record.event.payload.get<payload::ToolResult>();
        if (result.error.has_value() && *result.error == "Timeout") {
            timed_out = true;
        }
    }
    EXPECT_TRUE(timed_out);
}

} // namespace

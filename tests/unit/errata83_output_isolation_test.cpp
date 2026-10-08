#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/core/event.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

constexpr std::size_t kCap = 1u << 20;

payload::ToolCall tool_call(const std::string& name, nlohmann::json arguments,
                            std::string id = "call-1") {
    payload::ToolCall call;
    call.id        = std::move(id);
    call.name      = name;
    call.arguments = std::move(arguments);
    return call;
}

std::vector<ToolRegistry::Registration> register_builtins(ToolRegistry& registry) {
    std::vector<ToolRegistry::Registration> registrations;
    for (auto& tool : make_builtin_tools()) {
        registrations.push_back(registry.add(std::move(tool)));
    }
    return registrations;
}

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

FakeToolCallStep call_step(std::string name) {
    FakeToolCallStep step;
    step.name      = std::move(name);
    step.arguments = nlohmann::json::object();
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

class EmptyResultTool final : public Tool {
public:
    ToolSchema schema() const override {
        ToolSchema schema;
        schema.name         = ToolName{"empty_result"};
        schema.version      = ToolVersion{1, 0};
        schema.description  = "returns an empty result output";
        schema.input_schema = nlohmann::json{{"type", "object"},
                                             {"properties", nlohmann::json::object()},
                                             {"additionalProperties", false}};
        return schema;
    }

    Task<ToolResult> execute(const ToolContext& context, const ToolArguments&) override {
        ToolResult result;
        result.id      = context.callId();
        result.name    = "empty_result";
        result.outcome = payload::ToolOutcome::Ok;
        return Task<ToolResult>(std::move(result));
    }
};

class RendezvousStreamTool final : public Tool {
public:
    explicit RendezvousStreamTool(std::shared_ptr<std::barrier<>> barrier)
        : barrier_(std::move(barrier)) {}

    ToolSchema schema() const override {
        ToolSchema schema;
        schema.name         = ToolName{"rendezvous"};
        schema.version      = ToolVersion{1, 0};
        schema.description  = "streams its session id, rendezvouses, then materializes";
        schema.input_schema = nlohmann::json{{"type", "object"},
                                             {"properties", nlohmann::json::object()},
                                             {"additionalProperties", false}};
        return schema;
    }

    Task<ToolResult> execute(const ToolContext& context, const ToolArguments&) override {
        const std::string label = context.sessionId().value;
        context.output().write(label);
        barrier_->arrive_and_wait();
        ToolResult result;
        result.id      = context.callId();
        result.name    = "rendezvous";
        result.outcome = payload::ToolOutcome::Ok;
        result.output  = context.output().materialize(kCap);
        return Task<ToolResult>(std::move(result));
    }

private:
    std::shared_ptr<std::barrier<>> barrier_;
};

std::string all_tool_output(const std::shared_ptr<Session>& session) {
    std::string output;
    for (const EventRecord& record : session->events()) {
        if (record.event.type == EventType::ToolResult) {
            output += record.event.payload.get<payload::ToolResult>().output;
        }
    }
    return output;
}

TEST(Errata83, T1CallOutputSinkIsolationAndLiveTee) {
    auto live = std::make_shared<OutputRing>(kCap);
    CallOutputSink a(1024, live);
    CallOutputSink b(1024, live);
    a.write("alpha");
    b.write("beta");
    EXPECT_EQ(a.materialize(kCap), "alpha");
    EXPECT_EQ(b.materialize(kCap), "beta");
    EXPECT_EQ(live->tail(kCap), "alphabeta");

    auto live2 = std::make_shared<OutputRing>(kCap);
    CallOutputSink wrapped(4, live2);
    CallOutputSink clean(4, live2);
    wrapped.write("0123456789");
    clean.write("xy");
    EXPECT_TRUE(wrapped.truncated());
    EXPECT_FALSE(clean.truncated());
    EXPECT_EQ(wrapped.materialize(kCap), "6789");
    EXPECT_EQ(clean.materialize(kCap), "xy");
    EXPECT_EQ(live2->tail(kCap), "0123456789xy");
}

TEST(Errata83, T2TwoShellCallsDoNotBleed) {
    ToolEnv env("errata83_t2");
    ToolRegistry registry;
    auto registrations = register_builtins(registry);
    const SessionId session = env.session->id();

    const auto run_shell = [&](const std::string& command, const std::string& call_id) {
        CallOutputSink sink(kCap, env.governor.ringFor(session, nullptr));
        ToolContext context(env.env, *env.session, env.logger, CancellationToken{},
                            env.governor, sink, env.permission, call_id, 1, 1, std::nullopt);
        return registry.execute(tool_call("shell", {{"command", command}}, call_id), context).get();
    };

    const ToolResult first = run_shell("printf FIRST", "call-1");
    EXPECT_EQ(first.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(first.output.find("FIRST"), std::string::npos);

    const ToolResult second = run_shell("printf SECOND", "call-2");
    EXPECT_NE(second.output.find("SECOND"), std::string::npos);
    EXPECT_EQ(second.output.find("FIRST"), std::string::npos);
    EXPECT_FALSE(second.truncated);

    ToolRegistry empty_registry;
    const auto empty_registration = empty_registry.add(std::make_unique<EmptyResultTool>());
    CallOutputSink empty_sink(kCap, env.governor.ringFor(session, nullptr));
    ToolContext empty_context(env.env, *env.session, env.logger, CancellationToken{},
                              env.governor, empty_sink, env.permission, "call-3", 1, 1,
                              std::nullopt);
    const ToolResult empty =
        empty_registry
            .execute(tool_call("empty_result", nlohmann::json::object(), "call-3"), empty_context)
            .get();
    EXPECT_EQ(empty.output, "");
}

TEST(Errata83, T3SinkLifetimeAndNonMovable) {
    static_assert(!std::is_copy_constructible_v<CallOutputSink>);
    static_assert(!std::is_copy_assignable_v<CallOutputSink>);
    static_assert(!std::is_move_constructible_v<CallOutputSink>);
    static_assert(!std::is_move_assignable_v<CallOutputSink>);

    auto live = std::make_shared<OutputRing>(kCap);
    {
        CallOutputSink sink(kCap, live);
        sink.write("scoped");
        EXPECT_EQ(sink.materialize(kCap), "scoped");
    }
    EXPECT_EQ(live->tail(kCap), "scoped");
}

TEST(Errata83, T4ConcurrentCallOutputSinkIsolation) {
    std::barrier sync(2);
    CallOutputSink a(kCap, nullptr);
    CallOutputSink b(kCap, nullptr);
    std::string a_out;
    std::string b_out;
    std::thread ta([&] {
        a.write("AAAA");
        sync.arrive_and_wait();
        a_out = a.materialize(kCap);
    });
    std::thread tb([&] {
        b.write("BBBB");
        sync.arrive_and_wait();
        b_out = b.materialize(kCap);
    });
    ta.join();
    tb.join();
    EXPECT_EQ(a_out, "AAAA");
    EXPECT_EQ(b_out, "BBBB");
}

TEST(Errata83, T5ParentAndSubagentConcurrentStreamingIsolate) {
    auto barrier = std::make_shared<std::barrier<>>(2);
    AgentEnv env("errata83_t5",
                 std::make_unique<FakeLLM>(
                     FakeScript{{tool_step({call_step("rendezvous")}),
                                 tool_step({call_step("rendezvous")}), text_step("parent-done"),
                                 text_step("child-done")}}));
    env.keeper.add(std::make_unique<RendezvousStreamTool>(barrier));
    auto parent = env.createAgent();

    ChildSpawnRequest request;
    request.options.cwd           = env.workspace.path();
    request.options.serverProfile = "interactive";
    request.options.model         = "fake-model";
    request.options.title         = "errata83-child";
    request.options.parentSession = parent->session();
    request.parent_depth          = 0;
    request.max_depth             = 3;
    const std::expected<AgentId, AgentError> created = env.registry.createChild(request);
    ASSERT_TRUE(created.has_value());
    auto child = env.registry.getShared(*created);
    ASSERT_NE(child, nullptr);

    std::thread parent_turn([&] { (void)parent->send(user_message("go")); });
    std::thread child_turn([&] { (void)child->send(user_message("go")); });
    parent_turn.join();
    child_turn.join();

    const std::string parent_label = parent->session().value;
    const std::string child_label  = child->session().value;
    ASSERT_NE(parent_label, child_label);

    const std::string parent_out = all_tool_output(env.sessionOf(*parent));
    const std::string child_out  = all_tool_output(env.sessionOf(*child));
    EXPECT_NE(parent_out.find(parent_label), std::string::npos);
    EXPECT_EQ(parent_out.find(child_label), std::string::npos);
    EXPECT_NE(child_out.find(child_label), std::string::npos);
    EXPECT_EQ(child_out.find(parent_label), std::string::npos);

    auto parent_ring = env.governor.ringFor(parent->session(), nullptr);
    auto child_ring  = env.governor.ringFor(child->session(), nullptr);
    ASSERT_TRUE(parent_ring != nullptr);
    ASSERT_TRUE(child_ring != nullptr);
    EXPECT_NE(parent_ring->tail(kCap).find(parent_label), std::string::npos);
    EXPECT_EQ(parent_ring->tail(kCap).find(child_label), std::string::npos);
    EXPECT_NE(child_ring->tail(kCap).find(child_label), std::string::npos);
    EXPECT_EQ(child_ring->tail(kCap).find(parent_label), std::string::npos);
}

TEST(Errata83, T6EvictionSafetyAndStickiness) {
    ResourceGovernor governor;
    std::atomic<bool> disposed{false};
    const SessionId session = make_session_id();

    auto ring = governor.ringFor(session, &disposed);
    ASSERT_TRUE(ring != nullptr);
    ring->append("seed", false);
    disposed.store(true);
    governor.releaseSession(session);
    ASSERT_TRUE(ring != nullptr);
    ring->append("more", false);
    EXPECT_EQ(ring->tail(kCap), "seedmore");

    ring.reset();
    EXPECT_FALSE(governor.ringFor(session, &disposed));
    disposed.store(false);
    auto fresh = governor.ringFor(session, nullptr);
    ASSERT_TRUE(fresh != nullptr);
    EXPECT_TRUE(fresh->tail(kCap).empty());

    ResourceGovernor held_governor;
    std::atomic<bool> held_disposed{false};
    const SessionId held_session = make_session_id();
    auto held_ring = held_governor.ringFor(held_session, &held_disposed);
    CallOutputSink sink(kCap, held_ring);
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            sink.write("z");
        }
    });
    held_disposed.store(true);
    held_governor.releaseSession(held_session);
    writer.join();
    EXPECT_EQ(sink.materialize(kCap), std::string(1000, 'z'));
    EXPECT_EQ(held_ring->tail(kCap), std::string(1000, 'z'));

    for (int iter = 0; iter < 200; ++iter) {
        ResourceGovernor local;
        std::atomic<bool> local_disposed{false};
        const SessionId local_session = make_session_id();
        local.ringFor(local_session, &local_disposed)->append("x", false);

        std::barrier sync(3);
        std::shared_ptr<OutputRing> late;
        std::thread late_thread([&] {
            sync.arrive_and_wait();
            late = local.ringFor(local_session, &local_disposed);
        });
        std::thread disposer([&] {
            sync.arrive_and_wait();
            local_disposed.store(true);
            local.releaseSession(local_session);
        });
        sync.arrive_and_wait();
        late_thread.join();
        disposer.join();

        local_disposed.store(false);
        auto after = local.ringFor(local_session, nullptr);
        ASSERT_TRUE(after != nullptr);
        EXPECT_TRUE(after->tail(64).empty()) << "iteration " << iter;
    }
}

} // namespace

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/llm/fake_llm.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
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

std::size_t terminal_count(const EventRange& events) {
    return count_type(events, EventType::TurnEnded) + count_type(events, EventType::TurnCancelled) +
           count_type(events, EventType::TurnFailed);
}

std::string chunk_text(const EventRange& events) {
    std::string text;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::AssistantChunk) {
            text += record.event.payload.get<payload::AssistantChunk>().text;
        }
    }
    return text;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

FakeResponseStep text_step(std::string text, FinishReason finish = FinishReason::Stop) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = finish;
    return step;
}

FakeResponseStep tool_step(std::string name, nlohmann::json args) {
    FakeResponseStep step;
    step.tool_calls.push_back(FakeToolCallStep{std::move(name), std::move(args), std::nullopt});
    step.finish = FinishReason::ToolCalls;
    return step;
}

class CancellingProvider : public LLMProvider {
public:
    CancellingProvider(FakeScript script, std::function<void()> onFirstDelta)
        : fake_(std::move(script)), onFirstDelta_(std::move(onFirstDelta)) {}

    ProviderId id() const override { return "cancelling"; }
    ProviderCapabilities capabilities() const override { return fake_.capabilities(); }
    std::vector<ModelInfo> models() const override { return fake_.models(); }

    Task<LLMResponse> stream(const LLMRequest& request,
                             StreamSink sink,
                             CancellationToken cancel) override {
        bool       fired = false;
        StreamSink wrapped = [&](const StreamEvent& event) -> SinkFlow {
            const SinkFlow flow = sink(event);
            if (!fired && std::holds_alternative<TextDelta>(event)) {
                fired = true;
                if (onFirstDelta_) {
                    onFirstDelta_();
                }
            }
            return flow;
        };
        return fake_.stream(request, wrapped, cancel);
    }

private:
    FakeLLM               fake_;
    std::function<void()> onFirstDelta_;
};

class CountingCompactor : public Compactor {
public:
    int calls = 0;

    std::optional<payload::ContextCompaction> run(const Session& session,
                                                  const std::vector<Message>&,
                                                  CancellationToken) override {
        ++calls;
        payload::ContextCompaction compaction;
        const EventRange           events = session.events();
        compaction.boundary      = events.empty() ? 0 : events.back().seq;
        compaction.summary       = "compacted";
        compaction.tokenEstimate = 1;
        compaction.model         = "fake";
        return compaction;
    }
};

TEST(AgentLoop, FullTurnCoalescesAndEndsOnce) {
    FakeResponseStep step = text_step("Hello world");
    step.usage            = Usage{10, 3, 0, 0};

    AgentEnv env("agent_full", std::make_unique<FakeLLM>(script_of({step})));
    Agent&   agent = env.createAgent();

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);
    EXPECT_FALSE(agent.hasPendingWork());

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnStarted), 1u);
    EXPECT_EQ(count_type(events, EventType::StepStarted), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 1u);
    EXPECT_EQ(count_type(events, EventType::StepEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::TokenUsage), 1u);
    EXPECT_EQ(terminal_count(events), 1u);
    EXPECT_EQ(chunk_text(events), "Hello world");

    const std::vector<Message> messages = session.deriveMessages();
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].role, Role::User);
    EXPECT_EQ(messages[1].role, Role::Assistant);
    ASSERT_FALSE(messages[1].content.empty());
    EXPECT_EQ(messages[1].content.at(0).text, "Hello world");
}

TEST(AgentLoop, ToolCallLoopWithBuiltins) {
    AgentEnv env("agent_tool",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")})),
                 AgentConfig{}, allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "file-body");

    Agent& agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::ToolCall), 1u);
    EXPECT_EQ(count_type(events, EventType::PermissionDecision), 1u);
    EXPECT_EQ(count_type(events, EventType::ToolResult), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 2u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(terminal_count(events), 1u);

    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ToolResult) {
            const auto& result = record.event.payload.get<payload::ToolResult>();
            EXPECT_EQ(result.name, "read_file");
            EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
            EXPECT_EQ(result.output, "file-body");
        }
    }

    const std::vector<Message> messages = session.deriveMessages();
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages[0].role, Role::User);
    EXPECT_EQ(messages[1].role, Role::Assistant);
    ASSERT_FALSE(messages[1].content.empty());
    EXPECT_EQ(messages[1].content.at(0).kind, ContentBlockKind::ToolUse);
    EXPECT_EQ(messages[1].content.at(0).tool_name, "read_file");
    EXPECT_EQ(messages[2].role, Role::Tool);
    EXPECT_EQ(messages[2].content.at(0).text, "file-body");
    EXPECT_EQ(messages[3].role, Role::Assistant);
}

TEST(AgentLoop, CancelMidStreamYieldsOneTurnCancelled) {
    Agent* agentPtr = nullptr;
    auto   provider = std::make_unique<CancellingProvider>(
        script_of({text_step("abcdefghijklmnopqrstuvwxyz")}),
        [&agentPtr]() {
            if (agentPtr != nullptr) {
                agentPtr->cancel();
            }
        });

    AgentEnv env("agent_cancel", std::move(provider));
    Agent&   agent = env.createAgent();
    agentPtr       = &agent;

    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnCancelled), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
    EXPECT_EQ(terminal_count(events), 1u);
}

TEST(AgentLoop, ProviderFailureYieldsOneTurnFailed) {
    FakeResponseStep step;
    LLMError         error;
    error.code   = LLMErrorCode::NetworkError;
    error.detail = "network down";
    step.error   = error;

    AgentEnv env("agent_fail", std::make_unique<FakeLLM>(script_of({step})));
    Agent&   agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnCancelled), 0u);
    EXPECT_EQ(terminal_count(events), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            EXPECT_EQ(record.event.payload.get<payload::TurnFailed>().code, "ProviderFailed");
        }
    }
    EXPECT_EQ(agent.state(), AgentState::Error);
}

TEST(AgentLoop, ContextLengthExceededRetriesOnce) {
    FakeResponseStep overflow;
    LLMError         error;
    error.code = LLMErrorCode::ContextLengthExceeded;
    overflow.error = error;

    CountingCompactor compactor;
    AgentEnv          env("agent_compact",
                          std::make_unique<FakeLLM>(script_of({overflow, text_step("recovered")})),
                          AgentConfig{}, allow_all_permission_config(), {}, false, 4, &compactor);
    Agent&            agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(compactor.calls, 1);
    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::ContextCompaction), 1u);
    EXPECT_EQ(terminal_count(events), 1u);
}

TEST(AgentLoop, SecondContextOverflowFailsCompaction) {
    FakeResponseStep overflow;
    LLMError         error;
    error.code = LLMErrorCode::ContextLengthExceeded;
    overflow.error = error;

    CountingCompactor compactor;
    AgentEnv env("agent_compact_fail",
                 std::make_unique<FakeLLM>(script_of({overflow, overflow})), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, &compactor);
    Agent&   agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(compactor.calls, 1);
    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    ASSERT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnCancelled), 0u);
    EXPECT_EQ(terminal_count(events), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            EXPECT_EQ(record.event.payload.get<payload::TurnFailed>().code, "CompactionFailed");
        }
    }
}

TEST(AgentLoop, StepLimitFailsTurn) {
    AgentConfig config;
    config.max_steps = 2;

    AgentEnv env("agent_steps",
                 std::make_unique<FakeLLM>(
                     script_of({tool_step("read_file", {{"path", "a.txt"}}),
                                tool_step("read_file", {{"path", "a.txt"}}), text_step("never")})),
                 config, allow_all_permission_config(), {}, true);
    env.workspace.write("a.txt", "x");

    Agent& agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    ASSERT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 0u);
    EXPECT_EQ(terminal_count(events), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            EXPECT_EQ(record.event.payload.get<payload::TurnFailed>().code, "StepLimitExceeded");
        }
    }
}

TEST(AgentLoop, AskPermissionIsResolvedThroughGate) {
    PermissionConfig permission;
    permission.default_verdict = PolicyVerdict::Ask;

    AgentEnv env("agent_gate",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("read_file", {{"path", "a.txt"}}), text_step("ok")})),
                 AgentConfig{}, permission, {}, true, 4, nullptr, true);
    env.workspace.write("a.txt", "x");
    ASSERT_NE(env.gate, nullptr);
    env.gate->set_attention_hook([&env](const PermissionRequestId& id, const PermissionRequest&) {
        env.gate->decide(id, payload::PermissionDecisionKind::AllowAlways, GrantScope::Once,
                         "approved");
    });

    Agent& agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::PermissionDecision), 1u);
    bool sawOk = false;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::PermissionDecision) {
            EXPECT_EQ(record.event.payload.get<payload::PermissionDecision>().decision,
                      payload::PermissionDecisionKind::AllowAlways);
        }
        if (record.event.type == EventType::ToolResult) {
            EXPECT_EQ(record.event.payload.get<payload::ToolResult>().outcome,
                      payload::ToolOutcome::Ok);
            sawOk = true;
        }
    }
    EXPECT_TRUE(sawOk);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
}

TEST(AgentLoop, DeniedToolYieldsDeniedResultAndContinues) {    PermissionConfig permission;
    permission.default_verdict = PolicyVerdict::Deny;

    AgentEnv env("agent_deny",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("read_file", {{"path", "a.txt"}}), text_step("ok")})),
                 AgentConfig{}, permission, {}, true);
    env.workspace.write("a.txt", "x");

    Agent& agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    bool             sawDenied = false;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ToolResult) {
            EXPECT_EQ(record.event.payload.get<payload::ToolResult>().outcome,
                      payload::ToolOutcome::Denied);
            sawDenied = true;
        }
    }
    EXPECT_TRUE(sawDenied);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
}

} // namespace

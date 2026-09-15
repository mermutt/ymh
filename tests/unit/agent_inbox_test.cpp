#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
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

std::vector<payload::TurnOrigin> turn_origins(const EventRange& events) {
    std::vector<payload::TurnOrigin> origins;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnStarted) {
            origins.push_back(record.event.payload.get<payload::TurnStarted>().origin);
        }
    }
    return origins;
}

std::vector<std::string> user_texts(const EventRange& events) {
    std::vector<std::string> texts;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::UserMessage) {
            const auto& message = record.event.payload.get<payload::UserMessage>();
            for (const ContentBlock& block : message.content) {
                if (block.kind == ContentBlockKind::Text) {
                    texts.push_back(block.text);
                }
            }
        }
    }
    return texts;
}

bool request_has_text(const LLMRequest& request, Role role, const std::string& text) {
    for (const Message& message : request.messages) {
        if (message.role != role) {
            continue;
        }
        for (const ContentBlock& block : message.content) {
            if (block.kind == ContentBlockKind::Text && block.text == text) {
                return true;
            }
        }
    }
    return false;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

FakeResponseStep tool_step(std::string name, nlohmann::json args) {
    FakeResponseStep step;
    step.tool_calls.push_back(FakeToolCallStep{std::move(name), std::move(args), std::nullopt});
    step.finish = FinishReason::ToolCalls;
    return step;
}

class HookingProvider : public LLMProvider {
public:
    explicit HookingProvider(FakeScript script) : fake_(std::move(script)) {}

    std::function<void(int, const LLMRequest&)> onStream;
    std::vector<LLMRequest>                     requests;

    ProviderId id() const override { return "hooking"; }
    ProviderCapabilities capabilities() const override { return fake_.capabilities(); }
    std::vector<ModelInfo> models() const override { return fake_.models(); }

    Task<LLMResponse> stream(const LLMRequest& request,
                             StreamSink sink,
                             CancellationToken cancel) override {
        const int index = static_cast<int>(requests.size());
        requests.push_back(request);
        if (onStream) {
            onStream(index, request);
        }
        return fake_.stream(request, sink, cancel);
    }

private:
    FakeLLM fake_;
};

TEST(AgentInbox, SendStartsTurnWithUserOrigin) {
    auto             provider = std::make_unique<HookingProvider>(script_of({text_step("ok")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_send", std::move(provider));
    Agent&           agent = env.createAgent();

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(providerPtr->requests.size(), 1u);

    const std::vector<payload::TurnOrigin> origins = turn_origins(env.sessionOf(agent).events());
    ASSERT_EQ(origins.size(), 1u);
    EXPECT_EQ(origins[0], payload::TurnOrigin::User);
}

TEST(AgentInbox, FollowupQueuedWhileRunningUsesFollowUpOrigin) {
    auto             provider = std::make_unique<HookingProvider>(script_of({text_step("one"), text_step("two")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_followup", std::move(provider));
    Agent&           agent     = env.createAgent();
    Agent*           agentPtr  = &agent;

    providerPtr->onStream = [agentPtr](int index, const LLMRequest&) {
        if (index == 0) {
            agentPtr->followup(user_message("queued"));
        }
    };

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const EventRange                        events  = env.sessionOf(agent).events();
    const std::vector<payload::TurnOrigin>  origins = turn_origins(events);
    ASSERT_EQ(origins.size(), 2u);
    EXPECT_EQ(origins[0], payload::TurnOrigin::User);
    EXPECT_EQ(origins[1], payload::TurnOrigin::FollowUp);

    const std::vector<std::string> texts = user_texts(events);
    ASSERT_EQ(texts.size(), 2u);
    EXPECT_EQ(texts[0], "hi");
    EXPECT_EQ(texts[1], "queued");
}

TEST(AgentInbox, SteerFoldsIntoTheNextStepOnly) {
    auto             provider = std::make_unique<HookingProvider>(
        script_of({tool_step("read_file", {{"path", "a.txt"}}), text_step("done")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_steer", std::move(provider), AgentConfig{},
                         allow_all_permission_config(), {}, true);
    env.workspace.write("a.txt", "x");
    Agent& agent    = env.createAgent();
    Agent* agentPtr = &agent;

    providerPtr->onStream = [agentPtr](int index, const LLMRequest&) {
        if (index == 0) {
            agentPtr->steer(user_message("steer-text"));
        }
    };

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    ASSERT_EQ(providerPtr->requests.size(), 2u);
    EXPECT_FALSE(request_has_text(providerPtr->requests[0], Role::User, "steer-text"));
    EXPECT_TRUE(request_has_text(providerPtr->requests[1], Role::User, "steer-text"));

    const std::vector<std::string> texts = user_texts(env.sessionOf(agent).events());
    ASSERT_EQ(texts.size(), 2u);
    EXPECT_EQ(texts[1], "steer-text");
}

TEST(AgentInbox, InjectIsContextOnlyAndFoldsIntoNextAssembly) {
    auto             provider = std::make_unique<HookingProvider>(script_of({text_step("ok")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_inject", std::move(provider));
    Agent&           agent = env.createAgent();

    ContextMessage context;
    context.role       = Role::System;
    context.text       = "injected-context";
    context.startsTurn = false;
    ASSERT_EQ(agent.inject(context), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);
    EXPECT_FALSE(agent.hasPendingWork());
    EXPECT_EQ(providerPtr->requests.size(), 0u);

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(count_type(env.sessionOf(agent).events(), EventType::ContextInjected), 1u);
    ASSERT_EQ(providerPtr->requests.size(), 1u);
    EXPECT_TRUE(request_has_text(providerPtr->requests[0], Role::System, "injected-context"));
}

TEST(AgentInbox, FollowupsAreConsumedInFifoOrder) {
    auto provider = std::make_unique<HookingProvider>(
        script_of({text_step("one"), text_step("two"), text_step("three")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_fifo", std::move(provider));
    Agent&           agent    = env.createAgent();
    Agent*           agentPtr = &agent;

    providerPtr->onStream = [agentPtr](int index, const LLMRequest&) {
        if (index == 0) {
            agentPtr->followup(user_message("A"));
            agentPtr->followup(user_message("B"));
        }
    };

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const std::vector<std::string> texts = user_texts(env.sessionOf(agent).events());
    ASSERT_EQ(texts.size(), 3u);
    EXPECT_EQ(texts[0], "hi");
    EXPECT_EQ(texts[1], "A");
    EXPECT_EQ(texts[2], "B");
}

TEST(AgentInbox, OverflowIsRejectedAndNeverDropped) {
    AgentConfig config;
    config.max_inbox = 1;

    auto             provider = std::make_unique<HookingProvider>(script_of({text_step("ok")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_full", std::move(provider), config);
    Agent&           agent    = env.createAgent();
    Agent*           agentPtr = &agent;

    InboxResult first  = InboxResult::AgentDisposed;
    InboxResult second = InboxResult::AgentDisposed;
    providerPtr->onStream = [agentPtr, &first, &second](int index, const LLMRequest&) {
        if (index == 0) {
            first  = agentPtr->followup(user_message("A"));
            second = agentPtr->followup(user_message("B"));
        }
    };

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(first, InboxResult::Accepted);
    EXPECT_EQ(second, InboxResult::InboxFull);
}

TEST(AgentInbox, OperationsAfterDisposeReturnAgentDisposed) {
    auto     provider = std::make_unique<HookingProvider>(script_of({text_step("ok")}));
    AgentEnv env("inbox_dispose", std::move(provider));
    Agent&   agent = env.createAgent();

    agent.dispose();
    EXPECT_TRUE(agent.disposed());
    EXPECT_EQ(agent.send(user_message("hi")), InboxResult::AgentDisposed);
    EXPECT_EQ(agent.followup(user_message("hi")), InboxResult::AgentDisposed);
    EXPECT_EQ(agent.steer(user_message("hi")), InboxResult::AgentDisposed);
    EXPECT_EQ(agent.inject(ContextMessage{}), InboxResult::AgentDisposed);
}

TEST(AgentInbox, WhenIdleFiresImmediatelyAndDeferred) {
    auto             provider = std::make_unique<HookingProvider>(script_of({text_step("ok")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv         env("inbox_idle", std::move(provider));
    Agent&           agent    = env.createAgent();
    Agent*           agentPtr = &agent;

    bool immediate = false;
    agent.whenIdle([&immediate]() { immediate = true; });
    EXPECT_TRUE(immediate);

    bool deferred = false;
    providerPtr->onStream = [agentPtr, &deferred](int index, const LLMRequest&) {
        if (index == 0) {
            agentPtr->whenIdle([&deferred]() { deferred = true; });
        }
    };
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_TRUE(deferred);
}

TEST(AgentInbox, SuspendCancelsWithSupersededReason) {
    auto             provider = std::make_unique<HookingProvider>(script_of({text_step("abcdef")}));
    HookingProvider* providerPtr = provider.get();
    AgentEnv env("inbox_suspend", std::move(provider));
    Agent&   agent    = env.createAgent();
    const SessionId sessionId = agent.session();

    providerPtr->onStream = [&env, &sessionId](int index, const LLMRequest&) {
        if (index == 0) {
            env.registry.suspendSession(sessionId);
        }
    };

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    const EventRange events = env.sessionOf(agent).events();
    ASSERT_EQ(count_type(events, EventType::TurnCancelled), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnCancelled) {
            EXPECT_EQ(record.event.payload.get<payload::TurnCancelled>().reason, "superseded");
        }
    }
}

} // namespace

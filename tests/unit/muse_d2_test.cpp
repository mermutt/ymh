#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/openai_adapter.hpp"
#include "ymh/session/events.hpp"

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

class RecordingProvider final : public LLMProvider {
public:
    explicit RecordingProvider(FakeScript script) : fake_(std::move(script)) {}

    ProviderId id() const override { return "recording"; }
    ProviderCapabilities capabilities() const override { return fake_.capabilities(); }

    Task<LLMResponse> stream(const LLMRequest& request, StreamSink sink,
                             CancellationToken cancel) override {
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        return fake_.stream(request, std::move(sink), cancel);
    }

    std::size_t count() const {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

    LLMRequest at(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return requests_.at(index);
    }

private:
    mutable std::mutex      mutex_;
    std::vector<LLMRequest> requests_;
    FakeLLM                 fake_;
};

class StubCompactor final : public Compactor {
public:
    std::optional<payload::ContextCompaction> run(const Session& session,
                                                  const std::vector<Message>&,
                                                  CancellationToken) override {
        payload::ContextCompaction compaction;
        const EventRange           events = session.events();
        compaction.boundary      = events.empty() ? 0 : events.back().seq;
        compaction.summary       = "compacted";
        compaction.tokenEstimate = 1;
        compaction.model         = "fake";
        return compaction;
    }
};

AgentConfig muse_config() {
    AgentConfig config;
    const ModelProfile* profile = find_model_profile("muse-glimmer");
    EXPECT_NE(profile, nullptr);
    if (profile != nullptr) {
        config.profile = *profile;
    }
    return config;
}

std::vector<payload::LlmRequestHeader> headers_of(const EventRange& events) {
    std::vector<payload::LlmRequestHeader> headers;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::LlmRequestHeader) {
            headers.push_back(record.event.payload.get<payload::LlmRequestHeader>());
        }
    }
    return headers;
}

class FakeTransport final : public HttpTransport {
public:
    struct Script {
        HttpResponse response;
        std::size_t  chunk_size = 4096;
    };

    explicit FakeTransport(std::vector<Script> scripts) : scripts_(std::move(scripts)) {}

    Task<HttpResponse> postStream(const HttpRequest& request, HttpBodySink on_body,
                                  CancellationToken cancel) override {
        requests.push_back(request);
        const std::size_t index = calls_++;
        if (index >= scripts_.size()) {
            HttpResponse response;
            response.status = 500;
            response.body   = "{}";
            return Task<HttpResponse>{std::move(response)};
        }
        Script& script = scripts_[index];
        for (std::size_t offset = 0; offset < script.response.body.size();
             offset += script.chunk_size) {
            if (cancel.cancelled()) {
                break;
            }
            const std::size_t length =
                std::min(script.chunk_size, script.response.body.size() - offset);
            if (!on_body(script.response.body.data() + offset, length)) {
                break;
            }
        }
        return Task<HttpResponse>{std::move(script.response)};
    }

    std::vector<HttpRequest> requests;

private:
    std::vector<Script> scripts_;
    std::size_t         calls_ = 0;
};

} // namespace

TEST(MuseD2, FirstStepForcesRequired) {
    auto provider = std::make_unique<RecordingProvider>(
        script_of({tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("muse_d2_first_step", std::move(provider), muse_config(),
                 allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "body");

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    ASSERT_GE(recorder->count(), 1u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "required");
}

TEST(MuseD2, SecondStepDoesNotForce) {
    auto provider = std::make_unique<RecordingProvider>(
        script_of({tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("muse_d2_second_step", std::move(provider), muse_config(),
                 allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "body");

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    ASSERT_GE(recorder->count(), 2u);
    EXPECT_FALSE(recorder->at(1).parameters.tool_choice.has_value());
}

TEST(MuseD2, ForceAfterCompactionStaysOff) {
    StubCompactor compactor;
    auto provider = std::make_unique<RecordingProvider>(
        script_of({tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")}));
    RecordingProvider* recorder = provider.get();

    AgentConfig config              = muse_config();
    config.compaction_threshold_tokens = 1;

    AgentEnv env("muse_d2_compaction", std::move(provider), config,
                 allow_all_permission_config(), {}, true, 4, &compactor);
    env.workspace.write("hello.txt", "body");

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    ASSERT_GE(recorder->count(), 2u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "required");
    EXPECT_FALSE(recorder->at(1).parameters.tool_choice.has_value());
}

TEST(MuseD2, NoToolsDoesNotForce) {
    auto provider = std::make_unique<RecordingProvider>(script_of({text_step("hello")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("muse_d2_no_tools", std::move(provider), muse_config());

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 1u);
    EXPECT_FALSE(recorder->at(0).parameters.tool_choice.has_value());
}

TEST(MuseD2, ExplicitNonePreserved) {
    auto provider = std::make_unique<RecordingProvider>(script_of({text_step("hello")}));
    RecordingProvider* recorder = provider.get();

    AgentConfig config            = muse_config();
    config.parameters.tool_choice = std::string{"none"};

    AgentEnv env("muse_d2_explicit_none", std::move(provider), config,
                 allow_all_permission_config(), {}, true);

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 1u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "none");
}

TEST(MuseD2, ExplicitRequiredPreserved) {
    auto provider = std::make_unique<RecordingProvider>(script_of({text_step("hello")}));
    RecordingProvider* recorder = provider.get();

    AgentConfig config            = muse_config();
    config.parameters.tool_choice = std::string{"required"};

    AgentEnv env("muse_d2_explicit_required", std::move(provider), config,
                 allow_all_permission_config(), {}, true);

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 1u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "required");
}

TEST(MuseD2, ExplicitAutoIsOptOut) {
    auto provider = std::make_unique<RecordingProvider>(script_of({text_step("hello")}));
    RecordingProvider* recorder = provider.get();

    AgentConfig config            = muse_config();
    config.parameters.tool_choice = std::string{"auto"};

    AgentEnv env("muse_d2_explicit_auto", std::move(provider), config,
                 allow_all_permission_config(), {}, true);

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 1u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "auto");
}

TEST(MuseD2, OverrideInBothParametersAndConfig) {
    auto provider = std::make_unique<RecordingProvider>(
        script_of({tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("muse_d2_both_sources", std::move(provider), muse_config(),
                 allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "body");

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    const EventRange events = env.sessionOf(agent)->events();
    const auto       headers = headers_of(events);
    ASSERT_FALSE(headers.empty());
    ASSERT_TRUE(headers.front().config.tool_choice.has_value());
    EXPECT_EQ(*headers.front().config.tool_choice, "required");

    ASSERT_GE(recorder->count(), 1u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "required");
    EXPECT_EQ(*headers.front().config.tool_choice, *recorder->at(0).parameters.tool_choice);
}

TEST(MuseD2, TurnLocalStepNotSessionStep) {
    auto provider = std::make_unique<RecordingProvider>(script_of(
        {tool_step("read_file", {{"path", "hello.txt"}}), text_step("done"),
         tool_step("read_file", {{"path", "hello.txt"}}), text_step("done again")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("muse_d2_turn_local", std::move(provider), muse_config(),
                 allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "body");

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("read it again")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 4u);
    ASSERT_TRUE(recorder->at(0).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(0).parameters.tool_choice, "required");
    EXPECT_FALSE(recorder->at(1).parameters.tool_choice.has_value());
    ASSERT_TRUE(recorder->at(2).parameters.tool_choice.has_value());
    EXPECT_EQ(*recorder->at(2).parameters.tool_choice, "required");
    EXPECT_FALSE(recorder->at(3).parameters.tool_choice.has_value());
}

TEST(MuseD2, NoProfileLeavesChoiceUnset) {
    auto provider = std::make_unique<RecordingProvider>(script_of({text_step("hello")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("muse_d2_no_profile", std::move(provider), AgentConfig{});

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 1u);
    EXPECT_FALSE(recorder->at(0).parameters.tool_choice.has_value());
}

TEST(MuseD2, RetryReplaysFrozenChoice) {
    std::string first_sse;
    {
        nlohmann::json choice;
        choice["index"]         = 0;
        choice["delta"]         = nlohmann::json{{"content", "hi"}};
        choice["finish_reason"] = nullptr;
        nlohmann::json chunk;
        chunk["choices"] = nlohmann::json::array({choice});
        first_sse        = "data: " + chunk.dump() + "\n\n";
    }
    first_sse += "data: [DONE]\n\n";

    FakeTransport::Script retryable;
    retryable.response.status = 500;
    retryable.response.body   = "{}";
    FakeTransport::Script ok;
    ok.response.status = 200;
    ok.response.body   = first_sse;

    auto transport = std::make_shared<FakeTransport>(
        std::vector<FakeTransport::Script>{retryable, ok});

    LLMProviderConfig config;
    config.provider            = "openai-compatible";
    config.base_url            = "http://127.0.0.1:9/v1";
    config.model               = "test-model";
    config.api_key_env         = "YMH_TEST_API_KEY";
    config.api_key             = std::string{"test-key"};
    config.retry.max_attempts  = 2;
    config.retry.base_delay    = std::chrono::milliseconds{1};
    config.retry.max_delay     = std::chrono::milliseconds{2};
    config.retry.jitter        = 0.0;

    OpenAICompatibleProvider provider(config, openai_compatible_capabilities(), transport);

    LLMRequest request;
    request.model = "test-model";
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "hi";
    message.content.push_back(std::move(block));
    request.messages.push_back(std::move(message));
    request.parameters.tool_choice = std::string{"required"};

    const LLMResponse response =
        provider.stream(request, [](const StreamEvent&) { return SinkFlow::Continue; }, {}).get();
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);

    ASSERT_EQ(transport->requests.size(), 2u);
    EXPECT_NE(transport->requests[0].body.find("\"tool_choice\":\"required\""),
              std::string::npos);
    EXPECT_NE(transport->requests[1].body.find("\"tool_choice\":\"required\""),
              std::string::npos);
}

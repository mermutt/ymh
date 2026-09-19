#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/plan_mode.hpp"

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

std::string assistant_text(const EventRange& events) {
    std::string text;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::AssistantMessage) {
            continue;
        }
        for (const ContentBlock& block :
             record.event.payload.get<payload::AssistantMessage>().content) {
            if (block.kind == ContentBlockKind::Text) {
                text += block.text;
            }
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

// A provider that parks inside `stream()` until released, so a test can observe
// the loop while a turn body is live. `on_entered` runs inside `stream()` (the
// AL-U14 probe); `entered_` is published only after it returns.
class LatchProvider final : public LLMProvider {
public:
    explicit LatchProvider(std::function<void()> on_entered = {}, bool fail_on_release = false)
        : on_entered_(std::move(on_entered)), fail_on_release_(fail_on_release) {}

    ProviderId id() const override { return "latch"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken cancel) override {
        if (on_entered_) {
            on_entered_();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entered_ = true;
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, &cancel] { return released_ || cancel.cancelled(); });
        if (cancel.cancelled()) {
            LLMResponse cancelled;
            cancelled.outcome = StreamOutcome::Cancelled;
            cancelled.finish  = FinishReason::Other;
            return Task<LLMResponse>{cancelled};
        }
        if (fail_on_release_) {
            LLMResponse failed;
            failed.outcome = StreamOutcome::Failed;
            failed.error   = LLMError{LLMErrorCode::ProviderInternal, 0, "injected", "injected",
                                      false};
            return Task<LLMResponse>{failed};
        }
        sink(TextDelta{"hi"});
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        response.finish  = FinishReason::Stop;
        return Task<LLMResponse>{response};
    }

    bool wait_entered(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    entered_  = false;
    bool                    released_ = false;
    std::function<void()>   on_entered_;
    bool                    fail_on_release_ = false;
};

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

// Emits a text delta and then a ContextLengthExceeded failure on the first
// call, and a completed text on the second: the retried-attempt text must not
// leak into the settled message (34 §7.2).
class OverflowAfterTextProvider final : public LLMProvider {
public:
    ProviderId id() const override { return "overflow-after-text"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken) override {
        ++calls;
        if (calls == 1) {
            sink(StreamEvent{TextDelta{"attempt-zero-text"}});
            LLMResponse failed;
            failed.outcome      = StreamOutcome::Failed;
            failed.finish       = FinishReason::Error;
            failed.error.code   = LLMErrorCode::ContextLengthExceeded;
            failed.error.detail = "overflow";
            sink(StreamEvent{StreamError{failed.error}});
            return Task<LLMResponse>{failed};
        }
        sink(StreamEvent{TextDelta{"attempt-one-text"}});
        sink(StreamEvent{Finished{FinishReason::Stop, std::nullopt, std::nullopt}});
        LLMResponse done;
        done.outcome = StreamOutcome::Completed;
        done.finish  = FinishReason::Stop;
        return Task<LLMResponse>{done};
    }

    int calls = 0;
};

TEST(AgentLoop, RetriedAttemptTextIsAbsentFromTheSettledMessage) {
    CountingCompactor          compactor;
    std::unique_ptr<OverflowAfterTextProvider> provider =
        std::make_unique<OverflowAfterTextProvider>();
    OverflowAfterTextProvider* provider_raw = provider.get();
    AgentEnv                   env("agent_retry_text", std::move(provider), AgentConfig{},
                                  allow_all_permission_config(), {}, false, 4, &compactor);
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    const EventRange events = session_owner->events();
    EXPECT_EQ(provider_raw->calls, 2);
    EXPECT_EQ(compactor.calls, 1);
    EXPECT_EQ(count_type(events, EventType::AssistantAttempt), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 1u);
    EXPECT_EQ(assistant_text(events), "attempt-one-text");
    EXPECT_EQ(assistant_text(events).find("attempt-zero-text"), std::string::npos);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::AssistantAttempt) {
            EXPECT_FALSE(record.event.payload.get<payload::AssistantAttempt>().stream.empty());
        }
    }
}

TEST(AgentLoop, CompactionReattemptUsesDistinctFrozenRequests) {
    FakeResponseStep overflow;
    LLMError         error;
    error.code     = LLMErrorCode::ContextLengthExceeded;
    overflow.error = error;

    auto provider = std::make_unique<RecordingProvider>(
        script_of({overflow, text_step("recovered")}));
    RecordingProvider* recorder = provider.get();

    CountingCompactor compactor;
    AgentEnv env("agent_distinct_requests", std::move(provider), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, &compactor);
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    ASSERT_EQ(recorder->count(), 2u);
    const auto canonical = [](const LLMRequest& request) {
        return FrozenRequest::freeze(request, LlmCallConfig{}).canonical_json();
    };
    EXPECT_NE(canonical(recorder->at(0)), canonical(recorder->at(1)))
        << "the compaction re-attempt must build a new request, not re-dispatch";
}

TEST(AgentLoop, FullTurnCoalescesAndEndsOnce) {
    FakeResponseStep step = text_step("Hello world");
    step.usage            = Usage{10, 3, 0, 0};

    AgentEnv env("agent_full", std::make_unique<FakeLLM>(script_of({step})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);
    EXPECT_FALSE(agent.hasPendingWork());

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnStarted), 1u);
    EXPECT_EQ(count_type(events, EventType::StepStarted), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantAttempt), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantChunk), 0u);
    EXPECT_EQ(count_type(events, EventType::StepEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::TokenUsage), 1u);
    EXPECT_EQ(terminal_count(events), 1u);
    EXPECT_EQ(assistant_text(events), "Hello world");

    const std::vector<Message> messages = session.deriveMessages();
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].role, Role::User);
    EXPECT_EQ(messages[1].role, Role::Assistant);
    ASSERT_FALSE(messages[1].content.empty());
    EXPECT_EQ(messages[1].content.at(0).text, "Hello world");
}

TEST(AgentLoop, ReplayStatePropagatesToSettledMessage) {
    FakeResponseStep step = text_step("hi");
    ReplayEnvelope   envelope;
    envelope.provider       = "fake";
    envelope.version        = 1;
    envelope.state          = nlohmann::json{{"response_id", "r-1"}};
    step.replay_state       = envelope;

    AgentEnv env("agent_replay_state", std::make_unique<FakeLLM>(script_of({step})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    const EventRange events = session_owner->events();
    bool saw_message = false;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::AssistantMessage) {
            continue;
        }
        const auto& message = record.event.payload.get<payload::AssistantMessage>();
        ASSERT_TRUE(message.replay_state.has_value());
        EXPECT_EQ(message.replay_state->provider, "fake");
        EXPECT_FALSE(message.stream.empty());
        saw_message = true;
    }
    EXPECT_TRUE(saw_message);
}

TEST(AgentLoop, LiveChunksArePublishedButNeverDurable) {
    AgentEnv env("agent_live_chunks",
                 std::make_unique<FakeLLM>(script_of({text_step("streamed")})));

    std::vector<payload::AssistantChunk> live;
    Subscription subscription = env.bus.subscribe([&](const Event& event) {
        if (event.type == EventType::AssistantChunk) {
            live.push_back(event.payload.get<payload::AssistantChunk>());
        }
    });

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    subscription.unsubscribe();

    EXPECT_FALSE(live.empty());
    const EventRange events = env.sessionOf(agent)->events();
    EXPECT_EQ(count_type(events, EventType::AssistantChunk), 0u);
}

TEST(AgentLoop, ToolCallLoopWithBuiltins) {
    AgentEnv env("agent_tool",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")})),
                 AgentConfig{}, allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "file-body");

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
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

TEST(AgentLoop, OneProviderCallPerStep) {
    auto provider = std::make_unique<RecordingProvider>(
        script_of({tool_step("read_file", {{"path", "hello.txt"}}), text_step("done")}));
    RecordingProvider* recorder = provider.get();

    AgentEnv env("agent_one_call_per_step", std::move(provider), AgentConfig{},
                 allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "file-body");

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);

    // Two steps (tool call, then final answer): one provider stream each.
    EXPECT_EQ(recorder->count(), 2u);
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
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    agentPtr       = &agent;

    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnCancelled), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantAttempt), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 0u);
    EXPECT_EQ(terminal_count(events), 1u);
}

TEST(AgentLoop, ProviderFailureYieldsOneTurnFailed) {
    FakeResponseStep step;
    LLMError         error;
    error.code   = LLMErrorCode::NetworkError;
    error.detail = "network down";
    step.error   = error;

    AgentEnv env("agent_fail", std::make_unique<FakeLLM>(script_of({step})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnCancelled), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantAttempt), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 0u);
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
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(compactor.calls, 1);
    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::ContextCompaction), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantAttempt), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 1u);
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
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    EXPECT_EQ(compactor.calls, 1);
    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
    const EventRange events  = session.events();
    ASSERT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnCancelled), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantAttempt), 2u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 0u);
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

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
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

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
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

    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner = env.sessionOf(agent);
    Session& session = *session_owner;
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

// AL-U3/AL5/AL-F5: while a body is parked mid-turn, `hasPendingWork()` is true
// and a `whenIdle` registration is deferred; it fires only after the body
// returns. A dispose/teardown that falsified quiescence would fail here.
TEST(AgentLoop, AL_U3_PausedBodyReportsPendingWorkAndDefersIdleCallback) {
    auto           provider = std::make_unique<LatchProvider>();
    LatchProvider* raw      = provider.get();
    AgentEnv       env("agent_u3", std::move(provider));
    auto           agent_owner = env.createAgent();

    std::atomic<bool> idle_fired{false};
    std::thread       turn([&] { (void)agent_owner->send(user_message("hi")); });
    ASSERT_TRUE(raw->wait_entered());

    EXPECT_TRUE(agent_owner->hasPendingWork());
    EXPECT_EQ(agent_owner->status(), AgentStatus::Running);
    agent_owner->whenIdle([&] { idle_fired.store(true); });
    EXPECT_FALSE(idle_fired.load());

    raw->release();
    turn.join();

    EXPECT_TRUE(idle_fired.load());
    EXPECT_FALSE(agent_owner->hasPendingWork());
}

// AL-U14/AL15/AL-F9: the provider call runs with `control_mutex_` released. The
// probe re-enters the loop from inside `stream()`; if the lock were held across
// the blocking provider call, `hasPendingWork()` would deadlock.
TEST(AgentLoop, AL_U14_ControlMutexIsNotHeldAcrossProviderStream) {
    std::atomic<bool> probed{false};
    AgentLoop*        agent_ptr = nullptr;
    auto              provider  = std::make_unique<LatchProvider>([&] {
        if (agent_ptr != nullptr) {
            (void)agent_ptr->hasPendingWork();
            (void)agent_ptr->state();
            probed.store(true);
        }
    });
    LatchProvider* raw = provider.get();
    AgentEnv       env("agent_u14", std::move(provider));
    auto           agent_owner = env.createAgent();
    agent_ptr                  = agent_owner.get();

    std::thread turn([&] { (void)agent_owner->send(user_message("hi")); });
    ASSERT_TRUE(raw->wait_entered());
    EXPECT_TRUE(probed.load());
    raw->release();
    turn.join();
}

// AL-U20/AL34/AL-F24: the production decision hook captures
// `std::weak_ptr<Session>`, not `[this]`. The test snapshots the REAL hook the
// loop installs, tears the loop/session down, then invokes the captured copy:
// the weak capture no-ops, while a `[this]` capture dereferences freed memory
// (an ASan heap-use-after-free).
TEST(AgentLoop, AL_U20_RealDecisionHookCaptureIsWeak) {
    PermissionConfig permission;
    permission.default_verdict = PolicyVerdict::Ask;

    AgentEnv env("agent_u20",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("read_file", {{"path", "a.txt"}}), text_step("ok")})),
                 AgentConfig{}, permission, {}, true, 4, nullptr, true);
    env.workspace.write("a.txt", "x");
    ASSERT_NE(env.gate, nullptr);

    std::mutex                         id_mutex;
    std::optional<PermissionRequestId> pending_id;
    std::atomic<bool>                  asked{false};
    env.gate->set_attention_hook([&](const PermissionRequestId& id, const PermissionRequest&) {
        std::lock_guard<std::mutex> lock(id_mutex);
        pending_id = id;
        asked.store(true, std::memory_order_release);
    });

    auto                    agent_owner = env.createAgent();
    const AgentId           agent_id    = agent_owner->id();
    std::shared_ptr<Session> session_owner = env.sessionOf(*agent_owner);
    std::weak_ptr<Session>   weak_session  = session_owner;

    std::thread turn([&] { (void)agent_owner->send(user_message("go")); });
    for (int attempt = 0; attempt < 2000 && !asked.load(std::memory_order_acquire); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ASSERT_TRUE(asked.load(std::memory_order_acquire));

    const PermissionGate::DecisionHook hook = env.gate->decision_hook_for_test();
    ASSERT_TRUE(static_cast<bool>(hook));

    PermissionRequestId id;
    {
        std::lock_guard<std::mutex> lock(id_mutex);
        ASSERT_TRUE(pending_id.has_value());
        id = *pending_id;
    }
    env.gate->decide(id, payload::PermissionDecisionKind::Allow, GrantScope::Once, "approved");
    turn.join();

    payload::PermissionDecision recorded;
    recorded.call     = "after-teardown";
    recorded.decision = payload::PermissionDecisionKind::Allow;
    recorded.reason   = "test";

    const std::size_t before = session_owner->events().size();
    hook(recorded);
    EXPECT_EQ(session_owner->events().size(), before + 1);

    env.registry.dispose(agent_id);
    agent_owner.reset();
    session_owner.reset();
    EXPECT_TRUE(weak_session.expired());

    hook(recorded);
}

namespace {

std::size_t error_tool_results(const EventRange& events) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ToolResult &&
            record.event.payload.get<payload::ToolResult>().outcome ==
                payload::ToolOutcome::Error) {
            ++count;
        }
    }
    return count;
}

std::size_t ok_tool_results(const EventRange& events) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ToolResult &&
            record.event.payload.get<payload::ToolResult>().outcome ==
                payload::ToolOutcome::Ok) {
            ++count;
        }
    }
    return count;
}

} // namespace

// UX-U5: `exit_plan_mode` outside plan mode fails closed — one error ToolResult
// and no permission request.
TEST(AgentLoop, UX_U5_ExitPlanModeFailsClosedWhenInactive) {
    AgentEnv env("agent_plan_u5a",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("exit_plan_mode", {{"plan", "do it"}}), text_step("ok")})),
                 AgentConfig{}, allow_all_permission_config(), {}, true, 4, nullptr, false,
                 std::nullopt, std::chrono::system_clock::now, /*enable_plan_mode=*/true);
    auto agent_owner = env.createAgent();
    Agent& agent     = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto session_owner      = env.sessionOf(agent);
    const EventRange events = session_owner->events();
    EXPECT_EQ(count_type(events, EventType::PermissionDecision), 0u);
    EXPECT_EQ(count_type(events, EventType::PlanMode), 0u);
    EXPECT_EQ(error_tool_results(events), 1u);
}

// UX-U5: an active plan with no controller still fails closed.
TEST(AgentLoop, UX_U5_ExitPlanModeFailsClosedWithoutController) {
    AgentEnv env("agent_plan_u5b",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("exit_plan_mode", {{"plan", "do it"}}), text_step("ok")})),
                 AgentConfig{}, allow_all_permission_config(), {}, true);
    auto agent_owner = env.createAgent();
    Agent& agent     = *agent_owner;
    auto session_owner = env.sessionOf(agent);
    session_owner->append(payload::PlanMode{true});
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    const EventRange events = session_owner->events();
    EXPECT_EQ(count_type(events, EventType::PermissionDecision), 0u);
    EXPECT_EQ(count_type(events, EventType::PlanMode), 1u);
    EXPECT_EQ(error_tool_results(events), 1u);
}

// UX-U34: a malformed `plan` argument makes no review and cannot exit plan mode.
TEST(AgentLoop, UX_U34_MalformedPlanMakesNoReview) {
    const std::vector<nlohmann::json> malformed = {
        nlohmann::json::object(), {{"plan", ""}}, {{"plan", "   "}}, {{"plan", 5}}};
    for (const nlohmann::json& args : malformed) {
        AgentEnv env("agent_plan_u34",
                     std::make_unique<FakeLLM>(
                         script_of({tool_step("exit_plan_mode", args), text_step("ok")})),
                     AgentConfig{}, allow_all_permission_config(), {}, true, 4, nullptr, false,
                     std::nullopt, std::chrono::system_clock::now, /*enable_plan_mode=*/true);
        auto agent_owner = env.createAgent();
        Agent& agent     = *agent_owner;
        auto session_owner = env.sessionOf(agent);
        ASSERT_TRUE(env.plan_mode_controller.has_value());
        EXPECT_EQ(env.plan_mode_controller->set(*session_owner, false, true),
                  PlanModeSetResult::Committed);
        ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

        const EventRange events = session_owner->events();
        SCOPED_TRACE(args.dump());
        EXPECT_EQ(count_type(events, EventType::PermissionDecision), 0u);
        EXPECT_EQ(error_tool_results(events), 1u);
        EXPECT_TRUE(plan_mode_active(events));
        EXPECT_EQ(count_type(events, EventType::PlanMode), 1u);
    }
}

// UX-U6: an approved review exits plan mode (Allow => Ok + pending exit, then
// `plan/mode{false}` at the next step boundary); a denied review keeps it active.
TEST(AgentLoop, UX_U6_ApprovedExitCommitsOffDeniedStaysActive) {
    const auto run = [](payload::PermissionDecisionKind decision) {
        AgentEnv env("agent_plan_u6",
                     std::make_unique<FakeLLM>(script_of(
                         {tool_step("exit_plan_mode", {{"plan", "step 1"}}), text_step("ok")})),
                     AgentConfig{}, allow_all_permission_config(), {}, true, 4, nullptr, true,
                     std::nullopt, std::chrono::system_clock::now, /*enable_plan_mode=*/true);
        EXPECT_NE(env.gate, nullptr);
        env.gate->set_attention_hook(
            [&env, decision](const PermissionRequestId& id, const PermissionRequest&) {
                env.gate->decide(id, decision, GrantScope::Once, "reviewed");
            });
        auto agent_owner = env.createAgent();
        Agent& agent     = *agent_owner;
        auto session_owner = env.sessionOf(agent);
        EXPECT_EQ(env.plan_mode_controller->set(*session_owner, false, true),
                  PlanModeSetResult::Committed);
        EXPECT_EQ(agent.send(user_message("go")), InboxResult::Accepted);
        return session_owner->events();
    };

    const EventRange allowed = run(payload::PermissionDecisionKind::AllowAlways);
    EXPECT_EQ(count_type(allowed, EventType::PermissionDecision), 1u);
    EXPECT_EQ(ok_tool_results(allowed), 1u);
    EXPECT_EQ(count_type(allowed, EventType::PlanMode), 2u);
    EXPECT_FALSE(plan_mode_active(allowed));

    const EventRange denied = run(payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(count_type(denied, EventType::PermissionDecision), 1u);
    EXPECT_EQ(ok_tool_results(denied), 0u);
    EXPECT_EQ(count_type(denied, EventType::PlanMode), 1u);
    EXPECT_TRUE(plan_mode_active(denied));
}

// UX-U31 (N5/UX45): a selection queued after the step boundary is committed by
// the turn-exit scope guard on normal end, provider error, and cancellation.
TEST(AgentLoop, UX_U31_TurnEndFlushOnNormalErrorAndCancel) {
    enum class Exit { Normal, Error, Cancel };
    const auto run = [](Exit exit_kind) {
        auto provider = std::make_unique<LatchProvider>(std::function<void()>{},
                                                        exit_kind == Exit::Error);
        LatchProvider* raw = provider.get();
        AgentEnv       env("agent_plan_u31", std::move(provider), AgentConfig{},
                           allow_all_permission_config(), {}, false, 4, nullptr, false,
                           std::nullopt, std::chrono::system_clock::now, /*enable_plan_mode=*/true);
        auto agent_owner = env.createAgent();
        Agent& agent     = *agent_owner;
        auto session_owner = env.sessionOf(agent);
        std::thread turn([&] { (void)agent.send(user_message("go")); });
        EXPECT_TRUE(raw->wait_entered(std::chrono::seconds{2}));
        EXPECT_EQ(env.plan_mode_controller->set(*session_owner, /*turn_open=*/true, true),
                  PlanModeSetResult::Queued);
        if (exit_kind == Exit::Cancel) {
            agent.cancel();
        }
        raw->release();
        turn.join();
        return session_owner->events();
    };

    for (const Exit exit_kind : {Exit::Normal, Exit::Error, Exit::Cancel}) {
        const EventRange events = run(exit_kind);
        EXPECT_EQ(count_type(events, EventType::PlanMode), 1u);
        EXPECT_TRUE(plan_mode_active(events));
        EXPECT_EQ(terminal_count(events), 1u);
    }
}

} // namespace

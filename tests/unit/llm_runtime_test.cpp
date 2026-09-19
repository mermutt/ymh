#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

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

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

SinkFlow continue_sink(const StreamEvent&) {
    return SinkFlow::Continue;
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

LlmCallConfig config_for(std::string provider) {
    LlmCallConfig config;
    config.provider = std::move(provider);
    config.model    = "fake-model";
    return config;
}

FrozenRequest frozen_for(std::string provider) {
    LLMRequest request;
    request.session_id = SessionId{"session"};
    return FrozenRequest::freeze(std::move(request), config_for(std::move(provider)));
}

class NonTerminalProvider final : public LLMProvider {
public:
    [[nodiscard]] ProviderId id() const override { return "nonterminal"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken) override {
        sink(StreamEvent{TextDelta{"partial"}});
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        return Task<LLMResponse>{std::move(response)};
    }
};

class ThrowingProvider final : public LLMProvider {
public:
    [[nodiscard]] ProviderId id() const override { return "throwing"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink, CancellationToken) override {
        throw std::runtime_error("boom");
    }
};

TEST(CallConfigEquals, IsFieldWiseAndStopOrderSensitive) {
    const LlmCallConfig base = config_for("openai-compatible");
    EXPECT_TRUE(call_config_equals(base, base));

    const auto field_is_compared = [&](auto set_value) {
        LlmCallConfig left  = base;
        LlmCallConfig right = base;
        set_value(left);
        EXPECT_FALSE(call_config_equals(left, right)) << "field not compared";
        set_value(right);
        EXPECT_TRUE(call_config_equals(left, right)) << "equal fields reported unequal";
    };

    field_is_compared([](LlmCallConfig& config) { config.provider = "other"; });
    field_is_compared([](LlmCallConfig& config) { config.model = "other"; });
    field_is_compared([](LlmCallConfig& config) { config.reasoning_effort = "high"; });
    field_is_compared([](LlmCallConfig& config) { config.temperature = 0.25; });
    field_is_compared([](LlmCallConfig& config) { config.max_tokens = 512; });
    field_is_compared([](LlmCallConfig& config) { config.stop = {"a"}; });
    field_is_compared([](LlmCallConfig& config) { config.top_p = 0.5; });
    field_is_compared([](LlmCallConfig& config) { config.seed = 7; });
    field_is_compared([](LlmCallConfig& config) { config.tool_choice = "none"; });

    LlmCallConfig ordered_left  = base;
    LlmCallConfig ordered_right = base;
    ordered_left.stop  = {"a", "b"};
    ordered_right.stop = {"b", "a"};
    EXPECT_FALSE(call_config_equals(ordered_left, ordered_right));
}

TEST(FrozenRequest, CanonicalSerializationIsDeterministic) {
    const FrozenRequest first  = frozen_for("openai-compatible");
    const FrozenRequest second = frozen_for("openai-compatible");

    EXPECT_EQ(first.canonical_template(), second.canonical_template());
    EXPECT_EQ(first.canonical_json(), second.canonical_json());
    EXPECT_EQ(first.template_digest(), second.template_digest());
    EXPECT_EQ(first.digest(), second.digest());
    EXPECT_EQ(first.template_digest(), sha256_hex(first.canonical_template()));
    EXPECT_EQ(first.digest(), sha256_hex(first.canonical_json()));
    EXPECT_EQ(first.template_digest().size(), 64u);
}

TEST(FrozenRequest, TemplateExcludesMessagesButCanonicalJsonIncludesThem) {
    LLMRequest request;
    request.session_id = SessionId{"session"};
    request.messages.push_back(user_message("hello"));
    const FrozenRequest frozen = FrozenRequest::freeze(request, config_for("openai-compatible"));

    EXPECT_EQ(frozen.canonical_template().find("hello"), std::string::npos);
    EXPECT_NE(frozen.canonical_json().find("hello"), std::string::npos);
}

TEST(LlmRuntime, PrepareCallIsOneShot) {
    LlmRuntime runtime;
    auto       handle = runtime.register_adapter(
        {"fake"}, std::make_shared<FakeLLM>(script_of({text_step("hi")})));
    (void)handle;

    const FrozenRequest frozen = frozen_for("fake");
    PreparedCall        call =
        runtime.prepare_call(config_for("fake"), CancellationToken{}).get();
    EXPECT_EQ(call.config().provider, "fake");

    const LLMResponse response = call.stream(frozen, [](const StreamEvent&) {
        return SinkFlow::Continue;
    }, CancellationToken{}).get();
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);

    EXPECT_THROW((void)call.stream(frozen, continue_sink, CancellationToken{}),
                 PreparedCallError);
}

TEST(LlmRuntime, PrepareCallConfigMismatchThrows) {
    LlmRuntime runtime;
    auto       handle = runtime.register_adapter(
        {"fake"}, std::make_shared<FakeLLM>(script_of({text_step("hi")})));
    (void)handle;

    PreparedCall call =
        runtime.prepare_call(config_for("fake"), CancellationToken{}).get();
    EXPECT_THROW((void)call.stream(frozen_for("other"), continue_sink, CancellationToken{}),
                 PreparedCallError);
}

TEST(LlmRuntime, NoRouteFailsLoudAndTyped) {
    LlmRuntime runtime;
    EXPECT_THROW((void)runtime.prepare_call(config_for("missing"), CancellationToken{}),
                 NoProviderRouteError);
    EXPECT_THROW((void)runtime.prepare_call(config_for(""), CancellationToken{}),
                 NoProviderRouteError);

    const FrozenRequest frozen = frozen_for("missing");
    EXPECT_THROW((void)runtime.stream(frozen, continue_sink, CancellationToken{}),
                 NoProviderRouteError);
}

TEST(LlmRuntime, EmptyProviderResolvesFirstRegisteredRoute) {
    LlmRuntime runtime;
    auto       first = runtime.register_adapter(
        {"alpha"}, std::make_shared<FakeLLM>(script_of({text_step("alpha")})));
    auto second = runtime.register_adapter(
        {"beta"}, std::make_shared<FakeLLM>(script_of({text_step("beta")})));
    (void)first;
    (void)second;

    PreparedCall call = runtime.prepare_call(config_for(""), CancellationToken{}).get();
    EXPECT_EQ(call.config().provider, "");
    std::string served;
    const LLMResponse response = call.stream(frozen_for(""), [&served](const StreamEvent& event) {
        if (const auto* delta = std::get_if<TextDelta>(&event)) {
            served += delta->text;
        }
        return SinkFlow::Continue;
    }, CancellationToken{}).get();
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);
    EXPECT_EQ(served, "alpha");
}

TEST(LlmRuntime, AdapterHandleRemovalDropsTheRoute) {
    LlmRuntime runtime;
    {
        auto handle = runtime.register_adapter(
            {"fake"}, std::make_shared<FakeLLM>(script_of({text_step("hi")})));
        (void)handle;
        EXPECT_EQ(runtime.list_providers().size(), 1u);
    }
    EXPECT_TRUE(runtime.list_providers().empty());
    EXPECT_THROW((void)runtime.prepare_call(config_for("fake"), CancellationToken{}),
                 NoProviderRouteError);
}

TEST(LlmRuntime, NormalizesNonTerminalAdapterReturn) {
    LlmRuntime runtime;
    auto       handle = runtime.register_adapter({"nonterminal"},
                                                 std::make_shared<NonTerminalProvider>());
    (void)handle;

    const LLMResponse response = runtime.stream(frozen_for("nonterminal"),
                                                [](const StreamEvent&) { return SinkFlow::Continue; },
                                                CancellationToken{}).get();
    EXPECT_EQ(response.outcome, StreamOutcome::Failed);
    EXPECT_EQ(response.error.code, LLMErrorCode::MalformedResponse);
}

TEST(LlmRuntime, NormalizesAdapterThrowToTerminalFailure) {
    LlmRuntime runtime;
    auto handle = runtime.register_adapter({"throwing"}, std::make_shared<ThrowingProvider>());
    (void)handle;

    const LLMResponse response = runtime.stream(frozen_for("throwing"),
                                                [](const StreamEvent&) { return SinkFlow::Continue; },
                                                CancellationToken{}).get();
    EXPECT_EQ(response.outcome, StreamOutcome::Failed);
    EXPECT_EQ(response.error.code, LLMErrorCode::ProviderInternal);
}

TEST(AgentLoopLlmHeader, FreshSessionLogsExactlyOneHeader) {
    AgentEnv env("llm_header_fresh", std::make_unique<FakeLLM>(script_of({text_step("hi")})));
    auto     agent_owner = env.createAgent();
    Agent&   agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hello")), InboxResult::Accepted);

    auto            session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 1u);

    for (const EventRecord& record : events) {
        if (record.event.type != EventType::LlmRequestHeader) {
            continue;
        }
        const auto& header = record.event.payload.get<payload::LlmRequestHeader>();
        EXPECT_TRUE(header.starts_series);
        EXPECT_EQ(header.config.provider, "");
        EXPECT_EQ(header.turn, 1u);
        EXPECT_EQ(header.template_digest.size(), 64u);
        EXPECT_EQ(header.system_prompt_digest.size(), 64u);
        EXPECT_FALSE(header.system_prompt.has_value());
    }
}

TEST(AgentLoopLlmHeader, UnchangedTemplateDoesNotRelog) {
    AgentEnv env("llm_header_unchanged",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("first")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("second")), InboxResult::Accepted);

    auto            session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();
    EXPECT_EQ(count_type(events, EventType::LlmRequestHeader), 1u);
}

TEST(AgentLoopLlmHeader, ProviderComesFromAgentConfig) {
    AgentConfig config;
    config.provider = "fake";

    AgentEnv env("llm_header_provider", std::make_unique<FakeLLM>(script_of({text_step("hi")})),
                 config);
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hello")), InboxResult::Accepted);

    auto            session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::LlmRequestHeader) {
            EXPECT_EQ(record.event.payload.get<payload::LlmRequestHeader>().config.provider,
                      "fake");
        }
    }
}

TEST(AgentLoopLlmHeader, NoRouteNormalizesToProviderFailed) {
    AgentConfig config;
    config.provider = "missing";

    AgentEnv env("llm_header_no_route",
                 std::make_unique<FakeLLM>(script_of({text_step("never")})), config);
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hello")), InboxResult::Accepted);

    auto            session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();
    ASSERT_EQ(count_type(events, EventType::TurnFailed), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            EXPECT_EQ(record.event.payload.get<payload::TurnFailed>().code, "ProviderFailed");
        }
    }
}

TEST(AgentLoopLlmHeader, PersistPromptTextStoresAssembledSystemPrompt) {
    AgentConfig config;
    config.system_prompt       = "SYSTEM-TEXT";
    config.persist_prompt_text = true;

    AgentEnv env("llm_header_prompt", std::make_unique<FakeLLM>(script_of({text_step("hi")})),
                 config);
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hello")), InboxResult::Accepted);

    auto            session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::LlmRequestHeader) {
            continue;
        }
        const auto& header = record.event.payload.get<payload::LlmRequestHeader>();
        ASSERT_TRUE(header.system_prompt.has_value());
        EXPECT_EQ(*header.system_prompt, "SYSTEM-TEXT");
        EXPECT_EQ(header.system_prompt_digest, sha256_hex(*header.system_prompt));
    }
}

TEST(LlmRequestHeaderCodec, RoundTrips) {
    payload::LlmRequestHeader header;
    header.turn       = 3;
    header.step       = 4;
    header.session_id = SessionId{"session"};
    header.purpose    = CallPurpose::Compaction;
    header.config     = config_for("openai-compatible");
    header.config.temperature = 0.2;
    header.config.stop        = {"stop"};
    header.system_prompt_digest = "digest";
    header.system_prompt        = "prompt";
    header.tool_names           = {"read"};
    header.tool_schema_digests  = {"schema-digest"};
    header.template_digest      = "template";
    header.starts_series        = false;

    nlohmann::json json = header;
    const auto     back = json.get<payload::LlmRequestHeader>();

    EXPECT_EQ(back.turn, 3u);
    EXPECT_EQ(back.step, 4u);
    EXPECT_EQ(back.session_id.value, "session");
    ASSERT_TRUE(back.purpose.has_value());
    EXPECT_EQ(*back.purpose, CallPurpose::Compaction);
    EXPECT_EQ(back.config.provider, "openai-compatible");
    EXPECT_EQ(back.config.stop, std::vector<std::string>{"stop"});
    EXPECT_EQ(back.system_prompt_digest, "digest");
    ASSERT_TRUE(back.system_prompt.has_value());
    EXPECT_EQ(*back.system_prompt, "prompt");
    EXPECT_EQ(back.tool_names, std::vector<std::string>{"read"});
    EXPECT_EQ(back.tool_schema_digests, std::vector<std::string>{"schema-digest"});
    EXPECT_EQ(back.template_digest, "template");
    EXPECT_FALSE(back.starts_series);
}

// ---------------------------------------------------------------------------
// SHA-256 known-answer vectors (28 §4.2; review L-8)
// ---------------------------------------------------------------------------

TEST(Sha256, MatchesKnownAnswerVectors) {
    EXPECT_EQ(sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("The quick brown fox jumps over the lazy dog"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

// 31 §12.4/§12.8: `PreparedCall` misuse and an unroutable provider normalize
// to `ProviderFailed` at the loop boundary (`mapAgentError`).
TEST(AgentErrorMap, PreparedCallAndNoRouteNormalizeToProviderFailed) {
    EXPECT_EQ(mapAgentError(LLMErrorCode::InvalidPreparedCall), AgentErrorCode::ProviderFailed);
    EXPECT_EQ(mapAgentError(LLMErrorCode::NoProviderRoute), AgentErrorCode::ProviderFailed);
}

// ---------------------------------------------------------------------------
// Adapter lifetime (28 §12.5, §12.11; review LOW-5)
// ---------------------------------------------------------------------------

struct BlockingState {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    entered = false;
    bool                    release = false;
    std::atomic<bool>       destroyed{false};
};

class BlockingProvider final : public LLMProvider {
public:
    explicit BlockingProvider(std::shared_ptr<BlockingState> state) : state_(std::move(state)) {}
    ~BlockingProvider() override { state_->destroyed.store(true); }

    [[nodiscard]] ProviderId id() const override { return "blocking"; }
    [[nodiscard]] ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken) override {
        {
            std::unique_lock lock(state_->mutex);
            state_->entered = true;
            state_->cv.notify_all();
            state_->cv.wait(lock, [this] { return state_->release; });
        }
        sink(Finished{FinishReason::Stop, std::nullopt, std::nullopt});
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        response.finish  = FinishReason::Stop;
        return Task<LLMResponse>{std::move(response)};
    }

private:
    std::shared_ptr<BlockingState> state_;
};

TEST(LlmRuntime, InFlightDispatchSurvivesAdapterHandleDestruction) {
    auto       state = std::make_shared<BlockingState>();
    LlmRuntime runtime;
    std::optional<AdapterHandle> handle =
        runtime.register_adapter({"blocking"}, std::make_shared<BlockingProvider>(state));

    LLMResponse response;
    {
        PreparedCall call = runtime.prepare_call(config_for("blocking"), CancellationToken{}).get();
        const FrozenRequest frozen = frozen_for("blocking");

        std::thread worker([&] {
            response = call.stream(frozen, continue_sink, CancellationToken{}).get();
        });

        bool entered = false;
        {
            std::unique_lock lock(state->mutex);
            entered = state->cv.wait_for(lock, std::chrono::seconds{2},
                                         [&] { return state->entered; });
        }

        // Destroy the AdapterHandle while the dispatch is still in flight.
        handle.reset();
        EXPECT_TRUE(runtime.list_providers().empty());

        {
            std::lock_guard lock(state->mutex);
            state->release = true;
        }
        state->cv.notify_all();
        worker.join();

        EXPECT_TRUE(entered);
        EXPECT_EQ(response.outcome, StreamOutcome::Completed);
        // The in-flight PreparedCall still owns the adapter.
        EXPECT_FALSE(state->destroyed.load());
    }
    // The PreparedCall is gone: the adapter is finally released.
    EXPECT_TRUE(state->destroyed.load());
}

TEST(LlmRuntime, ConcurrentRegisterRemoveDuringDispatchIsRaceFree) {
    LlmRuntime runtime;
    auto       stable = runtime.register_adapter(
        {"stable"},
        std::make_shared<FakeLLM>(script_of(std::vector<FakeResponseStep>(64, text_step("ok")))));
    (void)stable;

    std::thread dispatcher([&] {
        for (int i = 0; i < 64; ++i) {
            PreparedCall call =
                runtime.prepare_call(config_for("stable"), CancellationToken{}).get();
            (void)call.stream(frozen_for("stable"), continue_sink, CancellationToken{}).get();
        }
    });
    std::thread churn([&] {
        for (int i = 0; i < 64; ++i) {
            auto transient = runtime.register_adapter(
                {"transient"},
                std::make_shared<FakeLLM>(script_of({text_step("x")})));
            (void)transient;
        }
    });

    dispatcher.join();
    churn.join();

    EXPECT_EQ(runtime.list_providers().size(), 1u);
}

// ---------------------------------------------------------------------------
// Non-null registration contract (28 §3.3; review LOW-2/LOW-7)
// ---------------------------------------------------------------------------

TEST(LlmRuntime, RegisterAdapterRejectsNull) {
    LlmRuntime runtime;
    EXPECT_THROW((void)runtime.register_adapter({"x"}, nullptr), std::invalid_argument);
    EXPECT_TRUE(runtime.list_providers().empty());
}

// ---------------------------------------------------------------------------
// AgentServices shape (31 §12.1, A19)
// ---------------------------------------------------------------------------

template <class T, class = void>
struct HasProviderMember : std::false_type {};
template <class T>
struct HasProviderMember<T, std::void_t<decltype(std::declval<T&>().provider)>> : std::true_type {};

template <class T, class = void>
struct HasProvidersMember : std::false_type {};
template <class T>
struct HasProvidersMember<T, std::void_t<decltype(std::declval<T&>().providers)>> : std::true_type {
};

template <class T, class = void>
struct HasProviderConfigMember : std::false_type {};
template <class T>
struct HasProviderConfigMember<T, std::void_t<decltype(std::declval<T&>().provider_config)>>
    : std::true_type {};

template <class T, class = void>
struct HasPoolMember : std::false_type {};
template <class T>
struct HasPoolMember<T, std::void_t<decltype(std::declval<T&>().pool)>> : std::true_type {};

static_assert(std::is_same_v<decltype(AgentServices::runtime), LlmRuntime*>,
              "AgentServices::runtime must be a single nullable LlmRuntime*");
static_assert(!HasProviderMember<AgentServices>::value, "AgentServices must not carry provider");
static_assert(!HasProvidersMember<AgentServices>::value, "AgentServices must not carry providers");
static_assert(!HasProviderConfigMember<AgentServices>::value,
              "AgentServices must not carry provider_config");
static_assert(HasPoolMember<AgentServices>::value, "AgentServices must retain pool");

// ---------------------------------------------------------------------------
// Header / request-series logic (28 §12.6, §12.7; 31 §12.5–§12.8)
// ---------------------------------------------------------------------------

class StubContextAssembler final : public ContextAssembler {
public:
    std::vector<Message>    messages;
    std::vector<ToolSchema> tool_schemas;

    std::vector<Message> assemble(const Session&, const TurnContext&) const override {
        return messages;
    }
    std::vector<ToolSchema> tools() const override { return tool_schemas; }
};

Message system_message(std::string text) {
    Message message;
    message.role = Role::System;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

ToolSchema read_tool_schema() {
    ToolSchema schema;
    schema.name.value   = "read_file";
    schema.description  = "Read a file";
    schema.input_schema = nlohmann::json{{"type", "object"}};
    return schema;
}

SessionOptions direct_options(const AgentEnv& env) {
    SessionOptions options;
    options.cwd           = env.workspace.path();
    options.serverProfile = "interactive";
    options.model         = "fake-model";
    options.title         = "test";
    return options;
}

std::unique_ptr<AgentLoop> make_direct_loop(AgentEnv& env,
                                            const std::shared_ptr<Session>& session,
                                            StubContextAssembler& assembler,
                                            AgentConfig config,
                                            LlmRuntime* runtime) {
    AgentServices services = make_agent_services(
        env.sessions, env.governor, env.tools, env.policy, nullptr, assembler, env.env, env.logger,
        env.sink, env.runtime, env.pool, env.estimator, AgentServices::PermissionResolver{},
        nullptr);
    services.runtime = runtime;
    return std::make_unique<AgentLoop>(AgentId{"direct"}, session, services, std::move(config));
}

std::optional<payload::LlmRequestHeader> last_header(const EventRange& events) {
    std::optional<payload::LlmRequestHeader> found;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::LlmRequestHeader) {
            found = record.event.payload.get<payload::LlmRequestHeader>();
        }
    }
    return found;
}

TEST(AgentLoopHeader, ConfigChangeStartsNewSeries) {
    AgentEnv env("hdr_config",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("SYS")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto first = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(first->send(user_message("one")), InboxResult::Accepted);

    AgentConfig changed;
    changed.provider = "fake";
    auto second = make_direct_loop(env, session, stub, changed, &env.runtime);
    ASSERT_EQ(second->send(user_message("two")), InboxResult::Accepted);

    const EventRange events = session->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 2u);
    const auto header = last_header(events);
    ASSERT_TRUE(header.has_value());
    EXPECT_TRUE(header->starts_series);
    EXPECT_EQ(header->config.provider, "fake");
}

TEST(AgentLoopHeader, ToolChangeStartsNewSeries) {
    AgentEnv env("hdr_tool",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("SYS")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto first = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(first->send(user_message("one")), InboxResult::Accepted);

    stub.tool_schemas = {read_tool_schema()};
    auto second = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(second->send(user_message("two")), InboxResult::Accepted);

    const EventRange events = session->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 2u);
    const auto header = last_header(events);
    ASSERT_TRUE(header.has_value());
    EXPECT_TRUE(header->starts_series);
    EXPECT_EQ(header->tool_names, std::vector<std::string>{"read_file"});
}

TEST(AgentLoopHeader, PromptChangeStartsNewSeries) {
    AgentEnv env("hdr_prompt",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("P1")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto first = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(first->send(user_message("one")), InboxResult::Accepted);

    stub.messages = {system_message("P2")};
    auto second = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(second->send(user_message("two")), InboxResult::Accepted);

    const EventRange events = session->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 2u);
    const auto header = last_header(events);
    ASSERT_TRUE(header.has_value());
    EXPECT_TRUE(header->starts_series);
    EXPECT_EQ(header->system_prompt_digest, sha256_hex("P2"));
}

TEST(AgentLoopHeader, PurposeOnlyChangeDoesNotStartSeries) {
    AgentEnv env("hdr_purpose",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("SYS")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto first = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(first->send(user_message("one")), InboxResult::Accepted);

    // Resume from a header that differs only by `purpose`.
    const auto logged = last_header(session->events());
    ASSERT_TRUE(logged.has_value());
    payload::LlmRequestHeader purpose_header = *logged;
    purpose_header.purpose                   = CallPurpose::Compaction;
    session->append(purpose_header);

    auto second = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(second->send(user_message("two")), InboxResult::Accepted);

    const EventRange events = session->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 3u);
    const auto header = last_header(events);
    ASSERT_TRUE(header.has_value());
    EXPECT_FALSE(header->starts_series);
    EXPECT_FALSE(header->purpose.has_value());
}

TEST(AgentLoopHeader, ResumedSessionDoesNotRelogUnchangedHeader) {
    AgentEnv env("hdr_resume",
                 std::make_unique<FakeLLM>(script_of({text_step("one"), text_step("two")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("SYS")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto first = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(first->send(user_message("one")), InboxResult::Accepted);

    auto second = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(second->send(user_message("two")), InboxResult::Accepted);

    EXPECT_EQ(count_type(session->events(), EventType::LlmRequestHeader), 1u);
}

TEST(AgentLoopHeader, LegacySessionLogsOneHeaderWithoutFabrication) {
    AgentEnv env("hdr_legacy", std::make_unique<FakeLLM>(script_of({text_step("hi")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("SYS")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto loop = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(loop->send(user_message("go")), InboxResult::Accepted);

    const EventRange events = session->events();
    ASSERT_EQ(count_type(events, EventType::LlmRequestHeader), 1u);
    const auto header = last_header(events);
    ASSERT_TRUE(header.has_value());
    EXPECT_TRUE(header->starts_series);
}

TEST(AgentLoopHeader, ReconstructionFromLoggedHeaderMatchesDigest) {
    AgentEnv env("hdr_reconstruct", std::make_unique<FakeLLM>(script_of({text_step("hi")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages     = {system_message("SYS")};
    stub.tool_schemas = {read_tool_schema()};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto loop = make_direct_loop(env, session, stub, AgentConfig{}, &env.runtime);
    ASSERT_EQ(loop->send(user_message("hi")), InboxResult::Accepted);

    const auto header = last_header(session->events());
    ASSERT_TRUE(header.has_value());

    LLMRequest rebuilt_request;
    rebuilt_request.session_id = header->session_id;
    rebuilt_request.purpose    = header->purpose;
    rebuilt_request.messages.push_back(system_message("SYS"));
    rebuilt_request.tools = stub.tool_schemas;

    const FrozenRequest rebuilt = FrozenRequest::freeze(rebuilt_request, header->config);
    EXPECT_EQ(rebuilt.template_digest(), header->template_digest);
    const FrozenRequest again = FrozenRequest::freeze(rebuilt_request, header->config);
    EXPECT_EQ(rebuilt.canonical_json(), again.canonical_json());
}

TEST(AgentLoopHeader, NullRuntimeFailsWithProviderFailed) {
    AgentEnv env("hdr_null_runtime", std::make_unique<FakeLLM>(script_of({text_step("never")})),
                 AgentConfig{});
    StubContextAssembler stub;
    stub.messages = {system_message("SYS")};
    const std::shared_ptr<Session> session =
        env.sessions.sessionPtr(env.sessions.createSession(direct_options(env)));

    auto loop = make_direct_loop(env, session, stub, AgentConfig{}, nullptr);
    ASSERT_EQ(loop->send(user_message("go")), InboxResult::Accepted);

    const EventRange events = session->events();
    ASSERT_EQ(count_type(events, EventType::TurnFailed), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            EXPECT_EQ(record.event.payload.get<payload::TurnFailed>().code, "ProviderFailed");
        }
    }
}

} // namespace

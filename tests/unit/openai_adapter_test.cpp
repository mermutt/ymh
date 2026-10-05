#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "support/mock_http_server.hpp"
#include "support/short_temp.hpp"
#include "ymh/llm/llm_transcript.hpp"
#include "ymh/llm/openai_adapter.hpp"
#include "ymh/llm/redaction.hpp"

namespace {

std::string frame(const nlohmann::json& json) {
    return "data: " + json.dump() + "\n\n";
}

std::string done_frame() {
    return "data: [DONE]\n\n";
}

nlohmann::json text_frame(const std::string& text) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"] = nlohmann::json{{"content", text}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

nlohmann::json reasoning_frame(const std::string& text) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"] = nlohmann::json{{"reasoning_content", text}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

nlohmann::json finish_frame(const std::string& reason) {
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"] = nlohmann::json::object();
    choice["finish_reason"] = reason;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

nlohmann::json usage_frame(std::int64_t input,
                           std::int64_t output,
                           std::int64_t cached,
                           std::int64_t reasoning) {
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array();
    nlohmann::json usage;
    usage["prompt_tokens"] = input;
    usage["completion_tokens"] = output;
    usage["prompt_tokens_details"] = nlohmann::json{{"cached_tokens", cached}};
    usage["completion_tokens_details"] = nlohmann::json{{"reasoning_tokens", reasoning}};
    chunk["usage"] = usage;
    return chunk;
}

nlohmann::json error_frame(const std::string& message, const std::string& code) {
    nlohmann::json error;
    error["message"] = message;
    error["code"] = code;
    nlohmann::json chunk;
    chunk["error"] = error;
    return chunk;
}

nlohmann::json tool_start_frame(std::uint32_t index,
                                const std::string& id,
                                const std::string& name) {
    nlohmann::json function;
    function["name"] = name;
    function["arguments"] = "";
    nlohmann::json tool_call;
    tool_call["index"] = index;
    tool_call["id"] = id;
    tool_call["type"] = "function";
    tool_call["function"] = function;
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"] = nlohmann::json{{"tool_calls", nlohmann::json::array({tool_call})}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

nlohmann::json tool_args_frame(std::uint32_t index, const std::string& fragment) {
    nlohmann::json function;
    function["arguments"] = fragment;
    nlohmann::json tool_call;
    tool_call["index"] = index;
    tool_call["function"] = function;
    nlohmann::json choice;
    choice["index"] = 0;
    choice["delta"] = nlohmann::json{{"tool_calls", nlohmann::json::array({tool_call})}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

std::string sse_body(std::initializer_list<nlohmann::json> frames) {
    std::string body;
    for (const auto& item : frames) {
        body += frame(item);
    }
    body += done_frame();
    return body;
}

class FakeTransport final : public ymh::HttpTransport {
public:
    struct Script {
        ymh::HttpResponse response;
        std::size_t chunk_size = 4096;
        std::chrono::milliseconds delay{0};
    };

    explicit FakeTransport(std::vector<Script> scripts) : scripts_(std::move(scripts)) {}

    // 08-D18: when a fake clock is attached, a scripted delay advances it
    // instead of sleeping, so retry-budget arithmetic is deterministic.
    void attach_clock(std::shared_ptr<std::atomic<std::int64_t>> now_ms) {
        clock_ = std::move(now_ms);
    }

    ymh::Task<ymh::HttpResponse> postStream(const ymh::HttpRequest& request,
                                            ymh::HttpBodySink on_body,
                                            ymh::CancellationToken cancel) override {
        requests.push_back(request);
        const std::size_t index = calls_++;
        if (index >= scripts_.size()) {
            ymh::HttpResponse response;
            response.status = 500;
            response.body = "{}";
            return ymh::Task<ymh::HttpResponse>{std::move(response)};
        }

        Script& script = scripts_[index];
        if (script.delay.count() > 0) {
            if (clock_) {
                clock_->fetch_add(script.delay.count());
            } else {
                std::this_thread::sleep_for(script.delay);
            }
        }
        for (std::size_t offset = 0; offset < script.response.body.size();
             offset += script.chunk_size) {
            if (cancel.cancelled()) {
                break;
            }
            const std::size_t length =
                std::min(script.chunk_size, script.response.body.size() - offset);
            if (!on_body(script.response.body.data() + offset, length)) {
                script.response.transport_error.code = ymh::LLMErrorCode::NetworkError;
                script.response.transport_error.detail = "aborted";
                break;
            }
        }
        return ymh::Task<ymh::HttpResponse>{std::move(script.response)};
    }

    std::vector<ymh::HttpRequest> requests;

private:
    std::vector<Script>                        scripts_;
    std::size_t                                calls_ = 0;
    std::shared_ptr<std::atomic<std::int64_t>> clock_;
};

struct Options {
    ymh::ProviderCapabilities caps = ymh::openai_compatible_capabilities();
    std::string api_key_env = "YMH_TEST_API_KEY";
    std::string base_url = "http://127.0.0.1:9/v1";
    std::uint32_t max_attempts = 3;
    std::size_t sse_line_bytes = 1u << 20;
    std::size_t max_arguments_bytes = 1u << 20;
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::chrono::milliseconds request_timeout{120'000};
    ymh::OpenAICompatibleProvider::SteadyNow now = [] {
        return std::chrono::steady_clock::now();
    };
    std::shared_ptr<ymh::LlmTranscript> transcript = nullptr;
};

ymh::OpenAICompatibleProvider make_provider(std::shared_ptr<ymh::HttpTransport> transport,
                                            Options options = {}) {
    ymh::LLMProviderConfig config;
    config.provider = "openai-compatible";
    config.base_url = std::move(options.base_url);
    config.model = "test-model";
    config.api_key_env = std::move(options.api_key_env);
    config.retry.base_delay = std::chrono::milliseconds{1};
    config.retry.max_delay = std::chrono::milliseconds{2};
    config.retry.jitter = 0.0;
    config.retry.max_attempts = options.max_attempts;
    config.sse_line_bytes = options.sse_line_bytes;
    config.max_arguments_bytes = options.max_arguments_bytes;
    config.connect_timeout = options.connect_timeout;
    config.idle_timeout = options.idle_timeout;
    config.request_timeout = options.request_timeout;
    config.transcript = std::move(options.transcript);
    return ymh::OpenAICompatibleProvider(config, options.caps, std::move(transport),
                                         std::move(options.now));
}

struct Run {
    ymh::LLMResponse              response;
    std::vector<ymh::StreamEvent> events;
};

Run run(ymh::LLMProvider& provider,
        const ymh::LLMRequest& request,
        ymh::CancellationToken cancel = {}) {
    Run result;
    result.response = provider
                          .stream(
                              request,
                              [&result](const ymh::StreamEvent& event) {
                                  result.events.push_back(event);
                                  return ymh::SinkFlow::Continue;
                              },
                              cancel)
                          .get();
    return result;
}

ymh::LLMRequest text_request() {
    ymh::LLMRequest request;
    request.model = "test-model";

    ymh::ContentBlock block;
    block.kind = ymh::ContentBlockKind::Text;
    block.text = "hi";

    ymh::Message message;
    message.role = ymh::Role::User;
    message.content.push_back(std::move(block));

    request.messages.push_back(std::move(message));
    return request;
}

std::string text_of(const std::vector<ymh::StreamEvent>& events) {
    std::string text;
    for (const ymh::StreamEvent& event : events) {
        if (const auto* delta = std::get_if<ymh::TextDelta>(&event)) {
            text += delta->text;
        }
    }
    return text;
}

std::size_t count_usage(const std::vector<ymh::StreamEvent>& events) {
    std::size_t count = 0;
    for (const ymh::StreamEvent& event : events) {
        count += std::holds_alternative<ymh::UsageEvent>(event) ? 1 : 0;
    }
    return count;
}

class OpenAiAdapterTest : public ::testing::Test {
protected:
    void SetUp() override { ::setenv("YMH_TEST_API_KEY", "test-key", 1); }
    void TearDown() override { ::unsetenv("YMH_TEST_API_KEY"); }
};

} // namespace

TEST_F(OpenAiAdapterTest, HappyPathTextStreaming) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body =
        sse_body({text_frame("Hel"), text_frame("lo"), finish_frame("stop")});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(result.response.finish, ymh::FinishReason::Stop);
    EXPECT_EQ(text_of(result.events), "Hello");
    EXPECT_EQ(transport->requests.size(), 1u);

    std::size_t finished = 0;
    for (const ymh::StreamEvent& event : result.events) {
        finished += std::holds_alternative<ymh::Finished>(event) ? 1 : 0;
    }
    EXPECT_EQ(finished, 1u);
}

TEST_F(OpenAiAdapterTest, ReasoningDeltaGatedByCapability) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body =
        sse_body({reasoning_frame("think"), text_frame("answer"), finish_frame("stop")});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    std::size_t reasoning = 0;
    for (const ymh::StreamEvent& event : result.events) {
        reasoning += std::holds_alternative<ymh::ReasoningDelta>(event) ? 1 : 0;
    }
    EXPECT_EQ(reasoning, 1u);
    EXPECT_EQ(text_of(result.events), "answer");

    ymh::ProviderCapabilities caps = ymh::openai_compatible_capabilities();
    caps.reasoning = false;
    auto transport2 = std::make_shared<FakeTransport>(std::vector{script});
    auto provider2 = make_provider(transport2, Options{.caps = caps});
    const auto result2 = run(provider2, text_request());

    reasoning = 0;
    for (const ymh::StreamEvent& event : result2.events) {
        reasoning += std::holds_alternative<ymh::ReasoningDelta>(event) ? 1 : 0;
    }
    EXPECT_EQ(reasoning, 0u);
    EXPECT_EQ(text_of(result2.events), "answer");
}

TEST_F(OpenAiAdapterTest, ToolCallsAssembleAndRoundTripIds) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body =
        sse_body({tool_start_frame(0, "call_1", "read_file"),
                  tool_args_frame(0, "{\"path\":"),
                  tool_args_frame(0, "\"a.cpp\"}"),
                  finish_frame("tool_calls")});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(result.response.finish, ymh::FinishReason::ToolCalls);
    ASSERT_EQ(result.response.tool_calls.size(), 1u);
    EXPECT_EQ(result.response.tool_calls[0].id, "call_1");
    EXPECT_EQ(result.response.tool_calls[0].name, "read_file");
    EXPECT_EQ(result.response.tool_calls[0].arguments, nlohmann::json({{"path", "a.cpp"}}));
}

TEST_F(OpenAiAdapterTest, UsageIsRelayedOnce) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body =
        sse_body({text_frame("x"), finish_frame("stop"), usage_frame(10, 5, 3, 2)});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(count_usage(result.events), 1u);
    ASSERT_TRUE(result.response.usage.has_value());
    EXPECT_EQ(result.response.usage->input_tokens, 10);
    EXPECT_EQ(result.response.usage->output_tokens, 5);
    EXPECT_EQ(result.response.usage->cached_tokens, 3);
    EXPECT_EQ(result.response.usage->reasoning_tokens, 2);
}

TEST_F(OpenAiAdapterTest, UsageOnlyStreamDoesNotDoubleEmit) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body = sse_body({usage_frame(1, 2, 0, 0)});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(count_usage(result.events), 1u);
    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
}

TEST_F(OpenAiAdapterTest, EmbeddedContentFilterMapsToContentFiltered) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body = sse_body(
        {text_frame("partial"), error_frame("content filter triggered", "content_filter")});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport, Options{.max_attempts = 1});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::ContentFiltered);
}

TEST_F(OpenAiAdapterTest, DuplicateUsageIsProviderInternal) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body = sse_body({usage_frame(1, 1, 0, 0), usage_frame(2, 2, 0, 0)});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport, Options{.max_attempts = 1});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::ProviderInternal);
    EXPECT_EQ(count_usage(result.events), 1u);
}

TEST_F(OpenAiAdapterTest, NoRetryAfterFirstDispatchedEvent) {
    FakeTransport::Script first;
    first.response.status = 200;
    first.response.body = frame(text_frame("partial"));
    first.response.transport_error.code = ymh::LLMErrorCode::NetworkError;
    first.response.transport_error.detail = "mid-stream drop";

    FakeTransport::Script second;
    second.response.status = 200;
    second.response.body = sse_body({text_frame("ok"), finish_frame("stop")});

    auto transport = std::make_shared<FakeTransport>(std::vector{first, second});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(transport->requests.size(), 1u);
    ASSERT_FALSE(result.events.empty());
    EXPECT_TRUE(std::holds_alternative<ymh::StreamError>(result.events.back()));
}

TEST_F(OpenAiAdapterTest, RetriesBeforeFirstEventThenSucceeds) {
    FakeTransport::Script first;
    first.response.status = 429;
    first.response.body = "{}";

    FakeTransport::Script second;
    second.response.status = 200;
    second.response.body = sse_body({text_frame("ok"), finish_frame("stop")});

    auto transport = std::make_shared<FakeTransport>(std::vector{first, second});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(transport->requests.size(), 2u);
}

TEST_F(OpenAiAdapterTest, AuthErrorIsNotRetriedAndIsRedacted) {
    FakeTransport::Script script;
    script.response.status = 401;
    script.response.body =
        "{\"error\":{\"message\":\"bad key sk-supersecret\",\"code\":\"invalid_api_key\"}}";

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::Auth);
    EXPECT_EQ(transport->requests.size(), 1u);
    EXPECT_EQ(result.response.error.provider_message.find("sk-supersecret"), std::string::npos);
    EXPECT_NE(result.response.error.provider_message.find("[REDACTED]"), std::string::npos);
}

TEST_F(OpenAiAdapterTest, ContextLengthExceededIsNotRetried) {
    FakeTransport::Script script;
    script.response.status = 400;
    script.response.body =
        "{\"error\":{\"message\":\"maximum context length exceeded\",\"code\":"
        "\"context_length_exceeded\"}}";

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::ContextLengthExceeded);
    EXPECT_EQ(transport->requests.size(), 1u);
}

TEST_F(OpenAiAdapterTest, MalformedSseIsMalformedResponse) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body = "data: {not valid json}\n\n";

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport, Options{.max_attempts = 1});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::MalformedResponse);
}

TEST_F(OpenAiAdapterTest, MissingApiKeyIsAuthBeforeNetwork) {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{});
    auto provider =
        make_provider(transport, Options{.api_key_env = "YMH_DEFINITELY_UNSET_KEY"});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::Auth);
    EXPECT_TRUE(transport->requests.empty());
}

TEST_F(OpenAiAdapterTest, EmptyModelIsConfigError) {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{});
    auto provider = make_provider(transport);

    ymh::LLMRequest request = text_request();
    request.model.clear();
    const auto result = run(provider, request);

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::ConfigError);
    EXPECT_TRUE(transport->requests.empty());
}

TEST_F(OpenAiAdapterTest, ToolsUnsupportedIsRejected) {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{});
    ymh::ProviderCapabilities caps = ymh::openai_compatible_capabilities();
    caps.tool_calls = false;
    auto provider = make_provider(transport, Options{.caps = caps});

    ymh::LLMRequest request = text_request();
    request.tools.push_back(ymh::ToolSchema{{"read_file"}, {}, "desc", nlohmann::json::object(), false});
    const auto result = run(provider, request);

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::UnsupportedModel);
    EXPECT_TRUE(transport->requests.empty());
}

TEST_F(OpenAiAdapterTest, ReasoningUnsupportedIsRejected) {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{});
    ymh::ProviderCapabilities caps = ymh::openai_compatible_capabilities();
    caps.reasoning = false;
    auto provider = make_provider(transport, Options{.caps = caps});

    ymh::LLMRequest request = text_request();
    request.parameters.reasoning_effort = "low";
    const auto result = run(provider, request);

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::BadRequest);
    EXPECT_TRUE(transport->requests.empty());
}

TEST_F(OpenAiAdapterTest, SinkStopReturnsCancelledWithoutError) {
    FakeTransport::Script script;
    script.response.status = 200;
    script.response.body =
        sse_body({text_frame("a"), text_frame("b"), finish_frame("stop")});

    auto transport = std::make_shared<FakeTransport>(std::vector{script});
    auto provider = make_provider(transport);

    std::vector<ymh::StreamEvent> events;
    const ymh::LLMResponse response =
        provider
            .stream(
                text_request(),
                [&events](const ymh::StreamEvent& event) {
                    events.push_back(event);
                    return ymh::SinkFlow::Stop;
                },
                ymh::CancellationToken{})
            .get();

    EXPECT_EQ(response.outcome, ymh::StreamOutcome::Cancelled);
    EXPECT_EQ(response.error.code, ymh::LLMErrorCode::None);
    ASSERT_FALSE(events.empty());
    EXPECT_FALSE(std::holds_alternative<ymh::StreamError>(events.back()));
}

TEST_F(OpenAiAdapterTest, PreCancelledTokenSkipsNetwork) {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{});
    auto provider = make_provider(transport);

    ymh::CancellationSource source;
    source.cancel();
    const auto result = run(provider, text_request(), source.token());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Cancelled);
    EXPECT_TRUE(transport->requests.empty());
}

TEST(OpenAiHelpersTest, MapHttpErrorTable) {
    EXPECT_EQ(ymh::map_http_error(401, "").code, ymh::LLMErrorCode::Auth);
    EXPECT_EQ(ymh::map_http_error(403, "").code, ymh::LLMErrorCode::Auth);
    EXPECT_EQ(ymh::map_http_error(429, "").code, ymh::LLMErrorCode::RateLimited);
    EXPECT_EQ(ymh::map_http_error(400, "").code, ymh::LLMErrorCode::BadRequest);
    EXPECT_EQ(ymh::map_http_error(422, "").code, ymh::LLMErrorCode::BadRequest);
    EXPECT_EQ(ymh::map_http_error(500, "").code, ymh::LLMErrorCode::ServerError);
    EXPECT_EQ(ymh::map_http_error(503, "").code, ymh::LLMErrorCode::ServerError);
    EXPECT_TRUE(ymh::is_retryable_code(ymh::map_http_error(500, "").code));
}

TEST(OpenAiHelpersTest, BackoffIsBoundedAndCapped) {
    ymh::RetryPolicy policy;
    policy.base_delay = std::chrono::milliseconds{1000};
    policy.max_delay = std::chrono::milliseconds{30000};
    policy.jitter = 0.25;

    EXPECT_EQ(ymh::backoff_delay(policy, 1, 0.0).count(), 750);
    EXPECT_EQ(ymh::backoff_delay(policy, 1, 1.0).count(), 1250);
    EXPECT_EQ(ymh::backoff_delay(policy, 2, 0.5).count(), 2000);
    EXPECT_EQ(ymh::backoff_delay(policy, 10, 0.5).count(), 30000);
}

TEST(OpenAiHelpersTest, RetryAfterParsing) {
    ASSERT_TRUE(ymh::parse_retry_after("5").has_value());
    EXPECT_EQ(ymh::parse_retry_after("5")->count(), 5000);

    ASSERT_TRUE(ymh::parse_retry_after("Wed, 21 Oct 2015 07:28:00 GMT").has_value());
    EXPECT_EQ(ymh::parse_retry_after("Wed, 21 Oct 2015 07:28:00 GMT")->count(), 0);

    EXPECT_FALSE(ymh::parse_retry_after("garbage").has_value());
}

TEST(OpenAiHelpersTest, BuildsChatCompletionsBody) {
    ymh::LLMRequest request = text_request();
    request.tools.push_back(ymh::ToolSchema{{"read_file"},
                                            {},
                                            "Read a file",
                                            nlohmann::json{{"type", "object"}},
                                            false});
    request.parameters.temperature = 0.5;
    request.parameters.max_output_tokens = 128;
    request.parameters.reasoning_effort = "low";

    const nlohmann::json body =
        ymh::build_chat_completions_body(request, ymh::openai_compatible_capabilities());

    EXPECT_EQ(body["model"], "test-model");
    EXPECT_EQ(body["stream"], true);
    EXPECT_EQ(body["stream_options"]["include_usage"], true);
    EXPECT_EQ(body["temperature"], 0.5);
    EXPECT_EQ(body["max_tokens"], 128);
    EXPECT_EQ(body["reasoning_effort"], "low");
    EXPECT_EQ(body["tools"][0]["function"]["name"], "read_file");
    EXPECT_EQ(body["messages"][0]["role"], "user");
}

TEST(RedactionTest, RedactsBearerAndHeaderValues) {
    EXPECT_EQ(ymh::redact_secrets("Bearer sk-abc123"), "Bearer [REDACTED]");
    EXPECT_EQ(ymh::redact_header_value("Authorization", "Bearer x"), "[REDACTED]");
    EXPECT_EQ(ymh::redact_header_value("Content-Type", "application/json"),
              "application/json");
    EXPECT_EQ(ymh::redact_secrets("api_key=secretvalue").find("secretvalue"),
              std::string::npos);
}

TEST_F(OpenAiAdapterTest, CurlHappyPathAgainstMockServer) {
    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        sse_body({text_frame("Hi"), finish_frame("stop")}),
        std::numeric_limits<std::size_t>::max()}});

    auto provider = make_provider(std::make_shared<ymh::CurlHttpTransport>(),
                                  Options{.base_url = server.base_url()});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(text_of(result.events), "Hi");
    EXPECT_EQ(server.requests(), 1u);
}

TEST_F(OpenAiAdapterTest, UI46_D12_ApiKeyReachesProvider) {
    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        sse_body({text_frame("Hi"), finish_frame("stop")}),
        std::numeric_limits<std::size_t>::max()}});

    ymh::LLMProviderConfig config;
    config.provider    = "openai-compatible";
    config.base_url    = server.base_url();
    config.model       = "test-model";
    config.api_key     = std::string{"SECRET"};
    config.api_key_env = std::string{};
    config.retry.base_delay    = std::chrono::milliseconds{1};
    config.retry.max_delay     = std::chrono::milliseconds{2};
    config.retry.jitter        = 0.0;
    config.retry.max_attempts  = 1;
    ymh::OpenAICompatibleProvider provider(
        config, ymh::openai_compatible_capabilities(),
        std::make_shared<ymh::CurlHttpTransport>());
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(server.requests(), 1u);
}

TEST_F(OpenAiAdapterTest, CurlRetriesServerErrorThenSucceeds) {
    ymh::test::MockHttpServer server(
        {ymh::test::MockHttpServer::Response{500, {}, "{}",
                                             std::numeric_limits<std::size_t>::max()},
         ymh::test::MockHttpServer::Response{
             200,
             {{"Content-Type", "text/event-stream"}},
             sse_body({text_frame("ok"), finish_frame("stop")}),
             std::numeric_limits<std::size_t>::max()}});

    auto provider = make_provider(std::make_shared<ymh::CurlHttpTransport>(),
                                  Options{.base_url = server.base_url()});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(server.requests(), 2u);
}

TEST_F(OpenAiAdapterTest, CurlMidStreamDisconnectDoesNotRetry) {
    const std::string partial = frame(text_frame("partial"));
    const std::string full = sse_body({text_frame("partial"), finish_frame("stop")});

    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        full,
        partial.size()}});

    auto provider = make_provider(std::make_shared<ymh::CurlHttpTransport>(),
                                  Options{.base_url = server.base_url()});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(server.requests(), 1u);
    EXPECT_EQ(text_of(result.events), "partial");
}

TEST_F(OpenAiAdapterTest, CurlMidStreamSilentGapDoesNotTimeout) {
    const std::string first = frame(text_frame("Hi"));
    const std::string full = sse_body({text_frame("Hi"), finish_frame("stop")});

    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        full,
        std::numeric_limits<std::size_t>::max(),
        first.size(),
        std::chrono::milliseconds{8000}}});

    auto provider = make_provider(
        std::make_shared<ymh::CurlHttpTransport>(),
        Options{.base_url = server.base_url(),
                .max_attempts = 1,
                .idle_timeout = std::chrono::milliseconds{1000},
                .request_timeout = std::chrono::milliseconds{30000}});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(text_of(result.events), "Hi");
    EXPECT_EQ(server.requests(), 1u);
}

TEST_F(OpenAiAdapterTest, CurlMidStreamIdleGuardWhenTotalDeadlineDisabled) {
    const std::string first = frame(text_frame("Hi"));
    const std::string full = sse_body({text_frame("Hi"), finish_frame("stop")});

    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        full,
        std::numeric_limits<std::size_t>::max(),
        first.size(),
        std::chrono::milliseconds{3000}}});

    auto provider = make_provider(
        std::make_shared<ymh::CurlHttpTransport>(),
        Options{.base_url = server.base_url(),
                .max_attempts = 1,
                .idle_timeout = std::chrono::milliseconds{1000},
                .request_timeout = std::chrono::milliseconds{0}});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::Timeout);
}

TEST_F(OpenAiAdapterTest, CurlPreFirstByteStallTimesOut) {
    const std::string full = sse_body({text_frame("Hi"), finish_frame("stop")});

    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        full,
        std::numeric_limits<std::size_t>::max(),
        0,
        std::chrono::milliseconds{3000}}});

    auto provider = make_provider(
        std::make_shared<ymh::CurlHttpTransport>(),
        Options{.base_url = server.base_url(),
                .max_attempts = 1,
                .idle_timeout = std::chrono::milliseconds{500},
                .request_timeout = std::chrono::milliseconds{10000}});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::Timeout);
    EXPECT_EQ(server.requests(), 1u);
}

TEST_F(OpenAiAdapterTest, RetryBudgetIsBoundedByRequestDeadline) {
    auto fake_now = std::make_shared<std::atomic<std::int64_t>>(0);

    FakeTransport::Script script;
    script.response.transport_error =
        ymh::LLMError{ymh::LLMErrorCode::Timeout, 0, "", "transport timeout", true};
    script.delay = std::chrono::milliseconds{300};
    auto transport = std::make_shared<FakeTransport>(
        std::vector<FakeTransport::Script>(5, script));
    transport->attach_clock(fake_now);

    auto provider = make_provider(
        transport,
        Options{.max_attempts = 5,
                .request_timeout = std::chrono::milliseconds{500},
                .now = [fake_now] {
                    return std::chrono::steady_clock::time_point{
                        std::chrono::milliseconds{fake_now->load()}};
                }});

    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::Timeout);
    ASSERT_EQ(transport->requests.size(), 2u);
    EXPECT_LT(transport->requests[1].total_timeout, transport->requests[0].total_timeout);
    EXPECT_EQ(fake_now->load(), 600);
}

TEST_F(OpenAiAdapterTest, CurlOversizedSseLineAborts) {
    const std::string huge = "data: " + std::string(128, 'x') + "\n\n";
    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        huge,
        std::numeric_limits<std::size_t>::max()}});

    auto provider = make_provider(std::make_shared<ymh::CurlHttpTransport>(),
                                  Options{.base_url = server.base_url(),
                                          .max_attempts = 1,
                                          .sse_line_bytes = 32});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::MalformedResponse);
}

TEST_F(OpenAiAdapterTest, CurlToolCallRoundTrip) {
    ymh::test::MockHttpServer server({ymh::test::MockHttpServer::Response{
        200,
        {{"Content-Type", "text/event-stream"}},
        sse_body({tool_start_frame(0, "call_9", "edit_file"),
                  tool_args_frame(0, "{\"path\":\"x\"}"),
                  finish_frame("tool_calls")}),
        std::numeric_limits<std::size_t>::max()}});

    auto provider = make_provider(std::make_shared<ymh::CurlHttpTransport>(),
                                  Options{.base_url = server.base_url()});
    const auto result = run(provider, text_request());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    ASSERT_EQ(result.response.tool_calls.size(), 1u);
    EXPECT_EQ(result.response.tool_calls[0].id, "call_9");
    EXPECT_EQ(result.response.tool_calls[0].arguments, nlohmann::json({{"path", "x"}}));
}

// 74-llm-transcript-errata.md §8 (74-T1, 74-T2, 74-T3, 74-T5).
namespace {

class LlmTranscriptTest : public ::testing::Test {
protected:
    void SetUp() override { ::setenv("YMH_TEST_API_KEY", "test-key", 1); }
    void TearDown() override { ::unsetenv("YMH_TEST_API_KEY"); }
};

ymh::HttpResponse http_ok(std::string body) {
    ymh::HttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    return response;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::vector<nlohmann::json> read_records(const std::filesystem::path& path) {
    std::vector<nlohmann::json> records;
    std::istringstream stream(read_text(path));
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            records.push_back(nlohmann::json::parse(line));
        }
    }
    return records;
}

std::size_t count_transcript_files(const std::filesystem::path& dir, std::uintmax_t& max_size) {
    std::size_t count = 0;
    max_size = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".jsonl") {
            ++count;
            max_size = std::max(max_size, entry.file_size());
        }
    }
    return count;
}

TEST_F(LlmTranscriptTest, EnabledRecordsExactRequestAndFullStreamedResponse) {
    ymh::test::ShortTempRoot root("ymh_tx_roundtrip");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path()});

    const std::string sse = sse_body({text_frame("hel"), text_frame("lo"), finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(
        std::vector<FakeTransport::Script>{{.response = http_ok(sse), .chunk_size = 1}});
    auto provider = make_provider(transport, Options{.transcript = transcript});

    const ymh::LLMRequest        request = text_request();
    const ymh::ProviderCapabilities caps = ymh::openai_compatible_capabilities();
    const auto                   result = run(provider, request);
    ASSERT_EQ(result.response.outcome, ymh::StreamOutcome::Completed)
        << "error code=" << static_cast<int>(result.response.error.code)
        << " http_status=" << result.response.error.http_status
        << " detail=" << result.response.error.detail
        << " message=" << result.response.error.provider_message;

    const auto records = read_records(transcript->path());
    ASSERT_EQ(records.size(), 2u);

    const nlohmann::json expected_body = ymh::build_chat_completions_body(request, caps);
    EXPECT_EQ(records[0]["direction"], "request");
    EXPECT_EQ(records[0]["model"], "test-model");
    EXPECT_EQ(records[0]["endpoint_host"], "127.0.0.1:9");
    EXPECT_EQ(records[0]["body"].get<std::string>(), expected_body.dump());
    EXPECT_EQ(records[0]["body_bytes"].get<std::size_t>(), expected_body.dump().size());

    EXPECT_EQ(records[1]["direction"], "response");
    EXPECT_EQ(records[1]["status"], 200);
    EXPECT_EQ(records[1]["raw"].get<std::string>(), sse);
    EXPECT_EQ(records[1]["assembled"]["content"], "hello");
    EXPECT_EQ(records[1]["assembled"]["finish_reason"], "stop");
    EXPECT_FALSE(records[1]["truncated"].get<bool>());
}

TEST_F(LlmTranscriptTest, DisabledByDefaultWritesNoFile) {
    ymh::test::ShortTempRoot root("ymh_tx_default_off");
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
        {.response = http_ok(sse_body({text_frame("hi"), finish_frame("stop")}))}});
    auto provider = make_provider(transport);
    (void)run(provider, text_request());

    std::uintmax_t max_size = 0;
    EXPECT_EQ(count_transcript_files(root.path(), max_size), 0u);
    EXPECT_FALSE(std::filesystem::exists(root.path() / "llm-transcript.jsonl"));
}

TEST_F(LlmTranscriptTest, NeverContainsApiKeyOrAuthorization) {
    ::setenv("YMH_TX_SECRET_KEY", "sk-sentinel-DO-NOT-LOG-0123456789", 1);
    ymh::test::ShortTempRoot root("ymh_tx_secret");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path()});
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
        {.response = http_ok(sse_body({text_frame("ok"), finish_frame("stop")}))}});
    auto provider = make_provider(
        transport, Options{.api_key_env = "YMH_TX_SECRET_KEY", .transcript = transcript});
    (void)run(provider, text_request());
    ::unsetenv("YMH_TX_SECRET_KEY");

    const std::string text = read_text(transcript->path());
    ASSERT_FALSE(text.empty());
    ASSERT_NE(text.find("\"direction\":\"request\""), std::string::npos);
    EXPECT_EQ(text.find("sk-sentinel-DO-NOT-LOG-0123456789"), std::string::npos);
    EXPECT_EQ(text.find("Authorization"), std::string::npos);
    EXPECT_EQ(text.find("Bearer"), std::string::npos);
    EXPECT_EQ(text.find("YMH_TX_SECRET_KEY"), std::string::npos);
}

TEST_F(LlmTranscriptTest, RotationBoundsRetainedFiles) {
    ymh::test::ShortTempRoot root("ymh_tx_rotation");
    const std::size_t      cap   = 700;
    const std::size_t      files = 2;
    ymh::LlmTranscriptOptions options;
    options.directory         = root.path();
    options.max_bytes_per_file = cap;
    options.max_files          = files;
    auto transcript = std::make_shared<ymh::LlmTranscript>(std::move(options));

    for (int i = 0; i < 8; ++i) {
        auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
            {.response = http_ok(sse_body({text_frame("chunk"), finish_frame("stop")}))}});
        auto provider = make_provider(transport, Options{.transcript = transcript});
        (void)run(provider, text_request());
    }

    std::uintmax_t    max_size = 0;
    const std::size_t retained = count_transcript_files(root.path(), max_size);
    EXPECT_EQ(retained, files);
    EXPECT_GT(max_size, 0u);
    EXPECT_LE(max_size, static_cast<std::uintmax_t>(cap));
}

TEST_F(LlmTranscriptTest, UnwritableWriterDoesNotBreakTheTurn) {
    ymh::test::ShortTempRoot root("ymh_tx_unwritable");
    root.write("blocker", "not a directory");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path() / "blocker" / "sub"});
    ASSERT_FALSE(transcript->healthy());

    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
        {.response = http_ok(sse_body({text_frame("ok"), finish_frame("stop")}))}});
    auto provider = make_provider(transport, Options{.transcript = transcript});
    const auto result = run(provider, text_request());
    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
}

TEST_F(LlmTranscriptTest, InvalidUtf8DoesNotEscapeTheWriter) {
    ymh::test::ShortTempRoot root("ymh_tx_badutf8");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path()});
    std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"";
    body.push_back(static_cast<char>(0xFF));
    body += "\"}}]}\n\n";

    auto transport = std::make_shared<FakeTransport>(
        std::vector<FakeTransport::Script>{{.response = http_ok(body)}});
    auto provider = make_provider(transport, Options{.transcript = transcript});
    (void)run(provider, text_request());
    EXPECT_FALSE(transcript->healthy());
}

TEST_F(LlmTranscriptTest, NonStreamingJsonBodyIsRecordedVerbatim) {
    ymh::test::ShortTempRoot root("ymh_tx_nonstream");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path()});

    const std::string body =
        R"({"id":"x","choices":[{"index":0,"message":{"role":"assistant","content":"hello json"},)"
        R"("finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":2}})";
    auto transport = std::make_shared<FakeTransport>(
        std::vector<FakeTransport::Script>{{.response = http_ok(body)}});
    auto provider = make_provider(transport, Options{.transcript = transcript});
    (void)run(provider, text_request());

    const auto records = read_records(transcript->path());
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["raw"].get<std::string>(), body);
    EXPECT_EQ(records[1]["assembled"]["content"], "hello json");
    EXPECT_EQ(records[1]["assembled"]["finish_reason"], "stop");
}

TEST_F(LlmTranscriptTest, DistinctRequestIdsPerInvocation) {
    ymh::test::ShortTempRoot root("ymh_tx_distinct_ids");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path()});
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
        {.response = http_ok(sse_body({text_frame("a"), finish_frame("stop")}))},
        {.response = http_ok(sse_body({text_frame("b"), finish_frame("stop")}))}});
    auto provider = make_provider(transport, Options{.transcript = transcript});

    (void)run(provider, text_request());
    (void)run(provider, text_request());

    const auto records = read_records(transcript->path());
    ASSERT_EQ(records.size(), 4u);
    const auto first_id  = records[0]["request_id"].get<std::uint64_t>();
    const auto second_id = records[2]["request_id"].get<std::uint64_t>();
    EXPECT_NE(first_id, 0u);
    EXPECT_NE(second_id, 0u);
    EXPECT_NE(first_id, second_id);
    EXPECT_EQ(records[1]["request_id"].get<std::uint64_t>(), first_id);
    EXPECT_EQ(records[3]["request_id"].get<std::uint64_t>(), second_id);
}

TEST_F(LlmTranscriptTest, RetriedRequestSharesOneIdAcrossAttempts) {
    ymh::test::ShortTempRoot root("ymh_tx_retry_id");
    auto transcript = std::make_shared<ymh::LlmTranscript>(
        ymh::LlmTranscriptOptions{.directory = root.path()});

    FakeTransport::Script first;
    first.response.status = 429;
    first.response.body = "{}";
    FakeTransport::Script second;
    second.response.status = 200;
    second.response.body = sse_body({text_frame("ok"), finish_frame("stop")});

    auto transport = std::make_shared<FakeTransport>(std::vector{first, second});
    auto provider = make_provider(transport, Options{.transcript = transcript});
    const auto result = run(provider, text_request());
    ASSERT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    ASSERT_EQ(transport->requests.size(), 2u);

    const auto records = read_records(transcript->path());
    ASSERT_EQ(records.size(), 4u);
    EXPECT_EQ(records[0]["direction"], "request");
    EXPECT_EQ(records[1]["direction"], "response");
    EXPECT_EQ(records[2]["direction"], "request");
    EXPECT_EQ(records[3]["direction"], "response");

    const auto id = records[0]["request_id"].get<std::uint64_t>();
    EXPECT_NE(id, 0u);
    for (const auto& record : records) {
        EXPECT_EQ(record["request_id"].get<std::uint64_t>(), id);
    }
    EXPECT_EQ(records[0]["attempt"], 1);
    EXPECT_EQ(records[1]["attempt"], 1);
    EXPECT_EQ(records[2]["attempt"], 2);
    EXPECT_EQ(records[3]["attempt"], 2);
    EXPECT_EQ(result.response.request_id, id);
}

TEST_F(LlmTranscriptTest, UsageMissingDistinguishesAbsentFromZero) {
    {
        ymh::test::ShortTempRoot root("ymh_tx_usage_absent");
        auto transcript = std::make_shared<ymh::LlmTranscript>(
            ymh::LlmTranscriptOptions{.directory = root.path()});
        auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
            {.response = http_ok(sse_body({text_frame("x"), finish_frame("stop")}))}});
        auto provider = make_provider(transport, Options{.transcript = transcript});
        (void)run(provider, text_request());

        const auto records = read_records(transcript->path());
        ASSERT_EQ(records.size(), 2u);
        EXPECT_TRUE(records[1]["usage_missing"].get<bool>());
        EXPECT_TRUE(records[1]["assembled"]["usage"].is_null());
    }
    {
        ymh::test::ShortTempRoot root("ymh_tx_usage_present");
        auto transcript = std::make_shared<ymh::LlmTranscript>(
            ymh::LlmTranscriptOptions{.directory = root.path()});
        auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
            {.response = http_ok(
                 sse_body({text_frame("x"), finish_frame("stop"), usage_frame(10, 5, 3, 2)}))}});
        auto provider = make_provider(transport, Options{.transcript = transcript});
        (void)run(provider, text_request());

        const auto records = read_records(transcript->path());
        ASSERT_EQ(records.size(), 2u);
        EXPECT_FALSE(records[1]["usage_missing"].get<bool>());
        EXPECT_TRUE(records[1]["assembled"]["usage"].is_object());
        EXPECT_EQ(records[1]["assembled"]["usage"]["input_tokens"], 10);
    }
}

TEST_F(LlmTranscriptTest, OversizedMarkerCarriesNoUsageFields) {
    ymh::test::ShortTempRoot root("ymh_tx_marker");
    const std::size_t      cap = 2000;
    ymh::LlmTranscriptOptions options;
    options.directory          = root.path();
    options.max_bytes_per_file = cap;
    auto transcript = std::make_shared<ymh::LlmTranscript>(std::move(options));

    ymh::LLMRequest request = text_request();
    request.messages.front().content.front().text = std::string(6000, 'x');

    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Script>{
        {.response = http_ok(sse_body({text_frame("ok"), finish_frame("stop")}))}});
    auto provider = make_provider(transport, Options{.transcript = transcript});
    (void)run(provider, request);

    const auto records = read_records(transcript->path());
    ASSERT_EQ(records.size(), 2u);
    ASSERT_TRUE(records[0]["truncated"].get<bool>());
    EXPECT_EQ(records[0]["direction"], "request");
    EXPECT_EQ(records[0].size(), 6u);
    EXPECT_FALSE(records[0].contains("assembled"));
    EXPECT_FALSE(records[0].contains("usage_missing"));
    EXPECT_FALSE(records[0].contains("request_id"));
    EXPECT_FALSE(records[0].contains("attempt"));
    EXPECT_GT(records[0]["original_bytes"].get<std::size_t>(), cap);
    EXPECT_EQ(records[1]["direction"], "response");
    EXPECT_TRUE(records[1].contains("usage_missing"));
}

} // namespace

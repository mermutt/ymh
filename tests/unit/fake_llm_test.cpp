#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ymh/llm/fake_llm.hpp"

namespace {

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

ymh::ContentBlock text_block(std::string text) {
    ymh::ContentBlock block;
    block.kind = ymh::ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

ymh::LLMRequest request_for(const std::string& model = "fake-model") {
    ymh::LLMRequest request;
    request.model = model;
    ymh::Message message;
    message.role = ymh::Role::User;
    message.content.push_back(text_block("hi"));
    request.messages.push_back(std::move(message));
    return request;
}

std::string describe(const std::vector<ymh::StreamEvent>& events) {
    std::string text;
    for (const ymh::StreamEvent& event : events) {
        text += std::to_string(event.index()) + ":";
        if (const auto* delta = std::get_if<ymh::TextDelta>(&event)) {
            text += delta->text;
        } else if (const auto* delta = std::get_if<ymh::ReasoningDelta>(&event)) {
            text += delta->text;
        } else if (const auto* started = std::get_if<ymh::ToolCallStarted>(&event)) {
            text += started->id + "/" + started->name;
        } else if (const auto* delta = std::get_if<ymh::ToolCallDelta>(&event)) {
            text += delta->arguments_fragment;
        } else if (const auto* finished = std::get_if<ymh::ToolCallFinished>(&event)) {
            text += finished->call.id + "/" + finished->call.name;
        } else if (const auto* usage = std::get_if<ymh::UsageEvent>(&event)) {
            text += std::to_string(usage->usage.output_tokens);
        } else if (const auto* finished = std::get_if<ymh::Finished>(&event)) {
            text += std::string{ymh::to_string(finished->reason)};
        } else if (const auto* error = std::get_if<ymh::StreamError>(&event)) {
            text += std::string{ymh::to_string(error->error.code)};
        }
        text += "|";
    }
    return text;
}

bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80) {
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[index + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

} // namespace

TEST(FakeLlmTest, OutputIsDeterministicForSameScriptAndRequest) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "hello world";
    step.reasoning = "thinking";
    script.steps.push_back(step);
    script.chunk_size = 3;

    ymh::FakeLLM first(script);
    ymh::FakeLLM second(script);

    const auto a = run(first, request_for());
    const auto b = run(second, request_for());

    EXPECT_EQ(describe(a.events), describe(b.events));
    EXPECT_EQ(a.response.outcome, ymh::StreamOutcome::Completed);
}

TEST(FakeLlmTest, TextSplitsOnUtf8Boundaries) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "héllo-日本-ok";
    script.steps.push_back(step);
    script.chunk_size = 2;

    ymh::FakeLLM provider(script);
    const auto result = run(provider, request_for());

    std::string reassembled;
    std::size_t deltas = 0;
    for (const ymh::StreamEvent& event : result.events) {
        if (const auto* delta = std::get_if<ymh::TextDelta>(&event)) {
            EXPECT_TRUE(valid_utf8(delta->text));
            reassembled += delta->text;
            ++deltas;
        }
    }
    EXPECT_EQ(reassembled, step.text);
    EXPECT_GT(deltas, 1u);
}

TEST(FakeLlmTest, ScriptedToolCallTranscript) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "ok";
    step.tool_calls.push_back(ymh::FakeToolCallStep{
        "read_file", nlohmann::json{{"path", "foo.cpp"}}, std::nullopt});
    step.finish = ymh::FinishReason::ToolCalls;
    script.steps.push_back(step);
    script.chunk_size = 4;

    ymh::FakeLLM provider(script);
    const auto result = run(provider, request_for());

    ASSERT_EQ(result.response.tool_calls.size(), 1u);
    EXPECT_EQ(result.response.tool_calls[0].name, "read_file");
    EXPECT_EQ(result.response.tool_calls[0].arguments, nlohmann::json({{"path", "foo.cpp"}}));
    EXPECT_EQ(result.response.tool_calls[0].id, "call_0_0");
    EXPECT_EQ(result.response.finish, ymh::FinishReason::ToolCalls);

    std::size_t started = 0;
    std::size_t deltas = 0;
    std::size_t finished = 0;
    for (const ymh::StreamEvent& event : result.events) {
        started += std::holds_alternative<ymh::ToolCallStarted>(event) ? 1 : 0;
        deltas += std::holds_alternative<ymh::ToolCallDelta>(event) ? 1 : 0;
        finished += std::holds_alternative<ymh::ToolCallFinished>(event) ? 1 : 0;
    }
    EXPECT_EQ(started, 1u);
    EXPECT_GE(deltas, 1u);
    EXPECT_EQ(finished, 1u);
}

TEST(FakeLlmTest, ScriptExhaustionReturnsDeterministicStop) {
    ymh::FakeScript script;
    ymh::FakeLLM provider(script);
    const auto result = run(provider, request_for());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_EQ(result.response.finish, ymh::FinishReason::Stop);
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ymh::Finished>(result.events[0]));
}

TEST(FakeLlmTest, ScriptedErrorIsTerminal) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "never streamed";
    ymh::LLMError error;
    error.code = ymh::LLMErrorCode::ServerError;
    step.error = error;
    script.steps.push_back(step);

    ymh::FakeLLM provider(script);
    const auto result = run(provider, request_for());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, ymh::LLMErrorCode::ServerError);
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ymh::StreamError>(result.events[0]));
}

TEST(FakeLlmTest, FailAfterFirstDeltaExercisesRetryBarrier) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "hello";
    ymh::LLMError error;
    error.code = ymh::LLMErrorCode::NetworkError;
    step.error = error;
    script.steps.push_back(step);
    script.fail_after_first_delta = true;

    ymh::FakeLLM provider(script);
    const auto result = run(provider, request_for());

    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Failed);
    ASSERT_EQ(result.events.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<ymh::TextDelta>(result.events[0]));
    EXPECT_TRUE(std::holds_alternative<ymh::StreamError>(result.events[1]));
}

TEST(FakeLlmTest, CancellationStopsBeforeAnyEvent) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "hello";
    script.steps.push_back(step);

    ymh::FakeLLM provider(script);
    ymh::CancellationSource source;
    source.cancel();

    const auto result = run(provider, request_for(), source.token());
    EXPECT_EQ(result.response.outcome, ymh::StreamOutcome::Cancelled);
    EXPECT_TRUE(result.events.empty());
}

TEST(FakeLlmTest, SinkStopReturnsCancelledWithoutError) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "hello";
    script.steps.push_back(step);

    ymh::FakeLLM provider(script);
    ymh::LLMResponse response;
    std::vector<ymh::StreamEvent> events;
    response = provider
                   .stream(
                       request_for(),
                       [&events](const ymh::StreamEvent& event) {
                           events.push_back(event);
                           return ymh::SinkFlow::Stop;
                       },
                       ymh::CancellationToken{})
                   .get();

    EXPECT_EQ(response.outcome, ymh::StreamOutcome::Cancelled);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_FALSE(std::holds_alternative<ymh::StreamError>(events[0]));
}

TEST(FakeLlmTest, UsageIsRelayedOnceAndMirrored) {
    ymh::FakeScript script;
    ymh::FakeResponseStep step;
    step.text = "hi";
    ymh::Usage usage;
    usage.input_tokens = 11;
    usage.output_tokens = 7;
    usage.cached_tokens = 3;
    usage.reasoning_tokens = 2;
    step.usage = usage;
    script.steps.push_back(step);

    ymh::FakeLLM provider(script);
    const auto result = run(provider, request_for());

    std::size_t usage_events = 0;
    for (const ymh::StreamEvent& event : result.events) {
        usage_events += std::holds_alternative<ymh::UsageEvent>(event) ? 1 : 0;
    }
    EXPECT_EQ(usage_events, 1u);
    ASSERT_TRUE(result.response.usage.has_value());
    EXPECT_EQ(result.response.usage->input_tokens, 11);
    EXPECT_EQ(result.response.usage->output_tokens, 7);
    EXPECT_EQ(result.response.usage->cached_tokens, 3);
    EXPECT_EQ(result.response.usage->reasoning_tokens, 2);

    const auto* finished = std::get_if<ymh::Finished>(&result.events.back());
    ASSERT_NE(finished, nullptr);
    ASSERT_TRUE(finished->usage.has_value());
    EXPECT_EQ(finished->usage->output_tokens, 7);
}

TEST(FakeLlmTest, DefaultCapabilitiesAreAdvertised) {
    ymh::FakeScript script;
    ymh::FakeLLM provider(script);
    const ymh::ProviderCapabilities capabilities = provider.capabilities();

    EXPECT_TRUE(capabilities.streaming);
    EXPECT_TRUE(capabilities.tool_calls);
    EXPECT_TRUE(capabilities.reasoning);
    EXPECT_TRUE(capabilities.usage_streaming);
    EXPECT_EQ(provider.id(), "fake");
}

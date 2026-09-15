#include <gtest/gtest.h>

#include <variant>

#include "ymh/llm/stream.hpp"

TEST(LlmTypesTest, ErrorCodeNames) {
    EXPECT_EQ(ymh::to_string(ymh::LLMErrorCode::None), "none");
    EXPECT_EQ(ymh::to_string(ymh::LLMErrorCode::Auth), "auth");
    EXPECT_EQ(ymh::to_string(ymh::LLMErrorCode::ContextLengthExceeded),
              "context_length_exceeded");
    EXPECT_EQ(ymh::to_string(ymh::LLMErrorCode::ProviderInternal), "provider_internal");
}

TEST(LlmTypesTest, RetryableCodesMatchSpec) {
    EXPECT_TRUE(ymh::is_retryable_code(ymh::LLMErrorCode::RateLimited));
    EXPECT_TRUE(ymh::is_retryable_code(ymh::LLMErrorCode::ServerError));
    EXPECT_TRUE(ymh::is_retryable_code(ymh::LLMErrorCode::NetworkError));
    EXPECT_TRUE(ymh::is_retryable_code(ymh::LLMErrorCode::Timeout));
    EXPECT_TRUE(ymh::is_retryable_code(ymh::LLMErrorCode::MalformedResponse));

    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::Auth));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::ConfigError));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::BadRequest));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::ContextLengthExceeded));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::MalformedToolCall));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::ContentFiltered));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::UnsupportedModel));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::ProviderInternal));
    EXPECT_FALSE(ymh::is_retryable_code(ymh::LLMErrorCode::Cancelled));
}

TEST(LlmTypesTest, FinishReasonAndOutcomeNames) {
    EXPECT_EQ(ymh::to_string(ymh::FinishReason::ToolCalls), "tool_calls");
    EXPECT_EQ(ymh::to_string(ymh::FinishReason::ContentFilter), "content_filter");
    EXPECT_EQ(ymh::to_string(ymh::StreamOutcome::Cancelled), "cancelled");
    EXPECT_EQ(ymh::to_string(ymh::StreamOutcome::Failed), "failed");
}

TEST(LlmTypesTest, StreamEventVariantCarriesDeltas) {
    const ymh::StreamEvent event = ymh::TextDelta{"hello"};
    ASSERT_TRUE(std::holds_alternative<ymh::TextDelta>(event));
    EXPECT_EQ(std::get<ymh::TextDelta>(event).text, "hello");
}

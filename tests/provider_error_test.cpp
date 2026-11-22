#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "ymh/llm/openai_adapter.hpp"
#include "ymh/llm/stream.hpp"

namespace {

using ymh::LLMErrorCode;
using ymh::map_http_error;

// The exact shape LiteLLM returns when an Anthropic upstream rejects an
// oversized prompt: HTTP 500 wrapping the provider's context-length error.
constexpr std::string_view kLiteLlmOverflowBody = R"JSON({
  "error": {
    "message": "litellm.InternalServerError: AnthropicException - Requested token count exceeds the model's maximum context length of 360448 tokens. You requested a total of 367939 tokens: 335939 tokens from the input messages and 32000 tokens for the completion. Please reduce the number of tokens in the input messages or the completion to fit within the limit. Handle with 'litellm.InternalServerError'.",
    "type": "internal_server_error",
    "param": null,
    "code": "500"
  }
})JSON";

TEST(ProviderError, Litellm500ContextOverflowIsContextLength) {
    const ymh::LLMError error = map_http_error(500, kLiteLlmOverflowBody);
    EXPECT_EQ(error.code, LLMErrorCode::ContextLengthExceeded);
    EXPECT_EQ(error.http_status, 500);
}

TEST(ProviderError, GenuineServerErrorStaysServerError) {
    const ymh::LLMError error =
        map_http_error(500, R"({"error":{"message":"upstream connect timeout","code":"500"}})");
    EXPECT_EQ(error.code, LLMErrorCode::ServerError);
}

TEST(ProviderError, BadRequestOverflowIsStillContextLength) {
    const ymh::LLMError error = map_http_error(
        400, R"({"error":{"message":"This model's maximum context length is 8192 tokens","code":"invalid_request_error"}})");
    EXPECT_EQ(error.code, LLMErrorCode::ContextLengthExceeded);
}

TEST(ProviderError, UnrelatedBadRequestStaysBadRequest) {
    const ymh::LLMError error =
        map_http_error(422, R"({"error":{"message":"messages[0].role is invalid","code":""}})");
    EXPECT_EQ(error.code, LLMErrorCode::BadRequest);
}

TEST(ProviderError, OverflowInTheCodeFieldIsAlsoDetected) {
    const ymh::LLMError error =
        map_http_error(503, R"({"error":{"message":"please retry","code":"context_length_exceeded"}})");
    EXPECT_EQ(error.code, LLMErrorCode::ContextLengthExceeded);
}

} // namespace

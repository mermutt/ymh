#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <variant>

#include "ymh/llm/openai_adapter.hpp"

namespace {

bool live_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

std::string live_model() {
    const char* model = std::getenv("YMH_LIVE_LLM_MODEL");
    if (model != nullptr && *model != '\0') {
        return model;
    }
    return "deepseek-flash";
}

} // namespace

TEST(LiveLlmTest, DeepSeekStreamsText) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    auto provider = ymh::OpenAICompatibleProvider(ymh::deepseek_config(),
                                                  ymh::openai_compatible_capabilities(),
                                                  std::make_shared<ymh::CurlHttpTransport>());

    ymh::ContentBlock block;
    block.kind = ymh::ContentBlockKind::Text;
    block.text = "Reply with the single word: pong";

    ymh::Message message;
    message.role = ymh::Role::User;
    message.content.push_back(std::move(block));

    ymh::LLMRequest request;
    request.model = live_model();
    request.messages.push_back(std::move(message));
    request.parameters.max_output_tokens = 512;
    request.parameters.reasoning_effort = "low";

    std::string text;
    std::size_t usage_events = 0;
    const ymh::LLMResponse response =
        provider
            .stream(
                request,
                [&text, &usage_events](const ymh::StreamEvent& event) {
                    if (const auto* delta = std::get_if<ymh::TextDelta>(&event)) {
                        text += delta->text;
                    }
                    usage_events += std::holds_alternative<ymh::UsageEvent>(event) ? 1 : 0;
                    return ymh::SinkFlow::Continue;
                },
                ymh::CancellationToken{})
            .get();

    EXPECT_EQ(response.outcome, ymh::StreamOutcome::Completed);
    EXPECT_FALSE(text.empty());
    EXPECT_LE(usage_events, 1u);
}

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/llm/stream.hpp"

namespace {

using namespace ymh;

template <class T>
void expect_round_trip(const StreamEvent& event, const std::string& type) {
    const nlohmann::json json = event;
    EXPECT_EQ(json.at("type"), type);
    EXPECT_EQ(json.get<StreamEvent>(), event);
    EXPECT_TRUE(std::holds_alternative<T>(json.get<StreamEvent>()));
}

TEST(StreamEventCodec, RoundTripsEveryAlternativeWithItsDiscriminator) {
    expect_round_trip<TextDelta>(StreamEvent{TextDelta{""}}, "text_delta");
    expect_round_trip<ReasoningDelta>(StreamEvent{ReasoningDelta{"r"}}, "reasoning_delta");
    expect_round_trip<ToolCallStarted>(
        StreamEvent{ToolCallStarted{2, "call-2", "read_file"}}, "tool_call_started");
    expect_round_trip<ToolCallDelta>(
        StreamEvent{ToolCallDelta{1, "{\"a\":"}}, "tool_call_delta");

    ToolCallAssembled call;
    call.id        = "call-1";
    call.name      = "read_file";
    call.arguments = nlohmann::json{{"path", "a.txt"}};
    expect_round_trip<ToolCallFinished>(StreamEvent{ToolCallFinished{0, call}},
                                        "tool_call_finished");

    expect_round_trip<UsageEvent>(StreamEvent{UsageEvent{Usage{1, 2, 3, 4}}}, "usage_event");
    expect_round_trip<Finished>(
        StreamEvent{Finished{FinishReason::Other, std::nullopt, std::nullopt}}, "finished");

    LLMError error;
    error.code        = LLMErrorCode::RateLimited;
    error.http_status = 429;
    error.detail      = "slow down";
    error.retryable   = true;
    expect_round_trip<StreamError>(StreamEvent{StreamError{error}}, "stream_error");
}

TEST(StreamEventCodec, FinishedPreservesOptionalOmission) {
    const nlohmann::json without =
        nlohmann::json(StreamEvent{Finished{FinishReason::Stop, std::nullopt, std::nullopt}});
    EXPECT_FALSE(without.contains("usage"));
    EXPECT_FALSE(without.contains("replay_state"));

    ReplayEnvelope envelope;
    envelope.provider = "fake";
    envelope.version  = 2;
    envelope.state    = nlohmann::json{{"k", nullptr}};
    const nlohmann::json with =
        nlohmann::json(StreamEvent{Finished{FinishReason::ToolCalls, Usage{5, 6, 0, 0}, envelope}});
    EXPECT_TRUE(with.contains("usage"));
    ASSERT_TRUE(with.contains("replay_state"));
    EXPECT_EQ(with.at("replay_state").at("provider"), "fake");
    EXPECT_EQ(with.at("replay_state").at("version"), 2);

    const StreamEvent decoded = with.get<StreamEvent>();
    const auto& finished      = std::get<Finished>(decoded);
    ASSERT_TRUE(finished.usage.has_value());
    EXPECT_EQ(finished.usage->input_tokens, 5);
    ASSERT_TRUE(finished.replay_state.has_value());
    EXPECT_EQ(finished.replay_state->state, nlohmann::json({{"k", nullptr}}));
}

TEST(StreamEventCodec, ReplayEnvelopeStateNullRoundTrips) {
    ReplayEnvelope envelope;
    envelope.provider = "fake";
    envelope.state    = nlohmann::json(nullptr);

    const nlohmann::json json =
        nlohmann::json(StreamEvent{Finished{FinishReason::Stop, std::nullopt, envelope}});
    ASSERT_TRUE(json.contains("replay_state"));
    EXPECT_TRUE(json.at("replay_state").at("state").is_null());

    const StreamEvent decoded = json.get<StreamEvent>();
    const auto&       finished = std::get<Finished>(decoded);
    ASSERT_TRUE(finished.replay_state.has_value());
    EXPECT_TRUE(finished.replay_state->state.is_null());

    const nlohmann::json defaulted = ReplayEnvelope{};
    EXPECT_TRUE(defaulted.at("state").is_null());
}

TEST(StreamEventCodec, DecodeRejectsUnknownEnumStrings) {
    nlohmann::json bad_reason = nlohmann::json(StreamEvent{
        Finished{FinishReason::Stop, std::nullopt, std::nullopt}});
    bad_reason["reason"] = "future_reason";
    EXPECT_THROW(static_cast<void>(bad_reason.get<StreamEvent>()), nlohmann::json::exception);

    LLMError error;
    error.code = LLMErrorCode::None;
    nlohmann::json bad_code = nlohmann::json(StreamEvent{StreamError{error}});
    bad_code["error"]["code"] = "future_code";
    EXPECT_THROW(static_cast<void>(bad_code.get<StreamEvent>()), nlohmann::json::exception);
}

TEST(StreamEventCodec, ToolCallArgumentsRoundTripAsObject) {
    ToolCallAssembled call;
    call.id        = "c";
    call.name      = "n";
    call.arguments = nlohmann::json::object();
    const nlohmann::json json =
        nlohmann::json(StreamEvent{ToolCallFinished{0, call}});
    EXPECT_TRUE(json.at("call").at("arguments").is_object());
    EXPECT_EQ(json.at("call").at("arguments"), nlohmann::json::object());
}

TEST(StreamEventCodec, EnumParsersAreInverseOfToString) {
    for (const FinishReason reason : {FinishReason::Stop,
                                      FinishReason::Length,
                                      FinishReason::ToolCalls,
                                      FinishReason::ContentFilter,
                                      FinishReason::Error,
                                      FinishReason::Other}) {
        EXPECT_EQ(parse_finish_reason(to_string(reason)), std::optional<FinishReason>{reason});
    }
    for (const LLMErrorCode code : {LLMErrorCode::None,
                                    LLMErrorCode::Auth,
                                    LLMErrorCode::ConfigError,
                                    LLMErrorCode::BadRequest,
                                    LLMErrorCode::ContextLengthExceeded,
                                    LLMErrorCode::RateLimited,
                                    LLMErrorCode::ServerError,
                                    LLMErrorCode::NetworkError,
                                    LLMErrorCode::Timeout,
                                    LLMErrorCode::MalformedResponse,
                                    LLMErrorCode::MalformedToolCall,
                                    LLMErrorCode::ContentFiltered,
                                    LLMErrorCode::UnsupportedModel,
                                    LLMErrorCode::ProviderInternal,
                                    LLMErrorCode::Cancelled,
                                    LLMErrorCode::InvalidPreparedCall,
                                    LLMErrorCode::NoProviderRoute}) {
        EXPECT_EQ(parse_llm_error_code(to_string(code)), std::optional<LLMErrorCode>{code});
    }
    EXPECT_FALSE(parse_finish_reason("bogus").has_value());
    EXPECT_FALSE(parse_llm_error_code("bogus").has_value());
}

TEST(StreamEventCodec, DecodeRejectsUnknownDiscriminator) {
    nlohmann::json json{{"type", "future_event"}};
    EXPECT_THROW(static_cast<void>(json.get<StreamEvent>()), nlohmann::json::exception);
}

TEST(StreamEventCodec, DecodeIgnoresUnknownKeysOnKnownType) {
    nlohmann::json json{{"type", "text_delta"}, {"text", "x"}, {"future", 1}};
    EXPECT_EQ(json.get<StreamEvent>(), StreamEvent{TextDelta{"x"}});
}

} // namespace

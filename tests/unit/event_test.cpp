#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"

namespace ymh {

struct TestPayload {
    int         value = 0;
    std::string name;
};

inline void to_json(nlohmann::json& json, const TestPayload& payload) {
    json = nlohmann::json{{"value", payload.value}, {"name", payload.name}};
}

inline void from_json(const nlohmann::json& json, TestPayload& payload) {
    payload.value = json.at("value").get<int>();
    payload.name  = json.at("name").get<std::string>();
}

template <>
struct EventTraits<TestPayload> {
    static constexpr EventType type = EventType::ContextInjected;
};

} // namespace ymh

namespace {

using namespace std::chrono_literals;
using namespace ymh;

ymh::Event make_event(ymh::EventType type, const std::string& session = "s1") {
    return ymh::Event{
        ymh::EventId{"evt-1"},
        ymh::SessionId{session},
        std::chrono::system_clock::time_point{1234ms},
        type,
        nlohmann::json::object(),
    };
}

TEST(EventTypeTest, WireNameIsInverseOfParseForEveryType) {
    for (const ymh::EventType type : ymh::all_event_types()) {
        const std::string_view wire = ymh::wire_name(type);
        ASSERT_FALSE(wire.empty()) << "missing wire name for enum value "
                                   << static_cast<int>(type);

        const std::optional<ymh::EventType> parsed = ymh::parse_event_type(wire);
        ASSERT_TRUE(parsed.has_value()) << "wire name did not parse: " << wire;
        EXPECT_EQ(*parsed, type);
    }
}

TEST(EventTypeTest, ParseRejectsUnknownAndEmptyNames) {
    EXPECT_FALSE(ymh::parse_event_type("nope/nope").has_value());
    EXPECT_FALSE(ymh::parse_event_type("").has_value());
    EXPECT_FALSE(ymh::parse_event_type("session").has_value());
}

TEST(EventTest, JsonRoundTripPreservesEveryField) {
    ymh::Event original = make_event(ymh::EventType::ToolResult);
    original.payload = {{"outcome", "ok"}, {"output", "hello"}};

    const nlohmann::json json = original;
    const ymh::Event restored = json.get<ymh::Event>();

    EXPECT_EQ(restored.id, original.id);
    EXPECT_EQ(restored.session_id, original.session_id);
    EXPECT_EQ(restored.timestamp, original.timestamp);
    EXPECT_EQ(restored.type, original.type);
    EXPECT_EQ(restored.payload, original.payload);
}

TEST(EventTest, FromJsonRejectsUnknownType) {
    nlohmann::json json = make_event(ymh::EventType::TurnStarted);
    json["type"] = "bogus/type";

    EXPECT_THROW(static_cast<void>(json.get<ymh::Event>()), std::runtime_error);
}

TEST(EventTypeTest, LlmRequestHeaderWireNameAndVocabularySize) {
    EXPECT_EQ(ymh::wire_name(ymh::EventType::LlmRequestHeader), "llm/request_header");
    EXPECT_EQ(ymh::wire_name(ymh::EventType::AssistantAttempt), "assistant/attempt");
    EXPECT_EQ(ymh::wire_name(ymh::EventType::AgentPresetSelected), "agent_preset/selected");
    EXPECT_EQ(ymh::wire_name(ymh::EventType::GoalChange), "goal/change");
    EXPECT_EQ(ymh::wire_name(ymh::EventType::CommandRun), "command/run");
    EXPECT_EQ(ymh::wire_name(ymh::EventType::CommandDone), "command/done");
    EXPECT_EQ(ymh::wire_name(ymh::EventType::JobChanged), "job/changed");
    EXPECT_EQ(ymh::all_event_types().size(), 30u);
}

TEST(EventTest, TryDecodeSkipsUnknownTypeButDecodesKnown) {
    nlohmann::json unknown = make_event(ymh::EventType::TurnStarted);
    unknown["type"]        = "future/unknown_event";
    EXPECT_FALSE(ymh::try_decode_event(unknown).has_value());

    const nlohmann::json known = make_event(ymh::EventType::ToolResult);
    const auto           decoded = ymh::try_decode_event(known);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, ymh::EventType::ToolResult);
}

TEST(TypedEventTest, EncodeDecodeRoundTrip) {
    const ymh::TypedEvent<ymh::TestPayload> typed{
        ymh::EventId{"evt-9"},
        ymh::SessionId{"s9"},
        std::chrono::system_clock::time_point{42ms},
        ymh::TestPayload{7, "seven"},
    };

    const ymh::Event erased = ymh::encode(typed);
    EXPECT_EQ(erased.type, ymh::EventType::ContextInjected);
    EXPECT_EQ(erased.id, typed.id);
    EXPECT_EQ(erased.session_id, typed.session_id);
    EXPECT_EQ(erased.timestamp, typed.timestamp);

    const ymh::TypedEvent<ymh::TestPayload> restored =
        ymh::decode<ymh::TestPayload>(erased);
    EXPECT_EQ(restored.payload.value, 7);
    EXPECT_EQ(restored.payload.name, "seven");
}

TEST(EventRecordTest, EventRecordCarriesSequence) {
    const ymh::EventRecord record{5, make_event(ymh::EventType::SessionStarted)};
    EXPECT_EQ(record.seq, 5);
    EXPECT_EQ(record.event.type, ymh::EventType::SessionStarted);
}

TEST(EventCodecTest, AssistantAttemptRoundTripsItsStream) {
    payload::AssistantAttempt attempt;
    attempt.turn = 2;
    attempt.step = 3;
    TextRun run;
    run.index    = 0;
    run.time0_ms = 5;
    run.dt_ms    = {2};
    run.texts    = {"a", "b"};
    attempt.stream.push_back(run);

    const nlohmann::json json = attempt;
    EXPECT_EQ(json.at("turn"), 2);
    EXPECT_EQ(json.at("step"), 3);
    const payload::AssistantAttempt restored = json.get<payload::AssistantAttempt>();
    EXPECT_EQ(restored.turn, 2u);
    EXPECT_EQ(restored.step, 3u);
    EXPECT_EQ(restored.stream, attempt.stream);
}

TEST(EventCodecTest, AssistantMessageCarriesStreamAndReplayState) {
    payload::AssistantMessage message;
    message.id = "m1";
    ChunkRecord chunk;
    chunk.time_ms = 1;
    chunk.event   = TextDelta{"x"};
    message.stream.push_back(chunk);
    ReplayEnvelope envelope;
    envelope.provider    = "p";
    envelope.state       = nlohmann::json{{"k", 1}};
    message.replay_state = envelope;

    const nlohmann::json json = message;
    ASSERT_TRUE(json.contains("stream"));
    ASSERT_TRUE(json.contains("replay_state"));
    const payload::AssistantMessage restored = json.get<payload::AssistantMessage>();
    EXPECT_EQ(restored.stream, message.stream);
    EXPECT_EQ(restored.replay_state, message.replay_state);
}

TEST(EventCodecTest, AssistantMessageStreamCoversAllRecordAndEventTypes) {
    payload::AssistantMessage message;
    message.id = "m-all";

    TextRun text_run;
    text_run.index    = 0;
    text_run.time0_ms = 1;
    text_run.dt_ms    = {1};
    text_run.texts    = {"a", "b"};

    ReasoningRun reasoning_run;
    reasoning_run.index    = 0;
    reasoning_run.time0_ms = 2;
    reasoning_run.dt_ms    = {2};
    reasoning_run.texts    = {"r1", "r2"};

    ToolCallRun tool_run;
    tool_run.index    = 0;
    tool_run.time0_ms = 3;
    tool_run.dt_ms    = {1};
    tool_run.id       = ToolCallId{"call-0"};
    tool_run.name     = "read_file";
    tool_run.args     = {"{}"};

    message.stream.push_back(text_run);
    message.stream.push_back(reasoning_run);
    message.stream.push_back(tool_run);

    ToolCallAssembled assembled;
    assembled.id        = "call-0";
    assembled.name      = "read_file";
    assembled.arguments = nlohmann::json::object();
    LLMError error;
    error.code = LLMErrorCode::None;

    const std::vector<StreamEvent> events{
        TextDelta{"t"},
        ReasoningDelta{"r"},
        ToolCallStarted{0, ToolCallId{"call-0"}, "read_file"},
        ToolCallDelta{0, "{}"},
        ToolCallFinished{0, assembled},
        UsageEvent{Usage{1, 2, 3, 4}},
        Finished{FinishReason::Stop, Usage{1, 2, 3, 4},
                 ReplayEnvelope{"fake", 1, nlohmann::json(nullptr)}},
        StreamError{error},
    };
    std::int64_t at = 10;
    for (const StreamEvent& event : events) {
        ChunkRecord chunk;
        chunk.time_ms = at++;
        chunk.event   = event;
        message.stream.push_back(chunk);
    }

    const TypedEvent<payload::AssistantMessage> typed{
        EventId{"evt-stream"},
        SessionId{"s1"},
        std::chrono::system_clock::time_point{7ms},
        message,
    };
    const Event erased   = encode(typed);
    const Event restored = nlohmann::json(erased).get<Event>();
    const payload::AssistantMessage decoded =
        decode<payload::AssistantMessage>(restored).payload;
    EXPECT_EQ(decoded.stream, message.stream);
}

TEST(EventCodecTest, AssistantMessageIgnoresUnknownKeys) {
    nlohmann::json json =
        payload::AssistantMessage{"m1", {}, std::nullopt, {}, std::nullopt};
    json["future_key"] = 1;
    EXPECT_NO_THROW(static_cast<void>(json.get<payload::AssistantMessage>()));
}

} // namespace

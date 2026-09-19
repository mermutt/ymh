#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"

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
    EXPECT_EQ(ymh::all_event_types().size(), 23u);
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

} // namespace

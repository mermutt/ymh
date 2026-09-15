#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"

namespace ymh {

struct BusTestPayload {
    int marker = 0;
};

inline void to_json(nlohmann::json& json, const BusTestPayload& payload) {
    json = nlohmann::json{{"marker", payload.marker}};
}

inline void from_json(const nlohmann::json& json, BusTestPayload& payload) {
    payload.marker = json.at("marker").get<int>();
}

template <>
struct EventTraits<BusTestPayload> {
    static constexpr EventType type = EventType::TokenUsage;
};

} // namespace ymh

namespace {

using namespace std::chrono_literals;

ymh::Event make_event(
    const std::string& session,
    int                marker,
    ymh::EventType     type = ymh::EventType::TurnStarted) {
    return ymh::Event{
        ymh::EventId{"evt"},
        ymh::SessionId{session},
        std::chrono::system_clock::time_point{1ms},
        type,
        {{"n", marker}},
    };
}

int marker_of(const ymh::Event& event) {
    return event.payload.at("n").get<int>();
}

TEST(EventBusTest, GlobalSubscriberReceivesEveryEventInPublishOrder) {
    ymh::EventBus bus;
    std::vector<int> seen;

    const ymh::Subscription subscription = bus.subscribe(
        [&seen](const ymh::Event& event) { seen.push_back(marker_of(event)); });
    EXPECT_TRUE(subscription.active());

    bus.publish(make_event("s1", 1));
    bus.publish(make_event("s2", 2));
    bus.publish(make_event("s1", 3));

    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

TEST(EventBusTest, SessionSubscriberReceivesOnlyItsSessionInOrder) {
    ymh::EventBus bus;
    std::vector<int> first;
    std::vector<int> second;

    const ymh::Subscription sub_a = bus.subscribe(
        ymh::SessionId{"s1"},
        [&first](const ymh::Event& event) { first.push_back(marker_of(event)); });
    const ymh::Subscription sub_b = bus.subscribe(
        ymh::SessionId{"s2"},
        [&second](const ymh::Event& event) { second.push_back(marker_of(event)); });

    bus.publish(make_event("s1", 1));
    bus.publish(make_event("s2", 10));
    bus.publish(make_event("s1", 2));
    bus.publish(make_event("s3", 99));

    EXPECT_EQ(first, (std::vector<int>{1, 2}));
    EXPECT_EQ(second, (std::vector<int>{10}));
}

TEST(EventBusTest, GlobalAndSessionSubscribersBothReceive) {
    ymh::EventBus bus;
    int global_count = 0;
    int session_count = 0;

    const ymh::Subscription global = bus.subscribe(
        [&global_count](const ymh::Event&) { ++global_count; });
    const ymh::Subscription session = bus.subscribe(
        ymh::SessionId{"s1"},
        [&session_count](const ymh::Event&) { ++session_count; });

    bus.publish(make_event("s1", 1));
    bus.publish(make_event("s2", 2));

    EXPECT_EQ(global_count, 2);
    EXPECT_EQ(session_count, 1);
}

TEST(EventBusTest, GlobalSubscriptionUnsubscribesOnDestruction) {
    ymh::EventBus bus;
    int count = 0;

    {
        const ymh::Subscription subscription = bus.subscribe(
            [&count](const ymh::Event&) { ++count; });
        bus.publish(make_event("s1", 1));
        EXPECT_EQ(count, 1);
    }

    bus.publish(make_event("s1", 2));
    EXPECT_EQ(count, 1);
}

TEST(EventBusTest, SessionSubscriptionUnsubscribesOnDestruction) {
    ymh::EventBus bus;
    int count = 0;

    {
        const ymh::Subscription subscription = bus.subscribe(
            ymh::SessionId{"s1"},
            [&count](const ymh::Event&) { ++count; });
        bus.publish(make_event("s1", 1));
        EXPECT_EQ(count, 1);
    }

    bus.publish(make_event("s1", 2));
    EXPECT_EQ(count, 1);
}

TEST(EventBusTest, MovingSubscriptionTransfersOwnership) {
    ymh::EventBus bus;
    int count = 0;

    ymh::Subscription outer;
    {
        ymh::Subscription inner = bus.subscribe([&count](const ymh::Event&) { ++count; });
        outer = std::move(inner);
    }

    bus.publish(make_event("s1", 1));
    EXPECT_EQ(count, 1);

    outer.unsubscribe();
    bus.publish(make_event("s1", 2));
    EXPECT_EQ(count, 1);
}

TEST(EventBusTest, UnsubscribeIsIdempotent) {
    ymh::EventBus bus;
    ymh::Subscription subscription = bus.subscribe([](const ymh::Event&) {});

    subscription.unsubscribe();
    subscription.unsubscribe();

    EXPECT_FALSE(subscription.active());
}

TEST(EventBusTest, TypedPublishReachesTypedSessionSubscriber) {
    ymh::EventBus bus;
    int received = 0;

    const ymh::Subscription subscription = bus.subscribe<ymh::BusTestPayload>(
        ymh::SessionId{"s1"},
        [&received](const ymh::BusTestPayload& payload) { received = payload.marker; });

    bus.publish(ymh::TypedEvent<ymh::BusTestPayload>{
        ymh::EventId{"evt"},
        ymh::SessionId{"s1"},
        std::chrono::system_clock::time_point{1ms},
        ymh::BusTestPayload{77},
    });

    EXPECT_EQ(received, 77);
}

TEST(SessionMailboxTest, PushDrainPreservesOrderAndReportsEmpty) {
    ymh::SessionMailbox mailbox;
    EXPECT_TRUE(mailbox.empty());

    mailbox.push(make_event("s1", 1));
    mailbox.push(make_event("s1", 2));
    EXPECT_FALSE(mailbox.empty());
    EXPECT_EQ(mailbox.size(), 2U);

    std::vector<int> drained;
    const bool empty = mailbox.drain([&drained](ymh::Event&& event) {
        drained.push_back(marker_of(event));
        return true;
    });

    EXPECT_TRUE(empty);
    EXPECT_TRUE(mailbox.empty());
    EXPECT_EQ(drained, (std::vector<int>{1, 2}));
}

TEST(SessionMailboxTest, DrainStopsWhenSinkReturnsFalse) {
    ymh::SessionMailbox mailbox;
    mailbox.push(make_event("s1", 1));
    mailbox.push(make_event("s1", 2));

    std::vector<int> drained;
    const bool empty = mailbox.drain([&drained](ymh::Event&& event) {
        drained.push_back(marker_of(event));
        return false;
    });

    EXPECT_FALSE(empty);
    EXPECT_EQ(drained, (std::vector<int>{1}));
    EXPECT_EQ(mailbox.size(), 1U);
}

TEST(EventBusTest, MailboxIsExposedForSessionWithSubscribers) {
    ymh::EventBus bus;
    const ymh::SessionId session{"s1"};

    EXPECT_EQ(bus.mailbox(session), nullptr);

    const ymh::Subscription subscription =
        bus.subscribe(session, [](const ymh::Event&) {});
    ASSERT_NE(bus.mailbox(session), nullptr);
    EXPECT_TRUE(bus.mailbox(session)->empty());
}

} // namespace

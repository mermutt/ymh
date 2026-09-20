#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

std::chrono::system_clock::time_point fixed_clock() {
    return std::chrono::system_clock::time_point{std::chrono::seconds{1000}};
}

struct SessionFixture {
    EventBus                 bus;
    MemorySessionStore       store;
    SessionHeader            header;
    std::unique_ptr<Session> session;

    explicit SessionFixture(std::string model = "fake-model") {
        header.id            = make_session_id();
        header.cwd           = "/tmp";
        header.model         = std::move(model);
        header.serverProfile = "interactive";
        header.kind          = SessionKind::Root;
        store.create(header);
        session = std::make_unique<Session>(header, store, bus);
    }

    void append_turn(TurnId turn, const std::string& user, const std::string& assistant) {
        session->append(payload::TurnStarted{turn, payload::TurnOrigin::User});
        payload::UserMessage um;
        um.id = make_event_id().value;
        um.content.push_back(text_block(user));
        session->append(um);
        payload::AssistantMessage am;
        am.id = make_event_id().value;
        am.content.push_back(text_block(assistant));
        session->append(am);
        session->append(payload::TurnEnded{turn});
    }
};

CompactionPolicy base_policy() {
    CompactionPolicy policy;
    policy.enabled             = true;
    policy.threshold_tokens    = 100;
    policy.keep_recent_turns   = 1;
    policy.min_prefix_messages = 4;
    return policy;
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

TEST(CompactionTriggerTest, PressureFiresOnlyAboveEffectiveThreshold) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    ProviderRuntime       provider(FakeScript{{text_step("S")}});
    LLMPool               pool(1);
    DefaultTokenEstimator estimator;

    CompactionPolicy high = base_policy();
    high.threshold_tokens = 1'000'000;
    ContextCompactor below(provider.runtime(), pool, estimator, high, fixed_clock);
    const std::vector<Message> messages = fixture.session->deriveMessages();
    const auto                 skipped =
        below
            .compact_if_needed(CompactionTrigger::Pressure, *fixture.session, messages,
                               CancellationToken{})
            .get();
    EXPECT_FALSE(skipped.has_value());
    EXPECT_EQ(count_type(fixture.session->events(), EventType::ContextCompaction), 0u);

    CompactionPolicy low = base_policy();
    low.threshold_tokens = 1;
    ContextCompactor above(provider.runtime(), pool, estimator, low, fixed_clock);
    const auto        fired =
        above
            .compact_if_needed(CompactionTrigger::Pressure, *fixture.session, messages,
                               CancellationToken{})
            .get();
    ASSERT_TRUE(fired.has_value());
    EXPECT_EQ(fired->outcome, CompactionOutcome::Compacted);
}

TEST(CompactionTriggerTest, ContextOverflowFiresBelowThreshold) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.threshold_tokens = 1'000'000;

    ProviderRuntime       provider(FakeScript{{text_step("S")}});
    LLMPool               pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor      compactor(provider.runtime(), pool, estimator, policy, fixed_clock);

    const auto result =
        compactor
            .compact_if_needed(CompactionTrigger::ContextOverflow, *fixture.session,
                               fixture.session->deriveMessages(), CancellationToken{})
            .get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, CompactionOutcome::Compacted);
}

TEST(CompactionTriggerTest, CompactNowAlwaysAttempts) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.threshold_tokens = 1'000'000;

    ProviderRuntime       provider(FakeScript{{text_step("S")}});
    LLMPool               pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor      compactor(provider.runtime(), pool, estimator, policy, fixed_clock);

    const CompactionResult result =
        compactor
            .compact_now(*fixture.session, fixture.session->deriveMessages(), CancellationToken{})
            .get();
    EXPECT_EQ(result.outcome, CompactionOutcome::Compacted);
}

TEST(CompactionTriggerTest, DisabledPolicyReturnsNoCompactionForBothTriggers) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.enabled          = false;

    ProviderRuntime       provider(FakeScript{{text_step("S")}});
    LLMPool               pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor      compactor(provider.runtime(), pool, estimator, policy, fixed_clock);

    const std::vector<Message> messages = fixture.session->deriveMessages();
    EXPECT_FALSE(compactor
                     .compact_if_needed(CompactionTrigger::Pressure, *fixture.session, messages,
                                        CancellationToken{})
                     .get()
                     .has_value());
    EXPECT_FALSE(compactor
                     .compact_if_needed(CompactionTrigger::ContextOverflow, *fixture.session,
                                        messages, CancellationToken{})
                     .get()
                     .has_value());
}

TEST(CompactionTriggerTest, PayloadCarriesProviderAndShadowedAccounting) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.threshold_tokens = 1;
    policy.provider         = "fake";

    ProviderRuntime       provider(FakeScript{{text_step("S")}});
    LLMPool               pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor      compactor(provider.runtime(), pool, estimator, policy, fixed_clock);

    const auto result =
        compactor
            .compact_if_needed(CompactionTrigger::Pressure, *fixture.session,
                               fixture.session->deriveMessages(), CancellationToken{})
            .get();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->compaction.has_value());
    const payload::ContextCompaction& payload = *result->compaction;

    EXPECT_EQ(payload.provider, "fake");
    EXPECT_EQ(payload.shadowedEnd, payload.boundary);

    std::vector<Sequence> expected;
    for (const EventRecord& record : fixture.session->events()) {
        if (record.seq <= payload.boundary) {
            expected.push_back(record.seq);
        }
    }
    EXPECT_EQ(payload.shadowedSeqs, expected);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(payload.shadowedStart, expected.front());
    EXPECT_GT(payload.shadowedTokenCount, 0u);
}

} // namespace

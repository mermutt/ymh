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

FakeResponseStep error_step(LLMErrorCode code) {
    FakeResponseStep step;
    LLMError         error;
    error.code = code;
    step.error = error;
    return step;
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

std::optional<payload::TurnStarted> last_turn_started(const EventRange& events) {
    std::optional<payload::TurnStarted> found;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnStarted) {
            found = record.event.payload.get<payload::TurnStarted>();
        }
    }
    return found;
}

struct SessionFixture {
    EventBus               bus;
    MemorySessionStore     store;
    SessionHeader          header;
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

std::chrono::system_clock::time_point fixed_clock() {
    return std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
}

// ---------------------------------------------------------------------------
// CompactionPolicy (13 §3.2)
// ---------------------------------------------------------------------------

TEST(CompactionPolicyTest, AbsoluteThresholdWins) {
    CompactionPolicy policy;
    policy.threshold_tokens      = 1234;
    policy.context_window_tokens = 10'000;
    EXPECT_EQ(policy.effective_threshold_tokens(), 1234u);
    policy.enabled = true;
    EXPECT_TRUE(policy.is_enabled());
}

TEST(CompactionPolicyTest, RatioThresholdReservesOutput) {
    CompactionPolicy policy;
    policy.context_window_tokens = 10'000;
    policy.reserve_output_tokens = 1'000;
    policy.threshold_ratio       = 0.80;
    EXPECT_EQ(policy.effective_threshold_tokens(), 7'200u);
    policy.enabled = true;
    EXPECT_TRUE(policy.is_enabled());
}

TEST(CompactionPolicyTest, UnknownWindowIsDisabled) {
    CompactionPolicy policy;
    policy.enabled = true;
    EXPECT_EQ(policy.effective_threshold_tokens(), 0u);
    EXPECT_FALSE(policy.is_enabled());
}

TEST(CompactionPolicyTest, WindowAtReserveBoundaryIsDisabled) {
    CompactionPolicy policy;
    policy.context_window_tokens = 4'096;
    policy.reserve_output_tokens = 4'096;
    EXPECT_EQ(policy.effective_threshold_tokens(), 0u);
}

// ---------------------------------------------------------------------------
// Boundary selection (13 §3.3)
// ---------------------------------------------------------------------------

TEST(CompactionPlanTest, SelectsTurnTerminalBoundary) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const CompactionPlan plan = compactor.plan(*fixture.session, fixture.session->deriveMessages());
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.prefix_messages, 4u);
    EXPECT_EQ(plan.kept_messages, 2u);

    const EventRange events = fixture.session->events();
    const auto       compactions = count_type(events, EventType::ContextCompaction);
    EXPECT_EQ(compactions, 0u);
}

TEST(CompactionPlanTest, KeepRecentTurnsZeroSummarizesEverything) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.keep_recent_turns = 0;

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionPlan plan = compactor.plan(*fixture.session, fixture.session->deriveMessages());
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.prefix_messages, 6u);
    EXPECT_EQ(plan.kept_messages, 0u);
}

TEST(CompactionPlanTest, NotNeededBelowMinPrefix) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.keep_recent_turns = 2;
    policy.min_prefix_messages = 4;

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionPlan plan = compactor.plan(*fixture.session, fixture.session->deriveMessages());
    EXPECT_FALSE(plan.valid);
}

TEST(CompactionPlanTest, NotNeededWhenBoundaryDoesNotAdvance) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");
    payload::ContextCompaction prior;
    prior.boundary = 1'000;
    prior.summary  = "prior";
    fixture.session->append(prior);

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const CompactionPlan plan = compactor.plan(*fixture.session, fixture.session->deriveMessages());
    EXPECT_FALSE(plan.valid);
}

TEST(CompactionPlanTest, IgnoresOpenTrailingTurn) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");
    fixture.session->append(payload::TurnStarted{4, payload::TurnOrigin::User});
    payload::UserMessage open;
    open.id = make_event_id().value;
    open.content.push_back(text_block("open"));
    fixture.session->append(open);

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const CompactionPlan plan = compactor.plan(*fixture.session, fixture.session->deriveMessages());
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.prefix_messages, 4u);
    EXPECT_EQ(plan.kept_messages, 3u);
}

// ---------------------------------------------------------------------------
// compact() (13 §5.5)
// ---------------------------------------------------------------------------

TEST(ContextCompactorTest, DisabledPolicyIsNotNeeded) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.enabled = false;

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    EXPECT_EQ(result.outcome, CompactionOutcome::NotNeeded);
    EXPECT_EQ(result.error.code, CompactionError::Code::Disabled);
    EXPECT_FALSE(result.compaction.has_value());
}

TEST(ContextCompactorTest, ProducesPayloadWithEstimateAndClock) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    FakeResponseStep summary_step = text_step("SUMMARY");
    summary_step.usage            = Usage{11, 5, 0, 0};

    FakeLLM            provider(FakeScript{{summary_step}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const std::vector<Message> messages = fixture.session->deriveMessages();
    const CompactionResult     result =
        compactor.compact(*fixture.session, messages, CancellationToken{});

    ASSERT_EQ(result.outcome, CompactionOutcome::Compacted);
    ASSERT_TRUE(result.compaction.has_value());
    EXPECT_EQ(result.compaction->boundary, 8);
    EXPECT_EQ(result.compaction->summary, "SUMMARY");
    EXPECT_EQ(result.compaction->model, "fake-model");
    EXPECT_EQ(result.compaction->createdAt, fixed_clock());
    ASSERT_TRUE(result.usage.has_value());
    EXPECT_EQ(result.usage->input_tokens, 11);

    std::vector<Message> expected;
    expected.push_back(Message{Role::System, {text_block("SUMMARY")}, {}});
    expected.push_back(messages[4]);
    expected.push_back(messages[5]);
    EXPECT_EQ(result.compaction->tokenEstimate, estimator.estimate(expected));

    EXPECT_EQ(count_type(fixture.session->events(), EventType::ContextCompaction), 0u);
}

TEST(ContextCompactorTest, TruncatesSummaryAtTokenBound) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.max_summary_tokens = 5;

    FakeLLM            provider(FakeScript{{text_step("abcdefghij")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    ASSERT_EQ(result.outcome, CompactionOutcome::Compacted);
    ASSERT_TRUE(result.compaction.has_value());
    EXPECT_EQ(result.compaction->summary.size(), 4u);
    EXPECT_EQ(result.compaction->summary, "abcd");
}

TEST(ContextCompactorTest, OversizedSummaryBytesFails) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.max_summary_tokens = 1'024;
    policy.max_summary_bytes  = 4;

    FakeLLM            provider(FakeScript{{text_step("long summary")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    EXPECT_EQ(result.outcome, CompactionOutcome::Failed);
    EXPECT_EQ(result.error.code, CompactionError::Code::OversizedSummary);
}

TEST(ContextCompactorTest, ReducesBoundaryOnceOnSummarizerOverflow) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");
    fixture.append_turn(4, "u4", "a4");

    CompactionPolicy policy = base_policy();
    policy.keep_recent_turns = 1;

    FakeLLM provider(FakeScript{{error_step(LLMErrorCode::ContextLengthExceeded),
                                 text_step("REDUCED")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    ASSERT_EQ(result.outcome, CompactionOutcome::Compacted);
    ASSERT_TRUE(result.compaction.has_value());
    EXPECT_EQ(result.compaction->summary, "REDUCED");
    const EventRange events = fixture.session->events();
    // keep=2 selects the second turn's terminal, which is earlier than keep=1.
    EXPECT_LT(result.compaction->boundary, events.back().seq);
}

TEST(ContextCompactorTest, SummarizerOverflowWithoutReductionFails) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    FakeLLM provider(FakeScript{{error_step(LLMErrorCode::ContextLengthExceeded),
                                 error_step(LLMErrorCode::ContextLengthExceeded)}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    EXPECT_EQ(result.outcome, CompactionOutcome::Failed);
    EXPECT_EQ(result.error.code, CompactionError::Code::SummarizerOverflow);
}

TEST(ContextCompactorTest, SummarizerTerminalFailureFails) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    FakeLLM provider(FakeScript{{error_step(LLMErrorCode::ServerError)}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    EXPECT_EQ(result.outcome, CompactionOutcome::Failed);
    EXPECT_EQ(result.error.code, CompactionError::Code::SummarizerFailed);
}

TEST(ContextCompactorTest, CancelledTokenReturnsCancelled) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    CancellationSource source;
    source.cancel();
    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), source.token());
    EXPECT_EQ(result.outcome, CompactionOutcome::Cancelled);
    EXPECT_EQ(result.error.code, CompactionError::Code::Cancelled);
}

TEST(ContextCompactorTest, SummarizerModelOverrideIsRecorded) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    CompactionPolicy policy = base_policy();
    policy.summarizer_model = "cheap-model";

    FakeLLM            provider(FakeScript{{text_step("S")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, policy, fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    ASSERT_EQ(result.outcome, CompactionOutcome::Compacted);
    ASSERT_TRUE(result.compaction.has_value());
    EXPECT_EQ(result.compaction->model, "cheap-model");
}

// ---------------------------------------------------------------------------
// Replay faithfulness (13 §10.3)
// ---------------------------------------------------------------------------

TEST(CompactionReplayTest, FoldIsDeterministicAndNeverReSummarizes) {
    SessionFixture fixture;
    fixture.append_turn(1, "u1", "a1");
    fixture.append_turn(2, "u2", "a2");
    fixture.append_turn(3, "u3", "a3");

    FakeLLM            provider(FakeScript{{text_step("DURABLE-SUMMARY")}});
    LLMPool            pool(1);
    DefaultTokenEstimator estimator;
    ContextCompactor   compactor(provider, pool, estimator, base_policy(), fixed_clock);

    const CompactionResult result =
        compactor.compact(*fixture.session, fixture.session->deriveMessages(), CancellationToken{});
    ASSERT_EQ(result.outcome, CompactionOutcome::Compacted);
    ASSERT_TRUE(result.compaction.has_value());
    fixture.session->append(*result.compaction);

    const std::vector<Message> live = fixture.session->deriveMessages();
    ASSERT_FALSE(live.empty());
    EXPECT_EQ(live.front().role, Role::System);
    EXPECT_EQ(live.front().content.at(0).text, "DURABLE-SUMMARY");

    const std::vector<Message> replay = deriveMessages(fixture.header, fixture.session->events());
    ASSERT_EQ(replay.size(), live.size());
    for (std::size_t index = 0; index < live.size(); ++index) {
        EXPECT_EQ(replay[index].role, live[index].role);
        EXPECT_EQ(replay[index].content.size(), live[index].content.size());
    }
}

// ---------------------------------------------------------------------------
// Agent loop integration (13 §6.1, §6.7)
// ---------------------------------------------------------------------------

AgentLoop& as_loop(Agent& agent) {
    return static_cast<AgentLoop&>(agent);
}

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    message.content.push_back(text_block(std::move(text)));
    return message;
}

TEST(CompactionLoopTest, ThresholdPathCompactsAndAttributesUsage) {
    CompactionPolicy policy;
    policy.enabled             = true;
    policy.threshold_tokens    = 1;
    policy.keep_recent_turns   = 1;
    policy.min_prefix_messages = 2;

    FakeResponseStep summary_step = text_step("SUMMARY");
    summary_step.usage            = Usage{11, 5, 0, 0};

    FakeScript script;
    script.steps = {text_step("r1"), text_step("r2"), summary_step, text_step("r3")};

    AgentEnv env("compaction_threshold", std::make_unique<FakeLLM>(script), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, nullptr, false, policy);

    Agent& agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("one")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("two")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("three")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const EventRange events  = session.events();
    EXPECT_EQ(count_type(events, EventType::ContextCompaction), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 3u);

    bool found_aux_usage = false;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::TokenUsage) {
            continue;
        }
        const auto& usage = record.event.payload.get<payload::TokenUsage>();
        if (usage.usage.input_tokens == 11) {
            found_aux_usage = true;
            ASSERT_TRUE(usage.turn.has_value());
            EXPECT_EQ(*usage.turn, 3u);
        }
    }
    EXPECT_TRUE(found_aux_usage);

    const std::vector<Message> projected = session.deriveMessages();
    ASSERT_FALSE(projected.empty());
    EXPECT_EQ(projected.front().role, Role::System);
    EXPECT_EQ(projected.front().content.at(0).text, "SUMMARY");
}

TEST(CompactionLoopTest, ManualCompactionRunsMaintenanceTurn) {
    CompactionPolicy policy;
    policy.enabled             = true;
    policy.threshold_tokens    = 1'000'000;
    policy.keep_recent_turns   = 1;
    policy.min_prefix_messages = 2;

    FakeResponseStep summary_step = text_step("MANUAL-SUMMARY");
    summary_step.usage            = Usage{7, 3, 0, 0};

    FakeScript script;
    script.steps = {text_step("r1"), text_step("r2"), text_step("r3"), summary_step};

    AgentEnv env("compaction_manual", std::make_unique<FakeLLM>(script), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, nullptr, false, policy);

    Agent& agent = env.createAgent();
    ASSERT_EQ(agent.send(user_message("one")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("two")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("three")), InboxResult::Accepted);

    Session&         session = env.sessionOf(agent);
    const SessionId  id      = session.id();
    const EventRange before  = session.events();
    EXPECT_EQ(count_type(before, EventType::ContextCompaction), 0u);

    const std::expected<CompactionOutcome, AgentError> queued =
        env.registry.requestCompaction(id);
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(*queued, CompactionOutcome::Queued);
    EXPECT_FALSE(agent.hasPendingWork());

    const EventRange events = session.events();
    EXPECT_EQ(count_type(events, EventType::ContextCompaction), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 3u);

    std::optional<payload::TurnStarted> maintenance;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnStarted) {
            const auto started = record.event.payload.get<payload::TurnStarted>();
            if (started.origin == payload::TurnOrigin::Maintenance) {
                maintenance = started;
            }
        }
    }
    ASSERT_TRUE(maintenance.has_value());

    bool found_aux_usage = false;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TokenUsage) {
            const auto& usage = record.event.payload.get<payload::TokenUsage>();
            if (usage.usage.input_tokens == 7 && usage.turn == maintenance->turn) {
                found_aux_usage = true;
            }
        }
    }
    EXPECT_TRUE(found_aux_usage);
}

TEST(CompactionLoopTest, ManualCompactionWithNothingToCompactEndsTurn) {
    CompactionPolicy policy;
    policy.enabled           = true;
    policy.threshold_tokens  = 1'000'000;
    policy.keep_recent_turns = 1;

    AgentEnv env("compaction_none", std::make_unique<FakeLLM>(FakeScript{}), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, nullptr, false, policy);
    Agent& agent = env.createAgent();

    Session&        session = env.sessionOf(agent);
    const std::expected<CompactionOutcome, AgentError> queued =
        env.registry.requestCompaction(session.id());
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(*queued, CompactionOutcome::Queued);

    const EventRange events = session.events();
    EXPECT_EQ(count_type(events, EventType::ContextCompaction), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    const std::optional<payload::TurnStarted> started = last_turn_started(events);
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(started->origin, payload::TurnOrigin::Maintenance);
}

TEST(CompactionLoopTest, FullInboxRejectionEmitsDeferredFailureTurn) {
    CompactionPolicy policy;
    policy.enabled           = true;
    policy.threshold_tokens  = 1'000'000;
    policy.keep_recent_turns = 1;

    AgentEnv env("compaction_inbox_full", std::make_unique<FakeLLM>(FakeScript{}), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, nullptr, false, policy);
    Agent& agent = env.createAgent();

    ContextMessage inject;
    inject.role       = Role::System;
    inject.text       = "context";
    inject.startsTurn = false;
    for (std::size_t index = 0; index < 64; ++index) {
        ASSERT_EQ(agent.inject(inject), InboxResult::Accepted);
    }
    EXPECT_FALSE(agent.hasPendingWork());

    Session& session = env.sessionOf(agent);
    const std::expected<CompactionOutcome, AgentError> rejected =
        env.registry.requestCompaction(session.id());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, AgentErrorCode::InboxFull);

    EXPECT_FALSE(agent.hasPendingWork());
    const EventRange events = session.events();
    EXPECT_EQ(count_type(events, EventType::ContextCompaction), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 1u);
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            EXPECT_EQ(record.event.payload.get<payload::TurnFailed>().code, "InboxFull");
        }
    }
}

TEST(CompactionLoopTest, RequestAfterDisposeAppendsNothing) {
    CompactionPolicy policy;
    policy.enabled           = true;
    policy.threshold_tokens  = 1'000'000;
    policy.keep_recent_turns = 1;

    AgentEnv env("compaction_disposed", std::make_unique<FakeLLM>(FakeScript{}), AgentConfig{},
                 allow_all_permission_config(), {}, false, 4, nullptr, false, policy);
    Agent& agent = env.createAgent();
    Session& session = env.sessionOf(agent);
    const std::size_t before = session.events().size();

    agent.dispose();
    const std::expected<CompactionOutcome, AgentError> rejected =
        as_loop(agent).requestCompaction();
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, AgentErrorCode::AgentDisposed);
    EXPECT_EQ(session.events().size(), before);
}

} // namespace

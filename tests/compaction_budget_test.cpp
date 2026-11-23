#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"

#include "ymh/agent/compactor.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/context_snapshot.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using ymh::CompactionBoundary;
using ymh::ContentBlock;
using ymh::ContentBlockKind;
using ymh::DefaultTokenEstimator;
using ymh::EventRange;
using ymh::EventRecord;
using ymh::EventType;
using ymh::Message;
using ymh::Role;
using ymh::Sequence;
using ymh::ToolSchema;

EventRecord record(Sequence seq, EventType type, nlohmann::json payload = nlohmann::json::object()) {
    return EventRecord{seq, ymh_test::event_for("session", type, std::move(payload))};
}

EventRecord terminal(Sequence seq) { return record(seq, EventType::TurnEnded); }

EventRecord compaction(Sequence seq, Sequence boundary) {
    return record(seq, EventType::ContextCompaction,
                  nlohmann::json{{"boundary", boundary}, {"summary", "checkpoint"}});
}

EventRecord usage(Sequence seq, std::int64_t input_tokens) {
    return record(seq, EventType::TokenUsage,
                  nlohmann::json{{"usage", nlohmann::json{{"input_tokens", input_tokens}}}});
}

TEST(CompactionBoundary, NominalKeepSelectsTheNthMostRecentTurn) {
    const EventRange events{terminal(10), terminal(20), terminal(30)};
    const CompactionBoundary selected =
        ymh::select_compaction_boundary(events, 2, /*allow_relaxed=*/false);
    ASSERT_TRUE(selected.valid);
    EXPECT_EQ(selected.kept_turns, 2U);
    EXPECT_EQ(selected.boundary, Sequence{10});
}

TEST(CompactionBoundary, RelaxedKeepCompactsTheOnlyCompletedTurn) {
    const EventRange events{terminal(10)};
    EXPECT_FALSE(ymh::select_compaction_boundary(events, 2, /*allow_relaxed=*/false).valid);

    const CompactionBoundary relaxed =
        ymh::select_compaction_boundary(events, 2, /*allow_relaxed=*/true);
    ASSERT_TRUE(relaxed.valid);
    EXPECT_EQ(relaxed.kept_turns, 0U);
    EXPECT_EQ(relaxed.boundary, Sequence{10});
}

TEST(CompactionBoundary, RelaxedKeepNeverRepeatsAnExistingBoundary) {
    const EventRange events{terminal(10), compaction(15, 10), terminal(20)};
    const CompactionBoundary selected =
        ymh::select_compaction_boundary(events, 2, /*allow_relaxed=*/true);
    ASSERT_TRUE(selected.valid);
    EXPECT_EQ(selected.kept_turns, 0U);
    EXPECT_EQ(selected.boundary, Sequence{20});
}

TEST(CompactionBoundary, RelaxedKeepStillRefusesWhenEverythingIsShadowed) {
    const EventRange events{terminal(10), compaction(15, 10)};
    EXPECT_FALSE(ymh::select_compaction_boundary(events, 2, /*allow_relaxed=*/true).valid);
}

TEST(CompactionBoundary, NoTerminalEventMeansNoBoundary) {
    const EventRange events{record(1, EventType::TurnStarted)};
    EXPECT_FALSE(ymh::select_compaction_boundary(events, 2, /*allow_relaxed=*/true).valid);
}

TEST(CompactionBudget, PreflightEstimateCountsToolSchemas) {
    DefaultTokenEstimator estimator;

    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::string(400, 'a');
    message.content.push_back(std::move(block));
    const std::vector<Message> messages{message};

    ToolSchema schema;
    schema.name.value  = "mcp.alpha.search";
    schema.description = std::string(400, 'd');
    schema.input_schema = nlohmann::json{{"type", "object"}};
    const std::vector<ToolSchema> tools{schema};

    const std::size_t messages_only = estimator.estimate(messages);
    const std::size_t schema_only =
        static_cast<std::size_t>(ymh::estimate_tool_schema_tokens(schema));
    ASSERT_GT(schema_only, 0U);
    EXPECT_EQ(ymh::estimate_request_tokens(estimator, messages, tools),
              messages_only + schema_only);
    EXPECT_GT(ymh::estimate_request_tokens(estimator, messages, tools), messages_only);
}

TEST(ReportedInputTokens, UsesTheLastUsageWhenNoCompactionExists) {
    const EventRange events{usage(1, 1000), usage(2, 2000)};
    EXPECT_EQ(ymh::reported_input_token_floor(events), 2000U);
}

TEST(ReportedInputTokens, IgnoresUsageShadowedByACompaction) {
    const EventRange events{usage(1, 1000), compaction(2, 1), usage(3, 300)};
    EXPECT_EQ(ymh::reported_input_token_floor(events), 300U);
}

TEST(ReportedInputTokens, ReturnsZeroWhenEverythingPredatesTheLastCompaction) {
    const EventRange events{usage(1, 1000), compaction(2, 1)};
    EXPECT_EQ(ymh::reported_input_token_floor(events), 0U);
}

TEST(ReportedInputTokens, IgnoresNonPositiveUsage) {
    const EventRange events{usage(1, 0), usage(2, -5)};
    EXPECT_EQ(ymh::reported_input_token_floor(events), 0U);
}

TEST(CompactionBudget, EmptyToolListKeepsTheMessageEstimate) {
    DefaultTokenEstimator estimator;
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.text = "hello";
    message.content.push_back(std::move(block));
    const std::vector<Message> messages{message};
    EXPECT_EQ(ymh::estimate_request_tokens(estimator, messages, {}),
              estimator.estimate(messages));
}

} // namespace

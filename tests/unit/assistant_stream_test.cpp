#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ymh/llm/assistant_stream.hpp"
#include "ymh/session/errors.hpp"

namespace {

using namespace ymh;

using Events = std::vector<TimedStreamEvent>;

TimedStreamEvent timed(std::int64_t at_ms, StreamEvent event) {
    return TimedStreamEvent{std::chrono::milliseconds{at_ms}, std::move(event)};
}

ToolCallAssembled assembled(std::string id, std::string name, nlohmann::json arguments) {
    ToolCallAssembled call;
    call.id        = std::move(id);
    call.name      = std::move(name);
    call.arguments = std::move(arguments);
    return call;
}

// 34 §13 items 1-7: the block-assembly algorithm.
TEST(BlockAssembler, GoldenAssemblyReasoningTextToolUse) {
    BlockAssembler assembler;
    assembler.push(ReasoningDelta{"think"});
    assembler.push(TextDelta{"hello "});
    assembler.push(TextDelta{"world"});
    assembler.push(ToolCallStarted{0, "call-1", "read_file"});
    assembler.push(ToolCallDelta{0, "{\"path\":"});
    assembler.push(ToolCallFinished{0, assembled("call-1", "read_file", {{"path", "a.txt"}})});

    const std::vector<ContentBlock> blocks = assembler.blocks();
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].kind, ContentBlockKind::Reasoning);
    EXPECT_EQ(blocks[0].text, "think");
    EXPECT_EQ(blocks[1].kind, ContentBlockKind::Text);
    EXPECT_EQ(blocks[1].text, "hello world");
    EXPECT_EQ(blocks[2].kind, ContentBlockKind::ToolUse);
    EXPECT_EQ(blocks[2].tool_call_id, "call-1");
    EXPECT_EQ(blocks[2].tool_name, "read_file");
    EXPECT_EQ(blocks[2].arguments, nlohmann::json({{"path", "a.txt"}}));
}

TEST(BlockAssembler, CoalescesInterleavedDeltasIntoOneBlockPerKind) {
    BlockAssembler assembler;
    assembler.push(TextDelta{"a"});
    assembler.push(ReasoningDelta{"r1"});
    assembler.push(TextDelta{"b"});
    assembler.push(ReasoningDelta{"r2"});

    const std::vector<ContentBlock> blocks = assembler.blocks();
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].kind, ContentBlockKind::Reasoning);
    EXPECT_EQ(blocks[0].text, "r1r2");
    EXPECT_EQ(blocks[1].kind, ContentBlockKind::Text);
    EXPECT_EQ(blocks[1].text, "ab");
}

TEST(BlockAssembler, ToolUseBlocksAscendByIndex) {
    BlockAssembler assembler;
    assembler.push(ToolCallStarted{1, "call-b", "second"});
    assembler.push(ToolCallFinished{1, assembled("call-b", "second", nlohmann::json::object())});
    assembler.push(ToolCallStarted{0, "call-a", "first"});
    assembler.push(ToolCallFinished{0, assembled("call-a", "first", nlohmann::json::object())});

    const std::vector<ContentBlock> blocks = assembler.blocks();
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].tool_call_id, "call-a");
    EXPECT_EQ(blocks[1].tool_call_id, "call-b");
}

TEST(BlockAssembler, InterruptedBlocksDropToolUseAndPrefixBlocks) {
    BlockAssembler assembler;
    assembler.push(ReasoningDelta{"think"});
    assembler.push(TextDelta{"partial"});
    assembler.push(ToolCallStarted{0, "call-1", "read_file"});

    const std::vector<ContentBlock> interrupted = assembler.interrupted_blocks();
    ASSERT_EQ(interrupted.size(), 2u);
    EXPECT_EQ(interrupted[0].kind, ContentBlockKind::Reasoning);
    EXPECT_EQ(interrupted[1].kind, ContentBlockKind::Text);

    const std::vector<ContentBlock> full = assembler.blocks();
    ASSERT_EQ(full.size(), 2u);
    for (std::size_t index = 0; index < interrupted.size(); ++index) {
        EXPECT_EQ(interrupted[index].kind, full[index].kind);
        EXPECT_EQ(interrupted[index].text, full[index].text);
    }
}

TEST(BlockAssembler, UsageAuthorityFinishedBeatsAdvisory) {
    BlockAssembler assembler;
    assembler.push(UsageEvent{Usage{1, 2, 0, 0}});
    EXPECT_EQ(assembler.usage()->input_tokens, 1);

    assembler.push(Finished{FinishReason::Stop, Usage{9, 8, 0, 0}, std::nullopt});
    EXPECT_EQ(assembler.usage()->input_tokens, 9);
}

TEST(BlockAssembler, UsageAdvisoryFallbackAndAbsent) {
    BlockAssembler empty;
    EXPECT_FALSE(empty.usage().has_value());

    BlockAssembler advisory;
    advisory.push(UsageEvent{Usage{3, 4, 0, 0}});
    EXPECT_EQ(advisory.usage()->output_tokens, 4);
}

TEST(BlockAssembler, FinishDefaultsToStopAndReplayStateIsCaptured) {
    BlockAssembler fresh;
    EXPECT_EQ(fresh.finish(), FinishReason::Stop);
    EXPECT_FALSE(fresh.replay_state().has_value());

    ReplayEnvelope envelope;
    envelope.provider = "fake";
    envelope.state    = nlohmann::json{{"id", "resp-1"}};
    BlockAssembler assembler;
    assembler.push(Finished{FinishReason::Length, std::nullopt, envelope});
    EXPECT_EQ(assembler.finish(), FinishReason::Length);
    ASSERT_TRUE(assembler.replay_state().has_value());
    EXPECT_EQ(assembler.replay_state()->provider, "fake");
}

// 34 §13 items 8-11: accumulator packing and expand().
TEST(AssistantStreamAccumulator, RoundTripsAMixedStream) {
    const Events pushed{
        timed(0, ReasoningDelta{"r0"}),
        timed(5, ReasoningDelta{"r1"}),
        timed(10, TextDelta{"t0"}),
        timed(12, TextDelta{"t1"}),
        timed(20, ToolCallStarted{0, "call-1", "read_file"}),
        timed(25, ToolCallDelta{0, "{\"p\""}),
        timed(30, ToolCallDelta{0, ":1}"}),
        timed(40, ToolCallFinished{0, assembled("call-1", "read_file", {{"p", 1}})}),
        timed(45, UsageEvent{Usage{1, 1, 0, 0}}),
        timed(50, Finished{FinishReason::Stop, Usage{1, 1, 0, 0}, std::nullopt}),
    };

    AssistantStreamAccumulator accumulator;
    for (const TimedStreamEvent& event : pushed) {
        accumulator.push(event);
    }

    const std::vector<TimedStreamEvent> expanded = expand(accumulator.snapshot());
    ASSERT_EQ(expanded.size(), pushed.size());
    for (std::size_t index = 0; index < pushed.size(); ++index) {
        EXPECT_EQ(expanded[index].at, pushed[index].at);
        EXPECT_EQ(expanded[index].event, pushed[index].event);
    }
}

TEST(AssistantStreamAccumulator, PacksMaximalRunsAndAnchorsToolCalls) {
    AssistantStreamAccumulator accumulator;
    accumulator.push(timed(0, TextDelta{"a"}));
    accumulator.push(timed(1, TextDelta{"b"}));
    accumulator.push(timed(2, UsageEvent{Usage{}}));
    accumulator.push(timed(3, TextDelta{"c"}));
    accumulator.push(timed(4, ToolCallStarted{0, "call-1", "read_file"}));
    accumulator.push(timed(6, ToolCallDelta{0, "x"}));
    accumulator.push(timed(9, ToolCallDelta{0, "y"}));

    const std::vector<AssistantStreamRecord> snapshot = accumulator.snapshot();
    ASSERT_EQ(snapshot.size(), 4u);
    ASSERT_TRUE(std::holds_alternative<TextRun>(snapshot[0]));
    EXPECT_EQ(std::get<TextRun>(snapshot[0]).texts.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<ChunkRecord>(snapshot[1]));
    ASSERT_TRUE(std::holds_alternative<TextRun>(snapshot[2]));
    EXPECT_EQ(std::get<TextRun>(snapshot[2]).texts, (std::vector<std::string>{"c"}));
    ASSERT_TRUE(std::holds_alternative<ToolCallRun>(snapshot[3]));
    const ToolCallRun& run = std::get<ToolCallRun>(snapshot[3]);
    EXPECT_EQ(run.id, "call-1");
    EXPECT_EQ(run.args, (std::vector<std::string>{"x", "y"}));
    EXPECT_EQ(run.dt_ms, (std::vector<std::int64_t>{2, 3}));
}

TEST(AssistantStreamAccumulator, OrphanToolCallDeltaIsRecordedVerbatim) {
    AssistantStreamAccumulator accumulator;
    accumulator.push(timed(7, ToolCallDelta{0, "orphan"}));

    const std::vector<AssistantStreamRecord> snapshot = accumulator.snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<ChunkRecord>(snapshot[0]));
    EXPECT_EQ(std::get<ChunkRecord>(snapshot[0]).event, (StreamEvent{ToolCallDelta{0, "orphan"}}));
}

TEST(Expand, RejectsMalformedRecordsLoudly) {
    TextRun bad_size;
    bad_size.time0_ms = 0;
    bad_size.texts    = {"a", "b"};
    bad_size.dt_ms    = {};
    EXPECT_THROW(static_cast<void>(expand({bad_size})), CorruptionError);

    TextRun negative_dt;
    negative_dt.time0_ms = 0;
    negative_dt.texts    = {"a", "b"};
    negative_dt.dt_ms    = {-1};
    EXPECT_THROW(static_cast<void>(expand({negative_dt})), CorruptionError);

    ToolCallRun empty_id;
    empty_id.time0_ms = 0;
    EXPECT_THROW(static_cast<void>(expand({empty_id})), CorruptionError);
}

} // namespace

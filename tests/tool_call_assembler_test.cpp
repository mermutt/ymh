#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

#include "ymh/llm/tool_call_assembler.hpp"

namespace {

using namespace ymh;

constexpr std::size_t kMaxArguments = 1u << 20;

TEST(ToolCallAssembler, AssignsAnIdWhenTheProviderOmitsOne) {
    ToolCallAssembler assembler(kMaxArguments);
    const ToolCallId  assigned = assembler.onStarted(0, ToolCallId{}, "shell");
    EXPECT_FALSE(assigned.empty());

    assembler.onDelta(0, "{}");
    const std::optional<ToolCallAssembled> call = assembler.onFinished(0);
    ASSERT_TRUE(call.has_value());
    EXPECT_EQ(call->id, assigned);
    EXPECT_EQ(call->name, "shell");
}

TEST(ToolCallAssembler, ReplacesDuplicateIdsWithinOneResponse) {
    ToolCallAssembler assembler(kMaxArguments);
    const ToolCallId  first  = assembler.onStarted(0, "0", "shell");
    const ToolCallId  second = assembler.onStarted(1, "0", "shell");

    EXPECT_EQ(first, "0");
    EXPECT_FALSE(second.empty());
    EXPECT_NE(first, second);

    assembler.onDelta(0, "{}");
    assembler.onDelta(1, "{}");
    const auto first_call  = assembler.onFinished(0);
    const auto second_call = assembler.onFinished(1);
    ASSERT_TRUE(first_call.has_value());
    ASSERT_TRUE(second_call.has_value());
    EXPECT_EQ(first_call->id, first);
    EXPECT_EQ(second_call->id, second);

    std::vector<ToolCallAssembled> calls = std::move(assembler).take_ordered();
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].id, first);
    EXPECT_EQ(calls[1].id, second);
}

TEST(ToolCallAssembler, SynthesizesDistinctIdsForEmptyCalls) {
    ToolCallAssembler assembler(kMaxArguments);
    const ToolCallId  first  = assembler.onStarted(0, ToolCallId{}, "shell");
    const ToolCallId  second = assembler.onStarted(1, ToolCallId{}, "shell");

    EXPECT_FALSE(first.empty());
    EXPECT_FALSE(second.empty());
    EXPECT_NE(first, second);
}

TEST(ToolCallAssembler, PreservesAUniqueNonEmptyId) {
    ToolCallAssembler assembler(kMaxArguments);
    EXPECT_EQ(assembler.onStarted(0, "call_abc", "shell"), "call_abc");
    EXPECT_EQ(assembler.onStarted(1, "call_def", "shell"), "call_def");
}

} // namespace

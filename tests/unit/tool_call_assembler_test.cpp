#include <gtest/gtest.h>

#include "ymh/llm/tool_call_assembler.hpp"

using ymh::LLMErrorCode;
using ymh::ToolCallAssembler;

TEST(ToolCallAssemblerTest, AssemblesFragmentsInArrivalOrder) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "call_1", "read_file");
    assembler.onDelta(0, "{\"pa");
    assembler.onDelta(0, "th\":\"a.cpp\"}");

    const auto finished = assembler.onFinished(0);
    ASSERT_TRUE(finished.has_value());
    EXPECT_EQ(finished->id, "call_1");
    EXPECT_EQ(finished->name, "read_file");
    EXPECT_EQ(finished->arguments, nlohmann::json({{"path", "a.cpp"}}));
    EXPECT_FALSE(assembler.has_error());
}

TEST(ToolCallAssemblerTest, EmptyArgumentsBecomeEmptyObject) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "call_1", "list_dir");
    const auto finished = assembler.onFinished(0);
    ASSERT_TRUE(finished.has_value());
    EXPECT_TRUE(finished->arguments.is_object());
    EXPECT_TRUE(finished->arguments.empty());
}

TEST(ToolCallAssemblerTest, InterleavedIndicesAssembleIndependently) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "a", "one");
    assembler.onStarted(1, "b", "two");
    assembler.onDelta(0, "{\"x\":");
    assembler.onDelta(1, "{\"y\":");
    assembler.onDelta(0, "1}");
    assembler.onDelta(1, "2}");

    const auto second = assembler.onFinished(1);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->arguments, nlohmann::json({{"y", 2}}));

    const auto first = assembler.onFinished(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->arguments, nlohmann::json({{"x", 1}}));

    std::vector<ymh::ToolCallAssembled> ordered = std::move(assembler).take_ordered();
    ASSERT_EQ(ordered.size(), 2u);
    EXPECT_EQ(ordered[0].id, "a");
    EXPECT_EQ(ordered[1].id, "b");
}

TEST(ToolCallAssemblerTest, DuplicateStartIsProviderInternal) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "a", "one");
    assembler.onStarted(0, "b", "two");
    EXPECT_TRUE(assembler.has_error());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::ProviderInternal);
}

TEST(ToolCallAssemblerTest, DeltaForUnknownIndexIsProviderInternal) {
    ToolCallAssembler assembler(1024);
    assembler.onDelta(0, "{}");
    EXPECT_TRUE(assembler.has_error());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::ProviderInternal);
}

TEST(ToolCallAssemblerTest, FinishForUnknownIndexIsProviderInternal) {
    ToolCallAssembler assembler(1024);
    EXPECT_FALSE(assembler.onFinished(3).has_value());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::ProviderInternal);
}

TEST(ToolCallAssemblerTest, DuplicateFinishIsProviderInternal) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "a", "one");
    ASSERT_TRUE(assembler.onFinished(0).has_value());
    EXPECT_FALSE(assembler.onFinished(0).has_value());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::ProviderInternal);
}

TEST(ToolCallAssemblerTest, InvalidJsonIsMalformedToolCall) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "a", "one");
    assembler.onDelta(0, "{not json");
    EXPECT_FALSE(assembler.onFinished(0).has_value());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::MalformedToolCall);
}

TEST(ToolCallAssemblerTest, NonObjectArgumentsAreMalformedToolCall) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "a", "one");
    assembler.onDelta(0, "[1,2,3]");
    EXPECT_FALSE(assembler.onFinished(0).has_value());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::MalformedToolCall);
}

TEST(ToolCallAssemblerTest, ArgumentsCapIsMalformedToolCall) {
    ToolCallAssembler assembler(4);
    assembler.onStarted(0, "a", "one");
    assembler.onDelta(0, "{\"abc");
    assembler.onDelta(0, "def\"}");
    EXPECT_EQ(assembler.error().code, LLMErrorCode::MalformedToolCall);
}

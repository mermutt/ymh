#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/execution/output.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;

TEST(OutputRing, UnderCapacityKeepsEverythingAndDoesNotTruncate) {
    OutputRing ring(64);
    ring.append("hello ", false);
    ring.append("world", true);

    EXPECT_EQ(ring.size(), 11u);
    EXPECT_FALSE(ring.truncated());
    EXPECT_EQ(ring.tail(64), "hello world");
}

TEST(OutputRing, OverCapacityKeepsTheTailAndTruncates) {
    OutputRing ring(8);
    ring.append("abcdef", false);
    ring.append("ghij", false);

    EXPECT_TRUE(ring.truncated());
    EXPECT_EQ(ring.size(), 8u);
    EXPECT_EQ(ring.tail(8), "cdefghij");
}

TEST(OutputRing, MaterializeNeverExceedsTheRequestedCap) {
    OutputRing ring(1024);
    ring.append(std::string(500, 'x'), false);

    EXPECT_EQ(ring.tail(100).size(), 100u);
    EXPECT_EQ(ring.tail(0).size(), 0u);
    EXPECT_LE(ring.tail(9999).size(), 500u);
}

TEST(OutputSink, LossyConvertsInvalidUtf8AndFlagsTruncation) {
    OutputRing ring(64);
    RingOutputSink sink(ring);
    const char raw[] = {'a', static_cast<char>(0xFF), 'b', static_cast<char>(0xFE), 'c'};
    sink.write(std::string_view(raw, sizeof(raw)));

    const std::string materialized = sink.materialize(64);
    EXPECT_EQ(materialized.find(static_cast<char>(0xFF)), std::string::npos);
    EXPECT_EQ(materialized.find(static_cast<char>(0xFE)), std::string::npos);
    EXPECT_NE(materialized.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_TRUE(sink.truncated());
    EXPECT_EQ(sink.bytesWritten(), sizeof(raw));
}

TEST(OutputSink, PreservesStdoutStderrWriteOrder) {
    OutputRing ring(64);
    RingOutputSink sink(ring);
    sink.write("out1-");
    sink.writeErr("err1-");
    sink.write("out2");

    EXPECT_EQ(sink.materialize(64), "out1-err1-out2");
}

TEST(DurableClamp, HugeOutputIsClampedOnTheSerializedSize) {
    ToolResult result;
    result.id = "call-1";
    result.name = "shell";
    result.output = std::string(100'000, 'a');

    const bool clamped = clamp_tool_result(result, 4096);
    EXPECT_TRUE(clamped);
    EXPECT_TRUE(result.truncated);

    nlohmann::json json = result;
    EXPECT_LE(json.dump().size(), 4096u);
}

TEST(DurableClamp, ControlCharactersAreMeasuredAfterEscaping) {
    ToolResult result;
    result.id = "call-2";
    result.name = "shell";
    result.output = std::string(20'000, '\0');
    result.output += std::string(20'000, '\x1b');

    const bool clamped = clamp_tool_result(result, 8192);
    EXPECT_TRUE(clamped);

    nlohmann::json json = result;
    EXPECT_LE(json.dump().size(), 8192u);
}

TEST(DurableClamp, SmallResultIsUntouched) {
    ToolResult result;
    result.id = "call-3";
    result.name = "read_file";
    result.output = "small";

    EXPECT_FALSE(clamp_tool_result(result, 4096));
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(result.output, "small");
}

} // namespace

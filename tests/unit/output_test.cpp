#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/execution/output.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

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

TEST(ToolResultRetention, LargeOutputIsBoundedAndReportsExactOmission) {
    ToolResult result;
    result.id     = "call-1";
    result.name   = "shell";
    result.output = std::string(100'000, 'a');

    retain_tool_result(result, 4096);

    EXPECT_EQ(result.omitted_kind, OmittedKind::Exact);
    EXPECT_GT(result.omitted_count, 0u);
    EXPECT_TRUE(result.truncated);

    const nlohmann::json json = result;
    EXPECT_LE(json.dump().size(), 4096u);
    EXPECT_NE(result.output.find("Omitted"), std::string::npos);
}

TEST(ToolResultRetention, EscapingHeavyOutputStaysWithinTheCap) {
    ToolResult result;
    result.id     = "call-2";
    result.name   = "shell";
    result.output = std::string(20'000, '\0');
    result.output += std::string(20'000, '\x1b');

    retain_tool_result(result, 8192);

    EXPECT_TRUE(result.truncated);

    const nlohmann::json json = result;
    EXPECT_LE(json.dump().size(), 8192u);
}

TEST(ToolResultRetention, SmallResultIsUntouched) {
    ToolResult result;
    result.id     = "call-3";
    result.name   = "read_file";
    result.output = "small";

    retain_tool_result(result, 4096);

    EXPECT_EQ(result.omitted_kind, OmittedKind::None);
    EXPECT_EQ(result.omitted_count, 0u);
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(result.output, "small");
}

TEST(ToolResultRetention, ToolDomainTruncationMapsToUnknown) {
    ToolResult result;
    result.id        = "call-4";
    result.name      = "read_file";
    result.output    = "01234";
    result.truncated = true;

    retain_tool_result(result, 4096);

    EXPECT_EQ(result.omitted_kind, OmittedKind::Unknown);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.output, "01234");
}

TEST(ClampRetirement, NoClampToolResultReferenceRemainsInTheTree) {
    const std::string needle = std::string{"clamp_"} + "tool_result";
    const std::filesystem::path root{YMH_SOURCE_DIR};

    std::vector<std::string> offenders;
    for (const char* relative : {"include", "src", "tests"}) {
        const std::filesystem::path base = root / relative;
        if (!std::filesystem::exists(base)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string extension = entry.path().extension().string();
            if (extension != ".cpp" && extension != ".hpp") {
                continue;
            }
            const std::string text = read_text_file(entry.path());
            if (text.find(needle) != std::string::npos) {
                offenders.push_back(entry.path().string());
            }
        }
    }
    EXPECT_TRUE(offenders.empty()) << "clamp references remain in: "
                                   << (offenders.empty() ? std::string{} : offenders.front());
}

} // namespace

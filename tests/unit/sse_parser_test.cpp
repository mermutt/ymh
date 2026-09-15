#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "ymh/llm/sse_parser.hpp"

namespace {

std::vector<ymh::SseEvent> parse_in_chunks(const std::vector<std::string>& chunks,
                                           std::size_t line_cap = 1u << 20) {
    ymh::SseParser parser(line_cap);
    std::vector<ymh::SseEvent> events;
    const auto sink = [&events](const ymh::SseEvent& event) { events.push_back(event); };
    for (const std::string& chunk : chunks) {
        EXPECT_EQ(parser.feed(chunk, sink), ymh::SseFeedStatus::Ok);
    }
    parser.finish(sink);
    return events;
}

} // namespace

TEST(SseParserTest, SingleDataFrame) {
    const auto events = parse_in_chunks({"data: {\"a\":1}\n\n"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "{\"a\":1}");
    EXPECT_FALSE(events[0].done);
}

TEST(SseParserTest, FrameSplitAcrossChunkBoundaries) {
    const auto events = parse_in_chunks({"data: {\"a\"", ":1}\n", "\n"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "{\"a\":1}");
}

TEST(SseParserTest, MultiLineDataIsJoinedWithNewline) {
    const auto events = parse_in_chunks({"data: line1\ndata: line2\n\n"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "line1\nline2");
}

TEST(SseParserTest, CommentsAndUnknownFieldsAreIgnored) {
    const auto events =
        parse_in_chunks({": keep-alive\n", "event: message\n", "id: 7\n", "data: x\n\n"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "x");
}

TEST(SseParserTest, CrlfIsAccepted) {
    const auto events = parse_in_chunks({"data: x\r\n\r\n"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "x");
}

TEST(SseParserTest, DoneTerminatesAndIgnoresTrailingBytes) {
    const auto events = parse_in_chunks({"data: [DONE]\n\ndata: ignored\n\n"});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].done);
    EXPECT_EQ(events[0].data, "[DONE]");
}

TEST(SseParserTest, LineCapOverflow) {
    ymh::SseParser parser(16);
    int emitted = 0;
    const auto sink = [&emitted](const ymh::SseEvent&) { ++emitted; };
    EXPECT_EQ(parser.feed("data: 0123456789abcdefghij\n\n", sink), ymh::SseFeedStatus::Overflow);
    EXPECT_TRUE(parser.overflowed());
    EXPECT_EQ(emitted, 0);
}

TEST(SseParserTest, Utf8SplitAcrossChunksIsPreserved) {
    const std::string text = "héllo-日本";
    std::string frame = "data: " + text + "\n\n";
    std::vector<std::string> chunks;
    for (const char c : frame) {
        chunks.emplace_back(1, c);
    }
    const auto events = parse_in_chunks(chunks);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, text);
}

TEST(SseParserTest, FinishFlushesTrailingDataWithoutBlankLine) {
    ymh::SseParser parser(1u << 20);
    std::vector<ymh::SseEvent> events;
    const auto sink = [&events](const ymh::SseEvent& event) { events.push_back(event); };
    EXPECT_EQ(parser.feed("data: tail", sink), ymh::SseFeedStatus::Ok);
    parser.finish(sink);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].data, "tail");
}

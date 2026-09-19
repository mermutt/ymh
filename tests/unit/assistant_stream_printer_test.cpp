#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/agent/message.hpp"
#include "ymh/cli/assistant_stream_printer.hpp"
#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"

namespace {

using namespace ymh;

Event typed_event(EventType type, const nlohmann::json& payload) {
    Event event;
    event.id         = EventId{"event"};
    event.session_id = SessionId{"session"};
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = type;
    event.payload    = payload;
    return event;
}

Event text_chunk(std::string message, std::string text) {
    payload::AssistantChunk chunk;
    chunk.message = std::move(message);
    chunk.text    = std::move(text);
    chunk.kind    = payload::AssistantChunkKind::Text;
    return typed_event(EventType::AssistantChunk, chunk);
}

Event reasoning_chunk(std::string message, std::string text) {
    payload::AssistantChunk chunk;
    chunk.message = std::move(message);
    chunk.text    = std::move(text);
    chunk.kind    = payload::AssistantChunkKind::Reasoning;
    return typed_event(EventType::AssistantChunk, chunk);
}

Event assistant_message(std::string id, std::string text) {
    payload::AssistantMessage message;
    message.id = std::move(id);
    if (!text.empty()) {
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = std::move(text);
        message.content.push_back(std::move(block));
    }
    return typed_event(EventType::AssistantMessage, message);
}

Event failed_attempt() {
    payload::AssistantAttempt attempt;
    attempt.turn = 1;
    attempt.step = 1;
    return typed_event(EventType::AssistantAttempt, attempt);
}

// 26-D9 / 35 §3.6: the durable settlement wins over the live buffer because it is
// the recovery path for a dropped live chunk.
TEST(AssistantStreamPrinter, DurableWinsOverLiveBuffer) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    EXPECT_TRUE(printer.feed(text_chunk("m1", "hello"), out, err, false).handled);
    EXPECT_TRUE(out.str().empty());

    const auto commit = printer.feed(assistant_message("m1", "hello world"), out, err, false);
    EXPECT_EQ(commit.text, "hello world");
    EXPECT_EQ(out.str(), "hello world");
}

TEST(AssistantStreamPrinter, FallsBackToLiveBufferWhenDurableEmpty) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(text_chunk("m1", "live-only"), out, err, false);
    const auto commit = printer.feed(assistant_message("m1", ""), out, err, false);
    EXPECT_EQ(commit.text, "live-only");
    EXPECT_EQ(out.str(), "live-only");
}

// 34 §15 item 1: a failed attempt's streamed text is discarded before the retry
// re-streams under the same message id.
TEST(AssistantStreamPrinter, DiscardsBufferOnFailedAttempt) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(text_chunk("m1", "attempt-zero-text"), out, err, false);
    printer.feed(failed_attempt(), out, err, false);
    EXPECT_TRUE(out.str().empty());

    printer.feed(text_chunk("m1", "attempt-one-text"), out, err, false);
    const auto commit = printer.feed(assistant_message("m1", "attempt-one-text"), out, err, false);
    EXPECT_EQ(commit.text, "attempt-one-text");
    EXPECT_EQ(out.str().find("attempt-zero-text"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("attempt-one-text"), std::string::npos) << out.str();
}

TEST(AssistantStreamPrinter, PrintsBufferedReasoningWhenEnabled) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(reasoning_chunk("m1", "thinking"), out, err, true);
    EXPECT_TRUE(err.str().empty());

    printer.feed(assistant_message("m1", "answer"), out, err, true);
    EXPECT_EQ(err.str(), "thinking");
    EXPECT_EQ(out.str(), "answer");
}

TEST(AssistantStreamPrinter, SuppressesBufferedReasoningWhenDisabled) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(reasoning_chunk("m1", "thinking"), out, err, false);
    printer.feed(assistant_message("m1", "answer"), out, err, false);
    EXPECT_TRUE(err.str().empty());
    EXPECT_EQ(out.str(), "answer");
}

// F8: without a settlement the live buffer must not grow without bound.
TEST(AssistantStreamPrinter, LiveBufferStaysBoundedWithoutSettlement) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    const std::string chunk(4096, 'x');
    const std::size_t target = AssistantStreamPrinter::kMaxBufferedBytes * 10;
    for (std::size_t fed = 0; fed < target; fed += chunk.size()) {
        printer.feed(text_chunk("m1", chunk), out, err, false);
    }

    const auto commit = printer.feed(assistant_message("m1", ""), out, err, false);
    EXPECT_EQ(commit.text.size(), AssistantStreamPrinter::kMaxBufferedBytes);
    EXPECT_EQ(out.str().size(), AssistantStreamPrinter::kMaxBufferedBytes);
}

TEST(AssistantStreamPrinter, FullDurableTextAfterBufferCapExceeded) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    const std::string chunk(4096, 'x');
    const std::size_t target = AssistantStreamPrinter::kMaxBufferedBytes * 10;
    for (std::size_t fed = 0; fed < target; fed += chunk.size()) {
        printer.feed(text_chunk("m1", chunk), out, err, false);
    }

    const std::string durable(AssistantStreamPrinter::kMaxBufferedBytes * 2, 'd');
    const auto        commit = printer.feed(assistant_message("m1", durable), out, err, false);
    EXPECT_EQ(commit.text, durable);
    EXPECT_EQ(out.str(), durable);
}

} // namespace

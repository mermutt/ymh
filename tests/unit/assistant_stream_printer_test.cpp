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

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

// 34-I12 / R1: live text deltas are written to `out` as they arrive, before any
// settlement; a delta carries no settlement text.
TEST(AssistantStreamPrinter, StreamsTextDeltasBeforeSettlement) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    const auto first = printer.feed(text_chunk("m1", "hello "), out, err, false);
    EXPECT_TRUE(first.handled);
    EXPECT_TRUE(first.text.empty());
    EXPECT_EQ(out.str(), "hello ");

    const auto second = printer.feed(text_chunk("m1", "world"), out, err, false);
    EXPECT_TRUE(second.handled);
    EXPECT_TRUE(second.text.empty());
    EXPECT_EQ(out.str(), "hello world");
    EXPECT_TRUE(err.str().empty());
}

// 34-I13 / R2: the durable settlement is the authority; only the not-yet-streamed
// suffix is printed.
TEST(AssistantStreamPrinter, DurableWinsOverLiveBuffer) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    EXPECT_TRUE(printer.feed(text_chunk("m1", "hello"), out, err, false).handled);
    EXPECT_EQ(out.str(), "hello");

    const auto commit = printer.feed(assistant_message("m1", "hello world"), out, err, false);
    EXPECT_EQ(commit.text, "hello world");
    EXPECT_EQ(out.str(), "hello world");
}

// 34-I13 / R2: a full live stream followed by an equal durable settlement prints
// the text exactly once.
TEST(AssistantStreamPrinter, CompletedSettlementDoesNotDoublePrintStreamedText) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(text_chunk("m1", "alpha"), out, err, false);
    printer.feed(text_chunk("m1", "beta"), out, err, false);
    EXPECT_EQ(out.str(), "alphabeta");

    const auto commit = printer.feed(assistant_message("m1", "alphabeta"), out, err, false);
    EXPECT_EQ(commit.text, "alphabeta");
    EXPECT_EQ(out.str(), "alphabeta");
    EXPECT_EQ(count_occurrences(out.str(), "alphabeta"), 1u);
}

// 34-I13 / R2: an empty durable settlement attributes the live record without
// re-printing the already-streamed text.
TEST(AssistantStreamPrinter, FallsBackToLiveBufferWhenDurableEmpty) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(text_chunk("m1", "live-only"), out, err, false);
    EXPECT_EQ(out.str(), "live-only");

    const auto commit = printer.feed(assistant_message("m1", ""), out, err, false);
    EXPECT_EQ(commit.text, "live-only");
    EXPECT_EQ(out.str(), "live-only");
}

// 34-I14 / R3: a failed attempt's streamed text cannot be retracted from
// forward-only stdout, so a visible retry marker is emitted on `err`.
TEST(AssistantStreamPrinter, FailedAttemptEmitsRetryMarker) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(text_chunk("m1", "partial"), out, err, false);
    const auto commit = printer.feed(failed_attempt(), out, err, false);
    EXPECT_TRUE(commit.handled);
    EXPECT_TRUE(commit.text.empty());
    EXPECT_EQ(out.str(), "partial");
    EXPECT_EQ(err.str(), std::string(AssistantStreamPrinter::kRetryMarker));
}

// 34-I14 / R3: the attempt record is discarded, so the retry's settlement adds
// no duplicate; the failed attempt's streamed text stays on `out`.
TEST(AssistantStreamPrinter, DiscardsBufferOnFailedAttempt) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(text_chunk("m1", "attempt-zero-text"), out, err, false);
    printer.feed(failed_attempt(), out, err, false);
    EXPECT_NE(out.str().find("attempt-zero-text"), std::string::npos) << out.str();
    EXPECT_NE(err.str().find("retry"), std::string::npos) << err.str();

    printer.feed(text_chunk("m1", "attempt-one-text"), out, err, false);
    const auto commit = printer.feed(assistant_message("m1", "attempt-one-text"), out, err, false);
    EXPECT_EQ(commit.text, "attempt-one-text");
    EXPECT_EQ(count_occurrences(out.str(), "attempt-one-text"), 1u) << out.str();
}

// 34-I16 / R5: reasoning is never streamed live and reaches `err` on commit only
// when `print_reasoning` is set.
TEST(AssistantStreamPrinter, PrintsBufferedReasoningWhenEnabled) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(reasoning_chunk("m1", "thinking"), out, err, true);
    EXPECT_TRUE(err.str().empty());
    EXPECT_TRUE(out.str().empty());

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

// 34-I16 / R5: reasoning accumulates in the buffer and is emitted once at
// commit; it never leaks to `out` while streaming.
TEST(AssistantStreamPrinter, ReasoningStaysBufferedUntilCommit) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    printer.feed(reasoning_chunk("m1", "step "), out, err, true);
    printer.feed(reasoning_chunk("m1", "by step"), out, err, true);
    EXPECT_TRUE(err.str().empty());
    EXPECT_TRUE(out.str().empty());

    printer.feed(assistant_message("m1", "final"), out, err, true);
    EXPECT_EQ(err.str(), "step by step");
    EXPECT_EQ(out.str(), "final");
}

// F8 / 34-I15 / R4: without a settlement the retained record must not grow
// without bound; the live output itself is not truncated.
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
    EXPECT_EQ(out.str().size(), target);
}

// 34-I13 / R4: prefix divergence past the cap means the durable is the authority
// and is printed in full.
TEST(AssistantStreamPrinter, FullDurableTextAfterBufferCapExceeded) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    const std::string chunk(4096, 'x');
    const std::size_t target = AssistantStreamPrinter::kMaxBufferedBytes * 10;
    for (std::size_t fed = 0; fed < target; fed += chunk.size()) {
        printer.feed(text_chunk("m1", chunk), out, err, false);
    }
    EXPECT_EQ(out.str().size(), target);

    const std::string durable(AssistantStreamPrinter::kMaxBufferedBytes * 2, 'd');
    const auto        commit = printer.feed(assistant_message("m1", durable), out, err, false);
    EXPECT_EQ(commit.text, durable);
    ASSERT_GE(out.str().size(), durable.size());
    EXPECT_EQ(out.str().substr(out.str().size() - durable.size()), durable);
}

// 34-I15 / R4: a matching stream past the cap is deduped by the streamed byte
// count, so the settlement does not re-print the already-streamed bytes.
TEST(AssistantStreamPrinter, LongStreamBeyondCapIsNotReprinted) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    const std::string chunk(4096, 'a');
    const std::size_t total = AssistantStreamPrinter::kMaxBufferedBytes + chunk.size() * 3;
    for (std::size_t fed = 0; fed < total; fed += chunk.size()) {
        printer.feed(text_chunk("m1", chunk), out, err, false);
    }

    const std::string durable(total, 'a');
    EXPECT_EQ(out.str(), durable);

    const auto commit = printer.feed(assistant_message("m1", durable), out, err, false);
    EXPECT_EQ(commit.text, durable);
    EXPECT_EQ(out.str(), durable);
}

} // namespace

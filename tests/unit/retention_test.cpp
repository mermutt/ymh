#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/omission.hpp"
#include "ymh/core/retention.hpp"
#include "ymh/session/events.hpp"

namespace {

using namespace ymh;

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::size_t escaped_width(std::string_view text) {
    std::size_t total = 0;
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte == '"' || byte == '\\') {
            total += 2;
        } else if (byte < 0x20) {
            total += 6;
        } else {
            total += 1;
        }
    }
    return total;
}

TEST(RetentionDeque, PushAndPopAtBothEnds) {
    Deque<int> deque;
    deque.push_back(2);
    deque.push_back(3);
    deque.push_front(1);

    EXPECT_EQ(deque.size(), 3u);
    const auto pop = [&deque] {
        const std::optional<int> value = deque.pop_front();
        EXPECT_TRUE(value.has_value());
        return value.value_or(-1);
    };
    EXPECT_EQ(pop(), 1);
    EXPECT_EQ(pop(), 2);
    EXPECT_EQ(pop(), 3);
}

TEST(RetentionDeque, PopFrontOnEmptyIsNullopt) {
    Deque<int> deque;
    EXPECT_FALSE(deque.pop_front().has_value());
    EXPECT_EQ(deque.size(), 0u);
}

TEST(RetentionDeque, ClearResetsToEmpty) {
    Deque<int> deque;
    deque.push_back(1);
    deque.push_front(2);
    deque.clear();

    EXPECT_EQ(deque.size(), 0u);
    EXPECT_EQ(deque.capacity(), 0u);
    EXPECT_FALSE(deque.pop_front().has_value());
}

TEST(RetentionDeque, ShrinksWhenLiveEntriesReachAQuarterOfCapacity) {
    Deque<int> deque;
    for (int i = 0; i < 16; ++i) {
        deque.push_back(i);
    }
    EXPECT_EQ(deque.size(), 16u);
    EXPECT_EQ(deque.capacity(), 16u);

    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(deque.pop_front().has_value());
    }
    EXPECT_EQ(deque.size(), 4u);
    EXPECT_LT(deque.capacity(), 16u);

    for (int i = 12; i < 16; ++i) {
        const std::optional<int> value = deque.pop_front();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
    EXPECT_EQ(deque.size(), 0u);
}

TEST(RetentionChunkedList, EmptyIteratesEmpty) {
    const std::optional<ChunkedList<int>> head;
    EXPECT_TRUE(iterate_chunked_list(head).empty());
}

TEST(RetentionChunkedList, IteratesInsertionOrderAcrossChunks) {
    std::optional<ChunkedList<int>> head;
    for (int i = 0; i < 100; ++i) {
        head = append_chunked_list(std::move(head), i);
    }

    const std::vector<int> values = iterate_chunked_list(head);
    ASSERT_EQ(values.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i);
    }
}

TEST(RetentionChunkedList, AppendSharesUnchangedOlderChunks) {
    std::optional<ChunkedList<int>> head;
    for (int i = 0; i < 64; ++i) {
        head = append_chunked_list(std::move(head), i);
    }
    ASSERT_EQ(head->values.size(), 64u);
    ASSERT_EQ(head->previous, nullptr);

    head = append_chunked_list(std::move(head), 64);
    ASSERT_NE(head->previous, nullptr);
    EXPECT_EQ(head->previous->values.size(), 64u);

    const std::shared_ptr<const ChunkedList<int>> first_shared = head->previous;
    head = append_chunked_list(std::move(head), 65);
    ASSERT_EQ(head->values.size(), 2u);
    EXPECT_EQ(head->previous.get(), first_shared.get());
    EXPECT_EQ(iterate_chunked_list(head), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                                            12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                                            22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                                                            32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                                                            42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
                                                            52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
                                                            62, 63, 64, 65}));
}

struct CopyCounted {
    static inline int copies = 0;
    int               value  = 0;

    CopyCounted() = default;
    explicit CopyCounted(int v) : value(v) {}
    CopyCounted(const CopyCounted& other) : value(other.value) { ++copies; }
    CopyCounted& operator=(const CopyCounted& other) {
        value = other.value;
        ++copies;
        return *this;
    }
    CopyCounted(CopyCounted&&) noexcept            = default;
    CopyCounted& operator=(CopyCounted&&) noexcept = default;
};

TEST(RetentionChunkedList, AppendCopiesAtMostOneChunk) {
    std::optional<ChunkedList<CopyCounted>> head;
    for (int i = 0; i < 65; ++i) {
        head = append_chunked_list(std::move(head), CopyCounted{i});
    }
    ASSERT_EQ(head->values.size(), 1u);
    ASSERT_NE(head->previous, nullptr);
    ASSERT_EQ(head->previous->values.size(), 64u);

    CopyCounted::copies = 0;
    const ChunkedList<CopyCounted> extended = append_chunked_list(head, CopyCounted{65});
    EXPECT_LE(CopyCounted::copies, static_cast<int>(ChunkedList<CopyCounted>::kChunkSize));
    EXPECT_EQ(extended.previous.get(), head->previous.get());
}

TEST(RetentionItemRetainer, KeepsHeadWindowWithoutOmissionUnderBudget) {
    ItemRetainer<std::string> retainer(3);
    EXPECT_TRUE(retainer.push("a").accepted);
    EXPECT_TRUE(retainer.push("b").accepted);
    const PushDecision third = retainer.push("c");
    EXPECT_TRUE(third.accepted);
    EXPECT_EQ(third.omitted.kind, OmittedKind::None);

    const RetainedItems<std::string> result = retainer.finish();
    EXPECT_EQ(result.items, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(result.omitted.kind, OmittedKind::None);
    EXPECT_EQ(result.omitted.count, 0u);
}

TEST(RetentionItemRetainer, ReportsExactOmissionForOverflow) {
    ItemRetainer<int> retainer(2);
    EXPECT_TRUE(retainer.push(1).accepted);
    EXPECT_TRUE(retainer.push(2).accepted);

    const PushDecision dropped = retainer.push(3);
    EXPECT_FALSE(dropped.accepted);
    EXPECT_EQ(dropped.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(dropped.omitted.count, 1u);

    const PushDecision dropped_again = retainer.push(4);
    EXPECT_EQ(dropped_again.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(dropped_again.omitted.count, 2u);

    const RetainedItems<int> result = retainer.finish();
    EXPECT_EQ(result.items, (std::vector<int>{1, 2}));
    EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(result.omitted.count, 2u);
}

TEST(RetentionTextRetainer, UnderBudgetKeepsEverythingAndReportsNone) {
    TextRetainer retainer(TextRetentionStrategy::HeadTail, 64);
    const PushDecision push = retainer.push(bytes_of("hello"));
    EXPECT_TRUE(push.accepted);
    EXPECT_EQ(push.omitted.kind, OmittedKind::None);

    const RetainedText result = retainer.finish();
    EXPECT_EQ(result.text, "hello");
    EXPECT_EQ(result.omitted.kind, OmittedKind::None);
}

TEST(RetentionTextRetainer, HeadKeepsPrefixAndReportsExactOmission) {
    TextRetainer retainer(TextRetentionStrategy::Head, 4);
    const PushDecision push = retainer.push(bytes_of("abcdefgh"));
    EXPECT_EQ(push.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(push.omitted.count, 4u);

    const RetainedText result = retainer.finish();
    EXPECT_EQ(result.text, "abcd");
    EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(result.omitted.count, 4u);
}

TEST(RetentionTextRetainer, TailKeepsSuffixAndReportsExactOmission) {
    TextRetainer retainer(TextRetentionStrategy::Tail, 4);
    retainer.push(bytes_of("abcdefgh"));

    const RetainedText result = retainer.finish();
    EXPECT_EQ(result.text, "efgh");
    EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(result.omitted.count, 4u);
}

TEST(RetentionTextRetainer, HeadTailKeepsBothEndsAndReportsExactOmission) {
    TextRetainer retainer(TextRetentionStrategy::HeadTail, 4);
    retainer.push(bytes_of("abcdefgh"));

    const RetainedText result = retainer.finish();
    EXPECT_EQ(result.text, "abgh");
    EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(result.omitted.count, 4u);
}

TEST(RetentionTextRetainer, Utf8BoundaryIsNeverSplitOnHeadCut) {
    TextRetainer retainer(TextRetentionStrategy::Head, 2);
    retainer.push(bytes_of("a\xC3\xA9"));

    const RetainedText result = retainer.finish();
    EXPECT_EQ(result.text, "a");
    EXPECT_EQ(result.text.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(result.omitted.count, 2u);
}

TEST(RetentionTextRetainer, Utf8BoundaryIsNeverSplitOnTailCut) {
    TextRetainer retainer(TextRetentionStrategy::Tail, 2);
    retainer.push(bytes_of("\xC3\xA9z"));

    const RetainedText result = retainer.finish();
    EXPECT_EQ(result.text, "z");
    EXPECT_EQ(result.text.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
    EXPECT_EQ(result.omitted.count, 2u);
}

TEST(RetentionTextRetainer, ByteBudgetIsEscapingAwareForControlHeavyText) {
    const std::string payload(1000, '\x01');
    for (const TextRetentionStrategy strategy :
         {TextRetentionStrategy::Head, TextRetentionStrategy::Tail, TextRetentionStrategy::HeadTail}) {
        TextRetainer             retainer(strategy, 64);
        const PushDecision       push = retainer.push(bytes_of(payload));
        const RetainedText       result = retainer.finish();

        EXPECT_EQ(push.omitted.kind, OmittedKind::Exact);
        EXPECT_LE(escaped_width(result.text), 64u);
        EXPECT_EQ(result.omitted.kind, OmittedKind::Exact);
        EXPECT_EQ(result.omitted.count, 1000u - result.text.size());
    }
}

TEST(RetentionTextRetainer, FormatNoticeJoinsLibraryClauseAndRecoveryVerbatim) {
    RetentionNotice notice;
    notice.omitted        = Omitted{OmittedKind::Exact, 5};
    notice.omitted_labels = {"bytes"};
    const std::string recovery = "Re-run with a narrower pattern.";

    const std::string text = format_retention_notice(
        notice, [&recovery](const RetentionNotice&) { return recovery; });

    EXPECT_NE(text.find("Omitted 5 bytes."), std::string::npos);
    EXPECT_NE(text.find(recovery), std::string::npos);
}

TEST(RetentionTextRetainer, FormatNoticeWithoutOmissionIsRecoveryOnly) {
    const RetentionNotice notice;
    const std::string     recovery = "No recovery needed.";
    EXPECT_EQ(format_retention_notice(notice, [&recovery](const RetentionNotice&) {
                  return recovery;
              }),
              recovery);
}

TEST(RetentionTextRetainer, FormatNoticeUnknownDoesNotInventACount) {
    RetentionNotice notice;
    notice.omitted        = Omitted{OmittedKind::Unknown, 0};
    notice.omitted_labels = {"bytes"};

    const std::string text = format_retention_notice(notice, {});
    EXPECT_EQ(text.find("More bytes were omitted."), 0u);
    EXPECT_EQ(text.find('0'), std::string::npos);
}

TEST(RetentionToolResult, DefaultOmissionMetadata) {
    const payload::ToolResult result;
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(result.omitted_kind, OmittedKind::None);
    EXPECT_EQ(result.omitted_count, 0u);
}

TEST(RetentionToolResult, CodecRoundTripsExactOmission) {
    payload::ToolResult result;
    result.id           = "call-1";
    result.name         = "shell";
    result.output       = "kept";
    result.truncated    = true;
    result.omitted_kind = OmittedKind::Exact;
    result.omitted_count = 42;

    const nlohmann::json json = result;
    EXPECT_TRUE(json.at("truncated").get<bool>());
    EXPECT_EQ(json.at("omitted_kind").get<std::string>(), "exact");
    EXPECT_EQ(json.at("omitted_count").get<std::size_t>(), 42u);

    const payload::ToolResult back = json.get<payload::ToolResult>();
    EXPECT_EQ(back.omitted_kind, OmittedKind::Exact);
    EXPECT_EQ(back.omitted_count, 42u);
    EXPECT_TRUE(back.truncated);
}

TEST(RetentionToolResult, CodecRoundTripsUnknownOmission) {
    payload::ToolResult result;
    result.id           = "call-2";
    result.name         = "shell";
    result.truncated    = true;
    result.omitted_kind = OmittedKind::Unknown;

    const payload::ToolResult back = nlohmann::json(result).get<payload::ToolResult>();
    EXPECT_EQ(back.omitted_kind, OmittedKind::Unknown);
    EXPECT_EQ(back.omitted_count, 0u);
    EXPECT_TRUE(back.truncated);
}

TEST(RetentionToolResult, LegacyTruncatedWithoutKindMapsToUnknown) {
    nlohmann::json json = payload::ToolResult{};
    json.erase("omitted_kind");
    json.erase("omitted_count");
    json["truncated"] = true;

    const payload::ToolResult back = json.get<payload::ToolResult>();
    EXPECT_EQ(back.omitted_kind, OmittedKind::Unknown);
    EXPECT_TRUE(back.truncated);
}

TEST(RetentionToolResult, LegacyTruncatedSerializesAsUnknown) {
    payload::ToolResult result;
    result.id        = "call-3";
    result.name      = "shell";
    result.truncated = true;

    const nlohmann::json json = result;
    EXPECT_EQ(json.at("omitted_kind").get<std::string>(), "unknown");
    EXPECT_TRUE(json.at("truncated").get<bool>());

    const payload::ToolResult back = json.get<payload::ToolResult>();
    EXPECT_EQ(back.omitted_kind, OmittedKind::Unknown);
    EXPECT_TRUE(back.truncated);
}

TEST(RetentionToolResult, TruncatedIsDerivedFromOmittedKind) {
    nlohmann::json json = payload::ToolResult{};
    json["truncated"]    = false;
    json["omitted_kind"] = "exact";
    json["omitted_count"] = 3;

    const payload::ToolResult back = json.get<payload::ToolResult>();
    EXPECT_EQ(back.omitted_kind, OmittedKind::Exact);
    EXPECT_EQ(back.omitted_count, 3u);
    EXPECT_TRUE(back.truncated);
}

} // namespace

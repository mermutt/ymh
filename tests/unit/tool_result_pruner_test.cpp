#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/tool_result_pruner.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

struct PrunerFixture {
    EventBus                 bus;
    MemorySessionStore       store;
    SessionHeader            header;
    std::unique_ptr<Session> session;

    PrunerFixture() {
        header.id            = make_session_id();
        header.cwd           = "/tmp";
        header.model         = "fake-model";
        header.serverProfile = "interactive";
        header.kind          = SessionKind::Root;
        store.create(header);
        session = std::make_unique<Session>(header, store, bus);
    }

    Sequence append_result(std::string id, std::string output) {
        payload::ToolResult result;
        result.id          = std::move(id);
        result.name        = "read";
        result.output      = std::move(output);
        result.source.call = result.id;
        return session->append(result);
    }
};

std::optional<payload::ToolResult> latest_result(const EventRange& events,
                                                 const ToolCallId&  id) {
    std::optional<payload::ToolResult> found;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::ToolResult) {
            continue;
        }
        const auto& result = record.event.payload.get<payload::ToolResult>();
        if (result.id == id) {
            found = result;
        }
    }
    return found;
}

std::size_t count_results(const EventRange& events, const ToolCallId& id) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::ToolResult) {
            continue;
        }
        if (record.event.payload.get<payload::ToolResult>().id == id) {
            ++count;
        }
    }
    return count;
}

std::optional<EventRecord> prune_before_replacement(const EventRange& events,
                                                    const ToolCallId&  id) {
    for (std::size_t index = 0; index + 1 < events.size(); ++index) {
        if (events[index].event.type != EventType::ContextPrune) {
            continue;
        }
        const EventRecord& next = events[index + 1];
        if (next.event.type == EventType::ToolResult &&
            next.event.payload.get<payload::ToolResult>().id == id) {
            return events[index];
        }
    }
    return std::nullopt;
}

TEST(ToolResultPrunerTest, PrunesAboveThresholdInCodePointsNotBytes) {
    PrunerFixture fixture;
    fixture.append_result("under", std::string(8192, 'a'));
    std::string multibyte;
    for (int i = 0; i < 5000; ++i) {
        multibyte += "\xC3\xA9";
    }
    fixture.append_result("multi", multibyte);
    fixture.append_result("over", std::string(8193, 'a'));

    ToolResultPruner pruner;
    const PruneResult result = pruner.prune_session(*fixture.session);

    EXPECT_EQ(result.pruned, 1u);
    ASSERT_EQ(result.replacements.size(), 1u);

    const EventRange events = fixture.session->events();
    const auto       under  = latest_result(events, "under");
    const auto       multi  = latest_result(events, "multi");
    const auto       over   = latest_result(events, "over");
    ASSERT_TRUE(under.has_value());
    ASSERT_TRUE(multi.has_value());
    ASSERT_TRUE(over.has_value());
    EXPECT_EQ(under->output.find(kPruneMarker), std::string::npos);
    EXPECT_EQ(multi->output.find(kPruneMarker), std::string::npos);
    EXPECT_NE(over->output.find(kPruneMarker), std::string::npos);
}

TEST(ToolResultPrunerTest, ReplacementKeepsHeadAndTailCodePoints) {
    PrunerFixture fixture;
    const std::string output = std::string(4096, 'A') + std::string(5000, 'B') +
                               std::string(1024, 'C');
    fixture.append_result("over", output);

    ToolResultPruner pruner;
    ASSERT_EQ(pruner.prune_session(*fixture.session).pruned, 1u);

    const auto latest = latest_result(fixture.session->events(), "over");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->output, std::string(4096, 'A') + std::string{kPruneMarker} +
                                  std::string(1024, 'C'));
}

TEST(ToolResultPrunerTest, ContextPrunePrecedesSameIdReplacementAndOriginalRemains) {
    PrunerFixture fixture;
    const Sequence original_seq = fixture.append_result("over", std::string(9000, 'x'));

    ToolResultPruner pruner;
    ASSERT_EQ(pruner.prune_session(*fixture.session).pruned, 1u);

    const EventRange events = fixture.session->events();
    const auto       prune  = prune_before_replacement(events, "over");
    ASSERT_TRUE(prune.has_value());
    const auto& payload = prune->event.payload.get<payload::ContextPrune>();
    EXPECT_EQ(payload.shadowedStart, original_seq);
    EXPECT_EQ(payload.shadowedEnd, original_seq);
    EXPECT_EQ(payload.shadowedSeqs, (std::vector<Sequence>{original_seq}));
    EXPECT_GT(payload.shadowedTokenCount, 0u);

    EXPECT_EQ(count_results(events, "over"), 2u);
}

TEST(ToolResultPrunerTest, ProjectionReplacesByIdExactlyOneToolMessage) {
    PrunerFixture fixture;
    fixture.append_result("over", std::string(9000, 'x'));

    ToolResultPruner pruner;
    ASSERT_EQ(pruner.prune_session(*fixture.session).pruned, 1u);

    const std::vector<Message> messages = fixture.session->deriveMessages();
    std::size_t                tools    = 0;
    for (const Message& message : messages) {
        if (message.role != Role::Tool || message.tool_call_id != "over") {
            continue;
        }
        ++tools;
        ASSERT_FALSE(message.content.empty());
        EXPECT_NE(message.content.front().text.find(kPruneMarker), std::string::npos);
        EXPECT_EQ(message.content.front().text.size(), 4096u + kPruneMarker.size() + 1024u);
    }
    EXPECT_EQ(tools, 1u);
}

TEST(ToolResultPrunerTest, SecondPassIsIdempotent) {
    PrunerFixture fixture;
    fixture.append_result("over", std::string(9000, 'x'));

    ToolResultPruner pruner;
    ASSERT_EQ(pruner.prune_session(*fixture.session).pruned, 1u);
    const std::size_t events_after_first = fixture.session->events().size();

    const PruneResult second = pruner.prune_session(*fixture.session);
    EXPECT_EQ(second.pruned, 0u);
    EXPECT_TRUE(second.replacements.empty());
    EXPECT_EQ(fixture.session->events().size(), events_after_first);
}

TEST(ToolResultPrunerTest, ContextPruneJsonRoundTrips) {
    payload::ContextPrune original;
    original.shadowedStart      = 4;
    original.shadowedEnd        = 9;
    original.shadowedSeqs       = {4, 5, 9};
    original.shadowedTokenCount = 1234;

    const nlohmann::json json     = original;
    const auto           restored = json.get<payload::ContextPrune>();
    EXPECT_EQ(restored.shadowedStart, original.shadowedStart);
    EXPECT_EQ(restored.shadowedEnd, original.shadowedEnd);
    EXPECT_EQ(restored.shadowedSeqs, original.shadowedSeqs);
    EXPECT_EQ(restored.shadowedTokenCount, original.shadowedTokenCount);
}

TEST(ContextCompactionCodecTest, LegacyPayloadDecodesWithD13Defaults) {
    const nlohmann::json legacy = {
        {"boundary", 8},
        {"summary", "S"},
        {"token_estimate", 12},
        {"model", "m"},
        {"created_at", 0},
    };
    const auto restored = legacy.get<payload::ContextCompaction>();
    EXPECT_EQ(restored.boundary, 8);
    EXPECT_EQ(restored.summary, "S");
    EXPECT_TRUE(restored.provider.empty());
    EXPECT_EQ(restored.shadowedStart, 0);
    EXPECT_EQ(restored.shadowedEnd, 0);
    EXPECT_TRUE(restored.shadowedSeqs.empty());
    EXPECT_EQ(restored.shadowedTokenCount, 0u);
}

TEST(ContextCompactionCodecTest, FullD13PayloadRoundTrips) {
    payload::ContextCompaction original;
    original.boundary           = 8;
    original.summary            = "S";
    original.tokenEstimate      = 12;
    original.model              = "m";
    original.provider           = "p";
    original.shadowedStart      = 1;
    original.shadowedEnd        = 8;
    original.shadowedSeqs       = {1, 3, 8};
    original.shadowedTokenCount = 99;

    const nlohmann::json json     = original;
    const auto           restored = json.get<payload::ContextCompaction>();
    EXPECT_EQ(restored.provider, "p");
    EXPECT_EQ(restored.shadowedStart, 1);
    EXPECT_EQ(restored.shadowedEnd, 8);
    EXPECT_EQ(restored.shadowedSeqs, (std::vector<Sequence>{1, 3, 8}));
    EXPECT_EQ(restored.shadowedTokenCount, 99u);
}

} // namespace

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "test_support.hpp"

#include "ymh/agent/tool_result_pruner.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using ymh::EventRecord;
using ymh::EventType;
using ymh::PruneResult;
using ymh::Session;
using ymh::ToolResultPruneConfig;
using ymh::ToolResultPruner;
using ymh::payload::ToolOutcome;
using ymh::payload::ToolResult;

const std::string kMarker{ymh::kPruneMarker};

class ToolResultPrunerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = ymh::SessionPersistence::open(workspace_.persistence());
        id_    = ymh_test::fresh_uuid();
        store_->create(ymh_test::root_header(workspace_.root, id_));
        session_.emplace(ymh_test::root_header(workspace_.root, id_), *store_, bus_);
    }

    std::string append_result(std::string output, std::string name = "shell",
                              ToolOutcome outcome = ToolOutcome::Ok) {
        const std::string call_id = "call-" + std::to_string(next_call_++);
        ToolResult        result;
        result.id      = call_id;
        result.name    = std::move(name);
        result.outcome = outcome;
        result.output  = std::move(output);
        session_->append(result);
        return call_id;
    }

    [[nodiscard]] std::size_t count_events(EventType type) const {
        std::size_t count = 0;
        for (const EventRecord& record : session_->events()) {
            if (record.event.type == type) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::string latest_output(const std::string& call_id) const {
        std::string output;
        for (const EventRecord& record : session_->events()) {
            if (record.event.type != EventType::ToolResult) {
                continue;
            }
            const auto& result = record.event.payload.get<ToolResult>();
            if (result.id == call_id) {
                output = result.output;
            }
        }
        return output;
    }

    ymh_test::TempWorkspace                  workspace_;
    std::unique_ptr<ymh::SessionPersistence> store_;
    std::string                              id_;
    ymh::EventBus                            bus_;
    std::optional<Session>                   session_;
    std::size_t                              next_call_ = 0;
};

TEST_F(ToolResultPrunerTest, DefaultsAreGenerousAndBalanced) {
    const ToolResultPruneConfig defaults;
    EXPECT_EQ(defaults.threshold_code_points, 30000U);
    EXPECT_EQ(defaults.head_code_points, 15000U);
    EXPECT_EQ(defaults.tail_code_points, 15000U);
    EXPECT_TRUE(defaults.is_consistent());
}

TEST_F(ToolResultPrunerTest, ExactlyTheThresholdIsUntouched) {
    const std::string original(30000, 'x');
    const std::string call_id = append_result(original);

    ToolResultPruner pruner;
    EXPECT_EQ(pruner.prune_session(*session_).pruned, 0U);
    EXPECT_EQ(latest_output(call_id), original);
    EXPECT_EQ(count_events(EventType::ContextPrune), 0U);
}

TEST_F(ToolResultPrunerTest, ThresholdPlusOneIsPruned) {
    const std::string call_id = append_result(std::string(30001, 'x'));

    ToolResultPruner pruner;
    EXPECT_EQ(pruner.prune_session(*session_).pruned, 1U);
    EXPECT_EQ(count_events(EventType::ContextPrune), 1U);
}

TEST_F(ToolResultPrunerTest, PrunedResultKeepsExactHeadAndTail) {
    const std::string original = std::string(20000, 'h') + std::string(10001, 't');
    const std::string call_id  = append_result(original);

    ToolResultPruner pruner;
    ASSERT_EQ(pruner.prune_session(*session_).pruned, 1U);

    const std::string pruned = latest_output(call_id);
    const std::string expected =
        original.substr(0, 15000) + kMarker + original.substr(original.size() - 15000);
    EXPECT_EQ(pruned, expected);
    EXPECT_EQ(pruned.size(), std::size_t{15000} + kMarker.size() + 15000);
}

TEST_F(ToolResultPrunerTest, MultiByteUtf8SplitsOnCodePointBoundaries) {
    // 1-, 2- and 4-byte sequences: a α 🚀 b β 🚀 c (7 code points, 15 bytes).
    const std::string original = "aα🚀bβ🚀c";
    const std::string call_id  = append_result(original);

    ToolResultPruner pruner(ToolResultPruneConfig{6, 3, 3});
    ASSERT_EQ(pruner.prune_session(*session_).pruned, 1U);

    const std::string expected = std::string{"aα🚀"} + kMarker + "β🚀c";
    EXPECT_EQ(latest_output(call_id), expected);
}

TEST_F(ToolResultPrunerTest, SuccessfulSkillResultsAreExemptButFailedOnesArePruned) {
    const std::string ok_call =
        append_result("abcdefg", "skill", ToolOutcome::Ok);
    const std::string failed_call =
        append_result("abcdefg", "skill", ToolOutcome::Error);

    ToolResultPruner pruner(ToolResultPruneConfig{6, 3, 3});
    const PruneResult      result = pruner.prune_session(*session_);
    EXPECT_EQ(result.pruned, 1U);
    EXPECT_EQ(latest_output(ok_call), "abcdefg");
    EXPECT_EQ(latest_output(failed_call), std::string{"abc"} + kMarker + "efg");
}

TEST_F(ToolResultPrunerTest, PruningIsIdempotent) {
    append_result(std::string(30001, 'x'));

    ToolResultPruner pruner;
    EXPECT_EQ(pruner.prune_session(*session_).pruned, 1U);
    const std::size_t events_after_first = session_->events().size();

    EXPECT_EQ(pruner.prune_session(*session_).pruned, 0U);
    EXPECT_EQ(session_->events().size(), events_after_first);
    EXPECT_EQ(count_events(EventType::ContextPrune), 1U);
    EXPECT_EQ(count_events(EventType::ToolResult), 2U);
}

TEST_F(ToolResultPrunerTest, InconsistentConfigIsRejected) {
    // head + tail < threshold.
    EXPECT_THROW(ToolResultPruner(ToolResultPruneConfig{10, 4, 4}), std::invalid_argument);
    // head >= threshold.
    EXPECT_THROW(ToolResultPruner(ToolResultPruneConfig{10, 10, 5}), std::invalid_argument);
    // tail >= threshold.
    EXPECT_THROW(ToolResultPruner(ToolResultPruneConfig{10, 5, 10}), std::invalid_argument);
    EXPECT_NO_THROW(ToolResultPruner(ToolResultPruneConfig{10, 5, 5}));
}

} // namespace

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

#include "support/manual_clock.hpp"
#include "support/test_env.hpp"
#include "ymh/execution/deadline.hpp"
#include "ymh/tools/tool_context.hpp"

namespace {

using namespace ymh;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(Errata46Deadline, UI46_D8_ZeroDisables) {
    const Clock::time_point now = Clock::now();
    EXPECT_FALSE(clamp_timeout(std::nullopt, 0ms, now).has_value());
    EXPECT_FALSE(clamp_timeout(std::nullopt, 5000ms, now).has_value());

    ymh::test::ToolEnv env("d8_zero_disables");
    ToolContext disabled = env.context("call-1", 1, 1, std::nullopt);
    EXPECT_FALSE(disabled.has_deadline());
    EXPECT_EQ(disabled.remaining(), std::chrono::milliseconds::max());
    EXPECT_FALSE(disabled.expired());
}

TEST(Errata46Deadline, UI46_D8_ExpiredDeadlineIsNotZeroAsDisabled) {
    const Clock::time_point now = Clock::now();
    const Clock::time_point past = now - 1s;

    const std::optional<std::chrono::milliseconds> clamped =
        clamp_timeout(std::optional<Clock::time_point>{past}, 5000ms, now);
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(*clamped, 0ms);
    const std::optional<std::chrono::milliseconds> caller_zero =
        clamp_timeout(std::optional<Clock::time_point>{past}, 0ms, now);
    ASSERT_TRUE(caller_zero.has_value());
    EXPECT_EQ(*caller_zero, 0ms);

    ymh::test::ToolEnv env("d8_expired");
    ToolContext expired = env.context("call-1", 1, 1, past);
    EXPECT_TRUE(expired.has_deadline());
    EXPECT_TRUE(expired.expired());
    EXPECT_EQ(expired.remaining(), 0ms);
}

TEST(Errata46Deadline, ClampUsesCallerAndRemaining) {
    const Clock::time_point now = Clock::now();
    const Clock::time_point deadline = now + 500ms;

    EXPECT_EQ(*clamp_timeout(std::optional<Clock::time_point>{deadline}, 0ms, now), 500ms);
    EXPECT_EQ(*clamp_timeout(std::optional<Clock::time_point>{deadline}, 200ms, now), 200ms);
    EXPECT_EQ(*clamp_timeout(std::optional<Clock::time_point>{deadline}, 900ms, now), 500ms);
}

TEST(Errata46Deadline, ContextRemainingUsesTheInjectedClock) {
    ymh::test::ToolEnv env("d8_clock");
    auto clock = std::make_shared<ymh::test::ManualClock>();
    const Clock::time_point start = clock->now();

    ToolContext context =
        env.context("call-1", 1, 1, start + 250ms, [clock] { return clock->now(); });
    EXPECT_TRUE(context.has_deadline());
    EXPECT_FALSE(context.expired());
    EXPECT_EQ(context.remaining(), 250ms);

    clock->advance(300ms);
    EXPECT_TRUE(context.expired());
    EXPECT_EQ(context.remaining(), 0ms);
}

} // namespace

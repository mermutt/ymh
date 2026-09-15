#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <thread>

#include "ymh/agent/llm_pool.hpp"

namespace {

using namespace ymh;

TEST(LLMPool, RaiiReleaseAndExhaustion) {
    LLMPool pool(1);

    std::optional<LLMPool::Slot> first = pool.acquire(CancellationToken{}).get();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->held());

    CancellationSource cancelled;
    cancelled.cancel();
    std::optional<LLMPool::Slot> blocked = pool.acquire(cancelled.token()).get();
    EXPECT_FALSE(blocked.has_value());

    first.reset();
    std::optional<LLMPool::Slot> third = pool.acquire(CancellationToken{}).get();
    EXPECT_TRUE(third.has_value());
}

TEST(LLMPool, MoveTransfersTheSlot) {
    LLMPool pool(1);

    std::optional<LLMPool::Slot> first = pool.acquire(CancellationToken{}).get();
    ASSERT_TRUE(first.has_value());

    LLMPool::Slot moved = std::move(*first);
    first.reset();
    EXPECT_TRUE(moved.held());

    moved = LLMPool::Slot{};
    std::optional<LLMPool::Slot> again = pool.acquire(CancellationToken{}).get();
    EXPECT_TRUE(again.has_value());
}

TEST(LLMPool, CancellableAcquireReturnsNulloptWithoutLeaking) {
    LLMPool pool(1);

    std::optional<LLMPool::Slot> held = pool.acquire(CancellationToken{}).get();
    ASSERT_TRUE(held.has_value());

    CancellationSource           source;
    std::optional<LLMPool::Slot> waiter;
    std::thread thread([&]() { waiter = pool.acquire(source.token()).get(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    source.cancel();
    thread.join();

    EXPECT_FALSE(waiter.has_value());

    held.reset();
    std::optional<LLMPool::Slot> after = pool.acquire(CancellationToken{}).get();
    EXPECT_TRUE(after.has_value());
}

} // namespace

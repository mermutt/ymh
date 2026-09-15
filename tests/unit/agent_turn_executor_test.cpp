#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "ymh/agent/turn_executor.hpp"

namespace {

using namespace ymh;
using namespace std::chrono_literals;

// A one-shot gate so a test body can hold a worker until released.
class Gate {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }

    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    open_ = false;
};

// Polls a predicate for up to `timeout`; avoids a fixed sleep where possible.
template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(TurnExecutor, RunsBodyOffTheCallerThread) {
    TurnExecutor executor(1, 4);
    const std::thread::id caller = std::this_thread::get_id();

    std::atomic<bool>  ran{false};
    std::thread::id    runner;
    EXPECT_TRUE(executor.submit([&] {
        runner = std::this_thread::get_id();
        ran    = true;
    }));

    EXPECT_TRUE(waitFor([&] { return ran.load(); }));
    EXPECT_NE(runner, caller);
    executor.drain(1s);
}

TEST(TurnExecutor, RejectsOverflowAndDropsTheRejectedBody) {
    TurnExecutor executor(1, 2);
    Gate         gate;
    std::atomic<bool> first_entered{false};

    EXPECT_TRUE(executor.submit([&] {
        first_entered = true;
        gate.wait();
    }));
    ASSERT_TRUE(waitFor([&] { return first_entered.load(); }));

    std::atomic<int> ran{0};
    EXPECT_TRUE(executor.submit([&] { ++ran; }));
    EXPECT_TRUE(executor.submit([&] { ++ran; }));
    EXPECT_FALSE(executor.submit([&] { ++ran; }));

    gate.open();
    executor.drain(2s);

    EXPECT_EQ(ran.load(), 2);
    EXPECT_EQ(executor.inFlight(), 0u);
}

TEST(TurnExecutor, NeverRunsMoreBodiesThanWorkers) {
    TurnExecutor executor(2, 8);

    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};

    for (int index = 0; index < 8; ++index) {
        EXPECT_TRUE(executor.submit([&] {
            const int now = concurrent.fetch_add(1) + 1;
            int       observed = peak.load();
            while (now > observed && !peak.compare_exchange_weak(observed, now)) {
            }
            std::this_thread::sleep_for(2ms);
            concurrent.fetch_sub(1);
            ++done;
        }));
    }

    EXPECT_TRUE(waitFor([&] { return done.load() == 8; }));
    EXPECT_LE(peak.load(), 2);
    executor.drain(2s);
}

TEST(TurnExecutor, InFlightCountsQueuedPlusRunning) {
    TurnExecutor executor(1, 3);
    Gate         gate;
    std::atomic<bool> entered{false};

    EXPECT_TRUE(executor.submit([&] {
        entered = true;
        gate.wait();
    }));
    ASSERT_TRUE(waitFor([&] { return entered.load(); }));
    EXPECT_TRUE(executor.submit([] {}));
    EXPECT_TRUE(executor.submit([] {}));
    EXPECT_EQ(executor.inFlight(), 3u);

    gate.open();
    executor.drain(2s);
    EXPECT_EQ(executor.inFlight(), 0u);
}

TEST(TurnExecutor, DrainRunsQueuedBodiesAndStopsAccepting) {
    TurnExecutor executor(2, 4);
    std::atomic<int> ran{0};

    for (int index = 0; index < 4; ++index) {
        EXPECT_TRUE(executor.submit([&] { ++ran; }));
    }
    executor.drain(2s);

    EXPECT_EQ(ran.load(), 4);
    EXPECT_FALSE(executor.submit([&] { ++ran; }));
    EXPECT_EQ(ran.load(), 4);
}

TEST(TurnExecutor, ExposesTheConfiguredBounds) {
    TurnExecutor executor(4, 4);
    EXPECT_EQ(executor.workerCount(), 4u);
    EXPECT_EQ(executor.queueCapacity(), 4u);
    executor.drain(1s);
}

TEST(TurnExecutor, ZeroWorkerCapIsClampedToOne) {
    TurnExecutor executor(0, 1);
    EXPECT_EQ(executor.workerCount(), 1u);
    std::atomic<bool> ran{false};
    EXPECT_TRUE(executor.submit([&] { ran = true; }));
    EXPECT_TRUE(waitFor([&] { return ran.load(); }));
    executor.drain(1s);
}

} // namespace

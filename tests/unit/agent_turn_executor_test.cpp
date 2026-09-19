#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "ymh/agent/turn_executor.hpp"

namespace {

using namespace ymh;
using namespace std::chrono_literals;

const SessionId kSession{"turn-executor-session"};

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
    EXPECT_TRUE(executor.submit(kSession, [&] {
        runner = std::this_thread::get_id();
        ran    = true;
    }));

    EXPECT_TRUE(waitFor([&] { return ran.load(); }));
    EXPECT_NE(runner, caller);
    EXPECT_EQ(executor.drain(1s), TurnExecutor::DrainResult::Quiesced);
}

TEST(TurnExecutor, RejectsOverflowAndDropsTheRejectedBody) {
    TurnExecutor executor(1, 2);
    Gate         gate;
    std::atomic<bool> first_entered{false};

    EXPECT_TRUE(executor.submit(kSession, [&] {
        first_entered = true;
        gate.wait();
    }));
    ASSERT_TRUE(waitFor([&] { return first_entered.load(); }));

    std::atomic<int> ran{0};
    EXPECT_TRUE(executor.submit(kSession, [&] { ++ran; }));
    EXPECT_TRUE(executor.submit(kSession, [&] { ++ran; }));
    EXPECT_FALSE(executor.submit(kSession, [&] { ++ran; }));

    gate.open();
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);

    EXPECT_EQ(ran.load(), 2);
    EXPECT_EQ(executor.inFlight(), 0u);
}

TEST(TurnExecutor, NeverRunsMoreBodiesThanWorkers) {
    TurnExecutor executor(2, 8);

    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};

    for (int index = 0; index < 8; ++index) {
        EXPECT_TRUE(executor.submit(kSession, [&] {
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
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
}

TEST(TurnExecutor, InFlightCountsQueuedPlusRunning) {
    TurnExecutor executor(1, 3);
    Gate         gate;
    std::atomic<bool> entered{false};

    EXPECT_TRUE(executor.submit(kSession, [&] {
        entered = true;
        gate.wait();
    }));
    ASSERT_TRUE(waitFor([&] { return entered.load(); }));
    EXPECT_TRUE(executor.submit(kSession, [] {}));
    EXPECT_TRUE(executor.submit(kSession, [] {}));
    EXPECT_EQ(executor.inFlight(), 3u);

    gate.open();
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
    EXPECT_EQ(executor.inFlight(), 0u);
}

TEST(TurnExecutor, DrainRunsQueuedBodiesAndStopsAccepting) {
    TurnExecutor executor(2, 4);
    std::atomic<int> ran{0};

    for (int index = 0; index < 4; ++index) {
        EXPECT_TRUE(executor.submit(kSession, [&] { ++ran; }));
    }
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);

    EXPECT_EQ(ran.load(), 4);
    EXPECT_FALSE(executor.submit(kSession, [&] { ++ran; }));
    EXPECT_EQ(ran.load(), 4);
}

TEST(TurnExecutor, ExposesTheConfiguredBounds) {
    TurnExecutor executor(4, 4);
    EXPECT_EQ(executor.workerCount(), 4u);
    EXPECT_EQ(executor.queueCapacity(), 4u);
    EXPECT_EQ(executor.drain(1s), TurnExecutor::DrainResult::Quiesced);
}

TEST(TurnExecutor, ZeroWorkerCapIsClampedToOne) {
    TurnExecutor executor(0, 1);
    EXPECT_EQ(executor.workerCount(), 1u);
    std::atomic<bool> ran{false};
    EXPECT_TRUE(executor.submit(kSession, [&] { ran = true; }));
    EXPECT_TRUE(waitFor([&] { return ran.load(); }));
    EXPECT_EQ(executor.drain(1s), TurnExecutor::DrainResult::Quiesced);
}

// AL-U6/AL-U10: a quiesced drain joins every worker and reports Quiesced.
TEST(TurnExecutor, AL_U6_DrainQuiescesAfterBodiesFinish) {
    TurnExecutor     executor(2, 4);
    std::atomic<int> ran{0};
    for (int index = 0; index < 4; ++index) {
        ASSERT_TRUE(executor.submit(kSession, [&] { ++ran; }));
    }

    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
    EXPECT_EQ(ran.load(), 4);
    EXPECT_EQ(executor.inFlight(), 0u);
    // Destructor safety: a quiesced executor joins cleanly (no hard exit).
    EXPECT_FALSE(executor.submit(kSession, [] {}));
}

// AL-U7/AL10 (§4.2 in-process re-drain): a latch-blocked body yields TimedOut
// and the workers stay joinable; after release a second drain joins and
// returns Quiesced. The old drain detached on timeout and latched `drained_`,
// so it could never report TimedOut or re-join.
TEST(TurnExecutor, AL_U7_TimedOutThenQuiescedAfterRelease) {
    TurnExecutor      executor(1, 1);
    Gate              gate;
    std::atomic<bool> entered{false};
    ASSERT_TRUE(executor.submit(kSession, [&] {
        entered = true;
        gate.wait();
    }));
    ASSERT_TRUE(waitFor([&] { return entered.load(); }));

    EXPECT_EQ(executor.drain(1ms), TurnExecutor::DrainResult::TimedOut);

    gate.open();
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
    EXPECT_EQ(executor.inFlight(), 0u);
}

// AL-U17: drain idempotency — Quiesced stays Quiesced, and a re-drain after
// TimedOut eventually reports Quiesced once the workers exit.
TEST(TurnExecutor, AL_U17_DrainIsIdempotent) {
    TurnExecutor     executor(1, 1);
    std::atomic<int> ran{0};
    ASSERT_TRUE(executor.submit(kSession, [&] { ++ran; }));

    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
    EXPECT_EQ(ran.load(), 1);
}

// AL-U5/AL6/AL19: `inFlight(session)` counts queued + active per session (not
// globally), and the bounded queue still rejects overflow. Under the old
// session-blind `inFlight()` a queued body for one session was invisible.
TEST(TurnExecutor, AL_U5_InFlightIsPerSessionAndCapacityRejectsOverflow) {
    TurnExecutor    executor(1, 2);
    const SessionId session_a{"al-u5-a"};
    const SessionId session_b{"al-u5-b"};
    Gate            gate;
    std::atomic<bool> entered{false};

    ASSERT_TRUE(executor.submit(session_a, [&] {
        entered = true;
        gate.wait();
    }));
    ASSERT_TRUE(waitFor([&] { return entered.load(); }));

    EXPECT_TRUE(executor.submit(session_a, [] {}));
    EXPECT_TRUE(executor.submit(session_b, [] {}));
    EXPECT_EQ(executor.inFlight(session_a), 2u);
    EXPECT_EQ(executor.inFlight(session_b), 1u);
    EXPECT_EQ(executor.inFlight(), 3u);
    EXPECT_FALSE(executor.submit(session_b, [] {}));

    gate.open();
    EXPECT_EQ(executor.drain(2s), TurnExecutor::DrainResult::Quiesced);
    EXPECT_EQ(executor.inFlight(session_a), 0u);
    EXPECT_EQ(executor.inFlight(session_b), 0u);
}

// AL-S1/AL-I5/AL11/AL-F12: a `TimedOut` drain leaves the worker joinable, so
// the destructor must `std::_Exit(17)` (HostExitCode::Internal) instead of
// destroying live worker state or calling `detach`. The child proves the exit
// code; a returned destructor reaches `_Exit(99)` and a joinable-thread
// destructor aborts, so the assertion is non-vacuous.
TEST(TurnExecutor, AL_S1_TimedOutDrainHardExitsTheProcess) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        std::atomic<bool> entered{false};
        {
            TurnExecutor executor(1, 1);
            static_cast<void>(executor.submit(kSession, [&] {
                entered.store(true);
                for (;;) {
                    std::this_thread::sleep_for(10ms);
                }
            }));
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            static_cast<void>(executor.drain(1ms));
        }
        std::_Exit(99);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 17);
}

} // namespace

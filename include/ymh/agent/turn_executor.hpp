#pragma once

// Bounded worker-thread turn executor (11-m2-errata.md §3.2, D6; invariants
// E6/E7; failure mode M-F4).
//
// The daemon owns one Asio io_context per workspace (04 §9). The synchronous
// agent turn body (`AgentLoop::activate`) must never run on that io thread: a
// single tool call or LLM wait would stall accept, heartbeat, lease renewal,
// and every other session's event stream (M-F4). `TurnExecutor` marshals each
// turn body onto a bounded pool of worker threads and returns the RPC
// acknowledgement to the transport thread immediately.
//
// Backpressure is the queue, never a thread spawn: `submit()` enqueues and
// returns `false` when the bounded queue is full (the caller maps that to a
// typed `AgentErrorCode::InboxFull` / `RpcCode::InternalError`, E7). The pool
// size is `ResourceCaps::max_llm_concurrency` (04 §2.1) and the queue capacity
// defaults to the same value (errata §3.2).
//
// This class is daemon-internal (it lives in `WorkspaceHost::Impl`); it is
// header-only so the seam stays dependency-free and unit-testable without the
// daemon.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ymh/core/event.hpp"

namespace ymh {

class TurnExecutor {
public:
    // 24-D3: `drain` reports whether the join completed inside the grace.
    // `TimedOut` never means "detached": the workers stay joinable and the
    // coordinator performs the bounded hard exit (AL10/AL11).
    enum class DrainResult { Quiesced, TimedOut };

    // `max_workers` defaults to `ResourceCaps::max_llm_concurrency` (04 §2.1);
    // `queue_capacity` defaults to the same value (errata §3.2). `max_workers`
    // is clamped to at least one so a misconfigured cap can never wedge the
    // executor. The queue is the only backpressure: `submit()` never spawns a
    // thread.
    explicit TurnExecutor(std::size_t max_workers = 4, std::size_t queue_capacity = 4);
    // 24-D3/AL10: only reachable after `Quiesced`. A non-quiesced destructor
    // would destroy `workers_` while a worker still runs, so it hard-exits
    // instead (never `detach`, never `std::terminate` on a joinable thread).
    ~TurnExecutor();

    TurnExecutor(const TurnExecutor&) = delete;
    TurnExecutor& operator=(const TurnExecutor&) = delete;
    TurnExecutor(TurnExecutor&&) = delete;
    TurnExecutor& operator=(TurnExecutor&&) = delete;

    // Submit a turn body. Returns false when the bounded queue is full or the
    // executor is draining; in that case the body is not run and no thread is
    // spawned. The body runs at most once, on one of the worker threads.
    // 24-D4: the task carries its `SessionId`, so the queue is observable per
    // session (`inFlight(session)`).
    bool submit(const SessionId& session, std::function<void()> body);

    // Stop accepting, run/drain queued + in-flight bodies, and join every
    // worker within `grace`. NEVER detaches (AL10). Idempotent:
    //   * once `Quiesced`, every later call returns `Quiesced`;
    //   * after `TimedOut`, a later call retries the join and returns
    //     `Quiesced` once the workers have exited (the workers are still
    //     joinable).
    [[nodiscard]] DrainResult drain(std::chrono::milliseconds grace);

    // 24-D4: queued bodies plus bodies currently running on a worker for one
    // session, and in total.
    [[nodiscard]] std::size_t inFlight(const SessionId& session) const noexcept;
    [[nodiscard]] std::size_t inFlight() const noexcept;

    [[nodiscard]] std::size_t workerCount() const noexcept { return max_workers_; }
    [[nodiscard]] std::size_t queueCapacity() const noexcept { return queue_capacity_; }

private:
    // 24-D4: each queued body remembers the session that submitted it so the
    // daemon's pending predicate can be per-session.
    struct Task {
        SessionId             session;
        std::function<void()> body;
    };

    // Shared with the workers. It no longer exists to let a detached worker
    // outlive the executor (24-D3 removed detaching); it remains the queue's
    // accounting owner. 24-D4/finding 10: the per-session counters live only
    // here, never duplicated on `TurnExecutor`.
    struct State {
        std::mutex                                   mutex;
        std::condition_variable                      cv;
        std::deque<Task>                             queue;
        std::unordered_map<std::string, std::size_t> queued_by_session;  // under mutex
        std::unordered_map<std::string, std::size_t> active_by_session;  // under mutex
        std::size_t                                  active = 0;
        std::size_t                                  exited = 0;
        bool                                         stopping = false;
    };

    void workerLoop(const std::shared_ptr<State>& state);

    const std::size_t        max_workers_;
    const std::size_t        queue_capacity_;
    std::shared_ptr<State>   state_;
    std::vector<std::thread> workers_;
    bool                     quiesced_ = false;
};

inline TurnExecutor::TurnExecutor(std::size_t max_workers, std::size_t queue_capacity)
    : max_workers_(max_workers == 0 ? 1 : max_workers),
      queue_capacity_(queue_capacity),
      state_(std::make_shared<State>()) {
    workers_.reserve(max_workers_);
    for (std::size_t index = 0; index < max_workers_; ++index) {
        workers_.emplace_back([this, state = state_]() { workerLoop(state); });
    }
}

inline TurnExecutor::~TurnExecutor() {
    if (!quiesced_) {
        // HostExitCode::Internal (include/ymh/host/workspace_host.hpp). The
        // executor is a dependency-free agent seam and deliberately does not
        // include the daemon header, so the pinned numeric value is repeated.
        std::_Exit(17);
    }
}

inline bool TurnExecutor::submit(const SessionId& session, std::function<void()> body) {
    if (!body) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping || state_->queue.size() >= queue_capacity_) {
        return false;
    }
    state_->queue.push_back(Task{session, std::move(body)});
    ++state_->queued_by_session[session.value];
    state_->cv.notify_one();
    return true;
}

inline TurnExecutor::DrainResult TurnExecutor::drain(std::chrono::milliseconds grace) {
    if (quiesced_) {
        return DrainResult::Quiesced;
    }
    bool quiesced = false;
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        state_->cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + grace;
        quiesced = state_->cv.wait_until(lock, deadline, [this] {
            return state_->exited == max_workers_;
        });
    }
    if (!quiesced) {
        return DrainResult::TimedOut;
    }
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->queued_by_session.clear();
        state_->active_by_session.clear();
    }
    quiesced_ = true;
    return DrainResult::Quiesced;
}

inline std::size_t TurnExecutor::inFlight(const SessionId& session) const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto                  queued = state_->queued_by_session.find(session.value);
    const auto                  active = state_->active_by_session.find(session.value);
    return (queued == state_->queued_by_session.end() ? 0 : queued->second) +
           (active == state_->active_by_session.end() ? 0 : active->second);
}

inline std::size_t TurnExecutor::inFlight() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->queue.size() + state_->active;
}

inline void TurnExecutor::workerLoop(const std::shared_ptr<State>& state) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&state] { return state->stopping || !state->queue.empty(); });
            if (state->queue.empty()) {
                if (state->stopping) {
                    ++state->exited;
                    state->cv.notify_all();
                    return;
                }
                continue;
            }
            task = std::move(state->queue.front());
            state->queue.pop_front();
            if (const auto queued = state->queued_by_session.find(task.session.value);
                queued != state->queued_by_session.end()) {
                if (--queued->second == 0) {
                    state->queued_by_session.erase(queued);
                }
            }
            ++state->active;
            ++state->active_by_session[task.session.value];
        }

        task.body();

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->active;
            if (const auto active = state->active_by_session.find(task.session.value);
                active != state->active_by_session.end()) {
                if (--active->second == 0) {
                    state->active_by_session.erase(active);
                }
            }
            state->cv.notify_all();
        }
    }
}

} // namespace ymh

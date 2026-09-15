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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ymh {

class TurnExecutor {
public:
    // `max_workers` defaults to `ResourceCaps::max_llm_concurrency` (04 §2.1);
    // `queue_capacity` defaults to the same value (errata §3.2). `max_workers`
    // is clamped to at least one so a misconfigured cap can never wedge the
    // executor. The queue is the only backpressure: `submit()` never spawns a
    // thread.
    explicit TurnExecutor(std::size_t max_workers = 4, std::size_t queue_capacity = 4);
    ~TurnExecutor();

    TurnExecutor(const TurnExecutor&) = delete;
    TurnExecutor& operator=(const TurnExecutor&) = delete;
    TurnExecutor(TurnExecutor&&) = delete;
    TurnExecutor& operator=(TurnExecutor&&) = delete;

    // Submit a turn body. Returns false when the bounded queue is full or the
    // executor is draining; in that case the body is not run and no thread is
    // spawned. The body runs at most once, on one of the worker threads.
    bool submit(std::function<void()> body);

    // Stop accepting, drain queued + in-flight bodies, and join the workers
    // within `grace`. If a body is still running when `grace` expires the
    // workers are detached instead of joined; the shared state keeps a detached
    // worker safe after the executor is destroyed. Idempotent.
    void drain(std::chrono::milliseconds grace);

    // Queued bodies plus bodies currently running on a worker.
    [[nodiscard]] std::size_t inFlight() const noexcept;

    [[nodiscard]] std::size_t workerCount() const noexcept { return max_workers_; }
    [[nodiscard]] std::size_t queueCapacity() const noexcept { return queue_capacity_; }

private:
    // Shared with the workers so a detached worker can outlive the executor
    // without touching freed memory (see `drain`).
    struct State {
        std::mutex                       mutex;
        std::condition_variable          cv;
        std::deque<std::function<void()>> queue;
        std::size_t                      active = 0;
        std::size_t                      exited = 0;
        bool                             stopping = false;
    };

    void workerLoop(const std::shared_ptr<State>& state);

    const std::size_t        max_workers_;
    const std::size_t        queue_capacity_;
    std::shared_ptr<State>   state_;
    std::vector<std::thread> workers_;
    bool                     drained_ = false;
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
    if (!drained_) {
        drain(std::chrono::milliseconds{0});
    }
}

inline bool TurnExecutor::submit(std::function<void()> body) {
    if (!body) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping || state_->queue.size() >= queue_capacity_) {
        return false;
    }
    state_->queue.push_back(std::move(body));
    state_->cv.notify_one();
    return true;
}

inline void TurnExecutor::drain(std::chrono::milliseconds grace) {
    if (drained_) {
        return;
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
    for (std::thread& worker : workers_) {
        if (!worker.joinable()) {
            continue;
        }
        if (quiesced) {
            worker.join();
        } else {
            worker.detach();
        }
    }
    drained_ = true;
}

inline std::size_t TurnExecutor::inFlight() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->queue.size() + state_->active;
}

inline void TurnExecutor::workerLoop(const std::shared_ptr<State>& state) {
    for (;;) {
        std::function<void()> body;
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
            body = std::move(state->queue.front());
            state->queue.pop_front();
            ++state->active;
        }

        body();

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->active;
            state->cv.notify_all();
        }
    }
}

} // namespace ymh

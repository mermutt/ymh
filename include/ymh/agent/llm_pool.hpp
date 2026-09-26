#pragma once

// The daemon-wide bounded LLM worker pool, pinned by 06-agent-loop.md §5.9
// (deferred to this spec by 08 decision (r) and 04 §8). One instance per
// WorkspaceHost; every provider call (including compaction) is bracketed by one
// RAII `Slot` (A12). Acquire is FIFO and cancellable; no thread is spawned and
// no busy-wait is used.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"

namespace ymh {

class LLMPool {
public:
    class Slot {
    public:
        Slot() noexcept = default;
        Slot(Slot&& other) noexcept;
        Slot& operator=(Slot&& other) noexcept;
        ~Slot();

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        [[nodiscard]] bool held() const noexcept { return pool_ != nullptr; }

    private:
        friend class LLMPool;

        explicit Slot(LLMPool* pool) noexcept : pool_(pool) {}

        LLMPool* pool_ = nullptr;
    };

    explicit LLMPool(std::size_t maxConcurrency);
    ~LLMPool();

    LLMPool(const LLMPool&) = delete;
    LLMPool& operator=(const LLMPool&) = delete;

    // Completes with a slot when one is free, or with nullopt when `cancel`
    // fires first. Waiters are served in FIFO order.
    Task<std::optional<Slot>> acquire(CancellationToken cancel);

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    friend class Slot;

    void release() noexcept;

    const std::size_t       capacity_;
    std::size_t             inUse_ = 0;
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::deque<std::uint64_t> waiters_;
    std::uint64_t           nextTicket_ = 0;
};

} // namespace ymh

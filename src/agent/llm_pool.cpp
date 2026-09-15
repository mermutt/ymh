#include "ymh/agent/llm_pool.hpp"

#include <algorithm>
#include <utility>

namespace ymh {

LLMPool::Slot::Slot(Slot&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)) {}

LLMPool::Slot& LLMPool::Slot::operator=(Slot&& other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr) {
            pool_->release();
        }
        pool_ = std::exchange(other.pool_, nullptr);
    }
    return *this;
}

LLMPool::Slot::~Slot() {
    if (pool_ != nullptr) {
        pool_->release();
    }
}

LLMPool::LLMPool(std::size_t maxConcurrency) : capacity_(maxConcurrency) {}

LLMPool::~LLMPool() = default;

void LLMPool::release() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inUse_ > 0) {
            --inUse_;
        }
    }
    cv_.notify_all();
}

Task<std::optional<LLMPool::Slot>> LLMPool::acquire(CancellationToken cancel) {
    if (cancel.cancelled() || capacity_ == 0) {
        return Task<std::optional<Slot>>{std::optional<Slot>{}};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t ticket = nextTicket_++;
    waiters_.push_back(ticket);

    cancel.on_cancel([this]() { cv_.notify_all(); });

    cv_.wait(lock, [&]() {
        return cancel.cancelled() ||
               (inUse_ < capacity_ && !waiters_.empty() && waiters_.front() == ticket);
    });

    if (cancel.cancelled()) {
        const auto it = std::find(waiters_.begin(), waiters_.end(), ticket);
        if (it != waiters_.end()) {
            waiters_.erase(it);
        }
        lock.unlock();
        cv_.notify_all();
        return Task<std::optional<Slot>>{std::optional<Slot>{}};
    }

    waiters_.pop_front();
    ++inUse_;
    return Task<std::optional<Slot>>{std::optional<Slot>{Slot{this}}};
}

} // namespace ymh

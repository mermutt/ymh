#pragma once

// Cooperative cancellation (00-architecture.md §34). Every asynchronous
// operation accepts a CancellationToken; cancellation propagates from the TUI
// through the agent loop to LLM requests, tools, and subprocesses.
//
// CancellationSource owns the state and fires it once; CancellationToken is a
// cheap copyable observer of that state.

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ymh {

class CancellationError : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "operation cancelled";
    }
};

namespace detail {

struct CancellationState {
    std::atomic<bool>                   cancelled{false};
    std::mutex                          mutex;
    std::vector<std::function<void()>>  callbacks;
};

} // namespace detail

class CancellationToken {
public:
    // A default token is never cancelled; on_cancel on it is a no-op.
    CancellationToken() noexcept = default;

    [[nodiscard]] bool cancelled() const noexcept {
        return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled(); }

    // Invokes `callback` on cancellation. If already cancelled, invokes it
    // immediately on the calling thread.
    void on_cancel(std::function<void()> callback) const {
        if (state_ == nullptr) {
            return;
        }

        bool invokeNow = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled.load(std::memory_order_relaxed)) {
                invokeNow = true;
            } else {
                state_->callbacks.push_back(std::move(callback));
            }
        }

        if (invokeNow) {
            callback();
        }
    }

    void throw_if_cancelled() const {
        if (cancelled()) {
            throw CancellationError{};
        }
    }

private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<detail::CancellationState>()) {}

    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) noexcept = default;
    CancellationSource& operator=(CancellationSource&&) noexcept = default;

    ~CancellationSource() = default;

    // Idempotent; fires every registered callback exactly once.
    void cancel() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            callbacks.swap(state_->callbacks);
        }

        for (const auto& callback : callbacks) {
            callback();
        }
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken{state_};
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

} // namespace ymh

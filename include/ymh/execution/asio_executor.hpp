#pragma once

// Concrete `Executor` over the daemon's single `asio::io_context` (14 §5.1,
// E-P3). Owned by 04's daemon (`WorkspaceHost`); the execution layer sees only
// the Asio-free `Executor` interface.

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

#include <asio.hpp>

#include "ymh/execution/executor.hpp"

namespace ymh {

class AsioExecutor final : public Executor {
public:
    explicit AsioExecutor(asio::io_context& io) noexcept : io_(io) {}

    bool post(std::function<void()> fn) override {
        if (io_.stopped()) {
            return false;
        }
        asio::post(io_, std::move(fn));
        return true;
    }

    [[nodiscard]] bool stopped() const noexcept override { return io_.stopped(); }

    [[nodiscard]] bool onLoopThread() const noexcept override {
        return std::this_thread::get_id() ==
               runner_.load(std::memory_order_acquire);
    }

    void bindRunnerThread(std::thread::id id) noexcept {
        runner_.store(id, std::memory_order_release);
    }

    [[nodiscard]] asio::io_context& io() noexcept { return io_; }

private:
    asio::io_context&            io_;
    std::atomic<std::thread::id> runner_{};
};

} // namespace ymh

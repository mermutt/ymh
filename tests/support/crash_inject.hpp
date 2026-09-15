#pragma once

// Crash-injection helpers for the two-process harness. A daemon crash is
// simulated by signalling its process group (the harness places the child in
// its own group), then reaping; `crash_during` runs a blocking action on a
// worker thread and crashes the child mid-flight so a test can exercise the
// recovery window (stale claim, orphaned socket).

#include <chrono>
#include <csignal>
#include <exception>
#include <thread>
#include <utility>

#include "support/child_process.hpp"

namespace ymh::test {

inline ExitStatus crash_now(ChildProcess& child, int signal = SIGKILL) {
    child.signal(signal);
    if (std::optional<ExitStatus> status = child.wait_for(std::chrono::seconds{5});
        status.has_value()) {
        return *status;
    }
    return child.terminate();
}

template <typename Action>
ExitStatus crash_during(ChildProcess& child,
                        Action&&      action,
                        std::chrono::milliseconds delay = std::chrono::milliseconds{50},
                        int           signal = SIGKILL) {
    std::exception_ptr failure;
    std::thread        worker([&action, &failure] {
        try {
            std::forward<Action>(action)();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    std::this_thread::sleep_for(delay);
    const ExitStatus status = crash_now(child, signal);
    worker.join();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return status;
}

} // namespace ymh::test

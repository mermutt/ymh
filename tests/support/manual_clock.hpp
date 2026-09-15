#pragma once

// Deterministic clock for timeout tests. Components that accept a callable
// time source (`std::function<time_point()>`) can be driven without sleeping;
// `advance()` moves virtual time and `reader()` hands out a snapshot callable.

#include <chrono>
#include <functional>

namespace ymh::test {

class ManualClock {
public:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration   = clock::duration;

    ManualClock() = default;
    explicit ManualClock(time_point start) : now_(start) {}

    [[nodiscard]] time_point now() const noexcept { return now_; }

    void advance(duration delta) noexcept { now_ += delta; }

    void advance(std::chrono::milliseconds delta) noexcept {
        now_ += std::chrono::duration_cast<duration>(delta);
    }

    void set(time_point value) noexcept { now_ = value; }

    [[nodiscard]] std::function<time_point()> reader() const {
        return [*this] { return now_; };
    }

private:
    time_point now_{};
};

} // namespace ymh::test

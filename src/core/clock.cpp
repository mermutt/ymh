#include "ymh/core/clock.hpp"

namespace ymh {

std::int64_t epoch_ms(std::chrono::system_clock::time_point point) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}

MonotonicClock default_monotonic_clock() {
    return [] { return std::chrono::steady_clock::now(); };
}

WallClockReader default_wall_clock() {
    return [] { return std::chrono::system_clock::now(); };
}

} // namespace ymh

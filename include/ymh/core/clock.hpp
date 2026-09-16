#pragma once

// The two-clock seam (16-D13; O-H1/C-M1).
//
// Ownership liveness needs both clocks and must never mix them:
//   * MonotonicClock (std::chrono::steady_clock) — local interval math ONLY
//     (frame age, watchdog grace, timeouts). It has no epoch.
//   * WallClockReader (std::chrono::system_clock) — durable heartbeat and
//     freshness ONLY, compared as epoch milliseconds.
//
// Subtracting a steady_clock value from an epoch-ms field yields a huge
// negative delta; the fail-safe direction is "not fresh", never a persistent
// phantom owner (§2.4). Cf. `WallClock` in include/ymh/agent/compactor.hpp:106;
// same shape, shared here for the daemon and the supervisor.

#include <chrono>
#include <cstdint>
#include <functional>

namespace ymh {

// Monotonic: local interval math ONLY (frame age, grace, timeouts).
using MonotonicClock = std::function<std::chrono::steady_clock::time_point()>;

// Wall: durable heartbeat/freshness ONLY (epoch ms).
using WallClockReader = std::function<std::chrono::system_clock::time_point()>;

[[nodiscard]] std::int64_t epoch_ms(std::chrono::system_clock::time_point point) noexcept;

[[nodiscard]] MonotonicClock default_monotonic_clock();  // steady_clock::now
[[nodiscard]] WallClockReader default_wall_clock();      // system_clock::now

} // namespace ymh

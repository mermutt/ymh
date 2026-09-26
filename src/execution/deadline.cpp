#include "ymh/execution/deadline.hpp"

#include <algorithm>

namespace ymh {

std::optional<std::chrono::milliseconds>
clamp_timeout(const std::optional<std::chrono::steady_clock::time_point>& deadline,
              std::chrono::milliseconds caller_ms,
              std::chrono::steady_clock::time_point now) {
    if (!deadline.has_value()) {
        return std::nullopt;
    }
    if (now >= *deadline) {
        // Expired: an explicit 0ms, never nullopt (which would read as
        // "disabled" at every shipped primitive).
        return std::chrono::milliseconds{0};
    }
    const std::chrono::milliseconds remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    if (remaining.count() == 0) {
        // Sub-millisecond remainder rounds down to expired.
        return std::chrono::milliseconds{0};
    }
    if (caller_ms.count() == 0) {
        return remaining;
    }
    return std::min(caller_ms, remaining);
}

} // namespace ymh

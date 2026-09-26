#include "ymh/registry/supervisor.hpp"

namespace ymh {

bool isFresh(const SupervisorRow& row, std::int64_t now_wall_ms,
             std::chrono::milliseconds ttl) noexcept {
    const std::int64_t delta = now_wall_ms - row.heartbeatMs;
    return delta >= 0 && delta <= ttl.count();
}

} // namespace ymh

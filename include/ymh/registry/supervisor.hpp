#pragma once

// The ownership set (16 §2.3, §7.1): one row per live supervisor process.
//
// The registry declares its own mirror of the transport's `ClientInstanceId`
// to avoid a registry → transport dependency (the same mirroring convention
// protocol.hpp:10-14 uses). A supervisor's durable identity is minted per
// process (16-D8) and is wire-identical UUIDv4 text.

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace ymh {

// Wire-identical to protocol::ClientInstanceId (UUIDv4 text). A supervisor's
// durable identity, minted per process (16-D8).
struct SupervisorId {
    std::string value;

    auto operator<=>(const SupervisorId&) const = default;
};

// One row of the `supervisors` ownership set (§2.3).
struct SupervisorRow {
    SupervisorId               id;
    std::int32_t               pid{0};
    std::string                bootId;         // per-process nonce (UUIDv4)
    std::int64_t               startedAtMs{0};
    std::int64_t               heartbeatMs{0};  // WALL epoch ms (system_clock)
    std::optional<std::string> tty;             // display only
};

// FRESH_OWNER predicate (16-D13, O-H1/C-M1). `now_wall_ms` is WALL epoch ms
// (system_clock), never a steady_clock value. Pinned:
//   delta := now_wall_ms - row.heartbeatMs;
//   return delta >= 0 && delta <= ttl;          // delta < 0 => STALE (fail-safe)
[[nodiscard]] bool isFresh(const SupervisorRow& row, std::int64_t now_wall_ms,
                           std::chrono::milliseconds ttl) noexcept;

} // namespace ymh

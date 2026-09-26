#pragma once

// The single source for the ownership cadence constants (N2-L7).
//
// Both `HostConfig` (§7.3) and `SupervisorPresence::Options` (§7.5) default
// from these, so they cannot drift. The static_assert pins the lease/heartbeat
// ratio (O-L7): a lease shorter than three missed heartbeats would let a
// healthy-but-slow supervisor be declared stale.

#include <chrono>

namespace ymh {

inline constexpr std::chrono::milliseconds kOwnerHeartbeatInterval{5'000};
inline constexpr std::chrono::milliseconds kOwnerLeaseTtl{15'000};
inline constexpr std::chrono::milliseconds kOwnerWatchdogInterval{2'000};
inline constexpr std::chrono::milliseconds kOwnerGrace{5'000};

static_assert(kOwnerLeaseTtl >= 3 * kOwnerHeartbeatInterval,
              "owner_lease_ttl must cover three missed heartbeats");

} // namespace ymh

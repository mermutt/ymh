#pragma once

// 89 (rev 3) L2 kernel confinement, reshaped by 119 Wave 1: the Landlock
// backend behind the `SandboxProvider` seam. `SandboxMode`/`ChildConfinement`
// live in `sandbox.hpp`; this header keeps the ABI probe, the rights contract,
// and the in-child application. On a host without Landlock, `probe()` is
// `Unavailable` and the provider refuses a confining mode.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "ymh/execution/sandbox.hpp"

namespace ymh {

// `Enforced` => the ruleset can be applied (Landlock ABI >= 3, which has
// `FS_TRUNCATE`); `Unavailable` => no Landlock or an ABI too old to bound
// writes; `NotApplicable` => the configured mode is `DangerFullAccess`.
enum class ConfinementStatus : std::uint8_t {
    Enforced,
    Unavailable,
    NotApplicable,
};

class KernelConfinement {
public:
    virtual ~KernelConfinement() = default;

    // ABI/rights probe. Cheap and side-effect-free (a version query only).
    [[nodiscard]] virtual ConfinementStatus probe() const = 0;

    // The probed ABI (a negative `-errno` when unavailable), cached after the
    // first probe. The provider needs it for the handled-rights mask.
    [[nodiscard]] virtual long long abi() const = 0;

    // Called in the CHILD between fork and exec. On ANY failure it writes a
    // status byte (when armed) and `_exit(127)`s -- it never returns to run the
    // command unconfined (SG17/SG-F24).
    virtual void apply_or_abort(const ChildConfinement&) = 0;
};

// Landlock FS access-right bits (linux/landlock.h). Mirrored here so the rights
// contract is testable without pulling the kernel header into every TU.
// 119-D43: the fixed pre-exec status record the child writes on the status
// pipe (`kind` = exec failure or confinement refusal). A command cannot forge
// it: only the pre-exec path writes it, and the pipe is close-on-exec.
inline constexpr std::uint8_t kExecStatusExec     = 0;
inline constexpr std::uint8_t kExecStatusRefused  = 1;

struct ExecStatusRecord {
    std::uint8_t kind        = kExecStatusExec;
    std::int32_t errno_value = 0;
};

inline constexpr std::uint64_t kLandlockFsRefer    = 1ULL << 13;
inline constexpr std::uint64_t kLandlockFsTruncate = 1ULL << 14;
inline constexpr std::uint64_t kLandlockFsIoctlDev = 1ULL << 15;

// 119 §5.2: the FS rights the ruleset handles for `abi`. `FS_TRUNCATE` requires
// ABI >= 3 and `FS_IOCTL_DEV` requires ABI >= 5; a partial ABI clears the
// unhandled class (otherwise `create_ruleset` rejects the whole mask).
[[nodiscard]] std::uint64_t confinement_handled_rights(long long abi) noexcept;

// 119 §5.2: map the raw `landlock_create_ruleset(VERSION)` result to a status.
// `< 0` (`ENOSYS`/`EPERM`/...) or ABI < 3 => `Unavailable`; otherwise
// `Enforced` (the provider reports `Full` at ABI >= 5 and `Partial` below it).
[[nodiscard]] ConfinementStatus classify_landlock_abi(long long abi_or_errno) noexcept;

// The process-wide, stateless kernel-confinement implementation (Landlock).
[[nodiscard]] KernelConfinement& kernel_confinement();

// Section 5.5: the child arms the status-pipe write end before `apply_or_abort`
// so a confinement failure can signal the parent (`_exit(127)` + one byte).
// Child-only (set after fork); `-1` disables the byte.
void set_confinement_status_fd(int fd) noexcept;

} // namespace ymh

#pragma once

// 89 (rev 3) L2 kernel confinement. The real boundary behind the L1 advisory
// classifier: a Landlock ruleset applied in the `{shell,terminal}` child between
// fork and exec, inherited across `execve`, mediating **resolved** paths. It
// defeats the interpreter, obfuscation, symlink, and TOCTOU cases L1 cannot
// parse. `Allow` requires `L1.None` AND L2 on (`L1.None => L2.on`, 89-D14).
//
// This header is portable: the concrete `LandlockConfinement` lives in
// `src/execution/landlock.cpp` behind guarded kernel includes. On a host without
// Landlock, `probe()` is `Unavailable` and the coupling fails to ASK.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace ymh {

// 89-D17/89-D14. `Enforced` => L2 on (the child cannot undo the ruleset);
// `Unavailable` => the coupling (89-D14) promotes a recognized destructive
// in-root call to `Confirm`, never `Allow`; `NotApplicable` => the configured
// sandbox is `Unrestricted` (the `danger-full-access` disposition, 89-D15).
enum class ConfinementStatus : std::uint8_t {
    Enforced,
    Unavailable,
    NotApplicable,
};

// 89-D17: the per-spawn allow-list. `root` is the workspace root
// (RW+MAKE+REMOVE+EXEC+REFER); `home`/`tmpdir` are throwaway dirs; `ro_paths`
// grant READ+EXECUTE only. Landlock grants nothing not listed.
struct ConfinementPolicy {
    std::filesystem::path                root;
    std::optional<std::filesystem::path> home;
    std::optional<std::filesystem::path> tmpdir;
    std::vector<std::filesystem::path>   ro_paths;
};

// 89-D17: probed once per environment, applied once per spawn in the CHILD.
class KernelConfinement {
public:
    virtual ~KernelConfinement() = default;

    // ABI/rights probe. Cheap and side-effect-free (a version query only).
    [[nodiscard]] virtual ConfinementStatus probe() const = 0;

    // Called in the CHILD between fork and exec. On ANY failure it writes a
    // status byte (when armed) and `_exit(127)`s -- it never returns to run the
    // command unconfined (SG17/SG-F24).
    virtual void apply_or_abort(const ConfinementPolicy&) = 0;
};

// Landlock FS access-right bits (linux/landlock.h). Mirrored here so the rights
// contract is testable without pulling the kernel header into every TU.
inline constexpr std::uint64_t kLandlockFsRefer    = 1ULL << 13;
inline constexpr std::uint64_t kLandlockFsTruncate = 1ULL << 14;
inline constexpr std::uint64_t kLandlockFsIoctlDev = 1ULL << 15;

// Section 5.4: the FS rights the ruleset handles. `FS_TRUNCATE` requires
// Landlock filesystem ABI >= 3 (kernel >= 6.2) and `FS_IOCTL_DEV` requires
// ABI >= 5; both must be present or L2 is `Unavailable`.
[[nodiscard]] std::uint64_t confinement_handled_rights() noexcept;

// Section 5.4: map the raw `landlock_create_ruleset(VERSION)` result to a
// status. `< 0` (an `-errno`: `ENOSYS`/`EPERM`/...) => `Unavailable`; ABI < 5 =>
// `Unavailable` (needs `FS_TRUNCATE`/ABI 3 and `FS_IOCTL_DEV`/ABI 5); otherwise
// `Enforced`. Exposed so the hermetic probe test can exercise every branch.
[[nodiscard]] ConfinementStatus classify_landlock_abi(long long abi_or_errno) noexcept;

// The process-wide, stateless kernel-confinement implementation (Landlock).
// Probed once per `LocalEnvironment`; applied once per spawn in the child.
[[nodiscard]] KernelConfinement& kernel_confinement();

// Section 5.5: the child arms the status-pipe write end before `apply_or_abort`
// so a confinement failure can signal the parent (`_exit(127)` + one byte).
// Child-only (set after fork); `-1` disables the byte.
void set_confinement_status_fd(int fd) noexcept;

// 89 §5.7/89-A11 (L2 clamp floor): a fresh per-spawn throwaway `HOME`/`TMPDIR`
// under a unique directory, plus the `ConfinementPolicy` that names them. The
// dirs are created at construction and removed at destruction (state lifetime:
// the tool call). `policy().root` is the workspace root; `ro_paths` is `{ "/" }`.
class SpawnConfinement {
public:
    explicit SpawnConfinement(const std::filesystem::path& workspace_root);
    ~SpawnConfinement();

    SpawnConfinement(SpawnConfinement&&) noexcept;
    SpawnConfinement& operator=(SpawnConfinement&&) noexcept;
    SpawnConfinement(const SpawnConfinement&) = delete;
    SpawnConfinement& operator=(const SpawnConfinement&) = delete;

    [[nodiscard]] const ConfinementPolicy&     policy() const noexcept { return policy_; }
    [[nodiscard]] const std::filesystem::path& home() const noexcept { return home_; }
    [[nodiscard]] const std::filesystem::path& tmpdir() const noexcept { return tmp_; }

private:
    void cleanup() noexcept;

    ConfinementPolicy     policy_;
    std::filesystem::path base_;
    std::filesystem::path home_;
    std::filesystem::path tmp_;
};

} // namespace ymh

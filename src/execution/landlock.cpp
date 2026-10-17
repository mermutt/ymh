#include "ymh/execution/confinement.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>

#if defined(__linux__) && __has_include(<linux/landlock.h>)
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#define YMH_LANDLOCK_HEADERS 1
#endif

#if defined(YMH_LANDLOCK_HEADERS) && defined(__NR_landlock_create_ruleset) && \
    defined(__NR_landlock_add_rule) && defined(__NR_landlock_restrict_self)
#define YMH_HAS_LANDLOCK 1
#endif

namespace ymh {
namespace {

constexpr std::uint64_t kFsExecute    = 1ULL << 0;
constexpr std::uint64_t kFsWriteFile  = 1ULL << 1;
constexpr std::uint64_t kFsReadFile   = 1ULL << 2;
constexpr std::uint64_t kFsReadDir    = 1ULL << 3;
constexpr std::uint64_t kFsRemoveDir  = 1ULL << 4;
constexpr std::uint64_t kFsRemoveFile = 1ULL << 5;
constexpr std::uint64_t kFsMakeChar   = 1ULL << 6;
constexpr std::uint64_t kFsMakeDir    = 1ULL << 7;
constexpr std::uint64_t kFsMakeReg    = 1ULL << 8;
constexpr std::uint64_t kFsMakeSock   = 1ULL << 9;
constexpr std::uint64_t kFsMakeFifo   = 1ULL << 10;
constexpr std::uint64_t kFsRefer      = 1ULL << 13;
constexpr std::uint64_t kFsTruncate   = 1ULL << 14;
constexpr std::uint64_t kFsIoctlDev   = 1ULL << 15;

constexpr std::uint64_t kReadRights = kFsExecute | kFsReadFile | kFsReadDir;
constexpr std::uint64_t kWriteRights = kFsWriteFile | kFsTruncate;
constexpr std::uint64_t kMakeRights =
    kFsMakeChar | kFsMakeDir | kFsMakeReg | kFsMakeSock | kFsMakeFifo;
constexpr std::uint64_t kRemoveRights = kFsRemoveFile | kFsRemoveDir;

constexpr std::uint64_t kRootRights =
    kReadRights | kWriteRights | kMakeRights | kRemoveRights | kFsRefer;
constexpr std::uint64_t kHomeRights = kReadRights | kWriteRights | kMakeRights | kRemoveRights;
constexpr std::uint64_t kTmpRights  = kReadRights | kWriteRights | kMakeRights | kRemoveRights;

// `FS_IOCTL_DEV` is handled but granted to no path, so a device `ioctl` outside
// the workspace is denied at the kernel boundary (the `dd`/`mkfs` class).
constexpr std::uint64_t kHandledRights = kRootRights | kFsIoctlDev;

int g_status_fd = -1;

[[noreturn]] void confine_fail(int err) {
    if (g_status_fd >= 0) {
        const char byte = static_cast<char>(err == 0 ? EACCES : err);
        (void)!::write(g_status_fd, &byte, 1);
    }
    ::_exit(127);
}

#if defined(YMH_HAS_LANDLOCK)

void add_path_rule(int ruleset_fd, const std::filesystem::path& path, std::uint64_t access) {
    const std::string           native = path.string();
    const int                   parent = ::open(native.c_str(), O_PATH | O_CLOEXEC);
    if (parent < 0) {
        confine_fail(errno);
    }
    landlock_path_beneath_attr attr{};
    attr.allowed_access = access;
    attr.parent_fd      = parent;
    const int rc    = static_cast<int>(::syscall(__NR_landlock_add_rule, ruleset_fd,
                                                 LANDLOCK_RULE_PATH_BENEATH, &attr, 0));
    const int saved = errno;
    ::close(parent);
    if (rc != 0) {
        confine_fail(saved);
    }
}

class LandlockConfinement final : public KernelConfinement {
public:
    [[nodiscard]] ConfinementStatus probe() const noexcept override {
        const long abi = static_cast<long>(
            ::syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
        if (abi < 0) {
            return ConfinementStatus::Unavailable;
        }
        return classify_landlock_abi(static_cast<long long>(abi));
    }

    void apply_or_abort(const ConfinementPolicy& policy) override {
        landlock_ruleset_attr attr{};
        attr.handled_access_fs = kHandledRights;
        const int ruleset_fd   = static_cast<int>(
            ::syscall(__NR_landlock_create_ruleset, &attr, sizeof(attr), 0));
        if (ruleset_fd < 0) {
            confine_fail(errno);
        }

        add_path_rule(ruleset_fd, policy.root, kRootRights);
        if (policy.home.has_value()) {
            add_path_rule(ruleset_fd, *policy.home, kHomeRights);
        }
        if (policy.tmpdir.has_value()) {
            add_path_rule(ruleset_fd, *policy.tmpdir, kTmpRights);
        }
        for (const std::filesystem::path& ro : policy.ro_paths) {
            add_path_rule(ruleset_fd, ro, kReadRights);
        }

        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
            const int saved = errno;
            ::close(ruleset_fd);
            confine_fail(saved);
        }
        if (static_cast<int>(::syscall(__NR_landlock_restrict_self, ruleset_fd, 0)) != 0) {
            const int saved = errno;
            ::close(ruleset_fd);
            confine_fail(saved);
        }
        ::close(ruleset_fd);
    }
};

#else  // no Landlock headers / syscall numbers at build time

class LandlockConfinement final : public KernelConfinement {
public:
    [[nodiscard]] ConfinementStatus probe() const noexcept override {
        return ConfinementStatus::Unavailable;
    }

    void apply_or_abort(const ConfinementPolicy& policy) override {
        (void)policy;
        confine_fail(ENOSYS);
    }
};

#endif

} // namespace

std::uint64_t confinement_handled_rights() noexcept { return kHandledRights; }

ConfinementStatus classify_landlock_abi(long long abi_or_errno) noexcept {
    if (abi_or_errno < 0) {
        return ConfinementStatus::Unavailable;
    }
    if (abi_or_errno < 5) {
        return ConfinementStatus::Unavailable;
    }
    return ConfinementStatus::Enforced;
}

KernelConfinement& kernel_confinement() {
    static LandlockConfinement instance;
    return instance;
}

void set_confinement_status_fd(int fd) noexcept { g_status_fd = fd; }

} // namespace ymh

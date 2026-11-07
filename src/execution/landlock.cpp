#include "ymh/execution/confinement.hpp"

#include <cerrno>
#include <limits>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <string>

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
// A non-directory rule (`/dev/null`, 118-D2) may not carry `READ_DIR`: the
// kernel rejects the whole ruleset with EINVAL, so the file rule is explicit.
constexpr std::uint64_t kDeviceRights = kFsReadFile | kFsWriteFile | kFsTruncate;
constexpr std::uint64_t kMakeRights =
    kFsMakeChar | kFsMakeDir | kFsMakeReg | kFsMakeSock | kFsMakeFifo;
constexpr std::uint64_t kRemoveRights = kFsRemoveFile | kFsRemoveDir;

constexpr std::uint64_t kRootRights =
    kReadRights | kWriteRights | kMakeRights | kRemoveRights | kFsRefer;
constexpr std::uint64_t kHomeRights = kReadRights | kWriteRights | kMakeRights | kRemoveRights;
constexpr std::uint64_t kTmpRights =
    kReadRights | kWriteRights | kMakeRights | kRemoveRights | kFsRefer;

// `FS_IOCTL_DEV` is handled but granted to no path, so a device `ioctl` outside
// the workspace is denied at the kernel boundary (the `dd`/`mkfs` class).
constexpr std::uint64_t kHandledRights = kRootRights | kFsIoctlDev;

int g_status_fd = -1;

[[noreturn]] void confine_fail(int err) {
    if (g_status_fd >= 0) {
        const ExecStatusRecord record{kExecStatusRefused, err == 0 ? EACCES : err};
        (void)!::write(g_status_fd, &record, sizeof(record));
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
    [[nodiscard]] ConfinementStatus probe() const override {
        (void)abi();
        return classify_landlock_abi(abi_cached_);
    }

    [[nodiscard]] long long abi() const override {
        if (abi_cached_ == kAbiUnprobed) {
            const long value = static_cast<long>(
                ::syscall(__NR_landlock_create_ruleset, nullptr, 0,
                          LANDLOCK_CREATE_RULESET_VERSION));
            abi_cached_ = value < 0 ? static_cast<long long>(-errno) : static_cast<long long>(value);
        }
        return abi_cached_;
    }

    void apply_or_abort(const ChildConfinement& policy) override {
        landlock_ruleset_attr attr{};
        attr.handled_access_fs = confinement_handled_rights(abi());
        const int ruleset_fd   = static_cast<int>(
            ::syscall(__NR_landlock_create_ruleset, &attr, sizeof(attr), 0));
        if (ruleset_fd < 0) {
            confine_fail(errno);
        }

        add_path_rule(ruleset_fd, policy.root,
                      policy.workspace_writable ? kRootRights : kReadRights);
        if (policy.temp.has_value()) {
            add_path_rule(ruleset_fd, *policy.temp, kTmpRights);
        }
        for (const std::filesystem::path& extra : policy.additional_writable_roots) {
            add_path_rule(ruleset_fd, extra, kTmpRights);
        }
        for (const std::filesystem::path& ro : policy.ro_paths) {
            add_path_rule(ruleset_fd, ro, kReadRights);
        }
        // 118-D2: `/dev/null` is the one writable device a well-behaved command
        // needs (git opens it O_RDWR at startup; `2>/dev/null` is ubiquitous).
        // It discards every byte, so it cannot leak or persist anything.
        {
            std::error_code dev_ec;
            if (std::filesystem::exists("/dev/null", dev_ec)) {
                add_path_rule(ruleset_fd, "/dev/null", kDeviceRights);
            }
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

private:
    static constexpr long long kAbiUnprobed = std::numeric_limits<long long>::min();
    mutable long long          abi_cached_  = kAbiUnprobed;
};

#else  // no Landlock headers / syscall numbers at build time

class LandlockConfinement final : public KernelConfinement {
public:
    [[nodiscard]] ConfinementStatus probe() const override {
        return ConfinementStatus::Unavailable;
    }
    [[nodiscard]] long long abi() const override { return -ENOSYS; }
    void apply_or_abort(const ChildConfinement& policy) override {
        (void)policy;
        confine_fail(ENOSYS);
    }
};

#endif

} // namespace

std::uint64_t confinement_handled_rights(long long abi) noexcept {
    if (abi > 0 && abi < 5) {
        return kHandledRights & ~kFsIoctlDev;
    }
    return kHandledRights;
}

ConfinementStatus classify_landlock_abi(long long abi_or_errno) noexcept {
    if (abi_or_errno < 3) {
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

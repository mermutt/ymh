#pragma once

// 92-D2: the opt-in core-dump policy applied once in the workspace daemon's
// startup, immediately after the SIGPIPE disposition and before the first
// thread. Pure decision math plus one bounded startup diagnostic.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <sys/resource.h>

namespace ymh {

// 92-D2: one opt-in switch, one directory name.
inline constexpr std::string_view kCoreDumpDirName = "coredumps";

// 92-I8/M-1: the ONLY line emitted by the InternalError path. It is a fixed
// literal (written with allocation-free fputs) and deliberately has no
// dir/pattern fields, because those allocations may be what failed.
inline constexpr char kCoreDumpInternalErrorLine[] =
    "ymh --host: core dumps: internal error; no limit change "
    "privacy=core-may-contain-prompts-tool-output-secrets\n";

enum class CoreDumpOutcome : std::uint8_t {
    Disabled,          // switch off or foreground: no syscall, no output
    AlreadyAtCeiling,  // soft >= hard; nothing to raise
    HardLimitZero,     // hard == 0; the kernel forbids a core
    RaiseToHard,       // setrlimit succeeded; soft == hard
    SetLimitFailed,    // setrlimit failed; the old limit is retained
    GetLimitFailed,    // getrlimit failed; no limit change
    InternalError,     // diagnostic-path exception; fixed line, no limit change
};

struct CoreDumpPlan {
    bool   apply = false;  // true iff setrlimit should be called
    rlim_t soft = 0;       // requested rlim_cur when apply
    rlim_t hard = 0;       // observed rlim_max; never raised
    CoreDumpOutcome outcome = CoreDumpOutcome::Disabled;
};

// Pure: no syscalls, no environment, no I/O, no allocation.
// Caller: apply_core_dump_policy (src/host/core_dump.cpp); unit tests.
[[nodiscard]] CoreDumpPlan plan_core_dump_raise(rlim_t current_soft,
                                                rlim_t hard) noexcept;

// Pure display-field formatter. Replaces bytes outside printable ASCII
// 0x20..0x7e with '?' and truncates to `max_bytes` plus "...". Caller:
// read_core_pattern (pattern, max 100) and apply_core_dump_policy (dir, max
// 140); unit tests.
[[nodiscard]] std::string sanitize_diagnostic_field(std::string_view text,
                                                    std::size_t max_bytes);

// I/O helper, not pure. Returns "unknown" for an unreadable or empty file;
// otherwise the sanitised, bounded pattern. `path` is injectable for tests.
// Caller: apply_core_dump_policy.
[[nodiscard]] std::string read_core_pattern(
    const std::filesystem::path& path = "/proc/sys/kernel/core_pattern");

// Side-effecting but non-fatal. Emits at most one bounded line to `diag` when
// `enabled` is true. Any allocation/path exception becomes InternalError plus
// the fixed internal-error line, never an escape from `noexcept`. Caller:
// WorkspaceHost::Impl::startup (src/host/workspace_host.cpp, immediately after
// ::signal(SIGPIPE, ...)).
[[nodiscard]] CoreDumpOutcome apply_core_dump_policy(
    bool enabled,
    const std::filesystem::path& workspace_root,
    std::FILE* diag) noexcept;

} // namespace ymh

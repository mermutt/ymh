#include "ymh/host/core_dump.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ymh {
namespace {

std::string render_limit(rlim_t value) {
    return value == RLIM_INFINITY ? std::string{"unlimited"}
                                  : std::to_string(static_cast<std::uint64_t>(value));
}

} // namespace

std::string sanitize_diagnostic_field(std::string_view text, std::size_t max_bytes) {
    std::string out;
    const std::size_t limit = text.size() < max_bytes ? text.size() : max_bytes;
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        out.push_back((byte >= 0x20 && byte <= 0x7e) ? text[i] : '?');
    }
    if (text.size() > max_bytes) {
        out.append("...");
    }
    return out;
}

std::string read_core_pattern(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::string text;
    if (!input) {
        return "unknown";
    }
    std::getline(input, text);
    if (text.empty()) {
        return "unknown";
    }
    return sanitize_diagnostic_field(text, 100);
}

CoreDumpPlan plan_core_dump_raise(rlim_t current_soft, rlim_t hard) noexcept {
    if (hard == 0) {
        return CoreDumpPlan{false, current_soft, hard, CoreDumpOutcome::HardLimitZero};
    }
    if (current_soft >= hard) {
        return CoreDumpPlan{false, current_soft, hard, CoreDumpOutcome::AlreadyAtCeiling};
    }
    return CoreDumpPlan{true, hard, hard, CoreDumpOutcome::RaiseToHard};
}

CoreDumpOutcome apply_core_dump_policy(bool enabled,
                                       const std::filesystem::path& workspace_root,
                                       std::FILE* diag) noexcept {
    if (!enabled) {
        return CoreDumpOutcome::Disabled;
    }

    try {
        ::umask(0077);

        const std::filesystem::path dir = workspace_root / ".ymh" / kCoreDumpDirName;
        std::error_code link_error;
        const std::filesystem::file_status link_status =
            std::filesystem::symlink_status(dir, link_error);
        const bool leaf_exists =
            !link_error && std::filesystem::exists(link_status);
        const bool leaf_is_bad =
            leaf_exists && (std::filesystem::is_symlink(link_status) ||
                            !std::filesystem::is_directory(link_status));

        bool dir_ok = false;
        if (!leaf_is_bad) {
            std::error_code create_error;
            std::filesystem::create_directories(dir, create_error);
            if (!create_error) {
                const int fd = ::open(
                    dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd >= 0) {
                    dir_ok = (::fchmod(fd, S_IRWXU) == 0);
                    ::close(fd);
                }
            }
        }
        const char* dir_status = dir_ok ? "ok" : "unavailable";
        const std::string dir_field =
            sanitize_diagnostic_field(dir.string(), 140);
        const std::string pattern = read_core_pattern();

        struct rlimit current {};
        if (::getrlimit(RLIMIT_CORE, &current) != 0) {
            const int saved = errno;
            const std::string error_text =
                sanitize_diagnostic_field(std::strerror(saved), 60);
            std::fprintf(diag,
                         "ymh --host: core dumps: getrlimit failed (%s); no limit change "
                         "dir=%s dir_status=%s pattern=%s "
                         "privacy=core-may-contain-prompts-tool-output-secrets\n",
                         error_text.c_str(), dir_field.c_str(), dir_status,
                         pattern.c_str());
            return CoreDumpOutcome::GetLimitFailed;
        }

        const CoreDumpPlan plan = plan_core_dump_raise(current.rlim_cur, current.rlim_max);

        if (plan.apply) {
            const struct rlimit next {plan.soft, plan.hard};
            if (::setrlimit(RLIMIT_CORE, &next) != 0) {
                const int saved = errno;
                const std::string error_text =
                    sanitize_diagnostic_field(std::strerror(saved), 60);
                std::fprintf(diag,
                             "ymh --host: core dumps: setrlimit failed (%s) soft=%s hard=%s "
                             "dir=%s dir_status=%s pattern=%s "
                             "privacy=core-may-contain-prompts-tool-output-secrets\n",
                             error_text.c_str(), render_limit(current.rlim_cur).c_str(),
                             render_limit(current.rlim_max).c_str(), dir_field.c_str(),
                             dir_status, pattern.c_str());
                return CoreDumpOutcome::SetLimitFailed;
            }
        }

        struct rlimit effective {};
        const bool have_effective = ::getrlimit(RLIMIT_CORE, &effective) == 0;
        const rlim_t soft = have_effective ? effective.rlim_cur : plan.soft;
        const rlim_t hard = have_effective ? effective.rlim_max : plan.hard;

        if (plan.outcome == CoreDumpOutcome::HardLimitZero) {
            std::fprintf(diag,
                         "ymh --host: core dumps: blocked (RLIMIT_CORE hard limit is 0; "
                         "raise the launching shell/unit LimitCORE) soft=%s hard=%s dir=%s "
                         "dir_status=%s pattern=%s "
                         "privacy=core-may-contain-prompts-tool-output-secrets\n",
                         render_limit(soft).c_str(), render_limit(hard).c_str(),
                         dir_field.c_str(), dir_status, pattern.c_str());
            return CoreDumpOutcome::HardLimitZero;
        }
        if (plan.outcome == CoreDumpOutcome::AlreadyAtCeiling) {
            std::fprintf(diag,
                         "ymh --host: core dumps: already at ceiling soft=%s hard=%s dir=%s "
                         "dir_status=%s pattern=%s "
                         "privacy=core-may-contain-prompts-tool-output-secrets\n",
                         render_limit(soft).c_str(), render_limit(hard).c_str(),
                         dir_field.c_str(), dir_status, pattern.c_str());
            return CoreDumpOutcome::AlreadyAtCeiling;
        }

        std::fprintf(diag,
                     "ymh --host: core dumps: enabled soft=%s hard=%s dir=%s dir_status=%s "
                     "pattern=%s privacy=core-may-contain-prompts-tool-output-secrets\n",
                     render_limit(soft).c_str(), render_limit(hard).c_str(),
                     dir_field.c_str(), dir_status, pattern.c_str());
        return CoreDumpOutcome::RaiseToHard;
    } catch (...) {
        if (diag != nullptr) {
            std::fputs(kCoreDumpInternalErrorLine, diag);
        }
        return CoreDumpOutcome::InternalError;
    }
}

} // namespace ymh

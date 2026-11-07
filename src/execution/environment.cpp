#include "ymh/execution/environment.hpp"

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

#include "ymh/execution/errors.hpp"
#include "ymh/execution/path_safety.hpp"

namespace ymh {
namespace {

std::filesystem::path weakly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : result;
}

bool component_prefix(const std::filesystem::path& base,
                      const std::filesystem::path& candidate) {
    auto base_it = base.begin();
    auto candidate_it = candidate.begin();
    for (; base_it != base.end(); ++base_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *base_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

} // namespace

bool path_is_within(const std::filesystem::path& candidate,
                    const std::filesystem::path& base) {
    return component_prefix(weakly(base), weakly(candidate));
}

LocalEnvironment::LocalEnvironment(std::filesystem::path root,
                                   SandboxMode mode,
                                   ToolConfig config,
                                   PtyService* pty,
                                   std::vector<std::filesystem::path> writable_roots)
    : root_(std::move(root)),
      mode_(mode),
      confinement_status_(mode == SandboxMode::DangerFullAccess
                              ? ConfinementStatus::NotApplicable
                              : kernel_confinement().probe()),
      fs_(std::filesystem::path{}, config),
      process_(config.terminate_grace),
      pty_(pty != nullptr ? pty : &pty_fallback_),
      config_(config),
      writable_roots_(std::move(writable_roots)) {
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec) ||
        !std::filesystem::is_directory(root_, ec)) {
        throw std::invalid_argument("workspace root is not a directory: " +
                                    root_.string());
    }
    root_ = std::filesystem::canonical(root_, ec);
    if (ec) {
        throw std::invalid_argument("workspace root cannot be canonicalized: " +
                                    root_.string());
    }
    fs_ = LocalFilesystem(root_, config);
    policy_service_ = make_deployment_policy_service(mode_, writable_roots_);
}

SandboxProvider& LocalEnvironment::sandbox() { return default_sandbox_provider(); }

SandboxPolicyService& LocalEnvironment::sandboxPolicy() {
    return external_policy_ != nullptr ? *external_policy_ : *policy_service_;
}

std::filesystem::path LocalEnvironment::resolve(std::string_view path) const {
    if (path.empty()) {
        throw ToolError{ToolErrorCode::PathEscape, "empty path"};
    }
    const std::filesystem::path raw{std::string(path)};
    const std::filesystem::path base = root_;
    std::filesystem::path candidate = raw.is_absolute() ? raw : base / raw;

    if (mode_ == SandboxMode::DangerFullAccess) {
        return candidate.lexically_normal();
    }

    const std::filesystem::path parent = weakly(candidate.parent_path());
    if (!path_is_within(parent, base)) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes workspace root: " + raw.string()};
    }

    std::filesystem::path final = parent / candidate.filename();
    std::error_code ec;
    if (std::filesystem::exists(final, ec)) {
        std::filesystem::path canonical = std::filesystem::canonical(final, ec);
        if (!ec) {
            final = std::move(canonical);
        }
    }
    if (!path_is_within(final, base)) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes workspace root: " + raw.string()};
    }
    return final;
}

} // namespace ymh

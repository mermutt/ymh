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
                                   std::vector<std::filesystem::path> writable_roots,
                                   std::shared_ptr<WorkspaceRoots> roots)
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
    roots_ = roots != nullptr ? std::move(roots)
                              : std::make_shared<WorkspaceRoots>(root_);
    std::vector<std::filesystem::path> allowed_roots;
    if (mode_ == SandboxMode::WorkspaceWrite) {
        // 119-D7: the workspace-write shell sandbox exposes the host /tmp as
        // its scratch area. File tools use the same area so a helper written
        // by `write_file` is readable by `read_file`/`edit_file` without a
        // second root.
        const std::filesystem::path& temp = canonical_tmp_root();
        if (!temp.empty() && temp != root_) {
            allowed_roots.push_back(temp);
        }
    }
    fs_ = LocalFilesystem(root_, config_, std::move(allowed_roots), roots_);
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

    // Workspace-write also exposes the host /tmp scratch area (the same area
    // the shell sandbox grants). Read-only keeps the historical root scope.
    const std::filesystem::path temp =
        mode_ == SandboxMode::WorkspaceWrite ? canonical_tmp_root()
                                             : std::filesystem::path{};
    const std::filesystem::path tmp_literal{"/tmp"};

    const std::filesystem::path normalized = candidate.lexically_normal();

    // Containment kind: 0..N-1 is the index of the containing root; negative
    // values are the /tmp scratch area or "outside every scope".
    constexpr int kNone = -1;
    constexpr int kTemp = -2;
    const std::vector<std::filesystem::path> roots = roots_->all();
    const auto index_of = [&roots](const std::filesystem::path& value) {
        for (std::size_t index = 0; index < roots.size(); ++index) {
            if (component_prefix(roots[index], value)) {
                return static_cast<int>(index);
            }
        }
        return kNone;
    };
    const auto lexical_kind = [&](const std::filesystem::path& value) {
        const int root_index = index_of(value);
        if (root_index != kNone) {
            return root_index;
        }
        if (component_prefix(tmp_literal, value)) {
            return kTemp;
        }
        return kNone;
    };
    const auto canonical_kind = [&](const std::filesystem::path& value) {
        const int root_index = index_of(weakly(value));
        if (root_index != kNone) {
            return root_index;
        }
        if (!temp.empty() && component_prefix(temp, weakly(value))) {
            return kTemp;
        }
        return kNone;
    };

    const int lexical_root = lexical_kind(normalized);
    if (lexical_root == kNone) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes the workspace roots or /tmp scratch area: " +
                            raw.string()};
    }

    const std::filesystem::path parent = weakly(candidate.parent_path());
    if (canonical_kind(parent) != lexical_root) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes the workspace roots or /tmp scratch area: " +
                            raw.string()};
    }

    std::filesystem::path final = parent / candidate.filename();
    std::error_code ec;
    if (std::filesystem::exists(final, ec)) {
        std::filesystem::path canonical = std::filesystem::canonical(final, ec);
        if (!ec) {
            final = std::move(canonical);
        }
    }
    if (canonical_kind(final) != lexical_root) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes the workspace roots or /tmp scratch area: " +
                            raw.string()};
    }
    return final;
}

} // namespace ymh

#include "ymh/execution/environment.hpp"

#include <stdexcept>
#include <string>
#include <system_error>

#include "ymh/execution/errors.hpp"

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
                                   PtyService* pty)
    : root_(std::move(root)),
      mode_(mode),
      fs_(std::filesystem::path{}, config),
      process_(config.terminate_grace),
      pty_(pty != nullptr ? pty : &pty_fallback_) {
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
}

std::filesystem::path LocalEnvironment::resolve(std::string_view path) const {
    if (path.empty()) {
        throw ToolError{ToolErrorCode::PathEscape, "empty path"};
    }
    const std::filesystem::path raw{std::string(path)};
    const std::filesystem::path base = root_;
    std::filesystem::path candidate = raw.is_absolute() ? raw : base / raw;

    if (mode_ == SandboxMode::Unrestricted) {
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

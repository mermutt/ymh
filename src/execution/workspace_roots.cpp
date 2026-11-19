#include "ymh/execution/workspace_roots.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <utility>

#include "ymh/execution/path_safety.hpp"

namespace ymh {
namespace {

std::filesystem::path canonical_or_lexical(const std::filesystem::path& path) {
    std::error_code       ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

std::optional<std::filesystem::path> home_directory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::nullopt;
    }
    return canonical_or_lexical(std::filesystem::path{home});
}

bool is_filesystem_root(const std::filesystem::path& path) {
    return path == path.root_path();
}

bool is_system_directory(const std::filesystem::path& path) {
    static const std::filesystem::path kForbidden[] = {
        std::filesystem::path{"/"},     std::filesystem::path{"/home"},
        std::filesystem::path{"/etc"},  std::filesystem::path{"/usr"},
    };
    return std::find(std::begin(kForbidden), std::end(kForbidden), path) !=
           std::end(kForbidden);
}

bool is_under_temp(const std::filesystem::path& path) {
    const std::filesystem::path& temp = canonical_tmp_root();
    if (temp.empty()) {
        return false;
    }
    return path == temp || path_is_under_tmp_root(temp, path);
}

} // namespace

bool path_is_under_root(const std::filesystem::path& root,
                        const std::filesystem::path& candidate) {
    const std::filesystem::path normalized_root = root.lexically_normal();
    const std::filesystem::path normalized      = candidate.lexically_normal();
    if (!normalized_root.is_absolute() || !normalized.is_absolute()) {
        return false;
    }
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized.end() || *candidate_it != *root_it) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> containing_root(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path&              candidate) {
    for (const std::filesystem::path& root : roots) {
        if (path_is_under_root(root, candidate)) {
            return root;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> relative_to_any_root(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path&              candidate) {
    for (const std::filesystem::path& root : roots) {
        if (!path_is_under_root(root, candidate)) {
            continue;
        }
        const std::filesystem::path relative =
            candidate.lexically_relative(root.lexically_normal());
        if (relative.is_absolute()) {
            continue;
        }
        return relative.empty() ? std::filesystem::path{"."} : relative;
    }
    return std::nullopt;
}

bool path_is_protected_in_roots(const std::vector<std::filesystem::path>& roots,
                                const std::filesystem::path&              candidate,
                                bool allow_ymh) {
    for (const std::filesystem::path& root : roots) {
        if (path_is_under_root(root, candidate) &&
            path_is_protected(root, candidate, allow_ymh)) {
            return true;
        }
    }
    return false;
}

std::expected<std::filesystem::path, std::string> validate_workspace_root(
    const std::filesystem::path&              candidate,
    const std::filesystem::path&              primary,
    const std::vector<std::filesystem::path>& existing) {
    if (candidate.empty()) {
        return std::unexpected("empty directory");
    }
    const std::filesystem::path primary_canonical = canonical_or_lexical(primary);
    std::filesystem::path       absolute =
        candidate.is_absolute() ? candidate : (primary_canonical / candidate);

    std::error_code       ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    if (ec || canonical.empty()) {
        return std::unexpected("cannot resolve directory: " + absolute.string());
    }
    canonical = canonical.lexically_normal();
    if (!std::filesystem::is_directory(canonical, ec) || ec) {
        return std::unexpected("not a directory: " + canonical.string());
    }
    if (is_filesystem_root(canonical) || is_system_directory(canonical)) {
        return std::unexpected("refusing to allow a system directory: " +
                               canonical.string());
    }
    if (const std::optional<std::filesystem::path> home = home_directory();
        home.has_value() && canonical == *home) {
        return std::unexpected("refusing to allow the home directory; choose a project "
                               "directory inside it");
    }
    if (is_under_temp(canonical)) {
        return std::unexpected("refusing to allow a directory inside /tmp: " +
                               canonical.string());
    }
    if (canonical == primary_canonical) {
        return std::unexpected("already the workspace root: " + canonical.string());
    }
    if (path_is_under_root(primary_canonical, canonical)) {
        return std::unexpected("already inside the workspace root: " + canonical.string());
    }
    if (path_is_under_root(canonical, primary_canonical)) {
        return std::unexpected("would widen the workspace root: " + canonical.string());
    }

    std::vector<std::filesystem::path> known = existing;
    known.push_back(primary_canonical);
    for (const std::filesystem::path& root : known) {
        const std::filesystem::path normalized = canonical_or_lexical(root);
        if (canonical == normalized || path_is_under_root(normalized, canonical)) {
            return std::unexpected("already allowed: " + canonical.string());
        }
        if (path_is_under_root(canonical, normalized)) {
            return std::unexpected("would widen an allowed root: " + canonical.string());
        }
    }
    return canonical;
}

std::optional<std::filesystem::path> suggest_workspace_root(
    const std::filesystem::path&              target,
    const std::filesystem::path&              primary,
    const std::vector<std::filesystem::path>& existing) {
    if (target.empty()) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> roots = existing;
    if (roots.empty() || roots.front() != primary) {
        roots.insert(roots.begin(), primary);
    }
    if (containing_root(roots, target).has_value()) {
        return std::nullopt;
    }

    std::error_code       ec;
    std::filesystem::path start = canonical_or_lexical(target);
    if (!std::filesystem::is_directory(start, ec) || ec) {
        start = canonical_or_lexical(target.parent_path());
    }
    if (start.empty()) {
        return std::nullopt;
    }

    const std::optional<std::filesystem::path> home = home_directory();
    std::filesystem::path                      candidate = start;
    for (std::filesystem::path dir = start;; dir = dir.parent_path()) {
        if (dir.empty() || is_filesystem_root(dir) || (home.has_value() && dir == *home) ||
            is_under_temp(dir)) {
            break;
        }
        std::error_code probe;
        if (std::filesystem::exists(dir / ".git", probe) ||
            std::filesystem::exists(dir / ".ymh", probe)) {
            candidate = dir;
            break;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
    }

    const std::expected<std::filesystem::path, std::string> validated =
        validate_workspace_root(candidate, primary, existing);
    if (!validated.has_value()) {
        return std::nullopt;
    }
    return *validated;
}

WorkspaceRoots::WorkspaceRoots(std::filesystem::path              primary,
                               std::vector<std::filesystem::path> additional)
    : primary_(canonical_or_lexical(primary)) {
    for (std::filesystem::path& root : additional) {
        if (!root.is_absolute()) {
            continue;
        }
        root = canonical_or_lexical(root);
        if (root == primary_ || path_is_under_root(primary_, root) ||
            path_is_under_root(root, primary_)) {
            continue;
        }
        const bool duplicate =
            std::any_of(additional_.begin(), additional_.end(),
                        [&root](const std::filesystem::path& known) {
                            return known == root;
                        });
        if (!duplicate) {
            additional_.push_back(std::move(root));
        }
    }
}

std::filesystem::path WorkspaceRoots::primary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return primary_;
}

std::vector<std::filesystem::path> WorkspaceRoots::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::filesystem::path> roots;
    roots.reserve(additional_.size() + 1);
    roots.push_back(primary_);
    roots.insert(roots.end(), additional_.begin(), additional_.end());
    return roots;
}

bool WorkspaceRoots::contains(const std::filesystem::path& candidate) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path_is_under_root(primary_, candidate)) {
        return true;
    }
    return std::any_of(additional_.begin(), additional_.end(),
                       [&candidate](const std::filesystem::path& root) {
                           return path_is_under_root(root, candidate);
                       });
}

std::optional<std::filesystem::path> WorkspaceRoots::relative(
    const std::filesystem::path& candidate) const {
    const std::vector<std::filesystem::path> roots = all();
    return relative_to_any_root(roots, candidate);
}

std::expected<std::filesystem::path, std::string> WorkspaceRoots::validate(
    const std::filesystem::path& candidate) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validate_workspace_root(candidate, primary_, additional_);
}

std::expected<std::filesystem::path, std::string> WorkspaceRoots::add(
    const std::filesystem::path& candidate) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::expected<std::filesystem::path, std::string> validated =
        validate_workspace_root(candidate, primary_, additional_);
    if (!validated.has_value()) {
        return validated;
    }
    additional_.push_back(*validated);
    return validated;
}

} // namespace ymh

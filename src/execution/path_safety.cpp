#include "ymh/execution/path_safety.hpp"

#include <string>
#include <system_error>

namespace ymh {

const std::filesystem::path& canonical_tmp_root() {
    static const std::filesystem::path root = [] {
        std::error_code error;
        std::filesystem::path canonical =
            std::filesystem::weakly_canonical(std::filesystem::path{"/tmp"}, error);
        if (error || canonical.empty()) {
            return std::filesystem::path{};
        }
        if (!std::filesystem::is_directory(canonical, error) || error) {
            return std::filesystem::path{};
        }
        return canonical;
    }();
    return root;
}

bool path_is_under_tmp_root(const std::filesystem::path& canonical_tmp_root,
                            const std::filesystem::path& candidate) {
    if (canonical_tmp_root.empty() || !candidate.is_absolute()) {
        return false;
    }
    for (const std::filesystem::path& component : candidate) {
        if (component == "..") {
            return false;
        }
    }
    const std::filesystem::path root = canonical_tmp_root.lexically_normal();
    const std::filesystem::path target = candidate.lexically_normal();
    if (!root.is_absolute() || target == root) {
        return false;
    }
    auto root_it = root.begin();
    auto target_it = target.begin();
    for (; root_it != root.end(); ++root_it, ++target_it) {
        if (target_it == target.end() || *root_it != *target_it) {
            return false;
        }
    }
    for (; target_it != target.end(); ++target_it) {
        if (!target_it->empty() && *target_it != ".") {
            return true;
        }
    }
    return false;
}

bool path_is_protected(const std::filesystem::path& canonical_root,
                       const std::filesystem::path& candidate) {
    const std::filesystem::path root = canonical_root.lexically_normal();
    const std::filesystem::path target = candidate.lexically_normal();
    if (!root.is_absolute() || !target.is_absolute()) {
        return false;
    }
    const std::filesystem::path relative = target.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    bool first = true;
    for (const std::filesystem::path& component : relative) {
        if (first && component == "..") {
            return false;
        }
        first = false;
        if (component == ".ymh" || component == ".git") {
            return true;
        }
    }
    return false;
}

} // namespace ymh

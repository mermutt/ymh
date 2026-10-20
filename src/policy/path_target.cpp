#include "ymh/policy/path_target.hpp"

#include <array>
#include <string>

namespace ymh {
namespace {

bool is_required_path_tool(std::string_view tool) noexcept {
    return tool == "read_file" || tool == "write_file" || tool == "edit_file";
}

bool is_optional_path_tool(std::string_view tool) noexcept {
    return tool == "grep" || tool == "glob" || tool == "git_diff";
}

const nlohmann::json* member(const nlohmann::json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(std::string{key});
    return it == object.end() ? nullptr : &(*it);
}

std::optional<std::filesystem::path> normalize_target(
    const nlohmann::json& arguments, const std::filesystem::path& canonical_root) {
    const nlohmann::json* value = member(arguments, "path");
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    const std::string text = value->get<std::string>();
    if (text.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path argument{text};
    std::filesystem::path       candidate =
        argument.is_absolute() ? argument : (canonical_root / argument);
    return candidate.lexically_normal();
}

} // namespace

bool tool_has_path_dimension(std::string_view tool) noexcept {
    static constexpr std::array<std::string_view, 7> kTools = {
        "read_file", "write_file", "edit_file", "grep", "glob", "git_status", "git_diff"};
    for (std::string_view candidate : kTools) {
        if (candidate == tool) {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> request_path_target(
    std::string_view tool, const nlohmann::json& arguments,
    const std::filesystem::path& canonical_root) {
    if (is_required_path_tool(tool)) {
        return normalize_target(arguments, canonical_root);
    }
    if (tool == "git_status") {
        // The tool schema has no path argument; an undocumented `path` is
        // ignored (90-A14).
        return canonical_root.lexically_normal();
    }
    if (is_optional_path_tool(tool)) {
        const nlohmann::json* value = member(arguments, "path");
        if (value == nullptr) {
            return canonical_root.lexically_normal();
        }
        if (!value->is_string()) {
            return std::nullopt;
        }
        if (value->get<std::string>().empty()) {
            return std::nullopt;
        }
        return normalize_target(arguments, canonical_root);
    }
    return std::nullopt;
}

bool path_is_within_root(const std::filesystem::path& canonical_root,
                         const std::filesystem::path& candidate) {
    const std::filesystem::path root = canonical_root.lexically_normal();
    const std::filesystem::path target = candidate.lexically_normal();
    if (!root.is_absolute() || !target.is_absolute()) {
        return false;
    }
    auto root_it = root.begin();
    auto target_it = target.begin();
    for (; root_it != root.end(); ++root_it, ++target_it) {
        if (target_it == target.end() || *target_it != *root_it) {
            return false;
        }
    }
    return true;
}

} // namespace ymh

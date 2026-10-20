#pragma once

// Spec 90 (90-D2/90-D4): pure, I/O-free mapping from a tool call's arguments
// to a lexical request target class. The target class is matching input only:
// it is never durable and never sent on the wire.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ymh {

// 90-D2: request target class. Matching input only; never durable, never wire.
enum class TargetClass : std::uint8_t { Any, InRoot, Outside, Unknown };

// 90-D2: exactly read_file, write_file, edit_file, grep, glob, git_status,
// git_diff (sec 4.2). Pure; no allocation.
[[nodiscard]] bool tool_has_path_dimension(std::string_view tool) noexcept;

// 90-D2: pure, total over malformed JSON. Returns the absolute, lexically
// normalized target, or nullopt for a non-path tool / missing required path.
// Never calls resolve(), stat, canonical, or weakly_canonical.
[[nodiscard]] std::optional<std::filesystem::path> request_path_target(
    std::string_view tool,
    const nlohmann::json& arguments,
    const std::filesystem::path& canonical_root);

// 90-D2/90-D4: lexical containment. Both operands must be absolute; the root
// itself is inside; no symlink resolution; no home special case.
[[nodiscard]] bool path_is_within_root(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& candidate);

} // namespace ymh

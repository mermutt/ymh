#pragma once

// Strict YAML-subset frontmatter parser for SKILL.md (20-skills.md §4.2, §5.3).
// Pure: no I/O. `error` is fatal (skip the skill); `warnings` is non-fatal (keep
// the skill, SK-F4/M1).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/skills/skill_types.hpp"

namespace ymh {

struct FrontmatterResult {
    SkillMetadata            meta;      // populated only when `error` is empty
    std::string              body;      // text after the closing fence (normalized)
    std::string              error;     // empty on success; otherwise a reason
    std::vector<std::string> warnings;  // non-fatal, e.g. description truncation
};

// `content` is the full SKILL.md text. `expected_name` is the directory
// basename; a mismatch yields an error (SK-F3).
[[nodiscard]] FrontmatterResult parse_skill_file(std::string_view content,
                                                 std::string_view expected_name,
                                                 std::size_t max_frontmatter_bytes,
                                                 std::size_t max_description_bytes);

} // namespace ymh

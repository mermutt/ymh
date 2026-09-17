#pragma once

// The skill model (20-skills.md §2, §5.1). A skill is a user-authored,
// filesystem-discovered markdown document whose only runtime effect is to place
// its text into the model's context (SK1). `SkillName` is a distinct namespace
// from `ToolName`: a skill can never shadow or become a tool (SK10).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {

// Grammar: [a-z][a-z0-9]*(-[a-z0-9]+)*, length 1..64 (20 §2.3).
struct SkillName {
    std::string value;
    auto operator<=>(const SkillName&) const = default;
};

[[nodiscard]] bool is_valid_skill_name(std::string_view name) noexcept;

enum class SkillSource : std::uint8_t {
    User,       // <config-root>/ymh/skills/<name>/SKILL.md
    Workspace,  // <workspace>/.ymh/skills/<name>/SKILL.md
    Builtin,    // reserved; never produced in v1 (20-D2, OQ-3)
};

enum class SkillTrust : std::uint8_t {
    Trusted,    // User
    Untrusted,  // Workspace (VCS-controllable)
};

struct SkillMetadata {
    SkillName                name;
    std::string              description;
    std::uint32_t            version{1};
    std::vector<std::string> allowed_tools;  // ADVISORY ONLY — never enforced
    std::vector<std::string> tags;
    std::string              license;
    std::string              model;          // advisory hint — never enforced
};

struct Skill {
    SkillMetadata         meta;
    SkillSource           source{SkillSource::User};
    SkillTrust            trust{SkillTrust::Trusted};
    std::string           body;          // eagerly loaded, bounded (20 §4.3)
    std::filesystem::path file;          // canonical SKILL.md path (provenance)
};

[[nodiscard]] std::string_view skill_source_name(SkillSource source) noexcept;
[[nodiscard]] std::string_view skill_trust_name(SkillTrust trust) noexcept;

} // namespace ymh

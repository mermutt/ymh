#pragma once

// Ordered skill/command root resolution (50-skills-mcp-and-session-lifecycle-
// errata.md §3.2, 50-D1.2/D1.3). Pure path arithmetic; no I/O and no getenv in
// the parameterized overloads, so a unit test injects `home`/`config_root`
// instead of mutating the process environment.

#include <filesystem>
#include <vector>

#include "ymh/skills/skill_types.hpp"

namespace ymh {

struct SkillRoot {
    std::filesystem::path path;   // MUST be absolute to be used
    SkillSource           source{SkillSource::User};
    SkillTrust            trust{SkillTrust::Trusted};
};

// The three user-tier candidates, in pinned precedence order:
//   $HOME/.ymh/skills > <config-root>/skills > $HOME/.claude/skills.
// A relative/empty base yields a relative candidate; the catalog disables that
// root with one warning (50-I1). The workspace root is appended by the caller.
[[nodiscard]] std::vector<SkillRoot>
skill_roots(const std::filesystem::path& home, const std::filesystem::path& config_root);

// Same order, with `commands` as the leaf. Command discovery is flat.
[[nodiscard]] std::vector<SkillRoot>
command_roots(const std::filesystem::path& home, const std::filesystem::path& config_root);

// Env-backed convenience wrappers (HOME / default_global_config_path()).
[[nodiscard]] std::vector<SkillRoot> default_skill_roots();
[[nodiscard]] std::vector<SkillRoot> default_command_roots();

} // namespace ymh

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
    // Warnings for a missing root are emitted only for required roots. Explicit
    // `[skills].roots` entries are required; conventional locations are not.
    bool                  required = false;
};

// The user-tier candidates, in pinned precedence order:
//   $HOME/.ymh/skills > <config-root>/skills > $HOME/.config/opencode/skills
//   > $HOME/opencode/skills > $HOME/.claude/skills > $HOME/.agents/skills.
// A relative/empty base yields a relative candidate; the catalog disables that
// root with one warning (50-I1). The workspace roots are appended by the caller.
[[nodiscard]] std::vector<SkillRoot>
skill_roots(const std::filesystem::path& home, const std::filesystem::path& config_root);

// The project-tier candidates for one workspace, in precedence order:
//   .ymh/skills > .opencode/skills > opencode/skills > .claude/skills
//   > .agents/skills.
// All are marked `Untrusted`; the catalog still applies workspace trust.
[[nodiscard]] std::vector<SkillRoot>
workspace_skill_roots(const std::filesystem::path& workspace);

// Same order, with `commands` as the leaf. Command discovery is flat.
[[nodiscard]] std::vector<SkillRoot>
command_roots(const std::filesystem::path& home, const std::filesystem::path& config_root);

// Env-backed convenience wrappers (HOME / default_global_config_path()).
[[nodiscard]] std::vector<SkillRoot> default_skill_roots();
[[nodiscard]] std::vector<SkillRoot> default_command_roots();

} // namespace ymh

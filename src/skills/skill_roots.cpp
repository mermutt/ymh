#include "ymh/skills/skill_roots.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/config/config.hpp"

namespace ymh {
namespace {

std::filesystem::path env_path(std::string_view name) {
    const char* raw = std::getenv(std::string{name}.c_str());
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    return std::filesystem::path{raw};
}

std::vector<SkillRoot> roots_with_leaf(const std::filesystem::path& home,
                                       const std::filesystem::path& config_root,
                                       std::string_view               leaf) {
    std::vector<SkillRoot> roots;
    roots.reserve(6);
    roots.push_back(SkillRoot{home / ".ymh" / std::string{leaf}, SkillSource::Home,
                              SkillTrust::Trusted});
    roots.push_back(SkillRoot{config_root / std::string{leaf}, SkillSource::User,
                              SkillTrust::Trusted});
    // OpenCode and its compatibility roots. A skill written for OpenCode is
    // picked up in place, without copying or rewriting it.
    roots.push_back(SkillRoot{home / ".config" / "opencode" / std::string{leaf},
                              SkillSource::Opencode, SkillTrust::Trusted});
    roots.push_back(SkillRoot{home / "opencode" / std::string{leaf}, SkillSource::Opencode,
                              SkillTrust::Trusted});
    roots.push_back(SkillRoot{home / ".claude" / std::string{leaf}, SkillSource::Claude,
                              SkillTrust::Trusted});
    roots.push_back(SkillRoot{home / ".agents" / std::string{leaf}, SkillSource::Agents,
                              SkillTrust::Trusted});
    return roots;
}

} // namespace

std::vector<SkillRoot> skill_roots(const std::filesystem::path& home,
                                   const std::filesystem::path& config_root) {
    return roots_with_leaf(home, config_root, "skills");
}

std::vector<SkillRoot> command_roots(const std::filesystem::path& home,
                                     const std::filesystem::path& config_root) {
    return roots_with_leaf(home, config_root, "commands");
}

std::vector<SkillRoot> default_skill_roots() {
    return skill_roots(env_path("HOME"), default_global_config_path().parent_path());
}

std::vector<SkillRoot> workspace_skill_roots(const std::filesystem::path& workspace) {
    // Project-tier skill sources. They stay untrusted: a repository can travel
    // with its own instruction files, so visibility is still gated by workspace
    // trust and `skills.expose_workspace`.
    return {SkillRoot{workspace / ".ymh" / "skills", SkillSource::Workspace,
                      SkillTrust::Untrusted},
            SkillRoot{workspace / ".opencode" / "skills", SkillSource::Opencode,
                      SkillTrust::Untrusted},
            // Some projects keep the OpenCode tree without the leading dot.
            SkillRoot{workspace / "opencode" / "skills", SkillSource::Opencode,
                      SkillTrust::Untrusted},
            SkillRoot{workspace / ".claude" / "skills", SkillSource::Claude,
                      SkillTrust::Untrusted},
            SkillRoot{workspace / ".agents" / "skills", SkillSource::Agents,
                      SkillTrust::Untrusted}};
}

std::vector<SkillRoot> default_command_roots() {
    return command_roots(env_path("HOME"), default_global_config_path().parent_path());
}

} // namespace ymh

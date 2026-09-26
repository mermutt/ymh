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
    roots.reserve(3);
    roots.push_back(SkillRoot{home / ".ymh" / std::string{leaf}, SkillSource::Home,
                              SkillTrust::Trusted});
    roots.push_back(SkillRoot{config_root / std::string{leaf}, SkillSource::User,
                              SkillTrust::Trusted});
    roots.push_back(SkillRoot{home / ".claude" / std::string{leaf}, SkillSource::Claude,
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

std::vector<SkillRoot> default_command_roots() {
    return command_roots(env_path("HOME"), default_global_config_path().parent_path());
}

} // namespace ymh

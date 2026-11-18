#include "ymh/skills/skill_types.hpp"

namespace ymh {
namespace {

constexpr std::size_t kMaxSkillNameLength = 64;

bool is_lower_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

} // namespace

bool is_valid_skill_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxSkillNameLength) {
        return false;
    }
    const char first = name.front();
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) {
        return false;
    }
    for (const char c : name) {
        if (!is_lower_alnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

std::string_view skill_source_name(SkillSource source) noexcept {
    switch (source) {
        case SkillSource::Home:
            return "home";
        case SkillSource::User:
            return "user";
        case SkillSource::Opencode:
            return "opencode";
        case SkillSource::Claude:
            return "claude";
        case SkillSource::Agents:
            return "agents";
        case SkillSource::Workspace:
            return "workspace";
        case SkillSource::Builtin:
            return "builtin";
    }
    return "unknown";
}

std::string_view skill_trust_name(SkillTrust trust) noexcept {
    switch (trust) {
        case SkillTrust::Trusted:
            return "trusted";
        case SkillTrust::Untrusted:
            return "untrusted";
    }
    return "unknown";
}

} // namespace ymh

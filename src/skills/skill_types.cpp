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
    if (name[0] < 'a' || name[0] > 'z') {
        return false;
    }
    std::size_t index = 1;
    while (index < name.size() && name[index] != '-') {
        if (!is_lower_alnum(name[index])) {
            return false;
        }
        ++index;
    }
    while (index < name.size()) {
        if (name[index] != '-') {
            return false;
        }
        ++index;
        const std::size_t segment = index;
        while (index < name.size() && name[index] != '-') {
            if (!is_lower_alnum(name[index])) {
                return false;
            }
            ++index;
        }
        if (index == segment) {
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
        case SkillSource::Claude:
            return "claude";
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

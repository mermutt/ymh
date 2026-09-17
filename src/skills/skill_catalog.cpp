#include "ymh/skills/skill_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "ymh/config/config.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/skills/frontmatter.hpp"

namespace ymh {
namespace {

std::filesystem::path weakly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : result;
}

struct ScanItem {
    std::optional<Skill>          skill;
    std::vector<SkillLoadWarning> warnings;
};

std::string join_reason(const std::filesystem::path& file, std::string_view reason) {
    return file.string() + ": " + std::string{reason};
}

std::vector<ScanItem> scan(const std::filesystem::path& root,
                           SkillSource                  source,
                           SkillTrust                   trust,
                           const SkillCatalogConfig&    config,
                           const ExecutionEnvironment*  environment,
                           bool                         workspace_tier) {
    std::vector<ScanItem> items;
    std::error_code       ec;
    const std::filesystem::path base = weakly(root);

    if (workspace_tier && !path_is_within(base, environment->root())) {
        items.push_back(ScanItem{std::nullopt, {SkillLoadWarning{root, "skills root escapes the workspace"}}});
        return items;
    }
    if (!std::filesystem::is_directory(base, ec) || ec) {
        items.push_back(ScanItem{std::nullopt, {SkillLoadWarning{root, "skills root is unavailable"}}});
        return items;
    }

    std::filesystem::directory_iterator iterator(base, ec);
    if (ec) {
        items.push_back(ScanItem{std::nullopt, {SkillLoadWarning{root, "skills root is unreadable"}}});
        return items;
    }

    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) {
            continue;
        }
        const std::filesystem::path directory = weakly(entry.path());
        if (!path_is_within(directory, base)) {
            items.push_back(ScanItem{std::nullopt,
                                     {SkillLoadWarning{entry.path(), "skill directory escapes its root"}}});
            continue;
        }

        const std::filesystem::path candidate = directory / "SKILL.md";
        std::filesystem::path       resolved  = candidate;
        if (workspace_tier) {
            std::error_code relative_ec;
            const std::filesystem::path relative =
                std::filesystem::relative(candidate, environment->root(), relative_ec);
            if (relative_ec) {
                items.push_back(ScanItem{std::nullopt,
                                         {SkillLoadWarning{candidate, "cannot resolve SKILL.md"}}});
                continue;
            }
            try {
                resolved = environment->resolve(relative.string());
            } catch (const ToolError&) {
                items.push_back(ScanItem{std::nullopt,
                                         {SkillLoadWarning{candidate, "SKILL.md escapes the workspace root"}}});
                continue;
            }
        }

        if (!std::filesystem::is_regular_file(resolved, entry_ec) || entry_ec) {
            items.push_back(ScanItem{std::nullopt,
                                     {SkillLoadWarning{candidate, "SKILL.md is not a regular file"}}});
            continue;
        }
        const std::filesystem::path canonical_file = weakly(resolved);
        if (!path_is_within(canonical_file, base)) {
            items.push_back(ScanItem{std::nullopt,
                                     {SkillLoadWarning{candidate, "SKILL.md escapes its root"}}});
            continue;
        }

        const std::uintmax_t size = std::filesystem::file_size(canonical_file, entry_ec);
        if (entry_ec) {
            items.push_back(ScanItem{std::nullopt,
                                     {SkillLoadWarning{candidate, "cannot stat SKILL.md"}}});
            continue;
        }
        if (size > config.max_skill_bytes) {
            items.push_back(ScanItem{std::nullopt,
                                     {SkillLoadWarning{candidate, "exceeds max_skill_bytes"}}});
            continue;
        }

        std::ifstream input(canonical_file, std::ios::binary);
        if (!input) {
            items.push_back(ScanItem{std::nullopt,
                                     {SkillLoadWarning{candidate, "cannot read SKILL.md"}}});
            continue;
        }
        const std::string content{std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()};

        FrontmatterResult parsed = parse_skill_file(
            content, directory.filename().string(), config.max_frontmatter_bytes,
            config.max_description_bytes);
        if (!parsed.error.empty()) {
            items.push_back(
                ScanItem{std::nullopt, {SkillLoadWarning{canonical_file, std::move(parsed.error)}}});
            continue;
        }

        Skill skill;
        skill.meta   = std::move(parsed.meta);
        skill.source = source;
        skill.trust  = trust;
        skill.body   = std::move(parsed.body);
        skill.file   = canonical_file;
        std::vector<SkillLoadWarning> warnings;
        warnings.reserve(parsed.warnings.size());
        for (const std::string& warning : parsed.warnings) {
            warnings.push_back(SkillLoadWarning{canonical_file, warning});
        }
        items.push_back(ScanItem{std::move(skill), std::move(warnings)});
    }
    return items;
}

std::string build_index(const std::vector<const Skill*>& skills, std::size_t max_index_bytes) {
    if (skills.empty()) {
        return {};
    }
    const std::string header =
        "Available skills (load a skill's full instructions with the `skill` tool):\n";
    const auto entry = [](const Skill* skill) {
        return "- " + skill->meta.name.value + ": " + skill->meta.description + "\n";
    };
    const auto note = [](std::size_t remaining) {
        return "(" + std::to_string(remaining) + " more skills not shown; run /skills)\n";
    };

    std::string out = header;
    std::size_t index = 0;
    for (; index < skills.size(); ++index) {
        const std::string line = entry(skills[index]);
        if (out.size() + line.size() > max_index_bytes) {
            break;
        }
        out += line;
    }
    if (index < skills.size()) {
        std::string trailing = note(skills.size() - index);
        while (index > 0 && out.size() + trailing.size() > max_index_bytes) {
            out.resize(out.size() - entry(skills[index - 1]).size());
            --index;
            trailing = note(skills.size() - index);
        }
        if (out.size() + trailing.size() <= max_index_bytes) {
            out += trailing;
        }
    }
    return out;
}

} // namespace

std::filesystem::path default_skills_root() {
    return default_global_config_path().parent_path() / "skills";
}

SkillCatalog::SkillCatalog(SkillCatalogConfig          config,
                           const ExecutionEnvironment& environment,
                           std::filesystem::path       user_root,
                           Logger&                     logger)
    : config_(config),
      environment_(&environment),
      user_root_(std::move(user_root)),
      logger_(&logger) {}

void SkillCatalog::discover() {
    all_.clear();
    model_visible_.clear();
    warnings_.clear();
    index_section_.clear();
    if (!config_.enabled) {
        return;
    }

    std::vector<ScanItem> user_items;
    if (user_root_.is_absolute()) {
        user_items = scan(user_root_, SkillSource::User, SkillTrust::Trusted, config_,
                          environment_, false);
    } else {
        warnings_.push_back(
            SkillLoadWarning{user_root_, "relative user root: user tier disabled"});
    }
    std::vector<ScanItem> workspace_items =
        scan(environment_->root() / ".ymh" / "skills", SkillSource::Workspace,
             SkillTrust::Untrusted, config_, environment_, true);

    std::vector<Skill>                ordered;
    std::map<std::string, std::size_t> by_name;
    const auto consider = [&](ScanItem&& item) {
        for (SkillLoadWarning& warning : item.warnings) {
            warnings_.push_back(std::move(warning));
        }
        if (!item.skill.has_value()) {
            return;
        }
        Skill skill = std::move(*item.skill);
        const auto existing = by_name.find(skill.meta.name.value);
        if (existing != by_name.end()) {
            warnings_.push_back(
                SkillLoadWarning{skill.file, "shadowed by " + ordered[existing->second].file.string()});
            return;
        }
        by_name.emplace(skill.meta.name.value, ordered.size());
        ordered.push_back(std::move(skill));
    };
    for (ScanItem& item : user_items) {
        consider(std::move(item));
    }
    for (ScanItem& item : workspace_items) {
        consider(std::move(item));
    }

    const auto order_key = [](const Skill& skill) {
        const int tier = skill.source == SkillSource::User ? 0 : 1;
        return std::make_tuple(tier, skill.meta.name.value, skill.file.string());
    };
    std::sort(ordered.begin(), ordered.end(), [&](const Skill& left, const Skill& right) {
        return order_key(left) < order_key(right);
    });

    if (ordered.size() > config_.max_skills) {
        for (std::size_t index = config_.max_skills; index < ordered.size(); ++index) {
            warnings_.push_back(SkillLoadWarning{ordered[index].file, "exceeds max_skills"});
        }
        ordered.resize(config_.max_skills);
    }

    all_ = std::move(ordered);
    model_visible_.reserve(all_.size());
    for (const Skill& skill : all_) {
        if (skill.trust == SkillTrust::Trusted || config_.expose_workspace) {
            model_visible_.push_back(&skill);
        }
    }
    index_section_ = build_index(model_visible_, config_.max_index_bytes);

    if (logger_ != nullptr) {
        for (const SkillLoadWarning& warning : warnings_) {
            logger_->warn(join_reason(warning.file, warning.reason));
        }
    }
}

const Skill* SkillCatalog::find(std::string_view name) const noexcept {
    for (const Skill& skill : all_) {
        if (skill.meta.name.value == name) {
            return &skill;
        }
    }
    return nullptr;
}

const Skill* SkillCatalog::find_model_visible(std::string_view name) const noexcept {
    for (const Skill* skill : model_visible_) {
        if (skill->meta.name.value == name) {
            return skill;
        }
    }
    return nullptr;
}

} // namespace ymh

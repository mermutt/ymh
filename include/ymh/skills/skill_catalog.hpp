#pragma once

// SkillCatalog: daemon-owned discovery + immutable store (20-skills.md §5.2).
// `discover()` performs all I/O once at daemon startup; the read accessors are
// const and safe because the vectors are never mutated after discovery.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/skills/skill_roots.hpp"
#include "ymh/skills/skill_types.hpp"

namespace ymh {

struct SkillLoadWarning {
    std::filesystem::path file;
    std::string           reason;
};

struct SkillCatalogConfig {
    bool        enabled = true;
    bool        expose_workspace = false;  // untrusted tier visible to the model
    // 50-D5: the workspace tier is loaded only when the operator has trusted
    // this workspace (a record outside the workspace). Untrusted => skipped.
    bool        workspace_trusted = false;
    std::size_t max_skills = 256;
    std::size_t max_skill_bytes = 64u * 1024u;
    std::size_t max_description_bytes = 512;
    std::size_t max_index_bytes = 8u * 1024u;
    std::size_t max_frontmatter_bytes = 4u * 1024u;
};

class SkillCatalog {
public:
    // 50-D1.2: ordered roots (user-tier candidates first, workspace last),
    // resolved by the caller. `discover()` uses the list verbatim.
    SkillCatalog(SkillCatalogConfig       config,
                 const ExecutionEnvironment& environment,
                 std::vector<SkillRoot>   roots,
                 Logger&                  logger);

    // Non-copyable, non-movable (L7): `model_visible_` stores `const Skill*`
    // into `all_`, so a copy would leave the pointers aliasing the source's
    // vector and a move-assignment could reallocate. The catalog is always held
    // by `shared_ptr` and never copied/moved.
    SkillCatalog(const SkillCatalog&) = delete;
    SkillCatalog& operator=(const SkillCatalog&) = delete;
    SkillCatalog(SkillCatalog&&) = delete;
    SkillCatalog& operator=(SkillCatalog&&) = delete;

    void discover();

    [[nodiscard]] const SkillCatalogConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<Skill>& all() const noexcept { return all_; }
    [[nodiscard]] const std::vector<const Skill*>& model_visible() const noexcept {
        return model_visible_;
    }
    // Two distinct lookups (H2). `find()` searches ALL discovered skills and is
    // used by the user-facing surfaces. `find_model_visible()` searches only
    // `model_visible_` and is the ONLY lookup the model-facing `SkillTool` uses.
    [[nodiscard]] const Skill* find(std::string_view name) const noexcept;
    [[nodiscard]] const Skill* find_model_visible(std::string_view name) const noexcept;
    [[nodiscard]] const std::vector<SkillLoadWarning>& warnings() const noexcept {
        return warnings_;
    }
    // The pinned, always-present system-prompt block (20 §8.1). Empty when
    // disabled or when there are no model-visible skills.
    [[nodiscard]] const std::string& index_section() const noexcept {
        return index_section_;
    }

private:
    SkillCatalogConfig          config_;
    const ExecutionEnvironment* environment_;
    std::vector<SkillRoot>      roots_;
    Logger*                     logger_;
    std::vector<Skill>          all_;
    std::vector<const Skill*>   model_visible_;
    std::vector<SkillLoadWarning> warnings_;
    std::string                 index_section_;
};

// The user skills root: default_global_config_path().parent_path() / "skills".
// May be relative; discovery disables the user tier rather than canonicalizing
// a relative root against the daemon cwd (SK-F13, M2).
[[nodiscard]] std::filesystem::path default_skills_root();

} // namespace ymh

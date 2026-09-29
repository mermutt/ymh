#pragma once

// Workspace trust store (50-skills-mcp-and-session-lifecycle-errata.md §3.2,
// 50-D5). A skill/command file is executable instruction content; a file that
// arrives with `git clone` must not be loaded until the operator trusts the
// workspace. The record lives in ymh's own state directory, NEVER inside the
// workspace, so a cloned repository cannot ship its own trust grant.

#include <filesystem>

namespace ymh {

class WorkspaceTrustStore {
public:
    explicit WorkspaceTrustStore(std::filesystem::path store_path = default_path());

    [[nodiscard]] static std::filesystem::path default_path();

    [[nodiscard]] bool is_trusted(const std::filesystem::path& workspace) const;
    [[nodiscard]] bool trust(const std::filesystem::path& workspace) const;
    [[nodiscard]] bool untrust(const std::filesystem::path& workspace) const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return store_path_; }

private:
    std::filesystem::path store_path_;
};

// 50-D5 as amended by the user decision (2026-09) recorded in spec 50 §6A.2:
// the workspace `.ymh/skills` / `.ymh/commands` tier (whose root is `workspace`)
// is trusted BY DEFAULT at the two locations ymh trusts — the global config root
// (`$HOME/.config/ymh`; already `SkillTrust::Trusted` in `skill_roots.cpp`) and
// the directory ymh was launched in — rather than requiring an opt-in grant.
// Returns true when `workspace` canonically equals `launch_dir` or
// `global_config_root`, or when the explicit `WorkspaceTrustStore` names it.
// Any other workspace stays fail-closed (the store remains the only grant path).
[[nodiscard]] bool workspace_tier_trusted(const std::filesystem::path& workspace,
                                          const std::filesystem::path& launch_dir,
                                          const std::filesystem::path& global_config_root,
                                          const WorkspaceTrustStore& store);

} // namespace ymh

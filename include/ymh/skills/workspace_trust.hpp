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

} // namespace ymh

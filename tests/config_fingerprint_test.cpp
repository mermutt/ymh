#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_support.hpp"

#include "ymh/config/config.hpp"

namespace {

using ymh_test::TempWorkspace;

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

TEST(ConfigFingerprint, ContentChangeChangesTheDigest) {
    TempWorkspace workspace;
    const auto    global    = workspace.root / "config" / "config.jsonc";
    const auto    workspace_config = workspace.root / ".ymh" / "config.jsonc";
    write_file(global, "{}\n");
    write_file(workspace_config, "{}\n");

    const std::string first = ymh::config_fingerprint(global, workspace_config);
    ASSERT_EQ(first.size(), 16U);
    EXPECT_EQ(first, ymh::config_fingerprint(global, workspace_config));

    write_file(global, "{ /* changed */ }\n");
    const std::string second = ymh::config_fingerprint(global, workspace_config);
    EXPECT_NE(first, second);

    write_file(global, "{}\n");
    write_file(workspace_config, "{ \"agent\": {} }\n");
    const std::string third = ymh::config_fingerprint(global, workspace_config);
    EXPECT_NE(first, third);
}

TEST(ConfigFingerprint, WorkspacePermissionsChangeChangesTheDigest) {
    TempWorkspace workspace;
    const auto    global = workspace.root / "config" / "config.jsonc";
    const auto    workspace_config = workspace.root / ".ymh" / "config.jsonc";
    write_file(global, "{}\n");
    write_file(workspace_config, "{}\n");

    const std::string first = ymh::config_fingerprint(global, workspace_config);
    write_file(workspace.root / ".ymh" / "permissions.jsonc", "{ \"allow\": [] }\n");
    const std::string second = ymh::config_fingerprint(global, workspace_config);
    EXPECT_NE(first, second);
}

TEST(ConfigFingerprint, AbsentWorkspaceLayerIsDistinctFromAnEmptyFile) {
    TempWorkspace workspace;
    const auto    global = workspace.root / "config" / "config.jsonc";
    write_file(global, "{}\n");

    const std::string absent = ymh::config_fingerprint(global, workspace.root / ".ymh" / "config.jsonc");
    ASSERT_EQ(absent.size(), 16U);

    write_file(workspace.root / ".ymh" / "config.jsonc", "");
    const std::string empty = ymh::config_fingerprint(global, workspace.root / ".ymh" / "config.jsonc");
    EXPECT_NE(absent, empty);
}

TEST(ConfigFingerprint, MissingGlobalConfigIsUnknown) {
    TempWorkspace workspace;
    EXPECT_TRUE(ymh::config_fingerprint(workspace.root / "missing.jsonc", {}).empty());
}

} // namespace

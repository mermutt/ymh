#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_support.hpp"

#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/filesystem.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;
using ymh_test::TempWorkspace;

std::filesystem::path unique_temp_dir(std::string_view label) {
    return canonical_tmp_root() /
           ("ymh-test-" + std::string{label} + "-" + ymh_test::fresh_uuid());
}

PermissionRequest write_request(const std::filesystem::path& root,
                                const std::filesystem::path& target,
                                SandboxMode mode = SandboxMode::WorkspaceWrite) {
    PermissionRequest request;
    request.tool    = "write_file";
    request.root    = root;
    request.path    = target;
    request.sandbox = mode;
    return request;
}

TEST(TmpFileAccess, PolicyAllowsTempTargetsWithoutAPrompt) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    const RulePermissionPolicy policy(config);

    const std::filesystem::path workspace = "/home/ymh-test/workspace";
    std::string                 reason;

    PermissionRequest request =
        write_request(workspace, canonical_tmp_root() / "ymh-scratch" / "parse.py");
    EXPECT_EQ(policy.evaluate(request, &reason), PolicyVerdict::Allow);
    EXPECT_TRUE(reason.empty());

    request.path = workspace / "src" / "main.cpp";
    EXPECT_EQ(policy.evaluate(request, &reason), PolicyVerdict::Ask);

    request.path    = canonical_tmp_root() / "ymh-scratch" / "parse.py";
    request.sandbox = SandboxMode::ReadOnly;
    EXPECT_EQ(policy.evaluate(request, &reason), PolicyVerdict::Deny);
}

TEST(TmpFileAccess, ExplicitDenyStillWins) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;

    PolicyRule deny;
    deny.tool   = "write_file";
    deny.effect = PolicyVerdict::Deny;
    deny.layer  = PolicyRule::Layer::Global;
    deny.id     = "test.deny.write_file";
    config.rules.push_back(deny);

    const RulePermissionPolicy policy(config);
    EXPECT_EQ(policy.evaluate(write_request(
                  "/home/ymh-test/workspace",
                  canonical_tmp_root() / "ymh-scratch" / "parse.py")),
              PolicyVerdict::Deny);
}

TEST(TmpFileAccess, EnvironmentReadsAndWritesTempFiles) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    TempWorkspace workspace;
    LocalEnvironment environment{workspace.root};  // workspace-write

    const std::filesystem::path dir = unique_temp_dir("write");
    std::filesystem::remove_all(dir);
    const std::filesystem::path target = environment.resolve((dir / "parse.py").string());
    environment.fs().write(target, Data{"print('hi')\n"}).get();
    ASSERT_TRUE(std::filesystem::is_regular_file(target));

    const FileData read = environment.fs().read(target).get();
    EXPECT_EQ(read.bytes, "print('hi')\n");

    std::filesystem::remove_all(dir);
}

TEST(TmpFileAccess, ReadOnlyDoesNotGainTheTempRoot) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    TempWorkspace    workspace;
    LocalEnvironment environment{workspace.root, SandboxMode::ReadOnly};
    const std::filesystem::path target = canonical_tmp_root() / "ymh-scratch" / "parse.py";
    EXPECT_THROW((void)environment.resolve(target.string()), ToolError);
}

TEST(TmpFileAccess, SymlinkOutOfTempIsRejected) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    TempWorkspace    workspace;
    LocalEnvironment environment{workspace.root};

    const std::filesystem::path dir = unique_temp_dir("link");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path link = dir / "escape";
    std::filesystem::create_directory_symlink("/etc", link);

    EXPECT_THROW((void)environment.resolve((link / "passwd").string()), ToolError);
    std::filesystem::remove_all(dir);
}

TEST(TmpFileAccess, TempSymlinkIntoTheWorkspaceDoesNotBypassTheRootScope) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    TempWorkspace    workspace;
    LocalEnvironment environment{workspace.root};

    const std::filesystem::path dir = unique_temp_dir("workspace-link");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path link = dir / "workspace";
    std::filesystem::create_directory_symlink(workspace.root, link);

    EXPECT_THROW((void)environment.resolve((link / "escape.txt").string()), ToolError);
    std::filesystem::remove_all(dir);
}

} // namespace

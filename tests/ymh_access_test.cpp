#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_support.hpp"

#include "ymh/execution/config.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/filesystem.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using ymh::GlobQuery;
using ymh::LocalFilesystem;
using ymh::PermissionConfig;
using ymh::PermissionRequest;
using ymh::PolicyVerdict;
using ymh::RulePermissionPolicy;
using ymh::SandboxMode;
using ymh::ToolConfig;
using ymh_test::TempWorkspace;

PermissionRequest path_request(const std::filesystem::path& root,
                               const std::string& tool, const std::filesystem::path& path) {
    PermissionRequest request;
    request.tool = tool;
    request.root = root;
    request.path = path;
    request.sandbox = SandboxMode::WorkspaceWrite;
    return request;
}

PermissionRequest shell_request(const std::filesystem::path& root, const std::string& command) {
    PermissionRequest request;
    request.tool      = "shell";
    request.root      = root;
    request.arguments = nlohmann::json{{"command", command}};
    request.sandbox   = SandboxMode::WorkspaceWrite;
    return request;
}

PermissionConfig permissive() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Allow;
    return config;
}

TEST(YmhMetadataAccess, RootMetadataStaysProtected) {
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.root / ".ymh");
    std::filesystem::create_directories(workspace.root / ".git");
    const auto log = workspace.root / ".ymh" / "host.log";
    {
        std::ofstream stream{log};
        stream << "debug\n";
    }

    const RulePermissionPolicy policy(permissive());
    EXPECT_EQ(policy.evaluate(path_request(workspace.root, "read_file", log)),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(path_request(workspace.root, "write_file", log)),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(
                  path_request(workspace.root, "read_file", workspace.root / ".git" / "config")),
              PolicyVerdict::Deny);
}

TEST(YmhMetadataAccess, NestedYmhIsOrdinaryContentButGitStaysProtected) {
    TempWorkspace workspace;
    const auto    nested = workspace.root / "project";
    std::filesystem::create_directories(nested / ".ymh");
    std::filesystem::create_directories(nested / ".git");
    const auto config = nested / ".ymh" / "permissions.jsonc";
    {
        std::ofstream stream{config};
        stream << "{}\n";
    }

    const RulePermissionPolicy policy(permissive());
    EXPECT_EQ(policy.evaluate(path_request(workspace.root, "read_file", config)),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(path_request(workspace.root, "write_file", config)),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(
                  path_request(workspace.root, "read_file", nested / ".git" / "config")),
              PolicyVerdict::Deny);
}

TEST(YmhMetadataAccess, ShellClassifierProtectsOnlyTheRootMetadata) {
    TempWorkspace workspace;
    const auto    nested = workspace.root / "project";
    std::filesystem::create_directories(nested / ".ymh");
    std::filesystem::create_directories(nested / ".git");

    const RulePermissionPolicy policy(permissive());
    EXPECT_EQ(policy.evaluate(shell_request(workspace.root, "ls -la .ymh")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.root, "ls -la project/.ymh")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(shell_request(workspace.root, "cat project/.git/config")),
              PolicyVerdict::Deny);
}

TEST(YmhMetadataAccess, GlobTraversesNestedMetadataButNotRootMetadata) {
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.root / ".ymh");
    std::filesystem::create_directories(workspace.root / "project" / ".ymh");
    {
        std::ofstream stream{workspace.root / ".ymh" / "host.log"};
        stream << "debug\n";
    }
    {
        std::ofstream stream{workspace.root / "project" / ".ymh" / "config.jsonc"};
        stream << "{}\n";
    }

    ToolConfig config;
    LocalFilesystem fs(workspace.root, config);

    GlobQuery protected_query;
    protected_query.base    = workspace.root / ".ymh";
    protected_query.pattern = "**";
    EXPECT_THROW((void)fs.glob(protected_query).get(), ymh::ToolError);

    GlobQuery nested_query;
    nested_query.base    = workspace.root / "project" / ".ymh";
    nested_query.pattern = "**";
    const auto results = fs.glob(nested_query).get();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().filename(), "config.jsonc");
}

} // namespace

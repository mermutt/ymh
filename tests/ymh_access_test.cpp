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

PermissionConfig permissive(bool allow_ymh_access) {
    PermissionConfig config;
    config.default_verdict  = PolicyVerdict::Allow;
    config.allow_ymh_access = allow_ymh_access;
    return config;
}

struct PathCases {
    PermissionRequest ymh_read;
    PermissionRequest ymh_write;
    PermissionRequest git_read;
};

TEST(YmhAccessSwitch, PathToolsStayProtectedUntilEnabled) {
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.root / ".ymh");
    std::filesystem::create_directories(workspace.root / ".git");
    const auto log = workspace.root / ".ymh" / "host.log";
    {
        std::ofstream stream{log};
        stream << "debug\n";
    }

    const PathCases cases{
        path_request(workspace.root, "read_file", log),
        path_request(workspace.root, "write_file", log),
        path_request(workspace.root, "read_file", workspace.root / ".git" / "config"),
    };

    const RulePermissionPolicy protected_policy(permissive(false));
    EXPECT_EQ(protected_policy.evaluate(cases.ymh_read), PolicyVerdict::Deny);
    EXPECT_EQ(protected_policy.evaluate(cases.ymh_write), PolicyVerdict::Deny);
    EXPECT_EQ(protected_policy.evaluate(cases.git_read), PolicyVerdict::Deny);

    const RulePermissionPolicy open_policy(permissive(true));
    EXPECT_EQ(open_policy.evaluate(cases.ymh_read), PolicyVerdict::Allow);
    EXPECT_EQ(open_policy.evaluate(cases.ymh_write), PolicyVerdict::Allow);
    EXPECT_EQ(open_policy.evaluate(cases.git_read), PolicyVerdict::Deny);
}

TEST(YmhAccessSwitch, ShellClassifierHonoursTheSwitchButKeepsGitProtected) {
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.root / ".ymh");
    std::filesystem::create_directories(workspace.root / ".git");

    const RulePermissionPolicy protected_policy(permissive(false));
    const RulePermissionPolicy open_policy(permissive(true));

    PermissionRequest ymh = shell_request(workspace.root, "ls -la .ymh");
    PermissionRequest git = shell_request(workspace.root, "cat .git/config");

    EXPECT_EQ(protected_policy.evaluate(ymh), PolicyVerdict::Deny);
    PermissionRequest ymh_open = ymh;
    ymh_open.allow_ymh_access = true;
    PermissionRequest git_open = git;
    git_open.allow_ymh_access = true;
    EXPECT_EQ(open_policy.evaluate(ymh_open), PolicyVerdict::Allow);
    EXPECT_EQ(open_policy.evaluate(git_open), PolicyVerdict::Deny);
}

TEST(YmhAccessSwitch, GlobTraversesYmhOnlyWhenEnabled) {
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.root / ".ymh");
    {
        std::ofstream stream{workspace.root / ".ymh" / "host.log"};
        stream << "debug\n";
    }

    GlobQuery query;
    query.base    = workspace.root / ".ymh";
    query.pattern = "**";

    ToolConfig closed;
    LocalFilesystem protected_fs(workspace.root, closed);
    EXPECT_THROW(protected_fs.glob(query).get(), ymh::ToolError);

    ToolConfig open;
    open.allow_ymh_access = true;
    LocalFilesystem open_fs(workspace.root, open);
    const auto results = open_fs.glob(query).get();
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().filename(), "host.log");
}
} // namespace

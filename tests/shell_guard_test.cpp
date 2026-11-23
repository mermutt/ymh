#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_support.hpp"

#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using ymh::PermissionRequest;
using ymh::ShellClassification;
using ymh_test::TempWorkspace;

ShellClassification classify(const std::filesystem::path& root, std::string command) {
    PermissionRequest request;
    request.tool      = "shell";
    request.arguments = nlohmann::json{{"command", std::move(command)}};
    request.root      = root;
    request.roots     = {root};
    return ymh::classify_shell_request(request);
}

TEST(ShellGuard, DotYmhInRegexIsNotAPath) {
    TempWorkspace workspace;
    const ShellClassification classification = classify(
        workspace.root, "cd " + workspace.root.string() +
                            " && grep -n \"foo|.ymh\" docs/file.md | head -20");
    EXPECT_FALSE(classification.path_protected);
}

TEST(ShellGuard, ExactSecondCommandWithDotLlmPathIsNotProtected) {
    TempWorkspace workspace;
    const ShellClassification classification = classify(
        workspace.root,
        "cd " + workspace.root.string() +
            " && grep -nE '^###.*(6\\.4|p\\.)[0-9]*' "
            "'.llm/wiki/pages/project/dld/euclid/nvme_completion_status/nvme_completion_status.md'"
            " | head -25");
    EXPECT_FALSE(classification.path_protected);
}

TEST(ShellGuard, ExactSecondCommandWithDotYmhPathIsProtected) {
    TempWorkspace workspace;
    const ShellClassification classification = classify(
        workspace.root,
        "cd " + workspace.root.string() +
            " && grep -nE '^###.*(6\\.4|p\\.)[0-9]*' "
            "'.ymh/wiki/pages/project/dld/euclid/nvme_completion_status/nvme_completion_status.md'"
            " | head -25");
    EXPECT_TRUE(classification.path_protected);
}

TEST(ShellGuard, DotYmhPathIsProtected) {
    TempWorkspace workspace;
    const ShellClassification classification = classify(
        workspace.root, "grep -n foo " + workspace.root.string() + "/.ymh/wiki/file.md");
    EXPECT_TRUE(classification.path_protected);
}

} // namespace

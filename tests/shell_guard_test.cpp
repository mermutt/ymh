#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_support.hpp"

#include "ymh/policy/permission_policy.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using ymh::PermissionConfig;
using ymh::PermissionRequest;
using ymh::PolicyRule;
using ymh::PolicyVerdict;
using ymh::RulePermissionPolicy;
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

TEST(ShellGuard, ExactSecondCommandWithDotYmhPathIsReadable) {
    TempWorkspace workspace;
    const ShellClassification classification = classify(
        workspace.root,
        "cd " + workspace.root.string() +
            " && grep -nE '^###.*(6\\.4|p\\.)[0-9]*' "
            "'.ymh/wiki/pages/project/dld/euclid/nvme_completion_status/nvme_completion_status.md'"
            " | head -25");
    EXPECT_FALSE(classification.path_protected);
}

ymh::PolicyVerdict evaluate_shell(const std::filesystem::path& root,
                                  std::string command, bool allow_shell = true) {
    PermissionConfig config;
    if (allow_shell) {
        PolicyRule rule;
        rule.tool   = "shell";
        rule.effect = PolicyVerdict::Allow;
        rule.id     = "shell.allow";
        config.rules.push_back(std::move(rule));
    }
    RulePermissionPolicy policy(std::move(config));
    PermissionRequest    request;
    request.tool      = "shell";
    request.arguments = nlohmann::json{{"command", std::move(command)}};
    request.root      = root;
    request.roots     = {root};
    return policy.evaluate(request);
}

TEST(ShellGuard, SafeYmhReadIsAllowedByShellRule) {
    TempWorkspace workspace;
    const PolicyVerdict verdict = evaluate_shell(
        workspace.root,
        "cd " + workspace.root.string() +
            " && grep -n \"foo|.ymh\" '.ymh/wiki/pages/file.md' | head -20");
    EXPECT_EQ(verdict, PolicyVerdict::Allow);
}

TEST(ShellGuard, SafeYmhReadWithoutAShellRuleAsks) {
    TempWorkspace workspace;
    EXPECT_EQ(evaluate_shell(workspace.root, "cat .ymh/wiki/file.md", /*allow_shell=*/false),
              PolicyVerdict::Ask);
}

TEST(ShellGuard, NestedYmhUserContentIsReadable) {
    TempWorkspace workspace;
    EXPECT_EQ(evaluate_shell(workspace.root, "grep -n foo .ymh/wiki/a/b.md"),
              PolicyVerdict::Allow);
    EXPECT_EQ(evaluate_shell(workspace.root, "cat .ymh/skills/x/SKILL.md"),
              PolicyVerdict::Allow);
    EXPECT_EQ(evaluate_shell(workspace.root, "find .ymh/wiki -type f"),
              PolicyVerdict::Allow);
}

TEST(ShellGuard, UnknownYmhSubdirectoryStaysDenied) {
    TempWorkspace workspace;
    EXPECT_EQ(evaluate_shell(workspace.root, "cat .ymh/notes/file.md"),
              PolicyVerdict::Deny);
}

TEST(ShellGuard, SensitiveYmhReadsStayDenied) {
    TempWorkspace workspace;
    const std::string root = workspace.root.string();
    for (const std::string& target :
         {".ymh/sessions/abc/events.jsonl", ".ymh/sessions.lock", ".ymh/config.jsonc",
          ".ymh/permissions.jsonc", ".ymh/host.log", ".ymh/checkpoints/x",
          ".ymh/transcripts/t.jsonl", ".ymh/handoffs/h.md", ".ymh/mcp/m.stderr.log"}) {
        EXPECT_EQ(evaluate_shell(workspace.root, "cat " + target), PolicyVerdict::Deny)
            << target;
    }
    EXPECT_EQ(evaluate_shell(workspace.root, "grep -r foo .ymh"), PolicyVerdict::Deny);
    EXPECT_EQ(evaluate_shell(workspace.root, "grep -r foo .ymh/wiki/*"), PolicyVerdict::Deny);
    EXPECT_EQ(evaluate_shell(workspace.root, "cd " + root + " && cat .ymh/sessions/x"),
              PolicyVerdict::Deny);
}

TEST(ShellGuard, YmhWritesStayDenied) {
    TempWorkspace workspace;
    EXPECT_EQ(evaluate_shell(workspace.root, "echo hi > .ymh/wiki/file.md"),
              PolicyVerdict::Deny);
    EXPECT_EQ(evaluate_shell(workspace.root, "cp foo .ymh/wiki/file.md"),
              PolicyVerdict::Deny);
    EXPECT_EQ(evaluate_shell(workspace.root, "rm .ymh/wiki/file.md"), PolicyVerdict::Deny);
}

TEST(ShellGuard, DotYmhMetadataPathIsProtected) {
    TempWorkspace workspace;
    const ShellClassification classification = classify(
        workspace.root,
        "grep -n foo " + workspace.root.string() + "/.ymh/sessions/abc/events.jsonl");
    EXPECT_TRUE(classification.path_protected);
}

} // namespace

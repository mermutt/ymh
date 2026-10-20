#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "support/test_env.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;



PermissionRequest request(const std::filesystem::path& root, std::string tool,
                          std::optional<std::filesystem::path> path = std::nullopt,
                          nlohmann::json arguments = nlohmann::json::object(),
                          bool kernel_confinement = true) {
    PermissionRequest value;
    value.call    = "call-1";
    value.session = SessionId{"s1"};
    value.tool    = std::move(tool);
    value.root    = root;
    value.path    = std::move(path);
    value.arguments = std::move(arguments);
    value.kernel_confinement = kernel_confinement;
    return value;
}

void approve(RulePermissionPolicy& policy, const PermissionRequest& req,
             GrantScope scope = GrantScope::Session) {
    policy.remember(req, payload::PermissionDecisionKind::Allow, scope);
}

} // namespace

TEST(Session, PathToolCoversInRootClass) {
    ymh::test::TempWorkspace workspace("p90_sess_path");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto first = request(root, "write_file", root / "src" / "x");
    EXPECT_EQ(policy.evaluate(first), PolicyVerdict::Ask);
    approve(policy, first);
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / "docs" / "y")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / ".." / "x")),
              PolicyVerdict::Ask);
}

TEST(Session, PathToolOutsideCreatesNoGrant) {
    ymh::test::TempWorkspace workspace("p90_sess_path_out");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto outside = request(root, "write_file", root / ".." / "x");
    approve(policy, outside);
    EXPECT_EQ(policy.evaluate(outside), PolicyVerdict::Ask);
}

TEST(Session, ShellPrefixCoversGitSubcommands) {
    ymh::test::TempWorkspace workspace("p90_sess_prefix");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt, {{"command", "git status"}}));
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "git diff"}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "ls"}})),
              PolicyVerdict::Ask);
}

TEST(Session, ShellOutsideStaysExact) {
    ymh::test::TempWorkspace workspace("p90_sess_outside");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt,
                            {{"command", "cat /etc/passwd"}}));
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "cat /etc/shadow"}})),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "cat /etc/passwd"}})),
              PolicyVerdict::Allow);
}

TEST(Session, ShellUnknownStaysExact) {
    ymh::test::TempWorkspace workspace("p90_sess_unknown");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt,
                            {{"command", "python3 -c 'x'"}}));
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "python3 -c 'y'"}})),
              PolicyVerdict::Ask);
}

TEST(Session, GuardConfirmNotSuppressed) {
    ymh::test::TempWorkspace workspace("p90_sess_confirm");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto req = request(root, "shell", std::nullopt, {{"command", "rm -rf build"}},
                             false);
    approve(policy, req);
    EXPECT_EQ(policy.evaluate(req), PolicyVerdict::Ask);
}

TEST(Session, GuardForbidNotSuppressed) {
    ymh::test::TempWorkspace workspace("p90_sess_forbid");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt, {{"command", "rm -rf ~"}}));
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "rm -rf ~"}})),
              PolicyVerdict::Deny);
}

TEST(Session, LostOnRestart) {
    ymh::test::TempWorkspace workspace("p90_sess_restart");
    const auto root = workspace.path();
    {
        RulePermissionPolicy policy(PermissionConfig{});
        approve(policy, request(root, "write_file", root / "src" / "x"));
    }
    RulePermissionPolicy restarted(PermissionConfig{});
    EXPECT_EQ(restarted.evaluate(request(root, "write_file", root / "src" / "x")),
              PolicyVerdict::Ask);
}

TEST(Session, AlwaysUnchangedExact) {
    ymh::test::TempWorkspace workspace("p90_sess_always");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt, {{"command", "git status"}}),
            GrantScope::Always);
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "git diff"}})),
              PolicyVerdict::Ask);
}

TEST(Session, GrantIdsDifferByClass) {
    ymh::test::TempWorkspace workspace("p90_sess_ids");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto in_root = request(root, "shell", std::nullopt,
                                 {{"command", "cat x"}});
    const auto outside = request(root, "shell", std::nullopt,
                                 {{"command", "cat /etc/passwd"}});
    const PolicyRule a = policy.remember(in_root, payload::PermissionDecisionKind::Allow,
                                         GrantScope::Session);
    const PolicyRule b = policy.remember(outside, payload::PermissionDecisionKind::Allow,
                                         GrantScope::Session);
    EXPECT_NE(a.id, b.id);
    EXPECT_EQ(a.target_class, TargetClass::InRoot);
    EXPECT_EQ(b.target_class, TargetClass::Outside);
}

TEST(Session, NonPathToolIsToolWide) {
    ymh::test::TempWorkspace workspace("p90_sess_mcp");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "mcp.github.x"));
    EXPECT_EQ(policy.evaluate(request(root, "mcp.github.x")), PolicyVerdict::Allow);
}

TEST(Session, TerminalWriteExactInput) {
    ymh::test::TempWorkspace workspace("p90_sess_termwrite");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto first = request(root, "terminal", std::nullopt,
                               {{"action", "write"}, {"input", "ls\n"}});
    approve(policy, first);
    EXPECT_EQ(policy.evaluate(first), PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "terminal", std::nullopt,
                                      {{"action", "write"}, {"input", "ls -la\n"}})),
              PolicyVerdict::Ask);
}

TEST(Session, TerminalWriteNoPrefix) {
    ymh::test::TempWorkspace workspace("p90_sess_termprefix");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const PolicyRule grant = policy.remember(
        request(root, "terminal", std::nullopt,
                {{"action", "write"}, {"input", "ls\n"}}),
        payload::PermissionDecisionKind::Allow, GrantScope::Session);
    EXPECT_FALSE(grant.prefix);
    EXPECT_EQ(grant.target_class, TargetClass::Unknown);
}

TEST(Session, CompoundCommandNoPrefix) {
    ymh::test::TempWorkspace workspace("p90_sess_compound");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt, {{"command", "git status"}}));
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "git status && rm -rf src"}})),
              PolicyVerdict::Ask);
}

TEST(Session, CompoundCommandExact) {
    ymh::test::TempWorkspace workspace("p90_sess_compound_exact");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto req = request(root, "shell", std::nullopt,
                             {{"command", "git status && ls"}});
    const PolicyRule grant =
        policy.remember(req, payload::PermissionDecisionKind::Allow, GrantScope::Session);
    EXPECT_FALSE(grant.prefix);
    EXPECT_TRUE(grant.literal);
    EXPECT_EQ(policy.evaluate(req), PolicyVerdict::Allow);
}

TEST(Session, ProtectedCreatesNoGrant) {
    ymh::test::TempWorkspace workspace("p90_sess_prot");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    const auto req = request(root, "write_file", root / ".ymh" / "config.jsonc");
    EXPECT_TRUE(policy.remember(req, payload::PermissionDecisionKind::Allow,
                                GrantScope::Session)
                    .tool.empty());
    EXPECT_EQ(policy.evaluate(req), PolicyVerdict::Deny);
}

TEST(Session, AlwaysPathToolInRoot) {
    ymh::test::TempWorkspace workspace("p90_sess_always_path");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "write_file", root / "src" / "x"), GrantScope::Always);
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / "docs" / "y")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / ".." / "x")),
              PolicyVerdict::Ask);
}

TEST(Session, WrapperPrefixShared) {
    ymh::test::TempWorkspace workspace("p90_sess_wrapper");
    RulePermissionPolicy policy(PermissionConfig{});
    const auto root = workspace.path();
    approve(policy, request(root, "shell", std::nullopt, {{"command", "git status"}}));
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "sudo git status"}})),
              PolicyVerdict::Allow);
}

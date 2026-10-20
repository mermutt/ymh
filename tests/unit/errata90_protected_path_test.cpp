#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/policy/path_target.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/terminal_tool.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;

PolicyRule rule(std::string tool, std::string path, PolicyVerdict effect,
                std::string id = "rule") {
    PolicyRule value;
    value.tool   = std::move(tool);
    value.path   = std::move(path);
    value.effect = effect;
    value.layer  = PolicyRule::Layer::Global;
    value.id     = std::move(id);
    return value;
}

PermissionConfig recommended() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(rule("*", "**", PolicyVerdict::Allow, "workspace.in-root"));
    config.rules.push_back(rule("*", ".ymh/**", PolicyVerdict::Deny, "protected.ymh"));
    config.rules.push_back(rule("*", ".git/**", PolicyVerdict::Deny, "protected.git"));
    return config;
}

PermissionConfig allow_all() {
    PermissionConfig config = recommended();
    config.default_verdict = PolicyVerdict::Allow;
    return config;
}

PermissionRequest request(const std::filesystem::path& root, std::string tool,
                          std::optional<std::filesystem::path> path = std::nullopt,
                          nlohmann::json arguments = nlohmann::json::object()) {
    PermissionRequest value;
    value.call    = "call-1";
    value.session = SessionId{"s1"};
    value.tool    = std::move(tool);
    value.root    = root;
    value.path    = std::move(path);
    value.arguments = std::move(arguments);
    value.kernel_confinement = true;
    return value;
}

PolicyVerdict eval(const std::filesystem::path& root, const std::string& command,
                   const PermissionConfig& config) {
    RulePermissionPolicy policy(config);
    return policy.evaluate(request(root, "shell", std::nullopt, {{"command", command}}));
}

// namespace continues

TEST(Protected, ConfigWriteDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_cfg");
    RulePermissionPolicy policy(recommended());
    const auto root = workspace.path();
    const auto req = request(root, "write_file", root / ".ymh" / "config.jsonc");
    EXPECT_EQ(policy.evaluate(req), PolicyVerdict::Deny);
    EXPECT_TRUE(policy.remember(req, payload::PermissionDecisionKind::Allow,
                                GrantScope::Session)
                    .tool.empty());
}

TEST(Protected, GrantWriteDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_grant");
    RulePermissionPolicy policy(recommended());
    const auto root = workspace.path();
    EXPECT_EQ(policy.evaluate(request(root, "write_file",
                                      root / ".ymh" / "permissions.jsonc")),
              PolicyVerdict::Deny);
}

TEST(Protected, GitHookDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_hook");
    const auto root = workspace.path();
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(root, "write_file",
                                      root / ".git" / "hooks" / "pre-commit")),
              PolicyVerdict::Deny);
    EXPECT_EQ(eval(root, "chmod +x .git/hooks/pre-commit", recommended()),
              PolicyVerdict::Deny);
}

TEST(Protected, SessionWriteDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_session");
    const auto root = workspace.path();
    RulePermissionPolicy policy(allow_all());
    EXPECT_EQ(policy.evaluate(request(root, "read_file", root / ".ymh" / "sessions.db")),
              PolicyVerdict::Deny);
}

TEST(Protected, PolicyBeatsDefaultAllow) {
    ymh::test::TempWorkspace workspace("p90_prot_allow");
    const auto root = workspace.path();
    RulePermissionPolicy policy(allow_all());
    EXPECT_EQ(policy.evaluate(request(root, "write_file",
                                      root / ".ymh" / "config.jsonc")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(request(root, "read_file",
                                      root / ".git" / "config")),
              PolicyVerdict::Deny);
    EXPECT_EQ(eval(root, "chmod +x .git/hooks/x", allow_all()), PolicyVerdict::Deny);
}

TEST(Protected, GrepGlobSkip) {
    ymh::test::TempWorkspace workspace("p90_prot_skip");
    workspace.write(".ymh/secret.txt", "needle");
    workspace.write(".git/config", "needle");
    workspace.write("src/visible.txt", "needle");
    RulePermissionPolicy policy(recommended());
    const auto req = request(workspace.path(), "grep", workspace.path(),
                             {{"pattern", "needle"}});
    ASSERT_EQ(policy.evaluate(req), PolicyVerdict::Allow);
}

TEST(Protected, NestedGitDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_nested");
    const auto root = workspace.path();
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(root, "write_file",
                                      root / "src" / ".git" / "hooks" / "x")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(request(root, "write_file",
                                      root / "src" / ".ymh" / "config.jsonc")),
              PolicyVerdict::Deny);
}

TEST(Protected, GitWildcardDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_glob");
    const auto root = workspace.path();
    EXPECT_EQ(eval(root, "rm -rf .ymh* .git*", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(root, "rm -rf .ym?", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(root, "rm -rf src/.ymh*", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(root, "rm -rf .ymhfoo", recommended()), PolicyVerdict::Allow);
    EXPECT_EQ(eval(root, "rm -rf *", recommended()), PolicyVerdict::Ask);
    EXPECT_EQ(eval(root, "rm -rf /tmp/repo/.git/config", recommended()),
              PolicyVerdict::Deny);
}

TEST(Protected, GitConfigLocalDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_config");
    EXPECT_EQ(eval(workspace.path(), "git config --local core.hooksPath .evil",
                   recommended()),
              PolicyVerdict::Deny);
}

TEST(Protected, GitCleanDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_clean");
    EXPECT_EQ(eval(workspace.path(), "git clean -fdx", recommended()),
              PolicyVerdict::Deny);
}

TEST(Protected, GitGlobalConfigAsks) {
    ymh::test::TempWorkspace workspace("p90_prot_git_global");
    EXPECT_EQ(eval(workspace.path(), "git config --global core.hooksPath .evil",
                   recommended()),
              PolicyVerdict::Ask);
}

TEST(Protected, GitDataPlaneAllowed) {
    ymh::test::TempWorkspace workspace("p90_prot_git_data");
    EXPECT_EQ(eval(workspace.path(), "git add .", recommended()), PolicyVerdict::Allow);
    EXPECT_EQ(eval(workspace.path(), "git commit -m x", recommended()),
              PolicyVerdict::Allow);
}

TEST(Protected, GitStashAllDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_stash");
    EXPECT_EQ(eval(workspace.path(), "git stash -a", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(workspace.path(), "git stash -u", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(workspace.path(), "git stash", recommended()), PolicyVerdict::Allow);
}

TEST(Protected, GitStashClusteredDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_stash_cluster");
    EXPECT_EQ(eval(workspace.path(), "git stash -au", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(workspace.path(), "git stash -ua", recommended()), PolicyVerdict::Deny);
    EXPECT_EQ(eval(workspace.path(), "git stash -uab", recommended()), PolicyVerdict::Deny);
}

TEST(Protected, GitRemoteAddDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_remote");
    EXPECT_EQ(eval(workspace.path(), "git remote add evil file:///tmp/x", recommended()),
              PolicyVerdict::Deny);
}

TEST(Protected, GitAutoTrackDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_track");
    const auto root = workspace.path();
    for (const char* command : {
             "git checkout -b feat origin/main", "git checkout -B feat2 origin/main",
             "git branch feat3 origin/main", "git switch -c feat4 origin/main",
             "git switch -C feat5 origin/main", "git switch -t origin/main",
             "git switch --track origin/main", "git branch -m old new",
             "git branch -M old new", "git branch -c main copyb",
             "git branch -C main copyc", "git checkout -b z1", "git branch z2",
             "git switch -c z3"}) {
        EXPECT_EQ(eval(root, command, recommended()), PolicyVerdict::Deny) << command;
    }
    EXPECT_EQ(eval(root, "git checkout -b z4 --no-track", recommended()),
              PolicyVerdict::Allow);
}

TEST(Protected, GitReadOnlyFormsAllowed) {
    ymh::test::TempWorkspace workspace("p90_prot_git_read");
    for (const char* command : {"git remote -v", "git remote prune",
                                "git submodule status", "git worktree list"}) {
        EXPECT_EQ(eval(workspace.path(), command, recommended()), PolicyVerdict::Allow)
            << command;
    }
}

TEST(Protected, GitFetchSetUpstreamDenied) {
    ymh::test::TempWorkspace workspace("p90_prot_git_fetch");
    EXPECT_EQ(eval(workspace.path(), "git fetch --set-upstream origin main",
                   recommended()),
              PolicyVerdict::Deny);
    EXPECT_EQ(eval(workspace.path(), "git pull --set-upstream origin main",
                   recommended()),
              PolicyVerdict::Deny);
    EXPECT_EQ(eval(workspace.path(), "git fetch -u origin main", recommended()),
              PolicyVerdict::Allow);
}

TEST(Protected, WorkspacePermissionsRejected) {
    ymh::test::TempWorkspace workspace("p90_ws_permissions");
    Config config;
    workspace.write(".ymh/config.jsonc",
                    R"({"permissions":{"default":"allow"}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / ".ymh" / "config.jsonc",
                                  false),
                 ConfigError);
}

TEST(Protected, WorkspaceConfigPersistenceEscalation) {
    ymh::test::TempWorkspace workspace("p90_ws_persist");
    workspace.write(".ymh/config.jsonc",
                    R"({"permissions":{"default":"allow"}})");
    Config config;
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / ".ymh" / "config.jsonc",
                                  false),
                 ConfigError);
    EXPECT_NE(config.permissions.default_verdict, "allow");
}

TEST(Protected, CanonicalSymlinkAlias) {
    ymh::test::ToolEnv env("p90_tool_alias", SandboxMode::Unrestricted);
    env.workspace.write(".ymh/keep.txt", "x");
    std::error_code error;
    std::filesystem::create_directory_symlink(env.workspace.path() / ".ymh",
                                              env.workspace.path() / ".work", error);
    ASSERT_FALSE(error) << error.message();
    auto tool = make_write_file_tool(ToolConfig{});
    ToolArguments arguments;
    arguments.value = {{"path", ".work/config.jsonc"}, {"content", "{}"}};
    EXPECT_THROW(tool->execute(env.context(), arguments).get(), ToolError);
}

TEST(Protected, TerminalOpenCwdToolRejectsOutside) {
    ymh::test::ToolEnv env("p90_tool_terminal", SandboxMode::Unrestricted);
    auto tool = make_terminal_tool(ToolConfig{});
    ToolArguments arguments;
    arguments.value = {{"action", "open"}, {"cwd", "/tmp"}, {"command", "true"}};
    try {
        tool->execute(env.context(), arguments).get();
        FAIL() << "expected a protected/escape rejection";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }
}

} // namespace

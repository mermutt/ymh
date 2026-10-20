#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

PolicyRule path_rule(std::string tool, std::string path, PolicyVerdict effect,
                     std::string id = "rule") {
    PolicyRule rule;
    rule.tool   = std::move(tool);
    rule.path   = std::move(path);
    rule.effect = effect;
    rule.layer  = PolicyRule::Layer::Global;
    rule.id     = std::move(id);
    return rule;
}

PermissionConfig recommended() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(path_rule("*", "**", PolicyVerdict::Allow, "workspace.in-root"));
    config.rules.push_back(
        path_rule("*", ".ymh/**", PolicyVerdict::Deny, "protected.ymh"));
    config.rules.push_back(
        path_rule("*", ".git/**", PolicyVerdict::Deny, "protected.git"));
    return config;
}

PermissionRequest request(const std::filesystem::path& root, std::string tool,
                          std::optional<std::filesystem::path> path = std::nullopt,
                          nlohmann::json arguments = nlohmann::json::object(),
                          bool kernel_confinement = true, bool force_ask = false) {
    PermissionRequest value;
    value.call    = "call-1";
    value.session = SessionId{"s1"};
    value.tool    = std::move(tool);
    value.root    = root;
    value.path    = std::move(path);
    value.arguments = std::move(arguments);
    value.sandbox = SandboxMode::Workspace;
    value.kernel_confinement = kernel_confinement;
    value.force_ask = force_ask;
    return value;
}

PermissionConfig with_read(PolicyVerdict read) {
    PermissionConfig config = recommended();
    config.rules.push_back(path_rule("read_file", "", read, "default.read_file"));
    config.rules.push_back(path_rule("grep", "", read, "default.grep"));
    config.rules.push_back(path_rule("glob", "", read, "default.glob"));
    return config;
}

void write_config(const std::filesystem::path& path, const std::string& body) {
    std::ofstream(path, std::ios::binary) << body;
}

} // namespace

TEST(PathRule, InRootMatches) {
    ymh::test::TempWorkspace workspace("p90_rule_inroot");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "write_file",
                                      workspace.path() / "src" / "x")),
              PolicyVerdict::Allow);
}

TEST(PathRule, OutsideDotDotDoesNotMatch) {
    ymh::test::TempWorkspace workspace("p90_rule_dotdot");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "write_file",
                                      workspace.path() / ".." / "x")),
              PolicyVerdict::Ask);
}

TEST(PathRule, AbsoluteOutsideDoesNotMatch) {
    ymh::test::TempWorkspace workspace("p90_rule_abs");
    RulePermissionPolicy policy(with_read(PolicyVerdict::Ask));
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "read_file", "/etc/passwd")),
              PolicyVerdict::Ask);
}

TEST(PathRule, RootMatchesDoubleStar) {
    ymh::test::TempWorkspace workspace("p90_rule_root");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "grep", workspace.path())),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "grep",
                                      workspace.path() / "src")),
              PolicyVerdict::Allow);
}

TEST(PathRule, NarrowPatternDoesNotMatchShell) {
    ymh::test::TempWorkspace workspace("p90_rule_narrow_shell");
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(path_rule("shell", "src/**", PolicyVerdict::Allow));
    RulePermissionPolicy policy(config);
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "shell", std::nullopt,
                                      {{"command", "ls"}})),
              PolicyVerdict::Ask);
}

TEST(PathRule, NonPathToolDoesNotMatch) {
    ymh::test::TempWorkspace workspace("p90_rule_mcp");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "mcp.github.x")),
              PolicyVerdict::Ask);
}

TEST(PathRule, MissingPathDoesNotMatch) {
    ymh::test::TempWorkspace workspace("p90_rule_missing");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "write_file")),
              PolicyVerdict::Ask);
}

TEST(PathRule, DoubleStarMatchesInRootShell) {
    ymh::test::TempWorkspace workspace("p90_rule_shell");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "shell", std::nullopt,
                                      {{"command", "git status"}})),
              PolicyVerdict::Allow);
}

TEST(PathRule, OutsideShellDoesNotMatch) {
    ymh::test::TempWorkspace workspace("p90_rule_shell_out");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "shell", std::nullopt,
                                      {{"command", "cat /etc/passwd"}})),
              PolicyVerdict::Ask);
}

TEST(PathRule, UnknownShellDoesNotMatch) {
    ymh::test::TempWorkspace workspace("p90_rule_shell_unknown");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "shell", std::nullopt,
                                      {{"command", "python3 -c 'print(1)'"}})),
              PolicyVerdict::Ask);
}

TEST(PathRule, GuardForbidStillDeny) {
    ymh::test::TempWorkspace workspace("p90_rule_forbid");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "shell", std::nullopt,
                                      {{"command", "rm -rf ~"}})),
              PolicyVerdict::Deny);
}

TEST(PathRule, GuardConfirmStillAsk) {
    ymh::test::TempWorkspace workspace("p90_rule_confirm");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "shell", std::nullopt,
                                      {{"command", "rm -rf build"}}, false)),
              PolicyVerdict::Ask);
}

TEST(PathRule, ForceAskStillAsk) {
    ymh::test::TempWorkspace workspace("p90_rule_force");
    RulePermissionPolicy policy(recommended());
    EXPECT_EQ(policy.evaluate(request(workspace.path(), "write_file",
                                      workspace.path() / "x", {}, true, true)),
              PolicyVerdict::Ask);
}

TEST(PathRule, PathRequiresToolConfigError) {
    ymh::test::TempWorkspace workspace("p90_cfg_no_tool");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"path":"**","effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, PathDotDotConfigError) {
    ymh::test::TempWorkspace workspace("p90_cfg_dotdot");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"tool":"*","path":"../**","effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, PathAbsoluteConfigError) {
    ymh::test::TempWorkspace workspace("p90_cfg_abs");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"tool":"*","path":"/etc/**","effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, EmptyPathConfigError) {
    ymh::test::TempWorkspace workspace("p90_cfg_empty");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"tool":"*","path":"","effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, BracketPatternConfigError) {
    ymh::test::TempWorkspace workspace("p90_cfg_bracket");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"tool":"write_file","path":"src/[ab].cpp","effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, MiddleDotConfigError) {
    ymh::test::TempWorkspace workspace("p90_cfg_middle_dot");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"tool":"write_file","path":"src/./x","effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, UnknownKeyStillFatal) {
    ymh::test::TempWorkspace workspace("p90_cfg_unknown");
    Config config;
    write_config(workspace.path() / "c.jsonc",
                 R"({"permissions":{"rules":[{"tool":"*","path":"**","nope":1,"effect":"allow"}]}})");
    EXPECT_THROW(apply_jsonc_file(config, workspace.path() / "c.jsonc", true),
                 ConfigError);
}

TEST(PathRule, UserRequestFragment) {
    ymh::test::TempWorkspace workspace("p90_fragment");
    RulePermissionPolicy policy(recommended());
    const auto root = workspace.path();
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / "src" / "x")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "read_file", root / "src" / "x")),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "ls"}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / ".." / "x")),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request(root, "read_file", "/etc/passwd")),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request(root, "shell", std::nullopt,
                                      {{"command", "cat /etc/passwd"}})),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request(root, "write_file",
                                      root / ".ymh" / "config.jsonc")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(request(root, "write_file", root / ".git" / "config")),
              PolicyVerdict::Deny);
    EXPECT_EQ(policy.evaluate(request(root, "terminal", std::nullopt,
                                      {{"action", "open"}})),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request(root, "terminal", std::nullopt,
                                      {{"action", "open"}, {"command", "ls"}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "terminal", std::nullopt,
                                      {{"action", "write"}, {"input", "ls\n"}})),
              PolicyVerdict::Ask);
    EXPECT_EQ(policy.evaluate(request(root, "mcp.github.x")), PolicyVerdict::Ask);
}

TEST(PathRule, ReadDefaultGotcha) {
    ymh::test::TempWorkspace workspace("p90_read_gotcha");
    RulePermissionPolicy allow_read(with_read(PolicyVerdict::Allow));
    EXPECT_EQ(allow_read.evaluate(request(workspace.path(), "read_file", "/etc/passwd")),
              PolicyVerdict::Allow);
    RulePermissionPolicy ask_read(with_read(PolicyVerdict::Ask));
    EXPECT_EQ(ask_read.evaluate(request(workspace.path(), "read_file", "/etc/passwd")),
              PolicyVerdict::Ask);
}

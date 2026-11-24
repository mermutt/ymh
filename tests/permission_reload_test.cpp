#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

namespace fs = std::filesystem;

PermissionRequest shell_request(std::string command) {
    PermissionRequest request;
    request.tool          = "shell";
    request.root          = "/ws";
    request.roots         = {fs::path{"/ws"}};
    request.arguments     = nlohmann::json{{"command", std::move(command)}};
    request.sandbox       = SandboxMode::WorkspaceWrite;
    request.session.value = "session-1";
    return request;
}

PolicyRule rule(std::string tool, PolicyVerdict effect, PolicyRule::Layer layer,
                std::string id) {
    PolicyRule entry;
    entry.tool   = std::move(tool);
    entry.effect = effect;
    entry.layer  = layer;
    entry.id     = std::move(id);
    return entry;
}

// The two baseline rules `to_permission_config` always emits: the tool-wide
// `repo.in-root` allow (Project, path-only) and the shipped `default.shell`
// verdict (Global). Outside-root shell targets match only the latter.
PermissionConfig base_config(PolicyVerdict default_shell = PolicyVerdict::Ask) {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(rule("shell", default_shell, PolicyRule::Layer::Global,
                                "default.shell"));
    return config;
}

TEST(PermissionReload, AllowRuleAddedTakesEffect) {
    RulePermissionPolicy policy(base_config());
    const PermissionRequest outside = shell_request("cat /elsewhere/file.txt");
    ASSERT_EQ(policy.evaluate(outside), PolicyVerdict::Ask);
    const std::uint64_t before = policy.generation().value;

    PermissionConfig next = base_config();
    next.rules.push_back(rule("shell", PolicyVerdict::Allow, PolicyRule::Layer::Project,
                              "shell.allow"));
    policy.reload(std::move(next));

    EXPECT_EQ(policy.evaluate(outside), PolicyVerdict::Allow);
    EXPECT_GT(policy.generation().value, before);
}

TEST(PermissionReload, DenyRuleReplacesAllow) {
    PermissionConfig allowed = base_config();
    allowed.rules.push_back(rule("shell", PolicyVerdict::Allow, PolicyRule::Layer::Project,
                                 "shell.allow"));
    RulePermissionPolicy policy(std::move(allowed));
    const PermissionRequest outside = shell_request("cat /elsewhere/file.txt");
    ASSERT_EQ(policy.evaluate(outside), PolicyVerdict::Allow);

    PermissionConfig denied = base_config();
    denied.rules.push_back(rule("shell", PolicyVerdict::Deny, PolicyRule::Layer::Project,
                                "shell.deny"));
    policy.reload(std::move(denied));

    EXPECT_EQ(policy.evaluate(outside), PolicyVerdict::Deny);
}

TEST(PermissionReload, SessionGrantSurvivesReload) {
    RulePermissionPolicy policy(base_config());
    const PermissionRequest echo = shell_request("echo hi");
    ASSERT_EQ(policy.evaluate(echo), PolicyVerdict::Ask);

    const PolicyRule grant =
        policy.remember(echo, payload::PermissionDecisionKind::Allow, GrantScope::Session);
    ASSERT_FALSE(grant.id.empty());
    ASSERT_EQ(policy.evaluate(echo), PolicyVerdict::Allow);

    // The reloaded config has no allow rule; the in-memory grant must still be
    // folded into the new rule set.
    policy.reload(base_config());
    EXPECT_EQ(policy.evaluate(echo), PolicyVerdict::Allow);
}

TEST(PermissionReload, InvalidConfigKeepsTheLastGoodRules) {
    PermissionConfig allowed = base_config();
    allowed.rules.push_back(rule("shell", PolicyVerdict::Allow, PolicyRule::Layer::Project,
                                 "shell.allow"));
    RulePermissionPolicy policy(std::move(allowed));
    const PermissionRequest outside = shell_request("cat /elsewhere/file.txt");
    ASSERT_EQ(policy.evaluate(outside), PolicyVerdict::Allow);

    PermissionConfig invalid = base_config();
    invalid.rules.push_back(rule("shell[", PolicyVerdict::Allow, PolicyRule::Layer::Project,
                                 "malformed"));
    EXPECT_THROW(policy.reload(std::move(invalid)), PolicyConfigError);

    EXPECT_EQ(policy.evaluate(outside), PolicyVerdict::Allow);
}

} // namespace

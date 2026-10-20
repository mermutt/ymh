#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

class RecordingGrantStore final : public GrantStore {
public:
    std::vector<PolicyRule> appended;

    std::vector<PolicyRule> load() override { return {}; }
    bool append(const PolicyRule& grant) override {
        appended.push_back(grant);
        return true;
    }
};

PermissionRequest request(const std::filesystem::path& root, std::string tool,
                          nlohmann::json arguments = nlohmann::json::object(),
                          std::optional<std::filesystem::path> path = std::nullopt,
                          bool force_ask = false) {
    PermissionRequest value;
    value.call    = "call-1";
    value.session = SessionId{"s1"};
    value.tool    = std::move(tool);
    value.root    = root;
    value.path    = std::move(path);
    value.arguments = std::move(arguments);
    value.sandbox = SandboxMode::Workspace;
    value.kernel_confinement = true;
    value.force_ask = force_ask;
    return value;
}

PermissionConfig asks() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    return config;
}

TEST(AlwaysGrant94, ExactIsIntended) {
    ymh::test::TempWorkspace workspace("p94_always_exact");
    const auto root = workspace.path();
    RecordingGrantStore store;
    RulePermissionPolicy policy(asks(), &store);

    const PermissionRequest first = request(root, "shell", {{"command", "grep -n x f"}});
    const PolicyRule grant = policy.remember(first, payload::PermissionDecisionKind::AllowAlways,
                                             GrantScope::Always);
    EXPECT_EQ(grant.tool, "shell");
    EXPECT_EQ(grant.command, "grep -n x f");
    EXPECT_TRUE(grant.literal);
    EXPECT_EQ(grant.target_class, TargetClass::Any);
    EXPECT_EQ(grant.layer, PolicyRule::Layer::LocalGrant);
    ASSERT_EQ(store.appended.size(), 1u);

    EXPECT_EQ(policy.evaluate(request(root, "shell", {{"command", "grep -n x f"}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(request(root, "shell", {{"command", "grep -n y g"}})),
              PolicyVerdict::Ask);
}

TEST(AlwaysGrant94, ExactDoesNotMatchOutsideVariant) {
    ymh::test::TempWorkspace workspace("p94_always_variant");
    const auto root = workspace.path();
    RulePermissionPolicy policy(asks());
    (void)policy.remember(request(root, "shell", {{"command", "grep x f"}}),
                          payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_EQ(policy.evaluate(request(root, "shell", {{"command", "grep x /etc/passwd"}})),
              PolicyVerdict::Ask);
}

TEST(AlwaysGrant94, CompoundStaysExact) {
    ymh::test::TempWorkspace workspace("p94_always_compound");
    const auto root = workspace.path();
    RulePermissionPolicy policy(asks());
    const PermissionRequest compound =
        request(root, "shell", {{"command", "cd build && cmake .."}});
    const PolicyRule grant = policy.remember(
        compound, payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_EQ(grant.command, "cd build && cmake ..");
    EXPECT_EQ(policy.evaluate(
                  request(root, "shell", {{"command", "cd build && cmake .."}})),
              PolicyVerdict::Allow);
    EXPECT_EQ(policy.evaluate(
                  request(root, "shell", {{"command", "cd other && cmake .."}})),
              PolicyVerdict::Ask);
}

TEST(AlwaysGrant94, TerminalWriteStaysExact) {
    ymh::test::TempWorkspace workspace("p94_always_terminal");
    const auto root = workspace.path();
    RulePermissionPolicy policy(asks());
    const PermissionRequest write = request(
        root, "terminal", {{"action", "write"}, {"input", "echo hi"}});
    const PolicyRule grant = policy.remember(
        write, payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_EQ(grant.tool, "terminal");
    EXPECT_EQ(grant.command, "echo hi");
    EXPECT_TRUE(grant.literal);
    EXPECT_EQ(grant.target_class, TargetClass::Any);
}

TEST(AlwaysGrant94, ForceAskCreatesNoGrant) {
    ymh::test::TempWorkspace workspace("p94_always_force");
    const auto root = workspace.path();
    RecordingGrantStore store;
    RulePermissionPolicy policy(asks(), &store);
    const PolicyRule grant = policy.remember(
        request(root, "shell", {{"command", "grep x f"}}, std::nullopt, true),
        payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_TRUE(grant.id.empty());
    EXPECT_TRUE(store.appended.empty());
}

TEST(AlwaysGrant94, ProtectedCreatesNoGrant) {
    ymh::test::TempWorkspace workspace("p94_always_protected");
    const auto root = workspace.path();
    RecordingGrantStore store;
    RulePermissionPolicy policy(asks(), &store);
    const PolicyRule grant = policy.remember(
        request(root, "write_file", {{"path", ".ymh/config.jsonc"}},
                root / ".ymh" / "config.jsonc"),
        payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_TRUE(grant.id.empty());
    EXPECT_TRUE(store.appended.empty());
}

TEST(AlwaysGrant94, PathToolGrantsAllInRoot) {
    ymh::test::TempWorkspace workspace("p94_always_path");
    const auto root = workspace.path();
    RulePermissionPolicy policy(asks());
    const PolicyRule grant =
        policy.remember(request(root, "write_file", {{"path", "a.txt"}}, root / "a.txt"),
                        payload::PermissionDecisionKind::AllowAlways, GrantScope::Always);
    EXPECT_EQ(grant.target_class, TargetClass::InRoot);
    EXPECT_TRUE(grant.command.empty());
    EXPECT_TRUE(grant.literal);
}

TEST(AlwaysGrant94, NonPathToolGrantsWholeTool) {
    ymh::test::TempWorkspace workspace("p94_always_mcp");
    const auto root = workspace.path();
    RulePermissionPolicy policy(asks());
    const PolicyRule grant = policy.remember(
        request(root, "mcp.foo.bar"), payload::PermissionDecisionKind::AllowAlways,
        GrantScope::Always);
    EXPECT_EQ(grant.target_class, TargetClass::Any);
    EXPECT_TRUE(grant.command.empty());
    EXPECT_EQ(policy.evaluate(request(root, "mcp.foo.bar")), PolicyVerdict::Allow);
}

TEST(AlwaysGrant94, SessionGrantIsInMemoryOnly) {
    ymh::test::TempWorkspace workspace("p94_session");
    const auto root = workspace.path();
    RecordingGrantStore store;
    RulePermissionPolicy policy(asks(), &store);
    const PolicyRule grant = policy.remember(
        request(root, "shell", {{"command", "grep -n x f"}}),
        payload::PermissionDecisionKind::Allow, GrantScope::Session);
    EXPECT_EQ(grant.layer, PolicyRule::Layer::SessionGrant);
    EXPECT_TRUE(store.appended.empty());
    EXPECT_EQ(policy.evaluate(request(root, "shell", {{"command", "grep -n x f"}})),
              PolicyVerdict::Allow);
}

} // namespace

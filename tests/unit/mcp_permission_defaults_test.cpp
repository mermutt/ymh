#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "support/short_temp.hpp"
#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

PermissionRequest request(const std::filesystem::path& root, std::string tool) {
    PermissionRequest value;
    value.call    = "call-1";
    value.session = SessionId{"s1"};
    value.tool    = std::move(tool);
    value.root    = root;
    value.arguments = nlohmann::json::object();
    value.sandbox = SandboxMode::Workspace;
    value.kernel_confinement = true;
    return value;
}

Config config_with_server(std::string global_default,
                          std::optional<std::string> server_default = std::nullopt,
                          bool enabled = true, std::string id = "github") {
    Config config;
    config.permissions.default_verdict = std::move(global_default);
    McpServerSettings server;
    server.id = std::move(id);
    server.enabled = enabled;
    server.default_verdict = std::move(server_default);
    config.mcp.servers.push_back(std::move(server));
    return config;
}

void add_rule(Config& config, std::string tool, std::string path, std::string effect) {
    PermissionRuleSettings rule;
    rule.tool   = std::move(tool);
    rule.path   = std::move(path);
    rule.effect = std::move(effect);
    rule.id     = "test.rule";
    config.permissions.rules.push_back(std::move(rule));
}

PolicyVerdict evaluate(const Config& config, const std::filesystem::path& root,
                       std::string tool) {
    RulePermissionPolicy policy(to_permission_config(config));
    return policy.evaluate(request(root, std::move(tool)));
}

const ToolDefault* find_default(const PermissionConfig& config, std::string_view prefix) {
    for (const ToolDefault& entry : config.tool_defaults) {
        if (entry.prefix == prefix) {
            return &entry;
        }
    }
    return nullptr;
}

TEST(McpPermissionDefaults, DefaultMcpServerIsAllow) {
    ymh::test::ShortTempRoot root("ymh-e93-allow");
    const Config config = config_with_server("ask");
    const PermissionConfig permissions = to_permission_config(config);
    const ToolDefault* entry = find_default(permissions, "mcp.github.");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->verdict, PolicyVerdict::Allow);
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Allow);
}

TEST(McpPermissionDefaults, DisabledServerHasNoDefault) {
    ymh::test::ShortTempRoot root("ymh-e93-disabled");
    const Config config = config_with_server("ask", std::nullopt, false);
    EXPECT_EQ(find_default(to_permission_config(config), "mcp.github."), nullptr);
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Ask);
}

TEST(McpPermissionDefaults, EmptyIdServerHasNoDefault) {
    Config config = config_with_server("ask", std::nullopt, true, "");
    EXPECT_TRUE(to_permission_config(config).tool_defaults.empty());
}

TEST(McpPermissionDefaults, GlobalDenyDeniesImplicitMcp) {
    ymh::test::ShortTempRoot root("ymh-e93-global-deny");
    const Config config = config_with_server("deny");
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Deny);
}

TEST(McpPermissionDefaults, ExplicitAllowOverridesGlobalDeny) {
    ymh::test::ShortTempRoot root("ymh-e93-explicit-allow");
    const Config config = config_with_server("deny", std::string{"allow"});
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Allow);
}

TEST(McpPermissionDefaults, ExplicitAskOverridesGlobalDeny) {
    ymh::test::ShortTempRoot root("ymh-e93-explicit-ask");
    const Config config = config_with_server("deny", std::string{"ask"});
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Ask);
}

TEST(McpPermissionDefaults, ExplicitDenyWins) {
    ymh::test::ShortTempRoot root("ymh-e93-explicit-deny");
    const Config config = config_with_server("ask", std::string{"deny"});
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Deny);
}

TEST(McpPermissionDefaults, ExplicitAskWins) {
    ymh::test::ShortTempRoot root("ymh-e93-ask");
    const Config config = config_with_server("ask", std::string{"ask"});
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Ask);
}

TEST(McpPermissionDefaults, MasterSwitchStillAllows) {
    ymh::test::ShortTempRoot root("ymh-e93-master");
    for (const char* value : {"ask", "deny"}) {
        const Config config = config_with_server("allow", std::string{value});
        EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Allow)
            << value;
    }
}

TEST(McpPermissionDefaults, RuleDenyBeatsMaster) {
    ymh::test::ShortTempRoot root("ymh-e93-rule-deny");
    Config config = config_with_server("allow");
    add_rule(config, "mcp.github.*", "", "deny");
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Deny);
}

TEST(McpPermissionDefaults, RuleAllowOverridesGlobalDenyForMcp) {
    ymh::test::ShortTempRoot root("ymh-e93-rule-allow");
    Config config = config_with_server("deny");
    add_rule(config, "mcp.github.*", "", "allow");
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Allow);
}

TEST(McpPermissionDefaults, ToolRuleAskBeatsDefault) {
    ymh::test::ShortTempRoot root("ymh-e93-rule-ask");
    Config config = config_with_server("ask", std::string{"allow"});
    add_rule(config, "mcp.github.search", "", "ask");
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Ask);
}

TEST(McpPermissionDefaults, PathRuleDoesNotMatchMcp) {
    ymh::test::ShortTempRoot root("ymh-e93-path-allow");
    Config config = config_with_server("ask", std::string{"ask"});
    add_rule(config, "*", "**", "allow");
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Ask);
}

TEST(McpPermissionDefaults, PathDenyDoesNotDenyMcp) {
    ymh::test::ShortTempRoot root("ymh-e93-path-deny");
    Config config = config_with_server("ask", std::string{"allow"});
    add_rule(config, "*", "**", "deny");
    EXPECT_EQ(evaluate(config, root.path(), "mcp.github.search"), PolicyVerdict::Allow);
}

TEST(McpPermissionDefaults, NonMcpToolUnaffected) {
    ymh::test::ShortTempRoot root("ymh-e93-nonmcp");
    const Config config = config_with_server("ask");
    EXPECT_EQ(evaluate(config, root.path(), "unknown_tool"), PolicyVerdict::Ask);
}

TEST(McpPermissionDefaults, DestructiveHintRequestDeniedInReadOnly) {
    ymh::test::ShortTempRoot root("ymh-e93-destructive");
    PermissionConfig config = to_permission_config(config_with_server("ask"));
    RulePermissionPolicy policy(std::move(config));
    PermissionRequest value = request(root.path(), "mcp.fake.delete");
    value.sandbox = SandboxMode::ReadOnly;
    value.destructive = true;
    EXPECT_EQ(policy.evaluate(value), PolicyVerdict::Deny);
    value.destructive = false;
    EXPECT_NE(policy.evaluate(value), PolicyVerdict::Deny);
}

TEST(McpPermissionDefaults, ExplicitPresenceIsTracked) {
    const std::pair<const char*, const char*> cases[] = {
        {"e93_presence_array",
         R"JSONC({"mcp": {"server": [{"id": "array", "command": "x", "default_verdict": "ask"}]}})JSONC"},
        {"e93_presence_object",
         R"JSONC({"mcp_servers": {"object": {"command": "y", "default_verdict": "ask"}}})JSONC"},
    };
    for (const auto& [name, body] : cases) {
        test::TempWorkspace workspace(name);
        workspace.write("global.jsonc", body);
        ConfigPaths paths;
        paths.global = workspace.path() / "global.jsonc";
        paths.workspace = workspace.path() / ".ymh" / "config.jsonc";
        const Config config = load_config(paths);
        ASSERT_EQ(config.mcp.servers.size(), 1u) << name;
        ASSERT_TRUE(config.mcp.servers[0].default_verdict.has_value()) << name;
        EXPECT_EQ(*config.mcp.servers[0].default_verdict, "ask") << name;
        EXPECT_EQ(evaluate(config, workspace.path(), "mcp." + config.mcp.servers[0].id + ".x"),
                  PolicyVerdict::Ask);
    }
}

TEST(McpPermissionDefaults, AbsentKeyIsNullopt) {
    test::TempWorkspace workspace("e93_absent");
    workspace.write("global.jsonc",
                    R"JSONC({"mcp": {"server": [{"id": "github", "command": "x"}]}})JSONC");
    ConfigPaths paths;
    paths.global = workspace.path() / "global.jsonc";
    paths.workspace = workspace.path() / ".ymh" / "config.jsonc";
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_FALSE(config.mcp.servers[0].default_verdict.has_value());
    EXPECT_EQ(evaluate(config, workspace.path(), "mcp.github.search"), PolicyVerdict::Allow);

    Config denied = config;
    denied.permissions.default_verdict = "deny";
    EXPECT_EQ(evaluate(denied, workspace.path(), "mcp.github.search"), PolicyVerdict::Deny);
}

} // namespace

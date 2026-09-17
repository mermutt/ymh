#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/mcp/mcp_manager.hpp"
#include "ymh/mcp/mcp_tool.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/tools/tool_registry.hpp"
#include "support/test_env.hpp"

namespace {

using namespace std::chrono_literals;
using ymh::McpConfig;
using ymh::McpErrorCode;
using ymh::McpServerConfig;
using ymh::McpServerId;
using ymh::McpServerState;
using ymh::McpToolInfo;
using ymh::ToolConfig;
using ymh::ToolName;
using ymh::ToolRegistry;
using ymh::ToolRegistryError;
using ymh::ToolRegistryErrorCode;

class StubTool final : public ymh::Tool {
public:
    explicit StubTool(std::string name) {
        schema_.name.value = std::move(name);
        schema_.input_schema = {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false},
        };
    }

    ymh::ToolSchema schema() const override { return schema_; }

    ymh::Task<ymh::ToolResult> execute(const ymh::ToolContext&,
                                       const ymh::ToolArguments&) override {
        return ymh::Task<ymh::ToolResult>(ymh::ToolResult{});
    }

private:
    ymh::ToolSchema schema_;
};

std::unique_ptr<ymh::Tool> make_stub(std::string name) {
    return std::make_unique<StubTool>(std::move(name));
}

McpToolInfo echo_tool(std::string name = "echo") {
    McpToolInfo info;
    info.remote_name.value = std::move(name);
    info.description = "echoes";
    info.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    return info;
}

class FakeManagerClient final : public ymh::McpClient {
public:
    FakeManagerClient(McpServerConfig config, std::vector<McpToolInfo> tools)
        : id_(config.id), tools_(std::move(tools)) {}

    const ymh::McpServerId& id() const noexcept override { return id_; }
    McpServerState state() const noexcept override { return state_; }

    ymh::Task<void> start(ymh::CancellationToken) override {
        state_ = McpServerState::Ready;
        return {};
    }

    ymh::Task<std::vector<McpToolInfo>> listTools(ymh::CancellationToken) override {
        return ymh::Task<std::vector<McpToolInfo>>(tools_);
    }

    ymh::Task<ymh::McpCallResult> callTool(std::string_view, const nlohmann::json&,
                                           const ymh::McpCallOptions&,
                                           ymh::CancellationToken) override {
        return ymh::Task<ymh::McpCallResult>(ymh::McpCallResult{});
    }

    std::optional<CallSlot> tryAcquireCallSlot() override {
        return CallSlot([] {});
    }

    ymh::Task<bool> ping() override { return ymh::Task<bool>{true}; }

    ymh::Task<void> shutdown(std::chrono::milliseconds) override {
        state_ = McpServerState::Stopped;
        return {};
    }

    void setNotificationHandler(
        std::function<void(std::string_view, const nlohmann::json&)>,
        std::function<void()> on_tools_changed) override {
        on_tools_changed_ = std::move(on_tools_changed);
    }

    ymh::McpServerStatus status() const override {
        ymh::McpServerStatus status;
        status.id = id_;
        status.state = state_;
        return status;
    }

    void set_tools(std::vector<McpToolInfo> tools) { tools_ = std::move(tools); }
    void fire_tools_changed() {
        if (on_tools_changed_) {
            on_tools_changed_();
        }
    }

private:
    ymh::McpServerId                     id_;
    std::vector<McpToolInfo>             tools_;
    McpServerState                       state_{McpServerState::Disabled};
    std::function<void()>                on_tools_changed_;
};

McpServerConfig stdio_server(std::string id) {
    McpServerConfig server;
    server.id.value = std::move(id);
    server.command = "unused";
    return server;
}

} // namespace

TEST(AdapterScopeTest, OpenBeforeFreezeReplaceAfterFreeze) {
    ToolRegistry registry;
    ToolRegistry::AdapterScope scope = registry.openAdapter("mcp.foo.");
    EXPECT_TRUE(scope.held());
    EXPECT_EQ(scope.prefix(), "mcp.foo.");

    std::vector<std::unique_ptr<ymh::Tool>> tools;
    tools.push_back(make_stub("mcp.foo.bar"));
    EXPECT_EQ(scope.replace(std::move(tools)), 1u);
    EXPECT_EQ(scope.size(), 1u);
    const std::uint64_t generation = registry.generation();

    registry.freeze();
    EXPECT_THROW((void)registry.add(make_stub("local")), ToolRegistryError);
    EXPECT_TRUE(registry.contains(ToolName{"mcp.foo.bar"}));

    std::vector<std::unique_ptr<ymh::Tool>> replaced;
    replaced.push_back(make_stub("mcp.foo.baz"));
    EXPECT_EQ(scope.replace(std::move(replaced)), 1u);
    EXPECT_GT(registry.generation(), generation);
    EXPECT_FALSE(registry.contains(ToolName{"mcp.foo.bar"}));
    EXPECT_TRUE(registry.contains(ToolName{"mcp.foo.baz"}));
}

TEST(AdapterScopeTest, FailedReplaceRetainsPreviousSet) {
    ToolRegistry registry;
    ToolRegistry::AdapterScope scope = registry.openAdapter("mcp.foo.");
    std::vector<std::unique_ptr<ymh::Tool>> tools;
    tools.push_back(make_stub("mcp.foo.bar"));
    scope.replace(std::move(tools));

    std::vector<std::unique_ptr<ymh::Tool>> invalid;
    invalid.push_back(make_stub("mcp.other.baz"));
    EXPECT_THROW(scope.replace(std::move(invalid)), ToolRegistryError);
    EXPECT_TRUE(registry.contains(ToolName{"mcp.foo.bar"}));
    EXPECT_EQ(registry.size(), 1u);
}

TEST(AdapterScopeTest, LocalShadowFailsLoud) {
    ToolRegistry registry;
    ToolRegistry::AdapterScope scope = registry.openAdapter("mcp.foo.");
    ymh::ToolRegistry::Registration local = registry.add(make_stub("mcp.foo.keep"));

    std::vector<std::unique_ptr<ymh::Tool>> tools;
    tools.push_back(make_stub("mcp.foo.keep"));
    EXPECT_THROW(scope.replace(std::move(tools)), ToolRegistryError);
    EXPECT_TRUE(registry.contains(ToolName{"mcp.foo.keep"}));
}

TEST(AdapterScopeTest, OpenRejectsPrefixCollisionAndFrozen) {
    ToolRegistry registry;
    ToolRegistry::AdapterScope first = registry.openAdapter("mcp.foo.");
    EXPECT_THROW((void)registry.openAdapter("mcp.foo."), ToolRegistryError);
    EXPECT_THROW((void)registry.openAdapter("mcp.foo.bar."), ToolRegistryError);
    EXPECT_THROW((void)registry.openAdapter("mcp."), ToolRegistryError);
    registry.freeze();
    EXPECT_THROW((void)registry.openAdapter("mcp.baz."), ToolRegistryError);
}

TEST(AdapterScopeTest, DestructorUnregistersAndBumpsGeneration) {
    ToolRegistry registry;
    std::uint64_t generation = 0;
    {
        ToolRegistry::AdapterScope scope = registry.openAdapter("mcp.foo.");
        std::vector<std::unique_ptr<ymh::Tool>> tools;
        tools.push_back(make_stub("mcp.foo.bar"));
        scope.replace(std::move(tools));
        generation = registry.generation();
        EXPECT_TRUE(registry.contains(ToolName{"mcp.foo.bar"}));
    }
    EXPECT_FALSE(registry.contains(ToolName{"mcp.foo.bar"}));
    EXPECT_GT(registry.generation(), generation);
}

TEST(McpManagerTest, StartInstallsToolsAndEmitsStatus) {
    ymh::test::ToolEnv env("mcp_manager_start");
    ToolRegistry registry;
    McpConfig config;
    config.servers.push_back(stdio_server("alpha"));
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);

    std::vector<FakeManagerClient*> created;
    manager.setClientFactory(
        [&created](const McpServerConfig& server, McpConfig&, ymh::ExecutionEnvironment&,
                   ymh::ResourceGovernor&, ymh::Logger&) -> std::unique_ptr<ymh::McpClient> {
            auto client = std::make_unique<FakeManagerClient>(server,
                                                              std::vector<McpToolInfo>{echo_tool()});
            created.push_back(client.get());
            return client;
        });

    std::vector<McpServerState> states;
    ymh::Subscription subscription = env.bus.subscribe<ymh::payload::McpServerStatusChanged>(
        [&states](const ymh::payload::McpServerStatusChanged& status) {
            states.push_back(status.state);
        });

    manager.start({}).get();
    registry.freeze();

    EXPECT_TRUE(registry.contains(ToolName{"mcp.alpha.echo"}));
    const std::vector<ymh::McpServerStatus> statuses = manager.statuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses.front().state, McpServerState::Ready);
    EXPECT_EQ(statuses.front().tool_count, 1u);
    EXPECT_NE(std::find(states.begin(), states.end(), McpServerState::Ready), states.end());

    manager.shutdown(100ms).get();
    EXPECT_FALSE(registry.contains(ToolName{"mcp.alpha.echo"}));
}

// 18 §3.5 (C8, CTX-F6): `statuses()` (dispatch thread) races `refresh()` (the
// reconnect path); the additive mutex must make the `slots_` access race-free.
// Run under -DYMH_TSAN=ON for the full check.
TEST(McpManagerTest, StatusesAndRefreshAreRaceFree) {
    ymh::test::ToolEnv env("mcp_manager_race");
    ToolRegistry        registry;
    McpConfig           config;
    config.servers.push_back(stdio_server("alpha"));
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    manager.setClientFactory(
        [](const McpServerConfig& server, McpConfig&, ymh::ExecutionEnvironment&,
           ymh::ResourceGovernor&, ymh::Logger&) -> std::unique_ptr<ymh::McpClient> {
            return std::make_unique<FakeManagerClient>(server,
                                                       std::vector<McpToolInfo>{echo_tool()});
        });
    manager.start({}).get();
    registry.freeze();

    std::atomic<bool> stop{false};
    std::thread       reader([&manager, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)manager.statuses();
        }
    });
    for (int index = 0; index < 2000; ++index) {
        manager.refresh(McpServerId{"alpha"}, {}).get();
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    ASSERT_EQ(manager.statuses().size(), 1u);
    EXPECT_EQ(manager.statuses().front().state, McpServerState::Ready);
    manager.shutdown(100ms).get();
}

TEST(McpManagerTest, ZeroToolsIsDegradedAndRequiredRejects) {
    ymh::test::ToolEnv env("mcp_manager_degraded");
    ToolRegistry registry;
    McpConfig config;
    config.servers.push_back(stdio_server("alpha"));
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    manager.setClientFactory(
        [](const McpServerConfig& server, McpConfig&, ymh::ExecutionEnvironment&,
           ymh::ResourceGovernor&, ymh::Logger&) -> std::unique_ptr<ymh::McpClient> {
            return std::make_unique<FakeManagerClient>(server, std::vector<McpToolInfo>{});
        });
    manager.start({}).get();
    ASSERT_EQ(manager.statuses().size(), 1u);
    EXPECT_EQ(manager.statuses().front().state, McpServerState::Degraded);
    EXPECT_FALSE(registry.contains(ToolName{"mcp.alpha.echo"}));
    manager.shutdown(100ms).get();
}

TEST(McpManagerTest, RequiredZeroToolsThrows) {
    ymh::test::ToolEnv env("mcp_manager_required");
    ToolRegistry registry;
    McpConfig config;
    McpServerConfig server = stdio_server("alpha");
    server.required = true;
    config.servers.push_back(server);
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    manager.setClientFactory(
        [](const McpServerConfig& server, McpConfig&, ymh::ExecutionEnvironment&,
           ymh::ResourceGovernor&, ymh::Logger&) -> std::unique_ptr<ymh::McpClient> {
            return std::make_unique<FakeManagerClient>(server, std::vector<McpToolInfo>{});
        });
    EXPECT_THROW(manager.start({}).get(), ymh::McpError);
}

TEST(McpManagerTest, RefreshReplacesNamespaceAtomically) {
    ymh::test::ToolEnv env("mcp_manager_refresh");
    ToolRegistry registry;
    McpConfig config;
    config.servers.push_back(stdio_server("alpha"));
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    FakeManagerClient* client = nullptr;
    manager.setClientFactory(
        [&client](const McpServerConfig& server, McpConfig&, ymh::ExecutionEnvironment&,
                  ymh::ResourceGovernor&, ymh::Logger&) -> std::unique_ptr<ymh::McpClient> {
            auto created = std::make_unique<FakeManagerClient>(
                server, std::vector<McpToolInfo>{echo_tool("first")});
            client = created.get();
            return created;
        });
    manager.start({}).get();
    registry.freeze();
    EXPECT_TRUE(registry.contains(ToolName{"mcp.alpha.first"}));

    client->set_tools({echo_tool("second")});
    manager.refresh(McpServerId{"alpha"}, {}).get();
    EXPECT_FALSE(registry.contains(ToolName{"mcp.alpha.first"}));
    EXPECT_TRUE(registry.contains(ToolName{"mcp.alpha.second"}));

    client->set_tools({});
    manager.refresh(McpServerId{"alpha"}, {}).get();
    EXPECT_FALSE(registry.contains(ToolName{"mcp.alpha.second"}));
    EXPECT_EQ(manager.statuses().front().state, McpServerState::Degraded);
    manager.shutdown(100ms).get();
}

TEST(McpManagerTest, ConfigValidationRejectsBadInput) {
    ymh::test::ToolEnv env("mcp_manager_config");
    ToolRegistry registry;
    auto build = [&](McpConfig config) {
        ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                                env.logger);
    };

    McpConfig bad_id;
    McpServerConfig server = stdio_server("Bad-Id");
    bad_id.servers.push_back(server);
    EXPECT_THROW(build(bad_id), ymh::ConfigError);

    McpConfig duplicate;
    duplicate.servers.push_back(stdio_server("alpha"));
    duplicate.servers.push_back(stdio_server("alpha"));
    EXPECT_THROW(build(duplicate), ymh::ConfigError);

    McpConfig missing_command;
    McpServerConfig no_command;
    no_command.id.value = "alpha";
    missing_command.servers.push_back(no_command);
    EXPECT_THROW(build(missing_command), ymh::ConfigError);

    McpConfig too_large;
    McpServerConfig large = stdio_server("alpha");
    large.max_result_bytes = ToolConfig{}.tool_result_max_bytes + 1;
    too_large.servers.push_back(large);
    EXPECT_THROW(build(too_large), ymh::ConfigError);

    McpConfig network;
    McpServerConfig http;
    http.id.value = "alpha";
    http.transport = ymh::McpTransportKind::HttpSse;
    http.url = "https://example.invalid";
    network.servers.push_back(http);
    EXPECT_THROW(build(network), ymh::ConfigError);
}

TEST(McpBackoffTest, ExponentialCappedAndJittered) {
    McpConfig config;
    config.reconnect_initial_backoff = 500ms;
    config.reconnect_max_backoff = 5000ms;
    config.reconnect_jitter = 0.25;

    EXPECT_EQ(ymh::compute_mcp_backoff(0, config, 0.5).count(), 500);
    EXPECT_EQ(ymh::compute_mcp_backoff(1, config, 0.5).count(), 1000);
    EXPECT_EQ(ymh::compute_mcp_backoff(2, config, 0.5).count(), 2000);
    EXPECT_EQ(ymh::compute_mcp_backoff(10, config, 0.5).count(), 5000);
    EXPECT_LT(ymh::compute_mcp_backoff(0, config, 0.0).count(), 500);
    EXPECT_GT(ymh::compute_mcp_backoff(0, config, 1.0).count(), 500);
}

TEST(McpPermissionTest, ToolDefaultsLoweredFromConfig) {
    ymh::Config config;
    ymh::McpServerSettings server;
    server.id = "github";
    server.default_verdict = "allow";
    config.mcp.servers.push_back(server);

    const ymh::PermissionConfig permissions = ymh::to_permission_config(config);
    ASSERT_EQ(permissions.tool_defaults.size(), 1u);
    EXPECT_EQ(permissions.tool_defaults.front().prefix, "mcp.github.");
    EXPECT_EQ(permissions.tool_defaults.front().verdict, ymh::PolicyVerdict::Allow);
    EXPECT_EQ(permissions.tool_defaults.front().id, "mcp.github.default");

    ymh::RulePermissionPolicy policy(permissions);
    ymh::PermissionRequest request;
    request.tool = "mcp.github.search";
    EXPECT_EQ(policy.evaluate(request), ymh::PolicyVerdict::Allow);

    ymh::PermissionRequest other;
    other.tool = "unknown_tool";
    EXPECT_EQ(policy.evaluate(other), ymh::PolicyVerdict::Ask);
}

TEST(McpPermissionTest, OperatorRuleOutranksServerDefault) {
    ymh::PermissionConfig permissions;
    ymh::ToolDefault fallback;
    fallback.prefix = "mcp.github.";
    fallback.verdict = ymh::PolicyVerdict::Allow;
    fallback.id = "mcp.github.default";
    permissions.tool_defaults.push_back(fallback);
    ymh::PolicyRule rule;
    rule.tool = "mcp.github.*";
    rule.effect = ymh::PolicyVerdict::Deny;
    rule.layer = ymh::PolicyRule::Layer::Global;
    rule.id = "operator.deny";
    permissions.rules.push_back(rule);

    ymh::RulePermissionPolicy policy(permissions);
    ymh::PermissionRequest request;
    request.tool = "mcp.github.search";
    EXPECT_EQ(policy.evaluate(request), ymh::PolicyVerdict::Deny);
}

TEST(McpConfigTest, StrictLoaderParsesAndRejectsUnknownKeys) {
    ymh::test::TempWorkspace workspace("mcp_config");
    const std::filesystem::path config_path = workspace.path() / "config.jsonc";
    workspace.write("config.jsonc", R"JSONC(
{
  "mcp": {
    "enabled": true,
    "max_servers": 2,
    "server": [
      {
        "id": "fs",
        "transport": "stdio",
        "command": "mcp-server-filesystem",
        "args": ["."],
        "cwd": ".",
        "allowed_tools": ["read_*"],
        "denied_tools": ["write_*"],
        "default_verdict": "ask",
        "call_timeout_ms": 1000
      }
    ]
  }
}
)JSONC");
    const ymh::Config config = ymh::load_config(ymh::ConfigPaths{config_path, {}});
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_EQ(config.mcp.servers.front().id, "fs");
    EXPECT_EQ(config.mcp.servers.front().allowed_tools,
              std::vector<std::string>{"read_*"});
    EXPECT_EQ(config.mcp.max_servers, 2u);

    workspace.write("config.jsonc", "{ \"mcp\": { \"bogus\": 1 } }\n");
    EXPECT_THROW((void)ymh::load_config(ymh::ConfigPaths{config_path, {}}), ymh::ConfigError);

    workspace.write("config.jsonc",
                    "{ \"mcp\": { \"server\": [ { \"id\": \"x\", \"command\": \"c\", "
                    "\"bogus\": 1 } ] } }\n");
    EXPECT_THROW((void)ymh::load_config(ymh::ConfigPaths{config_path, {}}), ymh::ConfigError);
}

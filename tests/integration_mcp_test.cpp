#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "ymh/core/logger.hpp"
#include "ymh/mcp/mcp_manager.hpp"
#include "ymh/tools/tool_registry.hpp"
#include "support/test_env.hpp"

#ifndef YMH_FAKE_MCP_SERVER
#error "YMH_FAKE_MCP_SERVER must name the fake MCP server binary"
#endif

namespace {

using namespace std::chrono_literals;
using ymh::McpConfig;
using ymh::McpServerConfig;
using ymh::McpServerState;
using ymh::ToolConfig;
using ymh::ToolName;
using ymh::ToolRegistry;

McpConfig scenario_config(const std::string& scenario,
                          const std::string& pidfile = {}) {
    McpConfig config;
    config.startup_deadline = 3000ms;
    config.handshake_timeout = 2000ms;
    config.list_timeout = 2000ms;
    config.shutdown_grace = 300ms;
    McpServerConfig server;
    server.id.value = "fake";
    server.command = YMH_FAKE_MCP_SERVER;
    server.args = {scenario};
    if (!pidfile.empty()) {
        server.args.push_back(pidfile);
    }
    server.call_timeout = 1500ms;
    config.servers.push_back(std::move(server));
    return config;
}

ymh::payload::ToolResult call_tool(ToolRegistry& registry,
                                   ymh::test::ToolEnv& env,
                                   const std::string& name) {
    ymh::payload::ToolCall call;
    call.id = "call-1";
    call.name = name;
    call.arguments = nlohmann::json::object();
    ymh::ToolContext context = env.context("call-1");
    return registry.execute(call, context).get();
}

} // namespace

TEST(McpIntegrationTest, HappyPathListsPaginatesAndCalls) {
    ymh::test::ToolEnv env("mcp_integration_happy");
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("happy"), ToolConfig{}, env.env, env.governor,
                            registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    EXPECT_TRUE(registry.contains(ToolName{"mcp.fake.alpha"}));
    EXPECT_TRUE(registry.contains(ToolName{"mcp.fake.beta"}));
    EXPECT_EQ(manager.statuses().front().state, McpServerState::Ready);

    const ymh::payload::ToolResult result =
        call_tool(registry, env, "mcp.fake.alpha");
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("echo:alpha"), std::string::npos);

    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, IsErrorMapsToErrorOutcome) {
    ymh::test::ToolEnv env("mcp_integration_is_error");
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("is_error"), ToolConfig{}, env.env, env.governor,
                            registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    const ymh::payload::ToolResult result = call_tool(registry, env, "mcp.fake.echo");
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Error);
    EXPECT_EQ(result.output, "boom");
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, RpcErrorMapsToError) {
    ymh::test::ToolEnv env("mcp_integration_rpc_error");
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("rpc_error"), ToolConfig{}, env.env, env.governor,
                            registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    const ymh::payload::ToolResult result = call_tool(registry, env, "mcp.fake.echo");
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "RpcError");
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, BadSchemaToolIsSkipped) {
    ymh::test::ToolEnv env("mcp_integration_bad_schema");
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("bad_schema"), ToolConfig{}, env.env, env.governor,
                            registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    EXPECT_TRUE(registry.contains(ToolName{"mcp.fake.good"}));
    EXPECT_FALSE(registry.contains(ToolName{"mcp.fake.bad"}));
    const std::vector<ymh::McpServerStatus> statuses = manager.statuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses.front().state, McpServerState::Ready);
    ASSERT_EQ(statuses.front().skipped_tools.size(), 1u);
    EXPECT_EQ(statuses.front().skipped_tools.front(), "bad");
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, CrashMidCallIsIoError) {
    ymh::test::ToolEnv env("mcp_integration_crash");
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("crash_mid_call"), ToolConfig{}, env.env,
                            env.governor, registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    const ymh::payload::ToolResult result = call_tool(registry, env, "mcp.fake.echo");
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "TransportClosed");
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, OversizedFrameIsRejected) {
    ymh::test::ToolEnv env("mcp_integration_huge");
    ToolRegistry registry;
    McpConfig config = scenario_config("huge");
    config.max_frame_bytes = 1024;
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    manager.start({}).get();
    registry.freeze();

    const ymh::payload::ToolResult result = call_tool(registry, env, "mcp.fake.echo");
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Error);
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, HandshakeTimeoutIsBoundedAndNonFatal) {
    ymh::test::ToolEnv env("mcp_integration_hang");
    ToolRegistry registry;
    McpConfig config = scenario_config("hang");
    config.handshake_timeout = 200ms;
    config.startup_deadline = 400ms;
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    manager.start({}).get();
    registry.freeze();

    ASSERT_EQ(manager.statuses().size(), 1u);
    EXPECT_EQ(manager.statuses().front().state, McpServerState::Failed);
    EXPECT_FALSE(registry.contains(ToolName{"mcp.fake.echo"}));
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, RequiredHandshakeTimeoutAborts) {
    ymh::test::ToolEnv env("mcp_integration_required");
    ToolRegistry registry;
    McpConfig config = scenario_config("hang");
    config.handshake_timeout = 200ms;
    config.startup_deadline = 400ms;
    config.servers.front().required = true;
    ymh::McpManager manager(config, ToolConfig{}, env.env, env.governor, registry, env.bus,
                            env.logger);
    EXPECT_THROW(manager.start({}).get(), ymh::McpError);
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, CrashThenRefreshReconnectsAndLaterCallSucceeds) {
    ymh::test::ToolEnv env("mcp_integration_reconnect");
    const std::filesystem::path marker = env.workspace.path() / "crashed.once";
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("crash_once", marker.string()), ToolConfig{},
                            env.env, env.governor, registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    const ymh::payload::ToolResult first = call_tool(registry, env, "mcp.fake.echo");
    EXPECT_EQ(first.outcome, ymh::payload::ToolOutcome::Error);
    ASSERT_TRUE(first.error.has_value());
    EXPECT_EQ(*first.error, "TransportClosed");

    manager.refresh(ymh::McpServerId{"fake"}, {}).get();
    EXPECT_TRUE(registry.contains(ToolName{"mcp.fake.echo"}));

    const ymh::payload::ToolResult second = call_tool(registry, env, "mcp.fake.echo");
    EXPECT_EQ(second.outcome, ymh::payload::ToolOutcome::Ok);
    EXPECT_NE(second.output.find("echo:echo"), std::string::npos);
    manager.shutdown(300ms).get();
}

TEST(McpIntegrationTest, ShutdownKillsIgnoringServerAndReapsIt) {
    ymh::test::ToolEnv env("mcp_integration_shutdown");
    const std::filesystem::path pidfile = env.workspace.path() / "server.pid";
    ToolRegistry registry;
    ymh::McpManager manager(scenario_config("ignore_sigterm", pidfile.string()), ToolConfig{},
                            env.env, env.governor, registry, env.bus, env.logger);
    manager.start({}).get();
    registry.freeze();

    std::ifstream input(pidfile);
    int pid = 0;
    input >> pid;
    ASSERT_GT(pid, 0);

    manager.shutdown(200ms).get();

    errno = 0;
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(errno, ESRCH);
}

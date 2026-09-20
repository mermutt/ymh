#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/host/host_runtime.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/mcp/mcp_client.hpp"
#include "ymh/mcp/mcp_types.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class NullPermissionTransport final : public PermissionTransport {
public:
    bool broadcast_permission_request(protocol::PermissionRequest) override { return true; }
    bool schedule_after(std::chrono::milliseconds, std::function<void()>) override {
        return true;
    }
};

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

McpToolInfo tool_info(std::string name) {
    McpToolInfo info;
    info.remote_name.value = std::move(name);
    info.description = "test tool";
    info.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
    return info;
}

// A scripted MCP client that reaches Ready and reports a fixed tool set, so the
// mcp.status serializer is exercised with a real `McpServerStatus`.
class FakeMcpClient final : public McpClient {
public:
    FakeMcpClient(McpServerConfig config, std::vector<McpToolInfo> tools)
        : id_(config.id), tools_(std::move(tools)) {}

    const McpServerId& id() const noexcept override { return id_; }
    McpServerState state() const noexcept override { return state_; }

    Task<void> start(CancellationToken) override {
        state_ = McpServerState::Ready;
        return {};
    }

    Task<std::vector<McpToolInfo>> listTools(CancellationToken) override {
        return Task<std::vector<McpToolInfo>>(tools_);
    }

    Task<McpCallResult> callTool(std::string_view, const nlohmann::json&, const McpCallOptions&,
                                 CancellationToken) override {
        return Task<McpCallResult>(McpCallResult{});
    }

    std::optional<CallSlot> tryAcquireCallSlot() override { return CallSlot([] {}); }

    Task<bool> ping() override { return Task<bool>{true}; }

    Task<void> shutdown(std::chrono::milliseconds) override {
        state_ = McpServerState::Stopped;
        return {};
    }

    void setNotificationHandler(std::function<void(std::string_view, const nlohmann::json&)>,
                                std::function<void()>) override {}

    McpServerStatus status() const override {
        McpServerStatus status;
        status.id = id_;
        status.state = state_;
        return status;
    }

private:
    McpServerId             id_;
    std::vector<McpToolInfo> tools_;
    McpServerState          state_{McpServerState::Disabled};
};

McpServerSettings stdio_server(std::string id, std::vector<std::string> denied = {}) {
    McpServerSettings server;
    server.id = std::move(id);
    server.command = "unused";
    server.denied_tools = std::move(denied);
    return server;
}

// Owns a WorkspaceRuntime + HostRuntime wired with one scripted MCP server.
class McpRpcFixture {
public:
    explicit McpRpcFixture(const std::string& prefix, std::vector<McpServerSettings> servers,
                           std::vector<McpToolInfo> tools) {
        root_ = std::filesystem::canonical(workspace_.path());
        RegistryConfig registry_config;
        registry_config.db_path = registry_dir_.path() / "registry.db";
        registry_config.lock_path = registry_dir_.path() / "registry.lock";
        registry_config.workspace_roots = {};
        registry_ = WorkspaceRegistry::open(registry_config);
        record_ = registry_->registerWorkspace(root_, prefix);

        WorkspaceRuntimeOptions options;
        options.config = Config{};
        options.config.mcp.servers = std::move(servers);
        options.root = root_;
        options.boot_id = BootId{prefix + "-boot"};
        options.store_factory = [] { return std::make_unique<MemorySessionStore>(); };
        options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            FakeScript script;
            script.steps.push_back(text_step("ok"));
            return std::make_unique<FakeLLM>(script);
        };
        options.mcp_client_factory =
            [tools = std::move(tools)](const McpServerConfig& config, McpConfig&,
                                       ExecutionEnvironment&, ResourceGovernor&,
                                       Logger&) -> std::unique_ptr<McpClient> {
            return std::make_unique<FakeMcpClient>(config, tools);
        };
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(std::move(options));
        if (!created.has_value()) {
            throw std::runtime_error("runtime create failed: " + created.error().detail);
        }
        runtime_ = std::move(*created);

        transport_ = std::make_unique<NullPermissionTransport>();
        broker_ = std::make_unique<PermissionBroker>(runtime_->policy(), *transport_,
                                                     PermissionConfig{});

        HostIdentity identity;
        identity.workspace = record_.id;
        identity.boot_id = HostBootId{prefix + "-boot"};
        identity.pid = 4242;
        identity.socket_path = root_ / ".ymh" / "host.sock";
        host_ = std::make_unique<HostRuntime>(*runtime_, *registry_, identity, turns_, *broker_);
    }

    [[nodiscard]] HostRuntime& host() { return *host_; }

    ~McpRpcFixture() { (void)turns_.drain(std::chrono::milliseconds{2000}); }

private:
    TempWorkspace                      workspace_{"mcp_rpc_ws"};
    TempWorkspace                      registry_dir_{"mcp_rpc_reg"};
    std::filesystem::path              root_;
    std::unique_ptr<WorkspaceRegistry> registry_;
    WorkspaceRecord                    record_;
    std::unique_ptr<WorkspaceRuntime>  runtime_;
    std::unique_ptr<NullPermissionTransport> transport_;
    std::unique_ptr<PermissionBroker>  broker_;
    TurnExecutor                       turns_{1, 1};
    std::unique_ptr<HostRuntime>       host_;
};

// 45-D6/45-I13/I14: the mcp.status wire shape is the shared base schema plus
// `connected` and `skipped_tools`; `skipped` is a count and `last_error` is
// never shipped.
TEST(McpRpcTest, UI45_D6_McpStatusWireShape) {
    McpRpcFixture fixture("mcp-shape", {stdio_server("alpha", {"echo"})},
                          {tool_info("echo"), tool_info("search")});
    const nlohmann::json result = fixture.host().mcpStatus();
    ASSERT_TRUE(result.contains("servers"));
    ASSERT_TRUE(result.contains("tool_total"));
    ASSERT_EQ(result.at("servers").size(), 1u);
    const nlohmann::json& server = result.at("servers").at(0);

    EXPECT_EQ(server.at("id").get<std::string>(), "alpha");
    EXPECT_EQ(server.at("state").get<std::string>(), "ready");
    EXPECT_TRUE(server.at("connected").get<bool>());
    EXPECT_EQ(server.at("tool_count").get<std::size_t>(), 1u);
    EXPECT_TRUE(server.at("skipped").is_number_integer());
    EXPECT_EQ(server.at("skipped").get<std::size_t>(), 1u);
    ASSERT_TRUE(server.at("skipped_tools").is_array());
    EXPECT_EQ(server.at("skipped_tools").at(0).get<std::string>(), "echo");
    EXPECT_FALSE(server.at("has_error").get<bool>());
    EXPECT_EQ(result.at("tool_total").get<std::size_t>(), 1u);

    EXPECT_FALSE(server.contains("last_error"));
    EXPECT_FALSE(server.contains("server_name"));
    EXPECT_FALSE(server.contains("server_version"));
}

// 45-D6.6/45-I15: no configured servers is an empty result, never an error.
TEST(McpRpcTest, UI45_D6_NoMcpIsEmptyNotError) {
    McpRpcFixture fixture("mcp-empty", {}, {});
    const nlohmann::json result = fixture.host().mcpStatus();
    EXPECT_EQ(result.at("servers"), nlohmann::json::array());
    EXPECT_EQ(result.at("tool_total").get<std::size_t>(), 0u);
}

// 45-D6.9/45-I30: context.show and mcp.status share one JSON schema (two
// serializers); `context.show` keeps `skipped` a count and gains no
// `connected`/`skipped_tools`.
TEST(McpRpcTest, UI45_D6_McpSharedSchema) {
    McpRpcFixture fixture("mcp-shared", {stdio_server("alpha", {"echo"})},
                          {tool_info("echo"), tool_info("search")});
    const protocol::SessionCreated created = fixture.host().createSession(nlohmann::json::object());
    const nlohmann::json context = fixture.host().showContext(created.session);
    const nlohmann::json status = fixture.host().mcpStatus();

    ASSERT_EQ(context.at("mcp_servers").size(), 1u);
    ASSERT_EQ(status.at("servers").size(), 1u);
    const nlohmann::json& ctx = context.at("mcp_servers").at(0);
    const nlohmann::json& ext = status.at("servers").at(0);

    EXPECT_FALSE(ctx.contains("connected"));
    EXPECT_FALSE(ctx.contains("skipped_tools"));
    EXPECT_TRUE(ctx.at("skipped").is_number_integer());
    EXPECT_EQ(ctx.at("skipped").get<std::size_t>(), ext.at("skipped").get<std::size_t>());
    EXPECT_EQ(ctx.at("id").get<std::string>(), ext.at("id").get<std::string>());
    EXPECT_EQ(ctx.at("state").get<std::string>(), ext.at("state").get<std::string>());
    EXPECT_EQ(ctx.at("tool_count").get<std::size_t>(), ext.at("tool_count").get<std::size_t>());
    EXPECT_EQ(ctx.at("has_error").get<bool>(), ext.at("has_error").get<bool>());
}

} // namespace

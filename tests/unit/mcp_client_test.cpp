#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/logger.hpp"
#include "ymh/mcp/mcp_client.hpp"
#include "ymh/mcp/mcp_tool.hpp"
#include "ymh/mcp/mcp_transport.hpp"
#include "support/manual_clock.hpp"
#include "support/test_env.hpp"

namespace {

using namespace std::chrono_literals;
using ymh::McpCallOptions;
using ymh::McpClient;
using ymh::McpConfig;
using ymh::McpError;
using ymh::McpErrorCode;
using ymh::McpServerConfig;
using ymh::McpServerId;
using ymh::McpTransport;
using ymh::McpTransportKind;

nlohmann::json response_for(const nlohmann::json& request) {
    const std::string method = request.value("method", std::string{});
    if (method == "initialize") {
        return {{"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"result",
                 {{"protocolVersion", "2025-06-18"},
                  {"capabilities", nlohmann::json::object()},
                  {"serverInfo", {{"name", "fake"}, {"version", "1"}}}}}};
    }
    if (method == "tools/list") {
        return {{"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"result", {{"tools", nlohmann::json::array()}}}};
    }
    if (method == "tools/call") {
        return {{"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"result",
                 {{"content",
                   nlohmann::json::array(
                       {{{"type", "text"}, {"text", "ok"}}})}}}};
    }
    if (method == "ping") {
        return {{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", nlohmann::json::object()}};
    }
    return nlohmann::json::object();
}

class ScriptedTransport : public McpTransport, public ymh::McpPollableTransport {
public:
    using Responder = std::function<std::optional<nlohmann::json>(const nlohmann::json&)>;

    Responder responder = [](const nlohmann::json& request) {
        return std::optional<nlohmann::json>{response_for(request)};
    };
    std::function<void()> on_poll;
    std::vector<nlohmann::json> sent;
    bool started = false;
    bool closed = false;

    ymh::Task<void> start(ymh::CancellationToken) override {
        started = true;
        return {};
    }

    ymh::Task<void> send(const nlohmann::json& message, ymh::CancellationToken) override {
        sent.push_back(message);
        const bool is_request = message.contains("id") && !message["id"].is_null() &&
                                message.contains("method");
        if (is_request && responder) {
            if (std::optional<nlohmann::json> reply = responder(message);
                reply.has_value()) {
                deliver(*reply);
            }
        }
        return {};
    }

    void setMessageHandler(std::function<void(nlohmann::json)> handler) override {
        handler_ = std::move(handler);
    }

    void setCloseHandler(std::function<void(ymh::McpDisconnectReason)> handler) override {
        close_ = std::move(handler);
    }

    ymh::Task<void> close(std::chrono::milliseconds) override {
        closed = true;
        return {};
    }

    std::uint64_t childPid() const noexcept override { return 4242; }

    bool poll(std::chrono::milliseconds) override {
        if (on_poll) {
            on_poll();
        }
        if (queue_.empty()) {
            return false;
        }
        nlohmann::json message = std::move(queue_.front());
        queue_.pop_front();
        deliver(std::move(message));
        return true;
    }

    void deliver(nlohmann::json message) {
        if (handler_) {
            handler_(std::move(message));
        }
    }

    void queue(nlohmann::json message) { queue_.push_back(std::move(message)); }

private:
    std::function<void(nlohmann::json)>       handler_;
    std::function<void(ymh::McpDisconnectReason)> close_;
    std::deque<nlohmann::json>                queue_;
};

struct Harness {
    explicit Harness(ymh::test::ManualClock* clock = nullptr) {
        config.id.value = "srv";
        config.command = "unused";
        if (clock != nullptr) {
            reader = [clock] { return clock->now(); };
        }
        auto transport = std::make_unique<ScriptedTransport>();
        transport_ = transport.get();
        client = ymh::make_mcp_client_with_transport(config, mcp_config, std::move(transport),
                                                     logger, reader);
    }

    McpServerConfig              config;
    McpConfig                    mcp_config;
    ScriptedTransport*           transport_ = nullptr;
    ymh::NullLogger              logger;
    ymh::McpClockReader          reader = ymh::McpClock::now;
    std::unique_ptr<McpClient>   client;
};

bool throws_code(const std::function<void()>& fn, McpErrorCode code) {
    try {
        fn();
    } catch (const McpError& error) {
        return error.code() == code;
    } catch (...) {
        return false;
    }
    return false;
}

} // namespace

TEST(McpClientTest, HandshakeBeforeUse) {
    Harness harness;
    EXPECT_EQ(harness.client->state(), ymh::McpServerState::Disabled);
    harness.client->start({}).get();
    EXPECT_EQ(harness.client->state(), ymh::McpServerState::Ready);
    ASSERT_FALSE(harness.transport_->sent.empty());
    EXPECT_EQ(harness.transport_->sent.front()["method"], "initialize");
    bool initialized = false;
    for (const nlohmann::json& message : harness.transport_->sent) {
        if (message.value("method", std::string{}) == "notifications/initialized") {
            initialized = true;
        }
    }
    EXPECT_TRUE(initialized);
    EXPECT_EQ(harness.client->status().protocol_version, "2025-06-18");
}

TEST(McpClientTest, CallBeforeReadyFailsClosed) {
    Harness harness;
    EXPECT_TRUE(throws_code(
        [&] { (void)harness.client->callTool("x", nlohmann::json::object(), {}, {}).get(); },
        McpErrorCode::TransportClosed));
}

TEST(McpClientTest, UnsupportedRevisionRejectsHandshake) {
    Harness harness;
    harness.transport_->responder = [](const nlohmann::json& request) {
        nlohmann::json response = response_for(request);
        if (request.value("method", std::string{}) == "initialize") {
            response["result"]["protocolVersion"] = "1999-01-01";
        }
        return std::optional<nlohmann::json>{response};
    };
    EXPECT_TRUE(throws_code([&] { harness.client->start({}).get(); },
                            McpErrorCode::HandshakeRejected));
    EXPECT_EQ(harness.client->state(), ymh::McpServerState::Failed);
}

TEST(McpClientTest, CallToolMapsContent) {
    Harness harness;
    harness.client->start({}).get();
    const ymh::McpCallResult result =
        harness.client->callTool("echo", nlohmann::json::object(), {}, {}).get();
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content.at(0).at("text"), "ok");
}

TEST(McpClientTest, RpcErrorObjectThrows) {
    Harness harness;
    harness.client->start({}).get();
    harness.transport_->responder = [](const nlohmann::json& request) {
        if (request.value("method", std::string{}) == "tools/call") {
            return std::optional<nlohmann::json>{nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"error", {{"code", -32000}, {"message", "boom"}}}}};
        }
        return std::optional<nlohmann::json>{response_for(request)};
    };
    EXPECT_TRUE(throws_code(
        [&] { (void)harness.client->callTool("echo", nlohmann::json::object(), {}, {}).get(); },
        McpErrorCode::RpcError));
}

TEST(McpClientTest, CallTimeoutUsesInjectedClockAndSendsCancellation) {
    auto clock = std::make_shared<ymh::test::ManualClock>();
    Harness harness(clock.get());
    harness.client->start({}).get();
    harness.transport_->responder = [](const nlohmann::json& request) -> std::optional<nlohmann::json> {
        if (request.value("method", std::string{}) == "tools/call") {
            return std::nullopt;
        }
        return response_for(request);
    };
    harness.transport_->on_poll = [clock] { clock->advance(100ms); };

    McpCallOptions options;
    options.timeout = 500ms;
    EXPECT_TRUE(throws_code(
        [&] {
            (void)harness.client->callTool("echo", nlohmann::json::object(), options, {}).get();
        },
        McpErrorCode::CallTimeout));

    bool cancelled = false;
    for (const nlohmann::json& message : harness.transport_->sent) {
        if (message.value("method", std::string{}) == "notifications/cancelled") {
            cancelled = true;
        }
    }
    EXPECT_TRUE(cancelled);
}

TEST(McpClientTest, CancellationSendsNotification) {
    Harness harness;
    harness.client->start({}).get();
    harness.transport_->responder = [](const nlohmann::json& request) -> std::optional<nlohmann::json> {
        if (request.value("method", std::string{}) == "tools/call") {
            return std::nullopt;
        }
        return response_for(request);
    };
    harness.transport_->on_poll = [] {};

    ymh::CancellationSource source;
    source.cancel();
    EXPECT_TRUE(throws_code(
        [&] {
            (void)harness.client->callTool("echo", nlohmann::json::object(), {}, source.token())
                .get();
        },
        McpErrorCode::Cancelled));
    bool cancelled = false;
    for (const nlohmann::json& message : harness.transport_->sent) {
        if (message.value("method", std::string{}) == "notifications/cancelled") {
            cancelled = true;
        }
    }
    EXPECT_TRUE(cancelled);
}

TEST(McpClientTest, PingReturnsFalseOnTimeoutAndNeverThrows) {
    auto clock = std::make_shared<ymh::test::ManualClock>();
    Harness harness(clock.get());
    harness.client->start({}).get();
    harness.transport_->responder = [](const nlohmann::json& request) -> std::optional<nlohmann::json> {
        if (request.value("method", std::string{}) == "ping") {
            return std::nullopt;
        }
        return response_for(request);
    };
    harness.transport_->on_poll = [clock] { clock->advance(1000ms); };
    EXPECT_FALSE(harness.client->ping().get());
}

TEST(McpClientTest, SlotsReleaseOnEveryPath) {
    Harness harness;
    harness.client->start({}).get();
    harness.mcp_config.max_inflight_calls_per_server = 2;
    // The client was built with the default config copy; rebuild with the cap.
    auto transport = std::make_unique<ScriptedTransport>();
    ScriptedTransport* raw = transport.get();
    McpConfig capped;
    capped.max_inflight_calls_per_server = 1;
    McpServerConfig config;
    config.id.value = "srv";
    auto client = ymh::make_mcp_client_with_transport(config, capped, std::move(transport),
                                                      harness.logger);
    client->start({}).get();
    std::optional<McpClient::CallSlot> first = client->tryAcquireCallSlot();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(client->tryAcquireCallSlot().has_value());
    first.reset();
    EXPECT_TRUE(client->tryAcquireCallSlot().has_value());
    (void)raw;
}

TEST(McpClientTest, ServerInitiatedRequestIsRejected) {
    Harness harness;
    harness.client->start({}).get();
    harness.transport_->deliver({{"jsonrpc", "2.0"},
                                 {"id", 77},
                                 {"method", "sampling/createMessage"},
                                 {"params", nlohmann::json::object()}});
    bool rejected = false;
    for (const nlohmann::json& message : harness.transport_->sent) {
        if (message.contains("error") && message["error"]["code"] == -32601) {
            rejected = true;
        }
    }
    EXPECT_TRUE(rejected);
}

namespace {

class FakeClient : public McpClient {
public:
    explicit FakeClient(McpServerConfig config) : id_(config.id), config_(std::move(config)) {}

    const McpServerId& id() const noexcept override { return id_; }
    ymh::McpServerState state() const noexcept override { return ymh::McpServerState::Ready; }

    ymh::Task<void> start(ymh::CancellationToken) override { return {}; }

    ymh::Task<std::vector<ymh::McpToolInfo>> listTools(ymh::CancellationToken) override {
        return ymh::Task<std::vector<ymh::McpToolInfo>>(tools_);
    }

    ymh::Task<ymh::McpCallResult> callTool(std::string_view, const nlohmann::json&,
                                           const ymh::McpCallOptions&,
                                           ymh::CancellationToken) override {
        if (throw_error) {
            throw McpError{error_code, "fake"};
        }
        return ymh::Task<ymh::McpCallResult>(result_);
    }

    std::optional<CallSlot> tryAcquireCallSlot() override {
        if (slots_exhausted) {
            return std::nullopt;
        }
        return CallSlot([] {});
    }

    ymh::Task<bool> ping() override { return ymh::Task<bool>{true}; }
    ymh::Task<void> shutdown(std::chrono::milliseconds) override { return {}; }
    void setNotificationHandler(std::function<void(std::string_view, const nlohmann::json&)>,
                                std::function<void()>) override {}
    ymh::McpServerStatus status() const override { return {}; }

    std::vector<ymh::McpToolInfo> tools_;
    ymh::McpCallResult            result_;
    bool                          throw_error = false;
    McpErrorCode                  error_code = McpErrorCode::Internal;
    bool                          slots_exhausted = false;

private:
    McpServerId     id_;
    McpServerConfig config_;
};

} // namespace

TEST(McpToolTest, OkResultIsProjectedAndClamped) {
    McpServerConfig config;
    config.id.value = "srv";
    auto client = std::make_shared<FakeClient>(config);
    client->result_.content = nlohmann::json::array({{{"type", "text"}, {"text", "hello"}}});

    ymh::ToolSchema schema;
    schema.name.value = "mcp.srv.echo";
    ymh::McpTool tool(client, config, schema, ymh::McpRemoteToolName{"echo"}, 1u << 20);

    ymh::test::ToolEnv env("mcp_tool_ok");
    ymh::ToolContext context = env.context();
    ymh::ToolArguments arguments{nlohmann::json::object()};
    const ymh::ToolResult result = tool.execute(context, arguments).get();
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Ok);
    EXPECT_EQ(result.output, "hello");
}

TEST(McpToolTest, IsErrorMapsToErrorWithTextPreserved) {
    McpServerConfig config;
    config.id.value = "srv";
    auto client = std::make_shared<FakeClient>(config);
    client->result_.is_error = true;
    client->result_.content =
        nlohmann::json::array({{{"type", "text"}, {"text", "failed"}}});

    ymh::ToolSchema schema;
    schema.name.value = "mcp.srv.echo";
    ymh::McpTool tool(client, config, schema, ymh::McpRemoteToolName{"echo"}, 1u << 20);

    ymh::test::ToolEnv env("mcp_tool_error");
    ymh::ToolContext context = env.context();
    const ymh::ToolResult result =
        tool.execute(context, ymh::ToolArguments{nlohmann::json::object()}).get();
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Error);
    EXPECT_EQ(result.output, "failed");
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "ServerError");
}

TEST(McpToolTest, SaturatedSlotsMapToResourceExhausted) {
    McpServerConfig config;
    config.id.value = "srv";
    auto client = std::make_shared<FakeClient>(config);
    client->slots_exhausted = true;

    ymh::ToolSchema schema;
    schema.name.value = "mcp.srv.echo";
    ymh::McpTool tool(client, config, schema, ymh::McpRemoteToolName{"echo"}, 1u << 20);

    ymh::test::ToolEnv env("mcp_tool_cap");
    ymh::ToolContext context = env.context();
    const ymh::ToolResult result =
        tool.execute(context, ymh::ToolArguments{nlohmann::json::object()}).get();
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "CapExhausted");
}

TEST(McpToolTest, CancellationMapsToCancelledOutcome) {
    McpServerConfig config;
    config.id.value = "srv";
    auto client = std::make_shared<FakeClient>(config);
    client->throw_error = true;
    client->error_code = McpErrorCode::Cancelled;

    ymh::ToolSchema schema;
    schema.name.value = "mcp.srv.echo";
    ymh::McpTool tool(client, config, schema, ymh::McpRemoteToolName{"echo"}, 1u << 20);

    ymh::test::ToolEnv env("mcp_tool_cancel");
    ymh::ToolContext context = env.context();
    const ymh::ToolResult result =
        tool.execute(context, ymh::ToolArguments{nlohmann::json::object()}).get();
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Cancelled);
}

TEST(McpToolTest, OversizedResultIsClampedAndFlagged) {
    McpServerConfig config;
    config.id.value = "srv";
    auto client = std::make_shared<FakeClient>(config);
    client->result_.content = nlohmann::json::array(
        {{{"type", "text"}, {"text", std::string(20000, 'x')}}});

    ymh::ToolSchema schema;
    schema.name.value = "mcp.srv.echo";
    ymh::McpTool tool(client, config, schema, ymh::McpRemoteToolName{"echo"}, 256);

    ymh::test::ToolEnv env("mcp_tool_truncate");
    ymh::ToolContext context = env.context();
    const ymh::ToolResult result =
        tool.execute(context, ymh::ToolArguments{nlohmann::json::object()}).get();
    EXPECT_EQ(result.outcome, ymh::payload::ToolOutcome::Ok);
    EXPECT_TRUE(result.truncated);
    EXPECT_LT(result.output.size(), 20000u);
}

TEST(McpEnvTest, ResolvesReferencesAndRejectsMissing) {
    ::setenv("YMH_MCP_TEST_VAR", "secret", 1);
    const std::vector<std::pair<std::string, std::string>> resolved =
        ymh::resolve_mcp_env({"KEY=${YMH_MCP_TEST_VAR}", "PLAIN=x"});
    ASSERT_EQ(resolved.size(), 2u);
    EXPECT_EQ(resolved[0].first, "KEY");
    EXPECT_EQ(resolved[0].second, "secret");
    EXPECT_EQ(resolved[1].second, "x");
    EXPECT_THROW((void)ymh::resolve_mcp_env({"KEY=${YMH_MCP_MISSING_VAR_XYZ}"}), McpError);
    EXPECT_THROW((void)ymh::resolve_mcp_env({"NOEQUALS"}), McpError);
}

TEST(McpEnvTest, DollarDollarBraceIsLiteralEscape) {
    ::setenv("YMH_MCP_TEST_VAR", "secret", 1);
    const std::vector<std::pair<std::string, std::string>> resolved =
        ymh::resolve_mcp_env({"A=$${YMH_MCP_TEST_VAR}", "B=$$", "C=$${", "D=x$${y}"});
    ASSERT_EQ(resolved.size(), 4u);
    EXPECT_EQ(resolved[0].second, "${YMH_MCP_TEST_VAR}");
    EXPECT_EQ(resolved[1].second, "$$");
    EXPECT_EQ(resolved[2].second, "${");
    EXPECT_EQ(resolved[3].second, "x${y}");
}

#pragma once

// MCP client contract and the default session state machine (15 §4.3). The
// client owns exactly one transport at a time; a reconnect recreates the
// transport inside the same client. Handshake before use (M5); `Ready` is the
// only callable state (M9).

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/mcp/mcp_transport.hpp"
#include "ymh/mcp/mcp_types.hpp"

namespace ymh {

using McpClock = std::chrono::steady_clock;
using McpClockReader = std::function<McpClock::time_point()>;

class McpClient {
public:
    virtual ~McpClient() = default;

    virtual const McpServerId& id() const noexcept = 0;
    virtual McpServerState     state() const noexcept = 0;

    virtual Task<void> start(CancellationToken) = 0;

    virtual Task<std::vector<McpToolInfo>> listTools(CancellationToken) = 0;

    virtual Task<McpCallResult> callTool(std::string_view remote_tool,
                                         const nlohmann::json& arguments,
                                         const McpCallOptions&,
                                         CancellationToken) = 0;

    class CallSlot {
    public:
        CallSlot() noexcept = default;
        CallSlot(CallSlot&&) noexcept;
        CallSlot& operator=(CallSlot&&) noexcept;
        ~CallSlot();

        // Implementation seam: a client (or a test fake) supplies the release
        // callback; the slot invokes it exactly once on destruction.
        explicit CallSlot(std::function<void()> release) noexcept;

        [[nodiscard]] bool held() const noexcept { return static_cast<bool>(release_); }

    private:
        std::function<void()> release_;
    };

    [[nodiscard]] virtual std::optional<CallSlot> tryAcquireCallSlot() = 0;

    virtual Task<bool> ping() = 0;

    virtual Task<void> shutdown(std::chrono::milliseconds grace) = 0;

    virtual void setNotificationHandler(
        std::function<void(std::string_view method, const nlohmann::json& params)>,
        std::function<void()> on_tools_changed) = 0;

    virtual McpServerStatus status() const = 0;
};

using McpClientFactory =
    std::function<std::unique_ptr<McpClient>(const McpServerConfig&,
                                             McpConfig&,
                                             ExecutionEnvironment&,
                                             ResourceGovernor&,
                                             Logger&)>;

// Production factory: a StdioMcpTransport over `environment.process()`.
[[nodiscard]] std::unique_ptr<McpClient> make_stdio_mcp_client(
    const McpServerConfig& config,
    McpConfig& mcp_config,
    ExecutionEnvironment& environment,
    ResourceGovernor& governor,
    Logger& logger);

// Test seam (15 §11.2): a client over an injected transport (scripted or fake).
[[nodiscard]] std::unique_ptr<McpClient> make_mcp_client_with_transport(
    const McpServerConfig& config,
    McpConfig& mcp_config,
    std::unique_ptr<McpTransport> transport,
    Logger& logger,
    McpClockReader now = McpClock::now);

// `2025-06-18` / `2025-03-26` (OQ-11).
[[nodiscard]] bool is_supported_mcp_revision(std::string_view revision) noexcept;
[[nodiscard]] std::string_view newest_mcp_revision() noexcept;

} // namespace ymh

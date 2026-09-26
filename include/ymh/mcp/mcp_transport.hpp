#pragma once

// MCP transport seam and the v1 stdio implementation (15-mcp-adapter.md §4.2,
// §5.1). One transport at a time per client; a reconnect replaces the transport
// object inside the same client (§5.4). Framing is newline-delimited JSON, not
// 05's length-prefixed codec (§2.3).

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/mcp/mcp_types.hpp"

namespace ymh {

class McpTransport {
public:
    virtual ~McpTransport() = default;

    virtual Task<void> start(CancellationToken) = 0;

    virtual Task<void> send(const nlohmann::json& message, CancellationToken) = 0;

    virtual void setMessageHandler(std::function<void(nlohmann::json)>) = 0;
    virtual void setCloseHandler(std::function<void(McpDisconnectReason)>) = 0;

    virtual Task<void> close(std::chrono::milliseconds grace) = 0;

    virtual std::uint64_t childPid() const noexcept = 0;
};

// Additive implementation seam (not part of the pinned 15 §4.2 interface): a
// synchronous poll hook, so a client can bound a read with a deadline on the
// eager `Task` model this codebase materializes. A transport that does not
// implement it is expected to dispatch responses during `send` (the scripted
// test transport).
class McpPollableTransport {
public:
    virtual ~McpPollableTransport() = default;

    // Wait up to `timeout` for at least one inbound frame, dispatching handlers.
    // Returns false on timeout or EOF. Never blocks past the timeout.
    virtual bool poll(std::chrono::milliseconds timeout) = 0;
};

// Resolve "KEY=VALUE" entries; each VALUE may contain ${ENV} references read
// from the process environment. Throws McpError{ConfigInvalid} when a variable
// is missing or an entry is malformed. Resolved values are never logged (M12).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> resolve_mcp_env(
    const std::vector<std::string>& entries);

class StdioMcpTransport final : public McpTransport, public McpPollableTransport {
public:
    StdioMcpTransport(const McpServerConfig& config,
                      McpConfig& mcp_config,
                      ExecutionEnvironment& environment,
                      Logger& logger);
    ~StdioMcpTransport() override;

    StdioMcpTransport(const StdioMcpTransport&) = delete;
    StdioMcpTransport& operator=(const StdioMcpTransport&) = delete;

    Task<void> start(CancellationToken cancel) override;
    Task<void> send(const nlohmann::json& message, CancellationToken cancel) override;
    void setMessageHandler(std::function<void(nlohmann::json)> handler) override;
    void setCloseHandler(std::function<void(McpDisconnectReason)> handler) override;
    Task<void> close(std::chrono::milliseconds grace) override;
    std::uint64_t childPid() const noexcept override;

    bool poll(std::chrono::milliseconds timeout) override;

private:
    void dispatchLine(std::string line);
    void notifyClose(McpDisconnectReason reason);

    McpServerConfig                     config_;
    std::size_t                         max_frame_bytes_;
    bool                                log_child_stderr_;
    ExecutionEnvironment&               environment_;
    Logger&                             logger_;
    std::unique_ptr<ChildProcessHandle> child_;
    std::function<void(nlohmann::json)> message_handler_;
    std::function<void(McpDisconnectReason)> close_handler_;
    std::mutex                          send_mutex_;
    std::string                         buffer_;
    bool                                eof_ = false;
    bool                                close_notified_ = false;
};

} // namespace ymh

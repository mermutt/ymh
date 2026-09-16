#pragma once

// McpManager: the daemon-owned orchestrator (15 §4.7). It turns `McpConfig` into
// registered tools, owns every `McpClient`, and is the only MCP type that knows
// about the registry, the bus, and the environment.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ymh/core/event_bus.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/mcp/mcp_client.hpp"
#include "ymh/mcp/mcp_types.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {

// Deterministic backoff schedule (§5.4): base * 2^attempt, capped at
// reconnect_max_backoff, then scaled by the +/- jitter band. `jitter_roll` in
// [0,1] is the test seam (0.5 is the un-jittered midpoint).
[[nodiscard]] std::chrono::milliseconds compute_mcp_backoff(std::uint32_t attempt,
                                                            const McpConfig& config,
                                                            double jitter_roll);

class McpManager {
public:
    McpManager(McpConfig               config,
               const ToolConfig&       tools,
               ExecutionEnvironment&   environment,
               ResourceGovernor&       governor,
               ToolRegistry&           registry,
               EventBus&               bus,
               Logger&                 logger,
               McpClockReader          now = McpClock::now);

    ~McpManager();

    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    Task<void> start(CancellationToken cancel);

    Task<void> refresh(McpServerId id, CancellationToken cancel);

    Task<void> shutdown(std::chrono::milliseconds grace);

    [[nodiscard]] std::vector<McpServerStatus> statuses() const;

    void setClientFactory(McpClientFactory factory);

private:
    struct ServerSlot {
        McpServerConfig                           config;
        std::shared_ptr<McpClient>                client;
        std::optional<ToolRegistry::AdapterScope> scope;
        McpServerState                            state{McpServerState::Disabled};
        std::uint32_t                             reconnect_attempt{0};
        std::chrono::milliseconds                 next_backoff{0};
        std::vector<std::string>                  skipped_tools;
        std::size_t                               tool_count{0};
        std::string                               last_error;
    };

    void validate() const;
    std::unique_ptr<McpClient> makeClient(const McpServerConfig& config,
                                          std::chrono::milliseconds handshake_timeout);
    std::size_t effectiveResultBytes(const McpServerConfig& config) const;
    void installTools(ServerSlot& slot, std::vector<McpToolInfo> tools);
    void emitStatus(const ServerSlot& slot, std::string reason);
    void setState(ServerSlot& slot, McpServerState state, std::string reason);
    ServerSlot* findSlot(const McpServerId& id);

    McpConfig              config_;
    ToolConfig             tool_config_;
    ExecutionEnvironment&  environment_;
    ResourceGovernor&      governor_;
    ToolRegistry&          registry_;
    EventBus&              bus_;
    Logger&                logger_;
    McpClockReader         now_;
    McpClientFactory       factory_;
    std::vector<ServerSlot> slots_;
    bool                   shutdown_ = false;
    std::uint64_t          status_sequence_ = 0;
};

} // namespace ymh

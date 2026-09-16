#pragma once

// McpTool: the per-remote-tool adapter (15 §4.6). The only MCP type the registry
// and the loop ever touch; it is an ordinary `Tool` (M1) that observes
// `ctx.cancellation()`, acquires the client's in-flight slot, and never appends,
// never touches the UI, and never decides policy.

#include <cstddef>
#include <memory>
#include <string>

#include "ymh/mcp/mcp_client.hpp"
#include "ymh/mcp/mcp_types.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

class McpTool final : public Tool {
public:
    McpTool(std::shared_ptr<McpClient> client,
            McpServerConfig           server,
            ToolSchema                schema,
            McpRemoteToolName         remote_name,
            std::size_t               result_max_bytes);

    [[nodiscard]] ToolSchema schema() const override;

    Task<ToolResult> execute(const ToolContext&,
                             const ToolArguments&) override;

private:
    std::shared_ptr<McpClient> client_;
    McpServerConfig            server_;
    ToolSchema                 schema_;
    McpRemoteToolName          remote_name_;
    std::size_t                result_max_bytes_{0};
};

} // namespace ymh

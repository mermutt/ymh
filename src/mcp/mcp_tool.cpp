#include "ymh/mcp/mcp_tool.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "ymh/mcp/schema_translation.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {

McpTool::McpTool(std::shared_ptr<McpClient> client,
                 McpServerConfig           server,
                 ToolSchema                schema,
                 McpRemoteToolName         remote_name,
                 std::size_t               result_max_bytes)
    : client_(std::move(client)),
      server_(std::move(server)),
      schema_(std::move(schema)),
      remote_name_(std::move(remote_name)),
      result_max_bytes_(result_max_bytes) {}

ToolSchema McpTool::schema() const { return schema_; }

Task<ToolResult> McpTool::execute(const ToolContext& context,
                                  const ToolArguments& arguments) {
    ToolResult result;
    result.name = schema_.name.value;

    std::optional<McpClient::CallSlot> slot = client_->tryAcquireCallSlot();
    if (!slot.has_value()) {
        result.outcome = payload::ToolOutcome::Error;
        result.error = std::string{to_string(McpErrorCode::CapExhausted)};
        retain_tool_result(result, result_max_bytes_);
        return Task<ToolResult>(std::move(result));
    }

    McpCallOptions options;
    options.call_id = context.callId();
    options.timeout = server_.call_timeout;
    options.max_bytes = result_max_bytes_;
    if (context.has_deadline()) {
        options.deadline = context.remaining();
    }

    try {
        McpCallResult call =
            client_->callTool(remote_name_.value, arguments.value, options,
                              context.cancellation())
                .get();
        result.output = project_mcp_content(call.content, call.structured_content);
        if (call.is_error) {
            result.outcome = payload::ToolOutcome::Error;
            result.error = std::string{to_string(McpErrorCode::ServerError)};
        } else {
            result.outcome = payload::ToolOutcome::Ok;
        }
    } catch (const McpError& error) {
        if (error.code() == McpErrorCode::Cancelled) {
            result.outcome = payload::ToolOutcome::Cancelled;
        } else if (error.code() == McpErrorCode::CallTimeout) {
            throw ToolError{ToolErrorCode::Timeout, "mcp call timed out"};
        } else {
            result.outcome = payload::ToolOutcome::Error;
            result.error = std::string{to_string(error.code())};
        }
    } catch (const std::exception&) {
        result.outcome = payload::ToolOutcome::Error;
        result.error = std::string{to_string(McpErrorCode::Internal)};
    }

    retain_tool_result(result, result_max_bytes_);
    return Task<ToolResult>(std::move(result));
}

} // namespace ymh

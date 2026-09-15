#include "ymh/tools/tool_context.hpp"

#include <utility>

namespace ymh {

ToolContext::ToolContext(ExecutionEnvironment& execution,
                         Session& session,
                         Logger& logger,
                         CancellationToken cancellation,
                         ResourceGovernor& governor,
                         OutputSink& output,
                         PermissionHandle& permission,
                         ToolCallId call_id,
                         TurnId turn,
                         StepId step)
    : execution_(&execution),
      session_(&session),
      logger_(&logger),
      cancellation_(std::move(cancellation)),
      governor_(&governor),
      output_(&output),
      permission_(&permission),
      call_id_(std::move(call_id)),
      turn_(turn),
      step_(step) {}

void ToolContext::emit(Event event) const { session_->emit(std::move(event)); }

} // namespace ymh

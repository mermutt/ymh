#include "ymh/tools/tool_context.hpp"

#include <algorithm>
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
                         StepId step,
                         std::optional<Clock::time_point> deadline,
                         ClockReader clock)
    : execution_(&execution),
      session_(&session),
      logger_(&logger),
      cancellation_(std::move(cancellation)),
      governor_(&governor),
      output_(&output),
      permission_(&permission),
      call_id_(std::move(call_id)),
      turn_(turn),
      step_(step),
      deadline_(deadline),
      clock_(std::move(clock)) {}

void ToolContext::emit(Event event) const { session_->emit(std::move(event)); }

std::chrono::milliseconds ToolContext::remaining() const noexcept {
    if (!deadline_.has_value()) {
        return std::chrono::milliseconds::max();
    }
    const Clock::time_point now = clock_();
    if (now >= *deadline_) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline_ - now);
}

bool ToolContext::expired() const noexcept {
    return deadline_.has_value() && clock_() >= *deadline_;
}

} // namespace ymh

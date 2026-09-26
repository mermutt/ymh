#pragma once

// ToolContext (07 §5.1): everything one dispatched tool call receives. It is
// constructed by the loop, one per payload::ToolCall, non-owning, and never
// constructed by a tool. Accessors are const so a `const ToolContext&` — the
// Tool::execute parameter — is usable.

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

class ToolContext {
public:
    using Clock       = std::chrono::steady_clock;
    using ClockReader = std::function<Clock::time_point()>;

    ToolContext(ExecutionEnvironment& execution,
                Session&              session,
                Logger&               logger,
                CancellationToken     cancellation,
                ResourceGovernor&     governor,
                OutputSink&           output,
                PermissionHandle&     permission,
                ToolCallId            call_id,
                TurnId                turn,
                StepId                step,
                std::optional<Clock::time_point> deadline,
                ClockReader                      clock = Clock::now);

    ExecutionEnvironment& execution() const noexcept { return *execution_; }
    Session&              session() const noexcept { return *session_; }
    Logger&               logger() const noexcept { return *logger_; }
    CancellationToken     cancellation() const noexcept { return cancellation_; }
    void                  emit(Event event) const;

    ToolCallId callId() const noexcept { return call_id_; }
    SessionId  sessionId() const noexcept { return session_->id(); }
    TurnId     turn() const noexcept { return turn_; }
    StepId     step() const noexcept { return step_; }

    // 46-D8: an absent optional is the only "disabled" sentinel. A
    // default-constructed time_point is not (the steady clock's epoch is past).
    [[nodiscard]] bool has_deadline() const noexcept { return deadline_.has_value(); }
    [[nodiscard]] Clock::time_point deadline() const { return *deadline_; }
    // Total: max() when disabled, duration_cast-clamped at zero otherwise.
    [[nodiscard]] std::chrono::milliseconds remaining() const noexcept;
    [[nodiscard]] bool expired() const noexcept;

    const std::filesystem::path& root() const noexcept { return execution_->root(); }
    std::filesystem::path        resolve(std::string_view path) const {
        return execution_->resolve(path);
    }

    ResourceGovernor& governor() const noexcept { return *governor_; }
    OutputSink&       output() const noexcept { return *output_; }
    PermissionHandle& permission() const noexcept { return *permission_; }

private:
    ExecutionEnvironment* execution_;
    Session*              session_;
    Logger*               logger_;
    CancellationToken     cancellation_;
    ResourceGovernor*     governor_;
    OutputSink*           output_;
    PermissionHandle*     permission_;
    ToolCallId            call_id_;
    TurnId                turn_;
    StepId                step_;
    std::optional<Clock::time_point> deadline_;
    ClockReader                      clock_;
};

} // namespace ymh

#pragma once

// Daemon-side plan-mode controller (25-D2). The durable session log is the only
// source of truth; this object serializes `plan/mode` appends and holds an
// uncommitted selection. Owned by `WorkspaceRuntime::Impl`.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/plan_mode.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

enum class PlanModeSetResult : std::uint8_t {
    Unchanged,
    Committed,
    Queued,
    Cancelled,
};

class PlanModeController {
public:
    using AppendFn = std::function<void(const SessionId&, payload::PlanMode)>;
    using ProjectionFn = std::function<bool(const EventRange&)>;

    explicit PlanModeController(AppendFn append, ProjectionFn project = plan_mode_active);

    [[nodiscard]] bool active(const Session& session) const;

    PlanModeSetResult set(const SessionId& session, bool turn_open, bool active);

    void request_exit(const SessionId& session);

    bool apply_pending_at_step_start(Session& session);

    bool flush_pending_at_turn_end(Session& session) noexcept;

    void erase(const SessionId& session) noexcept;

private:
    struct ProjectionMemo {
        bool valid  = false;
        bool active = false;
    };

    void commit_(const SessionId& id, bool value);

    AppendFn                              append_;
    ProjectionFn                          project_;
    mutable std::mutex                    mutex_;
    std::map<SessionId, bool>             pending_;
    std::map<SessionId, bool>             pending_exit_;
    mutable std::map<SessionId, ProjectionMemo> memo_;
};

} // namespace ymh

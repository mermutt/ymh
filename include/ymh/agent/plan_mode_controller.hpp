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

    // `session` (not just its id) is required so the logged-state read, the
    // comparison, and the append all run under `commit_mutex_`: the comparison
    // is then always against the durable log — even when the memo is cold on a
    // resumed/forked session — and a concurrent `commit_` cannot invalidate the
    // projection in between (25 review H1 TOCTOU). `turn_open` is supplied by
    // the caller; idle => append now, open => queue (25-D2/L-1/L-2 semantics
    // unchanged).
    PlanModeSetResult set(const Session& session, bool turn_open, bool active);

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

    // `commit_` body; the caller must hold `commit_mutex_`.
    void commit_locked_(const SessionId& id, bool value);

    AppendFn                              append_;
    ProjectionFn                          project_;
    mutable std::mutex                    mutex_;
    // Serializes the whole commit (invalidate -> append -> record) so the memo
    // write cannot land in the opposite order to the log append when `commit_`
    // runs concurrently on the io thread and a TurnExecutor worker (25 review
    // M8). `set()` also holds it across the logged-state read and the append, so
    // the compare-and-append is atomic (25 review H1 TOCTOU). Distinct from
    // `mutex_` so a committed handler calling `active()` cannot deadlock (N12).
    std::mutex                            commit_mutex_;
    std::map<SessionId, bool>             pending_;
    std::map<SessionId, bool>             pending_exit_;
    mutable std::map<SessionId, ProjectionMemo> memo_;
};

} // namespace ymh

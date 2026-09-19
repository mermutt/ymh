#include "ymh/agent/plan_mode_controller.hpp"

#include <utility>

namespace ymh {

PlanModeController::PlanModeController(AppendFn append, ProjectionFn project)
    : append_(std::move(append)), project_(std::move(project)) {}

bool PlanModeController::active(const Session& session) const {
    const SessionId id = session.id();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = memo_.find(id);
        if (it != memo_.end() && it->second.valid) {
            return it->second.active;
        }
    }
    const bool folded = project_(session.events());
    std::lock_guard<std::mutex> lock(mutex_);
    ProjectionMemo& entry = memo_[id];
    if (!entry.valid) {
        entry.valid  = true;
        entry.active = folded;
    }
    return entry.active;
}

PlanModeSetResult PlanModeController::set(const SessionId& session, bool turn_open, bool active) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_exit_.erase(session);

        const auto memo = memo_.find(session);
        const bool logged =
            (memo != memo_.end() && memo->second.valid) ? memo->second.active : false;
        const auto pending = pending_.find(session);

        if (logged == active) {
            if (pending != pending_.end() && pending->second != active) {
                pending_.erase(pending);
                return PlanModeSetResult::Cancelled;
            }
            return PlanModeSetResult::Unchanged;
        }

        if (turn_open) {
            pending_[session] = active;
            return PlanModeSetResult::Queued;
        }

        pending_.erase(session);
    }
    commit_(session, active);
    return PlanModeSetResult::Committed;
}

void PlanModeController::request_exit(const SessionId& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(session);
    pending_exit_[session] = true;
}

bool PlanModeController::apply_pending_at_step_start(Session& session) {
    const SessionId id = session.id();
    bool  effective   = false;
    bool  has_commit  = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto pending = pending_.find(id);
        if (pending != pending_.end()) {
            effective  = pending->second;
            has_commit = true;
            pending_.erase(pending);
        } else {
            const auto exit = pending_exit_.find(id);
            if (exit != pending_exit_.end()) {
                effective  = false;
                has_commit = true;
                pending_exit_.erase(exit);
            }
        }
    }
    if (has_commit) {
        commit_(id, effective);
        return effective;
    }
    return active(session);
}

bool PlanModeController::flush_pending_at_turn_end(Session& session) noexcept {
    const SessionId id = session.id();
    bool  effective    = false;
    bool  has_commit   = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto pending = pending_.find(id);
        if (pending != pending_.end()) {
            effective  = pending->second;
            has_commit = true;
            pending_.erase(pending);
        } else {
            const auto exit = pending_exit_.find(id);
            if (exit != pending_exit_.end()) {
                effective  = false;
                has_commit = true;
                pending_exit_.erase(exit);
            }
        }
    }
    if (has_commit) {
        try {
            commit_(id, effective);
        } catch (...) {
            // A failed flush drops the pending selection; the durable log stands.
        }
    }
    return active(session);
}

void PlanModeController::erase(const SessionId& session) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(session);
    pending_exit_.erase(session);
    memo_.erase(session);
}

void PlanModeController::commit_(const SessionId& id, bool value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memo_[id].valid = false;
    }
    append_(id, payload::PlanMode{value});
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memo_[id] = ProjectionMemo{true, value};
    }
}

} // namespace ymh

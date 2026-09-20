#include "ymh/goal/goal_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "ymh/agent/agent.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {
namespace {

std::int64_t epoch_ms(std::chrono::system_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}

std::string trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(begin, end - begin + 1)};
}

bool is_lower_kebab(std::string_view code) {
    if (code.empty() || code.front() == '-' || code.back() == '-') {
        return false;
    }
    bool previous_dash = false;
    for (const char c : code) {
        if (c == '-') {
            if (previous_dash) {
                return false;
            }
            previous_dash = true;
            continue;
        }
        previous_dash = false;
        if (c < 'a' || c > 'z') {
            if (c < '0' || c > '9') {
                return false;
            }
        }
    }
    return true;
}

[[noreturn]] void fail(GoalErrorCode code, std::string message) {
    throw GoalError{code, std::move(message)};
}

GoalId next_goal_id(const GoalProjectionState& state) {
    GoalId highest = 0;
    for (const GoalId id : state.seen_goal_ids) {
        highest = std::max(highest, id);
    }
    if (state.current.has_value()) {
        highest = std::max(highest, state.current->goal.id);
    }
    return highest + 1;
}

} // namespace

GoalProjectionState apply_goal_projection(const GoalProjectionState& state,
                                          const EventRecord&         record) {
    if (state.failure.has_value()) {
        return state;
    }
    const Event& event = record.event;

    if (event.type == EventType::GoalChange) {
        const auto& change = event.payload.get<payload::GoalChange>();
        GoalProjectionState next = state;

        if (change.operation == payload::GoalOperation::Clear) {
            if (change.cleared.has_value()) {
                next.seen_goal_ids.push_back(change.cleared->id);
            } else if (next.current.has_value()) {
                next.seen_goal_ids.push_back(next.current->goal.id);
            }
            next.current = std::nullopt;
            return next;
        }

        if (!change.goal.has_value()) {
            next.failure = "goal/change is missing its snapshot";
            return next;
        }

        GoalProjection projection;
        projection.goal           = *change.goal;
        projection.rounds_started = change.rounds_started;
        const std::int64_t stamp  = epoch_ms(event.timestamp);
        projection.updated_at     = stamp;

        const bool same_goal =
            next.current.has_value() && next.current->goal.id == projection.goal.id;
        projection.created_at = same_goal ? next.current->created_at : stamp;

        if (change.operation == payload::GoalOperation::Create) {
            if (next.current.has_value() && next.current->goal.phase != GoalPhase::Complete) {
                next.failure = "goal create while a non-complete goal is current";
                return next;
            }
            if (std::find(next.seen_goal_ids.begin(), next.seen_goal_ids.end(),
                          projection.goal.id) != next.seen_goal_ids.end()) {
                next.failure = "goal id reused";
                return next;
            }
            next.seen_goal_ids.push_back(projection.goal.id);
        } else if (!same_goal) {
            next.failure = "goal mutation for a non-current goal";
            return next;
        }

        next.current = std::move(projection);
        return next;
    }

    if (event.type == EventType::UserMessage) {
        const auto& message = event.payload.get<payload::UserMessage>();
        if (message.source.kind != MessageSource::Kind::Goal || !message.source.goal.has_value()) {
            return state;
        }
        GoalProjectionState next = state;
        if (!next.current.has_value()) {
            next.failure = "goal round without a current goal";
            return next;
        }
        const GoalProjection& current = *next.current;
        const GoalMessageRef& round   = *message.source.goal;
        const bool admitted = round.goal_id == current.goal.id &&
                              round.revision == current.goal.revision &&
                              current.goal.phase == GoalPhase::Active &&
                              round.round == current.rounds_started + 1 &&
                              round.round <= current.goal.max_goal_rounds;
        if (!admitted) {
            next.failure = "invalid goal round (non-contiguous, stale revision, or over cap)";
            return next;
        }
        next.current->rounds_started = round.round;
        next.current->updated_at     = epoch_ms(event.timestamp);
        return next;
    }

    return state;
}

GoalService::GoalService(SessionManager& sessions, EventBus& bus, std::uint32_t default_max_rounds)
    : sessions_(&sessions), bus_(&bus), default_max_rounds_(default_max_rounds) {}

GoalProjectionState GoalService::projection(const SessionId& session) const {
    GoalProjectionState state;
    const std::shared_ptr<Session> handle = sessions_->sessionPtr(session);
    if (handle == nullptr) {
        throw UnknownSession("unknown session");
    }
    for (const EventRecord& record : handle->events()) {
        state = apply_goal_projection(state, record);
    }
    return state;
}

GoalProjection GoalService::require_current(const SessionId& session) const {
    GoalProjectionState state = projection(session);
    if (state.failure.has_value()) {
        fail(GoalErrorCode::InvalidTransition, *state.failure);
    }
    if (!state.current.has_value()) {
        fail(GoalErrorCode::NotFound, "no current goal");
    }
    return *state.current;
}

GoalView GoalService::view(const SessionId& session, const GoalProjection& projection) const {
    GoalView result;
    static_cast<GoalProjection&>(result) = projection;
    const auto it = activation_.find(session.value);
    result.activation = it == activation_.end() ? GoalActivation::Disarmed : it->second;
    return result;
}

void GoalService::set_activation(const SessionId& session, GoalActivation activation) {
    activation_[session.value] = activation;
}

std::optional<GoalView> GoalService::get(const Agent& agent) const {
    const SessionId        session = agent.session();
    const GoalProjectionState state = projection(session);
    if (state.failure.has_value()) {
        fail(GoalErrorCode::InvalidTransition, *state.failure);
    }
    if (!state.current.has_value()) {
        return std::nullopt;
    }
    return view(session, *state.current);
}

GoalView GoalService::create(Agent& agent, const CreateGoalRequest& request) {
    const SessionId          session = agent.session();
    const GoalProjectionState state  = projection(session);
    if (state.failure.has_value()) {
        fail(GoalErrorCode::InvalidTransition, *state.failure);
    }
    if (state.current.has_value() && state.current->goal.phase != GoalPhase::Complete) {
        fail(GoalErrorCode::AlreadyExists, "a non-complete goal is already current");
    }
    if (trim(request.objective).empty()) {
        fail(GoalErrorCode::InvalidObjective, "goal objective must be non-empty");
    }
    const std::uint32_t max_rounds = request.max_goal_rounds.value_or(default_max_rounds_);
    if (max_rounds == 0) {
        fail(GoalErrorCode::InvalidMaxRounds, "goal max_goal_rounds must be positive");
    }

    GoalSnapshot snapshot;
    snapshot.id              = next_goal_id(state);
    snapshot.revision        = 1;
    snapshot.objective       = request.objective;
    snapshot.phase           = GoalPhase::Active;
    snapshot.blocked         = std::nullopt;
    snapshot.max_goal_rounds = max_rounds;

    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Create, snapshot, std::nullopt, 0});
    set_activation(session, GoalActivation::Armed);
    return view(session, require_current(session));
}

GoalView GoalService::edit(Agent& agent, GoalRef ref, const EditGoalRequest& request) {
    const SessionId    session = agent.session();
    const GoalProjection current = require_current(session);
    if (ref.id != current.goal.id || ref.revision != current.goal.revision) {
        fail(GoalErrorCode::StaleRevision, "goal revision is stale");
    }
    if (!request.objective.has_value() && !request.max_goal_rounds.has_value()) {
        fail(GoalErrorCode::InvalidEdit, "goal edit carries no change");
    }

    GoalSnapshot snapshot = current.goal;
    snapshot.revision     = current.goal.revision + 1;
    if (request.objective.has_value()) {
        if (trim(*request.objective).empty()) {
            fail(GoalErrorCode::InvalidObjective, "goal objective must be non-empty");
        }
        snapshot.objective = *request.objective;
    }
    if (request.max_goal_rounds.has_value()) {
        if (*request.max_goal_rounds == 0) {
            fail(GoalErrorCode::InvalidMaxRounds, "goal max_goal_rounds must be positive");
        }
        snapshot.max_goal_rounds = *request.max_goal_rounds;
    }

    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Edit, snapshot, std::nullopt, current.rounds_started});
    return view(session, require_current(session));
}

GoalView GoalService::pause(Agent& agent, GoalRef ref) {
    const SessionId      session = agent.session();
    const GoalProjection current = require_current(session);
    if (ref.id != current.goal.id || ref.revision != current.goal.revision) {
        fail(GoalErrorCode::StaleRevision, "goal revision is stale");
    }
    if (current.goal.phase != GoalPhase::Active) {
        fail(GoalErrorCode::InvalidTransition, "goal pause requires an active goal");
    }

    GoalSnapshot snapshot = current.goal;
    snapshot.revision     = current.goal.revision + 1;
    snapshot.phase        = GoalPhase::Paused;
    snapshot.blocked      = std::nullopt;

    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Pause, snapshot, std::nullopt, current.rounds_started});
    set_activation(session, GoalActivation::Disarmed);
    return view(session, require_current(session));
}

GoalView GoalService::resume(Agent& agent, GoalRef ref) {
    const SessionId      session = agent.session();
    const GoalProjection current = require_current(session);
    if (ref.id != current.goal.id || ref.revision != current.goal.revision) {
        fail(GoalErrorCode::StaleRevision, "goal revision is stale");
    }
    if (current.goal.phase == GoalPhase::Complete) {
        fail(GoalErrorCode::InvalidTransition, "goal resume requires a non-complete goal");
    }
    if (current.rounds_started >= current.goal.max_goal_rounds) {
        fail(GoalErrorCode::InvalidTransition, "goal resume requires remaining rounds");
    }
    if (current.goal.phase == GoalPhase::Active &&
        view(session, current).activation == GoalActivation::Armed) {
        fail(GoalErrorCode::InvalidTransition, "goal is already active and armed");
    }

    GoalSnapshot snapshot = current.goal;
    snapshot.revision     = current.goal.revision + 1;
    snapshot.phase        = GoalPhase::Active;
    snapshot.blocked      = std::nullopt;

    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Resume, snapshot, std::nullopt, current.rounds_started});
    set_activation(session, GoalActivation::Armed);
    return view(session, require_current(session));
}

GoalView GoalService::complete(Agent& agent, GoalRef ref) {
    const SessionId      session = agent.session();
    const GoalProjection current = require_current(session);
    if (ref.id != current.goal.id || ref.revision != current.goal.revision) {
        fail(GoalErrorCode::StaleRevision, "goal revision is stale");
    }
    if (current.goal.phase == GoalPhase::Complete) {
        fail(GoalErrorCode::InvalidTransition, "goal is already complete");
    }

    GoalSnapshot snapshot = current.goal;
    snapshot.revision     = current.goal.revision + 1;
    snapshot.phase        = GoalPhase::Complete;
    snapshot.blocked      = std::nullopt;

    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Complete, snapshot, std::nullopt, current.rounds_started});
    set_activation(session, GoalActivation::Disarmed);
    return view(session, require_current(session));
}

GoalView GoalService::block(Agent& agent, GoalRef ref, GoalBlockReason reason) {
    const SessionId      session = agent.session();
    const GoalProjection current = require_current(session);
    if (ref.id != current.goal.id || ref.revision != current.goal.revision) {
        fail(GoalErrorCode::StaleRevision, "goal revision is stale");
    }
    if (current.goal.phase != GoalPhase::Active) {
        fail(GoalErrorCode::InvalidTransition, "goal block requires an active goal");
    }
    if (!is_lower_kebab(reason.code)) {
        fail(GoalErrorCode::InvalidBlockReason, "goal block code must be lower-kebab-case");
    }
    reason.message = trim(reason.message);
    if (reason.message.empty()) {
        fail(GoalErrorCode::InvalidBlockReason, "goal block message must be non-empty");
    }

    GoalSnapshot snapshot = current.goal;
    snapshot.revision     = current.goal.revision + 1;
    snapshot.phase        = GoalPhase::Blocked;
    snapshot.blocked      = std::move(reason);

    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Block, snapshot, std::nullopt, current.rounds_started});
    set_activation(session, GoalActivation::Disarmed);
    return view(session, require_current(session));
}

void GoalService::clear(Agent& agent, GoalRef ref) {
    const SessionId      session = agent.session();
    const GoalProjection current = require_current(session);
    if (ref.id != current.goal.id || ref.revision != current.goal.revision) {
        fail(GoalErrorCode::StaleRevision, "goal revision is stale");
    }

    const GoalRef tombstone{current.goal.id, current.goal.revision + 1};
    sessions_->sessionPtr(session)->append(payload::GoalChange{
        payload::GoalOperation::Clear, std::nullopt, tombstone, 0});
    set_activation(session, GoalActivation::Disarmed);
}

GoalView GoalService::disarm(Agent& agent) {
    const SessionId session = agent.session();
    set_activation(session, GoalActivation::Disarmed);
    const GoalProjectionState state = projection(session);
    if (state.failure.has_value() || !state.current.has_value()) {
        GoalView empty;
        empty.activation = GoalActivation::Disarmed;
        return empty;
    }
    return view(session, *state.current);
}

} // namespace ymh

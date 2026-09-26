#pragma once

// The goal service and its pure replay projection, pinned by
// 44-goals-jobs-commands.md §5.2. Goal state is event-sourced (44-I1): the
// durable source of truth is the `goal/change` event, and the projection is a
// total pure fold over the session log. Single-threaded: owned by
// WorkspaceRuntime and called only on the agent executor thread (44-I17).

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "ymh/core/event.hpp"
#include "ymh/goal/goal.hpp"

namespace ymh {

class Agent;
class SessionManager;
class EventBus;

// dsh GoalErrorCode (dsh-goal/lib/types/domain.d.ts:75), lower-snake ymh form.
enum class GoalErrorCode : std::uint8_t {
    AgentNotLive,
    NotFound,
    AlreadyExists,
    StaleRevision,
    InvalidObjective,
    InvalidMaxRounds,
    InvalidBlockReason,
    InvalidEdit,
    InvalidTransition,
};

struct GoalError {
    GoalErrorCode code = GoalErrorCode::NotFound;
    std::string   message;
};

// Pure projection, the goal analogue of deriveMessages (01 §6.3). Folds one
// event into the state; the first strict replay error is retained as `failure`
// (44 §2.4). Total over every EventType.
[[nodiscard]] GoalProjectionState apply_goal_projection(const GoalProjectionState& state,
                                                        const EventRecord&        record);

class GoalService {
public:
    GoalService(SessionManager& sessions, EventBus& bus, std::uint32_t default_max_rounds);

    GoalService(const GoalService&) = delete;
    GoalService& operator=(const GoalService&) = delete;

    // All mutators append exactly one `goal/change` and return the fresh view.
    [[nodiscard]] std::optional<GoalView> get(const Agent&) const;
    GoalView create(Agent&, const CreateGoalRequest&);
    GoalView edit(Agent&, GoalRef, const EditGoalRequest&);
    GoalView pause(Agent&, GoalRef);
    GoalView resume(Agent&, GoalRef);
    GoalView complete(Agent&, GoalRef);
    GoalView block(Agent&, GoalRef, GoalBlockReason);
    void     clear(Agent&, GoalRef);

    // Removes process-local continuation authority without a durable change
    // (44 §5.2). Used by the lifetime owner before unload.
    GoalView disarm(Agent&);

    // The round driver's event source (44 §5.3); the pinned driver ctor omits
    // it, so the bus is carried here.
    [[nodiscard]] EventBus& bus() const noexcept { return *bus_; }

    [[nodiscard]] std::uint32_t default_max_rounds() const noexcept { return default_max_rounds_; }

private:
    [[nodiscard]] GoalProjectionState projection(const SessionId&) const;
    [[nodiscard]] GoalProjection      require_current(const SessionId&) const;
    [[nodiscard]] GoalView            view(const SessionId&, const GoalProjection&) const;
    void                              set_activation(const SessionId&, GoalActivation);

    SessionManager*                                      sessions_ = nullptr;
    EventBus*                                            bus_      = nullptr;
    std::uint32_t                                        default_max_rounds_ = 256;
    mutable std::unordered_map<std::string, GoalActivation> activation_;
};

} // namespace ymh

#pragma once

// The goal round driver, pinned by 44-goals-jobs-commands.md §5.3. It owns
// automatic same-session continuation: on `turn/end`, while an armed active
// goal has rounds remaining, it enqueues exactly one goal-sourced
// `user/message` via `Agent::followup`; at the cap it blocks `round-limit`.
// Single-threaded (44-I17).

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/agent/message.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/goal/goal.hpp"
#include "ymh/goal/goal_service.hpp"

namespace ymh {

class Agent;
class AgentRegistry;
struct AgentId;

// The verbatim round prompt (44 §2.6); `objective` is JSON-escaped.
[[nodiscard]] std::vector<ContentBlock> render_goal_round_prompt(const GoalView& goal,
                                                                 RoundNumber     round);

enum class GoalRoundDecision : std::uint8_t { None, Enqueue, BlockRoundLimit };

// The pure turn/end decision: continue, stop, or block at the cap.
[[nodiscard]] GoalRoundDecision plan_goal_round(const GoalView& goal) noexcept;

class GoalRoundDriver {
public:
    // The executor-post seam. The bus publishes committed events while the
    // session's append mutex is held, so the handler must defer any session
    // read; the pinned two-arg ctor leaves this empty (immediate) for tests.
    using Post = std::function<void(std::function<void()>)>;

    // The agent lookup seam; the pinned two-arg ctor adapts AgentRegistry.
    using Lookup = std::function<std::shared_ptr<Agent>(const SessionId&)>;

    GoalRoundDriver(GoalService& goals, AgentRegistry& agents, Post post = {});
    GoalRoundDriver(GoalService& goals, Lookup lookup, Post post = {});
    ~GoalRoundDriver();

    GoalRoundDriver(const GoalRoundDriver&) = delete;
    GoalRoundDriver& operator=(const GoalRoundDriver&) = delete;

    void start();   // subscribes; idempotent
    void stop();    // disarms all agents, unsubscribes

    // Processes the turn/end observations queued by the bus handler. The
    // handler runs under the session's append mutex (44 §5.3), so it only
    // enqueues; the executor calls pump() off that lock.
    void pump();

    // The pre-step fail-closed re-check (44-I7). Returns true when the queued
    // round still owns the exact live revision; otherwise it is rejected and a
    // still-live goal blocks with `prompt-rejected`.
    [[nodiscard]] bool admit_round(Agent& agent, const GoalMessageRef& round);

    [[nodiscard]] std::optional<GoalMessageRef> pending(const AgentId& agent) const;

private:
    void handle_turn_end(const Event& event);
    void run_turn_end(const SessionId& session);

    GoalService&                                    goals_;
    AgentRegistry*                                  registry_ = nullptr;
    Lookup                                          lookup_;
    Post                                            post_;
    Subscription                                    subscription_;
    bool                                            started_ = false;
    std::vector<SessionId>                          pending_turns_;
    std::unordered_map<std::string, GoalMessageRef> pending_;
    std::unordered_map<std::string, SessionId>      sessions_;
};

} // namespace ymh

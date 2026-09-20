#include "ymh/goal/round_driver.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"

namespace ymh {
namespace {

constexpr std::string_view kRoundPromptTail =
    "Continue working toward the objective in this same session. Treat the current\n"
    "workspace, tool results, and durable session state as authoritative; inspect\n"
    "them instead of assuming earlier narration is still current. Make concrete\n"
    "progress and verify the result. Before claiming completion, gather evidence\n"
    "that the whole objective is achieved, read the current goal, and mark it\n"
    "complete. If work remains, leave the goal active for the next round. Follow the\n"
    "configured goal-tool policy before reporting a blocker.\n";

} // namespace

std::vector<ContentBlock> render_goal_round_prompt(const GoalView& goal, RoundNumber round) {
    std::string text;
    text += "<goal_round>\n";
    text += "Objective: " + nlohmann::json(goal.goal.objective).dump() + "\n";
    text += "Round: " + std::to_string(round) + "/" + std::to_string(goal.goal.max_goal_rounds) +
            "\n\n";
    text += kRoundPromptTail;
    text += "</goal_round>";
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return {std::move(block)};
}

GoalRoundDecision plan_goal_round(const GoalView& goal) noexcept {
    if (goal.goal.phase != GoalPhase::Active || goal.activation != GoalActivation::Armed) {
        return GoalRoundDecision::None;
    }
    if (goal.rounds_started >= goal.goal.max_goal_rounds) {
        return GoalRoundDecision::BlockRoundLimit;
    }
    return GoalRoundDecision::Enqueue;
}

GoalRoundDriver::GoalRoundDriver(GoalService& goals, AgentRegistry& agents, Post post)
    : GoalRoundDriver(
          goals,
          [&agents](const SessionId& session) -> std::shared_ptr<Agent> {
              return agents.findShared(session);
          },
          std::move(post)) {
    registry_ = &agents;
}

GoalRoundDriver::GoalRoundDriver(GoalService& goals, Lookup lookup, Post post)
    : goals_(goals), lookup_(std::move(lookup)), post_(std::move(post)) {}

GoalRoundDriver::~GoalRoundDriver() {
    stop();
}

void GoalRoundDriver::start() {
    if (started_) {
        return;
    }
    started_       = true;
    subscription_ = goals_.bus().subscribe([this](const Event& event) {
        if (event.type == EventType::TurnEnded) {
            handle_turn_end(event);
        }
    });
}

void GoalRoundDriver::stop() {
    subscription_.unsubscribe();
    started_ = false;
    if (registry_ != nullptr) {
        for (const AgentId& id : registry_->list()) {
            if (const std::shared_ptr<AgentLoop> agent = registry_->getShared(id)) {
                goals_.disarm(*agent);
            }
        }
    } else {
        for (const auto& [agent_id, session] : sessions_) {
            if (const std::shared_ptr<Agent> agent = lookup_ ? lookup_(session) : nullptr) {
                goals_.disarm(*agent);
            }
        }
    }
    pending_.clear();
    sessions_.clear();
    pending_turns_.clear();
}

void GoalRoundDriver::handle_turn_end(const Event& event) {
    pending_turns_.push_back(event.session_id);
    if (post_) {
        post_([this] { pump(); });
    }
}

void GoalRoundDriver::pump() {
    std::vector<SessionId> turns;
    turns.swap(pending_turns_);
    for (const SessionId& session : turns) {
        run_turn_end(session);
    }
}

void GoalRoundDriver::run_turn_end(const SessionId& session) {
    const std::shared_ptr<Agent> agent = lookup_ ? lookup_(session) : nullptr;
    if (agent == nullptr) {
        return;
    }
    sessions_[agent->id().value] = session;
    const std::optional<GoalView> goal = goals_.get(*agent);
    if (!goal.has_value()) {
        return;
    }
    switch (plan_goal_round(*goal)) {
        case GoalRoundDecision::None:
            return;
        case GoalRoundDecision::BlockRoundLimit:
            goals_.block(*agent, GoalRef{goal->goal.id, goal->goal.revision},
                         GoalBlockReason{"round-limit", "goal reached max_goal_rounds"});
            return;
        case GoalRoundDecision::Enqueue:
            break;
    }

    const RoundNumber round = goal->rounds_started + 1;
    GoalMessageRef    ref{goal->goal.id, goal->goal.revision, round};
    Message           message;
    message.role    = Role::User;
    message.content = render_goal_round_prompt(*goal, round);
    message.source  = goal_message_source(ref);
    if (agent->followup(std::move(message)) != InboxResult::Accepted) {
        goals_.block(*agent, GoalRef{goal->goal.id, goal->goal.revision},
                     GoalBlockReason{"queue-failed", "goal round could not be enqueued"});
        return;
    }
    pending_[agent->id().value] = ref;
}

bool GoalRoundDriver::admit_round(Agent& agent, const GoalMessageRef& round) {
    const auto it = pending_.find(agent.id().value);
    if (it == pending_.end() || !(it->second == round)) {
        return false;
    }
    pending_.erase(it);

    const std::optional<GoalView> goal = goals_.get(agent);
    if (!goal.has_value()) {
        return false;
    }
    const bool admissible = goal->goal.id == round.goal_id &&
                            goal->goal.revision == round.revision &&
                            goal->goal.phase == GoalPhase::Active &&
                            goal->activation == GoalActivation::Armed &&
                            round.round == goal->rounds_started + 1;
    if (admissible) {
        return true;
    }
    if (goal->goal.phase == GoalPhase::Active) {
        goals_.block(agent, GoalRef{goal->goal.id, goal->goal.revision},
                     GoalBlockReason{"prompt-rejected", "goal changed after the round was queued"});
    }
    return false;
}

std::optional<GoalMessageRef> GoalRoundDriver::pending(const AgentId& agent) const {
    const auto it = pending_.find(agent.value);
    if (it == pending_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace ymh

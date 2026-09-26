#include "ymh/goal/goal_command.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ymh/agent/agent.hpp"
#include "ymh/goal/goal_service.hpp"

namespace ymh {
namespace {

std::string trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(begin, end - begin + 1)};
}

std::string legal_next_commands(const GoalView& goal) {
    switch (goal.goal.phase) {
        case GoalPhase::Active:
            if (goal.activation == GoalActivation::Armed) {
                return "/goal pause, /goal edit <objective>, /goal clear";
            }
            return "/goal resume, /goal pause, /goal edit <objective>, /goal clear";
        case GoalPhase::Paused:
        case GoalPhase::Blocked:
            return "/goal resume, /goal edit <objective>, /goal clear";
        case GoalPhase::Complete:
            return "/goal <objective>, /goal clear";
    }
    return {};
}

std::string render_goal(const GoalView& goal) {
    std::string text;
    text += "Objective: " + goal.goal.objective + "\n";
    text += "Phase: " + std::string{goal_phase_name(goal.goal.phase)} + "\n";
    if (goal.goal.blocked.has_value()) {
        text += "Blocked: " + goal.goal.blocked->code + ": " + goal.goal.blocked->message + "\n";
    }
    text += "Rounds: " + std::to_string(goal.rounds_started) + "/" +
            std::to_string(goal.goal.max_goal_rounds) + "\n";
    text += "Activation: " +
            std::string{goal.activation == GoalActivation::Armed ? "armed" : "disarmed"} + "\n";
    text += "Next: " + legal_next_commands(goal);
    return text;
}

CommandOutcome success(std::string text) {
    return CommandOutcome{CommandOutcomeKind::Success, std::move(text), std::nullopt};
}

CommandOutcome error(std::string text) {
    return CommandOutcome{CommandOutcomeKind::Error, std::move(text), std::nullopt};
}

CommandOutcome require_current_ref(GoalService& goals, Agent& agent, GoalRef& ref) {
    const std::optional<GoalView> goal = goals.get(agent);
    if (!goal.has_value()) {
        return error("No goal is current.");
    }
    ref = GoalRef{goal->goal.id, goal->goal.revision};
    return success(render_goal(*goal));
}

} // namespace

CommandOutcome run_goal_command(GoalService& goals, const CommandInput& input, Agent& agent) {
    const std::string args = trim(input.raw_input);
    try {
        if (args.empty()) {
            const std::optional<GoalView> goal = goals.get(agent);
            if (!goal.has_value()) {
                return success("No goal is current.\nNext: /goal <objective>");
            }
            return success(render_goal(*goal));
        }

        if (args == "clear") {
            GoalRef ref;
            if (const CommandOutcome failure = require_current_ref(goals, agent, ref);
                failure.kind == CommandOutcomeKind::Error) {
                return failure;
            }
            goals.clear(agent, ref);
            return success("Goal cleared.");
        }
        if (args == "pause") {
            GoalRef ref;
            if (const CommandOutcome failure = require_current_ref(goals, agent, ref);
                failure.kind == CommandOutcomeKind::Error) {
                return failure;
            }
            return success(render_goal(goals.pause(agent, ref)));
        }
        if (args == "resume") {
            GoalRef ref;
            if (const CommandOutcome failure = require_current_ref(goals, agent, ref);
                failure.kind == CommandOutcomeKind::Error) {
                return failure;
            }
            return success(render_goal(goals.resume(agent, ref)));
        }

        const std::size_t space = args.find_first_of(" \t");
        const std::string verb = space == std::string::npos ? args : args.substr(0, space);
        const std::string rest =
            space == std::string::npos ? std::string{} : trim(args.substr(space + 1));

        if (verb == "edit") {
            if (rest.empty()) {
                return error("Invalid edit: /goal edit requires an objective.");
            }
            GoalRef ref;
            if (const CommandOutcome failure = require_current_ref(goals, agent, ref);
                failure.kind == CommandOutcomeKind::Error) {
                return failure;
            }
            EditGoalRequest request;
            request.objective = rest;
            return success(render_goal(goals.edit(agent, ref, request)));
        }

        CreateGoalRequest request;
        request.objective = args;
        return success(render_goal(goals.create(agent, request)));
    } catch (const GoalError& goal_error) {
        return error(goal_error.message);
    }
}

CommandSpec make_goal_command(GoalService& goals) {
    CommandSpec spec;
    spec.name        = "goal";
    spec.description = "Create, view, edit, pause, resume, or clear the session goal";
    spec.input_hint  = "[<objective>|clear|edit <objective>|pause|resume]";
    spec.handler     = [&goals](const CommandInput& input, Agent& agent) {
        return run_goal_command(goals, input, agent);
    };
    return spec;
}

} // namespace ymh

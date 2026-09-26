#pragma once

// The `/goal` durable command (44-goals-jobs-commands.md §2.8, 44-D6). Its
// grammar is copied from dsh (`dsh-command-goal/lib/index.js:10-24`):
//
//   /goal [<objective>|clear|edit <objective>|pause|resume]
//
// `complete` and `block` are model-tool verbs, not `/goal` verbs. The handler
// is registered into the host-side CommandRegistry by the D20 surface; this
// header pins the grammar and the CommandSpec the registry consumes.

#include "ymh/commands/command.hpp"

namespace ymh {

class Agent;
class GoalService;

// Parses the pinned grammar and drives `GoalService`. `input.raw_input` is the
// argument string after the command name (verbatim, separator whitespace
// included).
[[nodiscard]] CommandOutcome run_goal_command(GoalService& goals,
                                              const CommandInput& input,
                                              Agent& agent);

// The `CommandSpec` the host-side registry registers (44 §5.6).
[[nodiscard]] CommandSpec make_goal_command(GoalService& goals);

} // namespace ymh

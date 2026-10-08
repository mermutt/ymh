#pragma once

// The `/handoff` durable command (82-handoff-command-errata.md §5.3). The
// handler parses the pinned grammar `/handoff [--no-seed] [--out <path>]
// [--title <text>]`, resolves the source session through the injected
// `SessionManager`, calls `HandoffService::run`, and maps its result to a
// `CommandOutcome`. Registered into the host-side `ymh::CommandRegistry` by
// `WorkspaceRuntime`; mirrors `make_goal_command`.

#include "ymh/commands/command.hpp"

namespace ymh {

class Agent;
class HandoffService;

[[nodiscard]] CommandOutcome run_handoff_command(HandoffService&     service,
                                                 const CommandInput& input,
                                                 Agent&              agent);

[[nodiscard]] CommandSpec make_handoff_command(HandoffService& service);

} // namespace ymh

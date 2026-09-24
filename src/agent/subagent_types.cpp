#include "ymh/agent/subagent_types.hpp"

#include <string>
#include <string_view>

namespace ymh {

std::string delegation_guidance_text(std::string_view tool_name) {
    return "Use " + std::string{tool_name} +
           " in the background by default. Start independent delegations "
           "together in one assistant message and continue useful work while "
           "they run. Set `run_in_background: false` only when your next action "
           "depends on that subagent's result. When a background run settles, "
           "the runtime sends you a notice containing its outcome and any "
           "final assistant message.";
}

} // namespace ymh

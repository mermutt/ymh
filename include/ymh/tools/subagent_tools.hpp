#pragma once

// 55-D2/§4: the model-facing delegation tools, thin adapters over
// `SubagentService`. The caller resolver mirrors `JobOwnerResolver` because
// `ToolContext` carries no agent id.

#include <functional>
#include <memory>
#include <optional>

#include "ymh/agent/subagent_service.hpp"
#include "ymh/agent/subagent_types.hpp"
#include "ymh/session/ids.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

class ToolContext;
class RouteCatalog;

using SubagentCallerResolver = std::function<std::optional<SessionId>(const ToolContext&)>;

[[nodiscard]] std::unique_ptr<Tool> make_subagent_tool(SubagentService&       service,
                                                       SubagentCallerResolver caller,
                                                       DelegationToolConfig   config);
[[nodiscard]] std::unique_ptr<Tool> make_send_message_tool(SubagentService&       service,
                                                           SubagentCallerResolver caller);
[[nodiscard]] std::unique_ptr<Tool> make_interrupt_agent_tool(SubagentService&       service,
                                                              SubagentCallerResolver caller);
[[nodiscard]] std::unique_ptr<Tool> make_list_agents_tool(SubagentService&       service,
                                                          SubagentCallerResolver caller);
[[nodiscard]] std::unique_ptr<Tool> make_list_subagent_models_tool(const RouteCatalog& catalog);

} // namespace ymh

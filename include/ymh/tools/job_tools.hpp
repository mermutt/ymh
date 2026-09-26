#pragma once

// The model-facing job controls, pinned by 44-goals-jobs-commands.md §5.5
// (26-D19): `job_output`, `job_list`, `job_kill`. Each is owner-scoped through
// the caller's `AgentId` (44-I8); the owner resolver supplies the executing
// agent's identity, which `ToolContext` does not carry.

#include <functional>
#include <memory>
#include <optional>

#include "ymh/agent/ids.hpp"
#include "ymh/jobs/job_registry.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

class ToolContext;

using JobOwnerResolver = std::function<std::optional<AgentId>(const ToolContext&)>;

[[nodiscard]] std::unique_ptr<Tool> make_job_output_tool(JobRegistry&     registry,
                                                         JobOwnerResolver owner);
[[nodiscard]] std::unique_ptr<Tool> make_job_list_tool(JobRegistry&     registry,
                                                       JobOwnerResolver owner);
[[nodiscard]] std::unique_ptr<Tool> make_job_kill_tool(JobRegistry&     registry,
                                                       JobOwnerResolver owner);

} // namespace ymh

#pragma once

// Read-only git tools (07 §4.3, §6.6): `git_status` and `git_diff`. Both route
// the repository base through `ToolContext::resolve()` / `root()` so every path
// obeys the workspace containment rule; neither issues a git write.

#include <memory>

#include "ymh/execution/config.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

[[nodiscard]] std::unique_ptr<Tool> make_git_status_tool(ToolConfig config = {});
[[nodiscard]] std::unique_ptr<Tool> make_git_diff_tool(ToolConfig config = {});

} // namespace ymh

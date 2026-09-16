#pragma once

// The model-facing `terminal` tool (14 §8). One tool with an `action`
// discriminator: open/write/read/resize/close/list. It drives the pinned
// `PtyService`/`PtySession` seam; it never opens a master fd, forks, or reaps
// (P2). Registered only when `pty().available()` (14 §4.4, E-P10).

#include <memory>

#include "ymh/execution/config.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

[[nodiscard]] std::unique_ptr<Tool> make_terminal_tool(ToolConfig config = {});

} // namespace ymh

#pragma once

// Plan-mode tools (25-D4): `exit_plan_mode` presents the completed plan for
// review. Registered unconditionally so the request tool catalog is stable.

#include <memory>

#include "ymh/tools/tool.hpp"

namespace ymh {

[[nodiscard]] std::unique_ptr<Tool> make_exit_plan_mode_tool();

} // namespace ymh

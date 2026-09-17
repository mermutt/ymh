#pragma once

// The single model-facing skill bridge (20-skills.md §5.4, §7.3). Registered in
// WorkspaceRuntime before ToolRegistry::freeze(). Pure execute: returns the
// cached body; appends nothing, touches no UI, makes no policy decision. Subject
// to the unchanged permission gate (SK9, SK13).

#include <memory>

#include "ymh/skills/skill_catalog.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

[[nodiscard]] std::unique_ptr<Tool> make_skill_tool(
    std::shared_ptr<const SkillCatalog> catalog);

} // namespace ymh

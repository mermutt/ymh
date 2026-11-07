#pragma once

// 119-D12/D27/D41: the narrow critical-path check. It is the only surviving
// verdict-shaped check: a recognized destructive shell call whose target is
// outside the workspace (the old L1 `Forbid` set) forces the approval flow.
// It never denies on its own; unresolved scans fail safe (treated as critical).
// The kernel sandbox remains the boundary.

#include <string>
#include <vector>

#include "ymh/policy/permission_policy.hpp"

namespace ymh {

struct CriticalPathResult {
    bool                     critical = false;
    std::string              why;
    std::vector<std::string> operands;
};

[[nodiscard]] CriticalPathResult critical_path(const PermissionRequest& request);

} // namespace ymh

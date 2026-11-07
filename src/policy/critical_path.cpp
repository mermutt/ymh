#include "ymh/policy/critical_path.hpp"

#include "ymh/policy/shell_guard.hpp"

namespace ymh {

CriticalPathResult critical_path(const PermissionRequest& request) {
    CriticalPathResult out;
    if (request.tool != "shell" && request.tool != "terminal") {
        return out;
    }
    try {
        const ShellClassification classification = classify_shell_request(request);
        if (classification.risk == ShellCommandRisk::Forbid) {
            out.critical = true;
            out.why      = "shell-guard:outside-root";
        } else if (classification.path_protected) {
            out.critical = true;
            out.why      = "permission:protected-path";
        }
    } catch (...) {
        // 119-D41: an unresolved scan is critical (fail safe).
        out.critical = true;
        out.why      = "scan-unresolved";
    }
    return out;
}

} // namespace ymh

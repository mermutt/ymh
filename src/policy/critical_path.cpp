#include "ymh/policy/critical_path.hpp"

#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace ymh {

CriticalPathResult critical_path(const PermissionRequest& request) {
    CriticalPathResult out;
    if (request.tool != "shell" && request.tool != "terminal") {
        return out;
    }
    try {
        // 119-D12: critical means a destructive verb with an outside-root
        // target, or a scan the classifier could not resolve. `Forbid` alone is
        // too broad: it also covers benign outside redirects such as
        // `>/tmp/build.log`, which the workspace-write sandbox grants. Compare
        // the strict verdict with the tmp-root-aware one and keep the strict
        // verdict for destructive verbs and unresolved scans.
        const ShellClassification strict = classify_shell_request(request);
        const ShellClassification relaxed =
            classify_shell_request(request, canonical_tmp_root());
        const bool destructive =
            shell_command_is_destructive(shell_guard_text(request));
        const bool outside_or_unresolved =
            strict.risk == ShellCommandRisk::Forbid &&
            (relaxed.risk == ShellCommandRisk::Forbid || destructive);
        if (outside_or_unresolved) {
            out.critical = true;
            out.why      = "shell-guard:outside-root";
        } else if (strict.path_protected) {
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

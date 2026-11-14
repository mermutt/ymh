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
        // 119-D12: critical means an outside-root target that even the
        // tmp-root-aware classifier rejects, or a scan it could not resolve.
        // `/tmp` is scratch: a command bounded to the canonical tmp root is not
        // critical even when it uses a destructive verb (`cp`, `rm`, …), which
        // matches the workspace-write sandbox's writable tmp. The tmp root
        // itself and `..` escapes are not "under" it, so `rm -rf /tmp` and
        // `rm -rf /tmp/../home/...` stay critical.
        const ShellClassification strict = classify_shell_request(request);
        const ShellClassification relaxed =
            classify_shell_request(request, canonical_tmp_root());
        const bool outside_or_unresolved =
            strict.risk == ShellCommandRisk::Forbid &&
            relaxed.risk == ShellCommandRisk::Forbid;
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

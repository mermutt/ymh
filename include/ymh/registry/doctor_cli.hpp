#pragma once

// 88-A6 (spec 88 §5, §7): the `ymh doctor` and `ymh workspace reap` command
// drivers. `run_doctor` is read-only (88-D1/DOC1); `run_workspace_reap` is the
// only mutation and delegates to the shipped, lock-gated `reapHost`. Both map
// their outcome to the 88-D9 exit codes.

#include <iosfwd>
#include <string>
#include <vector>

namespace ymh {

struct DoctorOptions {
    bool        json = false;
    std::string workspace;  // empty => all rows
};

// Resolves the target if set, opens the registry read-only, calls
// `probeWorkspaceLock(root, false)` + `socketOwnerPid(sock)` +
// `readProcessTableChecked()`, injects the production canonicalizer, folds them
// through `diagnose()`, renders, and returns the 88-D9 exit code (3 when
// `!processTableComplete`). Caller: the `Command::Doctor` dispatch arm in
// cli.cpp.
int run_doctor(const DoctorOptions& options, std::ostream& out, std::ostream& err);

// Executes `ymh workspace reap <id|path> [--yes]`. Resolves one target, prints
// its doctor findings, re-probes the sidecar lock (1-arg probe), refuses when the
// lock is held or the user declines, and otherwise delegates to
// `WorkspaceRegistry::reapHost`. Returns the 88-D9 exit code. Caller: the
// `Command::Workspace` dispatch arm `if (workspace_args[0] == "reap")` in
// cli.cpp, exactly where `run_workspace_stop` is called; `run_workspace_command`
// is unchanged.
int run_workspace_reap(const std::vector<std::string>& args, bool assume_yes,
                       std::ostream& out, std::ostream& err);

} // namespace ymh

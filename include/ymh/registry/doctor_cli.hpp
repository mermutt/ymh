#pragma once

// 88-A6 (spec 88 §5, §7): the `ymh doctor` command driver. `run_doctor` is
// read-only (88-D1/DOC1) and maps its outcome to the 88-D9 exit codes. The
// former `reap` mutation now lives inside `workspace stop`.

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

} // namespace ymh

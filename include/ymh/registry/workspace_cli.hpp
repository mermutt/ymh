#pragma once

// `ymh workspace` command surface (03 §7.2): `ymh workspace add <path>` and
// `ymh workspace list`. Kept in the registry component so the CLI layer only
// forwards argv; all registry knowledge stays here.

#include <iosfwd>
#include <string>
#include <vector>

namespace ymh {

// Executes a `ymh workspace` invocation. `args` excludes the leading
// `ymh workspace` words: `{"add", "<path>"}`, `{"list"}`, or `{}` for usage.
// Returns a process exit code; diagnostics go to `err`.
int run_workspace_command(const std::vector<std::string>& args,
                          std::ostream& out,
                          std::ostream& err);

} // namespace ymh

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

// 112-D1: `ymh workspace remove <target> [--yes]`. Deletes a *non-running*
// workspace's registry row after removing its session junction rows (mirrors
// the switcher's delete cascade, 51-I23); `<workspace>/.ymh/` — including
// `sessions.db` — is never deleted. A live daemon claim is refused.
// `args` is `{"remove", "<workspace-id|path>"}`; exit codes follow `reap`
// (2 usage/unknown target, 3 registry unavailable, 1 refused, 0 removed).
int run_workspace_remove(const std::vector<std::string>& args,
                         bool assume_yes,
                         std::ostream& out,
                         std::ostream& err);

} // namespace ymh

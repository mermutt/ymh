#pragma once

// Session inspection commands (§41): `ymh list`, `ymh show`, `ymh replay`, and
// `ymh fork`, all scoped to one workspace (the current directory in the MVP).
//
// The read commands use the store's read-only mode so they coexist with a
// running writer; `fork` needs the write lease.

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace ymh {

// 23 §5.1/§5.7: `ymh session prune` options. `--empty` is the only inclusion
// flag in v1; `--keep` is a modifier-only exclusion; `--older-than` is deferred
// and not represented.
struct PruneOptions {
    bool                                empty = false;
    std::optional<std::size_t>          keep;
    std::optional<std::filesystem::path> workspace;
    bool                                all   = false;
    bool                                yes   = false;
    bool                                force = false;
    bool                                json  = false;
};

// 23 §5.1-§5.6: `ymh session prune`. Dry run unless `yes`. Returns a process
// exit code (0 ok, 1 partial failure, 2 usage error). No daemon required.
int session_prune(const PruneOptions& options, std::ostream& out, std::ostream& err);

int session_list(const std::filesystem::path& workspace, std::ostream& out, std::ostream& err);

int session_show(const std::filesystem::path& workspace,
                 const std::string&         session,
                 std::ostream&              out,
                 std::ostream&              err);

int session_replay(const std::filesystem::path& workspace,
                   const std::string&         session,
                   std::ostream&              out,
                   std::ostream&              err);

int session_fork(const std::filesystem::path& workspace,
                 const std::string&         session,
                 std::ostream&              out,
                 std::ostream&              err);

} // namespace ymh

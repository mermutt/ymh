#pragma once

// Session inspection commands (§41): `ymh list`, `ymh show`, `ymh replay`, and
// `ymh fork`, all scoped to one workspace (the current directory in the MVP).
//
// The read commands use the store's read-only mode so they coexist with a
// running writer; `fork` needs the write lease.

#include <filesystem>
#include <iosfwd>
#include <string>

namespace ymh {

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

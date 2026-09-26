#pragma once

// Built-in v1 tools (07 §4.3): read_file, edit_file, write_file, grep, glob,
// shell. Each is a plain `Tool`; factories return owned instances so the daemon
// can register them without exposing concrete types.

#include <memory>
#include <vector>

#include "ymh/execution/config.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

[[nodiscard]] std::unique_ptr<Tool> make_read_file_tool(ToolConfig config = {});
[[nodiscard]] std::unique_ptr<Tool> make_write_file_tool(ToolConfig config = {});
[[nodiscard]] std::unique_ptr<Tool> make_edit_file_tool(ToolConfig config = {});
[[nodiscard]] std::unique_ptr<Tool> make_grep_tool(ToolConfig config = {});
[[nodiscard]] std::unique_ptr<Tool> make_glob_tool(ToolConfig config = {});
[[nodiscard]] std::unique_ptr<Tool> make_shell_tool(ToolConfig config = {});

// The §4.3 MVP set, in stable name order.
[[nodiscard]] std::vector<std::unique_ptr<Tool>> make_builtin_tools(ToolConfig config = {});

} // namespace ymh

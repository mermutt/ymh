#pragma once

// Runtime-context producer (36 §2.5). Runtime contexts are `PromptAssembly.
// contexts` entries, not sections: they append a sourced user-role snapshot
// under the verbatim supersession header and never enter `render()` (36-I6).
// The provider reads only session-invariant state (the daemon workspace root,
// the effective model) plus the current date, which the caller may inject.

#include <functional>
#include <string>
#include <string_view>

#include "ymh/prompt/system_prompt.hpp"

namespace ymh {

inline constexpr std::string_view kRuntimeContextName = "runtime-context";

inline constexpr std::string_view kRuntimeContextHeader =
    "Current runtime context. This snapshot supersedes earlier runtime-context snapshots.";

struct RuntimeContextConfig {
    std::string cwd;
    std::string model;
};

using DateProvider = std::function<std::string()>;

[[nodiscard]] std::string render_runtime_context(const RuntimeContextConfig& config,
                                                 const std::string&          date);

[[nodiscard]] ContextHandle register_runtime_context(SystemPrompt&       prompt,
                                                     RuntimeContextConfig config,
                                                     DateProvider         date = {});

} // namespace ymh

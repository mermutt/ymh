#pragma once

// Glob matching shared by the policy rule engine (09 §3.4) and the `glob`/`grep`
// tools (07 §6.3). `glob_match` treats the whole string as one segment (`*` may
// cross any character); `path_glob_match` splits on `/`, where `*` does not
// cross a separator and `**` matches zero or more whole segments.

#include <string_view>

namespace ymh {

[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text);
[[nodiscard]] bool path_glob_match(std::string_view pattern, std::string_view text);

} // namespace ymh

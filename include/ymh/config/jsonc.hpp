#pragma once

// JSONC parsing helpers shared by the layered config loader and the durable
// grants store (46-D2.11). Moved out of the anonymous namespace in
// `src/config/config.cpp` so the `GrantStore` can reuse the exact parser.

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ymh {

// Returns the parsed document; `nullopt` with an empty `error` means
// blank/comments-only (the shipped no-op semantics). A parse failure sets
// `error` and returns `nullopt`.
[[nodiscard]] std::optional<nlohmann::json>
parse_jsonc_document(std::string_view text, std::string& error);

[[nodiscard]] bool blank_or_comments_only(std::string_view text);

} // namespace ymh

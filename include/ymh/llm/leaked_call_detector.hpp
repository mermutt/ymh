#pragma once

// 47-D4/47-D5: generic leaked-call / native-ATEM detection. The parse
// functions are pure (structure only); the offered-name test, normalization,
// and error emission live in `OpenAiStreamDecoder::finalize` (47-O-M3). No
// profile identity appears here (47-I6).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {

struct LeakedCall {
    std::string    name;
    nlohmann::json arguments;
};

// 47-O2-M2: `complete_block_seen` is true only when one or more complete
// `<tool_call>…</tool_call>` blocks constitute the whole trimmed content;
// `malformed_block_seen` records a complete whole-content block that could not
// yield a `LeakedCall` (outer JSON not an object, or no string `name`).
struct LeakedParse {
    std::vector<LeakedCall> calls;
    bool                    complete_block_seen = false;
    bool                    malformed_block_seen = false;
};

[[nodiscard]] LeakedParse parse_leaked_json_call(std::string_view content);

// 47-D5: complete `<atem:invoke>…</atem:invoke>` blocks in a `to=<tool>` body
// (or bare in the trimmed content); `to=self`/`to=user` bodies are ignored.
// Names in document order. `nullopt` = none. Never converts.
[[nodiscard]] std::optional<std::vector<std::string>> detect_native_atem_calls(
    std::string_view content);

} // namespace ymh

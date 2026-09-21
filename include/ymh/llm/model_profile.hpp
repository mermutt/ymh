#pragma once

// Model-profile seam (47-D1). A profile is a generic declaration; the built-in
// profile values are defined in `src/llm/model_profile.cpp` only. With no
// profile configured (`id` empty) every profile-gated branch is a no-op and the
// wire body is byte-identical to the profile-free baseline (47-I1).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {

// 47-D9: per-profile capability declaration. `nullopt` keeps the base
// (openai_compatible_capabilities()); `true`/`false` overrides it.
struct ProfileCapabilities {
    std::optional<bool> streaming;
    std::optional<bool> tool_calls;
    std::optional<bool> parallel_tool_calls;
    std::optional<bool> reasoning;
    std::optional<bool> usage_streaming;
    std::optional<bool> prompt_caching;
};

// 47-D1: the profile seam. The type is generic; the built-in profile values
// live in `src/llm/model_profile.cpp` only.
struct ModelProfile {
    std::string                  id;                              // "" = inert
    bool                         force_first_tool_call = false;   // 47-D2
    bool                         normalize_tool_arguments = false; // 47-D6
    bool                         detect_leaked_tool_calls = false; // 47-D4 (G-JSON)
    bool                         detect_atem_tool_calls = false;   // 47-D5 (G-ATEM detection)
    std::vector<std::string>     forbidden_stop_tokens;           // 47-D3
    ProfileCapabilities          capabilities;                    // 47-D9
    std::optional<double>        temperature;                     // 47-D1.4
    std::optional<double>        top_p;                           // 47-D1.4
    std::optional<std::uint32_t> top_k;                           // 47-D1.4
};

// `nullptr` for "" and unknown ids; total and `noexcept` (47-I2).
[[nodiscard]] const ModelProfile* find_model_profile(std::string_view id) noexcept;
[[nodiscard]] bool is_known_model_profile(std::string_view id) noexcept;

// The profile id a caller pre-sets when it has no explicit user choice. The
// value lives in `src/llm/model_profile.cpp` only (47-I6), so callers outside
// the profile layer never name a built-in profile. Empty means "pre-set none".
[[nodiscard]] std::string_view default_import_profile_id() noexcept;

} // namespace ymh

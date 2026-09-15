#pragma once

// Shared FakeLLM scripting seam for `ymh run` (headless) and `ymh --host`
// (daemon). `YMH_FAKE_LLM_SCRIPT` names a JSON file that scripts a `FakeLLM`;
// this helper is the single parser/factory used by both entry points so the
// deterministic suite configures the provider identically in-process and
// across the two-process test harness.
//
// The JSON format (08 §7 / §45, mirrored by `FakeScript`):
//   * an array of response steps, or
//   * `{ "chunk_size": N, "steps": [ ... ] }`.
// Each step: `{ "text", "reasoning"?, "tool_calls"?, "usage"?, "finish",
// "error"? }`. A malformed document is a `ConfigError`, never a silent
// fallback: the test seam must fail loudly.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace ymh {

// The environment variable both entry points consult.
inline constexpr std::string_view kFakeLlmScriptEnv = "YMH_FAKE_LLM_SCRIPT";

// Parses an already-decoded document. Returns `nullopt` when the document is
// not a script shape (neither an array nor an object with a `steps` array).
[[nodiscard]] std::optional<FakeScript> parse_fake_llm_script(
    const nlohmann::json& document);

// Loads and parses a script file. Throws `ConfigError` when the file cannot be
// opened or does not parse into a script.
[[nodiscard]] FakeScript load_fake_llm_script(const std::filesystem::path& path);

// Reads `variable` (default `YMH_FAKE_LLM_SCRIPT`). Returns `nullopt` when the
// variable is unset/empty; throws `ConfigError` when set but invalid.
[[nodiscard]] std::optional<FakeScript> fake_llm_script_from_env(
    std::string_view variable = kFakeLlmScriptEnv);

// The provider factory shared by `ymh run` and `ymh --host`:
//   * `explicit_factory` wins when set (in-process test injection);
//   * else `YMH_FAKE_LLM_SCRIPT`, when set, yields a `FakeLLM`;
//   * else returns `nullptr` and the caller falls back to the registry.
[[nodiscard]] std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)>
make_provider_factory(
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> explicit_factory = {});

} // namespace ymh

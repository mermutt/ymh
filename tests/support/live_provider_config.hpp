#pragma once

// Dev-only: supplies the pinned dev provider (spec 61) as a global config layer
// for live (`YMH_LIVE_LLM=1`) runs.
//
// Live mode disables the process-wide FakeLLM (set by global_test_env), so a
// spawned daemon with no real provider rejects startup with "no LLM provider is
// configured (provider setup failed)". Hermetic callers are unaffected: this
// helper is a no-op unless live mode is on.
//
// `main` never sees this header (tests live on `dev`).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "support/dev_llm_config.hpp"

namespace ymh::test {

[[nodiscard]] inline bool live_llm_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key  = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

// The dev-pinned provider as the JSONC global config layer (the shape the live
// suite's daemons need). Mirrors `llm.default` in ui_supervisor_pty_test.cpp.
[[nodiscard]] inline std::string live_provider_config_json() {
    const LLMProviderConfig c = deepseek_config();
    std::string            s;
    s += "{\n  \"llm\": {\n    \"default\": {\n";
    s += "      \"base_url\": \"" + c.base_url + "\",\n";
    s += "      \"model\": \"" + c.model + "\",\n";
    s += "      \"api_key_env\": \"" + c.api_key_env + "\"\n";
    s += "    }\n  }\n}\n";
    return s;
}

// Writes `<config_dir>/ymh/config.jsonc` with the dev provider. No-op unless
// live mode is enabled, so hermetic callers are untouched. Overwrites any
// placeholder written earlier (e.g. `{}` from global_test_env).
inline void write_live_provider_config(const std::filesystem::path& config_dir) {
    if (!live_llm_enabled()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(config_dir / "ymh", error);
    std::ofstream file(config_dir / "ymh" / "config.jsonc",
                       std::ios::binary | std::ios::trunc);
    file << live_provider_config_json();
}

} // namespace ymh::test

#pragma once

// Dev-only vendor pin for the `main`/`dev` branch split (spec 61).
//
// The `main` (code) branch ships no built-in endpoint, model or credential-env
// defaults and does not export `deepseek_config()`. The hermetic and live test
// suites on `dev` still want one complete, real configuration to exercise the
// OpenAI-compatible adapter, so the pinned triple lives here instead of in the
// shipped library. `main` never sees this header.

#include <string>

#include "ymh/llm/provider_registry.hpp"

namespace ymh {

// The pinned dev endpoint/model/key-env triple (the values `main` no longer
// hardcodes).
[[nodiscard]] inline LLMProviderConfig deepseek_config() {
    LLMProviderConfig config;
    config.provider = "openai-compatible";
    config.base_url = "https://api.deepseek.com/v1";
    config.model = "deepseek-flash";
    config.api_key_env = "DEEPSEEK_API_KEY";
    return config;
}

} // namespace ymh

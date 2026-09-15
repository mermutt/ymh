#pragma once

// Provider registry, factory, and configuration (08-llm-provider.md §5.1).
// Spec 08 constructs the registry and registers the built-in adapters; the
// factory validates configuration by name only and never reads the secret and
// never performs network I/O (L-F12).

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ymh/llm/llm_provider.hpp"

namespace ymh {

// Provider configuration (08 §5.1). `api_key_env` names the environment
// variable; the secret value is read per request and never cached (decision
// (m)). `model` is the default and may be empty — the effective model is
// resolved by the caller and required per request (L11).
struct LLMProviderConfig {
    ProviderId   provider;                 // registry key, e.g. "openai-compatible"
    std::string  base_url;                 // e.g. "https://api.deepseek.com/v1"
    ModelId      model;                    // default model (may be empty)
    std::string  api_key_env;              // NAME of the env var, never the secret
    std::vector<std::pair<std::string, std::string>> headers;  // extra, non-secret
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::chrono::milliseconds request_timeout{120'000};
    RetryPolicy  retry;
    std::size_t  max_arguments_bytes = 1u << 20;  // tool-arg assembly cap (08 §4.2)
    std::size_t  sse_line_bytes = 1u << 20;       // max single SSE data line (L-F16)
};

using ProviderFactory =
    std::function<std::expected<std::unique_ptr<LLMProvider>, LLMError>(
        const LLMProviderConfig&)>;

class ProviderRegistry {
public:
    // Startup-only registration.
    void registerProvider(ProviderId id, ProviderFactory factory);

    // Returns `ConfigError` for an unknown provider or invalid config; never
    // throws and never performs network I/O (L-F12).
    std::expected<std::unique_ptr<LLMProvider>, LLMError>
    create(const LLMProviderConfig& config) const;

    std::vector<ProviderId> names() const;

private:
    std::vector<std::pair<ProviderId, ProviderFactory>> factories_;
};

// Registers the v1 built-ins (only `openai-compatible`, 08 §14.1(a)).
void register_builtin_providers(ProviderRegistry& registry);

// A registry with the built-in providers registered.
[[nodiscard]] ProviderRegistry make_default_provider_registry();

// Default DeepSeek/OpenAI-compatible configuration (base URL, model, key env).
[[nodiscard]] LLMProviderConfig deepseek_config();

// Static capability defaults for the OpenAI-compatible adapter. These describe
// the v1 DeepSeek target (tools + reasoning + streamed usage); a different
// endpoint can be constructed with explicit capabilities.
[[nodiscard]] ProviderCapabilities openai_compatible_capabilities();

} // namespace ymh

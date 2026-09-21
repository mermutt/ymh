#include "ymh/llm/provider_registry.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string_view>
#include <utility>

#include "ymh/llm/openai_adapter.hpp"

namespace ymh {
namespace {

LLMError config_error(std::string detail) {
    LLMError error;
    error.code = LLMErrorCode::ConfigError;
    error.detail = std::move(detail);
    error.retryable = false;
    return error;
}

bool is_http_url(std::string_view url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool is_valid_env_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto is_alpha = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    };
    const auto is_alnum = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    };
    if (!is_alpha(name.front()) && name.front() != '_') {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [&](char c) {
        return is_alnum(c) || c == '_';
    });
}

} // namespace

void ProviderRegistry::registerProvider(ProviderId id, ProviderFactory factory) {
    const auto existing = std::find_if(factories_.begin(), factories_.end(),
                                       [&id](const auto& entry) { return entry.first == id; });
    if (existing != factories_.end()) {
        existing->second = std::move(factory);
        return;
    }
    factories_.emplace_back(std::move(id), std::move(factory));
}

std::expected<std::unique_ptr<LLMProvider>, LLMError> ProviderRegistry::create(
    const LLMProviderConfig& config) const {
    const auto factory = std::find_if(factories_.begin(), factories_.end(),
                                      [&config](const auto& entry) {
                                          return entry.first == config.provider;
                                      });
    if (factory == factories_.end()) {
        return std::unexpected(config_error("unknown provider: " + config.provider));
    }
    if (!is_http_url(config.base_url)) {
        return std::unexpected(config_error("invalid base_url: " + config.base_url));
    }
    const bool has_literal_key = config.api_key.has_value() && !config.api_key->empty();
    if (!has_literal_key && !is_valid_env_name(config.api_key_env)) {
        return std::unexpected(config_error("invalid api_key_env: " + config.api_key_env));
    }
    if (config.max_arguments_bytes == 0) {
        return std::unexpected(config_error("max_arguments_bytes must be non-zero"));
    }
    if (config.sse_line_bytes == 0) {
        return std::unexpected(config_error("sse_line_bytes must be non-zero"));
    }
    return factory->second(config);
}

std::vector<ProviderId> ProviderRegistry::names() const {
    std::vector<ProviderId> result;
    result.reserve(factories_.size());
    for (const auto& entry : factories_) {
        result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

ProviderCapabilities openai_compatible_capabilities() {
    ProviderCapabilities capabilities;
    capabilities.streaming = true;
    capabilities.tool_calls = true;
    capabilities.parallel_tool_calls = true;
    capabilities.reasoning = true;
    capabilities.usage_streaming = true;
    capabilities.prompt_caching = true;
    return capabilities;
}

void apply_profile_capabilities(ProviderCapabilities& capabilities,
                                const ModelProfile& profile) noexcept {
    const ProfileCapabilities& declared = profile.capabilities;
    if (declared.streaming.has_value()) {
        capabilities.streaming = *declared.streaming;
    }
    if (declared.tool_calls.has_value()) {
        capabilities.tool_calls = *declared.tool_calls;
    }
    if (declared.parallel_tool_calls.has_value()) {
        capabilities.parallel_tool_calls = *declared.parallel_tool_calls;
    }
    if (declared.reasoning.has_value()) {
        capabilities.reasoning = *declared.reasoning;
    }
    if (declared.usage_streaming.has_value()) {
        capabilities.usage_streaming = *declared.usage_streaming;
    }
    if (declared.prompt_caching.has_value()) {
        capabilities.prompt_caching = *declared.prompt_caching;
    }
}

void register_builtin_providers(ProviderRegistry& registry) {
    registry.registerProvider(
        "openai-compatible",
        [](const LLMProviderConfig& config)
            -> std::expected<std::unique_ptr<LLMProvider>, LLMError> {
            ProviderCapabilities capabilities = openai_compatible_capabilities();
            apply_profile_capabilities(capabilities, config.profile);
            auto provider = std::make_unique<OpenAICompatibleProvider>(
                config, capabilities, std::make_shared<CurlHttpTransport>());
            return std::unique_ptr<LLMProvider>{std::move(provider)};
        });
}

ProviderRegistry make_default_provider_registry() {
    ProviderRegistry registry;
    register_builtin_providers(registry);
    return registry;
}

LLMProviderConfig deepseek_config() {
    LLMProviderConfig config;
    config.provider = "openai-compatible";
    config.base_url = "https://api.deepseek.com/v1";
    config.model = "deepseek-flash";
    config.api_key_env = "DEEPSEEK_API_KEY";
    return config;
}

} // namespace ymh

#include "ymh/cli/provider_factory.hpp"

#include <fstream>
#include <string>
#include <utility>

#include "ymh/config/config.hpp"

namespace ymh {
namespace {

FinishReason parse_finish(std::string_view name) {
    if (name == "stop") {
        return FinishReason::Stop;
    }
    if (name == "length") {
        return FinishReason::Length;
    }
    if (name == "tool_calls" || name == "tool_call") {
        return FinishReason::ToolCalls;
    }
    if (name == "content_filter") {
        return FinishReason::ContentFilter;
    }
    return FinishReason::Other;
}

std::optional<LLMError> parse_error(const nlohmann::json& node) {
    if (!node.is_object()) {
        return std::nullopt;
    }
    LLMError error;
    const std::string code = node.value("code", std::string{"provider_internal"});
    if (code == "auth") {
        error.code = LLMErrorCode::Auth;
    } else if (code == "config_error") {
        error.code = LLMErrorCode::ConfigError;
    } else if (code == "bad_request") {
        error.code = LLMErrorCode::BadRequest;
    } else if (code == "rate_limited") {
        error.code = LLMErrorCode::RateLimited;
    } else if (code == "server_error") {
        error.code = LLMErrorCode::ServerError;
    } else if (code == "network_error") {
        error.code = LLMErrorCode::NetworkError;
    } else if (code == "timeout") {
        error.code = LLMErrorCode::Timeout;
    } else if (code == "context_length_exceeded") {
        error.code = LLMErrorCode::ContextLengthExceeded;
    } else {
        error.code = LLMErrorCode::ProviderInternal;
    }
    error.detail = node.value("detail", std::string{});
    return error;
}

} // namespace

std::optional<FakeScript> parse_fake_llm_script(const nlohmann::json& document) {
    const nlohmann::json* steps = nullptr;
    FakeScript             script;
    if (document.is_array()) {
        steps = &document;
    } else if (document.is_object() && document.contains("steps")) {
        steps = &document["steps"];
        script.chunk_size = document.value("chunk_size", script.chunk_size);
    } else {
        return std::nullopt;
    }
    if (!steps->is_array()) {
        return std::nullopt;
    }

    for (const nlohmann::json& step : *steps) {
        if (!step.is_object()) {
            return std::nullopt;
        }
        FakeResponseStep response;
        response.text     = step.value("text", std::string{});
        response.reasoning = step.contains("reasoning")
                                 ? std::optional<std::string>{step["reasoning"].get<std::string>()}
                                 : std::nullopt;
        response.finish = parse_finish(step.value("finish", std::string{"stop"}));
        if (step.contains("usage") && step["usage"].is_object()) {
            Usage usage;
            usage.input_tokens  = step["usage"].value("input_tokens", std::int64_t{0});
            usage.output_tokens = step["usage"].value("output_tokens", std::int64_t{0});
            response.usage      = usage;
        }
        if (step.contains("error")) {
            response.error = parse_error(step["error"]);
        }
        if (step.contains("tool_calls") && step["tool_calls"].is_array()) {
            for (const nlohmann::json& call : step["tool_calls"]) {
                FakeToolCallStep tool;
                tool.name      = call.value("name", std::string{});
                tool.arguments = call.value("arguments", nlohmann::json::object());
                if (call.contains("id")) {
                    tool.id = call["id"].get<std::string>();
                }
                response.tool_calls.push_back(std::move(tool));
            }
            if (response.finish == FinishReason::Stop) {
                response.finish = FinishReason::ToolCalls;
            }
        }
        script.steps.push_back(std::move(response));
    }
    return script;
}

FakeScript load_fake_llm_script(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw ConfigError("YMH_FAKE_LLM_SCRIPT cannot be opened: " + path.string());
    }
    std::optional<FakeScript> script;
    try {
        script = parse_fake_llm_script(nlohmann::json::parse(input));
    } catch (const nlohmann::json::exception& error) {
        throw ConfigError("YMH_FAKE_LLM_SCRIPT is not valid JSON: " + path.string() + ": " +
                          error.what());
    }
    if (!script.has_value()) {
        throw ConfigError("YMH_FAKE_LLM_SCRIPT is not a valid FakeLLM script: " + path.string());
    }
    return std::move(*script);
}

std::optional<FakeScript> fake_llm_script_from_env(std::string_view variable) {
    const std::optional<std::string> path = env_value(variable);
    if (!path.has_value()) {
        return std::nullopt;
    }
    return load_fake_llm_script(*path);
}

std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> make_provider_factory(
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> explicit_factory) {
    return [explicit_factory = std::move(explicit_factory)](
               const LLMProviderConfig& provider_config) -> std::unique_ptr<LLMProvider> {
        if (explicit_factory) {
            return explicit_factory(provider_config);
        }
        if (std::optional<FakeScript> script = fake_llm_script_from_env(); script.has_value()) {
            return std::make_unique<FakeLLM>(std::move(*script));
        }
        return nullptr;
    };
}

} // namespace ymh

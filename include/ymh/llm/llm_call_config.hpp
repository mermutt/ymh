#pragma once

// Provider-neutral call configuration, pinned by
// 28-llm-service-boundary-errata.md §3.1/§3.2 (26-dsh-alignment-part2 §4.3.1).
//
// This header is deliberately dependency-light: `payload::LlmRequestHeader`
// (include/ymh/session/events.hpp) embeds `LlmCallConfig` by value, so the type
// must be available without pulling the whole runtime/request surface in.
// `LlmRuntime`, `PreparedCall`, and `FrozenRequest` live in
// `ymh/llm/llm_runtime.hpp`.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {

// Registry key and opaque model id (08 §2.1). Declared here as well as in
// `llm_request.hpp`; both are identical alias declarations (C++ permits the
// redeclaration) so either header may be included first.
using ProviderId = std::string;
using ModelId    = std::string;

// Provider-neutral call configuration. `max_tokens` is the single canonical
// sampling-budget name; `GenerationParameters::max_output_tokens` maps onto it
// 1:1 in exactly one place (`buildRequest`), so there is one source of truth at
// dispatch (26 §4.3.1 :177-180; T-M5).
struct LlmCallConfig {
    ProviderId                   provider;  // source + default-route fallback: 28 §3.5
    // 54-D2: the endpoint NAME (the `llm.endpoints` key; "" = the anonymous
    // default endpoint) and the selected model's profile id (`ModelProfile::id`;
    // "" = inert). Route resolution is endpoint-first, with `provider` retained
    // as the default/legacy fallback ONLY when `endpoint` is empty (54-D3).
    std::string                  endpoint;
    std::string                  profile_id;
    ModelId                      model;
    std::optional<std::string>   reasoning_effort;
    std::optional<double>        temperature;
    std::optional<std::uint32_t> max_tokens;
    std::vector<std::string>     stop;
    // ymh extensions beyond dsh's 6-field config; the adapter emits them on the
    // wire (src/llm/openai_adapter.cpp:736-752), so they are logged and covered
    // by canonical_json() (26 §4.3.1 :188-194).
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;
    std::optional<std::uint32_t> seed;
    std::optional<std::string>   tool_choice;
};

// dsh GenerateOptions.purpose (26-dsh-alignment.md §2.1; part2 §4.3.1
// :197-203). Absent = an ordinary conversation request.
enum class CallPurpose : std::uint8_t {
    Compaction,
    SessionTitle,
};

// Wire/diagnostic names for `CallPurpose` (29-event-family-errata.md §3.2):
// lowercase snake_case strings, the two pinned values.
[[nodiscard]] inline std::string_view call_purpose_name(CallPurpose purpose) noexcept {
    switch (purpose) {
        case CallPurpose::Compaction:   return "compaction";
        case CallPurpose::SessionTitle: return "session_title";
    }
    return {};
}

[[nodiscard]] inline std::optional<CallPurpose> parse_call_purpose(std::string_view name) noexcept {
    if (name == "compaction") {
        return CallPurpose::Compaction;
    }
    if (name == "session_title") {
        return CallPurpose::SessionTitle;
    }
    return std::nullopt;
}

// The ONLY change test. No defaulted `operator==`/`operator<=>` is declared:
// `call_config_equals` is the documented semantic (dsh `callConfigEquals`), so
// there is no second, subtly different equality (26 §4.3.1 :205-208). Defined
// in `src/llm/llm_runtime.cpp`.
[[nodiscard]] bool call_config_equals(const LlmCallConfig&, const LlmCallConfig&) noexcept;

// JSON codec for the nested config object carried by `payload::LlmRequestHeader`
// (29 §3.2 keys). `optional` fields are omitted when unset. Inline so the
// session codec can use it without a link dependency on `ymh::llm`.
inline void to_json(nlohmann::json& json, const LlmCallConfig& config) {
    json = nlohmann::json::object();
    json["provider"] = config.provider;
    if (!config.endpoint.empty()) {
        json["endpoint"] = config.endpoint;
    }
    if (!config.profile_id.empty()) {
        json["profile_id"] = config.profile_id;
    }
    json["model"]    = config.model;
    if (config.reasoning_effort.has_value()) {
        json["reasoning_effort"] = *config.reasoning_effort;
    }
    if (config.temperature.has_value()) {
        json["temperature"] = *config.temperature;
    }
    if (config.max_tokens.has_value()) {
        json["max_tokens"] = *config.max_tokens;
    }
    json["stop"] = config.stop;
    if (config.top_p.has_value()) {
        json["top_p"] = *config.top_p;
    }
    if (config.top_k.has_value()) {
        json["top_k"] = *config.top_k;
    }
    if (config.seed.has_value()) {
        json["seed"] = *config.seed;
    }
    if (config.tool_choice.has_value()) {
        json["tool_choice"] = *config.tool_choice;
    }
}

inline void from_json(const nlohmann::json& json, LlmCallConfig& config) {
    config = LlmCallConfig{};
    config.provider   = json.value("provider", std::string{});
    config.endpoint   = json.value("endpoint", std::string{});
    config.profile_id = json.value("profile_id", std::string{});
    config.model      = json.value("model", std::string{});
    if (json.contains("reasoning_effort")) {
        config.reasoning_effort = json.at("reasoning_effort").get<std::string>();
    }
    if (json.contains("temperature")) {
        config.temperature = json.at("temperature").get<double>();
    }
    if (json.contains("max_tokens")) {
        config.max_tokens = json.at("max_tokens").get<std::uint32_t>();
    }
    if (json.contains("stop")) {
        config.stop = json.at("stop").get<std::vector<std::string>>();
    }
    if (json.contains("top_p")) {
        config.top_p = json.at("top_p").get<double>();
    }
    if (json.contains("top_k")) {
        config.top_k = json.at("top_k").get<std::uint32_t>();
    }
    if (json.contains("seed")) {
        config.seed = json.at("seed").get<std::uint32_t>();
    }
    if (json.contains("tool_choice")) {
        config.tool_choice = json.at("tool_choice").get<std::string>();
    }
}

} // namespace ymh

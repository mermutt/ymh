#pragma once

// Provider-agnostic streaming algebra, pinned by 08-llm-provider.md §2.1, §2.2,
// §3.2, and §3.3. Adapters map their native wire format onto these events; the
// agent loop (spec 06) is the only durable producer (L1).

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include <nlohmann/json.hpp>

#include "ymh/agent/message.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

// Why the model stopped producing tokens (08 §2.1). `Other` is the
// forward-compatible escape hatch for provider-specific reasons.
enum class FinishReason : std::uint8_t {
    Stop,
    Length,
    ToolCalls,
    ContentFilter,
    Error,
    Other,
};

// Terminal disposition of `stream()` (08 §2.1).
enum class StreamOutcome : std::uint8_t {
    Completed,
    Cancelled,
    Failed,
};

// Total error taxonomy (08 §2.2). No exception crosses the provider seam.
enum class LLMErrorCode : std::uint8_t {
    None,
    Auth,
    ConfigError,
    BadRequest,
    ContextLengthExceeded,
    RateLimited,
    ServerError,
    NetworkError,
    Timeout,
    MalformedResponse,
    MalformedToolCall,
    ContentFiltered,
    UnsupportedModel,
    ProviderInternal,
    Cancelled,
    InvalidPreparedCall,
    NoProviderRoute,
};

// Provider failure value carried on the terminal `StreamError` and on
// `LLMResponse` (08 §2.2). `provider_message`/`detail` are always redacted
// before they are surfaced or logged (L12).
struct LLMError {
    LLMErrorCode code = LLMErrorCode::None;
    int          http_status = 0;      // 0 when not HTTP
    std::string  provider_message;     // provider's own message, redacted
    std::string  detail;               // short, redacted diagnostic
    bool         retryable = false;    // advisory; see 08 §3.7 / L7

    bool operator==(const LLMError&) const = default;
};

[[nodiscard]] constexpr std::string_view to_string(LLMErrorCode code) noexcept {
    switch (code) {
        case LLMErrorCode::None:                  return "none";
        case LLMErrorCode::Auth:                  return "auth";
        case LLMErrorCode::ConfigError:           return "config_error";
        case LLMErrorCode::BadRequest:            return "bad_request";
        case LLMErrorCode::ContextLengthExceeded: return "context_length_exceeded";
        case LLMErrorCode::RateLimited:           return "rate_limited";
        case LLMErrorCode::ServerError:           return "server_error";
        case LLMErrorCode::NetworkError:          return "network_error";
        case LLMErrorCode::Timeout:               return "timeout";
        case LLMErrorCode::MalformedResponse:     return "malformed_response";
        case LLMErrorCode::MalformedToolCall:     return "malformed_tool_call";
        case LLMErrorCode::ContentFiltered:       return "content_filtered";
        case LLMErrorCode::UnsupportedModel:      return "unsupported_model";
        case LLMErrorCode::ProviderInternal:      return "provider_internal";
        case LLMErrorCode::Cancelled:             return "cancelled";
        case LLMErrorCode::InvalidPreparedCall:   return "invalid_prepared_call";
        case LLMErrorCode::NoProviderRoute:       return "no_provider_route";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(FinishReason reason) noexcept {
    switch (reason) {
        case FinishReason::Stop:          return "stop";
        case FinishReason::Length:        return "length";
        case FinishReason::ToolCalls:     return "tool_calls";
        case FinishReason::ContentFilter: return "content_filter";
        case FinishReason::Error:         return "error";
        case FinishReason::Other:         return "other";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(StreamOutcome outcome) noexcept {
    switch (outcome) {
        case StreamOutcome::Completed: return "completed";
        case StreamOutcome::Cancelled: return "cancelled";
        case StreamOutcome::Failed:    return "failed";
    }
    return "unknown";
}

// Retryable only before the first dispatched event (08 §3.7, L7).
[[nodiscard]] constexpr bool is_retryable_code(LLMErrorCode code) noexcept {
    switch (code) {
        case LLMErrorCode::RateLimited:
        case LLMErrorCode::ServerError:
        case LLMErrorCode::NetworkError:
        case LLMErrorCode::Timeout:
        case LLMErrorCode::MalformedResponse:
            return true;
        default:
            return false;
    }
}

// 33-D4: the inverses of the two `to_string` maps above. Each accepts exactly
// the strings `to_string` emits and returns `nullopt` otherwise; the codec
// throws on an unknown string and never silently defaults.
[[nodiscard]] constexpr std::optional<FinishReason> parse_finish_reason(
    std::string_view name) noexcept {
    if (name == "stop") {
        return FinishReason::Stop;
    }
    if (name == "length") {
        return FinishReason::Length;
    }
    if (name == "tool_calls") {
        return FinishReason::ToolCalls;
    }
    if (name == "content_filter") {
        return FinishReason::ContentFilter;
    }
    if (name == "error") {
        return FinishReason::Error;
    }
    if (name == "other") {
        return FinishReason::Other;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<LLMErrorCode> parse_llm_error_code(
    std::string_view name) noexcept {
    if (name == "none") {
        return LLMErrorCode::None;
    }
    if (name == "auth") {
        return LLMErrorCode::Auth;
    }
    if (name == "config_error") {
        return LLMErrorCode::ConfigError;
    }
    if (name == "bad_request") {
        return LLMErrorCode::BadRequest;
    }
    if (name == "context_length_exceeded") {
        return LLMErrorCode::ContextLengthExceeded;
    }
    if (name == "rate_limited") {
        return LLMErrorCode::RateLimited;
    }
    if (name == "server_error") {
        return LLMErrorCode::ServerError;
    }
    if (name == "network_error") {
        return LLMErrorCode::NetworkError;
    }
    if (name == "timeout") {
        return LLMErrorCode::Timeout;
    }
    if (name == "malformed_response") {
        return LLMErrorCode::MalformedResponse;
    }
    if (name == "malformed_tool_call") {
        return LLMErrorCode::MalformedToolCall;
    }
    if (name == "content_filtered") {
        return LLMErrorCode::ContentFiltered;
    }
    if (name == "unsupported_model") {
        return LLMErrorCode::UnsupportedModel;
    }
    if (name == "provider_internal") {
        return LLMErrorCode::ProviderInternal;
    }
    if (name == "cancelled") {
        return LLMErrorCode::Cancelled;
    }
    if (name == "invalid_prepared_call") {
        return LLMErrorCode::InvalidPreparedCall;
    }
    if (name == "no_provider_route") {
        return LLMErrorCode::NoProviderRoute;
    }
    return std::nullopt;
}

struct TextDelta {
    std::string text;

    bool operator==(const TextDelta&) const = default;
};

struct ReasoningDelta {
    std::string text;  // emitted only when capabilities().reasoning (L14)

    bool operator==(const ReasoningDelta&) const = default;
};

// A fully assembled tool call (08 §3.2). `arguments` is always a JSON object.
struct ToolCallAssembled {
    ToolCallId     id;
    std::string    name;
    nlohmann::json arguments = nlohmann::json::object();

    bool operator==(const ToolCallAssembled&) const = default;
};

struct ToolCallStarted {
    std::uint32_t index = 0;  // provider-assigned position within the response
    ToolCallId    id;         // provider-assigned; becomes the tool_use block id
    std::string   name;

    bool operator==(const ToolCallStarted&) const = default;
};

struct ToolCallDelta {
    std::uint32_t index = 0;
    std::string   arguments_fragment;  // raw JSON text fragment, not parsed

    bool operator==(const ToolCallDelta&) const = default;
};

struct ToolCallFinished {
    std::uint32_t     index = 0;
    ToolCallAssembled call;  // id, name, parsed JSON object

    bool operator==(const ToolCallFinished&) const = default;
};

struct UsageEvent {
    Usage usage;

    bool operator==(const UsageEvent&) const = default;
};

// 26 §4.3.4 :529-533 / 34-D1: adapter-private, versioned, JSON-serializable
// replay state carried on a `Finished` event and settled into
// `AssistantMessage.replay_state`.
struct ReplayEnvelope {
    std::string    provider;
    std::uint32_t  version = 1;
    nlohmann::json state;

    bool operator==(const ReplayEnvelope&) const = default;
};

struct Finished {
    FinishReason                   reason = FinishReason::Other;
    std::optional<Usage>           usage;          // convenience mirror; may be nullopt
    std::optional<ReplayEnvelope>  replay_state;   // NEW (26 :586-589)

    bool operator==(const Finished&) const = default;
};

struct StreamError {
    LLMError error;

    bool operator==(const StreamError&) const = default;
};

using StreamEvent = std::variant<TextDelta,
                                 ReasoningDelta,
                                 ToolCallStarted,
                                 ToolCallDelta,
                                 ToolCallFinished,
                                 UsageEvent,
                                 Finished,
                                 StreamError>;

// 34-D4: a `StreamEvent` stamped with the loop's sink receive offset. `at` is a
// non-negative steady_clock delta from the per-attempt stream start; providers
// emit bare `StreamEvent`s and never stamp (L1).
struct TimedStreamEvent {
    std::chrono::milliseconds at{0};
    StreamEvent               event;
};

// Consumer flow control (08 §3.3). `Stop` is consumer-initiated cancellation
// for this request only; it aborts the stream and returns `Cancelled`.
enum class SinkFlow : std::uint8_t {
    Continue,
    Stop,
};

// Invoked serially, in stream order, on the provider's execution context. MUST
// be cheap, non-blocking, non-throwing, non-reentrant, and MUST NOT touch the
// session store (L1, L5).
using StreamSink = std::function<SinkFlow(const StreamEvent&)>;

// ---------------------------------------------------------------------------
// JSON codec (33-D2/33-D3/33-D5). Inline ADL `to_json`/`from_json` in namespace
// `ymh`, so `events.cpp` can encode the assistant stream without a link
// dependency on `ymh::llm` and without a second definition. Exactly one codec.
// ---------------------------------------------------------------------------

inline void to_json(nlohmann::json& json, const LLMError& error) {
    json = nlohmann::json{
        {"code", std::string{to_string(error.code)}},
        {"http_status", error.http_status},
        {"provider_message", error.provider_message},
        {"detail", error.detail},
        {"retryable", error.retryable},
    };
}

inline void from_json(const nlohmann::json& json, LLMError& error) {
    const std::string code = json.at("code").get<std::string>();
    const auto parsed = parse_llm_error_code(code);
    if (!parsed.has_value()) {
        throw nlohmann::json::other_error::create(
            501, "unknown LLMErrorCode: " + code, &json);
    }
    error.code             = *parsed;
    error.http_status      = json.value("http_status", 0);
    error.provider_message = json.value("provider_message", std::string{});
    error.detail           = json.value("detail", std::string{});
    error.retryable        = json.value("retryable", false);
}

inline void to_json(nlohmann::json& json, const ToolCallAssembled& call) {
    json = nlohmann::json{
        {"id", call.id},
        {"name", call.name},
        {"arguments", call.arguments},
    };
}

inline void from_json(const nlohmann::json& json, ToolCallAssembled& call) {
    call.id        = json.at("id").get<ToolCallId>();
    call.name      = json.value("name", std::string{});
    call.arguments = json.value("arguments", nlohmann::json::object());
}

inline void to_json(nlohmann::json& json, const ReplayEnvelope& envelope) {
    json = nlohmann::json{
        {"provider", envelope.provider},
        {"version", envelope.version},
        {"state", envelope.state},
    };
}

inline void from_json(const nlohmann::json& json, ReplayEnvelope& envelope) {
    envelope.provider = json.value("provider", std::string{});
    envelope.version  = json.value("version", std::uint32_t{1});
    envelope.state    = json.value("state", nlohmann::json::object());
}

inline void to_json(nlohmann::json& json, const StreamEvent& event) {
    std::visit(
        [&json](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, TextDelta>) {
                json = nlohmann::json{{"type", "text_delta"}, {"text", value.text}};
            } else if constexpr (std::is_same_v<Value, ReasoningDelta>) {
                json = nlohmann::json{{"type", "reasoning_delta"}, {"text", value.text}};
            } else if constexpr (std::is_same_v<Value, ToolCallStarted>) {
                json = nlohmann::json{
                    {"type", "tool_call_started"},
                    {"index", value.index},
                    {"id", value.id},
                    {"name", value.name},
                };
            } else if constexpr (std::is_same_v<Value, ToolCallDelta>) {
                json = nlohmann::json{
                    {"type", "tool_call_delta"},
                    {"index", value.index},
                    {"arguments_fragment", value.arguments_fragment},
                };
            } else if constexpr (std::is_same_v<Value, ToolCallFinished>) {
                json = nlohmann::json{
                    {"type", "tool_call_finished"},
                    {"index", value.index},
                    {"call", value.call},
                };
            } else if constexpr (std::is_same_v<Value, UsageEvent>) {
                json = nlohmann::json{{"type", "usage_event"}, {"usage", value.usage}};
            } else if constexpr (std::is_same_v<Value, Finished>) {
                json = nlohmann::json{
                    {"type", "finished"},
                    {"reason", std::string{to_string(value.reason)}},
                };
                if (value.usage.has_value()) {
                    json["usage"] = *value.usage;
                }
                if (value.replay_state.has_value()) {
                    json["replay_state"] = *value.replay_state;
                }
            } else if constexpr (std::is_same_v<Value, StreamError>) {
                json = nlohmann::json{{"type", "stream_error"}, {"error", value.error}};
            }
        },
        event);
}

inline void from_json(const nlohmann::json& json, StreamEvent& event) {
    const std::string type = json.at("type").get<std::string>();
    if (type == "text_delta") {
        event = TextDelta{json.value("text", std::string{})};
    } else if (type == "reasoning_delta") {
        event = ReasoningDelta{json.value("text", std::string{})};
    } else if (type == "tool_call_started") {
        event = ToolCallStarted{
            json.at("index").get<std::uint32_t>(),
            json.at("id").get<ToolCallId>(),
            json.value("name", std::string{}),
        };
    } else if (type == "tool_call_delta") {
        event = ToolCallDelta{
            json.at("index").get<std::uint32_t>(),
            json.value("arguments_fragment", std::string{}),
        };
    } else if (type == "tool_call_finished") {
        ToolCallFinished finished;
        finished.index = json.at("index").get<std::uint32_t>();
        finished.call  = json.at("call").get<ToolCallAssembled>();
        event          = finished;
    } else if (type == "usage_event") {
        event = UsageEvent{json.at("usage").get<Usage>()};
    } else if (type == "finished") {
        const std::string reason = json.at("reason").get<std::string>();
        const auto parsed = parse_finish_reason(reason);
        if (!parsed.has_value()) {
            throw nlohmann::json::other_error::create(
                501, "unknown FinishReason: " + reason, &json);
        }
        Finished finished;
        finished.reason = *parsed;
        if (json.contains("usage") && !json.at("usage").is_null()) {
            finished.usage = json.at("usage").get<Usage>();
        }
        if (json.contains("replay_state") && !json.at("replay_state").is_null()) {
            finished.replay_state = json.at("replay_state").get<ReplayEnvelope>();
        }
        event = finished;
    } else if (type == "stream_error") {
        event = StreamError{json.at("error").get<LLMError>()};
    } else {
        throw nlohmann::json::other_error::create(
            501, "unknown StreamEvent type: " + type, &json);
    }
}

} // namespace ymh

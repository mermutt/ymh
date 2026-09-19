#pragma once

// Provider-agnostic streaming algebra, pinned by 08-llm-provider.md §2.1, §2.2,
// §3.2, and §3.3. Adapters map their native wire format onto these events; the
// agent loop (spec 06) is the only durable producer (L1).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

struct TextDelta {
    std::string text;
};

struct ReasoningDelta {
    std::string text;  // emitted only when capabilities().reasoning (L14)
};

// A fully assembled tool call (08 §3.2). `arguments` is always a JSON object.
struct ToolCallAssembled {
    ToolCallId     id;
    std::string    name;
    nlohmann::json arguments = nlohmann::json::object();
};

struct ToolCallStarted {
    std::uint32_t index = 0;  // provider-assigned position within the response
    ToolCallId    id;         // provider-assigned; becomes the tool_use block id
    std::string   name;
};

struct ToolCallDelta {
    std::uint32_t index = 0;
    std::string   arguments_fragment;  // raw JSON text fragment, not parsed
};

struct ToolCallFinished {
    std::uint32_t    index = 0;
    ToolCallAssembled call;  // id, name, parsed JSON object
};

struct UsageEvent {
    Usage usage;
};

struct Finished {
    FinishReason        reason = FinishReason::Other;
    std::optional<Usage> usage;  // convenience mirror; may be nullopt
};

struct StreamError {
    LLMError error;
};

using StreamEvent = std::variant<TextDelta,
                                 ReasoningDelta,
                                 ToolCallStarted,
                                 ToolCallDelta,
                                 ToolCallFinished,
                                 UsageEvent,
                                 Finished,
                                 StreamError>;

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

} // namespace ymh

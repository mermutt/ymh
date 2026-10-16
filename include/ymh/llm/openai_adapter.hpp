#pragma once

// OpenAI-compatible HTTP adapter and the `HttpTransport` seam
// (08-llm-provider.md §6). v1 uses libcurl on the calling thread (the host's
// bounded LLMPool worker); the adapter owns no thread and no `io_context`
// (L13). TLS verification is never disabled (L-F18).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/llm/stream.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

// 47-O-M3: the decoder's profile/offered-name inputs. `profile == nullptr` is
// inert (no detection, no normalization); `offered` is built from
// `request.tools` by `OpenAICompatibleProvider::stream()`.
struct ToolCallPolicy {
    const ModelProfile*       profile = nullptr;
    std::span<const ToolName> offered;
};

// Fully-qualified streaming POST (08 §6.1).
struct HttpRequest {
    std::string                                     method;
    std::string                                     url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                     body;
    std::chrono::milliseconds                       connect_timeout{10'000};
    std::chrono::milliseconds                       idle_timeout{60'000};
    std::chrono::milliseconds                       total_timeout{120'000};
};

// Response body bytes as they arrive; returning false aborts the transfer.
using HttpBodySink = std::function<bool(const char* data, std::size_t len)>;

struct HttpResponse {
    int          status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string  body;             // bounded capture; populated for error bodies
    LLMError     transport_error;  // code != None on transport failure
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    // Streaming POST. `on_body` is invoked serially, in order. Never throws.
    virtual Task<HttpResponse> postStream(const HttpRequest& request,
                                          HttpBodySink on_body,
                                          CancellationToken cancel) = 0;
};

// libcurl implementation of the transport seam. SSL peer/host verification is
// always on; a progress callback observes cancellation.
class CurlHttpTransport final : public HttpTransport {
public:
    CurlHttpTransport() = default;
    ~CurlHttpTransport() override = default;

    Task<HttpResponse> postStream(const HttpRequest& request,
                                  HttpBodySink on_body,
                                  CancellationToken cancel) override;
};

class OpenAICompatibleProvider final : public LLMProvider {
public:
    // 84-D2: the steady clock used to measure elapsed time against the overall
    // retry budget. Defaults to `std::chrono::steady_clock::now`; a test may
    // inject a fake clock to exercise the budget arithmetic without real sleeps.
    using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;

    OpenAICompatibleProvider(
        LLMProviderConfig config,
        ProviderCapabilities capabilities,
        std::shared_ptr<HttpTransport> transport,
        SteadyNow now = [] { return std::chrono::steady_clock::now(); });

    [[nodiscard]] ProviderId id() const override;
    [[nodiscard]] ProviderCapabilities capabilities() const override;
    [[nodiscard]] std::vector<ModelInfo> models() const override;
    [[nodiscard]] RetryPolicy retry_policy() const override { return config_.retry; }

    Task<LLMResponse> stream(const LLMRequest& request,
                             StreamSink sink,
                             CancellationToken cancel) override;

private:
    LLMProviderConfig              config_;
    ProviderCapabilities           capabilities_;
    std::shared_ptr<HttpTransport> transport_;
    SteadyNow                      now_;
    std::shared_ptr<LlmTranscript> transcript_;  // 74-D7 (null => disabled)

    // 74-D11: process-local monotonic correlation id source. One per daemon
    // process, shared by every provider instance, so ids are unique within the
    // workspace transcript file. 0 is reserved and is never minted.
    static std::atomic<std::uint64_t> request_seq_;
};

// Builds the OpenAI chat-completions request body (exposed for golden tests).
[[nodiscard]] nlohmann::json build_chat_completions_body(
    const LLMRequest& request,
    const ProviderCapabilities& capabilities);

// Maps an HTTP status plus error body to the LLM error taxonomy (08 §2.2).
[[nodiscard]] LLMError map_http_error(int status, std::string_view body);

// Exponential backoff with bounded jitter (08 §3.7). `attempt` is 1-based;
// `jitter_sample` is uniform in [0,1), so the delay lies within
// [1-jitter, 1+jitter] x min(base * 2^(attempt-1), max_delay).
[[nodiscard]] std::chrono::milliseconds backoff_delay(const RetryPolicy& policy,
                                                      std::uint32_t attempt,
                                                      double jitter_sample);

// 84-D1/84-D4: the per-attempt wall-clock deadline. Amended to a total function:
// `configured <= 0` returns `max(deadline, 0)`; `deadline <= 0` returns
// `configured`; otherwise `min(deadline, configured)`. The only behavioral change
// from the pre-84 shape is the previously-unbounded `configured == 0 && deadline
// > 0` arm (dormant: no caller sets `LLMRequest.deadline`).
[[nodiscard]] std::chrono::milliseconds effective_timeout(
    std::chrono::milliseconds deadline,
    std::chrono::milliseconds configured);

// 84-D3: the derived overall budget (used when `retry.total_timeout == 0`).
// `attempt_deadline * max_attempts + backoff_envelope`; `0` means the overall
// budget is unbounded, returned when `attempt_deadline <= 0`. Saturating (RB-F1).
[[nodiscard]] std::chrono::milliseconds derive_total_budget(
    std::chrono::milliseconds attempt_deadline,
    const RetryPolicy&        retry,
    std::uint32_t             max_attempts);

// 84-D2/84-D4: the overall budget for one `stream()` call across every attempt
// and backoff. A positive `retry_total_timeout` wins; otherwise the derived
// budget is used. A non-zero `request_deadline` `min`-caps the result. `0` means
// unbounded. Saturating.
[[nodiscard]] std::chrono::milliseconds resolve_overall_budget(
    std::chrono::milliseconds retry_total_timeout,
    std::chrono::milliseconds request_deadline,
    std::chrono::milliseconds attempt_deadline,
    const RetryPolicy&        retry,
    std::uint32_t             max_attempts);

// 84-D5: the per-attempt transport deadline (algebra (e)). Total (L1):
//   overall_budget == 0        -> attempt_deadline        (0 == unbounded)
//   overall_budget  > 0:
//       remaining = max(overall_budget - elapsed, 0)      (clamped to 0)
//       attempt_deadline == 0  -> remaining
//       else                   -> min(attempt_deadline, remaining)
// The clamped `elapsed >= overall` arm returns 0; the caller must not issue a
// transport call with this 0 (the stream() loop guard runs first).
[[nodiscard]] std::chrono::milliseconds per_attempt_timeout(
    std::chrono::milliseconds attempt_deadline,
    std::chrono::milliseconds overall_budget,
    std::chrono::milliseconds elapsed);

// Parses a `Retry-After` value (delta-seconds or IMF-fixdate). `nullopt` when
// absent or unparseable.
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_retry_after(
    std::string_view value);

} // namespace ymh

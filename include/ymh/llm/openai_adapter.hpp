#pragma once

// OpenAI-compatible HTTP adapter and the `HttpTransport` seam
// (08-llm-provider.md §6). v1 uses libcurl on the calling thread (the host's
// bounded LLMPool worker); the adapter owns no thread and no `io_context`
// (L13). TLS verification is never disabled (L-F18).

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
    OpenAICompatibleProvider(LLMProviderConfig config,
                             ProviderCapabilities capabilities,
                             std::shared_ptr<HttpTransport> transport);

    [[nodiscard]] ProviderId id() const override;
    [[nodiscard]] ProviderCapabilities capabilities() const override;
    [[nodiscard]] std::vector<ModelInfo> models() const override;
    [[nodiscard]] RetryPolicy retry_policy() const override { return config_.retry; }

    Task<LLMResponse> stream(const LLMRequest& request,
                             StreamSink sink,
                             CancellationToken cancel) override;

private:
    LLMProviderConfig             config_;
    ProviderCapabilities          capabilities_;
    std::shared_ptr<HttpTransport> transport_;
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

// Parses a `Retry-After` value (delta-seconds or IMF-fixdate). `nullopt` when
// absent or unparseable.
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_retry_after(
    std::string_view value);

} // namespace ymh

#pragma once

// The `LLMProvider` seam (08-llm-provider.md §3.4, §3.7, §5.2). A provider is a
// leaf: it never touches the session log, store, registry, or `EventBus` (L1),
// owns no threads (L13), and reports every failure as a typed value (L16).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/stream.hpp"

namespace ymh {

// Enumerable model metadata (08 §3.4). `max_context_tokens == 0` => unknown.
struct ModelInfo {
    ModelId       id;
    std::string   display_name;
    std::uint64_t max_context_tokens = 0;
};

// Truthful capability advertisement (08 §3.4, L14). `streaming` is always true;
// a non-streaming backend is emulated as one `TextDelta` + `Finished` (L2).
struct ProviderCapabilities {
    bool                       streaming = true;
    bool                       tool_calls = false;
    bool                       parallel_tool_calls = false;
    bool                       reasoning = false;
    bool                       usage_streaming = false;
    bool                       prompt_caching = false;
    std::optional<std::size_t> max_context_tokens;  // nullopt => unknown
};

// Retry/backoff policy (08 §3.7). Applied by the adapter; a retry occurs only
// before the first dispatched event (L7).
struct RetryPolicy {
    std::uint32_t             max_attempts = 3;  // total tries, incl. the first
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{30'000};
    double                    jitter = 0.25;  // +/- fraction, full-jitter sample
    bool                      honor_retry_after = true;
};

// Authoritative terminal value of `stream()` (08 §3.4). Text and reasoning are
// not re-delivered here; the sink is the single delivery path.
struct LLMResponse {
    StreamOutcome                  outcome = StreamOutcome::Completed;
    FinishReason                   finish = FinishReason::Other;
    std::optional<Usage>           usage;       // nullopt if not reported
    std::vector<ToolCallAssembled> tool_calls;  // index order
    LLMError                       error;       // code != None iff Failed
    RequestId                      request_id = 0;
    std::chrono::milliseconds      latency{0};  // diagnostics only
};

class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    // Stable registry identity, e.g. "openai-compatible".
    virtual ProviderId id() const = 0;

    // Streaming-first (L2). `sink` is invoked serially in stream order (L3).
    // Never throws: all failures are delivered as a terminal
    // `StreamEvent::Error` and/or a non-`Completed` `LLMResponse` (L16).
    virtual Task<LLMResponse> stream(const LLMRequest& request,
                                     StreamSink sink,
                                     CancellationToken cancel) = 0;

    // Truthful capability advertisement (L14).
    virtual ProviderCapabilities capabilities() const = 0;

    // Enumerable models, if the provider can list them; empty => unknown.
    virtual std::vector<ModelInfo> models() const { return {}; }

    // The provider's configured retry/backoff policy (08 §3.7; 28 §3.1
    // MEDIUM-1). Non-pure so test doubles need not override;
    // OpenAICompatibleProvider returns its stored LLMProviderConfig::retry.
    [[nodiscard]] virtual RetryPolicy retry_policy() const { return {}; }
};

// Layered model-resolution inputs, first non-empty wins (08 §5.2). The caller
// (spec 06) resolves the effective model and records it in
// `SessionStarted.model`; the provider never re-resolves (L11).
struct ModelResolutionSources {
    std::optional<ModelId> request_override;  // compaction/summarization
    std::optional<ModelId> session_model;     // immutable per session
    std::optional<ModelId> profile_model;
    std::optional<ModelId> project_model;
    std::optional<ModelId> global_model;
    std::optional<ModelId> llm_default_model;
    std::optional<ModelId> builtin_default;
};

// Returns the first non-empty source, or `nullopt` when every source is empty
// (the caller then fails with `ConfigError`, L-F12).
[[nodiscard]] std::optional<ModelId> resolve_model(const ModelResolutionSources& sources);

} // namespace ymh

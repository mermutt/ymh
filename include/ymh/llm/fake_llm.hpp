#pragma once

// Deterministic `FakeLLM` test double (08-llm-provider.md §7, §45). It
// implements the same `LLMProvider` seam, never sleeps, and never touches the
// network, wall clock, randomness, environment, or filesystem (L15).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/stream.hpp"

namespace ymh {

struct FakeToolCallStep {
    std::string                name;
    nlohmann::json             arguments;
    std::optional<std::string> id;  // else generated deterministically
};

struct FakeResponseStep {
    std::string                   text;       // streamed as TextDelta(s)
    std::optional<std::string>    reasoning;  // streamed as ReasoningDelta(s)
    std::vector<FakeToolCallStep> tool_calls;
    std::optional<Usage>          usage;
    FinishReason                  finish = FinishReason::Stop;
    std::optional<LLMError>       error;    // inject a terminal failure
    std::chrono::milliseconds     latency{0};  // simulated, never slept
};

struct FakeScript {
    std::vector<FakeResponseStep> steps;  // consumed one per stream() call
    std::size_t                   chunk_size = 4;  // max bytes per delta
    bool                          fail_after_first_delta = false;  // exercise L7
};

class FakeLLM final : public LLMProvider {
public:
    explicit FakeLLM(FakeScript script);
    FakeLLM(FakeScript script, ProviderCapabilities capabilities);

    [[nodiscard]] ProviderId id() const override;
    [[nodiscard]] ProviderCapabilities capabilities() const override;
    [[nodiscard]] std::vector<ModelInfo> models() const override;

    Task<LLMResponse> stream(const LLMRequest& request,
                             StreamSink sink,
                             CancellationToken cancel) override;

private:
    FakeScript                       script_;
    ProviderCapabilities             capabilities_;
    std::atomic<std::size_t>         next_step_{0};
};

} // namespace ymh

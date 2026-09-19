#include "ymh/llm/fake_llm.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ymh {
namespace {

// Splits text into at most `max_bytes`-sized chunks without ever cutting a
// multi-byte UTF-8 codepoint across chunks (08 §7).
std::vector<std::string> split_utf8(std::string_view text, std::size_t max_bytes) {
    std::vector<std::string> chunks;
    if (text.empty()) {
        return chunks;
    }

    const std::size_t limit = std::max<std::size_t>(1, max_bytes);
    std::string current;
    std::size_t index = 0;

    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if ((lead & 0x80u) == 0) {
            length = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            length = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            length = 4;
        }
        if (index + length > text.size()) {
            length = text.size() - index;
        }

        if (!current.empty() && current.size() + length > limit) {
            chunks.push_back(std::move(current));
            current.clear();
        }
        current.append(text.substr(index, length));
        index += length;

        if (current.size() >= limit) {
            chunks.push_back(std::move(current));
            current.clear();
        }
    }

    if (!current.empty()) {
        chunks.push_back(std::move(current));
    }
    return chunks;
}

LLMError default_mid_stream_error() {
    LLMError error;
    error.code = LLMErrorCode::NetworkError;
    error.detail = "fake mid-stream failure";
    error.retryable = true;
    return error;
}

} // namespace

FakeLLM::FakeLLM(FakeScript script)
    : script_(std::move(script)) {
    capabilities_.streaming = true;
    capabilities_.tool_calls = true;
    capabilities_.parallel_tool_calls = true;
    capabilities_.reasoning = true;
    capabilities_.usage_streaming = true;
    capabilities_.prompt_caching = true;
}

FakeLLM::FakeLLM(FakeScript script, ProviderCapabilities capabilities)
    : script_(std::move(script)), capabilities_(capabilities) {}

ProviderId FakeLLM::id() const {
    return "fake";
}

ProviderCapabilities FakeLLM::capabilities() const {
    return capabilities_;
}

std::vector<ModelInfo> FakeLLM::models() const {
    return {ModelInfo{"fake-model", "FakeLLM", 0}};
}

Task<LLMResponse> FakeLLM::stream(const LLMRequest& request,
                                  StreamSink sink,
                                  CancellationToken cancel) {
    const std::size_t step_index = next_step_.fetch_add(1);

    LLMResponse response;
    response.request_id = request.request_id;

    bool aborted = false;
    bool stop_after_first = false;

    const auto abort_cancelled = [&]() -> Task<LLMResponse> {
        response.outcome = StreamOutcome::Cancelled;
        response.finish = FinishReason::Other;
        response.error = LLMError{};
        return Task<LLMResponse>{std::move(response)};
    };

    const auto raw_emit = [&](const StreamEvent& event) -> bool {
        if (aborted) {
            return false;
        }
        if (cancel.cancelled()) {
            aborted = true;
            return false;
        }
        if (sink(event) == SinkFlow::Stop) {
            aborted = true;
            return false;
        }
        return true;
    };

    const auto dispatch = [&](const StreamEvent& event) {
        if (aborted || stop_after_first) {
            return;
        }
        if (!raw_emit(event)) {
            return;
        }
        if (script_.fail_after_first_delta) {
            stop_after_first = true;
        }
    };

    if (step_index >= script_.steps.size()) {
        response.outcome = StreamOutcome::Completed;
        response.finish = FinishReason::Stop;
        if (!raw_emit(StreamEvent{Finished{FinishReason::Stop, std::nullopt, std::nullopt}})) {
            return abort_cancelled();
        }
        return Task<LLMResponse>{std::move(response)};
    }

    const FakeResponseStep& step = script_.steps[step_index];
    response.latency = step.latency;

    if (step.error.has_value() && !script_.fail_after_first_delta) {
        response.outcome = StreamOutcome::Failed;
        response.finish = FinishReason::Error;
        response.error = *step.error;
        if (!raw_emit(StreamEvent{StreamError{*step.error}})) {
            return abort_cancelled();
        }
        return Task<LLMResponse>{std::move(response)};
    }

    if (step.reasoning.has_value()) {
        for (std::string& chunk : split_utf8(*step.reasoning, script_.chunk_size)) {
            dispatch(StreamEvent{ReasoningDelta{std::move(chunk)}});
        }
    }

    for (std::string& chunk : split_utf8(step.text, script_.chunk_size)) {
        dispatch(StreamEvent{TextDelta{std::move(chunk)}});
    }

    std::vector<ToolCallAssembled> tool_calls;
    for (std::size_t index = 0; index < step.tool_calls.size(); ++index) {
        const FakeToolCallStep& call = step.tool_calls[index];
        const std::string id =
            call.id.value_or("call_" + std::to_string(step_index) + "_" + std::to_string(index));
        const auto position = static_cast<std::uint32_t>(index);

        dispatch(StreamEvent{ToolCallStarted{position, id, call.name}});
        for (std::string& fragment : split_utf8(call.arguments.dump(), script_.chunk_size)) {
            dispatch(StreamEvent{ToolCallDelta{position, std::move(fragment)}});
        }

        ToolCallAssembled assembled{id, call.name, call.arguments};
        tool_calls.push_back(assembled);
        dispatch(StreamEvent{ToolCallFinished{position, std::move(assembled)}});
    }

    if (aborted) {
        return abort_cancelled();
    }

    if (script_.fail_after_first_delta) {
        const LLMError error = step.error.value_or(default_mid_stream_error());
        response.outcome = StreamOutcome::Failed;
        response.finish = FinishReason::Error;
        response.error = error;
        if (!raw_emit(StreamEvent{StreamError{error}})) {
            return abort_cancelled();
        }
        return Task<LLMResponse>{std::move(response)};
    }

    if (step.usage.has_value()) {
        dispatch(StreamEvent{UsageEvent{*step.usage}});
    }

    if (aborted) {
        return abort_cancelled();
    }

    response.outcome = StreamOutcome::Completed;
    response.finish = step.finish;
    response.usage = step.usage;
    response.tool_calls = std::move(tool_calls);
    if (!raw_emit(StreamEvent{Finished{step.finish, step.usage, step.replay_state}})) {
        return abort_cancelled();
    }
    return Task<LLMResponse>{std::move(response)};
}

} // namespace ymh

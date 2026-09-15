#pragma once

// Provider-agnostic tool-call assembler (08-llm-provider.md §4.2). Every
// adapter shares it so assembly semantics are identical everywhere.
//
// One instance per in-flight response. Not thread-safe; the provider invokes
// it on its single stream context. Errors are surfaced by the provider as a
// terminal `StreamError` (L10, L-F7, L-F14); the assembler records the first
// error and rejects further mutation.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/llm/stream.hpp"

namespace ymh {

class ToolCallAssembler {
public:
    explicit ToolCallAssembler(std::size_t max_arguments_bytes);

    // Returns nullopt while the call is incomplete. A duplicate start for an
    // index is a `ProviderInternal` protocol violation.
    std::optional<ToolCallAssembled> onStarted(std::uint32_t index,
                                               ToolCallId id,
                                               std::string name);

    // Appends a raw JSON fragment. Unknown index / mutation after error /
    // mutation after finish is a `ProviderInternal` protocol violation.
    void onDelta(std::uint32_t index, std::string_view fragment);

    // Parses the accumulated fragments into a JSON object. Invalid JSON,
    // non-object, unknown index, or a duplicate finish is an error.
    std::optional<ToolCallAssembled> onFinished(std::uint32_t index);

    // All finished calls in index order. Consumes the assembler.
    std::vector<ToolCallAssembled> take_ordered() &&;

    [[nodiscard]] bool has_error() const noexcept { return error_.code != LLMErrorCode::None; }
    [[nodiscard]] const LLMError& error() const noexcept { return error_; }

private:
    struct CallState {
        ToolCallId   id;
        std::string  name;
        std::string  arguments;
        bool         started = false;
        bool         finished = false;
    };

    CallState* find(std::uint32_t index) noexcept;
    void fail(LLMErrorCode code, std::string detail);

    std::size_t                          max_arguments_bytes_;
    std::vector<std::pair<std::uint32_t, CallState>> calls_;
    LLMError                             error_;
};

} // namespace ymh

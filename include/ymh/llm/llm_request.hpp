#pragma once

// Request-side LLM seam types, pinned by 08-llm-provider.md §3.1.
//
// `Message`, `Role`, `ContentBlock`, and `Usage` are owned by 00 §12 /
// 01 §4.5 and reused verbatim from `ymh/agent/message.hpp`; they are never
// redefined here. This header adds `GenerationParameters`, `RequestId`, and an
// explicit deadline additively to the §12 sketch (08 §14.1(f)).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

// Registry key and opaque model id (08 §2.1). Neither is a path and neither is
// validated against a fixed list: a local endpoint may serve any model.
// (Also declared identically in `llm_call_config.hpp`.)

// Per-process monotonic correlation id used only for logs/diagnostics and to
// correlate a request with its retries (08 §2.1). Never persisted.
using RequestId = std::uint64_t;

// `ToolName`/`ToolVersion`/`ToolSchema` are the canonical tool-seam types owned
// by 07-tools-execution.md §3.2 and materialised in `ymh/tools/tool.hpp`; they
// are reused verbatim here (never redefined) so an `LLMRequest` and a
// `ToolRegistry` speak the same type. The earlier provisional definitions were
// relocated without renaming, exactly as their comment anticipated.

// Generation parameters mapped onto the provider wire format. Every field is
// optional; `nullopt` means "omit, let the provider choose" (08 §3.1).
struct GenerationParameters {
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;             // 47-D8
    std::optional<std::uint32_t> max_output_tokens;
    std::vector<std::string>     stop;
    std::optional<std::string>   tool_choice;       // "auto"|"none"|"required"|name
    std::optional<std::string>   reasoning_effort;  // "low"|"medium"|"high"|"xhigh" (47-D10)
    std::optional<std::uint32_t> seed;              // best-effort determinism
};

// A complete, already-assembled request. `model` is required and non-empty
// (L11); the provider never substitutes a model silently. `messages` is a
// complete, already-compacted list — the provider performs no context assembly
// and no history trimming (08 §3.1, §31/§32).
struct LLMRequest {
    ModelId                  model;
    std::vector<Message>     messages;
    std::vector<ToolSchema>  tools;                 // empty => no tool calling
    GenerationParameters     parameters;
    RequestId                request_id = 0;        // additive (08 §14.1(f))
    std::chrono::milliseconds deadline{0};          // 0 => provider default
    SessionId                session_id;            // NEW (26-D3)
    std::optional<CallPurpose> purpose;             // NEW; absent == conversation
};

} // namespace ymh

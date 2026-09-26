#include "ymh/llm/tool_call_assembler.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ymh {
namespace {

bool is_blank(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

} // namespace

ToolCallAssembler::ToolCallAssembler(std::size_t max_arguments_bytes,
                                     ToolArgumentPolicy policy)
    : max_arguments_bytes_(max_arguments_bytes), policy_(policy) {}

ToolCallAssembler::CallState* ToolCallAssembler::find(std::uint32_t index) noexcept {
    for (auto& entry : calls_) {
        if (entry.first == index) {
            return &entry.second;
        }
    }
    return nullptr;
}

void ToolCallAssembler::fail(LLMErrorCode code, std::string detail) {
    if (error_.code == LLMErrorCode::None) {
        error_.code = code;
        error_.detail = std::move(detail);
    }
}

std::optional<nlohmann::json> ToolCallAssembler::parse_arguments(const std::string& raw) {
    if (raw.empty() || is_blank(raw)) {
        return nlohmann::json::object();
    }
    nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) {
        if (policy_ == ToolArgumentPolicy::NonObjectToEmpty) {
            return nlohmann::json::object();
        }
        fail(LLMErrorCode::MalformedToolCall, "tool-call arguments are not valid JSON");
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        if (policy_ == ToolArgumentPolicy::NonObjectToEmpty) {
            return nlohmann::json::object();
        }
        fail(LLMErrorCode::MalformedToolCall, "tool-call arguments are not a JSON object");
        return std::nullopt;
    }
    return parsed;
}

std::optional<ToolCallAssembled> ToolCallAssembler::onStarted(std::uint32_t index,
                                                              ToolCallId id,
                                                              std::string name) {
    if (has_error()) {
        return std::nullopt;
    }
    if (find(index) != nullptr) {
        fail(LLMErrorCode::ProviderInternal, "duplicate tool-call index");
        return std::nullopt;
    }

    CallState state;
    state.id = std::move(id);
    state.name = std::move(name);
    state.started = true;
    calls_.emplace_back(index, std::move(state));
    return std::nullopt;
}

void ToolCallAssembler::onDelta(std::uint32_t index, std::string_view fragment) {
    if (has_error()) {
        return;
    }

    CallState* call = find(index);
    if (call == nullptr || !call->started) {
        fail(LLMErrorCode::ProviderInternal, "tool-call delta for unknown index");
        return;
    }
    if (call->finished) {
        fail(LLMErrorCode::ProviderInternal, "tool-call delta after finish");
        return;
    }
    if (call->arguments.size() + fragment.size() > max_arguments_bytes_) {
        fail(LLMErrorCode::MalformedToolCall, "tool-call arguments exceed cap");
        return;
    }

    call->arguments.append(fragment);
}

std::optional<ToolCallAssembled> ToolCallAssembler::onFinished(std::uint32_t index) {
    if (has_error()) {
        return std::nullopt;
    }

    CallState* call = find(index);
    if (call == nullptr || !call->started) {
        fail(LLMErrorCode::ProviderInternal, "finish for unknown tool-call index");
        return std::nullopt;
    }
    if (call->finished) {
        fail(LLMErrorCode::ProviderInternal, "duplicate tool-call finish");
        return std::nullopt;
    }
    call->finished = true;

    std::optional<nlohmann::json> arguments = parse_arguments(call->arguments);
    if (!arguments.has_value()) {
        return std::nullopt;
    }
    return ToolCallAssembled{call->id, call->name, std::move(*arguments)};
}

std::vector<ToolCallAssembled> ToolCallAssembler::take_ordered() && {
    std::vector<std::pair<std::uint32_t, CallState>> ordered = std::move(calls_);
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });

    std::vector<ToolCallAssembled> assembled;
    assembled.reserve(ordered.size());
    for (auto& entry : ordered) {
        if (!entry.second.finished || has_error()) {
            continue;
        }
        std::optional<nlohmann::json> arguments = parse_arguments(entry.second.arguments);
        if (!arguments.has_value()) {
            continue;
        }
        assembled.push_back(ToolCallAssembled{entry.second.id,
                                              entry.second.name,
                                              std::move(*arguments)});
    }
    return assembled;
}

} // namespace ymh

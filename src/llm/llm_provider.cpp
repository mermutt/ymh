#include "ymh/llm/llm_provider.hpp"

namespace ymh {
namespace {

bool has_value(const std::optional<ModelId>& source) {
    return source.has_value() && !source->empty();
}

} // namespace

std::optional<ModelId> resolve_model(const ModelResolutionSources& sources) {
    if (has_value(sources.request_override)) {
        return sources.request_override;
    }
    if (has_value(sources.session_model)) {
        return sources.session_model;
    }
    if (has_value(sources.profile_model)) {
        return sources.profile_model;
    }
    if (has_value(sources.project_model)) {
        return sources.project_model;
    }
    if (has_value(sources.global_model)) {
        return sources.global_model;
    }
    if (has_value(sources.llm_default_model)) {
        return sources.llm_default_model;
    }
    if (has_value(sources.builtin_default)) {
        return sources.builtin_default;
    }
    return std::nullopt;
}

} // namespace ymh

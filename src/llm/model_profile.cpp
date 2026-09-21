#include "ymh/llm/model_profile.hpp"

#include <string>
#include <string_view>

namespace ymh {
namespace {

// The ONLY Muse-named identifier table in the tree (47-I6, §19.2): removing
// this file (with its header) removes every Muse-specific value.
constexpr std::string_view kMuseGlimmerProfileId = "muse-glimmer";

const ModelProfile kMuseGlimmerProfile = [] {
    ModelProfile profile;
    profile.id                       = std::string{kMuseGlimmerProfileId};
    profile.force_first_tool_call    = true;
    profile.normalize_tool_arguments = true;
    profile.detect_leaked_tool_calls = true;
    profile.detect_atem_tool_calls   = true;
    profile.forbidden_stop_tokens    = {
        "<|begin_of_text|>", "<|end_of_text|>", "<|start|>",
        "<|message|>", "<|eom|>", "<|eot|>",
    };
    profile.capabilities.streaming            = true;
    profile.capabilities.tool_calls           = true;
    profile.capabilities.parallel_tool_calls  = true;
    profile.capabilities.reasoning            = true;
    profile.capabilities.usage_streaming      = true;
    profile.capabilities.prompt_caching       = true;
    profile.temperature                       = 1.0;
    profile.top_p                             = 0.95;
    profile.top_k                             = 64;
    return profile;
}();

} // namespace

const ModelProfile* find_model_profile(std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }
    if (id == kMuseGlimmerProfileId) {
        return &kMuseGlimmerProfile;
    }
    return nullptr;
}

bool is_known_model_profile(std::string_view id) noexcept {
    return find_model_profile(id) != nullptr;
}

std::string_view default_import_profile_id() noexcept {
    return kMuseGlimmerProfileId;
}

} // namespace ymh

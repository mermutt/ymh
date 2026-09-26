#pragma once

// Tool presentation mode (36 §2.6, 26 §2.3.6). `Native` presents each visible
// tool schema as a function definition. `Ptc` and `Both` are reserved values
// that fail loud at load (36-I13, 26-F14); the PTC SDK generator is deferred
// (26-D22). Only `Native` is implemented.

#include <cstdint>
#include <string_view>

namespace ymh {

enum class ToolPresentationMode : std::uint8_t {
    Native,
    Ptc,
    Both,
};

[[nodiscard]] inline std::string_view tool_presentation_name(
    ToolPresentationMode mode) noexcept {
    switch (mode) {
        case ToolPresentationMode::Native:
            return "native";
        case ToolPresentationMode::Ptc:
            return "ptc";
        case ToolPresentationMode::Both:
            return "both";
    }
    return {};
}

} // namespace ymh

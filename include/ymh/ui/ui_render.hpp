#pragma once

// Pure renderers (10 §8.2/§8.4, §20.12). `renderToAnsi` reads the model and
// writes ANSI to a caller buffer; it never performs I/O or advances a timer
// (U3, D16). The golden tests and the PTY assertions share this code path.

#include <string>

#include <ftxui/dom/elements.hpp>

#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/theme.hpp"
#include "ymh/ui/ui_model.hpp"

namespace ymh::ui {

enum class LayoutMode : std::uint8_t {
    Narrow,
    Normal,
    Wide,
};

[[nodiscard]] LayoutMode calculate_layout(int width);
[[nodiscard]] ftxui::Element build_ui(const UiModel& model, TerminalSize size,
                                      const Theme& theme);
[[nodiscard]] std::string render_to_ansi(const UiModel& model, TerminalSize size,
                                         const Theme& theme = {});

} // namespace ymh::ui

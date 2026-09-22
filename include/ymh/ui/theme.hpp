#pragma once

#include <cstdint>

#include <ftxui/screen/color.hpp>

namespace ymh::ui {

// 51-D2.8: variant scaffold. Selection is deferred (OQ-51-2); the supervisor
// constructs Dark for now. The variant field is appended last (field-order
// rule, 51-D2.2).
enum class ThemeVariant : std::uint8_t { Dark, Light };

struct Theme {
    bool color = true;
    // Independent user/theme opt-out of the user-block background on a color
    // terminal (17 §3, RB-01). The left bar is unconditional; `color == false`
    // already suppresses all color, so the background is absent on monochrome
    // regardless of this flag (F-14). 51-D2.4: it now also suppresses the
    // composer tint.
    bool user_block = true;
    // RB-16: accent for the selected row of the slash-command completion list.
    // Kept in the theme so the renderer never hard-codes the selection colour;
    // `paint()` still suppresses it when `color` is false. Brighter than the
    // unselected rows (which stay `Color::Green`).
    ftxui::Color completion_selected = ftxui::Color::CyanLight;
    // 48-D6.6: the text-hierarchy and tool-line seams. `paint()` no-ops when
    // `color` is false, so a monochrome terminal degrades to plain text.
    ftxui::Color user_foreground = ftxui::Color::White;
    // 51-D2.1: the user gutter is the pinned opencode blue, not `Color::Green`.
    ftxui::Color user_bar = ftxui::Color::RGB(92, 156, 245);
    ftxui::Color tool_name = ftxui::Color::CyanLight;
    ftxui::Color tool_args = ftxui::Color::GrayLight;
    // ---- 51-D2.2: new fields APPENDED after the shipped fields ----
    // 51-D2.1: the transcript/composer tint source (was the literal
    // `RGB(40,42,54)` in `ui_render.cpp`).
    ftxui::Color user_block_background = ftxui::Color::RGB(40, 42, 54);
    ThemeVariant variant = ThemeVariant::Dark;
};

// 51-D2.8: variant-resolved construction. `Dark` reproduces the defaults above;
// `Light` substitutes the pinned light tokens. There is no `true_color`
// parameter: `color` is the sole colour gate (51-D2.1). Defined `inline` here so
// no new translation unit or CMake change is needed.
[[nodiscard]] inline Theme make_theme(bool color, ThemeVariant variant) {
    Theme theme;
    theme.color = color;
    theme.variant = variant;
    if (variant == ThemeVariant::Light) {
        theme.user_bar = ftxui::Color::RGB(59, 125, 216);
        theme.user_block_background = ftxui::Color::RGB(238, 240, 244);
    }
    return theme;
}

} // namespace ymh::ui

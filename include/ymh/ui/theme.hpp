#pragma once

#include <ftxui/screen/color.hpp>

namespace ymh::ui {

struct Theme {
    bool color = true;
    // Independent user/theme opt-out of the user-block background on a color
    // terminal (17 §3, RB-01). The left bar is unconditional; `color == false`
    // already suppresses all color, so the background is absent on monochrome
    // regardless of this flag (F-14).
    bool user_block = true;
    // RB-16: accent for the selected row of the slash-command completion list.
    // Kept in the theme so the renderer never hard-codes the selection colour;
    // `paint()` still suppresses it when `color` is false. Brighter than the
    // unselected rows (which stay `Color::Green`).
    ftxui::Color completion_selected = ftxui::Color::CyanLight;
};

} // namespace ymh::ui

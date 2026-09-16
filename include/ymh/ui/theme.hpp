#pragma once

namespace ymh::ui {

struct Theme {
    bool color = true;
    // Independent user/theme opt-out of the user-block background on a color
    // terminal (17 §3, RB-01). The left bar is unconditional; `color == false`
    // already suppresses all color, so the background is absent on monochrome
    // regardless of this flag (F-14).
    bool user_block = true;
};

} // namespace ymh::ui

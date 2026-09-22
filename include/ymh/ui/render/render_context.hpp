#pragma once

#include <cstddef>

#include "ymh/ui/theme.hpp"

namespace ymh::ui {

struct RenderContext {
    int         width = 80;
    // 48-D7.3 (Rev 2): the conversation pane's content-box width. Set by
    // `build_ui` to `max(1, width - 2 /*border*/ - 1 /*vscroll_indicator*/)`;
    // the tool line truncates to THIS, not `width`.
    int         content_width = 80;
    Theme       theme{};
    bool        compact = false;
    std::size_t spinner_frame = 0;
    // 51-D3.4/51-H5: display columns consumed by enclosing prefixes (block quote
    // `"> "` = 2, list markers, the user-block LeftBar gutter = 2). Default 0.
    // Only the table layout reads it; other renderers' wrapping is unchanged.
    int         indent = 0;
};

} // namespace ymh::ui

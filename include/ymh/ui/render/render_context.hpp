#pragma once

#include <cstddef>

#include "ymh/ui/theme.hpp"

namespace ymh::ui {

struct RenderContext {
    int         width = 80;
    Theme       theme{};
    bool        compact = false;
    std::size_t spinner_frame = 0;
};

} // namespace ymh::ui

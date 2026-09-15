#pragma once

#include "ymh/ui/theme.hpp"

namespace ymh::ui {

struct RenderContext {
    int   width = 80;
    Theme theme{};
    bool  compact = false;
};

} // namespace ymh::ui

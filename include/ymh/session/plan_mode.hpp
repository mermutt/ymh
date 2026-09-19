#pragma once

// Plan-mode projection (25-D2). Pure fold over a session's resolved event log:
// the last `plan/mode` wins; an empty log (or one with none) is inactive.

#include "ymh/core/event.hpp"

namespace ymh {

[[nodiscard]] bool plan_mode_active(const EventRange& events) noexcept;

} // namespace ymh

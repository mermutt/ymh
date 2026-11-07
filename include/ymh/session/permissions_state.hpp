#pragma once

// 119-D17/D36: the per-session sandbox/approval fold. Whole-value-replace
// events; the last of each kind wins. Malformed payloads are ignored (the fold
// never throws), and an empty log yields no values so the caller can fall back
// to the header preset and then the deployment baseline.

#include <optional>
#include <string>

#include "ymh/session/events.hpp"

namespace ymh {

struct PermissionsState {
    std::optional<std::string> mode;    // read-only | workspace-write | danger-full-access
    std::optional<std::string> policy;  // ask | never
    std::optional<std::string> preset;  // permissions.presets name
};

[[nodiscard]] PermissionsState permissions_state(const EventRange& events) noexcept;

} // namespace ymh

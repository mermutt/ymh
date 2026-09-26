#pragma once

// 45-D6/45-D7: pure renderers for the read-only informational commands `/mcp`
// and `/status`. They take wire/model values and return the exact block text the
// supervisor appends to the active session (or pushes to the notice ring).

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/ui/ui_model.hpp"

namespace ymh::ui {

// 45-D6.11/45-F19: the pinned degradation notice for a daemon that cannot serve
// `mcp.status` (`MethodNotFound` / `MethodNotAllowedForProfile`). Any other code
// (including success) yields `std::nullopt`.
[[nodiscard]] std::optional<std::string> mcp_unavailable_notice(int error_code);

// 45-D9.9: the pinned empty-roster notice (also used when `agent.list` is
// unavailable, 45-D9.10).
[[nodiscard]] std::string agent_no_agents_notice();

// 45-D9.5: a blank session whose roster has no other selectable preset.
[[nodiscard]] std::string agent_no_others_notice();

// 45-D9.5: the daemon rejected `agent.select` because the composition is fixed.
[[nodiscard]] std::string agent_composition_fixed_notice();

// 45-D9.10: the pinned degradation for a daemon that cannot serve `agent.list`
// (`MethodNotFound` / `MethodNotAllowedForProfile`). Any other code yields
// `std::nullopt`.
[[nodiscard]] std::optional<std::string> agent_unavailable_notice(int error_code);

// 45-D6 §8.2: renders the `/mcp` block from the `mcp.status` result.
[[nodiscard]] std::string format_mcp_block(const nlohmann::json& result);

// 45-D7: the inputs to the `/status` block. `snapshot` is null when the session
// has no loaded `context.show` reply (renders `(unavailable)`); `has_session`
// false renders the local lines only with a `(no session)` note.
struct StatusBlockInputs {
    std::string            version;      // empty => "unknown"
    std::string            model;        // empty => "unknown"
    ApiConnectivity        api_state = ApiConnectivity::Unknown;
    std::string            last_error;
    DaemonStatus           daemon = DaemonStatus::NotRunning;
    std::string            workspace_title;
    bool                   has_session = false;
    const ContextSnapshot* snapshot = nullptr;
};

// 45-D7 §9.2: renders the `/status` block, bounded to 20 tool rows.
[[nodiscard]] std::string format_status_block(const StatusBlockInputs& inputs);

} // namespace ymh::ui

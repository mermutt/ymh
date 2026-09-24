#pragma once

// 55-D12: the shared per-child delegation value types and the route-catalog /
// routability seam. Included by `agent_registry.hpp` (for `ChildSpawnRequest`),
// `subagent_service.hpp`, and `subagent_tools.hpp` (spec 55 §4, declaration
// before use).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/agent/preset.hpp"

namespace ymh {

// 55-D12: the resolved per-child route. `endpoint` is the 54-D2 endpoint name
// ("" = the anonymous default endpoint); `model` is the wire id and `model_name`
// is the `llm.models` entry name ("" for a literal).
struct ChildRoute {
    std::string                  endpoint;
    std::optional<std::string>   profile_id;
    std::string                  model;
    std::string                  model_name;
    std::optional<std::string>   reasoning_effort;
    std::optional<std::uint32_t> max_tokens;
};

// 55-D12: `DelegationToolConfig::agent_options` — the configured child defaults
// overlaid by a per-call route override (55-D6 precedence step 1/2).
struct ChildAgentOptions {
    std::optional<std::string>   endpoint;
    std::optional<std::string>   profile_id;  // 55-H4
    std::optional<std::string>   model;
    std::optional<std::string>   reasoning_effort;
    std::optional<std::uint32_t> max_tokens;
};

// 55-D6/§4: the route-catalog / routability seam. Backs both the D6 step-4
// model/effort validation and `list_subagent_models`. The live implementation
// composes `ModelCatalog` with `LlmRuntime`'s registered routes.
class RouteCatalog {
public:
    virtual ~RouteCatalog() = default;

    // Endpoint names that currently resolve to a registered route ("" = the
    // anonymous default endpoint). `list_subagent_models` with no arguments.
    [[nodiscard]] virtual std::vector<std::string> routable_endpoints() const = 0;
    [[nodiscard]] virtual bool is_routable_endpoint(std::string_view endpoint) const = 0;
    // `llm.models` entry names advertised by an endpoint.
    [[nodiscard]] virtual std::vector<std::string> models_for(std::string_view endpoint) const = 0;
    // Accepted `reasoning_effort` values for an endpoint+model (empty = free-form).
    [[nodiscard]] virtual std::vector<std::string> efforts_for(std::string_view endpoint,
                                                               std::string_view model) const = 0;
    // Catalog membership of a wire model id under an endpoint (D6 validation).
    [[nodiscard]] virtual bool is_catalog_member(std::string_view endpoint,
                                                 std::string_view model) const = 0;
};

// 55-D12: the per-instance delegation tool configuration. `background_mode` is a
// property of the tool instance, never of a call (55-D1). No `maxDepth`:
// `presets.max_depth` is the single source of truth (42-D18, 55-D7).
struct DelegationToolConfig {
    std::string                    provider;                // provider name on ctx.subagents
    std::string                    tool_name = "subagent";  // distinct per instance
    enum class BackgroundMode : std::uint8_t { OneShot, Continuable };
    BackgroundMode                 background_mode = BackgroundMode::OneShot;
    bool                           enable_run_in_background = true;
    bool                           model_selection = false;  // exposes route fields + list tool
    std::optional<ChildAgentOptions> agent_options;
    std::optional<std::string>     persona;
    std::optional<ToolRestriction> tool_filter;
};

// 56-D1: the verbatim dsh `tool:<toolName>` guidance, `{tool}` = `tool_name`.
[[nodiscard]] std::string delegation_guidance_text(std::string_view tool_name);

} // namespace ymh

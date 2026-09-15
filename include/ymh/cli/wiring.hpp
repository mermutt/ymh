#pragma once

// Adapters from the layered `Config` (§37) onto the component config structs:
// `LLMProviderConfig` (08 §5.1), `PermissionConfig` (09), and `AgentConfig`
// (06 §3). Keeping these conversions out of `ymh_config` keeps that library
// free of a dependency on the agent/policy layers.

#include <string>

#include "ymh/agent/agent.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {

[[nodiscard]] LLMProviderConfig to_provider_config(const Config& config);

[[nodiscard]] PermissionConfig to_permission_config(const Config& config);

// `system_prompt` defaults to `default_system_prompt()` when the config's
// `agent.system_prompt` is empty.
[[nodiscard]] AgentConfig to_agent_config(const Config& config);

[[nodiscard]] std::string default_system_prompt();

// Per-process boot nonce (02 §2.1): pid + steady-clock + counter.
[[nodiscard]] std::string make_boot_id();

} // namespace ymh

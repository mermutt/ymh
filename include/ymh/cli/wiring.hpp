#pragma once

// Adapters from the layered `Config` (§37) onto the component config structs:
// `LLMProviderConfig` (08 §5.1), `PermissionConfig` (09), and `AgentConfig`
// (06 §3). Keeping these conversions out of `ymh_config` keeps that library
// free of a dependency on the agent/policy layers.

#include <string>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh {

[[nodiscard]] LLMProviderConfig to_provider_config(const Config& config);

[[nodiscard]] PermissionConfig to_permission_config(const Config& config);

// `system_prompt` defaults to `default_system_prompt()` when the config's
// `agent.system_prompt` is empty.
[[nodiscard]] AgentConfig to_agent_config(const Config& config);

// 13-context-compaction.md §3.2/§6.6: the compaction policy from the layered
// config. Validates `max_summary_bytes` against `PersistenceConfig::
// max_payload_bytes` (throws `ConfigError`, C18).
[[nodiscard]] CompactionPolicy to_compaction_policy(const Config& config);

[[nodiscard]] std::string default_system_prompt();

// 11-m2-errata §6 (D17): the per-process boot nonce. Minted EXACTLY ONCE at
// daemon startup (04 §3.3 step 3), after chdir, before store open; a UUIDv4
// (03 §9.7 / 02 §5.2) stored in `HostIdentity.boot_id` and read by reference
// everywhere else. The adapters are one-way and never mint.
[[nodiscard]] BootId mint_boot_id();
[[nodiscard]] HostBootId to_host_boot_id(const BootId& boot_id);
[[nodiscard]] BootId to_boot_id(const HostBootId& boot_id);
[[nodiscard]] protocol::HostBootId to_protocol_boot_id(const HostBootId& boot_id);

} // namespace ymh

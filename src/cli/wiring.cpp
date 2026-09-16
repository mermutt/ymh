#include "ymh/cli/wiring.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ymh {
namespace {

PolicyVerdict parse_verdict(std::string_view value, std::string_view key) {
    if (value == "allow") {
        return PolicyVerdict::Allow;
    }
    if (value == "ask") {
        return PolicyVerdict::Ask;
    }
    if (value == "deny") {
        return PolicyVerdict::Deny;
    }
    throw ConfigError("[permissions]." + std::string{key} +
                      " must be one of: allow, ask, deny (got '" + std::string{value} + "')");
}

void add_rule(PermissionConfig& config,
              std::string_view tool,
              PolicyVerdict verdict,
              std::string_view id) {
    PolicyRule rule;
    rule.tool   = std::string{tool};
    rule.effect = verdict;
    rule.layer  = PolicyRule::Layer::Global;
    rule.id     = std::string{id};
    config.rules.push_back(std::move(rule));
}

} // namespace

BootId mint_boot_id() { return BootId{generate_uuid_v4()}; }

HostBootId to_host_boot_id(const BootId& boot_id) { return HostBootId{boot_id.value}; }

BootId to_boot_id(const HostBootId& boot_id) { return BootId{boot_id.value}; }

protocol::HostBootId to_protocol_boot_id(const HostBootId& boot_id) {
    return protocol::HostBootId{boot_id.value};
}

std::string default_system_prompt() {
    return "You are ymh, a terminal coding agent. Inspect and modify the workspace with the "
           "provided tools. Prefer reading files before editing them. Be concise and never "
           "invent tool output.";
}

LLMProviderConfig to_provider_config(const Config& config) {
    LLMProviderConfig provider;
    provider.provider     = config.llm.provider;
    provider.base_url     = config.llm.base_url;
    provider.model        = effective_model(config);
    provider.api_key_env  = config.llm.api_key_env;
    provider.connect_timeout = config.llm.connect_timeout;
    provider.idle_timeout    = config.llm.idle_timeout;
    provider.request_timeout = config.llm.request_timeout;
    provider.retry.max_attempts      = config.llm.retry.max_attempts;
    provider.retry.base_delay        = config.llm.retry.base_delay;
    provider.retry.max_delay         = config.llm.retry.max_delay;
    provider.retry.jitter            = config.llm.retry.jitter;
    provider.retry.honor_retry_after = config.llm.retry.honor_retry_after;
    return provider;
}

PermissionConfig to_permission_config(const Config& config) {
    PermissionConfig permissions;
    permissions.default_verdict = PolicyVerdict::Ask;

    const PolicyVerdict read  = parse_verdict(config.permissions.read, "read");
    const PolicyVerdict write = parse_verdict(config.permissions.write, "write");
    const PolicyVerdict shell = parse_verdict(config.permissions.shell, "shell");

    add_rule(permissions, "read_file", read, "default.read_file");
    add_rule(permissions, "grep", read, "default.grep");
    add_rule(permissions, "glob", read, "default.glob");
    add_rule(permissions, "write_file", write, "default.write_file");
    add_rule(permissions, "edit_file", write, "default.edit_file");
    add_rule(permissions, "shell", shell, "default.shell");
    return permissions;
}

AgentConfig to_agent_config(const Config& config) {
    AgentConfig agent;
    agent.model       = effective_model(config);
    agent.max_steps   = config.agent.max_steps;
    agent.sandbox     = SandboxMode::Workspace;
    agent.system_prompt =
        config.agent.system_prompt.empty() ? default_system_prompt() : config.agent.system_prompt;
    if (config.agent.reasoning_effort.has_value()) {
        agent.parameters.reasoning_effort = config.agent.reasoning_effort;
    } else if (config.llm.reasoning_effort.has_value()) {
        agent.parameters.reasoning_effort = config.llm.reasoning_effort;
    }
    return agent;
}

CompactionPolicy to_compaction_policy(const Config& config) {
    const CompactionSettings& settings = config.agent.compaction;
    CompactionPolicy          policy;
    policy.threshold_tokens        = settings.threshold_tokens;
    policy.threshold_ratio         = settings.threshold_ratio;
    policy.context_window_tokens   = settings.context_window_tokens;
    policy.reserve_output_tokens   = settings.reserve_output_tokens;
    policy.keep_recent_turns       = settings.keep_recent_turns;
    policy.min_prefix_messages     = settings.min_prefix_messages;
    policy.max_summary_tokens      = settings.max_summary_tokens;
    policy.max_summary_bytes       = settings.max_summary_bytes;
    policy.summarizer_model        = settings.summarizer_model;
    policy.max_compactions_per_turn = settings.max_compactions_per_turn;
    policy.retry_on_context_length = settings.retry_on_context_length;
    policy.enabled = settings.enabled || policy.threshold_tokens > 0 ||
                     policy.context_window_tokens > 0;
    if (policy.max_summary_bytes > PersistenceConfig{}.max_payload_bytes) {
        throw ConfigError(
            "[agent.compaction].max_summary_bytes must be <= PersistenceConfig::max_payload_bytes");
    }
    return policy;
}

} // namespace ymh

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

SandboxMode parse_sandbox(std::string_view value) {
    if (value == "workspace") {
        return SandboxMode::Workspace;
    }
    if (value == "read-only") {
        return SandboxMode::ReadOnly;
    }
    if (value == "unrestricted") {
        return SandboxMode::Unrestricted;
    }
    throw ConfigError("[agent].sandbox must be one of: workspace, read-only, unrestricted (got '" +
                      std::string{value} + "')");
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

std::string default_plan_section() {
    return "You are in plan mode. Explore the codebase and design a concrete plan before "
           "acting. Every tool remains available, but do not make changes; when the plan is "
           "ready, present it by calling exit_plan_mode. The user may leave plan mode with "
           "/plan off.";
}

LLMProviderConfig to_provider_config(const Config& config) {
    const ResolvedModel  resolved = resolve_model(config);
    const ResolvedEndpoint& endpoint = resolved.endpoint;
    LLMProviderConfig    provider;
    provider.provider     = endpoint.provider;
    provider.base_url     = endpoint.base_url;
    provider.model        = resolved.model_id;
    provider.api_key_env  = endpoint.api_key_env;
    provider.api_key      = endpoint.api_key;
    provider.headers      = endpoint.headers;
    provider.connect_timeout = endpoint.connect_timeout;
    provider.idle_timeout    = endpoint.idle_timeout;
    provider.request_timeout = endpoint.request_timeout;
    provider.retry.max_attempts      = endpoint.retry.max_attempts;
    provider.retry.base_delay        = endpoint.retry.base_delay;
    provider.retry.max_delay         = endpoint.retry.max_delay;
    provider.retry.jitter            = endpoint.retry.jitter;
    provider.retry.honor_retry_after = endpoint.retry.honor_retry_after;
    provider.profile = resolved.profile;
    return provider;
}

PermissionConfig to_permission_config(const Config& config) {
    PermissionConfig permissions;

    const PolicyVerdict            default_verdict =
        parse_verdict(config.permissions.default_verdict, "default");
    const PermissionPresetSettings baseline = deployment_permission_baseline(config);
    const bool master = default_verdict == PolicyVerdict::Allow || baseline.approval == "never";
    permissions.default_verdict = master ? PolicyVerdict::Allow : default_verdict;

    const PolicyVerdict read =
        master ? PolicyVerdict::Allow : parse_verdict(config.permissions.read, "read");
    const PolicyVerdict write =
        master ? PolicyVerdict::Allow : parse_verdict(config.permissions.write, "write");
    const PolicyVerdict shell =
        master ? PolicyVerdict::Allow : parse_verdict(config.permissions.shell, "shell");

    add_rule(permissions, "read_file", read, "default.read_file");
    add_rule(permissions, "grep", read, "default.grep");
    add_rule(permissions, "glob", read, "default.glob");
    add_rule(permissions, "write_file", write, "default.write_file");
    add_rule(permissions, "edit_file", write, "default.edit_file");
    add_rule(permissions, "shell", shell, "default.shell");

    for (const McpServerSettings& server : config.mcp.servers) {
        if (!server.enabled || server.id.empty()) {
            continue;
        }
        ToolDefault fallback;
        fallback.prefix = "mcp." + server.id + ".";
        fallback.verdict = master ? PolicyVerdict::Allow
                                  : parse_verdict(server.default_verdict, "mcp.default_verdict");
        fallback.id = "mcp." + server.id + ".default";
        permissions.tool_defaults.push_back(std::move(fallback));
    }

    for (const PermissionRuleSettings& settings : config.permissions.rules) {
        PolicyRule rule;
        rule.tool    = settings.tool.value_or("");
        rule.command = settings.command.value_or("");
        rule.effect  = parse_verdict(settings.effect, "rules.effect");
        rule.layer   = PolicyRule::Layer::Project;
        rule.id      = settings.id;
        permissions.rules.push_back(std::move(rule));
    }
    return permissions;
}

McpConfig to_mcp_config(const Config& config) {
    McpConfig mcp;
    mcp.enabled = config.mcp.enabled;
    mcp.max_servers = config.mcp.max_servers;
    mcp.max_inflight_calls_per_server = config.mcp.max_inflight_calls_per_server;
    mcp.startup_deadline = std::chrono::milliseconds{config.mcp.startup_deadline_ms};
    mcp.handshake_timeout = std::chrono::milliseconds{config.mcp.handshake_timeout_ms};
    mcp.list_timeout = std::chrono::milliseconds{config.mcp.list_timeout_ms};
    mcp.list_max_pages = config.mcp.list_max_pages;
    mcp.reconnect_max_attempts = config.mcp.reconnect_max_attempts;
    mcp.reconnect_initial_backoff =
        std::chrono::milliseconds{config.mcp.reconnect_initial_backoff_ms};
    mcp.reconnect_max_backoff =
        std::chrono::milliseconds{config.mcp.reconnect_max_backoff_ms};
    mcp.reconnect_jitter = config.mcp.reconnect_jitter;
    mcp.reconnect_stable_window =
        std::chrono::milliseconds{config.mcp.reconnect_stable_window_ms};
    mcp.ping_interval = std::chrono::milliseconds{config.mcp.ping_interval_ms};
    mcp.shutdown_grace = std::chrono::milliseconds{config.mcp.shutdown_grace_ms};
    mcp.max_frame_bytes = config.mcp.max_frame_bytes;
    mcp.allow_network_servers = config.mcp.allow_network_servers;
    mcp.log_child_stderr = config.mcp.log_child_stderr;
    if (mcp.max_frame_bytes > protocol::TransportLimits{}.max_frame_bytes) {
        throw ConfigError(
            "[mcp].max_frame_bytes must be <= protocol::TransportLimits::max_frame_bytes");
    }

    for (const McpServerSettings& settings : config.mcp.servers) {
        McpServerConfig server;
        server.id.value = settings.id;
        server.enabled = settings.enabled;
        server.required = settings.required;
        const std::optional<McpTransportKind> transport =
            parse_mcp_transport(settings.transport);
        if (!transport.has_value()) {
            throw ConfigError("[mcp.server].transport must be one of: stdio, http_sse (got '" +
                              settings.transport + "')");
        }
        server.transport = *transport;
        server.command = settings.command;
        server.args = settings.args;
        server.env = settings.env;
        server.cwd = settings.cwd;
        server.url = settings.url;
        server.header_env = settings.header_env;
        server.protocol_version = settings.protocol_version;
        server.allowed_tools = settings.allowed_tools;
        server.denied_tools = settings.denied_tools;
        server.default_verdict =
            parse_verdict(settings.default_verdict, "mcp.default_verdict");
        server.call_timeout = std::chrono::milliseconds{settings.call_timeout_ms};
        server.max_result_bytes = settings.max_result_bytes;
        mcp.servers.push_back(std::move(server));
    }
    return mcp;
}

SkillCatalogConfig to_skill_catalog_config(const Config& config) {
    SkillCatalogConfig skills;
    skills.enabled               = config.skills.enabled;
    skills.expose_workspace      = config.skills.expose_workspace;
    skills.max_skills            = config.skills.max_skills;
    skills.max_skill_bytes       = config.skills.max_skill_bytes;
    skills.max_description_bytes = config.skills.max_description_bytes;
    skills.max_index_bytes       = config.skills.max_index_bytes;
    skills.max_frontmatter_bytes = config.skills.max_frontmatter_bytes;
    return skills;
}

AgentConfig to_agent_config(const Config& config) {
    const ResolvedModel resolved = resolve_model(config);
    AgentConfig agent;
    agent.provider    = resolved.endpoint.provider;
    agent.model       = resolved.model_id;
    agent.max_steps   = config.agent.max_steps;
    const PermissionPresetSettings effective_sandbox = narrow_permission_preset(
        PermissionPresetSettings{config.agent.sandbox, "ask"},
        deployment_permission_baseline(config));
    agent.sandbox = parse_sandbox(effective_sandbox.sandbox);
    agent.persist_prompt_text = config.session.persist_prompt_text;
    agent.system_prompt =
        config.agent.system_prompt.empty() ? default_system_prompt() : config.agent.system_prompt;
    agent.plan_section =
        config.agent.plan_section.empty() ? default_plan_section() : config.agent.plan_section;

    agent.profile             = resolved.profile;
    const bool has_profile    = !resolved.profile.id.empty();

    if (config.agent.reasoning_effort.has_value()) {
        agent.parameters.reasoning_effort = config.agent.reasoning_effort;
    } else if (resolved.reasoning_effort.has_value()) {
        agent.parameters.reasoning_effort = resolved.reasoning_effort;
    }
    if (resolved.max_tokens.has_value()) {
        agent.parameters.max_output_tokens = resolved.max_tokens;
    }
    if (resolved.temperature.has_value()) {
        agent.parameters.temperature = resolved.temperature;
    } else if (has_profile) {
        agent.parameters.temperature = resolved.profile.temperature;
    }
    if (resolved.top_p.has_value()) {
        agent.parameters.top_p = resolved.top_p;
    } else if (has_profile) {
        agent.parameters.top_p = resolved.profile.top_p;
    }
    if (resolved.top_k.has_value()) {
        agent.parameters.top_k = resolved.top_k;
    } else if (has_profile) {
        agent.parameters.top_k = resolved.profile.top_k;
    }
    if (resolved.tool_choice.has_value()) {
        agent.parameters.tool_choice = resolved.tool_choice;
    }
    if (!resolved.stop.empty()) {
        agent.parameters.stop = resolved.stop;
    }
    if (resolved.seed.has_value()) {
        agent.parameters.seed = resolved.seed;
    }
    return agent;
}

CompactionPolicy to_compaction_policy(const Config& config) {
    const ResolvedModel       resolved = resolve_model(config);
    const CompactionSettings& settings = config.agent.compaction;
    CompactionPolicy          policy;
    policy.provider                = resolved.endpoint.provider;
    policy.threshold_tokens        = settings.threshold_tokens;
    policy.threshold_ratio         = settings.threshold_ratio;
    policy.context_window_tokens   = resolved.context_window.value_or(settings.context_window_tokens);
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

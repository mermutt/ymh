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

std::string default_plan_section() {
    return
        "You are in plan mode. Stay in plan mode until exit_plan_mode succeeds or the user "
        "switches the session mode. Imperative language to implement changes means plan the "
        "implementation, not execute it. A user's conversational agreement \u2014 including an "
        "answer confirming something you asked \u2014 approves nothing and does not end plan mode; "
        "fold the confirmed decision into the plan and submit it through exit_plan_mode.\n\n"
        "Explore first. Use non-mutating reads, searches, static analysis, and checks to ground "
        "the plan in the actual repository. Do not edit or write files, change configuration, "
        "run formatters or code generation that rewrites tracked files, commit, or otherwise "
        "carry out the plan. Prefer existing functions and patterns over new machinery.\n\n"
        "The tool catalog stays the same across modes for request-cache stability. These "
        "plan-mode rules override any later tool description or guidance that suggests using "
        "mutation tools; those tools remain listed to keep the tool catalog unchanged.\n\n"
        "Resolve discoverable facts by inspection. Ask the user only about choices that are "
        "theirs to make or material ambiguity that inspection cannot answer; do not ask where "
        "code lives or how current behavior works when you can find out.\n\n"
        "Make the plan decision-complete: state the goal and success criteria; group "
        "implementation changes by subsystem; identify public API, schema, and data-flow "
        "changes; cover edge cases, failure modes, tests, acceptance criteria, and explicit "
        "assumptions. Keep it concise enough to review but detailed enough that another "
        "engineer can implement it without making design decisions.\n\n"
        "When ready, call exit_plan_mode with the complete plan markdown, starting with a # "
        "title. Make exit_plan_mode the only and final tool call in that assistant response: it "
        "presents the plan for approval, and implementation begins only in a later step after "
        "approval. Do not paste the final plan as a plain reply or ask \"should I proceed?\" "
        "through prose. If review rejects it, incorporate the feedback and present again. If the "
        "review channel is unavailable or aborted, stay in plan mode and ask the user to switch "
        "modes manually; do not proceed with implementation. The user may leave plan mode with "
        "/plan off.";
}

LLMProviderConfig to_provider_config(const ResolvedEndpoint& endpoint,
                                     const ModelProfile&     profile) {
    LLMProviderConfig provider;
    provider.provider     = endpoint.provider;
    provider.base_url     = endpoint.base_url;
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
    provider.profile = profile;
    return provider;
}

LLMProviderConfig to_provider_config(const Config& config) {
    const ResolvedModel resolved = resolve_model(config);
    LLMProviderConfig   provider = to_provider_config(resolved.endpoint, resolved.profile);
    provider.model               = resolved.model_id;
    return provider;
}

PermissionConfig to_permission_config(const Config& config) {
    PermissionConfig permissions;

    const PolicyVerdict            default_verdict =
        parse_verdict(config.permissions.default_verdict, "default");
    const PermissionPresetSettings baseline = deployment_permission_baseline(config);
    // 52-I11/FIX-5: `baseline.approval == "never"` (the `danger-full-access`
    // preset) is a deliberate deployment-level escape hatch, not a session
    // narrowing. `permissions.default_preset`/`permissions.presets` are
    // global-layer only (52-D15), so a workspace/cloned repo cannot reach this
    // switch; the sandbox dimension is still clamped by `effective_sandbox_mode`.
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
    agent.endpoint    = resolved.endpoint.name;
    agent.model_name  = resolved.model_name;
    agent.max_steps   = config.agent.max_steps;
    agent.sandbox     = effective_sandbox_mode(config);
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

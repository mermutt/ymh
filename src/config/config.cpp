#include "ymh/config/config.hpp"

#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <toml++/toml.h>

#include "ymh/core/logger.hpp"

namespace ymh {
namespace {

constexpr std::string_view kGlobalDir = "ymh";
constexpr std::string_view kConfigFile = "config.toml";

[[noreturn]] void fail(const std::filesystem::path& source, std::string_view detail) {
    throw ConfigError("config " + source.string() + ": " + std::string{detail});
}

void reject_unknown(const toml::table& table,
                    std::string_view table_name,
                    std::initializer_list<std::string_view> allowed,
                    const std::filesystem::path& source) {
    for (const auto& [key, node] : table) {
        (void)node;
        const std::string_view name = key.str();
        bool                   known = false;
        for (const std::string_view candidate : allowed) {
            if (candidate == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            fail(source, "unknown key '" + std::string{table_name} + "." + std::string{name} + "'");
        }
    }
}

template <class T>
T read_value(const toml::table& table,
             std::string_view key,
             std::string_view table_name,
             T fallback,
             const std::filesystem::path& source) {
    const auto node = table[key];
    if (!node) {
        return fallback;
    }
    if (const std::optional<T> value = node.value<T>()) {
        return *value;
    }
    fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
}

std::optional<std::string> read_optional_string(const toml::table& table,
                                                std::string_view key,
                                                std::string_view table_name,
                                                const std::filesystem::path& source) {
    const auto node = table[key];
    if (!node) {
        return std::nullopt;
    }
    if (const std::optional<std::string> value = node.value<std::string>()) {
        return *value;
    }
    fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
}

std::vector<std::string> read_string_array(const toml::table& table,
                                           std::string_view key,
                                           std::string_view table_name,
                                           const std::filesystem::path& source) {
    std::vector<std::string> result;
    const auto               node = table[key];
    if (!node) {
        return result;
    }
    const toml::array* array = node.as_array();
    if (array == nullptr) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    for (const toml::node& element : *array) {
        if (const std::optional<std::string> value = element.value<std::string>()) {
            result.push_back(*value);
        } else {
            fail(source, "'" + std::string{table_name} + "." + std::string{key} +
                            "' must be an array of strings");
        }
    }
    return result;
}

void apply_ui(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "ui", {"theme", "show_activity", "side_panel"}, source);
    config.ui.theme = read_value<std::string>(table, "theme", "ui", config.ui.theme, source);
    config.ui.show_activity =
        read_value<bool>(table, "show_activity", "ui", config.ui.show_activity, source);
    config.ui.side_panel =
        read_value<std::string>(table, "side_panel", "ui", config.ui.side_panel, source);
}

void apply_compaction(Config& config,
                      const toml::table& table,
                      const std::filesystem::path& source) {
    reject_unknown(table, "agent.compaction",
                   {"enabled", "threshold_tokens", "threshold_ratio", "context_window_tokens",
                    "reserve_output_tokens", "keep_recent_turns", "min_prefix_messages",
                    "max_summary_tokens", "max_summary_bytes", "summarizer_model",
                    "max_compactions_per_turn", "retry_on_context_length"},
                   source);
    CompactionSettings& compaction = config.agent.compaction;
    compaction.enabled = read_value<bool>(table, "enabled", "agent.compaction",
                                          compaction.enabled, source);
    compaction.threshold_tokens = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "threshold_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.threshold_tokens), source));
    compaction.threshold_ratio = read_value<double>(table, "threshold_ratio",
                                                    "agent.compaction",
                                                    compaction.threshold_ratio, source);
    compaction.context_window_tokens = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "context_window_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.context_window_tokens), source));
    compaction.reserve_output_tokens = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "reserve_output_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.reserve_output_tokens), source));
    compaction.keep_recent_turns = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "keep_recent_turns", "agent.compaction",
        static_cast<std::int64_t>(compaction.keep_recent_turns), source));
    compaction.min_prefix_messages = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "min_prefix_messages", "agent.compaction",
        static_cast<std::int64_t>(compaction.min_prefix_messages), source));
    compaction.max_summary_tokens = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_summary_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.max_summary_tokens), source));
    compaction.max_summary_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_summary_bytes", "agent.compaction",
        static_cast<std::int64_t>(compaction.max_summary_bytes), source));
    compaction.summarizer_model = read_value<std::string>(
        table, "summarizer_model", "agent.compaction", compaction.summarizer_model, source);
    compaction.max_compactions_per_turn = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_compactions_per_turn", "agent.compaction",
        static_cast<std::int64_t>(compaction.max_compactions_per_turn), source));
    compaction.retry_on_context_length = read_value<bool>(
        table, "retry_on_context_length", "agent.compaction",
        compaction.retry_on_context_length, source);
}

void apply_agent(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "agent",
                   {"model", "max_steps", "reasoning_effort", "system_prompt", "compaction",
                    "compaction_threshold_tokens"},
                   source);
    config.agent.model =
        read_value<std::string>(table, "model", "agent", config.agent.model, source);
    config.agent.max_steps =
        static_cast<std::size_t>(read_value<std::int64_t>(
            table, "max_steps", "agent", static_cast<std::int64_t>(config.agent.max_steps), source));
    config.agent.reasoning_effort =
        read_optional_string(table, "reasoning_effort", "agent", source);
    config.agent.system_prompt =
        read_value<std::string>(table, "system_prompt", "agent", config.agent.system_prompt, source);
    config.agent.compaction.threshold_tokens = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "compaction_threshold_tokens", "agent",
        static_cast<std::int64_t>(config.agent.compaction.threshold_tokens), source));
    if (const auto node = table["compaction"]; node) {
        const toml::table* nested = node.as_table();
        if (nested == nullptr) {
            fail(source, "invalid type for 'agent.compaction'");
        }
        apply_compaction(config, *nested, source);
    }
}

void apply_workspace(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "workspace", {"root", "workspace_roots"}, source);
    config.workspace.root =
        read_value<std::string>(table, "root", "workspace", config.workspace.root, source);
    if (const auto roots = read_string_array(table, "workspace_roots", "workspace", source);
        !roots.empty()) {
        config.workspace.workspace_roots = roots;
    }
}

void apply_permissions(Config& config,
                       const toml::table& table,
                       const std::filesystem::path& source) {
    reject_unknown(table, "permissions", {"shell", "write", "read"}, source);
    config.permissions.shell =
        read_value<std::string>(table, "shell", "permissions", config.permissions.shell, source);
    config.permissions.write =
        read_value<std::string>(table, "write", "permissions", config.permissions.write, source);
    config.permissions.read =
        read_value<std::string>(table, "read", "permissions", config.permissions.read, source);
}

void apply_logging(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "logging", {"level", "log_prompts"}, source);
    config.logging.level =
        read_value<std::string>(table, "level", "logging", config.logging.level, source);
    config.logging.log_prompts =
        read_value<bool>(table, "log_prompts", "logging", config.logging.log_prompts, source);
}

void apply_retry(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "llm.default.retry",
                   {"max_attempts", "base_delay_ms", "max_delay_ms", "jitter",
                    "honor_retry_after"},
                   source);
    config.llm.retry.max_attempts = static_cast<std::uint32_t>(read_value<std::int64_t>(
        table, "max_attempts", "llm.default.retry",
        static_cast<std::int64_t>(config.llm.retry.max_attempts), source));
    config.llm.retry.base_delay = std::chrono::milliseconds{read_value<std::int64_t>(
        table, "base_delay_ms", "llm.default.retry", config.llm.retry.base_delay.count(), source)};
    config.llm.retry.max_delay = std::chrono::milliseconds{read_value<std::int64_t>(
        table, "max_delay_ms", "llm.default.retry", config.llm.retry.max_delay.count(), source)};
    config.llm.retry.jitter =
        read_value<double>(table, "jitter", "llm.default.retry", config.llm.retry.jitter, source);
    config.llm.retry.honor_retry_after = read_value<bool>(
        table, "honor_retry_after", "llm.default.retry", config.llm.retry.honor_retry_after, source);
}

void apply_llm(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "llm",
                   {"default", "provider", "base_url", "model", "api_key_env",
                    "reasoning_effort", "max_concurrency", "connect_timeout_ms", "idle_timeout_ms",
                    "request_timeout_ms", "retry"},
                   source);

    const toml::table* section = &table;
    if (const auto node = table["default"]; node) {
        const toml::table* nested = node.as_table();
        if (nested == nullptr) {
            fail(source, "invalid type for 'llm.default'");
        }
        reject_unknown(*nested, "llm.default",
                       {"provider", "base_url", "model", "api_key_env", "reasoning_effort",
                        "max_concurrency", "connect_timeout_ms", "idle_timeout_ms",
                        "request_timeout_ms", "retry"},
                       source);
        section = nested;
    }

    config.llm.provider =
        read_value<std::string>(*section, "provider", "llm.default", config.llm.provider, source);
    config.llm.base_url =
        read_value<std::string>(*section, "base_url", "llm.default", config.llm.base_url, source);
    config.llm.model =
        read_value<std::string>(*section, "model", "llm.default", config.llm.model, source);
    config.llm.api_key_env = read_value<std::string>(*section, "api_key_env", "llm.default",
                                                     config.llm.api_key_env, source);
    config.llm.reasoning_effort =
        read_optional_string(*section, "reasoning_effort", "llm.default", source);
    config.llm.max_concurrency = static_cast<std::size_t>(read_value<std::int64_t>(
        *section, "max_concurrency", "llm.default",
        static_cast<std::int64_t>(config.llm.max_concurrency), source));
    config.llm.connect_timeout = std::chrono::milliseconds{read_value<std::int64_t>(
        *section, "connect_timeout_ms", "llm.default", config.llm.connect_timeout.count(), source)};
    config.llm.idle_timeout = std::chrono::milliseconds{read_value<std::int64_t>(
        *section, "idle_timeout_ms", "llm.default", config.llm.idle_timeout.count(), source)};
    config.llm.request_timeout = std::chrono::milliseconds{read_value<std::int64_t>(
        *section, "request_timeout_ms", "llm.default", config.llm.request_timeout.count(), source)};

    if (const auto node = (*section)["retry"]; node) {
        const toml::table* retry = node.as_table();
        if (retry == nullptr) {
            fail(source, "invalid type for 'llm.default.retry'");
        }
        apply_retry(config, *retry, source);
    }
}

void apply_mcp_server(McpServerSettings& server,
                      const toml::table& table,
                      const std::filesystem::path& source) {
    reject_unknown(table, "mcp.server",
                   {"id", "enabled", "required", "transport", "command", "args", "env",
                    "cwd", "url", "header_env", "protocol_version", "allowed_tools",
                    "denied_tools", "default_verdict", "call_timeout_ms",
                    "max_result_bytes"},
                   source);
    server.id =
        read_value<std::string>(table, "id", "mcp.server", server.id, source);
    server.enabled =
        read_value<bool>(table, "enabled", "mcp.server", server.enabled, source);
    server.required =
        read_value<bool>(table, "required", "mcp.server", server.required, source);
    server.transport = read_value<std::string>(table, "transport", "mcp.server",
                                               server.transport, source);
    server.command =
        read_value<std::string>(table, "command", "mcp.server", server.command, source);
    server.cwd = read_value<std::string>(table, "cwd", "mcp.server", server.cwd, source);
    server.url = read_value<std::string>(table, "url", "mcp.server", server.url, source);
    server.protocol_version = read_value<std::string>(
        table, "protocol_version", "mcp.server", server.protocol_version, source);
    server.default_verdict = read_value<std::string>(
        table, "default_verdict", "mcp.server", server.default_verdict, source);
    server.call_timeout_ms = read_value<std::int64_t>(
        table, "call_timeout_ms", "mcp.server", server.call_timeout_ms, source);
    server.max_result_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_result_bytes", "mcp.server",
        static_cast<std::int64_t>(server.max_result_bytes), source));

    if (auto values = read_string_array(table, "args", "mcp.server", source);
        !values.empty()) {
        server.args = std::move(values);
    }
    if (auto values = read_string_array(table, "env", "mcp.server", source);
        !values.empty()) {
        server.env = std::move(values);
    }
    if (auto values = read_string_array(table, "header_env", "mcp.server", source);
        !values.empty()) {
        server.header_env = std::move(values);
    }
    if (auto values = read_string_array(table, "allowed_tools", "mcp.server", source);
        !values.empty()) {
        server.allowed_tools = std::move(values);
    }
    if (auto values = read_string_array(table, "denied_tools", "mcp.server", source);
        !values.empty()) {
        server.denied_tools = std::move(values);
    }
}

void apply_mcp(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "mcp",
                   {"enabled", "max_servers", "max_inflight_calls_per_server",
                    "startup_deadline_ms", "handshake_timeout_ms", "list_timeout_ms",
                    "list_max_pages", "reconnect_max_attempts",
                    "reconnect_initial_backoff_ms", "reconnect_max_backoff_ms",
                    "reconnect_jitter", "reconnect_stable_window_ms", "ping_interval_ms",
                    "shutdown_grace_ms", "max_frame_bytes", "allow_network_servers",
                    "server"},
                   source);
    McpSettings& mcp = config.mcp;
    mcp.enabled = read_value<bool>(table, "enabled", "mcp", mcp.enabled, source);
    mcp.max_servers = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_servers", "mcp", static_cast<std::int64_t>(mcp.max_servers), source));
    mcp.max_inflight_calls_per_server = static_cast<std::size_t>(
        read_value<std::int64_t>(table, "max_inflight_calls_per_server", "mcp",
                                 static_cast<std::int64_t>(mcp.max_inflight_calls_per_server),
                                 source));
    mcp.startup_deadline_ms = read_value<std::int64_t>(
        table, "startup_deadline_ms", "mcp", mcp.startup_deadline_ms, source);
    mcp.handshake_timeout_ms = read_value<std::int64_t>(
        table, "handshake_timeout_ms", "mcp", mcp.handshake_timeout_ms, source);
    mcp.list_timeout_ms = read_value<std::int64_t>(table, "list_timeout_ms", "mcp",
                                                   mcp.list_timeout_ms, source);
    mcp.list_max_pages = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "list_max_pages", "mcp", static_cast<std::int64_t>(mcp.list_max_pages),
        source));
    mcp.reconnect_max_attempts = static_cast<std::uint32_t>(read_value<std::int64_t>(
        table, "reconnect_max_attempts", "mcp",
        static_cast<std::int64_t>(mcp.reconnect_max_attempts), source));
    mcp.reconnect_initial_backoff_ms = read_value<std::int64_t>(
        table, "reconnect_initial_backoff_ms", "mcp", mcp.reconnect_initial_backoff_ms,
        source);
    mcp.reconnect_max_backoff_ms = read_value<std::int64_t>(
        table, "reconnect_max_backoff_ms", "mcp", mcp.reconnect_max_backoff_ms, source);
    mcp.reconnect_jitter = read_value<double>(table, "reconnect_jitter", "mcp",
                                              mcp.reconnect_jitter, source);
    mcp.reconnect_stable_window_ms = read_value<std::int64_t>(
        table, "reconnect_stable_window_ms", "mcp", mcp.reconnect_stable_window_ms, source);
    mcp.ping_interval_ms = read_value<std::int64_t>(table, "ping_interval_ms", "mcp",
                                                    mcp.ping_interval_ms, source);
    mcp.shutdown_grace_ms = read_value<std::int64_t>(table, "shutdown_grace_ms", "mcp",
                                                     mcp.shutdown_grace_ms, source);
    mcp.max_frame_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_frame_bytes", "mcp", static_cast<std::int64_t>(mcp.max_frame_bytes),
        source));
    mcp.allow_network_servers = read_value<bool>(table, "allow_network_servers", "mcp",
                                                 mcp.allow_network_servers, source);

    if (const auto node = table["server"]; node) {
        const toml::array* entries = node.as_array();
        if (entries == nullptr) {
            fail(source, "invalid type for 'mcp.server'");
        }
        mcp.servers.clear();
        for (const toml::node& entry : *entries) {
            const toml::table* server_table = entry.as_table();
            if (server_table == nullptr) {
                fail(source, "invalid type for an 'mcp.server' entry");
            }
            McpServerSettings server;
            apply_mcp_server(server, *server_table, source);
            mcp.servers.push_back(std::move(server));
        }
    }
}

void apply_skills(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "skills",
                   {"enabled", "expose_workspace", "max_skills", "max_skill_bytes",
                    "max_description_bytes", "max_index_bytes", "max_frontmatter_bytes"},
                   source);
    SkillsSettings& skills = config.skills;
    skills.enabled = read_value<bool>(table, "enabled", "skills", skills.enabled, source);
    skills.expose_workspace = read_value<bool>(table, "expose_workspace", "skills",
                                               skills.expose_workspace, source);
    skills.max_skills = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_skills", "skills", static_cast<std::int64_t>(skills.max_skills), source));
    skills.max_skill_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_skill_bytes", "skills", static_cast<std::int64_t>(skills.max_skill_bytes),
        source));
    skills.max_description_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_description_bytes", "skills",
        static_cast<std::int64_t>(skills.max_description_bytes), source));
    skills.max_index_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_index_bytes", "skills", static_cast<std::int64_t>(skills.max_index_bytes),
        source));
    skills.max_frontmatter_bytes = static_cast<std::size_t>(read_value<std::int64_t>(
        table, "max_frontmatter_bytes", "skills",
        static_cast<std::int64_t>(skills.max_frontmatter_bytes), source));
}

void apply_document(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "",
                   {"ui", "agent", "workspace", "permissions", "logging", "llm", "mcp", "skills"},
                   source);

    const auto section = [&](std::string_view name) -> const toml::table* {
        const auto node = table[name];
        if (!node) {
            return nullptr;
        }
        const toml::table* nested = node.as_table();
        if (nested == nullptr) {
            fail(source, "invalid type for '" + std::string{name} + "'");
        }
        return nested;
    };

    if (const toml::table* ui = section("ui"); ui != nullptr) {
        apply_ui(config, *ui, source);
    }
    if (const toml::table* agent = section("agent"); agent != nullptr) {
        apply_agent(config, *agent, source);
    }
    if (const toml::table* workspace = section("workspace"); workspace != nullptr) {
        apply_workspace(config, *workspace, source);
    }
    if (const toml::table* permissions = section("permissions"); permissions != nullptr) {
        apply_permissions(config, *permissions, source);
    }
    if (const toml::table* logging = section("logging"); logging != nullptr) {
        apply_logging(config, *logging, source);
    }
    if (const toml::table* llm = section("llm"); llm != nullptr) {
        apply_llm(config, *llm, source);
    }
    if (const toml::table* mcp = section("mcp"); mcp != nullptr) {
        apply_mcp(config, *mcp, source);
    }
    if (const toml::table* skills = section("skills"); skills != nullptr) {
        apply_skills(config, *skills, source);
    }
}

std::size_t parse_size(std::string_view name, std::string_view value) {
    try {
        std::size_t consumed = 0;
        const auto  parsed   = std::stoll(std::string{value}, &consumed);
        if (consumed != value.size() || parsed < 0) {
            throw std::invalid_argument("negative or trailing characters");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw ConfigError("environment variable " + std::string{name} + " must be a non-negative integer");
    }
}

bool truthy(std::string_view value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

// Every key below is a real key accepted by the loader (unknown keys are
// rejected). Optional keys whose default is "absent" are commented out so the
// generated file loads to exactly the built-in defaults.
constexpr std::string_view kDefaultConfigToml =
    R"TOML(# ymh configuration — created on first run.
#
# Layering (last writer wins):
#   built-in defaults -> this global file -> <workspace>/.ymh/config.toml
#   -> YMH_* environment variables -> command-line flags.
#
# The loader is strict: an unknown key is an error, so add only real keys.
# Secrets are never stored here; `api_key_env` names the environment variable
# that holds the key.

[ui]
theme = "default"           # color theme name
show_activity = true        # show the activity indicator
side_panel = "auto"         # "auto" | "always" | "never"

[agent]
model = ""                  # empty => use llm.default.model
max_steps = 100             # max tool-calling steps per task
# reasoning_effort = "low"  # optional: "low" | "medium" | "high"
# system_prompt = ""        # empty => built-in default system prompt

[workspace]
root = "."                  # workspace root (relative to the process cwd)
# workspace_roots = []      # discovery roots for workspace selection

[permissions]
shell = "ask"               # "allow" | "ask" | "deny"
write = "ask"
read = "allow"

[logging]
level = "info"              # "debug" | "info" | "warn" | "error"
log_prompts = false         # never enable implicitly; prompt bodies are redacted

[llm.default]
provider = "openai-compatible"
base_url = "https://api.deepseek.com/v1"
model = "deepseek-flash"
api_key_env = "DEEPSEEK_API_KEY"
# reasoning_effort = "low"  # optional: "low" | "medium" | "high"
max_concurrency = 4
connect_timeout_ms = 10000
idle_timeout_ms = 60000
request_timeout_ms = 120000

[llm.default.retry]
max_attempts = 3
base_delay_ms = 500
max_delay_ms = 30000
jitter = 0.25
honor_retry_after = true
)TOML";

void scaffold_warn(Logger* logger, std::string_view message) {
    if (logger != nullptr) {
        logger->warn(message);
    }
}

bool ensure_directory(const std::filesystem::path& dir, Logger* logger, bool& created) {
    created = false;
    if (dir.empty()) {
        return true;
    }
    std::error_code error;
    if (std::filesystem::exists(dir, error)) {
        if (error) {
            scaffold_warn(logger,
                          "config: cannot inspect '" + dir.string() + "': " + error.message());
            return false;
        }
        return true;
    }
    created = std::filesystem::create_directories(dir, error);
    if (error) {
        scaffold_warn(logger,
                      "config: cannot create directory '" + dir.string() + "': " + error.message());
        return false;
    }
    return true;
}

bool write_default_config(const std::filesystem::path& file, Logger* logger, bool& created) {
    created = false;
    std::error_code error;
    if (std::filesystem::exists(file, error)) {
        if (error) {
            scaffold_warn(logger,
                          "config: cannot inspect '" + file.string() + "': " + error.message());
            return false;
        }
        return true;
    }
    std::ofstream output{file, std::ios::binary | std::ios::trunc};
    if (!output) {
        scaffold_warn(logger, "config: cannot write '" + file.string() + "'");
        return false;
    }
    output << kDefaultConfigToml;
    output.flush();
    if (!output) {
        scaffold_warn(logger, "config: cannot write '" + file.string() + "'");
        return false;
    }
    created = true;
    return true;
}

} // namespace

std::optional<std::string> env_value(std::string_view name) {
    const char* raw = std::getenv(std::string{name}.c_str());
    if (raw == nullptr || *raw == '\0') {
        return std::nullopt;
    }
    return std::string{raw};
}

std::filesystem::path default_global_config_path() {
    if (const std::optional<std::string> xdg = env_value("XDG_CONFIG_HOME"); xdg.has_value()) {
        return std::filesystem::path{*xdg} / kGlobalDir / kConfigFile;
    }
    if (const std::optional<std::string> home = env_value("HOME"); home.has_value()) {
        return std::filesystem::path{*home} / ".config" / kGlobalDir / kConfigFile;
    }
    return std::filesystem::path{".config"} / kGlobalDir / kConfigFile;
}

std::filesystem::path workspace_config_path(const std::filesystem::path& workspace_root) {
    return workspace_root / ".ymh" / kConfigFile;
}

ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                               const std::filesystem::path& global_config,
                               Logger* logger) {
    ScaffoldResult result;
    result.global_config = global_config;
    result.workspace_dir = workspace_root / ".ymh";

    if (ensure_directory(global_config.parent_path(), logger, result.global_dir_created)) {
        if (!write_default_config(global_config, logger, result.global_config_created)) {
            result.ok = false;
        }
    } else {
        result.ok = false;
    }

    if (!ensure_directory(result.workspace_dir, logger, result.workspace_dir_created)) {
        result.ok = false;
    }

    return result;
}

ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root, Logger* logger) {
    return scaffold_config(workspace_root, default_global_config_path(), logger);
}

void apply_toml_file(Config& config, const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return;
    }

    toml::table table;
    try {
        table = toml::parse_file(path.string());
    } catch (const toml::parse_error& parse_error) {
        std::ostringstream message;
        message << parse_error.description() << " (line " << parse_error.source().begin.line << ")";
        fail(path, message.str());
    } catch (const std::exception& other) {
        fail(path, other.what());
    }

    apply_document(config, table, path);
}

void apply_env_overrides(Config& config) {
    if (const auto value = env_value("YMH_LLM_PROVIDER")) {
        config.llm.provider = *value;
    }
    if (const auto value = env_value("YMH_LLM_BASE_URL")) {
        config.llm.base_url = *value;
    }
    if (const auto value = env_value("YMH_LLM_MODEL")) {
        config.llm.model = *value;
    }
    if (const auto value = env_value("YMH_API_KEY_ENV")) {
        config.llm.api_key_env = *value;
    }
    if (const auto value = env_value("YMH_REASONING_EFFORT")) {
        config.llm.reasoning_effort = *value;
    }
    if (const auto value = env_value("YMH_LLM_MAX_CONCURRENCY")) {
        config.llm.max_concurrency = parse_size("YMH_LLM_MAX_CONCURRENCY", *value);
    }
    if (const auto value = env_value("YMH_LLM_CONNECT_TIMEOUT_MS")) {
        config.llm.connect_timeout =
            std::chrono::milliseconds{static_cast<std::int64_t>(
                parse_size("YMH_LLM_CONNECT_TIMEOUT_MS", *value))};
    }
    if (const auto value = env_value("YMH_LLM_IDLE_TIMEOUT_MS")) {
        config.llm.idle_timeout = std::chrono::milliseconds{
            static_cast<std::int64_t>(parse_size("YMH_LLM_IDLE_TIMEOUT_MS", *value))};
    }
    if (const auto value = env_value("YMH_LLM_REQUEST_TIMEOUT_MS")) {
        config.llm.request_timeout = std::chrono::milliseconds{
            static_cast<std::int64_t>(parse_size("YMH_LLM_REQUEST_TIMEOUT_MS", *value))};
    }
    if (const auto value = env_value("YMH_AGENT_MODEL")) {
        config.agent.model = *value;
    }
    if (const auto value = env_value("YMH_AGENT_MAX_STEPS")) {
        config.agent.max_steps = parse_size("YMH_AGENT_MAX_STEPS", *value);
    }
    if (const auto value = env_value("YMH_AGENT_SYSTEM_PROMPT")) {
        config.agent.system_prompt = *value;
    }
    if (const auto value = env_value("YMH_LOG_LEVEL")) {
        config.logging.level = *value;
    }
    if (const auto value = env_value("YMH_LLM_LOG_PROMPTS")) {
        config.logging.log_prompts = truthy(*value);
    }
    if (const auto value = env_value("YMH_SKILLS_ENABLED")) {
        config.skills.enabled = truthy(*value);
    }
    if (const auto value = env_value("YMH_SKILLS_EXPOSE_WORKSPACE")) {
        config.skills.expose_workspace = truthy(*value);
    }
    if (const auto value = env_value("YMH_SKILLS_MAX_SKILLS")) {
        config.skills.max_skills = parse_size("YMH_SKILLS_MAX_SKILLS", *value);
    }
    if (const auto value = env_value("YMH_SKILLS_MAX_SKILL_BYTES")) {
        config.skills.max_skill_bytes = parse_size("YMH_SKILLS_MAX_SKILL_BYTES", *value);
    }
    if (const auto value = env_value("YMH_SKILLS_MAX_DESCRIPTION_BYTES")) {
        config.skills.max_description_bytes =
            parse_size("YMH_SKILLS_MAX_DESCRIPTION_BYTES", *value);
    }
    if (const auto value = env_value("YMH_SKILLS_MAX_INDEX_BYTES")) {
        config.skills.max_index_bytes = parse_size("YMH_SKILLS_MAX_INDEX_BYTES", *value);
    }
    if (const auto value = env_value("YMH_SKILLS_MAX_FRONTMATTER_BYTES")) {
        config.skills.max_frontmatter_bytes =
            parse_size("YMH_SKILLS_MAX_FRONTMATTER_BYTES", *value);
    }
}

Config load_config(const ConfigPaths& paths) {
    Config config;
    apply_toml_file(config, paths.global);
    apply_toml_file(config, paths.workspace);
    apply_env_overrides(config);
    return config;
}

Config load_config(const std::filesystem::path& workspace_root) {
    ConfigPaths paths;
    paths.global    = default_global_config_path();
    paths.workspace = workspace_config_path(workspace_root);
    return load_config(paths);
}

std::string effective_model(const Config& config) {
    if (!config.agent.model.empty()) {
        return config.agent.model;
    }
    return config.llm.model;
}

} // namespace ymh

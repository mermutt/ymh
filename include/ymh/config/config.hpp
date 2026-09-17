#pragma once

// Configuration (00-architecture.md §37, §48; 08-llm-provider.md §5.3).
//
// Layered, last-writer-wins:
//
//   built-in defaults
//         -> global config    (~/.config/ymh/config.toml)
//         -> project config   (<workspace>/.ymh/config.toml)
//         -> environment       (YMH_* variables)
//         -> command-line      (applied by the CLI layer)
//
// The loader is strict: an unknown key in a TOML file is a `ConfigError`, so a
// typo can never be silently ignored (08 §5.3). Secrets are never stored in
// `Config`; `llm.api_key_env` names the environment variable that holds the
// secret, and the value is read per request by the provider (08 §6.4).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {

// Thrown on a malformed/unreadable config file or an unknown key.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// [ui] — consumed by the TUI wave; parsed here so the schema is complete.
struct UiConfig {
    std::string theme = "default";
    bool        show_activity = true;
    std::string side_panel = "auto";
};

// [agent]
struct CompactionSettings {
    bool        enabled = false;
    std::size_t threshold_tokens = 0;
    double      threshold_ratio = 0.80;
    std::size_t context_window_tokens = 0;
    std::size_t reserve_output_tokens = 4'096;
    std::size_t keep_recent_turns = 2;
    std::size_t min_prefix_messages = 4;
    std::size_t max_summary_tokens = 1'024;
    std::size_t max_summary_bytes = 256u * 1024u;
    std::string summarizer_model;
    std::size_t max_compactions_per_turn = 1;
    bool        retry_on_context_length = true;
};

struct AgentDefaults {
    std::string                model;   // may be empty; resolved with llm.model
    std::size_t                max_steps = 100;
    std::optional<std::string> reasoning_effort;  // "low" | "medium" | "high"
    std::string                system_prompt;     // empty => built-in default
    CompactionSettings         compaction;
};

// [workspace]
struct WorkspaceSettings {
    std::string              root = ".";
    std::vector<std::string> workspace_roots;  // first-run discovery roots (§9.10)
};

// [permissions] — mapped onto `PermissionConfig` by the CLI wiring.
struct PermissionDefaults {
    std::string shell = "ask";
    std::string write = "ask";
    std::string read = "allow";
};

// [logging] — §40. `log_prompts` is off by default and must never be enabled
// implicitly; prompt bodies are redacted even when it is on.
struct LoggingSettings {
    std::string level = "info";
    bool        log_prompts = false;
};

struct RetrySettings {
    std::uint32_t             max_attempts = 3;
    std::chrono::milliseconds base_delay{500};
    std::chrono::milliseconds max_delay{30'000};
    double                    jitter = 0.25;
    bool                      honor_retry_after = true;
};

// [llm.default] — mapped onto `LLMProviderConfig` by the CLI wiring.
struct LlmSettings {
    std::string                provider = "openai-compatible";
    std::string                base_url = "https://api.deepseek.com/v1";
    std::string                model = "deepseek-flash";
    std::string                api_key_env = "DEEPSEEK_API_KEY";
    std::optional<std::string> reasoning_effort;
    std::size_t                max_concurrency = 4;
    std::chrono::milliseconds  connect_timeout{10'000};
    std::chrono::milliseconds  idle_timeout{60'000};
    std::chrono::milliseconds  request_timeout{120'000};
    RetrySettings              retry;
};

// [[mcp.server]] — one configured MCP server (15 §4.1/§5.6). Mapped onto
// `McpServerConfig` by the CLI wiring; secrets are env references only.
struct McpServerSettings {
    std::string              id;
    bool                     enabled = true;
    bool                     required = false;
    std::string              transport = "stdio";
    std::string              command;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::string              cwd;
    std::string              url;
    std::vector<std::string> header_env;
    std::string              protocol_version;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;
    std::string              default_verdict = "ask";
    std::int64_t             call_timeout_ms = 60'000;
    std::size_t              max_result_bytes = 1u * 1024u * 1024u;
};

// [mcp] — the layered section (15 §5.6). Arrays replace wholesale per layer.
struct McpSettings {
    bool                         enabled = true;
    std::vector<McpServerSettings> servers;
    std::size_t                  max_servers = 8;
    std::size_t                  max_inflight_calls_per_server = 4;
    std::int64_t                 startup_deadline_ms = 5'000;
    std::int64_t                 handshake_timeout_ms = 10'000;
    std::int64_t                 list_timeout_ms = 5'000;
    std::size_t                  list_max_pages = 64;
    std::uint32_t                reconnect_max_attempts = 5;
    std::int64_t                 reconnect_initial_backoff_ms = 500;
    std::int64_t                 reconnect_max_backoff_ms = 30'000;
    double                       reconnect_jitter = 0.25;
    std::int64_t                 reconnect_stable_window_ms = 30'000;
    std::int64_t                 ping_interval_ms = 15'000;
    std::int64_t                 shutdown_grace_ms = 2'000;
    std::size_t                  max_frame_bytes = 8u * 1024u * 1024u;
    bool                         allow_network_servers = false;
};

// [skills] — the skill subsystem (20 §5.6). Additive.
struct SkillsSettings {
    bool        enabled = true;
    bool        expose_workspace = false;  // DANGER: exposes repo-authored skills
    std::size_t max_skills = 256;
    std::size_t max_skill_bytes = 64u * 1024u;
    std::size_t max_description_bytes = 512;
    std::size_t max_index_bytes = 8u * 1024u;
    std::size_t max_frontmatter_bytes = 4u * 1024u;
};

struct Config {
    UiConfig           ui;
    AgentDefaults      agent;
    WorkspaceSettings  workspace;
    PermissionDefaults permissions;
    LoggingSettings    logging;
    LlmSettings        llm;
    McpSettings        mcp;
    SkillsSettings     skills;
};

// Explicit layer sources. `global`/`workspace` may be empty to skip a layer.
struct ConfigPaths {
    std::filesystem::path global;
    std::filesystem::path workspace;
};

// `$XDG_CONFIG_HOME/ymh/config.toml`, else `$HOME/.config/ymh/config.toml`.
[[nodiscard]] std::filesystem::path default_global_config_path();

// `<root>/.ymh/config.toml`.
[[nodiscard]] std::filesystem::path workspace_config_path(
    const std::filesystem::path& workspace_root);

// `ymh::Logger` (core/logging.hpp); only the pointer is used here so the config
// component stays free of a logging-library dependency.
class Logger;

// What `scaffold_config` did. All flags are false when the target already
// existed or the step failed; `ok` is false if any best-effort step failed.
struct ScaffoldResult {
    std::filesystem::path global_config;
    std::filesystem::path workspace_dir;
    bool global_dir_created = false;
    bool global_config_created = false;
    bool workspace_dir_created = false;
    bool ok = true;
};

// First-run scaffolding, best-effort and never throwing. Creates the global
// config directory, writes a commented default `config.toml` there if (and only
// if) it does not already exist, and ensures `<workspace_root>/.ymh/` exists.
// An existing config file is never overwritten. Failures are reported to
// `logger` (may be null) and reflected in `ScaffoldResult::ok`.
[[nodiscard]] ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                                             const std::filesystem::path& global_config,
                                             Logger* logger = nullptr);

// Convenience overload resolving the global config path with
// `default_global_config_path()`.
[[nodiscard]] ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                                             Logger* logger = nullptr);

// Loads defaults, then global, then project, then environment overrides.
[[nodiscard]] Config load_config(const ConfigPaths& paths);

// Convenience overload using `default_global_config_path()`.
[[nodiscard]] Config load_config(const std::filesystem::path& workspace_root);

// Merges one TOML file over `config`. A missing file is a no-op; a parse
// failure or unknown key throws `ConfigError`.
void apply_toml_file(Config& config, const std::filesystem::path& path);

// Merges `YMH_*` environment variables over `config`. Unset variables are a
// no-op; a malformed value (e.g. non-numeric timeout) throws `ConfigError`.
void apply_env_overrides(Config& config);

// The effective model: `agent.model` if set, else `llm.model`.
[[nodiscard]] std::string effective_model(const Config& config);

[[nodiscard]] std::optional<std::string> env_value(std::string_view name);

} // namespace ymh

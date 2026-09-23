#pragma once

// Configuration (00-architecture.md §37, §48; 08-llm-provider.md §5.3;
// 21-config-jsonc-errata.md).
//
// Layered, last-writer-wins:
//
//   built-in defaults
//         -> global config    (~/.config/ymh/config.jsonc)
//         -> project config   (<workspace>/.ymh/config.jsonc)
//         -> environment       (YMH_* variables)
//         -> command-line      (applied by the CLI layer)
//
// The loader is strict: an unknown key in a JSONC file is a `ConfigError`, so a
// typo can never be silently ignored (08 §5.3). `llm.api_key` (46-D12.2) is the
// one permitted literal secret; it is accepted only in the global config layer,
// the file is `0600`, and the value is never logged, rendered, or event-logged.
// All other credentials stay environment references: `llm.api_key_env` names the
// environment variable that holds the secret, read per request by the provider
// (08 §6.4).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/jobs/job_types.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/prompt/instructions.hpp"
#include "ymh/tools/presentation.hpp"

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
    std::optional<std::string> reasoning_effort;  // "low" | "medium" | "high" | "xhigh"
    std::string                system_prompt;     // empty => built-in default
    std::string                plan_section;      // empty => built-in default (25-D3)
    CompactionSettings         compaction;
    // 52-D15/52-I11: the deployment sandbox baseline, replacing the hardcode in
    // `to_agent_config`. One of "workspace"|"read-only"|"unrestricted"; a
    // permission preset may only narrow it.
    std::string                sandbox = "workspace";
};

// [workspace]
struct WorkspaceSettings {
    std::string              root = ".";
    std::vector<std::string> workspace_roots;  // first-run discovery roots (§9.10)
};

// 46-D1: one `permissions.rules` entry. Rev 5 / O-H1: no `path` field — the
// shipped loop never populates `PermissionRequest::path`, so a path rule would
// be dead code. `effect` is required; `id` defaults to `config.rule.<n>`.
struct PermissionRuleSettings {
    std::optional<std::string> tool;
    std::optional<std::string> command;
    std::string                effect;
    std::string                id;
};

// 52-D15: one named permission preset. `sandbox` is one of
// "workspace"|"read-only"|"unrestricted"; `approval` is "ask"|"never".
struct PermissionPresetSettings {
    std::string sandbox  = "workspace";
    std::string approval = "ask";
};

// 52-D15: the two shipped permission preset names.
inline constexpr std::string_view kWorkspaceWritePreset   = "workspace-write";
inline constexpr std::string_view kDangerFullAccessPreset = "danger-full-access";

// 52-D15: the built-in `permissions.presets` seed, so the two shipped names
// always resolve even when the deployment config does not restate them.
[[nodiscard]] inline std::map<std::string, PermissionPresetSettings, std::less<>>
builtin_permission_presets() {
    std::map<std::string, PermissionPresetSettings, std::less<>> presets;
    presets[std::string{kWorkspaceWritePreset}]   = PermissionPresetSettings{};
    presets[std::string{kDangerFullAccessPreset}] = PermissionPresetSettings{"unrestricted", "never"};
    return presets;
}

// 52-I11: sandbox/approval ordering for the narrowing check. `read-only` is the
// narrowest, `unrestricted` the widest; `ask` is narrower than `never`.
[[nodiscard]] int sandbox_rank(std::string_view sandbox);
[[nodiscard]] int approval_rank(std::string_view approval);

// 52-I11: true when `candidate` is no wider than `baseline` in either dimension.
[[nodiscard]] bool permission_preset_narrows(const PermissionPresetSettings& baseline,
                                             const PermissionPresetSettings& candidate);

// 52-I11: the per-dimension minimum of the two bindings. A preset can only
// narrow the deployment; a wider candidate is clamped, never applied.
[[nodiscard]] PermissionPresetSettings narrow_permission_preset(
    const PermissionPresetSettings& baseline, const PermissionPresetSettings& candidate);

// [permissions] — mapped onto `PermissionConfig` by the CLI wiring.
struct PermissionDefaults {
    std::string shell = "ask";
    std::string write = "ask";
    std::string read = "allow";
    // 46-D1: the master switch; `"allow"` upgrades every builtin/MCP rule and
    // the fallback verdict to Allow (never bypassing a hard deny or force_ask).
    std::string default_verdict = "ask";
    // 46-D1: the general rule list; becomes Layer::Project rules.
    std::vector<PermissionRuleSettings> rules;
    // 52-D15: named permission presets, seeded with the two shipped names.
    std::map<std::string, PermissionPresetSettings, std::less<>> presets =
        builtin_permission_presets();
    // 52-D15: names a `permissions.presets` entry; empty => built-in
    // `workspace-write`. An unknown name is a `ConfigError` (52-F17).
    std::string default_preset;
};

// 52-D15: session preset's `permission_preset` > `permissions.default_preset` >
// built-in `workspace-write`. An unknown named preset is a `ConfigError`
// (52-F17).
[[nodiscard]] PermissionPresetSettings resolve_permission_preset(
    const PermissionDefaults& permissions, std::optional<std::string> preset_name);

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

// [llm.endpoints.<name>] (52-D1). One named connection. The absent fields of an
// entry use the built-in endpoint defaults: `provider = "openai-compatible"` and
// `base_url = "https://api.deepseek.com/v1"` (52 §3.1). `api_key_env` has no
// built-in default: a named endpoint is keyless unless it sets one, so a
// DeepSeek credential is never sent to an unrelated endpoint (52-I4).
struct EndpointSettings {
    std::string  provider = "openai-compatible";
    std::string  base_url = "https://api.deepseek.com/v1";
    std::string  api_key_env;
    // 52-D6: global layer only; 0600 file; never logged or rendered.
    std::optional<std::string> api_key;
    // 52-D6/52-I4: a secret side-channel (global layer only, values redacted).
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t                max_concurrency = 4;
    std::chrono::milliseconds  connect_timeout{10'000};
    std::chrono::milliseconds  idle_timeout{60'000};
    std::chrono::milliseconds  request_timeout{120'000};
    RetrySettings              retry;
};

// [llm.models.<name>] (52-D1). One named generation target bound to an
// endpoint by name. `endpoint` and `model` are required and non-empty (52-I2,
// 52-F19).
struct ModelSettings {
    std::string                  endpoint;   // required; names an endpoint
    std::string                  model;      // required; wire id
    std::string                  profile;    // model-profile id; "" = inert
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::uint32_t> context_window;
    std::optional<std::string>   reasoning_effort;
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;
    std::optional<std::string>   tool_choice;
    std::vector<std::string>     stop;
    std::optional<std::uint32_t> seed;
};

// [llm.default] — mapped onto `LLMProviderConfig` by the CLI wiring.
struct LlmSettings {
    std::string                provider = "openai-compatible";
    std::string                base_url = "https://api.deepseek.com/v1";
    std::string                model = "deepseek-flash";
    std::string                api_key_env = "DEEPSEEK_API_KEY";
    // 46-D12.2: the one permitted literal secret; accepted only in the global
    // config layer (a workspace-layer `api_key` is a ConfigError). The file must
    // be 0600; the value is never logged, rendered, or event-logged. A literal
    // key outranks `api_key_env`.
    std::optional<std::string>   api_key;
    // 46-D12.4: imported from the localcode profile; reaches the wire through
    // `to_agent_config` -> `GenerationParameters::max_output_tokens`.
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::string> reasoning_effort;
    // 47-D1/47-D7: the model-profile id ("" = inert) and the sampling /
    // tool-choice surface mapped onto `GenerationParameters`.
    std::string                  profile;
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;
    std::optional<std::string>   tool_choice;
    std::vector<std::string>     stop;
    std::optional<std::uint32_t> seed;
    std::size_t                max_concurrency = 4;
    std::chrono::milliseconds  connect_timeout{10'000};
    std::chrono::milliseconds  idle_timeout{60'000};
    std::chrono::milliseconds  request_timeout{120'000};
    RetrySettings              retry;
    // 52-D1: the named maps. Entry names are a free namespace; every key inside
    // an entry is schema-checked (52-I6).
    std::map<std::string, EndpointSettings, std::less<>> endpoints;
    std::map<std::string, ModelSettings, std::less<>>    models;
    // 52-D1: names an `llm.models` entry; an unknown name is a ConfigError.
    std::optional<std::string> active_model;
    // 52 §3.3: the endpoint selector (`YMH_LLM_ENDPOINT`/`--endpoint`) used only
    // on the literal-id path. Not a config-file key; set by env/CLI.
    std::optional<std::string> active_endpoint;
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
    // 50-D3.5: opt-in per-server raw child-stderr capture to
    // <workspace>/.ymh/mcp/<server>.stderr.log (0600). Global-layer only
    // (the whole `mcp` section is; 50-I22).
    bool                         log_child_stderr = false;
};

// [session] — durable session-DB options (26-D23, 28 §5.4). Global layer only.
struct SessionSettings {
    // Store the full rendered system prompt in `llm/request_header` instead of
    // only its digest. Off by default and distinct from `logging.log_prompts`.
    bool persist_prompt_text = false;
};

// [prompt] — the prompt registry (36 §5). Additive.
struct PromptSettings {
    // 52 review (3B): on by default, matching dsh's `standard` preset, which
    // mounts `@deepseek-ai/dsh-agent-instructions` with `maxBytes: 65536`.
    bool                  instructions_enabled = true;
    InstructionFileConfig instructions;
};

// [tools] — the tool registry presentation (36 §5). Additive.
struct ToolsSettings {
    ToolPresentationMode     presentation = ToolPresentationMode::Native;
    std::vector<std::string> tool_order;
    // 46-D8: the generic per-tool-run deadline in milliseconds; 0 disables.
    std::int64_t             timeout_ms = 300'000;
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

// [presets] — agent presets (42 §2.5, 43 §3). Additive.
struct PresetsSettings {
    std::optional<std::filesystem::path> root;
    std::optional<std::string>           default_id;
    bool                                 include_shipped_root = true;
    bool                                 include_user_root    = true;
    std::uint32_t                        max_depth = 3;
};

// [goals] — the goal domain (44 §10.2, 26 §4.9). Additive.
struct GoalsSettings {
    std::uint32_t max_rounds = 256;
    std::uint32_t blocked_after_consecutive_rounds = 3;
};

// [jobs] — the owner-scoped job registry (44 §10.2, 26 §4.9). Additive.
struct JobsSettings {
    std::int64_t       wait_timeout_ms = 30'000;
    std::int64_t       max_wait_timeout_ms = 600'000;
    CompletionDelivery completion_delivery = CompletionDelivery::Wakeup;
    std::uint32_t      max_consecutive_wakes = 3;
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
    SessionSettings    session;
    PromptSettings     prompt;
    ToolsSettings      tools;
    PresetsSettings    presets;
    GoalsSettings      goals;
    JobsSettings       jobs;
};

// Explicit layer sources. `global` is required (21-D12): it must be non-empty
// and name an existing regular file. `workspace` may be empty (optional).
struct ConfigPaths {
    std::filesystem::path global;
    std::filesystem::path workspace;
};

// `$XDG_CONFIG_HOME/ymh/config.jsonc`, else `$HOME/.config/ymh/config.jsonc`.
[[nodiscard]] std::filesystem::path default_global_config_path();

// `<root>/.ymh/config.jsonc`.
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
// config directory, writes a commented default `config.jsonc` there if (and only
// if) it does not already exist, and ensures `<workspace_root>/.ymh/` exists.
// An existing config file is never overwritten. An empty `global_config` target
// is a deliberate skip (no directory, no write, no warning). Failures are
// reported to `logger` (may be null) and reflected in `ScaffoldResult::ok`.
[[nodiscard]] ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                                             const std::filesystem::path& global_config,
                                             Logger* logger = nullptr);

// Convenience overload resolving the global config path with
// `default_global_config_path()`.
[[nodiscard]] ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                                             Logger* logger = nullptr);

// Loads defaults, then global, then project, then environment overrides. The
// global layer is REQUIRED (21-D12): an empty or absent `paths.global` throws
// `ConfigError`; the workspace layer stays optional.
[[nodiscard]] Config load_config(const ConfigPaths& paths);

// Convenience overload using `default_global_config_path()`.
[[nodiscard]] Config load_config(const std::filesystem::path& workspace_root);

// Applies one JSONC document over `config`. `required == false` (the optional
// workspace layer): an empty path or an absent file contributes nothing.
// `required == true` (the global layer): an empty path or an absent file throws
// `ConfigError` (21-D12). A present path that is not a regular file is a
// `ConfigError` for either layer (21-D16). A parse failure or unknown key always
// throws.
void apply_jsonc_file(Config& config, const std::filesystem::path& path,
                      bool required = false);

// 25-D13: parses the localcode-shaped `mcp_servers` object into `mcp.servers`,
// applying key normalization, the id-dedupe algorithm, and the
// `type`/`transport`, `env`/`headers`, `url`/`cwd` rules. Replaces the server
// array wholesale (per-layer semantics). Throws `ConfigError` on any
// shape/grammar violation. `source` is used only for error text.
void apply_mcp_servers_object(McpSettings& mcp, const nlohmann::json& table,
                              const std::filesystem::path& source);

// 25-D15 / 46-D12 / 52-D7: builds a ymh config document (JSON object) from a
// parsed localcode document, including every `providers.<n>` as an
// `llm.endpoints.<n>` entry, every `profiles.<p>` as an `llm.models.<p>` entry
// (with `default_profile` -> `llm.active_model`), the mapped servers, the
// `skip_permissions`/`permission` rules, and per-model `max_tokens`/
// `context_window`. Pure mapping: no logging, no filesystem; escapes a literal
// `${` as `$${` in every copied MCP value and forces `required=false` on every
// copied server. Every skip pushes exactly one note in the same iteration
// (52-F7/52-F20), so note count == skip count. Returns `std::nullopt` and fills
// `error` only on an unrecoverable top-level shape problem. Semantic MCP
// validation lives in the CLI layer (25-D16).
struct LocalcodeImportResult {
    nlohmann::json           document;
    std::vector<std::string> notes;
};

[[nodiscard]] std::optional<LocalcodeImportResult> build_localcode_import(
    const nlohmann::json& localcode, std::string& error);

// `$HOME/.localcode/config.json`.
[[nodiscard]] std::filesystem::path localcode_config_path();

// Merges `YMH_*` environment variables over `config`. Unset variables are a
// no-op; a malformed value (e.g. non-numeric timeout) throws `ConfigError`.
void apply_env_overrides(Config& config);

// The effective model: `agent.model` if set, else `llm.model`.
[[nodiscard]] std::string effective_model(const Config& config);

// 52-D3: the fully resolved endpoint (connection) for one selection.
struct ResolvedEndpoint {
    std::string  name;   // "" for the anonymous default endpoint
    ProviderId   provider;
    std::string  base_url;
    std::string  api_key_env;
    std::optional<std::string> api_key;
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::chrono::milliseconds request_timeout{120'000};
    RetrySettings             retry;
    std::size_t               max_concurrency = 4;
};

// 52-D3: the fully resolved model (generation + its endpoint). `model_name` is
// the selected `llm.models` entry name ("" for the literal-id/default paths);
// `source` is the selector ORIGIN, never the resolved name (52-F3).
struct ResolvedModel {
    ResolvedEndpoint             endpoint;
    std::string                  model_name;
    std::string                  model_id;
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::uint32_t> context_window;
    std::optional<std::string>   reasoning_effort;
    ModelProfile                 profile;
    std::optional<double>        temperature;
    std::optional<double>        top_p;
    std::optional<std::uint32_t> top_k;
    std::optional<std::string>   tool_choice;
    std::vector<std::string>     stop;
    std::optional<std::uint32_t> seed;
    std::string                  source;
};

// 52-D3: total, deterministic resolution. Order: `agent.model` (name wins over
// a literal id) -> `llm.active_model` -> `llm.model` verbatim -> built-in
// `deepseek-flash`. Throws `ConfigError` for an unknown active model/endpoint.
[[nodiscard]] ResolvedModel resolve_model(const Config& config);

// 52 review (2A): re-check the LLM cross-references a `Config` carries
// (`llm.default.profile`, every `llm.models.*` endpoint/profile, and the
// `active_model`/`active_endpoint` selectors). `load_config` runs this after the
// file/env layers; the CLI runs it again after applying `--model`/`--endpoint`/…
// so an unknown selector exits 2 instead of throwing from `resolve_model` on an
// uncaught dispatch path.
void validate_llm_references(const Config& config);

// 52-D15: the deployment permission baseline. When `permissions.default_preset`
// names a preset its settings govern; otherwise the baseline is
// `{agent.sandbox, "ask"}`. A preset binding may only narrow this baseline
// (52-I11).
[[nodiscard]] PermissionPresetSettings deployment_permission_baseline(const Config& config);

// 52-D15/52-I12: the deployment's default permission preset name, pinned into a
// session at creation.
[[nodiscard]] std::string default_permission_preset_name(const Config& config);

// 52-I11: the effective permission preset name for a session. `binding` is a
// preset row's `permission_preset`; it is honored only when it does not widen
// `baseline`, otherwise `fallback` (the deployment default) is kept.
[[nodiscard]] std::string effective_permission_preset_name(
    const PermissionDefaults& permissions, const PermissionPresetSettings& baseline,
    std::string_view fallback, const std::optional<std::string>& binding);

// ymh/execution/environment.hpp (opaque declaration; keeps config.hpp free of
// the execution include).
enum class SandboxMode : std::uint8_t;

// 52-D15: the deployment sandbox after clamping `agent.sandbox` against the
// permission-preset baseline. Single source of truth for both the CLI wiring
// and the workspace runtime, so they cannot drift. Throws `ConfigError` for an
// unknown sandbox value.
[[nodiscard]] SandboxMode effective_sandbox_mode(const Config& config);

[[nodiscard]] std::optional<std::string> env_value(std::string_view name);

} // namespace ymh

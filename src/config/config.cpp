#include "ymh/config/config.hpp"

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <toml++/toml.h>

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

void apply_agent(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "agent",
                   {"model", "max_steps", "reasoning_effort", "system_prompt"}, source);
    config.agent.model =
        read_value<std::string>(table, "model", "agent", config.agent.model, source);
    config.agent.max_steps =
        static_cast<std::size_t>(read_value<std::int64_t>(
            table, "max_steps", "agent", static_cast<std::int64_t>(config.agent.max_steps), source));
    config.agent.reasoning_effort =
        read_optional_string(table, "reasoning_effort", "agent", source);
    config.agent.system_prompt =
        read_value<std::string>(table, "system_prompt", "agent", config.agent.system_prompt, source);
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

void apply_document(Config& config, const toml::table& table, const std::filesystem::path& source) {
    reject_unknown(table, "", {"ui", "agent", "workspace", "permissions", "logging", "llm"},
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

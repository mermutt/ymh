#include "ymh/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/config/jsonc.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/mcp/mcp_types.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {
namespace {

using Json = nlohmann::json;

// Raised by the localcode import mapper when a copied value has the wrong
// shape (UX-F14: a non-string env/header value must abort the import, never be
// silently dropped). `build_localcode_import` turns it into the out-`error`.
struct LocalcodeImportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr std::string_view kGlobalDir  = "ymh";
constexpr std::string_view kConfigFile = "config.jsonc";

[[noreturn]] void fail(const std::filesystem::path& source, std::string_view detail) {
    throw ConfigError("config " + source.string() + ": " + std::string{detail});
}

// 46-D12.2: a literal `llm.api_key` may only be read from a file with no
// group/other permission bits, so a chmod'd config cannot silently leak it.
void require_private_config_file(const std::filesystem::path& source) {
    struct stat info {};
    if (::stat(source.c_str(), &info) != 0) {
        return;
    }
    if ((info.st_mode & 077) != 0) {
        fail(source, "'llm.api_key' requires a 0600 config file");
    }
}

// Parse a JSONC document with comments tolerated. The shared
// `parse_jsonc_document` reports line/column; this path-aware wrapper raises
// `ConfigError` (parity with the retired TOML path).
Json parse_jsonc(const std::filesystem::path& path, std::string_view text) {
    if (blank_or_comments_only(text)) {
        return Json::object();  // empty / comments-only => no-op (J-L2)
    }
    std::string         error;
    std::optional<Json> parsed = parse_jsonc_document(text, error);
    if (!parsed.has_value()) {
        fail(path, error);
    }
    return std::move(*parsed);
}

void reject_unknown(const Json& table,
                    std::string_view table_name,
                    std::initializer_list<std::string_view> allowed,
                    const std::filesystem::path& source) {
    for (auto it = table.begin(); it != table.end(); ++it) {
        const std::string_view name = it.key();
        bool                   known = false;
        for (const std::string_view candidate : allowed) {
            if (candidate == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            const std::string qualified =
                table_name.empty()
                    ? std::string{name}
                    : std::string{table_name} + "." + std::string{name};
            fail(source, "unknown key '" + qualified + "'");
        }
    }
}

const Json* member(const Json& obj, std::string_view key) {
    const auto it = obj.find(std::string{key});
    return it == obj.end() ? nullptr : &(*it);
}

bool name_in(std::initializer_list<std::string_view> names, std::string_view key) {
    for (const std::string_view candidate : names) {
        if (candidate == key) {
            return true;
        }
    }
    return false;
}

// 52-I1: an endpoint/model name is non-empty, <=64 bytes, and matches
// [A-Za-z0-9][A-Za-z0-9._-]*. Entry names are a free namespace; only the
// grammar is checked (52-F5).
bool valid_config_entry_name(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (std::isalnum(first) == 0) {
        return false;
    }
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '.' || c == '_' || c == '-') {
            continue;
        }
        return false;
    }
    return true;
}

// 52-D5/52-I6: validates a user-named map. Each value must be an object with a
// well-formed name, every key INSIDE an entry is checked against `allowed`, and
// `retry`/`headers` recurse. Only the entry NAME is not schema-checked.
void reject_unknown_named_map(const Json& table,
                              std::string_view table_name,
                              std::initializer_list<std::string_view> allowed,
                              const std::filesystem::path& source) {
    if (!table.is_object()) {
        fail(source, "invalid type for '" + std::string{table_name} + "'");
    }
    const bool allow_retry   = name_in(allowed, "retry");
    const bool allow_headers = name_in(allowed, "headers");
    for (auto it = table.begin(); it != table.end(); ++it) {
        const std::string name = it.key();
        if (!valid_config_entry_name(name)) {
            fail(source, "invalid name '" + name + "' for '" + std::string{table_name} + "'");
        }
        const std::string label = std::string{table_name} + "." + name;
        if (!it.value().is_object()) {
            fail(source, "invalid type for '" + label + "'");
        }
        for (auto entry = it.value().begin(); entry != it.value().end(); ++entry) {
            if (!name_in(allowed, entry.key())) {
                fail(source, "unknown key '" + label + "." + entry.key() + "'");
            }
        }
        if (allow_retry) {
            if (const Json* retry = member(it.value(), "retry"); retry != nullptr) {
                if (!retry->is_object()) {
                    fail(source, "invalid type for '" + label + ".retry'");
                }
                for (auto field = retry->begin(); field != retry->end(); ++field) {
                    if (!name_in({"max_attempts", "base_delay_ms", "max_delay_ms", "jitter",
                                  "honor_retry_after"},
                                 field.key())) {
                        fail(source, "unknown key '" + label + ".retry." + field.key() + "'");
                    }
                }
            }
        }
        if (allow_headers) {
            if (const Json* headers = member(it.value(), "headers"); headers != nullptr) {
                if (!headers->is_object()) {
                    fail(source, "invalid type for '" + label + ".headers'");
                }
                for (auto header = headers->begin(); header != headers->end(); ++header) {
                    if (!header.value().is_string()) {
                        fail(source, "invalid type for '" + label + ".headers." + header.key() + "'");
                    }
                }
            }
        }
    }
}

std::string read_string(const Json& obj,
                        std::string_view key,
                        std::string_view table_name,
                        std::string fallback,
                        const std::filesystem::path& source) {
    const Json* node = member(obj, key);
    if (node == nullptr) {
        return fallback;
    }
    if (!node->is_string()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    return node->get<std::string>();
}

bool read_bool(const Json& obj,
               std::string_view key,
               std::string_view table_name,
               bool fallback,
               const std::filesystem::path& source) {
    const Json* node = member(obj, key);
    if (node == nullptr) {
        return fallback;
    }
    if (!node->is_boolean()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    return node->get<bool>();
}

double read_double(const Json& obj,
                   std::string_view key,
                   std::string_view table_name,
                   double fallback,
                   const std::filesystem::path& source) {
    const Json* node = member(obj, key);
    if (node == nullptr) {
        return fallback;
    }
    if (!node->is_number()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    return node->get<double>();
}

// Non-negative only: a negative integer is a ConfigError (J13, J-F10).
std::int64_t read_int64(const Json& obj,
                        std::string_view key,
                        std::string_view table_name,
                        std::int64_t fallback,
                        const std::filesystem::path& source) {
    const Json* node = member(obj, key);
    if (node == nullptr) {
        return fallback;
    }
    const std::string label = std::string{table_name} + "." + std::string{key};
    if (node->is_number_unsigned()) {
        const std::uint64_t value = node->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail(source, "value out of range for '" + label + "'");
        }
        return static_cast<std::int64_t>(value);
    }
    if (node->is_number_integer()) {
        const std::int64_t value = node->get<std::int64_t>();
        if (value < 0) {
            fail(source, "value must be non-negative for '" + label + "'");
        }
        return value;
    }
    fail(source, "invalid type for '" + label + "'");
}

std::optional<std::string> read_optional_string(const Json& obj,
                                                std::string_view key,
                                                std::string_view table_name,
                                                const std::filesystem::path& source) {
    const Json* node = member(obj, key);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (!node->is_string()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    return node->get<std::string>();
}

std::optional<double> read_optional_double(const Json& obj,
                                           std::string_view key,
                                           std::string_view table_name,
                                           const std::filesystem::path& source) {
    const Json* node = member(obj, key);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (!node->is_number()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    const double value = node->get<double>();
    if (!std::isfinite(value)) {
        fail(source, "value must be finite for '" + std::string{table_name} + "." +
                         std::string{key} + "'");
    }
    return value;
}

std::vector<std::string> read_string_array(const Json& obj,
                                           std::string_view key,
                                           std::string_view table_name,
                                           const std::filesystem::path& source) {
    std::vector<std::string> result;
    const Json*              node = member(obj, key);
    if (node == nullptr) {
        return result;
    }
    if (!node->is_array()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} + "'");
    }
    for (const Json& element : *node) {
        if (!element.is_string()) {
            fail(source, "'" + std::string{table_name} + "." + std::string{key} +
                             "' must be an array of strings");
        }
        result.push_back(element.get<std::string>());
    }
    return result;
}

void apply_ui(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "ui", {"theme", "show_activity", "side_panel"}, source);
    config.ui.theme = read_string(table, "theme", "ui", config.ui.theme, source);
    config.ui.show_activity =
        read_bool(table, "show_activity", "ui", config.ui.show_activity, source);
    config.ui.side_panel = read_string(table, "side_panel", "ui", config.ui.side_panel, source);
}

void apply_compaction(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "agent.compaction",
                   {"enabled", "threshold_tokens", "threshold_ratio", "context_window_tokens",
                    "reserve_output_tokens", "keep_recent_turns", "min_prefix_messages",
                    "max_summary_tokens", "max_summary_bytes", "summarizer_model",
                    "max_compactions_per_turn", "retry_on_context_length"},
                   source);
    CompactionSettings& compaction = config.agent.compaction;
    compaction.enabled = read_bool(table, "enabled", "agent.compaction", compaction.enabled, source);
    compaction.threshold_tokens = static_cast<std::size_t>(read_int64(
        table, "threshold_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.threshold_tokens), source));
    compaction.threshold_ratio =
        read_double(table, "threshold_ratio", "agent.compaction", compaction.threshold_ratio, source);
    compaction.context_window_tokens = static_cast<std::size_t>(read_int64(
        table, "context_window_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.context_window_tokens), source));
    compaction.reserve_output_tokens = static_cast<std::size_t>(read_int64(
        table, "reserve_output_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.reserve_output_tokens), source));
    compaction.keep_recent_turns = static_cast<std::size_t>(read_int64(
        table, "keep_recent_turns", "agent.compaction",
        static_cast<std::int64_t>(compaction.keep_recent_turns), source));
    compaction.min_prefix_messages = static_cast<std::size_t>(read_int64(
        table, "min_prefix_messages", "agent.compaction",
        static_cast<std::int64_t>(compaction.min_prefix_messages), source));
    compaction.max_summary_tokens = static_cast<std::size_t>(read_int64(
        table, "max_summary_tokens", "agent.compaction",
        static_cast<std::int64_t>(compaction.max_summary_tokens), source));
    compaction.max_summary_bytes = static_cast<std::size_t>(read_int64(
        table, "max_summary_bytes", "agent.compaction",
        static_cast<std::int64_t>(compaction.max_summary_bytes), source));
    compaction.summarizer_model = read_string(table, "summarizer_model", "agent.compaction",
                                              compaction.summarizer_model, source);
    compaction.max_compactions_per_turn = static_cast<std::size_t>(read_int64(
        table, "max_compactions_per_turn", "agent.compaction",
        static_cast<std::int64_t>(compaction.max_compactions_per_turn), source));
    compaction.retry_on_context_length =
        read_bool(table, "retry_on_context_length", "agent.compaction",
                  compaction.retry_on_context_length, source);
}

void apply_plan(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "agent.plan", {"section"}, source);
    config.agent.plan_section =
        read_string(table, "section", "agent.plan", config.agent.plan_section, source);
}

void apply_agent(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "agent",
                   {"model", "max_steps", "reasoning_effort", "system_prompt", "compaction",
                    "compaction_threshold_tokens", "plan"},
                   source);
    config.agent.model = read_string(table, "model", "agent", config.agent.model, source);
    config.agent.max_steps = static_cast<std::size_t>(read_int64(
        table, "max_steps", "agent", static_cast<std::int64_t>(config.agent.max_steps), source));
    config.agent.reasoning_effort = read_optional_string(table, "reasoning_effort", "agent", source);
    config.agent.system_prompt =
        read_string(table, "system_prompt", "agent", config.agent.system_prompt, source);
    config.agent.compaction.threshold_tokens = static_cast<std::size_t>(read_int64(
        table, "compaction_threshold_tokens", "agent",
        static_cast<std::int64_t>(config.agent.compaction.threshold_tokens), source));
    if (const Json* nested = member(table, "compaction"); nested != nullptr) {
        if (!nested->is_object()) {
            fail(source, "invalid type for 'agent.compaction'");
        }
        apply_compaction(config, *nested, source);
    }
    if (const Json* nested = member(table, "plan"); nested != nullptr) {
        if (!nested->is_object()) {
            fail(source, "invalid type for 'agent.plan'");
        }
        apply_plan(config, *nested, source);
    }
}

void apply_workspace(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "workspace", {"root", "workspace_roots"}, source);
    config.workspace.root = read_string(table, "root", "workspace", config.workspace.root, source);
    if (const auto roots = read_string_array(table, "workspace_roots", "workspace", source);
        !roots.empty()) {
        config.workspace.workspace_roots = roots;
    }
}

void apply_permissions(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "permissions", {"shell", "write", "read", "default", "rules"}, source);
    config.permissions.shell =
        read_string(table, "shell", "permissions", config.permissions.shell, source);
    config.permissions.write =
        read_string(table, "write", "permissions", config.permissions.write, source);
    config.permissions.read =
        read_string(table, "read", "permissions", config.permissions.read, source);
    config.permissions.default_verdict = read_string(
        table, "default", "permissions", config.permissions.default_verdict, source);

    const Json* rules = member(table, "rules");
    if (rules == nullptr) {
        return;
    }
    if (!rules->is_array()) {
        fail(source, "invalid type for 'permissions.rules'");
    }
    config.permissions.rules.clear();
    std::size_t index = 0;
    for (const Json& entry : *rules) {
        if (!entry.is_object()) {
            fail(source, "invalid type for 'permissions.rules[" + std::to_string(index) + "]'");
        }
        reject_unknown(entry, "permissions.rules", {"tool", "command", "effect", "id"}, source);
        PermissionRuleSettings rule;
        rule.tool    = read_optional_string(entry, "tool", "permissions.rules", source);
        rule.command = read_optional_string(entry, "command", "permissions.rules", source);
        if (member(entry, "effect") == nullptr) {
            fail(source, "missing 'permissions.rules.effect'");
        }
        rule.effect = read_string(entry, "effect", "permissions.rules", "", source);
        if (rule.effect != "allow" && rule.effect != "ask" && rule.effect != "deny") {
            fail(source, "invalid effect for 'permissions.rules.effect': '" + rule.effect + "'");
        }
        rule.id = read_string(entry, "id", "permissions.rules",
                              "config.rule." + std::to_string(index), source);
        try {
            validate_rule(rule.tool.value_or(""), rule.command.value_or(""));
        } catch (const PolicyConfigError& error) {
            fail(source, error.what());
        }
        config.permissions.rules.push_back(std::move(rule));
        ++index;
    }
}

void apply_logging(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "logging", {"level", "log_prompts"}, source);
    config.logging.level = read_string(table, "level", "logging", config.logging.level, source);
    config.logging.log_prompts =
        read_bool(table, "log_prompts", "logging", config.logging.log_prompts, source);
}

void apply_session(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "session", {"persist_prompt_text"}, source);
    config.session.persist_prompt_text = read_bool(
        table, "persist_prompt_text", "session", config.session.persist_prompt_text, source);
}

void read_retry(RetrySettings& retry, const Json& table, std::string_view label,
                const std::filesystem::path& source) {
    retry.max_attempts = static_cast<std::uint32_t>(read_int64(
        table, "max_attempts", label, static_cast<std::int64_t>(retry.max_attempts), source));
    retry.base_delay = std::chrono::milliseconds{
        read_int64(table, "base_delay_ms", label, retry.base_delay.count(), source)};
    retry.max_delay = std::chrono::milliseconds{
        read_int64(table, "max_delay_ms", label, retry.max_delay.count(), source)};
    retry.jitter = read_double(table, "jitter", label, retry.jitter, source);
    retry.honor_retry_after =
        read_bool(table, "honor_retry_after", label, retry.honor_retry_after, source);
}

void apply_retry(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "llm.default.retry",
                   {"max_attempts", "base_delay_ms", "max_delay_ms", "jitter", "honor_retry_after"},
                   source);
    read_retry(config.llm.retry, table, "llm.default.retry", source);
}

std::vector<std::pair<std::string, std::string>> read_string_object(
    const Json& table, std::string_view key, std::string_view label,
    const std::filesystem::path& source) {
    std::vector<std::pair<std::string, std::string>> result;
    const Json* node = member(table, key);
    if (node == nullptr) {
        return result;
    }
    if (!node->is_object()) {
        fail(source, "invalid type for '" + std::string{label} + "'");
    }
    for (auto it = node->begin(); it != node->end(); ++it) {
        if (!it.value().is_string()) {
            fail(source,
                 "invalid type for '" + std::string{label} + "." + it.key() + "'");
        }
        result.emplace_back(it.key(), it.value().get<std::string>());
    }
    return result;
}

void apply_endpoint_entry(EndpointSettings& entry, const Json& table, const std::string& label,
                          const std::filesystem::path& source, bool global_layer) {
    entry.provider =
        read_string(table, "provider", label, entry.provider, source);
    entry.base_url = read_string(table, "base_url", label, entry.base_url, source);
    entry.api_key_env = read_string(table, "api_key_env", label, entry.api_key_env, source);

    if (member(table, "api_key") != nullptr) {
        if (!global_layer) {
            fail(source, "'" + label + ".api_key' is global-layer only");
        }
        const std::string value = read_string(table, "api_key", label, "", source);
        if (!value.empty()) {
            require_private_config_file(source);
            entry.api_key = value;
        }
    }
    if (member(table, "headers") != nullptr) {
        if (!global_layer) {
            fail(source, "'" + label + ".headers' is global-layer only");
        }
        require_private_config_file(source);
        entry.headers = read_string_object(table, "headers", label + ".headers", source);
    }

    entry.max_concurrency = static_cast<std::size_t>(read_int64(
        table, "max_concurrency", label, static_cast<std::int64_t>(entry.max_concurrency), source));
    entry.connect_timeout = std::chrono::milliseconds{read_int64(
        table, "connect_timeout_ms", label, entry.connect_timeout.count(), source)};
    entry.idle_timeout = std::chrono::milliseconds{
        read_int64(table, "idle_timeout_ms", label, entry.idle_timeout.count(), source)};
    entry.request_timeout = std::chrono::milliseconds{
        read_int64(table, "request_timeout_ms", label, entry.request_timeout.count(), source)};

    if (const Json* retry = member(table, "retry"); retry != nullptr) {
        read_retry(entry.retry, *retry, label + ".retry", source);
    }
}

void apply_model_entry(ModelSettings& entry, const Json& table, const std::string& label,
                       const std::filesystem::path& source) {
    entry.endpoint = read_string(table, "endpoint", label, entry.endpoint, source);
    entry.model    = read_string(table, "model", label, entry.model, source);
    entry.profile  = read_string(table, "profile", label, entry.profile, source);

    if (member(table, "max_tokens") != nullptr) {
        const std::int64_t value = read_int64(table, "max_tokens", label, 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for '" + label + ".max_tokens'");
        }
        entry.max_tokens = static_cast<std::uint32_t>(value);
    }
    if (member(table, "context_window") != nullptr) {
        const std::int64_t value = read_int64(table, "context_window", label, 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for '" + label + ".context_window'");
        }
        entry.context_window = static_cast<std::uint32_t>(value);
    }
    entry.reasoning_effort = read_optional_string(table, "reasoning_effort", label, source);
    if (member(table, "temperature") != nullptr) {
        const std::optional<double> value = read_optional_double(table, "temperature", label, source);
        if (!value.has_value() || *value < 0.0 || *value > 2.0) {
            fail(source, "'" + label + ".temperature' must be in [0.0, 2.0]");
        }
        entry.temperature = value;
    }
    if (member(table, "top_p") != nullptr) {
        const std::optional<double> value = read_optional_double(table, "top_p", label, source);
        if (!value.has_value() || *value <= 0.0 || *value > 1.0) {
            fail(source, "'" + label + ".top_p' must be in (0.0, 1.0]");
        }
        entry.top_p = value;
    }
    if (member(table, "top_k") != nullptr) {
        const std::int64_t value = read_int64(table, "top_k", label, 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for '" + label + ".top_k'");
        }
        entry.top_k = value == 0 ? std::nullopt
                                 : std::optional<std::uint32_t>{static_cast<std::uint32_t>(value)};
    }
    if (member(table, "tool_choice") != nullptr) {
        const std::string value = read_string(table, "tool_choice", label, "", source);
        if (value.empty()) {
            fail(source, "'" + label + ".tool_choice' must not be empty");
        }
        entry.tool_choice = value;
    }
    if (member(table, "stop") != nullptr) {
        entry.stop = read_string_array(table, "stop", label, source);
    }
    if (const ModelProfile* profile = find_model_profile(entry.profile); profile != nullptr) {
        for (const std::string& stop : entry.stop) {
            if (std::find(profile->forbidden_stop_tokens.begin(),
                          profile->forbidden_stop_tokens.end(),
                          stop) != profile->forbidden_stop_tokens.end()) {
                fail(source, label + ".stop must not contain '" + stop +
                                 "': it ends a message, not the turn");
            }
        }
    }
    if (member(table, "seed") != nullptr) {
        const std::int64_t value = read_int64(table, "seed", label, 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for '" + label + ".seed'");
        }
        entry.seed = static_cast<std::uint32_t>(value);
    }
}

void apply_endpoints(Config& config, const Json& table, const std::filesystem::path& source,
                     bool global_layer) {
    reject_unknown_named_map(table, "llm.endpoints",
                             {"provider", "base_url", "api_key", "api_key_env", "headers",
                              "max_concurrency", "connect_timeout_ms", "idle_timeout_ms",
                              "request_timeout_ms", "retry"},
                             source);
    for (auto it = table.begin(); it != table.end(); ++it) {
        const std::string name  = it.key();
        const std::string label = "llm.endpoints." + name;
        EndpointSettings  entry;
        apply_endpoint_entry(entry, it.value(), label, source, global_layer);
        if (!global_layer) {
            const auto existing = config.llm.endpoints.find(name);
            if (existing != config.llm.endpoints.end()) {
                if (existing->second.api_key.has_value()) {
                    fail(source, "workspace endpoint '" + name +
                                     "' shadows a global endpoint with a literal api_key");
                }
                if (!existing->second.headers.empty()) {
                    fail(source, "workspace endpoint '" + name +
                                     "' shadows a global endpoint with credential headers");
                }
            }
        }
        config.llm.endpoints[name] = std::move(entry);
    }
}

void apply_models(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown_named_map(table, "llm.models",
                             {"endpoint", "model", "profile", "max_tokens", "context_window",
                              "reasoning_effort", "temperature", "top_p", "top_k", "tool_choice",
                              "stop", "seed"},
                             source);
    for (auto it = table.begin(); it != table.end(); ++it) {
        const std::string name  = it.key();
        const std::string label = "llm.models." + name;
        ModelSettings     entry;
        apply_model_entry(entry, it.value(), label, source);
        if (entry.endpoint.empty()) {
            fail(source, "missing 'endpoint' for '" + label + "'");
        }
        if (entry.model.empty()) {
            fail(source, "missing 'model' for '" + label + "'");
        }
        config.llm.models[name] = std::move(entry);
    }
}

void apply_llm(Config& config, const Json& table, const std::filesystem::path& source,
               bool global_layer) {
    reject_unknown(table, "llm",
                   {"default", "provider", "base_url", "model", "api_key_env", "api_key",
                    "max_tokens", "reasoning_effort", "profile", "temperature", "top_p", "top_k",
                    "tool_choice", "stop", "seed", "max_concurrency", "connect_timeout_ms",
                    "idle_timeout_ms", "request_timeout_ms", "retry", "endpoints", "models",
                    "active_model"},
                   source);

    if (const Json* endpoints = member(table, "endpoints"); endpoints != nullptr) {
        apply_endpoints(config, *endpoints, source, global_layer);
    }
    if (const Json* models = member(table, "models"); models != nullptr) {
        apply_models(config, *models, source);
    }
    if (const Json* active = member(table, "active_model"); active != nullptr) {
        if (!active->is_string()) {
            fail(source, "invalid type for 'llm.active_model'");
        }
        config.llm.active_model = active->get<std::string>();
    }

    const Json* section = &table;
    if (const Json* nested = member(table, "default"); nested != nullptr) {
        if (!nested->is_object()) {
            fail(source, "invalid type for 'llm.default'");
        }
        reject_unknown(*nested, "llm.default",
                       {"provider", "base_url", "model", "api_key_env", "api_key", "max_tokens",
                        "reasoning_effort", "profile", "temperature", "top_p", "top_k",
                        "tool_choice", "stop", "seed", "max_concurrency", "connect_timeout_ms",
                        "idle_timeout_ms", "request_timeout_ms", "retry"},
                       source);
        section = nested;
    }

    if (member(*section, "api_key") != nullptr) {
        if (!global_layer) {
            fail(source, "'llm.api_key' is global-layer only");
        }
        const std::string value = read_string(*section, "api_key", "llm.default", "", source);
        if (!value.empty()) {
            require_private_config_file(source);
            config.llm.api_key = value;
        }
    }
    if (member(*section, "max_tokens") != nullptr) {
        const std::int64_t value =
            read_int64(*section, "max_tokens", "llm.default", 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for 'llm.default.max_tokens'");
        }
        config.llm.max_tokens = static_cast<std::uint32_t>(value);
    }

    config.llm.profile =
        read_string(*section, "profile", "llm.default", config.llm.profile, source);
    if (member(*section, "temperature") != nullptr) {
        const std::optional<double> value =
            read_optional_double(*section, "temperature", "llm.default", source);
        if (!value.has_value() || *value < 0.0 || *value > 2.0) {
            fail(source, "'llm.default.temperature' must be in [0.0, 2.0]");
        }
        config.llm.temperature = value;
    }
    if (member(*section, "top_p") != nullptr) {
        const std::optional<double> value =
            read_optional_double(*section, "top_p", "llm.default", source);
        if (!value.has_value() || *value <= 0.0 || *value > 1.0) {
            fail(source, "'llm.default.top_p' must be in (0.0, 1.0]");
        }
        config.llm.top_p = value;
    }
    if (member(*section, "top_k") != nullptr) {
        const std::int64_t value = read_int64(*section, "top_k", "llm.default", 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for 'llm.default.top_k'");
        }
        if (value == 0) {
            config.llm.top_k = std::nullopt;
        } else {
            config.llm.top_k = static_cast<std::uint32_t>(value);
        }
    }
    if (member(*section, "tool_choice") != nullptr) {
        const std::string value = read_string(*section, "tool_choice", "llm.default", "", source);
        if (value.empty()) {
            fail(source, "'llm.default.tool_choice' must not be empty");
        }
        config.llm.tool_choice = value;
    }
    if (member(*section, "stop") != nullptr) {
        config.llm.stop = read_string_array(*section, "stop", "llm.default", source);
    }
    if (const ModelProfile* profile = find_model_profile(config.llm.profile);
        profile != nullptr) {
        for (const std::string& entry : config.llm.stop) {
            if (std::find(profile->forbidden_stop_tokens.begin(),
                          profile->forbidden_stop_tokens.end(),
                          entry) != profile->forbidden_stop_tokens.end()) {
                fail(source, "llm.default.stop must not contain '" + entry +
                                 "': it ends a message, not the turn");
            }
        }
    }
    if (member(*section, "seed") != nullptr) {
        const std::int64_t value = read_int64(*section, "seed", "llm.default", 0, source);
        if (value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(source, "value out of range for 'llm.default.seed'");
        }
        config.llm.seed = static_cast<std::uint32_t>(value);
    }

    config.llm.provider =
        read_string(*section, "provider", "llm.default", config.llm.provider, source);
    config.llm.base_url =
        read_string(*section, "base_url", "llm.default", config.llm.base_url, source);
    config.llm.model = read_string(*section, "model", "llm.default", config.llm.model, source);
    config.llm.api_key_env =
        read_string(*section, "api_key_env", "llm.default", config.llm.api_key_env, source);
    config.llm.reasoning_effort =
        read_optional_string(*section, "reasoning_effort", "llm.default", source);
    config.llm.max_concurrency = static_cast<std::size_t>(read_int64(
        *section, "max_concurrency", "llm.default",
        static_cast<std::int64_t>(config.llm.max_concurrency), source));
    config.llm.connect_timeout = std::chrono::milliseconds{read_int64(
        *section, "connect_timeout_ms", "llm.default", config.llm.connect_timeout.count(), source)};
    config.llm.idle_timeout = std::chrono::milliseconds{read_int64(
        *section, "idle_timeout_ms", "llm.default", config.llm.idle_timeout.count(), source)};
    config.llm.request_timeout = std::chrono::milliseconds{read_int64(
        *section, "request_timeout_ms", "llm.default", config.llm.request_timeout.count(), source)};

    if (const Json* retry = member(*section, "retry"); retry != nullptr) {
        if (!retry->is_object()) {
            fail(source, "invalid type for 'llm.default.retry'");
        }
        apply_retry(config, *retry, source);
    }
}

void apply_mcp_server(McpServerSettings& server, const Json& table,
                      const std::filesystem::path& source) {
    reject_unknown(table, "mcp.server",
                   {"id", "enabled", "required", "transport", "command", "args", "env", "cwd", "url",
                    "header_env", "protocol_version", "allowed_tools", "denied_tools",
                    "default_verdict", "call_timeout_ms", "max_result_bytes"},
                   source);
    server.id = read_string(table, "id", "mcp.server", server.id, source);
    server.enabled = read_bool(table, "enabled", "mcp.server", server.enabled, source);
    server.required = read_bool(table, "required", "mcp.server", server.required, source);
    server.transport =
        read_string(table, "transport", "mcp.server", server.transport, source);
    server.command = read_string(table, "command", "mcp.server", server.command, source);
    server.cwd = read_string(table, "cwd", "mcp.server", server.cwd, source);
    server.url = read_string(table, "url", "mcp.server", server.url, source);
    server.protocol_version =
        read_string(table, "protocol_version", "mcp.server", server.protocol_version, source);
    server.default_verdict =
        read_string(table, "default_verdict", "mcp.server", server.default_verdict, source);
    server.call_timeout_ms =
        read_int64(table, "call_timeout_ms", "mcp.server", server.call_timeout_ms, source);
    server.max_result_bytes = static_cast<std::size_t>(read_int64(
        table, "max_result_bytes", "mcp.server",
        static_cast<std::int64_t>(server.max_result_bytes), source));

    if (auto values = read_string_array(table, "args", "mcp.server", source); !values.empty()) {
        server.args = std::move(values);
    }
    if (auto values = read_string_array(table, "env", "mcp.server", source); !values.empty()) {
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

void apply_mcp(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "mcp",
                   {"enabled", "max_servers", "max_inflight_calls_per_server",
                    "startup_deadline_ms", "handshake_timeout_ms", "list_timeout_ms", "list_max_pages",
                    "reconnect_max_attempts", "reconnect_initial_backoff_ms",
                    "reconnect_max_backoff_ms", "reconnect_jitter", "reconnect_stable_window_ms",
                    "ping_interval_ms", "shutdown_grace_ms", "max_frame_bytes",
                    "allow_network_servers", "log_child_stderr", "server"},
                   source);
    McpSettings& mcp = config.mcp;
    mcp.enabled = read_bool(table, "enabled", "mcp", mcp.enabled, source);
    mcp.max_servers = static_cast<std::size_t>(read_int64(
        table, "max_servers", "mcp", static_cast<std::int64_t>(mcp.max_servers), source));
    mcp.max_inflight_calls_per_server = static_cast<std::size_t>(
        read_int64(table, "max_inflight_calls_per_server", "mcp",
                   static_cast<std::int64_t>(mcp.max_inflight_calls_per_server), source));
    mcp.startup_deadline_ms =
        read_int64(table, "startup_deadline_ms", "mcp", mcp.startup_deadline_ms, source);
    mcp.handshake_timeout_ms =
        read_int64(table, "handshake_timeout_ms", "mcp", mcp.handshake_timeout_ms, source);
    mcp.list_timeout_ms = read_int64(table, "list_timeout_ms", "mcp", mcp.list_timeout_ms, source);
    mcp.list_max_pages = static_cast<std::size_t>(read_int64(
        table, "list_max_pages", "mcp", static_cast<std::int64_t>(mcp.list_max_pages), source));
    mcp.reconnect_max_attempts = static_cast<std::uint32_t>(read_int64(
        table, "reconnect_max_attempts", "mcp",
        static_cast<std::int64_t>(mcp.reconnect_max_attempts), source));
    mcp.reconnect_initial_backoff_ms = read_int64(
        table, "reconnect_initial_backoff_ms", "mcp", mcp.reconnect_initial_backoff_ms, source);
    mcp.reconnect_max_backoff_ms = read_int64(
        table, "reconnect_max_backoff_ms", "mcp", mcp.reconnect_max_backoff_ms, source);
    mcp.reconnect_jitter =
        read_double(table, "reconnect_jitter", "mcp", mcp.reconnect_jitter, source);
    mcp.reconnect_stable_window_ms = read_int64(
        table, "reconnect_stable_window_ms", "mcp", mcp.reconnect_stable_window_ms, source);
    mcp.ping_interval_ms =
        read_int64(table, "ping_interval_ms", "mcp", mcp.ping_interval_ms, source);
    mcp.shutdown_grace_ms =
        read_int64(table, "shutdown_grace_ms", "mcp", mcp.shutdown_grace_ms, source);
    mcp.max_frame_bytes = static_cast<std::size_t>(read_int64(
        table, "max_frame_bytes", "mcp", static_cast<std::int64_t>(mcp.max_frame_bytes), source));
    mcp.allow_network_servers =
        read_bool(table, "allow_network_servers", "mcp", mcp.allow_network_servers, source);
    mcp.log_child_stderr =
        read_bool(table, "log_child_stderr", "mcp", mcp.log_child_stderr, source);

    if (const Json* node = member(table, "server"); node != nullptr) {
        if (!node->is_array()) {
            fail(source, "invalid type for 'mcp.server'");
        }
        mcp.servers.clear();
        for (const Json& entry : *node) {
            if (!entry.is_object()) {
                fail(source, "invalid type for an 'mcp.server' entry");
            }
            McpServerSettings server;
            apply_mcp_server(server, entry, source);
            mcp.servers.push_back(std::move(server));
        }
    }
}

bool is_valid_localcode_server_name(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string canonical_mcp_transport(std::string_view value) {
    if (value == "http" || value == "sse") {
        return "http_sse";
    }
    return std::string{value};
}

std::vector<std::string> read_string_object_as_env(const Json& obj,
                                                   std::string_view key,
                                                   std::string_view table_name,
                                                   const std::filesystem::path& source) {
    std::vector<std::string> result;
    const Json*              node = member(obj, key);
    if (node == nullptr) {
        return result;
    }
    if (!node->is_object()) {
        fail(source, "invalid type for '" + std::string{table_name} + "." + std::string{key} +
                         "' (expected an object)");
    }
    for (auto it = node->begin(); it != node->end(); ++it) {
        if (!it.value().is_string()) {
            fail(source, "'" + std::string{table_name} + "." + std::string{key} + "." + it.key() +
                             "' must be a string");
        }
        result.push_back(it.key() + "=" + it.value().get<std::string>());
    }
    return result;
}

void apply_mcp_servers_entry(McpServerSettings& server, const Json& entry, const std::string& key,
                             const std::filesystem::path& source) {
    const std::string table = "mcp_servers." + key;
    reject_unknown(entry, table,
                   {"type", "transport", "command", "args", "env", "headers", "url", "cwd",
                    "header_env", "protocol_version", "enabled", "required", "allowed_tools",
                    "denied_tools", "default_verdict", "call_timeout_ms", "max_result_bytes"},
                   source);

    const std::optional<std::string> type = read_optional_string(entry, "type", table, source);
    const std::optional<std::string> transport =
        read_optional_string(entry, "transport", table, source);
    if (type.has_value() && transport.has_value() &&
        canonical_mcp_transport(*type) != canonical_mcp_transport(*transport)) {
        fail(source, "'" + table + "': 'type' and 'transport' must not differ");
    }
    std::string kind = type.has_value() ? *type : (transport.has_value() ? *transport : "stdio");
    kind = canonical_mcp_transport(kind);
    if (kind != "stdio" && kind != "http_sse") {
        fail(source, "'" + table + ".type' must be one of: stdio, http_sse, http, sse (got '" +
                         kind + "')");
    }
    server.transport = std::move(kind);

    server.enabled = read_bool(entry, "enabled", table, server.enabled, source);
    server.required = read_bool(entry, "required", table, server.required, source);
    server.command = read_string(entry, "command", table, server.command, source);
    server.cwd = read_string(entry, "cwd", table, server.cwd, source);
    server.url = read_string(entry, "url", table, server.url, source);
    server.protocol_version =
        read_string(entry, "protocol_version", table, server.protocol_version, source);
    server.default_verdict =
        read_string(entry, "default_verdict", table, server.default_verdict, source);
    server.call_timeout_ms =
        read_int64(entry, "call_timeout_ms", table, server.call_timeout_ms, source);
    server.max_result_bytes = static_cast<std::size_t>(read_int64(
        entry, "max_result_bytes", table, static_cast<std::int64_t>(server.max_result_bytes),
        source));

    if (auto values = read_string_array(entry, "args", table, source); !values.empty()) {
        server.args = std::move(values);
    }
    if (auto values = read_string_array(entry, "header_env", table, source); !values.empty()) {
        server.header_env = std::move(values);
    }
    if (auto values = read_string_array(entry, "allowed_tools", table, source); !values.empty()) {
        server.allowed_tools = std::move(values);
    }
    if (auto values = read_string_array(entry, "denied_tools", table, source); !values.empty()) {
        server.denied_tools = std::move(values);
    }

    if (const Json* env = member(entry, "env"); env != nullptr) {
        server.env = read_string_object_as_env(entry, "env", table, source);
    }
    if (const Json* headers = member(entry, "headers"); headers != nullptr) {
        server.header_env = read_string_object_as_env(entry, "headers", table, source);
    }

    if (server.transport != "stdio" && server.url.empty()) {
        fail(source, "'" + table + "': non-stdio transport requires a non-empty 'url'");
    }
    if (server.transport == "stdio" && server.command.empty()) {
        fail(source, "'" + table + "': stdio transport requires a non-empty 'command'");
    }
}

void apply_skills(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "skills",
                   {"enabled", "expose_workspace", "max_skills", "max_skill_bytes",
                    "max_description_bytes", "max_index_bytes", "max_frontmatter_bytes"},
                   source);
    SkillsSettings& skills = config.skills;
    skills.enabled = read_bool(table, "enabled", "skills", skills.enabled, source);
    skills.expose_workspace =
        read_bool(table, "expose_workspace", "skills", skills.expose_workspace, source);
    skills.max_skills = static_cast<std::size_t>(read_int64(
        table, "max_skills", "skills", static_cast<std::int64_t>(skills.max_skills), source));
    skills.max_skill_bytes = static_cast<std::size_t>(read_int64(
        table, "max_skill_bytes", "skills", static_cast<std::int64_t>(skills.max_skill_bytes),
        source));
    skills.max_description_bytes = static_cast<std::size_t>(read_int64(
        table, "max_description_bytes", "skills",
        static_cast<std::int64_t>(skills.max_description_bytes), source));
    skills.max_index_bytes = static_cast<std::size_t>(read_int64(
        table, "max_index_bytes", "skills", static_cast<std::int64_t>(skills.max_index_bytes),
        source));
    skills.max_frontmatter_bytes = static_cast<std::size_t>(read_int64(
        table, "max_frontmatter_bytes", "skills",
        static_cast<std::int64_t>(skills.max_frontmatter_bytes), source));
}

void apply_presets(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "presets",
                   {"root", "default", "include_shipped_root", "include_user_root", "max_depth"},
                   source);
    PresetsSettings& presets = config.presets;
    if (member(table, "root") != nullptr) {
        const std::string value = read_string(table, "root", "presets", std::string{}, source);
        if (value.empty()) {
            fail(source, "'presets.root' must be a non-empty path");
        }
        presets.root = std::filesystem::path{value};
    }
    presets.default_id = read_optional_string(table, "default", "presets", source);
    presets.include_shipped_root =
        read_bool(table, "include_shipped_root", "presets", presets.include_shipped_root, source);
    presets.include_user_root =
        read_bool(table, "include_user_root", "presets", presets.include_user_root, source);
    const std::int64_t depth = read_int64(table, "max_depth", "presets",
                                          static_cast<std::int64_t>(presets.max_depth), source);
    if (depth > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        fail(source, "value out of range for 'presets.max_depth'");
    }
    presets.max_depth = static_cast<std::uint32_t>(depth);
}

void apply_goals(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "goals", {"max_rounds", "blocked_after_consecutive_rounds"}, source);
    GoalsSettings& goals = config.goals;
    const std::int64_t rounds =
        read_int64(table, "max_rounds", "goals", static_cast<std::int64_t>(goals.max_rounds), source);
    if (rounds < 1 ||
        rounds > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        fail(source, "value out of range for 'goals.max_rounds'");
    }
    goals.max_rounds = static_cast<std::uint32_t>(rounds);
    const std::int64_t blocked =
        read_int64(table, "blocked_after_consecutive_rounds", "goals",
                   static_cast<std::int64_t>(goals.blocked_after_consecutive_rounds), source);
    if (blocked < 1 ||
        blocked > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        fail(source, "value out of range for 'goals.blocked_after_consecutive_rounds'");
    }
    goals.blocked_after_consecutive_rounds = static_cast<std::uint32_t>(blocked);
}

void apply_jobs(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "jobs",
                   {"wait_timeout_ms", "max_wait_timeout_ms", "completion_delivery",
                    "max_consecutive_wakes"},
                   source);
    JobsSettings& jobs = config.jobs;

    const std::int64_t wait =
        read_int64(table, "wait_timeout_ms", "jobs", jobs.wait_timeout_ms, source);
    if (wait < 0) {
        fail(source, "value out of range for 'jobs.wait_timeout_ms'");
    }
    jobs.wait_timeout_ms = wait;

    const std::int64_t max_wait =
        read_int64(table, "max_wait_timeout_ms", "jobs", jobs.max_wait_timeout_ms, source);
    if (max_wait < 1) {
        fail(source, "value out of range for 'jobs.max_wait_timeout_ms'");
    }
    jobs.max_wait_timeout_ms = max_wait;

    const std::string delivery =
        read_string(table, "completion_delivery", "jobs",
                    std::string{completion_delivery_name(jobs.completion_delivery)}, source);
    const std::optional<CompletionDelivery> parsed_delivery = parse_completion_delivery(delivery);
    if (!parsed_delivery.has_value()) {
        fail(source, "unknown jobs.completion_delivery '" + delivery + "'");
    }
    jobs.completion_delivery = *parsed_delivery;

    const std::int64_t wakes =
        read_int64(table, "max_consecutive_wakes", "jobs",
                   static_cast<std::int64_t>(jobs.max_consecutive_wakes), source);
    if (wakes < 0 ||
        wakes > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        fail(source, "value out of range for 'jobs.max_consecutive_wakes'");
    }
    jobs.max_consecutive_wakes = static_cast<std::uint32_t>(wakes);
}

void apply_prompt(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "prompt", {"instructions"}, source);
    const Json* instructions = member(table, "instructions");
    if (instructions == nullptr) {
        return;
    }
    if (!instructions->is_object()) {
        fail(source, "invalid type for 'prompt.instructions'");
    }
    reject_unknown(*instructions, "prompt.instructions",
                   {"enabled", "max_bytes", "project_root_markers", "candidates", "local_candidates",
                    "load_local", "max_source_bytes"},
                   source);

    InstructionFileConfig& cfg = config.prompt.instructions;
    config.prompt.instructions_enabled =
        read_bool(*instructions, "enabled", "prompt.instructions",
                  config.prompt.instructions_enabled, source);
    if (member(*instructions, "max_bytes") != nullptr) {
        cfg.max_bytes = static_cast<std::size_t>(
            read_int64(*instructions, "max_bytes", "prompt.instructions", 0, source));
    }
    if (member(*instructions, "project_root_markers") != nullptr) {
        cfg.project_root_markers =
            read_string_array(*instructions, "project_root_markers", "prompt.instructions", source);
    }
    if (member(*instructions, "candidates") != nullptr) {
        cfg.candidates = read_string_array(*instructions, "candidates", "prompt.instructions", source);
    }
    if (member(*instructions, "local_candidates") != nullptr) {
        cfg.local_candidates =
            read_string_array(*instructions, "local_candidates", "prompt.instructions", source);
    }
    cfg.load_local = read_bool(*instructions, "load_local", "prompt.instructions", cfg.load_local,
                               source);
    if (member(*instructions, "max_source_bytes") != nullptr) {
        cfg.max_source_bytes = static_cast<std::size_t>(
            read_int64(*instructions, "max_source_bytes", "prompt.instructions",
                       static_cast<std::int64_t>(cfg.max_source_bytes), source));
    }
    if (config.prompt.instructions_enabled && cfg.max_bytes == 0) {
        fail(source,
             "prompt.instructions.max_bytes is required when prompt.instructions.enabled is true");
    }
}

void apply_tools(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "tools", {"presentation", "tool_order", "timeout_ms"}, source);
    const std::string presentation =
        read_string(table, "presentation", "tools",
                    std::string{tool_presentation_name(config.tools.presentation)}, source);
    if (presentation == "native") {
        config.tools.presentation = ToolPresentationMode::Native;
    } else if (presentation == "ptc" || presentation == "both") {
        fail(source, "tools.presentation '" + presentation +
                         "' is reserved and not implemented; only 'native' is supported");
    } else {
        fail(source, "unknown tools.presentation '" + presentation + "'");
    }

    config.tools.tool_order = read_string_array(table, "tool_order", "tools", source);
    if (!config.tools.tool_order.empty()) {
        std::set<std::string> named;
        std::size_t           rest = 0;
        for (const std::string& entry : config.tools.tool_order) {
            if (entry == "<unlisted-tools>") {
                ++rest;
                continue;
            }
            if (!named.insert(entry).second) {
                fail(source, "duplicate tools.tool_order entry '" + entry + "'");
            }
        }
        if (rest != 1) {
            fail(source, "tools.tool_order must contain <unlisted-tools> exactly once");
        }
    }
    config.tools.timeout_ms =
        read_int64(table, "timeout_ms", "tools", config.tools.timeout_ms, source);
}

void apply_document(Config& config, const Json& table, const std::filesystem::path& source,
                    bool global_layer) {
    reject_unknown(table, "",
                   {"ui", "agent", "workspace", "permissions", "logging", "llm", "mcp", "skills",
                    "session", "prompt", "tools", "presets", "goals", "jobs", "mcp_servers"},
                   source);

    const auto section = [&](std::string_view name) -> const Json* {
        const Json* nested = member(table, name);
        if (nested == nullptr) {
            return nullptr;
        }
        if (!nested->is_object()) {
            fail(source, "invalid type for '" + std::string{name} + "'");
        }
        return nested;
    };

    const Json* mcp_servers = nullptr;
    if (const Json* node = member(table, "mcp_servers"); node != nullptr) {
        if (!global_layer) {
            fail(source, "'mcp_servers' is global-layer only");
        }
        if (!node->is_object()) {
            fail(source, "invalid type for 'mcp_servers'");
        }
        if (const Json* mcp = member(table, "mcp");
            mcp != nullptr && mcp->is_object()) {
            if (const Json* server = member(*mcp, "server");
                server != nullptr && server->is_array() && !server->empty()) {
                fail(source, "define MCP servers in mcp_servers or mcp.server, not both");
            }
        }
        mcp_servers = node;
    }

    if (const Json* ui = section("ui"); ui != nullptr) {
        apply_ui(config, *ui, source);
    }
    if (const Json* agent = section("agent"); agent != nullptr) {
        apply_agent(config, *agent, source);
    }
    if (const Json* workspace = section("workspace"); workspace != nullptr) {
        apply_workspace(config, *workspace, source);
    }
    if (const Json* permissions = section("permissions"); permissions != nullptr) {
        apply_permissions(config, *permissions, source);
    }
    if (const Json* logging = section("logging"); logging != nullptr) {
        apply_logging(config, *logging, source);
    }
    if (const Json* llm = section("llm"); llm != nullptr) {
        apply_llm(config, *llm, source, global_layer);
    }
    if (const Json* mcp = section("mcp"); mcp != nullptr) {
        if (!global_layer) {
            fail(source, "'mcp' is global-layer only");
        }
        apply_mcp(config, *mcp, source);
    }
    if (const Json* skills = section("skills"); skills != nullptr) {
        apply_skills(config, *skills, source);
    }
    if (const Json* session = section("session"); session != nullptr) {
        if (!global_layer) {
            fail(source, "'session' is global-layer only");
        }
        apply_session(config, *session, source);
    }
    if (const Json* prompt = section("prompt"); prompt != nullptr) {
        apply_prompt(config, *prompt, source);
    }
    if (const Json* tools = section("tools"); tools != nullptr) {
        apply_tools(config, *tools, source);
    }
    if (const Json* presets = section("presets"); presets != nullptr) {
        apply_presets(config, *presets, source);
    }
    if (const Json* goals = section("goals"); goals != nullptr) {
        apply_goals(config, *goals, source);
    }
    if (const Json* jobs = section("jobs"); jobs != nullptr) {
        apply_jobs(config, *jobs, source);
    }
    if (mcp_servers != nullptr) {
        apply_mcp_servers_object(config.mcp, *mcp_servers, source);
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
// generated file loads to exactly the built-in defaults. Comments are `//`
// only and no trailing comma follows a live key (J15).
constexpr std::string_view kDefaultConfigJsonc =
    R"JSONC(// ymh configuration — created on first run.
//
// Layering (last writer wins):
//   built-in defaults -> this global file -> <workspace>/.ymh/config.jsonc
//   -> YMH_* environment variables -> command-line flags.
//
// Format: JSON with // and /* */ comments (JSONC). Trailing commas are NOT
// allowed. The loader is strict: an unknown key is an error, so add only real
// keys. Secrets are never stored here; "api_key_env" names the environment
// variable that holds the key.

{
  "ui": {
    "theme": "default",          // color theme name
    "show_activity": true,       // show the activity indicator
    "side_panel": "auto"         // "auto" | "always" | "never"
  },
  "agent": {
    "model": "",                 // empty => use llm.default.model
    "max_steps": 100             // max tool-calling steps per task
    // optional: "reasoning_effort": "low"   // "low" | "medium" | "high" | "xhigh"
    // optional: "system_prompt": ""         // empty => built-in default
  },
  "workspace": {
    "root": "."                  // workspace root (relative to the process cwd)
    // optional: "workspace_roots": []       // discovery roots for workspace selection
  },
  "permissions": {
    "shell": "ask",              // "allow" | "ask" | "deny"
    "write": "ask",
    "read": "allow"
  },
  "logging": {
    "level": "info",             // "debug" | "info" | "warn" | "error"
    "log_prompts": false         // never enable implicitly; prompt bodies are redacted
  },
  "llm": {
    "default": {
      "provider": "openai-compatible",
      "base_url": "https://api.deepseek.com/v1",
      "model": "deepseek-flash",
      "api_key_env": "DEEPSEEK_API_KEY",
      // optional: "profile": ""              // "" = no profile
      // optional: "temperature": 1.0         // 0.0 .. 2.0
      // optional: "top_p": 0.95              // (0.0, 1.0]
      // optional: "top_k": 64                // 0 = omit; otherwise a positive integer
      // optional: "tool_choice": "auto"      // "auto" | "none" | "required" | <function name>
      // optional: "stop": []                 // array of stop strings
      // optional: "seed": 0                  // [0, 2^32)
      // optional: "reasoning_effort": "low"   // "low" | "medium" | "high" | "xhigh"
      "max_concurrency": 4,
      "connect_timeout_ms": 10000,
      "idle_timeout_ms": 60000,
      "request_timeout_ms": 120000,
      "retry": {
        "max_attempts": 3,
        "base_delay_ms": 500,
        "max_delay_ms": 30000,
        "jitter": 0.25,
        "honor_retry_after": true
      }
    }
  }
}
)JSONC";

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
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return true;
        }
        scaffold_warn(logger, "config: cannot write '" + file.string() + "'");
        return false;
    }
    const std::string body{kDefaultConfigJsonc};
    bool              ok   = true;
    std::size_t       written = 0;
    while (written < body.size()) {
        const ssize_t count = ::write(fd, body.data() + written, body.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        scaffold_warn(logger, "config: cannot write '" + file.string() + "'");
        (void)::unlink(file.c_str());
        return false;
    }
    created = true;
    return true;
}

} // namespace

void apply_mcp_servers_object(McpSettings& mcp, const Json& table,
                              const std::filesystem::path& source) {
    if (!table.is_object()) {
        fail(source, "invalid type for 'mcp_servers'");
    }

    std::vector<std::pair<std::string, std::string>> assigned;
    assigned.reserve(table.size());
    std::set<std::string> used;
    for (auto it = table.begin(); it != table.end(); ++it) {
        const std::string& key = it.key();
        if (!is_valid_localcode_server_name(key)) {
            fail(source, "invalid mcp server name '" + key + "'");
        }
        const std::string base = normalize_mcp_server_id(key);
        std::string       id   = base;
        if (used.count(id) != 0) {
            id.clear();
            for (std::size_t n = 2; n <= kMaxMcpDedupeAttempts; ++n) {
                const std::string suffix    = "_" + std::to_string(n);
                const std::size_t room      = suffix.size() < 32 ? 32 - suffix.size() : 0;
                std::string       candidate = base.substr(0, room) + suffix;
                if (used.count(candidate) == 0) {
                    id = std::move(candidate);
                    break;
                }
            }
            if (id.empty()) {
                fail(source, "mcp_servers: too many id collisions for '" + key + "'");
            }
        }
        used.insert(id);
        assigned.emplace_back(key, std::move(id));
    }

    mcp.servers.clear();
    for (const auto& [key, id] : assigned) {
        const Json& entry = table.at(key);
        if (!entry.is_object()) {
            fail(source, "invalid type for mcp_servers.'" + key + "'");
        }
        McpServerSettings server;
        server.id = id;
        apply_mcp_servers_entry(server, entry, key, source);
        mcp.servers.push_back(std::move(server));
    }
}

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

std::filesystem::path localcode_config_path() {
    if (const std::optional<std::string> home = env_value("HOME"); home.has_value()) {
        return std::filesystem::path{*home} / ".localcode" / "config.json";
    }
    return std::filesystem::path{".localcode"} / "config.json";
}

ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                               const std::filesystem::path& global_config,
                               Logger* logger) {
    ScaffoldResult result;
    result.global_config = global_config;
    result.workspace_dir = workspace_root / ".ymh";

    if (!global_config.empty()) {  // NEW (J-C12): a deliberate empty target skips the write
        if (ensure_directory(global_config.parent_path(), logger, result.global_dir_created)) {
            if (!write_default_config(global_config, logger, result.global_config_created)) {
                result.ok = false;
            }
        } else {
            result.ok = false;
        }
    }

    if (!ensure_directory(result.workspace_dir, logger, result.workspace_dir_created)) {
        result.ok = false;
    }

    return result;
}

ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root, Logger* logger) {
    return scaffold_config(workspace_root, default_global_config_path(), logger);
}

void apply_jsonc_file(Config& config, const std::filesystem::path& path, bool required) {
    if (path.empty()) {
        if (required) {
            throw ConfigError("config: required global config path is empty");
        }
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (required) {
            fail(path, "required global config not found");
        }
        return;
    }
    if (!std::filesystem::is_regular_file(path, error)) {
        fail(path, "config path is not a regular file");
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        fail(path, "cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    const Json document = parse_jsonc(path, text);
    if (!document.is_object()) {
        fail(path, "top-level value must be an object");
    }
    apply_document(config, document, path, required);
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
    if (const auto value = env_value("YMH_LLM_ACTIVE_MODEL")) {
        config.llm.active_model = *value;
    }
    if (const auto value = env_value("YMH_LLM_ENDPOINT")) {
        config.llm.active_endpoint = *value;
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
    apply_jsonc_file(config, paths.global, /*required=*/true);
    apply_jsonc_file(config, paths.workspace, /*required=*/false);
    apply_env_overrides(config);
    if (!config.llm.profile.empty() && !is_known_model_profile(config.llm.profile)) {
        throw ConfigError("unknown llm.default.profile: " + config.llm.profile);
    }
    for (const auto& [name, model] : config.llm.models) {
        if (config.llm.endpoints.find(model.endpoint) == config.llm.endpoints.end()) {
            throw ConfigError("unknown endpoint '" + model.endpoint + "' for model '" + name + "'");
        }
        if (!model.profile.empty() && !is_known_model_profile(model.profile)) {
            throw ConfigError("unknown llm.models." + name + ".profile: " + model.profile);
        }
    }
    if (config.llm.active_model.has_value() && !config.llm.active_model->empty() &&
        config.llm.models.find(*config.llm.active_model) == config.llm.models.end()) {
        throw ConfigError("unknown llm.active_model: " + *config.llm.active_model);
    }
    if (config.llm.active_endpoint.has_value() && !config.llm.active_endpoint->empty() &&
        config.llm.endpoints.find(*config.llm.active_endpoint) == config.llm.endpoints.end()) {
        throw ConfigError("unknown endpoint '" + *config.llm.active_endpoint + "'");
    }
    return config;
}

Config load_config(const std::filesystem::path& workspace_root) {
    ConfigPaths paths;
    paths.global    = default_global_config_path();
    paths.workspace = workspace_config_path(workspace_root);
    return load_config(paths);
}

namespace {

std::string escape_localcode_reference(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    std::size_t i = 0;
    while (i < value.size()) {
        if (value[i] == '$' && i + 1 < value.size() && value[i + 1] == '{') {
            const std::size_t name_start = i + 2;
            const bool        starts_name =
                name_start < value.size() &&
                ((value[name_start] >= 'A' && value[name_start] <= 'Z') ||
                 (value[name_start] >= 'a' && value[name_start] <= 'z') ||
                 value[name_start] == '_');
            if (starts_name) {
                std::size_t end = name_start + 1;
                while (end < value.size() && ((value[end] >= 'A' && value[end] <= 'Z') ||
                                              (value[end] >= 'a' && value[end] <= 'z') ||
                                              (value[end] >= '0' && value[end] <= '9') ||
                                              value[end] == '_')) {
                    ++end;
                }
                if (end < value.size() && value[end] == '}') {
                    result.append(value.substr(i, end + 1 - i));
                    i = end + 1;
                    continue;
                }
            }
            result += "$${";
            i += 2;
            continue;
        }
        result.push_back(value[i]);
        ++i;
    }
    return result;
}

bool is_http_url(std::string_view url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::optional<std::int64_t> read_non_negative_integer(const Json& value) {
    if (value.is_number_unsigned()) {
        const std::uint64_t raw = value.get<std::uint64_t>();
        if (raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(raw);
        }
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        const std::int64_t raw = value.get<std::int64_t>();
        if (raw >= 0) {
            return raw;
        }
    }
    return std::nullopt;
}

std::optional<std::string> map_localcode_tool(const std::string& tool) {
    static constexpr std::string_view kKnown[] = {
        "read_file", "write_file", "edit_file", "grep", "glob", "shell", "terminal"};
    if (tool == "bash") {
        return std::string{"shell"};
    }
    for (std::string_view known : kKnown) {
        if (tool == known) {
            return tool;
        }
    }
    return std::nullopt;
}

bool is_valid_localcode_decision(const std::string& decision) {
    return decision == "allow" || decision == "ask" || decision == "deny";
}

void copy_mcp_string(const Json& entry, std::string_view key, Json& out) {
    const Json* node = member(entry, key);
    if (node != nullptr && node->is_string()) {
        out[std::string{key}] = escape_localcode_reference(node->get<std::string>());
    }
}

void copy_mcp_string_array(const Json& entry, std::string_view key, Json& out) {
    const Json* node = member(entry, key);
    if (node == nullptr || !node->is_array()) {
        return;
    }
    Json array = Json::array();
    for (const Json& element : *node) {
        if (!element.is_string()) {
            throw LocalcodeImportError("'" + std::string{key} + "' entries must be strings");
        }
        array.push_back(escape_localcode_reference(element.get<std::string>()));
    }
    out[std::string{key}] = std::move(array);
}

void copy_mcp_string_object(const Json& entry, std::string_view key, Json& out) {
    const Json* node = member(entry, key);
    if (node == nullptr || !node->is_object()) {
        return;
    }
    Json object = Json::object();
    for (auto it = node->begin(); it != node->end(); ++it) {
        if (!it.value().is_string()) {
            throw LocalcodeImportError("'" + std::string{key} + "." + it.key() +
                                       "' must be a string");
        }
        object[it.key()] = escape_localcode_reference(it.value().get<std::string>());
    }
    out[std::string{key}] = std::move(object);
}

Json map_localcode_mcp_server(const Json& entry) {
    Json mapped = Json::object();
    copy_mcp_string(entry, "type", mapped);
    copy_mcp_string(entry, "transport", mapped);
    copy_mcp_string(entry, "command", mapped);
    copy_mcp_string_array(entry, "args", mapped);
    copy_mcp_string_object(entry, "env", mapped);
    copy_mcp_string_object(entry, "headers", mapped);
    copy_mcp_string(entry, "url", mapped);
    copy_mcp_string(entry, "cwd", mapped);
    copy_mcp_string_array(entry, "header_env", mapped);
    copy_mcp_string(entry, "protocol_version", mapped);
    if (const Json* enabled = member(entry, "enabled");
        enabled != nullptr && enabled->is_boolean()) {
        mapped["enabled"] = enabled->get<bool>();
    }
    copy_mcp_string_array(entry, "allowed_tools", mapped);
    copy_mcp_string_array(entry, "denied_tools", mapped);
    copy_mcp_string(entry, "default_verdict", mapped);
    if (const Json* timeout = member(entry, "call_timeout_ms"); timeout != nullptr) {
        if (const auto value = read_non_negative_integer(*timeout); value.has_value()) {
            mapped["call_timeout_ms"] = *value;
        }
    }
    if (const Json* max_bytes = member(entry, "max_result_bytes"); max_bytes != nullptr) {
        if (const auto value = read_non_negative_integer(*max_bytes); value.has_value()) {
            mapped["max_result_bytes"] = *value;
        }
    }
    mapped["required"] = false;
    return mapped;
}

} // namespace

std::optional<LocalcodeImportResult> build_localcode_import(const Json& localcode,
                                                            std::string& error) {
    error.clear();
    if (!localcode.is_object()) {
        error = "localcode config must be a JSON object";
        return std::nullopt;
    }

    LocalcodeImportResult result;
    Json&                document = result.document;
    std::vector<std::string>& notes = result.notes;
    document = Json::object();

    if (const Json* servers = member(localcode, "mcp_servers"); servers != nullptr) {
        if (servers->is_object()) {
            Json mapped = Json::object();
            for (auto it = servers->begin(); it != servers->end(); ++it) {
                if (!it.value().is_object()) {
                    notes.push_back("localcode mcp server '" + it.key() +
                                    "' is not an object; skipped");
                    continue;
                }
                try {
                    mapped[it.key()] = map_localcode_mcp_server(it.value());
                } catch (const LocalcodeImportError& failure) {
                    notes.push_back("localcode mcp server '" + it.key() + "' skipped: " +
                                    failure.what());
                }
            }
            if (!mapped.empty()) {
                document["mcp_servers"] = std::move(mapped);
            }
        }
    }

    if (const Json* enabled = member(localcode, "auto_compact_enabled");
        enabled != nullptr && enabled->is_boolean()) {
        document["agent"]["compaction"]["enabled"] = enabled->get<bool>();
    }
    if (const Json* percent = member(localcode, "auto_compact_percent");
        percent != nullptr && percent->is_number()) {
        double ratio = percent->get<double>() / 100.0;
        if (!std::isfinite(ratio)) {
            ratio = 1.0;
        }
        document["agent"]["compaction"]["threshold_ratio"] =
            std::clamp(ratio, std::numeric_limits<double>::min(), 1.0);
    }
    if (const Json* tasks = member(localcode, "max_concurrent_tasks"); tasks != nullptr) {
        if (const auto value = read_non_negative_integer(*tasks);
            value.has_value() && *value > 0) {
            document["llm"]["default"]["max_concurrency"] = *value;
        }
    }

    if (const Json* skip = member(localcode, "skip_permissions");
        skip != nullptr && skip->is_boolean() && skip->get<bool>()) {
        document["permissions"]["default"] = "allow";
    }

    if (const Json* permission = member(localcode, "permission");
        permission != nullptr && permission->is_array() && !permission->empty()) {
        notes.push_back("localcode 'permission' is a flat array, not the supported object shape; "
                        "permission rules are not imported");
    }

    if (const Json* permission = member(localcode, "permission");
        permission != nullptr && permission->is_object()) {
        Json rules = Json::array();
        for (auto it = permission->begin(); it != permission->end(); ++it) {
            const std::optional<std::string> mapped = map_localcode_tool(it.key());
            if (!mapped.has_value() || !it.value().is_array()) {
                continue;
            }
            std::size_t index = 0;
            for (const Json& entry : it.value()) {
                const Json* match    = entry.is_object() ? member(entry, "match") : nullptr;
                const Json* decision = entry.is_object() ? member(entry, "decision") : nullptr;
                if (match != nullptr && match->is_string() && decision != nullptr &&
                    decision->is_string() &&
                    is_valid_localcode_decision(decision->get<std::string>())) {
                    Json rule;
                    rule["tool"]    = *mapped;
                    rule["command"] = match->get<std::string>();
                    rule["effect"]  = decision->get<std::string>();
                    rule["id"]      = "localcode.rule." + it.key() + "." + std::to_string(index);
                    rules.push_back(std::move(rule));
                }
                ++index;
            }
        }
        if (!rules.empty()) {
            document["permissions"]["rules"] = std::move(rules);
        }
    }

    const Json*           providers = member(localcode, "providers");
    std::set<std::string> imported_endpoints;
    if (providers != nullptr && providers->is_object()) {
        Json endpoints = Json::object();
        for (auto it = providers->begin(); it != providers->end(); ++it) {
            const std::string name = it.key();
            if (!it.value().is_object()) {
                notes.push_back("localcode provider '" + name + "' is not an object; skipped");
                continue;
            }
            const Json& provider = it.value();
            const Json* type     = member(provider, "type");
            const std::string type_name =
                (type != nullptr && type->is_string()) ? type->get<std::string>() : std::string{};
            if (type_name != "openai-compatible" && type_name != "openai-compat") {
                notes.push_back("localcode provider '" + name + "' type '" + type_name +
                                "' is not supported; endpoint skipped");
                continue;
            }
            const Json*       base_url = member(provider, "base_url");
            const std::string url =
                (base_url != nullptr && base_url->is_string()) ? base_url->get<std::string>()
                                                               : std::string{};
            if (!is_http_url(url)) {
                notes.push_back("localcode provider '" + name +
                                "' base_url is not an http(s) URL; endpoint skipped");
                continue;
            }
            Json entry        = Json::object();
            entry["provider"] = "openai-compatible";
            entry["base_url"] = url;
            const Json*       key     = member(provider, "api_key");
            const std::string api_key =
                (key != nullptr && key->is_string()) ? key->get<std::string>() : std::string{};
            if (!api_key.empty()) {
                entry["api_key"] = api_key;
            } else {
                const Json*       env         = member(provider, "api_key_env");
                const std::string api_key_env =
                    (env != nullptr && env->is_string()) ? env->get<std::string>() : std::string{};
                if (!api_key_env.empty()) {
                    entry["api_key_env"] = api_key_env;
                } else {
                    notes.push_back("localcode provider '" + name +
                                    "' has no api_key or api_key_env; imported as a keyless "
                                    "endpoint");
                }
            }
            endpoints[name] = std::move(entry);
            imported_endpoints.insert(name);
        }
        if (!endpoints.empty()) {
            document["llm"]["endpoints"] = std::move(endpoints);
        }
    }

    const Json*           profiles = member(localcode, "profiles");
    std::set<std::string> imported_models;
    if (profiles != nullptr && profiles->is_object()) {
        Json models = Json::object();
        for (auto it = profiles->begin(); it != profiles->end(); ++it) {
            const std::string name = it.key();
            if (!it.value().is_object()) {
                notes.push_back("localcode profile '" + name + "' is not an object; skipped");
                continue;
            }
            const Json&       profile  = it.value();
            const Json*       model    = member(profile, "model");
            const std::string model_id =
                (model != nullptr && model->is_string()) ? model->get<std::string>() : std::string{};
            if (model_id.empty()) {
                notes.push_back("localcode profile '" + name + "' has no model; entry skipped");
                continue;
            }
            const Json*       provider_name = member(profile, "provider");
            const std::string endpoint =
                (provider_name != nullptr && provider_name->is_string())
                    ? provider_name->get<std::string>()
                    : std::string{};
            if (endpoint.empty() || imported_endpoints.find(endpoint) == imported_endpoints.end()) {
                notes.push_back("localcode profile '" + name + "' references provider '" + endpoint +
                                "' which was not imported; entry skipped");
                continue;
            }
            Json entry          = Json::object();
            entry["endpoint"]   = endpoint;
            entry["model"]      = model_id;
            if (const Json* tokens = member(profile, "max_tokens"); tokens != nullptr) {
                if (const auto value = read_non_negative_integer(*tokens);
                    value.has_value() && *value > 0) {
                    entry["max_tokens"] = *value;
                }
            }
            if (const Json* window = member(profile, "context_window"); window != nullptr) {
                if (const auto value = read_non_negative_integer(*window); value.has_value()) {
                    entry["context_window"] = *value;
                }
            }
            const std::string_view profile_id = import_profile_id_for_model(model_id);
            if (!profile_id.empty()) {
                entry["profile"] = std::string{profile_id};
            }
            models[name] = std::move(entry);
            imported_models.insert(name);
        }
        if (!models.empty()) {
            document["llm"]["models"] = std::move(models);
        }
    }

    if (const Json* default_profile = member(localcode, "default_profile");
        default_profile != nullptr && default_profile->is_string()) {
        const std::string profile_name = default_profile->get<std::string>();
        if (!profile_name.empty() && imported_models.find(profile_name) != imported_models.end()) {
            document["llm"]["active_model"] = profile_name;
        }
    }

    return result;
}

std::string effective_model(const Config& config) {
    return resolve_model(config).model_id;
}

namespace {

ResolvedEndpoint resolve_named_endpoint(const Config& config, const std::string& name) {
    const auto it = config.llm.endpoints.find(name);
    if (it == config.llm.endpoints.end()) {
        throw ConfigError("unknown endpoint '" + name + "'");
    }
    const EndpointSettings& settings = it->second;
    ResolvedEndpoint        resolved;
    resolved.name            = name;
    resolved.provider        = settings.provider;
    resolved.base_url        = settings.base_url;
    resolved.api_key_env     = settings.api_key_env;
    resolved.api_key         = settings.api_key;
    resolved.headers         = settings.headers;
    resolved.connect_timeout = settings.connect_timeout;
    resolved.idle_timeout    = settings.idle_timeout;
    resolved.request_timeout = settings.request_timeout;
    resolved.retry           = settings.retry;
    resolved.max_concurrency = settings.max_concurrency;
    return resolved;
}

ResolvedEndpoint default_endpoint(const Config& config) {
    ResolvedEndpoint resolved;
    resolved.name            = "";
    resolved.provider        = config.llm.provider;
    resolved.base_url        = config.llm.base_url;
    resolved.api_key_env     = config.llm.api_key_env;
    resolved.api_key         = config.llm.api_key;
    resolved.connect_timeout = config.llm.connect_timeout;
    resolved.idle_timeout    = config.llm.idle_timeout;
    resolved.request_timeout = config.llm.request_timeout;
    resolved.retry           = config.llm.retry;
    resolved.max_concurrency = config.llm.max_concurrency;
    return resolved;
}

ModelProfile resolved_profile(const std::string& id) {
    if (const ModelProfile* profile = find_model_profile(id); profile != nullptr) {
        return *profile;
    }
    return ModelProfile{};
}

void fill_default_model(ResolvedModel& out, const Config& config, const std::string& model_id,
                        const std::string& source) {
    out.endpoint         = default_endpoint(config);
    out.model_name       = "";
    out.model_id         = model_id;
    out.max_tokens       = config.llm.max_tokens;
    out.context_window   = std::nullopt;
    out.reasoning_effort = config.llm.reasoning_effort;
    out.profile          = resolved_profile(config.llm.profile);
    out.temperature      = config.llm.temperature;
    out.top_p            = config.llm.top_p;
    out.top_k            = config.llm.top_k;
    out.tool_choice      = config.llm.tool_choice;
    out.stop             = config.llm.stop;
    out.seed             = config.llm.seed;
    out.source           = source;
}

void fill_named_model(ResolvedModel& out, const Config& config, const std::string& name,
                      const std::string& source) {
    const auto it = config.llm.models.find(name);
    if (it == config.llm.models.end()) {
        throw ConfigError("unknown model '" + name + "'");
    }
    const ModelSettings& model = it->second;
    out.endpoint         = resolve_named_endpoint(config, model.endpoint);
    out.model_name       = name;
    out.model_id         = model.model;
    out.max_tokens       = model.max_tokens;
    out.context_window   = model.context_window;
    out.reasoning_effort = model.reasoning_effort;
    out.profile          = resolved_profile(model.profile);
    out.temperature      = model.temperature;
    out.top_p            = model.top_p;
    out.top_k            = model.top_k;
    out.tool_choice      = model.tool_choice;
    out.stop             = model.stop;
    out.seed             = model.seed;
    out.source           = source;
}

} // namespace

ResolvedModel resolve_model(const Config& config) {
    ResolvedModel resolved;
    if (!config.agent.model.empty()) {
        if (config.llm.models.find(config.agent.model) != config.llm.models.end()) {
            fill_named_model(resolved, config, config.agent.model, "agent.model");
        } else {
            fill_default_model(resolved, config, config.agent.model, "agent.model");
        }
    } else if (config.llm.active_model.has_value() && !config.llm.active_model->empty()) {
        fill_named_model(resolved, config, *config.llm.active_model, "llm.active_model");
    } else if (!config.llm.model.empty()) {
        fill_default_model(resolved, config, config.llm.model, "llm.default.model");
    } else {
        fill_default_model(resolved, config, "deepseek-flash", "builtin");
    }

    if (resolved.model_name.empty() && config.llm.active_endpoint.has_value() &&
        !config.llm.active_endpoint->empty()) {
        resolved.endpoint = resolve_named_endpoint(config, *config.llm.active_endpoint);
    }
    return resolved;
}

} // namespace ymh

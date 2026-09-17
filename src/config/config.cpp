#include "ymh/config/config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/logger.hpp"

namespace ymh {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kGlobalDir       = "ymh";
constexpr std::string_view kConfigFile      = "config.jsonc";
constexpr std::string_view kLegacyConfigFile = "config.toml";

[[noreturn]] void fail(const std::filesystem::path& source, std::string_view detail) {
    throw ConfigError("config " + source.string() + ": " + std::string{detail});
}

// True iff `text` contains only whitespace and `//` / `/* */` comments (no JSON
// token at all), ignoring a single optional leading UTF-8 BOM. This needs no
// string-literal handling: any non-whitespace byte that is not a comment opener
// makes it return false before any string is entered. Used so an empty,
// comments-only, or BOM-only config is a no-op `{}`, matching the retired TOML
// loader where an empty file parsed to an empty table (J-L2).
bool blank_or_comments_only(std::string_view text) {
    std::size_t i = 0;
    // A leading UTF-8 BOM (EF BB BF) is not a JSON token: skip it so BOM-only,
    // BOM+whitespace and BOM+comments files are blank (the "BOM accepted" rule,
    // §4.3). Only at byte 0; a BOM anywhere else is left to `parse`, which
    // rejects it.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }
    while (i < text.size()) {
        const char c = text[i];
        // RFC 8259 whitespace only: space, tab, LF, CR. `\f` (0x0C) and `\v`
        // (0x0B) are NOT JSON whitespace, so a file containing only them is not
        // blank and reaches `parse`, which rejects it (J-F1) instead of being
        // silently no-op'd. A literal `{}` is likewise not blank: `{` is not
        // whitespace, so it is parsed normally.
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i += 2;
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                ++i;
            }
            if (i + 1 >= text.size()) {
                return false;  // unterminated block comment is malformed, not blank
            }
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

// Byte offset -> 1-based line, for the error message.
std::size_t line_for_byte(std::string_view text, std::size_t byte) {
    const std::size_t end = std::min(byte, text.size());
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + end, '\n'));
}

// Parse a JSONC document with comments tolerated. Exceptions are enabled so the
// caller can report line/column (parity with the retired TOML path).
Json parse_jsonc(const std::filesystem::path& path, std::string_view text) {
    if (blank_or_comments_only(text)) {
        return Json::object();  // empty / comments-only => no-op (J-L2)
    }
    try {
        return Json::parse(text,
                           /*cb=*/nullptr,
                           /*allow_exceptions=*/true,
                           /*ignore_comments=*/true);
    } catch (const Json::parse_error& error) {
        std::ostringstream message;
        message << error.what() << " (line " << line_for_byte(text, error.byte) << ')';
        fail(path, message.str());
    } catch (const std::exception& other) {
        fail(path, other.what());
    }
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
            fail(source, "unknown key '" + std::string{table_name} + "." + std::string{name} + "'");
        }
    }
}

const Json* member(const Json& obj, std::string_view key) {
    const auto it = obj.find(std::string{key});
    return it == obj.end() ? nullptr : &(*it);
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

void apply_agent(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "agent",
                   {"model", "max_steps", "reasoning_effort", "system_prompt", "compaction",
                    "compaction_threshold_tokens"},
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
    reject_unknown(table, "permissions", {"shell", "write", "read"}, source);
    config.permissions.shell =
        read_string(table, "shell", "permissions", config.permissions.shell, source);
    config.permissions.write =
        read_string(table, "write", "permissions", config.permissions.write, source);
    config.permissions.read =
        read_string(table, "read", "permissions", config.permissions.read, source);
}

void apply_logging(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "logging", {"level", "log_prompts"}, source);
    config.logging.level = read_string(table, "level", "logging", config.logging.level, source);
    config.logging.log_prompts =
        read_bool(table, "log_prompts", "logging", config.logging.log_prompts, source);
}

void apply_retry(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "llm.default.retry",
                   {"max_attempts", "base_delay_ms", "max_delay_ms", "jitter", "honor_retry_after"},
                   source);
    config.llm.retry.max_attempts = static_cast<std::uint32_t>(read_int64(
        table, "max_attempts", "llm.default.retry",
        static_cast<std::int64_t>(config.llm.retry.max_attempts), source));
    config.llm.retry.base_delay = std::chrono::milliseconds{read_int64(
        table, "base_delay_ms", "llm.default.retry", config.llm.retry.base_delay.count(), source)};
    config.llm.retry.max_delay = std::chrono::milliseconds{read_int64(
        table, "max_delay_ms", "llm.default.retry", config.llm.retry.max_delay.count(), source)};
    config.llm.retry.jitter =
        read_double(table, "jitter", "llm.default.retry", config.llm.retry.jitter, source);
    config.llm.retry.honor_retry_after = read_bool(
        table, "honor_retry_after", "llm.default.retry", config.llm.retry.honor_retry_after, source);
}

void apply_llm(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "llm",
                   {"default", "provider", "base_url", "model", "api_key_env", "reasoning_effort",
                    "max_concurrency", "connect_timeout_ms", "idle_timeout_ms", "request_timeout_ms",
                    "retry"},
                   source);

    const Json* section = &table;
    if (const Json* nested = member(table, "default"); nested != nullptr) {
        if (!nested->is_object()) {
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
                    "allow_network_servers", "server"},
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

void apply_document(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "",
                   {"ui", "agent", "workspace", "permissions", "logging", "llm", "mcp", "skills"},
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
        apply_llm(config, *llm, source);
    }
    if (const Json* mcp = section("mcp"); mcp != nullptr) {
        apply_mcp(config, *mcp, source);
    }
    if (const Json* skills = section("skills"); skills != nullptr) {
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
    // optional: "reasoning_effort": "low"   // "low" | "medium" | "high"
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
      // optional: "reasoning_effort": "low"   // "low" | "medium" | "high"
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
    std::ofstream output{file, std::ios::binary | std::ios::trunc};
    if (!output) {
        scaffold_warn(logger, "config: cannot write '" + file.string() + "'");
        return false;
    }
    output << kDefaultConfigJsonc;
    output.flush();
    if (!output) {
        scaffold_warn(logger, "config: cannot write '" + file.string() + "'");
        return false;
    }
    created = true;
    return true;
}

} // namespace

std::filesystem::path legacy_config_path(const std::filesystem::path& jsonc_path) {
    if (jsonc_path.filename() == kLegacyConfigFile) {
        return jsonc_path;  // itself legacy
    }
    if (jsonc_path.filename() == kConfigFile) {
        return jsonc_path.parent_path() / kLegacyConfigFile;  // conventional sibling
    }
    return {};  // custom basename: no sibling (V3-6)
}

std::filesystem::path jsonc_target(const std::filesystem::path& slot) {
    return slot.filename() == kLegacyConfigFile ? slot.parent_path() / kConfigFile : slot;
}

std::filesystem::path scaffold_target(const std::filesystem::path& slot) {
    return slot.filename() == kLegacyConfigFile ? std::filesystem::path{} : slot;
}

namespace {

struct LegacyFile {
    std::filesystem::path legacy;                  // the existing config.toml
    std::filesystem::path target;                  // the JSONC path to convert to
    bool                  slot_is_legacy = false;  // slot itself named config.toml
};

// Never throws. Returns 0, 1, or 2 entries (global + workspace slots).
std::vector<LegacyFile> collect_legacy(const ConfigPaths& paths) {
    std::vector<LegacyFile> found;
    for (const std::filesystem::path& slot : {paths.global, paths.workspace}) {
        if (slot.empty()) {
            continue;
        }
        const std::filesystem::path legacy = legacy_config_path(slot);
        if (legacy.empty()) {
            continue;  // custom-named slot: never probe a sibling (V3-6)
        }
        std::error_code error;
        if (std::filesystem::exists(legacy, error) && !error) {
            found.push_back(LegacyFile{legacy, jsonc_target(slot),
                                       slot.filename() == kLegacyConfigFile});
        }
    }
    return found;
}

void warn_legacy(const std::vector<LegacyFile>& legacy, Logger* logger) {
    if (logger == nullptr || legacy.empty()) {
        return;
    }
    std::ostringstream message;
    message << "config: ignoring legacy TOML file(s); ymh reads JSONC only.\n";
    for (const LegacyFile& entry : legacy) {
        message << "  " << entry.legacy.string() << "  ->  " << entry.target.string();
        if (entry.slot_is_legacy) {
            message << "   (explicit --config slot: update --config)";
        }
        message << '\n';
    }
    message << "convert to JSONC (// and /* */ comments; # is not a comment; "
               "trailing commas are NOT allowed). this file is not read. For a "
               "conventional slot, ymh uses the config.jsonc beside it when "
               "present, otherwise built-in defaults for that layer. An explicitly "
               "named --config slot is not substituted: convert the file and point "
               "--config at the JSONC path.";
    logger->warn(message.str());
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

void apply_jsonc_file(Config& config, const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return;
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
    apply_document(config, document, path);
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

Config load_config(const ConfigPaths& paths, Logger* logger) {
    Config config;
    warn_legacy(collect_legacy(paths), logger);  // once per call (21-D4)

    const auto loadable = [](const std::filesystem::path& p) {
        return !p.empty() && p.filename() != kLegacyConfigFile;
    };
    if (loadable(paths.global)) {
        apply_jsonc_file(config, paths.global);
    }
    if (loadable(paths.workspace)) {
        apply_jsonc_file(config, paths.workspace);
    }
    apply_env_overrides(config);
    return config;
}

Config load_config(const std::filesystem::path& workspace_root, Logger* logger) {
    ConfigPaths paths;
    paths.global    = default_global_config_path();
    paths.workspace = workspace_config_path(workspace_root);
    return load_config(paths, logger);
}

std::string effective_model(const Config& config) {
    if (!config.agent.model.empty()) {
        return config.agent.model;
    }
    return config.llm.model;
}

} // namespace ymh

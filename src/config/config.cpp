#include "ymh/config/config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/logger.hpp"
#include "ymh/mcp/mcp_types.hpp"

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

void apply_session(Config& config, const Json& table, const std::filesystem::path& source) {
    reject_unknown(table, "session", {"persist_prompt_text"}, source);
    config.session.persist_prompt_text = read_bool(
        table, "persist_prompt_text", "session", config.session.persist_prompt_text, source);
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
    reject_unknown(table, "tools", {"presentation", "tool_order"}, source);
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
}

void apply_document(Config& config, const Json& table, const std::filesystem::path& source,
                    bool global_layer) {
    reject_unknown(table, "",
                   {"ui", "agent", "workspace", "permissions", "logging", "llm", "mcp", "skills",
                    "session", "prompt", "tools", "presets", "mcp_servers"},
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
        apply_llm(config, *llm, source);
    }
    if (const Json* mcp = section("mcp"); mcp != nullptr) {
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

std::optional<Json> build_localcode_import(const Json& localcode, std::string& error) {
    error.clear();
    if (!localcode.is_object()) {
        error = "localcode config must be a JSON object";
        return std::nullopt;
    }

    Json document = Json::object();

    if (const Json* servers = member(localcode, "mcp_servers"); servers != nullptr) {
        if (servers->is_object()) {
            Json mapped = Json::object();
            for (auto it = servers->begin(); it != servers->end(); ++it) {
                if (it.value().is_object()) {
                    try {
                        mapped[it.key()] = map_localcode_mcp_server(it.value());
                    } catch (const LocalcodeImportError& failure) {
                        error = "mcp_servers." + it.key() + ": " + failure.what();
                        return std::nullopt;
                    }
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

    const Json* default_profile = member(localcode, "default_profile");
    if (default_profile != nullptr && default_profile->is_string()) {
        const std::string profile_name = default_profile->get<std::string>();
        const Json*       profiles     = member(localcode, "profiles");
        const Json*       profile      = nullptr;
        if (profiles != nullptr && profiles->is_object()) {
            if (auto it = profiles->find(profile_name);
                it != profiles->end() && it->is_object()) {
                profile = &(*it);
            }
        }

        const Json* provider = nullptr;
        if (profile != nullptr) {
            const Json* provider_name = member(*profile, "provider");
            const Json* providers     = member(localcode, "providers");
            if (provider_name != nullptr && provider_name->is_string() && providers != nullptr &&
                providers->is_object()) {
                if (auto it = providers->find(provider_name->get<std::string>());
                    it != providers->end() && it->is_object()) {
                    provider = &(*it);
                }
            }
        }

        bool openai_compatible = false;
        if (provider != nullptr) {
            const Json* type = member(*provider, "type");
            openai_compatible = type != nullptr && type->is_string() &&
                                type->get<std::string>() == "openai-compatible";
        }
        if (openai_compatible) {
            if (const Json* window = member(*profile, "context_window"); window != nullptr) {
                if (const auto value = read_non_negative_integer(*window); value.has_value()) {
                    document["agent"]["compaction"]["context_window_tokens"] = *value;
                }
            }
            const Json* base_url = member(*provider, "base_url");
            if (base_url != nullptr && base_url->is_string()) {
                const std::string url = base_url->get<std::string>();
                if (is_http_url(url)) {
                    document["llm"]["default"]["base_url"] = url;
                    if (const Json* model = member(*profile, "model");
                        model != nullptr && model->is_string()) {
                        document["llm"]["default"]["model"] = model->get<std::string>();
                    }
                }
            } else if (base_url == nullptr) {
                if (const Json* model = member(*profile, "model");
                    model != nullptr && model->is_string()) {
                    document["llm"]["default"]["model"] = model->get<std::string>();
                }
            }
        }
    }

    return document;
}

std::string effective_model(const Config& config) {
    if (!config.agent.model.empty()) {
        return config.agent.model;
    }
    return config.llm.model;
}

} // namespace ymh

#pragma once

// Logging (§40): spdlog-backed category loggers behind the `Logger` seam.
//
// Categories (§40): agent, llm, tool, process, filesystem, session, tui, mcp,
// lsp, plugin, network.
//
// Security rule (§40, 08 §8): full prompts and sensitive tool output are NEVER
// logged by default. `log_prompt` is a no-op unless `log_prompts` was explicitly
// opted in, and even then the text is secret-redacted and truncated. The
// session event log remains the authoritative trace.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "ymh/core/logger.hpp"

namespace ymh {

enum class LogCategory : std::uint8_t {
    Agent,
    Llm,
    Tool,
    Process,
    Filesystem,
    Session,
    Tui,
    Mcp,
    Lsp,
    Plugin,
    Network,
};

[[nodiscard]] std::string_view log_category_name(LogCategory category) noexcept;

struct LoggingOptions {
    LogLevel                  level = LogLevel::Info;
    std::string               pattern = "%Y-%m-%d %H:%M:%S.%e [%n] [%l] %v";
    bool                      color = false;
    std::filesystem::path     file;            // empty => stderr
    bool                      log_prompts = false;  // opt-in only
};

// (Re)configures the process-wide category loggers. Idempotent; safe to call
// again to change level/sink.
void init_logging(const LoggingOptions& options);
void shutdown_logging();

// The seam logger for a category. Never null; lazily initialises with defaults
// if `init_logging` has not run.
Logger& category_logger(LogCategory category);

// True only when prompt logging was explicitly opted in.
[[nodiscard]] bool prompt_logging_enabled() noexcept;

// Logs a prompt body at debug level. No-op unless opted in. The text is always
// secret-redacted and truncated before it reaches the sink.
void log_prompt(LogCategory category, std::string_view text);

// Redacts secrets and bounds length for a diagnostic line.
[[nodiscard]] std::string redact_log_text(std::string_view text,
                                          std::size_t max_bytes = 512);

[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view name) noexcept;

} // namespace ymh

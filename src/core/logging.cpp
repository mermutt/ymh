#include "ymh/core/logging.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include "ymh/llm/redaction.hpp"

namespace ymh {
namespace {

constexpr std::size_t kCategoryCount = 11;

std::size_t category_index(LogCategory category) noexcept {
    return static_cast<std::size_t>(category);
}

spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
    }
    return spdlog::level::info;
}

class SpdlogLogger final : public Logger {
public:
    explicit SpdlogLogger(std::shared_ptr<spdlog::logger> logger) : logger_(std::move(logger)) {}

    void log(LogLevel level, std::string_view message) override {
        logger_->log(to_spdlog(level), "{}", redact_secrets(message));
    }

private:
    std::shared_ptr<spdlog::logger> logger_;
};

struct State {
    std::mutex                                                          mutex;
    std::array<std::shared_ptr<spdlog::logger>, kCategoryCount>         loggers{};
    std::array<std::unique_ptr<SpdlogLogger>, kCategoryCount>           seams{};
    std::shared_ptr<spdlog::logger>                                     fallback;
    std::unique_ptr<SpdlogLogger>                                       fallback_seam;
    bool                                                                prompts = false;
    bool                                                                initialized = false;
};

State& state() {
    static State instance;
    return instance;
}

void configure_locked(const LoggingOptions& options) {
    State& s = state();

    std::shared_ptr<spdlog::sinks::sink> sink;
    if (!options.file.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.file.parent_path(), error);
        sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(options.file.string(), false);
    } else if (options.color) {
        sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    } else {
        sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    }
    sink->set_pattern(options.pattern);

    for (std::size_t index = 0; index < kCategoryCount; ++index) {
        const LogCategory category = static_cast<LogCategory>(index);
        auto logger = std::make_shared<spdlog::logger>(std::string{log_category_name(category)},
                                                       sink);
        logger->set_level(to_spdlog(options.level));
        logger->flush_on(spdlog::level::trace);
        s.loggers[index] = std::move(logger);
        s.seams[index]   = std::make_unique<SpdlogLogger>(s.loggers[index]);
    }

    s.fallback = std::make_shared<spdlog::logger>("ymh", sink);
    s.fallback->set_level(to_spdlog(options.level));
    s.fallback->flush_on(spdlog::level::trace);
    s.fallback_seam = std::make_unique<SpdlogLogger>(s.fallback);

    s.prompts     = options.log_prompts;
    s.initialized = true;
}

} // namespace

std::string_view log_category_name(LogCategory category) noexcept {
    switch (category) {
        case LogCategory::Agent:
            return "agent";
        case LogCategory::Llm:
            return "llm";
        case LogCategory::Tool:
            return "tool";
        case LogCategory::Process:
            return "process";
        case LogCategory::Filesystem:
            return "filesystem";
        case LogCategory::Session:
            return "session";
        case LogCategory::Tui:
            return "tui";
        case LogCategory::Mcp:
            return "mcp";
        case LogCategory::Lsp:
            return "lsp";
        case LogCategory::Plugin:
            return "plugin";
        case LogCategory::Network:
            return "network";
    }
    return "unknown";
}

void init_logging(const LoggingOptions& options) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    configure_locked(options);
}

void shutdown_logging() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.fallback) {
        s.fallback->flush();
    }
    for (auto& logger : s.loggers) {
        if (logger) {
            logger->flush();
        }
    }
    for (auto& seam : s.seams) {
        seam.reset();
    }
    for (auto& logger : s.loggers) {
        logger.reset();
    }
    s.fallback.reset();
    s.fallback_seam.reset();
    s.prompts     = false;
    s.initialized = false;
}

Logger& category_logger(LogCategory category) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.initialized) {
        configure_locked(LoggingOptions{});
    }
    return *s.seams[category_index(category)];
}

bool prompt_logging_enabled() noexcept {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.prompts;
}

std::string redact_log_text(std::string_view text, std::size_t max_bytes) {
    std::string redacted = redact_secrets(text);
    if (max_bytes != 0 && redacted.size() > max_bytes) {
        redacted.resize(max_bytes);
        redacted += "...[truncated]";
    }
    return redacted;
}

void log_prompt(LogCategory category, std::string_view text) {
    if (!prompt_logging_enabled()) {
        return;
    }
    category_logger(category).debug(redact_log_text(text));
}

std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "info";
}

std::optional<LogLevel> parse_log_level(std::string_view name) noexcept {
    if (name == "debug" || name == "trace") {
        return LogLevel::Debug;
    }
    if (name == "info") {
        return LogLevel::Info;
    }
    if (name == "warn" || name == "warning") {
        return LogLevel::Warn;
    }
    if (name == "error" || name == "err" || name == "critical" || name == "off") {
        return LogLevel::Error;
    }
    return std::nullopt;
}

} // namespace ymh

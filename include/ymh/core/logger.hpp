#pragma once

// Minimal logging seam. 07 §5.1 pins `Logger&` as a `ToolContext` member but no
// component spec pins the `Logger` shape; this is the smallest faithful
// materialisation so the pinned `ToolContext` constructor compiles without
// pulling a logging framework into the tool layer. The daemon/supervisor wave
// (spec 10, §40) wires the real spdlog-backed implementation behind this seam.

#include <string_view>
#include <utility>

namespace ymh {

enum class LogLevel : unsigned char {
    Debug,
    Info,
    Warn,
    Error,
};

class Logger {
public:
    virtual ~Logger() = default;

    virtual void log(LogLevel level, std::string_view message) = 0;

    void debug(std::string_view message) { log(LogLevel::Debug, message); }
    void info(std::string_view message) { log(LogLevel::Info, message); }
    void warn(std::string_view message) { log(LogLevel::Warn, message); }
    void error(std::string_view message) { log(LogLevel::Error, message); }
};

// Discards everything. Used by tools/tests that do not assert on logs.
class NullLogger final : public Logger {
public:
    void log(LogLevel, std::string_view) override {}
};

} // namespace ymh

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "support/test_env.hpp"
#include "ymh/core/logging.hpp"

namespace {

using namespace ymh;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream     input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

TEST(Logging, RedactsSecretsAndTruncates) {
    const std::string redacted = redact_log_text("Authorization: Bearer sk-abcdef1234567890");
    EXPECT_NE(redacted.find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(redacted.find("sk-abcdef1234567890"), std::string::npos);

    const std::string bounded = redact_log_text(std::string(100, 'x'), 10);
    EXPECT_LE(bounded.size(), 10u + std::string{"...[truncated]"}.size());
    EXPECT_NE(bounded.find("...[truncated]"), std::string::npos);
}

TEST(Logging, PromptNotLoggedByDefault) {
    test::TempWorkspace workspace("logging_default");
    const std::filesystem::path log_path = workspace.path() / "ymh.log";

    LoggingOptions options;
    options.level       = LogLevel::Debug;
    options.file        = log_path;
    options.log_prompts = false;
    init_logging(options);

    category_logger(LogCategory::Agent).info("metadata-only-line");
    log_prompt(LogCategory::Llm, "PROMPT_SENTINEL_12345");
    shutdown_logging();

    const std::string contents = read_file(log_path);
    EXPECT_NE(contents.find("metadata-only-line"), std::string::npos);
    EXPECT_EQ(contents.find("PROMPT_SENTINEL_12345"), std::string::npos);
}

TEST(Logging, PromptLoggedOnlyWhenOptedIn) {
    test::TempWorkspace workspace("logging_optin");
    const std::filesystem::path log_path = workspace.path() / "ymh.log";

    LoggingOptions options;
    options.level       = LogLevel::Debug;
    options.file        = log_path;
    options.log_prompts = true;
    init_logging(options);

    log_prompt(LogCategory::Llm, "PROMPT_SENTINEL_12345 key sk-secret123456");
    shutdown_logging();

    const std::string contents = read_file(log_path);
    EXPECT_NE(contents.find("PROMPT_SENTINEL_12345"), std::string::npos);
    EXPECT_EQ(contents.find("sk-secret123456"), std::string::npos);
}

TEST(Logging, UI46_D12_LogSinkRedacts) {
    test::TempWorkspace         workspace("logging_sink_redact");
    const std::filesystem::path log_path = workspace.path() / "ymh.log";

    LoggingOptions options;
    options.level = LogLevel::Debug;
    options.file  = log_path;
    init_logging(options);

    category_logger(LogCategory::Agent).info(R"({"api_key": "SECRET123"})");
    shutdown_logging();

    const std::string contents = read_file(log_path);
    EXPECT_EQ(contents.find("SECRET123"), std::string::npos);
    EXPECT_NE(contents.find("[REDACTED]"), std::string::npos);
}

TEST(Logging, UI46_D12_ApiKeyNeverLogged) {
    test::TempWorkspace         workspace("logging_never_logged");
    const std::filesystem::path log_path = workspace.path() / "ymh.log";

    LoggingOptions options;
    options.level = LogLevel::Debug;
    options.file  = log_path;
    init_logging(options);

    category_logger(LogCategory::Tool).info("provider call api_key=SECRET123 done");
    shutdown_logging();

    const std::string contents = read_file(log_path);
    EXPECT_EQ(contents.find("SECRET123"), std::string::npos);
}

TEST(Logging, LevelParsing) {
    EXPECT_EQ(parse_log_level("debug"), LogLevel::Debug);
    EXPECT_EQ(parse_log_level("warn"), LogLevel::Warn);
    EXPECT_EQ(parse_log_level("error"), LogLevel::Error);
    EXPECT_FALSE(parse_log_level("bogus").has_value());
}

} // namespace

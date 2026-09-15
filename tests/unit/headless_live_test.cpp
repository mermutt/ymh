#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>
#include <string>

#include "support/test_env.hpp"
#include "ymh/cli/headless.hpp"
#include "ymh/core/logging.hpp"

namespace {

using namespace ymh;

bool live_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key  = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

TEST(LiveHeadlessTest, RunsAgainstDeepSeek) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    LoggingOptions logging;
    logging.level = LogLevel::Warn;
    init_logging(logging);

    test::TempWorkspace workspace("headless_live");
    std::ostringstream  out;
    std::ostringstream  err;

    Config config;
    if (const char* model = std::getenv("YMH_LIVE_LLM_MODEL"); model != nullptr && *model != '\0') {
        config.llm.model = model;
    }

    HeadlessOptions options;
    options.workspace = workspace.path();
    options.task      = "reply with the word pong";
    options.config    = config;
    options.out       = &out;
    options.err       = &err;

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 0) << err.str();
    EXPECT_FALSE(result.assistant_text.empty());
    EXPECT_NE(out.str().find(result.assistant_text), std::string::npos);
}

} // namespace

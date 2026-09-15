#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace {

using namespace ymh;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() { ::unsetenv(name_.c_str()); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
};

TEST(Config, DefaultsMatchDeepSeek) {
    const Config config;
    EXPECT_EQ(config.llm.provider, "openai-compatible");
    EXPECT_EQ(config.llm.base_url, "https://api.deepseek.com/v1");
    EXPECT_EQ(config.llm.model, "deepseek-flash");
    EXPECT_EQ(config.llm.api_key_env, "DEEPSEEK_API_KEY");
    EXPECT_EQ(config.agent.max_steps, 100u);
    EXPECT_EQ(config.logging.level, "info");
    EXPECT_FALSE(config.logging.log_prompts);
    EXPECT_EQ(config.permissions.read, "allow");
    EXPECT_EQ(config.permissions.write, "ask");
}

TEST(Config, DefaultsMatchPinnedDeepSeekConfig) {
    const LLMProviderConfig pinned = deepseek_config();
    const Config           config;
    EXPECT_EQ(config.llm.provider, pinned.provider);
    EXPECT_EQ(config.llm.base_url, pinned.base_url);
    EXPECT_EQ(config.llm.model, pinned.model);
    EXPECT_EQ(config.llm.api_key_env, pinned.api_key_env);
}

TEST(Config, WorkspaceOverridesGlobal) {
    test::TempWorkspace workspace("config_precedence");

    const std::filesystem::path global = workspace.path() / "global.toml";
    workspace.write("global.toml",
                    "[agent]\nmodel = \"global-model\"\nmax_steps = 5\n"
                    "[llm.default]\nmodel = \"global-llm\"\nbase_url = \"http://global.example/v1\"\n");

    workspace.write(".ymh/config.toml",
                    "[agent]\nmodel = \"workspace-model\"\n"
                    "[llm.default]\nbase_url = \"http://workspace.example/v1\"\n");

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace_config_path(workspace.path());

    const Config config = load_config(paths);
    EXPECT_EQ(config.agent.model, "workspace-model");
    EXPECT_EQ(config.agent.max_steps, 5u);
    EXPECT_EQ(config.llm.model, "global-llm");
    EXPECT_EQ(config.llm.base_url, "http://workspace.example/v1");
    EXPECT_EQ(effective_model(config), "workspace-model");
}

TEST(Config, EnvironmentOverridesFiles) {
    test::TempWorkspace workspace("config_env");
    workspace.write(".ymh/config.toml", "[llm.default]\nmodel = \"file-model\"\n");

    ScopedEnv model("YMH_LLM_MODEL", "env-model");
    ScopedEnv steps("YMH_AGENT_MAX_STEPS", "7");
    ScopedEnv level("YMH_LOG_LEVEL", "debug");

    ConfigPaths paths;
    paths.global    = workspace.path() / "missing-global.toml";
    paths.workspace = workspace_config_path(workspace.path());

    const Config config = load_config(paths);
    EXPECT_EQ(config.llm.model, "env-model");
    EXPECT_EQ(config.agent.max_steps, 7u);
    EXPECT_EQ(config.logging.level, "debug");
}

TEST(Config, UnknownKeyRejected) {
    test::TempWorkspace workspace("config_unknown");
    workspace.write(".ymh/config.toml", "[agent]\nmodle = \"typo\"\n");

    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, RetryAndTimeoutsParse) {
    test::TempWorkspace workspace("config_retry");
    workspace.write(".ymh/config.toml",
                    "[llm.default]\nconnect_timeout_ms = 1234\n"
                    "[llm.default.retry]\nmax_attempts = 9\nbase_delay_ms = 250\njitter = 0.5\n");

    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    EXPECT_EQ(config.llm.connect_timeout.count(), 1234);
    EXPECT_EQ(config.llm.retry.max_attempts, 9u);
    EXPECT_EQ(config.llm.retry.base_delay.count(), 250);
    EXPECT_DOUBLE_EQ(config.llm.retry.jitter, 0.5);
}

TEST(Config, MalformedTimeoutRejected) {
    test::TempWorkspace workspace("config_bad_env");
    ScopedEnv timeout("YMH_LLM_REQUEST_TIMEOUT_MS", "not-a-number");

    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

} // namespace

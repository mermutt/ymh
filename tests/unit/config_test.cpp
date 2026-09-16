#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace {

using namespace ymh;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = ::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string                name_;
    std::optional<std::string> previous_;
};

class CapturingLogger final : public Logger {
public:
    void log(LogLevel level, std::string_view message) override {
        if (level == LogLevel::Warn || level == LogLevel::Error) {
            warnings.emplace_back(message);
        }
    }

    std::vector<std::string> warnings;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream      input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool same_config(const Config& a, const Config& b) {
    return a.ui.theme == b.ui.theme && a.ui.show_activity == b.ui.show_activity &&
           a.ui.side_panel == b.ui.side_panel && a.agent.model == b.agent.model &&
           a.agent.max_steps == b.agent.max_steps &&
           a.agent.reasoning_effort == b.agent.reasoning_effort &&
           a.agent.system_prompt == b.agent.system_prompt && a.workspace.root == b.workspace.root &&
           a.workspace.workspace_roots == b.workspace.workspace_roots &&
           a.permissions.shell == b.permissions.shell &&
           a.permissions.write == b.permissions.write && a.permissions.read == b.permissions.read &&
           a.logging.level == b.logging.level &&
           a.logging.log_prompts == b.logging.log_prompts && a.llm.provider == b.llm.provider &&
           a.llm.base_url == b.llm.base_url && a.llm.model == b.llm.model &&
           a.llm.api_key_env == b.llm.api_key_env &&
           a.llm.reasoning_effort == b.llm.reasoning_effort &&
           a.llm.max_concurrency == b.llm.max_concurrency &&
           a.llm.connect_timeout == b.llm.connect_timeout &&
           a.llm.idle_timeout == b.llm.idle_timeout &&
           a.llm.request_timeout == b.llm.request_timeout &&
           a.llm.retry.max_attempts == b.llm.retry.max_attempts &&
           a.llm.retry.base_delay == b.llm.retry.base_delay &&
           a.llm.retry.max_delay == b.llm.retry.max_delay &&
           a.llm.retry.jitter == b.llm.retry.jitter &&
           a.llm.retry.honor_retry_after == b.llm.retry.honor_retry_after;
}

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

TEST(Config, ScaffoldCreatesGlobalConfigAndWorkspaceDir) {
    test::TempWorkspace workspace("config_scaffold");
    const std::filesystem::path global = workspace.path() / "xdg" / "ymh" / "config.toml";

    const ScaffoldResult result = scaffold_config(workspace.path(), global);

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.global_dir_created);
    EXPECT_TRUE(result.global_config_created);
    EXPECT_TRUE(result.workspace_dir_created);
    EXPECT_TRUE(std::filesystem::is_regular_file(global));
    EXPECT_TRUE(std::filesystem::is_directory(workspace.path() / ".ymh"));
}

TEST(Config, ScaffoldRespectsXdgConfigHome) {
    test::TempWorkspace         workspace("config_scaffold_xdg");
    const std::filesystem::path xdg = workspace.path() / "xdg-home";
    ScopedEnv                   xdg_env("XDG_CONFIG_HOME", xdg.string());

    const std::filesystem::path expected = xdg / "ymh" / "config.toml";
    EXPECT_EQ(default_global_config_path(), expected);

    const ScaffoldResult result = scaffold_config(workspace.path());

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(std::filesystem::is_regular_file(expected));
    EXPECT_TRUE(std::filesystem::is_directory(workspace.path() / ".ymh"));
}

TEST(Config, ScaffoldDoesNotOverwriteExistingFile) {
    test::TempWorkspace workspace("config_scaffold_nooverwrite");
    const std::filesystem::path global = workspace.path() / "global.toml";
    const std::string sentinel = "# user-customized\n[agent]\nmax_steps = 7\n";
    workspace.write("global.toml", sentinel);

    const ScaffoldResult result = scaffold_config(workspace.path(), global);

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.global_config_created);
    EXPECT_EQ(read_file(global), sentinel);
}

TEST(Config, ScaffoldDefaultLoadsAsBuiltinDefaults) {
    test::TempWorkspace workspace("config_scaffold_load");
    const std::filesystem::path global = workspace.path() / "xdg" / "ymh" / "config.toml";

    ASSERT_TRUE(scaffold_config(workspace.path(), global).ok);

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace_config_path(workspace.path());
    const Config loaded = load_config(paths);

    Config expected;
    apply_env_overrides(expected);
    EXPECT_TRUE(same_config(loaded, expected));
}

TEST(Config, ScaffoldFailureLogsWarningAndDoesNotThrow) {
    test::TempWorkspace workspace("config_scaffold_fail");
    workspace.write("blocker", "not a directory");

    const std::filesystem::path global = workspace.path() / "blocker" / "ymh" / "config.toml";
    CapturingLogger             logger;

    ScaffoldResult result;
    ASSERT_NO_THROW(result = scaffold_config(workspace.path(), global, &logger));

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.global_config_created);
    EXPECT_FALSE(logger.warnings.empty());
}

TEST(Config, CompactionPolicyParsesAndMaps) {
    test::TempWorkspace workspace("config_compaction");
    workspace.write(".ymh/config.toml",
                    "[agent]\ncompaction_threshold_tokens = 5000\n"
                    "[agent.compaction]\nkeep_recent_turns = 3\n"
                    "summarizer_model = \"cheap\"\nmax_summary_bytes = 1024\n"
                    "retry_on_context_length = false\n");

    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    EXPECT_EQ(config.agent.compaction.threshold_tokens, 5000u);
    EXPECT_EQ(config.agent.compaction.keep_recent_turns, 3u);
    EXPECT_EQ(config.agent.compaction.summarizer_model, "cheap");
    EXPECT_EQ(config.agent.compaction.max_summary_bytes, 1024u);
    EXPECT_FALSE(config.agent.compaction.retry_on_context_length);

    const CompactionPolicy policy = to_compaction_policy(config);
    EXPECT_TRUE(policy.is_enabled());
    EXPECT_EQ(policy.effective_threshold_tokens(), 5000u);
    EXPECT_EQ(policy.keep_recent_turns, 3u);
}

TEST(Config, CompactionOversizedSummaryBytesRejected) {
    Config config;
    config.agent.compaction.max_summary_bytes = PersistenceConfig{}.max_payload_bytes + 1;
    EXPECT_THROW((void)to_compaction_policy(config), ConfigError);
}

} // namespace

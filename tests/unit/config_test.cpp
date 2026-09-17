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

    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    "{\n  \"agent\": { \"model\": \"global-model\", \"max_steps\": 5 },\n"
                    "  \"llm\": { \"default\": { \"model\": \"global-llm\", "
                    "\"base_url\": \"http://global.example/v1\" } }\n}\n");

    workspace.write(".ymh/config.jsonc",
                    "{\n  \"agent\": { \"model\": \"workspace-model\" },\n"
                    "  \"llm\": { \"default\": { \"base_url\": \"http://workspace.example/v1\" } }\n}\n");

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
    workspace.write(".ymh/config.jsonc",
                    "{ \"llm\": { \"default\": { \"model\": \"file-model\" } } }\n");

    ScopedEnv model("YMH_LLM_MODEL", "env-model");
    ScopedEnv steps("YMH_AGENT_MAX_STEPS", "7");
    ScopedEnv level("YMH_LOG_LEVEL", "debug");

    ConfigPaths paths;
    paths.global    = workspace.path() / "missing-global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());

    const Config config = load_config(paths);
    EXPECT_EQ(config.llm.model, "env-model");
    EXPECT_EQ(config.agent.max_steps, 7u);
    EXPECT_EQ(config.logging.level, "debug");
}

TEST(Config, UnknownKeyRejected) {
    test::TempWorkspace workspace("config_unknown");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"modle\": \"typo\" } }\n");

    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, RetryAndTimeoutsParse) {
    test::TempWorkspace workspace("config_retry");
    workspace.write(".ymh/config.jsonc",
                    "{\n  \"llm\": { \"default\": {\n    \"connect_timeout_ms\": 1234,\n"
                    "    \"retry\": { \"max_attempts\": 9, \"base_delay_ms\": 250, "
                    "\"jitter\": 0.5 }\n  } }\n}\n");

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
    const std::filesystem::path global = workspace.path() / "xdg" / "ymh" / "config.jsonc";

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

    const std::filesystem::path expected = xdg / "ymh" / "config.jsonc";
    EXPECT_EQ(default_global_config_path(), expected);

    const ScaffoldResult result = scaffold_config(workspace.path());

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(std::filesystem::is_regular_file(expected));
    EXPECT_TRUE(std::filesystem::is_directory(workspace.path() / ".ymh"));
}

TEST(Config, ScaffoldDoesNotOverwriteExistingFile) {
    test::TempWorkspace workspace("config_scaffold_nooverwrite");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    const std::string sentinel = "// user-customized\n{ \"agent\": { \"max_steps\": 7 } }\n";
    workspace.write("global.jsonc", sentinel);

    const ScaffoldResult result = scaffold_config(workspace.path(), global);

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.global_config_created);
    EXPECT_EQ(read_file(global), sentinel);
}

TEST(Config, ScaffoldDefaultLoadsAsBuiltinDefaults) {
    test::TempWorkspace workspace("config_scaffold_load");
    const std::filesystem::path global = workspace.path() / "xdg" / "ymh" / "config.jsonc";

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

    const std::filesystem::path global = workspace.path() / "blocker" / "ymh" / "config.jsonc";
    CapturingLogger             logger;

    ScaffoldResult result;
    ASSERT_NO_THROW(result = scaffold_config(workspace.path(), global, &logger));

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.global_config_created);
    EXPECT_FALSE(logger.warnings.empty());
}

TEST(Config, CompactionPolicyParsesAndMaps) {
    test::TempWorkspace workspace("config_compaction");
    workspace.write(".ymh/config.jsonc",
                    "{\n  \"agent\": {\n    \"compaction_threshold_tokens\": 5000,\n"
                    "    \"compaction\": {\n      \"keep_recent_turns\": 3,\n"
                    "      \"summarizer_model\": \"cheap\",\n      \"max_summary_bytes\": 1024,\n"
                    "      \"retry_on_context_length\": false\n    }\n  }\n}\n");

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

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

TEST(Config, JsoncCommentsAccepted) {
    test::TempWorkspace workspace("config_jsonc_comments");
    workspace.write(".ymh/config.jsonc",
                    "{\n  // line comment\n  \"agent\": { /* block */ \"max_steps\": 5 }\n}\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_EQ(load_config(paths).agent.max_steps, 5u);
}

TEST(Config, JsoncTrailingCommaRejected) {
    test::TempWorkspace workspace("config_jsonc_trailing");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 5, } }\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JsoncHashCommentRejected) {
    test::TempWorkspace workspace("config_jsonc_hash");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 5 # five } }\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JsoncWrongTypeRejected) {
    test::TempWorkspace workspace("config_jsonc_type");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": \"many\" } }\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JsoncNegativeIntegerRejected) {
    test::TempWorkspace workspace("config_jsonc_negative");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": -1 } }\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, TopLevelNonObjectRejected) {
    test::TempWorkspace workspace("config_jsonc_toplevel");
    workspace.write(".ymh/config.jsonc", "[1,2,3]\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, EmptyAndCommentsOnlyAreNoOp) {
    test::TempWorkspace workspace("config_jsonc_empty");
    ConfigPaths         paths;
    paths.workspace = workspace_config_path(workspace.path());
    Config expected;
    apply_env_overrides(expected);
    for (const std::string& text : {std::string{}, std::string{"   \n"}, std::string{"// only\n"},
                                    std::string{"/* only */"}}) {
        workspace.write(".ymh/config.jsonc", text);
        EXPECT_TRUE(same_config(load_config(paths), expected)) << "input size " << text.size();
    }
}

TEST(Config, NonJsonWhitespaceRejected) {
    test::TempWorkspace workspace("config_jsonc_ws");
    ConfigPaths         paths;
    paths.workspace = workspace_config_path(workspace.path());
    for (const std::string& text : {std::string{"\f"}, std::string{"\v"}}) {
        workspace.write(".ymh/config.jsonc", text);
        EXPECT_THROW((void)load_config(paths), ConfigError);
    }
}

TEST(Config, EmptyObjectIsValidNoOp) {
    test::TempWorkspace workspace("config_jsonc_emptyobj");
    ConfigPaths         paths;
    paths.workspace = workspace_config_path(workspace.path());
    Config expected;
    apply_env_overrides(expected);
    for (const std::string& text : {std::string{"{}"}, std::string{"{ /* c */ }"}}) {
        workspace.write(".ymh/config.jsonc", text);
        EXPECT_TRUE(same_config(load_config(paths), expected)) << "input " << text;
    }
}

TEST(Config, LeadingBomBoundary) {
    test::TempWorkspace workspace("config_jsonc_bom");
    ConfigPaths         paths;
    paths.workspace = workspace_config_path(workspace.path());
    Config expected;
    apply_env_overrides(expected);
    for (const std::string& text : {std::string{"\xEF\xBB\xBF"}, std::string{"\xEF\xBB\xBF  \n"},
                                    std::string{"\xEF\xBB\xBF// c\n"},
                                    std::string{"\xEF\xBB\xBF{}"}}) {
        workspace.write(".ymh/config.jsonc", text);
        EXPECT_TRUE(same_config(load_config(paths), expected)) << "input size " << text.size();
    }
    for (const std::string& text :
         {std::string{"  \xEF\xBB\xBF{}"}, std::string{"\xEF\xBB\xBF\xEF\xBB\xBF{}"}}) {
        workspace.write(".ymh/config.jsonc", text);
        EXPECT_THROW((void)load_config(paths), ConfigError);
    }
}

TEST(Config, MalformedJsoncNamesLine) {
    test::TempWorkspace workspace("config_malformed_line");
    workspace.write(".ymh/config.jsonc", "{\n  \"agent\": {\n    \"max_steps\": ,\n  }\n}\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("line 3"), std::string::npos) << error.what();
    }
}

TEST(Config, JsoncLlmNestedAndFlatParity) {
    test::TempWorkspace nested_workspace("config_llm_nested");
    nested_workspace.write(
        ".ymh/config.jsonc",
        "{\n  \"llm\": { \"default\": { \"model\": \"m\", \"max_concurrency\": 2 } }\n}\n");
    ConfigPaths nested_paths;
    nested_paths.workspace = workspace_config_path(nested_workspace.path());
    const Config nested = load_config(nested_paths);

    test::TempWorkspace flat_workspace("config_llm_flat");
    flat_workspace.write(".ymh/config.jsonc",
                         "{\n  \"llm\": { \"model\": \"m\", \"max_concurrency\": 2 }\n}\n");
    ConfigPaths flat_paths;
    flat_paths.workspace = workspace_config_path(flat_workspace.path());
    const Config flat = load_config(flat_paths);

    EXPECT_EQ(nested.llm.model, flat.llm.model);
    EXPECT_EQ(nested.llm.max_concurrency, flat.llm.max_concurrency);
    EXPECT_EQ(nested.llm.provider, flat.llm.provider);
    EXPECT_EQ(nested.llm.base_url, flat.llm.base_url);
}

TEST(Config, JsoncMcpServerArrayParses) {
    test::TempWorkspace workspace("config_jsonc_mcp");
    workspace.write(".ymh/config.jsonc",
                    "{\n  \"mcp\": { \"server\": [\n"
                    "    { \"id\": \"a\", \"command\": \"ca\" },\n"
                    "    { \"id\": \"b\", \"command\": \"cb\" }\n  ] }\n}\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 2u);
    EXPECT_EQ(config.mcp.servers[0].id, "a");
    EXPECT_EQ(config.mcp.servers[1].command, "cb");
}

TEST(Config, EmptyWorkspaceRootsDoesNotOverride) {
    test::TempWorkspace workspace("config_roots");
    workspace.write("global.jsonc", "{ \"workspace\": { \"workspace_roots\": [\"a\"] } }\n");
    workspace.write(".ymh/config.jsonc", "{ \"workspace\": { \"workspace_roots\": [] } }\n");
    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_EQ(load_config(paths).workspace.workspace_roots, (std::vector<std::string>{"a"}));
}

TEST(Config, EmptyMcpServerArrayClears) {
    test::TempWorkspace workspace("config_mcp_clear");
    workspace.write("global.jsonc",
                    "{ \"mcp\": { \"server\": [ { \"id\": \"a\" }, { \"id\": \"b\" } ] } }\n");
    workspace.write(".ymh/config.jsonc", "{ \"mcp\": { \"server\": [] } }\n");
    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_TRUE(load_config(paths).mcp.servers.empty());
}

TEST(Config, PerServerEmptyArrayEqualsAbsent) {
    test::TempWorkspace workspace("config_mcp_empty_arrays");
    workspace.write(
        "global.jsonc",
        "{ \"mcp\": { \"server\": [ { \"id\": \"s\", \"args\": [\"--x\"], \"env\": [\"A=1\"] } ] } }\n");
    workspace.write(".ymh/config.jsonc",
                    "{ \"mcp\": { \"server\": [ { \"id\": \"s\", \"args\": [], \"env\": [] } ] } }\n");
    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_TRUE(config.mcp.servers[0].args.empty());
    EXPECT_TRUE(config.mcp.servers[0].env.empty());
}

TEST(Config, LegacyTomlWarnsOnceAndIgnores) {
    test::TempWorkspace workspace("config_legacy");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    CapturingLogger logger;
    const Config    config = load_config(paths, &logger);
    EXPECT_EQ(config.agent.max_steps, 100u);
    ASSERT_EQ(logger.warnings.size(), 1u);
    EXPECT_NE(logger.warnings[0].find("ignoring legacy TOML"), std::string::npos);
}

TEST(Config, LegacyWarningOncePerCall) {
    test::TempWorkspace workspace("config_legacy_percall");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    CapturingLogger logger;
    (void)load_config(paths, &logger);
    (void)load_config(paths, &logger);
    EXPECT_EQ(logger.warnings.size(), 2u);
}

TEST(Config, BothFilesPrefersJsoncAndWarns) {
    test::TempWorkspace workspace("config_both");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 3 } }\n");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    CapturingLogger logger;
    const Config    config = load_config(paths, &logger);
    EXPECT_EQ(config.agent.max_steps, 3u);
    EXPECT_EQ(logger.warnings.size(), 1u);
}

TEST(Config, LegacyOnlyContinuesOnDefaults) {
    test::TempWorkspace workspace("config_legacy_only");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    CapturingLogger logger;
    const Config    config = load_config(paths, &logger);
    Config          expected;
    apply_env_overrides(expected);
    EXPECT_TRUE(same_config(config, expected));
    EXPECT_EQ(logger.warnings.size(), 1u);
}

TEST(Config, LegacyWarningTargetsBothSlots) {
    test::TempWorkspace workspace("config_legacy_both");
    workspace.write("config.toml", "[agent]\nmax_steps = 7\n");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");
    ConfigPaths paths;
    paths.global    = workspace.path() / "config.toml";
    paths.workspace = workspace_config_path(workspace.path());
    CapturingLogger logger;
    (void)load_config(paths, &logger);
    ASSERT_EQ(logger.warnings.size(), 1u);
    const std::string& message = logger.warnings[0];
    EXPECT_EQ(count_occurrences(message, "->"), 2u);
    EXPECT_NE(message.find((workspace.path() / "config.jsonc").string()), std::string::npos);
    EXPECT_NE(message.find((workspace.path() / ".ymh" / "config.jsonc").string()),
              std::string::npos);
    EXPECT_NE(message.find("(explicit --config slot: update --config)"), std::string::npos);
}

TEST(Config, NoSiblingProbeForCustomBasename) {
    test::TempWorkspace workspace("config_custom_basename");
    workspace.write("config.toml", "[agent]\nmax_steps = 7\n");
    workspace.write("other.jsonc", "{ \"agent\": { \"max_steps\": 2 } }\n");
    ConfigPaths paths;
    paths.global = workspace.path() / "other.jsonc";
    CapturingLogger logger;
    const Config    config = load_config(paths, &logger);
    EXPECT_EQ(config.agent.max_steps, 2u);
    EXPECT_TRUE(logger.warnings.empty());
}

TEST(Config, ReadStringArrayRejectsNonString) {
    test::TempWorkspace workspace("config_array_type");
    workspace.write(".ymh/config.jsonc", "{ \"workspace\": { \"workspace_roots\": [\"a\", 1] } }\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, ReadSizeRejectsUint64Overflow) {
    test::TempWorkspace workspace("config_overflow");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 18446744073709551615 } }\n");
    ConfigPaths paths;
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, LegacyPathFromJsonc) {
    EXPECT_EQ(legacy_config_path("/x/ymh/config.jsonc"),
              std::filesystem::path("/x/ymh/config.toml"));
    EXPECT_EQ(legacy_config_path("/x/config.toml"), std::filesystem::path("/x/config.toml"));
    EXPECT_TRUE(legacy_config_path("/x/other.jsonc").empty());
    EXPECT_TRUE(legacy_config_path("/x/other.toml").empty());
}

TEST(Config, JsoncTargetPerSlot) {
    EXPECT_EQ(jsonc_target("/x/ymh/config.jsonc"), std::filesystem::path("/x/ymh/config.jsonc"));
    EXPECT_EQ(jsonc_target("/x/config.toml"), std::filesystem::path("/x/config.jsonc"));
}

TEST(Config, ScaffoldTargetPerSlot) {
    EXPECT_EQ(scaffold_target("/x/ymh/config.jsonc"),
              std::filesystem::path("/x/ymh/config.jsonc"));
    EXPECT_TRUE(scaffold_target("/x/config.toml").empty());
    EXPECT_EQ(scaffold_target("/x/other.jsonc"), std::filesystem::path("/x/other.jsonc"));
}

TEST(Config, ScaffoldEmptyTargetIsNoOp) {
    test::TempWorkspace workspace("config_scaffold_empty");
    CapturingLogger     logger;
    ScaffoldResult      result;
    ASSERT_NO_THROW(result = scaffold_config(workspace.path(), std::filesystem::path{}, &logger));
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.global_dir_created);
    EXPECT_FALSE(result.global_config_created);
    EXPECT_TRUE(result.workspace_dir_created);
    EXPECT_TRUE(std::filesystem::is_directory(workspace.path() / ".ymh"));
    EXPECT_TRUE(logger.warnings.empty());
}

TEST(Config, ScaffoldOutputReparsesToDefaults) {
    test::TempWorkspace workspace("config_scaffold_reparse");
    const std::filesystem::path global = workspace.path() / "xdg" / "ymh" / "config.jsonc";
    ASSERT_TRUE(scaffold_config(workspace.path(), global).ok);

    const std::string text = read_file(global);
    EXPECT_EQ(text.find('#'), std::string::npos);

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace_config_path(workspace.path());
    Config expected;
    apply_env_overrides(expected);
    EXPECT_TRUE(same_config(load_config(paths), expected));
}

} // namespace

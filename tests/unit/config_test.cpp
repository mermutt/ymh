#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/mcp/mcp_types.hpp"

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

// The global layer is required (21-D12); a test that exercises the workspace
// layer must still supply a valid global document.
std::filesystem::path write_global(const test::TempWorkspace& workspace) {
    workspace.write("global.jsonc", "{}\n");
    return workspace.path() / "global.jsonc";
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
    paths.global    = write_global(workspace);
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
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, WorkspaceLayerMcpServersIsAConfigError) {
    test::TempWorkspace workspace("config_ws_mcp_servers");
    workspace.write(".ymh/config.jsonc",
                    "{ \"mcp_servers\": { \"fake\": { \"command\": \"true\" } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("'mcp_servers' is global-layer only"),
                  std::string::npos);
        EXPECT_NE(std::string{error.what()}.find(paths.workspace.string()), std::string::npos);
    }
}

TEST(Config, WorkspaceLayerMcpSectionIsAConfigError) {
    test::TempWorkspace workspace("config_ws_mcp_section");
    workspace.write(".ymh/config.jsonc", "{ \"mcp\": { \"enabled\": false } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("'mcp' is global-layer only"),
                  std::string::npos);
    }
}

TEST(Config, GlobalLayerMcpIsAccepted) {
    test::TempWorkspace workspace("config_global_mcp");
    workspace.write("global.jsonc",
                    "{ \"mcp\": { \"enabled\": true, \"log_child_stderr\": true } }\n");

    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    EXPECT_TRUE(config.mcp.enabled);
    EXPECT_TRUE(config.mcp.log_child_stderr);
}

TEST(Config, RetryAndTimeoutsParse) {
    test::TempWorkspace workspace("config_retry");
    workspace.write(".ymh/config.jsonc",
                    "{\n  \"llm\": { \"default\": {\n    \"connect_timeout_ms\": 1234,\n"
                    "    \"retry\": { \"max_attempts\": 9, \"base_delay_ms\": 250, "
                    "\"jitter\": 0.5 }\n  } }\n}\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
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
    paths.global    = write_global(workspace);
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
    paths.global    = write_global(workspace);
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

TEST(Config, JsoncCommentsAccepted) {
    test::TempWorkspace workspace("config_jsonc_comments");
    workspace.write(".ymh/config.jsonc",
                    "{\n  // line comment\n  \"agent\": { /* block */ \"max_steps\": 5 }\n}\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_EQ(load_config(paths).agent.max_steps, 5u);
}

TEST(Config, JsoncTrailingCommaRejected) {
    test::TempWorkspace workspace("config_jsonc_trailing");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 5, } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JsoncHashCommentRejected) {
    test::TempWorkspace workspace("config_jsonc_hash");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 5 # five } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JsoncWrongTypeRejected) {
    test::TempWorkspace workspace("config_jsonc_type");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": \"many\" } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JsoncNegativeIntegerRejected) {
    test::TempWorkspace workspace("config_jsonc_negative");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": -1 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, TopLevelNonObjectRejected) {
    test::TempWorkspace workspace("config_jsonc_toplevel");
    workspace.write(".ymh/config.jsonc", "[1,2,3]\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, EmptyAndCommentsOnlyAreNoOp) {
    test::TempWorkspace workspace("config_jsonc_empty");
    ConfigPaths         paths;
    paths.global    = write_global(workspace);
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
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    for (const std::string& text : {std::string{"\f"}, std::string{"\v"}}) {
        workspace.write(".ymh/config.jsonc", text);
        EXPECT_THROW((void)load_config(paths), ConfigError);
    }
}

TEST(Config, EmptyObjectIsValidNoOp) {
    test::TempWorkspace workspace("config_jsonc_emptyobj");
    ConfigPaths         paths;
    paths.global    = write_global(workspace);
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
    paths.global    = write_global(workspace);
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
    paths.global    = write_global(workspace);
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
    nested_paths.global    = write_global(nested_workspace);
    nested_paths.workspace = workspace_config_path(nested_workspace.path());
    const Config nested = load_config(nested_paths);

    test::TempWorkspace flat_workspace("config_llm_flat");
    flat_workspace.write(".ymh/config.jsonc",
                         "{\n  \"llm\": { \"model\": \"m\", \"max_concurrency\": 2 }\n}\n");
    ConfigPaths flat_paths;
    flat_paths.global    = write_global(flat_workspace);
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
    paths.global    = write_global(workspace);
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

TEST(Config, ReadStringArrayRejectsNonString) {
    test::TempWorkspace workspace("config_array_type");
    workspace.write(".ymh/config.jsonc", "{ \"workspace\": { \"workspace_roots\": [\"a\", 1] } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, ReadSizeRejectsUint64Overflow) {
    test::TempWorkspace workspace("config_overflow");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 18446744073709551615 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
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

TEST(Config, TomlSiblingIsInvisible) {
    test::TempWorkspace workspace("config_toml_invisible");
    workspace.write("global.jsonc", "{}\n");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"max_steps\": 3 } }\n");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");

    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_EQ(load_config(paths).agent.max_steps, 3u);
}

TEST(Config, TomlOnlyWorkspaceContributesNothing) {
    test::TempWorkspace workspace("config_toml_only");
    workspace.write("global.jsonc", "{}\n");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");

    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    Config expected;
    apply_env_overrides(expected);
    EXPECT_TRUE(same_config(load_config(paths), expected));
}

TEST(Config, MissingGlobalConfigThrows) {
    try {
        (void)load_config(ConfigPaths{});
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_EQ(std::string{error.what()}, "config: required global config path is empty");
    }
}

TEST(Config, MissingGlobalFileThrows) {
    test::TempWorkspace         workspace("config_global_absent");
    const std::filesystem::path absent = workspace.path() / "absent.jsonc";
    ConfigPaths                 paths;
    paths.global = absent;
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_EQ(std::string{error.what()},
                  "config " + absent.string() + ": required global config not found");
    }
}

TEST(Config, TopLevelUnknownKeyMessage) {
    test::TempWorkspace workspace("config_unknown_toplevel");
    workspace.write(".ymh/config.jsonc", "{ \"auto_compact_enabled\": true }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("unknown key 'auto_compact_enabled'"), std::string::npos) << message;
        EXPECT_EQ(message.find("'.auto_compact_enabled"), std::string::npos) << message;
    }
}

TEST(Config, NestedUnknownKeyMessage) {
    test::TempWorkspace workspace("config_unknown_nested");
    workspace.write(".ymh/config.jsonc", "{ \"agent\": { \"compaction\": { \"foo\": 1 } } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("unknown key 'agent.compaction.foo'"),
                  std::string::npos)
            << error.what();
    }
}

TEST(Config, WorkspaceLayerOptional) {
    test::TempWorkspace workspace("config_workspace_optional");
    ConfigPaths         paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace.path() / "absent.jsonc";
    Config expected;
    apply_env_overrides(expected);
    EXPECT_TRUE(same_config(load_config(paths), expected));
}

TEST(Config, SessionPersistPromptTextDefaultsOffAndParsesGlobally) {
    Config defaults;
    EXPECT_FALSE(defaults.session.persist_prompt_text);

    test::TempWorkspace workspace("config_session_prompt");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc", "{ \"session\": { \"persist_prompt_text\": true } }\n");

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace.path() / "absent.jsonc";
    EXPECT_TRUE(load_config(paths).session.persist_prompt_text);
}

TEST(Config, SessionSectionRejectedInWorkspaceLayer) {
    test::TempWorkspace workspace("config_session_workspace");
    workspace.write(".ymh/config.jsonc",
                    "{ \"session\": { \"persist_prompt_text\": true } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError for a workspace-layer [session] section";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("global-layer only"), std::string::npos)
            << error.what();
    }
}

TEST(Config, ApplyJsoncRequiredFlag) {
    test::TempWorkspace workspace("config_required_flag");
    const std::filesystem::path absent = workspace.path() / "absent.jsonc";

    Config config;
    EXPECT_THROW(apply_jsonc_file(config, std::filesystem::path{}, true), ConfigError);
    EXPECT_THROW(apply_jsonc_file(config, absent, true), ConfigError);
    EXPECT_NO_THROW(apply_jsonc_file(config, std::filesystem::path{}, false));
    EXPECT_NO_THROW(apply_jsonc_file(config, absent, false));
}

TEST(Config, GlobalConfigDirectoryRejected) {
    test::TempWorkspace         workspace("config_global_dir");
    const std::filesystem::path directory = workspace.path() / "adir";
    std::filesystem::create_directories(directory);
    ConfigPaths paths;
    paths.global = directory;
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_EQ(std::string{error.what()},
                  "config " + directory.string() + ": config path is not a regular file");
    }
}

TEST(Config, WorkspaceNonRegularRejected) {
    test::TempWorkspace         workspace("config_workspace_dir");
    const std::filesystem::path directory = workspace.path() / "adir";
    std::filesystem::create_directories(directory);
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = directory;
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_EQ(std::string{error.what()},
                  "config " + directory.string() + ": config path is not a regular file");
    }
}

TEST(Config, McpServersObjectParsesAndNormalizes) {
    test::TempWorkspace         workspace("config_mcp_servers_parse");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    R"JSONC({
  "mcp_servers": {
    "1234": { "command": "digits" },
    "BRAVE-SEARCH": { "command": "other" },
    "brave-search": {
      "type": "stdio",
      "command": "python3",
      "args": ["a"],
      "env": { "TOKEN": "x", "A": "y" }
    }
  }
})JSONC");
    ConfigPaths paths;
    paths.global = global;
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 3u);
    EXPECT_EQ(config.mcp.servers[0].id, "s1234");
    EXPECT_EQ(config.mcp.servers[1].id, "brave_search");
    EXPECT_EQ(config.mcp.servers[2].id, "brave_search_2");
    EXPECT_EQ(config.mcp.servers[2].transport, "stdio");
    EXPECT_EQ(config.mcp.servers[2].command, "python3");
    EXPECT_EQ(config.mcp.servers[2].env, (std::vector<std::string>{"A=y", "TOKEN=x"}));
}

TEST(Config, McpServersTransportAliasesAndHeaders) {
    nlohmann::json table = nlohmann::json::object();
    table["net"] = {{"type", "sse"}, {"url", "https://example.test/sse"},
                    {"headers", {{"Authorization", "Bearer x"}}}};
    McpSettings mcp;
    apply_mcp_servers_object(mcp, table, std::filesystem::path{"test.jsonc"});
    ASSERT_EQ(mcp.servers.size(), 1u);
    EXPECT_EQ(mcp.servers[0].transport, "http_sse");
    EXPECT_EQ(mcp.servers[0].url, "https://example.test/sse");
    EXPECT_EQ(mcp.servers[0].header_env, (std::vector<std::string>{"Authorization=Bearer x"}));
}

TEST(Config, McpServersDedupeTruncatesBaseFirst) {
    const std::string base(32, 'a');
    nlohmann::json    table = nlohmann::json::object();
    table[base]             = {{"command", "x"}};
    table[base + "-"]       = {{"command", "y"}};
    McpSettings mcp;
    apply_mcp_servers_object(mcp, table, std::filesystem::path{"test.jsonc"});
    ASSERT_EQ(mcp.servers.size(), 2u);
    EXPECT_EQ(mcp.servers[0].id, base);
    EXPECT_EQ(mcp.servers[1].id, std::string(30, 'a') + "_2");
    EXPECT_TRUE(is_valid_mcp_server_id(mcp.servers[0].id));
    EXPECT_TRUE(is_valid_mcp_server_id(mcp.servers[1].id));
}

TEST(Config, McpServersDedupeExhaustionThrows) {
    const std::string base(32, 'z');
    nlohmann::json    table = nlohmann::json::object();
    table[std::string(32, 'Z')] = {{"command", "x"}};
    for (std::size_t n = 2; n <= kMaxMcpDedupeAttempts; ++n) {
        const std::string suffix = "_" + std::to_string(n);
        const std::size_t room   = suffix.size() < 32 ? 32 - suffix.size() : 0;
        table[base.substr(0, room) + suffix] = {{"command", "x"}};
    }
    table[base] = {{"command", "x"}};
    McpSettings mcp;
    EXPECT_THROW(apply_mcp_servers_object(mcp, table, std::filesystem::path{"test.jsonc"}),
                 ConfigError);
}

TEST(Config, McpServersStrictness) {
    const std::filesystem::path source{"test.jsonc"};
    {
        nlohmann::json table = {{"s", {{"unknown", "x"}}}};
        McpSettings    mcp;
        EXPECT_THROW(apply_mcp_servers_object(mcp, table, source), ConfigError);
    }
    {
        nlohmann::json table = {{"s", {{"command", "c"}, {"env", {{"K", 5}}}}}};
        McpSettings    mcp;
        EXPECT_THROW(apply_mcp_servers_object(mcp, table, source), ConfigError);
    }
    {
        nlohmann::json table = {{"bad/name", {{"command", "c"}}}};
        McpSettings    mcp;
        EXPECT_THROW(apply_mcp_servers_object(mcp, table, source), ConfigError);
    }
    {
        nlohmann::json table = {
            {"s", {{"type", "stdio"}, {"transport", "http_sse"}, {"url", "u"}}}};
        McpSettings mcp;
        EXPECT_THROW(apply_mcp_servers_object(mcp, table, source), ConfigError);
    }
    {
        nlohmann::json table = {{"s", {{"type", "http"}}}};
        McpSettings    mcp;
        EXPECT_THROW(apply_mcp_servers_object(mcp, table, source), ConfigError);
    }
}

TEST(Config, McpMigrationOldArrayAloneLoads) {
    test::TempWorkspace         workspace("config_mcp_old");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    R"JSONC({"mcp": {"server": [{"id": "alpha", "command": "a"}]}})JSONC");
    ConfigPaths paths;
    paths.global = global;
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_EQ(config.mcp.servers[0].id, "alpha");
}

TEST(Config, McpMigrationNewObjectAloneLoads) {
    test::TempWorkspace         workspace("config_mcp_new");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    R"JSONC({"mcp_servers": {"alpha": {"command": "a"}}})JSONC");
    ConfigPaths paths;
    paths.global = global;
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_EQ(config.mcp.servers[0].id, "alpha");
}

TEST(Config, McpMigrationBothKeysSameLayerRejected) {
    test::TempWorkspace         workspace("config_mcp_both");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    R"JSONC({"mcp": {"server": [{"id": "a", "command": "x"}]},
                              "mcp_servers": {"b": {"command": "y"}}})JSONC");
    ConfigPaths paths;
    paths.global = global;
    try {
        (void)load_config(paths);
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("not both"), std::string::npos) << error.what();
    }
}

TEST(Config, McpMigrationEmptyOldArrayWithNewObjectLoads) {
    test::TempWorkspace         workspace("config_mcp_empty_old");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    R"JSONC({"mcp": {"server": []}, "mcp_servers": {"b": {"command": "y"}}})JSONC");
    ConfigPaths paths;
    paths.global = global;
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_EQ(config.mcp.servers[0].id, "b");
}

TEST(Config, McpMigrationWorkspaceReplacesGlobalWholesale) {
    test::TempWorkspace         workspace("config_mcp_layers");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    workspace.write("global.jsonc",
                    R"JSONC({"mcp": {"server": [{"id": "g", "command": "g"}]}})JSONC");
    workspace.write(".ymh/config.jsonc",
                    R"JSONC({"mcp_servers": {"w": {"command": "w"}}})JSONC");
    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    ASSERT_EQ(config.mcp.servers.size(), 1u);
    EXPECT_EQ(config.mcp.servers[0].id, "w");
}

TEST(Config, BuildLocalcodeImportMapsSupportedKeys) {
    const nlohmann::json localcode = nlohmann::json::parse(R"JSON({
      "mcp_servers": {
        "brave-search": {
          "type": "stdio", "command": "python3", "args": ["a"],
          "env": { "TOKEN": "secret", "REF": "${HOST}" }, "required": true
        }
      },
      "auto_compact_enabled": true,
      "auto_compact_percent": 80,
      "max_concurrent_tasks": 3,
      "skip_permissions": true,
      "permission": { "bash": [{ "match": "*.sh", "decision": "allow" }] },
      "default_profile": "p",
      "profiles": { "p": { "provider": "prov", "model": "m", "max_tokens": 5,
                           "context_window": 128000 } },
      "providers": { "prov": { "type": "openai-compatible",
                               "base_url": "https://x.test/v1", "api_key": "SECRET" } },
      "orchestrate": true
    })JSON");

    std::string                         error;
    const std::optional<nlohmann::json> document = build_localcode_import(localcode, default_import_profile_id(), error);
    ASSERT_TRUE(document.has_value()) << error;
    const nlohmann::json& doc = *document;

    ASSERT_TRUE(doc.contains("mcp_servers"));
    const nlohmann::json& server = doc["mcp_servers"]["brave-search"];
    EXPECT_FALSE(server["required"].get<bool>());
    EXPECT_EQ(server["env"]["TOKEN"].get<std::string>(), "secret");
    EXPECT_EQ(server["env"]["REF"].get<std::string>(), "${HOST}");
    EXPECT_FALSE(doc.contains("providers"));
    EXPECT_FALSE(doc.contains("orchestrate"));
    EXPECT_TRUE(doc["agent"]["compaction"]["enabled"].get<bool>());
    EXPECT_DOUBLE_EQ(doc["agent"]["compaction"]["threshold_ratio"].get<double>(), 0.8);
    EXPECT_EQ(doc["agent"]["compaction"]["context_window_tokens"].get<std::int64_t>(), 128000);
    EXPECT_EQ(doc["llm"]["default"]["base_url"].get<std::string>(), "https://x.test/v1");
    EXPECT_EQ(doc["llm"]["default"]["model"].get<std::string>(), "m");
    EXPECT_EQ(doc["llm"]["default"]["api_key"].get<std::string>(), "SECRET");
    EXPECT_EQ(doc["llm"]["default"]["max_tokens"].get<std::int64_t>(), 5);
    EXPECT_EQ(doc["llm"]["default"]["max_concurrency"].get<std::int64_t>(), 3);
    EXPECT_EQ(doc["permissions"]["default"].get<std::string>(), "allow");
    ASSERT_EQ(doc["permissions"]["rules"].size(), 1u);
    EXPECT_EQ(doc["permissions"]["rules"][0]["tool"].get<std::string>(), "shell");
    EXPECT_EQ(doc["permissions"]["rules"][0]["command"].get<std::string>(), "*.sh");
    EXPECT_EQ(doc["permissions"]["rules"][0]["effect"].get<std::string>(), "allow");
    EXPECT_EQ(doc["permissions"]["rules"][0]["id"].get<std::string>(), "localcode.rule.bash.0");
}

TEST(Config, BuildLocalcodeImportEscapesLiteralDollarBrace) {
    const nlohmann::json localcode = nlohmann::json::parse(R"JSON({
      "mcp_servers": {
        "s": { "command": "echo ${not_a_ref", "env": { "K": "a${1}" } }
      }
    })JSON");
    std::string                         error;
    const std::optional<nlohmann::json> document = build_localcode_import(localcode, default_import_profile_id(), error);
    ASSERT_TRUE(document.has_value()) << error;
    EXPECT_EQ((*document)["mcp_servers"]["s"]["command"].get<std::string>(), "echo $${not_a_ref");
    EXPECT_EQ((*document)["mcp_servers"]["s"]["env"]["K"].get<std::string>(), "a$${1}");
}

TEST(Config, BuildLocalcodeImportSkipsNonOpenAiProvider) {
    const nlohmann::json localcode = nlohmann::json::parse(R"JSON({
      "default_profile": "p",
      "profiles": { "p": { "provider": "prov", "model": "m" } },
      "providers": { "prov": { "type": "anthropic", "base_url": "https://x.test/v1" } }
    })JSON");
    std::string                         error;
    const std::optional<nlohmann::json> document = build_localcode_import(localcode, default_import_profile_id(), error);
    ASSERT_TRUE(document.has_value()) << error;
    EXPECT_FALSE(document->contains("llm"));
}

TEST(Config, LocalcodeConfigPathUsesHome) {
    test::TempWorkspace workspace("config_localcode_path");
    ScopedEnv           home("HOME", (workspace.path() / "home").string());
    EXPECT_EQ(localcode_config_path(), workspace.path() / "home" / ".localcode" / "config.json");
}

// 25 review M7 / UX-F14: a hand-written stdio server with no command must fail
// at config load (ConfigError), not brick daemon start inside McpManager.
TEST(Config, McpServersStdioRequiresNonEmptyCommand) {
    const std::filesystem::path source{"test.jsonc"};
    for (const nlohmann::json& entry :
         std::vector<nlohmann::json>{{{"type", "stdio"}}, nlohmann::json::object()}) {
        nlohmann::json table = {{"s", entry}};
        McpSettings    mcp;
        EXPECT_THROW(apply_mcp_servers_object(mcp, table, source), ConfigError);
    }
}

// 25 review M5 / UX-F14: a non-string env/header value aborts the import with
// an error naming the server and key, and no document is produced.
TEST(Config, BuildLocalcodeImportRejectsNonStringEnvAndHeaders) {
    const nlohmann::json localcode = nlohmann::json::parse(R"JSON({
      "mcp_servers": {
        "brave-search": { "command": "c", "env": { "PORT": 8080, "TOKEN": "x" } }
      }
    })JSON");
    std::string                         error;
    const std::optional<nlohmann::json> document = build_localcode_import(localcode, default_import_profile_id(), error);
    EXPECT_FALSE(document.has_value());
    EXPECT_NE(error.find("mcp_servers.brave-search"), std::string::npos) << error;
    EXPECT_NE(error.find("PORT"), std::string::npos) << error;

    const nlohmann::json bad_headers = nlohmann::json::parse(R"JSON({
      "mcp_servers": { "s": { "url": "https://x", "headers": { "Authorization": 5 } } }
    })JSON");
    error.clear();
    EXPECT_FALSE(build_localcode_import(bad_headers, default_import_profile_id(), error).has_value());
    EXPECT_NE(error.find("mcp_servers.s"), std::string::npos) << error;
    EXPECT_NE(error.find("Authorization"), std::string::npos) << error;
}

TEST(Config, PresetsKeysParseWithDefaults) {
    Config defaults;
    EXPECT_EQ(defaults.presets.max_depth, 3u);
    EXPECT_TRUE(defaults.presets.include_shipped_root);
    EXPECT_TRUE(defaults.presets.include_user_root);
    EXPECT_FALSE(defaults.presets.root.has_value());
    EXPECT_FALSE(defaults.presets.default_id.has_value());

    test::TempWorkspace workspace("config_presets");
    workspace.write("global.jsonc",
                    "{ \"presets\": { \"root\": \"/tmp/presets\", \"default\": \"standard\", "
                    "\"include_shipped_root\": false, \"include_user_root\": false, "
                    "\"max_depth\": 5 } }\n");
    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace.path() / "absent.jsonc";
    const Config config = load_config(paths);

    ASSERT_TRUE(config.presets.root.has_value());
    EXPECT_EQ(config.presets.root->string(), "/tmp/presets");
    ASSERT_TRUE(config.presets.default_id.has_value());
    EXPECT_EQ(*config.presets.default_id, "standard");
    EXPECT_FALSE(config.presets.include_shipped_root);
    EXPECT_FALSE(config.presets.include_user_root);
    EXPECT_EQ(config.presets.max_depth, 5u);
}

TEST(Config, PresetsNegativeMaxDepthRejected) {
    test::TempWorkspace workspace("config_presets_negative");
    workspace.write(".ymh/config.jsonc", "{ \"presets\": { \"max_depth\": -1 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, PresetsNonIntegerMaxDepthRejected) {
    test::TempWorkspace workspace("config_presets_type");
    workspace.write(".ymh/config.jsonc", "{ \"presets\": { \"max_depth\": \"deep\" } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, PresetsUnknownKeyRejected) {
    test::TempWorkspace workspace("config_presets_unknown");
    workspace.write(".ymh/config.jsonc", "{ \"presets\": { \"bogus\": 1 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, GoalsDefaults) {
    const Config config;
    EXPECT_EQ(config.goals.max_rounds, 256u);
    EXPECT_EQ(config.goals.blocked_after_consecutive_rounds, 3u);
}

TEST(Config, GoalsKeysParse) {
    test::TempWorkspace workspace("config_goals_keys");
    workspace.write(".ymh/config.jsonc",
                    "{ \"goals\": { \"max_rounds\": 12, "
                    "\"blocked_after_consecutive_rounds\": 5 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    EXPECT_EQ(config.goals.max_rounds, 12u);
    EXPECT_EQ(config.goals.blocked_after_consecutive_rounds, 5u);
}

TEST(Config, GoalsZeroMaxRoundsRejected) {
    test::TempWorkspace workspace("config_goals_zero");
    workspace.write(".ymh/config.jsonc", "{ \"goals\": { \"max_rounds\": 0 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, GoalsUnknownKeyRejected) {
    test::TempWorkspace workspace("config_goals_unknown");
    workspace.write(".ymh/config.jsonc", "{ \"goals\": { \"bogus\": 1 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JobsDefaults) {
    const Config config;
    EXPECT_EQ(config.jobs.wait_timeout_ms, 30'000);
    EXPECT_EQ(config.jobs.max_wait_timeout_ms, 600'000);
    EXPECT_EQ(config.jobs.completion_delivery, CompletionDelivery::Wakeup);
    EXPECT_EQ(config.jobs.max_consecutive_wakes, 3u);
}

TEST(Config, JobsKeysParse) {
    test::TempWorkspace workspace("config_jobs_keys");
    workspace.write(".ymh/config.jsonc",
                    "{ \"jobs\": { \"wait_timeout_ms\": 500, \"max_wait_timeout_ms\": 9000, "
                    "\"completion_delivery\": \"quiet\", \"max_consecutive_wakes\": 7 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);
    EXPECT_EQ(config.jobs.wait_timeout_ms, 500);
    EXPECT_EQ(config.jobs.max_wait_timeout_ms, 9000);
    EXPECT_EQ(config.jobs.completion_delivery, CompletionDelivery::Quiet);
    EXPECT_EQ(config.jobs.max_consecutive_wakes, 7u);
}

TEST(Config, JobsUnknownDeliveryRejected) {
    test::TempWorkspace workspace("config_jobs_delivery");
    workspace.write(".ymh/config.jsonc", "{ \"jobs\": { \"completion_delivery\": \"bogus\" } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JobsNegativeWaitRejected) {
    test::TempWorkspace workspace("config_jobs_negative");
    workspace.write(".ymh/config.jsonc", "{ \"jobs\": { \"wait_timeout_ms\": -1 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Config, JobsUnknownKeyRejected) {
    test::TempWorkspace workspace("config_jobs_unknown");
    workspace.write(".ymh/config.jsonc", "{ \"jobs\": { \"bogus\": 1 } }\n");
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

} // namespace

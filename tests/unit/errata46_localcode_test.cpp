#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <sys/stat.h>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/llm/redaction.hpp"

namespace {

using namespace ymh;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
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

void write_file(const std::filesystem::path& path, const std::string& body, mode_t mode = 0600) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << body;
    ::chmod(path.c_str(), mode);
}

nlohmann::json import(const nlohmann::json& localcode) {
    std::string                          error;
    std::optional<LocalcodeImportResult> result = build_localcode_import(localcode, error);
    EXPECT_TRUE(result.has_value()) << error;
    return result.has_value() ? result->document : nlohmann::json::object();
}

constexpr const char* kProviderProfile = R"JSON({
  "default_profile": "p",
  "profiles": { "p": { "provider": "prov", "model": "m", "max_tokens": 42,
                       "context_window": 128000 } },
  "providers": { "prov": { "type": "openai-compatible",
                           "base_url": "https://x.test/v1", "api_key": "SECRET" } }
})JSON";

TEST(Errata46D12, UI46_D12_ImportApiKey) {
    const nlohmann::json doc = import(nlohmann::json::parse(kProviderProfile));
    EXPECT_EQ(doc["llm"]["endpoints"]["prov"]["api_key"].get<std::string>(), "SECRET");
}

TEST(Errata46D12, UI46_D12_ApiKeyReachesProvider) {
    Config config;
    config.llm.api_key = "SECRET";
    const LLMProviderConfig provider = to_provider_config(config);
    ASSERT_TRUE(provider.api_key.has_value());
    EXPECT_EQ(*provider.api_key, "SECRET");
}

TEST(Errata46D12, UI46_D12_ApiKeyEnvOptional) {
    LLMProviderConfig config = deepseek_config();
    config.api_key           = "SECRET";
    config.api_key_env.clear();
    ProviderRegistry registry = make_default_provider_registry();
    const auto       created  = registry.create(config);
    EXPECT_TRUE(created.has_value());
}

TEST(Errata46D12, UI46_D12_WorkspaceLayerApiKeyRejected) {
    test::TempWorkspace         workspace("d12_ws_key");
    const std::filesystem::path global    = workspace.path() / "global" / "config.jsonc";
    const std::filesystem::path workspace_config = workspace.path() / ".ymh" / "config.jsonc";
    write_file(global, R"JSON({"llm":{"default":{"model":"g"}}})JSON");
    write_file(workspace_config, R"JSON({"llm":{"default":{"api_key":"SECRET"}}})JSON");

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace_config;
    EXPECT_THROW((void)load_config(paths), ConfigError);

    write_file(global, R"JSON({"llm":{"default":{"api_key":"SECRET"}}})JSON");
    ConfigPaths global_only;
    global_only.global = global;
    const Config loaded = load_config(global_only);
    ASSERT_TRUE(loaded.llm.api_key.has_value());
    EXPECT_EQ(*loaded.llm.api_key, "SECRET");
}

TEST(Errata46D12, UI46_D12_ScaffoldIs0600) {
    test::TempWorkspace         workspace("d12_scaffold");
    const std::filesystem::path global = workspace.path() / "config" / "ymh" / "config.jsonc";
    const ScaffoldResult        result = scaffold_config(workspace.path(), global);
    ASSERT_TRUE(result.global_config_created);
    struct stat info {};
    ASSERT_EQ(::stat(global.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 077, 0);
}

TEST(Errata46D12, UI46_D12_RedactsJsonApiKey) {
    const std::string redacted = redact_secrets(R"({"api_key": "SECRET123"})");
    EXPECT_EQ(redacted.find("SECRET123"), std::string::npos);
    EXPECT_NE(redacted.find("[REDACTED]"), std::string::npos);
}

TEST(Errata46D12, UI46_D12_ImportSkipPermissions) {
    const nlohmann::json doc =
        import(nlohmann::json::parse(R"JSON({"skip_permissions": true})JSON"));
    EXPECT_EQ(doc["permissions"]["default"].get<std::string>(), "allow");
}

TEST(Errata46D12, UI46_D12_ImportPermissionRulesObject) {
    const nlohmann::json doc = import(nlohmann::json::parse(
        R"JSON({"permission":{"bash":[{"match":"dir *","decision":"allow"}]}})JSON"));
    ASSERT_EQ(doc["permissions"]["rules"].size(), 1u);
    EXPECT_EQ(doc["permissions"]["rules"][0]["tool"].get<std::string>(), "shell");
    EXPECT_EQ(doc["permissions"]["rules"][0]["command"].get<std::string>(), "dir *");
    EXPECT_EQ(doc["permissions"]["rules"][0]["effect"].get<std::string>(), "allow");
}

TEST(Errata46D12, UI46_D12_InvalidDecisionDropped) {
    const nlohmann::json doc = import(nlohmann::json::parse(
        R"JSON({"permission":{"bash":[{"match":"ls","decision":"allow"},{"match":"rm","decision":"maybe"}]}})JSON"));
    ASSERT_EQ(doc["permissions"]["rules"].size(), 1u);
    EXPECT_EQ(doc["permissions"]["rules"][0]["command"].get<std::string>(), "ls");
}

TEST(Errata46D12, UI46_D12_FlatPermissionArraySkipped) {
    const nlohmann::json doc = import(nlohmann::json::parse(
        R"JSON({"permission":[{"match":"*.sh","decision":"allow"}]})JSON"));
    EXPECT_FALSE(doc.contains("permissions"));
}

TEST(Errata46D12, UI46_D12_OpenAiCompatTypeAccepted) {
    const nlohmann::json doc = import(nlohmann::json::parse(R"JSON({
      "default_profile": "p",
      "profiles": { "p": { "provider": "prov", "model": "m" } },
      "providers": { "prov": { "type": "openai-compat", "base_url": "https://x.test/v1" } }
    })JSON"));
    EXPECT_EQ(doc["llm"]["models"]["p"]["model"].get<std::string>(), "m");
    EXPECT_EQ(doc["llm"]["models"]["p"]["endpoint"].get<std::string>(), "prov");
    EXPECT_FALSE(doc["llm"]["models"]["p"].contains("profile"));
}

TEST(Errata46D12, UI46_D12_BedrockTypeSkipped) {
    const nlohmann::json doc = import(nlohmann::json::parse(R"JSON({
      "default_profile": "p",
      "profiles": { "p": { "provider": "prov", "model": "m" } },
      "providers": { "prov": { "type": "bedrock", "base_url": "https://x.test/v1" } }
    })JSON"));
    EXPECT_FALSE(doc.contains("llm"));
}

TEST(Errata46D12, UI46_D12_ImportMaxTokens) {
    const nlohmann::json doc = import(nlohmann::json::parse(kProviderProfile));
    EXPECT_EQ(doc["llm"]["models"]["p"]["max_tokens"].get<std::int64_t>(), 42);
}

TEST(Errata46D12, UI46_D12_MaxTokensTargetsCallConfig) {
    Config config;
    config.llm.max_tokens = 42;
    const AgentConfig agent = to_agent_config(config);
    ASSERT_TRUE(agent.parameters.max_output_tokens.has_value());
    EXPECT_EQ(*agent.parameters.max_output_tokens, 42u);
}

TEST(Errata46D12, UI46_D12_ModelFieldsRetained) {
    const nlohmann::json doc = import(nlohmann::json::parse(kProviderProfile));
    EXPECT_EQ(doc["llm"]["endpoints"]["prov"]["base_url"].get<std::string>(), "https://x.test/v1");
    EXPECT_EQ(doc["llm"]["models"]["p"]["model"].get<std::string>(), "m");
    EXPECT_EQ(doc["llm"]["models"]["p"]["context_window"].get<std::int64_t>(), 128000);
}

TEST(Errata46D12, UI46_D12_NoProfileNoImport) {
    const nlohmann::json doc =
        import(nlohmann::json::parse(R"JSON({"mcp_servers":{}})JSON"));
    EXPECT_FALSE(doc.contains("llm"));
}

TEST(Errata46D12, UI46_D12_GlobalLayerOverridable) {
    test::TempWorkspace         workspace("d12_layers");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    const std::filesystem::path local  = workspace.path() / ".ymh" / "config.jsonc";
    write_file(global, R"JSON({"llm":{"default":{"model":"global-model"}}})JSON");
    write_file(local, R"JSON({"llm":{"default":{"model":"workspace-model"}}})JSON");

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = local;
    const Config config = load_config(paths);
    EXPECT_EQ(config.llm.model, "workspace-model");
}

TEST(Errata46D12, UI46_D12_NoReimportWhenGlobalExists) {
    test::TempWorkspace workspace("d12_reimport");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    const std::filesystem::path localcode =
        workspace.path() / "home" / ".localcode" / "config.json";
    write_file(localcode, R"JSON({"mcp_servers":{"s":{"command":"c"}}})JSON", 0644);
    const std::filesystem::path global = workspace.path() / "config" / "ymh" / "config.jsonc";
    std::filesystem::create_directories(global.parent_path());

    CliInvocation invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("\n");
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_FALSE(maybe_import_localcode_config(invocation, global, /*interactive=*/true, in, out,
                                               err));
    EXPECT_FALSE(std::filesystem::exists(global));
}

TEST(Errata46D12, UI46_D12_LocalcodeFixture) {
    const nlohmann::json doc = import(nlohmann::json::parse(R"JSON({
      "skip_permissions": true,
      "permission": { "bash": [ { "match": "dir *", "decision": "allow" },
                                { "match": "rm *", "decision": "deny" } ] },
      "default_profile": "p",
      "profiles": { "p": { "provider": "prov", "model": "m", "max_tokens": 42,
                           "context_window": 128000 } },
      "providers": { "prov": { "type": "openai-compat",
                               "base_url": "https://x.test/v1", "api_key": "SECRET" } },
      "mcp_servers": { "s": { "command": "c" } }
    })JSON"));
    EXPECT_EQ(doc["permissions"]["default"].get<std::string>(), "allow");
    ASSERT_EQ(doc["permissions"]["rules"].size(), 2u);
    EXPECT_EQ(doc["llm"]["endpoints"]["prov"]["api_key"].get<std::string>(), "SECRET");
    EXPECT_EQ(doc["llm"]["models"]["p"]["model"].get<std::string>(), "m");
    EXPECT_EQ(doc["llm"]["models"]["p"]["max_tokens"].get<std::int64_t>(), 42);
    EXPECT_TRUE(doc.contains("mcp_servers"));
}

} // namespace

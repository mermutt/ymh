#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "support/test_env.hpp"
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream      input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

Config load_global(const test::TempWorkspace& workspace, const std::string& body,
                   mode_t mode = 0600) {
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    write_file(global, body, mode);
    ConfigPaths paths;
    paths.global = global;
    return load_config(paths);
}

Config load_layers(const test::TempWorkspace& workspace, const std::string& global_body,
                   const std::string& workspace_body) {
    const std::filesystem::path global    = workspace.path() / "global.jsonc";
    const std::filesystem::path workspace_config = workspace.path() / ".ymh" / "config.jsonc";
    write_file(global, global_body, 0600);
    write_file(workspace_config, workspace_body, 0600);
    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = workspace_config;
    return load_config(paths);
}

constexpr const char* kTwoEndpoints = R"JSON({
  "llm": {
    "endpoints": {
      "ted-ai": { "provider": "openai-compatible",
                  "base_url": "https://ted.example/v1",
                  "api_key": "TED-SECRET" },
      "itg":    { "provider": "openai-compatible",
                  "base_url": "https://itg.example/v1" }
    },
    "models": {
      "strong":   { "endpoint": "ted-ai", "model": "Muse-Glimmer-30B",
                    "max_tokens": 8192, "context_window": 32768 },
      "balanced": { "endpoint": "ted-ai", "model": "Muse-Glimmer-30B",
                    "reasoning_effort": "low", "temperature": 1.0 },
      "cheap":    { "endpoint": "itg", "model": "nvidia/nemotron-3-super" }
    },
    "active_model": "balanced"
  }
})JSON";

constexpr const char* kRealLocalcodeShape = R"JSON({
  "skip_permissions": true,
  "permission": { "bash": [ { "match": "dir *", "decision": "allow" } ] },
  "default_profile": "balanced",
  "profiles": {
    "strong":   { "provider": "ted-ai", "model": "Muse-Glimmer-30B",
                  "max_tokens": 8192, "context_window": 32768 },
    "balanced": { "provider": "ted-ai", "model": "Muse-Glimmer-30B",
                  "max_tokens": 8192, "context_window": 32768 },
    "cheap":    { "provider": "itg", "model": "nvidia/nemotron-3-super" },
    "cat-md1":  { "provider": "itg", "model": "gemma-4-31B-it",
                  "max_tokens": 4096, "context_window": 8192 },
    "cat-gma":  { "provider": "itg", "model": "gemma-4-31B-it",
                  "max_tokens": 4096, "context_window": 8192 }
  },
  "providers": {
    "bedrock": { "type": "bedrock", "region": "us-east-1", "profile": "p" },
    "itg":     { "type": "openai-compat", "base_url": "https://itg.example/v1",
                 "api_key": "ITG-SECRET" },
    "ted-ai":  { "type": "openai-compatible", "base_url": "https://ted.example/v1",
                 "api_key": "TED-SECRET" }
  }
})JSON";

nlohmann::json import_document(const nlohmann::json& localcode, std::vector<std::string>* notes) {
    std::string                          error;
    std::optional<LocalcodeImportResult> result = build_localcode_import(localcode, error);
    EXPECT_TRUE(result.has_value()) << error;
    if (!result.has_value()) {
        return nlohmann::json::object();
    }
    if (notes != nullptr) {
        *notes = result->notes;
    }
    return result->document;
}

TEST(Config52, EndpointsParse) {
    test::TempWorkspace workspace("s52_endpoints");
    const Config        config = load_global(workspace, kTwoEndpoints);

    EXPECT_EQ(config.llm.endpoints.size(), 2u);
    EXPECT_EQ(config.llm.models.size(), 3u);

    const ResolvedModel resolved = resolve_model(config);
    EXPECT_EQ(resolved.source, "llm.active_model");
    EXPECT_EQ(resolved.model_name, "balanced");
    EXPECT_EQ(resolved.model_id, "Muse-Glimmer-30B");
    EXPECT_EQ(resolved.endpoint.name, "ted-ai");
    EXPECT_EQ(resolved.endpoint.base_url, "https://ted.example/v1");
    ASSERT_TRUE(resolved.endpoint.api_key.has_value());
    EXPECT_EQ(*resolved.endpoint.api_key, "TED-SECRET");
    ASSERT_TRUE(resolved.reasoning_effort.has_value());
    EXPECT_EQ(*resolved.reasoning_effort, "low");
}

TEST(Config52, ModelUnknownEndpoint) {
    test::TempWorkspace workspace("s52_dangling");
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({
          "llm": { "models": { "m": { "endpoint": "nope", "model": "x" } } }
        })JSON"),
        ConfigError);
}

TEST(Config52, ActiveModelUnknown) {
    test::TempWorkspace workspace("s52_active_unknown");
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({
          "llm": { "models": { "m": { "endpoint": "e", "model": "x" } },
                   "endpoints": { "e": {} },
                   "active_model": "nope" }
        })JSON"),
        ConfigError);
}

TEST(Config52, NameGrammar) {
    const std::string long_name(65, 'a');
    for (const std::string& name :
         std::vector<std::string>{"", "has space", "a/b", long_name, "-leading"}) {
        test::TempWorkspace workspace("s52_name");
        nlohmann::json     doc;
        doc["llm"]["endpoints"][name] = nlohmann::json::object();
        EXPECT_THROW((void)load_global(workspace, doc.dump()), ConfigError) << name;
    }
    test::TempWorkspace workspace("s52_name_ok");
    EXPECT_NO_THROW((void)load_global(
        workspace, R"JSON({"llm":{"endpoints":{"a.b_c-1":{}}}})JSON"));
}

TEST(Config52, UnknownKeyInsideEntry) {
    test::TempWorkspace workspace("s52_unknown_key");
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({"llm":{"endpoints":{"e":{"bogus":1}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({"llm":{"models":{"m":{"endpoint":"e","model":"x",
                                                          "bogus":1}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(workspace,
                          R"JSON({"llm":{"endpoints":{"e":{"retry":{"bogus":1}}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(workspace,
                          R"JSON({"llm":{"endpoints":{"e":{"headers":{"X":5}}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({"llm":{"endpoints":{"e":{"headers":[]}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({"llm":{"endpoints":{"e":{"retry":[]}}}})JSON"),
        ConfigError);
}

TEST(Config52, TypeCheckEndpointsModels) {
    test::TempWorkspace workspace("s52_types");
    EXPECT_THROW((void)load_global(workspace, R"JSON({"llm":{"endpoints":[]}})JSON"),
                 ConfigError);
    EXPECT_THROW((void)load_global(workspace, R"JSON({"llm":{"models":[]}})JSON"),
                 ConfigError);
    EXPECT_THROW((void)load_global(workspace, R"JSON({"llm":{"active_model":5}})JSON"),
                 ConfigError);
    EXPECT_THROW((void)load_global(workspace, R"JSON({"llm":{"default":[]}})JSON"),
                 ConfigError);
}

TEST(Config52, BackwardCompat) {
    test::TempWorkspace workspace("s52_backcompat");
    const Config config = load_global(workspace, R"JSON({
      "llm": { "default": { "provider": "openai-compatible",
                            "base_url": "https://legacy.example/v1",
                            "model": "legacy-model",
                            "api_key_env": "LEGACY_KEY",
                            "max_tokens": 321,
                            "temperature": 0.5,
                            "retry": { "max_attempts": 7 } } }
    })JSON");

    const ResolvedModel resolved = resolve_model(config);
    EXPECT_EQ(resolved.source, "llm.default.model");
    EXPECT_EQ(resolved.model_name, "");
    EXPECT_EQ(resolved.model_id, effective_model(config));
    EXPECT_EQ(resolved.model_id, "legacy-model");
    EXPECT_EQ(resolved.endpoint.name, "");
    EXPECT_EQ(resolved.endpoint.provider, config.llm.provider);
    EXPECT_EQ(resolved.endpoint.base_url, config.llm.base_url);
    EXPECT_EQ(resolved.endpoint.api_key_env, config.llm.api_key_env);
    ASSERT_TRUE(resolved.max_tokens.has_value());
    EXPECT_EQ(*resolved.max_tokens, 321u);
    ASSERT_TRUE(resolved.temperature.has_value());
    EXPECT_DOUBLE_EQ(*resolved.temperature, 0.5);

    const LLMProviderConfig provider = to_provider_config(config);
    EXPECT_EQ(provider.provider, config.llm.provider);
    EXPECT_EQ(provider.base_url, config.llm.base_url);
    EXPECT_EQ(provider.model, "legacy-model");
    EXPECT_EQ(provider.api_key_env, "LEGACY_KEY");
    EXPECT_EQ(provider.retry.max_attempts, 7u);
}

TEST(Config52, AgentModelDual) {
    test::TempWorkspace workspace("s52_dual");
    Config             config = load_global(workspace, kTwoEndpoints);

    config.agent.model = "balanced";
    const ResolvedModel named = resolve_model(config);
    EXPECT_EQ(named.source, "agent.model");
    EXPECT_EQ(named.model_name, "balanced");
    EXPECT_EQ(named.endpoint.name, "ted-ai");

    config.agent.model = "some-wire-id";
    const ResolvedModel literal = resolve_model(config);
    EXPECT_EQ(literal.source, "agent.model");
    EXPECT_EQ(literal.model_name, "");
    EXPECT_EQ(literal.model_id, "some-wire-id");
    EXPECT_EQ(literal.endpoint.name, "");
}

TEST(Config52, AgentModelNameWinsOverLiteralId) {
    test::TempWorkspace workspace("s52_name_wins");
    Config             config = load_global(workspace, R"JSON({
      "llm": {
        "endpoints": { "e": { "base_url": "https://named.example/v1" } },
        "models": { "Muse-Glimmer-30B": { "endpoint": "e", "model": "wire" } }
      }
    })JSON");
    config.agent.model = "Muse-Glimmer-30B";
    const ResolvedModel resolved = resolve_model(config);
    EXPECT_EQ(resolved.model_name, "Muse-Glimmer-30B");
    EXPECT_EQ(resolved.model_id, "wire");
    EXPECT_EQ(resolved.endpoint.name, "e");
}

TEST(Config52, EntryShadowWholesale) {
    test::TempWorkspace workspace("s52_shadow");
    const Config config = load_layers(
        workspace, R"JSON({"llm":{"endpoints":{"e":{"base_url":"https://global.example/v1"}}}})JSON",
        R"JSON({"llm":{"endpoints":{"e":{}}}})JSON");
    EXPECT_EQ(config.llm.endpoints.at("e").base_url, "https://api.deepseek.com/v1");
}

TEST(Config52, EntryShadowCredentialRejected) {
    test::TempWorkspace workspace("s52_shadow_cred");
    EXPECT_THROW((void)load_layers(
                     workspace,
                     R"JSON({"llm":{"endpoints":{"e":{"base_url":"https://global.example/v1",
                                                     "api_key":"SECRET"}}}})JSON",
                     R"JSON({"llm":{"endpoints":{"e":{"base_url":"https://ws.example/v1"}}}})JSON"),
                 ConfigError);
}

TEST(Config52, HeadersGlobalOnly) {
    test::TempWorkspace workspace("s52_headers_ws");
    EXPECT_THROW((void)load_layers(workspace, "{}",
                                   R"JSON({"llm":{"endpoints":{"e":{"headers":{"X":"1"}}}}})JSON"),
                 ConfigError);

    test::TempWorkspace world_readable("s52_headers_0644");
    try {
        (void)load_global(world_readable,
                          R"JSON({"llm":{"endpoints":{"e":{"headers":{"X":"1"}}}}})JSON",
                          0644);
        FAIL() << "a 0644 config carrying headers must be rejected";
    } catch (const ConfigError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("llm.endpoints.e.headers"), std::string::npos) << message;
        EXPECT_EQ(message.find("llm.api_key"), std::string::npos) << message;
    }

    test::TempWorkspace ok("s52_headers_ok");
    const Config config =
        load_global(ok, R"JSON({"llm":{"endpoints":{"e":{"headers":{"X-Client":"ymh"}}}}})JSON");
    EXPECT_EQ(config.llm.endpoints.at("e").headers.size(), 1u);
}

TEST(Config52, LiteralKeyWorkspaceLayer) {
    test::TempWorkspace workspace("s52_key_ws");
    EXPECT_THROW((void)load_layers(workspace, "{}",
                                   R"JSON({"llm":{"endpoints":{"e":{"api_key":"SECRET"}}}})JSON"),
                 ConfigError);

    test::TempWorkspace world_readable("s52_key_0644");
    EXPECT_THROW((void)load_global(
                     world_readable,
                     R"JSON({"llm":{"endpoints":{"e":{"api_key":"SECRET"}}}})JSON", 0644),
                 ConfigError);
}

TEST(Config52, MissingRequiredModelField) {
    test::TempWorkspace workspace("s52_missing");
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({"llm":{"models":{"m":{"model":"x"}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(workspace, R"JSON({"llm":{"models":{"m":{"endpoint":"e"}}}})JSON"),
        ConfigError);
    EXPECT_THROW(
        (void)load_global(
            workspace, R"JSON({"llm":{"endpoints":{"e":{}},"models":{"m":{"endpoint":"","model":"x"}}}})JSON"),
        ConfigError);

    const Config config =
        load_global(workspace, R"JSON({"llm":{"endpoints":{"e":{}}}})JSON");
    EXPECT_EQ(config.llm.endpoints.at("e").provider, "openai-compatible");
    EXPECT_EQ(config.llm.endpoints.at("e").base_url, "https://api.deepseek.com/v1");
    EXPECT_TRUE(config.llm.endpoints.at("e").api_key_env.empty());
}

TEST(Config52, SecretNeverLogged) {
    const std::string config_text =
        R"JSON({"llm":{"endpoints":{"e":{"api_key":"sk-SECRET","headers":{"Authorization":"Bearer tok"}}}}})JSON";
    const std::string redacted = redact_secrets(config_text);
    EXPECT_EQ(redacted.find("sk-SECRET"), std::string::npos);
    EXPECT_EQ(redacted.find("Bearer tok"), std::string::npos);
    EXPECT_NE(redacted.find("[REDACTED]"), std::string::npos);
    EXPECT_NE(redacted.find("Authorization"), std::string::npos);

    test::TempWorkspace workspace("s52_secret_provider");
    const Config config = load_global(workspace, R"JSON({
      "llm": { "endpoints": { "e": { "api_key": "sk-SECRET",
                                     "headers": { "X-Api-Key": "sk-SECRET" } } },
               "models": { "m": { "endpoint": "e", "model": "wire" } },
               "active_model": "m" }
    })JSON");
    const LLMProviderConfig provider = to_provider_config(config);
    ASSERT_TRUE(provider.api_key.has_value());
    EXPECT_EQ(*provider.api_key, "sk-SECRET");
    const std::string rendered =
        redact_secrets(R"({"headers":{"X-Api-Key":"header-secret-value"}})");
    EXPECT_EQ(rendered.find("header-secret-value"), std::string::npos);
    EXPECT_NE(rendered.find("[REDACTED]"), std::string::npos);
    EXPECT_NE(rendered.find("X-Api-Key"), std::string::npos);
}

TEST(Config52, ActiveEndpointSelector) {
    test::TempWorkspace workspace("s52_selector");
    Config             config = load_global(workspace, kTwoEndpoints);
    config.llm.active_endpoint = "itg";
    const ResolvedModel resolved = resolve_model(config);
    EXPECT_EQ(resolved.source, "llm.active_model");
    EXPECT_EQ(resolved.endpoint.name, "ted-ai");

    Config literal = load_global(workspace, kTwoEndpoints);
    literal.llm.active_model.reset();
    literal.llm.active_endpoint = "itg";
    const ResolvedModel picked = resolve_model(literal);
    EXPECT_EQ(picked.endpoint.name, "itg");

    literal.llm.active_endpoint = "nope";
    EXPECT_THROW((void)resolve_model(literal), ConfigError);
}

TEST(Import52, AllProvidersAllProfiles) {
    std::vector<std::string> notes;
    const nlohmann::json     doc =
        import_document(nlohmann::json::parse(kRealLocalcodeShape), &notes);

    ASSERT_TRUE(doc.contains("llm"));
    EXPECT_EQ(doc["llm"]["endpoints"].size(), 2u);
    EXPECT_TRUE(doc["llm"]["endpoints"].contains("itg"));
    EXPECT_TRUE(doc["llm"]["endpoints"].contains("ted-ai"));
    EXPECT_FALSE(doc["llm"]["endpoints"].contains("bedrock"));
    EXPECT_EQ(doc["llm"]["models"].size(), 5u);
    EXPECT_EQ(doc["llm"]["active_model"].get<std::string>(), "balanced");
    EXPECT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("bedrock"), std::string::npos) << notes[0];

    const nlohmann::json again =
        import_document(nlohmann::json::parse(kRealLocalcodeShape), nullptr);
    EXPECT_EQ(doc.dump(), again.dump());
}

TEST(Import52, BedrockSkippedWithNote) {
    std::vector<std::string> notes;
    const nlohmann::json     doc = import_document(nlohmann::json::parse(R"JSON({
      "default_profile": "p",
      "profiles": { "p": { "provider": "bedrock", "model": "m" } },
      "providers": { "bedrock": { "type": "bedrock", "region": "us-east-1" } }
    })JSON"),
                                                  &notes);
    EXPECT_FALSE(doc.contains("llm"));
    EXPECT_EQ(notes.size(), 2u);
}

TEST(Import52, MuseProfileOnlyForMuse) {
    const nlohmann::json doc =
        import_document(nlohmann::json::parse(kRealLocalcodeShape), nullptr);
    EXPECT_EQ(doc["llm"]["models"]["balanced"]["profile"].get<std::string>(), "muse-glimmer");
    EXPECT_FALSE(doc["llm"]["models"]["cheap"].contains("profile"));
    EXPECT_FALSE(doc["llm"]["models"]["cat-md1"].contains("profile"));
}

TEST(Import52, KeylessEndpointNote) {
    std::vector<std::string> notes;
    const nlohmann::json     doc = import_document(nlohmann::json::parse(R"JSON({
      "default_profile": "p",
      "profiles": { "p": { "provider": "local", "model": "m" } },
      "providers": { "local": { "type": "openai-compatible",
                                "base_url": "http://127.0.0.1:8080/v1" } }
    })JSON"),
                                                  &notes);
    ASSERT_TRUE(doc.contains("llm"));
    EXPECT_TRUE(doc["llm"]["endpoints"].contains("local"));
    EXPECT_FALSE(doc["llm"]["endpoints"]["local"].contains("api_key"));
    EXPECT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("keyless"), std::string::npos) << notes[0];
}

TEST(Import52, MalformedEntrySkippedWithNote) {
    std::vector<std::string> notes;
    const nlohmann::json     doc = import_document(nlohmann::json::parse(R"JSON({
      "default_profile": "p",
      "profiles": { "p": { "provider": "prov", "model": "m" } },
      "providers": { "prov": { "type": "openai-compatible",
                               "base_url": "https://x.test/v1", "api_key": "SECRET" } },
      "mcp_servers": { "bad": { "command": "c", "env": { "PORT": 8080 } },
                       "good": { "command": "c" } }
    })JSON"),
                                                  &notes);
    ASSERT_TRUE(doc.contains("llm"));
    EXPECT_TRUE(doc["llm"]["endpoints"].contains("prov"));
    EXPECT_TRUE(doc["llm"]["models"].contains("p"));
    EXPECT_FALSE(doc["mcp_servers"].contains("bad"));
    EXPECT_TRUE(doc["mcp_servers"].contains("good"));
    EXPECT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("bad"), std::string::npos) << notes[0];
}

TEST(Import52, ReimportClobber) {
    test::TempWorkspace workspace("s52_reimport");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    const std::filesystem::path localcode =
        workspace.path() / "home" / ".localcode" / "config.json";
    write_file(localcode, R"JSON({"mcp_servers":{"s":{"command":"c"}}})JSON", 0644);

    const std::filesystem::path global = workspace.path() / "config" / "ymh" / "config.jsonc";
    write_file(global, "{\n  \"agent\": { \"max_steps\": 7 }\n}\n", 0600);
    const std::string before = read_file(global);

    CliInvocation invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("\n");
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_FALSE(
        maybe_import_localcode_config(invocation, global, true, in, out, err));
    EXPECT_EQ(read_file(global), before);
}

TEST(Import52, WriteAtomicity) {
    test::TempWorkspace workspace("s52_atomic");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    const std::filesystem::path localcode =
        workspace.path() / "home" / ".localcode" / "config.json";
    write_file(localcode,
               R"JSON({
                 "default_profile": "bad name",
                 "profiles": { "bad name": { "provider": "prov", "model": "m" } },
                 "providers": { "prov": { "type": "openai-compatible",
                                          "base_url": "https://x.test/v1",
                                          "api_key": "SECRET" } }
               })JSON",
               0644);

    const std::filesystem::path global = workspace.path() / "config" / "ymh" / "config.jsonc";
    CliInvocation               invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("\n");
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_FALSE(maybe_import_localcode_config(invocation, global, true, in, out, err));
    EXPECT_FALSE(std::filesystem::exists(global));
    EXPECT_FALSE(std::filesystem::exists(global.string() + ".import.tmp"));
}

TEST(Import52, ImportEndToEnd) {
    test::TempWorkspace workspace("s52_import_e2e");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    const std::filesystem::path localcode =
        workspace.path() / "home" / ".localcode" / "config.json";
    write_file(localcode, kRealLocalcodeShape, 0644);

    const std::filesystem::path global = workspace.path() / "config" / "ymh" / "config.jsonc";
    CliInvocation               invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("\n");
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_TRUE(maybe_import_localcode_config(invocation, global, true, in, out, err));
    ASSERT_TRUE(std::filesystem::exists(global));
    EXPECT_FALSE(std::filesystem::exists(global.string() + ".import.tmp"));

    struct stat info {};
    ASSERT_EQ(::stat(global.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 077, 0);

    Config validated;
    EXPECT_NO_THROW(apply_jsonc_file(validated, global, /*required=*/true));
    EXPECT_EQ(validated.llm.models.size(), 5u);
    ASSERT_TRUE(validated.llm.active_model.has_value());
    EXPECT_EQ(*validated.llm.active_model, "balanced");
}

} // namespace

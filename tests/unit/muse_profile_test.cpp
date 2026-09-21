#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/openai_adapter.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace {

using namespace ymh;

std::filesystem::path write_global(const test::TempWorkspace& workspace,
                                   std::string_view content = "{}\n") {
    workspace.write("global.jsonc", std::string{content});
    return workspace.path() / "global.jsonc";
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream      input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::vector<std::string> files_containing_muse(const std::filesystem::path& root,
                                               const std::filesystem::path& excluded) {
    std::vector<std::string> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path() == excluded) {
            continue;
        }
        if (lowercase(read_file(entry.path())).find("muse") != std::string::npos) {
            hits.push_back(entry.path().string());
        }
    }
    return hits;
}

LLMRequest text_request(std::string model = "golden-model") {
    LLMRequest request;
    request.model      = std::move(model);
    request.session_id = SessionId{"session"};
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "hello";
    message.content.push_back(std::move(block));
    request.messages.push_back(std::move(message));
    return request;
}

} // namespace

TEST(MuseProfile, LookupEmptyIsNull) {
    EXPECT_EQ(find_model_profile(""), nullptr);
    EXPECT_FALSE(is_known_model_profile(""));
}

TEST(MuseProfile, LookupUnknownIsNull) {
    EXPECT_EQ(find_model_profile("not-a-profile"), nullptr);
    EXPECT_FALSE(is_known_model_profile("not-a-profile"));
}

TEST(MuseProfile, KnownIdsResolve) {
    const ModelProfile* profile = find_model_profile("muse-glimmer");
    ASSERT_NE(profile, nullptr);
    EXPECT_TRUE(is_known_model_profile("muse-glimmer"));
    EXPECT_EQ(profile->id, "muse-glimmer");
    EXPECT_TRUE(profile->force_first_tool_call);
    EXPECT_TRUE(profile->normalize_tool_arguments);
    EXPECT_TRUE(profile->detect_leaked_tool_calls);
    EXPECT_TRUE(profile->detect_atem_tool_calls);
    EXPECT_EQ(profile->forbidden_stop_tokens.size(), 6u);
    ASSERT_TRUE(profile->capabilities.parallel_tool_calls.has_value());
    EXPECT_TRUE(*profile->capabilities.parallel_tool_calls);
    ASSERT_TRUE(profile->capabilities.reasoning.has_value());
    EXPECT_TRUE(*profile->capabilities.reasoning);
    ASSERT_TRUE(profile->temperature.has_value());
    EXPECT_DOUBLE_EQ(*profile->temperature, 1.0);
    ASSERT_TRUE(profile->top_p.has_value());
    EXPECT_DOUBLE_EQ(*profile->top_p, 0.95);
    ASSERT_TRUE(profile->top_k.has_value());
    EXPECT_EQ(*profile->top_k, 64u);
}

TEST(MuseProfile, ConfigUnknownIdIsError) {
    test::TempWorkspace workspace("muse_unknown_profile");
    workspace.write(".ymh/config.jsonc",
                    "{ \"llm\": { \"default\": { \"profile\": \"no-such-profile\" } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(MuseProfile, ConfigAbsentIsInert) {
    test::TempWorkspace workspace("muse_absent_profile");
    workspace.write(".ymh/config.jsonc",
                    "{ \"llm\": { \"default\": { \"model\": \"plain-model\" } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);

    EXPECT_TRUE(config.llm.profile.empty());
    EXPECT_EQ(find_model_profile(config.llm.profile), nullptr);

    const LLMProviderConfig provider = to_provider_config(config);
    EXPECT_TRUE(provider.profile.id.empty());

    ProviderCapabilities capabilities = openai_compatible_capabilities();
    apply_profile_capabilities(capabilities, provider.profile);
    const ProviderCapabilities fixed = openai_compatible_capabilities();
    EXPECT_EQ(capabilities.streaming, fixed.streaming);
    EXPECT_EQ(capabilities.tool_calls, fixed.tool_calls);
    EXPECT_EQ(capabilities.parallel_tool_calls, fixed.parallel_tool_calls);
    EXPECT_EQ(capabilities.reasoning, fixed.reasoning);
    EXPECT_EQ(capabilities.usage_streaming, fixed.usage_streaming);
    EXPECT_EQ(capabilities.prompt_caching, fixed.prompt_caching);

    const AgentConfig agent = to_agent_config(config);
    EXPECT_TRUE(agent.profile.id.empty());
    EXPECT_FALSE(agent.parameters.temperature.has_value());
    EXPECT_FALSE(agent.parameters.top_p.has_value());
    EXPECT_FALSE(agent.parameters.top_k.has_value());
}

TEST(MuseProfile, InertBodyGolden) {
    const LLMRequest request = text_request();
    LLMRequest configured   = request;
    configured.parameters.temperature       = 0.5;
    configured.parameters.max_output_tokens = 128;
    configured.parameters.reasoning_effort  = "high";

    const nlohmann::json body =
        build_chat_completions_body(configured, openai_compatible_capabilities());

    const std::string expected =
        R"({"max_tokens":128,"messages":[{"content":"hello","role":"user"}],"model":"golden-model",)"
        R"("reasoning_effort":"high","stream":true,"stream_options":{"include_usage":true},)"
        R"("temperature":0.5})";
    EXPECT_EQ(body.dump(), expected);
}

TEST(MuseProfile, InertCanonicalJsonGolden) {
    LlmCallConfig config;
    config.provider = "openai-compatible";
    config.model    = "golden-model";

    const FrozenRequest frozen = FrozenRequest::freeze(text_request(), config);
    const std::string   json   = frozen.canonical_json();

    EXPECT_EQ(json.find("\"profile\""), std::string::npos);
    EXPECT_EQ(json.find("\"top_k\""), std::string::npos);
    EXPECT_EQ(json.find("\"temperature\""), std::string::npos);
    EXPECT_EQ(json.find("\"top_p\""), std::string::npos);
    EXPECT_EQ(frozen.digest(), FrozenRequest::freeze(text_request(), config).digest());
}

TEST(MuseProfile, MuseNamesConfinedToProfileTable) {
    const std::filesystem::path source_root{YMH_SOURCE_DIR};
    const std::filesystem::path excluded = source_root / "src" / "llm" / "model_profile.cpp";

    const std::vector<std::string> include_hits =
        files_containing_muse(source_root / "include", excluded);
    const std::vector<std::string> src_hits = files_containing_muse(source_root / "src", excluded);

    EXPECT_TRUE(include_hits.empty())
        << "profile identity leaked into include/: " << include_hits.front();
    EXPECT_TRUE(src_hits.empty()) << "profile identity leaked into src/: " << src_hits.front();
}

TEST(MuseProfile, ScaffoldHasNoMuseName) {
    test::TempWorkspace         workspace("muse_scaffold_name");
    const std::filesystem::path global = workspace.path() / "config.jsonc";
    ASSERT_TRUE(scaffold_config(workspace.path(), global).ok);

    const std::string text = lowercase(read_file(global));
    EXPECT_EQ(text.find("muse"), std::string::npos);
    EXPECT_NE(text.find("\"profile\": \"\""), std::string::npos);
}

TEST(MuseProfile, SamplingDefaultsFillUnset) {
    test::TempWorkspace workspace("muse_sampling_defaults");
    workspace.write(".ymh/config.jsonc",
                    "{ \"llm\": { \"default\": { \"profile\": \"muse-glimmer\" } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);

    const AgentConfig agent = to_agent_config(config);
    ASSERT_EQ(agent.profile.id, "muse-glimmer");
    ASSERT_TRUE(agent.parameters.temperature.has_value());
    EXPECT_DOUBLE_EQ(*agent.parameters.temperature, 1.0);
    ASSERT_TRUE(agent.parameters.top_p.has_value());
    EXPECT_DOUBLE_EQ(*agent.parameters.top_p, 0.95);
    ASSERT_TRUE(agent.parameters.top_k.has_value());
    EXPECT_EQ(*agent.parameters.top_k, 64u);
}

TEST(MuseProfile, ExplicitBeatsProfileDefault) {
    test::TempWorkspace workspace("muse_sampling_explicit");
    workspace.write(".ymh/config.jsonc",
                    "{ \"llm\": { \"default\": { \"profile\": \"muse-glimmer\", "
                    "\"temperature\": 0.2, \"top_p\": 0.5, \"top_k\": 7 } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    const AgentConfig agent = to_agent_config(load_config(paths));

    ASSERT_TRUE(agent.parameters.temperature.has_value());
    EXPECT_DOUBLE_EQ(*agent.parameters.temperature, 0.2);
    ASSERT_TRUE(agent.parameters.top_p.has_value());
    EXPECT_DOUBLE_EQ(*agent.parameters.top_p, 0.5);
    ASSERT_TRUE(agent.parameters.top_k.has_value());
    EXPECT_EQ(*agent.parameters.top_k, 7u);
}

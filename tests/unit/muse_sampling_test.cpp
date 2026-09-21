#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/llm_runtime.hpp"
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

Config load_workspace(const test::TempWorkspace& workspace, std::string_view content) {
    workspace.write(".ymh/config.jsonc", std::string{content});
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    return load_config(paths);
}

LLMRequest text_request(std::string model) {
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

SinkFlow continue_sink(const StreamEvent&) { return SinkFlow::Continue; }

FakeScript one_text_step() {
    FakeScript script;
    FakeResponseStep step;
    step.text   = "hi";
    step.finish = FinishReason::Stop;
    script.steps.push_back(std::move(step));
    return script;
}

LlmCallConfig call_config(std::string provider, std::string model) {
    LlmCallConfig config;
    config.provider = std::move(provider);
    config.model    = std::move(model);
    return config;
}

} // namespace

TEST(MuseD7, AllSamplingKeysAccepted) {
    test::TempWorkspace workspace("muse_d7_all_keys");
    const Config config = load_workspace(
        workspace,
        "{ \"llm\": { \"default\": { \"profile\": \"muse-glimmer\", \"temperature\": 0.7, "
        "\"top_p\": 0.8, \"top_k\": 40, \"tool_choice\": \"auto\", \"stop\": [\"END\"], "
        "\"seed\": 123 } } }\n");

    EXPECT_EQ(config.llm.profile, "muse-glimmer");
    ASSERT_TRUE(config.llm.temperature.has_value());
    EXPECT_DOUBLE_EQ(*config.llm.temperature, 0.7);
    ASSERT_TRUE(config.llm.top_p.has_value());
    EXPECT_DOUBLE_EQ(*config.llm.top_p, 0.8);
    ASSERT_TRUE(config.llm.top_k.has_value());
    EXPECT_EQ(*config.llm.top_k, 40u);
    ASSERT_TRUE(config.llm.tool_choice.has_value());
    EXPECT_EQ(*config.llm.tool_choice, "auto");
    EXPECT_EQ(config.llm.stop, std::vector<std::string>{"END"});
    ASSERT_TRUE(config.llm.seed.has_value());
    EXPECT_EQ(*config.llm.seed, 123u);
}

TEST(MuseD7, UnknownSamplingKeyIsError) {
    test::TempWorkspace workspace("muse_d7_unknown_key");
    workspace.write(".ymh/config.jsonc", "{ \"llm\": { \"default\": { \"topq\": 1 } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(MuseD7, TemperatureRangeIsError) {
    test::TempWorkspace workspace("muse_d7_temperature_range");
    workspace.write(".ymh/config.jsonc",
                    "{ \"llm\": { \"default\": { \"temperature\": 3.0 } } }\n");

    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(MuseD7, NestedShadowsFlat) {
    test::TempWorkspace nested("muse_d7_nested_shadows");
    const Config shadowed =
        load_workspace(nested, "{ \"llm\": { \"temperature\": 0.1, "
                               "\"default\": { \"temperature\": 0.9 } } }\n");
    ASSERT_TRUE(shadowed.llm.temperature.has_value());
    EXPECT_DOUBLE_EQ(*shadowed.llm.temperature, 0.9);

    test::TempWorkspace flat("muse_d7_flat_only");
    const Config flat_only = load_workspace(flat, "{ \"llm\": { \"temperature\": 0.3 } }\n");
    ASSERT_TRUE(flat_only.llm.temperature.has_value());
    EXPECT_DOUBLE_EQ(*flat_only.llm.temperature, 0.3);
}

TEST(MuseD7, KeysMapToParameters) {
    test::TempWorkspace workspace("muse_d7_map");
    const Config config = load_workspace(
        workspace,
        "{ \"llm\": { \"default\": { \"temperature\": 0.7, \"top_p\": 0.8, \"top_k\": 40, "
        "\"tool_choice\": \"none\", \"stop\": [\"END\"], \"seed\": 99 } } }\n");

    const AgentConfig agent = to_agent_config(config);
    ASSERT_TRUE(agent.parameters.temperature.has_value());
    EXPECT_DOUBLE_EQ(*agent.parameters.temperature, 0.7);
    ASSERT_TRUE(agent.parameters.top_p.has_value());
    EXPECT_DOUBLE_EQ(*agent.parameters.top_p, 0.8);
    ASSERT_TRUE(agent.parameters.top_k.has_value());
    EXPECT_EQ(*agent.parameters.top_k, 40u);
    ASSERT_TRUE(agent.parameters.tool_choice.has_value());
    EXPECT_EQ(*agent.parameters.tool_choice, "none");
    EXPECT_EQ(agent.parameters.stop, std::vector<std::string>{"END"});
    ASSERT_TRUE(agent.parameters.seed.has_value());
    EXPECT_EQ(*agent.parameters.seed, 99u);
}

TEST(MuseD7, StopSurvivesLayerWithoutKey) {
    test::TempWorkspace workspace("muse_d7_stop_layers");
    workspace.write(".ymh/config.jsonc", "{ \"llm\": { \"default\": { \"model\": \"x\" } } }\n");

    ConfigPaths paths;
    paths.global =
        write_global(workspace, "{ \"llm\": { \"default\": { \"stop\": [\"GLOBAL\"] } } }\n");
    paths.workspace = workspace_config_path(workspace.path());
    const Config config = load_config(paths);

    EXPECT_EQ(config.llm.stop, std::vector<std::string>{"GLOBAL"});
}

TEST(MuseD7, ScaffoldMentionsNewKeys) {
    test::TempWorkspace         workspace("muse_d7_scaffold");
    const std::filesystem::path global = workspace.path() / "config.jsonc";
    ASSERT_TRUE(scaffold_config(workspace.path(), global).ok);

    const std::string text = read_file(global);
    for (const std::string key : {"\"profile\"", "\"temperature\"", "\"top_p\"", "\"top_k\"",
                                  "\"tool_choice\"", "\"stop\"", "\"seed\"", "\"xhigh\""}) {
        EXPECT_NE(text.find(key), std::string::npos) << "scaffold is missing " << key;
    }
}

TEST(MuseD8, TopKReachesBody) {
    test::TempWorkspace workspace("muse_d8_body");
    const Config config =
        load_workspace(workspace, "{ \"llm\": { \"default\": { \"model\": \"m\", "
                                  "\"top_k\": 40 } } }\n");

    const AgentConfig agent = to_agent_config(config);
    ASSERT_TRUE(agent.parameters.top_k.has_value());
    EXPECT_EQ(*agent.parameters.top_k, 40u);

    LLMRequest request = text_request(agent.model);
    request.parameters = agent.parameters;
    const nlohmann::json body =
        build_chat_completions_body(request, openai_compatible_capabilities());
    EXPECT_EQ(body["top_k"], 40);
}

TEST(MuseD8, TopKZeroIsOmitted) {
    test::TempWorkspace workspace("muse_d8_zero");
    const Config config =
        load_workspace(workspace, "{ \"llm\": { \"default\": { \"model\": \"m\", "
                                  "\"top_k\": 0 } } }\n");

    EXPECT_FALSE(config.llm.top_k.has_value());
    const AgentConfig agent = to_agent_config(config);
    EXPECT_FALSE(agent.parameters.top_k.has_value());

    LLMRequest request = text_request(agent.model);
    request.parameters = agent.parameters;
    const nlohmann::json body =
        build_chat_completions_body(request, openai_compatible_capabilities());
    EXPECT_FALSE(body.contains("top_k"));
}

TEST(MuseD8, TopKMismatchIsConfigMismatch) {
    LlmRuntime runtime;
    auto       handle =
        runtime.register_adapter({"fake"}, std::make_shared<FakeLLM>(one_text_step()));
    (void)handle;

    PreparedCall call =
        runtime.prepare_call(call_config("fake", "fake-model"), CancellationToken{}).get();

    LlmCallConfig mismatched = call_config("fake", "fake-model");
    mismatched.top_k         = 7;
    LLMRequest request;
    request.session_id = SessionId{"session"};
    const FrozenRequest frozen = FrozenRequest::freeze(std::move(request), mismatched);

    try {
        (void)call.stream(frozen, continue_sink, CancellationToken{});
        FAIL() << "expected PreparedCallError";
    } catch (const PreparedCallError& error) {
        EXPECT_EQ(error.reason, PreparedCallError::Reason::ConfigMismatch);
    }
}

TEST(MuseD8, TopKInCanonicalJson) {
    LlmCallConfig config = call_config("openai-compatible", "m");
    config.top_k         = 9;
    LLMRequest request;
    request.model      = "m";
    request.session_id = SessionId{"session"};

    const FrozenRequest frozen = FrozenRequest::freeze(std::move(request), config);
    EXPECT_NE(frozen.canonical_json().find("\"top_k\":9"), std::string::npos);
}

TEST(MuseD8, OldConfigDecodesWithoutTopK) {
    const nlohmann::json legacy = {{"provider", "p"}, {"model", "m"}};
    const LlmCallConfig  decoded = legacy.get<LlmCallConfig>();
    EXPECT_FALSE(decoded.top_k.has_value());

    const nlohmann::json encoded = decoded;
    EXPECT_FALSE(encoded.contains("top_k"));

    LlmCallConfig with_top_k;
    with_top_k.top_k = 12;
    const nlohmann::json round = with_top_k;
    ASSERT_TRUE(round.contains("top_k"));
    EXPECT_EQ(round["top_k"], 12);
    const LlmCallConfig back = round.get<LlmCallConfig>();
    ASSERT_TRUE(back.top_k.has_value());
    EXPECT_EQ(*back.top_k, 12u);
}

TEST(MuseD9, OverlayDeclaredFlags) {
    ProviderCapabilities base;
    ModelProfile         profile;
    profile.capabilities.streaming           = true;
    profile.capabilities.tool_calls          = true;
    profile.capabilities.parallel_tool_calls = true;
    profile.capabilities.reasoning           = true;
    profile.capabilities.usage_streaming     = true;
    profile.capabilities.prompt_caching      = true;

    apply_profile_capabilities(base, profile);
    EXPECT_TRUE(base.streaming);
    EXPECT_TRUE(base.tool_calls);
    EXPECT_TRUE(base.parallel_tool_calls);
    EXPECT_TRUE(base.reasoning);
    EXPECT_TRUE(base.usage_streaming);
    EXPECT_TRUE(base.prompt_caching);
}

TEST(MuseD9, OverlayKeepsBaseForNullopt) {
    ProviderCapabilities base;
    base.tool_calls  = true;
    base.reasoning   = true;
    base.streaming   = false;

    ModelProfile profile;
    apply_profile_capabilities(base, profile);

    EXPECT_TRUE(base.tool_calls);
    EXPECT_TRUE(base.reasoning);
    EXPECT_FALSE(base.streaming);
}

TEST(MuseD9, OverlayOverridesSyntheticBase) {
    ProviderCapabilities base;
    base.parallel_tool_calls = false;
    base.reasoning           = false;

    const ModelProfile* profile = find_model_profile("muse-glimmer");
    ASSERT_NE(profile, nullptr);
    apply_profile_capabilities(base, *profile);

    EXPECT_TRUE(base.parallel_tool_calls);
    EXPECT_TRUE(base.reasoning);
}

TEST(MuseD9, NoProfileUsesFixedCapabilities) {
    ProviderCapabilities base = openai_compatible_capabilities();
    const ModelProfile   inert;

    apply_profile_capabilities(base, inert);

    const ProviderCapabilities fixed = openai_compatible_capabilities();
    EXPECT_EQ(base.streaming, fixed.streaming);
    EXPECT_EQ(base.tool_calls, fixed.tool_calls);
    EXPECT_EQ(base.parallel_tool_calls, fixed.parallel_tool_calls);
    EXPECT_EQ(base.reasoning, fixed.reasoning);
    EXPECT_EQ(base.usage_streaming, fixed.usage_streaming);
    EXPECT_EQ(base.prompt_caching, fixed.prompt_caching);
}

TEST(MuseD10, XhighReachesBodyUnclamped) {
    test::TempWorkspace workspace("muse_d10_xhigh");
    const Config config =
        load_workspace(workspace, "{ \"llm\": { \"default\": { \"model\": \"m\", "
                                  "\"reasoning_effort\": \"xhigh\" } } }\n");

    const AgentConfig agent = to_agent_config(config);
    ASSERT_TRUE(agent.parameters.reasoning_effort.has_value());
    EXPECT_EQ(*agent.parameters.reasoning_effort, "xhigh");

    LLMRequest request = text_request(agent.model);
    request.parameters = agent.parameters;
    const nlohmann::json body =
        build_chat_completions_body(request, openai_compatible_capabilities());
    EXPECT_EQ(body["reasoning_effort"], "xhigh");
}

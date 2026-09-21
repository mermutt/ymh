#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/openai_adapter.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace {

using namespace ymh;

Config load_layers(const test::TempWorkspace& workspace, std::string_view global,
                   std::string_view local) {
    workspace.write("global.jsonc", std::string{global});
    workspace.write(".ymh/config.jsonc", std::string{local});
    ConfigPaths paths;
    paths.global    = workspace.path() / "global.jsonc";
    paths.workspace = workspace_config_path(workspace.path());
    return load_config(paths);
}

class FakeTransport final : public HttpTransport {
public:
    explicit FakeTransport(std::string sse) : sse_(std::move(sse)) {}

    Task<HttpResponse> postStream(const HttpRequest& request, HttpBodySink on_body,
                                  CancellationToken) override {
        requests.push_back(request);
        for (std::size_t offset = 0; offset < sse_.size(); offset += 4096) {
            const std::size_t length = std::min<std::size_t>(4096, sse_.size() - offset);
            if (!on_body(sse_.data() + offset, length)) {
                break;
            }
        }
        HttpResponse response;
        response.status = 200;
        return Task<HttpResponse>{std::move(response)};
    }

    std::vector<HttpRequest> requests;

private:
    std::string sse_;
};

std::string text_sse() {
    nlohmann::json choice;
    choice["index"]         = 0;
    choice["delta"]         = nlohmann::json{{"content", "hi"}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return "data: " + chunk.dump() + "\n\n" "data: [DONE]\n\n";
}

LLMProviderConfig provider_config_with_profile() {
    LLMProviderConfig config;
    config.provider    = "openai-compatible";
    config.base_url    = "http://127.0.0.1:9/v1";
    config.model       = "test-model";
    config.api_key     = std::string{"test-key"};
    config.api_key_env = "YMH_TEST_API_KEY";
    const ModelProfile* profile = find_model_profile("muse-glimmer");
    if (profile != nullptr) {
        config.profile = *profile;
    }
    return config;
}

LLMRequest stop_request(std::vector<std::string> stop) {
    LLMRequest request;
    request.model = "test-model";
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "hi";
    message.content.push_back(std::move(block));
    request.messages.push_back(std::move(message));
    request.parameters.stop = std::move(stop);
    return request;
}

LLMResponse run_stream(LLMProvider& provider, const LLMRequest& request) {
    return provider.stream(request, [](const StreamEvent&) { return SinkFlow::Continue; }, {}).get();
}

} // namespace

TEST(MuseD3, ConfigEomStopIsError) {
    test::TempWorkspace workspace("muse_d3_config_eom");
    EXPECT_THROW((void)load_layers(workspace,
                                   "{ \"llm\": { \"default\": { \"profile\": \"muse-glimmer\" } } }\n",
                                   "{ \"llm\": { \"default\": { \"stop\": [\"<|eom|>\"] } } }\n"),
                 ConfigError);
}

TEST(MuseD3, ConfigErrorNamesOffendingToken) {
    test::TempWorkspace workspace("muse_d3_config_token_name");
    try {
        (void)load_layers(workspace,
                          "{ \"llm\": { \"default\": { \"profile\": \"muse-glimmer\" } } }\n",
                          "{ \"llm\": { \"default\": { \"stop\": [\"<|eot|>\"] } } }\n");
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_NE(std::string{error.what()}.find("<|eot|>"), std::string::npos);
    }
}

TEST(MuseD3, CrossLayerForbiddenStopIsCaught) {
    test::TempWorkspace workspace("muse_d3_cross_layer");
    EXPECT_THROW((void)load_layers(workspace,
                                   "{ \"llm\": { \"default\": { \"profile\": \"muse-glimmer\" } } }\n",
                                   "{ \"llm\": { \"default\": { \"stop\": [\"<|start|>\"] } } }\n"),
                 ConfigError);
}

TEST(MuseD3, AdapterRejectsMergedForbiddenStop) {
    auto transport = std::make_shared<FakeTransport>(text_sse());
    OpenAICompatibleProvider provider(provider_config_with_profile(),
                                      openai_compatible_capabilities(), transport);

    const LLMResponse response = run_stream(provider, stop_request({"<|eom|>"}));
    EXPECT_EQ(response.outcome, StreamOutcome::Failed);
    EXPECT_EQ(response.error.code, LLMErrorCode::BadRequest);
    EXPECT_TRUE(transport->requests.empty());
}

TEST(MuseD3, AllowedStopPasses) {
    auto transport = std::make_shared<FakeTransport>(text_sse());
    OpenAICompatibleProvider provider(provider_config_with_profile(),
                                      openai_compatible_capabilities(), transport);

    const LLMResponse response = run_stream(provider, stop_request({"END"}));
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(transport->requests.size(), 1u);
    EXPECT_NE(transport->requests[0].body.find("END"), std::string::npos);
}

TEST(MuseD3, NoProfileStopUnchanged) {
    test::TempWorkspace workspace("muse_d3_no_profile");
    const Config config = load_layers(workspace, "{}\n",
                                      "{ \"llm\": { \"default\": { \"stop\": [\"<|eom|>\"] } } }\n");
    ASSERT_EQ(config.llm.stop, std::vector<std::string>{"<|eom|>"});

    auto transport = std::make_shared<FakeTransport>(text_sse());
    LLMProviderConfig provider_config;
    provider_config.provider    = "openai-compatible";
    provider_config.base_url    = "http://127.0.0.1:9/v1";
    provider_config.model       = "test-model";
    provider_config.api_key     = std::string{"test-key"};
    provider_config.api_key_env = "YMH_TEST_API_KEY";
    OpenAICompatibleProvider provider(provider_config, openai_compatible_capabilities(), transport);

    const LLMResponse response = run_stream(provider, stop_request({"<|eom|>"}));
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(transport->requests.size(), 1u);
    EXPECT_NE(transport->requests[0].body.find("<|eom|>"), std::string::npos);
}

TEST(MuseD3, DenylistIsTheSixControlTokens) {
    const ModelProfile* profile = find_model_profile("muse-glimmer");
    ASSERT_NE(profile, nullptr);
    const std::vector<std::string> expected{"<|begin_of_text|>", "<|end_of_text|>", "<|start|>",
                                            "<|message|>", "<|eom|>", "<|eot|>"};
    EXPECT_EQ(profile->forbidden_stop_tokens, expected);
    EXPECT_NE(std::find(profile->forbidden_stop_tokens.begin(),
                        profile->forbidden_stop_tokens.end(), "<|eom|>"),
              profile->forbidden_stop_tokens.end());
    EXPECT_NE(std::find(profile->forbidden_stop_tokens.begin(),
                        profile->forbidden_stop_tokens.end(), "<|eot|>"),
              profile->forbidden_stop_tokens.end());
}

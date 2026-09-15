#include <gtest/gtest.h>

#include <memory>

#include "ymh/llm/provider_registry.hpp"

namespace {

class StubProvider final : public ymh::LLMProvider {
public:
    [[nodiscard]] ymh::ProviderId id() const override { return "stub"; }
    [[nodiscard]] ymh::ProviderCapabilities capabilities() const override { return {}; }
    ymh::Task<ymh::LLMResponse> stream(const ymh::LLMRequest&,
                                       ymh::StreamSink,
                                       ymh::CancellationToken) override {
        return ymh::Task<ymh::LLMResponse>{ymh::LLMResponse{}};
    }
};

} // namespace

TEST(ProviderRegistryTest, DefaultRegistryRegistersOpenAiCompatible) {
    const ymh::ProviderRegistry registry = ymh::make_default_provider_registry();
    const auto names = registry.names();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "openai-compatible");
}

TEST(ProviderRegistryTest, CreateBuildsProviderWithoutNetworkIo) {
    const ymh::ProviderRegistry registry = ymh::make_default_provider_registry();
    const auto created = registry.create(ymh::deepseek_config());
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ((*created)->id(), "openai-compatible");
}

TEST(ProviderRegistryTest, UnknownProviderIsConfigError) {
    const ymh::ProviderRegistry registry = ymh::make_default_provider_registry();
    ymh::LLMProviderConfig config = ymh::deepseek_config();
    config.provider = "does-not-exist";

    const auto created = registry.create(config);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code, ymh::LLMErrorCode::ConfigError);
}

TEST(ProviderRegistryTest, BadBaseUrlIsConfigError) {
    const ymh::ProviderRegistry registry = ymh::make_default_provider_registry();
    ymh::LLMProviderConfig config = ymh::deepseek_config();
    config.base_url = "ftp://example.invalid";

    const auto created = registry.create(config);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code, ymh::LLMErrorCode::ConfigError);
}

TEST(ProviderRegistryTest, BadApiKeyEnvNameIsConfigError) {
    const ymh::ProviderRegistry registry = ymh::make_default_provider_registry();
    ymh::LLMProviderConfig config = ymh::deepseek_config();
    config.api_key_env = "1-bad-name";

    const auto created = registry.create(config);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code, ymh::LLMErrorCode::ConfigError);
}

TEST(ProviderRegistryTest, CustomProviderCanBeRegistered) {
    ymh::ProviderRegistry registry;
    registry.registerProvider("stub", [](const ymh::LLMProviderConfig&)
                                          -> std::expected<std::unique_ptr<ymh::LLMProvider>,
                                                           ymh::LLMError> {
        return std::make_unique<StubProvider>();
    });

    ymh::LLMProviderConfig config;
    config.provider = "stub";
    config.base_url = "http://localhost:1/v1";
    config.api_key_env = "STUB_KEY";

    const auto created = registry.create(config);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ((*created)->id(), "stub");
}

TEST(ProviderRegistryTest, ResolveModelFirstNonEmptyWins) {
    ymh::ModelResolutionSources sources;
    sources.global_model = "global";
    sources.session_model = "session";
    EXPECT_EQ(ymh::resolve_model(sources), std::optional<ymh::ModelId>{"session"});

    sources.request_override = "override";
    EXPECT_EQ(ymh::resolve_model(sources), std::optional<ymh::ModelId>{"override"});

    sources.request_override = "";
    EXPECT_EQ(ymh::resolve_model(sources), std::optional<ymh::ModelId>{"session"});
}

TEST(ProviderRegistryTest, ResolveModelEmptyIsNullopt) {
    ymh::ModelResolutionSources sources;
    sources.session_model = "";
    EXPECT_FALSE(ymh::resolve_model(sources).has_value());
}

TEST(ProviderRegistryTest, DeepseekDefaultsMatchLiveFacts) {
    const ymh::LLMProviderConfig config = ymh::deepseek_config();
    EXPECT_EQ(config.provider, "openai-compatible");
    EXPECT_EQ(config.base_url, "https://api.deepseek.com/v1");
    EXPECT_EQ(config.model, "deepseek-flash");
    EXPECT_EQ(config.api_key_env, "DEEPSEEK_API_KEY");
}

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "support/short_temp.hpp"
#include "ymh/cli/provider_factory.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/fake_llm.hpp"

namespace {

class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::optional<std::string>& value)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) {
            previous_ = std::string{existing};
        }
        if (value.has_value()) {
            ::setenv(name_.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
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

ymh::FakeScript script_from(const std::string& text) {
    return ymh::parse_fake_llm_script(nlohmann::json::parse(text)).value();
}

bool load_throws_config_error(const std::filesystem::path& path) {
    try {
        const ymh::FakeScript script = ymh::load_fake_llm_script(path);
        EXPECT_TRUE(script.steps.empty());
    } catch (const ymh::ConfigError&) {
        return true;
    }
    return false;
}

} // namespace

TEST(ProviderFactory, ParsesArrayDocument) {
    const ymh::FakeScript script = script_from(R"([{"text": "hello", "finish": "stop"}])");
    ASSERT_EQ(script.steps.size(), 1u);
    EXPECT_EQ(script.steps[0].text, "hello");
    EXPECT_EQ(script.steps[0].finish, ymh::FinishReason::Stop);
}

TEST(ProviderFactory, ParsesObjectDocumentWithChunkSize) {
    const ymh::FakeScript script =
        script_from(R"({"chunk_size": 9, "steps": [{"text": "a"}, {"text": "b"}]})");
    EXPECT_EQ(script.chunk_size, 9u);
    ASSERT_EQ(script.steps.size(), 2u);
    EXPECT_EQ(script.steps[1].text, "b");
}

TEST(ProviderFactory, ParsesToolCallsAndForcesFinish) {
    const ymh::FakeScript script = script_from(
        R"([{"text": "", "tool_calls": [{"name": "read", "arguments": {"path": "a"}}]}])");
    ASSERT_EQ(script.steps.size(), 1u);
    ASSERT_EQ(script.steps[0].tool_calls.size(), 1u);
    EXPECT_EQ(script.steps[0].tool_calls[0].name, "read");
    EXPECT_EQ(script.steps[0].finish, ymh::FinishReason::ToolCalls);
}

TEST(ProviderFactory, RejectsNonScriptDocument) {
    EXPECT_FALSE(ymh::parse_fake_llm_script(nlohmann::json::object()).has_value());
    EXPECT_FALSE(ymh::parse_fake_llm_script(nlohmann::json(42)).has_value());
    EXPECT_FALSE(ymh::parse_fake_llm_script(nlohmann::json::parse("[7]")).has_value());
}

TEST(ProviderFactory, LoadsScriptFromFile) {
    ymh::test::ShortTempRoot root("ymh-provider");
    const std::filesystem::path path = root.path() / "script.json";
    root.write("script.json", R"([{"text": "from-file"}])");
    const ymh::FakeScript script = ymh::load_fake_llm_script(path);
    ASSERT_EQ(script.steps.size(), 1u);
    EXPECT_EQ(script.steps[0].text, "from-file");
}

TEST(ProviderFactory, MissingFileThrowsConfigError) {
    ymh::test::ShortTempRoot root("ymh-provider-missing");
    EXPECT_TRUE(load_throws_config_error(root.path() / "absent.json"));
}

TEST(ProviderFactory, InvalidFileThrowsConfigError) {
    ymh::test::ShortTempRoot root("ymh-provider-invalid");
    root.write("bad.json", "not json");
    EXPECT_TRUE(load_throws_config_error(root.path() / "bad.json"));
}

TEST(ProviderFactory, EnvUnsetReturnsNullopt) {
    ScopedEnv guard{"YMH_FAKE_LLM_SCRIPT", std::nullopt};
    EXPECT_FALSE(ymh::fake_llm_script_from_env().has_value());
}

TEST(ProviderFactory, EnvSetLoadsScript) {
    ymh::test::ShortTempRoot root("ymh-provider-env");
    root.write("script.json", R"([{"text": "env"}])");
    ScopedEnv guard{"YMH_FAKE_LLM_SCRIPT", (root.path() / "script.json").string()};
    const std::optional<ymh::FakeScript> script = ymh::fake_llm_script_from_env();
    ASSERT_TRUE(script.has_value());
    ASSERT_EQ(script->steps.size(), 1u);
    EXPECT_EQ(script->steps[0].text, "env");
}

TEST(ProviderFactory, FactoryFallsBackToNullWithoutEnv) {
    ScopedEnv guard{"YMH_FAKE_LLM_SCRIPT", std::nullopt};
    const auto factory = ymh::make_provider_factory();
    EXPECT_EQ(factory(ymh::LLMProviderConfig{}), nullptr);
}

TEST(ProviderFactory, FactoryUsesEnvScript) {
    ymh::test::ShortTempRoot root("ymh-provider-factory");
    root.write("script.json", R"([{"text": "env"}])");
    ScopedEnv guard{"YMH_FAKE_LLM_SCRIPT", (root.path() / "script.json").string()};
    const auto factory = ymh::make_provider_factory();
    EXPECT_NE(factory(ymh::LLMProviderConfig{}), nullptr);
}

TEST(ProviderFactory, ExplicitFactoryWins) {
    ymh::test::ShortTempRoot root("ymh-provider-explicit");
    root.write("script.json", R"([{"text": "env"}])");
    ScopedEnv guard{"YMH_FAKE_LLM_SCRIPT", (root.path() / "script.json").string()};
    const auto factory = ymh::make_provider_factory(
        [](const ymh::LLMProviderConfig&) -> std::unique_ptr<ymh::LLMProvider> { return nullptr; });
    EXPECT_EQ(factory(ymh::LLMProviderConfig{}), nullptr);
}

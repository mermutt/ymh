#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "ymh/cli/wiring.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/prompt/persona.hpp"
#include "ymh/prompt/system_prompt.hpp"

namespace {

using namespace ymh;

constexpr const char* kModel = "deepseek-flash";
constexpr const char* kCwd   = "/workspace/example";

DefaultPromptConfig base_config() {
    DefaultPromptConfig config;
    config.identity = default_system_prompt();
    config.persona  = default_persona_config();
    config.model    = kModel;
    config.cwd      = kCwd;
    return config;
}

std::string render_fixture(const DefaultPromptConfig& config) {
    SystemPrompt         prompt;
    DefaultPromptHandles handles = register_default_prompt(prompt, config);
    (void)handles;
    return prompt.render(AssembleContext{});
}

std::filesystem::path golden_path(const std::string& fixture) {
    return std::filesystem::path{YMH_SOURCE_DIR} / "tests" / "fixtures" / "prompt" /
           (fixture + ".sha256");
}

void check_golden(const std::string& fixture, const std::string& rendered) {
    const std::string           hash = sha256_hex(rendered);
    const std::filesystem::path path = golden_path(fixture);
    if (std::getenv("YMH_UPDATE_PROMPT_GOLDEN") != nullptr) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::trunc);
        ASSERT_TRUE(out.good()) << "cannot write " << path;
        out << hash << "\n";
        return;
    }
    std::ifstream in(path);
    ASSERT_TRUE(in.good()) << "missing golden file " << path
                           << " (regenerate with YMH_UPDATE_PROMPT_GOLDEN=1)";
    std::string expected;
    std::getline(in, expected);
    EXPECT_EQ(hash, expected) << "rendered prompt:\n" << rendered;
}

TEST(PromptGolden, Default) {
    check_golden("prompt_default", render_fixture(base_config()));
}

TEST(PromptGolden, ConfigOverride) {
    DefaultPromptConfig config = base_config();
    config.identity            = "You are a custom agent.";
    check_golden("prompt_config_override", render_fixture(config));
}

TEST(PromptGolden, PersonaSuffix) {
    DefaultPromptConfig config = base_config();
    config.persona.suffix      = "The workspace root is {{cwd}}.";
    check_golden("prompt_persona_suffix", render_fixture(config));
}

TEST(PromptGolden, Minimal) {
    DefaultPromptConfig config = base_config();
    config.persona.complete    = true;
    check_golden("prompt_minimal", render_fixture(config));
}

} // namespace

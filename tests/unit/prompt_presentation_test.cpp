#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/tools/presentation.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

std::filesystem::path write_global(const TempWorkspace& workspace) {
    workspace.write("global.jsonc", "{}\n");
    return workspace.path() / "global.jsonc";
}

Config load_workspace(const TempWorkspace& workspace) {
    ConfigPaths paths;
    paths.global    = write_global(workspace);
    paths.workspace = workspace_config_path(workspace.path());
    return load_config(paths);
}

TEST(PromptPresentation, NamesAreStable) {
    EXPECT_EQ(tool_presentation_name(ToolPresentationMode::Native), "native");
    EXPECT_EQ(tool_presentation_name(ToolPresentationMode::Ptc), "ptc");
    EXPECT_EQ(tool_presentation_name(ToolPresentationMode::Both), "both");
}

TEST(PromptPresentation, DefaultIsNative) {
    TempWorkspace workspace("prompt_present_default");
    const Config  config = load_workspace(workspace);
    EXPECT_EQ(config.tools.presentation, ToolPresentationMode::Native);
    EXPECT_TRUE(config.tools.tool_order.empty());
}

TEST(PromptPresentation, NativeAndToolOrderParse) {
    TempWorkspace workspace("prompt_present_native");
    workspace.write(".ymh/config.jsonc",
                    "{\n  \"tools\": {\n    \"presentation\": \"native\",\n"
                    "    \"tool_order\": [\"read_file\", \"<unlisted-tools>\"]\n  }\n}\n");

    const Config config = load_workspace(workspace);
    EXPECT_EQ(config.tools.presentation, ToolPresentationMode::Native);
    ASSERT_EQ(config.tools.tool_order.size(), 2u);
    EXPECT_EQ(config.tools.tool_order[0], "read_file");
    EXPECT_EQ(config.tools.tool_order[1], "<unlisted-tools>");
}

TEST(PromptPresentation, ReservedModesFailLoud) {
    TempWorkspace ptc("prompt_present_ptc");
    ptc.write(".ymh/config.jsonc", "{ \"tools\": { \"presentation\": \"ptc\" } }\n");
    EXPECT_THROW((void)load_workspace(ptc), ConfigError);

    TempWorkspace both("prompt_present_both");
    both.write(".ymh/config.jsonc", "{ \"tools\": { \"presentation\": \"both\" } }\n");
    EXPECT_THROW((void)load_workspace(both), ConfigError);
}

TEST(PromptPresentation, UnknownModeFailsLoud) {
    TempWorkspace workspace("prompt_present_unknown");
    workspace.write(".ymh/config.jsonc", "{ \"tools\": { \"presentation\": \"xml\" } }\n");
    EXPECT_THROW((void)load_workspace(workspace), ConfigError);
}

TEST(PromptPresentation, ToolOrderRequiresExactlyOneRest) {
    TempWorkspace missing("prompt_present_no_rest");
    missing.write(".ymh/config.jsonc", "{ \"tools\": { \"tool_order\": [\"read_file\"] } }\n");
    EXPECT_THROW((void)load_workspace(missing), ConfigError);

    TempWorkspace duplicated("prompt_present_two_rest");
    duplicated.write(
        ".ymh/config.jsonc",
        "{ \"tools\": { \"tool_order\": [\"<unlisted-tools>\", \"<unlisted-tools>\"] } }\n");
    EXPECT_THROW((void)load_workspace(duplicated), ConfigError);
}

TEST(PromptPresentation, ConfigToolOrderDrivesAssembleOrder) {
    TempWorkspace workspace("prompt_present_order");
    workspace.write(".ymh/config.jsonc",
                    "{ \"tools\": { \"tool_order\": [\"write_file\", \"<unlisted-tools>\"] } }\n");
    const Config config = load_workspace(workspace);

    SystemPrompt prompt(config.tools.tool_order);
    prompt.set_tool_provider([](const AssembleContext&) {
        std::vector<ToolSchema> tools;
        tools.push_back(ToolSchema{ToolName{"read_file"}, ToolVersion{}, "", nlohmann::json::object(),
                                   false});
        tools.push_back(ToolSchema{ToolName{"write_file"}, ToolVersion{}, "",
                                   nlohmann::json::object(), false});
        return tools;
    });

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.tools.size(), 2u);
    EXPECT_EQ(assembly.tools[0].name.value, "write_file");
    EXPECT_EQ(assembly.tools[1].name.value, "read_file");
}

TEST(PromptPresentation, InstructionsExplicitZeroMaxBytesRejected) {
    TempWorkspace workspace("prompt_present_instr_required");
    workspace.write(".ymh/config.jsonc",
                    "{ \"prompt\": { \"instructions\": { \"enabled\": true, \"max_bytes\": 0 } } }\n");
    EXPECT_THROW((void)load_workspace(workspace), ConfigError);
}

TEST(PromptPresentation, InstructionsEnabledByDefaultWithDshBudget) {
    TempWorkspace workspace("prompt_present_instr_default");
    workspace.write(".ymh/config.jsonc", "{}\n");
    const Config config = load_workspace(workspace);
    EXPECT_TRUE(config.prompt.instructions_enabled);
    EXPECT_EQ(config.prompt.instructions.max_bytes, 65536u);
    EXPECT_TRUE(config.prompt.instructions.load_local);
}

TEST(PromptPresentation, InstructionsParse) {
    TempWorkspace workspace("prompt_present_instr_parse");
    workspace.write(
        ".ymh/config.jsonc",
        "{\n  \"prompt\": { \"instructions\": {\n    \"enabled\": true,\n"
        "    \"max_bytes\": 8192,\n    \"load_local\": true,\n"
        "    \"candidates\": [\"GUIDE.md\"],\n"
        "    \"local_candidates\": [\"GUIDE.local.md\"],\n"
        "    \"project_root_markers\": [\".hg\"]\n  } }\n}\n");

    const Config config = load_workspace(workspace);
    EXPECT_TRUE(config.prompt.instructions_enabled);
    EXPECT_EQ(config.prompt.instructions.max_bytes, 8192u);
    EXPECT_TRUE(config.prompt.instructions.load_local);
    ASSERT_EQ(config.prompt.instructions.candidates.size(), 1u);
    EXPECT_EQ(config.prompt.instructions.candidates[0], "GUIDE.md");
    ASSERT_EQ(config.prompt.instructions.local_candidates.size(), 1u);
    EXPECT_EQ(config.prompt.instructions.local_candidates[0], "GUIDE.local.md");
    ASSERT_EQ(config.prompt.instructions.project_root_markers.size(), 1u);
    EXPECT_EQ(config.prompt.instructions.project_root_markers[0], ".hg");
}

} // namespace

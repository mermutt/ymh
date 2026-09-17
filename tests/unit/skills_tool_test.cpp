#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "support/test_env.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/skills/skill_catalog.hpp"
#include "ymh/skills/skill_tool.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

void write_skill(const std::filesystem::path& root, const std::string& name,
                 const std::string& description, const std::string& body) {
    const std::filesystem::path directory = root / name;
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "SKILL.md")
        << "---\nname: " << name << "\ndescription: " << description << "\n---\n" << body;
}

struct SkillToolEnv {
    explicit SkillToolEnv(const std::string& prefix) : tools(prefix), user_base(prefix + "_user") {
        user_root = user_base.path() / "skills";
        std::filesystem::create_directories(user_root);
    }

    void discover() {
        catalog = std::make_shared<SkillCatalog>(SkillCatalogConfig{}, tools.env, user_root, logger);
        catalog->discover();
        registration = registry.add(make_skill_tool(catalog));
    }

    ToolEnv                    tools;
    TempWorkspace              user_base;
    std::filesystem::path      user_root;
    NullLogger                 logger;
    std::shared_ptr<SkillCatalog> catalog;
    ToolRegistry               registry;
    ToolRegistry::Registration registration;
};

payload::ToolCall call(const std::string& name, nlohmann::json arguments) {
    payload::ToolCall tool_call;
    tool_call.id = "call-1";
    tool_call.name = name;
    tool_call.arguments = std::move(arguments);
    return tool_call;
}

ToolResult run(SkillToolEnv& env, nlohmann::json arguments) {
    return env.registry.execute(call("skill", std::move(arguments)), env.tools.context()).get();
}

TEST(SkillTool, SchemaIsPinned) {
    SkillToolEnv env("skill_tool_schema");
    env.discover();
    Tool* tool = env.registry.find(ToolName{"skill"});
    ASSERT_NE(tool, nullptr);
    const ToolSchema schema = tool->schema();
    EXPECT_EQ(schema.name.value, "skill");
    EXPECT_EQ(schema.version.major, 1u);
    EXPECT_FALSE(schema.destructive);
    EXPECT_TRUE(schema.input_schema.contains("properties"));
}

TEST(SkillTool, MissingOrNonStringNameIsError) {
    SkillToolEnv env("skill_tool_args");
    env.discover();
    EXPECT_EQ(run(env, nlohmann::json::object()).outcome, payload::ToolOutcome::Error);
    EXPECT_EQ(run(env, nlohmann::json{{"name", 42}}).outcome, payload::ToolOutcome::Error);
}

TEST(SkillTool, UnknownNameIsError) {
    SkillToolEnv env("skill_tool_unknown");
    env.discover();
    const ToolResult result = run(env, nlohmann::json{{"name", "nope"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_NE(result.error->find("unknown"), std::string::npos);
}

TEST(SkillTool, TrustedSkillLoadsBody) {
    SkillToolEnv env("skill_tool_trusted");
    write_skill(env.user_root, "git-commit", "Commit helper.", "Use conventional commits.\n");
    env.discover();

    const ToolResult result = run(env, nlohmann::json{{"name", "git-commit"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(result.output, "[skill: git-commit]\nUse conventional commits.\n");
}

TEST(SkillTool, WorkspaceSkillHiddenWhenNotExposed) {
    SkillToolEnv env("skill_tool_workspace");
    write_skill(env.tools.workspace.path() / ".ymh" / "skills", "repo", "Repo skill.", "Secret.\n");
    env.discover();

    const ToolResult result = run(env, nlohmann::json{{"name", "repo"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
}

TEST(SkillTool, WorkspaceSkillLoadsWhenExposed) {
    SkillToolEnv env("skill_tool_exposed");
    write_skill(env.tools.workspace.path() / ".ymh" / "skills", "repo", "Repo skill.", "Visible.\n");
    env.catalog = std::make_shared<SkillCatalog>(
        SkillCatalogConfig{.enabled = true, .expose_workspace = true}, env.tools.env,
        env.user_root, env.logger);
    env.catalog->discover();
    env.registration = env.registry.add(make_skill_tool(env.catalog));

    const ToolResult result = run(env, nlohmann::json{{"name", "repo"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_NE(result.output.find("Visible."), std::string::npos);
}

TEST(SkillTool, RejectsAdditionalProperties) {
    SkillToolEnv env("skill_tool_extra");
    env.discover();
    const ToolResult result =
        run(env, nlohmann::json{{"name", "x"}, {"unexpected", true}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
}

} // namespace

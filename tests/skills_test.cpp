#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_support.hpp"

#include "ymh/config/config.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/skills/frontmatter.hpp"
#include "ymh/skills/skill_catalog.hpp"
#include "ymh/skills/skill_roots.hpp"
#include "ymh/skills/skill_types.hpp"

namespace {

using ymh::LocalEnvironment;
using ymh::NullLogger;
using ymh::SkillCatalog;
using ymh::SkillCatalogConfig;
using ymh::SkillTrust;
using ymh_test::TempWorkspace;

constexpr std::size_t kMaxFrontmatter = 4u * 1024u;
constexpr std::size_t kMaxDescription = 512;

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream << content;
}

// The exact shape of the user's skill: a bare header (no `---`), an underscore
// id, `tools:` instead of `allowed-tools`, and a decorative `====` body.
const char* kOpenCodeStyleSkill = R"(name: improve_ut
description: Improve the code coverage of an existing Louvre/Nik project
tools: bash, read, write, edit
==============================================================
You are a **Coverage Improvement Specialist** for the Louvre/Nik project.
==============================================================
# Skill: Improve Unit Test Coverage

Iteratively raise the coverage of an existing golden-file test suite.
)";

TEST(SkillFrontmatter, AcceptsBareOpenCodeHeaderWithUnderscoreName) {
    const ymh::FrontmatterResult parsed =
        ymh::parse_skill_file(kOpenCodeStyleSkill, "improve_ut", kMaxFrontmatter, kMaxDescription);

    ASSERT_TRUE(parsed.error.empty()) << parsed.error;
    EXPECT_EQ(parsed.meta.name.value, "improve_ut");
    EXPECT_EQ(parsed.meta.description,
              "Improve the code coverage of an existing Louvre/Nik project");
    ASSERT_EQ(parsed.meta.allowed_tools.size(), 4U);
    EXPECT_EQ(parsed.meta.allowed_tools[0], "bash");
    EXPECT_EQ(parsed.meta.allowed_tools[3], "edit");
    EXPECT_NE(parsed.body.find("# Skill: Improve Unit Test Coverage"), std::string::npos);
    EXPECT_NE(parsed.body.find("You are a **Coverage Improvement Specialist**"),
              std::string::npos);
}

TEST(SkillFrontmatter, FencedOpenCodeNameIsADisplayLabelNotTheId) {
    const std::string content = R"(---
name: Git Release
description: Prepare a repository release
metadata:
  opencode/autoinvoke: false
---

## Workflow
)";
    const ymh::FrontmatterResult parsed =
        ymh::parse_skill_file(content, "git-release", kMaxFrontmatter, kMaxDescription);

    ASSERT_TRUE(parsed.error.empty()) << parsed.error;
    EXPECT_EQ(parsed.meta.name.value, "git-release");
    EXPECT_EQ(parsed.meta.display_name, "Git Release");
    EXPECT_NE(parsed.body.find("## Workflow"), std::string::npos);
    ASSERT_FALSE(parsed.warnings.empty());
}

TEST(SkillFrontmatter, ToleratesUnknownKeysAndYamlComments) {
    const std::string content = R"(---
# comment
name: review
description: Review a change
unknown-field: whatever
allowed-tools: [read, write]
---
Body
)";
    const ymh::FrontmatterResult parsed =
        ymh::parse_skill_file(content, "review", kMaxFrontmatter, kMaxDescription);

    ASSERT_TRUE(parsed.error.empty()) << parsed.error;
    ASSERT_EQ(parsed.meta.allowed_tools.size(), 2U);
    EXPECT_EQ(parsed.meta.allowed_tools[0], "read");
    EXPECT_EQ(parsed.meta.allowed_tools[1], "write");
    EXPECT_FALSE(parsed.warnings.empty());
}

TEST(SkillNames, AcceptsUnderscoresAndDigits) {
    EXPECT_TRUE(ymh::is_valid_skill_name("improve_ut"));
    EXPECT_TRUE(ymh::is_valid_skill_name("git-release"));
    EXPECT_TRUE(ymh::is_valid_skill_name("3d-model"));
    EXPECT_FALSE(ymh::is_valid_skill_name(""));
    EXPECT_FALSE(ymh::is_valid_skill_name("Git Release"));
    EXPECT_FALSE(ymh::is_valid_skill_name("../escape"));
}

TEST(SkillDiscovery, FindsOpenCodeProjectTreesUnchanged) {
    TempWorkspace workspace;
    write_file(workspace.root / ".opencode" / "skills" / "improve_ut" / "SKILL.md",
               kOpenCodeStyleSkill);
    write_file(workspace.root / ".opencode" / "skills" / "teams" / "release" / "SKILL.md",
               "---\nname: Release\ndescription: Cut a release\n---\n\nRelease steps.\n");
    write_file(workspace.root / "opencode" / "skills" / "flat-skill.md",
               "description: A flat markdown skill\ntools: read\n\nFlat body.\n");

    LocalEnvironment environment{workspace.root};
    NullLogger       logger;
    SkillCatalogConfig config;
    config.workspace_trusted = true;

    SkillCatalog catalog{config, environment, ymh::workspace_skill_roots(workspace.root), logger};
    catalog.discover();

    const ymh::Skill* skill = catalog.find("improve_ut");
    ASSERT_NE(skill, nullptr);
    EXPECT_EQ(skill->meta.description,
              "Improve the code coverage of an existing Louvre/Nik project");
    EXPECT_EQ(skill->file.filename(), "SKILL.md");

    EXPECT_NE(catalog.find("release"), nullptr);
    EXPECT_NE(catalog.find("flat-skill"), nullptr);

    // A trusted workspace's project skills are visible to the model.
    bool visible = false;
    for (const ymh::Skill* listed : catalog.model_visible()) {
        visible = visible || listed->meta.name.value == "improve_ut";
    }
    EXPECT_TRUE(visible);
}

TEST(SkillDiscovery, UntrustedWorkspaceNeedsTheExposeFlag) {
    TempWorkspace workspace;
    write_file(workspace.root / ".opencode" / "skills" / "improve_ut" / "SKILL.md",
               kOpenCodeStyleSkill);

    LocalEnvironment environment{workspace.root};
    NullLogger       logger;

    {
        SkillCatalogConfig config;
        config.workspace_trusted = false;
        config.expose_workspace  = false;
        SkillCatalog catalog{config, environment, ymh::workspace_skill_roots(workspace.root),
                             logger};
        catalog.discover();
        EXPECT_EQ(catalog.find("improve_ut"), nullptr);
    }
    {
        SkillCatalogConfig config;
        config.workspace_trusted = false;
        config.expose_workspace  = true;
        SkillCatalog catalog{config, environment, ymh::workspace_skill_roots(workspace.root),
                             logger};
        catalog.discover();
        const ymh::Skill* skill = catalog.find("improve_ut");
        ASSERT_NE(skill, nullptr);
        EXPECT_EQ(skill->trust, SkillTrust::Untrusted);
        EXPECT_NE(catalog.find_model_visible("improve_ut"), nullptr);
    }
}

TEST(SkillConfig, RootsAndExposeAreGlobalLayerOnly) {
    TempWorkspace workspace;
    ymh::ConfigPaths paths;
    paths.global    = workspace.root / "global.jsonc";
    paths.workspace = workspace.root / ".ymh" / "config.jsonc";

    write_file(paths.global, R"({"skills": {"roots": ["./opencode/skills"],
                                            "expose_workspace": true}})");
    const ymh::Config config = ymh::load_config(paths);
    ASSERT_EQ(config.skills.roots.size(), 1U);
    EXPECT_TRUE(config.skills.expose_workspace);

    // A repository-shipped config must not whitelist its own instruction files.
    write_file(paths.workspace, R"({"skills": {"roots": ["./opencode/skills"]}})");
    EXPECT_THROW((void)ymh::load_config(paths), ymh::ConfigError);

    write_file(paths.workspace, R"({"skills": {"expose_workspace": true}})");
    EXPECT_THROW((void)ymh::load_config(paths), ymh::ConfigError);

    // A workspace `skills` section that does not name `roots` keeps the global
    // roots instead of clearing them.
    write_file(paths.workspace, R"({"skills": {"max_skills": 32}})");
    const ymh::Config merged = ymh::load_config(paths);
    ASSERT_EQ(merged.skills.roots.size(), 1U);
    EXPECT_EQ(merged.skills.max_skills, 32U);
}

} // namespace

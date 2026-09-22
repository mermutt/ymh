#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "support/test_env.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/skills/skill_catalog.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

std::string skill_document(const std::string& name, const std::string& description,
                           const std::string& body = "Body.\n") {
    return "---\nname: " + name + "\ndescription: " + description + "\n---\n" + body;
}

void write_skill(const std::filesystem::path& root, const std::string& name,
                 const std::string& description, const std::string& body = "Body.\n") {
    const std::filesystem::path directory = root / name;
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "SKILL.md") << skill_document(name, description, body);
}

std::vector<std::string> names_of(const SkillCatalog& catalog) {
    std::vector<std::string> names;
    for (const Skill& skill : catalog.all()) {
        names.push_back(skill.meta.name.value);
    }
    return names;
}

bool has_warning(const SkillCatalog& catalog, const std::string& needle) {
    for (const SkillLoadWarning& warning : catalog.warnings()) {
        if (warning.reason.find(needle) != std::string::npos ||
            warning.file.string().find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Fixture {
    explicit Fixture(const std::string& prefix)
        : workspace(prefix), user_base(prefix + "_user"), env(workspace.path()) {
        user_root = user_base.path() / "skills";
        std::filesystem::create_directories(user_root);
    }

    SkillCatalogConfig config() const {
        SkillCatalogConfig value;
        value.workspace_trusted = true;
        return value;
    }

    std::vector<SkillRoot> roots(const std::filesystem::path& user = {}) const {
        std::vector<SkillRoot> value;
        value.push_back(SkillRoot{user.empty() ? user_root : user, SkillSource::User,
                                  SkillTrust::Trusted});
        value.push_back(SkillRoot{workspace.path() / ".ymh" / "skills", SkillSource::Workspace,
                                  SkillTrust::Untrusted});
        return value;
    }

    TempWorkspace      workspace;
    TempWorkspace      user_base;
    LocalEnvironment   env;
    std::filesystem::path user_root;
    NullLogger         logger;
};

TEST(SkillCatalog, DiscoversBothTiersUserFirst) {
    Fixture fixture("skills_two_tiers");
    write_skill(fixture.user_root, "git-commit", "User commit helper.");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "repo-conventions",
                "Repo conventions.");

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 2u);
    EXPECT_EQ(catalog.all()[0].meta.name.value, "git-commit");
    EXPECT_EQ(catalog.all()[0].source, SkillSource::User);
    EXPECT_EQ(catalog.all()[0].trust, SkillTrust::Trusted);
    EXPECT_EQ(catalog.all()[1].meta.name.value, "repo-conventions");
    EXPECT_EQ(catalog.all()[1].source, SkillSource::Workspace);
    EXPECT_EQ(catalog.all()[1].trust, SkillTrust::Untrusted);
    EXPECT_EQ(catalog.model_visible().size(), 1u);
}

TEST(SkillCatalog, WorkspaceCannotShadowUser) {
    Fixture fixture("skills_shadow");
    write_skill(fixture.user_root, "git-commit", "Trusted.");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "git-commit", "Hostile.");

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 1u);
    EXPECT_EQ(catalog.all()[0].meta.description, "Trusted.");
    EXPECT_TRUE(has_warning(catalog, "shadowed by"));
    const Skill* skill = catalog.find("git-commit");
    ASSERT_NE(skill, nullptr);
    EXPECT_EQ(skill->trust, SkillTrust::Trusted);
}

TEST(SkillCatalog, UntrustedNotModelVisibleUnlessExposed) {
    Fixture fixture("skills_expose");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "repo", "Repo skill.");

    SkillCatalog hidden(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    hidden.discover();
    EXPECT_NE(hidden.find("repo"), nullptr);
    EXPECT_EQ(hidden.find_model_visible("repo"), nullptr);
    EXPECT_TRUE(hidden.model_visible().empty());
    EXPECT_TRUE(hidden.index_section().empty());

    SkillCatalogConfig exposed = fixture.config();
    exposed.expose_workspace = true;
    SkillCatalog open(exposed, fixture.env, fixture.roots(), fixture.logger);
    open.discover();
    EXPECT_NE(open.find_model_visible("repo"), nullptr);
    EXPECT_FALSE(open.index_section().empty());
}

TEST(SkillCatalog, MalformedSkillSkippedWithWarning) {
    Fixture fixture("skills_malformed");
    const auto root = fixture.workspace.path() / ".ymh" / "skills";
    write_skill(root, "good", "Good skill.");
    std::filesystem::create_directories(root / "broken");
    std::ofstream(root / "broken" / "SKILL.md") << "name: broken\ndescription: no fence\n";

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 1u);
    EXPECT_EQ(catalog.all()[0].meta.name.value, "good");
    EXPECT_TRUE(has_warning(catalog, "missing opening"));
}

TEST(SkillCatalog, NonFatalWarningKeepsSkill) {
    Fixture fixture("skills_warning");
    SkillCatalogConfig config = fixture.config();
    config.max_description_bytes = 16;
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "wordy",
                "This description is far too long to fit.");

    SkillCatalog catalog(config, fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 1u);
    EXPECT_TRUE(has_warning(catalog, "description truncated"));
}

TEST(SkillCatalog, OversizedBodySkipped) {
    Fixture fixture("skills_oversized");
    SkillCatalogConfig config = fixture.config();
    config.max_skill_bytes = 64;
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "big", "Big.", std::string(200, 'x'));

    SkillCatalog catalog(config, fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    EXPECT_TRUE(catalog.all().empty());
    EXPECT_TRUE(has_warning(catalog, "max_skill_bytes"));
}

TEST(SkillCatalog, MaxSkillsCapsAfterSort) {
    Fixture fixture("skills_cap");
    const auto root = fixture.workspace.path() / ".ymh" / "skills";
    write_skill(root, "alpha", "A.");
    write_skill(root, "bravo", "B.");
    write_skill(root, "charlie", "C.");
    SkillCatalogConfig config = fixture.config();
    config.max_skills = 2;

    SkillCatalog catalog(config, fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    const std::vector<std::string> names = names_of(catalog);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "bravo");
    EXPECT_TRUE(has_warning(catalog, "max_skills"));
}

TEST(SkillCatalog, SymlinkEscapeSkipped) {
    Fixture fixture("skills_symlink");
    TempWorkspace outside("skills_outside");
    write_skill(outside.path(), "escape", "Outside the root.");

    const auto root = fixture.workspace.path() / ".ymh" / "skills";
    std::filesystem::create_directories(root);
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside.path() / "escape", root / "escape", ec);
    ASSERT_FALSE(ec);

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    EXPECT_TRUE(catalog.all().empty());
    EXPECT_TRUE(has_warning(catalog, "escape"));
}

TEST(SkillCatalog, SymlinkInsideRootAccepted) {
    Fixture fixture("skills_symlink_ok");
    const auto root = fixture.workspace.path() / ".ymh" / "skills";
    write_skill(root, "real", "Real skill.");
    std::error_code ec;
    std::filesystem::create_directory_symlink(root / "real", root / "linked", ec);
    ASSERT_FALSE(ec);

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 1u);
    EXPECT_EQ(catalog.all()[0].meta.name.value, "real");
}

TEST(SkillCatalog, NonRegularSkillFileSkipped) {
    Fixture fixture("skills_fifo");
    const auto root = fixture.workspace.path() / ".ymh" / "skills";
    std::filesystem::create_directories(root / "fifo");
    ASSERT_EQ(::mkfifo((root / "fifo" / "SKILL.md").c_str(), 0600), 0);
    std::filesystem::create_directories(root / "dironly" / "SKILL.md");

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    EXPECT_TRUE(catalog.all().empty());
    EXPECT_TRUE(has_warning(catalog, "not a regular file"));
}

TEST(SkillCatalog, AbsentRootYieldsWarning) {
    Fixture fixture("skills_absent");
    const std::filesystem::path missing = fixture.user_base.path() / "does-not-exist";
    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(missing), fixture.logger);
    catalog.discover();

    EXPECT_TRUE(catalog.all().empty());
    EXPECT_TRUE(has_warning(catalog, "unavailable"));
}

TEST(SkillCatalog, RelativeUserRootDisablesTier) {
    Fixture fixture("skills_relative");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "repo", "Repo skill.");

    SkillCatalog catalog(fixture.config(), fixture.env,
                         fixture.roots(std::filesystem::path{"relative/skills"}), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 1u);
    EXPECT_EQ(catalog.all()[0].meta.name.value, "repo");
    EXPECT_EQ(catalog.all()[0].source, SkillSource::Workspace);
    EXPECT_TRUE(has_warning(catalog, "relative user root"));
}

TEST(SkillCatalog, DeterministicDiscovery) {
    Fixture fixture("skills_deterministic");
    write_skill(fixture.user_root, "zulu", "Z.");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "alpha", "A.");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "mike", "M.");

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();
    const std::vector<std::string> first_names = names_of(catalog);
    const std::string              first_index = catalog.index_section();

    catalog.discover();
    EXPECT_EQ(names_of(catalog), first_names);
    EXPECT_EQ(catalog.index_section(), first_index);
}

TEST(SkillCatalog, IndexTruncationAddsNote) {
    Fixture fixture("skills_index_cap");
    for (int index = 0; index < 10; ++index) {
        write_skill(fixture.user_root, "alpha" + std::to_string(index), "D.");
    }
    SkillCatalogConfig config = fixture.config();
    config.max_index_bytes = 200;

    SkillCatalog catalog(config, fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 10u);
    ASSERT_EQ(catalog.model_visible().size(), 10u);
    EXPECT_NE(catalog.index_section().find("more skills not shown"), std::string::npos);
    EXPECT_LE(catalog.index_section().size(), config.max_index_bytes);
}

TEST(SkillCatalog, DisabledCatalogIsEmpty) {
    Fixture fixture("skills_disabled");
    write_skill(fixture.user_root, "git-commit", "Trusted.");
    SkillCatalogConfig config = fixture.config();
    config.enabled = false;

    SkillCatalog catalog(config, fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    EXPECT_TRUE(catalog.all().empty());
    EXPECT_TRUE(catalog.index_section().empty());
}

TEST(SkillCatalog, IsNotCopyableOrMovable) {
    EXPECT_FALSE(std::is_copy_constructible_v<SkillCatalog>);
    EXPECT_FALSE(std::is_copy_assignable_v<SkillCatalog>);
    EXPECT_FALSE(std::is_move_constructible_v<SkillCatalog>);
    EXPECT_FALSE(std::is_move_assignable_v<SkillCatalog>);
}

TEST(SkillCatalog, IndexListsNameAndDescription) {
    Fixture fixture("skills_index_text");
    write_skill(fixture.user_root, "git-commit", "Write a conventional commit.");

    SkillCatalog catalog(fixture.config(), fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    const std::string& index = catalog.index_section();
    EXPECT_NE(index.find("git-commit"), std::string::npos);
    EXPECT_NE(index.find("Write a conventional commit."), std::string::npos);
}

TEST(SkillCatalog, UntrustedWorkspaceSkillIsAbsentFromTheCatalog) {
    Fixture fixture("skills_untrusted_absent");
    write_skill(fixture.workspace.path() / ".ymh" / "skills", "repo", "Hostile.");

    SkillCatalogConfig config = fixture.config();
    config.workspace_trusted = false;
    SkillCatalog catalog(config, fixture.env, fixture.roots(), fixture.logger);
    catalog.discover();

    EXPECT_EQ(catalog.find("repo"), nullptr);
    EXPECT_TRUE(catalog.all().empty());
    EXPECT_TRUE(has_warning(catalog, "not trusted"));
}

TEST(SkillCatalog, NewUserRootsPrecedeTheConfigRoot) {
    TempWorkspace home("skills_home_root");
    TempWorkspace config_base("skills_cfg_root");
    const std::filesystem::path home_skills = home.path() / ".ymh" / "skills";
    const std::filesystem::path claude_skills = home.path() / ".claude" / "skills";
    const std::filesystem::path config_skills = config_base.path() / "skills";
    write_skill(home_skills, "shared", "From ~/.ymh.");
    write_skill(config_skills, "shared", "From the config root.");
    write_skill(claude_skills, "claude-only", "From ~/.claude.");

    LocalEnvironment   env(home.path());
    NullLogger         logger;
    SkillCatalogConfig config;
    config.workspace_trusted = false;
    std::vector<SkillRoot> roots{SkillRoot{home_skills, SkillSource::Home, SkillTrust::Trusted},
                                 SkillRoot{config_skills, SkillSource::User, SkillTrust::Trusted},
                                 SkillRoot{claude_skills, SkillSource::Claude, SkillTrust::Trusted}};
    SkillCatalog catalog(config, env, roots, logger);
    catalog.discover();

    ASSERT_EQ(catalog.all().size(), 2u);
    EXPECT_EQ(catalog.all()[0].meta.name.value, "shared");
    EXPECT_EQ(catalog.all()[0].source, SkillSource::Home);
    EXPECT_EQ(catalog.all()[0].meta.description, "From ~/.ymh.");
    EXPECT_EQ(catalog.all()[1].meta.name.value, "claude-only");
    EXPECT_EQ(catalog.all()[1].source, SkillSource::Claude);
    EXPECT_TRUE(has_warning(catalog, "shadowed by"));
}

} // namespace

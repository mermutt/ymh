#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "support/test_env.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"

namespace {

using namespace ymh;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
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

void write_file(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << body;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream      input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string skill_md(const std::string& name) {
    return "---\nname: " + name + "\ndescription: the " + name + " skill\n---\n\n# " + name + "\n";
}

TEST(Spec71D1, ImportsLocalcodeSkillsTreeIntoUserTier) {
    test::TempWorkspace workspace("spec71_import");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    const std::filesystem::path localcode = workspace.path() / "home" / ".localcode";
    write_file(localcode / "config.json", "{}");
    write_file(localcode / "skills" / "git-commit" / "SKILL.md", skill_md("git-commit"));
    write_file(localcode / "skills" / "git-commit" / "template.md", "TEMPLATE\n");
    write_file(localcode / "skills" / "review" / "SKILL.md", skill_md("review"));

    CliInvocation    invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("y\n");
    std::ostringstream out;
    std::ostringstream err;
    const std::filesystem::path global = workspace.path() / "config" / "ymh" / "config.jsonc";

    ASSERT_TRUE(maybe_import_localcode_config(invocation, global, /*interactive=*/true, in, out,
                                              err))
        << err.str();

    const std::filesystem::path skills = workspace.path() / "config" / "ymh" / "skills";
    ASSERT_TRUE(std::filesystem::is_regular_file(skills / "git-commit" / "SKILL.md"));
    EXPECT_EQ(read_text(skills / "git-commit" / "SKILL.md"), skill_md("git-commit"));
    EXPECT_EQ(read_text(skills / "git-commit" / "template.md"), "TEMPLATE\n");
    EXPECT_TRUE(std::filesystem::is_regular_file(skills / "review" / "SKILL.md"));
    EXPECT_TRUE(std::filesystem::is_regular_file(global));
}

TEST(Spec71D3, NonClobberingAndIdempotent) {
    test::TempWorkspace workspace("spec71_noclobber");
    const std::filesystem::path localcode = workspace.path() / "localcode";
    const std::filesystem::path skills   = workspace.path() / "config" / "skills";

    write_file(localcode / "skills" / "a" / "SKILL.md", "NEW-A\n");
    write_file(localcode / "skills" / "b" / "SKILL.md", "NEW-B\n");
    write_file(skills / "a" / "SKILL.md", "ORIGINAL-A\n");

    std::ostringstream err;
    EXPECT_EQ(import_localcode_skills(localcode, skills, err), 1u) << err.str();
    EXPECT_EQ(read_text(skills / "a" / "SKILL.md"), "ORIGINAL-A\n");
    EXPECT_EQ(read_text(skills / "b" / "SKILL.md"), "NEW-B\n");

    EXPECT_EQ(import_localcode_skills(localcode, skills, err), 0u);
    EXPECT_EQ(read_text(skills / "a" / "SKILL.md"), "ORIGINAL-A\n");
}

TEST(Spec71D2, SkipsNonSkillEntriesAndAbsentSource) {
    test::TempWorkspace workspace("spec71_skip");
    const std::filesystem::path localcode = workspace.path() / "localcode";
    const std::filesystem::path skills   = workspace.path() / "config" / "skills";

    write_file(localcode / "skills" / "good" / "SKILL.md", "GOOD\n");
    write_file(localcode / "skills" / "loose.txt", "LOOSE\n");
    write_file(localcode / "skills" / "noskill" / "readme.txt", "README\n");

    std::ostringstream err;
    EXPECT_EQ(import_localcode_skills(localcode, skills, err), 1u) << err.str();
    EXPECT_TRUE(std::filesystem::is_regular_file(skills / "good" / "SKILL.md"));
    EXPECT_FALSE(std::filesystem::exists(skills / "noskill"));
    EXPECT_FALSE(std::filesystem::exists(skills / "loose.txt"));

    std::ostringstream second;
    EXPECT_EQ(import_localcode_skills(workspace.path() / "missing",
                                      workspace.path() / "other" / "skills", second),
              0u);
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / "other" / "skills"));
}

TEST(Spec71D2, SymlinkedSkillMdAndDirectoryAreNotFollowed) {
    test::TempWorkspace workspace("spec71_symlink");
    const std::filesystem::path localcode = workspace.path() / "localcode";
    const std::filesystem::path skills   = workspace.path() / "config" / "skills";

    write_file(localcode / "skills" / "real" / "SKILL.md", "REAL\n");
    const std::filesystem::path outside = workspace.path() / "outside.md";
    write_file(outside, "OUTSIDE\n");
    std::filesystem::create_directories(localcode / "skills" / "linked");
    std::filesystem::create_symlink(outside, localcode / "skills" / "linked" / "SKILL.md");
    std::filesystem::create_symlink(localcode / "skills" / "real",
                                    localcode / "skills" / "linked-dir");

    std::ostringstream err;
    EXPECT_EQ(import_localcode_skills(localcode, skills, err), 1u) << err.str();
    EXPECT_TRUE(std::filesystem::is_regular_file(skills / "real" / "SKILL.md"));
    EXPECT_FALSE(std::filesystem::exists(skills / "linked"));
    EXPECT_FALSE(std::filesystem::exists(skills / "linked-dir"));
    EXPECT_NE(err.str().find("no regular SKILL.md"), std::string::npos);
}

TEST(Spec71D5, ImportedPermissionsSectionIsConsumed) {
    const nlohmann::json localcode = nlohmann::json::parse(R"JSON({
      "skip_permissions": true,
      "permission": { "bash": [ { "match": "dir *", "decision": "allow" },
                                { "match": "rm *", "decision": "deny" } ] }
    })JSON");

    std::string                          error;
    std::optional<LocalcodeImportResult> result = build_localcode_import(localcode, error);
    ASSERT_TRUE(result.has_value()) << error;
    ASSERT_TRUE(result->document.contains("permissions"));
    ASSERT_EQ(result->document["permissions"]["rules"].size(), 2u);

    test::TempWorkspace workspace("spec71_perms");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    write_file(global, result->document.dump(2));

    ConfigPaths paths;
    paths.global        = global;
    const Config config = load_config(paths);
    EXPECT_EQ(config.permissions.default_verdict, "allow");
    ASSERT_EQ(config.permissions.rules.size(), 2u);

    const PermissionConfig policy = to_permission_config(config);
    EXPECT_EQ(policy.default_verdict, PolicyVerdict::Allow);
}

TEST(Spec71D4, GlobalDirPresentCopiesNoSkills) {
    test::TempWorkspace workspace("spec71_gate");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    write_file(workspace.path() / "home" / ".localcode" / "config.json", "{}");
    write_file(workspace.path() / "home" / ".localcode" / "skills" / "s" / "SKILL.md", "S\n");
    std::filesystem::create_directories(workspace.path() / "config" / "ymh");

    CliInvocation    invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("y\n");
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_FALSE(maybe_import_localcode_config(invocation,
                                               workspace.path() / "config" / "ymh" / "config.jsonc",
                                               /*interactive=*/true, in, out, err));
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / "config" / "ymh" / "skills"));
}

TEST(Spec71D4, DeclineAndNonInteractiveCopyNoSkills) {
    test::TempWorkspace workspace("spec71_decline");
    ScopedEnv           xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv           home("HOME", (workspace.path() / "home").string());

    write_file(workspace.path() / "home" / ".localcode" / "config.json", "{}");
    write_file(workspace.path() / "home" / ".localcode" / "skills" / "s" / "SKILL.md", "S\n");

    CliInvocation    invocation;
    invocation.command = CliInvocation::Command::Tui;
    std::istringstream in("n\n");
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_FALSE(maybe_import_localcode_config(invocation,
                                               workspace.path() / "config2" / "ymh" / "config.jsonc",
                                               /*interactive=*/true, in, out, err));
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / "config2" / "ymh" / "skills"));

    std::istringstream noninteractive_in;
    EXPECT_FALSE(maybe_import_localcode_config(
        invocation, workspace.path() / "config3" / "ymh" / "config.jsonc",
        /*interactive=*/false, noninteractive_in, out, err));
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / "config3" / "ymh" / "skills"));
}

} // namespace

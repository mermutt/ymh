#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/skills/file_commands.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

SkillRoot trusted_root(const std::filesystem::path& path) {
    return SkillRoot{path, SkillSource::User, SkillTrust::Trusted};
}

SkillRoot workspace_root(const std::filesystem::path& path) {
    return SkillRoot{path, SkillSource::Workspace, SkillTrust::Untrusted};
}

void write_command(const std::filesystem::path& root, const std::string& name,
                   const std::string& content) {
    std::filesystem::create_directories(root);
    std::ofstream(root / (name + ".md")) << content;
}

bool has_warning(const FileCommandCatalog& catalog, const std::string& needle) {
    return std::any_of(catalog.warnings.begin(), catalog.warnings.end(),
                       [&needle](const FileCommandLoadWarning& warning) {
                           return warning.reason.find(needle) != std::string::npos;
                       });
}

TEST(FileCommands, DiscoversFrontmatterAndBody) {
    TempWorkspace root("file_commands_front");
    write_command(root.path(), "fix",
                  "---\ndescription: Fix a bug\nargument-hint: <issue>\n"
                  "allowed-tools: [read_file, write_file]\n---\nBody line one.\n");

    const FileCommandCatalog catalog =
        discover_file_commands({trusted_root(root.path())}, {"help"}, true);
    ASSERT_EQ(catalog.commands.size(), 1u);
    const FileCommand& command = catalog.commands.front();
    EXPECT_EQ(command.name, "fix");
    EXPECT_EQ(command.description, "Fix a bug");
    EXPECT_EQ(command.argument_hint, "<issue>");
    ASSERT_EQ(command.allowed_tools.size(), 2u);
    EXPECT_EQ(command.allowed_tools[0], "read_file");
    EXPECT_EQ(command.allowed_tools[1], "write_file");
    EXPECT_EQ(command.body, "Body line one.\n");
    EXPECT_TRUE(command.trusted);
}

TEST(FileCommands, MissingFrontmatterUsesFirstBodyLine) {
    TempWorkspace root("file_commands_bodyline");
    write_command(root.path(), "explain", "\n# Explain\n\nDo the thing.\n");

    const FileCommandCatalog catalog =
        discover_file_commands({trusted_root(root.path())}, {}, true);
    ASSERT_EQ(catalog.commands.size(), 1u);
    EXPECT_EQ(catalog.commands.front().description, "# Explain");
    EXPECT_EQ(catalog.commands.front().body, "\n# Explain\n\nDo the thing.\n");
}

TEST(FileCommands, MalformedFrontmatterDegradesNotFatal) {
    TempWorkspace root("file_commands_malformed");
    write_command(root.path(), "broken", "---\ndescription: no closing fence\nBody.\n");

    const FileCommandCatalog catalog =
        discover_file_commands({trusted_root(root.path())}, {}, true);
    ASSERT_EQ(catalog.commands.size(), 1u);
    EXPECT_EQ(catalog.commands.front().description, "---");
    EXPECT_TRUE(catalog.commands.front().body.find("no closing fence") != std::string::npos);
}

TEST(FileCommands, ReservedNameIsRejected) {
    TempWorkspace root("file_commands_reserved");
    write_command(root.path(), "help", "Sneaky replacement.\n");

    const FileCommandCatalog catalog =
        discover_file_commands({trusted_root(root.path())}, {"help"}, true);
    EXPECT_TRUE(catalog.commands.empty());
    EXPECT_TRUE(has_warning(catalog, "reserved"));
}

TEST(FileCommands, FirstRootWinsWithinUserTier) {
    TempWorkspace home("file_commands_home");
    TempWorkspace claude("file_commands_claude");
    write_command(home.path(), "fix", "home body\n");
    write_command(claude.path(), "fix", "claude body\n");

    const FileCommandCatalog catalog =
        discover_file_commands({trusted_root(home.path()), trusted_root(claude.path())}, {}, true);
    ASSERT_EQ(catalog.commands.size(), 1u);
    EXPECT_EQ(catalog.commands.front().body, "home body\n");
    EXPECT_TRUE(has_warning(catalog, "shadowed by"));
}

TEST(FileCommands, UntrustedWorkspaceIsSkipped) {
    TempWorkspace root("file_commands_untrusted");
    write_command(root.path(), "deploy", "Deploy everything.\n");

    const FileCommandCatalog catalog =
        discover_file_commands({workspace_root(root.path())}, {}, false);
    EXPECT_TRUE(catalog.commands.empty());
    EXPECT_TRUE(has_warning(catalog, "not trusted"));
}

TEST(FileCommands, TrustedWorkspaceIsLoaded) {
    TempWorkspace root("file_commands_trusted_ws");
    write_command(root.path(), "deploy", "Deploy everything.\n");

    const FileCommandCatalog catalog =
        discover_file_commands({workspace_root(root.path())}, {}, true);
    ASSERT_EQ(catalog.commands.size(), 1u);
    EXPECT_EQ(catalog.commands.front().name, "deploy");
    EXPECT_FALSE(catalog.commands.front().trusted);
}

TEST(FileCommands, InvalidNameAndNonMarkdownAreIgnored) {
    TempWorkspace root("file_commands_invalid");
    write_command(root.path(), "Bad Name", "nope\n");
    std::ofstream(root.path() / "notes.txt") << "not a command\n";

    const FileCommandCatalog catalog =
        discover_file_commands({trusted_root(root.path())}, {}, true);
    EXPECT_TRUE(catalog.commands.empty());
    EXPECT_TRUE(has_warning(catalog, "invalid command name"));
}

TEST(FileCommands, SubstitutesArgumentsExactlyOnce) {
    EXPECT_EQ(substitute_arguments("a $ARGUMENTS b $ARGUMENTS", "X"), "a X b X");
    EXPECT_EQ(substitute_arguments("no token", "ignored"), "no token");
    EXPECT_EQ(substitute_arguments("$ARGUMENTS", ""), "");
}

} // namespace

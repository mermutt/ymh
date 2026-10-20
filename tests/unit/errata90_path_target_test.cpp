#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "support/test_env.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/path_target.hpp"

namespace {

using namespace ymh;
using nlohmann::json;

std::optional<std::filesystem::path> target(const std::filesystem::path& root,
                                            std::string tool,
                                            json arguments = json::object()) {
    return request_path_target(tool, arguments, root);
}

TEST(PathTarget, AllSevenToolsAreMapped) {
    ymh::test::TempWorkspace workspace("p90_all_tools");
    const auto root = workspace.path();
    EXPECT_EQ(target(root, "read_file", {{"path", "a"}}), root / "a");
    EXPECT_EQ(target(root, "write_file", {{"path", "a"}}), root / "a");
    EXPECT_EQ(target(root, "edit_file", {{"path", "a"}}), root / "a");
    EXPECT_EQ(target(root, "grep", {{"path", "a"}}), root / "a");
    EXPECT_EQ(target(root, "glob", {{"path", "a"}}), root / "a");
    EXPECT_EQ(target(root, "git_status"), root.lexically_normal());
    EXPECT_EQ(target(root, "git_diff", {{"path", "a"}}), root / "a");
    EXPECT_FALSE(target(root, "shell", {{"command", "ls"}}).has_value());
    EXPECT_FALSE(target(root, "mcp.github.x").has_value());
}

TEST(PathTarget, RelativeIsJoinedAndNormalized) {
    ymh::test::TempWorkspace workspace("p90_relative");
    const auto root = workspace.path();
    EXPECT_EQ(target(root, "write_file", {{"path", "src/../src/x"}}),
              (root / "src" / "x").lexically_normal());
}

TEST(PathTarget, DotDotEscapesToAbsolute) {
    ymh::test::TempWorkspace workspace("p90_dotdot");
    const auto root = workspace.path();
    const auto value = target(root, "write_file", {{"path", "../x"}});
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(value->is_absolute());
    EXPECT_FALSE(path_is_within_root(root, *value));
}

TEST(PathTarget, AbsoluteOutsideIsKept) {
    ymh::test::TempWorkspace workspace("p90_absolute");
    EXPECT_EQ(target(workspace.path(), "read_file", {{"path", "/etc/passwd"}}),
              std::filesystem::path{"/etc/passwd"});
}

TEST(PathTarget, MissingRequiredIsNullopt) {
    ymh::test::TempWorkspace workspace("p90_missing");
    const auto root = workspace.path();
    EXPECT_FALSE(target(root, "read_file").has_value());
    EXPECT_FALSE(target(root, "read_file", {{"path", 3}}).has_value());
    EXPECT_FALSE(target(root, "read_file", {{"path", ""}}).has_value());
}

TEST(PathTarget, OptionalDefaultsToRoot) {
    ymh::test::TempWorkspace workspace("p90_optional");
    const auto root = workspace.path().lexically_normal();
    EXPECT_EQ(target(root, "grep"), root);
    EXPECT_EQ(target(root, "glob"), root);
    EXPECT_EQ(target(root, "git_diff"), root);
    EXPECT_EQ(target(root, "git_status", {{"path", "ignored"}}), root);
}

TEST(PathTarget, EmptyOptionalIsNullopt) {
    ymh::test::TempWorkspace workspace("p90_empty_optional");
    EXPECT_FALSE(target(workspace.path(), "grep", {{"path", ""}}).has_value());
    EXPECT_FALSE(target(workspace.path(), "glob", {{"path", 7}}).has_value());
}

TEST(PathTarget, TildeIsLiteralForToolPaths) {
    ymh::test::TempWorkspace workspace("p90_tilde");
    EXPECT_EQ(target(workspace.path(), "read_file", {{"path", "~"}}),
              workspace.path() / "~");
}

TEST(PathTarget, NoIO) {
    ymh::test::TempWorkspace workspace("p90_no_io");
    const auto value = target(workspace.path(), "read_file", {{"path", "does/not/exist"}});
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, workspace.path() / "does" / "not" / "exist");
}

TEST(PathTarget, ProtectedDetection) {
    ymh::test::TempWorkspace workspace("p90_protected");
    const auto root = workspace.path();
    EXPECT_TRUE(path_is_protected(root, root / ".ymh"));
    EXPECT_TRUE(path_is_protected(root, root / ".ymh" / "config.jsonc"));
    EXPECT_TRUE(path_is_protected(root, root / ".git"));
    EXPECT_TRUE(path_is_protected(root, root / ".git" / "hooks" / "x"));
    EXPECT_TRUE(path_is_protected(root, root / "src" / ".git" / "hooks" / "x"));
    EXPECT_TRUE(path_is_protected(root, root / "src" / ".ymh" / "config.jsonc"));
    EXPECT_FALSE(path_is_protected(root, root / ".ymhfoo"));
    EXPECT_FALSE(path_is_protected(root, root / ".gitignore"));
    EXPECT_FALSE(path_is_protected(root, root / "src" / "x"));
}

TEST(PathTarget, ProtectedOutsideNotProtected) {
    ymh::test::TempWorkspace workspace("p90_protected_outside");
    EXPECT_FALSE(path_is_protected(workspace.path(), "/tmp/.ymh/config.jsonc"));
}

TEST(PathTarget, ProtectedRootRelativeCaveat) {
    EXPECT_FALSE(path_is_protected("/srv/.ymh/project", "/srv/.ymh/project/x"));
}

} // namespace

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "support/test_env.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"

namespace {

using namespace ymh;
using ymh::test::TempWorkspace;

TEST(PathSafety, RelativePathResolvesUnderRoot) {
    TempWorkspace workspace("path_rel");
    LocalEnvironment env(workspace.path());

    EXPECT_EQ(env.resolve("a.txt"), env.root() / "a.txt");
    EXPECT_EQ(env.resolve("sub/dir/a.txt"), env.root() / "sub" / "dir" / "a.txt");
}

TEST(PathSafety, AbsolutePathUnderRootResolves) {
    TempWorkspace workspace("path_abs");
    LocalEnvironment env(workspace.path());

    const std::filesystem::path target = env.root() / "sub" / "a.txt";
    EXPECT_EQ(env.resolve(target.string()), target);
}

TEST(PathSafety, DotDotStayingInsideRootResolves) {
    TempWorkspace workspace("path_dotdot_in");
    std::filesystem::create_directories(workspace.path() / "a");
    LocalEnvironment env(workspace.path());

    EXPECT_EQ(env.resolve("a/../b.txt"), env.root() / "b.txt");
}

TEST(PathSafety, DotDotEscapingRootIsRejected) {
    TempWorkspace workspace("path_dotdot_out");
    LocalEnvironment env(workspace.path());

    EXPECT_THROW(env.resolve("../escape.txt"), ToolError);
    try {
        env.resolve("../escape.txt");
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }
}

TEST(PathSafety, AbsolutePathOutsideRootIsRejected) {
    TempWorkspace workspace("path_abs_out");
    LocalEnvironment env(workspace.path());

    try {
        env.resolve("/etc/passwd");
        FAIL() << "expected PathEscape";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }
}

TEST(PathSafety, NonexistentWriteTargetUnderRootResolves) {
    TempWorkspace workspace("path_write_new");
    LocalEnvironment env(workspace.path());

    EXPECT_EQ(env.resolve("new/dir/file.txt"),
              env.root() / "new" / "dir" / "file.txt");
}

TEST(PathSafety, SymlinkAtFinalComponentOutsideRootIsRejected) {
    TempWorkspace outside("path_link_target");
    TempWorkspace workspace("path_link_final");
    std::error_code ec;
    std::filesystem::create_symlink(outside.path(), workspace.path() / "link", ec);
    ASSERT_FALSE(ec) << ec.message();

    LocalEnvironment env(workspace.path());
    try {
        env.resolve("link");
        FAIL() << "expected PathEscape";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }
}

TEST(PathSafety, SymlinkDirectoryInTheMiddleOutsideRootIsRejected) {
    TempWorkspace outside("path_mid_target");
    TempWorkspace workspace("path_mid_link");
    std::error_code ec;
    std::filesystem::create_symlink(outside.path(), workspace.path() / "dir", ec);
    ASSERT_FALSE(ec) << ec.message();

    LocalEnvironment env(workspace.path());
    try {
        env.resolve("dir/file.txt");
        FAIL() << "expected PathEscape";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }
}

TEST(PathSafety, InRootSymlinkResolvesToItsTarget) {
    TempWorkspace workspace("path_link_inside");
    std::filesystem::create_directories(workspace.path());
    {
        std::ofstream(workspace.path() / "target.txt") << "hello";
    }
    std::error_code ec;
    std::filesystem::create_symlink(workspace.path() / "target.txt",
                                    workspace.path() / "alias.txt", ec);
    ASSERT_FALSE(ec) << ec.message();

    LocalEnvironment env(workspace.path());
    EXPECT_EQ(env.resolve("alias.txt"),
              std::filesystem::canonical(workspace.path() / "target.txt"));
}

TEST(PathSafety, ComponentWisePrefixBoundary) {
    TempWorkspace workspace("path_prefix");
    LocalEnvironment env(workspace.path());

    EXPECT_TRUE(path_is_within(env.root() / "a" / "bc", env.root() / "a"));
    EXPECT_FALSE(path_is_within(env.root() / "ab", env.root() / "a"));
    EXPECT_FALSE(path_is_within(env.root().parent_path() / "other", env.root()));
}

TEST(PathSafety, EmptyPathIsRejected) {
    TempWorkspace workspace("path_empty");
    LocalEnvironment env(workspace.path());

    EXPECT_THROW(env.resolve(""), ToolError);
}

TEST(PathSafety, UnrestrictedSkipsContainment) {
    TempWorkspace workspace("path_unrestricted");
    LocalEnvironment env(workspace.path(), SandboxMode::Unrestricted);

    const std::filesystem::path expected =
        (env.root() / ".." / "escape.txt").lexically_normal();
    EXPECT_EQ(env.resolve("../escape.txt"), expected);
}

TEST(PathSafety, GetcwdIsNeverAResolutionBase) {
    TempWorkspace root_workspace("path_getcwd_root");
    TempWorkspace other_workspace("path_getcwd_other");
    LocalEnvironment env(root_workspace.path());

    const std::filesystem::path saved = std::filesystem::current_path();
    std::filesystem::current_path(other_workspace.path());
    const std::filesystem::path resolved = env.resolve("file.txt");
    std::filesystem::current_path(saved);

    EXPECT_EQ(resolved, env.root() / "file.txt");
    EXPECT_NE(resolved, other_workspace.path() / "file.txt");
}

} // namespace

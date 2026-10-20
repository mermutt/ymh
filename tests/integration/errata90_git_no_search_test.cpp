#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "support/test_env.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/git_service.hpp"
#include "ymh/execution/services.hpp"

namespace {

using namespace ymh;

int git(const std::filesystem::path& dir, const std::string& args) {
    const std::string command =
        "GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null "
        "GIT_AUTHOR_NAME=ymh GIT_AUTHOR_EMAIL=ymh@example.com "
        "GIT_COMMITTER_NAME=ymh GIT_COMMITTER_EMAIL=ymh@example.com git -C '" +
        dir.string() + "' " + args + " >/dev/null 2>&1";
    return std::system(command.c_str());
}

TEST(Errata90GitNoSearch, ChildWorkspaceDoesNotReadAncestorRepository) {
    ymh::test::TempWorkspace parent("p90_git_parent");
    ASSERT_EQ(git(parent.path(), "init -q -b main"), 0);
    parent.write("tracked.txt", "x\n");
    ASSERT_EQ(git(parent.path(), "add tracked.txt"), 0);
    ASSERT_EQ(git(parent.path(), "commit -q -m init"), 0);

    const std::filesystem::path child = parent.path() / "child";
    std::filesystem::create_directories(child);

    LibGit2GitService service;
    GitQuery query;
    query.root = child;
    EXPECT_THROW(service.status(query).get(), ToolError);
}

TEST(Errata90GitNoSearch, WorkspaceRepositoryStillWorks) {
    ymh::test::TempWorkspace root("p90_git_workspace");
    ASSERT_EQ(git(root.path(), "init -q -b main"), 0);
    root.write("tracked.txt", "x\n");
    ASSERT_EQ(git(root.path(), "add tracked.txt"), 0);
    ASSERT_EQ(git(root.path(), "commit -q -m init"), 0);
    root.write("tracked.txt", "changed\n");

    LibGit2GitService service;
    GitQuery query;
    query.root = root.path();
    const GitStatus status = service.status(query).get();
    EXPECT_NE(status.text.find("M tracked.txt"), std::string::npos);
}

} // namespace

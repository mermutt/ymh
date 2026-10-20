#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/filesystem.hpp"

namespace {

using namespace ymh;

TEST(Errata90GrepSymlink, DoesNotFollowSymlinkEntries) {
    ymh::test::TempWorkspace outside("p90_grep_outside");
    outside.write("secret.txt", "TOP_SECRET outside\n");
    ymh::test::TempWorkspace workspace("p90_grep_root");
    workspace.write("visible.txt", "TOP_SECRET inside\n");
    std::error_code error;
    std::filesystem::create_symlink(outside.path() / "secret.txt",
                                    workspace.path() / "leak", error);
    ASSERT_FALSE(error) << error.message();

    LocalFilesystem fs(workspace.path(), ToolConfig{});
    GrepQuery query;
    query.pattern = "TOP_SECRET";
    query.base = workspace.path();
    query.max_results = 100;
    const std::vector<GrepMatch> matches = fs.grep(query).get();
    bool saw_visible = false;
    for (const GrepMatch& match : matches) {
        EXPECT_EQ(match.path.filename().string(), "visible.txt");
        saw_visible = true;
    }
    EXPECT_TRUE(saw_visible);
}

TEST(Errata90GrepSymlink, SkipsProtectedTraversal) {
    ymh::test::TempWorkspace workspace("p90_grep_protected");
    workspace.write("visible.txt", "needle\n");
    workspace.write(".ymh/sessions.db", "needle\n");
    workspace.write(".git/config", "needle\n");
    workspace.write("src/nested.txt", "needle\n");

    LocalFilesystem fs(workspace.path(), ToolConfig{});
    GrepQuery query;
    query.pattern = "needle";
    query.base = workspace.path();
    query.max_results = 100;
    const std::vector<GrepMatch> matches = fs.grep(query).get();
    std::vector<std::string> names;
    for (const GrepMatch& match : matches) {
        names.push_back(match.path.filename().string());
    }
    EXPECT_NE(std::find(names.begin(), names.end(), "visible.txt"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "nested.txt"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "sessions.db"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "config"), names.end());

    GlobQuery glob;
    glob.pattern = "**/*";
    glob.base = workspace.path();
    glob.max_results = 100;
    const auto files = fs.glob(glob).get();
    for (const auto& file : files) {
        const std::string text = file.generic_string();
        EXPECT_EQ(text.find("/.ymh/"), std::string::npos) << text;
        EXPECT_EQ(text.find("/.git/"), std::string::npos) << text;
    }
}

} // namespace

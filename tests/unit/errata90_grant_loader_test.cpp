#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

struct Loaded {
    ymh::test::TempWorkspace workspace{"p90_grant"};
    std::filesystem::path    path = workspace.path() / "permissions.jsonc";

    void write(const std::string& grants) {
        workspace.write("permissions.jsonc",
                        R"({"schema":"ymh.permissions/1","workspace_id":"ws-1","grants":)" +
                            grants + "}");
    }

    std::vector<PolicyRule> load() {
        auto store = open_file_grant_store(path, std::string{"ws-1"});
        return store->load();
    }
};

TEST(GrantLoader, RejectsWildcardTool) {
    Loaded loaded;
    loaded.write(R"([{"tool":"*","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
    loaded.write(R"([{"tool":"git_*","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
}

TEST(GrantLoader, RejectsEmptyShellCommand) {
    Loaded loaded;
    loaded.write(R"([{"tool":"shell","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
    loaded.write(R"([{"tool":"terminal","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
}

TEST(GrantLoader, RejectsUnboundedShellCommand) {
    Loaded loaded;
    loaded.write(R"([{"tool":"shell","command":"*","match":"glob","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
    loaded.write(R"([{"tool":"terminal","command":"?*","match":"glob","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
    loaded.write(R"([{"tool":"shell","command":"git *","match":"glob","effect":"allow"}])");
    EXPECT_EQ(loaded.load().size(), 1u);
}

TEST(GrantLoader, RejectsPathToolCommand) {
    Loaded loaded;
    loaded.write(R"([{"tool":"write_file","command":"src/*","match":"glob","effect":"allow"}])");
    EXPECT_TRUE(loaded.load().empty());
}

TEST(GrantLoader, PathToolLoadsInRoot) {
    Loaded loaded;
    loaded.write(R"([{"tool":"write_file","effect":"allow"}])");
    const auto rules = loaded.load();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].target_class, TargetClass::InRoot);
}

TEST(GrantLoader, NonPathToolRemainsToolWide) {
    Loaded loaded;
    loaded.write(R"([{"tool":"mcp.github.x","effect":"allow"}])");
    const auto rules = loaded.load();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].target_class, TargetClass::Any);
}

TEST(GrantLoader, FileShapeUnchanged) {
    Loaded loaded;
    loaded.write(R"([{"tool":"shell","command":"git status","match":"literal",)"
                 R"("effect":"allow","id":"g"}])");
    const auto rules = loaded.load();
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].tool, "shell");
    EXPECT_EQ(rules[0].command, "git status");
    EXPECT_TRUE(rules[0].literal);
}

} // namespace

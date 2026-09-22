#include <gtest/gtest.h>

#include <filesystem>

#include "ymh/skills/skill_roots.hpp"

namespace {

using namespace ymh;

TEST(SkillRoots, PinnedOrderAndTiers) {
    const std::filesystem::path home = "/home/u";
    const std::filesystem::path config_root = "/home/u/.config/ymh";

    const std::vector<SkillRoot> roots = skill_roots(home, config_root);
    ASSERT_EQ(roots.size(), 3u);

    EXPECT_EQ(roots[0].path, home / ".ymh" / "skills");
    EXPECT_EQ(roots[0].source, SkillSource::Home);
    EXPECT_EQ(roots[0].trust, SkillTrust::Trusted);

    EXPECT_EQ(roots[1].path, config_root / "skills");
    EXPECT_EQ(roots[1].source, SkillSource::User);
    EXPECT_EQ(roots[1].trust, SkillTrust::Trusted);

    EXPECT_EQ(roots[2].path, home / ".claude" / "skills");
    EXPECT_EQ(roots[2].source, SkillSource::Claude);
    EXPECT_EQ(roots[2].trust, SkillTrust::Trusted);
}

TEST(SkillRoots, EmptyHomeYieldsRelativeCandidates) {
    const std::vector<SkillRoot> roots = skill_roots({}, "/cfg/ymh");
    ASSERT_EQ(roots.size(), 3u);
    EXPECT_FALSE(roots[0].path.is_absolute());
    EXPECT_FALSE(roots[2].path.is_absolute());
    EXPECT_TRUE(roots[1].path.is_absolute());
}

TEST(SkillRoots, CommandRootsUseCommandsLeaf) {
    const std::vector<SkillRoot> roots = command_roots("/home/u", "/cfg/ymh");
    ASSERT_EQ(roots.size(), 3u);
    EXPECT_EQ(roots[0].path, std::filesystem::path{"/home/u/.ymh/commands"});
    EXPECT_EQ(roots[1].path, std::filesystem::path{"/cfg/ymh/commands"});
    EXPECT_EQ(roots[2].path, std::filesystem::path{"/home/u/.claude/commands"});
}

} // namespace

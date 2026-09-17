#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ymh/skills/frontmatter.hpp"
#include "ymh/skills/skill_types.hpp"

namespace {

using namespace ymh;

constexpr std::size_t kMaxFence = 4u * 1024u;
constexpr std::size_t kMaxDescription = 512;

FrontmatterResult parse(std::string_view content, std::string_view name = "git-commit") {
    return parse_skill_file(content, name, kMaxFence, kMaxDescription);
}

TEST(SkillName, Grammar) {
    EXPECT_TRUE(is_valid_skill_name("a"));
    EXPECT_TRUE(is_valid_skill_name("git-commit"));
    EXPECT_TRUE(is_valid_skill_name("a1-b2-c3"));
    EXPECT_TRUE(is_valid_skill_name("release-notes"));
    EXPECT_FALSE(is_valid_skill_name(""));
    EXPECT_FALSE(is_valid_skill_name("Git-commit"));
    EXPECT_FALSE(is_valid_skill_name("-a"));
    EXPECT_FALSE(is_valid_skill_name("a-"));
    EXPECT_FALSE(is_valid_skill_name("a--b"));
    EXPECT_FALSE(is_valid_skill_name("a_b"));
    EXPECT_FALSE(is_valid_skill_name("a.b"));
    EXPECT_FALSE(is_valid_skill_name("a/b"));
    EXPECT_FALSE(is_valid_skill_name("1abc"));
    EXPECT_FALSE(is_valid_skill_name(std::string(65, 'a')));
}

TEST(SkillFrontmatter, ParsesEveryField) {
    const std::string content =
        "---\n"
        "name: git-commit\n"
        "description: Write a conventional commit message.\n"
        "version: 3\n"
        "allowed-tools: [git_status, git_diff]\n"
        "tags: [git, workflow]\n"
        "license: MIT\n"
        "model: deepseek-flash\n"
        "---\n"
        "\n"
        "Do the thing.\n";
    const FrontmatterResult result = parse(content);
    ASSERT_EQ(result.error, "");
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(result.meta.name.value, "git-commit");
    EXPECT_EQ(result.meta.description, "Write a conventional commit message.");
    EXPECT_EQ(result.meta.version, 3u);
    ASSERT_EQ(result.meta.allowed_tools.size(), 2u);
    EXPECT_EQ(result.meta.allowed_tools[0], "git_status");
    EXPECT_EQ(result.meta.allowed_tools[1], "git_diff");
    ASSERT_EQ(result.meta.tags.size(), 2u);
    EXPECT_EQ(result.meta.tags[0], "git");
    EXPECT_EQ(result.meta.license, "MIT");
    EXPECT_EQ(result.meta.model, "deepseek-flash");
    EXPECT_EQ(result.body, "Do the thing.\n");
}

TEST(SkillFrontmatter, DefaultsAndEmptyValues) {
    const std::string content =
        "---\n"
        "name: minimal\n"
        "description: A minimal skill.\n"
        "license:\n"
        "---\n"
        "body\n";
    const FrontmatterResult result = parse(content, "minimal");
    ASSERT_EQ(result.error, "");
    EXPECT_EQ(result.meta.version, 1u);
    EXPECT_TRUE(result.meta.allowed_tools.empty());
    EXPECT_TRUE(result.meta.tags.empty());
    EXPECT_EQ(result.meta.license, "");
    EXPECT_EQ(result.meta.model, "");
}

TEST(SkillFrontmatter, FlowSequenceSpacingAndQuoting) {
    const std::string content =
        "---\n"
        "name: quoting\n"
        "description: 'has: colon, and comma'\n"
        "tags: [\"a,b\", c]\n"
        "---\n";
    const FrontmatterResult result = parse(content, "quoting");
    ASSERT_EQ(result.error, "");
    EXPECT_EQ(result.meta.description, "has: colon, and comma");
    ASSERT_EQ(result.meta.tags.size(), 2u);
    EXPECT_EQ(result.meta.tags[0], "a,b");
    EXPECT_EQ(result.meta.tags[1], "c");
}

TEST(SkillFrontmatter, MissingFences) {
    EXPECT_NE(parse("name: x\ndescription: y\n---\n").error, "");
    EXPECT_NE(parse("---\nname: x\ndescription: y\n").error, "");
    EXPECT_NE(parse("\n---\nname: x\ndescription: y\n---\n").error, "");
}

TEST(SkillFrontmatter, RejectsUnknownDuplicateAndUnsupported) {
    EXPECT_NE(parse("---\nname: a\ndescription: d\nbogus: 1\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\nname: b\ndescription: d\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\ndescription: |\n  text\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\ndescription: d\ntags: &anchor [a]\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\ndescription: d\n# comment\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\ndescription: d\nversion: abc\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\ndescription: d\ntags: not-a-list\n---\n", "a").error, "");
}

TEST(SkillFrontmatter, MissingRequiredFields) {
    EXPECT_NE(parse("---\ndescription: d\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\n---\n", "a").error, "");
    EXPECT_NE(parse("---\nname: a\ndescription:\n---\n", "a").error, "");
}

TEST(SkillFrontmatter, NameMustMatchDirectory) {
    const FrontmatterResult result = parse("---\nname: other\ndescription: d\n---\n", "expected");
    EXPECT_NE(result.error, "");
}

TEST(SkillFrontmatter, InvalidNameRejected) {
    EXPECT_NE(parse("---\nname: Bad_Name\ndescription: d\n---\n", "Bad_Name").error, "");
}

TEST(SkillFrontmatter, CrlfAndBomNormalized) {
    const std::string bom = "\xEF\xBB\xBF";
    const std::string content = bom + "---\r\nname: crlf\r\ndescription: d\r\n---\r\nHello\r\n";
    const FrontmatterResult result = parse(content, "crlf");
    ASSERT_EQ(result.error, "");
    EXPECT_EQ(result.meta.name.value, "crlf");
    EXPECT_EQ(result.meta.description, "d");
    EXPECT_EQ(result.body, "Hello\n");
}

TEST(SkillFrontmatter, FenceRegionBound) {
    std::string content = "---\nname: a\ndescription: d\n";
    content += std::string(200, 'x');
    content += "\n---\n";
    const FrontmatterResult result =
        parse_skill_file(content, "a", 32, kMaxDescription);
    EXPECT_NE(result.error, "");
}

TEST(SkillFrontmatter, DescriptionTruncationIsWarningNotError) {
    const std::string long_description(200, 'a');
    const std::string content =
        "---\nname: long\ndescription: " + long_description + "\n---\nbody\n";
    const FrontmatterResult result =
        parse_skill_file(content, "long", kMaxFence, 64);
    ASSERT_EQ(result.error, "");
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_LE(result.meta.description.size(), 64u);
    EXPECT_EQ(result.body, "body\n");
}

TEST(SkillFrontmatter, DescriptionTruncatesAtWordBoundary) {
    const std::string content =
        "---\nname: words\ndescription: alpha beta gamma delta epsilon\n---\n";
    const FrontmatterResult result = parse_skill_file(content, "words", kMaxFence, 18);
    ASSERT_EQ(result.error, "");
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_EQ(result.meta.description, "alpha beta gamma");
}

TEST(SkillFrontmatter, EmptyBodyIsEmpty) {
    const FrontmatterResult result = parse("---\nname: a\ndescription: d\n---\n", "a");
    ASSERT_EQ(result.error, "");
    EXPECT_TRUE(result.body.empty());
}

TEST(SkillFrontmatter, FuzzNeverThrows) {
    std::uint32_t state = 0x12345678u;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const std::size_t length = next() % 96;
        std::string       content;
        content.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            content.push_back(static_cast<char>(next() & 0xFF));
        }
        const FrontmatterResult result = parse(content, "fuzz");
        if (!result.error.empty()) {
            EXPECT_TRUE(result.meta.name.value.empty() || is_valid_skill_name(result.meta.name.value));
        }
    }
    SUCCEED();
}

} // namespace

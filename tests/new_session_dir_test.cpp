// `/new [dir]` target resolution. The supervisor resolves the command's tail
// against the active workspace root, expands `~`, and enforces the same
// daemon-start clamp: the home tree and sensitive system paths are refused.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "test_support.hpp"

#include "ymh/cli/wiring.hpp"

namespace {

namespace fs = std::filesystem;

using ymh::resolve_session_directory;

class NewSessionDirTest : public ::testing::Test {
protected:
    ymh_test::TempWorkspace workspace;
    fs::path                root;
    fs::path                home;
    fs::path                sub;
    fs::path                spaced;

    void SetUp() override {
        root   = workspace.root / "proj";
        home   = workspace.root / "home";
        sub    = root / "sub";
        spaced = root / "sub dir";
        fs::create_directories(sub);
        fs::create_directories(spaced);
        fs::create_directories(home / "chat");
        std::ofstream{root / "file.txt"} << "x";
    }

    std::optional<std::string> resolve(std::string_view raw, fs::path& out) const {
        return resolve_session_directory(raw, root, home, out);
    }

    static fs::path canonical(const fs::path& path) {
        std::error_code error;
        return fs::weakly_canonical(path, error);
    }
};

TEST_F(NewSessionDirTest, EmptyAndDotUseTheActiveRoot) {
    fs::path out;
    EXPECT_FALSE(resolve("", out).has_value());
    EXPECT_EQ(out, canonical(root));

    EXPECT_FALSE(resolve("   ", out).has_value());
    EXPECT_EQ(out, canonical(root));

    EXPECT_FALSE(resolve(".", out).has_value());
    EXPECT_EQ(out, canonical(root));
}

TEST_F(NewSessionDirTest, RelativePathResolvesUnderTheActiveRoot) {
    fs::path out;
    EXPECT_FALSE(resolve("sub", out).has_value());
    EXPECT_EQ(out, canonical(sub));
}

TEST_F(NewSessionDirTest, AbsolutePathIsAccepted) {
    fs::path out;
    EXPECT_FALSE(resolve(canonical(spaced).string(), out).has_value());
    EXPECT_EQ(out, canonical(spaced));
}

TEST_F(NewSessionDirTest, UnquotedAndQuotedPathsWithSpaces) {
    fs::path out;
    EXPECT_FALSE(resolve("sub dir", out).has_value());
    EXPECT_EQ(out, canonical(spaced));

    EXPECT_FALSE(resolve("\"sub dir\"", out).has_value());
    EXPECT_EQ(out, canonical(spaced));

    EXPECT_FALSE(resolve("'sub dir'", out).has_value());
    EXPECT_EQ(out, canonical(spaced));
}

TEST_F(NewSessionDirTest, TildeExpandsToHome) {
    fs::path out;
    EXPECT_FALSE(resolve("~/chat", out).has_value());
    EXPECT_EQ(out, canonical(home / "chat"));
}

TEST_F(NewSessionDirTest, HomeItselfIsRefused) {
    fs::path out;
    const auto refusal = resolve("~", out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("$HOME"), std::string::npos);

    EXPECT_TRUE(resolve(canonical(home).string(), out).has_value());
}

TEST_F(NewSessionDirTest, AncestorOfHomeIsRefused) {
    // `root/..` is the temp root that contains the fake home: daemon startup
    // refuses a root that is (or contains) `$HOME`, so the resolver must too.
    fs::path out;
    const auto refusal = resolve("..", out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("$HOME"), std::string::npos);
}

TEST_F(NewSessionDirTest, SensitiveSystemPathIsRefused) {
    fs::path out;
    const auto refusal = resolve("/etc", out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("sensitive"), std::string::npos);
}

TEST_F(NewSessionDirTest, FileIsNotADirectory) {
    fs::path out;
    const auto refusal = resolve("file.txt", out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("not a directory"), std::string::npos);
}

TEST_F(NewSessionDirTest, MissingDirectoryIsRefused) {
    fs::path out;
    const auto refusal = resolve("missing", out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("not a directory"), std::string::npos);
}

TEST_F(NewSessionDirTest, UnsupportedTildeFormIsRejected) {
    fs::path out;
    const auto refusal = resolve("~bob/project", out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("~/..."), std::string::npos);
}

TEST_F(NewSessionDirTest, MissingHomeDisablesTildeButNotAbsolutePaths) {
    fs::path out;
    const auto refusal = resolve_session_directory("~/chat", root, {}, out);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_NE(refusal->find("$HOME is not set"), std::string::npos);

    EXPECT_FALSE(resolve_session_directory(canonical(sub).string(), root, {}, out).has_value());
    EXPECT_EQ(out, canonical(sub));
}

} // namespace

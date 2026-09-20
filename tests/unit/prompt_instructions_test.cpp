#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/prompt/instructions.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class ScopedXdg {
public:
    explicit ScopedXdg(std::string value) {
        if (const char* previous = ::getenv("XDG_CONFIG_HOME"); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv("XDG_CONFIG_HOME", value.c_str(), 1);
    }
    ~ScopedXdg() {
        if (previous_.has_value()) {
            ::setenv("XDG_CONFIG_HOME", previous_->c_str(), 1);
        } else {
            ::unsetenv("XDG_CONFIG_HOME");
        }
    }

    ScopedXdg(const ScopedXdg&) = delete;
    ScopedXdg& operator=(const ScopedXdg&) = delete;

private:
    std::optional<std::string> previous_;
};

InstructionFileConfig base_config(std::size_t max_bytes) {
    InstructionFileConfig config;
    config.max_bytes = max_bytes;
    return config;
}

TEST(PromptInstructions, NoFilesLoadsEmpty) {
    TempWorkspace workspace("prompt_instr_none");
    ScopedXdg     xdg(workspace.path().string());
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(4096), env);
    const LoadedInstructions loaded = loader.load();

    EXPECT_TRUE(loaded.files.empty());
    EXPECT_FALSE(loaded.render_message().has_value());
}

TEST(PromptInstructions, GitRootDiscoveryFindsEveryCandidateInOrder) {
    TempWorkspace workspace("prompt_instr_root");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write(".git", "");
    workspace.write("AGENTS.md", "root agents");
    workspace.write("CLAUDE.md", "root claude");
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(4096), env);
    const LoadedInstructions loaded = loader.load();

    ASSERT_EQ(loaded.files.size(), 2u);
    EXPECT_EQ(loaded.files[0].display_path, "AGENTS.md");
    EXPECT_EQ(loaded.files[0].content, "root agents");
    EXPECT_EQ(loaded.files[1].display_path, "CLAUDE.md");
    EXPECT_EQ(loaded.files[1].content, "root claude");
}

TEST(PromptInstructions, LocalCandidatesAppendAfterBaseFiles) {
    TempWorkspace workspace("prompt_instr_local");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write("AGENTS.md", "base");
    workspace.write("AGENTS.local.md", "local");
    LocalEnvironment env(workspace.path());

    InstructionFileConfig config = base_config(4096);
    config.load_local            = true;
    InstructionLoader loader(config, env);
    const LoadedInstructions loaded = loader.load();

    ASSERT_EQ(loaded.files.size(), 2u);
    EXPECT_EQ(loaded.files[0].display_path, "AGENTS.md");
    EXPECT_EQ(loaded.files[1].display_path, "AGENTS.local.md");
}

TEST(PromptInstructions, MessageIsSystemReminderWrapped) {
    TempWorkspace workspace("prompt_instr_wrap");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write("AGENTS.md", "hello");
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(4096), env);
    const LoadedInstructions loaded = loader.load();
    const std::optional<std::string> message = loaded.render_message();

    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->rfind("<system-reminder>", 0), 0u);
    EXPECT_TRUE(message->ends_with("</system-reminder>"));
    EXPECT_NE(message->find("The following workspace instructions may be relevant to your work."),
              std::string::npos);
    EXPECT_NE(message->find("Instructions from: AGENTS.md"), std::string::npos);
    EXPECT_NE(message->find("hello"), std::string::npos);
}

TEST(PromptInstructions, MaxBytesTruncatesMostSpecificFile) {
    TempWorkspace workspace("prompt_instr_cap");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write("AGENTS.md", std::string(200, 'x'));
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(64), env);
    const LoadedInstructions loaded = loader.load();

    ASSERT_EQ(loaded.files.size(), 1u);
    EXPECT_TRUE(loaded.files[0].truncated);
    EXPECT_EQ(loaded.files[0].content.size(), 64u);
    ASSERT_TRUE(loaded.budget_notice.has_value());
    EXPECT_NE(loaded.budget_notice->find("truncated AGENTS.md from 200 to 64 bytes"),
              std::string::npos);
}

TEST(PromptInstructions, DropsBroaderFilesBeforeTruncating) {
    TempWorkspace workspace("prompt_instr_drop");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write("AGENTS.md", std::string(100, 'a'));
    workspace.write("CLAUDE.md", std::string(100, 'b'));
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(128), env);
    const LoadedInstructions loaded = loader.load();

    ASSERT_EQ(loaded.files.size(), 1u);
    EXPECT_EQ(loaded.files[0].display_path, "AGENTS.md");
    EXPECT_FALSE(loaded.files[0].truncated);
    ASSERT_EQ(loaded.omitted.size(), 1u);
    EXPECT_EQ(loaded.omitted[0], "CLAUDE.md");
    ASSERT_TRUE(loaded.budget_notice.has_value());
    EXPECT_NE(loaded.budget_notice->find("omitted CLAUDE.md"), std::string::npos);
}

TEST(PromptInstructions, RefreshForNestedFileUsesAdditionalFraming) {
    TempWorkspace workspace("prompt_instr_nested");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write("sub/AGENTS.md", "nested");
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(4096), env);
    (void)loader.load();
    const LoadedInstructions loaded = loader.refresh_for(workspace.path() / "sub" / "file.txt");

    ASSERT_EQ(loaded.files.size(), 1u);
    EXPECT_EQ(loaded.files[0].display_path, "sub/AGENTS.md");
    EXPECT_EQ(loaded.files[0].scope, "sub");
    const std::optional<std::string> message = loaded.render_message();
    ASSERT_TRUE(message.has_value());
    EXPECT_NE(message->find("Additional instructions from: sub/AGENTS.md"), std::string::npos);
    EXPECT_NE(message->find("These instructions apply to work under `sub`."), std::string::npos);
}

TEST(PromptInstructions, RefreshForChangedAndRemovedFilesEmitsNotices) {
    TempWorkspace workspace("prompt_instr_refresh");
    ScopedXdg     xdg(workspace.path().string());
    workspace.write("AGENTS.md", "first");
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(4096), env);
    (void)loader.load();

    workspace.write("AGENTS.md", "second");
    const LoadedInstructions changed = loader.refresh_for(workspace.path() / "AGENTS.md");
    ASSERT_EQ(changed.notices.size(), 1u);
    EXPECT_NE(changed.notices[0].find("Updated instructions from: AGENTS.md"), std::string::npos);
    EXPECT_NE(changed.notices[0].find("second"), std::string::npos);

    std::filesystem::remove(workspace.path() / "AGENTS.md");
    const LoadedInstructions removed = loader.refresh_for(workspace.path() / "AGENTS.md");
    ASSERT_EQ(removed.notices.size(), 1u);
    EXPECT_NE(removed.notices[0].find("Instructions removed: AGENTS.md"), std::string::npos);
}

TEST(PromptInstructions, MaxBytesIsRequired) {
    TempWorkspace workspace("prompt_instr_required");
    ScopedXdg     xdg(workspace.path().string());
    LocalEnvironment env(workspace.path());

    InstructionLoader loader(base_config(0), env);
    EXPECT_THROW((void)loader.load(), ConfigError);
}

} // namespace

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "ymh/ui/command_registry.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

SessionUiState& attach_session(UiModel& model) {
    model.activeWorkspaceId = WorkspaceId{"ws"};
    SessionUiState& state   = model.ensureSession(SessionId{"session"});
    state.status.model      = "test-model";
    return state;
}

TEST(CommandRegistryTest, QuitIsAnAliasOfExit) {
    CommandRegistry registry = CommandRegistry::builtin();
    const Command*  exit     = registry.find("exit");
    ASSERT_NE(exit, nullptr);
    EXPECT_EQ(registry.find("quit"), exit);

    UiModel         model;
    SessionUiState& state = attach_session(model);
    CommandContext  context{model};
    context.session      = &state;
    bool exited          = false;
    context.request_exit = [&exited] { exited = true; };

    EXPECT_TRUE(registry.dispatch("/quit", context));
    EXPECT_TRUE(exited);
}

TEST(CommandRegistryTest, AliasesAreNotCompleted) {
    const CommandRegistry registry = CommandRegistry::builtin();
    EXPECT_TRUE(registry.complete("qu").empty());
    EXPECT_EQ(registry.complete("exit").size(), 1u);
}

TEST(CommandRegistryTest, HelpRendersAlias) {
    CommandRegistry registry = CommandRegistry::builtin();
    UiModel         model;
    SessionUiState& state = attach_session(model);
    CommandContext  context{model};
    context.session = &state;

    EXPECT_TRUE(registry.dispatch("/help", context));
    std::string rendered;
    for (const ConversationEntry& entry : state.conversation.entries) {
        rendered += entry.text + "\n";
    }
    EXPECT_NE(rendered.find("/exit  quit the supervisor (alias: /quit)"), std::string::npos);
}

TEST(CommandRegistryTest, SkillCommandRemovedAndSkillsDescriptionPinned) {
    const CommandRegistry registry = CommandRegistry::builtin();
    EXPECT_EQ(registry.find("skill"), nullptr);
    const Command* skills = registry.find("skills");
    ASSERT_NE(skills, nullptr);
    EXPECT_EQ(skills->description, "list available skills");
    const std::vector<const Command*> matches = registry.complete("sk");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->name, "skills");
}

TEST(CommandRegistryTest, PlanCommandSelectsMode) {
    CommandRegistry registry = CommandRegistry::builtin();
    UiModel         model;
    SessionUiState& state = attach_session(model);
    CommandContext  context{model};
    context.session = &state;

    std::optional<std::pair<bool, std::string>> selection;
    context.plan_mode = [&selection](bool active, const std::string& message) {
        selection = std::make_pair(active, message);
    };

    EXPECT_TRUE(registry.dispatch("/plan", context));
    ASSERT_TRUE(selection.has_value());
    EXPECT_TRUE(selection->first);
    EXPECT_TRUE(selection->second.empty());

    selection.reset();
    EXPECT_TRUE(registry.dispatch("/plan off", context));
    ASSERT_TRUE(selection.has_value());
    EXPECT_FALSE(selection->first);
    EXPECT_TRUE(selection->second.empty());

    selection.reset();
    EXPECT_TRUE(registry.dispatch("/plan write the tests", context));
    ASSERT_TRUE(selection.has_value());
    EXPECT_TRUE(selection->first);
    EXPECT_EQ(selection->second, "write the tests");
}

TEST(CommandRegistryTest, PlanWithoutSessionAppendsNotice) {
    CommandRegistry registry = CommandRegistry::builtin();
    UiModel         model;
    CommandContext  context{model};

    bool called = false;
    context.plan_mode = [&called](bool, const std::string&) { called = true; };
    EXPECT_TRUE(registry.dispatch("/plan", context));
    EXPECT_FALSE(called);
}

} // namespace

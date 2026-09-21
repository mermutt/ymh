#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
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

// 46-D5 (46-I10): completion matches canonical names and aliases, reporting the
// matched spelling. Rewritten from `AliasesAreNotCompleted` (46-D5/46-S1).
TEST(CommandRegistryTest, UI46_D5_CompleteMatchesAliases) {
    const CommandRegistry registry = CommandRegistry::builtin();
    const std::vector<CompletionCandidate> qu = registry.complete_candidates("qu");
    ASSERT_EQ(qu.size(), 1u);
    ASSERT_NE(qu.front().command, nullptr);
    EXPECT_EQ(qu.front().command->name, "exit");
    EXPECT_EQ(qu.front().spelling, "quit");
    EXPECT_EQ(registry.find("quit"), qu.front().command);

    const std::vector<CompletionCandidate> exi = registry.complete_candidates("exi");
    ASSERT_EQ(exi.size(), 1u);
    EXPECT_EQ(exi.front().command->name, "exit");
    EXPECT_EQ(exi.front().spelling, "exit");
    EXPECT_EQ(registry.complete_candidates("exit").size(), 1u);
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
    EXPECT_NE(rendered.find("/exit(quit) - quit the supervisor"), std::string::npos);
}

TEST(CommandRegistryTest, SkillCommandRemovedAndSkillsDescriptionPinned) {
    const CommandRegistry registry = CommandRegistry::builtin();
    EXPECT_EQ(registry.find("skill"), nullptr);
    const Command* skills = registry.find("skills");
    ASSERT_NE(skills, nullptr);
    EXPECT_EQ(skills->description, "list available skills");
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("sk");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().command->name, "skills");
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

// 45-D8.1 (45-I19): the display name folds aliases into the rendered label.
TEST(CommandRegistryTest, UI45_D8_ExitRowLiteral) {
    const CommandRegistry registry = CommandRegistry::builtin();
    const Command*       exit     = registry.find("exit");
    ASSERT_NE(exit, nullptr);
    EXPECT_EQ(command_display_name(*exit), "exit(quit)");
    EXPECT_EQ(command_display_name(Command{"help", "list slash commands", {}, {}}), "help");
}

// 45-D8.4 (45-I19): `/quit` is an alias, never a separate command row.
TEST(CommandRegistryTest, UI45_D8_QuitNotSeparateRow) {
    const CommandRegistry registry = CommandRegistry::builtin();
    for (const Command& command : registry.commands()) {
        EXPECT_NE(command.name, "quit");
    }
    EXPECT_EQ(registry.find("quit"), registry.find("exit"));
    EXPECT_EQ(registry.complete_candidates("qu").size(), 1u);
}

// 46-D5: a canonical prefix completes to the canonical spelling, while the
// displayed label still folds the alias (45-D8 retained). Rewritten from the
// canonical-only contract (46-D5/46-S1).
TEST(CommandRegistryTest, UI45_D8_TabCompletesCanonicalName) {
    const CommandRegistry registry = CommandRegistry::builtin();
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("exi");
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_NE(matches.front().command, nullptr);
    EXPECT_EQ(matches.front().command->name, "exit");
    EXPECT_EQ(matches.front().spelling, "exit");
    EXPECT_EQ(command_display_name(*matches.front().command), "exit(quit)");
    EXPECT_EQ(registry.complete_candidates("quit").size(), 1u);
}

// 46-D5 (46-I10): a command matched by both its name and an alias yields exactly
// one candidate.
TEST(CommandRegistryTest, UI46_D5_CompleteDedupesByCommand) {
    CommandRegistry registry;
    registry.add(Command{"foo", "foo desc", {}, {"foobar"}});
    registry.add(Command{"fizz", "fizz desc", {}, {"fizzy"}});
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("foo");
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_NE(matches.front().command, nullptr);
    EXPECT_EQ(matches.front().command->name, "foo");
}

// 46-D5 (46-I10): when a command's name and an alias both match, the reported
// spelling is the canonical name.
TEST(CommandRegistryTest, UI46_D5_CompleteCanonicalPreferred) {
    CommandRegistry registry;
    registry.add(Command{"build", "build desc", {}, {"builder"}});
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("build");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().spelling, "build");
}

// 46-D5 (46-I10): candidates preserve registration order, not alphabetical.
TEST(CommandRegistryTest, UI46_D5_CompleteOrderRegistration) {
    CommandRegistry registry;
    registry.add(Command{"charlie", "c", {}, {}});
    registry.add(Command{"alpha", "a", {}, {}});
    registry.add(Command{"bravo", "b", {}, {}});
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("");
    ASSERT_EQ(matches.size(), 3u);
    EXPECT_EQ(matches[0].command->name, "charlie");
    EXPECT_EQ(matches[1].command->name, "alpha");
    EXPECT_EQ(matches[2].command->name, "bravo");
}

// 46-D6 (46-I32): `/q` completes to the `quit` alias spelling of `exit`.
TEST(CommandRegistryTest, UI46_D6_QCompletesToQuit) {
    const CommandRegistry registry = CommandRegistry::builtin();
    const std::vector<CompletionCandidate> matches = registry.complete_candidates("q");
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_NE(matches.front().command, nullptr);
    EXPECT_EQ(matches.front().command->name, "exit");
    EXPECT_EQ(matches.front().spelling, "quit");
}

// 46-D6 (46-I10, compile-level): `CommandRegistry::complete` is retired.
template <typename T, typename = void>
struct HasCompleteMember : std::false_type {};
template <typename T>
struct HasCompleteMember<
    T, std::void_t<decltype(std::declval<const T&>().complete(std::declval<std::string>()))>>
    : std::true_type {};

TEST(CommandRegistryTest, UI46_D6_CompleteRetired) {
    static_assert(!HasCompleteMember<CommandRegistry>::value,
                  "CommandRegistry::complete must be retired (46-D6)");
    SUCCEED();
}

// 45-D8.4 (45-S3): `/help` renders the display name with the " - " separator,
// superseding 25-D12's "(alias: /quit)" suffix.
TEST(CommandRegistryTest, UI45_D8_HelpUsesDisplayName) {
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
    EXPECT_NE(rendered.find("/exit(quit) - quit the supervisor"), std::string::npos);
    EXPECT_EQ(rendered.find("(alias:"), std::string::npos);
}

} // namespace

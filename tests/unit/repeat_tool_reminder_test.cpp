#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/repeat_tool_reminder.hpp"
#include "ymh/llm/stream.hpp"

namespace {

using namespace ymh;

ToolCallAssembled make_call(std::string id, std::string name, nlohmann::json arguments) {
    ToolCallAssembled call;
    call.id        = std::move(id);
    call.name      = std::move(name);
    call.arguments = std::move(arguments);
    return call;
}

TEST(RepeatToolReminderTest, FiresAtEachThresholdOncePerRun) {
    RepeatToolReminder reminder;
    const ToolCallAssembled call = make_call("c", "read", {{"path", "a.txt"}});

    for (std::size_t count = 1; count <= 9; ++count) {
        const std::optional<ContextMessage> message = reminder.observe(call);
        if (count == 3 || count == 5 || count == 8) {
            EXPECT_TRUE(message.has_value()) << "expected a reminder at count " << count;
        } else {
            EXPECT_FALSE(message.has_value()) << "unexpected reminder at count " << count;
        }
    }
}

TEST(RepeatToolReminderTest, CounterResetsOnADifferentCall) {
    RepeatToolReminder reminder;
    const ToolCallAssembled first  = make_call("c1", "read", {{"path", "a.txt"}});
    const ToolCallAssembled second = make_call("c2", "read", {{"path", "b.txt"}});

    EXPECT_FALSE(reminder.observe(first).has_value());
    EXPECT_FALSE(reminder.observe(first).has_value());
    EXPECT_TRUE(reminder.observe(first).has_value());

    EXPECT_FALSE(reminder.observe(second).has_value());
    EXPECT_FALSE(reminder.observe(second).has_value());
    EXPECT_TRUE(reminder.observe(second).has_value());
}

TEST(RepeatToolReminderTest, GentleAtThreeAndDetailedAtFiveAndEight) {
    RepeatToolReminder reminder;
    const ToolCallAssembled call = make_call("c", "shell", {{"cmd", "ls"}});

    const std::optional<ContextMessage> gentle = reminder.observe(call);
    EXPECT_FALSE(gentle.has_value());
    EXPECT_FALSE(reminder.observe(call).has_value());
    const std::optional<ContextMessage> at_three = reminder.observe(call);
    ASSERT_TRUE(at_three.has_value());
    EXPECT_NE(at_three->text.find("You are repeating the exact same tool call"),
              std::string::npos);
    EXPECT_EQ(at_three->text.find("Repeated tool call detected:"), std::string::npos);

    EXPECT_FALSE(reminder.observe(call).has_value());
    const std::optional<ContextMessage> at_five = reminder.observe(call);
    ASSERT_TRUE(at_five.has_value());
    EXPECT_NE(at_five->text.find("Repeated tool call detected:"), std::string::npos);
    EXPECT_NE(at_five->text.find("- tool: `shell`"), std::string::npos);
    EXPECT_NE(at_five->text.find("- consecutive_calls: `5`"), std::string::npos);

    EXPECT_FALSE(reminder.observe(call).has_value());
    EXPECT_FALSE(reminder.observe(call).has_value());
    const std::optional<ContextMessage> at_eight = reminder.observe(call);
    ASSERT_TRUE(at_eight.has_value());
    EXPECT_NE(at_eight->text.find("- consecutive_calls: `8`"), std::string::npos);
}

TEST(RepeatToolReminderTest, IdenticalCanonicalArgumentsRegardlessOfKeyOrder) {
    RepeatToolReminder reminder;
    const ToolCallAssembled first  = make_call("c1", "read", {{"b", 2}, {"a", 1}});
    const ToolCallAssembled second = make_call("c2", "read", {{"a", 1}, {"b", 2}});

    EXPECT_FALSE(reminder.observe(first).has_value());
    EXPECT_FALSE(reminder.observe(second).has_value());
    EXPECT_TRUE(reminder.observe(first).has_value());
}

TEST(RepeatToolReminderTest, DetailedArgumentsArePreviewedAtFiveHundredChars) {
    RepeatToolReminder reminder;
    const nlohmann::json arguments = {{"cmd", std::string(600, 'z')}};
    const std::string    canonical = arguments.dump();
    ASSERT_GT(canonical.size(), 500u);
    const ToolCallAssembled call = make_call("c", "shell", arguments);

    std::optional<ContextMessage> detailed;
    for (int i = 0; i < 5; ++i) {
        detailed = reminder.observe(call);
    }
    ASSERT_TRUE(detailed.has_value());
    EXPECT_NE(detailed->text.find(canonical.substr(0, 500)), std::string::npos);
    EXPECT_EQ(detailed->text.find(canonical), std::string::npos);
}

TEST(RepeatToolReminderTest, MessageIsPluginStampedNotice) {
    RepeatToolReminder reminder;
    const ToolCallAssembled call = make_call("c", "read", {{"path", "a.txt"}});

    std::optional<ContextMessage> message;
    for (int i = 0; i < 3; ++i) {
        message = reminder.observe(call);
    }
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->role, Role::User);
    EXPECT_EQ(message->source.kind, MessageSource::Kind::Plugin);
    EXPECT_EQ(message->source.plugin, "repeat-tool-reminder");
    EXPECT_EQ(message->context.form, ContextForm::Notice);
    EXPECT_FALSE(message->context.summary.empty());
}

} // namespace

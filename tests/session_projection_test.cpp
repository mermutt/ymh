#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>

#include "test_support.hpp"

#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/session/session.hpp"

namespace {

using ymh::ContentBlockKind;
using ymh::EventRecord;
using ymh::EventRange;
using ymh::EventType;
using ymh::Message;
using ymh::Role;
using ymh::Sequence;
using ymh_test::TempWorkspace;
using ymh_test::event_for;
using ymh_test::fresh_uuid;
using ymh_test::root_header;

EventRecord record(Sequence seq, const std::string& session, EventType type,
                   nlohmann::json payload) {
    return EventRecord{seq, event_for(session, type, std::move(payload))};
}

nlohmann::json turn_started() {
    return nlohmann::json{{"turn", 1}, {"origin", "user"}};
}

nlohmann::json step(std::int64_t number) {
    return nlohmann::json{{"turn", 1}, {"step", number}};
}

nlohmann::json assistant_tool_call(const std::string& message_id, const std::string& call_id) {
    return nlohmann::json{
        {"id", message_id},
        {"content", nlohmann::json::array({nlohmann::json{
            {"kind", "tool_use"},
            {"tool_call_id", call_id},
            {"tool_name", "write_file"},
            {"arguments", nlohmann::json::object()},
        }})},
    };
}

nlohmann::json assistant_text(const std::string& message_id, const std::string& text) {
    return nlohmann::json{
        {"id", message_id},
        {"content", nlohmann::json::array({nlohmann::json{
            {"kind", "text"},
            {"text", text},
        }})},
    };
}

nlohmann::json tool_call(const std::string& call_id) {
    return nlohmann::json{
        {"id", call_id},
        {"turn", 1},
        {"step", 1},
        {"name", "write_file"},
        {"arguments", nlohmann::json::object()},
    };
}

nlohmann::json user_message(const std::string& text) {
    return nlohmann::json{
        {"id", fresh_uuid()},
        {"content", nlohmann::json::array({nlohmann::json{
            {"kind", "text"},
            {"text", text},
        }})},
    };
}

nlohmann::json context_message(const std::string& text) {
    return nlohmann::json{{"id", fresh_uuid()}, {"role", "user"}, {"text", text}};
}

bool has_tool_use(const Message& message) {
    return std::any_of(message.content.begin(), message.content.end(),
                       [](const ymh::ContentBlock& block) {
                           return block.kind == ContentBlockKind::ToolUse;
                       });
}

std::size_t find_tool(const std::vector<Message>& messages, const std::string& call_id) {
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (messages[index].role == Role::Tool &&
            messages[index].tool_call_id == call_id) {
            return index;
        }
    }
    return messages.size();
}

} // namespace

TEST(SessionProjection, DanglingToolCallIsClosedBeforeFollowingUserMessages) {
    TempWorkspace workspace;
    const std::string id = fresh_uuid();
    const auto        header = root_header(workspace.root, id);

    EventRange events;
    events.push_back(record(1, id, EventType::TurnStarted, turn_started()));
    events.push_back(record(2, id, EventType::StepStarted, step(1)));
    events.push_back(
        record(3, id, EventType::AssistantMessage, assistant_tool_call("m1", "call-1")));
    events.push_back(record(4, id, EventType::ToolCall, tool_call("call-1")));
    events.push_back(record(5, id, EventType::ContextInjected,
                            context_message("<system-reminder>instructions</system-reminder>")));
    events.push_back(record(6, id, EventType::UserMessage, user_message("continue")));

    const std::vector<Message> messages = ymh::deriveMessages(header, events);
    const std::size_t          tool_index = find_tool(messages, "call-1");
    ASSERT_LT(tool_index, messages.size());

    std::size_t assistant_index = messages.size();
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (messages[index].role == Role::Assistant && has_tool_use(messages[index])) {
            assistant_index = index;
            break;
        }
    }
    ASSERT_LT(assistant_index, messages.size());

    std::size_t first_user = messages.size();
    for (std::size_t index = assistant_index + 1; index < messages.size(); ++index) {
        if (messages[index].role == Role::User) {
            first_user = index;
            break;
        }
    }
    ASSERT_LT(first_user, messages.size());
    EXPECT_EQ(tool_index, assistant_index + 1);
    EXPECT_LT(tool_index, first_user);
    EXPECT_FALSE(messages[tool_index].content.empty());
}

TEST(SessionProjection, DanglingToolCallAtTheLogTailGetsASyntheticResult) {
    TempWorkspace workspace;
    const std::string id = fresh_uuid();
    const auto        header = root_header(workspace.root, id);

    EventRange events;
    events.push_back(record(1, id, EventType::TurnStarted, turn_started()));
    events.push_back(record(2, id, EventType::StepStarted, step(1)));
    events.push_back(
        record(3, id, EventType::AssistantMessage, assistant_tool_call("m1", "call-1")));
    events.push_back(record(4, id, EventType::ToolCall, tool_call("call-1")));

    const std::vector<Message> messages = ymh::deriveMessages(header, events);
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].role, Role::Assistant);
    EXPECT_EQ(messages[1].role, Role::Tool);
    EXPECT_EQ(messages[1].tool_call_id, "call-1");
}

TEST(SessionProjection, StepBoundaryClosesThePreviousOpenCall) {
    TempWorkspace workspace;
    const std::string id = fresh_uuid();
    const auto        header = root_header(workspace.root, id);

    EventRange events;
    events.push_back(record(1, id, EventType::TurnStarted, turn_started()));
    events.push_back(record(2, id, EventType::StepStarted, step(1)));
    events.push_back(
        record(3, id, EventType::AssistantMessage, assistant_tool_call("m1", "call-1")));
    events.push_back(record(4, id, EventType::ToolCall, tool_call("call-1")));
    events.push_back(record(5, id, EventType::StepEnded, step(1)));
    events.push_back(record(6, id, EventType::StepStarted, step(2)));
    events.push_back(record(7, id, EventType::AssistantMessage, assistant_text("m2", "next")));

    const std::vector<Message> messages = ymh::deriveMessages(header, events);
    const std::size_t          tool_index = find_tool(messages, "call-1");
    ASSERT_LT(tool_index, messages.size());

    std::size_t second_assistant = messages.size();
    for (std::size_t index = tool_index + 1; index < messages.size(); ++index) {
        if (messages[index].role == Role::Assistant) {
            second_assistant = index;
            break;
        }
    }
    ASSERT_LT(second_assistant, messages.size());
    EXPECT_LT(tool_index, second_assistant);
}

TEST(SessionProjection, CompactionDoesNotLeaveAnOrphanToolResult) {
    TempWorkspace workspace;
    const std::string id = fresh_uuid();
    const auto        header = root_header(workspace.root, id);

    EventRange events;
    events.push_back(record(1, id, EventType::TurnStarted, turn_started()));
    events.push_back(record(2, id, EventType::StepStarted, step(1)));
    events.push_back(
        record(3, id, EventType::AssistantMessage, assistant_tool_call("m1", "call-1")));
    events.push_back(record(4, id, EventType::ToolCall, tool_call("call-1")));
    events.push_back(record(5, id, EventType::ContextCompaction,
                            nlohmann::json{{"boundary", 3},
                                           {"summary", "compact summary"},
                                           {"token_estimate", 1},
                                           {"model", "test-model"}}));
    events.push_back(record(6, id, EventType::UserMessage, user_message("continue")));

    const std::vector<Message> messages = ymh::deriveMessages(header, events);
    EXPECT_EQ(find_tool(messages, "call-1"), messages.size());
    EXPECT_EQ(messages.front().role, Role::System);
    EXPECT_EQ(messages.back().role, Role::User);
}

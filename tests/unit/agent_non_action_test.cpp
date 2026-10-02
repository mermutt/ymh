#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/session/events.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

std::size_t count_type(const EventRange& events, EventType type) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type == type) {
            ++count;
        }
    }
    return count;
}

std::size_t terminal_count(const EventRange& events) {
    return count_type(events, EventType::TurnEnded) + count_type(events, EventType::TurnCancelled) +
           count_type(events, EventType::TurnFailed);
}

std::size_t nudge_count(const EventRange& events) {
    std::size_t count = 0;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::UserMessage) {
            continue;
        }
        const auto& message = record.event.payload.get<payload::UserMessage>();
        if (message.source.kind == MessageSource::Kind::Plugin &&
            message.source.plugin == "agent-nudge") {
            ++count;
        }
    }
    return count;
}

std::string assistant_text(const EventRange& events) {
    std::string text;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::AssistantMessage) {
            continue;
        }
        for (const ContentBlock& block :
             record.event.payload.get<payload::AssistantMessage>().content) {
            if (block.kind == ContentBlockKind::Text) {
                text += block.text;
            }
        }
    }
    return text;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

TEST(AgentLoopNonActionGuard, BlankThenDoneNudgesOnceAndEnds) {
    AgentEnv env("agent_non_action_done",
                 std::make_unique<FakeLLM>(script_of({text_step(""), text_step("Done.")})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto session_owner  = env.sessionOf(agent);
    const EventRange events = session_owner->events();

    EXPECT_EQ(nudge_count(events), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 2u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
    EXPECT_EQ(terminal_count(events), 1u);
    EXPECT_EQ(assistant_text(events), "Done.");
}

TEST(AgentLoopNonActionGuard, BlankThenBlankBoundedStop) {
    AgentEnv env("agent_non_action_blank",
                 std::make_unique<FakeLLM>(script_of({text_step(""), text_step("")})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto session_owner  = env.sessionOf(agent);
    const EventRange events = session_owner->events();

    EXPECT_EQ(nudge_count(events), 1u);
    // Two settled attempts only: the guard adds exactly one call and cannot loop.
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 2u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(terminal_count(events), 1u);

    bool saw_recoverable_stop = false;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::TurnFailed) {
            continue;
        }
        const auto& failed = record.event.payload.get<payload::TurnFailed>();
        saw_recoverable_stop = failed.code == "StepLimitExceeded";
    }
    EXPECT_TRUE(saw_recoverable_stop);
}

TEST(AgentLoopNonActionGuard, ShortAnswerNoNudge) {
    AgentEnv env("agent_non_action_short",
                 std::make_unique<FakeLLM>(script_of({text_step("Done.")})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto session_owner  = env.sessionOf(agent);
    const EventRange events = session_owner->events();

    EXPECT_EQ(nudge_count(events), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
    EXPECT_EQ(assistant_text(events), "Done.");
}

TEST(AgentLoopNonActionGuard, RepeatedAnswerToDifferentPromptsNudgesThenEscalates) {
    const std::string repeated = "I am a general-purpose assistant and cannot act on that.";
    AgentEnv          env("agent_non_action_repeat",
                          std::make_unique<FakeLLM>(script_of(
                              {text_step(repeated), text_step(repeated), text_step(repeated),
                               text_step(repeated)})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("first request")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);
    ASSERT_EQ(agent.send(user_message("second request")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);
    ASSERT_EQ(agent.send(user_message("third request")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 1u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 4u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 2u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 1u);
    EXPECT_EQ(terminal_count(events), 3u);

    bool saw_recoverable_stop = false;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::TurnFailed) {
            saw_recoverable_stop =
                record.event.payload.get<payload::TurnFailed>().code == "StepLimitExceeded";
        }
    }
    EXPECT_TRUE(saw_recoverable_stop);
}

TEST(AgentLoopNonActionGuard, SamePromptSameAnswerNeverFires) {
    AgentEnv env("agent_non_action_same_prompt",
                 std::make_unique<FakeLLM>(
                     script_of({text_step("Done."), text_step("Done."), text_step("Done.")})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("status?")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("status?")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("status?")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
    EXPECT_EQ(terminal_count(events), 3u);
}

TEST(AgentLoopNonActionGuard, TwoRepeatsThenDistinctAnswerDoesNotFire) {
    const std::string repeated = "I am a general-purpose assistant and cannot act on that.";
    AgentEnv          env("agent_non_action_two_repeats",
                          std::make_unique<FakeLLM>(script_of(
                              {text_step(repeated), text_step(repeated),
                               text_step("Here is the result.")})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("first request")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("second request")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("third request")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 0u);
    EXPECT_EQ(count_type(events, EventType::AssistantMessage), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
}

TEST(AgentLoopNonActionGuard, SamePromptRetryPlusDifferentPromptNeverFires) {
    const std::string repeated = "I am a general-purpose assistant and cannot act on that.";
    AgentEnv          env("agent_non_action_retry",
                          std::make_unique<FakeLLM>(script_of(
                              {text_step(repeated), text_step(repeated), text_step(repeated),
                               text_step(repeated)})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("same prompt")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("same prompt")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("different prompt")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
}

TEST(AgentLoopNonActionGuard, RepetitionMatchesAfterNormalization) {
    AgentEnv env("agent_non_action_norm",
                 std::make_unique<FakeLLM>(script_of(
                     {text_step("I am stuck."), text_step("i AM   stuck."),
                      text_step("  I am stuck.  "), text_step("I Am Stuck.")})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("first request")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("second request")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("third request")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 1u);
}

TEST(AgentLoopNonActionGuard, InjectionTurnDoesNotParticipateInRepetition) {
    const std::string repeated = "I am a general-purpose assistant and cannot act on that.";
    AgentEnv          env("agent_non_action_inject",
                          std::make_unique<FakeLLM>(script_of(
                              {text_step(repeated), text_step(repeated), text_step(repeated),
                               text_step(repeated)})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("first request")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("second request")), InboxResult::Accepted);

    ContextMessage context;
    context.role       = Role::User;
    context.text       = "caller context";
    context.startsTurn = true;
    ASSERT_EQ(agent.inject(context), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
}

TEST(AgentLoopNonActionGuard, VariedRepliesNoToolNoNudge) {
    AgentEnv env("agent_non_action_varied",
                 std::make_unique<FakeLLM>(script_of({text_step("Hello there."),
                                                      text_step("The answer is 4."),
                                                      text_step("Goodbye.")})));
    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;

    ASSERT_EQ(agent.send(user_message("greet me")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("what is 2+2")), InboxResult::Accepted);
    ASSERT_EQ(agent.send(user_message("say bye")), InboxResult::Accepted);
    EXPECT_EQ(agent.state(), AgentState::Idle);

    auto             session_owner = env.sessionOf(agent);
    const EventRange events        = session_owner->events();

    EXPECT_EQ(nudge_count(events), 0u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 3u);
    EXPECT_EQ(count_type(events, EventType::TurnFailed), 0u);
}

} // namespace

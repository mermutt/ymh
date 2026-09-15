#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/subagent.hpp"
#include "ymh/llm/fake_llm.hpp"

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

SessionOptions workspace_options(const TempWorkspace& workspace) {
    SessionOptions options;
    options.cwd           = workspace.path();
    options.serverProfile = "interactive";
    options.model         = "fake-model";
    options.title         = "test";
    return options;
}

TEST(AgentRegistry, CreateIsIdleAndStartsNoTurn) {
    AgentEnv env("registry_create", std::make_unique<FakeLLM>(script_of({text_step("never")})));
    Agent&   agent = env.createAgent();

    EXPECT_EQ(agent.state(), AgentState::Idle);
    EXPECT_EQ(agent.status(), AgentStatus::Idle);
    EXPECT_FALSE(agent.hasPendingWork());
    EXPECT_EQ(count_type(env.sessionOf(agent).events(), EventType::TurnStarted), 0u);
    EXPECT_EQ(env.registry.activeCount(), 0u);
}

TEST(AgentRegistry, ResumeIsIdleAndDoesNotAutoContinue) {
    AgentEnv env("registry_resume", std::make_unique<FakeLLM>(script_of({text_step("first")})));
    Agent&   agent = env.createAgent();

    const SessionId sessionId = agent.session();
    const AgentId   agentId   = agent.id();
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    const std::size_t before = env.sessionOf(agent).events().size();

    env.registry.dispose(agentId);
    const std::expected<AgentId, AgentError> resumed = env.registry.resume(sessionId);
    ASSERT_TRUE(resumed.has_value());

    Agent& resumedAgent = env.registry.get(*resumed);
    EXPECT_EQ(resumedAgent.state(), AgentState::Idle);
    EXPECT_FALSE(resumedAgent.hasPendingWork());
    EXPECT_EQ(env.sessions.session(sessionId).events().size(), before);
}

TEST(AgentRegistry, ResumeIsIdempotent) {
    AgentEnv env("registry_idem", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    Agent&   agent     = env.createAgent();
    const SessionId sessionId = agent.session();

    const std::expected<AgentId, AgentError> first  = env.registry.resume(sessionId);
    const std::expected<AgentId, AgentError> second = env.registry.resume(sessionId);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(env.registry.list().size(), 1u);
}

TEST(AgentRegistry, ResumeUnknownSessionFails) {
    AgentEnv env("registry_unknown", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    const std::expected<AgentId, AgentError> resumed = env.registry.resume(SessionId{"missing"});
    ASSERT_FALSE(resumed.has_value());
    EXPECT_EQ(resumed.error().code, AgentErrorCode::UnknownSession);
}

TEST(AgentRegistry, OpenTurnAtResumeIsNotAutoContinued) {
    AgentEnv env("registry_open_turn", std::make_unique<FakeLLM>(script_of({text_step("never")})));

    const SessionId sessionId = env.sessions.createSession(workspace_options(env.workspace));
    Session&        session   = env.sessions.session(sessionId);
    session.append(payload::TurnStarted{1, payload::TurnOrigin::User});
    session.append(payload::StepStarted{1, 1});
    session.append(payload::UserMessage{"u1", {}});
    session.append(payload::AssistantMessage{"a1", {}, std::nullopt});
    const std::size_t before = session.events().size();

    const std::expected<AgentId, AgentError> resumed = env.registry.resume(sessionId);
    ASSERT_TRUE(resumed.has_value());
    Agent& resumedAgent = env.registry.get(*resumed);
    EXPECT_EQ(resumedAgent.state(), AgentState::Idle);
    EXPECT_EQ(env.sessions.session(sessionId).events().size(), before);
}

TEST(AgentRegistry, SessionManagerAgentDelegatesToRegistry) {
    AgentEnv env("registry_delegate", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    Agent&   agent     = env.createAgent();
    const SessionId sessionId = agent.session();

    EXPECT_EQ(env.sessions.findAgent(sessionId), &agent);
    EXPECT_EQ(&env.sessions.agent(sessionId), &agent);

    env.registry.dispose(agent.id());
    EXPECT_EQ(env.sessions.findAgent(sessionId), nullptr);
    EXPECT_THROW(env.sessions.agent(sessionId), UnknownSession);
}

TEST(AgentRegistry, ActivateSessionIsIdempotentAndNeverFocusGated) {
    AgentEnv env("registry_activate", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    Agent&   first  = env.createAgent();
    Agent&   second = env.createAgent();

    env.registry.activateSession(first.session());
    env.registry.activateSession(first.session());
    env.registry.activateSession(second.session());
    env.registry.activateSession(SessionId{"missing"});

    EXPECT_EQ(count_type(env.sessionOf(first).events(), EventType::TurnStarted), 0u);
    EXPECT_EQ(count_type(env.sessionOf(second).events(), EventType::TurnStarted), 0u);
    EXPECT_EQ(env.registry.activeCount(), 0u);
    EXPECT_EQ(first.state(), AgentState::Idle);
    EXPECT_EQ(second.state(), AgentState::Idle);
}

TEST(AgentRegistry, SubagentSpawnAndFanInRecordEdges) {    AgentEnv env("registry_subagent", std::make_unique<FakeLLM>(script_of({text_step("child done")})));
    Agent&   parent = env.createAgent();

    SubagentRunner runner(env.registry, env.sessions, env.sessionOf(parent),
                          workspace_options(env.workspace));
    std::string    summary;
    const payload::SubagentOutcome outcome = runner.run("do the task", summary);

    EXPECT_EQ(outcome, payload::SubagentOutcome::Completed);
    const EventRange events = env.sessionOf(parent).events();
    EXPECT_EQ(count_type(events, EventType::SubagentSpawned), 1u);
    EXPECT_EQ(count_type(events, EventType::SubagentFanIn), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 0u);
}

} // namespace

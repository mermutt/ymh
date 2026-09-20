#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/commands/command.hpp"
#include "ymh/commands/command_registry.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/goal/goal_command.hpp"
#include "ymh/goal/goal_service.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace {

using namespace ymh;

class FakeAgent final : public Agent {
public:
    FakeAgent(AgentId id, SessionId session) : id_(std::move(id)), session_(std::move(session)) {}

    AgentId id() const override { return id_; }
    SessionId session() const override { return session_; }
    AgentStatus status() const noexcept override { return AgentStatus::Idle; }
    AgentState state() const noexcept override { return AgentState::Idle; }
    bool disposed() const noexcept override { return false; }
    bool hasPendingWork() const noexcept override { return false; }

    InboxResult send(Message) override { return InboxResult::Accepted; }
    InboxResult followup(Message) override { return InboxResult::Accepted; }
    InboxResult steer(Message) override { return InboxResult::Accepted; }
    InboxResult inject(ContextMessage) override { return InboxResult::Accepted; }
    void cancel() override {}
    void dispose() override {}
    void whenIdle(std::function<void()>) override {}

private:
    AgentId   id_;
    SessionId session_;
};

std::optional<ScopeKey> parent_of(const ScopeKey& key) {
    if (key == "agent:a1") {
        return ScopeKey{"session:s1"};
    }
    if (key == "session:s1") {
        return ScopeKey{"preset:p1"};
    }
    if (key == "preset:p1") {
        return ScopeKey{};
    }
    return std::nullopt;
}

CommandSpec spec(std::string name, bool record_input, CommandHandler handler) {
    CommandSpec value;
    value.name         = std::move(name);
    value.description  = "test command";
    value.record_input = record_input;
    value.handler      = std::move(handler);
    return value;
}

CommandHandler constant(std::string text) {
    return [text = std::move(text)](const CommandInput&, Agent&) {
        return CommandOutcome{CommandOutcomeKind::Success, text, std::nullopt};
    };
}

struct CommandFixture {
    EventBus          bus;
    ymh::test::MemorySessionStore store;
    SessionManager    sessions{store, bus};
    SessionId         id;
    std::shared_ptr<FakeAgent> agent;

    CommandFixture() {
        SessionOptions options;
        options.cwd           = std::filesystem::current_path();
        options.serverProfile = "test";
        options.model         = "fake-model";
        options.title         = "command-test";
        id    = sessions.createSession(options);
        agent = std::make_shared<FakeAgent>(AgentId{"a1"}, id);
    }

    std::shared_ptr<Session> session() { return sessions.sessionPtr(id); }

    EventRange tail(std::size_t count) {
        const EventRange events = session()->events();
        if (events.size() <= count) {
            return events;
        }
        return EventRange(events.end() - static_cast<std::ptrdiff_t>(count), events.end());
    }

    CommandRegistry registry(ScopeFor scope_for = {}) {
        return CommandRegistry(sessions, std::move(scope_for), parent_of);
    }
};

TEST(CommandCodec, RunRoundTripsWithAndWithoutArgs) {
    payload::CommandRun run;
    run.command_id = 9;
    run.name       = "goal";
    run.args       = "edit  x";
    run.source     = CommandSource::Agent;

    const nlohmann::json json = run;
    EXPECT_EQ(json.at("command_id").get<std::uint64_t>(), 9u);
    EXPECT_EQ(json.at("name").get<std::string>(), "goal");
    EXPECT_EQ(json.at("args").get<std::string>(), "edit  x");
    EXPECT_EQ(json.at("source").get<std::string>(), "agent");
    EXPECT_EQ(json.get<payload::CommandRun>().source, CommandSource::Agent);

    payload::CommandRun bare;
    bare.command_id = 10;
    bare.name       = "compact";
    const nlohmann::json bare_json = bare;
    EXPECT_FALSE(bare_json.contains("args"));
    EXPECT_EQ(bare_json.at("source").get<std::string>(), "user");
}

TEST(CommandCodec, DoneRoundTripsWithOptionalFields) {
    payload::CommandDone done;
    done.command_id       = 11;
    done.kind             = payload::CommandDoneKind::Error;
    done.text             = "boom";
    done.source_event_seq = 42;

    const nlohmann::json json = done;
    EXPECT_EQ(json.at("kind").get<std::string>(), "error");
    EXPECT_EQ(json.at("source_event_seq").get<std::int64_t>(), 42);
    const payload::CommandDone back = json.get<payload::CommandDone>();
    EXPECT_EQ(back.kind, payload::CommandDoneKind::Error);
    EXPECT_EQ(back.text, "boom");

    payload::CommandDone minimal;
    minimal.command_id = 12;
    const nlohmann::json minimal_json = minimal;
    EXPECT_FALSE(minimal_json.contains("text"));
    EXPECT_FALSE(minimal_json.contains("source_event_seq"));
}

TEST(CommandRegistry, AddRejectsDuplicateWithinOneScope) {
    CommandFixture   fixture;
    CommandRegistry  registry = fixture.registry();
    registry.add(spec("deploy", true, constant("global")), "");
    EXPECT_THROW(registry.add(spec("deploy", true, constant("again")), ""), std::invalid_argument);
}

TEST(CommandRegistry, NearestScopeShadowsFarther) {
    CommandFixture  fixture;
    CommandRegistry registry = fixture.registry();
    registry.add(spec("deploy", true, constant("global")), "");
    registry.add(spec("deploy", true, constant("preset")), "preset:p1");
    registry.add(spec("deploy", true, constant("session")), "session:s1");

    const AgentContext context{AgentId{"a1"}, "agent:a1"};
    const CommandSpec* resolved = registry.resolve(context, "deploy");
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->handler(CommandInput{}, *fixture.agent).text, "session");

    const std::vector<CommandDescriptor> listed = registry.list(context);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed.front().name, "deploy");
}

TEST(CommandRegistry, InvokeAppendsPairedRunAndDone) {
    CommandFixture  fixture;
    CommandRegistry registry = fixture.registry();
    registry.add(spec("deploy", true, constant("deployed")), "");

    const CommandOutcome outcome = registry.invoke(*fixture.agent, "/deploy now", CommandSource::User);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_EQ(outcome.text, "deployed");

    const EventRange events = fixture.tail(2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].event.type, EventType::CommandRun);
    EXPECT_EQ(events[1].event.type, EventType::CommandDone);

    const auto& run  = events[0].event.payload.get<payload::CommandRun>();
    const auto& done = events[1].event.payload.get<payload::CommandDone>();
    EXPECT_EQ(run.name, "deploy");
    EXPECT_EQ(run.args, "now");
    EXPECT_EQ(run.source, CommandSource::User);
    EXPECT_EQ(run.command_id, done.command_id);
    EXPECT_EQ(done.kind, payload::CommandDoneKind::Success);
    EXPECT_EQ(done.text, "deployed");
}

TEST(CommandRegistry, ThrowingHandlerSettlesError) {
    CommandFixture  fixture;
    CommandRegistry registry = fixture.registry();
    registry.add(spec("boom", true, [](const CommandInput&, Agent&) -> CommandOutcome {
                     throw std::runtime_error("kaboom");
                 }),
                 "");

    const CommandOutcome outcome = registry.invoke(*fixture.agent, "/boom", CommandSource::Agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Error);
    EXPECT_EQ(outcome.text, "kaboom");

    const EventRange events = fixture.tail(2);
    ASSERT_EQ(events.size(), 2u);
    const auto& run  = events[0].event.payload.get<payload::CommandRun>();
    const auto& done = events[1].event.payload.get<payload::CommandDone>();
    EXPECT_EQ(run.source, CommandSource::Agent);
    EXPECT_EQ(done.kind, payload::CommandDoneKind::Error);
}

TEST(CommandRegistry, RecordInputFalseOmitsArgs) {
    CommandFixture  fixture;
    CommandRegistry registry = fixture.registry();
    registry.add(spec("quiet", false, constant("ok")), "");

    registry.invoke(*fixture.agent, "/quiet secret", CommandSource::User);
    const EventRange events = fixture.tail(2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_FALSE(events[0].event.payload.get<payload::CommandRun>().args.has_value());
}

TEST(CommandRegistry, UnknownCommandReturnsErrorWithoutEvents) {
    CommandFixture  fixture;
    CommandRegistry registry = fixture.registry();
    const CommandOutcome outcome = registry.invoke(*fixture.agent, "/nope", CommandSource::User);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Error);
    for (const EventRecord& record : fixture.session()->events()) {
        EXPECT_NE(record.event.type, EventType::CommandRun);
        EXPECT_NE(record.event.type, EventType::CommandDone);
    }
}

TEST(CommandRegistry, CommandEventsStayOutOfDeriveMessages) {
    CommandFixture  fixture;
    CommandRegistry registry = fixture.registry();
    registry.add(spec("deploy", true, constant("ok")), "");

    registry.invoke(*fixture.agent, "/deploy", CommandSource::User);
    EXPECT_TRUE(fixture.session()->deriveMessages().empty());
}

TEST(CommandRegistry, AgentScopedShadowingEndToEnd) {
    CommandFixture  fixture;
    CommandRegistry registry =
        fixture.registry([](const Agent&) { return ScopeKey{"agent:a1"}; });
    registry.add(spec("cmd", true, constant("global")), "");
    registry.add(spec("cmd", true, constant("session")), "session:s1");
    registry.add(spec("cmd", true, constant("agent")), "agent:a1");

    const CommandOutcome outcome = registry.invoke(*fixture.agent, "/cmd", CommandSource::Agent);
    EXPECT_EQ(outcome.text, "agent");

    const EventRange events = fixture.tail(2);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].event.payload.get<payload::CommandRun>().name, "cmd");
}

TEST(CommandRegistry, GoalCommandRoundTripsThroughTheLog) {
    CommandFixture  fixture;
    GoalService     goals(fixture.sessions, fixture.bus, 256);
    CommandRegistry registry = fixture.registry();
    registry.add(make_goal_command(goals), "");

    const CommandOutcome outcome = registry.invoke(*fixture.agent, "/goal ship it", CommandSource::User);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);

    const EventRange events = fixture.tail(3);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].event.type, EventType::CommandRun);
    EXPECT_EQ(events[1].event.type, EventType::GoalChange);
    EXPECT_EQ(events[2].event.type, EventType::CommandDone);

    const auto& change = events[1].event.payload.get<payload::GoalChange>();
    ASSERT_TRUE(change.goal.has_value());
    EXPECT_EQ(change.goal->objective, "ship it");
    EXPECT_EQ(events[2].event.payload.get<payload::CommandDone>().kind,
              payload::CommandDoneKind::Success);
}

} // namespace

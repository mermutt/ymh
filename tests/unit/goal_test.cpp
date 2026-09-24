#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/goal/goal.hpp"
#include "ymh/goal/goal_command.hpp"
#include "ymh/goal/goal_service.hpp"
#include "ymh/goal/round_driver.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

#include "support/test_env.hpp"

namespace {

using namespace ymh;

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

class FakeAgent final : public Agent {
public:
    explicit FakeAgent(SessionId session) : session_(std::move(session)) {}

    AgentId id() const override { return AgentId{"agent-1"}; }
    SessionId session() const override { return session_; }
    AgentStatus status() const noexcept override { return AgentStatus::Idle; }
    AgentState state() const noexcept override { return AgentState::Idle; }
    bool disposed() const noexcept override { return false; }
    bool hasPendingWork() const noexcept override { return false; }

    InboxResult send(Message) override { return InboxResult::Accepted; }
    InboxResult followup(Message message) override {
        last = std::move(message);
        ++followups;
        return followup_result;
    }
    InboxResult steer(Message) override { return InboxResult::Accepted; }
    InboxResult inject(ContextMessage) override { return InboxResult::Accepted; }
    void cancel() override {}
    void dispose() override {}
    void whenIdle(std::function<void()>) override {}
    void onSettled(std::function<void()>) override {}

    Message      last;
    int          followups      = 0;
    InboxResult  followup_result = InboxResult::Accepted;

private:
    SessionId session_;
};

struct GoalFixture {
    EventBus          bus;
    ymh::test::MemorySessionStore store;
    SessionManager    sessions{store, bus};
    SessionId         id;
    std::shared_ptr<FakeAgent> agent;
    std::unique_ptr<GoalService> goals;

    explicit GoalFixture(std::uint32_t default_max_rounds = 256) {
        SessionOptions options;
        options.cwd           = std::filesystem::current_path();
        options.serverProfile = "test";
        options.model         = "fake-model";
        options.title         = "goal-test";
        id    = sessions.createSession(options);
        agent = std::make_shared<FakeAgent>(id);
        goals = std::make_unique<GoalService>(sessions, bus, default_max_rounds);
    }

    std::shared_ptr<Session> session() { return sessions.sessionPtr(id); }

    void append_round(GoalId goal_id, std::uint64_t revision, RoundNumber round) {
        payload::UserMessage message;
        message.id      = make_event_id().value;
        message.content = {text_block("round")};
        message.source  = goal_message_source(GoalMessageRef{goal_id, revision, round});
        session()->append(message);
    }

    void publish_turn_end() {
        TypedEvent<payload::TurnEnded> typed;
        typed.id         = make_event_id();
        typed.session_id = id;
        typed.timestamp  = std::chrono::system_clock::now();
        typed.payload    = payload::TurnEnded{0};
        bus.publish(typed);
    }
};

TEST(GoalCodec, SnapshotRoundTrips) {
    GoalSnapshot snapshot;
    snapshot.id              = 7;
    snapshot.revision        = 3;
    snapshot.objective       = "ship the thing";
    snapshot.phase           = GoalPhase::Blocked;
    snapshot.blocked         = GoalBlockReason{"needs-input", "waiting on a decision"};
    snapshot.max_goal_rounds = 12;

    const nlohmann::json json = snapshot;
    EXPECT_EQ(json.at("phase").get<std::string>(), "blocked");
    EXPECT_EQ(json.at("blocked").at("code").get<std::string>(), "needs-input");
    const GoalSnapshot back = json.get<GoalSnapshot>();
    EXPECT_TRUE(back == snapshot);
}

TEST(GoalCodec, PhaseNamesRoundTrip) {
    for (const GoalPhase phase : {GoalPhase::Active, GoalPhase::Paused, GoalPhase::Blocked,
                                  GoalPhase::Complete}) {
        const auto parsed = parse_goal_phase(goal_phase_name(phase));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, phase);
    }
    EXPECT_FALSE(parse_goal_phase("bogus").has_value());
}

TEST(GoalEvent, ChangeRoundTripsAndWireName) {
    payload::GoalChange change;
    change.operation      = payload::GoalOperation::Create;
    change.goal           = GoalSnapshot{GoalRef{2, 1}, "objective", GoalPhase::Active, std::nullopt, 5};
    change.rounds_started = 0;

    const nlohmann::json json = change;
    EXPECT_EQ(json.at("operation").get<std::string>(), "create");
    const payload::GoalChange back = json.get<payload::GoalChange>();
    ASSERT_TRUE(back.goal.has_value());
    EXPECT_TRUE(*back.goal == *change.goal);

    EXPECT_EQ(wire_name(EventType::GoalChange), "goal/change");
    ASSERT_TRUE(parse_event_type("goal/change").has_value());
    EXPECT_EQ(*parse_event_type("goal/change"), EventType::GoalChange);

    bool found = false;
    for (const EventType type : all_event_types()) {
        found = found || type == EventType::GoalChange;
    }
    EXPECT_TRUE(found);
}

TEST(GoalEvent, ClearTombstoneCodec) {
    payload::GoalChange change;
    change.operation = payload::GoalOperation::Clear;
    change.cleared   = GoalRef{4, 6};

    const nlohmann::json json = change;
    EXPECT_FALSE(json.contains("goal"));
    EXPECT_EQ(json.at("cleared").at("id").get<GoalId>(), 4u);
    const payload::GoalChange back = json.get<payload::GoalChange>();
    EXPECT_EQ(back.operation, payload::GoalOperation::Clear);
    ASSERT_TRUE(back.cleared.has_value());
    EXPECT_EQ(back.cleared->revision, 6u);
    EXPECT_FALSE(back.goal.has_value());
}

TEST(GoalProjection, AdvancesOnContiguousRounds) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5});
    EXPECT_EQ(created.rounds_started, 0u);

    fixture.append_round(created.goal.id, created.goal.revision, 1);
    const auto after_one = fixture.goals->get(*fixture.agent);
    ASSERT_TRUE(after_one.has_value());
    EXPECT_EQ(after_one->rounds_started, 1u);

    fixture.append_round(created.goal.id, created.goal.revision, 2);
    const auto after_two = fixture.goals->get(*fixture.agent);
    ASSERT_TRUE(after_two.has_value());
    EXPECT_EQ(after_two->rounds_started, 2u);
}

TEST(GoalProjection, RejectsNonContiguousRound) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5});

    fixture.append_round(created.goal.id, created.goal.revision, 2);
    EXPECT_THROW(static_cast<void>(fixture.goals->get(*fixture.agent)), GoalError);
}

TEST(GoalProjection, RejectsOverCapRound) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 1});

    fixture.append_round(created.goal.id, created.goal.revision, 1);
    ASSERT_EQ(fixture.goals->get(*fixture.agent)->rounds_started, 1u);

    fixture.append_round(created.goal.id, created.goal.revision, 2);
    EXPECT_THROW(static_cast<void>(fixture.goals->get(*fixture.agent)), GoalError);
}

TEST(GoalProjection, RejectsStaleRevisionRound) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5});
    const GoalView edited =
        fixture.goals->edit(*fixture.agent, GoalRef{created.goal.id, created.goal.revision},
                            EditGoalRequest{std::string{"go v2"}, std::nullopt});

    fixture.append_round(edited.goal.id, created.goal.revision, 1);
    EXPECT_THROW(static_cast<void>(fixture.goals->get(*fixture.agent)), GoalError);
}

TEST(GoalService, CreateProducesRevisionOneAndArms) {
    GoalFixture fixture;
    const GoalView view = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});

    EXPECT_EQ(view.goal.revision, 1u);
    EXPECT_EQ(view.goal.phase, GoalPhase::Active);
    EXPECT_EQ(view.goal.max_goal_rounds, 256u);
    EXPECT_EQ(view.activation, GoalActivation::Armed);
    EXPECT_EQ(view.rounds_started, 0u);
}

TEST(GoalService, StaleRevisionRejectedAndAppendsNothing) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});
    const std::size_t before = fixture.session()->events().size();

    EXPECT_THROW(
        static_cast<void>(fixture.goals->edit(
            *fixture.agent, GoalRef{created.goal.id, created.goal.revision + 5},
            EditGoalRequest{std::string{"nope"}, std::nullopt})),
        GoalError);
    EXPECT_EQ(fixture.session()->events().size(), before);
}

TEST(GoalService, MutationsIncrementRevision) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});
    const GoalView edited =
        fixture.goals->edit(*fixture.agent, GoalRef{created.goal.id, created.goal.revision},
                            EditGoalRequest{std::string{"go v2"}, std::nullopt});
    EXPECT_EQ(edited.goal.revision, 2u);
    const GoalView paused =
        fixture.goals->pause(*fixture.agent, GoalRef{edited.goal.id, edited.goal.revision});
    EXPECT_EQ(paused.goal.revision, 3u);
    const GoalView resumed =
        fixture.goals->resume(*fixture.agent, GoalRef{paused.goal.id, paused.goal.revision});
    EXPECT_EQ(resumed.goal.revision, 4u);
}

TEST(GoalService, CreateOnCurrentGoalThrowsAlreadyExists) {
    GoalFixture fixture;
    static_cast<void>(fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt}));
    try {
        static_cast<void>(fixture.goals->create(*fixture.agent, CreateGoalRequest{"again", std::nullopt}));
        FAIL() << "expected AlreadyExists";
    } catch (const GoalError& error) {
        EXPECT_EQ(error.code, GoalErrorCode::AlreadyExists);
    }
}

TEST(GoalService, ResumeOnCompleteThrows) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});
    const GoalView completed =
        fixture.goals->complete(*fixture.agent, GoalRef{created.goal.id, created.goal.revision});
    try {
        static_cast<void>(
            fixture.goals->resume(*fixture.agent, GoalRef{completed.goal.id, completed.goal.revision}));
        FAIL() << "expected InvalidTransition";
    } catch (const GoalError& error) {
        EXPECT_EQ(error.code, GoalErrorCode::InvalidTransition);
    }
}

TEST(GoalService, ResumeOnExhaustedGoalThrows) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 1});
    fixture.append_round(created.goal.id, created.goal.revision, 1);
    const GoalView paused =
        fixture.goals->pause(*fixture.agent, GoalRef{created.goal.id, created.goal.revision});
    try {
        static_cast<void>(
            fixture.goals->resume(*fixture.agent, GoalRef{paused.goal.id, paused.goal.revision}));
        FAIL() << "expected InvalidTransition";
    } catch (const GoalError& error) {
        EXPECT_EQ(error.code, GoalErrorCode::InvalidTransition);
    }
}

TEST(GoalService, BlockReasonValidation) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});
    const GoalRef  ref{created.goal.id, created.goal.revision};

    try {
        static_cast<void>(fixture.goals->block(*fixture.agent, ref, GoalBlockReason{"Bad Code", "why"}));
        FAIL() << "expected InvalidBlockReason";
    } catch (const GoalError& error) {
        EXPECT_EQ(error.code, GoalErrorCode::InvalidBlockReason);
    }
    try {
        static_cast<void>(fixture.goals->block(*fixture.agent, ref, GoalBlockReason{"ok", "   "}));
        FAIL() << "expected InvalidBlockReason";
    } catch (const GoalError& error) {
        EXPECT_EQ(error.code, GoalErrorCode::InvalidBlockReason);
    }
}

TEST(GoalService, ActivationTransitions) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->activation, GoalActivation::Armed);

    const GoalView paused =
        fixture.goals->pause(*fixture.agent, GoalRef{created.goal.id, created.goal.revision});
    EXPECT_EQ(paused.activation, GoalActivation::Disarmed);

    const GoalView resumed =
        fixture.goals->resume(*fixture.agent, GoalRef{paused.goal.id, paused.goal.revision});
    EXPECT_EQ(resumed.activation, GoalActivation::Armed);

    const GoalView blocked =
        fixture.goals->block(*fixture.agent, GoalRef{resumed.goal.id, resumed.goal.revision},
                             GoalBlockReason{"needs-input", "decide"});
    EXPECT_EQ(blocked.activation, GoalActivation::Disarmed);
    EXPECT_EQ(blocked.goal.phase, GoalPhase::Blocked);
    ASSERT_TRUE(blocked.goal.blocked.has_value());
    EXPECT_EQ(blocked.goal.blocked->code, "needs-input");

    const GoalView completed =
        fixture.goals->complete(*fixture.agent, GoalRef{blocked.goal.id, blocked.goal.revision});
    EXPECT_EQ(completed.goal.phase, GoalPhase::Complete);
    EXPECT_EQ(completed.activation, GoalActivation::Disarmed);
}

TEST(GoalService, ClearRemovesCurrentAndRejectsIdReuse) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", std::nullopt});
    fixture.goals->clear(*fixture.agent, GoalRef{created.goal.id, created.goal.revision});
    EXPECT_FALSE(fixture.goals->get(*fixture.agent).has_value());

    const GoalView next = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go again", std::nullopt});
    EXPECT_GT(next.goal.id, created.goal.id);
}

TEST(GoalCommand, CreateAndRender) {
    GoalFixture fixture;
    CommandInput input;
    input.name      = "goal";
    input.raw_input = "do the thing";

    const CommandOutcome outcome = run_goal_command(*fixture.goals, input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_NE(outcome.text.find("Objective: do the thing"), std::string::npos);
    EXPECT_NE(outcome.text.find("Rounds: 0/256"), std::string::npos);
    EXPECT_NE(outcome.text.find("Activation: armed"), std::string::npos);
}

TEST(GoalCommand, EmptyShowsCurrentGoal) {
    GoalFixture fixture;
    CommandInput create;
    create.name      = "goal";
    create.raw_input = "objective one";
    static_cast<void>(run_goal_command(*fixture.goals, create, *fixture.agent));

    CommandInput show;
    show.name = "goal";
    const CommandOutcome outcome = run_goal_command(*fixture.goals, show, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_NE(outcome.text.find("Phase: active"), std::string::npos);
    EXPECT_NE(outcome.text.find("Rounds: 0/256"), std::string::npos);
}

TEST(GoalCommand, EmptyWithNoGoal) {
    GoalFixture fixture;
    CommandInput show;
    show.name = "goal";
    const CommandOutcome outcome = run_goal_command(*fixture.goals, show, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_NE(outcome.text.find("No goal"), std::string::npos);
}

TEST(GoalCommand, BareEditIsInvalid) {
    GoalFixture fixture;
    CommandInput create;
    create.name      = "goal";
    create.raw_input = "objective one";
    static_cast<void>(run_goal_command(*fixture.goals, create, *fixture.agent));

    CommandInput edit;
    edit.name      = "goal";
    edit.raw_input = "edit";
    const CommandOutcome outcome = run_goal_command(*fixture.goals, edit, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Error);
}

TEST(GoalCommand, EditPauseResumeClear) {
    GoalFixture fixture;
    CommandInput input;
    input.name      = "goal";
    input.raw_input = "objective one";
    static_cast<void>(run_goal_command(*fixture.goals, input, *fixture.agent));

    input.raw_input = "edit objective two";
    CommandOutcome outcome = run_goal_command(*fixture.goals, input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->goal.objective, "objective two");

    input.raw_input = "pause";
    outcome         = run_goal_command(*fixture.goals, input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->goal.phase, GoalPhase::Paused);

    input.raw_input = "resume";
    outcome         = run_goal_command(*fixture.goals, input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->goal.phase, GoalPhase::Active);

    input.raw_input = "clear";
    outcome         = run_goal_command(*fixture.goals, input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    EXPECT_FALSE(fixture.goals->get(*fixture.agent).has_value());
}

TEST(GoalCommand, UnknownInputBecomesCreate) {
    GoalFixture fixture;
    CommandInput input;
    input.name      = "goal";
    input.raw_input = "fix the parser bug";
    const CommandOutcome outcome = run_goal_command(*fixture.goals, input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
    ASSERT_TRUE(fixture.goals->get(*fixture.agent).has_value());
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->goal.objective, "fix the parser bug");
}

TEST(GoalCommand, MakeGoalCommandRegistersGrammar) {
    GoalFixture fixture;
    const CommandSpec spec = make_goal_command(*fixture.goals);
    EXPECT_EQ(spec.name, "goal");
    EXPECT_TRUE(spec.record_input);
    ASSERT_TRUE(spec.handler);

    CommandInput input;
    input.name      = "goal";
    input.raw_input = "handled through the spec";
    const CommandOutcome outcome = spec.handler(input, *fixture.agent);
    EXPECT_EQ(outcome.kind, CommandOutcomeKind::Success);
}

TEST(GoalDriver, PlanDecisions) {
    GoalView view;
    view.goal.phase           = GoalPhase::Active;
    view.activation           = GoalActivation::Disarmed;
    view.goal.max_goal_rounds = 3;
    EXPECT_EQ(plan_goal_round(view), GoalRoundDecision::None);

    view.activation = GoalActivation::Armed;
    EXPECT_EQ(plan_goal_round(view), GoalRoundDecision::Enqueue);

    view.rounds_started = 3;
    EXPECT_EQ(plan_goal_round(view), GoalRoundDecision::BlockRoundLimit);

    view.goal.phase = GoalPhase::Complete;
    EXPECT_EQ(plan_goal_round(view), GoalRoundDecision::None);
}

TEST(GoalDriver, RenderPrompt) {
    GoalView view;
    view.goal.objective       = "say \"hi\"";
    view.goal.max_goal_rounds = 4;

    const std::vector<ContentBlock> blocks = render_goal_round_prompt(view, 2);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks.at(0).kind, ContentBlockKind::Text);
    const std::string& text = blocks.at(0).text;
    EXPECT_NE(text.find("<goal_round>"), std::string::npos);
    EXPECT_NE(text.find("Objective: \"say \\\"hi\\\"\""), std::string::npos);
    EXPECT_NE(text.find("Round: 2/4"), std::string::npos);
    EXPECT_NE(text.find("</goal_round>"), std::string::npos);
}

TEST(GoalDriver, EnqueuesExactlyOneGoalRound) {
    GoalFixture fixture;
    static_cast<void>(fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5}));

    GoalRoundDriver driver(*fixture.goals,
                           [agent = fixture.agent](const SessionId& session) -> std::shared_ptr<Agent> {
                               if (session.value == agent->session().value) {
                                   return agent;
                               }
                               return nullptr;
                           });
    driver.start();
    fixture.publish_turn_end();
    driver.pump();

    EXPECT_EQ(fixture.agent->followups, 1);
    ASSERT_TRUE(fixture.agent->last.source.has_value());
    EXPECT_EQ(fixture.agent->last.source->kind, MessageSource::Kind::Goal);
    ASSERT_TRUE(fixture.agent->last.source->goal.has_value());
    EXPECT_EQ(fixture.agent->last.source->goal->round, 1u);
    ASSERT_TRUE(driver.pending(fixture.agent->id()).has_value());
    driver.stop();
}

TEST(GoalDriver, BlocksAtRoundLimit) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 1});
    fixture.append_round(created.goal.id, created.goal.revision, 1);

    GoalRoundDriver driver(*fixture.goals,
                           [agent = fixture.agent](const SessionId& session) -> std::shared_ptr<Agent> {
                               if (session.value == agent->session().value) {
                                   return agent;
                               }
                               return nullptr;
                           });
    driver.start();
    fixture.publish_turn_end();
    driver.pump();

    EXPECT_EQ(fixture.agent->followups, 0);
    const auto goal = fixture.goals->get(*fixture.agent);
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->goal.phase, GoalPhase::Blocked);
    ASSERT_TRUE(goal->goal.blocked.has_value());
    EXPECT_EQ(goal->goal.blocked->code, "round-limit");
    driver.stop();
}

TEST(GoalDriver, FailClosedRejectsStaleRound) {
    GoalFixture fixture;
    const GoalView created = fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5});

    GoalRoundDriver driver(*fixture.goals,
                           [agent = fixture.agent](const SessionId& session) -> std::shared_ptr<Agent> {
                               if (session.value == agent->session().value) {
                                   return agent;
                               }
                               return nullptr;
                           });
    driver.start();
    fixture.publish_turn_end();
    driver.pump();
    const std::optional<GoalMessageRef> queued = driver.pending(fixture.agent->id());
    ASSERT_TRUE(queued.has_value());

    static_cast<void>(fixture.goals->edit(
        *fixture.agent, GoalRef{created.goal.id, created.goal.revision},
        EditGoalRequest{std::string{"changed after queueing"}, std::nullopt}));

    EXPECT_FALSE(driver.admit_round(*fixture.agent, *queued));
    const auto goal = fixture.goals->get(*fixture.agent);
    ASSERT_TRUE(goal.has_value());
    ASSERT_TRUE(goal->goal.blocked.has_value());
    EXPECT_EQ(goal->goal.blocked->code, "prompt-rejected");
    driver.stop();
}

TEST(GoalDriver, AdmitAcceptsUnchangedRound) {
    GoalFixture fixture;
    static_cast<void>(fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5}));

    GoalRoundDriver driver(*fixture.goals,
                           [agent = fixture.agent](const SessionId& session) -> std::shared_ptr<Agent> {
                               if (session.value == agent->session().value) {
                                   return agent;
                               }
                               return nullptr;
                           });
    driver.start();
    fixture.publish_turn_end();
    driver.pump();
    const std::optional<GoalMessageRef> queued = driver.pending(fixture.agent->id());
    ASSERT_TRUE(queued.has_value());

    EXPECT_TRUE(driver.admit_round(*fixture.agent, *queued));
    EXPECT_FALSE(driver.pending(fixture.agent->id()).has_value());
    driver.stop();
}

TEST(GoalDriver, StopDisarmsAgents) {
    GoalFixture fixture;
    static_cast<void>(fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5}));
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->activation, GoalActivation::Armed);

    GoalRoundDriver driver(*fixture.goals,
                           [agent = fixture.agent](const SessionId& session) -> std::shared_ptr<Agent> {
                               if (session.value == agent->session().value) {
                                   return agent;
                               }
                               return nullptr;
                           });
    driver.start();
    fixture.publish_turn_end();
    driver.pump();
    driver.stop();
    EXPECT_EQ(fixture.goals->get(*fixture.agent)->activation, GoalActivation::Disarmed);
}

TEST(GoalProvenance, GoalKindCodec) {
    const MessageSource source = goal_message_source(GoalMessageRef{9, 2, 3});
    const nlohmann::json json  = source;
    EXPECT_EQ(json.at("kind").get<std::string>(), "goal");
    EXPECT_EQ(json.at("goal").at("round").get<RoundNumber>(), 3u);
    const MessageSource back = json.get<MessageSource>();
    EXPECT_TRUE(back == source);
}

TEST(GoalProjection, DeriveMessagesIgnoresGoalChange) {
    GoalFixture fixture;
    const std::size_t before = fixture.session()->deriveMessages().size();
    static_cast<void>(fixture.goals->create(*fixture.agent, CreateGoalRequest{"go", 5}));
    static_cast<void>(fixture.goals->pause(
        *fixture.agent, GoalRef{fixture.goals->get(*fixture.agent)->goal.id, 1}));
    EXPECT_EQ(fixture.session()->deriveMessages().size(), before);
}

TEST(GoalCodec, ActivationIsNeverPersisted) {
    payload::GoalChange change;
    change.operation = payload::GoalOperation::Create;
    change.goal = GoalSnapshot{GoalRef{1, 1}, "objective", GoalPhase::Active, std::nullopt, 5};
    const nlohmann::json json = change;
    EXPECT_FALSE(json.contains("activation"));
    EXPECT_FALSE(json.at("goal").contains("activation"));
}

} // namespace

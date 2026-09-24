// Spec 55 (multi-agent delegation) acceptance tests: the durable settlement
// notice, the two delegation modes, the onSettled await, and the additive
// codec fields (55-A6/A7/A11/A13).

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/agent_test_env.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/model_selection.hpp"
#include "ymh/agent/session_activator.hpp"
#include "ymh/agent/subagent_service.hpp"
#include "ymh/agent/subagent_types.hpp"
#include "ymh/jobs/job_registry.hpp"
#include "ymh/jobs/job_wakeup.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

FakeResponseStep text_step(std::string text, FinishReason finish = FinishReason::Stop) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = finish;
    return step;
}

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

class StubRouteCatalog final : public RouteCatalog {
public:
    std::vector<std::string> routable_endpoints() const override {
        return {std::string{}, "east"};
    }
    bool is_routable_endpoint(std::string_view endpoint) const override {
        return endpoint.empty() || endpoint == "east";
    }
    std::vector<std::string> models_for(std::string_view) const override { return {"fake-model"}; }
    std::vector<std::string> efforts_for(std::string_view, std::string_view) const override {
        return {};
    }
    bool is_catalog_member(std::string_view endpoint, std::string_view model) const override {
        return (endpoint.empty() || endpoint == "east") && model == "fake-model";
    }
};

class SyncActivator final : public SessionActivator {
public:
    void set_service(SubagentService* service) { service_ = service; }

    bool submit(const SessionId& child) override {
        if (service_ == nullptr) {
            return false;
        }
        return service_->activateChild(child);
    }

private:
    SubagentService* service_ = nullptr;
};

struct DelegationEnv {
    explicit DelegationEnv(FakeScript script)
        : env("spec55", std::make_unique<FakeLLM>(std::move(script))) {
        service = std::make_unique<SubagentService>(
            env.registry, env.sessions, nullptr, jobs, wakeup, model_selection, route_catalog,
            env.runtime, activator, env.bus);
        activator.set_service(service.get());
        wakeup.start();
    }

    AgentEnv                    env;
    StubRouteCatalog            route_catalog;
    ModelSelectionController    model_selection{
        [](const SessionId&, payload::SessionModelChanged) {},
        [](const std::string&) -> std::optional<ModelSelection> { return std::nullopt; }};
    JobRegistry                 jobs;
    JobWakeupPolicy             wakeup{jobs, env.bus, JobWakeupConfig{}, env.registry};
    SyncActivator               activator;
    std::unique_ptr<SubagentService> service;
};

std::size_t count_notices(const Session& session) {
    std::size_t count = 0;
    for (const EventRecord& record : session.events()) {
        std::optional<MessageSource> source;
        if (record.event.type == EventType::ContextInjected) {
            source = record.event.payload.get<payload::ContextInjected>().source;
        } else if (record.event.type == EventType::UserMessage) {
            source = record.event.payload.get<payload::UserMessage>().source;
        } else {
            continue;
        }
        if (source.has_value() && source->plugin.rfind("subagent-settlement:", 0) == 0) {
            ++count;
        }
    }
    return count;
}

std::size_t count_fanin(const Session& session, bool notice_expected) {
    std::size_t count = 0;
    for (const EventRecord& record : session.events()) {
        if (record.event.type != EventType::SubagentFanIn) {
            continue;
        }
        if (record.event.payload.get<payload::SubagentFanIn>().notice_expected == notice_expected) {
            ++count;
        }
    }
    return count;
}

TEST(Spec55OnSettled, DefersAndFiresExactlyOnce) {
    DelegationEnv env(script_of({text_step("hi"), text_step("again")}));
    const std::shared_ptr<AgentLoop> agent = env.env.createAgent();

    int calls = 0;
    agent->onSettled([&calls] { ++calls; });
    EXPECT_EQ(calls, 0) << "onSettled must never fire synchronously during registration";

    agent->send(user_message("go"));
    EXPECT_EQ(calls, 1);

    agent->send(user_message("go again"));
    EXPECT_EQ(calls, 1) << "the callback is one-shot";
}

TEST(Spec55OnSettled, NeverFiresAfterDispose) {
    DelegationEnv env(script_of({text_step("hi")}));
    const std::shared_ptr<AgentLoop> agent = env.env.createAgent();
    int calls = 0;
    agent->onSettled([&calls] { ++calls; });
    agent->dispose();
    EXPECT_EQ(calls, 0);
}

TEST(Spec55Delegation, OneShotForegroundAppendsForegroundFanIn) {
    DelegationEnv env(script_of({text_step("child says hi")}));
    const std::shared_ptr<AgentLoop> parent = env.env.createAgent();

    SubagentService::StartRequest request;
    request.parent = parent->session();
    request.label  = "task";
    request.prompt = "do it";

    const StartResult result = env.service->startOneShot(request).get();
    EXPECT_FALSE(result.error.has_value());
    EXPECT_EQ(result.kind, StartResult::Kind::Foreground);
    EXPECT_EQ(result.text, "child says hi");

    const std::shared_ptr<Session> parent_session = env.env.sessionOf(*parent);
    EXPECT_EQ(count_fanin(*parent_session, /*notice_expected=*/false), 1u);
    EXPECT_EQ(count_fanin(*parent_session, /*notice_expected=*/true), 0u);
    EXPECT_EQ(count_notices(*parent_session), 0u);
}

TEST(Spec55Delegation, ContinuableLifecycleProducesOneNoticePerEpoch) {
    DelegationEnv env(script_of({text_step("first"), text_step("second"), text_step("third"),
                                 text_step("fourth")}));
    const std::shared_ptr<AgentLoop> parent = env.env.createAgent();

    SubagentService::StartRequest request;
    request.parent            = parent->session();
    request.label             = "task";
    request.prompt            = "do it";
    request.run_in_background = true;

    const StartResult started = env.service->startContinuable(request).get();
    ASSERT_FALSE(started.error.has_value());
    EXPECT_EQ(started.kind, StartResult::Kind::Continuable);

    const std::shared_ptr<Session> parent_session = env.env.sessionOf(*parent);
    EXPECT_EQ(count_fanin(*parent_session, true), 1u);
    EXPECT_EQ(count_notices(*parent_session), 1u);

    const SendResult sent = env.service->sendMessage(parent->session(), started.child, "more").get();
    EXPECT_FALSE(sent.error.has_value());
    EXPECT_EQ(count_fanin(*parent_session, true), 2u);
    EXPECT_EQ(count_notices(*parent_session), 2u);
}

TEST(Spec55Delegation, ReplayDeliversOneNoticePerUnreportedBackgroundFanIn) {
    DelegationEnv env(script_of({}));
    SessionOptions options;
    options.cwd   = env.env.workspace.path();
    options.model = "fake-model";
    options.title = "orphan";
    const SessionId parent = env.env.sessions.createSession(options);

    payload::SubagentFanIn fan_in;
    fan_in.subagent        = SessionId{"child-1"};
    fan_in.outcome         = payload::SubagentOutcome::Completed;
    fan_in.summary         = "done";
    fan_in.notice_expected = true;
    env.env.sessions.sessionPtr(parent)->append(fan_in);

    // A foreground fan-in must never be replayed (55-D4 / 55-R4-H1).
    payload::SubagentFanIn foreground;
    foreground.subagent        = SessionId{"child-2"};
    foreground.outcome         = payload::SubagentOutcome::Completed;
    foreground.notice_expected = false;
    env.env.sessions.sessionPtr(parent)->append(foreground);

    env.service->replayUnreportedSettlements(parent);
    const std::shared_ptr<Session> session = env.env.sessions.sessionPtr(parent);
    EXPECT_EQ(count_notices(*session), 1u);
    for (const EventRecord& record : session->events()) {
        if (record.event.type != EventType::ContextInjected) {
            continue;
        }
        EXPECT_EQ(record.event.payload.get<payload::ContextInjected>().source.plugin,
                  "subagent-settlement:child-1#1");
    }

    env.service->replayUnreportedSettlements(parent);
    EXPECT_EQ(count_notices(*session), 1u) << "replay is idempotent";
}

TEST(Spec55Codec, SubagentFanInNoticeExpectedRoundTrips) {
    payload::SubagentFanIn fan_in;
    fan_in.subagent        = SessionId{"c"};
    fan_in.outcome         = payload::SubagentOutcome::Failed;
    fan_in.summary         = "boom";
    fan_in.notice_expected = true;

    const nlohmann::json json = fan_in;
    EXPECT_EQ(json.at("notice_expected"), true);
    const payload::SubagentFanIn decoded = json.get<payload::SubagentFanIn>();
    EXPECT_TRUE(decoded.notice_expected);
    EXPECT_EQ(decoded.outcome, payload::SubagentOutcome::Failed);

    payload::SubagentFanIn legacy;
    legacy.subagent = SessionId{"c"};
    legacy.outcome  = payload::SubagentOutcome::Completed;
    const nlohmann::json legacy_json = legacy;
    EXPECT_FALSE(legacy_json.contains("notice_expected"));
    EXPECT_FALSE(legacy_json.get<payload::SubagentFanIn>().notice_expected);
}

TEST(Spec55Codec, TurnEndedFinishReasonOmitsStop) {
    payload::TurnEnded ended;
    ended.turn          = 3;
    ended.finish_reason = FinishReason::Length;
    const nlohmann::json json = ended;
    EXPECT_EQ(json.at("finish_reason"), "length");
    EXPECT_EQ(json.get<payload::TurnEnded>().finish_reason, FinishReason::Length);

    payload::TurnEnded stopped;
    stopped.turn          = 4;
    stopped.finish_reason = FinishReason::Stop;
    EXPECT_FALSE(nlohmann::json(stopped).contains("finish_reason"));
}

TEST(Spec55Codec, MessageSourceSenderRoundTrips) {
    MessageSource source = message_source(MessageSource::Kind::Plugin);
    source.plugin        = "agent-message";
    source.sender        = "sender-session";
    source.context       = ContextFormed{ContextForm::Relay};

    const nlohmann::json json = source;
    EXPECT_EQ(json.at("sender"), "sender-session");
    EXPECT_EQ(json.get<MessageSource>().sender, "sender-session");

    const MessageSource plain = message_source(MessageSource::Kind::Plugin);
    EXPECT_FALSE(nlohmann::json(plain).contains("sender"));
}

TEST(Spec55Delegation, DepthExceededRefusesWithoutCreatingAChild) {
    DelegationEnv env(script_of({}));
    const std::shared_ptr<AgentLoop> parent = env.env.createAgent();

    ChildSpawnRequest request;
    request.options.cwd           = env.env.workspace.path();
    request.options.serverProfile = "interactive";
    request.options.model         = "fake-model";
    request.options.title         = "deep";
    request.options.parentSession = parent->session();
    request.parent_depth          = 3;
    request.max_depth             = 3;

    const std::expected<AgentId, AgentError> created = env.env.registry.createChild(request);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code, AgentErrorCode::DelegationDepthExceeded);
    EXPECT_EQ(env.env.registry.liveSubagentCount(), 0u);
}

TEST(Spec55Delegation, HalfRouteAndUnknownModelAreRejected) {
    DelegationEnv env(script_of({text_step("unused")}));
    const std::shared_ptr<AgentLoop> parent = env.env.createAgent();

    SubagentService::StartRequest half;
    half.parent = parent->session();
    half.label  = "t";
    half.prompt = "p";
    ChildAgentOptions half_options;
    half_options.endpoint = "somewhere";
    half.agent_options    = half_options;
    EXPECT_TRUE(env.service->startOneShot(half).get().error.has_value());

    SubagentService::StartRequest unknown;
    unknown.parent = parent->session();
    unknown.label  = "t";
    unknown.prompt = "p";
    ChildAgentOptions unknown_options;
    unknown_options.endpoint = "east";
    unknown_options.model    = "not-a-model";
    unknown.agent_options    = unknown_options;
    const StartResult result = env.service->startOneShot(unknown).get();
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, AgentErrorCode::ProviderFailed);
}

TEST(Spec55Delegation, TokenLimitStopYieldsTheHeadline) {
    DelegationEnv env(script_of({text_step("partial", FinishReason::Length)}));
    const std::shared_ptr<AgentLoop> parent = env.env.createAgent();

    SubagentService::StartRequest request;
    request.parent = parent->session();
    request.label  = "t";
    request.prompt = "p";

    const StartResult result = env.service->startOneShot(request).get();
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->detail, "subagent run hit its token limit before finishing");
    EXPECT_EQ(result.text, "subagent run hit its token limit before finishing");
}

TEST(Spec55Codec, SessionHeaderRoutePairRoundTrips) {
    SessionHeader header;
    header.id         = make_session_id();
    header.cwd        = std::filesystem::temp_directory_path();
    header.model      = "fake-model";
    header.model_name = "fake-model";
    header.endpoint   = "west";
    header.profile_id = "p1";

    const nlohmann::json json = header;
    EXPECT_EQ(json.at("endpoint"), "west");
    EXPECT_EQ(json.at("profile_id"), "p1");
    const SessionHeader decoded = json.get<SessionHeader>();
    ASSERT_TRUE(decoded.endpoint.has_value());
    EXPECT_EQ(*decoded.endpoint, "west");
    ASSERT_TRUE(decoded.profile_id.has_value());
    EXPECT_EQ(*decoded.profile_id, "p1");
}

} // namespace

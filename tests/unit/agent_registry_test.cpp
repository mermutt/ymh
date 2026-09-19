#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/subagent.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

// A provider whose stream() blocks until released, so a test can hold a turn
// in flight and drive the registry's activation gate concurrently.
class BlockingLLM final : public LLMProvider {
public:
    ProviderId id() const override { return "blocking"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken cancel) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this, &cancel] { return released_ || cancel.cancelled(); });
        }
        if (cancel.cancelled()) {
            LLMResponse cancelled;
            cancelled.outcome = StreamOutcome::Cancelled;
            cancelled.finish  = FinishReason::Other;
            return Task<LLMResponse>{cancelled};
        }
        sink(TextDelta{"hello"});
        sink(Finished{FinishReason::Stop, std::nullopt});
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        response.finish  = FinishReason::Stop;
        return Task<LLMResponse>{response};
    }

    bool waitEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    entered_ = false;
    bool                    released_ = false;
};

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

// A durable twin of `AgentEnv`: same wiring, but the session store is a real
// `SessionPersistence`, so a spawned subagent's row can be read back from
// SQLite (23-D53 durable assertion). `AgentEnv` itself is memory-backed.
struct DurableAgentEnv {
    DurableAgentEnv(const std::string& prefix, std::unique_ptr<LLMProvider> provider)
        : workspace(prefix),
          store(SessionPersistence::open(persistence_config(workspace.path()))),
          sessions(*store, bus),
          env(workspace.path(), SandboxMode::Workspace, ToolConfig{}),
          provider(std::move(provider)),
          runtime(),
          adapter_handle(register_test_adapter(runtime, this->provider)),
          policy(allow_all_permission_config()),
          assembler(tools, ""),
          pool(4),
          registry(make_agent_services(sessions, governor, tools, policy, nullptr, assembler, env,
                                       logger, sink, runtime, pool, estimator,
                                       AgentServices::PermissionResolver{}, nullptr, nullptr),
                   AgentConfig{}) {}

    std::shared_ptr<AgentLoop> createAgent() {
        const std::expected<AgentId, AgentError> created = registry.create(workspace_options(workspace));
        if (!created.has_value()) {
            throw std::runtime_error("create failed: " + created.error().detail);
        }
        return registry.getShared(*created);
    }

    std::shared_ptr<Session> sessionOf(const Agent& agent) {
        return sessions.sessionPtr(agent.session());
    }

    static PersistenceConfig persistence_config(const std::filesystem::path& root) {
        PersistenceConfig config;
        config.db_path   = root / ".ymh" / "sessions.db";
        config.lock_path = root / ".ymh" / "sessions.lock";
        config.boot_id   = BootId{"durable-subagent-test"};
        return config;
    }

    TempWorkspace                       workspace;
    EventBus                            bus;
    std::unique_ptr<SessionPersistence> store;
    SessionManager                      sessions;
    LocalEnvironment                    env;
    ResourceGovernor                    governor;
    OutputRing                          ring{1u << 20};
    RingOutputSink                      sink{ring};
    NullLogger                          logger;
    ToolRegistry                        tools;
    RegistrationKeeper                  keeper{tools};
    std::shared_ptr<LLMProvider>        provider;
    LlmRuntime                          runtime;
    std::optional<AdapterHandle>        adapter_handle;
    RulePermissionPolicy                policy;
    std::unique_ptr<PermissionGate>     gate;
    SessionContextAssembler             assembler;
    DefaultTokenEstimator               estimator;
    LLMPool                             pool;
    std::unique_ptr<ContextCompactor>   context_compactor;
    AgentRegistry                       registry;
};

TEST(AgentRegistry, CreateIsIdleAndStartsNoTurn) {
    AgentEnv env("registry_create", std::make_unique<FakeLLM>(script_of({text_step("never")})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    EXPECT_EQ(agent.state(), AgentState::Idle);
    EXPECT_EQ(agent.status(), AgentStatus::Idle);
    EXPECT_FALSE(agent.hasPendingWork());
    EXPECT_EQ(count_type(env.sessionOf(agent)->events(), EventType::TurnStarted), 0u);
    EXPECT_EQ(env.registry.activeCount(), 0u);
}

TEST(AgentRegistry, ConstructsNoAdapterItself) {
    AgentEnv env("registry_no_adapter",
                 std::make_unique<FakeLLM>(script_of({text_step("never")})));
    LlmRuntime     clean;
    AgentServices services = make_agent_services(
        env.sessions, env.governor, env.tools, env.policy, nullptr, env.assembler, env.env,
        env.logger, env.sink, clean, env.pool, env.estimator, AgentServices::PermissionResolver{},
        nullptr);

    AgentRegistry registry(services, AgentConfig{});
    EXPECT_TRUE(clean.list_providers().empty());

    // A null runtime is equally accepted; the registry still creates no adapter.
    services.runtime = nullptr;
    AgentRegistry null_registry(services, AgentConfig{});
    EXPECT_TRUE(clean.list_providers().empty());
}

TEST(AgentRegistry, ResumeIsIdleAndDoesNotAutoContinue) {
    AgentEnv env("registry_resume", std::make_unique<FakeLLM>(script_of({text_step("first")})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    const SessionId sessionId = agent.session();
    const AgentId   agentId   = agent.id();
    ASSERT_EQ(agent.send(user_message("hi")), InboxResult::Accepted);
    const std::size_t before = env.sessionOf(agent)->events().size();

    env.registry.dispose(agentId);
    const std::expected<AgentId, AgentError> resumed = env.registry.resume(sessionId);
    ASSERT_TRUE(resumed.has_value());

    auto   resumed_owner = env.registry.getShared(*resumed);
    Agent& resumedAgent  = *resumed_owner;
    EXPECT_EQ(resumedAgent.state(), AgentState::Idle);
    EXPECT_FALSE(resumedAgent.hasPendingWork());
    EXPECT_EQ(env.sessions.sessionPtr(sessionId)->events().size(), before);
}

TEST(AgentRegistry, ResumeIsIdempotent) {
    AgentEnv env("registry_idem", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
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
    auto            session_owner = env.sessions.sessionPtr(sessionId);
    Session&        session       = *session_owner;
    session.append(payload::TurnStarted{1, payload::TurnOrigin::User});
    session.append(payload::StepStarted{1, 1});
    session.append(payload::UserMessage{"u1", {}});
    session.append(payload::AssistantMessage{"a1", {}, std::nullopt});
    const std::size_t before = session.events().size();

    const std::expected<AgentId, AgentError> resumed = env.registry.resume(sessionId);
    ASSERT_TRUE(resumed.has_value());
    auto   resumed_owner = env.registry.getShared(*resumed);
    Agent& resumedAgent  = *resumed_owner;
    EXPECT_EQ(resumedAgent.state(), AgentState::Idle);
    EXPECT_EQ(env.sessions.sessionPtr(sessionId)->events().size(), before);
}

TEST(AgentRegistry, ActivateSessionIsIdempotentAndNeverFocusGated) {
    AgentEnv env("registry_activate", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    auto first_owner = env.createAgent();
    Agent& first = *first_owner;
    auto second_owner = env.createAgent();
    Agent& second = *second_owner;

    env.registry.activateSession(first.session());
    env.registry.activateSession(first.session());
    env.registry.activateSession(second.session());
    env.registry.activateSession(SessionId{"missing"});

    EXPECT_EQ(count_type(env.sessionOf(first)->events(), EventType::TurnStarted), 0u);
    EXPECT_EQ(count_type(env.sessionOf(second)->events(), EventType::TurnStarted), 0u);
    EXPECT_EQ(env.registry.activeCount(), 0u);
    EXPECT_EQ(first.state(), AgentState::Idle);
    EXPECT_EQ(second.state(), AgentState::Idle);
}

TEST(AgentRegistry, SubagentSpawnAndFanInRecordEdges) {    AgentEnv env("registry_subagent", std::make_unique<FakeLLM>(script_of({text_step("child done")})));
    auto parent_owner = env.createAgent();
    Agent& parent = *parent_owner;

    auto parent_session = env.sessionOf(parent);
    SubagentRunner runner(env.registry, env.sessions, *parent_session,
                          workspace_options(env.workspace));
    std::string    summary;
    const payload::SubagentOutcome outcome = runner.run("do the task", summary);

    EXPECT_EQ(outcome, payload::SubagentOutcome::Completed);
    const EventRange events = env.sessionOf(parent)->events();
    EXPECT_EQ(count_type(events, EventType::SubagentSpawned), 1u);
    EXPECT_EQ(count_type(events, EventType::SubagentFanIn), 1u);
    EXPECT_EQ(count_type(events, EventType::TurnEnded), 0u);
}

// 23-D53 / SL-U10 + SL-U11: a spawned subagent's durable row is
// kind='subagent' with parent_session set to the spawning session, while a
// default SessionOptions still creates a root with no parent.
TEST(AgentRegistry, SL_U10_U11_SubagentKindAndParentArePersisted) {
    AgentEnv env("registry_subagent_kind",
                 std::make_unique<FakeLLM>(script_of({text_step("child done")})));
    auto parent_owner = env.createAgent();
    Agent& parent = *parent_owner;
    const SessionId parentId = parent.session();

    const std::optional<SessionHeader> parentHeader = env.store.load(parentId);
    ASSERT_TRUE(parentHeader.has_value());
    EXPECT_EQ(parentHeader->kind, SessionKind::Root);
    EXPECT_FALSE(parentHeader->parentSession.has_value());

    auto parent_session = env.sessionOf(parent);
    SubagentRunner runner(env.registry, env.sessions, *parent_session,
                          workspace_options(env.workspace));
    std::string    summary;
    ASSERT_EQ(runner.run("do the task", summary), payload::SubagentOutcome::Completed);

    std::optional<SessionId> childId;
    for (const EventRecord& record : env.sessionOf(parent)->events()) {
        if (record.event.type == EventType::SubagentSpawned) {
            childId = record.event.payload.get<payload::SubagentSpawned>().subagent;
        }
    }
    ASSERT_TRUE(childId.has_value());

    const std::optional<SessionHeader> childHeader = env.store.load(*childId);
    ASSERT_TRUE(childHeader.has_value());
    EXPECT_EQ(childHeader->kind, SessionKind::Subagent);
    ASSERT_TRUE(childHeader->parentSession.has_value());
    EXPECT_EQ(*childHeader->parentSession, parentId);

    // A default-constructed SessionOptions remains a root (no regression).
    const SessionId defaultRoot = env.sessions.createSession(workspace_options(env.workspace));
    const std::optional<SessionHeader> rootHeader = env.store.load(defaultRoot);
    ASSERT_TRUE(rootHeader.has_value());
    EXPECT_EQ(rootHeader->kind, SessionKind::Root);
    EXPECT_FALSE(rootHeader->parentSession.has_value());
}

// 23-D53 / SL-U11 (durable): a SubagentRunner child's SQLite row is
// kind='subagent' with parent_session set, it owns events, and it creates NO
// registry junction (subagents are routed parent-side, not workspace sessions).
TEST(AgentRegistry, SL_U11_SubagentDurableRowKindParentAndZeroJunctions) {
    DurableAgentEnv env("registry_subagent_durable",
                        std::make_unique<FakeLLM>(script_of({text_step("child done")})));
    auto parent_owner = env.createAgent();
    Agent& parent = *parent_owner;
    const SessionId parentId = parent.session();

    auto parent_session = env.sessionOf(parent);
    SubagentRunner runner(env.registry, env.sessions, *parent_session,
                          workspace_options(env.workspace));
    std::string    summary;
    ASSERT_EQ(runner.run("do the task", summary), payload::SubagentOutcome::Completed);

    std::optional<SessionId> childId;
    for (const EventRecord& record : env.sessionOf(parent)->events()) {
        if (record.event.type == EventType::SubagentSpawned) {
            childId = record.event.payload.get<payload::SubagentSpawned>().subagent;
        }
    }
    ASSERT_TRUE(childId.has_value());

    const std::optional<SessionHeader> child = env.store->load(*childId);
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ(child->kind, SessionKind::Subagent);
    ASSERT_TRUE(child->parentSession.has_value());
    EXPECT_EQ(*child->parentSession, parentId);
    EXPECT_GT(env.store->read(*childId).size(), 0u);

    EXPECT_NE(env.store->leaseState(parentId), LeaseState::Absent);
    EXPECT_NE(env.store->leaseState(*childId), LeaseState::Absent);

    // Zero junctions for the subagent (and for the parent, which the runner
    // never registers either).
    const std::filesystem::path state = env.workspace.path() / "state";
    RegistryConfig              registry_config;
    registry_config.db_path         = state / "registry.db";
    registry_config.lock_path       = state / "registry.lock";
    registry_config.workspace_roots = {state / "none"};
    registry_config.bootstrap_depth = 4;
    const std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    const WorkspaceRecord record = registry->registerWorkspace(env.workspace.path(), "durable");
    EXPECT_TRUE(registry->listSessions(record.id).empty());
    EXPECT_FALSE(registry->findSession(record.id, *childId).has_value());
}

// 23-D53 / schema CHECK (src/session/session_persistence.cpp:50): a subagent
// without a parent violates the kind/parent matrix and must be rejected before
// it can reach the store.
TEST(AgentRegistry, SL_U10_SubagentWithoutParentIsRejected) {
    AgentEnv env("registry_subagent_no_parent",
                 std::make_unique<FakeLLM>(script_of({text_step("never")})));
    SessionOptions options = workspace_options(env.workspace);
    options.kind           = SessionKind::Subagent;

    EXPECT_THROW((void)env.sessions.createSession(options), std::invalid_argument);
}

TEST(AgentRegistry, ActivationIsAllowedOnlyWhenIdleOrBlocked) {
    EXPECT_TRUE(activationAllowed(AgentState::Idle));
    EXPECT_TRUE(activationAllowed(AgentState::WaitingForPermission));
    EXPECT_TRUE(activationAllowed(AgentState::WaitingForInput));

    EXPECT_FALSE(activationAllowed(AgentState::Thinking));
    EXPECT_FALSE(activationAllowed(AgentState::CallingTool));
    EXPECT_FALSE(activationAllowed(AgentState::Cancelling));
    EXPECT_FALSE(activationAllowed(AgentState::Error));
}

TEST(AgentRegistry, ActivateSessionDoesNotCancelAnInFlightTurn) {
    auto         owner = std::make_unique<BlockingLLM>();
    BlockingLLM* provider = owner.get();
    AgentEnv     env("registry_no_cancel", std::move(owner));
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;
    const SessionId sessionId = agent.session();

    std::thread turn([&agent] { (void)agent.send(user_message("hi")); });
    ASSERT_TRUE(provider->waitEntered());
    EXPECT_EQ(agent.state(), AgentState::Thinking);
    EXPECT_TRUE(env.registry.hasPendingWork(sessionId));

    env.registry.activateSession(sessionId);

    EXPECT_EQ(agent.state(), AgentState::Thinking);
    EXPECT_TRUE(env.registry.hasPendingWork(sessionId));

    provider->release();
    turn.join();

    EXPECT_EQ(agent.state(), AgentState::Idle);
    EXPECT_EQ(count_type(env.sessionOf(agent)->events(), EventType::TurnEnded), 1u);
    EXPECT_FALSE(env.registry.hasPendingWork(sessionId));
}

TEST(AgentRegistry, HasPendingWorkIsFalseForUnknownSession) {
    AgentEnv env("registry_pending_unknown", std::make_unique<FakeLLM>(script_of({text_step("x")})));
    EXPECT_FALSE(env.registry.hasPendingWork(SessionId{"missing"}));
}

// AL-U19/AL-U13/AL-F22: findShared yields a strong handle that survives
// finalizeAll erasing the map entry; the held agent (and, through its
// session_owner_, its Session) stays alive until the caller drops it.
TEST(AgentRegistry, AL_U19_HeldHandleSurvivesFinalizeAll) {
    AgentEnv env("registry_finalize_hold",
                 std::make_unique<FakeLLM>(script_of({text_step("x")})));
    std::shared_ptr<AgentLoop> held = env.createAgent();
    ASSERT_NE(held, nullptr);
    const SessionId         session      = held->session();
    std::shared_ptr<Session> held_session = env.sessionOf(*held);
    ASSERT_NE(held_session, nullptr);
    std::weak_ptr<AgentLoop> weak_agent   = held;
    std::weak_ptr<Session>   weak_session = held_session;

    env.registry.finalizeAll();

    EXPECT_EQ(env.registry.findShared(session), nullptr);
    EXPECT_EQ(env.registry.getShared(held->id()), nullptr);
    EXPECT_TRUE(held->disposed());
    EXPECT_FALSE(weak_agent.expired());
    EXPECT_FALSE(weak_session.expired());
    EXPECT_THROW((void)env.sessions.sessionPtr(session), UnknownSession);

    held.reset();
    held_session.reset();
    EXPECT_TRUE(weak_agent.expired());
    EXPECT_TRUE(weak_session.expired());
}

// AL15/24-D7: finalizeAll() runs on the coordinator thread while a worker is
// live. The pinned control_mutex_/atomics must make the concurrent inbox
// mutation, state reads, whenIdle registration, and turn_cancel_ access
// race-free. The provider is parked deterministically, so the teardown and the
// contender overlap by construction rather than by chance.
TEST(AgentRegistry, FinalizeAllRacesLiveWorkerWithoutCorruption) {
    auto         owner = std::make_unique<BlockingLLM>();
    BlockingLLM* provider = owner.get();
    AgentEnv     env("registry_finalize_race", std::move(owner));
    std::shared_ptr<AgentLoop> agent = env.createAgent();
    ASSERT_NE(agent, nullptr);

    std::thread worker([&agent] { (void)agent->send(user_message("hi")); });
    ASSERT_TRUE(provider->waitEntered());

    std::thread contender([&agent] {
        for (int i = 0; i < 2000; ++i) {
            (void)agent->send(user_message("x"));
            (void)agent->hasPendingWork();
            (void)agent->state();
            agent->whenIdle([] {});
        }
    });

    env.registry.finalizeAll();
    contender.join();
    provider->release();
    worker.join();

    EXPECT_TRUE(agent->disposed());
    EXPECT_EQ(env.registry.findShared(agent->session()), nullptr);
}

// AL-U15/AL27/AL-F18: concurrent create/dispose/reads from N threads must not
// race the registry maps (run under TSan). `create` is serialized by the test
// because the memory store seam is not itself thread-safe; the registry maps
// still see create concurrent with dispose and with the read paths.
TEST(AgentRegistry, AL_U15_ConcurrentCreateDisposeAndReadsAreRaceFree) {
    AgentEnv env("registry_concurrent", std::make_unique<FakeLLM>(script_of({text_step("x")})));

    std::atomic<bool>  stop{false};
    std::atomic<int>   created{0};
    std::mutex         create_mutex;
    std::vector<std::thread> threads;

    for (int index = 0; index < 3; ++index) {
        threads.emplace_back([&] {
            while (!stop.load()) {
                std::expected<AgentId, AgentError> made;
                {
                    std::lock_guard<std::mutex> lock(create_mutex);
                    SessionOptions              options;
                    options.cwd           = env.workspace.path();
                    options.serverProfile = "interactive";
                    options.model         = "fake-model";
                    options.title         = "concurrent";
                    made                  = env.registry.create(options);
                }
                if (!made.has_value()) {
                    continue;
                }
                ++created;
                const std::shared_ptr<AgentLoop> agent = env.registry.getShared(*made);
                if (agent != nullptr) {
                    (void)env.registry.hasPendingWork(agent->session());
                    (void)env.registry.findShared(agent->session());
                }
                env.registry.dispose(*made);
            }
        });
    }
    threads.emplace_back([&] {
        while (!stop.load()) {
            (void)env.registry.list();
            (void)env.registry.activeCount();
            (void)env.registry.findShared(SessionId{"missing"});
        }
    });

    std::this_thread::sleep_for(300ms);
    stop.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_GT(created.load(), 0);
    EXPECT_TRUE(env.registry.list().empty());
}

} // namespace

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class WorkspaceRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
    }

    static WorkspaceRuntimeOptions options_for(const TempWorkspace& workspace) {
        WorkspaceRuntimeOptions options;
        options.config  = Config{};
        options.root    = workspace.path();
        options.boot_id = BootId{"runtime-test-boot"};
        return options;
    }
};

TEST_F(WorkspaceRuntimeTest, WiresStoreBusToolsLeaseAndRegistry) {
    TempWorkspace workspace("runtime_wiring");
    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(options_for(workspace));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    EXPECT_EQ(runtime.persistence()->schemaVersion(), kSchemaVersion);
    EXPECT_TRUE(runtime.tools().frozen());
    EXPECT_TRUE(runtime.tools().contains(ToolName{"read_file"}));
    EXPECT_TRUE(runtime.tools().contains(ToolName{"write_file"}));
    EXPECT_TRUE(runtime.tools().contains(ToolName{"shell"}));
    EXPECT_TRUE(runtime.tools().contains(ToolName{"git_status"}));
    EXPECT_TRUE(runtime.tools().contains(ToolName{"git_diff"}));
    EXPECT_TRUE(runtime.has_provider());
    EXPECT_EQ(runtime.pool().capacity(), runtime.governor().caps().max_llm_concurrency);
    EXPECT_EQ(runtime.agents().activeCount(), 0u);
    EXPECT_TRUE(runtime.sessions().list().empty());

    SessionOptions session_options;
    session_options.cwd           = workspace.path();
    session_options.serverProfile = "automation";
    session_options.model         = "fake-model";
    session_options.title         = "runtime test";
    const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    const SessionId session = runtime.agents().getShared(*created)->session();

    EXPECT_EQ(runtime.agents().list().size(), 1u);
    ASSERT_NE(runtime.agents().findShared(session), nullptr);
    EXPECT_EQ(runtime.sessions().list().size(), 1u);
    EXPECT_FALSE(runtime.store().read(session).empty());

    EXPECT_TRUE(runtime.acquireLease(session));
    EXPECT_TRUE(runtime.store().isLeaseHolder(session));

    std::atomic<int> published{0};
    Subscription subscription = runtime.bus().subscribe([&](const Event& event) {
        if (event.session_id.value == session.value) {
            published.fetch_add(1);
        }
    });

    const std::size_t before = runtime.store().read(session).size();
    runtime.sessions().sessionPtr(session)->append(payload::TurnStarted{1, payload::TurnOrigin::User});
    EXPECT_EQ(runtime.store().read(session).size(), before + 1);
    EXPECT_EQ(published.load(), 1);
    subscription.unsubscribe();

    EXPECT_TRUE(runtime.releaseLease(session));
    runtime.agents().dispose(*created);
}

TEST_F(WorkspaceRuntimeTest, RegistryRenderReachesAssembledSystemMessage) {
    TempWorkspace workspace("runtime_prompt");
    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(options_for(workspace));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    SessionOptions session_options;
    session_options.cwd           = workspace.path();
    session_options.serverProfile = "automation";
    session_options.model         = "fake-model";
    session_options.title         = "prompt test";
    const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    const SessionId session_id = runtime.agents().getShared(*created)->session();
    auto            session_owner = runtime.sessions().sessionPtr(session_id);
    Session&        session       = *session_owner;

    const std::vector<Message> messages = runtime.context().assemble(session, TurnContext{});
    ASSERT_FALSE(messages.empty());
    ASSERT_EQ(messages.front().role, Role::System);
    std::string text;
    for (const ContentBlock& block : messages.front().content) {
        text += block.text;
    }
    EXPECT_NE(text.find(default_system_prompt()), std::string::npos);
    EXPECT_NE(text.find("You are a coding agent powered by the"), std::string::npos);
    EXPECT_NE(text.find(workspace.path().string()), std::string::npos);
    EXPECT_EQ(text.find("{{model}}"), std::string::npos);
    EXPECT_EQ(text.find("{{cwd}}"), std::string::npos);

    runtime.agents().dispose(*created);
}

TEST_F(WorkspaceRuntimeTest, MissingWorkspaceIsRejected) {
    WorkspaceRuntimeOptions options;
    options.config  = Config{};
    options.root    = std::filesystem::path{"/nonexistent/ymh/runtime/root"};
    options.boot_id = BootId{"runtime-test-boot"};

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_FALSE(runtime_result.has_value());
    EXPECT_EQ(runtime_result.error().code, WorkspaceRuntimeErrorCode::WorkspaceMissing);
}

TEST_F(WorkspaceRuntimeTest, ProviderFactoryThrowIsReported) {
    TempWorkspace workspace("runtime_provider");
    WorkspaceRuntimeOptions options = options_for(workspace);
    options.provider_factory        = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        throw std::runtime_error("boom");
    };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_FALSE(runtime_result.has_value());
    EXPECT_EQ(runtime_result.error().code, WorkspaceRuntimeErrorCode::ProviderSetupFailed);
    EXPECT_EQ(runtime_result.error().detail, "boom");
}

TEST_F(WorkspaceRuntimeTest, StoreFactorySeamSkipsRealDatabase) {
    TempWorkspace workspace("runtime_store_factory");
    WorkspaceRuntimeOptions options = options_for(workspace);
    options.store_factory           = []() -> std::unique_ptr<SessionStore> {
        return std::make_unique<MemorySessionStore>();
    };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    EXPECT_EQ(runtime.persistence(), nullptr);
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / ".ymh" / "sessions.db"));

    SessionOptions session_options;
    session_options.cwd           = workspace.path();
    session_options.serverProfile = "automation";
    session_options.model         = "fake-model";
    const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    const SessionId session = runtime.agents().getShared(*created)->session();
    EXPECT_FALSE(runtime.store().read(session).empty());
}

TEST_F(WorkspaceRuntimeTest, LeaseOpsAreNoOpsWithoutDurableStore) {
    TempWorkspace workspace("runtime_fake_lease");
    WorkspaceRuntimeOptions options = options_for(workspace);
    options.store_factory           = []() -> std::unique_ptr<SessionStore> {
        return std::make_unique<MemorySessionStore>();
    };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;
    EXPECT_FALSE(runtime.hasDurableStore());

    SessionOptions session_options;
    session_options.cwd           = workspace.path();
    session_options.serverProfile = "automation";
    session_options.model         = "fake-model";
    const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    const SessionId session = runtime.agents().getShared(*created)->session();

    EXPECT_TRUE(runtime.acquireLease(session));
    EXPECT_NO_THROW(runtime.renewLeases());
    EXPECT_FALSE(runtime.releaseLease(session));
}

TEST_F(WorkspaceRuntimeTest, NullStoreFactoryIsStoreUnavailable) {
    TempWorkspace workspace("runtime_store_null");
    WorkspaceRuntimeOptions options = options_for(workspace);
    options.store_factory = []() -> std::unique_ptr<SessionStore> { return nullptr; };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_FALSE(runtime_result.has_value());
    EXPECT_EQ(runtime_result.error().code, WorkspaceRuntimeErrorCode::StoreUnavailable);
}

TEST_F(WorkspaceRuntimeTest, ThrowingStoreFactoryIsStoreUnavailable) {
    TempWorkspace workspace("runtime_store_throw");
    WorkspaceRuntimeOptions options = options_for(workspace);
    options.store_factory           = []() -> std::unique_ptr<SessionStore> {
        throw std::runtime_error("no store");
    };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_FALSE(runtime_result.has_value());
    EXPECT_EQ(runtime_result.error().code, WorkspaceRuntimeErrorCode::StoreUnavailable);
}

TEST_F(WorkspaceRuntimeTest, HeldFlockMapsToWorkspaceBusy) {
    TempWorkspace workspace("runtime_busy");
    PersistenceConfig persistence;
    persistence.db_path   = workspace.path() / ".ymh" / "sessions.db";
    persistence.lock_path = workspace.path() / ".ymh" / "sessions.lock";
    persistence.boot_id   = BootId{"runtime-busy-holder"};
    auto holder           = SessionPersistence::open(persistence);
    ASSERT_NE(holder, nullptr);

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(options_for(workspace));
    ASSERT_FALSE(runtime_result.has_value());
    EXPECT_EQ(runtime_result.error().code, WorkspaceRuntimeErrorCode::WorkspaceBusy);
}

TEST_F(WorkspaceRuntimeTest, ExecutorInjectionEnablesPtyAndTerminalTool) {
    TempWorkspace workspace("runtime_pty");
    asio::io_context io;
    AsioExecutor     executor(io);

    WorkspaceRuntimeOptions options = options_for(workspace);
    options.executor                = &executor;

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    EXPECT_TRUE(runtime.environment().pty().available());
    EXPECT_TRUE(runtime.tools().contains(ToolName{"terminal"}));
    EXPECT_TRUE(runtime.tools().contains(ToolName{"shell"}));
}

namespace {

void write_preset_file(const std::filesystem::path& root, const std::string& id,
                       const std::string& body) {
    const std::filesystem::path dir = root / id;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "preset.jsonc", std::ios::binary) << body;
}

Message preset_test_user_message(std::string text) {
    Message      message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

} // namespace

TEST_F(WorkspaceRuntimeTest, PresetScopeReachesAssembledSystemPrompt) {
    TempWorkspace              workspace("runtime_preset_scope");
    const std::filesystem::path presets = workspace.path() / "presets";
    write_preset_file(presets, "minimal",
                      R"JSON({
                        "id": "minimal",
                        "display_name": "Minimal",
                        "rows": [
                          {"id": "persona", "persona": {
                            "prefix": "MINIMAL-PERSONA-SENTINEL",
                            "complete": true,
                            "include_runtime_context": false}},
                          {"id": "shell", "tools": {"allow": ["shell"]}}
                        ]
                      })JSON");
    write_preset_file(presets, "standard",
                      R"JSON({
                        "id": "standard",
                        "display_name": "Standard",
                        "rows": [
                          {"id": "persona", "persona": {
                            "prefix": "STANDARD-PERSONA-SENTINEL"}}
                        ]
                      })JSON");

    WorkspaceRuntimeOptions options = options_for(workspace);
    options.config.presets.root                 = presets;
    options.config.presets.include_shipped_root = false;
    options.config.presets.include_user_root    = false;
    options.config.session.persist_prompt_text  = true;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        for (int index = 0; index < 4; ++index) {
            FakeResponseStep step;
            step.text = "ok";
            script.steps.push_back(step);
        }
        return std::make_unique<FakeLLM>(std::move(script));
    };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    const auto run_and_read_system_prompt = [&](const std::string& preset) -> std::string {
        SessionOptions session_options;
        session_options.cwd           = workspace.path();
        session_options.serverProfile = "automation";
        session_options.model         = "fake-model";
        session_options.agent_preset  = preset;
        const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
        EXPECT_TRUE(created.has_value()) << created.error().detail;
        std::shared_ptr<AgentLoop> agent = runtime.agents().getShared(*created);
        EXPECT_NE(agent, nullptr);

        const std::optional<ScopeKey> scope = runtime.presets().scope_for(agent->id());
        EXPECT_TRUE(scope.has_value()) << "agent on preset '" << preset << "' is not mounted";

        EXPECT_EQ(agent->send(preset_test_user_message("go")), InboxResult::Accepted);

        std::string system_prompt;
        const SessionId session_id = agent->session();
        for (const EventRecord& record : runtime.sessions().sessionPtr(session_id)->events()) {
            if (record.event.type != EventType::LlmRequestHeader) {
                continue;
            }
            const auto& header = record.event.payload.get<payload::LlmRequestHeader>();
            if (header.system_prompt.has_value()) {
                system_prompt = *header.system_prompt;
            }
        }
        return system_prompt;
    };

    const std::string minimal  = run_and_read_system_prompt("minimal");
    const std::string standard = run_and_read_system_prompt("standard");

    ASSERT_FALSE(minimal.empty());
    ASSERT_FALSE(standard.empty());
    EXPECT_NE(minimal, standard);
    EXPECT_NE(minimal.find("MINIMAL-PERSONA-SENTINEL"), std::string::npos) << minimal;
    EXPECT_NE(standard.find("STANDARD-PERSONA-SENTINEL"), std::string::npos) << standard;
}

TEST_F(WorkspaceRuntimeTest, WorkspaceAgentsMdReachesTheSession) {
    TempWorkspace workspace("runtime_instructions");
    workspace.write("AGENTS.md", "INSTRUCTION-SENTINEL-67890\n");

    WorkspaceRuntimeOptions options = options_for(workspace);
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        FakeResponseStep step;
        step.text = "ok";
        script.steps.push_back(step);
        return std::make_unique<FakeLLM>(std::move(script));
    };

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    SessionOptions session_options;
    session_options.cwd           = workspace.path();
    session_options.serverProfile = "automation";
    session_options.model         = "fake-model";
    const std::expected<AgentId, AgentError> created = runtime.agents().create(session_options);
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    std::shared_ptr<AgentLoop> agent = runtime.agents().getShared(*created);
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->send(preset_test_user_message("go")), InboxResult::Accepted);

    bool found_instructions = false;
    bool found_sandbox      = false;
    const SessionId session_id = agent->session();
    for (const EventRecord& record : runtime.sessions().sessionPtr(session_id)->events()) {
        if (record.event.type != EventType::ContextInjected) {
            continue;
        }
        const auto& injected = record.event.payload.get<payload::ContextInjected>();
        if (injected.text.find("INSTRUCTION-SENTINEL-67890") != std::string::npos) {
            found_instructions = true;
        }
        if (injected.text.find("- Sandbox: workspace") != std::string::npos) {
            found_sandbox = true;
        }
    }
    EXPECT_TRUE(found_instructions) << "the workspace AGENTS.md did not reach the session";
    EXPECT_TRUE(found_sandbox) << "the runtime context omitted the sandbox fact";
}

TEST_F(WorkspaceRuntimeTest, WithoutExecutorPtyIsUnavailableAndToolAbsent) {
    TempWorkspace workspace("runtime_no_pty");
    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(options_for(workspace));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    EXPECT_FALSE(runtime.environment().pty().available());
    EXPECT_FALSE(runtime.tools().contains(ToolName{"terminal"}));
}

std::vector<std::string> settlement_plugins(const EventRange& events) {
    std::vector<std::string> plugins;
    for (const EventRecord& record : events) {
        std::optional<MessageSource> source;
        if (record.event.type == EventType::ContextInjected) {
            source = record.event.payload.get<payload::ContextInjected>().source;
        } else if (record.event.type == EventType::UserMessage) {
            source = record.event.payload.get<payload::UserMessage>().source;
        } else {
            continue;
        }
        if (source.has_value() && source->plugin.rfind("subagent-settlement:", 0) == 0) {
            plugins.push_back(source->plugin);
        }
    }
    return plugins;
}

TEST_F(WorkspaceRuntimeTest, DaemonStartReplaysUnreportedSettlementExactlyOnce) {
    TempWorkspace workspace("runtime_settlement_replay");
    SessionId     parent;

    {
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(options_for(workspace));
        ASSERT_TRUE(created.has_value()) << created.error().detail;
        WorkspaceRuntime& runtime = **created;

        SessionOptions session_options;
        session_options.cwd           = workspace.path();
        session_options.serverProfile = "automation";
        session_options.model         = "fake-model";
        session_options.title         = "parent";
        const std::expected<AgentId, AgentError> agent = runtime.agents().create(session_options);
        ASSERT_TRUE(agent.has_value()) << agent.error().detail;
        parent = runtime.agents().getShared(*agent)->session();

        payload::SubagentFanIn fan_in;
        fan_in.subagent        = SessionId{"child-1"};
        fan_in.outcome         = payload::SubagentOutcome::Completed;
        fan_in.summary         = "done";
        fan_in.notice_expected = true;
        runtime.sessions().sessionPtr(parent)->append(fan_in);

        payload::SubagentFanIn foreground;
        foreground.subagent        = SessionId{"child-2"};
        foreground.outcome         = payload::SubagentOutcome::Completed;
        foreground.notice_expected = false;
        runtime.sessions().sessionPtr(parent)->append(foreground);
    }

    {
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(options_for(workspace));
        ASSERT_TRUE(created.has_value()) << created.error().detail;
        EXPECT_EQ(settlement_plugins((**created).store().read(parent)),
                  (std::vector<std::string>{"subagent-settlement:child-1#1"}));
    }

    {
        std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
            make_workspace_runtime(options_for(workspace));
        ASSERT_TRUE(created.has_value()) << created.error().detail;
        EXPECT_EQ(settlement_plugins((**created).store().read(parent)),
                  (std::vector<std::string>{"subagent-settlement:child-1#1"}))
            << "a second daemon start must not re-deliver";
    }
}

} // namespace

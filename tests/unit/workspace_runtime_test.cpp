#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <asio.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/environment.hpp"
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

TEST_F(WorkspaceRuntimeTest, WithoutExecutorPtyIsUnavailableAndToolAbsent) {
    TempWorkspace workspace("runtime_no_pty");
    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(options_for(workspace));
    ASSERT_TRUE(runtime_result.has_value()) << runtime_result.error().detail;
    WorkspaceRuntime& runtime = **runtime_result;

    EXPECT_FALSE(runtime.environment().pty().available());
    EXPECT_FALSE(runtime.tools().contains(ToolName{"terminal"}));
}

} // namespace

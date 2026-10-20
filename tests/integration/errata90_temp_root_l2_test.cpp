#include <gtest/gtest.h>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"

#include <unistd.h>

#include "support/fake_pty.hpp"
#include "support/test_env.hpp"
#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/terminal_tool.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

class PtyIoFixture {
public:
    PtyIoFixture() {
        executor_ = std::make_unique<AsioExecutor>(io_);
        work_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(io_));
        thread_ = std::thread([this] { io_.run(); });
        executor_->bindRunnerThread(thread_.get_id());
    }

    ~PtyIoFixture() {
        work_->reset();
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    PtyIoFixture(const PtyIoFixture&) = delete;
    PtyIoFixture& operator=(const PtyIoFixture&) = delete;

    AsioExecutor& executor() { return *executor_; }

private:
    asio::io_context        io_;
    std::unique_ptr<AsioExecutor> executor_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread             thread_;
};

class TempTarget {
public:
    explicit TempTarget(const std::string& name) {
        path_ = canonical_tmp_root() /
                ("ymh90_l2_" + name + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
    }

    ~TempTarget() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempTarget(const TempTarget&) = delete;
    TempTarget& operator=(const TempTarget&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

payload::ToolCall tool_call(const std::string& name, nlohmann::json arguments) {
    payload::ToolCall call;
    call.id = "call-1";
    call.name = name;
    call.arguments = std::move(arguments);
    return call;
}

ToolResult run_tool(ToolRegistry& registry, ToolEnv& env, const std::string& name,
                    nlohmann::json arguments) {
    return registry.execute(tool_call(name, std::move(arguments)), env.context()).get();
}

std::uint64_t opened_id(const ToolResult& result) {
    const auto parsed = nlohmann::json::parse(result.output);
    return std::stoull(parsed.at("terminal_id").get<std::string>());
}

void wait_running(PtySession& session) {
    for (int attempt = 0; attempt < 500 && session.state() == PtyState::Starting; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
}

} // namespace

TEST(Errata90TempRootL2, TmpWriteSurvivesWorkspaceReadOnly) {
    TempWorkspace workspace("p90_l2_tmp_write", test_workspace_base());
    TempTarget    target("write");
    ToolEnv       env("p90_l2_tmp_write_tool", SandboxMode::Workspace, {}, {},
                        nullptr, test_workspace_base());
    if (env.env.confinementStatus() != ConfinementStatus::Enforced) {
        GTEST_SKIP() << "Landlock unavailable";
    }
    ToolRegistry         registry;
    RegistrationKeeper   keeper{registry};
    keeper.add(make_shell_tool());
    registry.freeze();

    const std::filesystem::path file = target.path() / "x";
    const ToolResult result =
        run_tool(registry, env, "shell", {{"command", "touch " + file.string()}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok)
        << "out=" << result.output << " err=" << result.error.value_or("");
    EXPECT_TRUE(std::filesystem::exists(file));
}

TEST(Errata90TempRootL2, ReadOnlyWorkspaceStaysUnwritable) {
    TempWorkspace workspace("p90_l2_read_only_ws", test_workspace_base());
    ToolEnv       env("p90_l2_read_only_tool", SandboxMode::ReadOnly, {}, {},
                        nullptr, test_workspace_base());
    if (env.env.confinementStatus() != ConfinementStatus::Enforced) {
        GTEST_SKIP() << "Landlock unavailable";
    }
    ToolRegistry         registry;
    RegistrationKeeper   keeper{registry};
    keeper.add(make_shell_tool());
    registry.freeze();

    const std::filesystem::path file = workspace.path() / "hidden";
    const ToolResult result =
        run_tool(registry, env, "shell", {{"command", "touch " + file.string()}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST(Errata90TempRootL2, CrossDirectoryRenameInsideTmpUsesRefer) {
    TempWorkspace workspace("p90_l2_refer", test_workspace_base());
    TempTarget    target("refer");
    const std::filesystem::path sub = target.path() / "sub";
    ASSERT_TRUE(std::filesystem::create_directories(sub));
    const std::filesystem::path source = target.path() / "a";
    { std::ofstream(source) << "x"; }
    ToolEnv env("p90_l2_refer_tool", SandboxMode::Workspace, {}, {}, nullptr,
                test_workspace_base());
    if (env.env.confinementStatus() != ConfinementStatus::Enforced) {
        GTEST_SKIP() << "Landlock unavailable";
    }
    ToolRegistry         registry;
    RegistrationKeeper   keeper{registry};
    keeper.add(make_shell_tool());
    registry.freeze();

    const ToolResult result = run_tool(
        registry, env, "shell",
        {{"command", "mv " + source.string() + " " + (sub / "b").string()}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_TRUE(std::filesystem::exists(sub / "b"));
}

TEST(Errata90TempRootL2, TerminalOpensAtTempCwd) {
    PtyIoFixture      fixture;
    TempWorkspace workspace("p90_l2_terminal", test_workspace_base());
    TempTarget        target("terminal");
    ResourceGovernor  governor;
    NoopPtyEventSink  sink;
    LocalPtyService   pty(fixture.executor(), governor, sink);
    ToolEnv           env("p90_l2_terminal_tool", SandboxMode::Workspace, {}, {}, &pty,
                          test_workspace_base());
    ToolRegistry      registry;
    RegistrationKeeper keeper{registry};
    keeper.add(make_terminal_tool());
    registry.freeze();

    const ToolResult result =
        run_tool(registry, env, "terminal",
                 {{"action", "open"},
                  {"cwd", target.path().string()},
                  {"command", "touch x"}});
    ASSERT_EQ(result.outcome, payload::ToolOutcome::Ok);
    PtySession* session = pty.find(PtySessionId{opened_id(result)});
    ASSERT_NE(session, nullptr);
    wait_running(*session);
    (void)session->wait(5s, std::nullopt, CancellationToken{}).get();
    EXPECT_TRUE(std::filesystem::exists(target.path() / "x"));
}

TEST(Errata90TempRootL2, TerminalRejectsMissingTempCwd) {
    PtyIoFixture      fixture;
    TempWorkspace workspace("p90_l2_terminal_missing", test_workspace_base());
    TempTarget        target("terminal_missing");
    ResourceGovernor  governor;
    NoopPtyEventSink  sink;
    LocalPtyService   pty(fixture.executor(), governor, sink);
    ToolEnv           env("p90_l2_terminal_missing_tool", SandboxMode::Workspace, {},
                         {}, &pty, test_workspace_base());
    ToolRegistry      registry;
    RegistrationKeeper keeper{registry};
    keeper.add(make_terminal_tool());
    registry.freeze();

    const ToolResult result =
        run_tool(registry, env, "terminal",
                 {{"action", "open"},
                  {"cwd", (target.path() / "missing").string()},
                  {"command", "touch x"}});
    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
}

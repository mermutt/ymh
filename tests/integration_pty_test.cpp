#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <asio.hpp>

#include <cerrno>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/execution/resource_governor.hpp"

namespace {

using namespace ymh;
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

PtyRequest shell_request(const std::filesystem::path& cwd, const std::string& script,
                         int rows = 24, int cols = 80) {
    PtyRequest request;
    request.executable = "/bin/sh";
    request.argv = {"/bin/sh", "-c", script};
    request.cwd = cwd;
    request.rows = rows;
    request.cols = cols;
    request.session = SessionId{"pty-integration"};
    return request;
}

void wait_running(PtySession& session) {
    for (int attempt = 0; attempt < 500 && session.state() == PtyState::Starting; ++attempt) {
        std::this_thread::sleep_for(5ms);
    }
}

std::string read_until_eof(PtySession& session) {
    std::string output;
    for (int attempt = 0; attempt < 400; ++attempt) {
        const PtyRead read =
            session.read(64 * 1024, 250ms, std::nullopt, CancellationToken{}).get();
        output += read.data;
        if (read.eof) {
            break;
        }
    }
    return output;
}

TEST(PtyIntegration, BasicIoAndExitStatus) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_basic");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "echo hello; exit 3"),
                            CancellationToken{})
                       .get();
    wait_running(*session);
    const std::string output = read_until_eof(*session);
    EXPECT_NE(output.find("hello"), std::string::npos);

    const PtyExit exit = session->wait(2s, std::nullopt, CancellationToken{}).get();
    EXPECT_FALSE(exit.signalled);
    EXPECT_EQ(exit.exit_code, 3);
}

TEST(PtyIntegration, WriteReadRoundTripThroughCat) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_cat");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "cat"), CancellationToken{}).get();
    wait_running(*session);
    session->write("round-trip\n");

    std::string output;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const PtyRead read = session->read(64 * 1024, 100ms, std::nullopt, CancellationToken{}).get();
        output += read.data;
        if (output.find("round-trip") != std::string::npos) {
            break;
        }
    }
    EXPECT_NE(output.find("round-trip"), std::string::npos);

    session->kill();
    (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
}

TEST(PtyIntegration, InitialGeometryIsApplied) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_geometry");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "stty size", 30, 100),
                            CancellationToken{})
                       .get();
    wait_running(*session);
    const std::string output = read_until_eof(*session);
    EXPECT_NE(output.find("30 100"), std::string::npos);
}

TEST(PtyIntegration, TerminateDeliversSighupAndReaps) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_term");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "sleep 100"), CancellationToken{})
                       .get();
    wait_running(*session);
    session->terminate();
    const PtyExit exit = session->wait(5s, std::nullopt, CancellationToken{}).get();
    EXPECT_TRUE(exit.signalled);
    EXPECT_EQ(exit.signal, SIGHUP);
}

TEST(PtyIntegration, KillForcePathSkipsTheGrace) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_kill");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session =
        pty.open(shell_request(workspace.path(), "trap '' HUP; echo ready; sleep 100"),
                 CancellationToken{})
            .get();
    wait_running(*session);

    std::string output;
    for (int attempt = 0; attempt < 200; ++attempt) {
        const PtyRead read = session->read(64 * 1024, 50ms, std::nullopt, CancellationToken{}).get();
        output += read.data;
        if (output.find("ready") != std::string::npos) {
            break;
        }
    }
    ASSERT_NE(output.find("ready"), std::string::npos);

    session->kill();
    const PtyExit exit = session->wait(5s, std::nullopt, CancellationToken{}).get();
    EXPECT_TRUE(exit.signalled);
    EXPECT_EQ(exit.signal, SIGKILL);
}

TEST(PtyIntegration, NoZombiesAfterOpenCloseCycles) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_zombie");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    std::vector<pid_t> children;
    for (int cycle = 0; cycle < 3; ++cycle) {
        auto session =
            pty.open(shell_request(workspace.path(), "echo done"), CancellationToken{})
                .get();
        wait_running(*session);
        children.push_back(session->pid());
        (void)read_until_eof(*session);
        session->terminate();
        (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
    }

    // A global `waitpid(-1)` sweep is forbidden (execution/signal_policy.hpp):
    // in a shared test process it steals other owners' child status. Check the
    // exact pids this test opened instead, so an unrelated child (e.g. a
    // two-process daemon) cannot make this assertion fail.
    for (const pid_t child : children) {
        EXPECT_GT(child, 0);
        EXPECT_EQ(::waitpid(child, nullptr, WNOHANG), -1)
            << "pty child " << child << " was not reaped";
        EXPECT_EQ(errno, ECHILD);
    }
}

TEST(PtyIntegration, NaturalExitDrainsBufferedOutput) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_drain");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session =
        pty.open(shell_request(workspace.path(), "printf abcdef; exit 0"),
                 CancellationToken{})
            .get();
    wait_running(*session);
    const std::string output = read_until_eof(*session);
    EXPECT_NE(output.find("abcdef"), std::string::npos);
    EXPECT_EQ(session->state(), PtyState::Exited);
}

TEST(PtyIntegration, CapEnforcementRejectsTheNextOpen) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_cap");
    ResourceCaps          caps;
    caps.max_session_ptys = 1;
    caps.max_global_ptys = 1;
    ResourceGovernor governor(caps);
    NoopPtyEventSink sink;
    LocalPtyService  pty(fixture.executor(), governor, sink);

    auto first = pty.open(shell_request(workspace.path(), "sleep 100"), CancellationToken{})
                     .get();
    wait_running(*first);

    try {
        (void)pty.open(shell_request(workspace.path(), "sleep 100"), CancellationToken{}).get();
        FAIL() << "expected CapExceeded";
    } catch (const PtyError& error) {
        EXPECT_EQ(error.code(), PtyErrorCode::CapExceeded);
    }

    first->kill();
    (void)first->wait(2s, std::nullopt, CancellationToken{}).get();
}

TEST(PtyIntegration, ConcurrentFacadeCallsAreSafe) {
    PtyIoFixture             fixture;
    ymh::test::TempWorkspace workspace("pty_concurrent");
    ResourceGovernor         governor;
    NoopPtyEventSink         sink;
    LocalPtyService          pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "cat"), CancellationToken{}).get();
    wait_running(*session);

    std::atomic<bool> stop{false};
    std::thread       reader([&] {
        while (!stop.load()) {
            (void)session->read(1024, 10ms, std::nullopt, CancellationToken{}).get();
        }
    });
    std::thread writer([&] {
        for (int i = 0; i < 200; ++i) {
            try {
                session->write("x\n");
            } catch (const PtyError&) {
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    std::thread resizer([&] {
        for (int i = 0; i < 50; ++i) {
            session->resize(24 + i % 10, 80);
            std::this_thread::sleep_for(1ms);
        }
    });

    writer.join();
    resizer.join();
    std::this_thread::sleep_for(50ms);
    stop.store(true);
    reader.join();

    session->kill();
    (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
}

TEST(PtyIntegration, ReadCancellationLeavesSessionUsable) {
    PtyIoFixture             fixture;
    ymh::test::TempWorkspace workspace("pty_read_cancel");
    ResourceGovernor         governor;
    NoopPtyEventSink         sink;
    LocalPtyService          pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "cat"), CancellationToken{}).get();
    wait_running(*session);

    CancellationSource source;
    std::thread canceller([&] {
        std::this_thread::sleep_for(50ms);
        source.cancel();
    });
    const PtyRead read = session->read(1024, 5s, std::nullopt, source.token()).get();
    canceller.join();

    EXPECT_TRUE(read.cancelled);
    EXPECT_TRUE(read.data.empty());
    EXPECT_EQ(session->state(), PtyState::Running);

    session->write("still-alive\n");
    std::string output;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const PtyRead next = session->read(1024, 100ms, std::nullopt, CancellationToken{}).get();
        output += next.data;
        if (output.find("still-alive") != std::string::npos) {
            break;
        }
    }
    EXPECT_NE(output.find("still-alive"), std::string::npos);

    session->kill();
    (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
}

TEST(PtyIntegration, WaitCancellationThrowsAndLeavesSessionOpen) {
    PtyIoFixture             fixture;
    ymh::test::TempWorkspace workspace("pty_wait_cancel");
    ResourceGovernor         governor;
    NoopPtyEventSink         sink;
    LocalPtyService          pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "sleep 100"), CancellationToken{})
                       .get();
    wait_running(*session);

    CancellationSource source;
    source.cancel();
    EXPECT_THROW((void)session->wait(0ms, std::nullopt, source.token()).get(), CancellationError);
    EXPECT_NE(session->state(), PtyState::Closed);

    session->kill();
    (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
}

TEST(PtyIntegration, ExecFailureSurfacesExit127) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_execfail");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    PtyRequest request = shell_request(workspace.path(), "true");
    request.executable = "/definitely/not/a/real/binary";
    request.argv = {request.executable};

    auto session = pty.open(request, CancellationToken{}).get();
    const PtyExit exit = session->wait(5s, std::nullopt, CancellationToken{}).get();
    EXPECT_EQ(exit.exit_code, 127);
}

TEST(PtyIntegration, UI46_D8_PtyExpiredDeadlineReturnsImmediately) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_d8_expired");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "cat"), CancellationToken{}).get();
    wait_running(*session);

    const auto start = std::chrono::steady_clock::now();
    const PtyRead read = session->read(64 * 1024, 0ms,
                                       std::optional<std::chrono::milliseconds>{0ms},
                                       CancellationToken{})
                             .get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_TRUE(read.timed_out);
    EXPECT_TRUE(read.data.empty());
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});

    session->kill();
    (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
}

TEST(PtyIntegration, UI46_D8_PtyDisabledDeadlineKeepsWaitMs) {
    PtyIoFixture          fixture;
    ymh::test::TempWorkspace workspace("pty_d8_disabled");
    ResourceGovernor      governor;
    NoopPtyEventSink      sink;
    LocalPtyService       pty(fixture.executor(), governor, sink);

    auto session = pty.open(shell_request(workspace.path(), "cat"), CancellationToken{}).get();
    wait_running(*session);

    const auto start = std::chrono::steady_clock::now();
    const PtyRead read =
        session->read(64 * 1024, 0ms, std::nullopt, CancellationToken{}).get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_FALSE(read.timed_out);
    EXPECT_FALSE(read.cancelled);
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});

    session->kill();
    (void)session->wait(2s, std::nullopt, CancellationToken{}).get();
}

} // namespace

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/execution/reap_guard.hpp"
#include "ymh/execution/signal_policy.hpp"

namespace {

using namespace ymh;
using Clock = std::chrono::steady_clock;

ProcessRequest shell_request(const std::filesystem::path& cwd,
                             const std::string& command,
                             OutputSink* sink = nullptr) {
    ProcessRequest request;
    request.executable = "/bin/bash";
    request.argv = {"/bin/bash", "-lc", command};
    request.cwd = cwd;
    request.sink = sink;
    return request;
}

TEST(ProcessService, RunsCommandAndCapturesOutput) {
    ymh::test::TempWorkspace workspace("proc_capture");
    LocalProcessService service;
    OutputRing ring(4096);
    RingOutputSink sink(ring);

    const ProcessResult result =
        service.run(shell_request(workspace.path(), "printf hello", &sink), {}).get();

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(result.signalled);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(sink.materialize(4096), "hello");
}

TEST(ProcessService, NonZeroExitIsReportedInTheResult) {
    ymh::test::TempWorkspace workspace("proc_nonzero");
    LocalProcessService service;

    const ProcessResult result =
        service.run(shell_request(workspace.path(), "exit 3"), {}).get();

    EXPECT_EQ(result.exit_code, 3);
    EXPECT_FALSE(result.signalled);
    EXPECT_FALSE(result.timed_out);
}

TEST(ProcessService, TimeoutKillsTheChildGroup) {
    ymh::test::TempWorkspace workspace("proc_timeout");
    LocalProcessService service;
    ProcessRequest request = shell_request(workspace.path(), "sleep 5");
    request.timeout = std::chrono::milliseconds{200};

    const auto start = Clock::now();
    const ProcessResult result = service.run(request, {}).get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);

    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(elapsed, std::chrono::milliseconds{3000});
}

TEST(ProcessService, SpawnFailureThrowsIo) {
    ymh::test::TempWorkspace workspace("proc_spawn");
    LocalProcessService service;

    ProcessRequest bad;
    bad.executable = "/nonexistent/definitely-not-a-binary";
    bad.argv = {bad.executable};
    bad.cwd = workspace.path();
    try {
        service.run(bad, {}).get();
        FAIL() << "expected exec failure";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::Io);
    }
}

TEST(ProcessService, CancellationTerminatesTheChild) {
    ymh::test::TempWorkspace workspace("proc_cancel");
    LocalProcessService service;
    CancellationSource source;

    std::thread canceller([&source]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        source.cancel();
    });

    ProcessRequest request = shell_request(workspace.path(), "sleep 5");
    const auto start = Clock::now();
    const ProcessResult result = service.run(request, source.token()).get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);
    canceller.join();

    EXPECT_TRUE(result.signalled || result.exit_code != 0);
    EXPECT_LT(elapsed, std::chrono::milliseconds{3000});
}

TEST(ProcessService, EmptyExecutableIsRejected) {
    ymh::test::TempWorkspace workspace("proc_empty");
    LocalProcessService service;
    ProcessRequest request;
    request.cwd = workspace.path();

    EXPECT_THROW(service.run(request, {}).get(), ToolError);
}

TEST(ProcessService, ConcurrentChildrenEachReapTheirOwnStatus) {
    ymh::test::TempWorkspace workspace("proc_siblings");
    LocalProcessService service;

    ProcessResult first;
    ProcessResult second;
    std::thread   a([&] { first = service.run(shell_request(workspace.path(), "exit 3"), {}).get(); });
    std::thread   b([&] { second = service.run(shell_request(workspace.path(), "exit 7"), {}).get(); });
    a.join();
    b.join();

    EXPECT_EQ(first.exit_code, 3);
    EXPECT_EQ(second.exit_code, 7);
}

TEST(ProcessService, SignalPolicyDetectsAutoReapDispositions) {
    struct sigaction saved {};
    ASSERT_EQ(::sigaction(SIGCHLD, nullptr, &saved), 0);

    struct sigaction specific {};
    specific.sa_handler = SIG_DFL;
    sigemptyset(&specific.sa_mask);
    specific.sa_flags = 0;
    ASSERT_EQ(::sigaction(SIGCHLD, &specific, nullptr), 0);
    EXPECT_EQ(childReapPolicy(), ChildReapPolicy::SpecificPid);
    EXPECT_TRUE(specificPidReapIsSafe());

    struct sigaction ignored {};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    ignored.sa_flags = 0;
    ASSERT_EQ(::sigaction(SIGCHLD, &ignored, nullptr), 0);
    EXPECT_EQ(childReapPolicy(), ChildReapPolicy::Ignored);
    EXPECT_FALSE(specificPidReapIsSafe());

    struct sigaction nocldwait {};
    nocldwait.sa_handler = SIG_DFL;
    sigemptyset(&nocldwait.sa_mask);
    nocldwait.sa_flags = SA_NOCLDWAIT;
    ASSERT_EQ(::sigaction(SIGCHLD, &nocldwait, nullptr), 0);
    EXPECT_EQ(childReapPolicy(), ChildReapPolicy::NoCldWait);
    EXPECT_FALSE(specificPidReapIsSafe());

    ASSERT_EQ(::sigaction(SIGCHLD, &saved, nullptr), 0);
}

TEST(ReapGuard, ReProbesUnderTheLockBeforeClearing) {
    std::vector<std::string> order;
    const bool cleared = reapClaimUnderLock(
        [&order] {
            order.emplace_back("probe");
            return false;
        },
        [&order] {
            order.emplace_back("clear");
            return true;
        });

    EXPECT_TRUE(cleared);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "probe");
    EXPECT_EQ(order[1], "clear");
}

TEST(ReapGuard, AbortsAndNeverClearsWhenTheSidecarLockIsHeld) {
    bool clear_ran = false;
    const bool reaped = reapClaimUnderLock([] { return true; }, [&clear_ran] {
        clear_ran = true;
        return true;
    });

    EXPECT_FALSE(reaped);
    EXPECT_FALSE(clear_ran);
}

TEST(ProcessService, UI46_D8_ProcessExpiredDeadlineNotStarted) {
    ymh::test::TempWorkspace workspace("proc_d8_expired");
    LocalProcessService service;

    ProcessRequest request = shell_request(workspace.path(), "touch marker");
    request.deadline = std::chrono::milliseconds{0};

    const auto start = Clock::now();
    const ProcessResult result = service.run(request, {}).get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);

    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / "marker"));
}

TEST(ProcessService, UI46_D8_DisabledDeadlineNeverTimesOut) {
    ymh::test::TempWorkspace workspace("proc_d8_disabled");
    LocalProcessService service;
    OutputRing ring(4096);
    RingOutputSink sink(ring);

    ProcessRequest request = shell_request(workspace.path(), "printf hi", &sink);
    request.deadline = std::nullopt;
    request.timeout = std::chrono::milliseconds{0};

    const ProcessResult result = service.run(request, {}).get();

    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(sink.materialize(4096), "hi");
}

TEST(ProcessService, UI46_D8_ModelTimeoutNeverExceedsDeadline) {
    ymh::test::TempWorkspace workspace("proc_d8_ceiling");
    LocalProcessService service;

    ProcessRequest request = shell_request(workspace.path(), "sleep 5");
    request.timeout = std::chrono::seconds{10};
    request.deadline = std::chrono::milliseconds{150};

    const auto start = Clock::now();
    const ProcessResult result = service.run(request, {}).get();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);

    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(elapsed, std::chrono::milliseconds{3000});
}

std::string spawn_and_read(LocalProcessService& service, const ProcessRequest& request) {
    std::unique_ptr<ChildProcessHandle> handle = service.spawn(request).get();
    EXPECT_NE(handle, nullptr);
    handle->closeStdin();
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        const std::size_t count =
            handle->readStdout(std::span<char>{buffer.data(), buffer.size()}, {}).get();
        if (count == 0) {
            break;
        }
        output.append(buffer.data(), count);
    }
    (void)handle->wait({}).get();
    return output;
}

ProcessRequest env_request(const std::filesystem::path& cwd) {
    ProcessRequest request;
    request.executable = "/usr/bin/env";
    request.argv = {"/usr/bin/env"};
    request.cwd = cwd;
    request.capture_stdout = true;
    request.capture_stderr = false;
    return request;
}

TEST(ProcessService, SpawnInheritModeInheritsTheAmbientEnvironment) {
    ymh::test::TempWorkspace workspace("proc_spawn_inherit");
    LocalProcessService service;
    ::setenv("YMH_TEST_AMBIENT", "ambient-123", 1);

    ProcessRequest request = env_request(workspace.path());
    request.env_mode = ProcessEnvMode::Inherit;
    request.environment = {{"YMH_TEST_OVERLAY", "overlay-456"}};
    const std::string output = spawn_and_read(service, request);
    ::unsetenv("YMH_TEST_AMBIENT");

    EXPECT_NE(output.find("YMH_TEST_AMBIENT=ambient-123"), std::string::npos);
    EXPECT_NE(output.find("YMH_TEST_OVERLAY=overlay-456"), std::string::npos);
}

TEST(ProcessService, SpawnMinimalModeSeedsOnlyPathAndOverlay) {
    ymh::test::TempWorkspace workspace("proc_spawn_minimal");
    LocalProcessService service;
    ::setenv("YMH_TEST_AMBIENT", "ambient-123", 1);

    ProcessRequest request = env_request(workspace.path());
    request.env_mode = ProcessEnvMode::Minimal;
    request.environment = {{"YMH_TEST_OVERLAY", "overlay-456"}};
    const std::string output = spawn_and_read(service, request);
    ::unsetenv("YMH_TEST_AMBIENT");

    EXPECT_NE(output.find("PATH=/usr/local/bin:/usr/bin:/bin"), std::string::npos);
    EXPECT_NE(output.find("YMH_TEST_OVERLAY=overlay-456"), std::string::npos);
    EXPECT_EQ(output.find("YMH_TEST_AMBIENT="), std::string::npos);
}

} // namespace

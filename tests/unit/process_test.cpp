#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "support/test_env.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/process.hpp"

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

} // namespace

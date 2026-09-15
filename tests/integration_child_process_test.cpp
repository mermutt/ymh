#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>

#include "support/child_process.hpp"
#include "support/crash_inject.hpp"
#include "support/short_temp.hpp"

namespace {

using namespace std::chrono_literals;

ymh::test::ChildOptions shell_command(const std::string& command) {
    ymh::test::ChildOptions options;
    options.argv = {"/bin/sh", "-c", command};
    return options;
}

} // namespace

TEST(ChildProcessIntegration, CapturesExitCode) {
    const ymh::test::ExitStatus status = ymh::test::run_child(shell_command("exit 7"));
    ASSERT_TRUE(status.exited);
    EXPECT_EQ(status.code, 7);
    EXPECT_FALSE(status.ok());
}

TEST(ChildProcessIntegration, ReportsSuccess) {
    const ymh::test::ExitStatus status = ymh::test::run_child(shell_command("exit 0"));
    EXPECT_TRUE(status.ok());
}

TEST(ChildProcessIntegration, CapturesStdoutAndStderr) {
    ymh::test::ShortTempRoot root("ymh-child-capture");
    ymh::test::ChildOptions   options = shell_command("printf 'out-text'; printf 'err-text' 1>&2");
    options.stdout_path = root.path() / "stdout.txt";
    options.stderr_path = root.path() / "stderr.txt";
    const ymh::test::ExitStatus status = ymh::test::run_child(std::move(options));
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(ymh::test::read_text_file(root.path() / "stdout.txt"), "out-text");
    EXPECT_EQ(ymh::test::read_text_file(root.path() / "stderr.txt"), "err-text");
}

TEST(ChildProcessIntegration, AppliesEnvironmentAndCwd) {
    ymh::test::ShortTempRoot root("ymh-child-env");
    ymh::test::ChildOptions   options = shell_command("printf '%s|%s' \"$YMH_TEST_VAR\" \"$(pwd)\"");
    options.cwd = root.path();
    options.env["YMH_TEST_VAR"] = "value-42";
    options.stdout_path = root.path() / "out.txt";
    const ymh::test::ExitStatus status = ymh::test::run_child(std::move(options));
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(ymh::test::read_text_file(root.path() / "out.txt"),
              "value-42|" + root.path().string());
}

TEST(ChildProcessIntegration, WaitForTimesOutWithoutReaping) {
    ymh::test::ChildProcess child(shell_command("sleep 30"));
    EXPECT_TRUE(child.running());
    EXPECT_FALSE(child.wait_for(50ms).has_value());
    const ymh::test::ExitStatus status = child.terminate();
    EXPECT_FALSE(child.running());
    EXPECT_TRUE(status.signalled || status.exited);
}

TEST(ChildProcessIntegration, CrashReportsSignal) {
    ymh::test::ChildProcess child(shell_command("sleep 30"));
    ASSERT_TRUE(child.running());
    const ymh::test::ExitStatus status = ymh::test::crash_now(child, SIGKILL);
    ASSERT_TRUE(status.signalled);
    EXPECT_EQ(status.signal, SIGKILL);
    EXPECT_FALSE(child.running());
}

TEST(ChildProcessIntegration, DestructorReapsRunningChild) {
    pid_t pid = -1;
    {
        ymh::test::ChildProcess child(shell_command("sleep 30"));
        pid = child.pid();
        EXPECT_GT(pid, 0);
    }
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(errno, ESRCH);
}

TEST(ChildProcessIntegration, ResolvesExecutableFromPath) {
    const std::filesystem::path resolved = ymh::test::resolve_executable("sh");
    EXPECT_FALSE(resolved.empty());
    EXPECT_TRUE(std::filesystem::exists(resolved));
}

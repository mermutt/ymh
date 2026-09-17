#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <CLI/CLI.hpp>

#include "support/test_env.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/core/logging.hpp"

namespace {

using namespace ymh;

bool stop_proceeds(std::size_t live_supervisors, std::size_t live_automation, bool force,
                   bool interactive, const std::string& answer, std::string* out = nullptr,
                   std::string* err = nullptr) {
    std::istringstream input(answer);
    std::ostringstream output;
    std::ostringstream error;
    const bool result = workspace_stop_may_proceed(live_supervisors, live_automation, force,
                                                   interactive, input, output, error, "ws-id");
    if (out != nullptr) {
        *out = output.str();
    }
    if (err != nullptr) {
        *err = error.str();
    }
    return result;
}

TEST(Cli, DefaultsToTui) {
    const CliInvocation invocation = parse_cli({});
    EXPECT_EQ(invocation.command, CliInvocation::Command::Tui);
}

TEST(Cli, VersionFlag) {
    const CliInvocation invocation = parse_cli({"--version"});
    EXPECT_EQ(invocation.command, CliInvocation::Command::Version);
}

TEST(Cli, RunTakesTask) {
    const CliInvocation invocation = parse_cli({"run", "inspect this project"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::Run);
    EXPECT_EQ(invocation.task, "inspect this project");
}

TEST(Cli, RunResume) {
    const CliInvocation invocation = parse_cli({"--resume", "abc-123", "run", "continue"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::Run);
    EXPECT_EQ(invocation.session, "abc-123");
    EXPECT_EQ(invocation.task, "continue");
}

TEST(Cli, SessionCommands) {
    EXPECT_EQ(parse_cli({"list"}).command, CliInvocation::Command::List);

    const CliInvocation show = parse_cli({"show", "sid-1"});
    EXPECT_EQ(show.command, CliInvocation::Command::Show);
    EXPECT_EQ(show.session, "sid-1");

    const CliInvocation replay = parse_cli({"replay", "sid-2"});
    EXPECT_EQ(replay.command, CliInvocation::Command::Replay);
    EXPECT_EQ(replay.session, "sid-2");

    const CliInvocation fork = parse_cli({"fork", "sid-3"});
    EXPECT_EQ(fork.command, CliInvocation::Command::Fork);
    EXPECT_EQ(fork.session, "sid-3");
}

TEST(Cli, Overrides) {
    const CliInvocation invocation =
        parse_cli({"--model", "my-model", "--provider", "openai-compatible",
                   "--base-url", "http://localhost:8000/v1", "--log-level", "debug", "list"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::List);
    EXPECT_EQ(invocation.model, "my-model");
    EXPECT_EQ(invocation.provider, "openai-compatible");
    EXPECT_EQ(invocation.base_url, "http://localhost:8000/v1");
    EXPECT_EQ(invocation.log_level, "debug");
}

TEST(Cli, WorkspaceOverride) {
    const CliInvocation invocation = parse_cli({"--workspace", "/tmp/ws", "list"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::List);
    EXPECT_EQ(invocation.workspace, "/tmp/ws");
}

TEST(Cli, RunRequiresTask) {
    EXPECT_THROW((void)parse_cli({"run"}), CLI::ParseError);
}

TEST(Cli, UnknownCommandRejected) {
    EXPECT_THROW((void)parse_cli({"definitely-not-a-command"}), CLI::ParseError);
}

TEST(Cli, RunVersionPrintsAndExitsZero) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"--version"}, out, err), 0);
    EXPECT_NE(out.str().find("ymh "), std::string::npos);
}

TEST(Cli, HelpPrintsUsageAndExitsZero) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"--help"}, out, err), 0);
    EXPECT_NE(out.str().find("Usage:"), std::string::npos);
    EXPECT_NE(out.str().find("run"), std::string::npos);
    EXPECT_TRUE(err.str().empty());
}

TEST(Cli, SubcommandHelpShowsSubcommandUsage) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"run", "--help"}, out, err), 0);
    EXPECT_NE(out.str().find("task"), std::string::npos);
    EXPECT_TRUE(err.str().empty());
}

TEST(Cli, RunListEmptyWorkspace) {
    test::TempWorkspace workspace("cli_run_list");
    std::ostringstream  out;
    std::ostringstream  err;
    const std::string   config = (workspace.path() / "missing.toml").string();
    EXPECT_EQ(run_cli({"--config", config, "--workspace", workspace.path().string(), "list"}, out,
                      err),
              0);
    EXPECT_NE(out.str().find("no sessions"), std::string::npos);
}

TEST(Cli, DefaultCommandIsTui) {
    const CliInvocation invocation = parse_cli({});
    EXPECT_EQ(invocation.command, CliInvocation::Command::Tui);
}

TEST(Cli, ConfigPathSubcommandParses) {
    const CliInvocation invocation = parse_cli({"config", "path"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::Config);
    ASSERT_EQ(invocation.config_args.size(), 1u);
    EXPECT_EQ(invocation.config_args[0], "path");
}

TEST(Cli, ConfigPathPrintsEffectivePath) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"--config", "/tmp/ymh-test-config.toml", "config", "path"}, out, err), 0);
    EXPECT_NE(out.str().find("/tmp/ymh-test-config.toml"), std::string::npos);
    EXPECT_TRUE(err.str().empty());
}

TEST(Cli, WorkspaceStopForceFlagParses) {
    const CliInvocation invocation = parse_cli({"workspace", "stop", "ws-id", "--force"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::Workspace);
    ASSERT_EQ(invocation.workspace_args.size(), 2u);
    EXPECT_EQ(invocation.workspace_args[0], "stop");
    EXPECT_EQ(invocation.workspace_args[1], "ws-id");
    EXPECT_TRUE(invocation.workspace_force);
}

TEST(Cli, WorkspaceStopWithoutForceDefaultsFalse) {
    const CliInvocation invocation = parse_cli({"workspace", "stop", "ws-id"});
    ASSERT_EQ(invocation.command, CliInvocation::Command::Workspace);
    EXPECT_FALSE(invocation.workspace_force);
}

TEST(Cli, WorkspaceStopProceedsWithoutLiveOwners) {
    std::string out;
    EXPECT_TRUE(stop_proceeds(0, 0, false, false, "", &out));
    EXPECT_TRUE(out.empty());
}

TEST(Cli, WorkspaceStopForceSkipsConfirmation) {
    std::string out;
    EXPECT_TRUE(stop_proceeds(3, 1, true, false, "n\n", &out));
    EXPECT_TRUE(out.empty());
}

TEST(Cli, WorkspaceStopDeclinesNonInteractiveWithoutForce) {
    std::string out;
    std::string err;
    EXPECT_FALSE(stop_proceeds(1, 0, false, false, "y\n", &out, &err));
    EXPECT_NE(out.find("in use"), std::string::npos);
    EXPECT_NE(err.find("--force"), std::string::npos);
}

TEST(Cli, WorkspaceStopInteractiveConfirmationAccepted) {
    std::string out;
    EXPECT_TRUE(stop_proceeds(1, 2, false, true, "y\n", &out));
    EXPECT_NE(out.find("[y/N]"), std::string::npos);
    EXPECT_NE(out.find("in use"), std::string::npos);
}

TEST(Cli, WorkspaceStopInteractiveConfirmationDeclined) {
    for (const std::string& answer : {std::string{"n\n"}, std::string{"\n"},
                                      std::string{"no\n"}, std::string{"   "}}) {
        std::string out;
        EXPECT_FALSE(stop_proceeds(1, 0, false, true, answer, &out)) << "answer=" << answer;
    }
}

class StderrCapture {
public:
    StderrCapture() : saved_(::dup(STDERR_FILENO)) {
        file_ = std::tmpfile();
        if (file_ != nullptr) {
            ::dup2(::fileno(file_), STDERR_FILENO);
        }
    }

    ~StderrCapture() {
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string read() {
        if (file_ == nullptr) {
            return {};
        }
        std::fflush(nullptr);
        std::fflush(file_);
        std::rewind(file_);
        std::string result;
        char        buffer[512];
        std::size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file_)) > 0) {
            result.append(buffer, count);
        }
        return result;
    }

private:
    int        saved_ = -1;
    std::FILE* file_  = nullptr;
};

TEST(Cli, LegacyTomlWarningOnStderr) {
    shutdown_logging();
    test::TempWorkspace workspace("cli_legacy_warn");
    workspace.write(".ymh/config.toml", "[agent]\nmax_steps = 7\n");

    std::ostringstream out;
    std::ostringstream err;
    StderrCapture      capture;
    const int          code = run_cli({"--config", (workspace.path() / "custom.jsonc").string(),
                                       "--workspace", workspace.path().string(), "list"},
                                      out, err);
    const std::string  stderr_text = capture.read();

    EXPECT_EQ(code, 0);
    EXPECT_NE(stderr_text.find("ignoring legacy TOML"), std::string::npos);
}

TEST(Cli, ConfigPathLegacyNote) {
    test::TempWorkspace         workspace("cli_config_note");
    const std::filesystem::path jsonc = workspace.path() / "config.jsonc";
    const std::filesystem::path toml  = workspace.path() / "config.toml";
    workspace.write("config.toml", "[agent]\nmax_steps = 7\n");

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"--config", jsonc.string(), "config", "path"}, out, err), 0);
    const std::string conventional = out.str();
    EXPECT_NE(conventional.find(toml.string()), std::string::npos);
    EXPECT_NE(conventional.find(jsonc.string()), std::string::npos);
    EXPECT_EQ(conventional.find("update --config"), std::string::npos);

    std::ostringstream legacy_out;
    std::ostringstream legacy_err;
    EXPECT_EQ(run_cli({"--config", toml.string(), "config", "path"}, legacy_out, legacy_err), 0);
    const std::string legacy = legacy_out.str();
    EXPECT_NE(legacy.find(toml.string()), std::string::npos);
    EXPECT_NE(legacy.find(jsonc.string()), std::string::npos);
    EXPECT_NE(legacy.find("update --config to that path"), std::string::npos);
}

} // namespace

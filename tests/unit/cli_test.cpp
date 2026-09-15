#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "support/test_env.hpp"
#include "ymh/cli/cli.hpp"

namespace {

using namespace ymh;

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

} // namespace

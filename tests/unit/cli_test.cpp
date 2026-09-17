#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <CLI/CLI.hpp>

#include "support/test_env.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/core/logging.hpp"

namespace {

using namespace ymh;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = ::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string                name_;
    std::optional<std::string> previous_;
};

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
    workspace.write("config.jsonc", "{}\n");
    std::ostringstream out;
    std::ostringstream err;
    const std::string  config = (workspace.path() / "config.jsonc").string();
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
    EXPECT_EQ(run_cli({"--config", "/tmp/ymh-test-config.jsonc", "config", "path"}, out, err), 0);
    EXPECT_NE(out.str().find("/tmp/ymh-test-config.jsonc"), std::string::npos);
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

TEST(Cli, ExplicitConfigMissingIsNotCreated) {
    test::TempWorkspace         workspace("cli_explicit_missing");
    const std::filesystem::path missing = workspace.path() / "missing.jsonc";
    std::ostringstream          out;
    std::ostringstream          err;
    EXPECT_NE(run_cli({"--config", missing.string(), "--workspace", workspace.path().string(),
                       "list"},
                      out, err),
              0);
    EXPECT_NE(err.str().find("required global config not found"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists(missing));
    EXPECT_TRUE(std::filesystem::is_directory(workspace.path() / ".ymh"));
}

TEST(Cli, ConventionalMissingIsScaffoldedThenLoads) {
    test::TempWorkspace         workspace("cli_conventional_scaffold");
    const std::filesystem::path xdg      = workspace.path() / "xdg";
    const std::filesystem::path expected = xdg / "ymh" / "config.jsonc";
    ScopedEnv                   xdg_env("XDG_CONFIG_HOME", xdg.string());
    ScopedEnv                   home_env("HOME", (workspace.path() / "home").string());
    ASSERT_FALSE(std::filesystem::exists(expected));

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"--workspace", workspace.path().string(), "list"}, out, err), 0) << err.str();
    EXPECT_TRUE(std::filesystem::is_regular_file(expected));
    EXPECT_TRUE(std::filesystem::is_directory(workspace.path() / ".ymh"));
}

TEST(Cli, ScaffoldFailureThenRequiredError) {
    test::TempWorkspace workspace("cli_scaffold_fail");
    workspace.write("blocker", "not a directory");
    const std::filesystem::path xdg = workspace.path() / "blocker";
    ScopedEnv                   xdg_env("XDG_CONFIG_HOME", xdg.string());
    ScopedEnv                   home_env("HOME", (workspace.path() / "home").string());

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"--workspace", workspace.path().string(), "list"}, out, err), 2);
    EXPECT_NE(err.str().find("required global config not found"), std::string::npos) << err.str();
    EXPECT_FALSE(std::filesystem::exists(xdg / "ymh" / "config.jsonc"));
}

TEST(Cli, WorkspaceCommandNeedsNoConfig) {
    test::TempWorkspace         workspace("cli_workspace_noconfig");
    const std::filesystem::path xdg   = workspace.path() / "xdg";
    const std::filesystem::path state = workspace.path() / "state";
    ScopedEnv                   xdg_env("XDG_CONFIG_HOME", xdg.string());
    ScopedEnv                   home_env("HOME", (workspace.path() / "home").string());
    ScopedEnv                   state_env("XDG_STATE_HOME", state.string());

    std::ostringstream add_out;
    std::ostringstream add_err;
    EXPECT_EQ(run_cli({"workspace", "add", workspace.path().string()}, add_out, add_err), 0)
        << add_err.str();

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_cli({"workspace", "list"}, out, err), 0) << err.str();
    EXPECT_EQ(err.str().find("ConfigError"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(xdg / "ymh" / "config.jsonc"));
}

} // namespace

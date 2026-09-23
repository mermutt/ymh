#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "support/child_process.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/config/config.hpp"
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

struct ImportRun {
    bool                  result = false;
    std::string           out;
    std::string           err;
    std::filesystem::path global;
};

ImportRun run_import(const test::TempWorkspace& workspace, const std::string& localcode_body,
                     const std::string& input, bool interactive,
                     const std::string&          config_path = std::string(),
                     CliInvocation::Command      command     = CliInvocation::Command::Tui) {
    ScopedEnv xdg("XDG_CONFIG_HOME", (workspace.path() / "config").string());
    ScopedEnv home("HOME", (workspace.path() / "home").string());

    const std::filesystem::path localcode =
        workspace.path() / "home" / ".localcode" / "config.json";
    if (!localcode_body.empty()) {
        std::filesystem::create_directories(localcode.parent_path());
        std::ofstream(localcode, std::ios::binary) << localcode_body;
    }

    CliInvocation invocation;
    invocation.command     = command;
    invocation.config_path = config_path;

    ImportRun          run;
    run.global = workspace.path() / "config" / "ymh" / "config.jsonc";
    std::istringstream in(input);
    std::ostringstream out;
    std::ostringstream err;
    run.result = maybe_import_localcode_config(invocation, run.global, interactive, in, out, err);
    run.out    = out.str();
    run.err    = err.str();
    return run;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream      input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

constexpr const char* kOneServer = R"JSON({"mcp_servers": {"s": {"command": "c"}}})JSON";

TEST(CliImport, NonInteractiveSkipsSilently) {
    test::TempWorkspace workspace("cli_import_noninteractive");
    const ImportRun     run = run_import(workspace, kOneServer, "", false);
    EXPECT_FALSE(run.result);
    EXPECT_TRUE(run.out.empty());
    EXPECT_TRUE(run.err.empty());
    EXPECT_FALSE(std::filesystem::exists(run.global));
}

TEST(CliImport, OnlyTuiCommandPrompts) {
    test::TempWorkspace workspace("cli_import_non_tui");
    const ImportRun     run =
        run_import(workspace, kOneServer, "\n", true, std::string(), CliInvocation::Command::List);
    EXPECT_FALSE(run.result);
    EXPECT_TRUE(run.out.empty());
}

TEST(CliImport, ExplicitConfigSkips) {
    test::TempWorkspace workspace("cli_import_explicit");
    const ImportRun     run = run_import(workspace, kOneServer, "\n", true,
                                         (workspace.path() / "explicit.jsonc").string());
    EXPECT_FALSE(run.result);
    EXPECT_TRUE(run.out.empty());
}

TEST(CliImport, GlobalDirPresentSkips) {
    test::TempWorkspace workspace("cli_import_dir_present");
    std::filesystem::create_directories(workspace.path() / "config" / "ymh");
    const ImportRun run = run_import(workspace, kOneServer, "\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_TRUE(run.out.empty());
}

TEST(CliImport, LocalcodeAbsentSkips) {
    test::TempWorkspace workspace("cli_import_absent");
    const ImportRun     run = run_import(workspace, "", "\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_TRUE(run.out.empty());
}

TEST(CliImport, EmptyAnswerImportsAndWritesMode0600) {
    test::TempWorkspace workspace("cli_import_accept");
    const ImportRun     run = run_import(workspace, kOneServer, "\n", true);
    EXPECT_TRUE(run.result) << run.err;
    EXPECT_NE(run.out.find("first run"), std::string::npos) << run.out;
    EXPECT_NE(run.out.find("[Y/n]"), std::string::npos) << run.out;
    ASSERT_TRUE(std::filesystem::is_regular_file(run.global));
    struct stat st {};
    ASSERT_EQ(::stat(run.global.c_str(), &st), 0);
    EXPECT_EQ(static_cast<unsigned>(st.st_mode & 0777u), 0600u);

    const nlohmann::json written = nlohmann::json::parse(read_text(run.global));
    ASSERT_TRUE(written["mcp_servers"].contains("s"));
    EXPECT_FALSE(written["mcp_servers"]["s"]["required"].get<bool>());

    ConfigPaths paths;
    paths.global        = run.global;
    const Config config = load_config(paths);
    EXPECT_EQ(config.mcp.servers.size(), 1u);
}

TEST(CliImport, NoDeclinesWithoutWriting) {
    test::TempWorkspace workspace("cli_import_decline");
    const ImportRun     run = run_import(workspace, kOneServer, "n\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_FALSE(std::filesystem::exists(run.global));
}

TEST(CliImport, EofDeclines) {
    test::TempWorkspace workspace("cli_import_eof");
    const ImportRun     run = run_import(workspace, kOneServer, "", true);
    EXPECT_FALSE(run.result);
    EXPECT_FALSE(std::filesystem::exists(run.global));
}

TEST(CliImport, UnknownAnswerRepromptsOnce) {
    test::TempWorkspace workspace("cli_import_reprompt");
    const ImportRun     run = run_import(workspace, kOneServer, "huh\ny\n", true);
    EXPECT_TRUE(run.result) << run.err;
    EXPECT_EQ(count_occurrences(run.out, "[Y/n]"), 2u) << run.out;
}

TEST(CliImport, UnknownTwiceDeclines) {
    test::TempWorkspace workspace("cli_import_unknown");
    const ImportRun     run = run_import(workspace, kOneServer, "huh\nmeh\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_FALSE(std::filesystem::exists(run.global));
}

TEST(CliImport, MalformedJsonIsSkippedWithOneLine) {
    test::TempWorkspace workspace("cli_import_malformed");
    const ImportRun     run = run_import(workspace, "not json", "\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_TRUE(run.out.empty());
    EXPECT_NE(run.err.find("is not valid JSON; skipping import"), std::string::npos) << run.err;
    EXPECT_EQ(count_occurrences(run.err, "\n"), 1u) << run.err;
}

TEST(CliImport, NonObjectJsonIsInvalid) {
    test::TempWorkspace workspace("cli_import_nonobject");
    const ImportRun     run = run_import(workspace, "[]", "\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_NE(run.err.find("is not valid JSON; skipping import"), std::string::npos) << run.err;
}

TEST(CliImport, OversizedFileIsSkipped) {
    test::TempWorkspace workspace("cli_import_oversized");
    const std::string   body =
        "{\"x\":\"" + std::string(4u * 1024u * 1024u, 'a') + "\"}";
    const ImportRun run = run_import(workspace, body, "\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_NE(run.err.find("larger than 4 MiB; skipping import"), std::string::npos) << run.err;
}

TEST(CliImport, SecretsAreNeverLogged) {
    test::TempWorkspace workspace("cli_import_secrets");
    const std::string   body = R"JSON({
      "mcp_servers": {
        "s": { "command": "c", "env": { "TOKEN": "SUPERSECRET_VALUE" },
               "headers": { "Authorization": "SECRET_HEADER_VALUE" } }
      }
    })JSON";
    const ImportRun run = run_import(workspace, body, "\n", true);
    EXPECT_TRUE(run.result) << run.err;
    EXPECT_EQ(run.out.find("SUPERSECRET_VALUE"), std::string::npos);
    EXPECT_EQ(run.err.find("SUPERSECRET_VALUE"), std::string::npos);
    EXPECT_EQ(run.out.find("SECRET_HEADER_VALUE"), std::string::npos);
    EXPECT_EQ(run.err.find("SECRET_HEADER_VALUE"), std::string::npos);
    EXPECT_NE(read_text(run.global).find("SUPERSECRET_VALUE"), std::string::npos);
}

TEST(CliImport, SemanticValidationSkipsAndNotes) {
    ::unsetenv("YMH_IMPORT_MISSING_VAR");
    test::TempWorkspace workspace("cli_import_semantic");
    const std::string   body = R"JSON({
      "mcp_servers": {
        "net": { "type": "http", "url": "https://x.test/sse" },
        "badref": { "command": "c", "env": { "K": "${YMH_IMPORT_MISSING_VAR}" } },
        "toobig": { "command": "c", "max_result_bytes": 999999999 },
        "needed": { "command": "c", "required": true },
        "good": { "command": "c" }
      }
    })JSON";
    const ImportRun run = run_import(workspace, body, "\n", true);
    EXPECT_TRUE(run.result) << run.err;
    EXPECT_NE(run.err.find("skipping mcp server 'net': no http_sse transport implemented"),
              std::string::npos)
        << run.err;
    EXPECT_NE(run.err.find("skipping mcp server 'badref': server 'badref': missing environment "
                           "variable: YMH_IMPORT_MISSING_VAR"),
              std::string::npos)
        << run.err;
    EXPECT_NE(run.err.find("skipping mcp server 'toobig'"), std::string::npos) << run.err;
    EXPECT_NE(run.err.find("server 'needed': 'required' is not imported"), std::string::npos)
        << run.err;

    const nlohmann::json written = nlohmann::json::parse(read_text(run.global));
    EXPECT_FALSE(written["mcp_servers"].contains("net"));
    EXPECT_FALSE(written["mcp_servers"].contains("badref"));
    EXPECT_FALSE(written["mcp_servers"].contains("toobig"));
    ASSERT_TRUE(written["mcp_servers"].contains("needed"));
    EXPECT_FALSE(written["mcp_servers"]["needed"]["required"].get<bool>());
    EXPECT_TRUE(written["mcp_servers"].contains("good"));
}

// 25 review M6 / UX-I18: the anti-brick guarantee. An imported config with an
// unusable http server, an unset-${VAR} stdio server, and a required server must
// still start a workspace runtime (the unusable servers are erased / forced
// non-required before the write).
TEST(CliImport, ImportedConfigStartsRuntime) {
    ::unsetenv("YMH_IMPORT_MISSING_VAR");
    test::TempWorkspace workspace("cli_import_starts_runtime");
    const std::string   body = R"JSON({
      "mcp_servers": {
        "net": { "type": "http", "url": "https://x.test/sse" },
        "badref": { "command": "c", "env": { "K": "${YMH_IMPORT_MISSING_VAR}" } },
        "needed": { "command": "c", "required": true },
        "good": { "command": "c" }
      }
    })JSON";
    const ImportRun run = run_import(workspace, body, "\n", true);
    ASSERT_TRUE(run.result) << run.err;
    ASSERT_TRUE(std::filesystem::is_regular_file(run.global));

    ConfigPaths paths;
    paths.global = run.global;
    WorkspaceRuntimeOptions options;
    options.config  = load_config(paths);
    options.root    = workspace.path();
    options.boot_id = BootId{"import-runtime-boot"};

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        make_workspace_runtime(std::move(options));
    ASSERT_TRUE(created.has_value()) << created.error().detail;
}

TEST(CliImport, LoaderRejectedDocumentIsNotWritten) {
    test::TempWorkspace workspace("cli_import_loader_reject");
    const ImportRun     run = run_import(
        workspace, R"JSON({"mcp_servers": {"s": {"type": "bogus", "command": "c"}}})JSON",
        "\n", true);
    EXPECT_FALSE(run.result);
    EXPECT_FALSE(std::filesystem::exists(run.global));
    EXPECT_NE(run.err.find("imported config failed validation"), std::string::npos) << run.err;
}

TEST(Cli52Review, DaemonOverrideFlagsDetectsSelectors) {
    CliInvocation invocation;
    EXPECT_TRUE(daemon_override_flags(invocation).empty());

    invocation.model            = "m";
    invocation.endpoint         = "e";
    invocation.provider         = "p";
    invocation.base_url         = "u";
    invocation.api_key_env      = "k";
    invocation.reasoning_effort = "low";
    const std::vector<std::string> flags = daemon_override_flags(invocation);
    ASSERT_EQ(flags.size(), 6u);
    EXPECT_EQ(flags[0], "--model");
    EXPECT_EQ(flags[1], "--endpoint");
    EXPECT_EQ(flags[2], "--provider");
    EXPECT_EQ(flags[3], "--base-url");
    EXPECT_EQ(flags[4], "--api-key-env");
    EXPECT_EQ(flags[5], "--reasoning-effort");
}

TEST(Cli52Review, UnknownEndpointExitsTwoNotAbort) {
    test::TempWorkspace workspace("cli52_unknown_endpoint");
    const std::string  config_path = (workspace.path() / "config.jsonc").string();
    std::ofstream(config_path)
        << R"JSON({
             "llm": {
               "endpoints": { "real": { "base_url": "https://example.invalid/v1",
                                        "api_key_env": "YMH_TEST_KEY" } },
               "models": { "m": { "endpoint": "real", "model": "wire" } },
               "active_model": "m"
             }
           })JSON";

    ymh::test::ChildOptions options;
#ifdef YMH_TEST_BINARY
    options.executable = std::filesystem::path{YMH_TEST_BINARY};
#else
    options.executable = std::filesystem::path{"ymh"};
#endif
    options.argv       = {"ymh",        "--config",    config_path, "--workspace",
                          workspace.path().string(), "--endpoint", "bogus", "run", "hi"};
    options.env_remove = {"YMH_LLM_ENDPOINT", "YMH_LLM_ACTIVE_MODEL", "YMH_LLM_MODEL"};
    options.stdout_path = workspace.path() / "stdout.txt";
    options.stderr_path = workspace.path() / "stderr.txt";
    const ymh::test::ExitStatus status = ymh::test::run_child(std::move(options));
    ASSERT_TRUE(status.exited);
    EXPECT_FALSE(status.signalled) << "killed by signal " << status.signal;
    EXPECT_EQ(status.code, 2);
    const std::string err_text = ymh::test::read_text_file(workspace.path() / "stderr.txt");
    EXPECT_NE(err_text.find("unknown endpoint 'bogus'"), std::string::npos) << err_text;
}

TEST(CliImport, MoreThanMaxServersStaysNonRequired) {
    test::TempWorkspace workspace("cli_import_many");
    std::string         body = "{\"mcp_servers\": {";
    for (int i = 0; i < 9; ++i) {
        if (i > 0) {
            body += ",";
        }
        body += "\"s" + std::to_string(i) + "\": {\"command\": \"c\"}";
    }
    body += "}}";
    const ImportRun run = run_import(workspace, body, "\n", true);
    EXPECT_TRUE(run.result) << run.err;
    const nlohmann::json written = nlohmann::json::parse(read_text(run.global));
    ASSERT_EQ(written["mcp_servers"].size(), 9u);
    for (auto it = written["mcp_servers"].begin(); it != written["mcp_servers"].end(); ++it) {
        EXPECT_FALSE(it.value()["required"].get<bool>());
    }
}

} // namespace

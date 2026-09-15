#pragma once

// CLI surface (§41) and dispatch. `parse_cli` is pure argument parsing (no I/O)
// so it can be unit-tested; `run_cli` loads config, initialises logging, and
// dispatches to the TUI placeholder, headless mode, or a session command.
//
// Commands:
//   ymh [--resume SESSION] [--new]      default: TUI (placeholder this wave)
//   ymh run "task"                      headless (§42)
//   ymh list | show SESSION | replay SESSION | fork SESSION
//   ymh config path
//   ymh --version

#include <iosfwd>
#include <string>
#include <vector>

namespace ymh {

struct CliInvocation {
    enum class Command {
        Tui,
        Run,
        List,
        Show,
        Replay,
        Fork,
        Workspace,
        Config,
        Version,
    };

    Command     command = Command::Tui;
    std::string task;
    std::string session;   // show/replay/fork/--resume target
    bool        new_session = false;
    std::string workspace;     // empty => cwd
    std::string config_path;   // empty => default global config
    std::string model;
    std::string provider;
    std::string base_url;
    std::string api_key_env;
    std::string log_level;
    std::string reasoning_effort;
    bool        verbose = false;
    bool        version_requested = false;
    std::vector<std::string> workspace_args;
    std::vector<std::string> config_args;
};

// Throws `CLI::ParseError` on a malformed command line.
[[nodiscard]] CliInvocation parse_cli(const std::vector<std::string>& args);
[[nodiscard]] CliInvocation parse_cli(int argc, char** argv);

// Parses and executes. Returns a process exit code.
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int run_cli(int argc, char** argv);

} // namespace ymh

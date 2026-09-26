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

#include <cstddef>
#include <filesystem>
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
        Session,
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
    std::string endpoint;
    std::string provider;
    std::string base_url;
    std::string api_key_env;
    std::string log_level;
    std::string reasoning_effort;
    bool        verbose = false;
    bool        version_requested = false;
    bool        workspace_force = false;   // `workspace stop --force` (§4.6)
    std::vector<std::string> workspace_args;
    std::vector<std::string> config_args;

    // 23 §5.1: `ymh session prune` options.
    bool        session_empty = false;
    bool        session_keep_set = false;
    std::size_t session_keep = 0;
    bool        session_all = false;
    bool        session_yes = false;
    bool        session_force = false;
    bool        session_json = false;
    bool        session_older_than = false;  // 23 §5.1: rejected in v1 (exit 2)
};

// Throws `CLI::ParseError` on a malformed command line.
[[nodiscard]] CliInvocation parse_cli(const std::vector<std::string>& args);
[[nodiscard]] CliInvocation parse_cli(int argc, char** argv);

// 16 §4.6 step 3: the `ymh workspace stop` confirmation contract. Returns true
// when the stop may proceed: no live owners, or `--force`, or an interactive
// "yes". `interactive` is false when stdin is not a terminal (scripted use); a
// stop with live owners is then declined unless `force`. Prints the in-use
// notice/prompt to `out` and a refusal hint to `err`. Pure I/O (no transport),
// so the CLI decision is unit-testable; `run_workspace_stop` is its only
// production caller.
[[nodiscard]] bool workspace_stop_may_proceed(std::size_t live_supervisors,
                                              std::size_t live_automation, bool force,
                                              bool interactive, std::istream& in,
                                              std::ostream& out, std::ostream& err,
                                              const std::string& workspace_label);

// 52 review (2B): the CLI provider/model selectors present on `invocation`, in
// flag order. The live-daemon `run` path owns its provider config and cannot
// honor these, so the CLI must reject rather than silently drop them. Pure, so
// the detection is unit-testable.
[[nodiscard]] std::vector<std::string> daemon_override_flags(const CliInvocation& invocation);

// Parses and executes. Returns a process exit code.
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
int run_cli(int argc, char** argv);

// 25-D14/D15: first-run localcode import. Returns true iff a config was imported
// and written. Runs only under 25-D14's trigger (Tui, no explicit `--config`, the
// conventional global config directory absent, `~/.localcode/config.json` a
// regular file, interactive). `in`/`out`/`err` are injected for tests. Creates
// the global config directory and writes via a 0600 temp + atomic rename.
[[nodiscard]] bool maybe_import_localcode_config(const CliInvocation& invocation,
                                                 const std::filesystem::path& global_config,
                                                 bool interactive, std::istream& in,
                                                 std::ostream& out, std::ostream& err);

// 25-D14: hard size bound on the localcode file read before prompting.
inline constexpr std::size_t kLocalcodeImportMaxBytes = 4u * 1024u * 1024u;

} // namespace ymh

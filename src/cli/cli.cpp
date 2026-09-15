#include "ymh/cli/cli.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <unistd.h>

#include "ymh/cli/headless.hpp"
#include "ymh/cli/session_cli.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/ui/ui_application.hpp"

#ifndef YMH_VERSION
#define YMH_VERSION "0.0.0"
#endif

namespace ymh {
namespace {

void add_common(CLI::App& app, CliInvocation& invocation) {
    app.add_flag("-V,--version", invocation.version_requested, "Print version and exit");
    app.add_option("--resume", invocation.session, "Resume session ID");
    app.add_flag("--new", invocation.new_session, "Force a new session");
    app.add_option("--workspace", invocation.workspace, "Workspace root (default: cwd)");
    app.add_option("--config", invocation.config_path, "Global config file override");
    app.add_option("--model", invocation.model, "Override the model");
    app.add_option("--provider", invocation.provider, "Override the provider");
    app.add_option("--base-url", invocation.base_url, "Override the LLM base URL");
    app.add_option("--api-key-env", invocation.api_key_env, "Override the API-key env var name");
    app.add_option("--log-level", invocation.log_level, "Override the log level");
    app.add_option("--reasoning-effort", invocation.reasoning_effort,
                   "Override the reasoning effort");
    app.add_flag("-v,--verbose", invocation.verbose, "Verbose diagnostics");
}

Config load_invocation_config(const CliInvocation& invocation,
                              const std::filesystem::path& root) {
    ConfigPaths paths;
    paths.global = invocation.config_path.empty()
                       ? default_global_config_path()
                       : std::filesystem::path{invocation.config_path};
    paths.workspace = workspace_config_path(root);
    Config config   = load_config(paths);

    if (!invocation.model.empty()) {
        config.agent.model = invocation.model;
    }
    if (!invocation.provider.empty()) {
        config.llm.provider = invocation.provider;
    }
    if (!invocation.base_url.empty()) {
        config.llm.base_url = invocation.base_url;
    }
    if (!invocation.api_key_env.empty()) {
        config.llm.api_key_env = invocation.api_key_env;
    }
    if (!invocation.log_level.empty()) {
        config.logging.level = invocation.log_level;
    }
    if (!invocation.reasoning_effort.empty()) {
        config.agent.reasoning_effort = invocation.reasoning_effort;
    }
    return config;
}

std::filesystem::path resolve_workspace(const CliInvocation& invocation) {
    if (!invocation.workspace.empty()) {
        return std::filesystem::path{invocation.workspace};
    }
    return std::filesystem::current_path();
}

std::filesystem::path effective_global_config(const CliInvocation& invocation) {
    if (!invocation.config_path.empty()) {
        return std::filesystem::path{invocation.config_path};
    }
    return default_global_config_path();
}

void scaffold_for_invocation(const CliInvocation& invocation,
                             const std::filesystem::path& root) {
    switch (invocation.command) {
        case CliInvocation::Command::Tui:
        case CliInvocation::Command::Run:
        case CliInvocation::Command::List:
        case CliInvocation::Command::Show:
        case CliInvocation::Command::Replay:
        case CliInvocation::Command::Fork:
            (void)scaffold_config(root, effective_global_config(invocation),
                                  &category_logger(LogCategory::Filesystem));
            break;
        case CliInvocation::Command::Workspace:
        case CliInvocation::Command::Config:
        case CliInvocation::Command::Version:
            break;
    }
}

int run_config_command(const CliInvocation& invocation, std::ostream& out, std::ostream& err) {
    if (invocation.config_args.size() != 1 || invocation.config_args[0] != "path") {
        err << "ymh: usage: ymh config path\n";
        return 2;
    }
    const std::filesystem::path path = effective_global_config(invocation);
    std::error_code             error;
    const bool                  exists = std::filesystem::exists(path, error) && !error;
    out << path.string() << (exists ? " (exists)" : " (missing)") << '\n';
    return 0;
}

// CLI11 reports `--help`/`--help-all` by throwing a `ParseError` whose `what()`
// is a fixed internal message; the actual help text must be requested from the
// app. Prefer the help of the subcommand that was parsed (e.g. `ymh run --help`),
// falling back to the top-level help.
std::string help_text(CLI::App& app, std::initializer_list<CLI::App*> subcommands) {
    for (CLI::App* subcommand : subcommands) {
        if (subcommand != nullptr && subcommand->parsed()) {
            return subcommand->help();
        }
    }
    return app.help();
}

} // namespace

CliInvocation parse_cli(const std::vector<std::string>& args) {
    CLI::App    app{"ymh - terminal coding-agent harness"};
    CliInvocation invocation;

    add_common(app, invocation);

    std::string run_task;
    CLI::App*   run = app.add_subcommand("run", "Run a task headlessly");
    add_common(*run, invocation);
    run->add_option("task", run_task, "Task text")->required();

    CLI::App* list = app.add_subcommand("list", "List sessions in the workspace");
    add_common(*list, invocation);

    std::string show_session;
    CLI::App*   show = app.add_subcommand("show", "Show a session's messages");
    add_common(*show, invocation);
    show->add_option("session", show_session, "Session ID")->required();

    std::string replay_session;
    CLI::App*   replay = app.add_subcommand("replay", "Replay a session's event log");
    add_common(*replay, invocation);
    replay->add_option("session", replay_session, "Session ID")->required();

    std::string fork_session;
    CLI::App*   fork = app.add_subcommand("fork", "Fork a session");
    add_common(*fork, invocation);
    fork->add_option("session", fork_session, "Session ID")->required();

    std::string workspace_action;
    std::string workspace_path;
    CLI::App*   workspace = app.add_subcommand("workspace", "Workspace registry commands");
    workspace->add_option("action", workspace_action, "add | list");
    workspace->add_option("path", workspace_path, "Workspace path (for `add`)");

    std::string config_action;
    CLI::App*   config = app.add_subcommand("config", "Configuration commands");
    config->add_option("action", config_action, "path");

    std::vector<std::string> reversed(args.rbegin(), args.rend());
    try {
        app.parse(reversed);
    } catch (const CLI::CallForAllHelp&) {
        throw CLI::ParseError(app.help("", CLI::AppFormatMode::All), 0);
    } catch (const CLI::CallForHelp&) {
        throw CLI::ParseError(help_text(app, {run, list, show, replay, fork, workspace, config}), 0);
    }

    if (invocation.version_requested) {
        invocation.command = CliInvocation::Command::Version;
        return invocation;
    }
    if (run->parsed()) {
        invocation.command = CliInvocation::Command::Run;
        invocation.task    = run_task;
        return invocation;
    }
    if (list->parsed()) {
        invocation.command = CliInvocation::Command::List;
        return invocation;
    }
    if (show->parsed()) {
        invocation.command = CliInvocation::Command::Show;
        invocation.session = show_session;
        return invocation;
    }
    if (replay->parsed()) {
        invocation.command = CliInvocation::Command::Replay;
        invocation.session = replay_session;
        return invocation;
    }
    if (fork->parsed()) {
        invocation.command = CliInvocation::Command::Fork;
        invocation.session = fork_session;
        return invocation;
    }
    if (workspace->parsed()) {
        invocation.command = CliInvocation::Command::Workspace;
        if (!workspace_action.empty()) {
            invocation.workspace_args.push_back(workspace_action);
        }
        if (!workspace_path.empty()) {
            invocation.workspace_args.push_back(workspace_path);
        }
        return invocation;
    }
    if (config->parsed()) {
        invocation.command = CliInvocation::Command::Config;
        if (!config_action.empty()) {
            invocation.config_args.push_back(config_action);
        }
        return invocation;
    }

    invocation.command = CliInvocation::Command::Tui;
    return invocation;
}

CliInvocation parse_cli(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    return parse_cli(args);
}

int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    CliInvocation invocation;
    try {
        invocation = parse_cli(args);
    } catch (const CLI::ParseError& parse_error) {
        const int exit_code = parse_error.get_exit_code();
        if (exit_code == 0) {
            out << parse_error.what() << '\n';
        } else {
            err << parse_error.what() << '\n';
        }
        return exit_code;
    }

    if (invocation.command == CliInvocation::Command::Version) {
        out << "ymh " << YMH_VERSION << '\n';
        return 0;
    }

    if (invocation.command == CliInvocation::Command::Config) {
        return run_config_command(invocation, out, err);
    }

    const std::filesystem::path root = resolve_workspace(invocation);
    scaffold_for_invocation(invocation, root);

    Config config;
    try {
        config = load_invocation_config(invocation, root);
    } catch (const ConfigError& config_error) {
        err << "ymh: " << config_error.what() << '\n';
        return 2;
    }

    LoggingOptions logging;
    logging.level = parse_log_level(config.logging.level).value_or(LogLevel::Info);
    logging.log_prompts = config.logging.log_prompts;
    logging.color       = ::isatty(::fileno(stderr)) != 0;
    init_logging(logging);

    switch (invocation.command) {
        case CliInvocation::Command::Tui: {
            ui::UiRunOptions ui_options;
            ui_options.workspace = root;
            ui_options.config = config;
            ui_options.verbose = invocation.verbose;
            return ui::run_tui(ui_options);
        }

        case CliInvocation::Command::Run: {
            HeadlessOptions options;
            options.workspace = root;
            options.task      = invocation.task;
            options.config    = config;
            options.verbose   = invocation.verbose;
            options.out       = &out;
            options.err       = &err;
            if (!invocation.session.empty()) {
                options.resume = SessionId{invocation.session};
            }
            const HeadlessResult result = run_headless(options);
            return result.exit_code;
        }

        case CliInvocation::Command::List:
            return session_list(root, out, err);

        case CliInvocation::Command::Show:
            return session_show(root, invocation.session, out, err);

        case CliInvocation::Command::Replay:
            return session_replay(root, invocation.session, out, err);

        case CliInvocation::Command::Fork:
            return session_fork(root, invocation.session, out, err);

        case CliInvocation::Command::Workspace:
            err << "ymh: `ymh workspace` requires the workspace registry, which is not part of "
                   "the single-process MVP.\n";
            return 2;

        case CliInvocation::Command::Config:
            return run_config_command(invocation, out, err);

        case CliInvocation::Command::Version:
            out << "ymh " << YMH_VERSION << '\n';
            return 0;
    }
    return 2;
}

int run_cli(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    return run_cli(args, std::cout, std::cerr);
}

} // namespace ymh

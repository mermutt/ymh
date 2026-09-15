#include "ymh/cli/cli.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <unistd.h>

#include "ymh/cli/headless.hpp"
#include "ymh/cli/provider_factory.hpp"
#include "ymh/cli/session_cli.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/registry/workspace_cli.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
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

std::string host_usage() {
    return "ymh --host --workspace <uuid> --root <canonical-root> --socket <path>\n"
           "          [--config <path>] [--boot-id <uuid>]\n";
}

bool contains_host_flag(const std::vector<std::string>& args) {
    return std::find(args.begin(), args.end(), "--host") != args.end();
}

int run_host_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    std::string workspace_id;
    std::string root;
    std::string socket_path;
    std::string config_path;
    std::string boot_id;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        if (argument == "--host") {
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            out << host_usage();
            return 0;
        }
        auto take_value = [&](std::string& target) {
            if (index + 1 >= args.size()) {
                return false;
            }
            target = args[++index];
            return true;
        };
        if (argument == "--workspace") {
            if (!take_value(workspace_id)) { err << host_usage(); return 2; }
        } else if (argument == "--root") {
            if (!take_value(root)) { err << host_usage(); return 2; }
        } else if (argument == "--socket") {
            if (!take_value(socket_path)) { err << host_usage(); return 2; }
        } else if (argument == "--config") {
            if (!take_value(config_path)) { err << host_usage(); return 2; }
        } else if (argument == "--boot-id") {
            if (!take_value(boot_id)) { err << host_usage(); return 2; }
        } else {
            err << "ymh --host: unknown argument '" << argument << "'\n";
            return 2;
        }
    }

    if (workspace_id.empty() || root.empty()) {
        err << host_usage();
        return 2;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        err << "ymh --host: workspace root is not a directory: " << root << '\n';
        return static_cast<int>(HostExitCode::WorkspaceMissing);
    }
    const std::filesystem::path canonical = std::filesystem::canonical(root, error);
    if (error) {
        err << "ymh --host: cannot canonicalize " << root << '\n';
        return static_cast<int>(HostExitCode::WorkspaceMissing);
    }

    Config config;
    try {
        ConfigPaths paths;
        paths.global = config_path.empty() ? default_global_config_path()
                                           : std::filesystem::path{config_path};
        paths.workspace = workspace_config_path(canonical);
        config = load_config(paths);
    } catch (const ConfigError& config_error) {
        err << "ymh --host: " << config_error.what() << '\n';
        return 2;
    }

    HostConfig host_config;
    host_config.workspace      = WorkspaceId{workspace_id};
    host_config.workspace_root = canonical;
    host_config.socket_path    = socket_path.empty()
                                     ? canonical / ".ymh" / "host.sock"
                                     : std::filesystem::path{socket_path};
    host_config.log_sink       = canonical / ".ymh" / "host.log";
    host_config.registry       = default_registry_config();
    host_config.persistence.db_path   = canonical / ".ymh" / "sessions.db";
    host_config.persistence.lock_path = canonical / ".ymh" / "sessions.lock";
    host_config.config                = std::move(config);
    host_config.provider_factory      = make_provider_factory({});
    host_config.foreground            = false;
    if (!boot_id.empty()) {
        host_config.boot_id = HostBootId{boot_id};
    }

    std::unique_ptr<WorkspaceHost> host;
    try {
        host = WorkspaceHost::create(std::move(host_config));
    } catch (const HostError& host_error) {
        err << "ymh --host: " << host_error.what() << '\n';
        return static_cast<int>(HostExitCode::WorkspaceMissing);
    }
    return static_cast<int>(host->run());
}

int run_workspace_stop(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() != 2) {
        err << "ymh: usage: ymh workspace stop <workspace-id|path>\n";
        return 2;
    }

    std::unique_ptr<WorkspaceRegistry> registry;
    try {
        registry = WorkspaceRegistry::openReadOnly(default_registry_config());
    } catch (const std::exception& registry_error) {
        err << "ymh: registry unavailable: " << registry_error.what() << '\n';
        return 1;
    }

    std::optional<WorkspaceRecord> record = registry->findById(WorkspaceId{args[1]});
    if (!record.has_value()) {
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::canonical(args[1], error);
        if (!error) {
            record = registry->findByCanonicalPath(canonical);
        }
    }
    if (!record.has_value()) {
        err << "ymh: unknown workspace: " << args[1] << '\n';
        return 1;
    }
    if (!record->host.has_value()) {
        err << "ymh: workspace is not running\n";
        return 1;
    }

    try {
        protocol::HostConnection connection;
        connection.connect(record->host->socketPath.string());
        [[maybe_unused]] const protocol::HelloResult hello = connection.handshake(
            protocol::ServerProfile::Interactive,
            protocol::ClientInstanceId{generate_uuid_v4()});
        [[maybe_unused]] const nlohmann::json reply =
            connection.request(protocol::method::kHostShutdown, {{"reason", "workspace stop"}});
        connection.close();
    } catch (const std::exception& stop_error) {
        err << "ymh: cannot stop daemon: " << stop_error.what() << '\n';
        return 1;
    }
    out << "stopped " << record->id.value << '\n';
    return 0;
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

    CLI::App*   workspace = app.add_subcommand("workspace", "Workspace registry commands");
    CLI::App*   workspace_add = workspace->add_subcommand("add", "Register a workspace");
    std::string workspace_path;
    workspace_add->add_option("path", workspace_path, "Workspace path")->required();
    CLI::App*   workspace_list = workspace->add_subcommand("list", "List registered workspaces");
    CLI::App*   workspace_stop = workspace->add_subcommand("stop", "Stop a workspace daemon");
    std::string workspace_stop_target;
    workspace_stop->add_option("target", workspace_stop_target, "Workspace ID or path")->required();

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
        if (workspace_add->parsed()) {
            invocation.workspace_args.emplace_back("add");
            if (!workspace_path.empty()) {
                invocation.workspace_args.push_back(workspace_path);
            }
        } else if (workspace_list->parsed()) {
            invocation.workspace_args.emplace_back("list");
        } else if (workspace_stop->parsed()) {
            invocation.workspace_args.emplace_back("stop");
            invocation.workspace_args.push_back(workspace_stop_target);
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
    if (contains_host_flag(args)) {
        return run_host_command(args, out, err);
    }

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
            if (!invocation.workspace_args.empty() && invocation.workspace_args[0] == "stop") {
                return run_workspace_stop(invocation.workspace_args, out, err);
            }
            return run_workspace_command(invocation.workspace_args, out, err);

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

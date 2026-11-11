#include "ymh/registry/workspace_cli.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ymh/registry/liveness.hpp"
#include "ymh/registry/registry.hpp"

namespace ymh {
namespace {

int workspace_add(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() < 2 || args[1].empty()) {
        err << "usage: ymh workspace add <path>\n";
        return 2;
    }
    try {
        const auto registry = WorkspaceRegistry::open(default_registry_config());
        const WorkspaceRecord record =
            registry->registerWorkspace(std::filesystem::path{args[1]}, std::string{});
        registry->importSessionsFromDisk(record.id, record.canonicalPath);
        out << record.id.value << '\t' << record.canonicalPath.string() << '\n';
        return 0;
    } catch (const RegistryError& error) {
        err << "ymh: workspace add failed: " << error.what() << '\n';
        return 1;
    }
}

int workspace_list(std::ostream& out, std::ostream& err) {
    const RegistryConfig config = default_registry_config();
    // 111-D3: a machine that has never opened a workspace has no registry.db.
    // Listing the empty set is not an error (`openReadOnly` neither creates the
    // file nor bootstraps). A non-`ENOENT` stat error still surfaces below.
    std::error_code exists_error;
    const bool      exists = std::filesystem::exists(config.db_path, exists_error);
    if (!exists && !exists_error) {
        return 0;
    }
    try {
        const auto registry = WorkspaceRegistry::openReadOnly(config);
        for (const WorkspaceRecord& record : registry->listWorkspaces()) {
            // 111-D1: the host columns are a claim, not a liveness check. Report
            // the sidecar-flock truth beside them: `running` (claim + held
            // lock), `stale` (claim, lock free) or `stopped` (no claim).
            const HostLiveness liveness = registry->probeLiveness(record.id);
            const char*        state    = "stopped";
            if (liveness == HostLiveness::Live) {
                state = "running";
            } else if (liveness == HostLiveness::Stale) {
                state = "stale";
            }
            out << record.id.value << '\t' << record.canonicalPath.string() << '\t'
                << record.displayTitle << '\t' << registry->listSessions(record.id).size()
                << "\tstate=" << state;
            if (record.host.has_value()) {
                out << "\thost=" << record.host->pid;
            }
            out << '\n';
        }
        return 0;
    } catch (const RegistryError& error) {
        err << "ymh: workspace list failed: " << error.what() << '\n';
        return 1;
    }
}

// `workspace remove` accepts an unambiguous prefix of any workspace handle —
// its ID, its canonical path, or its display title — instead of only the full
// ID/path. Resolution order is exact ID, exact canonical path, exact display
// title, then a prefix; a prefix matching more than one workspace is reported
// as ambiguous (with the candidates) rather than guessed.
struct TargetResolution {
    std::optional<WorkspaceRecord> resolved;
    std::vector<WorkspaceRecord>   ambiguous;
};

TargetResolution resolve_remove_target(const WorkspaceRegistry& registry,
                                       const std::string&        target) {
    if (auto exact = registry.findById(WorkspaceId{target})) {
        return {std::move(exact), {}};
    }
    std::error_code             canonical_error;
    const std::filesystem::path canonical = std::filesystem::canonical(target, canonical_error);
    if (!canonical_error) {
        if (auto exact = registry.findByCanonicalPath(canonical)) {
            return {std::move(exact), {}};
        }
    }

    const std::vector<WorkspaceRecord> workspaces = registry.listWorkspaces();

    // An exact title wins before any prefix: with workspaces titled `proj` and
    // `proj2`, `remove proj` means the former, not an ambiguous match.
    std::vector<WorkspaceRecord> exact_title;
    for (const WorkspaceRecord& record : workspaces) {
        if (record.displayTitle == target) {
            exact_title.push_back(record);
        }
    }
    if (exact_title.size() == 1) {
        return {std::move(exact_title.front()), {}};
    }
    if (exact_title.size() > 1) {
        return {std::nullopt, std::move(exact_title)};
    }

    std::vector<WorkspaceRecord> prefixed;
    for (const WorkspaceRecord& record : workspaces) {
        if (record.id.value.starts_with(target) ||
            record.canonicalPath.string().starts_with(target) ||
            record.displayTitle.starts_with(target)) {
            prefixed.push_back(record);
        }
    }
    if (prefixed.size() == 1) {
        return {std::move(prefixed.front()), {}};
    }
    return {std::nullopt, std::move(prefixed)};
}

} // namespace

int run_workspace_command(const std::vector<std::string>& args, std::ostream& out,
                          std::ostream& err) {
    if (args.empty()) {
        err << "usage: ymh workspace <add|list|stop|reap|remove> ...\n";
        return 2;
    }
    if (args[0] == "add") {
        return workspace_add(args, out, err);
    }
    if (args[0] == "list") {
        return workspace_list(out, err);
    }
    err << "ymh: unknown workspace command: " << args[0] << '\n';
    return 2;
}

int run_workspace_remove(const std::vector<std::string>& args, bool assume_yes, std::ostream& out,
                         std::ostream& err) {
    if (args.size() != 2 || args[1].empty()) {
        err << "ymh: usage: ymh workspace remove <workspace-id|path|title-prefix> [--yes]\n";
        return 2;
    }

    std::unique_ptr<WorkspaceRegistry> registry;
    try {
        registry = WorkspaceRegistry::open(default_registry_config());
    } catch (const std::exception& registry_error) {
        err << "ymh: registry unavailable: " << registry_error.what() << '\n';
        return 3;
    }

    TargetResolution resolution = resolve_remove_target(*registry, args[1]);
    if (!resolution.ambiguous.empty()) {
        err << "ymh: " << resolution.ambiguous.size() << " workspaces start with '" << args[1]
            << "':\n";
        for (const WorkspaceRecord& candidate : resolution.ambiguous) {
            err << "  " << candidate.id.value << '\t' << candidate.canonicalPath.string() << '\t'
                << candidate.displayTitle << '\n';
        }
        err << "ymh: provide more characters to identify one workspace\n";
        return 2;
    }
    if (!resolution.resolved.has_value()) {
        err << "ymh: unknown workspace: " << args[1] << '\n';
        return 2;
    }
    const std::optional<WorkspaceRecord> record = std::move(resolution.resolved);

    // 112-D2: a live daemon owns the row; removal is only for a stopped (or
    // stale-claimed) workspace. `removeWorkspace` re-checks this under the lock.
    if (registry->probeLiveness(record->id) == HostLiveness::Live) {
        err << "ymh: workspace " << record->id.value
            << " is running; stop it first: ymh workspace stop " << record->id.value << '\n';
        return 1;
    }

    const std::vector<WorkspaceSessionRecord> sessions = registry->listSessions(record->id);
    out << record->id.value << '\t' << record->canonicalPath.string() << '\t' << sessions.size()
        << " session(s)\n";
    // 51-I24: the registry row and its junctions go; the workspace's `.ymh`
    // tree (including sessions.db) is left untouched.
    out << "keeping " << (record->canonicalPath / ".ymh").string() << '\n';

    if (!assume_yes) {
        if (::isatty(STDIN_FILENO) == 0) {
            err << "ymh: refusing to remove without --yes (stdin is not a terminal)\n";
            return 1;
        }
        out << "remove the registry row and its session junctions? [y/N] ";
        out.flush();
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES" &&
            answer != "Yes") {
            err << "ymh: declined\n";
            return 1;
        }
    }

    try {
        for (const WorkspaceSessionRecord& session : sessions) {
            registry->removeSession(record->id, session.sessionId);
        }
        registry->removeWorkspace(record->id);
    } catch (const RegistryError& error) {
        err << "ymh: workspace remove failed: " << error.what() << '\n';
        return 1;
    }
    out << "removed " << record->id.value << '\n';
    return 0;
}

} // namespace ymh

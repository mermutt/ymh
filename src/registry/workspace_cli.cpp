#include "ymh/registry/workspace_cli.hpp"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

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

} // namespace

int run_workspace_command(const std::vector<std::string>& args, std::ostream& out,
                          std::ostream& err) {
    if (args.empty()) {
        err << "usage: ymh workspace <add|list|stop|reap> ...\n";
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

} // namespace ymh

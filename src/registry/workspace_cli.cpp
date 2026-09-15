#include "ymh/registry/workspace_cli.hpp"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

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
    try {
        const auto registry = WorkspaceRegistry::openReadOnly(default_registry_config());
        for (const WorkspaceRecord& record : registry->listWorkspaces()) {
            out << record.id.value << '\t' << record.canonicalPath.string() << '\t'
                << record.displayTitle << '\t'
                << registry->listSessions(record.id).size();
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
        err << "usage: ymh workspace <add|list> [path]\n";
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

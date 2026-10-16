#include "ymh/registry/doctor_cli.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ymh/registry/doctor.hpp"
#include "ymh/registry/liveness.hpp"
#include "ymh/registry/process_scan.hpp"
#include "ymh/registry/registry.hpp"

namespace ymh {
namespace {

namespace fs = std::filesystem;

Canonicalizer production_canonicalizer() {
    return [](const fs::path& path) -> std::optional<fs::path> {
        std::error_code error;
        const fs::path  canonical = fs::canonical(path, error);
        if (error) {
            return std::nullopt;
        }
        return canonical;
    };
}

std::optional<WorkspaceRecord> resolve_target(WorkspaceRegistry& registry,
                                              const std::string&  target) {
    std::optional<WorkspaceRecord> record = registry.findById(WorkspaceId{target});
    if (!record.has_value()) {
        std::error_code error;
        const fs::path  canonical = fs::canonical(target, error);
        if (!error) {
            record = registry.findByCanonicalPath(canonical);
        }
    }
    return record;
}

WorkspaceObservation observe(const WorkspaceRecord& record) {
    WorkspaceObservation observation;
    observation.record = record;
    observation.lease = probeWorkspaceLock(record.canonicalPath, false);
    if (record.host.has_value() && !record.host->socketPath.empty()) {
        observation.socketOwnerPid = socketOwnerPid(record.host->socketPath);
    }
    return observation;
}

} // namespace

int run_doctor(const DoctorOptions& options, std::ostream& out, std::ostream& err) {
    std::unique_ptr<WorkspaceRegistry> registry;
    try {
        registry = WorkspaceRegistry::openReadOnly(default_registry_config());
    } catch (const std::exception& error) {
        err << "ymh: registry unavailable: " << error.what() << '\n';
        return 3;
    }

    std::vector<WorkspaceRecord> rows;
    if (!options.workspace.empty()) {
        std::optional<WorkspaceRecord> record = resolve_target(*registry, options.workspace);
        if (!record.has_value()) {
            err << "ymh: unknown workspace: " << options.workspace << '\n';
            return 2;
        }
        rows.push_back(std::move(*record));
    } else {
        rows = registry->listWorkspaces();
    }

    const ProcessTable          table = readProcessTableChecked();
    const Canonicalizer         canon = production_canonicalizer();
    std::vector<WorkspaceObservation> observations;
    observations.reserve(rows.size());
    for (const WorkspaceRecord& record : rows) {
        observations.push_back(observe(record));
    }

    const DoctorReport report = diagnose(observations, table, canon);
    if (options.json) {
        render_doctor_json(report, out);
    } else {
        render_doctor_human(report, out);
    }
    return doctor_exit_code(report);
}

int run_workspace_reap(const std::vector<std::string>& args, bool assume_yes, std::ostream& out,
                       std::ostream& err) {
    if (args.size() != 2) {
        err << "ymh: usage: ymh workspace reap <workspace-id|path> [--yes]\n";
        return 2;
    }

    std::unique_ptr<WorkspaceRegistry> registry;
    try {
        registry = WorkspaceRegistry::open(default_registry_config());
    } catch (const std::exception& error) {
        err << "ymh: registry unavailable: " << error.what() << '\n';
        return 3;
    }

    std::optional<WorkspaceRecord> record = resolve_target(*registry, args[1]);
    if (!record.has_value()) {
        err << "ymh: unknown workspace: " << args[1] << '\n';
        return 2;
    }

    const ProcessTable  table = readProcessTableChecked();
    const Canonicalizer canon = production_canonicalizer();
    const std::vector<WorkspaceObservation> observations{observe(*record)};
    render_doctor_human(diagnose(observations, table, canon), out);

    const WorkspaceLockProbe lease = probeWorkspaceLock(record->canonicalPath);
    if (lease.held) {
        err << "ymh: host claim for " << record->id.value
            << " is held by a live daemon; run `ymh workspace stop " << record->id.value << "`\n";
        return 1;
    }

    if (!assume_yes) {
        if (::isatty(STDIN_FILENO) == 0) {
            err << "ymh: refusing to reap without --yes (stdin is not a terminal)\n";
            return 1;
        }
        out << "clear host claim for " << record->id.value << "? [y/N] ";
        out.flush();
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES" &&
            answer != "Yes") {
            err << "ymh: declined\n";
            return 1;
        }
    }

    const bool cleared = registry->reapHost(record->id);
    if (cleared) {
        out << "reaped " << record->id.value << '\n';
    } else {
        out << "nothing to reap " << record->id.value << '\n';
    }
    return 0;
}

} // namespace ymh

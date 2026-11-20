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
#include "ymh/registry/workspace_cli.hpp"

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
        WorkspaceTargetResolution resolution =
            resolve_workspace_target(*registry, options.workspace);
        if (!resolution.ambiguous.empty()) {
            err << "ymh: " << resolution.ambiguous.size() << " workspaces start with '"
                << options.workspace << "':\n";
            for (const WorkspaceRecord& candidate : resolution.ambiguous) {
                err << "  " << candidate.id.value << '\t' << candidate.canonicalPath.string()
                    << '\t' << candidate.displayTitle << '\n';
            }
            return 2;
        }
        if (!resolution.resolved.has_value()) {
            err << "ymh: unknown workspace: " << options.workspace << '\n';
            return 2;
        }
        rows.push_back(std::move(*resolution.resolved));
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

} // namespace ymh

#include "ymh/registry/doctor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kIndent = 2;

std::optional<std::string> argv_flag(const std::vector<std::string>& argv,
                                     const std::string&              flag) {
    for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
        if (argv[index] == flag) {
            return argv[index + 1];
        }
    }
    return std::nullopt;
}

const ProcessEntry* find_entry(const ProcessTable& table, std::int32_t pid) {
    if (pid == 0) {
        return nullptr;
    }
    for (const ProcessEntry& entry : table.entries) {
        if (entry.pid == pid) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<fs::path> canonicalized(const Canonicalizer& canon, const fs::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    return canon(path);
}

bool at_least_medium(DoctorSeverity severity) {
    return severity != DoctorSeverity::Low;
}

std::string stop_command(const WorkspaceId& id) {
    return "ymh workspace stop " + id.value + " --force";
}

std::string reap_command(const WorkspaceId& id) {
    return "ymh workspace reap " + id.value + " --yes";
}

std::string add_command(const fs::path& root) {
    return "ymh workspace add " + root.string();
}

std::optional<WorkspaceId> registered_for_root(
    std::span<const WorkspaceObservation> observations, const fs::path& root,
    const WorkspaceId& self) {
    if (root.empty()) {
        return std::nullopt;
    }
    for (const WorkspaceObservation& observation : observations) {
        if (observation.record.id.value == self.value) {
            continue;
        }
        if (observation.record.canonicalPath == root) {
            return observation.record.id;
        }
    }
    return std::nullopt;
}

std::string remediation_for(DoctorFindingCode code, bool held, const fs::path& observedRoot,
                            const WorkspaceId& self,
                            std::span<const WorkspaceObservation> observations) {
    const auto resolved_or_self = [&]() -> WorkspaceId {
        if (const auto other = registered_for_root(observations, observedRoot, self);
            other.has_value()) {
            return *other;
        }
        return self;
    };

    switch (code) {
        case DoctorFindingCode::DeadPid:
        case DoctorFindingCode::LeaseMissing:
        case DoctorFindingCode::LeasePidMismatch:
        case DoctorFindingCode::LeaseUnreadable:
        case DoctorFindingCode::LeaseBootMismatch:
        case DoctorFindingCode::BootMismatch:
            return held ? stop_command(resolved_or_self()) : reap_command(self);
        case DoctorFindingCode::ForeignPid:
            return held ? stop_command(self) : reap_command(self);
        case DoctorFindingCode::RootMismatch:
        case DoctorFindingCode::LeaseRootMismatch:
        case DoctorFindingCode::SocketOwnerMismatch:
        case DoctorFindingCode::WorkspaceIdMismatch:
            if (held) {
                return stop_command(resolved_or_self());
            }
            if (observedRoot.empty()) {
                return {};
            }
            if (const auto other = registered_for_root(observations, observedRoot, self);
                other.has_value()) {
                return stop_command(*other);
            }
            return add_command(observedRoot);
        case DoctorFindingCode::OrphanDaemon:
            return stop_command(self);
        case DoctorFindingCode::SocketArgMismatch:
            if (observedRoot.empty()) {
                return {};
            }
            if (registered_for_root(observations, observedRoot, self).has_value()) {
                return stop_command(self);
            }
            return add_command(observedRoot);
        case DoctorFindingCode::SocketOwnerUnknown:
        case DoctorFindingCode::ProcessTableUnavailable:
            return {};
    }
    return {};
}

std::string_view severity_label(DoctorSeverity severity) {
    switch (severity) {
        case DoctorSeverity::Low:
            return "LOW";
        case DoctorSeverity::Medium:
            return "MEDIUM";
        case DoctorSeverity::High:
            return "HIGH";
    }
    return "LOW";
}

std::string_view state_token(DoctorWorkspaceReport::State state) {
    switch (state) {
        case DoctorWorkspaceReport::State::Clean:
            return "clean";
        case DoctorWorkspaceReport::State::NotRunning:
            return "not-running";
        case DoctorWorkspaceReport::State::Corrupt:
            return "corrupt";
    }
    return "clean";
}

} // namespace

std::string_view doctor_code_token(DoctorFindingCode code) noexcept {
    switch (code) {
        case DoctorFindingCode::DeadPid:
            return "dead-pid";
        case DoctorFindingCode::ForeignPid:
            return "foreign-pid";
        case DoctorFindingCode::RootMismatch:
            return "root-mismatch";
        case DoctorFindingCode::WorkspaceIdMismatch:
            return "workspace-id-mismatch";
        case DoctorFindingCode::BootMismatch:
            return "boot-mismatch";
        case DoctorFindingCode::SocketArgMismatch:
            return "socket-arg-mismatch";
        case DoctorFindingCode::LeaseMissing:
            return "lease-missing";
        case DoctorFindingCode::LeasePidMismatch:
            return "lease-pid-mismatch";
        case DoctorFindingCode::LeaseUnreadable:
            return "lease-unreadable";
        case DoctorFindingCode::LeaseRootMismatch:
            return "lease-root-mismatch";
        case DoctorFindingCode::LeaseBootMismatch:
            return "lease-boot-mismatch";
        case DoctorFindingCode::OrphanDaemon:
            return "orphan-daemon";
        case DoctorFindingCode::SocketOwnerMismatch:
            return "socket-owner-mismatch";
        case DoctorFindingCode::SocketOwnerUnknown:
            return "socket-owner-unknown";
        case DoctorFindingCode::ProcessTableUnavailable:
            return "process-table-unavailable";
    }
    return "unknown";
}

std::string_view doctor_severity_token(DoctorSeverity severity) noexcept {
    switch (severity) {
        case DoctorSeverity::Low:
            return "low";
        case DoctorSeverity::Medium:
            return "medium";
        case DoctorSeverity::High:
            return "high";
    }
    return "low";
}

DoctorReport diagnose(std::span<const WorkspaceObservation> observations,
                      const ProcessTable& process_table, const Canonicalizer& canon) {
    std::vector<WorkspaceObservation> ordered(observations.begin(), observations.end());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const WorkspaceObservation& left, const WorkspaceObservation& right) {
                         return left.record.canonicalPath < right.record.canonicalPath;
                     });

    DoctorReport report;
    report.processTableComplete = process_table.complete;

    for (const WorkspaceObservation& observation : ordered) {
        const WorkspaceRecord& record = observation.record;
        const bool             have_host = record.host.has_value();
        const HostClaim*       claim = have_host ? &*record.host : nullptr;
        const std::int32_t     p_reg = claim != nullptr ? claim->pid : 0;
        const std::int32_t     p_lease = observation.lease.pid.value_or(0);
        const std::int32_t     p_sock = observation.socketOwnerPid.value_or(0);

        const ProcessEntry* entry_reg = find_entry(process_table, p_reg);
        const ProcessEntry* entry_lease = find_entry(process_table, p_lease);
        const bool          present_reg = entry_reg != nullptr;
        const bool          host_reg = present_reg && is_ymh_host_process(entry_reg->argv);

        DoctorWorkspaceReport workspace;
        workspace.id = record.id;
        workspace.canonicalPath = record.canonicalPath;
        workspace.recordedPid = p_reg;
        workspace.bootId = claim != nullptr ? std::optional<HostBootId>{claim->bootId} : std::nullopt;
        workspace.socket = claim != nullptr ? claim->socketPath : fs::path{};
        workspace.lease = observation.lease;
        workspace.socketOwnerPid = observation.socketOwnerPid;

        const auto emit = [&](DoctorFindingCode code, DoctorSeverity severity,
                              std::int32_t recorded_pid, std::int32_t observed_pid,
                              const fs::path& observed_root, std::string detail, bool held) {
            DoctorFinding finding;
            finding.code = code;
            finding.severity = severity;
            finding.recordedPid = recorded_pid;
            finding.observedPid = observed_pid;
            finding.observedRoot = observed_root;
            finding.detail = std::move(detail);
            finding.remediation =
                remediation_for(code, held, observed_root, record.id, ordered);
            workspace.findings.push_back(std::move(finding));
        };

        if (process_table.complete && p_reg != 0 && !present_reg) {
            emit(DoctorFindingCode::DeadPid, DoctorSeverity::Medium, p_reg, 0, {},
                 "recorded pid " + std::to_string(p_reg) + " is not running",
                 observation.lease.held);
        }

        if (process_table.complete && p_reg != 0 && present_reg && !host_reg) {
            emit(DoctorFindingCode::ForeignPid, DoctorSeverity::High, p_reg, p_reg, {},
                 "recorded pid " + std::to_string(p_reg) + " is not a ymh host",
                 observation.lease.held);
        }

        if (p_reg != 0 && host_reg) {
            const std::optional<fs::path> root = canonicalized(canon, argv_flag(entry_reg->argv,
                                                                                 "--root")
                                                                         .value_or(std::string{}));
            if (root.has_value() && *root != record.canonicalPath) {
                emit(DoctorFindingCode::RootMismatch, DoctorSeverity::High, p_reg, p_reg, *root,
                     "recorded pid " + std::to_string(p_reg) + " is a ymh host for root " +
                         root->string() + " (expected " + record.canonicalPath.string() + ")",
                     observation.lease.held);
            }
        }

        if (p_reg != 0 && host_reg) {
            const std::optional<std::string> wid = argv_flag(entry_reg->argv, "--workspace");
            if (wid.has_value() && *wid != record.id.value) {
                emit(DoctorFindingCode::WorkspaceIdMismatch, DoctorSeverity::High, p_reg, p_reg, {},
                     "recorded pid " + std::to_string(p_reg) + " records workspace " + *wid +
                         " (expected " + record.id.value + ")",
                     observation.lease.held);
            }
        }

        if (p_reg != 0 && host_reg && claim != nullptr) {
            const std::optional<std::string> bid = argv_flag(entry_reg->argv, "--boot-id");
            if (bid.has_value() && *bid != claim->bootId.value) {
                emit(DoctorFindingCode::BootMismatch, DoctorSeverity::Medium, p_reg, p_reg, {},
                     "recorded pid " + std::to_string(p_reg) + " boot id " + *bid +
                         " != recorded boot id " + claim->bootId.value,
                     observation.lease.held);
            }
        }

        if (p_reg != 0 && host_reg && claim != nullptr && !claim->socketPath.empty()) {
            const std::optional<std::string> sock_arg = argv_flag(entry_reg->argv, "--socket");
            if (sock_arg.has_value()) {
                const std::optional<fs::path> observed = canonicalized(canon, *sock_arg);
                const std::optional<fs::path> expected = canonicalized(canon, claim->socketPath);
                if (observed.has_value() && expected.has_value() && *observed != *expected) {
                    emit(DoctorFindingCode::SocketArgMismatch, DoctorSeverity::Medium, p_reg, p_reg,
                         {}, "recorded pid " + std::to_string(p_reg) + " socket arg " +
                                 *sock_arg + " != recorded socket " +
                                 claim->socketPath.string(),
                         observation.lease.held);
                }
            }
        }

        if (p_reg != 0 && have_host && !observation.lease.held) {
            emit(DoctorFindingCode::LeaseMissing, DoctorSeverity::Medium, p_reg, 0, {},
                 "registry claims pid " + std::to_string(p_reg) + " but the sidecar lock is free",
                 observation.lease.held);
        }

        if (have_host && observation.lease.held && p_lease != 0 && p_lease != p_reg) {
            emit(DoctorFindingCode::LeasePidMismatch, DoctorSeverity::Medium, p_reg, p_lease, {},
                 "registry host_pid " + std::to_string(p_reg) + " != lease diagnostic pid " +
                     std::to_string(p_lease),
                 observation.lease.held);
        }

        if (have_host && observation.lease.held && !observation.lease.pid.has_value()) {
            emit(DoctorFindingCode::LeaseUnreadable, DoctorSeverity::Low, p_reg, 0, {},
                 "sidecar lock is held but the diagnostic is unreadable", observation.lease.held);
        }

        if (have_host && p_lease != 0 && entry_lease != nullptr &&
            is_ymh_host_process(entry_lease->argv)) {
            const std::optional<fs::path> root = canonicalized(
                canon, argv_flag(entry_lease->argv, "--root").value_or(std::string{}));
            if (root.has_value() && *root != record.canonicalPath) {
                emit(DoctorFindingCode::LeaseRootMismatch, DoctorSeverity::High, p_reg, p_lease,
                     *root,
                     "lease pid " + std::to_string(p_lease) + " is a ymh host for root " +
                         root->string() + " (expected " + record.canonicalPath.string() + ")",
                     observation.lease.held);
            }
        }

        if (have_host && claim != nullptr && observation.lease.held && entry_lease != nullptr) {
            const std::optional<std::string> bid = argv_flag(entry_lease->argv, "--boot-id");
            if (bid.has_value() && *bid != claim->bootId.value) {
                emit(DoctorFindingCode::LeaseBootMismatch, DoctorSeverity::Medium, p_reg, p_lease,
                     {},
                     "lease boot id " + *bid + " != recorded boot id " + claim->bootId.value,
                     observation.lease.held);
            }
        }

        if (process_table.complete) {
            for (const ProcessEntry& entry : process_table.entries) {
                if (!is_ymh_host_process(entry.argv)) {
                    continue;
                }
                if (entry.pid == p_reg || entry.pid == p_lease) {
                    continue;
                }
                const std::optional<fs::path> root =
                    canonicalized(canon,
                                  argv_flag(entry.argv, "--root").value_or(std::string{}));
                const bool root_matches = root.has_value() && *root == record.canonicalPath;
                bool       socket_matches = false;
                if (claim != nullptr && !claim->socketPath.empty()) {
                    const std::optional<std::string> sock_arg = argv_flag(entry.argv, "--socket");
                    if (sock_arg.has_value()) {
                        const std::optional<fs::path> observed = canonicalized(canon, *sock_arg);
                        const std::optional<fs::path> expected =
                            canonicalized(canon, claim->socketPath);
                        socket_matches = observed.has_value() && expected.has_value() &&
                                         *observed == *expected;
                    }
                }
                if (!root_matches && !socket_matches) {
                    continue;
                }
                const fs::path observed_root = root.value_or(fs::path{});
                emit(DoctorFindingCode::OrphanDaemon, DoctorSeverity::Medium, p_reg, entry.pid,
                     observed_root,
                     "live ymh host pid " + std::to_string(entry.pid) + " for root " +
                         (observed_root.empty() ? std::string{"<unknown>"}
                                                : observed_root.string()) +
                         " is not recorded",
                     observation.lease.held);
            }
        }

        if (process_table.complete && p_sock != 0 && p_reg != 0 && p_sock != p_reg) {
            emit(DoctorFindingCode::SocketOwnerMismatch, DoctorSeverity::High, p_reg, p_sock, {},
                 "socket " + (claim != nullptr ? claim->socketPath.string() : std::string{}) +
                     " is held by pid " + std::to_string(p_sock) + " (expected recorded pid " +
                     std::to_string(p_reg) + ")",
                 observation.lease.held);
        }

        if (claim != nullptr && !claim->socketPath.empty() && !observation.socketOwnerPid.has_value()) {
            emit(DoctorFindingCode::SocketOwnerUnknown, DoctorSeverity::Low, p_reg, 0, {},
                 "socket " + claim->socketPath.string() + " owner is unknown",
                 observation.lease.held);
        }

        if (!process_table.complete) {
            emit(DoctorFindingCode::ProcessTableUnavailable, DoctorSeverity::Low, p_reg, 0, {},
                 "process table unavailable (/proc unreadable); process checks skipped",
                 observation.lease.held);
        }

        bool corrupt = false;
        for (const DoctorFinding& finding : workspace.findings) {
            if (at_least_medium(finding.severity)) {
                corrupt = true;
                break;
            }
        }
        if (corrupt) {
            workspace.state = DoctorWorkspaceReport::State::Corrupt;
        } else if (!have_host) {
            workspace.state = DoctorWorkspaceReport::State::NotRunning;
        } else {
            workspace.state = DoctorWorkspaceReport::State::Clean;
        }

        for (const DoctorFinding& finding : workspace.findings) {
            switch (finding.severity) {
                case DoctorSeverity::High:
                    ++report.high;
                    break;
                case DoctorSeverity::Medium:
                    ++report.medium;
                    break;
                case DoctorSeverity::Low:
                    ++report.low;
                    break;
            }
        }

        report.workspaces.push_back(std::move(workspace));
    }

    report.clean = report.high == 0 && report.medium == 0;
    return report;
}

int doctor_exit_code(const DoctorReport& report) noexcept {
    if (!report.processTableComplete) {
        return 3;
    }
    return report.clean ? 0 : 1;
}

void render_doctor_human(const DoctorReport& report, std::ostream& out) {
    for (const DoctorWorkspaceReport& workspace : report.workspaces) {
        out << workspace.id.value << '\t' << workspace.canonicalPath.string() << '\t'
            << state_token(workspace.state) << '\n';
        for (const DoctorFinding& finding : workspace.findings) {
            out << "  " << severity_label(finding.severity) << '\t'
                << doctor_code_token(finding.code) << '\t' << finding.detail << '\n';
            if (!finding.remediation.empty()) {
                out << "    fix: " << finding.remediation << '\n';
            }
        }
    }
    if (report.workspaces.empty()) {
        out << "doctor: 0 workspaces\n";
        return;
    }
    out << "doctor: " << report.workspaces.size() << " workspaces, " << report.high << " high, "
        << report.medium << " medium, " << report.low << " low\n";
}

void render_doctor_json(const DoctorReport& report, std::ostream& out) {
    nlohmann::ordered_json root;
    root["schema"] = 1;
    root["clean"] = report.clean;
    root["process_table_complete"] = report.processTableComplete;
    root["counts"] = {{"high", report.high}, {"medium", report.medium}, {"low", report.low}};

    nlohmann::ordered_json workspaces = nlohmann::ordered_json::array();
    for (const DoctorWorkspaceReport& workspace : report.workspaces) {
        nlohmann::ordered_json json_workspace;
        json_workspace["id"] = workspace.id.value;
        json_workspace["canonical_path"] = workspace.canonicalPath.string();
        json_workspace["state"] = state_token(workspace.state);
        json_workspace["recorded_pid"] = workspace.recordedPid;
        if (workspace.bootId.has_value()) {
            json_workspace["host_boot_id"] = workspace.bootId->value;
        }
        if (!workspace.socket.empty()) {
            json_workspace["socket"] = workspace.socket.string();
        }

        nlohmann::ordered_json lease;
        lease["held"] = workspace.lease.held;
        if (workspace.lease.held) {
            if (workspace.lease.pid.has_value()) {
                lease["diagnostic_pid"] = *workspace.lease.pid;
            }
            if (workspace.lease.bootId.has_value()) {
                lease["diagnostic_boot_id"] = *workspace.lease.bootId;
            }
        }
        json_workspace["lease"] = lease;

        if (workspace.socketOwnerPid.has_value()) {
            json_workspace["socket_owner_pid"] = *workspace.socketOwnerPid;
        }

        nlohmann::ordered_json findings = nlohmann::ordered_json::array();
        for (const DoctorFinding& finding : workspace.findings) {
            nlohmann::ordered_json json_finding;
            json_finding["code"] = doctor_code_token(finding.code);
            json_finding["severity"] = doctor_severity_token(finding.severity);
            json_finding["recorded_pid"] = finding.recordedPid;
            json_finding["observed_pid"] = finding.observedPid;
            if (!finding.observedRoot.empty()) {
                json_finding["observed_root"] = finding.observedRoot.string();
            }
            json_finding["detail"] = finding.detail;
            if (!finding.remediation.empty()) {
                json_finding["remediation"] = finding.remediation;
            }
            findings.push_back(std::move(json_finding));
        }
        json_workspace["findings"] = findings;
        workspaces.push_back(std::move(json_workspace));
    }
    root["workspaces"] = workspaces;
    out << root.dump(static_cast<int>(kIndent)) << '\n';
}

} // namespace ymh

#pragma once

// 88-A5 (spec 88 §4, §7): the injectable, pure ownership-integrity check core and
// its renderers. `diagnose` performs no I/O of its own: process data is a
// `ProcessTable` argument and the sole filesystem step (path canonicalization) is
// the injected `Canonicalizer` (88-D2/DOC11). It flags a recorded owner whose
// `/proc/<pid>/cmdline --root` names a different workspace root -- the louvre
// incident -- without sending a signal or mutating anything (88-D1/DOC2).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/registry/liveness.hpp"
#include "ymh/registry/process_scan.hpp"
#include "ymh/registry/registry.hpp"

namespace ymh {

enum class DoctorSeverity : std::uint8_t { Low, Medium, High };

enum class DoctorFindingCode : std::uint8_t {
    DeadPid, ForeignPid, RootMismatch, WorkspaceIdMismatch, BootMismatch,
    SocketArgMismatch, LeaseMissing, LeasePidMismatch, LeaseUnreadable,
    LeaseRootMismatch, LeaseBootMismatch, OrphanDaemon,
    SocketOwnerMismatch, SocketOwnerUnknown, ProcessTableUnavailable,
};

// 88-D2/M1: the core's ONLY external dependency. Production: a
// `std::filesystem::canonical` wrapper (nullopt on error). Tests: identity or a
// stub map, so synthetic roots canonicalize deterministically.
using Canonicalizer =
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)>;

// One registry row plus its two observations, built by the driver. Pure core
// input. `lease` comes from `probeWorkspaceLock(root, false)` and `socketOwnerPid`
// from `socketOwnerPid(record.host->socketPath)`; both may be unknown.
struct WorkspaceObservation {
    WorkspaceRecord             record;
    WorkspaceLockProbe          lease;
    std::optional<std::int32_t> socketOwnerPid;
};

struct DoctorFinding {
    DoctorFindingCode     code        = DoctorFindingCode::DeadPid;
    DoctorSeverity        severity    = DoctorSeverity::Low;
    std::int32_t          recordedPid = 0;  // 0 == none
    std::int32_t          observedPid = 0;  // 0 == unknown/none
    std::filesystem::path observedRoot;     // empty == unknown
    std::string           detail;           // one line, ASCII
    std::string           remediation;      // suggested command, or empty
};

// One reported workspace: the row facts plus its findings in the frozen check
// order (sec 4). Every registry row yields exactly one entry, so a clean row is
// `state == Clean` with an empty `findings`.
struct DoctorWorkspaceReport {
    enum class State : std::uint8_t { Clean, NotRunning, Corrupt };
    WorkspaceId                 id;
    std::filesystem::path       canonicalPath;
    State                       state        = State::Clean;
    std::int32_t                recordedPid  = 0;  // 0 == no host claim
    std::optional<HostBootId>   bootId;            // when the row has a claim
    std::filesystem::path       socket;            // recorded host_socket
    WorkspaceLockProbe          lease;             // {held, pid?, bootId?} snapshot
    std::optional<std::int32_t> socketOwnerPid;    // nullopt == unknown
    std::vector<DoctorFinding>  findings;          // frozen check order
};

struct DoctorReport {
    std::vector<DoctorWorkspaceReport> workspaces;  // canonical_path ASC (88-D6)
    bool        processTableComplete = true;        // false == C12 / exit 3
    bool        clean                = true;        // no >= MEDIUM finding anywhere
    std::size_t high                 = 0;
    std::size_t medium               = 0;
    std::size_t low                  = 0;
};

// Pure over its arguments (88-D2/DOC11): the core performs no I/O of its own.
// `processTable` is one `readProcessTableChecked()` snapshot; `canon` is the
// injected canonicalizer; `observations` is one per registry row. Returns one
// report entry per observation, ordered by `canonicalPath` ASC, with findings in
// the frozen check order (sec 4). Total: never throws (88-D13).
// Caller: run_doctor (production driver); errata88_doctor_test (fixtures).
[[nodiscard]] DoctorReport diagnose(
    std::span<const WorkspaceObservation> observations,
    const ProcessTable&                   processTable,
    const Canonicalizer&                  canon);

// 88-D9 exit mapping from a report, pure. Returns 3 when the process table was
// unavailable (C12), 1 when any finding is >= MEDIUM, else 0. Caller: run_doctor.
[[nodiscard]] int doctor_exit_code(const DoctorReport& report) noexcept;

// Stable lowercase token for `--json` and the human code column. Pure.
// Caller: render_doctor_json / render_doctor_human.
[[nodiscard]] std::string_view doctor_code_token(DoctorFindingCode code) noexcept;
[[nodiscard]] std::string_view doctor_severity_token(DoctorSeverity severity) noexcept;

// Pure renderers of the whole report (both already carry id/canonicalPath).
// Callers: run_doctor (both); errata88_doctor_test (golden).
void render_doctor_human(const DoctorReport& report, std::ostream& out);
void render_doctor_json(const DoctorReport& report, std::ostream& out);

} // namespace ymh

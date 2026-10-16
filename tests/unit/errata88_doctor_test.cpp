#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ymh/registry/doctor.hpp"
#include "ymh/registry/doctor_cli.hpp"
#include "ymh/registry/liveness.hpp"
#include "ymh/registry/process_scan.hpp"
#include "ymh/registry/registry.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

const std::string kLouvreId = "11111111-1111-4111-8111-111111111111";
const std::string kTediId   = "22222222-2222-4222-8222-222222222222";
const fs::path    kLouvre{"/louvre"};
const fs::path    kTedi{"/bb_tedi"};
const fs::path    kLouvreSock{"/louvre/.ymh/host.sock"};
const fs::path    kTediSock{"/bb_tedi/.ymh/host.sock"};

const Canonicalizer kIdentity = [](const fs::path& path) -> std::optional<fs::path> {
    return path;
};

ProcessEntry mkHost(std::int32_t pid, const fs::path& root, const std::string& id,
                    const fs::path& sock, std::optional<std::string> boot = std::nullopt) {
    ProcessEntry entry;
    entry.pid = pid;
    entry.argv = {"ymh", "--host", "--workspace", id, "--root", root.string(), "--socket",
                  sock.string()};
    if (boot.has_value()) {
        entry.argv.push_back("--boot-id");
        entry.argv.push_back(*boot);
    }
    return entry;
}

WorkspaceRecord mkRow(const std::string& id, const fs::path& path,
                      std::optional<std::int32_t> pid  = std::nullopt,
                      std::optional<std::string>  boot = std::nullopt,
                      std::optional<fs::path>     sock = std::nullopt) {
    WorkspaceRecord row;
    row.id = WorkspaceId{id};
    row.canonicalPath = path;
    if (pid.has_value()) {
        HostClaim claim;
        claim.workspace = row.id;
        claim.pid = *pid;
        claim.bootId = HostBootId{boot.value_or("boot")};
        claim.socketPath = sock.value_or(fs::path{});
        row.host = claim;
    }
    return row;
}

ProcessTable make_table(std::vector<ProcessEntry> entries, bool complete = true) {
    ProcessTable process_table;
    process_table.complete = complete;
    process_table.entries = std::move(entries);
    return process_table;
}

WorkspaceObservation observe(const WorkspaceRecord& row, bool held,
                             std::optional<std::int32_t> lease_pid = std::nullopt,
                             std::optional<std::string>  lease_boot = std::nullopt,
                             std::optional<std::int32_t> sock_owner = std::nullopt) {
    WorkspaceObservation observation;
    observation.record = row;
    observation.lease.held = held;
    observation.lease.pid = lease_pid;
    observation.lease.bootId = lease_boot;
    observation.socketOwnerPid = sock_owner;
    return observation;
}

const DoctorFinding* find_finding(const DoctorWorkspaceReport& workspace, DoctorFindingCode code) {
    for (const DoctorFinding& finding : workspace.findings) {
        if (finding.code == code) {
            return &finding;
        }
    }
    return nullptr;
}

std::size_t count_findings(const DoctorWorkspaceReport& workspace, DoctorFindingCode code) {
    std::size_t count = 0;
    for (const DoctorFinding& finding : workspace.findings) {
        if (finding.code == code) {
            ++count;
        }
    }
    return count;
}

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                (prefix + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string                name_;
    std::optional<std::string> previous_;
};

int hold_file_lock(const fs::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void release_file_lock(int fd) {
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
}

void claim_stale_host(WorkspaceRegistry& registry, const WorkspaceRecord& row) {
    HostClaim claim;
    claim.workspace = row.id;
    claim.pid = 123456;
    claim.bootId = HostBootId{"stale-boot"};
    claim.socketPath = row.canonicalPath / ".ymh" / "host.sock";
    registry.claimHost(claim);
}

} // namespace

TEST(Errata88DoctorTest, CleanWhenRecordedOwnerMatches) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "B", 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    ASSERT_EQ(report.workspaces.size(), 1U);
    EXPECT_EQ(report.workspaces[0].state, DoctorWorkspaceReport::State::Clean);
    EXPECT_TRUE(report.workspaces[0].findings.empty());
    EXPECT_TRUE(report.clean);
}

TEST(Errata88DoctorTest, RootMismatchIsHigh) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42);
    const ProcessTable    process_table = make_table({mkHost(42, kTedi, kTediId, kTediSock)});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    ASSERT_EQ(report.workspaces.size(), 1U);
    const DoctorWorkspaceReport& workspace = report.workspaces[0];
    EXPECT_EQ(count_findings(workspace, DoctorFindingCode::RootMismatch), 1U);
    const DoctorFinding* mismatch = find_finding(workspace, DoctorFindingCode::RootMismatch);
    ASSERT_NE(mismatch, nullptr);
    EXPECT_EQ(mismatch->severity, DoctorSeverity::High);
    EXPECT_EQ(mismatch->observedRoot, kTedi);
    EXPECT_NE(mismatch->remediation.find("workspace stop"), std::string::npos);
    EXPECT_EQ(workspace.state, DoctorWorkspaceReport::State::Corrupt);
    EXPECT_FALSE(report.clean);
}

TEST(Errata88DoctorTest, LeaseRootMismatchIsHigh) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "B"),
                    mkHost(99, kTedi, kTediId, kTediSock)});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 99, std::nullopt, 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    ASSERT_EQ(report.workspaces.size(), 1U);
    const DoctorWorkspaceReport& workspace = report.workspaces[0];
    const DoctorFinding* mismatch = find_finding(workspace, DoctorFindingCode::LeaseRootMismatch);
    ASSERT_NE(mismatch, nullptr);
    EXPECT_EQ(mismatch->severity, DoctorSeverity::High);
    EXPECT_EQ(mismatch->observedRoot, kTedi);
    EXPECT_EQ(count_findings(workspace, DoctorFindingCode::RootMismatch), 0U);
    EXPECT_EQ(count_findings(workspace, DoctorFindingCode::ForeignPid), 0U);
}

TEST(Errata88DoctorTest, DeadPidIsMedium) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42);
    const ProcessTable    process_table = make_table({});
    const std::vector<WorkspaceObservation> observations{observe(row, false)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorWorkspaceReport& workspace = report.workspaces[0];
    const DoctorFinding* dead = find_finding(workspace, DoctorFindingCode::DeadPid);
    ASSERT_NE(dead, nullptr);
    EXPECT_EQ(dead->severity, DoctorSeverity::Medium);
    EXPECT_NE(dead->remediation.find("workspace reap"), std::string::npos);
    EXPECT_NE(dead->remediation.find("--yes"), std::string::npos);
    EXPECT_NE(find_finding(workspace, DoctorFindingCode::LeaseMissing), nullptr);
}

TEST(Errata88DoctorTest, ForeignPidNotHost) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42);
    ProcessEntry          shell;
    shell.pid = 42;
    shell.argv = {"/bin/sh", "-c", "sleep 1000"};
    const ProcessTable process_table = make_table({shell});
    const std::vector<WorkspaceObservation> observations{observe(row, false)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorWorkspaceReport& workspace = report.workspaces[0];
    const DoctorFinding* foreign = find_finding(workspace, DoctorFindingCode::ForeignPid);
    ASSERT_NE(foreign, nullptr);
    EXPECT_EQ(foreign->severity, DoctorSeverity::High);
    EXPECT_TRUE(foreign->observedRoot.empty());
    EXPECT_EQ(count_findings(workspace, DoctorFindingCode::RootMismatch), 0U);
}

TEST(Errata88DoctorTest, WorkspaceIdMismatch) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kTediId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "B", 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::WorkspaceIdMismatch);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::High);
}

TEST(Errata88DoctorTest, SocketArgMismatch) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kTediSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "B", 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::SocketArgMismatch);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::Medium);
}

TEST(Errata88DoctorTest, LeasePidMismatch) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 99, "B", 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::LeasePidMismatch);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::Medium);
}

TEST(Errata88DoctorTest, LeaseUnreadableIsLow) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{
        observe(row, true, std::nullopt, std::nullopt, 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::LeaseUnreadable);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::Low);
    EXPECT_TRUE(report.clean);
    EXPECT_EQ(doctor_exit_code(report), 0);
}

TEST(Errata88DoctorTest, SocketOwnerMismatch) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 77, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(77, kLouvre, kLouvreId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 77, "B", 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::SocketOwnerMismatch);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::High);
}

TEST(Errata88DoctorTest, SocketOwnerUnknownIsLow) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "B", std::nullopt)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::SocketOwnerUnknown);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::Low);
    EXPECT_TRUE(report.clean);
}

TEST(Errata88DoctorTest, OrphanDaemon) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre);
    const ProcessTable    process_table = make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock)});
    const std::vector<WorkspaceObservation> observations{observe(row, false)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorWorkspaceReport& workspace = report.workspaces[0];
    const DoctorFinding* orphan = find_finding(workspace, DoctorFindingCode::OrphanDaemon);
    ASSERT_NE(orphan, nullptr);
    EXPECT_EQ(orphan->severity, DoctorSeverity::Medium);
    EXPECT_EQ(orphan->observedPid, 42);
    EXPECT_EQ(workspace.state, DoctorWorkspaceReport::State::Corrupt);
}

TEST(Errata88DoctorTest, BootMismatchOnlyWhenArgvHasBootId) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "Y", kLouvreSock);
    const ProcessTable    with_boot =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "X")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "X", 42)};

    const DoctorReport report = diagnose(observations, with_boot, kIdentity);
    const DoctorFinding* mismatch =
        find_finding(report.workspaces[0], DoctorFindingCode::BootMismatch);
    ASSERT_NE(mismatch, nullptr);
    EXPECT_EQ(mismatch->severity, DoctorSeverity::Medium);

    const ProcessTable without_boot = make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock)});
    const std::vector<WorkspaceObservation> no_boot_observations{
        observe(row, true, 42, std::nullopt, 42)};
    const DoctorReport no_boot_report = diagnose(no_boot_observations, without_boot, kIdentity);
    EXPECT_EQ(count_findings(no_boot_report.workspaces[0], DoctorFindingCode::BootMismatch), 0U);
}

TEST(Errata88DoctorTest, LeaseBootMismatch) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42, "Y", kLouvreSock);
    const ProcessTable    process_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "X")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "Y", 42)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    const DoctorFinding* finding =
        find_finding(report.workspaces[0], DoctorFindingCode::LeaseBootMismatch);
    ASSERT_NE(finding, nullptr);
    EXPECT_EQ(finding->severity, DoctorSeverity::Medium);
}

TEST(Errata88DoctorTest, NotRunningEmitsNothing) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre);
    const ProcessTable    process_table = make_table({});
    const std::vector<WorkspaceObservation> observations{observe(row, false)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    ASSERT_EQ(report.workspaces.size(), 1U);
    EXPECT_TRUE(report.workspaces[0].findings.empty());
    EXPECT_EQ(report.workspaces[0].state, DoctorWorkspaceReport::State::NotRunning);
    EXPECT_TRUE(report.clean);
}

TEST(Errata88DoctorTest, DeterministicOrder) {
    const WorkspaceRecord tedi_row = mkRow(kTediId, kTedi);
    const WorkspaceRecord louvre_row = mkRow(kLouvreId, kLouvre);
    const ProcessTable    process_table = make_table({});
    const std::vector<WorkspaceObservation> observations{observe(tedi_row, false),
                                                         observe(louvre_row, false)};

    const DoctorReport first = diagnose(observations, process_table, kIdentity);
    const DoctorReport second = diagnose(observations, process_table, kIdentity);

    ASSERT_EQ(first.workspaces.size(), 2U);
    EXPECT_EQ(first.workspaces[0].canonicalPath, kTedi);
    EXPECT_EQ(first.workspaces[1].canonicalPath, kLouvre);

    std::ostringstream first_out;
    std::ostringstream second_out;
    render_doctor_human(first, first_out);
    render_doctor_human(second, second_out);
    EXPECT_EQ(first_out.str(), second_out.str());
}

TEST(Errata88DoctorTest, HumanRenderGolden) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42);
    const ProcessTable    process_table = make_table({mkHost(42, kTedi, kTediId, kTediSock)});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42)};
    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    std::ostringstream out;
    render_doctor_human(report, out);

    const std::string expected =
        "11111111-1111-4111-8111-111111111111\t/louvre\tcorrupt\n"
        "  HIGH\troot-mismatch\trecorded pid 42 is a ymh host for root /bb_tedi (expected "
        "/louvre)\n"
        "    fix: ymh workspace stop 11111111-1111-4111-8111-111111111111 --force\n"
        "  HIGH\tworkspace-id-mismatch\trecorded pid 42 records workspace "
        "22222222-2222-4222-8222-222222222222 (expected 11111111-1111-4111-8111-111111111111)\n"
        "    fix: ymh workspace stop 11111111-1111-4111-8111-111111111111 --force\n"
        "  HIGH\tlease-root-mismatch\tlease pid 42 is a ymh host for root /bb_tedi (expected "
        "/louvre)\n"
        "    fix: ymh workspace stop 11111111-1111-4111-8111-111111111111 --force\n"
        "doctor: 1 workspaces, 3 high, 0 medium, 0 low\n";
    EXPECT_EQ(out.str(), expected);
}

TEST(Errata88DoctorTest, JsonRenderGolden) {
    const WorkspaceRecord row = mkRow(kLouvreId, kLouvre, 42);
    const ProcessTable    process_table = make_table({mkHost(42, kTedi, kTediId, kTediSock)});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42)};
    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    std::ostringstream out;
    render_doctor_json(report, out);
    const nlohmann::json root = nlohmann::json::parse(out.str());

    EXPECT_EQ(root.at("schema"), 1);
    EXPECT_EQ(root.at("process_table_complete"), true);
    EXPECT_EQ(root.at("clean"), false);
    EXPECT_EQ(root.at("counts").at("high"), 3);
    EXPECT_EQ(root.at("counts").at("medium"), 0);

    const nlohmann::json& workspace = root.at("workspaces").at(0);
    EXPECT_EQ(workspace.at("state"), "corrupt");
    EXPECT_EQ(workspace.at("recorded_pid"), 42);
    EXPECT_FALSE(workspace.contains("socket"));
    EXPECT_EQ(workspace.at("lease").at("held"), true);
    EXPECT_EQ(workspace.at("lease").at("diagnostic_pid"), 42);

    const nlohmann::json& first = workspace.at("findings").at(0);
    EXPECT_EQ(first.at("code"), "root-mismatch");
    EXPECT_EQ(first.at("severity"), "high");
    EXPECT_EQ(first.at("observed_root"), "/bb_tedi");
    EXPECT_TRUE(first.contains("remediation"));

    const nlohmann::json& second = workspace.at("findings").at(1);
    EXPECT_EQ(second.at("code"), "workspace-id-mismatch");
    EXPECT_FALSE(second.contains("observed_root"));
}

TEST(Errata88DoctorTest, NonCreatingLockProbe) {
    TempDir         dir{"ymh_doctor_lock"};
    std::error_code error;
    std::filesystem::create_directories(dir.path() / ".ymh", error);

    const fs::path lock = dir.path() / ".ymh" / "sessions.lock";
    EXPECT_FALSE(std::filesystem::exists(lock));

    const WorkspaceLockProbe non_creating = probeWorkspaceLock(dir.path(), false);
    EXPECT_FALSE(non_creating.held);
    EXPECT_FALSE(std::filesystem::exists(lock));

    const WorkspaceLockProbe creating = probeWorkspaceLock(dir.path(), true);
    EXPECT_FALSE(creating.held);
    EXPECT_TRUE(std::filesystem::exists(lock));
}

TEST(Errata88DoctorTest, SocketOwnerLookup) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    const fs::path socket_path =
        fs::temp_directory_path() / ("ymh_doctor_socket_" + std::to_string(::getpid()));
    ::unlink(socket_path.c_str());

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);

    const std::optional<std::int32_t> owner = socketOwnerPid(socket_path);
    ::close(fd);
    ::unlink(socket_path.c_str());

    if (!owner.has_value()) {
        GTEST_SKIP() << "/proc/net/unix lacks the test socket";
    }
    EXPECT_EQ(*owner, ::getpid());
}

TEST(Errata88DoctorTest, ReapRefusesWhenHeld) {
    TempDir   state{"ymh_doctor_reap_held_state"};
    TempDir   workspace{"ymh_doctor_reap_held_ws"};
    ScopedEnv env{"XDG_STATE_HOME", state.path().string()};

    WorkspaceId id;
    {
        auto                  registry = WorkspaceRegistry::open(default_registry_config());
        const WorkspaceRecord row = registry->registerWorkspace(workspace.path(), "t");
        id = row.id;
        claim_stale_host(*registry, row);
    }

    const int lock_fd = hold_file_lock(workspace.path() / ".ymh" / "sessions.lock");
    ASSERT_GE(lock_fd, 0);
    {
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(run_workspace_reap({"reap", id.value}, true, out, err), 1);
    }
    {
        auto                                 registry = WorkspaceRegistry::openReadOnly(
            default_registry_config());
        const std::optional<WorkspaceRecord> row = registry->findById(id);
        ASSERT_TRUE(row.has_value());
        EXPECT_TRUE(row->host.has_value());
    }
    release_file_lock(lock_fd);
}

TEST(Errata88DoctorTest, ReapClearsWhenFree) {
    TempDir   state{"ymh_doctor_reap_free_state"};
    TempDir   workspace{"ymh_doctor_reap_free_ws"};
    ScopedEnv env{"XDG_STATE_HOME", state.path().string()};

    WorkspaceId id;
    {
        auto                  registry = WorkspaceRegistry::open(default_registry_config());
        const WorkspaceRecord row = registry->registerWorkspace(workspace.path(), "t");
        id = row.id;
        claim_stale_host(*registry, row);
    }
    {
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(run_workspace_reap({"reap", id.value}, true, out, err), 0);
    }
    {
        auto                                 registry = WorkspaceRegistry::openReadOnly(
            default_registry_config());
        const std::optional<WorkspaceRecord> row = registry->findById(id);
        ASSERT_TRUE(row.has_value());
        EXPECT_FALSE(row->host.has_value());
    }
}

TEST(Errata88DoctorTest, ExitCodeMapping) {
    const ProcessTable clean_table =
        make_table({mkHost(42, kLouvre, kLouvreId, kLouvreSock, "B")});
    const DoctorReport clean = diagnose(
        std::vector<WorkspaceObservation>{
            observe(mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock), true, 42, "B", 42)},
        clean_table, kIdentity);
    EXPECT_EQ(doctor_exit_code(clean), 0);

    const DoctorReport low_only = diagnose(
        std::vector<WorkspaceObservation>{
            observe(mkRow(kLouvreId, kLouvre, 42, "B", kLouvreSock), true, 42, "B", std::nullopt)},
        clean_table, kIdentity);
    EXPECT_EQ(doctor_exit_code(low_only), 0);

    const DoctorReport medium = diagnose(
        std::vector<WorkspaceObservation>{observe(mkRow(kLouvreId, kLouvre, 42), false)},
        make_table({}), kIdentity);
    EXPECT_EQ(doctor_exit_code(medium), 1);

    const DoctorReport incomplete = diagnose(
        std::vector<WorkspaceObservation>{
            observe(mkRow(kLouvreId, kLouvre, 42, "B"), true, 42, "B", 42)},
        make_table({}, false), kIdentity);
    EXPECT_EQ(doctor_exit_code(incomplete), 3);
}

TEST(Errata88DoctorTest, ProcessTableUnavailable) {
    const WorkspaceRecord louvre_row = mkRow(kLouvreId, kLouvre, 42, "B");
    const WorkspaceRecord tedi_row = mkRow(kTediId, kTedi, 43, "B");
    const ProcessTable    process_table = make_table({}, false);
    const std::vector<WorkspaceObservation> observations{observe(louvre_row, true, 42, "B", 42),
                                                         observe(tedi_row, true, 43, "B", 43)};

    const DoctorReport report = diagnose(observations, process_table, kIdentity);

    EXPECT_FALSE(report.processTableComplete);
    EXPECT_TRUE(report.clean);
    EXPECT_EQ(report.medium, 0U);
    EXPECT_EQ(report.low, 2U);
    EXPECT_EQ(doctor_exit_code(report), 3);
    for (const DoctorWorkspaceReport& workspace : report.workspaces) {
        EXPECT_EQ(count_findings(workspace, DoctorFindingCode::ProcessTableUnavailable), 1U);
        EXPECT_EQ(count_findings(workspace, DoctorFindingCode::DeadPid), 0U);
    }
}

TEST(Errata88DoctorTest, StubCanonicalizerDrivesMismatch) {
    const WorkspaceRecord row = mkRow(kLouvreId, "/canon/louvre", 42, "B", kLouvreSock);
    const ProcessTable    process_table = make_table({mkHost(42, "/x", kLouvreId, kLouvreSock, "B")});
    const std::vector<WorkspaceObservation> observations{observe(row, true, 42, "B", 42)};

    const DoctorReport identity_report = diagnose(observations, process_table, kIdentity);
    ASSERT_NE(find_finding(identity_report.workspaces[0], DoctorFindingCode::RootMismatch),
              nullptr);

    const Canonicalizer stub = [](const fs::path&) -> std::optional<fs::path> {
        return fs::path{"/canon/louvre"};
    };
    const DoctorReport stub_report = diagnose(observations, process_table, stub);
    EXPECT_EQ(find_finding(stub_report.workspaces[0], DoctorFindingCode::RootMismatch), nullptr);
    EXPECT_TRUE(stub_report.clean);
}

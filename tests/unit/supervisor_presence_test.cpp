#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/short_temp.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/registry/supervisor.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_presence.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace ymh::ui;
using namespace std::chrono_literals;

RegistryConfig presence_registry_config(const std::filesystem::path& dir) {
    RegistryConfig config;
    config.db_path = dir / "registry.db";
    config.lock_path = dir / "registry.lock";
    config.lock_retry_budget = std::chrono::milliseconds{80};
    config.lock_retry_interval = std::chrono::milliseconds{5};
    config.workspace_roots = {dir / "no-such-root"};
    config.bootstrap_depth = 4;
    return config;
}

int hold_file_lock(const std::filesystem::path& path) {
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

// Fork-based. The child is forked before this process mints its instance, so its
// function-local static is initialized in an independent address space; the two
// processes must therefore observe different ids (16-D8). Keep this first in the
// file so no earlier test initializes the parent's static.
TEST(SupervisorPresenceTest, ProcessClientInstanceDistinctAcrossProcesses) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::close(fds[0]);
        const std::string value = process_client_instance().value;
        const ssize_t     written = ::write(fds[1], value.data(), value.size());
        (void)written;
        ::close(fds[1]);
        ::_exit(0);
    }
    ::close(fds[1]);
    char          buffer[64] = {};
    const ssize_t count      = ::read(fds[0], buffer, sizeof(buffer) - 1);
    ::close(fds[0]);
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_GT(count, 0);
    const std::string child_value{buffer, static_cast<std::size_t>(count)};
    EXPECT_NE(child_value, process_client_instance().value);
}

TEST(SupervisorPresenceTest, ProcessClientInstanceStableWithinProcess) {
    const std::string first  = process_client_instance().value;
    const std::string second = process_client_instance().value;
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

TEST(SupervisorPresenceTest, RegisterHeartbeatDeregisterRoundTrip) {
    ShortTempRoot root("ymh-presence");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(presence_registry_config(root.state_dir()));
    const SupervisorId id{"11111111-1111-4111-8111-111111111111"};

    {
        SupervisorPresence presence =
            SupervisorPresence::registerSelf(*registry, id, SupervisorPresence::Options{});
        EXPECT_EQ(presence.id().value, id.value);

        const std::vector<SupervisorRow> rows = registry->listSupervisors();
        ASSERT_EQ(rows.size(), 1U);
        EXPECT_EQ(rows[0].id.value, id.value);
        EXPECT_EQ(rows[0].pid, static_cast<std::int32_t>(::getpid()));
        EXPECT_FALSE(rows[0].bootId.empty());

        EXPECT_TRUE(presence.heartbeat(1'000));
        EXPECT_TRUE(presence.heartbeat(2'000));
        const std::vector<SupervisorRow> refreshed = registry->listSupervisors();
        ASSERT_EQ(refreshed.size(), 1U);
        EXPECT_EQ(refreshed[0].heartbeatMs, 2'000);

        presence.deregister();
        EXPECT_TRUE(registry->listSupervisors().empty());
        presence.deregister();  // absent: best-effort, never throws
    }
}

TEST(SupervisorPresenceTest, ReRegisterAfterPrunedHeartbeat) {
    ShortTempRoot root("ymh-presence-reregister");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(presence_registry_config(root.state_dir()));
    const SupervisorId id{"22222222-2222-4222-8222-222222222222"};

    const auto fixed = std::chrono::system_clock::time_point{std::chrono::milliseconds{5'000}};
    SupervisorPresence::Options options;
    options.heartbeat_interval = 5s;
    options.owner_lease_ttl = 15s;
    options.wall_clock = [fixed] { return fixed; };

    SupervisorPresence presence = SupervisorPresence::registerSelf(*registry, id, options);
    ASSERT_EQ(registry->listSupervisors().size(), 1U);

    const std::int64_t far_future_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(fixed.time_since_epoch()).count() +
        1'000'000;
    EXPECT_EQ(registry->pruneStaleSupervisors(far_future_ms, options.owner_lease_ttl), 1U);
    EXPECT_TRUE(registry->listSupervisors().empty());

    EXPECT_FALSE(presence.heartbeat(far_future_ms));
    presence.reRegister();
    const std::vector<SupervisorRow> rows = registry->listSupervisors();
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].id.value, id.value);
    EXPECT_TRUE(presence.heartbeat(far_future_ms + 1));
}

TEST(SupervisorPresenceTest, DeregisterIsNoexcept) {
    ShortTempRoot root("ymh-presence-noexcept");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(presence_registry_config(root.state_dir()));
    SupervisorPresence presence = SupervisorPresence::registerSelf(
        *registry, SupervisorId{"33333333-3333-4333-8333-333333333333"},
        SupervisorPresence::Options{});
    static_assert(noexcept(presence.deregister()));
    presence.deregister();
}

TEST(SupervisorPresenceTest, RunOptionsDefaultsAreNullAndPinned) {
    const SupervisorRunOptions options;
    EXPECT_EQ(options.lifecycle, nullptr);
    EXPECT_EQ(options.registry, nullptr);
    EXPECT_FALSE(options.no_prompt);
    EXPECT_EQ(options.scan_interval, 2s);
    EXPECT_EQ(options.ownership_query_timeout, 2s);
    EXPECT_EQ(options.teardown_grace, 5s);
    EXPECT_EQ(options.identity.role, protocol::ClientRole::Supervisor);
}

TEST(SupervisorPresenceTest, OrphaningViewTruthTable) {
    protocol::OwnershipView view;
    view.live_supervisors = 1;
    view.live_automation = 0;
    view.other_fresh_owners = 0;
    EXPECT_TRUE(is_orphaning_view(view)) << "sole live supervisor with no other owner";

    view.live_supervisors = 2;
    EXPECT_FALSE(is_orphaning_view(view)) << "a peer supervisor holds the daemon";

    view.live_supervisors = 1;
    view.live_automation = 1;
    EXPECT_FALSE(is_orphaning_view(view)) << "ymh run holds the daemon";

    view.live_automation = 0;
    view.other_fresh_owners = 1;
    EXPECT_FALSE(is_orphaning_view(view)) << "a fresh owner row survives the caller";

    view.live_supervisors = 0;
    view.other_fresh_owners = 0;
    EXPECT_FALSE(is_orphaning_view(view)) << "caller must be counted";
}

TEST(DaemonSetScannerTest, ScanOnceReturnsOnlyLiveClaims) {
    ShortTempRoot root("ymh-scan");
    const std::filesystem::path state = root.state_dir();
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(presence_registry_config(state));

    const std::filesystem::path live_root = root.path() / "live";
    const std::filesystem::path stale_root = root.path() / "stale";
    std::filesystem::create_directories(live_root);
    std::filesystem::create_directories(stale_root);
    const WorkspaceRecord live_record = registry->registerWorkspace(live_root, "live");
    const WorkspaceRecord stale_record = registry->registerWorkspace(stale_root, "stale");

    registry->claimHost(HostClaim{live_record.id, 4242, HostBootId{"live-boot"},
                                  live_root / ".ymh" / "host.sock"});
    registry->claimHost(HostClaim{stale_record.id, 4243, HostBootId{"stale-boot"},
                                  stale_root / ".ymh" / "host.sock"});

    const int lock_fd = hold_file_lock(live_root / ".ymh" / "sessions.lock");
    ASSERT_GE(lock_fd, 0);

    DaemonSetScanner scanner(*registry, 2s, {});
    const std::vector<SupervisorWorkspace> live = scanner.scanOnce();
    ASSERT_EQ(live.size(), 1U);
    EXPECT_EQ(live[0].id.value, live_record.id.value);
    EXPECT_EQ(live[0].boot_id, "live-boot");
    EXPECT_EQ(live[0].socket_path, (live_root / ".ymh" / "host.sock").string());

    release_file_lock(lock_fd);
}

} // namespace

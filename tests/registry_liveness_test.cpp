#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "test_support.hpp"

#include "ymh/registry/registry.hpp"

namespace {

using ymh::HostBootId;
using ymh::HostClaim;
using ymh::HostLiveness;
using ymh::RegistryConfig;
using ymh::WorkspaceRegistry;
using ymh_test::TempWorkspace;

TEST(RegistryLiveness, LocalSocketIsLiveRemoteLockIsRemoteAndFreeIsStale) {
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.root / ".ymh");
    std::filesystem::create_directories(workspace.root / "empty-scan");

    RegistryConfig config;
    config.db_path        = workspace.root / "state" / "registry.db";
    config.lock_path      = workspace.root / "state" / "registry.lock";
    config.workspace_roots = {workspace.root / "empty-scan"};

    auto registry = WorkspaceRegistry::open(config);
    const auto record = registry->registerWorkspace(workspace.root, "test workspace");

    const std::filesystem::path socket_path = workspace.root / ".ymh" / "host.sock";
    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    ASSERT_LT(socket_path.string().size(), sizeof(address.sun_path));
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(server, 1), 0);

    HostClaim claim;
    claim.workspace  = record.id;
    claim.pid        = static_cast<ymh::HostPid>(::getpid());
    claim.bootId     = HostBootId{"test-boot"};
    claim.socketPath = socket_path;
    registry->claimHost(claim);

    EXPECT_EQ(registry->probeLiveness(record.id), HostLiveness::Live);

    // A socket owned by a local process is Live. Closing it without clearing
    // the claim must not make a remotely held sidecar lock look local.
    ::close(server);
    ::unlink(socket_path.c_str());

    const std::filesystem::path lock_path = workspace.root / ".ymh" / "sessions.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(lock_fd, 0);
    ASSERT_EQ(::flock(lock_fd, LOCK_EX | LOCK_NB), 0);

    EXPECT_EQ(registry->probeLiveness(record.id), HostLiveness::Remote);
    EXPECT_FALSE(registry->reapHost(record.id));

    ASSERT_EQ(::flock(lock_fd, LOCK_UN), 0);
    ::close(lock_fd);

    EXPECT_EQ(registry->probeLiveness(record.id), HostLiveness::Stale);
    EXPECT_TRUE(registry->reapHost(record.id));
    EXPECT_EQ(registry->probeLiveness(record.id), HostLiveness::Absent);
}

} // namespace

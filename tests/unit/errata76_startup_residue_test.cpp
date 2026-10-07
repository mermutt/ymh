#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "support/fake_transport_host.hpp"
#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/transport/protocol_server.hpp"
#include "ymh/transport/transport_server.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ymh;

RegistryConfig registry_config_for(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = std::chrono::milliseconds{200};
    config.lock_retry_interval = std::chrono::milliseconds{5};
    return config;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<ino_t> inode_of(const std::string& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        return std::nullopt;
    }
    return status.st_ino;
}

// A minimal AF_UNIX peer at `host.sock`. In `hold` mode it accepts and never
// answers `host.hello`; otherwise it reads once and closes (still no valid
// hello), producing a fast `Residue`.
class RawPeer {
public:
    RawPeer(std::string path, bool hold) : path_(std::move(path)), hold_(hold) {
        listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) {
            return;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        ::unlink(path_.c_str());
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 16) != 0) {
            ::close(listener_);
            listener_ = -1;
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    ~RawPeer() { stop(); }

    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    [[nodiscard]] bool listening() const noexcept { return listener_ >= 0; }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        std::lock_guard lock(mutex_);
        for (const int fd : accepted_) {
            ::close(fd);
        }
        accepted_.clear();
        if (listener_ >= 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }

private:
    void run() {
        while (!stopped_.load()) {
            pollfd descriptor{};
            descriptor.fd = listener_;
            descriptor.events = POLLIN;
            if (::poll(&descriptor, 1, 20) <= 0) {
                continue;
            }
            const int accepted = ::accept(listener_, nullptr, nullptr);
            if (accepted < 0) {
                continue;
            }
            if (hold_) {
                std::lock_guard lock(mutex_);
                accepted_.push_back(accepted);
            } else {
                char scratch[512];
                static_cast<void>(::read(accepted, scratch, sizeof(scratch)));
                ::close(accepted);
            }
        }
    }

    std::string                     path_;
    bool                            hold_{false};
    int                             listener_{-1};
    std::atomic<bool>               stopped_{false};
    std::thread                     thread_;
    std::mutex                      mutex_;
    std::vector<int>                accepted_;
};

class CountingLauncher final : public HostLauncher {
public:
    SpawnResult spawn(const HostConfig& config) override {
        ++spawn_count;
        return SpawnResult{HostPid{4242}, HostBootId{"fake-boot"}, config.socket_path};
    }
    void requestStop(HostPid pid, ShutdownReason) override { stopped_pid = pid; }
    [[nodiscard]] bool isAlive(HostPid) const override { return true; }
    [[nodiscard]] std::optional<int> tryReap(HostPid) override { return std::nullopt; }

    int    spawn_count = 0;
    HostPid stopped_pid = 0;
};

struct Fixture {
    explicit Fixture(const std::string& tag) : root(tag) {
        std::filesystem::create_directories(root.path() / ".ymh");
        canonical = std::filesystem::canonical(root.path());
    }
    ymh::test::ShortTempRoot       root;
    std::filesystem::path          canonical;
    std::unique_ptr<WorkspaceRegistry> registry;
    std::optional<WorkspaceRecord> record;
    int                            lock_fd{-1};

    std::string socket_path() const { return (canonical / ".ymh" / "host.sock").string(); }
    std::string lock_path() const { return (canonical / ".ymh" / "sessions.lock").string(); }

    void open_registry() { registry = WorkspaceRegistry::open(registry_config_for(canonical)); }

    void register_and_claim() {
        record = registry->registerWorkspace(canonical, "residue");
        registry->claimHost(HostClaim{record->id, HostPid{static_cast<std::int32_t>(::getpid())},
                                      HostBootId{"22222222-2222-4222-8222-222222222222"},
                                      canonical / ".ymh" / "host.sock"});
    }

    void hold_lock(const std::optional<nlohmann::json>& diagnostic) {
        lock_fd = ::open(lock_path().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock_fd < 0) {
            return;
        }
        static_cast<void>(::flock(lock_fd, LOCK_EX));
        if (diagnostic.has_value()) {
            const std::string body = diagnostic->dump();
            static_cast<void>(::ftruncate(lock_fd, 0));
            static_cast<void>(::pwrite(lock_fd, body.data(), body.size(), 0));
        }
    }

    ~Fixture() {
        if (lock_fd >= 0) {
            static_cast<void>(::flock(lock_fd, LOCK_UN));
            ::close(lock_fd);
        }
    }
};

AttachIdentity make_identity() {
    return AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                          protocol::ClientRole::Supervisor};
}

protocol::ProtocolServerConfig config_for(const test::FakeTransportHost& host) {
    protocol::ProtocolServerConfig config;
    config.uid = static_cast<std::uint32_t>(::getuid());
    config.workspace = host.workspace;
    config.boot_id = host.boot_id;
    config.pid = host.pid;
    return config;
}

// 76-I5 / 76-I6 / 76-F6: a wedged (or SIGSTOPped) daemon whose lock diagnostic
// matches the claim yields a specific terminal error, never a respawn.
TEST(Err76, EnsureRunningBoundedOnWedge) {
    Fixture fixture("ymh-e76-wedge");
    fixture.open_registry();
    fixture.register_and_claim();
    fixture.hold_lock(nlohmann::json{{"pid", ::getpid()},
                                     {"boot_id", "22222222-2222-4222-8222-222222222222"}});

    RawPeer peer(fixture.socket_path(), /*hold=*/true);
    ASSERT_TRUE(peer.listening());

    CountingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *fixture.registry);

    AttachBudget budget;
    budget.total     = 1000ms;
    budget.connect   = 300ms;
    budget.handshake = 200ms;

    const auto started = std::chrono::steady_clock::now();
    protocol::HostErrorCode code = protocol::HostErrorCode::HostUnreachable;
    std::string             message;
    try {
        lifecycle.ensureRunning(fixture.record->id, make_identity(), budget);
        FAIL() << "ensureRunning unexpectedly attached to a wedged daemon";
    } catch (const HostError& error) {
        code = error.code();
        message = error.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(code, protocol::HostErrorCode::HostUnresponsive) << message;
    EXPECT_LT(elapsed, 2500ms);
    EXPECT_EQ(launcher.spawn_count, 0);
    const std::optional<WorkspaceRecord> current = fixture.registry->findById(fixture.record->id);
    ASSERT_TRUE(current.has_value());
    EXPECT_TRUE(current->host.has_value());
}

// 76-I7 / 76-F5: a live, unrelated process holding the sidecar lock yields the
// specific foreign-holder error; never a spawn and never a signal.
TEST(Err76, ForeignLockHolderSpecific) {
    Fixture fixture("ymh-e76-foreign");
    fixture.open_registry();
    fixture.register_and_claim();
    fixture.hold_lock(std::nullopt);

    RawPeer peer(fixture.socket_path(), /*hold=*/true);
    ASSERT_TRUE(peer.listening());

    CountingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *fixture.registry);

    AttachBudget budget;
    budget.total     = 1000ms;
    budget.connect   = 300ms;
    budget.handshake = 200ms;

    protocol::HostErrorCode code = protocol::HostErrorCode::HostUnreachable;
    std::string             message;
    try {
        lifecycle.ensureRunning(fixture.record->id, make_identity(), budget);
        FAIL() << "ensureRunning unexpectedly attached to a foreign lock holder";
    } catch (const HostError& error) {
        code = error.code();
        message = error.what();
    }

    EXPECT_EQ(code, protocol::HostErrorCode::WorkspaceLockForeign) << message;
    EXPECT_NE(message.find(fixture.lock_path()), std::string::npos);
    EXPECT_EQ(launcher.spawn_count, 0);
}

// 76-I8 / 76-F4: a connectable `host.sock` that never authenticates as this
// workspace is residue; the probe reports `Residue` and the daemon reclaims it.
TEST(Err76, StaleSocketReclaimed) {
    Fixture fixture("ymh-e76-stale");
    const std::string path = fixture.socket_path();

    RawPeer peer(path, /*hold=*/false);
    ASSERT_TRUE(peer.listening());
    ASSERT_TRUE(std::filesystem::exists(path));

    const protocol::WorkspaceId workspace{generate_uuid_v4()};
    const protocol::SocketProbeResult probe =
        protocol::probe_existing_socket(path, workspace, std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(probe, protocol::SocketProbeResult::Residue);

    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    protocol::TransportServer socket_server(engine, path);
    ASSERT_NO_THROW(socket_server.start());
    EXPECT_TRUE(socket_server.running());
    socket_server.stop();
}

// 76-I8 / 76-F13: a peer that answers `host.hello` for this workspace is a live
// daemon; the probe must not unlink its socket.
TEST(Err76, LiveHelloNotReclaimed) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    ymh::test::ShortTempRoot root("ymh-e76-live");
    std::filesystem::create_directories(root.path() / ".ymh");
    const std::string path = (root.path() / ".ymh" / "host.sock").string();

    protocol::TransportServer socket_server(engine, path);
    socket_server.start();

    const std::optional<ino_t> before = inode_of(path);
    ASSERT_TRUE(before.has_value());
    const protocol::SocketProbeResult probe =
        protocol::probe_existing_socket(path, host.workspace, std::chrono::steady_clock::now() + 2s);
    EXPECT_EQ(probe, protocol::SocketProbeResult::LiveYmhDaemon);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(inode_of(path), before);

    socket_server.stop();
}

// 76-I9: reclaiming a residue socket must not touch any other workspace state.
TEST(Err76, NoDataDeleted) {
    Fixture fixture("ymh-e76-nodata");
    const std::filesystem::path base = fixture.canonical / ".ymh";
    std::filesystem::create_directories(base);
    const std::filesystem::path sessions_db = base / "sessions.db";
    const std::filesystem::path host_log = base / "host.log";
    const std::filesystem::path permissions = base / "permissions.jsonc";
    {
        std::ofstream(sessions_db, std::ios::binary) << "db-bytes";
        std::ofstream(host_log, std::ios::binary) << "log-bytes";
        std::ofstream(permissions, std::ios::binary) << "{\"allow\":[]}";
    }

    RawPeer peer(fixture.socket_path(), /*hold=*/false);
    ASSERT_TRUE(peer.listening());

    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    protocol::TransportServer socket_server(engine, fixture.socket_path());
    ASSERT_NO_THROW(socket_server.start());
    socket_server.stop();

    EXPECT_EQ(read_file(sessions_db), "db-bytes");
    EXPECT_EQ(read_file(host_log), "log-bytes");
    EXPECT_EQ(read_file(permissions), "{\"allow\":[]}");
}

// 76-I11 / 76-F9: exhausting the budget names the phase and stays a bounded
// ASCII error; a spawn that never publishes a claim cannot loop forever.
TEST(Err76, BudgetNamesPhase) {
    Fixture fixture("ymh-e76-budget");
    fixture.open_registry();
    fixture.record = fixture.registry->registerWorkspace(fixture.canonical, "budget");

    CountingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *fixture.registry);

    AttachBudget budget;
    budget.total           = 200ms;
    budget.spawn_readiness = 10000ms;
    budget.winner          = 5000ms;

    std::string message;
    try {
        lifecycle.ensureRunning(fixture.record->id, make_identity(), budget);
        FAIL() << "ensureRunning unexpectedly succeeded without a claim";
    } catch (const HostError& error) {
        message = error.what();
    }
    EXPECT_NE(message.find("startup exceeded"), std::string::npos) << message;
    EXPECT_NE(message.find("during"), std::string::npos) << message;
    EXPECT_EQ(launcher.spawn_count, 1);
    EXPECT_EQ(launcher.stopped_pid, 4242);
}

} // namespace

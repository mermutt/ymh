#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ymh;

// A residue `host.sock`: accepts a connection, reads the probe's hello, and
// closes without answering. The daemon-side probe classifies it `Residue`
// quickly (no valid hello) and reclaims the node.
class ResidueSocket {
public:
    explicit ResidueSocket(std::filesystem::path path) : path_(std::move(path)) {
        std::filesystem::create_directories(path_.parent_path());
        listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) {
            return;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.string().size() + 1);
        ::unlink(path_.c_str());
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 16) != 0) {
            ::close(listener_);
            listener_ = -1;
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    ~ResidueSocket() {
        stopped_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ >= 0) {
            ::close(listener_);
        }
    }

    ResidueSocket(const ResidueSocket&) = delete;
    ResidueSocket& operator=(const ResidueSocket&) = delete;

    [[nodiscard]] bool listening() const noexcept { return listener_ >= 0; }

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
            char scratch[512];
            static_cast<void>(::read(accepted, scratch, sizeof(scratch)));
            ::close(accepted);
        }
    }

    std::filesystem::path path_;
    int                   listener_{-1};
    std::atomic<bool>     stopped_{false};
    std::thread           thread_;
};

RegistryConfig harness_registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = std::chrono::milliseconds{500};
    config.lock_retry_interval = std::chrono::milliseconds{5};
    return config;
}

class Err76Integration : public ::testing::Test {
protected:
    void SetUp() override {
        binary_ = ymh::test::resolve_ymh_binary();
        if (!ymh::test::HostHarness::binary_supports_host(binary_)) {
            GTEST_SKIP() << "ymh --host is not implemented in " << binary_;
        }
    }

    std::filesystem::path binary_;
};

// 76-I5 / 76-I6 / spec 16 O11: a second attach must reuse the live daemon and
// never signal it.
TEST_F(Err76Integration, LiveDaemonNeverKilled) {
    ymh::test::ShortTempRoot root("ymh-e76-live");
    root.write("fake.json", R"([{"text": "ok"}])");

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    const pid_t daemon_pid = harness.pid();
    ASSERT_GT(daemon_pid, 0);

    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const std::optional<WorkspaceRecord> record =
        registry->findByCanonicalPath(root.path());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->host.has_value());

    ForkExecLauncher launcher;
    HostLifecycle    lifecycle(launcher, *registry);
    const AttachIdentity identity{protocol::ClientInstanceId{generate_uuid_v4()},
                                  protocol::ClientRole::Supervisor};

    AttachResult first = lifecycle.ensureRunning(record->id, identity, AttachBudget{});
    EXPECT_TRUE(first.connection != nullptr);
    EXPECT_FALSE(first.spawned);

    AttachResult second = lifecycle.ensureRunning(record->id, identity, AttachBudget{});
    EXPECT_TRUE(second.connection != nullptr);
    EXPECT_FALSE(second.spawned);
    EXPECT_EQ(harness.pid(), daemon_pid);
    EXPECT_TRUE(harness.running());

    first.connection.reset();
    second.connection.reset();
    const ymh::test::ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
    EXPECT_EQ(status.code, 0);
}

// 76-I9 / 76-I10: a residue `host.sock` beside durable workspace state is
// reclaimed; the durable state survives and a second start converges.
TEST_F(Err76Integration, SecondStartConverges) {
    ymh::test::ShortTempRoot root("ymh-e76-residue");
    root.write("fake.json", R"([{"text": "ok"}])");
    const std::filesystem::path permissions =
        root.path() / ".ymh" / "permissions.jsonc";
    root.write(".ymh/permissions.jsonc", "{\"allow\":[]}");

    ResidueSocket residue(root.path() / ".ymh" / "host.sock");
    ASSERT_TRUE(residue.listening());
    ASSERT_TRUE(std::filesystem::exists(root.path() / ".ymh" / "host.sock"));

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    std::string stored_workspace_id;
    {
        ymh::test::HostHarness first(options);
        first.start();
        ASSERT_TRUE(first.wait_ready()) << first.read_log();
        const ymh::test::ExitStatus status = first.stop();
        EXPECT_TRUE(status.exited);
        EXPECT_EQ(status.code, 0);

        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(harness_registry_config(root.path()));
        const std::optional<WorkspaceRecord> record =
            registry->findByCanonicalPath(root.path());
        ASSERT_TRUE(record.has_value());
        stored_workspace_id = record->id.value;
    }

    EXPECT_TRUE(std::filesystem::exists(permissions));
    EXPECT_GT(std::filesystem::file_size(permissions), 0u);

    ymh::test::HostHarnessOptions second_options = options;
    second_options.workspace_id = stored_workspace_id;
    ymh::test::HostHarness second(second_options);
    second.start();
    ASSERT_TRUE(second.wait_ready()) << second.read_log();
    EXPECT_TRUE(second.running());
    EXPECT_TRUE(std::filesystem::exists(second.socket_path()));

    const ymh::test::ExitStatus second_status = second.stop();
    EXPECT_TRUE(second_status.exited);
    EXPECT_EQ(second_status.code, 0);
}

} // namespace

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/short_temp.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/config/config.hpp"
#include "ymh/execution/signal_policy.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr std::string_view kWorkspaceId = "11111111-1111-4111-8111-111111111111";

FakeScript hello_script() {
    FakeResponseStep step;
    step.text   = "hello";
    step.finish = FinishReason::Stop;
    FakeScript script;
    script.steps.push_back(step);
    return script;
}

RegistryConfig registry_config_for(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = std::chrono::milliseconds{200};
    config.lock_retry_interval = std::chrono::milliseconds{5};
    return config;
}

HostConfig base_config(const std::filesystem::path& root,
                       const std::string& workspace_id = std::string{kWorkspaceId}) {
    HostConfig config;
    config.workspace        = WorkspaceId{workspace_id};
    config.workspace_root   = root;
    config.socket_path      = root / ".ymh" / "host.sock";
    config.log_sink         = root / ".ymh" / "host.log";
    config.registry         = registry_config_for(root);
    config.persistence.db_path   = root / ".ymh" / "sessions.db";
    config.persistence.lock_path = root / ".ymh" / "sessions.lock";
    config.foreground       = true;
    // 16 §7.3: the watchdog default is fail-safe (require_owner=true), so the
    // in-process lifecycle tests opt OUT explicitly.
    config.require_owner    = false;
    config.heartbeat_interval = std::chrono::milliseconds{100};
    config.shutdown_grace   = std::chrono::milliseconds{2000};
    config.caps.max_llm_concurrency = 2;
    config.store_factory = [] { return std::make_unique<MemorySessionStore>(); };
    const FakeScript script = hello_script();
    config.provider_factory =
        [script](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(script);
    };
    return config;
}

WorkspaceId register_workspace(const std::filesystem::path& root,
                               const std::string& display = "test") {
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(registry_config_for(root));
    return registry->registerWorkspace(root, display).id;
}

std::size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    while (dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    ::closedir(directory);
    return count;
}

int bind_unix_socket(const std::filesystem::path& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const std::string text = path.string();
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

class ForegroundHost {
public:
    explicit ForegroundHost(HostConfig config)
        : host_(WorkspaceHost::create(std::move(config))) {}

    ~ForegroundHost() { stopAndJoin(); }

    ForegroundHost(const ForegroundHost&) = delete;
    ForegroundHost& operator=(const ForegroundHost&) = delete;

    void start() { thread_ = std::thread([this] { code_ = host_->run(); }); }

    [[nodiscard]] WorkspaceHost& host() { return *host_; }

    [[nodiscard]] bool waitReady(std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                auto candidate = std::make_unique<protocol::HostConnection>();
                candidate->connect(host_->socketPath().string());
                [[maybe_unused]] const protocol::HelloResult hello = candidate->handshake(
                    protocol::ServerProfile::Interactive,
                    protocol::ClientInstanceId{generate_uuid_v4()});
                connection_ = std::move(candidate);
                return true;
            } catch (...) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    [[nodiscard]] protocol::HostConnection* connection() noexcept { return connection_.get(); }

    HostExitCode stopAndJoin() {
        if (connection_ != nullptr) {
            connection_->close();
            connection_.reset();
        }
        if (thread_.joinable()) {
            host_->requestShutdown(ShutdownReason::ClientRequest);
            thread_.join();
        }
        return code_;
    }

private:
    std::unique_ptr<WorkspaceHost>             host_;
    std::thread                                thread_;
    std::unique_ptr<protocol::HostConnection>  connection_;
    HostExitCode                               code_{HostExitCode::Internal};
};

TEST(WorkspaceHostConfig, CreateRejectsMissingRoot) {
    ShortTempRoot root("ymh-host-missing");
    HostConfig    config = base_config(root.path() / "does-not-exist");
    EXPECT_THROW(
        {
            [[maybe_unused]] std::unique_ptr<WorkspaceHost> host =
                WorkspaceHost::create(std::move(config));
        },
        HostError);
}

TEST(WorkspaceHostConfig, ExitCodeValuesArePinned) {
    EXPECT_EQ(static_cast<int>(HostExitCode::Ok), 0);
    EXPECT_EQ(static_cast<int>(HostExitCode::WorkspaceBusy), 10);
    EXPECT_EQ(static_cast<int>(HostExitCode::WorkspaceMissing), 11);
    EXPECT_EQ(static_cast<int>(HostExitCode::StoreOpenFailed), 12);
    EXPECT_EQ(static_cast<int>(HostExitCode::RegistryFailed), 13);
    EXPECT_EQ(static_cast<int>(HostExitCode::SocketBindFailed), 14);
    EXPECT_EQ(static_cast<int>(HostExitCode::SocketPathTooLong), 15);
    EXPECT_EQ(static_cast<int>(HostExitCode::StartupRejected), 16);
    EXPECT_EQ(static_cast<int>(HostExitCode::Internal), 17);
}

TEST(WorkspaceHostStartup, WorkspaceMissingWhenRootDeletedBeforeRun) {
    ShortTempRoot root("ymh-host-deleted");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    HostConfig config = base_config(canonical);
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    std::filesystem::remove_all(canonical);
    EXPECT_EQ(host->run(), HostExitCode::WorkspaceMissing);
}

TEST(WorkspaceHostStartup, StoreOpenFailedWhenFactoryThrows) {
    ShortTempRoot root("ymh-host-store");
    HostConfig config = base_config(std::filesystem::canonical(root.path()));
    config.store_factory = []() -> std::unique_ptr<SessionStore> {
        throw std::runtime_error("boom");
    };
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::StoreOpenFailed);
}

TEST(WorkspaceHostStartup, ProviderSetupFailureIsStartupRejected) {
    ShortTempRoot root("ymh-host-provider");
    HostConfig config = base_config(std::filesystem::canonical(root.path()));
    config.provider_factory =
        [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        throw std::runtime_error("no provider");
    };
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::StartupRejected);
}

TEST(WorkspaceHostStartup, RegistryFailedWhenDbUnopenable) {
    ShortTempRoot root("ymh-host-registry");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    HostConfig config = base_config(canonical);
    config.registry.db_path = canonical / "registry-is-a-dir";
    std::filesystem::create_directories(config.registry.db_path);
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::RegistryFailed);
}

TEST(WorkspaceHostStartup, SocketPathTooLong) {
    ShortTempRoot root("ymh-host-long");
    HostConfig config = base_config(std::filesystem::canonical(root.path()));
    config.socket_path = root.path() / std::string(140, 's');
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::SocketPathTooLong);
}

TEST(WorkspaceHostStartup, SocketBindFailedWhenParentMissing) {
    ShortTempRoot root("ymh-host-bind");
    HostConfig config = base_config(std::filesystem::canonical(root.path()));
    config.socket_path = root.path() / "missing" / "host.sock";
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::SocketBindFailed);
}

TEST(WorkspaceHostStartup, StaleSocketIsReplaced) {
    ShortTempRoot root("ymh-host-stale");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    std::filesystem::create_directories(canonical / ".ymh");
    const int stale = bind_unix_socket(canonical / ".ymh" / "host.sock");
    ASSERT_GE(stale, 0);
    ::close(stale);

    ForegroundHost daemon(base_config(canonical));
    daemon.start();
    ASSERT_TRUE(daemon.waitReady());
    EXPECT_EQ(daemon.stopAndJoin(), HostExitCode::Ok);
}

TEST(WorkspaceHostStartup, LiveSocketProbeIsWorkspaceBusy) {
    ShortTempRoot root("ymh-host-live");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    std::filesystem::create_directories(canonical / ".ymh");
    const int listener = bind_unix_socket(canonical / ".ymh" / "host.sock");
    ASSERT_GE(listener, 0);

    HostConfig config = base_config(canonical);
    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::WorkspaceBusy);

    ::close(listener);
    std::filesystem::remove(canonical / ".ymh" / "host.sock");
}

TEST(WorkspaceHostStartup, WorkspaceBusyOnHeldFlock) {
    ShortTempRoot root("ymh-host-busy");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    HostConfig first = base_config(canonical);
    first.store_factory = {};
    first.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(hello_script());
    };

    ForegroundHost daemon(first);
    daemon.start();
    ASSERT_TRUE(daemon.waitReady());

    HostConfig second = base_config(canonical);
    second.store_factory = {};
    std::unique_ptr<WorkspaceHost> blocked = WorkspaceHost::create(std::move(second));
    EXPECT_EQ(blocked->run(), HostExitCode::WorkspaceBusy);

    EXPECT_EQ(daemon.stopAndJoin(), HostExitCode::Ok);
}

TEST(WorkspaceHostLifecycle, ForegroundServingClaimAndGracefulShutdown) {
    ShortTempRoot root("ymh-host-lifecycle");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    const std::filesystem::path original_cwd = std::filesystem::current_path();

    const WorkspaceId workspace_id = register_workspace(canonical, "lifecycle");
    HostConfig config = base_config(canonical, workspace_id.value);
    config.store_factory = {};
    config.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(hello_script());
    };

    ForegroundHost daemon(config);
    daemon.start();
    ASSERT_TRUE(daemon.waitReady());

    EXPECT_EQ(daemon.host().state(), protocol::HostState::Serving);
    EXPECT_EQ(daemon.host().workspace().value, workspace_id.value);
    EXPECT_GT(daemon.host().pid(), 0);
    EXPECT_TRUE(std::filesystem::exists(canonical / ".ymh" / "host.sock"));
    EXPECT_EQ(std::filesystem::current_path(), canonical);

    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config_for(canonical));
        const std::optional<WorkspaceRecord> record = registry->findById(workspace_id);
        ASSERT_TRUE(record.has_value());
        ASSERT_TRUE(record->host.has_value());
        EXPECT_EQ(record->host->pid, daemon.host().pid());
        EXPECT_EQ(record->host->bootId.value, daemon.host().bootId().value);
        EXPECT_EQ(record->host->socketPath, canonical / ".ymh" / "host.sock");
    }

    ASSERT_NE(daemon.connection(), nullptr);
    [[maybe_unused]] const nlohmann::json reply = daemon.connection()->request(
        protocol::method::kHostShutdown, {{"reason", "test"}});
    EXPECT_EQ(daemon.stopAndJoin(), HostExitCode::Ok);

    EXPECT_FALSE(std::filesystem::exists(canonical / ".ymh" / "host.sock"));
    EXPECT_EQ(std::filesystem::current_path(), original_cwd);

    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config_for(canonical));
        const std::optional<WorkspaceRecord> record = registry->findById(workspace_id);
        ASSERT_TRUE(record.has_value());
        EXPECT_FALSE(record->host.has_value());
    }

    PersistenceConfig persistence;
    persistence.db_path   = canonical / ".ymh" / "sessions.db";
    persistence.lock_path = canonical / ".ymh" / "sessions.lock";
    persistence.boot_id   = BootId{"after-shutdown"};
    EXPECT_NO_THROW({
        std::unique_ptr<SessionPersistence> store = SessionPersistence::open(persistence);
        store->close();
    });
}

TEST(WorkspaceHostLifecycle, FailedStartupReleasesFlock) {
    ShortTempRoot root("ymh-host-rollback");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    HostConfig config = base_config(canonical);
    config.store_factory = {};
    config.socket_path   = root.path() / "missing" / "host.sock";

    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    EXPECT_EQ(host->run(), HostExitCode::SocketBindFailed);

    PersistenceConfig persistence;
    persistence.db_path   = canonical / ".ymh" / "sessions.db";
    persistence.lock_path = canonical / ".ymh" / "sessions.lock";
    persistence.boot_id   = BootId{"rollback-probe"};
    EXPECT_NO_THROW({
        std::unique_ptr<SessionPersistence> store = SessionPersistence::open(persistence);
        store->close();
    });
}

TEST(WorkspaceHostSignals, ChildReapPolicyStaysSpecificPid) {
    EXPECT_TRUE(specificPidReapIsSafe());
    EXPECT_EQ(childReapPolicy(), ChildReapPolicy::SpecificPid);
}

TEST(WorkspaceHostSignals, SighupIsIgnoredAndSigtermStops) {
    ShortTempRoot root("ymh-host-signals");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    ForegroundHost daemon(base_config(canonical));
    daemon.start();
    ASSERT_TRUE(daemon.waitReady());

    ::kill(::getpid(), SIGHUP);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    EXPECT_EQ(daemon.host().state(), protocol::HostState::Serving);
    EXPECT_TRUE(std::filesystem::exists(canonical / ".ymh" / "host.sock"));

    ::kill(::getpid(), SIGTERM);
    EXPECT_EQ(daemon.stopAndJoin(), HostExitCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(canonical / ".ymh" / "host.sock"));
}

TEST(WorkspaceHostResources, FdHygieneAcrossRun) {
    ShortTempRoot root("ymh-host-fds");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    const std::size_t before = open_fd_count();
    {
        ForegroundHost daemon(base_config(canonical));
        daemon.start();
        ASSERT_TRUE(daemon.waitReady());
        EXPECT_EQ(daemon.stopAndJoin(), HostExitCode::Ok);
    }
    const std::size_t after = open_fd_count();
    EXPECT_LE(after, before + 2) << "before=" << before << " after=" << after;
}

TEST(ForkExecLauncherTest, BuildArgvMatchesPinnedHostEntry) {
    HostConfig config;
    config.workspace       = WorkspaceId{"22222222-2222-4222-8222-222222222222"};
    config.workspace_root  = "/tmp/ws";
    config.socket_path     = "/tmp/ws/.ymh/host.sock";
    config.boot_id         = HostBootId{"boot-123"};

    const std::vector<std::string> argv =
        ForkExecLauncher::build_argv(config, "/opt/ymh");
    const std::vector<std::string> expected = {
        "/opt/ymh",
        "--host",
        "--workspace",
        "22222222-2222-4222-8222-222222222222",
        "--root",
        "/tmp/ws",
        "--socket",
        "/tmp/ws/.ymh/host.sock",
        "--boot-id",
        "boot-123",
    };
    EXPECT_EQ(argv, expected);
}

class FakeLauncher final : public HostLauncher {
public:
    SpawnResult spawn(const HostConfig& config) override {
        ++spawn_count;
        spawned_socket = config.socket_path;
        last_config = config;
        return SpawnResult{HostPid{4242}, HostBootId{"fake-boot"}, config.socket_path};
    }
    void requestStop(HostPid pid, ShutdownReason) override { stopped_pid = pid; }
    [[nodiscard]] bool isAlive(HostPid) const override { return alive; }

    int                               spawn_count = 0;
    std::filesystem::path             spawned_socket;
    HostConfig                        last_config;
    HostPid                           stopped_pid = 0;
    bool                              alive = false;
};

TEST(HostLifecycleTest, EnsureRunningSpawnsWhenNoClaim) {
    ShortTempRoot root("ymh-lifecycle-spawn");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(registry_config_for(canonical));
    const WorkspaceRecord record = registry->registerWorkspace(canonical, "lifecycle");

    FakeLauncher  launcher;
    HostLifecycle lifecycle(launcher, *registry);
    EXPECT_THROW(lifecycle.ensureRunning(
                     record.id,
                     AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                                    protocol::ClientRole::Supervisor}),
                 HostError);
    EXPECT_EQ(launcher.spawn_count, 1);
    EXPECT_EQ(launcher.stopped_pid, 4242);
}

TEST(HostLifecycleTest, ReapIfStaleClearsStaleClaim) {
    ShortTempRoot root("ymh-lifecycle-reap");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(registry_config_for(canonical));
    const WorkspaceRecord record = registry->registerWorkspace(canonical, "lifecycle");
    registry->claimHost(HostClaim{record.id, 999999, HostBootId{"dead-boot"},
                                  canonical / ".ymh" / "host.sock"});

    FakeLauncher  launcher;
    HostLifecycle lifecycle(launcher, *registry);
    EXPECT_THROW(lifecycle.ensureRunning(
                     record.id,
                     AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                                    protocol::ClientRole::Supervisor}),
                 HostError);
    const std::optional<WorkspaceRecord> refreshed = registry->findById(record.id);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_FALSE(refreshed->host.has_value());
}

TEST(WorkspaceHostLease, FakeStoreDaemonSessionLifecycleDoesNotThrow) {
    ShortTempRoot root("ymh-host-lease-fake");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    const WorkspaceId workspace_id = register_workspace(canonical, "lease-fake");
    // base_config injects a MemorySessionStore + FakeLLM, so the daemon runs
    // with no durable SessionPersistence (the D9 store seam).
    HostConfig config = base_config(canonical, workspace_id.value);

    ForegroundHost daemon(config);
    daemon.start();
    ASSERT_TRUE(daemon.waitReady());
    ASSERT_NE(daemon.connection(), nullptr);

    nlohmann::json created;
    ASSERT_NO_THROW(created = daemon.connection()->request(
                        protocol::method::kSessionCreate, nlohmann::json::object()));
    ASSERT_TRUE(created.contains("session")) << created.dump();
    const SessionId session{created.at("session").get<std::string>()};

    // resume and fork both take the lease path inside HostRuntime.
    nlohmann::json resumed;
    EXPECT_NO_THROW(resumed = daemon.connection()->request(
                        protocol::method::kSessionResume, {{"session", session.value}}));
    EXPECT_TRUE(resumed.contains("session"));
    nlohmann::json forked;
    ASSERT_NO_THROW(forked = daemon.connection()->request(
                        protocol::method::kSessionFork,
                        {{"session", session.value}, {"seed_length", 1}}));
    EXPECT_TRUE(forked.contains("session"));

    nlohmann::json prompted;
    EXPECT_NO_THROW(prompted = daemon.connection()->request(
                        protocol::method::kAgentPrompt,
                        {{"session", session.value}, {"message", "hi"}}));

    EXPECT_GE(daemon.host().store().list().size(), 1u);
    EXPECT_EQ(daemon.stopAndJoin(), HostExitCode::Ok);
}

TEST(WorkspaceHostOwnership, WatchdogArmingTruthTable) {
    HostConfig config;
    config.require_owner     = true;
    config.watchdog_disabled = false;
    EXPECT_TRUE(owner_watchdog_armed(config));
    config.watchdog_disabled = true;
    EXPECT_FALSE(owner_watchdog_armed(config));
    config.require_owner     = false;
    config.watchdog_disabled = false;
    EXPECT_FALSE(owner_watchdog_armed(config));
    config.watchdog_disabled = true;
    EXPECT_FALSE(owner_watchdog_armed(config));

    HostConfig foreground = config;
    foreground.foreground        = true;
    foreground.require_owner     = true;
    foreground.watchdog_disabled = false;
    EXPECT_TRUE(owner_watchdog_armed(foreground));

    HostConfig defaults;
    EXPECT_TRUE(defaults.require_owner);
    EXPECT_FALSE(defaults.watchdog_disabled);
    EXPECT_TRUE(owner_watchdog_armed(defaults));
}

TEST(WorkspaceHostOwnership, FailLoudGuardRejectsBothOptOutArms) {
    ShortTempRoot root("ymh-host-guard");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());

    HostConfig require_owner_off = base_config(canonical);
    require_owner_off.foreground        = false;
    require_owner_off.require_owner     = false;
    require_owner_off.watchdog_disabled = false;
    std::unique_ptr<WorkspaceHost> first = WorkspaceHost::create(std::move(require_owner_off));
    EXPECT_EQ(first->run(), HostExitCode::StartupRejected);

    HostConfig watchdog_off = base_config(canonical);
    watchdog_off.foreground        = false;
    watchdog_off.require_owner     = true;
    watchdog_off.watchdog_disabled = true;
    std::unique_ptr<WorkspaceHost> second = WorkspaceHost::create(std::move(watchdog_off));
    EXPECT_EQ(second->run(), HostExitCode::StartupRejected);

    HostConfig both_off = base_config(canonical);
    both_off.foreground        = false;
    both_off.require_owner     = false;
    both_off.watchdog_disabled = true;
    std::unique_ptr<WorkspaceHost> third = WorkspaceHost::create(std::move(both_off));
    EXPECT_EQ(third->run(), HostExitCode::StartupRejected);
}

TEST(WorkspaceHostOwnership, ProductionConstructorsArmTheWatchdog) {
    ShortTempRoot root("ymh-host-prod-arming");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(registry_config_for(canonical));
    const WorkspaceRecord record = registry->registerWorkspace(canonical, "prod-arming");

    FakeLauncher  launcher;
    HostLifecycle lifecycle(launcher, *registry);
    EXPECT_THROW(lifecycle.ensureRunning(
                     record.id,
                     AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                                    protocol::ClientRole::Supervisor}),
                 HostError);
    ASSERT_EQ(launcher.spawn_count, 1);
    EXPECT_TRUE(launcher.last_config.require_owner);
    EXPECT_FALSE(launcher.last_config.watchdog_disabled);
    EXPECT_TRUE(owner_watchdog_armed(launcher.last_config));
}

TEST(WorkspaceHostOwnership, ShutdownReasonMappingIsTotalAndReversible) {
    const ShutdownReason native[] = {
        ShutdownReason::ClientRequest, ShutdownReason::Signal, ShutdownReason::StartupFailure,
        ShutdownReason::LastSupervisor, ShutdownReason::NoOwners, ShutdownReason::WorkspaceStop,
    };
    for (const ShutdownReason reason : native) {
        EXPECT_EQ(from_protocol(to_protocol(reason)), reason);
    }
    const protocol::ShutdownReason wire[] = {
        protocol::ShutdownReason::ClientRequest, protocol::ShutdownReason::Signal,
        protocol::ShutdownReason::StartupFailure, protocol::ShutdownReason::LastSupervisor,
        protocol::ShutdownReason::NoOwners, protocol::ShutdownReason::WorkspaceStop,
    };
    for (const protocol::ShutdownReason reason : wire) {
        EXPECT_EQ(to_protocol(from_protocol(reason)), reason);
    }
}

TEST(WorkspaceHostOwnership, DeterministicWatchdogFiresNoOwnersAndTearsDown) {
    ShortTempRoot root("ymh-host-watchdog");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    HostConfig config = base_config(canonical);
    config.require_owner     = true;
    config.watchdog_disabled = false;
    config.watchdog_interval = std::chrono::milliseconds{2};
    config.owner_grace       = std::chrono::milliseconds{40};
    config.shutdown_grace    = std::chrono::milliseconds{200};

    auto mono_ms = std::make_shared<std::atomic<std::int64_t>>(0);
    config.owner_monotonic_clock = [mono_ms] {
        return std::chrono::steady_clock::time_point{std::chrono::milliseconds{mono_ms->load()}};
    };
    config.owner_wall_clock = [] { return std::chrono::system_clock::time_point{}; };

    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    HostExitCode                  code = HostExitCode::Internal;
    std::thread                   runner([&] { code = host->run(); });

    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (host->state() != protocol::HostState::Serving &&
           std::chrono::steady_clock::now() < ready_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    ASSERT_EQ(host->state(), protocol::HostState::Serving);

    for (std::int64_t step = 0; step <= 200 && !host->ownerless(); ++step) {
        mono_ms->store(step);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_TRUE(host->ownerless());

    runner.join();
    EXPECT_EQ(code, HostExitCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(canonical / ".ymh" / "host.sock"));

    PersistenceConfig persistence;
    persistence.db_path   = canonical / ".ymh" / "sessions.db";
    persistence.lock_path = canonical / ".ymh" / "sessions.lock";
    persistence.boot_id   = BootId{"after-watchdog"};
    EXPECT_NO_THROW({
        std::unique_ptr<SessionPersistence> store = SessionPersistence::open(persistence);
        store->close();
    });
}

TEST(WorkspaceHostOwnership, WatchdogExceptionGuardPublishesEmptyAndContinues) {
    ShortTempRoot root("ymh-host-watchdog-exc");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    const WorkspaceId workspace_id = register_workspace(canonical, "watchdog-exc");
    HostConfig config = base_config(canonical, workspace_id.value);
    config.require_owner     = true;
    config.watchdog_disabled = false;
    config.watchdog_interval = std::chrono::milliseconds{2};

    auto throw_now = std::make_shared<std::atomic<bool>>(false);
    auto mono_ms   = std::make_shared<std::atomic<std::int64_t>>(0);
    config.owner_monotonic_clock = [throw_now, mono_ms] {
        if (throw_now->load()) {
            throw std::runtime_error("injected clock failure");
        }
        return std::chrono::steady_clock::time_point{std::chrono::milliseconds{mono_ms->load()}};
    };
    config.owner_wall_clock = [] { return std::chrono::system_clock::now(); };

    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(registry_config_for(canonical));
        const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        registry->registerSupervisor(SupervisorRow{
            SupervisorId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"}, 4321,
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd", now, now, std::nullopt});
    }

    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    HostExitCode                  code = HostExitCode::Internal;
    std::thread                   runner([&] { code = host->run(); });

    const auto wait_for = [](auto predicate, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return predicate();
    };

    ASSERT_TRUE(wait_for([&] { return !host->freshOwnerSnapshot()->empty(); },
                         std::chrono::seconds{2}));

    throw_now->store(true);
    EXPECT_TRUE(wait_for([&] { return host->freshOwnerSnapshot()->empty(); },
                         std::chrono::seconds{2}));

    throw_now->store(false);
    EXPECT_TRUE(wait_for([&] { return !host->freshOwnerSnapshot()->empty(); },
                         std::chrono::seconds{2}));

    host->requestShutdown(ShutdownReason::Signal);
    runner.join();
    EXPECT_EQ(code, HostExitCode::Ok);
}

} // namespace

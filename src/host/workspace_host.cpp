#include "ymh/host/workspace_host.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <asio.hpp>

#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event.hpp"
#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/host/host_runtime.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/permission/permission_transport.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol_server.hpp"
#include "ymh/transport/transport_server.hpp"

namespace ymh {
namespace {

using protocol::HostConnection;
using protocol::ProtocolServer;
using protocol::TransportServer;

// Adapts `ProtocolServer` + `TransportServer` to the broker's narrow port
// (permission_transport.hpp). Both calls marshal onto the transport io thread;
// `attach` is called once after the construction cycle completes.
class TransportServerAdapter final : public PermissionTransport {
public:
    void attach(ProtocolServer& server, TransportServer& transport) {
        server_    = &server;
        transport_ = &transport;
    }

    bool broadcast_permission_request(protocol::PermissionRequest request) override {
        if (transport_ == nullptr || server_ == nullptr) {
            return false;
        }
        return transport_->post([server = server_, request = std::move(request)]() mutable {
            if (server != nullptr) {
                server->onPermissionRequest(request);
            }
        });
    }

    bool schedule_after(std::chrono::milliseconds delay, std::function<void()> fn) override {
        if (transport_ == nullptr) {
            return false;
        }
        asio::io_context* io = &transport_->io();
        return transport_->post([io, delay, fn = std::move(fn)]() {
            auto timer = std::make_shared<asio::steady_timer>(*io);
            timer->expires_after(delay);
            timer->async_wait([timer, fn](const std::error_code& error) {
                if (!error && fn) {
                    fn();
                }
            });
        });
    }

private:
    ProtocolServer*  server_    = nullptr;
    TransportServer* transport_ = nullptr;
};

std::int32_t host_pid() noexcept { return static_cast<std::int32_t>(::getpid()); }

void write_self_ignoring_gitignore(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / ".gitignore";
    std::error_code             error;
    if (std::filesystem::exists(path, error)) {
        return;
    }
    std::ofstream(path, std::ios::binary) << "*\n";
}

void close_extra_fds() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
        return;
    }
    const int        directory_fd = ::dirfd(directory);
    std::vector<int> fds;
    while (dirent* entry = ::readdir(directory)) {
        const int fd = std::atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != directory_fd) {
            fds.push_back(fd);
        }
    }
    ::closedir(directory);
    for (const int fd : fds) {
        ::close(fd);
    }
}

std::unique_ptr<HostConnection> connect_to(const std::filesystem::path& socket_path) {
    auto connection = std::make_unique<HostConnection>();
    connection->connect(socket_path.string());
    [[maybe_unused]] const protocol::HelloResult hello =
        connection->handshake(protocol::ServerProfile::Interactive,
                              protocol::ClientInstanceId{generate_uuid_v4()});
    return connection;
}

// D20.7: never trust a socket just because it answers. The hello must name the
// registry row's workspace and boot nonce; a reused/stale socket throws
// `AttachRejected` so the caller re-enters discovery instead of binding a model
// to the wrong daemon.
std::unique_ptr<HostConnection> connect_checked(const std::filesystem::path& socket_path,
                                                const WorkspaceId&           workspace,
                                                const HostBootId&            boot_id,
                                                const AttachIdentity&        identity) {
    auto connection = std::make_unique<HostConnection>();
    connection->connect(socket_path.string());
    const protocol::HelloResult hello =
        connection->handshake(protocol::profile_for_role(identity.role),
                              identity.client_instance, identity.role);
    if (hello.workspace.value != workspace.value || hello.boot_id.value != boot_id.value) {
        connection->close();
        throw HostError(protocol::HostErrorCode::AttachRejected,
                        "attach identity mismatch at " + socket_path.string());
    }
    return connection;
}

} // namespace

protocol::ShutdownReason to_protocol(ShutdownReason reason) noexcept {
    switch (reason) {
        case ShutdownReason::ClientRequest:
            return protocol::ShutdownReason::ClientRequest;
        case ShutdownReason::Signal:
            return protocol::ShutdownReason::Signal;
        case ShutdownReason::StartupFailure:
            return protocol::ShutdownReason::StartupFailure;
        case ShutdownReason::LastSupervisor:
            return protocol::ShutdownReason::LastSupervisor;
        case ShutdownReason::NoOwners:
            return protocol::ShutdownReason::NoOwners;
        case ShutdownReason::WorkspaceStop:
            return protocol::ShutdownReason::WorkspaceStop;
    }
    return protocol::ShutdownReason::ClientRequest;
}

ShutdownReason from_protocol(protocol::ShutdownReason reason) noexcept {
    switch (reason) {
        case protocol::ShutdownReason::ClientRequest:
            return ShutdownReason::ClientRequest;
        case protocol::ShutdownReason::Signal:
            return ShutdownReason::Signal;
        case protocol::ShutdownReason::StartupFailure:
            return ShutdownReason::StartupFailure;
        case protocol::ShutdownReason::LastSupervisor:
            return ShutdownReason::LastSupervisor;
        case protocol::ShutdownReason::NoOwners:
            return ShutdownReason::NoOwners;
        case protocol::ShutdownReason::WorkspaceStop:
            return ShutdownReason::WorkspaceStop;
    }
    return ShutdownReason::ClientRequest;
}

class WorkspaceHost::Impl {
public:
    explicit Impl(HostConfig config) : config_(std::move(config)) {}

    ~Impl() {
        requestWatchdogStop();
        if (watchdog_thread_.joinable()) {
            watchdog_thread_.join();
        }
    }

    HostExitCode run() {
        // 16 §2.5 step 1' (N3-H1/G1): reject before any state (no chdir, no
        // store, no claimHost) so no H21 releaseHost is owed. `foreground` is
        // exempt; both opt-out arms are rejected.
        if (!config_.foreground && !owner_watchdog_armed(config_)) {
            return HostExitCode::StartupRejected;
        }
        HostExitCode code = HostExitCode::Internal;
        try {
            code = startup();
        } catch (const std::exception&) {
            code = HostExitCode::Internal;
        }
        if (code != HostExitCode::Ok) {
            cleanupStartupFailure();
            restoreCwd();
            state_.store(protocol::HostState::Failed);
            return code;
        }

        code = HostExitCode::Internal;
        try {
            code = serve();
        } catch (const std::exception&) {
            cleanupStartupFailure();
            restoreCwd();
            state_.store(protocol::HostState::Failed);
            return HostExitCode::Internal;
        }
        restoreCwd();
        return code;
    }

    void requestShutdown(ShutdownReason reason) noexcept {
        {
            std::lock_guard<std::mutex> lock(shutdown_mutex_);
            if (shutdown_requested_) {
                return;
            }
            shutdown_requested_ = true;
            shutdown_reason_    = reason;
        }
        state_.store(protocol::HostState::Draining);
        if (host_runtime_ != nullptr) {
            host_runtime_->setState(protocol::HostState::Draining);
        }
        shutdown_cv_.notify_all();
    }

    [[nodiscard]] protocol::HostState state() const noexcept { return state_.load(); }
    [[nodiscard]] WorkspaceId workspace() const noexcept { return config_.workspace; }
    [[nodiscard]] HostBootId bootId() const noexcept { return boot_id_; }
    [[nodiscard]] HostPid pid() const noexcept { return pid_; }
    [[nodiscard]] const std::filesystem::path& socketPath() const noexcept {
        return config_.socket_path;
    }

    [[nodiscard]] SessionManager& sessions() { return runtime_->sessions(); }
    [[nodiscard]] WorkspaceRegistry& registry() { return *registry_; }
    [[nodiscard]] SessionStore& store() { return runtime_->store(); }
    [[nodiscard]] ExecutionEnvironment& environment() { return runtime_->environment(); }
    [[nodiscard]] ResourceGovernor& caps() { return runtime_->governor(); }

    [[nodiscard]] std::size_t attachedClients() const noexcept {
        return protocol_ != nullptr ? protocol_->attachedClients() : 0;
    }

    [[nodiscard]] bool ownerless() const noexcept { return escalation_armed_.load(); }

    [[nodiscard]] std::shared_ptr<const std::vector<SupervisorId>> freshOwnerSnapshot() const {
        return ownerSnapshot();
    }

    void activateSession(SessionId session) {
        if (host_runtime_ != nullptr) {
            host_runtime_->activateSession(session);
        }
        active_session_ = std::move(session);
    }

    void suspendSession(SessionId session) noexcept {
        if (host_runtime_ != nullptr) {
            try {
                host_runtime_->suspendSession(session);
            } catch (const std::exception&) {
            }
        }
        if (active_session_.has_value() && active_session_->value == session.value) {
            active_session_.reset();
        }
    }

    [[nodiscard]] std::optional<SessionId> activeSession() const noexcept {
        return active_session_;
    }

    protocol::ClientId onClientAttached(protocol::ClientId id) {
        attached_clients_.fetch_add(1);
        return id;
    }

    void onClientDetached(protocol::ClientId) noexcept {
        const std::size_t current = attached_clients_.load();
        if (current > 0) {
            attached_clients_.fetch_sub(1);
        }
    }

private:
    HostExitCode startup();
    HostExitCode serve();
    HostExitCode coordinator();

    void cleanupStartupFailure();
    void restoreCwd();
    void forwardEvent(const EventRecord& record);
    void armSignals();
    void armHeartbeat();
    void armLeaseRenewal();
    void armOwnerWatchdog();
    void requestWatchdogStop() noexcept;
    void ownerWatchdogLoop();
    void publishOwnerSnapshot(std::shared_ptr<const std::vector<SupervisorId>> snapshot);
    [[nodiscard]] std::shared_ptr<const std::vector<SupervisorId>> ownerSnapshot() const;
    void cancelTimersAndSignals();
    void unlinkSocketIfOwned();
    [[nodiscard]] bool ensureWorkspaceRegistered();

    [[nodiscard]] static HostExitCode mapRuntimeError(WorkspaceRuntimeErrorCode code) noexcept;
    [[nodiscard]] static HostExitCode mapTransportError(protocol::HostErrorCode code) noexcept;

    HostConfig            config_;
    std::filesystem::path canonical_root_;
    std::filesystem::path saved_cwd_;
    bool                  chdir_done_ = false;
    bool                  db_preexisted_ = false;

    HostBootId   boot_id_;
    HostClaim    claim_;
    std::int32_t pid_ = 0;

    asio::io_context                  io_;
    std::unique_ptr<AsioExecutor>     executor_;

    std::unique_ptr<WorkspaceRuntime>       runtime_;
    std::unique_ptr<WorkspaceRegistry>      registry_;
    std::unique_ptr<TransportServerAdapter> permission_adapter_;
    std::unique_ptr<TurnExecutor>           turns_;
    std::unique_ptr<PermissionBroker>       broker_;
    std::unique_ptr<HostRuntime>            host_runtime_;
    std::unique_ptr<ProtocolServer>         protocol_;
    std::unique_ptr<TransportServer>        transport_;

    std::unique_ptr<asio::signal_set>    signals_;
    std::unique_ptr<asio::steady_timer>  heartbeat_timer_;
    std::unique_ptr<asio::steady_timer>  lease_timer_;

    std::atomic<protocol::HostState> state_{protocol::HostState::Starting};
    std::optional<ino_t>             own_socket_inode_;

    std::mutex              shutdown_mutex_;
    std::condition_variable shutdown_cv_;
    bool                    shutdown_requested_ = false;
    ShutdownReason          shutdown_reason_    = ShutdownReason::ClientRequest;

    std::atomic<std::size_t> attached_clients_{0};
    std::optional<SessionId> active_session_;

    // Owner watchdog (16 §5.1). The two liveness atomics are written on the io
    // thread by the ProtocolServer sink and read by the watchdog thread.
    std::atomic<std::size_t>                           live_owner_count_{0};
    std::atomic<std::chrono::steady_clock::time_point> last_owner_frame_mono_{};
    std::atomic<bool>                                  watchdog_stop_{false};
    std::atomic<bool>                                  escalation_armed_{false};
    std::mutex                                         watchdog_mutex_;
    std::condition_variable                            watchdog_cv_;
    std::thread                                        watchdog_thread_;
    std::optional<std::chrono::steady_clock::time_point> ownerless_since_;
    std::chrono::steady_clock::time_point               escalation_deadline_{};
    mutable std::mutex                                  snapshot_mutex_;
    std::shared_ptr<const std::vector<SupervisorId>>    fresh_owner_snapshot_{
        std::make_shared<const std::vector<SupervisorId>>()};
};

HostExitCode WorkspaceHost::Impl::mapRuntimeError(WorkspaceRuntimeErrorCode code) noexcept {
    switch (code) {
        case WorkspaceRuntimeErrorCode::WorkspaceMissing:
            return HostExitCode::WorkspaceMissing;
        case WorkspaceRuntimeErrorCode::StoreUnavailable:
            return HostExitCode::StoreOpenFailed;
        case WorkspaceRuntimeErrorCode::WorkspaceBusy:
            return HostExitCode::WorkspaceBusy;
        case WorkspaceRuntimeErrorCode::ProviderSetupFailed:
        case WorkspaceRuntimeErrorCode::Internal:
            return HostExitCode::StartupRejected;
    }
    return HostExitCode::StartupRejected;
}

HostExitCode WorkspaceHost::Impl::mapTransportError(protocol::HostErrorCode code) noexcept {
    switch (code) {
        case protocol::HostErrorCode::AlreadyRunning:
            return HostExitCode::WorkspaceBusy;
        case protocol::HostErrorCode::SocketPathTooLong:
            return HostExitCode::SocketPathTooLong;
        case protocol::HostErrorCode::SocketUnavailable:
        default:
            return HostExitCode::SocketBindFailed;
    }
}

bool WorkspaceHost::Impl::ensureWorkspaceRegistered() {
    if (registry_->findById(config_.workspace).has_value()) {
        return true;
    }
    if (const std::optional<WorkspaceRecord> existing =
            registry_->findByCanonicalPath(canonical_root_);
        existing.has_value()) {
        return existing->id == config_.workspace;
    }
    try {
        const WorkspaceRecord record =
            registry_->registerWorkspace(canonical_root_, canonical_root_.filename().string());
        config_.workspace = record.id;
        return true;
    } catch (const std::exception& register_error) {
        std::fprintf(stderr, "ymh --host: registerWorkspace failed: %s\n", register_error.what());
        return false;
    }
}

HostExitCode WorkspaceHost::Impl::startup() {
    std::error_code error;
    if (!std::filesystem::is_directory(config_.workspace_root, error)) {
        return HostExitCode::WorkspaceMissing;
    }
    canonical_root_ = std::filesystem::canonical(config_.workspace_root, error);
    if (error) {
        return HostExitCode::WorkspaceMissing;
    }

    if (config_.socket_path.empty()) {
        config_.socket_path = canonical_root_ / ".ymh" / "host.sock";
    }
    if (config_.log_sink.empty()) {
        config_.log_sink = canonical_root_ / ".ymh" / "host.log";
    }

    struct sockaddr_un probe_address {};
    if (config_.socket_path.string().size() >= sizeof(probe_address.sun_path)) {
        return HostExitCode::SocketPathTooLong;
    }

    if (config_.foreground) {
        saved_cwd_ = std::filesystem::current_path(error);
    }
    if (::chdir(canonical_root_.c_str()) != 0) {
        return HostExitCode::StartupRejected;
    }
    chdir_done_ = true;

    const std::filesystem::path state_dir = canonical_root_ / ".ymh";
    std::filesystem::create_directories(state_dir, error);
    if (error) {
        return HostExitCode::StartupRejected;
    }
    ::chmod(state_dir.c_str(), S_IRWXU);
    write_self_ignoring_gitignore(state_dir);

    ::signal(SIGPIPE, SIG_IGN);

    boot_id_ = config_.boot_id.value_or(HostBootId{generate_uuid_v4()});
    pid_     = host_pid();

    executor_ = std::make_unique<AsioExecutor>(io_);

    db_preexisted_ =
        std::filesystem::exists(canonical_root_ / ".ymh" / "sessions.db", error) && !error;
    error.clear();

    WorkspaceRuntimeOptions runtime_options;
    runtime_options.config                = config_.config;
    runtime_options.root                  = canonical_root_;
    runtime_options.boot_id               = to_boot_id(boot_id_);
    runtime_options.attach_permission_gate = false;
    runtime_options.attach_permission_resolver = true;
    runtime_options.store_factory         = config_.store_factory;
    runtime_options.provider_factory      = config_.provider_factory;
    runtime_options.executor              = executor_.get();

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> created =
        WorkspaceRuntime::create(std::move(runtime_options));
    if (!created.has_value()) {
        return mapRuntimeError(created.error().code);
    }
    runtime_ = std::move(*created);

    if (!runtime_->has_provider()) {
        return HostExitCode::StartupRejected;
    }

    try {
        registry_ = WorkspaceRegistry::open(config_.registry);
        registry_->resolvePendingMutations();
    } catch (const std::exception& registry_error) {
        std::fprintf(stderr, "ymh --host: registry open failed: %s\n", registry_error.what());
        return HostExitCode::RegistryFailed;
    }

    if (!ensureWorkspaceRegistered()) {
        std::fprintf(stderr, "ymh --host: workspace registration failed\n");
        return HostExitCode::RegistryFailed;
    }

    permission_adapter_ = std::make_unique<TransportServerAdapter>();
    const std::size_t workers =
        config_.caps.max_llm_concurrency == 0 ? 1 : config_.caps.max_llm_concurrency;
    turns_ = std::make_unique<TurnExecutor>(workers, workers);
    broker_ = std::make_unique<PermissionBroker>(runtime_->policy(), *permission_adapter_,
                                                 to_permission_config(config_.config));
    runtime_->agents().set_permission_resolver(
        [this](const PermissionRequest& request, CancellationToken token) -> PermissionOutcome {
            std::future<PermissionOutcome> outcome = broker_->resolve(request, token);
            if (!outcome.valid()) {
                return PermissionOutcome{payload::PermissionDecisionKind::Deny, "no-resolver"};
            }
            return outcome.get();
        });

    HostIdentity identity;
    identity.workspace   = config_.workspace;
    identity.boot_id     = boot_id_;
    identity.pid         = pid_;
    identity.socket_path = config_.socket_path;

    host_runtime_ = std::make_unique<HostRuntime>(
        *runtime_, *registry_, identity, *turns_, *broker_,
        [this](const EventRecord& record) { forwardEvent(record); });
    host_runtime_->setShutdownHook(
        [this](ShutdownReason reason) { requestShutdown(reason); });

    protocol::ProtocolServerConfig server_config;
    server_config.uid       = protocol::current_uid();
    server_config.workspace = protocol::WorkspaceId{config_.workspace.value};
    server_config.boot_id   = to_protocol_boot_id(boot_id_);
    server_config.pid       = pid_;
    protocol_ = std::make_unique<ProtocolServer>(*host_runtime_, server_config);
    host_runtime_->attachServer(*protocol_);
    protocol_->set_owner_liveness_sink(
        [this](std::size_t live, std::chrono::steady_clock::time_point last) {
            live_owner_count_.store(live);
            last_owner_frame_mono_.store(last);
        });
    host_runtime_->setOwnerSnapshotSource([this] { return ownerSnapshot(); });

    transport_ = std::make_unique<TransportServer>(*protocol_, config_.socket_path.string(),
                                                   io_);
    transport_->setRunnerStartHook(
        [this] { executor_->bindRunnerThread(std::this_thread::get_id()); });
    permission_adapter_->attach(*protocol_, *transport_);

    signals_ = std::make_unique<asio::signal_set>(transport_->io(), SIGTERM, SIGINT, SIGHUP);
    heartbeat_timer_ = std::make_unique<asio::steady_timer>(transport_->io());
    lease_timer_     = std::make_unique<asio::steady_timer>(transport_->io());

    try {
        transport_->start();
    } catch (const protocol::TransportError& transport_error) {
        return mapTransportError(transport_error.code());
    } catch (const std::exception&) {
        return HostExitCode::SocketBindFailed;
    }

    struct stat socket_status {};
    if (::lstat(config_.socket_path.c_str(), &socket_status) == 0) {
        own_socket_inode_ = socket_status.st_ino;
    }

    claim_.workspace   = config_.workspace;
    claim_.pid         = pid_;
    claim_.bootId      = boot_id_;
    claim_.socketPath  = config_.socket_path;
    try {
        registry_->claimHost(claim_);
    } catch (const std::exception& claim_error) {
        std::fprintf(stderr, "ymh --host: claimHost failed: %s\n", claim_error.what());
        return HostExitCode::RegistryFailed;
    }

    // 23 §7 (SL-I19/SL-I20): repair a junction whose session erase committed but
    // whose junction removal was lost to a crash. Runs only in this flock-held
    // startup window (between claimHost and Serving) and only when sessions.db
    // pre-existed the writable open -- a freshly created empty DB would
    // otherwise classify every junction as an orphan.
    if (db_preexisted_ && runtime_->persistence() != nullptr) {
        for (const WorkspaceSessionRecord& junction : registry_->listSessions(config_.workspace)) {
            if (!runtime_->persistence()->load(junction.sessionId).has_value()) {
                try {
                    registry_->removeSession(config_.workspace, junction.sessionId);
                } catch (const std::exception& sweep_error) {
                    std::fprintf(stderr, "ymh --host: orphan sweep failed: %s\n",
                                 sweep_error.what());
                }
            }
        }
    }

    armSignals();
    armHeartbeat();
    armLeaseRenewal();
    armOwnerWatchdog();

    host_runtime_->setState(protocol::HostState::Serving);
    state_.store(protocol::HostState::Serving);
    return HostExitCode::Ok;
}

HostExitCode WorkspaceHost::Impl::serve() {
    std::unique_lock<std::mutex> lock(shutdown_mutex_);
    shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
    lock.unlock();
    return coordinator();
}

HostExitCode WorkspaceHost::Impl::coordinator() {
    state_.store(protocol::HostState::Draining);
    if (host_runtime_ != nullptr) {
        host_runtime_->setState(protocol::HostState::Draining);
    }

    // 24-D3 §4 (amends 16 §4.3): finalize before join. Flush client outbound
    // queues, then cancel every in-flight turn + park + close each resident
    // session, then deny pending permissions so no worker is left blocked on
    // the broker. Only then join (AL13): a worker waiting on a permission
    // future must be woken first or the join deadlocks.
    if (protocol_ != nullptr) {
        protocol_->waitForDrain(config_.shutdown_grace);
    }
    if (runtime_ != nullptr) {
        runtime_->agents().finalizeAll();
    }
    if (broker_ != nullptr) {
        broker_->denyAll("shutdown");
    }

    if (turns_ != nullptr) {
        const TurnExecutor::DrainResult result = turns_->drain(config_.shutdown_grace);
        if (result == TurnExecutor::DrainResult::TimedOut) {
            // AL11: a worker could not be joined within the grace. Destructors
            // must not run over live worker state; best-effort socket unlink
            // then hard-exit, matching 16 §5.1's watchdog.
            unlinkSocketIfOwned();
            std::_Exit(static_cast<int>(HostExitCode::Internal));
        }
    }

    // Quiesced: the ordered teardown below is now destructor-safe. Children
    // (PTY sessions, MCP servers) are torn down before the transport socket
    // stops so none outlives it (24-D9/AL28).
    if (runtime_ != nullptr) {
        runtime_->shutdownChildren(config_.shutdown_grace);
    }

    // 16 §4.3 step 7 (N2-L3/N3-M1): stop and UNCONDITIONALLY join the watchdog
    // before TransportServer::stop(); the stop/join handshake, not a timeout,
    // bounds the wait, so steps 8-9 still run on the orphan path.
    requestWatchdogStop();
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }

    cancelTimersAndSignals();
    if (transport_ != nullptr) {
        transport_->stop();
    }

    if (runtime_ != nullptr) {
        if (SessionPersistence* persistence = runtime_->persistence(); persistence != nullptr) {
            try {
                persistence->close();
            } catch (const std::exception&) {
            }
        }
    }

    if (registry_ != nullptr) {
        try {
            registry_->releaseHost(config_.workspace, boot_id_);
        } catch (const std::exception&) {
        }
    }

    unlinkSocketIfOwned();

    state_.store(protocol::HostState::Stopped);
    if (host_runtime_ != nullptr) {
        host_runtime_->setState(protocol::HostState::Stopped);
    }
    return HostExitCode::Ok;
}

void WorkspaceHost::Impl::cleanupStartupFailure() {
    cancelTimersAndSignals();
    if (transport_ != nullptr) {
        try {
            transport_->stop();
        } catch (const std::exception&) {
        }
    }
    if (runtime_ != nullptr) {
        if (SessionPersistence* persistence = runtime_->persistence(); persistence != nullptr) {
            try {
                persistence->close();
            } catch (const std::exception&) {
            }
        }
    }
    if (registry_ != nullptr && !boot_id_.value.empty()) {
        try {
            registry_->releaseHost(config_.workspace, boot_id_);
        } catch (const std::exception&) {
        }
    }

    signals_.reset();
    heartbeat_timer_.reset();
    lease_timer_.reset();
    transport_.reset();
    protocol_.reset();
    host_runtime_.reset();
    broker_.reset();
    // 24-D3/AL10: ~TurnExecutor hard-exits on a non-quiesced state, so every
    // teardown path must drain first. Startup never submitted a body, so this
    // joins the idle workers immediately.
    if (turns_ != nullptr) {
        (void)turns_->drain(config_.shutdown_grace);
    }
    turns_.reset();
    permission_adapter_.reset();
    registry_.reset();
    runtime_.reset();
}

void WorkspaceHost::Impl::restoreCwd() {
    if (chdir_done_ && config_.foreground && !saved_cwd_.empty()) {
        std::error_code error;
        std::filesystem::current_path(saved_cwd_, error);
    }
    chdir_done_ = false;
}

void WorkspaceHost::Impl::forwardEvent(const EventRecord& record) {
    if (transport_ != nullptr && transport_->running()) {
        transport_->post([this, record] {
            if (protocol_ != nullptr) {
                protocol_->onEventCommitted(record);
            }
        });
    }
}

void WorkspaceHost::Impl::armSignals() {
    if (signals_ == nullptr) {
        return;
    }
    signals_->async_wait([this](const std::error_code& error, int signal_number) {
        if (error) {
            return;
        }
        if (signal_number == SIGTERM || signal_number == SIGINT) {
            requestShutdown(ShutdownReason::Signal);
            return;
        }
        armSignals();
    });
}

void WorkspaceHost::Impl::armHeartbeat() {
    if (heartbeat_timer_ == nullptr || registry_ == nullptr) {
        return;
    }
    heartbeat_timer_->expires_after(config_.heartbeat_interval);
    heartbeat_timer_->async_wait([this](const std::error_code& error) {
        if (error) {
            return;
        }
        try {
            registry_->heartbeat(claim_);
        } catch (const std::exception&) {
        }
        armHeartbeat();
    });
}

void WorkspaceHost::Impl::armLeaseRenewal() {
    if (lease_timer_ == nullptr || runtime_ == nullptr) {
        return;
    }
    lease_timer_->expires_after(config_.persistence.renew_interval);
    lease_timer_->async_wait([this](const std::error_code& error) {
        if (error) {
            return;
        }
        try {
            runtime_->renewLeases();
        } catch (const std::exception&) {
        }
        armLeaseRenewal();
    });
}

void WorkspaceHost::Impl::armOwnerWatchdog() {
    if (!owner_watchdog_armed(config_) || watchdog_thread_.joinable()) {
        return;
    }
    watchdog_stop_.store(false);
    watchdog_thread_ = std::thread([this] { ownerWatchdogLoop(); });
}

void WorkspaceHost::Impl::requestWatchdogStop() noexcept {
    watchdog_stop_.store(true);
    watchdog_cv_.notify_all();
}

void WorkspaceHost::Impl::publishOwnerSnapshot(
    std::shared_ptr<const std::vector<SupervisorId>> snapshot) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    fresh_owner_snapshot_ = std::move(snapshot);
}

std::shared_ptr<const std::vector<SupervisorId>> WorkspaceHost::Impl::ownerSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return fresh_owner_snapshot_;
}

void WorkspaceHost::Impl::ownerWatchdogLoop() {
    const std::chrono::milliseconds idle_timeout = protocol::TransportLimits{}.idle_timeout;
    // 16 §5.1 (R4-L4): the whole-teardown bound = 3 * shutdown_grace (session
    // drain/close, waitForDrain, TurnExecutor::drain) + 2 * watchdog_interval
    // (join handshake + one tick margin) = 34 s at the defaults.
    const std::chrono::milliseconds teardown_budget =
        3 * config_.shutdown_grace + 2 * config_.watchdog_interval;

    std::unique_lock<std::mutex> lock(watchdog_mutex_);
    while (true) {
        watchdog_cv_.wait_for(lock, config_.watchdog_interval);
        if (watchdog_stop_.load()) {
            return;
        }
        try {
            const std::chrono::steady_clock::time_point m = config_.owner_monotonic_clock();
            const std::int64_t w = epoch_ms(config_.owner_wall_clock());
            const bool live = live_owner_count_.load() > 0 &&
                              (m - last_owner_frame_mono_.load()) <= idle_timeout;

            std::vector<SupervisorId> ids;
            for (const SupervisorRow& row : registry_->listSupervisors()) {
                if (isFresh(row, w, config_.owner_lease_ttl)) {
                    ids.push_back(row.id);
                }
            }
            const bool fresh = !ids.empty();
            publishOwnerSnapshot(
                std::make_shared<const std::vector<SupervisorId>>(std::move(ids)));

            if (live || fresh) {
                ownerless_since_.reset();
                escalation_armed_.store(false);
                continue;
            }
            if (!ownerless_since_.has_value()) {
                ownerless_since_ = m;
            }
            if (!escalation_armed_.load() &&
                (m - *ownerless_since_) >= config_.owner_grace) {
                escalation_armed_.store(true);
                escalation_deadline_ = m + teardown_budget;
                requestShutdown(ShutdownReason::NoOwners);
                continue;
            }
            if (escalation_armed_.load() && m >= escalation_deadline_) {
                std::_Exit(static_cast<int>(HostExitCode::Internal));
            }
        } catch (...) {
            // 16 §5.1 (O-M2): publish an EMPTY snapshot (fail-safe toward
            // termination) and keep the thread alive; an escaping exception
            // must never remove orphan prevention.
            publishOwnerSnapshot(std::make_shared<const std::vector<SupervisorId>>());
            continue;
        }
    }
}

void WorkspaceHost::Impl::cancelTimersAndSignals() {
    if (signals_ != nullptr) {
        signals_->cancel();
    }
    if (heartbeat_timer_ != nullptr) {
        heartbeat_timer_->cancel();
    }
    if (lease_timer_ != nullptr) {
        lease_timer_->cancel();
    }
}

void WorkspaceHost::Impl::unlinkSocketIfOwned() {
    if (!own_socket_inode_.has_value()) {
        return;
    }
    struct stat status {};
    if (::lstat(config_.socket_path.c_str(), &status) == 0 && status.st_ino == *own_socket_inode_) {
        ::unlink(config_.socket_path.c_str());
    }
    own_socket_inode_.reset();
}

WorkspaceHost::WorkspaceHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WorkspaceHost::~WorkspaceHost() = default;

std::unique_ptr<WorkspaceHost> WorkspaceHost::create(HostConfig config) {
    std::error_code error;
    if (!std::filesystem::is_directory(config.workspace_root, error)) {
        throw HostError(protocol::HostErrorCode::WorkspaceMissing,
                        "workspace root is not a directory: " + config.workspace_root.string());
    }
    return std::unique_ptr<WorkspaceHost>(
        new WorkspaceHost(std::make_unique<Impl>(std::move(config))));
}

HostExitCode WorkspaceHost::run() { return impl_->run(); }

void WorkspaceHost::requestShutdown(ShutdownReason reason) noexcept {
    impl_->requestShutdown(reason);
}

protocol::HostState WorkspaceHost::state() const noexcept { return impl_->state(); }
WorkspaceId         WorkspaceHost::workspace() const noexcept { return impl_->workspace(); }
HostBootId          WorkspaceHost::bootId() const noexcept { return impl_->bootId(); }
HostPid             WorkspaceHost::pid() const noexcept { return impl_->pid(); }
const std::filesystem::path& WorkspaceHost::socketPath() const noexcept {
    return impl_->socketPath();
}

SessionManager&       WorkspaceHost::sessions() { return impl_->sessions(); }
WorkspaceRegistry&    WorkspaceHost::registry() { return impl_->registry(); }
SessionStore&         WorkspaceHost::store() { return impl_->store(); }
ExecutionEnvironment& WorkspaceHost::environment() { return impl_->environment(); }
ResourceGovernor&     WorkspaceHost::caps() { return impl_->caps(); }

std::size_t WorkspaceHost::attachedClients() const noexcept { return impl_->attachedClients(); }

bool WorkspaceHost::ownerless() const noexcept { return impl_->ownerless(); }

std::shared_ptr<const std::vector<SupervisorId>> WorkspaceHost::freshOwnerSnapshot() const {
    return impl_->freshOwnerSnapshot();
}

void WorkspaceHost::activateSession(SessionId session) {
    impl_->activateSession(std::move(session));
}
void WorkspaceHost::suspendSession(SessionId session) noexcept {
    impl_->suspendSession(std::move(session));
}
std::optional<SessionId> WorkspaceHost::activeSession() const noexcept {
    return impl_->activeSession();
}

protocol::ClientId WorkspaceHost::onClientAttached(protocol::ClientId id) {
    return impl_->onClientAttached(id);
}
void WorkspaceHost::onClientDetached(protocol::ClientId id) noexcept {
    impl_->onClientDetached(id);
}

std::vector<std::string> ForkExecLauncher::build_argv(
    const HostConfig& config, const std::filesystem::path& executable) {
    std::vector<std::string> argv;
    argv.push_back(executable.string());
    argv.push_back("--host");
    argv.push_back("--workspace");
    argv.push_back(config.workspace.value);
    argv.push_back("--root");
    argv.push_back(config.workspace_root.string());
    argv.push_back("--socket");
    argv.push_back(config.socket_path.string());
    if (!config.config_path.empty()) {
        argv.push_back("--config");
        argv.push_back(config.config_path.string());
    }
    if (config.boot_id.has_value()) {
        argv.push_back("--boot-id");
        argv.push_back(config.boot_id->value);
    }
    return argv;
}

HostLauncher::SpawnResult ForkExecLauncher::spawn(const HostConfig& config) {
    std::filesystem::path executable = executable_;
    if (executable.empty()) {
        std::error_code error;
        executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error) {
            throw HostError(protocol::HostErrorCode::SocketUnavailable,
                            "cannot resolve /proc/self/exe");
        }
    }
    if (executable.is_relative()) {
        executable = std::filesystem::absolute(executable);
    }

    std::error_code error;
    std::filesystem::create_directories(config.workspace_root / ".ymh", error);
    write_self_ignoring_gitignore(config.workspace_root / ".ymh");

    const HostBootId boot_id = config.boot_id.value_or(HostBootId{generate_uuid_v4()});
    HostConfig       effective = config;
    effective.boot_id          = boot_id;
    const std::vector<std::string> argv_strings = build_argv(effective, executable);

    const pid_t child = ::fork();
    if (child < 0) {
        throw HostError(protocol::HostErrorCode::SocketUnavailable, "fork failed");
    }
    if (child == 0) {
        ::setsid();
        ::umask(0077);

        const int log_fd =
            ::open(config.log_sink.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (log_fd < 0) {
            ::_exit(static_cast<int>(HostExitCode::StartupRejected));
        }
        ::dup2(log_fd, STDOUT_FILENO);
        ::dup2(log_fd, STDERR_FILENO);
        const int dev_null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDIN_FILENO);
            if (dev_null > STDERR_FILENO) {
                ::close(dev_null);
            }
        }
        if (log_fd > STDERR_FILENO) {
            ::close(log_fd);
        }
        close_extra_fds();

        std::vector<char*> argv;
        argv.reserve(argv_strings.size() + 1);
        for (const std::string& argument : argv_strings) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        ::_exit(static_cast<int>(HostExitCode::StartupRejected));
    }

    return SpawnResult{HostPid{static_cast<std::int32_t>(child)}, boot_id, config.socket_path};
}

void ForkExecLauncher::requestStop(HostPid pid, ShutdownReason) {
    if (pid > 0) {
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    }
}

bool ForkExecLauncher::isAlive(HostPid pid) const {
    if (pid <= 0) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

HostConfig HostLifecycle::configFor(const WorkspaceRecord& record) const {
    HostConfig config;
    config.workspace      = record.id;
    config.workspace_root = record.canonicalPath;
    config.socket_path    = record.canonicalPath / ".ymh" / "host.sock";
    config.log_sink       = record.canonicalPath / ".ymh" / "host.log";
    config.config_path    = config_path_;
    config.registry       = default_registry_config();
    config.persistence.db_path   = record.canonicalPath / ".ymh" / "sessions.db";
    config.persistence.lock_path = record.canonicalPath / ".ymh" / "sessions.lock";
    config.require_owner         = true;
    config.watchdog_disabled     = false;
    return config;
}

ReapResult HostLifecycle::reapIfStale(const WorkspaceRecord& record) {
    if (!record.host.has_value()) {
        return ReapResult::Absent;
    }
    if (registry_.probeLiveness(record.id) == HostLiveness::Live) {
        return ReapResult::Live;
    }
    return registry_.reapHost(record.id) ? ReapResult::Reaped : ReapResult::Absent;
}

AttachResult HostLifecycle::spawnAndAttach(const WorkspaceRecord& record,
                                           const AttachIdentity&  identity) {
    const HostConfig                    config  = configFor(record);
    const HostLauncher::SpawnResult     spawned = launcher_.spawn(config);
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{10};
    std::string last_error;
    while (std::chrono::steady_clock::now() < deadline) {
        // Ordering invariant: the daemon binds its socket before publishing its
        // claim, so a published claim implies the socket is bound. Gate readiness
        // on the claim and connect second; the retry loop covers a socket that is
        // bound but not yet accepting.
        const std::optional<WorkspaceRecord> current = registry_.findById(record.id);
        if (current.has_value() && current->host.has_value()) {
            // D-F1: a claim whose boot nonce is not ours means a concurrent
            // supervisor's daemon won the workspace flock and ours is exiting.
            // Retrying our own nonce can never succeed, so hand off to the
            // winner-attach path immediately instead of burning the budget.
            if (current->host->bootId.value != spawned.bootId.value) {
                last_error = "another daemon owns the workspace";
                break;
            }
            try {
                return AttachResult{connect_checked(spawned.socketPath, record.id,
                                                    spawned.bootId, identity),
                                    true};
            } catch (const std::exception& error) {
                last_error = error.what();
            }
        } else {
            last_error = "host claim not yet published";
        }
        if (!launcher_.isAlive(spawned.pid)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    launcher_.requestStop(spawned.pid, ShutdownReason::StartupFailure);

    // D-F1 spawn race: another supervisor may have won the workspace flock and
    // our daemon exited `WorkspaceBusy`. Attach to the winner instead of
    // failing; never signal it.
    const std::chrono::steady_clock::time_point winner_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < winner_deadline) {
        const std::optional<WorkspaceRecord> current = registry_.findById(record.id);
        if (current.has_value() && current->host.has_value()) {
            try {
                return AttachResult{connect_checked(current->host->socketPath, current->id,
                                                    current->host->bootId, identity),
                                    false};
            } catch (const std::exception& error) {
                last_error = error.what();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    throw HostError(protocol::HostErrorCode::HostUnreachable,
                    "daemon did not become ready: " + last_error);
}

AttachResult HostLifecycle::ensureRunning(WorkspaceId workspace, AttachIdentity identity) {
    const std::optional<WorkspaceRecord> record = registry_.findById(workspace);
    if (!record.has_value()) {
        throw HostError(protocol::HostErrorCode::WorkspaceMissing,
                        "no workspace row for id " + workspace.value);
    }
    if (!record->host.has_value()) {
        return spawnAndAttach(*record, identity);
    }
    if (registry_.probeLiveness(workspace) == HostLiveness::Live) {
        // H10/H11: a held sidecar flock always means live. Attach only if the
        // hello identity matches; a mismatch is refused, never reaped, and a
        // connection failure (e.g. SIGSTOP) never spawns a second daemon.
        return AttachResult{connect_checked(record->host->socketPath, record->id,
                                            record->host->bootId, identity),
                            false};
    }
    reapIfStale(*record);
    return spawnAndAttach(*record, identity);
}

void HostLifecycle::detach(WorkspaceId, protocol::ClientId) {}

void HostLifecycle::requestGracefulStop(WorkspaceId workspace) {
    const std::optional<WorkspaceRecord> record = registry_.findById(workspace);
    if (!record.has_value() || !record->host.has_value()) {
        return;
    }
    try {
        auto connection = connect_to(record->host->socketPath);
        [[maybe_unused]] const nlohmann::json reply =
            connection->request(protocol::method::kHostShutdown, {{"reason", "supervisor stop"}});
        connection->close();
    } catch (const std::exception&) {
    }
}

} // namespace ymh

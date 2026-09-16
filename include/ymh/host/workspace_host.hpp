#pragma once

// WorkspaceHost: the per-workspace daemon process (docs/design/04-workspace-host-daemon.md,
// amended by docs/design/11-m2-errata.md §3/§9/§10/§11).
//
// One `WorkspaceHost` owns exactly one workspace root: it chdirs once, opens the
// writer `SessionPersistence` (and thereby the sidecar flock), opens/reconciles
// the shared `WorkspaceRegistry`, builds the in-process runtime, binds the Unix
// socket, publishes its host claim, and serves the transport until SIGTERM /
// SIGINT / `host.shutdown`.
//
// The construction chain mirrors errata §11.2:
//
//     WorkspaceRuntime            # store, env, bus, sessions, agents, tools, provider
//       └─ HostRuntime            # TransportHost adapter
//            └─ ProtocolServer    # fan-out, dispatch, cursors
//                 └─ TransportServer  # acceptor, io thread, socket
//
// plus the daemon-owned `TurnExecutor` (agent turns never run on the transport
// io thread, errata §3.2 E6/E7) and the `PermissionBroker` (errata §7).
//
// `foreground == true` skips the fork/setsid/exec wrapper: the caller's process
// becomes the daemon and `run()` restores the caller's cwd on exit. Everything
// else — chdir, flock, claim, heartbeat, socket, shutdown — is identical (H19).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ymh/config/config.hpp"
#include "ymh/core/clock.hpp"
#include "ymh/core/ownership.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh {

class ExecutionEnvironment;
class LLMProvider;
class SessionManager;
class SessionStore;
class WorkspaceRuntime;

struct LLMProviderConfig;

// Process exit codes. Distinct so the launcher and tests can assert the cause
// (04 §2.1, errata §3.4 D8). `Internal` is the catch-all for an unexpected
// exception escaping startup/serve.
enum class HostExitCode : std::uint8_t {
    Ok                = 0,
    WorkspaceBusy     = 10,  // another daemon holds sessions.lock (02 §5.1)
    WorkspaceMissing  = 11,  // workspace root absent / not a directory
    StoreOpenFailed   = 12,  // 02 StoreOpenError / SchemaVersionError
    RegistryFailed    = 13,  // 03 RegistryError (open, corrupt, schema, lock)
    SocketBindFailed  = 14,
    SocketPathTooLong = 15,  // sun_path limit (D-F6)
    StartupRejected   = 16,  // invalid args / provider setup / unwritable log sink
    Internal          = 17,  // unexpected exception
};

// Why a shutdown was requested (04 §2.1; 16 §7.3). Total and 1:1 with the
// transport mirror protocol::ShutdownReason (16 §7.4).
enum class ShutdownReason : std::uint8_t {
    ClientRequest,    // host.shutdown with no recognized reason (admission-checked)
    Signal,           // SIGTERM / SIGINT
    StartupFailure,   // a startup step failed after partial state was created
    LastSupervisor,   // the last owner's confirmed exit (§4.4)
    NoOwners,         // owner watchdog: K(d) false for owner_grace (§5.1)
    WorkspaceStop,    // `ymh workspace stop` administrative override (§4.6)
};

// 16 §7.4 (G3): the two conversions between the daemon reason and its transport
// mirror. Both are total and 1:1 over the six values (each is a `-Wswitch`-checked
// switch, so adding a value to either enum without updating the map is a build
// error). There is exactly one mirror→native map, in the HostRuntime adapter.
[[nodiscard]] protocol::ShutdownReason to_protocol(ShutdownReason reason) noexcept;
[[nodiscard]] ShutdownReason from_protocol(protocol::ShutdownReason reason) noexcept;

// Daemon startup/operation error carrying a `protocol::HostErrorCode` (04 §2.2).
// `create()` throws `HostError{WorkspaceMissing}` on a bad root.
class HostError final : public std::runtime_error {
public:
    HostError(protocol::HostErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] protocol::HostErrorCode code() const noexcept { return code_; }

private:
    protocol::HostErrorCode code_;
};

// Pinned configuration (04 §4.1) plus the additive test/supervisor seams the
// errata pin: `boot_id` (supervisor mint-and-pass, §9.1), and the
// `store_factory` / `provider_factory` injection seams (D9/D10) so the
// deterministic suite can run `foreground == true` without a real DB or network.
struct HostConfig {
    WorkspaceId           workspace;       // UUIDv4, never the path
    std::filesystem::path workspace_root;  // canonical realpath; chdir target
    std::filesystem::path socket_path;     // v1 default <root>/.ymh/host.sock
    std::filesystem::path log_sink;        // v1 default <root>/.ymh/host.log

    RegistryConfig    registry;     // 03 §4.1
    PersistenceConfig persistence;  // 02 §4.1; run() sets boot_id before open()

    ResourceCaps              caps;  // 04 §9.11, H16
    std::chrono::milliseconds heartbeat_interval{5'000};
    std::chrono::milliseconds shutdown_grace{10'000};

    // Test/debug: run in the caller's process (no fork, no setsid).
    bool foreground{false};

    // ---- ownership watchdog (16 §7.3, R-M2/N2-H1) -------------------------
    // Armed iff `require_owner && !watchdog_disabled`. `require_owner` defaults
    // TRUE (fail-closed): a daemon built without an explicit opt-out always has
    // a watchdog. `foreground` does NOT participate, so the deterministic suite
    // runs in-process with require_owner=true and injected clocks.
    bool require_owner{true};
    bool watchdog_disabled{false};

    // Cadence defaults come from core/ownership.hpp — the single source (N2-L7).
    std::chrono::milliseconds owner_heartbeat_interval{kOwnerHeartbeatInterval};
    std::chrono::milliseconds owner_lease_ttl{kOwnerLeaseTtl};
    std::chrono::milliseconds watchdog_interval{kOwnerWatchdogInterval};
    std::chrono::milliseconds owner_grace{kOwnerGrace};

    // Two-clock seam (O-M10): the defaults are the real clocks; tests inject
    // fakes. Read only by the watchdog thread.
    MonotonicClock  owner_monotonic_clock{default_monotonic_clock()};
    WallClockReader owner_wall_clock{default_wall_clock()};

    // ---- additive seams ----------------------------------------------------
    Config config;  // layered §37 config; the runtime's provider/agent/policy source

    // Supervisor mint-and-pass (errata §9.1). When unset the daemon mints a
    // UUIDv4 after chdir and before store open (04 §3.3 step 3).
    std::optional<HostBootId> boot_id;

    // D9: substitute the store (must be a non-final `SessionStore`). Unset in
    // production. Caller keeps it alive; one store/one flock (H6).
    std::function<std::unique_ptr<SessionStore>()> store_factory;

    // D10: explicit provider factory (wins over the registry). The CLI fills it
    // from `make_provider_factory()` so `YMH_FAKE_LLM_SCRIPT` works identically
    // in-process and across the two-process harness.
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory;
};

// 16 §5.1/§7.3: the owner watchdog is armed iff `require_owner && !watchdog_disabled`.
[[nodiscard]] inline bool owner_watchdog_armed(const HostConfig& config) noexcept {
    return config.require_owner && !config.watchdog_disabled;
}

// The daemon (04 §4.2). `create()` validates and builds the object; `run()` is
// the only entry point that mutates process state (chdir, flock, socket).
class WorkspaceHost {
public:
    // Validates config and constructs the daemon object; does NOT chdir, open
    // the store, or bind. Throws `HostError{WorkspaceMissing}` on a bad root.
    [[nodiscard]] static std::unique_ptr<WorkspaceHost> create(HostConfig config);

    ~WorkspaceHost();

    WorkspaceHost(const WorkspaceHost&) = delete;
    WorkspaceHost& operator=(const WorkspaceHost&) = delete;

    // Blocking entry point: runs the §3.3 startup sequence, then the loop until
    // a shutdown trigger; returns the process exit code.
    HostExitCode run();

    // Idempotent; safe to call from a signal-handler bridge or a client request.
    void requestShutdown(ShutdownReason reason) noexcept;

    // ---- introspection (all noexcept) -------------------------------------
    [[nodiscard]] protocol::HostState state() const noexcept;
    [[nodiscard]] WorkspaceId         workspace() const noexcept;
    [[nodiscard]] HostBootId          bootId() const noexcept;  // valid after §3.3 step 3
    [[nodiscard]] HostPid             pid() const noexcept;     // getpid()
    [[nodiscard]] const std::filesystem::path& socketPath() const noexcept;

    // ---- owned runtime (valid only while Serving) -------------------------
    [[nodiscard]] SessionManager&       sessions();
    [[nodiscard]] WorkspaceRegistry&    registry();
    [[nodiscard]] SessionStore&         store();
    [[nodiscard]] ExecutionEnvironment& environment();
    [[nodiscard]] ResourceGovernor&     caps();

    [[nodiscard]] std::size_t attachedClients() const noexcept;

    // ---- owner watchdog introspection (16 §7.3) ---------------------------
    // True once the owner watchdog has fired (diagnostics/tests).
    [[nodiscard]] bool ownerless() const noexcept;
    // The watchdog's published immutable fresh-owner snapshot (16 §5.1).
    [[nodiscard]] std::shared_ptr<const std::vector<SupervisorId>> freshOwnerSnapshot() const;

    // Single-active-session control (decision (m)).
    void                     activateSession(SessionId session);
    void                     suspendSession(SessionId session) noexcept;
    [[nodiscard]] std::optional<SessionId> activeSession() const noexcept;

    // Spec 05 calls these as frames arrive; the daemon owns the bookkeeping.
    protocol::ClientId onClientAttached(protocol::ClientId id);
    void               onClientDetached(protocol::ClientId id) noexcept;

private:
    class Impl;

    explicit WorkspaceHost(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace ymh

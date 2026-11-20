#pragma once

// Supervisor-side spawn/attach seam (docs/design/04-workspace-host-daemon.md §6,
// amended by docs/design/11-m2-errata.md §9).
//
// `HostLauncher` owns process spawn/stop; `ForkExecLauncher` is the real
// fork+setsid+execve implementation (04 §3.2). `HostLifecycle` implements
// `ensureRunning` (04 §6.2): attach to a live daemon or spawn one, clearing a
// stale claim only when the sidecar lock is absent (never killing on `ps`
// evidence, H11). The supervisor never `chdir()`s (E17).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ymh/host/workspace_host.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::protocol {
class HostConnection;
} // namespace ymh::protocol

namespace ymh {

// The caller's pinned identity for an attach (16 §7.3 C-H2, 16-D8). The profile
// is derived from the role (16 §7.4 C-M6): `Automation ⇒ Automation`, otherwise
// `Interactive`.
struct AttachIdentity {
    protocol::ClientInstanceId client_instance;
    protocol::ClientRole       role{protocol::ClientRole::Supervisor};
};

// Spawns and signals daemon processes. Tests inject a fake launcher or run the
// host in-process (04 §6.1).
class HostLauncher {
public:
    virtual ~HostLauncher() = default;

    struct SpawnResult {
        HostPid                pid;         // child pid (> 0)
        HostBootId             bootId;      // provisional until host.hello
        std::filesystem::path  socketPath;  // the path the daemon will bind

        // 69-D2/69-D5 (69-host-startup-diagnosability-errata): read end of the
        // CLOEXEC pre-exec diagnostic pipe, or -1 when none exists (fake
        // launchers; pipe-creation failure). Parent-owned and single-consumer:
        // the caller MUST close it. `HostLifecycle::spawnAndAttach` wraps it in
        // an RAII closer on every path. It holds the child's `strerror(errno)`
        // reason for a log-sink-open or `execve` failure, and is empty on a
        // successful `execve` (the write end is CLOEXEC).
        int                    diagnostic_fd{-1};
    };

    // Fork+setsid+exec. Returns as soon as the child is exec'd; readiness is
    // established by connecting and completing the handshake.
    virtual SpawnResult spawn(const HostConfig& config) = 0;

    // SIGTERM. Explicit supervisor-driven stop, never orphan reaping (H11).
    virtual void requestStop(HostPid pid, ShutdownReason reason) = 0;

    // True iff kill(pid, 0) does not report ESRCH. A hint only (03 §6.3): a
    // zombie still satisfies it, so never use it alone to detect an exit.
    [[nodiscard]] virtual bool isAlive(HostPid pid) const = 0;

    // Reaps a spawned child that has already exited; returns WEXITSTATUS (or
    // 128+signal), else `nullopt` while it runs or when the pid is not this
    // process's child (an injected fake). The authoritative early-exit signal.
    [[nodiscard]] virtual std::optional<int> tryReap(HostPid /*pid*/) { return std::nullopt; }
};

// Real launcher: fork + setsid + open log sink + dup2 + execve the absolute
// `/proc/self/exe` with the pinned `--host` argv (04 §3.2).
class ForkExecLauncher final : public HostLauncher {
public:
    ForkExecLauncher() = default;
    explicit ForkExecLauncher(std::filesystem::path executable)
        : executable_(std::move(executable)) {}

    SpawnResult spawn(const HostConfig& config) override;
    void        requestStop(HostPid pid, ShutdownReason reason) override;
    [[nodiscard]] bool isAlive(HostPid pid) const override;
    [[nodiscard]] std::optional<int> tryReap(HostPid pid) override;

    void setExecutable(std::filesystem::path executable) {
        executable_ = std::move(executable);
    }

    // The argv the launcher will exec, exposed for tests (04 §3.2). `executable`
    // defaults to the launcher's configured path.
    [[nodiscard]] static std::vector<std::string> build_argv(
        const HostConfig& config, const std::filesystem::path& executable);

private:
    std::filesystem::path executable_;
};

struct AttachResult {
    std::unique_ptr<protocol::HostConnection> connection;
    bool                                      spawned = false;
};

// Outcome of a lazy stale-claim clear (§7.3). Never the outcome of a signal.
enum class ReapResult : std::uint8_t {
    Live,    // the sidecar lock is held; the workspace is live, nothing changed
    Reaped,  // the claim was stale and was cleared under the D22 write lock
    Absent,  // no claim was recorded; nothing to do
};

// 76-D3: the whole bare-`ymh` synchronous attach is bounded by `total`; each
// sub-step is separately capped so one slow phase cannot consume the rest.
inline constexpr std::chrono::milliseconds kAttachTotalBudget{25000};

struct AttachBudget {
    std::chrono::milliseconds connect{protocol::kHostConnectTimeout};  // 5 s (76-D1)
    std::chrono::milliseconds handshake{5000};        // matches 05 handshake default
    std::chrono::milliseconds spawn_readiness{10000}; // existing spawnAndAttach
    std::chrono::milliseconds winner{5000};           // existing winner loop
    std::chrono::milliseconds total{kAttachTotalBudget};  // hard ceiling (76-I4)
};

// 76-D4/76-D5: how a `Live` (held-lock) claim was corroborated.
enum class LiveProbeOutcome : std::uint8_t {
    Attached,               // authenticated hello matched the claim
    NoHolder,               // the sidecar lock is free (claim is stale)
    IdentityMismatch,       // hello answered, named a different workspace/boot
    OurDaemonUnresponsive,  // lock diagnostic matches the claim, hello failed
    ForeignLockHolder,      // lock diagnostic missing or names a different boot
};

struct LiveProbe {
    LiveProbeOutcome                          outcome = LiveProbeOutcome::ForeignLockHolder;
    std::unique_ptr<protocol::HostConnection> connection;  // set iff Attached
    std::optional<std::int32_t>               holder_pid;  // diagnostic when known
};

class HostLifecycle {
public:
    HostLifecycle(HostLauncher& launcher, WorkspaceRegistry& registry,
                  std::filesystem::path config_path = {})
        : launcher_(launcher), registry_(registry), config_path_(std::move(config_path)) {}

    // Attach to a live daemon or spawn one. NEVER kills a process. May clear a
    // stale claim (lazy reap) under the D22 write lock (03 §6.4, H10, H11).
    // The supplied identity is sent verbatim at hello (C-H2); the profile is
    // derived from `identity.role` (C-M6). `budget` bounds the whole call
    // (76-D3); the default is the production budget.
    AttachResult ensureRunning(WorkspaceId workspace, AttachIdentity identity,
                               const AttachBudget& budget = AttachBudget{});

    // Detach this supervisor. Does NOT terminate the daemon (H8, H9).
    void detach(WorkspaceId workspace, protocol::ClientId client);

    // Explicit graceful stop (host.shutdown). Distinct from detach.
    void requestGracefulStop(WorkspaceId workspace);

private:
    // Clears a claim only when lock-absence holds, under the D22 write lock.
    ReapResult reapIfStale(const WorkspaceRecord& record);

    // 76-D3: takes the budget + the overall deadline; the hardcoded 10 s / 5 s
    // loop bounds now derive from `budget` and `remaining_until`.
    AttachResult spawnAndAttach(const WorkspaceRecord& record,
                                const AttachIdentity&   identity,
                                const AttachBudget&     budget,
                                std::chrono::steady_clock::time_point deadline);

    // 76-D4/76-D5: re-probes the sidecar lock and only attaches after a bounded
    // authenticated hello naming the claim's workspace and boot. Never reaps,
    // never signals, never spawns.
    [[nodiscard]] LiveProbe probeLive(const WorkspaceRecord& record,
                                      const AttachIdentity&  identity,
                                      const AttachBudget&    budget,
                                      std::chrono::steady_clock::time_point deadline);

    [[nodiscard]] HostConfig configFor(const WorkspaceRecord& record) const;

    HostLauncher&      launcher_;
    WorkspaceRegistry& registry_;
    std::filesystem::path config_path_;
};

} // namespace ymh

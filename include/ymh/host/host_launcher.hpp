#pragma once

// Supervisor-side spawn/attach seam (docs/design/04-workspace-host-daemon.md §6,
// amended by docs/design/11-m2-errata.md §9).
//
// `HostLauncher` owns process spawn/stop; `ForkExecLauncher` is the real
// fork+setsid+execve implementation (04 §3.2). `HostLifecycle` implements
// `ensureRunning` (04 §6.2): attach to a live daemon or spawn one, clearing a
// stale claim only when the sidecar lock is absent (never killing on `ps`
// evidence, H11). The supervisor never `chdir()`s (E17).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ymh/host/workspace_host.hpp"
#include "ymh/registry/registry.hpp"

namespace ymh::protocol {
class HostConnection;
} // namespace ymh::protocol

namespace ymh {

// Spawns and signals daemon processes. Tests inject a fake launcher or run the
// host in-process (04 §6.1).
class HostLauncher {
public:
    virtual ~HostLauncher() = default;

    struct SpawnResult {
        HostPid                pid;         // child pid (> 0)
        HostBootId             bootId;      // provisional until host.hello
        std::filesystem::path  socketPath;  // the path the daemon will bind
    };

    // Fork+setsid+exec. Returns as soon as the child is exec'd; readiness is
    // established by connecting and completing the handshake.
    virtual SpawnResult spawn(const HostConfig& config) = 0;

    // SIGTERM. Explicit supervisor-driven stop, never orphan reaping (H11).
    virtual void requestStop(HostPid pid, ShutdownReason reason) = 0;

    // True iff kill(pid, 0) does not report ESRCH. A hint only (03 §6.3).
    [[nodiscard]] virtual bool isAlive(HostPid pid) const = 0;
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

class HostLifecycle {
public:
    HostLifecycle(HostLauncher& launcher, WorkspaceRegistry& registry)
        : launcher_(launcher), registry_(registry) {}

    // Attach to a live daemon or spawn one. NEVER kills a process. May clear a
    // stale claim (lazy reap) under the D22 write lock (03 §6.4, H10, H11).
    AttachResult ensureRunning(WorkspaceId workspace);

    // Detach this supervisor. Does NOT terminate the daemon (H8, H9).
    void detach(WorkspaceId workspace, protocol::ClientId client);

    // Explicit graceful stop (host.shutdown). Distinct from detach.
    void requestGracefulStop(WorkspaceId workspace);

private:
    // Clears a claim only when lock-absence holds, under the D22 write lock.
    ReapResult reapIfStale(const WorkspaceRecord& record);

    AttachResult spawnAndAttach(const WorkspaceRecord& record);
    [[nodiscard]] HostConfig configFor(const WorkspaceRecord& record) const;

    HostLauncher&      launcher_;
    WorkspaceRegistry& registry_;
};

} // namespace ymh

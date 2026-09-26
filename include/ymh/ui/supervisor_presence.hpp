#pragma once

// Supervisor presence: the ownership-of-record row this process keeps in the
// shared registry, plus the daemon-set scan that discovers live daemons
// (docs/design/16-daemon-ownership.md §2.3, §3.2, §7.5).

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ymh/core/clock.hpp"
#include "ymh/core/ownership.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/registry/supervisor.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor.hpp"

namespace ymh::ui {

// 16-D8. Mints the process's ClientInstanceId once, in memory; stable for the
// process lifetime and never persisted, so two concurrent supervisors cannot
// collide on a shared file.
[[nodiscard]] protocol::ClientInstanceId process_client_instance();

// 16 §4.1 orphaning predicate. `live_supervisors` is a raw count INCLUDING the
// caller, so `== 1` means "just me"; `other_fresh_owners` already excludes the
// caller. Display/exit logic only; never writes the registry.
[[nodiscard]] bool is_orphaning_view(const protocol::OwnershipView& view) noexcept;

// Owns this supervisor's `supervisors` row: register at start, heartbeat on a
// timer, deregister on clean exit. Never dereferences another supervisor's row.
class SupervisorPresence {
public:
    struct Options {
        std::chrono::milliseconds heartbeat_interval{kOwnerHeartbeatInterval};
        std::chrono::milliseconds owner_lease_ttl{kOwnerLeaseTtl};
        std::optional<std::string> tty;   // display only
        WallClockReader wall_clock{default_wall_clock()};
    };

    // Registers (idempotent upsert). Must run before any daemon is spawned so
    // FRESH_OWNER covers the spawn window (§2.6).
    static SupervisorPresence registerSelf(WorkspaceRegistry& registry, SupervisorId id,
                                           Options options);

    // Timer-driven. `now_wall_ms` is WALL epoch ms (16-D10). Returns false when
    // the row was pruned by a peer (C-M8): the caller MUST re-register and
    // reconnect (§2.6).
    [[nodiscard]] bool heartbeat(std::int64_t now_wall_ms);
    void reRegister();   // after a false heartbeat (C-M8)

    // Confirmed-exit path (§4.3). Best-effort: it catches registry errors
    // internally and inspects the registry call's return value, never throwing
    // (N2-L4/L5). A failed deregister leaves the row to age out (ttl) or be
    // pruned.
    void deregister() noexcept;

    [[nodiscard]] const SupervisorId& id() const noexcept;
    [[nodiscard]] std::chrono::milliseconds heartbeat_interval() const noexcept;
    [[nodiscard]] std::chrono::milliseconds owner_lease_ttl() const noexcept;

private:
    SupervisorPresence(WorkspaceRegistry& registry, SupervisorId id, Options options);

    [[nodiscard]] SupervisorRow row(std::int64_t heartbeat_ms) const;

    WorkspaceRegistry* registry_;
    SupervisorId       id_;
    Options            options_;
    std::string        boot_id_;
    std::int64_t       started_at_ms_{0};
    std::int64_t       last_heartbeat_ms_{0};
};

// The continuous registry scan (§3.2, 16-D3/O7). Read-only: it never takes the
// registry write lock and never spawns. Owns its own thread; the sink runs on
// that thread and must post to the UI thread itself.
class DaemonSetScanner {
public:
    using Sink = std::function<void(std::vector<SupervisorWorkspace>)>;

    DaemonSetScanner(WorkspaceRegistry& registry, std::chrono::milliseconds scan_interval,
                     Sink sink);
    ~DaemonSetScanner();

    DaemonSetScanner(const DaemonSetScanner&) = delete;
    DaemonSetScanner& operator=(const DaemonSetScanner&) = delete;

    void start();
    void stop();

    // One read-only tick (§3.2). Bounded: O(workspaces) `probeLiveness`
    // open+flock syscalls per tick (C-L3), one read transaction, no filesystem
    // walk. Returns only workspaces whose host claim is live.
    [[nodiscard]] std::vector<SupervisorWorkspace> scanOnce() const;

private:
    void loop();

    WorkspaceRegistry*        registry_;
    std::chrono::milliseconds scan_interval_;
    Sink                      sink_;
    std::thread               thread_;
    std::mutex                mutex_;
    std::condition_variable   cv_;
    bool                      running_{false};
};

} // namespace ymh::ui

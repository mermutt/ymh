#include "ymh/ui/supervisor_presence.hpp"

#include <exception>
#include <utility>

#include <unistd.h>

#include "ymh/core/clock.hpp"

namespace ymh::ui {

protocol::ClientInstanceId process_client_instance() {
    static const protocol::ClientInstanceId instance{generate_uuid_v4()};
    return instance;
}

bool is_orphaning_view(const protocol::OwnershipView& view) noexcept {
    return view.live_supervisors == 1 && view.live_automation == 0 &&
           view.other_fresh_owners == 0;
}

SupervisorPresence::SupervisorPresence(WorkspaceRegistry& registry, SupervisorId id,
                                       Options options)
    : registry_(&registry), id_(std::move(id)), options_(std::move(options)),
      boot_id_(generate_uuid_v4()) {
    last_heartbeat_ms_ = epoch_ms(options_.wall_clock());
    started_at_ms_ = last_heartbeat_ms_;
}

SupervisorPresence SupervisorPresence::registerSelf(WorkspaceRegistry& registry, SupervisorId id,
                                                    Options options) {
    SupervisorPresence presence(registry, std::move(id), std::move(options));
    presence.registry_->registerSupervisor(presence.row(presence.last_heartbeat_ms_));
    return presence;
}

SupervisorRow SupervisorPresence::row(std::int64_t heartbeat_ms) const {
    SupervisorRow result;
    result.id = id_;
    result.pid = static_cast<std::int32_t>(::getpid());
    result.bootId = boot_id_;
    result.startedAtMs = started_at_ms_;
    result.heartbeatMs = heartbeat_ms;
    result.tty = options_.tty;
    return result;
}

bool SupervisorPresence::heartbeat(std::int64_t now_wall_ms) {
    const bool updated = registry_->heartbeatSupervisor(id_, now_wall_ms);
    if (updated) {
        last_heartbeat_ms_ = now_wall_ms;
    }
    return updated;
}

void SupervisorPresence::reRegister() {
    last_heartbeat_ms_ = epoch_ms(options_.wall_clock());
    registry_->registerSupervisor(row(last_heartbeat_ms_));
}

void SupervisorPresence::deregister() noexcept {
    try {
        static_cast<void>(registry_->deregisterSupervisor(id_));
    } catch (const std::exception&) {
    } catch (...) {
    }
}

const SupervisorId& SupervisorPresence::id() const noexcept { return id_; }

std::chrono::milliseconds SupervisorPresence::heartbeat_interval() const noexcept {
    return options_.heartbeat_interval;
}

std::chrono::milliseconds SupervisorPresence::owner_lease_ttl() const noexcept {
    return options_.owner_lease_ttl;
}

DaemonSetScanner::DaemonSetScanner(WorkspaceRegistry& registry,
                                   std::chrono::milliseconds scan_interval, Sink sink)
    : registry_(&registry), scan_interval_(scan_interval), sink_(std::move(sink)) {}

DaemonSetScanner::~DaemonSetScanner() { stop(); }

std::vector<SupervisorWorkspace> DaemonSetScanner::scanOnce() const {
    std::vector<SupervisorWorkspace> live;
    for (const WorkspaceRecord& record : registry_->listWorkspaces()) {
        if (!record.host.has_value()) {
            continue;
        }
        if (registry_->probeLiveness(record.id) != HostLiveness::Live) {
            continue;
        }
        SupervisorWorkspace workspace;
        workspace.id = WorkspaceId{record.id.value};
        workspace.cwd = record.canonicalPath.string();
        workspace.title = record.displayTitle;
        workspace.socket_path = record.host->socketPath.string();
        workspace.boot_id = record.host->bootId.value;
        live.push_back(std::move(workspace));
    }
    return live;
}

void DaemonSetScanner::start() {
    {
        const std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    thread_ = std::thread([this] { loop(); });
}

void DaemonSetScanner::stop() {
    {
        const std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DaemonSetScanner::loop() {
    while (true) {
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, scan_interval_, [this] { return !running_; })) {
            return;
        }
        lock.unlock();
        std::vector<SupervisorWorkspace> live;
        try {
            live = scanOnce();
        } catch (const std::exception&) {
            continue;
        }
        if (sink_) {
            sink_(std::move(live));
        }
    }
}

} // namespace ymh::ui

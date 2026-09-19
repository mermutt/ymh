#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool_context.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh::test {

inline std::filesystem::path make_temp_dir(const std::string& prefix) {
    const auto base =
        std::filesystem::temp_directory_path() /
        (prefix + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

class TempWorkspace {
public:
    explicit TempWorkspace(const std::string& prefix)
        : path_(make_temp_dir(prefix)) {}

    ~TempWorkspace() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void write(const std::string& relative, const std::string& content) const {
        const std::filesystem::path target = path_ / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << content;
    }

private:
    std::filesystem::path path_;
};

class MemorySessionStore final : public SessionStore {
public:
    SessionHeader create(SessionHeader header) override {
        std::lock_guard<std::mutex> lock(mutex_);
        headers_[header.id.value] = header;
        logs_[header.id.value] = {};
        return header;
    }

    std::optional<SessionHeader> load(SessionId id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = headers_.find(id.value);
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<SessionHeader> list() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SessionHeader> result;
        for (const auto& [key, header] : headers_) {
            result.push_back(header);
        }
        return result;
    }

    void erase(SessionId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        headers_.erase(id.value);
        logs_.erase(id.value);
    }

    EventRange read(SessionId id, Sequence after = 0) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        EventRange result;
        for (const EventRecord& record : logs_.at(id.value)) {
            if (record.seq > after) {
                result.push_back(record);
            }
        }
        return result;
    }

    EventRange readRange(SessionId id, Sequence from, Sequence to) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        EventRange result;
        for (const EventRecord& record : logs_.at(id.value)) {
            if (record.seq >= from && record.seq <= to) {
                result.push_back(record);
            }
        }
        return result;
    }

    Sequence append(SessionId id, Event event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!lease_) {
            throw LeaseLost("memory store is not the lease holder");
        }
        if (throw_on_append_type.has_value() && event.type == *throw_on_append_type) {
            throw StoreError("injected append failure");
        }
        const Sequence seq = ++global_;
        logs_[id.value].push_back(EventRecord{seq, std::move(event)});
        return seq;
    }

    bool isLeaseHolder(SessionId) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lease_;
    }

    bool lease_ = true;
    // Test seam: when set, `append` rejects an event of this type (19 §4.3 L7).
    std::optional<EventType> throw_on_append_type;

private:
    mutable std::mutex                             mutex_;
    std::unordered_map<std::string, SessionHeader> headers_;
    std::unordered_map<std::string, EventRange>    logs_;
    Sequence                                       global_ = 0;
};

struct ToolEnv {
    explicit ToolEnv(const std::string& prefix,
                     SandboxMode mode = SandboxMode::Workspace,
                     ToolConfig config = {},
                     ResourceCaps caps = {},
                     PtyService* pty = nullptr)
        : workspace(prefix), env(workspace.path(), mode, config, pty), governor(caps) {
        header.id = make_session_id();
        header.cwd = workspace.path();
        header.createdAt = 0;
        header.updatedAt = 0;
        header.model = "test";
        header.serverProfile = "interactive";
        store.create(header);
        session.emplace(header, store, bus);
    }

    ToolContext context(const std::string& call_id = "call-1",
                        TurnId turn = 1,
                        StepId step = 1) {
        return ToolContext(env, *session, logger, CancellationToken{}, governor, sink,
                           permission, call_id, turn, step);
    }

    TempWorkspace workspace;
    EventBus bus;
    MemorySessionStore store;
    SessionHeader header;
    std::optional<Session> session;
    LocalEnvironment env;
    ResourceGovernor governor;
    OutputRing ring{1u << 20};
    RingOutputSink sink{ring};
    StaticPermissionHandle permission{payload::PermissionDecisionKind::Allow};
    NullLogger logger;
};

// Keeps `Registration` handles alive: dropping one unregisters the tool
// (07 §4.2), so tests must hold them for the registry's lifetime.
class RegistrationKeeper {
public:
    explicit RegistrationKeeper(ToolRegistry& registry) : registry_(registry) {}

    void add(std::unique_ptr<Tool> tool) {
        registrations_.push_back(registry_.add(std::move(tool)));
    }

private:
    ToolRegistry& registry_;
    std::vector<ToolRegistry::Registration> registrations_;
};

} // namespace ymh::test

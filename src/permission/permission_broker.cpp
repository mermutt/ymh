#include "ymh/permission/permission_broker.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ymh {
namespace {

struct Pending {
    PermissionRequest               core;
    protocol::PermissionRequest     wire;
    std::promise<PermissionOutcome> promise;
    Clock::time_point               deadline{};
    bool                            resolved = false;
};

std::future<PermissionOutcome> ready_outcome(PermissionOutcome outcome) {
    std::promise<PermissionOutcome> promise;
    promise.set_value(std::move(outcome));
    return promise.get_future();
}

std::int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

nlohmann::json bounded_arguments(const nlohmann::json& arguments,
                                 std::size_t max_bytes,
                                 bool& truncated) {
    try {
        if (max_bytes == 0 || arguments.dump().size() > max_bytes) {
            truncated = true;
            return nlohmann::json{{"truncated", true}};
        }
        return arguments;
    } catch (...) {
        truncated = true;
        return nlohmann::json{{"truncated", true}};
    }
}

std::string bounded_summary(const PermissionRequest& request, bool truncated) {
    constexpr std::size_t kMaxSummary = 256;

    std::string summary = request.tool;
    try {
        if (request.arguments.is_object()) {
            const auto command = request.arguments.find("command");
            if (command != request.arguments.end() && command->is_string()) {
                summary += " ";
                summary += command->get<std::string>();
            } else if (request.path.has_value()) {
                summary += " ";
                summary += request.path->generic_string();
            }
        } else if (request.path.has_value()) {
            summary += " ";
            summary += request.path->generic_string();
        }
    } catch (...) {
        truncated = true;
    }

    if (summary.size() > kMaxSummary) {
        summary.resize(kMaxSummary);
        summary += "...";
    }
    if (truncated) {
        summary += " (truncated)";
    }
    return summary;
}

protocol::PermissionRequest make_wire(const PermissionRequest& request,
                                      const std::string& request_id,
                                      const PermissionConfig& config) {
    protocol::PermissionRequest wire;
    wire.request_id = request_id;
    wire.session = request.session;
    wire.tool = request.tool;

    bool truncated = false;
    wire.arguments = bounded_arguments(request.arguments, config.arguments_max_bytes, truncated);
    wire.summary = bounded_summary(request, truncated);
    wire.expires_at_ms = wall_now_ms() + config.permission_timeout.count();
    return wire;
}

payload::PermissionDecisionKind decision_kind(protocol::PermissionAnswer answer,
                                              protocol::PermissionScope scope) {
    if (answer == protocol::PermissionAnswer::Deny) {
        return payload::PermissionDecisionKind::Deny;
    }
    if (scope == protocol::PermissionScope::Always) {
        return payload::PermissionDecisionKind::AllowAlways;
    }
    return payload::PermissionDecisionKind::Allow;
}

GrantScope grant_scope(protocol::PermissionScope scope) {
    switch (scope) {
        case protocol::PermissionScope::Once:    return GrantScope::Once;
        case protocol::PermissionScope::Session: return GrantScope::Session;
        case protocol::PermissionScope::Always:  return GrantScope::Always;
    }
    return GrantScope::Once;
}

} // namespace

struct PermissionBroker::State {
    State(PermissionPolicy& policy_ref,
          PermissionTransport& transport_ref,
          PermissionConfig config_value,
          ClockReader now_reader)
        : policy(policy_ref),
          transport(transport_ref),
          config(std::move(config_value)),
          now(std::move(now_reader)) {}

    std::size_t subscriber_count_for(const SessionId& session) const {
        SubscriberCount reader;
        {
            std::lock_guard<std::mutex> lock(mutex);
            reader = subscriber_count;
        }
        return reader ? reader(session) : 0;
    }

    std::shared_ptr<Pending> claim(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if ((*it)->wire.request_id != id) {
                continue;
            }
            if ((*it)->resolved) {
                return nullptr;
            }
            (*it)->resolved = true;
            std::shared_ptr<Pending> claimed = *it;
            pending.erase(it);
            return claimed;
        }
        return nullptr;
    }

    std::future<PermissionOutcome> resolve(const PermissionRequest& request,
                                           CancellationToken cancel,
                                           std::weak_ptr<State> weak);

    PermissionPolicy&  policy;
    PermissionTransport& transport;
    PermissionConfig   config;
    ClockReader        now;

    mutable std::mutex                    mutex;
    std::vector<std::shared_ptr<Pending>> pending;
    std::uint64_t                         next_id = 1;
    SubscriberCount                       subscriber_count;
    bool                                  shutting_down = false;
    std::string                           shutdown_reason = "shutdown";
};

std::future<PermissionOutcome> PermissionBroker::State::resolve(const PermissionRequest& request,
                                                                CancellationToken cancel,
                                                                std::weak_ptr<State> weak) {
    const PolicyVerdict verdict = policy.evaluate(request);
    if (verdict == PolicyVerdict::Allow) {
        return ready_outcome({payload::PermissionDecisionKind::Allow, "rule"});
    }
    if (verdict == PolicyVerdict::Deny) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "rule"});
    }
    if (cancel.cancelled()) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "cancelled"});
    }
    if (subscriber_count_for(request.session) == 0) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "no-subscribers"});
    }

    auto entry = std::make_shared<Pending>();
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (shutting_down) {
            return ready_outcome({payload::PermissionDecisionKind::Deny, shutdown_reason});
        }
        id = "perm-" + std::to_string(next_id++);
        entry->core = request;
        entry->wire = make_wire(request, id, config);
        entry->deadline = now() + config.permission_timeout;
        pending.push_back(entry);
    }

    std::future<PermissionOutcome> future = entry->promise.get_future();

    cancel.on_cancel([weak, id] {
        if (std::shared_ptr<State> locked = weak.lock()) {
            if (std::shared_ptr<Pending> claimed = locked->claim(id)) {
                claimed->promise.set_value(
                    {payload::PermissionDecisionKind::Deny, "cancelled"});
            }
        }
    });

    if (config.permission_timeout.count() > 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry->deadline - now());
        const std::chrono::milliseconds delay =
            remaining.count() > 0 ? remaining : std::chrono::milliseconds{1};
        const bool scheduled = transport.schedule_after(delay, [weak, id] {
            if (std::shared_ptr<State> locked = weak.lock()) {
                if (std::shared_ptr<Pending> claimed = locked->claim(id)) {
                    claimed->promise.set_value(
                        {payload::PermissionDecisionKind::Deny, "timeout"});
                }
            }
        });
        if (!scheduled) {
            if (std::shared_ptr<Pending> claimed = claim(id)) {
                claimed->promise.set_value(
                    {payload::PermissionDecisionKind::Deny, "transport"});
            }
            return future;
        }
    }

    if (!transport.broadcast_permission_request(entry->wire)) {
        if (std::shared_ptr<Pending> claimed = claim(id)) {
            claimed->promise.set_value(
                {payload::PermissionDecisionKind::Deny, "transport"});
        }
    }
    return future;
}

PermissionBroker::PermissionBroker(PermissionPolicy& policy,
                                   PermissionTransport& transport,
                                   PermissionConfig config,
                                   ClockReader now)
    : state_(std::make_shared<State>(policy, transport, std::move(config), std::move(now))) {}

PermissionBroker::~PermissionBroker() { denyAll("shutdown"); }

void PermissionBroker::denyAll(std::string reason) {
    std::shared_ptr<State> state = state_;
    std::vector<std::shared_ptr<Pending>> remaining;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->shutting_down   = true;
        state->shutdown_reason = reason;
        remaining.reserve(state->pending.size());
        for (auto& request : state->pending) {
            request->resolved = true;
            remaining.push_back(request);
        }
        state->pending.clear();
    }
    for (auto& request : remaining) {
        request->promise.set_value({payload::PermissionDecisionKind::Deny, reason});
    }
}

std::future<PermissionOutcome> PermissionBroker::resolve(const PermissionRequest& request,
                                                         CancellationToken cancel) {
    std::shared_ptr<State> state = state_;
    try {
        return state->resolve(request, cancel, state);
    } catch (...) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "internal"});
    }
}

bool PermissionBroker::onDecision(const protocol::PermissionDecisionParams& params) {
    std::shared_ptr<State> state = state_;
    std::shared_ptr<Pending> entry = state->claim(params.request_id);
    if (!entry) {
        return false;
    }

    const payload::PermissionDecisionKind kind = decision_kind(params.decision, params.scope);
    entry->promise.set_value({kind, "user"});
    state->policy.remember(entry->core, kind, grant_scope(params.scope));
    return true;
}

std::vector<protocol::PermissionRequest> PermissionBroker::pending(SessionId session) const {
    std::shared_ptr<State> state = state_;
    std::vector<protocol::PermissionRequest> requests;
    std::lock_guard<std::mutex> lock(state->mutex);
    for (const auto& request : state->pending) {
        if (!request->resolved && request->core.session == session) {
            requests.push_back(request->wire);
        }
    }
    return requests;
}

std::size_t PermissionBroker::pendingCount() const noexcept {
    std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->pending.size();
}

void PermissionBroker::set_subscriber_count(SubscriberCount count) {
    std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->subscriber_count = std::move(count);
}

} // namespace ymh

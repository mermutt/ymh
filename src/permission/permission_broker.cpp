#include "ymh/permission/permission_broker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/execution/workspace_roots.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace ymh {
namespace {

struct Pending {
    PermissionRequest               core;
    protocol::PermissionRequest     wire;
    std::promise<PermissionOutcome> promise;
    Clock::time_point               deadline{};
    bool                            resolved = false;
    // 95-D3a: true once the request post has returned true. A resolution that
    // wins the publication race defers its notice here until phase 2 flushes
    // it AFTER the request post (request-before-notice, 95-I10).
    bool request_posted = false;
    std::optional<protocol::PermissionResolvedNotice> deferred_notice;
    // Multi-root: the candidate directory the dialog offered (fixture for the
    // `Directory` answer). Computed once at registration so the decision uses
    // exactly what the dialog showed.
    std::optional<std::filesystem::path> add_root;
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

std::string truncate_plan(const std::string& plan) {
    constexpr std::size_t kMaxPlanSummary = 4096;
    constexpr std::size_t kMaxPlanSummaryLines = 12;

    std::string result = plan;
    bool        capped = false;
    std::size_t lines  = 1;
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (result[index] != '\n') {
            continue;
        }
        ++lines;
        if (lines > kMaxPlanSummaryLines) {
            result.resize(index);
            capped = true;
            break;
        }
    }
    if (result.size() > kMaxPlanSummary) {
        result.resize(kMaxPlanSummary);
        capped = true;
    }
    if (capped) {
        result += "...";
    }
    return result;
}

std::string bounded_summary(const PermissionRequest& request, bool truncated) {
    constexpr std::size_t kMaxSummary = 256;

    if (request.tool == "exit_plan_mode") {
        if (!request.arguments.is_object()) {
            return "exit_plan_mode (no plan provided)";
        }
        const auto plan = request.arguments.find("plan");
        if (plan == request.arguments.end() || !plan->is_string() ||
            plan->get_ref<const std::string&>().empty()) {
            return "exit_plan_mode (no plan provided)";
        }
        return truncate_plan(plan->get_ref<const std::string&>());
    }

    std::string summary = request.tool;
    try {
        if (request.arguments.is_object()) {
            const auto command = request.arguments.find("command");
            if (command != request.arguments.end() && command->is_string()) {
                summary += " ";
                summary += command->get<std::string>();
            } else if (request.path.has_value()) {
                // 90-D16: never expose the absolute workspace path; the root
                // itself is reported by the tool name alone.
                const std::filesystem::path relative =
                    request.path->lexically_relative(request.root);
                if (!relative.empty() && relative != "." && !relative.is_absolute()) {
                    summary += " ";
                    summary += relative.generic_string();
                }
            }
        } else if (request.path.has_value()) {
            const std::filesystem::path relative =
                request.path->lexically_relative(request.root);
            if (!relative.empty() && relative != "." && !relative.is_absolute()) {
                summary += " ";
                summary += relative.generic_string();
            }
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
    // 114-D1: `0` means "no deadline" on the wire as well, so a client never
    // renders an already-expired question when the timeout is disabled.
    wire.expires_at_ms =
        config.permission_timeout.count() > 0
            ? wall_now_ms() + config.permission_timeout.count()
            : 0;
    wire.force_ask = request.force_ask;
    return wire;
}

payload::PermissionDecisionKind decision_kind(protocol::PermissionAnswer answer,
                                              protocol::PermissionScope scope) {
    if (answer == protocol::PermissionAnswer::Deny) {
        return payload::PermissionDecisionKind::Deny;
    }
    // Multi-root: adding a directory is a one-shot approval, never a durable
    // tool grant (the policy is not asked to remember it).
    if (scope == protocol::PermissionScope::Directory) {
        return payload::PermissionDecisionKind::Allow;
    }
    if (scope == protocol::PermissionScope::Always) {
        return payload::PermissionDecisionKind::AllowAlways;
    }
    return payload::PermissionDecisionKind::Allow;
}

GrantScope grant_scope(protocol::PermissionScope scope) {
    switch (scope) {
        case protocol::PermissionScope::Once:      return GrantScope::Once;
        case protocol::PermissionScope::Session:   return GrantScope::Session;
        case protocol::PermissionScope::Always:    return GrantScope::Always;
        case protocol::PermissionScope::Directory: return GrantScope::Once;
    }
    return GrantScope::Once;
}

std::vector<std::filesystem::path> request_roots(const PermissionRequest& request) {
    if (!request.roots.empty()) {
        return request.roots;
    }
    return {request.root};
}

// The directory a permission dialog should offer to add as a root, or nullopt
// when this request is not eligible (no outside target, forced review, or no
// safe candidate). Pure classification apart from the bounded stat-like probes
// inside `suggest_workspace_root`; never mutates anything.
std::optional<std::filesystem::path> suggest_add_root(const PermissionRequest& request) {
    if (request.force_ask) {
        return std::nullopt;
    }
    const std::vector<std::filesystem::path> roots = request_roots(request);
    if (roots.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path& primary = roots.front();
    if (request.path.has_value()) {
        return suggest_workspace_root(*request.path, primary, roots);
    }
    if (request.tool == "shell" || request.tool == "terminal") {
        const ShellClassification classification =
            classify_shell_request(request, {});
        std::vector<std::filesystem::path> targets = classification.outside_targets;
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (const std::filesystem::path& target : targets) {
            if (auto candidate = suggest_workspace_root(target, primary, roots)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

} // namespace

struct PermissionBroker::State {
    State(PermissionPolicy& policy_ref,
          PermissionTransport& transport_ref,
          PermissionConfig config_value,
          ClockReader now_reader,
          RootAddHook add_root_hook)
        : policy(policy_ref),
          transport(transport_ref),
          config(std::move(config_value)),
          now(std::move(now_reader)),
          add_root(std::move(add_root_hook)) {}

    std::size_t subscriber_count_for(const SessionId& session) const {
        SubscriberCount reader;
        {
            std::lock_guard<std::mutex> lock(mutex);
            reader = subscriber_count;
        }
        return reader ? reader(session) : 0;
    }

    // 95-D18: caller holds `mutex`; `claim` is the locking wrapper.
    std::shared_ptr<Pending> claim_locked(const std::string& id) {
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

    std::shared_ptr<Pending> claim(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        return claim_locked(id);
    }

    // 95-D3/95-D15: best-effort resolution notice. Reads `request_posted` under
    // the mutex and posts WITHOUT it (no virtual call under `mutex`, 95-M1);
    // a pre-post resolution defers to the phase-2 flush. Never throws.
    void notify_resolved(const std::shared_ptr<Pending>& entry,
                         protocol::PermissionAnswer answer,
                         protocol::PermissionScope scope,
                         std::string_view reason) noexcept {
        try {
            protocol::PermissionResolvedNotice notice;
            notice.request_id = entry->wire.request_id;
            notice.session = entry->core.session;
            notice.decision = answer;
            notice.scope = scope;
            notice.reason = std::string(reason);
            // 95-D12: at most 64 bytes: first 61 + "...".
            if (notice.reason.size() > 61) {
                notice.reason.resize(61);
                notice.reason += "...";
            }
            bool posted = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                posted = entry->request_posted;
                if (!posted) {
                    entry->deferred_notice = notice;
                }
            }
            if (posted) {
                (void)transport.broadcast_permission_resolved(std::move(notice));
            }
        } catch (...) {
        }
    }

    std::future<PermissionOutcome> resolve(const PermissionRequest& request,
                                           CancellationToken cancel,
                                           std::weak_ptr<State> weak);

    PermissionPolicy&  policy;
    PermissionTransport& transport;
    PermissionConfig   config;
    ClockReader        now;
    RootAddHook        add_root;

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
    std::string         reason;
    const PolicyVerdict verdict = policy.evaluate(request, &reason);
    const std::string   fallback_reason = reason.empty() ? std::string{"rule"} : reason;
    if (verdict == PolicyVerdict::Allow) {
        return ready_outcome({payload::PermissionDecisionKind::Allow, fallback_reason});
    }
    if (verdict == PolicyVerdict::Deny) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, fallback_reason});
    }
    // 119-D2: `never` rejects the request deterministically, before any
    // answerer or subscriber check.
    if (request.approval == ApprovalPolicy::Never) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "approval:rejected"});
    }
    if (cancel.cancelled()) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "cancelled"});
    }
    if (subscriber_count_for(request.session) == 0) {
        return ready_outcome({payload::PermissionDecisionKind::Deny, "no-subscribers"});
    }

    auto entry = std::make_shared<Pending>();
    // Multi-root: compute the candidate once, before any lock, and use exactly
    // this value for both the dialog and the decision.
    const std::optional<std::filesystem::path> add_root = suggest_add_root(request);
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (shutting_down) {
            return ready_outcome({payload::PermissionDecisionKind::Deny, shutdown_reason});
        }
        id = "perm-" + std::to_string(next_id++);
        entry->core = request;
        entry->wire = make_wire(request, id, config);
        if (add_root.has_value()) {
            entry->wire.add_root = add_root->string();
        }
        entry->add_root = add_root;
        entry->deadline = now() + config.permission_timeout;
        pending.push_back(entry);
    }

    std::future<PermissionOutcome> future = entry->promise.get_future();
    try {
        cancel.on_cancel([weak, id] {
            if (std::shared_ptr<State> locked = weak.lock()) {
                if (std::shared_ptr<Pending> claimed = locked->claim(id)) {
                    claimed->promise.set_value(
                        {payload::PermissionDecisionKind::Deny, "cancelled"});
                    locked->notify_resolved(claimed, protocol::PermissionAnswer::Deny,
                                            protocol::PermissionScope::Once, "cancelled");
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
                        locked->notify_resolved(claimed, protocol::PermissionAnswer::Deny,
                                                protocol::PermissionScope::Once, "timeout");
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

        // 95-D3a phase 2: never post the request under the mutex. A resolution
        // that won the race above deferred its notice; flush it only AFTER the
        // request post so a client can never see the notice first (95-I10).
        bool already_resolved = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            already_resolved = entry->resolved;
        }
        if (already_resolved) {
            return future;
        }

        const bool posted = transport.broadcast_permission_request(entry->wire);
        std::optional<protocol::PermissionResolvedNotice> flush;
        {
            std::lock_guard<std::mutex> lock(mutex);
            entry->request_posted = posted;
            if (posted && entry->deferred_notice.has_value()) {
                flush = std::move(entry->deferred_notice);
            }
            entry->deferred_notice.reset();
        }
        if (flush.has_value()) {
            try {
                (void)transport.broadcast_permission_resolved(std::move(*flush));
            } catch (...) {
                // 95-D15: a notice failure never changes the resolution.
            }
        }
        if (!posted) {
            if (std::shared_ptr<Pending> claimed = claim(id)) {
                claimed->promise.set_value(
                    {payload::PermissionDecisionKind::Deny, "transport"});
            }
        }
        return future;
    } catch (...) {
        // 95-D18: contain every post-publication failure. A pre-post throw was
        // never delivered, so this path never emits a notice (95-I3).
        std::shared_ptr<Pending> claimed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            claimed = claim_locked(id);
        }
        if (claimed) {
            claimed->promise.set_value({payload::PermissionDecisionKind::Deny, "internal"});
        }
        return future;
    }
}

PermissionBroker::PermissionBroker(PermissionPolicy& policy,
                                   PermissionTransport& transport,
                                   PermissionConfig config,
                                   ClockReader now,
                                   RootAddHook add_root)
    : state_(std::make_shared<State>(policy, transport, std::move(config), std::move(now),
                                     std::move(add_root))) {}

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

    // Multi-root: "Also allow directory <candidate>" adds the stored candidate
    // as an additional workspace root. The decision was already taken; no
    // tool/command grant is remembered for this scope.
    if (params.decision == protocol::PermissionAnswer::Allow &&
        params.scope == protocol::PermissionScope::Directory) {
        PermissionOutcome outcome{payload::PermissionDecisionKind::Deny,
                                  "user:add-root-unavailable"};
        if (entry->add_root.has_value() && state->add_root) {
            try {
                if (state->add_root(*entry->add_root).has_value()) {
                    outcome = {payload::PermissionDecisionKind::Allow, "user:added-root"};
                } else {
                    outcome = {payload::PermissionDecisionKind::Deny, "user:add-root-failed"};
                }
            } catch (...) {
                outcome = {payload::PermissionDecisionKind::Deny, "user:add-root-failed"};
            }
        }
        entry->promise.set_value(outcome);
        // 95-D3: emit AFTER `set_value`. A failed add is a Deny for every
        // attached UI, so the notice must not echo the Allow/Directory answer.
        if (outcome.decision == payload::PermissionDecisionKind::Deny) {
            state->notify_resolved(entry, protocol::PermissionAnswer::Deny,
                                   protocol::PermissionScope::Once, outcome.reason);
        } else {
            state->notify_resolved(entry, params.decision, params.scope, outcome.reason);
        }
        return true;
    }

    const payload::PermissionDecisionKind kind = decision_kind(params.decision, params.scope);
    entry->promise.set_value({kind, "user"});
    // 95-D3: emit AFTER `set_value`; verbatim wire answer and scope (95-D3a:
    // cancel/timeout may have already deferred their notice).
    state->notify_resolved(entry, params.decision, params.scope, "user");
    if (!entry->core.force_ask) {
        state->policy.remember(entry->core, kind, grant_scope(params.scope));
    }
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

std::optional<SessionId> PermissionBroker::session_of(const std::string& request_id) const {
    std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    for (const auto& request : state->pending) {
        if (!request->resolved && request->wire.request_id == request_id) {
            return request->core.session;
        }
    }
    return std::nullopt;
}

void PermissionBroker::set_subscriber_count(SubscriberCount count) {
    std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->subscriber_count = std::move(count);
}

} // namespace ymh

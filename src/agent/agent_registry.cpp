#include "ymh/agent/agent_registry.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

#include "ymh/agent/preset.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {
namespace {

AgentServices make_services(SessionManager& sessions,
                            ResourceGovernor& governor,
                            ToolRegistry& tools,
                            PermissionPolicy& policy,
                            LlmRuntime& runtime,
                            ContextAssembler& context) {
    AgentServices services;
    services.sessions = &sessions;
    services.governor = &governor;
    services.tools    = &tools;
    services.policy   = &policy;
    services.runtime  = &runtime;
    services.context  = &context;
    return services;
}

} // namespace

AgentRegistry::AgentRegistry(AgentServices services, AgentConfig config)
    : services_(std::move(services)), config_(std::move(config)) {
    if (services_.pool != nullptr) {
        pool_ = services_.pool;
    } else {
        std::size_t concurrency = 4;
        if (services_.governor != nullptr) {
            concurrency = services_.governor->caps().max_llm_concurrency;
        }
        poolStorage_ = std::make_unique<LLMPool>(concurrency);
        pool_        = poolStorage_.get();
    }
}

AgentRegistry::AgentRegistry(SessionManager& sessions,
                             ResourceGovernor& governor,
                             ToolRegistry& tools,
                             PermissionPolicy& policy,
                             LlmRuntime& runtime,
                             ContextAssembler& context,
                             AgentConfig config)
    : AgentRegistry(make_services(sessions, governor, tools, policy, runtime, context),
                    std::move(config)) {}

std::expected<AgentId, AgentError> AgentRegistry::create(const SessionOptions& options) {
    if (services_.sessions == nullptr) {
        return std::unexpected(AgentError{AgentErrorCode::Internal, "no session manager"});
    }

    SessionId sessionId;
    try {
        sessionId = services_.sessions->createSession(options);
    } catch (const std::invalid_argument& error) {
        return std::unexpected(AgentError{AgentErrorCode::UnknownSession, error.what()});
    } catch (const LeaseLost& error) {
        return std::unexpected(AgentError{AgentErrorCode::LeaseHeldByOther, error.what()});
    } catch (const std::exception& error) {
        return std::unexpected(AgentError{AgentErrorCode::StoreUnavailable, error.what()});
    }
    return registerAgent(sessionId);
}

bool AgentRegistry::reserveSubagentSlot() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t cap =
        services_.governor != nullptr ? services_.governor->caps().max_live_subagents : 8;
    if (live_subagent_count_ >= cap) {
        return false;
    }
    ++live_subagent_count_;
    return true;
}

void AgentRegistry::releaseSubagentSlot() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (live_subagent_count_ > 0) {
        --live_subagent_count_;
    }
}

std::size_t AgentRegistry::liveSubagentCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_subagent_count_;
}

std::expected<AgentId, AgentError> AgentRegistry::createChild(const ChildSpawnRequest& request) {
    if (services_.sessions == nullptr) {
        return std::unexpected(AgentError{AgentErrorCode::Internal, "no session manager"});
    }
    if (const std::optional<AgentError> depth =
            check_delegation_depth(request.parent_depth, request.max_depth);
        depth.has_value()) {
        return std::unexpected(*depth);
    }
    if (request.route.has_value() && services_.route_catalog != nullptr) {
        const ChildRoute& route = *request.route;
        if (!services_.route_catalog->is_routable_endpoint(route.endpoint)) {
            return std::unexpected(
                AgentError{AgentErrorCode::ProviderFailed, "unknown endpoint: " + route.endpoint});
        }
        if (!services_.route_catalog->is_catalog_member(route.endpoint, route.model)) {
            return std::unexpected(AgentError{AgentErrorCode::ProviderFailed,
                                              "model is not a catalog member: " + route.model});
        }
        if (route.reasoning_effort.has_value()) {
            const std::vector<std::string> efforts =
                services_.route_catalog->efforts_for(route.endpoint, route.model);
            if (!efforts.empty() && std::find(efforts.begin(), efforts.end(),
                                              *route.reasoning_effort) == efforts.end()) {
                return std::unexpected(
                    AgentError{AgentErrorCode::ProviderFailed, "unsupported reasoning_effort"});
            }
        }
    }
    if (!reserveSubagentSlot()) {
        return std::unexpected(AgentError{AgentErrorCode::InboxFull, "fan-out cap exceeded"});
    }

    SessionOptions options = request.options;
    options.kind           = SessionKind::Subagent;
    options.depth          = request.parent_depth + 1;
    if (request.route.has_value()) {
        options.endpoint         = request.route->endpoint;
        options.profile_id       = request.route->profile_id;
        options.model            = request.route->model;
        options.model_name       = request.route->model_name;
        options.reasoning_effort = request.route->reasoning_effort;
        options.max_tokens       = request.route->max_tokens;
    }

    SessionId session_id;
    try {
        session_id = services_.sessions->createSession(options);
    } catch (const std::invalid_argument& error) {
        releaseSubagentSlot();
        return std::unexpected(AgentError{AgentErrorCode::UnknownSession, error.what()});
    } catch (const LeaseLost& error) {
        releaseSubagentSlot();
        return std::unexpected(AgentError{AgentErrorCode::LeaseHeldByOther, error.what()});
    } catch (const std::exception& error) {
        releaseSubagentSlot();
        return std::unexpected(AgentError{AgentErrorCode::StoreUnavailable, error.what()});
    }

    const std::expected<AgentId, AgentError> registered = registerAgent(session_id);
    if (!registered.has_value()) {
        releaseSubagentSlot();
        try {
            services_.sessions->closeSession(session_id);
        } catch (const std::exception&) {
        }
        return registered;
    }

    if (services_.presets != nullptr && request.composition.has_value()) {
        try {
            std::shared_ptr<AgentLoop> parent;
            if (options.parentSession.has_value()) {
                parent = findShared(*options.parentSession);
            }
            if (parent != nullptr) {
                AgentContext child_context{*registered, {}};
                services_.presets->apply_child_composition(child_context, *parent,
                                                           *request.composition);
            }
        } catch (const std::exception& error) {
            dispose(*registered);
            releaseSubagentSlot();
            try {
                services_.sessions->closeSession(session_id);
            } catch (const std::exception&) {
            }
            return std::unexpected(AgentError{AgentErrorCode::Internal, error.what()});
        }
    }
    return registered;
}

std::expected<AgentId, AgentError> AgentRegistry::resume(const SessionId& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = bySession_.find(id.value); it != bySession_.end()) {
            return it->second;
        }
    }
    if (services_.sessions == nullptr) {
        return std::unexpected(AgentError{AgentErrorCode::Internal, "no session manager"});
    }

    try {
        services_.sessions->resumeSession(id);
    } catch (const UnknownSession& error) {
        return std::unexpected(AgentError{AgentErrorCode::UnknownSession, error.what()});
    } catch (const LeaseLost& error) {
        return std::unexpected(AgentError{AgentErrorCode::LeaseHeldByOther, error.what()});
    } catch (const std::exception& error) {
        return std::unexpected(AgentError{AgentErrorCode::StoreUnavailable, error.what()});
    }
    return registerAgent(id);
}

std::expected<AgentId, AgentError> AgentRegistry::registerAgent(const SessionId& sessionId) {
    AgentServices services = services_;
    services.pool          = pool_;

    const AgentId agentId{make_event_id().value};
    try {
        // Resolve the owning session handle before taking mutex_ (lock order:
        // SessionManager::mutex_ then AgentRegistry::mutex_, never held upward).
        std::shared_ptr<Session> session = services_.sessions->sessionPtr(sessionId);
        if (services_.presets != nullptr) {
            AgentContext ctx{agentId, {}};
            try {
                services_.presets->mount(ctx, session->header().agent_preset);
            } catch (const std::exception&) {
                // An absent/unavailable preset leaves the agent on the root layer.
            }
        }
        std::shared_ptr<AgentLoop> agent =
            std::make_shared<AgentLoop>(agentId, std::move(session), std::move(services), config_);
        std::lock_guard<std::mutex> lock(mutex_);
        bySession_[sessionId.value] = agentId;
        agents_[agentId.value]      = std::move(agent);
    } catch (const std::exception& error) {
        return std::unexpected(AgentError{AgentErrorCode::Internal, error.what()});
    }
    return agentId;
}

std::shared_ptr<AgentLoop> AgentRegistry::findShared(SessionId id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = bySession_.find(id.value);
    if (session == bySession_.end()) {
        return nullptr;
    }
    const auto agent = agents_.find(session->second.value);
    if (agent == agents_.end()) {
        return nullptr;
    }
    return agent->second;
}

std::shared_ptr<AgentLoop> AgentRegistry::getShared(AgentId id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = agents_.find(id.value);
    if (it == agents_.end()) {
        return nullptr;
    }
    return it->second;
}

void AgentRegistry::dispose(AgentId id) {
    // Snapshot the strong ref under mutex_, release, then call dispose()/
    // closeSession() outside it, then re-lock to erase (24-D18/R5).
    std::shared_ptr<AgentLoop> agent;
    SessionId                  sessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = agents_.find(id.value);
        if (it == agents_.end()) {
            return;
        }
        agent     = it->second;
        sessionId = agent->session();
    }

    agent->dispose();
    if (services_.sessions != nullptr) {
        services_.sessions->closeSession(sessionId);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bySession_.erase(sessionId.value);
    agents_.erase(id.value);
    leases_.erase(sessionId.value);
}

void AgentRegistry::finalizeAll() {
    // Snapshot the owning handles under mutex_, then release it before any
    // dispose()/closeSession() call (24-D18). Erasing while iterating the map
    // directly would invalidate the iterator, so the snapshot is the contract.
    std::vector<std::shared_ptr<AgentLoop>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(agents_.size());
        for (const auto& entry : agents_) {
            snapshot.push_back(entry.second);
        }
    }

    for (const std::shared_ptr<AgentLoop>& agent : snapshot) {
        // dispose() cancels an in-flight turn (its cancel path flushes the
        // pending chunk batch and appends exactly one terminal event) and parks
        // the agent without waiting (AL25). The local shared_ptr keeps the agent
        // — and, via its session_owner_, the Session — alive across the call.
        agent->dispose();
        if (services_.sessions != nullptr) {
            services_.sessions->closeSession(agent->session());
        }
    }

    // Re-lock to erase. The erased shared_ptrs are destroyed after mutex_ is
    // released because the snapshot still holds them.
    std::lock_guard<std::mutex> lock(mutex_);
    agents_.clear();
    bySession_.clear();
    leases_.clear();
}

std::vector<AgentId> AgentRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AgentId> ids;
    ids.reserve(agents_.size());
    for (const auto& entry : agents_) {
        ids.push_back(entry.second->id());
    }
    return ids;
}

std::size_t AgentRegistry::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : agents_) {
        if (entry.second->status() == AgentStatus::Running) {
            ++count;
        }
    }
    return count;
}

void AgentRegistry::activateSession(const SessionId& id) {
    // R1: copy the strong ref under mutex_, release, call activate() outside.
    std::shared_ptr<AgentLoop> agent = findShared(id);
    if (agent == nullptr || !activationAllowed(agent->state())) {
        return;
    }
    agent->activate();
}

bool AgentRegistry::hasPendingWork(const SessionId& id) const noexcept {
    // R4: copy the strong ref under mutex_, release, call the predicate outside.
    std::shared_ptr<AgentLoop> agent;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session = bySession_.find(id.value);
        if (session == bySession_.end()) {
            return false;
        }
        const auto it = agents_.find(session->second.value);
        if (it == agents_.end()) {
            return false;
        }
        agent = it->second;
    }
    return agent->hasPendingWork();
}

std::expected<CompactionOutcome, AgentError> AgentRegistry::requestCompaction(const SessionId& id) {
    // R3: hold the strong ref for the whole (potentially long) call.
    std::shared_ptr<AgentLoop> agent = findShared(id);
    if (agent == nullptr) {
        return std::unexpected(AgentError{AgentErrorCode::UnknownSession, "unknown session"});
    }
    return agent->requestCompaction();
}

void AgentRegistry::suspendSession(const SessionId& id) {
    // R2: this runs directly on the io thread; hold the strong ref across it.
    std::shared_ptr<AgentLoop> agent = findShared(id);
    if (agent != nullptr) {
        agent->suspend();
    }
}

void AgentRegistry::set_permission_resolver(AgentServices::PermissionResolver resolver) {
    services_.permission_resolver = std::move(resolver);
}

} // namespace ymh

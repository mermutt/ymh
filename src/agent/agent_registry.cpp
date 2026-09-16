#include "ymh/agent/agent_registry.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

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
                            ProviderRegistry& providers,
                            ContextAssembler& context) {
    AgentServices services;
    services.sessions  = &sessions;
    services.governor  = &governor;
    services.tools     = &tools;
    services.policy    = &policy;
    services.providers = &providers;
    services.context   = &context;
    return services;
}

} // namespace

AgentRegistry::AgentRegistry(AgentServices services, AgentConfig config)
    : services_(std::move(services)), config_(std::move(config)) {
    if (services_.provider == nullptr && services_.providers != nullptr) {
        std::expected<std::unique_ptr<LLMProvider>, LLMError> created =
            services_.providers->create(services_.provider_config);
        if (created.has_value()) {
            providerStorage_ = std::move(*created);
        }
    }
    provider_ = providerStorage_ ? providerStorage_.get() : services_.provider;

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

    if (services_.sessions != nullptr) {
        services_.sessions->setAgentLookup([this](const SessionId& id) { return find(id); });
    }
}

AgentRegistry::AgentRegistry(SessionManager& sessions,
                             ResourceGovernor& governor,
                             ToolRegistry& tools,
                             PermissionPolicy& policy,
                             ProviderRegistry& providers,
                             ContextAssembler& context,
                             AgentConfig config)
    : AgentRegistry(make_services(sessions, governor, tools, policy, providers, context),
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

std::expected<AgentId, AgentError> AgentRegistry::resume(const SessionId& id) {
    if (const auto it = bySession_.find(id.value); it != bySession_.end()) {
        return it->second;
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
    services.provider      = provider_;
    services.pool          = pool_;

    const AgentId agentId{make_event_id().value};
    try {
        Session& session = services_.sessions->session(sessionId);
        auto agent = std::make_unique<AgentLoop>(agentId, session, std::move(services), config_);
        bySession_[sessionId.value] = agentId;
        agents_[agentId.value]      = std::move(agent);
    } catch (const std::exception& error) {
        return std::unexpected(AgentError{AgentErrorCode::Internal, error.what()});
    }
    return agentId;
}

Agent& AgentRegistry::get(AgentId id) {
    const auto it = agents_.find(id.value);
    if (it == agents_.end()) {
        throw std::out_of_range("unknown agent: " + id.value);
    }
    return *it->second;
}

Agent* AgentRegistry::find(SessionId id) noexcept {
    const auto session = bySession_.find(id.value);
    if (session == bySession_.end()) {
        return nullptr;
    }
    const auto agent = agents_.find(session->second.value);
    if (agent == agents_.end()) {
        return nullptr;
    }
    return agent->second.get();
}

void AgentRegistry::dispose(AgentId id) {
    const auto it = agents_.find(id.value);
    if (it == agents_.end()) {
        return;
    }
    const SessionId sessionId = it->second->session();
    it->second->dispose();
    if (services_.sessions != nullptr) {
        services_.sessions->closeSession(sessionId);
    }
    bySession_.erase(sessionId.value);
    agents_.erase(it);
    leases_.erase(sessionId.value);
}

std::vector<AgentId> AgentRegistry::list() const {
    std::vector<AgentId> ids;
    ids.reserve(agents_.size());
    for (const auto& entry : agents_) {
        ids.push_back(entry.second->id());
    }
    return ids;
}

std::size_t AgentRegistry::activeCount() const {
    std::size_t count = 0;
    for (const auto& entry : agents_) {
        if (entry.second->status() == AgentStatus::Running) {
            ++count;
        }
    }
    return count;
}

void AgentRegistry::activateSession(const SessionId& id) {
    const auto session = bySession_.find(id.value);
    if (session == bySession_.end()) {
        return;
    }
    const auto agent = agents_.find(session->second.value);
    if (agent == agents_.end()) {
        return;
    }
    if (!activationAllowed(agent->second->state())) {
        return;
    }
    agent->second->activate();
}

bool AgentRegistry::hasPendingWork(const SessionId& id) const noexcept {
    const auto session = bySession_.find(id.value);
    if (session == bySession_.end()) {
        return false;
    }
    const auto agent = agents_.find(session->second.value);
    if (agent == agents_.end()) {
        return false;
    }
    return agent->second->hasPendingWork();
}

std::expected<CompactionOutcome, AgentError> AgentRegistry::requestCompaction(const SessionId& id) {
    const auto session = bySession_.find(id.value);
    if (session == bySession_.end()) {
        return std::unexpected(AgentError{AgentErrorCode::UnknownSession, "unknown session"});
    }
    const auto agent = agents_.find(session->second.value);
    if (agent == agents_.end()) {
        return std::unexpected(AgentError{AgentErrorCode::UnknownSession, "unknown session"});
    }
    return agent->second->requestCompaction();
}

void AgentRegistry::suspendSession(const SessionId& id) {
    const auto session = bySession_.find(id.value);
    if (session == bySession_.end()) {
        return;
    }
    const auto agent = agents_.find(session->second.value);
    if (agent != agents_.end()) {
        agent->second->suspend();
    }
}

void AgentRegistry::set_permission_resolver(AgentServices::PermissionResolver resolver) {
    services_.permission_resolver = std::move(resolver);
}

} // namespace ymh

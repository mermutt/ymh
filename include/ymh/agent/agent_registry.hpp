#pragma once

// AgentRegistry: the daemon-level owner of agent lifetime, pinned by
// 06-agent-loop.md §4. One instance per WorkspaceHost. It owns the
// create/resume transaction (session + lease -> loop construction ->
// registration), enforces one agent per session (A1), and is the single
// target of `SessionManager::agent()` (decision (l)).

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_loop.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/session/session_handle.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {

class AgentRegistry {
public:
    AgentRegistry(AgentServices services, AgentConfig config);
    AgentRegistry(SessionManager& sessions,
                  ResourceGovernor& governor,
                  ToolRegistry& tools,
                  PermissionPolicy& policy,
                  ProviderRegistry& providers,
                  ContextAssembler& context,
                  AgentConfig config);

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    std::expected<AgentId, AgentError> create(const SessionOptions& options);
    std::expected<AgentId, AgentError> resume(const SessionId& id);

    Agent&                get(AgentId id);
    Agent*                find(SessionId id) noexcept;
    void                  dispose(AgentId id);
    std::vector<AgentId>  list() const;
    std::size_t           activeCount() const;

    // Daemon-driven activation (04 (m), A7/A8). Never focus-gated: attachment
    // and UI focus do not appear here. `activateSession` is idempotent;
    // `suspendSession` cancels the in-flight turn with reason "superseded".
    void activateSession(const SessionId& id);
    void suspendSession(const SessionId& id);

private:
    std::expected<AgentId, AgentError> registerAgent(const SessionId& sessionId);

    std::unique_ptr<LLMProvider> providerStorage_;
    std::unique_ptr<LLMPool>     poolStorage_;
    LLMProvider*                 provider_ = nullptr;
    LLMPool*                     pool_ = nullptr;
    AgentServices                services_;
    AgentConfig                  config_;
    std::unordered_map<std::string, AgentId>                      bySession_;
    std::unordered_map<std::string, std::unique_ptr<AgentLoop>>   agents_;
    std::unordered_map<std::string, std::unique_ptr<SessionHandle>> leases_;
};

} // namespace ymh

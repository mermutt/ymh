#pragma once

// AgentRegistry: the daemon-level owner of agent lifetime, pinned by
// 06-agent-loop.md §4. One instance per WorkspaceHost. It owns the
// create/resume transaction (session + lease -> loop construction ->
// registration), enforces one agent per session (A1), and is the single
// target of `SessionManager::agent()` (decision (l)).

#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_loop.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/session/session_handle.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {

class LlmRuntime;

class AgentRegistry {
public:
    AgentRegistry(AgentServices services, AgentConfig config);
    AgentRegistry(SessionManager& sessions,
                  ResourceGovernor& governor,
                  ToolRegistry& tools,
                  PermissionPolicy& policy,
                  LlmRuntime& runtime,
                  ContextAssembler& context,
                  AgentConfig config);

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    std::expected<AgentId, AgentError> create(const SessionOptions& options);
    std::expected<AgentId, AgentError> resume(const SessionId& id);

    // 24-D14/24-D17: the only handle-producing accessors. Each locks mutex_
    // only long enough to copy the shared_ptr; the returned strong reference
    // keeps the agent alive after mutex_ is released and after dispose erases
    // the map entry (AL2/AL30/AL31/AL35). There is no raw Agent*/Agent& accessor.
    [[nodiscard]] std::shared_ptr<AgentLoop> findShared(SessionId id) noexcept;
    [[nodiscard]] std::shared_ptr<AgentLoop> getShared(AgentId id) noexcept;

    void                  dispose(AgentId id);

    // 24-D10/24-D15/AL25/AL33: last-supervisor teardown. Cancels each in-flight
    // turn (its cancel path flushes the chunk batch and appends exactly one
    // terminal event) then parks each agent, calls
    // `SessionManager::closeSession(id)` (the explicit last-exit close), and
    // erases the maps. Never waits; the coordinator's drain joins afterwards.
    // Snapshot-release-erase contract (24-D18): snapshot the `shared_ptr`s under
    // `mutex_`, RELEASE, call `dispose()`/`closeSession()` on each snapshot
    // entry, then re-lock to erase; `mutex_` is never held across those calls
    // and no iterator is held across them. A concurrent caller that took
    // `findShared` keeps its agent (and, via the agent's `session_owner_`, its
    // `Session`) alive until it returns (AL31).
    void finalizeAll();

    std::vector<AgentId>  list() const;
    std::size_t           activeCount() const;

    // Daemon-driven activation (04 (m), A7/A8). Never focus-gated: attachment
    // and UI focus do not appear here. `activateSession` is idempotent and is
    // gated on Idle/blocked state (11 §12.4): it never cancels or re-enters an
    // in-flight turn. `suspendSession` cancels the in-flight turn with reason
    // "superseded".
    void activateSession(const SessionId& id);
    void suspendSession(const SessionId& id);

    // 11 §12.4 / 04 §3.7 arbiter predicate: a queued turn trigger or an
    // in-flight turn. False for an unknown session.
    [[nodiscard]] bool hasPendingWork(const SessionId& id) const noexcept;

    // Additive enqueue target for the manual `/compact` path
    // (13-context-compaction.md §6.8, errata A5). Looks up the registered
    // `AgentLoop` and delegates to its public `requestCompaction()`.
    std::expected<CompactionOutcome, AgentError> requestCompaction(const SessionId& id);

    // 11-m2-errata §7 (D18/D19): the daemon installs the async
    // `PermissionBroker` resolver once the runtime is built. Must be called
    // before any agent is created; the resolver runs on the turn thread and
    // blocks on the broker's future, never on the transport thread.
    void set_permission_resolver(AgentServices::PermissionResolver resolver);

private:
    std::expected<AgentId, AgentError> registerAgent(const SessionId& sessionId);

    std::unique_ptr<LLMPool>     poolStorage_;
    LLMPool*                     pool_ = nullptr;
    AgentServices                services_;
    AgentConfig                  config_;
    // mutex_ is a map-only lock (24-D18): held exactly while agents_/
    // bySession_/leases_ are read or mutated, never across a call into
    // AgentLoop/Session/SessionManager (AL27/AL36).
    mutable std::mutex                                            mutex_;
    std::unordered_map<std::string, AgentId>                      bySession_;
    std::unordered_map<std::string, std::shared_ptr<AgentLoop>>   agents_;
    std::unordered_map<std::string, std::unique_ptr<SessionHandle>> leases_;
};

} // namespace ymh

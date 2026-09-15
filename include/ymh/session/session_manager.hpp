#pragma once

// SessionManager: one instance per WorkspaceHost daemon (01 §8, 00 §9.6). It
// owns the sessions of its daemon over the abstract `SessionStore` seam; the
// focused/active session is supervisor-local state, not manager state.
//
// `env()` from the §9.6 sketch is deferred to the execution-environment wave.
// `agent()` is a thin delegate to the daemon's `AgentRegistry` (06 §4.1), which
// remains the single owner of agent lifetime; this layer holds no agent state.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

class Agent;

struct SessionOptions {
    std::filesystem::path cwd;
    std::string           serverProfile;
    std::string           model;
    std::string           title;
};

class SessionManager {
public:
    SessionManager(SessionStore& store, EventBus& bus);

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    SessionId createSession(const SessionOptions& options);
    SessionId resumeSession(const SessionId& id);
    SessionId forkSession(const SessionId& parent, std::size_t seedLength);
    SessionId replaySession(const SessionId& id);

    void closeSession(const SessionId& id);
    void deleteSession(const SessionId& id);

    [[nodiscard]] std::vector<SessionId> list() const;

    Session& session(const SessionId& id);

    // Installed by AgentRegistry; `agent()` then forwards to it (06 §4.1).
    using AgentLookup = std::function<Agent*(const SessionId&)>;
    void                 setAgentLookup(AgentLookup lookup);
    [[nodiscard]] Agent* findAgent(const SessionId& id) const;
    Agent&               agent(const SessionId& id) const;

private:
    Session& loadInto(const SessionHeader& header);

    SessionStore* store_;
    EventBus*     bus_;
    AgentLookup   agent_lookup_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
};

} // namespace ymh

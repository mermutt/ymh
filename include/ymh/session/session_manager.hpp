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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

class Agent;

struct SessionOptions {
    std::filesystem::path    cwd;
    std::string              serverProfile;
    std::string              model;
    std::string              title;
    SessionKind              kind          = SessionKind::Root;
    std::optional<SessionId> parentSession = std::nullopt;
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

    // 19 §5.4: append a User rename. Loads the session if not resident (like
    // forkSession). Validates `title` via normalize_title (RN8); throws
    // std::invalid_argument on a bad title and UnknownSession when absent.
    // LeaseLost / StoreError propagate from the store (RN13).
    Sequence renameSession(const SessionId& id, std::string title);

    // 19 §4.3: daemon-only. Appends SessionRenamed{origin=Auto} iff RN5/RN6
    // hold and derive_auto_title yields a value; otherwise a no-op. Never
    // throws for a suppressed name. May still throw LeaseLost/StoreError from
    // the store append; the sole call site (HostRuntime::agentPrompt) swallows
    // those so advisory auto-naming cannot fail the prompt (19 §4.3). Returns
    // the assigned Sequence when an event was appended.
    std::optional<Sequence> maybeAutoName(const SessionId& id, std::string_view firstUserText);

    void closeSession(const SessionId& id);
    void deleteSession(const SessionId& id, bool only_if_empty = false);

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

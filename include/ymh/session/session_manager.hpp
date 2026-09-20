#pragma once

// SessionManager: one instance per WorkspaceHost daemon (01 §8, 00 §9.6). It
// owns the sessions of its daemon over the abstract `SessionStore` seam; the
// focused/active session is supervisor-local state, not manager state.
//
// 24-D1: `sessions_` owns each `Session` by `std::shared_ptr`, and the only
// public handle-producing accessor is `sessionPtr` (owning). `session()` is
// private and may be used only while `mutex_` is held or while the caller holds
// a strong `sessionPtr` for the returned reference's whole use (24-D17). There
// is no agent delegate: `AgentRegistry` is the single owner of agent lifetime
// (06 §4.1) and `AgentLoop` anchors its own session via a strong `shared_ptr`.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

struct SessionOptions {
    std::filesystem::path    cwd;
    std::string              serverProfile;
    std::string              model;
    std::string              title;
    SessionKind              kind          = SessionKind::Root;
    std::optional<SessionId> parentSession = std::nullopt;
    // 43 §2 (42-D8/42-D5): the creation-path copy of the delegation depth and
    // the start preset.
    std::uint32_t              depth = 0;
    std::optional<std::string> agent_preset;
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

    // 24-D1: strong-reference accessor for the agent lifetime anchor. The
    // returned `shared_ptr` keeps the `Session` alive after a concurrent
    // `closeSession`/`deleteSession` erases the manager's reference. Throws
    // UnknownSession when the id is not resident (24-D14/AL1).
    [[nodiscard]] std::shared_ptr<Session> sessionPtr(const SessionId& id);

    void closeSession(const SessionId& id);              // park: erase the manager ref
    void deleteSession(const SessionId& id, bool only_if_empty = false);

    [[nodiscard]] std::vector<SessionId> list() const;

private:
    // Requires `mutex_` held. 24-D13: returns the resident Session unchanged
    // when present; never replaces a live object.
    Session& loadInto(const SessionHeader& header);

    // Private: callers must hold `mutex_` (this is the raw map lookup) or a
    // strong `sessionPtr` for the returned reference's whole use. External
    // callers use `sessionPtr` (24-D17).
    Session& session(const SessionId& id);

    SessionStore*                                             store_ = nullptr;
    EventBus*                                                 bus_   = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;   // 24-D1
    mutable std::mutex                                        mutex_;      // 24-D11
};

} // namespace ymh

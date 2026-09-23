#pragma once

// Daemon-side seam for the transport (docs/design/05-transport.md §1.4, §7).
//
// The transport owns framing, handshake, profiles, dispatch, streaming, and
// cursors; everything semantic (session lifecycle, agent input, the event log,
// permissions, registry reads) is delegated to a `TransportHost` implementation
// owned by the daemon (spec 04) or a fake in tests. Handlers either return a
// value or throw `RpcException` carrying an RpcCode|AppCode.

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::protocol {

class RpcException final : public std::runtime_error {
public:
    RpcException(int rpc_code, std::string message, nlohmann::json error_data = {})
        : std::runtime_error(std::move(message)),
          code_(rpc_code),
          data_(std::move(error_data)) {}

    [[nodiscard]] int code() const noexcept { return code_; }
    [[nodiscard]] const nlohmann::json& data() const noexcept { return data_; }

private:
    int            code_;
    nlohmann::json data_;
};

struct HostStatusInfo {
    HostState                state{HostState::Starting};
    WorkspaceId              workspace;
    HostBootId               boot_id;
    HostPid                  pid{0};
    std::optional<SessionId> active_session;
};

struct SessionCreated {
    SessionId      session;
    nlohmann::json header;
};

struct SessionResumed {
    SessionId   session;
    std::string status;   // Idle | Running (01 §9.8)
};

// 19 §5.4: the `session.rename` result. Named `...Result` to avoid colliding
// with `payload::SessionRenamed` (the durable event payload).
struct SessionRenamedResult {
    SessionId   session;
    std::string title;
};

// 25-D2/D5: the `session.set_mode` result. `active` is the effective selection;
// `pending` is true iff the change was queued behind an open turn.
struct SetModeResult {
    SessionId session;
    bool      active  = false;
    bool      pending = false;
};

// 53-D5: the `session.set_model` result. `model` is the effective wire id;
// `model_name` is the `llm.models` entry name ("" for a literal id); `pending`
// is true iff the change was queued behind an open turn.
struct SetModelResult {
    SessionId   session;
    std::string model;
    std::string model_name;
    bool        pending = false;
};

class TransportHost {
public:
    virtual ~TransportHost() = default;

    virtual HostState hostState() const = 0;
    virtual HostStatusInfo hostStatus() const = 0;
    virtual void requestShutdown(ShutdownReason reason) = 0;

    // 16 §7.4 (N-M3/N2-M2): the daemon's published fresh-id snapshot, typed in
    // transport terms. HostRuntime converts the ymh::SupervisorId snapshot
    // (§5.1) to wire-identical ClientInstanceId text. io thread only.
    [[nodiscard]] virtual std::shared_ptr<const std::vector<ClientInstanceId>>
    freshOwnerSnapshot() const = 0;

    virtual std::vector<WorkspaceSummary> listWorkspaces() = 0;
    virtual WorkspaceDetail showWorkspace(const WorkspaceId& id) = 0;

    virtual std::vector<SessionSummary> listSessions() = 0;
    virtual SessionDetail showSession(const SessionId& id) = 0;

    // 18 §3.4 (CX-13): read-only assembled-context snapshot, alongside
    // `showSession`. Returns the `ContextSnapshot` JSON form.
    virtual nlohmann::json showContext(const SessionId& id) = 0;

    virtual SessionCreated createSession(const nlohmann::json& params) = 0;
    virtual SessionResumed resumeSession(const SessionId& id) = 0;
    virtual SessionCreated forkSession(const SessionId& id, std::int64_t seed_length) = 0;
    virtual SessionRenamedResult renameSession(const nlohmann::json& params) = 0;
    virtual void closeSession(const SessionId& id) = 0;
    virtual void deleteSession(const SessionId& id, bool only_if_empty, bool force) = 0;
    virtual void activateSession(const SessionId& id) = 0;
    virtual void suspendSession(const SessionId& id) = 0;
    virtual void compactSession(const SessionId& id) = 0;

    virtual void agentPrompt(const SessionId& id, const nlohmann::json& message) = 0;
    virtual void agentFollowup(const SessionId& id, const nlohmann::json& message) = 0;
    virtual void agentSteer(const SessionId& id, const nlohmann::json& message) = 0;
    virtual void        agentInject(const SessionId& id, const nlohmann::json& context) = 0;
    virtual bool        agentCancel(const SessionId& id, const std::optional<std::string>& reason) = 0;
    virtual std::string agentStatus(const SessionId& id) = 0;

    // 20 §5.7: read-only skill listing/detail. Daemon-owned catalog; the
    // supervisor never reads skill files.
    virtual nlohmann::json listSkills() = 0;
    virtual nlohmann::json showSkill(const std::string& name) = 0;

    // 45-D6: read-only, session-less MCP inventory (both profiles), backed by
    // the shared MCP JSON schema (45-D6.9).
    virtual nlohmann::json mcpStatus() = 0;

    // 45-D9: roster + daemon-owned blank/can_select (both profiles); and the
    // blank-session-only switch (Interactive only; MethodNotAllowedForProfile
    // in Automation).
    virtual nlohmann::json listAgents(const nlohmann::json& params) = 0;
    virtual nlohmann::json selectAgent(const nlohmann::json& params) = 0;

    virtual SetModeResult  setSessionMode(const nlohmann::json& params) = 0;
    virtual SetModelResult setSessionModel(const nlohmann::json& params) = 0;

    virtual bool decidePermission(const std::string& request_id, PermissionAnswer decision,
                                  PermissionScope scope) = 0;

    virtual bool sessionExists(const SessionId& id) const = 0;
    virtual std::vector<EventRecord> readEvents(const SessionId& id, Sequence after,
                                                std::size_t limit) = 0;
    virtual Sequence headSequence(const SessionId& id) const = 0;
    virtual std::optional<Sequence> resolveCursor(const SessionId& id,
                                                  const EventCursor& cursor) const = 0;
    virtual EventCursor cursorFor(const SessionId& id, Sequence sequence) const = 0;
};

} // namespace ymh::protocol

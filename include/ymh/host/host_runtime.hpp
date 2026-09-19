#pragma once

// HostRuntime: the daemon-side adapter that binds the ~30
// `protocol::TransportHost` virtuals (include/ymh/transport/host.hpp) onto a
// `WorkspaceRuntime` + `WorkspaceRegistry` + `AgentRegistry` + `SessionStore`
// (docs/design/11-m2-errata.md §4, D11; docs/design/04 §3.7, docs/design/05 §7).
//
// It is the semantic half of the daemon: the transport owns framing, handshake,
// profiles, dispatch, streaming, and cursors; every method here translates a
// wire request into exactly one domain call and every domain failure back into
// exactly one `RpcCode|AppCode` with a stable `data.kind` (errata §4.4, E12).
//
// Two construction modes are provided:
//   * the errata §4.2 ctor, which takes an already-built `ProtocolServer&`;
//   * a two-phase variant (no server) plus `attachServer`, needed because
//     `ProtocolServer` stores a `TransportHost&` at construction and
//     `WorkspaceRuntime -> HostRuntime -> ProtocolServer -> TransportServer`
//     (errata §11.2) is therefore a construction cycle. Wave 3's daemon uses
//     the two-phase variant; tests and simple wiring use the pinned ctor.
//
// Event marshalling (errata §2.2 E2 / §4): HostRuntime owns a live `EventBus`
// subscription for the live-only MCP status branch and a committed-record
// subscription (24-D6). The committed channel carries the store `Sequence`, so
// the forwarder forwards the record directly and never re-reads the store
// (AL16/AL17). The daemon wires that forwarder to
// `TransportServer::post([server, r]{ server->onEventCommitted(r); })`, so the
// `ProtocolServer` fan-out always runs on the transport io thread (M-F1).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/permission/permission_broker.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session.hpp"
#include "ymh/transport/host.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh {

class WorkspaceRuntime;
class TurnExecutor;
class Agent;
struct AgentError;
enum class WorkspaceRuntimeErrorCode : std::uint8_t;
enum class ShutdownReason : std::uint8_t;  // workspace_host.hpp; opaque enum is complete

namespace protocol {
class ProtocolServer;
} // namespace protocol

// Daemon identity supplied once at startup (04 §2.1 / §4.4, errata §4.2).
struct HostIdentity {
    WorkspaceId           workspace;
    HostBootId            boot_id;
    HostPid               pid{0};
    std::filesystem::path socket_path;
};

class HostRuntime final : public protocol::TransportHost {
public:
    // Marshalling seam for committed events. The daemon wires this to
    // `TransportServer::post()`; when unset HostRuntime calls
    // `ProtocolServer::onEventCommitted` directly (single-threaded tests only).
    using EventForwarder = std::function<void(const EventRecord&)>;

    // A wire error: the numeric JSON-RPC/application code plus the stable
    // `data.kind` token (errata §4.4, E12). Exposed so the daemon and the test
    // suite can assert the mapping table without provoking every failure.
    struct WireError {
        int         code = 0;
        std::string kind;
    };

    // Errata §4.2 pinned constructor, plus the event-marshalling seam.
    HostRuntime(WorkspaceRuntime& runtime,
                WorkspaceRegistry& registry,
                HostIdentity identity,
                protocol::ProtocolServer& server,
                TurnExecutor& turns,
                PermissionBroker& broker,
                EventForwarder forwarder = {});

    // Two-phase variant for the daemon (see the construction-cycle note above).
    // The server is attached later with `attachServer`.
    HostRuntime(WorkspaceRuntime& runtime,
                WorkspaceRegistry& registry,
                HostIdentity identity,
                TurnExecutor& turns,
                PermissionBroker& broker,
                EventForwarder forwarder = {});

    ~HostRuntime() override;

    HostRuntime(const HostRuntime&) = delete;
    HostRuntime& operator=(const HostRuntime&) = delete;

    // Wires the `ProtocolServer` hooks: the subscriber-count snapshot reader
    // (D19.3) and the subscribe observer that re-broadcasts pending permission
    // requests to a late-attaching supervisor (D19.4). Idempotent.
    void attachServer(protocol::ProtocolServer& server);

    // Starts the single EventBus forwarding subscriber (idempotent; the ctor
    // already calls it). Exposed for the daemon after `attachServer`.
    void startForwarding();

    // ---- daemon hooks (Wave 3) ---------------------------------------------
    void setShutdownHook(std::function<void(ymh::ShutdownReason)> hook);
    void setState(protocol::HostState state) noexcept;

    // 16 §5.1/§7.4: the daemon's fresh-owner snapshot source, read (never a
    // SQLite call) by `freshOwnerSnapshot`. Unset yields an empty snapshot.
    using OwnerSnapshotSource =
        std::function<std::shared_ptr<const std::vector<SupervisorId>>()>;
    void setOwnerSnapshotSource(OwnerSnapshotSource source);

    // D19.3: publish the live Interactive-subscriber count for a session. The
    // io thread calls this from the subscribe observer; the daemon may also
    // call it. Thread-safe; the broker reads the snapshot off the turn thread.
    void publishSubscriberCount(const SessionId& session, std::size_t count);
    // io thread only: read `ProtocolServer::sessionSubscriberCount` and publish.
    void refreshSubscriberCount(const SessionId& session);

    // D19.4: the subscribe observer body, exposed so tests can drive the
    // late-attach re-broadcast without a live socket. Runs on the io thread.
    void onSubscriberAttached(const SessionId& session, protocol::ClientId client);

    // ---- TransportHost overrides -------------------------------------------
    protocol::HostState      hostState() const override;
    protocol::HostStatusInfo hostStatus() const override;
    void                     requestShutdown(protocol::ShutdownReason reason) override;
    [[nodiscard]] std::shared_ptr<const std::vector<protocol::ClientInstanceId>>
    freshOwnerSnapshot() const override;

    std::vector<protocol::WorkspaceSummary> listWorkspaces() override;
    protocol::WorkspaceDetail showWorkspace(const protocol::WorkspaceId& id) override;

    std::vector<protocol::SessionSummary> listSessions() override;
    protocol::SessionDetail               showSession(const SessionId& id) override;
    nlohmann::json                        showContext(const SessionId& id) override;

    protocol::SessionCreated createSession(const nlohmann::json& params) override;
    protocol::SessionResumed resumeSession(const SessionId& id) override;
    protocol::SessionCreated forkSession(const SessionId& id,
                                         std::int64_t seed_length) override;
    protocol::SessionRenamedResult renameSession(const nlohmann::json& params) override;
    void                     closeSession(const SessionId& id) override;
    void                     deleteSession(const SessionId& id, bool only_if_empty = false,
                                            bool force = false) override;
    // 24-D4/AL6: the single queue-aware pending predicate for the daemon. True
    // iff the agent has a queued trigger / in-flight turn OR the executor has a
    // queued or active body for the session. `deleteSession` refuses while this
    // holds (AL7).
    [[nodiscard]] bool hasPendingWork(const SessionId& id) const;
    void                     activateSession(const SessionId& id) override;
    void                     suspendSession(const SessionId& id) override;
    void                     compactSession(const SessionId& id) override;

    void        agentPrompt(const SessionId& id, const nlohmann::json& message) override;
    void        agentFollowup(const SessionId& id, const nlohmann::json& message) override;
    void        agentSteer(const SessionId& id, const nlohmann::json& message) override;
    void        agentInject(const SessionId& id, const nlohmann::json& context) override;
    bool        agentCancel(const SessionId& id,
                            const std::optional<std::string>& reason) override;
    std::string agentStatus(const SessionId& id) override;

    nlohmann::json listSkills() override;
    nlohmann::json showSkill(const std::string& name) override;

    bool decidePermission(const std::string& request_id, protocol::PermissionAnswer decision,
                          protocol::PermissionScope scope) override;

    bool                     sessionExists(const SessionId& id) const override;
    std::vector<EventRecord> readEvents(const SessionId& id, Sequence after,
                                        std::size_t limit) override;
    Sequence                 headSequence(const SessionId& id) const override;
    std::optional<Sequence>  resolveCursor(const SessionId& id,
                                           const protocol::EventCursor& cursor) const override;
    protocol::EventCursor    cursorFor(const SessionId& id, Sequence sequence) const override;

    // ---- typed error mapping (errata §4.4) ---------------------------------
    [[nodiscard]] static WireError map_agent_error(const AgentError& error) noexcept;
    [[nodiscard]] static WireError map_store_error(const std::exception& error) noexcept;
    [[nodiscard]] static WireError map_registry_error(const RegistryError& error) noexcept;
    [[nodiscard]] static WireError map_workspace_error(WorkspaceRuntimeErrorCode code) noexcept;

private:
    [[noreturn]] static void throw_mapped(const WireError& error, nlohmann::json data = {});

    template <class Fn>
    static decltype(auto) translate(Fn&& fn) {
        try {
            return std::forward<Fn>(fn)();
        } catch (const protocol::RpcException&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw;
        } catch (const RegistryError& error) {
            throw_mapped(map_registry_error(error));
        } catch (const std::exception& error) {
            throw_mapped(map_store_error(error));
        }
    }

    void wireServer();
    void handleCommittedRecord(const EventRecord& record);
    void ensureAgent(const SessionId& id);
    void acquireLeaseOrThrow(const SessionId& id);
    void releaseLeaseIfDurable(const SessionId& id);
    [[nodiscard]] std::size_t subscriberCount(const SessionId& session) const;

    WorkspaceRuntime&   runtime_;
    WorkspaceRegistry&  registry_;
    HostIdentity        identity_;
    protocol::ProtocolServer* server_ = nullptr;
    TurnExecutor&       turns_;
    PermissionBroker&   broker_;
    EventForwarder      forwarder_;

    std::atomic<protocol::HostState>       state_{protocol::HostState::Serving};
    std::optional<SessionId>               active_session_;
    std::function<void(ymh::ShutdownReason)> shutdown_hook_;
    OwnerSnapshotSource                    owner_snapshot_source_;

    Subscription committed_subscription_;
    Subscription live_subscription_;
    bool         forwarding_ = false;

    std::mutex                                 forward_mutex_;
    std::unordered_map<std::string, Sequence>  last_forwarded_;

    mutable std::mutex                            subscriber_mutex_;
    std::unordered_map<std::string, std::size_t>  subscriber_counts_;
};

} // namespace ymh

#pragma once

// In-process protocol engine (docs/design/05-transport.md).
//
// `ProtocolServer` owns everything above the socket: handshake negotiation,
// per-connection profiles, method dispatch, event multiplexing, per-session
// ordered delivery, opaque-cursor resume, reconnect supersede, and bounded
// per-client backpressure. The socket layer (spec 04's acceptor, wrapped by
// `TransportServer`) only moves bytes: it calls `openConnection`/`receiveBytes`
// and installs a `SendFn`/`DropFn`.
//
// Dispatch is serialized: every `receiveBytes`, `onEventCommitted`, and host
// notification runs on the owning thread, which is what makes per-session
// ordering (T6) and atomic subscribe (T8) hold without a second lock.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/transport/host.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::protocol {

using SendFn = std::function<void(ClientId, std::string)>;
using DropFn = std::function<void(ClientId, std::string)>;

struct ProtocolServerConfig {
    TransportLimits limits;
    std::uint32_t   uid{0};
    WorkspaceId     workspace;
    HostBootId      boot_id;
    HostPid         pid{0};
};

// D4/E3: the same-UID trust boundary (05 §4.2) compares against the daemon's
// real uid. The `ProtocolServerConfig::uid{0}` default is a sentinel and must
// never reach a live server; daemon construction sets `uid = current_uid()`.
[[nodiscard]] std::uint32_t current_uid() noexcept;

class ProtocolServer {
public:
    ProtocolServer(TransportHost& host, ProtocolServerConfig config);

    ProtocolServer(const ProtocolServer&) = delete;
    ProtocolServer& operator=(const ProtocolServer&) = delete;

    [[nodiscard]] ClientId openConnection(std::uint32_t peer_uid, std::int32_t peer_pid,
                                          SendFn send, DropFn drop);
    void receiveBytes(ClientId id, std::string_view bytes);
    void closeConnection(ClientId id);
    void onFrameWritten(ClientId id, std::size_t bytes);

    void onEventCommitted(const EventRecord& record);
    void onSessionClosed(const SessionId& session, std::string reason);
    void onLeaseLost(const SessionId& session, std::string detail);
    void onDaemonShuttingDown(std::string detail);
    void onPermissionRequest(const PermissionRequest& request);

    [[nodiscard]] std::size_t attachedClients() const;
    [[nodiscard]] bool hasClient(ClientId id) const;
    [[nodiscard]] std::optional<ServerProfile> profileOf(ClientId id) const;
    [[nodiscard]] std::optional<ClientInstanceId> instanceOf(ClientId id) const;
    [[nodiscard]] std::size_t subscriptionCount(ClientId id) const;
    [[nodiscard]] std::size_t outstandingBytes(ClientId id) const;
    [[nodiscard]] bool isDropped(ClientId id) const;
    [[nodiscard]] bool isHandshaken(ClientId id) const;

    // D19.3: attached Interactive connections subscribed to `session`. Carries
    // the same single-thread contract as every other accessor: io thread only.
    [[nodiscard]] std::size_t sessionSubscriberCount(const SessionId& session) const;

    // D19.4: invoked as `observer(session, client)` exactly once at the tail of
    // `handle_subscribe`, after the subscription is installed, and only for
    // Interactive connections (never on the replay-only early return). Runs
    // synchronously on the io/dispatch thread; must not block or re-enter
    // request dispatch.
    using SubscribeObserver = std::function<void(const SessionId&, ClientId)>;
    void set_subscribe_observer(SubscribeObserver observer);

    // E19: blocks the calling (non-io) thread until every attached client's
    // outbound queue is empty, or `grace` elapses. Returns true iff fully
    // drained. Never reads `connections_` off the io thread.
    bool waitForDrain(std::chrono::milliseconds grace);

private:
    struct Subscription {
        SessionId session;
    };

    struct Connection {
        ClientId                 id;
        std::uint32_t            peer_uid{0};
        std::int32_t             peer_pid{0};
        bool                     hello_done{false};
        bool                     dropped{false};
        bool                     close_when_drained{false};
        bool                     pumping{false};
        bool                     suppress_response{false};
        ServerProfile            profile{ServerProfile::Interactive};
        ClientInstanceId         instance;
        std::string              read_buffer;
        std::deque<std::string>  outbound;
        std::size_t              outstanding{0};
        std::set<std::string>    batch_in_flight;
        std::map<std::uint64_t, Subscription> subscriptions;
        SendFn                   send;
        DropFn                   drop;
    };

    Connection* find(ClientId id);

    void handle_body(Connection& conn, std::string_view body);
    void dispatch_request(Connection& conn, const Request& request);
    void dispatch_notification(Connection& conn, const Notification& notification);

    std::optional<nlohmann::json> handle_hello(Connection& conn, const Request& request);
    void handle_method(Connection& conn, const Request& request, std::string_view method_name);
    void handle_subscribe(Connection& conn, const Request& request, bool replay_only);

    void respond(Connection& conn, const RequestId& id, nlohmann::json result);
    void respond_error(Connection& conn, const RequestId& id, int code, std::string message,
                       nlohmann::json data = {});

    void enqueue(Connection& conn, const nlohmann::json& message);
    void enqueue_frame(Connection& conn, std::string frame);
    void pump(Connection& conn);
    void maybe_close_after_drain(Connection& conn);
    void drop_client(Connection& conn, std::string reason);
    void end_subscriptions(Connection& conn, const SessionId& session, std::string reason);

    [[nodiscard]] HostStatus compose_status(const Connection& conn) const;
    [[nodiscard]] nlohmann::json stream_notification(SubscriptionId subscription, bool replay,
                                                     const EventRecord& record);
    [[nodiscard]] bool session_known(const SessionId& session) const;
    void signal_drain_progress();

    TransportHost&        host_;
    ProtocolServerConfig  config_;
    std::map<std::uint64_t, Connection> connections_;
    std::uint64_t         next_client_{1};
    std::uint64_t         next_subscription_{1};

    SubscribeObserver            subscribe_observer_;
    bool                         shutdown_notice_emitted_{false};
    std::atomic<std::size_t>     outstanding_total_{0};
    std::mutex                   drain_mutex_;
    std::condition_variable      drain_cv_;
};

} // namespace ymh::protocol

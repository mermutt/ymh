#pragma once

// Supervisor-side pump-thread wrapper around `protocol::HostConnection`
// (docs/design/10-supervisor-tui.md §3.3, §5.4; docs/design/11-m2-errata.md §8).
//
// `HostConnection` is a synchronous, single-threaded client: `request()` and
// `nextNotification()` share one read buffer and one pending queue and are NOT
// safe to call concurrently. `SupervisorConnection` therefore owns the
// connection and confines ALL fd access to one pump thread:
//
//   UI thread                         pump thread
//   ─────────                         ───────────
//   submit(method, params, reply) ──► (queued) request() ──► reply(result)
//   track(session) / untrack          hello + event.subscribe (reconnect)
//   state() / cursor()                nextNotification() ──► sink callbacks
//
// Decoded wire frames (core `Event` envelopes, `HostNotice`, permission
// requests) are delivered to the sink callbacks on the pump thread; the sink is
// responsible for posting them to the UI thread. `UiEvent` is never on the wire
// (10 §4.5, U4).
//
// Reconnect (errata §8.2):
//   D20.1  two phases — hello, then per-session `event.subscribe`.
//   D20.2  `resume_hint` is unused in v1 (reserved).
//   D20.3  `ClientInstanceId` is supplied by the caller (supervisor-local).
//   D20.4  per-session cursors are in-memory, updated from `StreamNotification`.
//   D20.5  `CursorInvalid` → re-subscribe `beginning`, never `now`.
//   D20.6  idempotent apply lives in `UiEventAdapter` (keyed by `Event.id`).
//   D20.7  the hello is cross-checked against the registry row.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/session/ids.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/ui_event.hpp"

namespace ymh::ui {

// Supervisor-local view of the link to one daemon (mirrors `DaemonStatus`,
// 10 §2.1). `Dead` means the socket is gone and a reconnect is pending/backoff.
enum class SupervisorLinkState : std::uint8_t {
    Connecting,
    Attached,
    Detached,
    Dead,
};

[[nodiscard]] const char* supervisor_link_state_name(SupervisorLinkState state) noexcept;

struct SupervisorConnectionConfig {
    std::string                   socket_path;
    WorkspaceId                   workspace;              // expected registry id
    std::string                   expected_boot_id;       // registry row host->bootId (may be empty)
    protocol::ClientInstanceId    client_instance;        // persisted supervisor-local (D20.3)
    protocol::ServerProfile       profile = protocol::ServerProfile::Interactive;

    // Bounded intervals so a test can drive reconnect deterministically.
    std::chrono::milliseconds poll_interval{20};
    std::chrono::milliseconds reconnect_backoff{200};
    std::chrono::milliseconds handshake_timeout{5'000};
    std::chrono::milliseconds request_timeout{10'000};
    std::chrono::milliseconds ping_interval{protocol::kPingInterval};

    // Requests that stay in flight longer than this are answered with an error
    // rather than stalling the pump forever. Zero disables the cap.
    std::size_t max_queued_requests{256};
};

// All callbacks run ON THE PUMP THREAD (10 §3.3). Keep them short and never
// call back into the connection's blocking methods.
struct SupervisorSink {
    std::function<void(const protocol::SessionEnvelope&)> on_envelope;
    std::function<void(const protocol::HostNotice&)>      on_notice;
    std::function<void(const protocol::PermissionRequest&)> on_permission;
    std::function<void(SupervisorLinkState, const std::string& detail)> on_state;
};

// Outcome of one marshalled request.
struct SupervisorReply {
    bool                 ok = false;
    nlohmann::json       result;
    int                  error_code = 0;
    std::string          error;
};

class SupervisorConnection {
public:
    using ReplyFn = std::function<void(SupervisorReply)>;

    SupervisorConnection(SupervisorConnectionConfig config, SupervisorSink sink);
    ~SupervisorConnection();

    SupervisorConnection(const SupervisorConnection&) = delete;
    SupervisorConnection& operator=(const SupervisorConnection&) = delete;

    // Spawns the pump thread. Idempotent.
    void start();

    // Stops the pump thread and closes the socket. Safe to call twice. Must not
    // be called from a sink callback (it would self-join); in that case it only
    // requests shutdown.
    void stop();

    // Begin tracking a session: it is subscribed on the next attach (or
    // immediately if already attached). Idempotent.
    void track(const SessionId& session);
    void untrack(const SessionId& session);

    // Marshals `method`/`params` onto the pump. `reply` is invoked exactly once
    // on the pump thread (or synchronously if the connection is stopping).
    void submit(std::string method, nlohmann::json params, ReplyFn reply);

    // ---- observation (thread-safe) ----------------------------------------
    [[nodiscard]] SupervisorLinkState state() const;
    [[nodiscard]] bool attached() const;
    [[nodiscard]] protocol::ClientId clientId() const;
    [[nodiscard]] std::optional<protocol::EventCursor> cursor(const SessionId& session) const;
    [[nodiscard]] std::uint64_t attachCount() const;   // successful hello+subscribe phases

    // ---- test seams --------------------------------------------------------
    // Force the pump to drop the link and re-enter the reconnect state machine.
    void forceReconnect();
    // Block until `predicate` is true or the timeout elapses. Returns the
    // predicate value. Uses a condition variable, never a sleep loop.
    [[nodiscard]] bool waitUntil(const std::function<bool()>& predicate,
                                 std::chrono::milliseconds timeout);
    [[nodiscard]] bool waitForState(SupervisorLinkState state,
                                    std::chrono::milliseconds timeout);

private:
    struct PendingRequest {
        std::string    method;
        nlohmann::json params;
        ReplyFn        reply;
    };

    void pump();
    [[nodiscard]] bool attempt_attach();
    void subscribe_tracked();
    void subscribe_one(const SessionId& session);
    void dispatch(const protocol::Notification& notification);
    void process_requests();
    void maybe_ping();
    void handle_disconnect(const std::string& detail);

    void set_state(SupervisorLinkState state, std::string detail);
    [[nodiscard]] bool stopping() const;

    SupervisorConnectionConfig                       config_;
    SupervisorSink                                   sink_;
    mutable std::mutex                               mutex_;
    std::condition_variable                          cv_;
    std::vector<PendingRequest>                      requests_;
    std::map<SessionId, protocol::EventCursor>       cursors_;
    std::vector<SessionId>                           tracked_;   // insertion order
    std::set<SessionId>                              subscribed_;  // on the live link
    std::unique_ptr<protocol::HostConnection>        connection_;   // pump-owned
    SupervisorLinkState                              state_ = SupervisorLinkState::Connecting;
    std::string                                      state_detail_;
    protocol::ClientId                               client_id_{};
    std::thread                                      pump_;
    bool                                             started_ = false;
    bool                                             stop_requested_ = false;
    bool                                             force_reconnect_ = false;
    bool                                             subscribe_pending_ = false;
    std::uint64_t                                    attach_count_ = 0;
    std::uint64_t                                    state_generation_ = 0;
    std::chrono::steady_clock::time_point            last_ping_ = std::chrono::steady_clock::now();
};

} // namespace ymh::ui

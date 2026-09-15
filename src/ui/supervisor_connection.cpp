#include "ymh/ui/supervisor_connection.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace ymh::ui {
namespace {

[[nodiscard]] bool is_cursor_invalid(const protocol::RpcException& error) {
    return error.code() == static_cast<int>(protocol::AppCode::CursorInvalid);
}

[[nodiscard]] bool is_unknown_session(const protocol::RpcException& error) {
    return error.code() == static_cast<int>(protocol::AppCode::UnknownSession);
}

} // namespace

const char* supervisor_link_state_name(SupervisorLinkState state) noexcept {
    switch (state) {
        case SupervisorLinkState::Connecting:
            return "connecting";
        case SupervisorLinkState::Attached:
            return "attached";
        case SupervisorLinkState::Detached:
            return "detached";
        case SupervisorLinkState::Dead:
            return "dead";
    }
    return "unknown";
}

SupervisorConnection::SupervisorConnection(SupervisorConnectionConfig config,
                                           SupervisorSink sink)
    : config_(std::move(config)), sink_(std::move(sink)) {}

SupervisorConnection::~SupervisorConnection() { stop(); }

void SupervisorConnection::start() {
    std::lock_guard lock(mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    pump_ = std::thread([this] { pump(); });
}

void SupervisorConnection::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (pump_.joinable() && pump_.get_id() != std::this_thread::get_id()) {
        pump_.join();
    }
}

void SupervisorConnection::track(const SessionId& session) {
    {
        std::lock_guard lock(mutex_);
        if (std::find(tracked_.begin(), tracked_.end(), session) == tracked_.end()) {
            tracked_.push_back(session);
        }
        subscribe_pending_ = true;
    }
    cv_.notify_all();
}

void SupervisorConnection::untrack(const SessionId& session) {
    {
        std::lock_guard lock(mutex_);
        tracked_.erase(std::remove(tracked_.begin(), tracked_.end(), session), tracked_.end());
        subscribed_.erase(session);
        cursors_.erase(session);
    }
}

void SupervisorConnection::submit(std::string method, nlohmann::json params, ReplyFn reply) {
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_) {
            if (reply) {
                reply(SupervisorReply{false, {}, 0, "supervisor connection stopped"});
            }
            return;
        }
        if (config_.max_queued_requests != 0 &&
            requests_.size() >= config_.max_queued_requests) {
            if (reply) {
                reply(SupervisorReply{false, {}, 0, "too many in-flight supervisor requests"});
            }
            return;
        }
        requests_.push_back(PendingRequest{std::move(method), std::move(params), std::move(reply)});
    }
    cv_.notify_all();
}

SupervisorLinkState SupervisorConnection::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool SupervisorConnection::attached() const { return state() == SupervisorLinkState::Attached; }

protocol::ClientId SupervisorConnection::clientId() const {
    std::lock_guard lock(mutex_);
    return client_id_;
}

std::optional<protocol::EventCursor> SupervisorConnection::cursor(const SessionId& session) const {
    std::lock_guard lock(mutex_);
    const auto it = cursors_.find(session);
    if (it == cursors_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint64_t SupervisorConnection::attachCount() const {
    std::lock_guard lock(mutex_);
    return attach_count_;
}

void SupervisorConnection::forceReconnect() {
    {
        std::lock_guard lock(mutex_);
        force_reconnect_ = true;
    }
    cv_.notify_all();
}

bool SupervisorConnection::waitUntil(const std::function<bool()>& predicate,
                                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (predicate()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return predicate();
        }
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::min(std::chrono::milliseconds{20},
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        deadline - now)));
    }
}

bool SupervisorConnection::waitForState(SupervisorLinkState state,
                                        std::chrono::milliseconds timeout) {
    return waitUntil([this, state] { return this->state() == state; }, timeout);
}

bool SupervisorConnection::stopping() const {
    std::lock_guard lock(mutex_);
    return stop_requested_;
}

void SupervisorConnection::set_state(SupervisorLinkState state, std::string detail) {
    {
        std::lock_guard lock(mutex_);
        state_ = state;
        state_detail_ = detail;
        ++state_generation_;
    }
    cv_.notify_all();
    if (sink_.on_state) {
        sink_.on_state(state, detail);
    }
}

bool SupervisorConnection::attempt_attach() {
    set_state(SupervisorLinkState::Connecting, "handshake in flight");
    auto connection = std::make_unique<protocol::HostConnection>();
    try {
        connection->connect(config_.socket_path);
        const protocol::HelloResult hello = connection->handshake(
            config_.profile, config_.client_instance, config_.handshake_timeout);
        if (hello.workspace.value != config_.workspace.value) {
            connection->close();
            set_state(SupervisorLinkState::Dead, "attach identity mismatch (workspace)");
            return false;
        }
        if (!config_.expected_boot_id.empty() && hello.boot_id.value != config_.expected_boot_id) {
            connection->close();
            set_state(SupervisorLinkState::Dead, "attach identity mismatch (boot_id)");
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            subscribed_.clear();
            client_id_ = hello.client_id;
        }
        connection_ = std::move(connection);
    } catch (const std::exception& error) {
        if (connection != nullptr) {
            connection->close();
        }
        set_state(SupervisorLinkState::Dead, error.what());
        return false;
    }

    set_state(SupervisorLinkState::Attached, "hello accepted");
    subscribe_tracked();
    {
        std::lock_guard lock(mutex_);
        ++attach_count_;
    }
    cv_.notify_all();
    return true;
}

void SupervisorConnection::subscribe_tracked() {
    std::vector<SessionId> pending;
    {
        std::lock_guard lock(mutex_);
        for (const SessionId& session : tracked_) {
            if (subscribed_.find(session) == subscribed_.end()) {
                pending.push_back(session);
            }
        }
    }
    for (const SessionId& session : pending) {
        subscribe_one(session);
    }
}

void SupervisorConnection::subscribe_one(const SessionId& session) {
    protocol::StreamFrom from;
    {
        std::lock_guard lock(mutex_);
        const auto it = cursors_.find(session);
        if (it != cursors_.end()) {
            from.kind = protocol::StreamFrom::Kind::Cursor;
            from.cursor = it->second;
        } else {
            from.kind = protocol::StreamFrom::Kind::Beginning;
        }
    }

    nlohmann::json params;
    protocol::to_json(params, protocol::SubscribeParams{session, from});
    try {
        static_cast<void>(connection_->request(protocol::method::kEventSubscribe, params,
                                               config_.request_timeout));
        std::lock_guard lock(mutex_);
        subscribed_.insert(session);
        return;
    } catch (const protocol::RpcException& error) {
        if (is_unknown_session(error)) {
            std::lock_guard lock(mutex_);
            subscribed_.insert(session);
            return;
        }
        if (!is_cursor_invalid(error)) {
            return;
        }
    } catch (const std::exception&) {
        return;
    }

    // D20.5: drop the cursor and re-subscribe from `beginning`, never `now`.
    {
        std::lock_guard lock(mutex_);
        cursors_.erase(session);
    }
    protocol::StreamFrom beginning;
    beginning.kind = protocol::StreamFrom::Kind::Beginning;
    nlohmann::json retry;
    protocol::to_json(retry, protocol::SubscribeParams{session, beginning});
    try {
        static_cast<void>(connection_->request(protocol::method::kEventSubscribe, retry,
                                               config_.request_timeout));
        std::lock_guard lock(mutex_);
        subscribed_.insert(session);
    } catch (const std::exception&) {
        // Left unsubscribed; the next reconnect retries the whole phase.
    }
}

void SupervisorConnection::dispatch(const protocol::Notification& notification) {
    if (notification.method == protocol::notify::kEventStream) {
        const auto stream = notification.params.get<protocol::StreamNotification>();
        const protocol::SessionEnvelope& envelope = stream.envelope;
        if (envelope.session != envelope.event.session_id) {
            return;
        }
        if (sink_.on_envelope) {
            sink_.on_envelope(envelope);
        }
        std::lock_guard lock(mutex_);
        cursors_[envelope.session] = stream.cursor;
        return;
    }
    if (notification.method == protocol::notify::kPermissionRequest) {
        const auto request = notification.params.get<protocol::PermissionRequest>();
        if (sink_.on_permission) {
            sink_.on_permission(request);
        }
        return;
    }
    if (notification.method == protocol::notify::kHostEvent) {
        const auto notice = notification.params.get<protocol::HostNotice>();
        if (sink_.on_notice) {
            sink_.on_notice(notice);
        }
    }
}

void SupervisorConnection::process_requests() {
    std::vector<PendingRequest> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(requests_);
    }
    for (PendingRequest& request : batch) {
        SupervisorReply reply;
        try {
            reply.ok = true;
            reply.result = connection_->request(request.method, std::move(request.params),
                                                config_.request_timeout);
        } catch (const protocol::RpcException& error) {
            reply.ok = false;
            reply.error_code = error.code();
            reply.error = error.what();
        } catch (const std::exception& error) {
            reply.ok = false;
            reply.error = error.what();
        }
        if (request.reply) {
            request.reply(std::move(reply));
        }
        {
            std::lock_guard lock(mutex_);
            ++state_generation_;
        }
        cv_.notify_all();
        if (connection_ == nullptr || !connection_->isConnected()) {
            handle_disconnect("connection lost while awaiting a reply");
            return;
        }
    }
}

void SupervisorConnection::maybe_ping() {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex_);
        if (now - last_ping_ < config_.ping_interval) {
            return;
        }
        last_ping_ = now;
    }
    try {
        static_cast<void>(
            connection_->request(protocol::method::kHostPing, nlohmann::json::object(),
                                 config_.request_timeout));
    } catch (const std::exception& error) {
        handle_disconnect(std::string("ping failed: ") + error.what());
    }
}

void SupervisorConnection::handle_disconnect(const std::string& detail) {
    if (connection_ != nullptr) {
        connection_->close();
    }
    {
        std::lock_guard lock(mutex_);
        subscribed_.clear();
        client_id_ = protocol::ClientId{};
    }
    set_state(SupervisorLinkState::Dead, detail);
}

void SupervisorConnection::pump() {
    set_state(SupervisorLinkState::Connecting, "starting");
    while (true) {
        bool reconnect = false;
        bool pending_subscribe = false;
        {
            std::lock_guard lock(mutex_);
            if (stop_requested_) {
                break;
            }
            reconnect = force_reconnect_;
            force_reconnect_ = false;
            pending_subscribe = subscribe_pending_;
            subscribe_pending_ = false;
        }
        if (reconnect) {
            if (connection_ != nullptr) {
                connection_->close();
            }
            {
                std::lock_guard lock(mutex_);
                subscribed_.clear();
            }
            set_state(SupervisorLinkState::Dead, "reconnect requested");
        }

        if (!attached()) {
            if (!attempt_attach()) {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, config_.reconnect_backoff,
                             [this] { return stop_requested_; });
                continue;
            }
        } else if (pending_subscribe) {
            subscribe_tracked();
        }

        process_requests();
        if (!attached()) {
            continue;
        }
        maybe_ping();
        if (!attached()) {
            continue;
        }
        try {
            const std::optional<protocol::Notification> notification =
                connection_->nextNotification(config_.poll_interval);
            if (notification.has_value()) {
                dispatch(*notification);
            } else if (connection_ == nullptr || !connection_->isConnected()) {
                handle_disconnect("connection closed");
            }
        } catch (const std::exception& error) {
            handle_disconnect(error.what());
        }
    }

    std::vector<PendingRequest> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(requests_);
    }
    for (PendingRequest& request : pending) {
        if (request.reply) {
            request.reply(SupervisorReply{false, {}, 0, "supervisor connection stopped"});
        }
    }
    if (connection_ != nullptr) {
        connection_->close();
        connection_.reset();
    }
    set_state(SupervisorLinkState::Detached, "stopped");
}

} // namespace ymh::ui

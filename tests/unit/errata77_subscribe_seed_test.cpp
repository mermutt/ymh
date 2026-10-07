// 77 §10.2: the `event.subscribe` seed rule (77-D2 / OL2, OL3, OL7, OL10,
// OL-F2), driven against a scripted host so the captured `SubscribeParams.from`
// is observable. The `SupervisorConnection` owns the pump; the fake server
// records every subscribe.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/short_temp.hpp"
#include "ymh/session/events.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_connection.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr std::string_view kWorkspace = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kBoot = "22222222-2222-4222-8222-222222222222";
const SessionId kSession{"session-1"};

// Minimal JSON-RPC host: answers hello/event.subscribe, records the subscribe
// params, can reject a cursor once, and can push a stream notification.
class ScriptedHost {
public:
    explicit ScriptedHost(const std::filesystem::path& socket_path) : path_(socket_path) {
        ::unlink(path_.c_str());
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string text = path_.string();
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("scripted host bind failed");
        }
        if (::listen(listen_fd_, 4) != 0) {
            throw std::runtime_error("scripted host listen failed");
        }
        thread_ = std::thread([this] { accept_loop(); });
    }

    ~ScriptedHost() { stop(); }

    ScriptedHost(const ScriptedHost&) = delete;
    ScriptedHost& operator=(const ScriptedHost&) = delete;

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        {
            const std::lock_guard lock(clients_mutex_);
            for (const int fd : clients_) {
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        for (std::thread& handler : handlers_) {
            if (handler.joinable()) {
                handler.join();
            }
        }
        handlers_.clear();
        ::unlink(path_.c_str());
    }

    void set_invalid_cursor_once() { invalid_cursor_once_.store(true); }

    [[nodiscard]] std::vector<protocol::SubscribeParams> subscribes() const {
        const std::lock_guard lock(state_mutex_);
        return subscribes_;
    }

    void push_stream(const SessionId& session, const std::string& cursor) {
        protocol::StreamNotification notification;
        notification.subscription = protocol::SubscriptionId{1};
        notification.replay = false;
        notification.cursor = protocol::EventCursor{cursor};
        notification.envelope.session = session;
        notification.envelope.event.id.value = "e-" + cursor;
        notification.envelope.event.session_id = session;
        notification.envelope.event.timestamp = std::chrono::system_clock::now();
        notification.envelope.event.type = EventType::AssistantMessage;
        notification.envelope.event.payload = payload::AssistantMessage{};
        nlohmann::json body;
        protocol::to_json(body, notification);
        broadcast(protocol::encode(
            protocol::Notification{std::string(protocol::notify::kEventStream), std::move(body)}));
    }

private:
    void accept_loop() {
        while (running_.load()) {
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            {
                const std::lock_guard lock(clients_mutex_);
                clients_.push_back(client);
            }
            handlers_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(int client) {
        std::string buffer;
        while (running_.load()) {
            char chunk[65536];
            const ssize_t count = ::read(client, chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(count));
            std::string frame;
            while (true) {
                protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
                try {
                    status = protocol::FrameCodec::decode_step(
                        buffer, protocol::TransportLimits{}.max_frame_bytes, frame);
                } catch (const std::exception&) {
                    status = protocol::DecodeStatus::NeedMore;
                }
                if (status != protocol::DecodeStatus::Ok) {
                    break;
                }
                handle(frame, client);
            }
        }
        ::close(client);
        const std::lock_guard lock(clients_mutex_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    }

    void handle(const std::string& frame, int client) {
        protocol::Message message = protocol::parse_message(frame);
        const auto* request = std::get_if<protocol::Request>(&message);
        if (request == nullptr) {
            return;
        }
        if (request->method == protocol::method::kHostHello) {
            protocol::HelloResult hello;
            hello.workspace.value = std::string{kWorkspace};
            hello.boot_id.value = std::string{kBoot};
            hello.client_id = protocol::ClientId{1};
            nlohmann::json body;
            protocol::to_json(body, hello);
            respond(request->id, body, client);
            return;
        }
        if (request->method == protocol::method::kHostPing) {
            respond(request->id, {{"server_time_ms", 1}}, client);
            return;
        }
        if (request->method == protocol::method::kEventSubscribe) {
            const auto params = request->params.get<protocol::SubscribeParams>();
            {
                const std::lock_guard lock(state_mutex_);
                subscribes_.push_back(params);
            }
            if (params.from.kind == protocol::StreamFrom::Kind::Cursor &&
                invalid_cursor_once_.exchange(false)) {
                send_to(client, protocol::encode(protocol::ErrorResponse{
                                    request->id, static_cast<int>(protocol::AppCode::CursorInvalid),
                                    "cursor invalid", nlohmann::json::object()}));
                return;
            }
            protocol::SubscribeResult result;
            result.subscription = protocol::SubscriptionId{1};
            result.cursor = protocol::EventCursor{"c1:s:0"};
            nlohmann::json body;
            protocol::to_json(body, result);
            respond(request->id, body, client);
            return;
        }
        if (request->method == protocol::method::kEventUnsubscribe) {
            respond(request->id, nlohmann::json::object(), client);
            return;
        }
        respond(request->id, nlohmann::json::object(), client);
    }

    void respond(const protocol::RequestId& id, nlohmann::json result, int client) {
        send_to(client, protocol::encode(protocol::Response{id, std::move(result)}));
    }

    void broadcast(const nlohmann::json& message) {
        std::vector<int> clients;
        {
            const std::lock_guard lock(clients_mutex_);
            clients = clients_;
        }
        for (const int fd : clients) {
            send_to(fd, message);
        }
    }

    void send_to(int fd, const nlohmann::json& message) {
        if (fd < 0) {
            return;
        }
        const std::string frame =
            protocol::FrameCodec::encode(message.dump(), protocol::TransportLimits{}.max_frame_bytes);
        const std::lock_guard lock(write_mutex_);
        std::size_t written = 0;
        while (written < frame.size()) {
            const ssize_t count =
                ::send(fd, frame.data() + written, frame.size() - written, MSG_NOSIGNAL);
            if (count <= 0) {
                break;
            }
            written += static_cast<std::size_t>(count);
        }
    }

    std::filesystem::path path_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{true};
    std::atomic<bool> invalid_cursor_once_{false};
    std::thread thread_;
    std::vector<std::thread> handlers_;
    mutable std::mutex clients_mutex_;
    std::vector<int> clients_;
    mutable std::mutex write_mutex_;
    mutable std::mutex state_mutex_;
    std::vector<protocol::SubscribeParams> subscribes_;
};

struct Collected {
    mutable std::mutex mutex;
    std::vector<protocol::SessionEnvelope> envelopes;
    void add(const protocol::SessionEnvelope& envelope) {
        const std::lock_guard lock(mutex);
        envelopes.push_back(envelope);
    }
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex);
        return envelopes.size();
    }
};

struct Fixture {
    explicit Fixture(const std::string& name)
        : root(name), socket_path(root.path() / "host.sock"), host(socket_path) {
        SupervisorConnectionConfig config;
        config.socket_path = socket_path.string();
        config.workspace = WorkspaceId{std::string{kWorkspace}};
        config.expected_boot_id = std::string{kBoot};
        config.client_instance = protocol::ClientInstanceId{"errata77-seed"};
        config.poll_interval = 5ms;
        config.reconnect_backoff = 10ms;
        config.ping_interval = 10s;
        config.request_timeout = 2s;
        SupervisorSink sink;
        sink.on_envelope = [this](const protocol::SessionEnvelope& envelope, bool,
                                  std::optional<protocol::EventCursor>) {
            collected.add(envelope);
        };
        connection = std::make_unique<SupervisorConnection>(std::move(config), std::move(sink));
        connection->start();
        EXPECT_TRUE(connection->waitForState(SupervisorLinkState::Attached, 3s));
    }

    ShortTempRoot root;
    std::filesystem::path socket_path;
    ScriptedHost host;
    Collected collected;
    std::unique_ptr<SupervisorConnection> connection;
};

TEST(Errata77SubscribeSeed, OL2CallerSeedInitializesTheCursor) {
    Fixture fixture("errata77-seed-ol2");
    EXPECT_TRUE(fixture.connection->waitForState(SupervisorLinkState::Attached, 3s));

    fixture.connection->track(kSession, protocol::EventCursor{"S"});
    ASSERT_TRUE(fixture.connection->waitUntil(
        [&] { return fixture.host.subscribes().size() >= 1; }, 3s));
    const auto subscribes = fixture.host.subscribes();
    ASSERT_EQ(subscribes.size(), 1u);
    EXPECT_EQ(subscribes[0].from.kind, protocol::StreamFrom::Kind::Cursor);
    ASSERT_TRUE(subscribes[0].from.cursor.has_value());
    EXPECT_EQ(subscribes[0].from.cursor->value, "S");
}

TEST(Errata77SubscribeSeed, OL7FreshOpenUsesBeginning) {
    Fixture fixture("errata77-seed-ol7");
    EXPECT_TRUE(fixture.connection->waitForState(SupervisorLinkState::Attached, 3s));

    fixture.connection->track(kSession);
    ASSERT_TRUE(fixture.connection->waitUntil(
        [&] { return fixture.host.subscribes().size() >= 1; }, 3s));
    const auto subscribes = fixture.host.subscribes();
    ASSERT_EQ(subscribes.size(), 1u);
    EXPECT_EQ(subscribes[0].from.kind, protocol::StreamFrom::Kind::Beginning);
}

TEST(Errata77SubscribeSeed, OL3NeverUsesNow) {
    Fixture fixture("errata77-seed-ol3");
    EXPECT_TRUE(fixture.connection->waitForState(SupervisorLinkState::Attached, 3s));

    fixture.connection->track(kSession);
    ASSERT_TRUE(fixture.connection->waitUntil(
        [&] { return fixture.host.subscribes().size() >= 1; }, 3s));
    for (const auto& params : fixture.host.subscribes()) {
        EXPECT_NE(params.from.kind, protocol::StreamFrom::Kind::Now);
    }
}

TEST(Errata77SubscribeSeed, OL10ReconnectUsesFresherCursorNotStaleSeed) {
    Fixture fixture("errata77-seed-ol10");
    EXPECT_TRUE(fixture.connection->waitForState(SupervisorLinkState::Attached, 3s));

    fixture.connection->track(kSession, protocol::EventCursor{"S"});
    ASSERT_TRUE(fixture.connection->waitUntil(
        [&] { return fixture.host.subscribes().size() >= 1; }, 3s));
    ASSERT_EQ(fixture.host.subscribes()[0].from.cursor->value, "S");

    fixture.host.push_stream(kSession, "A");
    ASSERT_TRUE(fixture.connection->waitUntil([&] { return fixture.collected.size() >= 1; }, 3s));

    // A stale track-time seed must not overwrite the fresher delivered cursor.
    fixture.connection->track(kSession, protocol::EventCursor{"S"});
    fixture.connection->forceReconnect();
    ASSERT_TRUE(fixture.connection->waitUntil(
        [&] { return fixture.host.subscribes().size() >= 2; }, 3s));
    const auto subscribes = fixture.host.subscribes();
    ASSERT_GE(subscribes.size(), 2u);
    EXPECT_EQ(subscribes.back().from.kind, protocol::StreamFrom::Kind::Cursor);
    ASSERT_TRUE(subscribes.back().from.cursor.has_value());
    EXPECT_EQ(subscribes.back().from.cursor->value, "A");
}

TEST(Errata77SubscribeSeed, OLF2CursorInvalidFallsBackToBeginning) {
    Fixture fixture("errata77-seed-olf2");
    EXPECT_TRUE(fixture.connection->waitForState(SupervisorLinkState::Attached, 3s));

    fixture.host.set_invalid_cursor_once();
    fixture.connection->track(kSession, protocol::EventCursor{"S"});
    ASSERT_TRUE(fixture.connection->waitUntil(
        [&] { return fixture.host.subscribes().size() >= 2; }, 3s));
    const auto subscribes = fixture.host.subscribes();
    ASSERT_GE(subscribes.size(), 2u);
    EXPECT_EQ(subscribes[0].from.kind, protocol::StreamFrom::Kind::Cursor);
    EXPECT_EQ(subscribes[0].from.cursor->value, "S");
    EXPECT_EQ(subscribes[1].from.kind, protocol::StreamFrom::Kind::Beginning);
}

} // namespace

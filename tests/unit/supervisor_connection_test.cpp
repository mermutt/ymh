#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
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
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_connection.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace std::chrono_literals;

constexpr std::string_view kWorkspace = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kBoot = "22222222-2222-4222-8222-222222222222";

// A minimal JSON-RPC server that speaks the pinned framing so the pump can be
// driven deterministically: it counts pings, records subscribes, can reject a
// cursor once, and can push notifications and permission requests on demand.
class ScriptedServer {
public:
    explicit ScriptedServer(const std::filesystem::path& socket_path)
        : path_(socket_path) {
        ::unlink(path_.c_str());
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string text = path_.string();
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("scripted server bind failed");
        }
        if (::listen(listen_fd_, 4) != 0) {
            throw std::runtime_error("scripted server listen failed");
        }
        thread_ = std::thread([this] { accept_loop(); });
    }

    ~ScriptedServer() { stop(); }

    ScriptedServer(const ScriptedServer&) = delete;
    ScriptedServer& operator=(const ScriptedServer&) = delete;

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        drop_client();
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

    void drop_client() {
        std::vector<int> clients;
        {
            const std::lock_guard lock(clients_mutex_);
            clients = clients_;
        }
        for (const int fd : clients) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    [[nodiscard]] int hello_count() const { return hello_count_.load(); }
    [[nodiscard]] int ping_count() const { return ping_count_.load(); }
    [[nodiscard]] int attach_generation() const { return hello_count_.load(); }

    [[nodiscard]] std::vector<protocol::SubscribeParams> subscribes() const {
        const std::lock_guard lock(state_mutex_);
        return subscribes_;
    }

    [[nodiscard]] std::optional<protocol::HelloParams> last_hello() const {
        const std::lock_guard lock(state_mutex_);
        return last_hello_;
    }

    void set_invalid_cursor_once() { invalid_cursor_once_.store(true); }
    void set_reject_second_decide(bool value) { reject_second_decide_.store(value); }

    void push_stream(const SessionId& session, EventType type, const std::string& cursor,
                     bool replay = false) {
        protocol::StreamNotification notification;
        notification.subscription = protocol::SubscriptionId{1};
        notification.replay = replay;
        notification.cursor = protocol::EventCursor{cursor};
        notification.envelope.session = session;
        notification.envelope.event.id.value = "e-" + cursor;
        notification.envelope.event.session_id = session;
        notification.envelope.event.timestamp = std::chrono::system_clock::now();
        notification.envelope.event.type = type;
        nlohmann::json body;
        protocol::to_json(body, notification);
        broadcast(protocol::encode(
            protocol::Notification{std::string(protocol::notify::kEventStream), std::move(body)}));
    }

    void push_live(const SessionId& session, EventType type) {
        protocol::LiveNotification notification;
        notification.envelope.session           = session;
        notification.envelope.event.id.value    = "live-1";
        notification.envelope.event.session_id  = session;
        notification.envelope.event.timestamp   = std::chrono::system_clock::now();
        notification.envelope.event.type        = type;
        nlohmann::json body;
        protocol::to_json(body, notification);
        broadcast(protocol::encode(
            protocol::Notification{std::string(protocol::notify::kEventLive), std::move(body)}));
    }

    void push_unknown_stream(const SessionId& session, const std::string& type,
                             const std::string& cursor) {
        nlohmann::json body;
        body["subscription"] = 1;
        body["replay"]       = false;
        body["cursor"]       = cursor;
        body["envelope"]     = nlohmann::json{
            {"session", session.value},
            {"event",
             nlohmann::json{{"id", "e-" + cursor},
                            {"session_id", session.value},
                            {"timestamp", 0},
                            {"type", type},
                            {"payload", nlohmann::json::object()}}}};
        broadcast(protocol::encode(
            protocol::Notification{std::string(protocol::notify::kEventStream), std::move(body)}));
    }

    void push_permission(const protocol::PermissionRequest& request) {
        nlohmann::json body;
        protocol::to_json(body, request);
        broadcast(protocol::encode(protocol::Notification{
            std::string(protocol::notify::kPermissionRequest), std::move(body)}));
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
            hello_count_.fetch_add(1);
            {
                const std::lock_guard lock(state_mutex_);
                last_hello_ = request->params.get<protocol::HelloParams>();
            }
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
            ping_count_.fetch_add(1);
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
        if (request->method == protocol::method::kPermissionDecide) {
            if (reject_second_decide_.load() && decision_count_.fetch_add(1) > 0) {
                send_to(client, protocol::encode(protocol::ErrorResponse{
                                    request->id, static_cast<int>(protocol::RpcCode::InvalidParams),
                                    "already decided", nlohmann::json::object()}));
                return;
            }
            decision_count_.fetch_add(1);
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
    int listen_fd_{-1};
    std::thread thread_;
    std::vector<std::thread> handlers_;
    std::atomic<bool> running_{true};
    std::atomic<int> hello_count_{0};
    std::atomic<int> ping_count_{0};
    std::atomic<int> decision_count_{0};
    std::atomic<bool> invalid_cursor_once_{false};
    std::atomic<bool> reject_second_decide_{false};
    mutable std::mutex state_mutex_;
    std::vector<protocol::SubscribeParams> subscribes_;
    std::optional<protocol::HelloParams> last_hello_;
    mutable std::mutex clients_mutex_;
    std::vector<int> clients_;
    std::mutex write_mutex_;
};

struct Collected {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<protocol::SessionEnvelope> envelopes;
    std::vector<protocol::PermissionRequest> permissions;
    std::vector<SupervisorLinkState> states;

    void add_envelope(const protocol::SessionEnvelope& envelope) {
        const std::lock_guard lock(mutex);
        envelopes.push_back(envelope);
        cv.notify_all();
    }
    void add_permission(const protocol::PermissionRequest& request) {
        const std::lock_guard lock(mutex);
        permissions.push_back(request);
        cv.notify_all();
    }
    void add_state(SupervisorLinkState state) {
        const std::lock_guard lock(mutex);
        states.push_back(state);
        cv.notify_all();
    }
    bool wait_envelopes(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return envelopes.size() >= count; });
    }
    bool wait_permissions(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return permissions.size() >= count; });
    }
};

SupervisorConnectionConfig config_for(const std::filesystem::path& socket_path,
                                      std::chrono::milliseconds ping = 10s) {
    SupervisorConnectionConfig config;
    config.socket_path = socket_path.string();
    config.workspace = WorkspaceId{std::string{kWorkspace}};
    config.expected_boot_id = std::string{kBoot};
    config.client_instance = protocol::ClientInstanceId{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
    config.poll_interval = 10ms;
    config.reconnect_backoff = 20ms;
    config.ping_interval = ping;
    config.request_timeout = 2s;
    return config;
}

SupervisorSink sink_for(Collected& collected) {
    SupervisorSink sink;
    sink.on_envelope = [&collected](const protocol::SessionEnvelope& envelope) {
        collected.add_envelope(envelope);
    };
    sink.on_permission = [&collected](const protocol::PermissionRequest& request) {
        collected.add_permission(request);
    };
    sink.on_state = [&collected](SupervisorLinkState state, const std::string&) {
        collected.add_state(state);
    };
    return sink;
}

const SessionId kSession{"session-1"};

TEST(SupervisorConnection, DispatchesLiveEventsWithoutAdvancingCursor) {
    test::ShortTempRoot root("ymh_sup_live");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected      collected;

    SupervisorConnection connection(config_for(socket_path), sink_for(collected));
    connection.track(kSession);
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));

    server.push_stream(kSession, EventType::TurnStarted, "c1:s:1", true);
    ASSERT_TRUE(collected.wait_envelopes(1, 3s));
    server.push_live(kSession, EventType::AssistantChunk);
    ASSERT_TRUE(collected.wait_envelopes(2, 3s));
    EXPECT_EQ(collected.envelopes[1].event.type, EventType::AssistantChunk);
    ASSERT_TRUE(connection.cursor(kSession).has_value());
    EXPECT_EQ(connection.cursor(kSession)->value, "c1:s:1");

    connection.stop();
}

TEST(SupervisorConnection, MarshalsRequestsAndDeliversEnvelopes) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnection connection(config_for(socket_path), sink_for(collected));
    connection.track(kSession);
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));

    server.push_stream(kSession, EventType::TurnStarted, "c1:s:1", true);
    server.push_stream(kSession, EventType::TurnEnded, "c1:s:2", false);
    ASSERT_TRUE(collected.wait_envelopes(2, 3s));
    EXPECT_EQ(collected.envelopes[0].event.type, EventType::TurnStarted);
    EXPECT_EQ(connection.cursor(kSession)->value, "c1:s:2");

    std::atomic<int> replies{0};
    connection.submit(std::string(protocol::method::kSessionCreate), nlohmann::json::object(),
                      [&replies](SupervisorReply reply) {
                          EXPECT_TRUE(reply.ok);
                          ++replies;
                      });
    ASSERT_TRUE(connection.waitUntil([&replies] { return replies.load() == 1; }, 3s));

    connection.stop();
    EXPECT_EQ(connection.state(), SupervisorLinkState::Detached);
}

TEST(SupervisorConnection, ReconnectReSubscribesFromCursor) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnection connection(config_for(socket_path), sink_for(collected));
    connection.track(kSession);
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));
    ASSERT_TRUE(connection.waitUntil([&server] { return !server.subscribes().empty(); }, 3s));
    EXPECT_EQ(server.subscribes().front().from.kind, protocol::StreamFrom::Kind::Beginning);

    server.push_stream(kSession, EventType::TurnStarted, "c1:s:1", true);
    ASSERT_TRUE(collected.wait_envelopes(1, 3s));
    ASSERT_TRUE(connection.cursor(kSession).has_value());

    server.drop_client();
    ASSERT_TRUE(connection.waitUntil([&connection] { return connection.attachCount() >= 2; }, 4s));

    bool saw_cursor_resume = false;
    const auto subscribes = server.subscribes();
    for (const protocol::SubscribeParams& params : subscribes) {
        if (params.from.kind == protocol::StreamFrom::Kind::Cursor && params.from.cursor.has_value() &&
            params.from.cursor->value == "c1:s:1") {
            saw_cursor_resume = true;
        }
    }
    EXPECT_TRUE(saw_cursor_resume) << "reconnect must resume from the last cursor";
    EXPECT_EQ(server.hello_count(), 2);

    connection.stop();
}

TEST(SupervisorConnection, UnknownWireEventTypeIsSkippedAndCursorAdvances) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnection connection(config_for(socket_path), sink_for(collected));
    connection.track(kSession);
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));

    server.push_unknown_stream(kSession, "future/unknown_event", "c9:s:9");

    ASSERT_TRUE(connection.waitUntil(
        [&connection] {
            const auto cursor = connection.cursor(kSession);
            return cursor.has_value() && cursor->value == "c9:s:9";
        },
        3s));
    EXPECT_TRUE(collected.envelopes.empty());

    server.drop_client();
    ASSERT_TRUE(connection.waitUntil([&connection] { return connection.attachCount() >= 2; }, 4s));

    bool resumed_from_skipped_cursor = false;
    for (const protocol::SubscribeParams& params : server.subscribes()) {
        if (params.from.kind == protocol::StreamFrom::Kind::Cursor &&
            params.from.cursor.has_value() && params.from.cursor->value == "c9:s:9") {
            resumed_from_skipped_cursor = true;
        }
    }
    EXPECT_TRUE(resumed_from_skipped_cursor)
        << "the skipped event must not be replayed on reconnect";

    connection.stop();
}

TEST(SupervisorConnection, CursorInvalidResubscribesFromBeginning) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnection connection(config_for(socket_path), sink_for(collected));
    connection.track(kSession);
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));
    server.push_stream(kSession, EventType::TurnStarted, "c1:s:1", true);
    ASSERT_TRUE(collected.wait_envelopes(1, 3s));
    ASSERT_TRUE(connection.cursor(kSession).has_value());

    server.set_invalid_cursor_once();
    connection.forceReconnect();
    ASSERT_TRUE(connection.waitUntil([&connection] { return connection.attachCount() >= 2; }, 4s));

    bool saw_beginning_after_cursor = false;
    for (const protocol::SubscribeParams& params : server.subscribes()) {
        if (params.from.kind == protocol::StreamFrom::Kind::Beginning) {
            saw_beginning_after_cursor = true;
        }
    }
    EXPECT_TRUE(saw_beginning_after_cursor)
        << "CursorInvalid must fall back to `beginning`, never `now`";
    for (const protocol::SubscribeParams& params : server.subscribes()) {
        EXPECT_NE(params.from.kind, protocol::StreamFrom::Kind::Now);
    }

    connection.stop();
}

TEST(SupervisorConnection, PingTimerKeepsLinkAlive) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnection connection(config_for(socket_path, 30ms), sink_for(collected));
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));
    ASSERT_TRUE(connection.waitUntil([&server] { return server.ping_count() >= 2; }, 3s));
    EXPECT_TRUE(connection.attached());
    connection.stop();
}

TEST(SupervisorConnection, RejectsAttachIdentityMismatch) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnectionConfig config = config_for(socket_path);
    config.expected_boot_id = "99999999-9999-4999-8999-999999999999";
    SupervisorConnection connection(std::move(config), sink_for(collected));
    connection.start();
    EXPECT_FALSE(connection.waitForState(SupervisorLinkState::Attached, 500ms));
    EXPECT_EQ(connection.state(), SupervisorLinkState::Dead);
    connection.stop();
}

TEST(SupervisorConnection, PermissionRoundTripFirstWins) {
    test::ShortTempRoot root("ymh_sup");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    server.set_reject_second_decide(true);

    Collected first_collected;
    Collected second_collected;
    SupervisorConnection first(config_for(socket_path), sink_for(first_collected));
    SupervisorConnectionConfig second_config = config_for(socket_path);
    second_config.client_instance =
        protocol::ClientInstanceId{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"};
    SupervisorConnection second(std::move(second_config), sink_for(second_collected));
    first.start();
    second.start();
    ASSERT_TRUE(first.waitForState(SupervisorLinkState::Attached, 3s));
    ASSERT_TRUE(second.waitForState(SupervisorLinkState::Attached, 3s));

    protocol::PermissionRequest request;
    request.request_id = "req-1";
    request.session = kSession;
    request.tool = "shell";
    request.summary = "run a command";
    server.push_permission(request);
    ASSERT_TRUE(first_collected.wait_permissions(1, 3s));
    ASSERT_TRUE(second_collected.wait_permissions(1, 3s));
    EXPECT_EQ(first_collected.permissions.front().request_id, "req-1");

    std::atomic<int> first_code{-1};
    std::atomic<int> second_code{-1};
    first.submit(std::string(protocol::method::kPermissionDecide), {{"request_id", "req-1"}},
                 [&first_code](SupervisorReply reply) {
                     first_code.store(reply.ok ? 0 : reply.error_code);
                 });
    ASSERT_TRUE(first.waitUntil([&first_code] { return first_code.load() == 0; }, 3s));
    second.submit(std::string(protocol::method::kPermissionDecide), {{"request_id", "req-1"}},
                  [&second_code](SupervisorReply reply) {
                      second_code.store(reply.ok ? 0 : reply.error_code);
                  });
    ASSERT_TRUE(second.waitUntil(
        [&second_code] { return second_code.load() == static_cast<int>(protocol::RpcCode::InvalidParams); },
        3s));

    first.stop();
    second.stop();
}

TEST(SupervisorConnection, SendsPinnedIdentityAndRoleAtHello) {
    test::ShortTempRoot root("ymh_sup_identity");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnectionConfig config = config_for(socket_path);
    config.client_instance = protocol::ClientInstanceId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
    config.role = protocol::ClientRole::Supervisor;
    config.profile = protocol::profile_for_role(config.role);

    SupervisorConnection connection(std::move(config), sink_for(collected));
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Attached, 3s));

    const std::optional<protocol::HelloParams> hello = server.last_hello();
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->client_instance.value, "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    EXPECT_EQ(hello->role, protocol::ClientRole::Supervisor);
    EXPECT_EQ(hello->profile, protocol::ServerProfile::Interactive);
    connection.stop();
}

TEST(SupervisorConnection, RejectsRoleProfileMismatchBeforeHello) {
    test::ShortTempRoot root("ymh_sup_mismatch");
    const std::filesystem::path socket_path = root.host_socket();
    std::filesystem::create_directories(socket_path.parent_path());
    ScriptedServer server(socket_path);
    Collected collected;

    SupervisorConnectionConfig config = config_for(socket_path);
    config.role = protocol::ClientRole::Supervisor;
    config.profile = protocol::ServerProfile::Automation;

    SupervisorConnection connection(std::move(config), sink_for(collected));
    connection.start();
    ASSERT_TRUE(connection.waitForState(SupervisorLinkState::Dead, 3s));
    EXPECT_EQ(server.hello_count(), 0);
    connection.stop();
}

} // namespace

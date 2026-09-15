#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "support/fake_transport_host.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol_server.hpp"
#include "ymh/transport/transport_server.hpp"

namespace {

using namespace ymh;

class TempSocketDir {
public:
    TempSocketDir() {
        static std::uint64_t counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("ymh_transport_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }

    ~TempSocketDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempSocketDir(const TempSocketDir&) = delete;
    TempSocketDir& operator=(const TempSocketDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

protocol::ProtocolServerConfig config_for(const test::FakeTransportHost& host) {
    protocol::ProtocolServerConfig config;
    config.uid = static_cast<std::uint32_t>(::getuid());
    config.workspace = host.workspace;
    config.boot_id = host.boot_id;
    config.pid = host.pid;
    return config;
}

void bind_without_listening(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ::close(fd);
}

std::optional<ino_t> inode_of(const std::string& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        return std::nullopt;
    }
    return status.st_ino;
}

TEST(TransportSocket, HandshakeRequestAndReplay) {
    test::FakeTransportHost host;
    const SessionId session = host.seed("s1");
    host.append(session, EventType::TurnStarted, {{"n", 1}});
    host.append(session, EventType::TurnEnded, {{"n", 2}});

    protocol::ProtocolServerConfig config;
    config.uid = static_cast<std::uint32_t>(::getuid());
    config.workspace = host.workspace;
    config.boot_id = host.boot_id;
    config.pid = host.pid;
    protocol::ProtocolServer engine(host, config);

    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportServer socket_server(engine, path);
    socket_server.start();

    protocol::HostConnection connection;
    connection.connect(path);
    const auto hello = connection.handshake(
        protocol::ServerProfile::Interactive,
        protocol::ClientInstanceId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"});
    EXPECT_EQ(hello.client_id.value, 1u);
    EXPECT_EQ(hello.workspace.value, host.workspace.value);
    EXPECT_TRUE(connection.isConnected());

    const auto pong = connection.request(protocol::method::kHostPing);
    EXPECT_TRUE(pong.contains("server_time_ms"));

    nlohmann::json now_params;
    protocol::to_json(now_params, protocol::SubscribeParams{session, protocol::StreamFrom{}});
    const auto now_result = connection.request(protocol::method::kEventSubscribe, now_params);
    EXPECT_EQ(now_result.at("cursor").get<std::string>(), "c1:s1:2");

    protocol::StreamFrom beginning;
    beginning.kind = protocol::StreamFrom::Kind::Beginning;
    nlohmann::json begin_params;
    protocol::to_json(begin_params, protocol::SubscribeParams{session, beginning});
    const auto begin_result = connection.request(protocol::method::kEventSubscribe, begin_params);
    EXPECT_TRUE(begin_result.contains("subscription"));

    const auto first = connection.nextNotification(std::chrono::seconds{2});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->method, "event.stream");
    const auto first_stream = first->params.get<protocol::StreamNotification>();
    EXPECT_TRUE(first_stream.replay);
    EXPECT_EQ(first_stream.cursor.value, "c1:s1:1");
    EXPECT_EQ(first_stream.envelope.event.type, EventType::TurnStarted);

    const auto second = connection.nextNotification(std::chrono::seconds{2});
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->params.get<protocol::StreamNotification>().cursor.value, "c1:s1:2");

    connection.detach();
    EXPECT_FALSE(connection.isConnected());
    socket_server.stop();
}

TEST(TransportSocket, DetachLeavesDaemonServing) {
    test::FakeTransportHost host;
    host.seed("s1");
    protocol::ProtocolServerConfig config;
    config.uid = static_cast<std::uint32_t>(::getuid());
    config.workspace = host.workspace;
    config.boot_id = host.boot_id;
    protocol::ProtocolServer engine(host, config);

    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportServer socket_server(engine, path);
    socket_server.start();

    {
        protocol::HostConnection connection;
        connection.connect(path);
        static_cast<void>(connection.handshake(
            protocol::ServerProfile::Automation,
            protocol::ClientInstanceId{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"}));
        connection.detach();
    }
    EXPECT_EQ(std::count(host.calls.begin(), host.calls.end(), "session.delete"), 0);

    protocol::HostConnection second;
    second.connect(path);
    const auto hello = second.handshake(
        protocol::ServerProfile::Interactive,
        protocol::ClientInstanceId{"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"});
    EXPECT_GT(hello.client_id.value, 1u);
    second.close();
    socket_server.stop();
}

TEST(TransportSocket, CurrentUidMatchesProcessUid) {
    EXPECT_EQ(protocol::current_uid(), static_cast<std::uint32_t>(::getuid()));
}

TEST(TransportSocket, PostMarshalsOntoIoThreadAndTracksRunning) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportServer socket_server(engine, path);

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<int> runs{0};
    EXPECT_FALSE(socket_server.running());
    EXPECT_FALSE(socket_server.post([&runs] { ++runs; }));

    socket_server.start();
    EXPECT_TRUE(socket_server.running());

    std::promise<std::thread::id> promise;
    std::future<std::thread::id> future = promise.get_future();
    ASSERT_TRUE(socket_server.post([&runs, &promise] {
        ++runs;
        promise.set_value(std::this_thread::get_id());
    }));
    ASSERT_EQ(future.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_NE(future.get(), caller);
    EXPECT_EQ(runs.load(), 1);

    asio::steady_timer timer(socket_server.io());
    timer.expires_after(std::chrono::milliseconds{1});
    EXPECT_TRUE(socket_server.post([&timer] { timer.cancel(); }));

    socket_server.stop();
    EXPECT_FALSE(socket_server.running());
    EXPECT_FALSE(socket_server.post([&runs] { ++runs; }));
    EXPECT_EQ(runs.load(), 1);
}

TEST(TransportSocket, StopUnlinksOwnSocket) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportServer socket_server(engine, path);
    socket_server.start();
    ASSERT_TRUE(std::filesystem::exists(path));
    socket_server.stop();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(TransportSocket, StartRejectsLiveServerWithAlreadyRunning) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportServer first(engine, path);
    first.start();

    protocol::TransportServer second(engine, path);
    bool threw = false;
    try {
        second.start();
    } catch (const protocol::TransportError& error) {
        threw = true;
        EXPECT_EQ(error.code(), protocol::HostErrorCode::AlreadyRunning);
    }
    EXPECT_TRUE(threw);
    first.stop();
}

TEST(TransportSocket, StartUnlinksStaleSocketAfterRefusedProbe) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    bind_without_listening(path);
    ASSERT_TRUE(std::filesystem::exists(path));

    protocol::TransportServer socket_server(engine, path);
    socket_server.start();
    EXPECT_TRUE(socket_server.running());
    socket_server.stop();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(TransportSocket, StopDoesNotUnlinkASuccessorsSocket) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, config_for(host));
    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportServer socket_server(engine, path);
    socket_server.start();

    ASSERT_EQ(::unlink(path.c_str()), 0);
    const int successor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(successor, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(::bind(successor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(successor, 1), 0);
    const std::optional<ino_t> successor_inode = inode_of(path);
    ASSERT_TRUE(successor_inode.has_value());

    socket_server.stop();
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(inode_of(path), successor_inode);
    ::close(successor);
    ::unlink(path.c_str());
}

TEST(TransportSocket, PingKeepsIdleConnectionAlive) {
    test::FakeTransportHost host;
    host.seed("s1");
    protocol::ProtocolServer engine(host, config_for(host));
    TempSocketDir directory;
    const std::string path = (directory.path() / "host.sock").string();
    protocol::TransportLimits limits;
    limits.handshake_timeout = std::chrono::seconds{2};
    limits.idle_timeout = std::chrono::milliseconds{300};
    protocol::TransportServer socket_server(engine, path, limits);
    socket_server.start();

    protocol::HostConnection connection;
    connection.connect(path);
    static_cast<void>(connection.handshake(
        protocol::ServerProfile::Automation,
        protocol::ClientInstanceId{"ffffffff-ffff-4fff-8fff-ffffffffffff"}));

    const auto began = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds{700}) {
        const auto pong = connection.request(protocol::method::kHostPing);
        EXPECT_TRUE(pong.contains("server_time_ms"));
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    EXPECT_TRUE(connection.isConnected());

    std::this_thread::sleep_for(std::chrono::milliseconds{600});
    EXPECT_THROW(
        { static_cast<void>(connection.request(protocol::method::kHostPing)); }, std::exception);
    socket_server.stop();
}

} // namespace

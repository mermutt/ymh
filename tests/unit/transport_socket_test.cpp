#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

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

} // namespace

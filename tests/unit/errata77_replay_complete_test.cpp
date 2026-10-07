// 77 §10.3: the additive `HostNoticeKind::ReplayComplete` wire notice (77-D4a /
// OL13) and its enum mapping (OL9), driven through the real `ProtocolServer`.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/fake_transport_host.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/transport/protocol_server.hpp"

namespace {

using namespace ymh;

constexpr std::string_view kInstanceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";

struct Peer {
    protocol::ClientId id;
    std::vector<std::string> frames;
    std::optional<std::string> drop_reason;
    bool ack{true};
};

class Harness {
public:
    Harness() {
        protocol::ProtocolServerConfig config;
        config.uid = 1000;
        config.workspace = host.workspace;
        config.boot_id = host.boot_id;
        config.pid = host.pid;
        server = std::make_unique<protocol::ProtocolServer>(host, config);
    }

    test::FakeTransportHost host;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::unique_ptr<protocol::ProtocolServer> server;

    Peer* open() {
        peers_.push_back(std::make_unique<Peer>());
        Peer* peer = peers_.back().get();
        protocol::ProtocolServer* engine = server.get();
        peer->id = server->openConnection(
            1000, 4321,
            [peer, engine](protocol::ClientId id, std::string frame) {
                peer->frames.push_back(std::move(frame));
                engine->onFrameWritten(id, peer->frames.back().size());
            },
            [peer](protocol::ClientId, std::string reason) {
                peer->drop_reason = std::move(reason);
            });
        return peer;
    }

    void send(Peer& peer, const nlohmann::json& message) {
        const std::string frame =
            protocol::FrameCodec::encode(message.dump(), protocol::TransportLimits{}.max_frame_bytes);
        server->receiveBytes(peer.id, frame);
    }

    std::vector<nlohmann::json> drain(Peer& peer) {
        std::string buffer;
        for (const std::string& frame : peer.frames) {
            buffer += frame;
        }
        peer.frames.clear();
        std::vector<nlohmann::json> messages;
        for (const std::string& body :
             protocol::FrameCodec::decode(buffer, protocol::TransportLimits{}.max_frame_bytes)) {
            messages.push_back(nlohmann::json::parse(body));
        }
        return messages;
    }

    static nlohmann::json request(std::int64_t id, std::string_view method,
                                  nlohmann::json params = nlohmann::json::object()) {
        return protocol::encode(
            protocol::Request{protocol::RequestId{id}, std::string{method}, std::move(params)});
    }

    void hello(Peer& peer, protocol::ServerProfile profile, std::string_view instance,
               std::int64_t id = 1) {
        nlohmann::json params;
        protocol::to_json(params, protocol::HelloParams{protocol::kProtocolVersion, profile,
                                                        protocol::ClientInstanceId{
                                                            std::string{instance}},
                                                        std::nullopt,
                                                        protocol::ClientRole::Supervisor});
        send(peer, request(id, protocol::method::kHostHello, params));
    }

    static nlohmann::json subscribe_params(const SessionId& session, protocol::StreamFrom from) {
        nlohmann::json params;
        protocol::to_json(params, protocol::SubscribeParams{session, from});
        return params;
    }

    EventRecord emit(const SessionId& session, EventType type, nlohmann::json payload = {}) {
        const EventRecord record = host.append(session, type, std::move(payload));
        server->onEventCommitted(record);
        return record;
    }
};

TEST(Errata77ReplayComplete, OL13InteractiveSubscribeEmitsOneNoticeAfterReplay) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    harness.emit(session, EventType::TurnStarted, {{"n", 1}});
    harness.emit(session, EventType::TurnEnded, {{"n", 2}});

    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);

    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Beginning;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 4u);

    std::vector<nlohmann::json> host_events;
    std::size_t last_stream_index = 0;
    std::size_t first_host_index = frames.size();
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const std::string method = frames[index].value("method", std::string{});
        if (method == "event.stream") {
            last_stream_index = index;
        }
        if (method == "host.event") {
            host_events.push_back(frames[index]);
            first_host_index = std::min(first_host_index, index);
        }
    }
    ASSERT_EQ(host_events.size(), 1u);
    EXPECT_GT(first_host_index, last_stream_index);
    EXPECT_EQ(host_events[0].at("params").at("kind").get<std::string>(), "replay_complete");
    EXPECT_EQ(host_events[0].at("params").at("session").get<std::string>(), "s1");
}

TEST(Errata77ReplayComplete, OL13SessionReplayExcludesTheNotice) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    harness.emit(session, EventType::TurnStarted, {{"n", 1}});

    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);

    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Beginning;
    harness.send(*peer, Harness::request(2, protocol::method::kSessionReplay,
                                         Harness::subscribe_params(session, from)));
    const auto frames = harness.drain(*peer);
    ASSERT_GE(frames.size(), 2u);

    bool saw_unsubscribed = false;
    for (const auto& frame : frames) {
        const std::string method = frame.value("method", std::string{});
        EXPECT_NE(method, "host.event");
        if (method == "event.unsubscribed" &&
            frame.at("params").at("reason").get<std::string>() == "replay_complete") {
            saw_unsubscribed = true;
        }
    }
    EXPECT_TRUE(saw_unsubscribed);
}

TEST(Errata77ReplayComplete, OL9ReplayCompleteStringIsAdditive) {
    const auto replay = protocol::parse_host_notice_kind("replay_complete");
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(*replay, protocol::HostNoticeKind::ReplayComplete);

    EXPECT_EQ(protocol::parse_host_notice_kind("session_closed"),
              protocol::HostNoticeKind::SessionClosed);
    EXPECT_EQ(protocol::parse_host_notice_kind("session_created"),
              protocol::HostNoticeKind::SessionCreated);
    EXPECT_EQ(protocol::parse_host_notice_kind("lease_lost"),
              protocol::HostNoticeKind::LeaseLost);
    EXPECT_EQ(protocol::parse_host_notice_kind("daemon_shutting_down"),
              protocol::HostNoticeKind::DaemonShuttingDown);
    EXPECT_EQ(protocol::parse_host_notice_kind("mcp_server_status"),
              protocol::HostNoticeKind::McpServerStatus);
}

} // namespace

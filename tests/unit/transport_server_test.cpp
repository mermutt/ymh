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
#include "ymh/transport/protocol_server.hpp"

namespace {

using namespace ymh;

constexpr std::string_view kInstanceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr std::string_view kInstanceB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

struct Peer {
    protocol::ClientId id;
    std::vector<std::string> frames;
    std::optional<std::string> drop_reason;
    bool ack{true};
};

class Harness {
public:
    explicit Harness(protocol::TransportLimits limits = {}) : limits_(limits) {
        protocol::ProtocolServerConfig config;
        config.limits = limits_;
        config.uid = 1000;
        config.workspace = host.workspace;
        config.boot_id = host.boot_id;
        config.pid = host.pid;
        server = std::make_unique<protocol::ProtocolServer>(host, config);
    }

    test::FakeTransportHost host;
    protocol::TransportLimits limits_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::unique_ptr<protocol::ProtocolServer> server;

    Peer* open(bool ack = true, std::uint32_t uid = 1000) {
        peers_.push_back(std::make_unique<Peer>());
        Peer* peer = peers_.back().get();
        peer->ack = ack;
        protocol::ProtocolServer* engine = server.get();
        peer->id = server->openConnection(
            uid, 4321,
            [peer, engine](protocol::ClientId id, std::string frame) {
                peer->frames.push_back(std::move(frame));
                if (peer->ack) {
                    engine->onFrameWritten(id, peer->frames.back().size());
                }
            },
            [peer](protocol::ClientId, std::string reason) {
                peer->drop_reason = std::move(reason);
            });
        return peer;
    }

    void send(Peer& peer, const nlohmann::json& message) {
        const std::string frame =
            protocol::FrameCodec::encode(message.dump(), limits_.max_frame_bytes);
        server->receiveBytes(peer.id, frame);
    }

    void send_raw(Peer& peer, std::string_view bytes) { server->receiveBytes(peer.id, bytes); }

    std::vector<nlohmann::json> drain(Peer& peer) {
        std::string buffer;
        for (const std::string& frame : peer.frames) {
            buffer += frame;
        }
        peer.frames.clear();
        std::vector<nlohmann::json> messages;
        for (const std::string& body :
             protocol::FrameCodec::decode(buffer, limits_.max_frame_bytes)) {
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
               std::int64_t id = 1,
               protocol::ClientRole role = protocol::ClientRole::Supervisor) {
        nlohmann::json params;
        protocol::to_json(params, protocol::HelloParams{protocol::kProtocolVersion, profile,
                                                        protocol::ClientInstanceId{
                                                            std::string{instance}},
                                                        std::nullopt, role});
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

int error_code(const nlohmann::json& frame) {
    return frame.at("error").at("code").get<int>();
}

std::shared_ptr<const std::vector<protocol::ClientInstanceId>> snapshot_of(
    std::initializer_list<std::string_view> instances) {
    auto snapshot = std::make_shared<std::vector<protocol::ClientInstanceId>>();
    for (const std::string_view instance : instances) {
        snapshot->push_back(protocol::ClientInstanceId{std::string{instance}});
    }
    return snapshot;
}

TEST(TransportServer, HandshakeAssignsClientId) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    const auto result = frames[0].at("result").get<protocol::HelloResult>();
    EXPECT_EQ(result.client_id.value, peer->id.value);
    EXPECT_EQ(result.workspace.value, harness.host.workspace.value);
    EXPECT_EQ(result.profiles.size(), 2u);
    EXPECT_EQ(result.protocol_version, protocol::kProtocolVersion);
    EXPECT_TRUE(harness.server->isHandshaken(peer->id));
    EXPECT_FALSE(peer->drop_reason.has_value());
}

TEST(TransportServer, OwnerLivenessSinkCountsOnlyOwnerRoles) {
    Harness harness;
    std::size_t                           live = 0;
    std::size_t                           sink_calls = 0;
    std::chrono::steady_clock::time_point last{};
    harness.server->set_owner_liveness_sink(
        [&](std::size_t count, std::chrono::steady_clock::time_point frame) {
            live = count;
            last = frame;
            ++sink_calls;
        });

    Peer* supervisor = harness.open();
    harness.hello(*supervisor, protocol::ServerProfile::Interactive, kInstanceA);
    EXPECT_EQ(live, 1u);
    EXPECT_GT(sink_calls, 0u);
    EXPECT_NE(last, std::chrono::steady_clock::time_point{});

    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation, kInstanceB, 2,
                  protocol::ClientRole::Automation);
    EXPECT_EQ(live, 2u);

    Peer* observer = harness.open();
    harness.hello(*observer, protocol::ServerProfile::Interactive,
                  "cccccccc-cccc-4ccc-8ccc-cccccccccccc", 3, protocol::ClientRole::Observer);
    EXPECT_EQ(live, 2u);

    harness.send(*observer, Harness::request(4, protocol::method::kHostPing));
    EXPECT_EQ(live, 2u);

    harness.server->closeConnection(observer->id);
    EXPECT_EQ(live, 2u);

    harness.server->closeConnection(automation->id);
    EXPECT_EQ(live, 1u);

    harness.server->closeConnection(supervisor->id);
    EXPECT_EQ(live, 0u);
}

TEST(TransportServer, NonHelloFirstIsRejectedAndClosed) {
    Harness harness;
    Peer* peer = harness.open();
    harness.send(*peer, Harness::request(1, protocol::method::kHostPing));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::HandshakeRequired));
    ASSERT_TRUE(peer->drop_reason.has_value());
    EXPECT_EQ(*peer->drop_reason, "closed");
    EXPECT_FALSE(harness.server->hasClient(peer->id));
}

TEST(TransportServer, VersionMismatchIsUnsupportedProtocol) {
    Harness harness;
    Peer* peer = harness.open();
    nlohmann::json params;
    protocol::to_json(params, protocol::HelloParams{99, protocol::ServerProfile::Interactive,
                                                    protocol::ClientInstanceId{
                                                        std::string{kInstanceA}},
                                                    std::nullopt});
    harness.send(*peer, Harness::request(1, protocol::method::kHostHello, params));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::UnsupportedProtocol));
    EXPECT_TRUE(peer->drop_reason.has_value());
}

TEST(TransportServer, InvalidClientInstanceIsRejected) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, "not-a-uuid");
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::InvalidParams));
    EXPECT_TRUE(peer->drop_reason.has_value());
}

TEST(TransportServer, WrongPeerUidIsAuthFailed) {
    Harness harness;
    Peer* peer = harness.open(true, /*uid=*/2000);
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::AuthFailed));
    EXPECT_TRUE(peer->drop_reason.has_value());
}

TEST(TransportServer, DrainingDaemonIsShutdownInProgress) {
    Harness harness;
    harness.host.state = protocol::HostState::Draining;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::ShutdownInProgress));
}

TEST(TransportServer, DuplicateHelloIsInvalidRequest) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA, 2);
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::InvalidRequest));
}

TEST(TransportServer, UnknownMethodIsMethodNotFound) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.send(*peer, Harness::request(2, "host.nope"));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::MethodNotFound));
}

TEST(TransportServer, AutomationCannotCallOperatorControls) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Automation, kInstanceA);
    harness.drain(*peer);
    const SessionId session = harness.host.seed("s1");
    nlohmann::json params{{"session", session.value}};
    harness.send(*peer, Harness::request(2, protocol::method::kSessionActivate, params));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]),
              protocol::code_value(protocol::AppCode::MethodNotAllowedForProfile));
}

TEST(TransportServer, SessionCompactAcksQueuedAndIsInteractiveOnly) {
    Harness harness;
    Peer*   peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    const SessionId session = harness.host.seed("s1");
    nlohmann::json  params{{"session", session.value}};
    harness.send(*peer, Harness::request(2, protocol::method::kSessionCompact, params));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("result").at("outcome").get<std::string>(), "Queued");
    ASSERT_EQ(harness.host.calls.size(), 1u);
    EXPECT_EQ(harness.host.calls[0], "session.compact");
}

TEST(TransportServer, AutomationCannotCallSessionCompact) {
    Harness harness;
    Peer*   peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Automation, kInstanceA);
    harness.drain(*peer);
    const SessionId session = harness.host.seed("s1");
    nlohmann::json  params{{"session", session.value}};
    harness.send(*peer, Harness::request(2, protocol::method::kSessionCompact, params));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]),
              protocol::code_value(protocol::AppCode::MethodNotAllowedForProfile));
    EXPECT_TRUE(harness.host.calls.empty());
}

TEST(TransportServer, PingAndStatus) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);

    harness.send(*peer, Harness::request(2, protocol::method::kHostPing));
    auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_TRUE(frames[0].at("result").contains("server_time_ms"));

    harness.send(*peer, Harness::request(3, protocol::method::kHostStatus));
    frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    const auto status = frames[0].at("result").get<protocol::HostStatus>();
    EXPECT_EQ(status.state, protocol::HostState::Serving);
    EXPECT_EQ(status.attached_clients, 1u);
    EXPECT_EQ(status.profile, protocol::ServerProfile::Interactive);
}

TEST(TransportServer, SessionCreateAndAgentPrompt) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);

    harness.send(*peer, Harness::request(2, protocol::method::kSessionCreate,
                                         nlohmann::json{{"title", "demo"}}));
    auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 2u);
    const std::string session = frames[0].at("result").at("session").get<std::string>();
    EXPECT_TRUE(frames[0].at("result").contains("header"));
    EXPECT_EQ(frames[1].at("method").get<std::string>(), "host.event");
    EXPECT_EQ(frames[1].at("params").at("kind").get<std::string>(), "session_created");
    EXPECT_EQ(frames[1].at("params").at("session").get<std::string>(), session);

    harness.send(*peer, Harness::request(3, protocol::method::kAgentPrompt,
                                         nlohmann::json{{"session", session},
                                                        {"message", {{"text", "hi"}}}}));
    frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_TRUE(frames[0].at("result").at("accepted").get<bool>());
    ASSERT_TRUE(harness.host.last_message.has_value());
    EXPECT_EQ(harness.host.last_message->at("text").get<std::string>(), "hi");
}

TEST(TransportServer, SessionRenameReturnsEchoAndRecordsCall) {
    Harness harness;
    Peer*   peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    const SessionId session = harness.host.seed("s1");

    harness.send(*peer, Harness::request(2, protocol::method::kSessionRename,
                                         nlohmann::json{{"session", session.value},
                                                        {"title", "fix the flaky PTY test"}}));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("result").at("session").get<std::string>(), session.value);
    EXPECT_EQ(frames[0].at("result").at("title").get<std::string>(), "fix the flaky PTY test");
    ASSERT_EQ(harness.host.calls.size(), 1u);
    EXPECT_EQ(harness.host.calls[0], "session.rename");
    ASSERT_EQ(harness.host.listSessions().size(), 1u);
    EXPECT_EQ(harness.host.listSessions()[0].title, "fix the flaky PTY test");
}

TEST(TransportServer, SessionRenameUnknownSessionIsTypedError) {
    Harness harness;
    Peer*   peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.send(*peer, Harness::request(2, protocol::method::kSessionRename,
                                         nlohmann::json{{"session", "missing"},
                                                        {"title", "nope"}}));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]),
              protocol::code_value(protocol::AppCode::UnknownSession));
}

TEST(TransportServer, SessionRenameIsAllowedForAutomation) {
    Harness harness;
    Peer*   peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Automation, kInstanceA);
    harness.drain(*peer);
    const SessionId session = harness.host.seed("s1");
    harness.send(*peer, Harness::request(2, protocol::method::kSessionRename,
                                         nlohmann::json{{"session", session.value},
                                                        {"title", "auto ok"}}));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FALSE(frames[0].contains("error"));
    EXPECT_EQ(frames[0].at("result").at("title").get<std::string>(), "auto ok");
}

TEST(TransportServer, DeleteRequiresConfirm) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    const SessionId session = harness.host.seed("s1");
    harness.send(*peer, Harness::request(2, protocol::method::kSessionDelete,
                                         nlohmann::json{{"session", session.value}}));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::InvalidParams));
}

TEST(TransportServer, PermissionDecideUnknownRequest) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.send(*peer,
                 Harness::request(2, protocol::method::kPermissionDecide,
                                  nlohmann::json{{"request_id", "missing"},
                                                 {"decision", "allow"},
                                                 {"scope", "once"}}));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::InvalidParams));
}

TEST(TransportServer, DuplicateInFlightIdInOneBatch) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    std::string batch =
        protocol::FrameCodec::encode(Harness::request(5, protocol::method::kHostPing).dump(),
                                     harness.limits_.max_frame_bytes);
    batch += protocol::FrameCodec::encode(
        Harness::request(5, protocol::method::kHostPing).dump(), harness.limits_.max_frame_bytes);
    harness.send_raw(*peer, batch);
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_TRUE(frames[0].contains("result"));
    EXPECT_EQ(error_code(frames[1]), protocol::code_value(protocol::RpcCode::InvalidRequest));
}

TEST(TransportServer, SubscribeBeginningReplaysThenGoesLive) {
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
    auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_TRUE(frames[0].contains("result"));
    const auto result = frames[0].at("result").get<protocol::SubscribeResult>();
    EXPECT_EQ(result.cursor.value, "c1:s1:2");
    EXPECT_EQ(frames[1].at("method").get<std::string>(), "event.stream");
    EXPECT_TRUE(frames[1].at("params").at("replay").get<bool>());
    EXPECT_EQ(frames[1].at("params").at("cursor").get<std::string>(), "c1:s1:1");
    EXPECT_TRUE(frames[2].at("params").at("replay").get<bool>());
    EXPECT_EQ(frames[2].at("params").at("cursor").get<std::string>(), "c1:s1:2");

    harness.emit(session, EventType::AssistantMessage, {{"n", 3}});
    const auto live = harness.drain(*peer);
    ASSERT_EQ(live.size(), 1u);
    EXPECT_FALSE(live[0].at("params").at("replay").get<bool>());
    EXPECT_EQ(live[0].at("params").at("cursor").get<std::string>(), "c1:s1:3");
    EXPECT_EQ(live[0].at("params").at("envelope").at("session").get<std::string>(), "s1");
}

TEST(TransportServer, SubscribeNowReplaysNothing) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    harness.emit(session, EventType::TurnStarted);
    harness.emit(session, EventType::TurnEnded);

    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_TRUE(frames[0].contains("result"));

    harness.emit(session, EventType::AssistantMessage);
    frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("params").at("cursor").get<std::string>(), "c1:s1:3");
}

TEST(TransportServer, CursorResumeIsNoLossNoDuplicate) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    for (int index = 1; index <= 3; ++index) {
        harness.emit(session, EventType::AssistantChunk, {{"n", index}});
    }
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Beginning;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 4u);
    const std::string cursor3 = frames[3].at("params").at("cursor").get<std::string>();
    EXPECT_EQ(cursor3, "c1:s1:3");

    harness.emit(session, EventType::AssistantChunk, {{"n", 4}});
    harness.emit(session, EventType::AssistantChunk, {{"n", 5}});
    harness.server->closeConnection(peer->id);

    Peer* resumed_peer = harness.open();
    harness.hello(*resumed_peer, protocol::ServerProfile::Interactive, kInstanceA, 3);
    harness.drain(*resumed_peer);
    protocol::StreamFrom resume;
    resume.kind = protocol::StreamFrom::Kind::Cursor;
    resume.cursor = protocol::EventCursor{cursor3};
    harness.send(*resumed_peer, Harness::request(4, protocol::method::kEventSubscribe,
                                                 Harness::subscribe_params(session, resume)));
    const auto resumed = harness.drain(*resumed_peer);
    ASSERT_EQ(resumed.size(), 3u);
    EXPECT_TRUE(resumed[0].contains("result"));
    EXPECT_EQ(resumed[1].at("params").at("cursor").get<std::string>(), "c1:s1:4");
    EXPECT_EQ(resumed[2].at("params").at("cursor").get<std::string>(), "c1:s1:5");

    harness.emit(session, EventType::AssistantChunk, {{"n", 6}});
    const auto live = harness.drain(*resumed_peer);
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0].at("params").at("cursor").get<std::string>(), "c1:s1:6");
}

TEST(TransportServer, InvalidCursorIsCursorInvalid) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    harness.emit(session, EventType::TurnStarted);
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Cursor;
    from.cursor = protocol::EventCursor{"c1:other:1"};
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::CursorInvalid));
}

TEST(TransportServer, UnknownSessionSubscribeIsUnknownSession) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(SessionId{"nope"}, from)));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::UnknownSession));
}

TEST(TransportServer, BackpressureDropsClientNotSession) {
    protocol::TransportLimits limits;
    limits.max_outbound_bytes = 4096;
    Harness harness(limits);
    const SessionId session = harness.host.seed("s1");

    Peer* slow = harness.open(/*ack=*/false);
    harness.hello(*slow, protocol::ServerProfile::Interactive, kInstanceA);
    Peer* fast = harness.open();
    harness.hello(*fast, protocol::ServerProfile::Interactive, kInstanceB);
    harness.drain(*fast);

    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*slow, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    harness.send(*fast, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));

    for (int index = 0; index < 6; ++index) {
        harness.emit(session, EventType::AssistantChunk, {{"text", std::string(1000, 'x')}});
    }
    ASSERT_TRUE(slow->drop_reason.has_value());
    EXPECT_EQ(*slow->drop_reason, "outbound_queue_overflow");
    EXPECT_FALSE(harness.server->hasClient(slow->id));
    EXPECT_TRUE(harness.server->hasClient(fast->id));

    const auto delivered = harness.drain(*fast);
    EXPECT_EQ(delivered.size(), 7u);
}

TEST(TransportServer, ReconnectSupersedesSameInstance) {
    Harness harness;
    Peer* first = harness.open();
    harness.hello(*first, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*first);
    Peer* second = harness.open();
    harness.hello(*second, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*second);
    ASSERT_TRUE(first->drop_reason.has_value());
    EXPECT_EQ(*first->drop_reason, "superseded");
    EXPECT_FALSE(harness.server->hasClient(first->id));
    EXPECT_TRUE(harness.server->isHandshaken(second->id));
}

TEST(TransportServer, PermissionRequestOnlyToInteractiveSubscribers) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* interactive = harness.open();
    harness.hello(*interactive, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*interactive);
    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation, kInstanceB);
    harness.drain(*automation);

    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*interactive, Harness::request(2, protocol::method::kEventSubscribe,
                                                Harness::subscribe_params(session, from)));
    harness.send(*automation, Harness::request(2, protocol::method::kEventSubscribe,
                                               Harness::subscribe_params(session, from)));
    harness.drain(*interactive);
    harness.drain(*automation);

    protocol::PermissionRequest request;
    request.request_id = "req-1";
    request.session = session;
    request.tool = "shell";
    request.expires_at_ms = 1;
    harness.server->onPermissionRequest(request);

    const auto to_interactive = harness.drain(*interactive);
    const auto to_automation = harness.drain(*automation);
    ASSERT_EQ(to_interactive.size(), 1u);
    EXPECT_EQ(to_interactive[0].at("method").get<std::string>(), "permission.request");
    EXPECT_TRUE(to_automation.empty());
}

TEST(TransportServer, DeleteSignalsUnsubscribedAndHostNotice) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    harness.drain(*peer);

    harness.emit(session, EventType::SessionEnded, {{"reason", "Deleted"}});
    harness.send(*peer, Harness::request(3, protocol::method::kSessionDelete,
                                         nlohmann::json{{"session", session.value},
                                                        {"confirm", true}}));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 4u);
    EXPECT_EQ(frames[0].at("method").get<std::string>(), "event.stream");
    EXPECT_EQ(frames[0].at("params").at("envelope").at("event").at("type").get<std::string>(),
              "session/end");
    EXPECT_TRUE(frames[1].contains("result"));
    EXPECT_EQ(frames[2].at("method").get<std::string>(), "event.unsubscribed");
    EXPECT_EQ(frames[2].at("params").at("reason").get<std::string>(), "session_closed");
    EXPECT_EQ(frames[3].at("method").get<std::string>(), "host.event");
    EXPECT_EQ(frames[3].at("params").at("kind").get<std::string>(), "session_closed");
}

TEST(TransportServer, UnsubscribeEndsStream) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    auto frames = harness.drain(*peer);
    const auto subscription = frames[0].at("result").at("subscription").get<std::uint64_t>();

    harness.send(*peer, Harness::request(3, protocol::method::kEventUnsubscribe,
                                         nlohmann::json{{"subscription", subscription}}));
    frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_TRUE(frames[0].contains("result"));
    EXPECT_EQ(frames[1].at("method").get<std::string>(), "event.unsubscribed");
    EXPECT_EQ(frames[1].at("params").at("reason").get<std::string>(), "unsubscribed");
    EXPECT_EQ(harness.server->subscriptionCount(peer->id), 0u);
}

TEST(TransportServer, DetachRepliesThenCloses) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.send(*peer, Harness::request(2, protocol::method::kHostDetach));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_TRUE(frames[0].contains("result"));
    ASSERT_TRUE(peer->drop_reason.has_value());
    EXPECT_EQ(*peer->drop_reason, "closed");
}

TEST(TransportServer, LeaseLostNoticeReachesInteractiveSubscribers) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom from;
    from.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, from)));
    harness.drain(*peer);
    harness.server->onLeaseLost(session, "read-only");
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("method").get<std::string>(), "host.event");
    EXPECT_EQ(frames[0].at("params").at("kind").get<std::string>(), "lease_lost");
}

TEST(TransportServer, SessionSubscriberCountTracksInteractiveSubscriptions) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    const SessionId other = harness.host.seed("s2");
    Peer* interactive = harness.open();
    harness.hello(*interactive, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*interactive);
    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation, kInstanceB);
    harness.drain(*automation);

    EXPECT_EQ(harness.server->sessionSubscriberCount(session), 0u);

    protocol::StreamFrom now;
    now.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*interactive, Harness::request(2, protocol::method::kEventSubscribe,
                                                Harness::subscribe_params(session, now)));
    harness.drain(*interactive);
    EXPECT_EQ(harness.server->sessionSubscriberCount(session), 1u);
    EXPECT_EQ(harness.server->sessionSubscriberCount(other), 0u);

    harness.send(*automation, Harness::request(2, protocol::method::kEventSubscribe,
                                               Harness::subscribe_params(session, now)));
    harness.drain(*automation);
    EXPECT_EQ(harness.server->sessionSubscriberCount(session), 1u);

    Peer* second = harness.open();
    harness.hello(*second, protocol::ServerProfile::Interactive, kInstanceB);
    harness.drain(*second);
    harness.send(*second, Harness::request(2, protocol::method::kEventSubscribe,
                                           Harness::subscribe_params(session, now)));
    harness.drain(*second);
    EXPECT_EQ(harness.server->sessionSubscriberCount(session), 2u);

    harness.server->onSessionClosed(session, "closed");
    EXPECT_EQ(harness.server->sessionSubscriberCount(session), 0u);
}

TEST(TransportServer, SubscribeObserverFiresAfterInstallForInteractiveOnly) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    std::vector<std::pair<SessionId, protocol::ClientId>> observed;
    harness.server->set_subscribe_observer(
        [&](const SessionId& subscribed, protocol::ClientId client) {
            observed.emplace_back(subscribed, client);
            EXPECT_GE(harness.server->sessionSubscriberCount(subscribed), 1u);
        });

    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    protocol::StreamFrom now;
    now.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, now)));
    harness.drain(*peer);
    ASSERT_EQ(observed.size(), 1u);
    EXPECT_EQ(observed[0].first.value, session.value);
    EXPECT_EQ(observed[0].second.value, peer->id.value);

    harness.send(*peer, Harness::request(3, protocol::method::kSessionReplay,
                                         Harness::subscribe_params(session, now)));
    harness.drain(*peer);
    EXPECT_EQ(observed.size(), 1u);

    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation, kInstanceB);
    harness.drain(*automation);
    harness.send(*automation, Harness::request(2, protocol::method::kEventSubscribe,
                                               Harness::subscribe_params(session, now)));
    harness.drain(*automation);
    EXPECT_EQ(observed.size(), 1u);

    harness.send(*peer, Harness::request(4, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, now)));
    harness.drain(*peer);
    EXPECT_EQ(observed.size(), 2u);
}

TEST(TransportServer, WaitForDrainTrueWhenNoOutboundIsPending) {
    Harness harness;
    EXPECT_TRUE(harness.server->waitForDrain(std::chrono::milliseconds{10}));
}

TEST(TransportServer, WaitForDrainBlocksUntilOutboundAcked) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* peer = harness.open(/*ack=*/false);
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    protocol::StreamFrom now;
    now.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, now)));
    harness.emit(session, EventType::TurnStarted, {{"n", 1}});
    ASSERT_GT(harness.server->outstandingBytes(peer->id), 0u);

    EXPECT_FALSE(harness.server->waitForDrain(std::chrono::milliseconds{20}));

    harness.server->onFrameWritten(peer->id, harness.server->outstandingBytes(peer->id));
    EXPECT_TRUE(harness.server->waitForDrain(std::chrono::milliseconds{20}));
}

TEST(TransportServer, CloseConnectionReleasesOutstandingBytes) {
    Harness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* peer = harness.open(/*ack=*/false);
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    protocol::StreamFrom now;
    now.kind = protocol::StreamFrom::Kind::Now;
    harness.send(*peer, Harness::request(2, protocol::method::kEventSubscribe,
                                         Harness::subscribe_params(session, now)));
    harness.emit(session, EventType::TurnStarted, {{"n", 1}});
    ASSERT_GT(harness.server->outstandingBytes(peer->id), 0u);
    EXPECT_FALSE(harness.server->waitForDrain(std::chrono::milliseconds{10}));

    harness.server->closeConnection(peer->id);
    EXPECT_TRUE(harness.server->waitForDrain(std::chrono::milliseconds{10}));
}

TEST(TransportServer, DaemonShuttingDownNoticeIsEmittedOnce) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.server->onDaemonShuttingDown("bye");
    harness.server->onDaemonShuttingDown("bye again");
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].at("params").at("kind").get<std::string>(), "daemon_shutting_down");
}

TEST(TransportServer, UnexpectedExceptionMapsToInternalError) {
    Harness harness;
    harness.host.throw_internal_on = "session.list";
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.send(*peer, Harness::request(2, protocol::method::kSessionList));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::InternalError));
}

TEST(TransportServer, MalformedParamsMapToInvalidParams) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.send(*peer, Harness::request(2, protocol::method::kPermissionDecide));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::RpcCode::InvalidParams));
}

TEST(TransportServer, HostShutdownRefusedWhenAnotherSupervisorIsLive) {
    Harness harness;
    Peer* first = harness.open();
    harness.hello(*first, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*first);
    Peer* second = harness.open();
    harness.hello(*second, protocol::ServerProfile::Interactive, kInstanceB, 2);
    harness.drain(*second);

    harness.send(*first, Harness::request(3, protocol::method::kHostShutdown,
                                          nlohmann::json{{"reason", "last_supervisor"}}));
    const auto frames = harness.drain(*first);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::NotLastOwner));
    EXPECT_EQ(harness.host.state, protocol::HostState::Serving);
    EXPECT_TRUE(harness.host.calls.empty());
    EXPECT_FALSE(harness.host.last_shutdown_reason.has_value());
    EXPECT_FALSE(harness.server->isDropped(first->id));
}

TEST(TransportServer, HostShutdownRefusedWhenAutomationIsLive) {
    Harness harness;
    Peer* supervisor = harness.open();
    harness.hello(*supervisor, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*supervisor);
    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation, kInstanceB, 2,
                  protocol::ClientRole::Automation);
    harness.drain(*automation);

    harness.send(*supervisor, Harness::request(3, protocol::method::kHostShutdown,
                                               nlohmann::json{{"reason", "last_supervisor"}}));
    const auto frames = harness.drain(*supervisor);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::NotLastOwner));
    EXPECT_EQ(harness.host.state, protocol::HostState::Serving);
}

TEST(TransportServer, HostShutdownRefusedByFreshSnapshotOwnerExcludingCaller) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);

    harness.host.owner_snapshot = snapshot_of({kInstanceA, kInstanceB});
    harness.send(*peer, Harness::request(2, protocol::method::kHostShutdown,
                                         nlohmann::json{{"reason", "last_supervisor"}}));
    auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::NotLastOwner));
    EXPECT_EQ(harness.host.state, protocol::HostState::Serving);

    harness.host.owner_snapshot = snapshot_of({kInstanceA});
    harness.send(*peer, Harness::request(3, protocol::method::kHostShutdown,
                                         nlohmann::json{{"reason", "last_supervisor"}}));
    frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_TRUE(frames[0].contains("result"));
    EXPECT_EQ(frames[1].at("params").at("kind").get<std::string>(), "daemon_shutting_down");
    EXPECT_EQ(harness.host.last_shutdown_reason, protocol::ShutdownReason::LastSupervisor);
}

TEST(TransportServer, HostShutdownWorkspaceStopOverridesOwners) {
    Harness harness;
    Peer* supervisor = harness.open();
    harness.hello(*supervisor, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*supervisor);
    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation, kInstanceB, 2,
                  protocol::ClientRole::Automation);
    harness.drain(*automation);
    harness.host.owner_snapshot = snapshot_of({kInstanceA, kInstanceB});

    harness.send(*supervisor, Harness::request(3, protocol::method::kHostShutdown,
                                               nlohmann::json{{"reason", "workspace_stop"}}));
    const auto frames = harness.drain(*supervisor);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_TRUE(frames[0].contains("result"));
    EXPECT_EQ(harness.host.last_shutdown_reason, protocol::ShutdownReason::WorkspaceStop);
}

TEST(TransportServer, ObserverShutdownRefusedWhileSupervisorOwns) {
    Harness harness;
    Peer* supervisor = harness.open();
    harness.hello(*supervisor, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*supervisor);
    Peer* observer = harness.open();
    harness.hello(*observer, protocol::ServerProfile::Interactive, kInstanceB, 2,
                  protocol::ClientRole::Observer);
    harness.drain(*observer);

    harness.send(*observer, Harness::request(3, protocol::method::kHostShutdown,
                                             nlohmann::json{{"reason", "last_supervisor"}}));
    const auto frames = harness.drain(*observer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::NotLastOwner));
    EXPECT_EQ(harness.host.state, protocol::HostState::Serving);
    EXPECT_FALSE(harness.host.last_shutdown_reason.has_value());
}

TEST(TransportServer, ObserverShutdownRefusedForUnknownReasonWhileSupervisorOwns) {
    Harness harness;
    Peer* supervisor = harness.open();
    harness.hello(*supervisor, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*supervisor);
    Peer* observer = harness.open();
    harness.hello(*observer, protocol::ServerProfile::Interactive, kInstanceB, 2,
                  protocol::ClientRole::Observer);
    harness.drain(*observer);

    harness.send(*observer, Harness::request(3, protocol::method::kHostShutdown,
                                             nlohmann::json::object()));
    const auto frames = harness.drain(*observer);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(error_code(frames[0]), protocol::code_value(protocol::AppCode::NotLastOwner));
    EXPECT_EQ(harness.host.state, protocol::HostState::Serving);
}

TEST(TransportServer, ObserverWorkspaceStopOverridesOwners) {
    Harness harness;
    Peer* supervisor = harness.open();
    harness.hello(*supervisor, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*supervisor);
    Peer* observer = harness.open();
    harness.hello(*observer, protocol::ServerProfile::Interactive, kInstanceB, 2,
                  protocol::ClientRole::Observer);
    harness.drain(*observer);
    harness.host.owner_snapshot = snapshot_of({kInstanceA});

    harness.send(*observer, Harness::request(3, protocol::method::kHostShutdown,
                                             nlohmann::json{{"reason", "workspace_stop"}}));
    const auto frames = harness.drain(*observer);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_TRUE(frames[0].contains("result"));
    EXPECT_EQ(harness.host.last_shutdown_reason, protocol::ShutdownReason::WorkspaceStop);
}

TEST(TransportServer, HostShutdownReasonMappingDoesNotDegrade) {
    {
        Harness harness;
        Peer* peer = harness.open();
        harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
        harness.drain(*peer);
        harness.send(*peer, Harness::request(2, protocol::method::kHostShutdown,
                                             nlohmann::json{{"reason", "last_supervisor"}}));
        harness.drain(*peer);
        EXPECT_EQ(harness.host.last_shutdown_reason, protocol::ShutdownReason::LastSupervisor);
    }
    {
        Harness harness;
        Peer* peer = harness.open();
        harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
        harness.drain(*peer);
        harness.send(*peer, Harness::request(2, protocol::method::kHostShutdown,
                                             nlohmann::json{{"reason", "workspace_stop"}}));
        harness.drain(*peer);
        EXPECT_EQ(harness.host.last_shutdown_reason, protocol::ShutdownReason::WorkspaceStop);
    }
    {
        Harness harness;
        Peer* peer = harness.open();
        harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
        harness.drain(*peer);
        harness.send(*peer, Harness::request(2, protocol::method::kHostShutdown,
                                             nlohmann::json{{"reason", "not_a_reason"}}));
        harness.drain(*peer);
        EXPECT_EQ(harness.host.last_shutdown_reason, protocol::ShutdownReason::ClientRequest);
    }
}

TEST(TransportServer, HostOwnershipRawCountsIncludeCallerOnlyFreshExcludes) {
    Harness harness;
    Peer* first = harness.open();
    harness.hello(*first, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*first);
    Peer* second = harness.open();
    harness.hello(*second, protocol::ServerProfile::Interactive, kInstanceB, 2);
    harness.drain(*second);
    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation,
                  "cccccccc-cccc-4ccc-8ccc-cccccccccccc", 3, protocol::ClientRole::Automation);
    harness.drain(*automation);
    Peer* observer = harness.open();
    harness.hello(*observer, protocol::ServerProfile::Interactive,
                  "dddddddd-dddd-4ddd-8ddd-dddddddddddd", 4, protocol::ClientRole::Observer);
    harness.drain(*observer);

    harness.host.owner_snapshot =
        snapshot_of({kInstanceA, kInstanceB, "cccccccc-cccc-4ccc-8ccc-cccccccccccc"});

    harness.send(*first,
                 Harness::request(5, protocol::method::kHostOwnership, nlohmann::json::object()));
    const auto frames = harness.drain(*first);
    ASSERT_EQ(frames.size(), 1u);
    const auto view = frames[0].at("result").get<protocol::OwnershipView>();
    EXPECT_EQ(view.live_supervisors, 2u);
    EXPECT_EQ(view.live_automation, 1u);
    EXPECT_EQ(view.other_fresh_owners, 2u);
    EXPECT_EQ(view.clients.size(), 3u);
    EXPECT_FALSE(view.shutting_down);
}

TEST(TransportServer, HostOwnershipSoleCallerIsJustMe) {
    Harness harness;
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*peer);
    harness.host.owner_snapshot = snapshot_of({kInstanceA});

    harness.send(*peer,
                 Harness::request(2, protocol::method::kHostOwnership, nlohmann::json::object()));
    const auto frames = harness.drain(*peer);
    ASSERT_EQ(frames.size(), 1u);
    const auto view = frames[0].at("result").get<protocol::OwnershipView>();
    EXPECT_EQ(view.live_supervisors, 1u);
    EXPECT_EQ(view.live_automation, 0u);
    EXPECT_EQ(view.other_fresh_owners, 0u);
    ASSERT_EQ(view.clients.size(), 1u);
    EXPECT_EQ(view.clients[0].client_instance, kInstanceA);
    EXPECT_EQ(view.clients[0].role, protocol::ClientRole::Supervisor);
}

TEST(TransportServer, RoleOfIsNulloptUntilHandshaken) {
    Harness harness;
    Peer* peer = harness.open();
    EXPECT_FALSE(harness.server->roleOf(peer->id).has_value());
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceA, 1,
                  protocol::ClientRole::Observer);
    harness.drain(*peer);
    ASSERT_TRUE(harness.server->roleOf(peer->id).has_value());
    EXPECT_EQ(*harness.server->roleOf(peer->id), protocol::ClientRole::Observer);
}

TEST(TransportServer, SessionCreatedBroadcastsToAllInteractiveNotSubscriptionGated) {
    Harness harness;
    Peer* creator = harness.open();
    harness.hello(*creator, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*creator);
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceB, 2);
    harness.drain(*peer);
    Peer* automation = harness.open();
    harness.hello(*automation, protocol::ServerProfile::Automation,
                  "cccccccc-cccc-4ccc-8ccc-cccccccccccc", 3, protocol::ClientRole::Automation);
    harness.drain(*automation);

    harness.send(*creator, Harness::request(4, protocol::method::kSessionCreate,
                                            nlohmann::json{{"title", "demo"}}));
    const auto creator_frames = harness.drain(*creator);
    ASSERT_EQ(creator_frames.size(), 2u);
    const std::string session = creator_frames[0].at("result").at("session").get<std::string>();
    EXPECT_EQ(creator_frames[1].at("method").get<std::string>(), "host.event");
    EXPECT_EQ(creator_frames[1].at("params").at("kind").get<std::string>(), "session_created");
    EXPECT_EQ(creator_frames[1].at("params").at("session").get<std::string>(), session);

    const auto peer_frames = harness.drain(*peer);
    ASSERT_EQ(peer_frames.size(), 1u);
    EXPECT_EQ(peer_frames[0].at("method").get<std::string>(), "host.event");
    EXPECT_EQ(peer_frames[0].at("params").at("kind").get<std::string>(), "session_created");
    EXPECT_EQ(peer_frames[0].at("params").at("session").get<std::string>(), session);

    EXPECT_TRUE(harness.drain(*automation).empty());
}

TEST(TransportServer, SessionCreatedBroadcastOnResumeAndFork) {
    Harness harness;
    const SessionId seeded = harness.host.seed("s1");
    Peer* creator = harness.open();
    harness.hello(*creator, protocol::ServerProfile::Interactive, kInstanceA);
    harness.drain(*creator);
    Peer* peer = harness.open();
    harness.hello(*peer, protocol::ServerProfile::Interactive, kInstanceB, 2);
    harness.drain(*peer);

    harness.send(*creator, Harness::request(3, protocol::method::kSessionResume,
                                            nlohmann::json{{"session", seeded.value}}));
    auto frames = harness.drain(*creator);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_TRUE(frames[0].contains("result"));
    EXPECT_EQ(frames[1].at("params").at("kind").get<std::string>(), "session_created");
    EXPECT_EQ(frames[1].at("params").at("session").get<std::string>(), seeded.value);
    auto peer_frames = harness.drain(*peer);
    ASSERT_EQ(peer_frames.size(), 1u);
    EXPECT_EQ(peer_frames[0].at("params").at("kind").get<std::string>(), "session_created");

    harness.send(*creator, Harness::request(4, protocol::method::kSessionFork,
                                            nlohmann::json{{"session", seeded.value},
                                                           {"seed_length", 1}}));
    frames = harness.drain(*creator);
    ASSERT_EQ(frames.size(), 2u);
    const std::string forked = frames[0].at("result").at("session").get<std::string>();
    EXPECT_EQ(frames[1].at("params").at("kind").get<std::string>(), "session_created");
    EXPECT_EQ(frames[1].at("params").at("session").get<std::string>(), forked);
    peer_frames = harness.drain(*peer);
    ASSERT_EQ(peer_frames.size(), 1u);
    EXPECT_EQ(peer_frames[0].at("params").at("session").get<std::string>(), forked);
}

} // namespace

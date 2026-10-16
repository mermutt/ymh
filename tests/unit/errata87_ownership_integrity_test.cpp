// Spec 87 (daemon-ownership-integrity errata): the single boot-checked
// "live daemon" predicate (87-D1), the non-owning transient probe identity
// (87-D2), the daemon-initiated close protocol (87-D3), and the typed transport
// fault taxonomy (87-D4). Openers land in this file; the supervisor-pump loss
// tests and the UI create-notice test land in errata87_supervisor_loss_test.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "support/fake_transport_host.hpp"
#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/transport/protocol_server.hpp"
#include "ymh/transport/transport_server.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/supervisor_presence.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace std::chrono_literals;

constexpr std::string_view kInstanceA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr std::string_view kInstanceB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

// ---------------------------------------------------------------------------
// Part A: the pure predicate and the transient probe identity.
// ---------------------------------------------------------------------------

TEST(Ui87, NamesLiveDaemonPredicate) {
    protocol::HelloResult hello;
    hello.workspace.value = "ws-1";
    hello.boot_id.value = "boot-1";
    const protocol::WorkspaceId workspace{"ws-1"};

    EXPECT_TRUE(protocol::names_live_daemon(hello, workspace, protocol::HostBootId{"boot-1"}));
    EXPECT_FALSE(protocol::names_live_daemon(hello, workspace, protocol::HostBootId{"boot-2"}));
    EXPECT_TRUE(protocol::names_live_daemon(hello, workspace, protocol::HostBootId{}));
    EXPECT_FALSE(protocol::names_live_daemon(hello, protocol::WorkspaceId{"ws-2"},
                                             protocol::HostBootId{"boot-1"}));
    EXPECT_FALSE(
        protocol::names_live_daemon(hello, protocol::WorkspaceId{"ws-2"}, protocol::HostBootId{}));
}

TEST(Presence87, ProbeIdentityIsObserver) {
    const AttachIdentity first = ui::probe_attach_identity();
    const AttachIdentity second = ui::probe_attach_identity();

    EXPECT_EQ(first.role, protocol::ClientRole::Observer);
    EXPECT_NE(first.client_instance.value, ui::process_client_instance().value);
    EXPECT_NE(first.client_instance.value, second.client_instance.value);
}

TEST(Transport87, LinkDroppedWireRoundTrip) {
    EXPECT_EQ(protocol::wire_name(protocol::HostNoticeKind::LinkDropped), "link_dropped");
    EXPECT_EQ(protocol::parse_host_notice_kind("link_dropped"),
              protocol::HostNoticeKind::LinkDropped);

    protocol::HostNotice notice;
    notice.kind = protocol::HostNoticeKind::LinkDropped;
    notice.workspace.value = "ws-1";
    notice.detail = "superseded";
    nlohmann::json json;
    protocol::to_json(json, notice);
    EXPECT_EQ(json.at("kind").get<std::string>(), "link_dropped");
    const protocol::HostNotice round = json.get<protocol::HostNotice>();
    EXPECT_EQ(round.kind, protocol::HostNoticeKind::LinkDropped);
    EXPECT_EQ(round.detail, "superseded");
}

TEST(Protocol87, LinkDroppedOldClientCompat) {
    // 87-F13: the strict `from_json` rejects a kind the parser does not know.
    // `link_dropped` is exactly such a token to a pre-87 parser; this binary
    // knows it (additive, `kProtocolVersion` stays 1), and the benign effect is
    // identical (the daemon is closing anyway).
    const nlohmann::json pre_87_only{{"kind", "some_pre_87_unknown_kind"},
                                     {"workspace", "ws-1"},
                                     {"session", nullptr},
                                     {"detail", "superseded"}};
    EXPECT_THROW(static_cast<void>(pre_87_only.get<protocol::HostNotice>()), std::invalid_argument);

    EXPECT_EQ(protocol::parse_host_notice_kind("link_dropped"),
              protocol::HostNoticeKind::LinkDropped);
    const nlohmann::json known{{"kind", "link_dropped"},
                               {"workspace", "ws-1"},
                               {"session", nullptr},
                               {"detail", "superseded"}};
    EXPECT_NO_THROW(static_cast<void>(known.get<protocol::HostNotice>()));
}

// ---------------------------------------------------------------------------
// Part B: probe_existing_socket against a live in-process daemon.
// ---------------------------------------------------------------------------

protocol::ProtocolServerConfig server_config_for(const test::FakeTransportHost& host) {
    protocol::ProtocolServerConfig config;
    config.uid = static_cast<std::uint32_t>(::getuid());
    config.workspace = host.workspace;
    config.boot_id = host.boot_id;
    config.pid = host.pid;
    return config;
}

TEST(Transport87, ProbeBootMismatchIsResidue) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, server_config_for(host));
    test::ShortTempRoot root("ymh-e87-probe");
    std::filesystem::create_directories(root.host_socket().parent_path());
    protocol::TransportServer server(engine, root.host_socket().string());
    ASSERT_NO_THROW(server.start());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    // Non-empty expected boot that differs from the live daemon => Residue.
    EXPECT_EQ(protocol::probe_existing_socket(root.host_socket().string(), host.workspace,
                                              protocol::HostBootId{"99999999-9999-4999-8999-999999999999"},
                                              deadline),
              protocol::SocketProbeResult::Residue);
    // Matching boot => Live.
    EXPECT_EQ(protocol::probe_existing_socket(root.host_socket().string(), host.workspace,
                                              host.boot_id, deadline),
              protocol::SocketProbeResult::LiveYmhDaemon);
    // Empty expected boot + workspace match => conservative accept (never Residue).
    EXPECT_EQ(protocol::probe_existing_socket(root.host_socket().string(), host.workspace,
                                              protocol::HostBootId{}, deadline),
              protocol::SocketProbeResult::LiveYmhDaemon);

    server.stop();
}

TEST(Transport87, ProbeRefusesLivePeer) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, server_config_for(host));
    test::ShortTempRoot root("ymh-e87-refuse");
    std::filesystem::create_directories(root.host_socket().parent_path());
    protocol::TransportServer server(engine, root.host_socket().string());
    ASSERT_NO_THROW(server.start());

    struct stat before {};
    ASSERT_EQ(::stat(root.host_socket().c_str(), &before), 0);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    EXPECT_EQ(protocol::probe_existing_socket(root.host_socket().string(), host.workspace,
                                              host.boot_id, deadline),
              protocol::SocketProbeResult::LiveYmhDaemon);

    struct stat after {};
    ASSERT_EQ(::stat(root.host_socket().c_str(), &after), 0);
    EXPECT_EQ(before.st_ino, after.st_ino) << "a live peer's socket must never be unlinked";

    server.stop();
}

TEST(Transport76, ProbeSignatureUpdated) {
    // L6: the 3-arg `probe_existing_socket` call was migrated to the 4-arg
    // signature in the same change set; this pins the new call shape.
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, server_config_for(host));
    test::ShortTempRoot root("ymh-e87-sig");
    std::filesystem::create_directories(root.host_socket().parent_path());
    protocol::TransportServer server(engine, root.host_socket().string());
    ASSERT_NO_THROW(server.start());

    EXPECT_EQ(protocol::probe_existing_socket(root.host_socket().string(), host.workspace,
                                              host.boot_id,
                                              std::chrono::steady_clock::now() + 2s),
              protocol::SocketProbeResult::LiveYmhDaemon);
    server.stop();
}

// ---------------------------------------------------------------------------
// Part C: an in-process ProtocolServer driven directly (no sockets).
// ---------------------------------------------------------------------------

struct Peer {
    protocol::ClientId id;
    std::vector<std::string> frames;
    std::optional<std::string> drop_reason;
    bool ack{true};
};

class PeerHarness {
public:
    explicit PeerHarness() {
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
        server->receiveBytes(peer.id,
                             protocol::FrameCodec::encode(message.dump(), protocol::TransportLimits{}
                                                                                  .max_frame_bytes));
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

    void hello(Peer& peer, std::string_view instance,
               protocol::ClientRole role = protocol::ClientRole::Supervisor, std::int64_t id = 1) {
        nlohmann::json params;
        protocol::to_json(params, protocol::HelloParams{protocol::kProtocolVersion,
                                                        protocol::ServerProfile::Interactive,
                                                        protocol::ClientInstanceId{
                                                            std::string{instance}},
                                                        std::nullopt, role});
        send(peer, request(id, protocol::method::kHostHello, params));
    }

    void subscribe(Peer& peer, const SessionId& session, std::int64_t id) {
        protocol::StreamFrom from;
        from.kind = protocol::StreamFrom::Kind::Now;
        nlohmann::json params;
        protocol::to_json(params, protocol::SubscribeParams{session, from});
        send(peer, request(id, protocol::method::kEventSubscribe, params));
    }
};

TEST(Protocol87, SupersedeIsRoleAware) {
    PeerHarness harness;
    Peer* supervisor = harness.open();
    harness.hello(*supervisor, kInstanceA, protocol::ClientRole::Supervisor);
    harness.drain(*supervisor);

    // Same instance, different role: must NOT evict the supervisor link.
    Peer* observer = harness.open();
    harness.hello(*observer, kInstanceA, protocol::ClientRole::Observer);
    harness.drain(*observer);
    EXPECT_FALSE(supervisor->drop_reason.has_value())
        << "a cross-role duplicate must not supersede";
    EXPECT_FALSE(observer->drop_reason.has_value());

    // Same instance, same role: the older link is superseded.
    Peer* duplicate = harness.open();
    harness.hello(*duplicate, kInstanceA, protocol::ClientRole::Supervisor);
    harness.drain(*duplicate);
    EXPECT_TRUE(supervisor->drop_reason.has_value())
        << "a same-(instance, role) duplicate must supersede";
    EXPECT_FALSE(duplicate->drop_reason.has_value());
}

TEST(Protocol87, GracefulDropSendsFrames) {
    PeerHarness harness;
    const SessionId session = harness.host.seed("s1");
    harness.host.append(session, EventType::TurnStarted, nlohmann::json{{"n", 1}});

    Peer* first = harness.open();
    harness.hello(*first, kInstanceA, protocol::ClientRole::Supervisor);
    harness.drain(*first);
    harness.subscribe(*first, session, 2);
    harness.drain(*first);

    // Supersede with a same-(instance, role) hello.
    Peer* second = harness.open();
    harness.hello(*second, kInstanceA, protocol::ClientRole::Supervisor);

    const auto frames = harness.drain(*first);
    ASSERT_GE(frames.size(), 2u);
    ASSERT_EQ(frames[0].at("method").get<std::string>(), "event.unsubscribed");
    EXPECT_EQ(frames[0].at("params").at("reason").get<std::string>(), "superseded");
    ASSERT_EQ(frames[1].at("method").get<std::string>(), "host.event");
    EXPECT_EQ(frames[1].at("params").at("kind").get<std::string>(), "link_dropped");
    EXPECT_EQ(frames[1].at("params").at("detail").get<std::string>(), "superseded");
}

TEST(Protocol87, EndAllSubscriptions) {
    PeerHarness harness;
    const SessionId session_a = harness.host.seed("sa");
    const SessionId session_b = harness.host.seed("sb");

    Peer* first = harness.open();
    harness.hello(*first, kInstanceA, protocol::ClientRole::Supervisor);
    harness.drain(*first);
    harness.subscribe(*first, session_a, 2);
    harness.subscribe(*first, session_b, 3);
    harness.drain(*first);

    Peer* second = harness.open();
    harness.hello(*second, kInstanceA, protocol::ClientRole::Supervisor);

    const auto frames = harness.drain(*first);
    int unsubscribed = 0;
    int link_dropped = 0;
    for (const auto& frame : frames) {
        const std::string method = frame.at("method").get<std::string>();
        if (method == "event.unsubscribed") {
            ++unsubscribed;
        } else if (method == "host.event" &&
                   frame.at("params").at("kind").get<std::string>() == "link_dropped") {
            ++link_dropped;
        }
    }
    // One `event.unsubscribed` per subscription (two sessions => two), not the
    // session-scoped single.
    EXPECT_EQ(unsubscribed, 2);
    EXPECT_EQ(link_dropped, 1);
}

// ---------------------------------------------------------------------------
// Part D: predecessor boot threading through HostLifecycle.
// ---------------------------------------------------------------------------

RegistryConfig registry_config_for(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path = root / ".state" / "ymh" / "registry.db";
    config.lock_path = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots = {};
    config.lock_retry_budget = 200ms;
    config.lock_retry_interval = 5ms;
    return config;
}

class CapturingLauncher final : public HostLauncher {
public:
    SpawnResult spawn(const HostConfig& config) override {
        ++spawn_count;
        captured = config;
        return SpawnResult{HostPid{4242}, HostBootId{"fake-boot"}, config.socket_path};
    }
    void requestStop(HostPid, ShutdownReason) override {}
    [[nodiscard]] bool isAlive(HostPid) const override { return true; }
    [[nodiscard]] std::optional<int> tryReap(HostPid) override { return std::nullopt; }

    int spawn_count = 0;
    std::optional<HostConfig> captured;
};

TEST(Host87, PredecessorBootThreaded) {
    test::ShortTempRoot root("ymh-e87-pred");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    std::filesystem::create_directories(canonical / ".ymh");
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config_for(canonical));

    const HostBootId claim_boot{"22222222-2222-4222-8222-222222222222"};

    // (a) A pre-existing claim: the pre-reap copy is threaded even after reap.
    {
        const std::optional<WorkspaceRecord> record =
            registry->registerWorkspace(canonical, "pred");
        ASSERT_TRUE(record.has_value());
        registry->claimHost(HostClaim{record->id, HostPid{static_cast<std::int32_t>(::getpid())},
                                      claim_boot, canonical / ".ymh" / "host.sock"});
        CapturingLauncher launcher;
        HostLifecycle lifecycle(launcher, *registry);
        AttachBudget budget;
        budget.total = 150ms;
        budget.spawn_readiness = 60ms;
        budget.winner = 40ms;
        try {
            static_cast<void>(lifecycle.ensureRunning(record->id, ui::probe_attach_identity(), budget));
        } catch (const std::exception&) {
        }
        ASSERT_EQ(launcher.spawn_count, 1);
        ASSERT_TRUE(launcher.captured->predecessor_boot_id.has_value());
        EXPECT_EQ(launcher.captured->predecessor_boot_id->value, claim_boot.value);
    }

    // (b) The no-claim path carries no predecessor nonce.
    {
        test::ShortTempRoot bare_root("ymh-e87-noclaim");
        const std::filesystem::path bare = std::filesystem::canonical(bare_root.path());
        std::filesystem::create_directories(bare / ".ymh");
        std::unique_ptr<WorkspaceRegistry> bare_registry =
            WorkspaceRegistry::open(registry_config_for(bare));
        const std::optional<WorkspaceRecord> record =
            bare_registry->registerWorkspace(bare, "noclaim");
        ASSERT_TRUE(record.has_value());
        CapturingLauncher launcher;
        HostLifecycle lifecycle(launcher, *bare_registry);
        AttachBudget budget;
        budget.total = 150ms;
        budget.spawn_readiness = 60ms;
        budget.winner = 40ms;
        try {
            static_cast<void>(
                lifecycle.ensureRunning(record->id, ui::probe_attach_identity(), budget));
        } catch (const std::exception&) {
        }
        ASSERT_EQ(launcher.spawn_count, 1);
        EXPECT_FALSE(launcher.captured->predecessor_boot_id.has_value());
    }
}

// ---------------------------------------------------------------------------
// Part E: raw HostConnection faults against a scripted peer.
// ---------------------------------------------------------------------------

// A single-connection scripted JSON-RPC peer that answers `host.hello`, then
// either stalls (never answers another request) or closes.
class ScriptedPeer {
public:
    enum class Mode { Stall, CloseAfterHello };

    ScriptedPeer(const std::filesystem::path& path, Mode mode) : mode_(mode) {
        ::unlink(path.c_str());
        listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string text = path.string();
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0) {
            throw std::runtime_error("scripted peer bind/listen failed");
        }
        thread_ = std::thread([this] { serve(); });
    }

    ~ScriptedPeer() {
        running_.store(false);
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
        }
        if (client_ >= 0) {
            ::shutdown(client_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ScriptedPeer(const ScriptedPeer&) = delete;
    ScriptedPeer& operator=(const ScriptedPeer&) = delete;

    void drop_client() {
        if (client_ >= 0) {
            ::shutdown(client_, SHUT_RDWR);
        }
    }

private:
    void serve() {
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) {
            return;
        }
        client_ = client;
        std::string buffer;
        while (running_.load()) {
            char chunk[65536];
            const ssize_t count = ::read(client, chunk, sizeof(chunk));
            if (count <= 0) {
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(count));
            std::string frame;
            while (true) {
                protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
                try {
                    status = protocol::FrameCodec::decode_step(
                        buffer, protocol::TransportLimits{}.max_frame_bytes, frame);
                } catch (const std::exception&) {
                    return;
                }
                if (status != protocol::DecodeStatus::Ok) {
                    break;
                }
                protocol::Message message = protocol::parse_message(frame);
                const auto* request = std::get_if<protocol::Request>(&message);
                if (request == nullptr) {
                    continue;
                }
                if (request->method == protocol::method::kHostHello) {
                    protocol::HelloResult hello;
                    hello.workspace.value = "ws-1";
                    hello.boot_id.value = "boot-1";
                    hello.client_id = protocol::ClientId{1};
                    nlohmann::json body;
                    protocol::to_json(body, hello);
                    send_to(client, protocol::encode(
                                        protocol::Response{request->id, std::move(body)}));
                    if (mode_ == Mode::CloseAfterHello) {
                        ::shutdown(client, SHUT_RDWR);
                        return;
                    }
                }
                // Non-hello requests are ignored: the client's bounded wait
                // expires (`Stall`) or the socket is already closed.
            }
        }
    }

    static void send_to(int fd, const nlohmann::json& message) {
        const std::string frame = protocol::FrameCodec::encode(
            message.dump(), protocol::TransportLimits{}.max_frame_bytes);
        std::size_t written = 0;
        while (written < frame.size()) {
            const ssize_t count =
                ::send(fd, frame.data() + written, frame.size() - written, MSG_NOSIGNAL);
            if (count <= 0) {
                return;
            }
            written += static_cast<std::size_t>(count);
        }
    }

    Mode                mode_;
    int                 listener_{-1};
    std::atomic<int>    client_{-1};
    std::atomic<bool>   running_{true};
    std::thread         thread_;
};

TEST(HostConnection87, ReadWaitThrowsTimeoutKind) {
    test::ShortTempRoot root("ymh-e87-readtimeout");
    std::filesystem::create_directories(root.host_socket().parent_path());
    ScriptedPeer peer(root.host_socket(), ScriptedPeer::Mode::Stall);

    protocol::HostConnection connection;
    connection.connect(root.host_socket().string());
    static_cast<void>(connection.handshake(protocol::ServerProfile::Interactive,
                                           protocol::ClientInstanceId{std::string{kInstanceA}},
                                           1s));

    try {
        static_cast<void>(connection.request(protocol::method::kHostPing,
                                             nlohmann::json::object(), 200ms));
        FAIL() << "the stalled request must time out";
    } catch (const protocol::ConnectionError& error) {
        EXPECT_EQ(error.kind(), protocol::ConnectionErrorKind::Timeout);
        EXPECT_FALSE(error.is_link_loss());
    }
}

TEST(HostConnection87, WritePeerCloseIsLinkLoss) {
    test::ShortTempRoot root("ymh-e87-writeclose");
    std::filesystem::create_directories(root.host_socket().parent_path());
    ScriptedPeer peer(root.host_socket(), ScriptedPeer::Mode::CloseAfterHello);

    protocol::HostConnection connection;
    connection.connect(root.host_socket().string());
    static_cast<void>(connection.handshake(protocol::ServerProfile::Interactive,
                                           protocol::ClientInstanceId{std::string{kInstanceA}},
                                           1s));
    // Let the peer's close propagate so the next write/read observes it.
    std::this_thread::sleep_for(50ms);

    try {
        static_cast<void>(connection.request(protocol::method::kHostPing,
                                             nlohmann::json::object(), 300ms));
        FAIL() << "a closed peer must surface as a link loss";
    } catch (const protocol::ConnectionError& error) {
        EXPECT_TRUE(error.is_link_loss());
        EXPECT_TRUE(error.kind() == protocol::ConnectionErrorKind::Epipe ||
                    error.kind() == protocol::ConnectionErrorKind::PeerClosed);
    }
}

// ---------------------------------------------------------------------------
// Part F: the UiEventAdapter LinkDropped case + the live-link probe test.
// ---------------------------------------------------------------------------

TEST(UiEventAdapter87, LinkDroppedCase) {
    UiModel model;
    const WorkspaceId workspace{"ws-1"};
    WorkspaceModel workspace_model;
    workspace_model.id = workspace;
    workspace_model.cwd = "/work";
    workspace_model.title = "alpha";
    model.workspaces.emplace(workspace, std::move(workspace_model));
    UiEventAdapter adapter(model);

    protocol::HostNotice notice;
    notice.kind = protocol::HostNoticeKind::LinkDropped;
    notice.workspace.value = workspace.value;
    notice.detail = "superseded";
    EXPECT_NO_THROW(adapter.onHostNotice(workspace, notice));
    adapter.onTick(std::chrono::steady_clock::now(), 1ms);
}

TEST(Ui87, Err87ProbeDoesNotEvictLiveLink) {
    test::FakeTransportHost host;
    protocol::ProtocolServer engine(host, server_config_for(host));
    std::atomic<std::size_t> live_owners{0};
    engine.set_owner_liveness_sink(
        [&live_owners](std::size_t owners, std::chrono::steady_clock::time_point) {
            live_owners.store(owners);
        });

    test::ShortTempRoot root("ymh-e87-probe-live");
    std::filesystem::create_directories(root.host_socket().parent_path());
    protocol::TransportServer server(engine, root.host_socket().string());
    ASSERT_NO_THROW(server.start());

    ui::SupervisorConnectionConfig config;
    config.socket_path = root.host_socket().string();
    config.workspace = ui::WorkspaceId{host.workspace.value};
    config.expected_boot_id = host.boot_id.value;
    config.client_instance = protocol::ClientInstanceId{std::string{kInstanceB}};
    config.role = protocol::ClientRole::Supervisor;
    config.profile = protocol::ServerProfile::Interactive;
    config.poll_interval = 10ms;
    config.reconnect_backoff = 50ms;
    config.request_timeout = 2s;

    ui::SupervisorConnection connection(config, ui::SupervisorSink{});
    connection.start();
    ASSERT_TRUE(connection.waitForState(ui::SupervisorLinkState::Attached, 3s));
    ASSERT_TRUE(connection.waitUntil([&live_owners] { return live_owners.load() == 1; }, 2s));

    // A transient probe: fresh instance + Observer (io thread liveness excludes it).
    const AttachIdentity probe = ui::probe_attach_identity();
    protocol::HostConnection probe_connection;
    probe_connection.connect(root.host_socket().string());
    static_cast<void>(probe_connection.handshake(protocol::ServerProfile::Interactive,
                                                 probe.client_instance, probe.role, 1s));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(live_owners.load(), 1u) << "an Observer probe must not enter the owner set";

    // The durable link survives the probe and still serves requests.
    std::atomic<bool> replied{false};
    connection.submit(std::string(protocol::method::kHostPing), nlohmann::json::object(),
                      [&replied](ui::SupervisorReply reply) {
                          EXPECT_TRUE(reply.ok);
                          replied.store(true);
                      });
    ASSERT_TRUE(connection.waitUntil([&replied] { return replied.load(); }, 3s));
    EXPECT_EQ(connection.state(), ui::SupervisorLinkState::Attached);

    probe_connection.close();
    connection.stop();
    server.stop();
}

} // namespace

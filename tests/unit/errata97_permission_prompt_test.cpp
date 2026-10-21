// Spec 97 (Rev 26): prompts belong to the viewed session; a second instance
// joins the live session; a decision requires a subscription to its session.
// Tests T1-T14 from section 8.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/fake_transport_host.hpp"
#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol_server.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

const WorkspaceId kW{"ws-a"};
const WorkspaceId kOtherW{"ws-b"};
const SessionId   kA{"s-a"};
const SessionId   kB{"s-b"};
const SessionId   kC{"s-c"};
const SessionId   kOtherS{"s-other"};

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = kW;
    WorkspaceModel a;
    a.id           = kW;
    a.cwd          = "/tmp/ws-a";
    a.daemonStatus = DaemonStatus::Attached;
    a.live         = true;
    model.workspaces.emplace(kW, a);
    WorkspaceModel b;
    b.id           = kOtherW;
    b.cwd          = "/tmp/ws-b";
    b.daemonStatus = DaemonStatus::Attached;
    b.live         = true;
    model.workspaces.emplace(kOtherW, b);
    model.focusSessionIn(kW, kA);
    model.ensureSessionIn(kOtherW, kB);
    model.ensureSessionIn(kOtherW, kOtherS);
    model.ensureSessionIn(kW, kC);
    model.mode = UiMode::Conversation;
    return model;
}

void request(UiEventAdapter& adapter, const SessionId& session, const std::string& id,
             bool force_ask = false) {
    PermissionRequest wire;
    wire.session   = session;
    wire.tool      = "shell";
    wire.arguments = {{"command", "ls"}};
    wire.force_ask = force_ask;
    adapter.onPermissionRequest(session, PermissionRequestId{id}, wire);
}

void resolved(UiEventAdapter& adapter, const SessionId& session, const std::string& id) {
    adapter.onPermissionResolved(session, PermissionRequestId{id},
                                 payload::PermissionDecisionKind::Allow);
}

bool has_queue(const UiModel& model, const SessionId& session, const std::string& id) {
    const SessionUiState* state = model.session(session);
    if (state == nullptr) {
        return false;
    }
    return std::any_of(state->pending_permissions.begin(), state->pending_permissions.end(),
                       [&id](const PendingPermission& pending) {
                           return pending.request.value == id;
                       });
}

WorkspaceModel live_workspace(const WorkspaceId& id) {
    WorkspaceModel workspace;
    workspace.id           = id;
    workspace.title        = id.value;
    workspace.cwd          = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    return workspace;
}

SessionCell cell(const SessionId& id) {
    SessionCell value;
    value.id    = id;
    value.title = id.value;
    return value;
}

WorkspaceModel model_with_sessions(const WorkspaceId& workspace,
                                   const std::vector<SessionId>& sessions) {
    WorkspaceModel model = live_workspace(workspace);
    for (const SessionId& session : sessions) {
        model.sessions.push_back(cell(session));
    }
    return model;
}

SessionCatalogSnapshot catalog_snapshot(const WorkspaceId& workspace,
                                        const std::vector<SessionId>& sessions) {
    SessionCatalogSnapshot snapshot;
    snapshot.generation = 1;
    snapshot.complete   = true;
    WorkspaceHistory history;
    history.id            = workspace;
    history.title         = workspace.value;
    history.canonicalPath = "/" + workspace.value;
    history.live          = true;
    for (const SessionId& session : sessions) {
        SessionHistoryEntry entry;
        entry.id        = session;
        entry.title     = session.value;
        entry.kind      = "root";
        entry.model     = "m";
        entry.updatedAt = 1;
        history.sessions.push_back(std::move(entry));
    }
    snapshot.workspaces.push_back(std::move(history));
    return snapshot;
}

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata97-harness"},
                          protocol::ClientRole::Supervisor};
}

// ---- T1: a request for a non-viewed session queues, never draws --------------

TEST(Errata97, T1NonViewedSessionQueuesWithoutTouchingDialogOrMode) {
    UiModel model = make_model();   // viewing kA in kW
    UiEventAdapter adapter(model);

    request(adapter, kB, "p1");

    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Conversation);
    EXPECT_EQ(model.dialog.prev_mode, UiMode::Conversation);
    ASSERT_TRUE(has_queue(model, kB, "p1"));
    EXPECT_TRUE(model.session(kB)->attention.needsInput);
    EXPECT_FALSE(model.session(kA)->attention.needsInput);
    EXPECT_EQ(model.dialog.request.value, "");
}

// ---- T2: focus through the real handler opens the stored prompt -------------

TEST(Errata97, T2FocusOpensQueuedPromptAndModelFocusAloneDoesNot) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness =
        make_supervisor_harness(std::move(options));
    harness->seed_workspace(model_with_sessions(kW, {kA, kB}));
    harness->activate_session(kW, kA);
    harness->on_catalog_snapshot(catalog_snapshot(kW, {kA, kB}));

    PermissionRequested queued;
    queued.session   = kB;
    queued.request   = PermissionRequestId{"p1"};
    queued.tool      = "shell";
    queued.summary   = "run ls";
    queued.force_ask = false;
    harness->mutable_model().apply(UiEvent{queued});
    ASSERT_FALSE(harness->model().dialog.open);

    ASSERT_TRUE(harness->dispatch_key("ctrl-s"));
    ASSERT_EQ(harness->model().mode, UiMode::Dashboard);
    auto& dashboard = harness->mutable_model().dashboard;
    const auto row = std::find_if(dashboard.rows.begin(), dashboard.rows.end(),
                                  [](const DashboardRow& candidate) {
                                      return candidate.session == kB;
                                  });
    ASSERT_NE(row, dashboard.rows.end());
    dashboard.cursor = static_cast<std::size_t>(row - dashboard.rows.begin());

    ASSERT_TRUE(harness->dispatch_key("enter"));
    ASSERT_TRUE(harness->model().dialog.open);
    EXPECT_EQ(harness->model().dialog.session, kB);
    EXPECT_EQ(harness->model().dialog.request.value, "p1");
    EXPECT_EQ(harness->model().dialog.tool, "shell");
    EXPECT_EQ(harness->model().dialog.summary, "run ls");
    EXPECT_EQ(harness->model().dialog.prev_mode, UiMode::Conversation);
    EXPECT_TRUE(harness->model().session(kB)->pending_permissions.empty());
    EXPECT_EQ(harness->model().mode, UiMode::Dialog);

    // The model-level focus hook is not what opens it: a second hook run with an
    // empty queue is a no-op.
    harness->mutable_model().openPendingPermissionIfAny();
    EXPECT_TRUE(harness->model().dialog.open);
    EXPECT_EQ(harness->model().dialog.request.value, "p1");

    // Model-level `focusSession` alone does not open the queue.
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    request(adapter, kB, "p2");
    model.focusSession(kB);
    EXPECT_FALSE(model.dialog.open);
    model.openPendingPermissionIfAny();
    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.request.value, "p2");
}

// ---- T3: resolution clears only the matching stored prompt ------------------

TEST(Errata97, T3ResolutionClearsMatchingQueuedPromptAndAttention) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    request(adapter, kB, "p1");
    request(adapter, kB, "p2");
    ASSERT_TRUE(has_queue(model, kB, "p1"));
    ASSERT_TRUE(has_queue(model, kB, "p2"));

    // A sibling remains: attention stays Waiting.
    resolved(adapter, kB, "p1");
    EXPECT_FALSE(has_queue(model, kB, "p1"));
    EXPECT_TRUE(has_queue(model, kB, "p2"));
    EXPECT_TRUE(model.session(kB)->attention.needsInput);

    // Last one: attention clears.
    resolved(adapter, kB, "p2");
    EXPECT_FALSE(has_queue(model, kB, "p2"));
    EXPECT_FALSE(model.session(kB)->attention.needsInput);

    // A dialog open on ANOTHER session does not block clearing this session.
    request(adapter, kA, "pa");
    ASSERT_TRUE(model.dialog.open);
    request(adapter, kB, "p3");
    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.session, kA);
    EXPECT_TRUE(model.session(kB)->attention.needsInput);
    resolved(adapter, kB, "p3");
    EXPECT_FALSE(model.session(kB)->attention.needsInput);
    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.session, kA);

    // Another session's resolution does not touch this one.
    request(adapter, kB, "p4");
    resolved(adapter, kA, "pa");
    EXPECT_FALSE(model.dialog.open);
    EXPECT_TRUE(has_queue(model, kB, "p4"));
    EXPECT_TRUE(model.session(kB)->attention.needsInput);
}

// ---- T4: dashboard queues despite a set active session ----------------------

TEST(Errata97, T4DashboardQueuesWithoutModal) {
    UiModel model = make_model();
    model.openDashboard();
    ASSERT_EQ(model.mode, UiMode::Dashboard);
    ASSERT_EQ(model.workspaces.at(kW).activeSessionId(), kA);

    UiEventAdapter adapter(model);
    request(adapter, kA, "p1");

    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Dashboard);
    ASSERT_TRUE(has_queue(model, kA, "p1"));
    EXPECT_TRUE(model.session(kA)->attention.needsInput);
}

// ---- T5: close/link/erase clear stored prompts even with no dialog ----------

TEST(Errata97, T5CloseAndEraseClearStoredPrompts) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    request(adapter, kB, "p1");
    ASSERT_TRUE(has_queue(model, kB, "p1"));

    // Session close helper with no open dialog clears the queue + attention.
    model.closePermissionDialogForSession(kB);
    EXPECT_FALSE(has_queue(model, kB, "p1"));
    EXPECT_FALSE(model.session(kB)->attention.needsInput);
    EXPECT_EQ(model.mode, UiMode::Conversation);

    // SessionClosed event erases the state (and its queue).
    request(adapter, kB, "p2");
    protocol::HostNotice notice;
    notice.kind    = protocol::HostNoticeKind::SessionClosed;
    notice.session = kB;
    adapter.onHostNotice(kOtherW, notice);
    EXPECT_EQ(model.session(kB), nullptr);
    EXPECT_FALSE(model.dialog.open);

    // Erasing the dialog's session closes it and restores prev_mode.
    model.focusSessionIn(kOtherW, kOtherS);
    request(adapter, kOtherS, "p3");
    ASSERT_TRUE(model.dialog.open);
    model.dialog.prev_mode = UiMode::Dashboard;
    model.mode             = UiMode::Dialog;
    model.eraseSession(kOtherW, kOtherS);
    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Dashboard);
    EXPECT_EQ(model.dialog.session.value, "");
    EXPECT_EQ(model.session(kOtherS), nullptr);

    // Erasing the dialog's workspace (incl. the workspace-missing early return)
    // closes the dialog, drops the queue, and restores the mode.
    UiModel early = make_model();
    UiEventAdapter early_adapter(early);
    early.focusSessionIn(kW, kA);
    request(early_adapter, kA, "p4");
    ASSERT_TRUE(early.dialog.open);
    early.dialog.prev_mode = UiMode::Switcher;
    early.mode             = UiMode::Dialog;
    early.workspaces.erase(kW);          // force the early-return path
    early.eraseWorkspace(kW);
    EXPECT_FALSE(early.dialog.open);
    EXPECT_EQ(early.mode, UiMode::Switcher);
    EXPECT_EQ(early.dialog.session.value, "");
}

// ---- T6: several queued requests survive a view change in FIFO order -------

TEST(Errata97, T6ViewChangeDemotesOpenRequestToQueueFront) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    request(adapter, kA, "r1");
    ASSERT_TRUE(model.dialog.open);
    request(adapter, kA, "r2");
    ASSERT_TRUE(has_queue(model, kA, "r2"));

    model.focusSessionIn(kW, kC);
    EXPECT_FALSE(model.dialog.open);
    ASSERT_EQ(model.session(kA)->pending_permissions.size(), 2u);
    EXPECT_EQ(model.session(kA)->pending_permissions[0].request.value, "r1");
    EXPECT_EQ(model.session(kA)->pending_permissions[1].request.value, "r2");
    EXPECT_TRUE(model.session(kA)->attention.needsInput);

    model.focusSessionIn(kW, kA);
    EXPECT_FALSE(model.dialog.open);
    model.openPendingPermissionIfAny();
    ASSERT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.request.value, "r1");
    resolved(adapter, kA, "r1");
    ASSERT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.request.value, "r2");

    // Erasing the session before the focus drops its prompt and leaves no ghost.
    UiModel erased = make_model();
    UiEventAdapter erased_adapter(erased);
    erased.focusSessionIn(kOtherW, kB);
    request(erased_adapter, kB, "e1");
    ASSERT_TRUE(erased.dialog.open);
    request(erased_adapter, kB, "e2");
    erased.eraseSession(kOtherW, kB);
    EXPECT_FALSE(erased.dialog.open);
    EXPECT_EQ(erased.session(kB), nullptr);
    EXPECT_EQ(erased.dialog.session.value, "");
}

// ---- T7: only a subscriber of the request's session may decide -------------

struct Peer {
    protocol::ClientId       id;
    std::vector<std::string> frames;
    std::optional<std::string> drop_reason;
};

class ServerHarness {
public:
    ServerHarness() {
        protocol::ProtocolServerConfig config;
        config.uid       = 1000;
        config.workspace = host.workspace;
        config.boot_id   = host.boot_id;
        config.pid       = host.pid;
        server = std::make_unique<protocol::ProtocolServer>(host, config);
    }

    test::FakeTransportHost                   host;
    protocol::TransportLimits                 limits;
    std::unique_ptr<protocol::ProtocolServer> server;
    std::vector<std::unique_ptr<Peer>>        peers;

    Peer* open() {
        peers.push_back(std::make_unique<Peer>());
        Peer* peer = peers.back().get();
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
        server->receiveBytes(
            peer.id, protocol::FrameCodec::encode(message.dump(), limits.max_frame_bytes));
    }

    std::vector<nlohmann::json> drain(Peer& peer) {
        std::string buffer;
        for (const std::string& frame : peer.frames) {
            buffer += frame;
        }
        peer.frames.clear();
        std::vector<nlohmann::json> messages;
        for (const std::string& body :
             protocol::FrameCodec::decode(buffer, limits.max_frame_bytes)) {
            messages.push_back(nlohmann::json::parse(body));
        }
        return messages;
    }

    void hello(Peer& peer, protocol::ServerProfile profile, std::string_view instance,
               std::int64_t id = 1) {
        nlohmann::json params;
        protocol::to_json(params, protocol::HelloParams{
                                      protocol::kProtocolVersion, profile,
                                      protocol::ClientInstanceId{std::string{instance}},
                                      std::nullopt, protocol::ClientRole::Supervisor});
        send(peer, request(id, protocol::method::kHostHello, std::move(params)));
    }

    void subscribe(Peer& peer, const SessionId& session, std::int64_t id) {
        protocol::StreamFrom from;
        from.kind = protocol::StreamFrom::Kind::Now;
        nlohmann::json params;
        protocol::to_json(params, protocol::SubscribeParams{session, from});
        send(peer, request(id, protocol::method::kEventSubscribe, std::move(params)));
    }

    static nlohmann::json request(std::int64_t id, std::string_view method,
                                  nlohmann::json params = nlohmann::json::object()) {
        return nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"method", std::string{method}},
                              {"params", std::move(params)}};
    }
};

nlohmann::json decide_request(std::int64_t id, const std::string& request_id) {
    return ServerHarness::request(id, protocol::method::kPermissionDecide,
                                  nlohmann::json{{"request_id", request_id},
                                                 {"decision", "allow"},
                                                 {"scope", "session"}});
}

TEST(Errata97, T7NonSubscriberDecideIsRejected) {
    ServerHarness harness;
    const SessionId session = harness.host.seed("s1");
    Peer* subscriber = harness.open();
    harness.hello(*subscriber, protocol::ServerProfile::Interactive,
                  "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    harness.drain(*subscriber);
    Peer* outsider = harness.open();
    harness.hello(*outsider, protocol::ServerProfile::Interactive,
                  "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    harness.drain(*outsider);

    harness.subscribe(*subscriber, session, 2);
    harness.drain(*subscriber);

    protocol::PermissionRequest request;
    request.request_id = "perm-9";
    request.session    = session;
    request.tool       = "shell";
    harness.host.known_permissions.insert("perm-9");
    harness.host.permission_sessions["perm-9"] = session;
    harness.server->onPermissionRequest(request);
    harness.drain(*subscriber);

    // Outsider: rejected before the host sees a decision.
    harness.send(*outsider, decide_request(3, "perm-9"));
    const auto outsider_frames = harness.drain(*outsider);
    ASSERT_EQ(outsider_frames.size(), 1u);
    ASSERT_TRUE(outsider_frames[0].contains("error"));
    EXPECT_EQ(outsider_frames[0].at("error").at("message").get<std::string>(),
              "not subscribed to this session");
    EXPECT_EQ(std::count(harness.host.calls.begin(), harness.host.calls.end(),
                         "permission.decide"),
              0);

    // Subscriber: succeeds.
    harness.send(*subscriber, decide_request(3, "perm-9"));
    const auto subscriber_frames = harness.drain(*subscriber);
    ASSERT_EQ(subscriber_frames.size(), 1u);
    EXPECT_TRUE(subscriber_frames[0].contains("result"));
    EXPECT_EQ(std::count(harness.host.calls.begin(), harness.host.calls.end(),
                         "permission.decide"),
              1);
}

// ---- T8: two UIs viewing one session share the prompt and its resolution ----

TEST(Errata97, T8TwoViewersSeePromptAndOneAnswerClosesBoth) {
    ServerHarness harness;
    const SessionId session = harness.host.seed("shared");
    Peer* a = harness.open();
    harness.hello(*a, protocol::ServerProfile::Interactive,
                  "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    harness.drain(*a);
    Peer* b = harness.open();
    harness.hello(*b, protocol::ServerProfile::Interactive,
                  "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    harness.drain(*b);
    harness.subscribe(*a, session, 2);
    harness.subscribe(*b, session, 2);
    harness.drain(*a);
    harness.drain(*b);

    UiModel model_a;
    model_a.activeWorkspaceId = kW;
    model_a.workspaces.emplace(kW, live_workspace(kW));
    model_a.focusSessionIn(kW, session);
    UiModel model_b = model_a;
    UiEventAdapter adapter_a(model_a);
    UiEventAdapter adapter_b(model_b);

    protocol::PermissionRequest request;
    request.request_id = "perm-shared";
    request.session    = session;
    request.tool       = "shell";
    request.summary    = "shared prompt";
    request.arguments  = nlohmann::json::object();
    harness.host.known_permissions.insert("perm-shared");
    harness.host.permission_sessions["perm-shared"] = session;
    adapter_a.onPermissionRequest(kW, request);
    adapter_b.onPermissionRequest(kW, request);
    harness.server->onPermissionRequest(request);
    const auto requested_a = harness.drain(*a);
    const auto requested_b = harness.drain(*b);
    EXPECT_NE(std::find_if(requested_a.begin(), requested_a.end(),
                           [](const nlohmann::json& frame) {
                               return frame.value("method", std::string{}) ==
                                      "permission.request";
                           }),
              requested_a.end());
    EXPECT_NE(std::find_if(requested_b.begin(), requested_b.end(),
                           [](const nlohmann::json& frame) {
                               return frame.value("method", std::string{}) ==
                                      "permission.request";
                           }),
              requested_b.end());
    EXPECT_TRUE(model_a.dialog.open);
    EXPECT_TRUE(model_b.dialog.open);
    EXPECT_EQ(model_a.dialog.request.value, "perm-shared");
    EXPECT_EQ(model_b.dialog.request.value, "perm-shared");

    // A answers; the daemon broadcasts the resolution.
    harness.send(*a, decide_request(3, "perm-shared"));
    const auto a_reply = harness.drain(*a);
    ASSERT_EQ(a_reply.size(), 1u);
    EXPECT_TRUE(a_reply[0].contains("result"));

    protocol::PermissionResolvedNotice notice;
    notice.request_id = "perm-shared";
    notice.session    = session;
    notice.decision   = protocol::PermissionAnswer::Allow;
    notice.scope      = protocol::PermissionScope::Session;
    notice.reason     = "user";
    harness.server->onPermissionResolved(notice);
    const auto to_a = harness.drain(*a);
    const auto to_b = harness.drain(*b);
    EXPECT_NE(std::find_if(to_a.begin(), to_a.end(),
                           [](const nlohmann::json& frame) {
                               return frame.value("method", std::string{}) ==
                                      "permission.resolved";
                           }),
              to_a.end());
    EXPECT_NE(std::find_if(to_b.begin(), to_b.end(),
                           [](const nlohmann::json& frame) {
                               return frame.value("method", std::string{}) ==
                                      "permission.resolved";
                           }),
              to_b.end());

    adapter_a.onPermissionResolved(session, PermissionRequestId{"perm-shared"},
                                   payload::PermissionDecisionKind::Allow);
    adapter_b.onPermissionResolved(session, PermissionRequestId{"perm-shared"},
                                   payload::PermissionDecisionKind::Allow);
    EXPECT_FALSE(model_a.dialog.open);
    EXPECT_FALSE(model_b.dialog.open);
    EXPECT_EQ(model_a.mode, UiMode::Conversation);
    EXPECT_EQ(model_b.mode, UiMode::Conversation);
}

// ---- T9: a second instance joins the last-used live session ----------------

RegistryConfig launch_registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = 200ms;
    config.lock_retry_interval = 5ms;
    return config;
}

SupervisorWorkspace launch_spec(const WorkspaceRecord& record, std::string boot_id) {
    SupervisorWorkspace spec;
    spec.id          = record.id;
    spec.cwd         = record.canonicalPath.string();
    spec.title       = record.displayTitle;
    spec.socket_path = (record.canonicalPath / ".ymh" / "host.sock").string();
    spec.boot_id     = std::move(boot_id);
    return spec;
}

struct LaunchFixture {
    explicit LaunchFixture(const std::string& name, bool force_new = false)
        : root(name),
          registry(WorkspaceRegistry::open(launch_registry_config(root.path()))) {
        std::filesystem::create_directories(root.path() / "ws");
        row       = registry->registerWorkspace(root.path() / "ws", "ws");
        workspace = row.id;
        SupervisorRunOptions options;
        options.registry          = registry.get();
        options.identity          = test_identity();
        options.force_new_session = force_new;
        harness                   = make_supervisor_harness(std::move(options));
        harness->on_scan({launch_spec(row, "boot-a")});
        harness->drain_actions();
        WorkspaceModel model = live_workspace(workspace);
        model.daemonStatus   = DaemonStatus::Dead;
        model.live           = false;
        harness->seed_active_workspace(model);
    }

    ShortTempRoot                      root;
    std::unique_ptr<WorkspaceRegistry> registry;
    WorkspaceRecord                    row;
    WorkspaceId                        workspace;
    std::unique_ptr<SupervisorHarness> harness;
};

nlohmann::json launch_row(const SessionId& id, std::int64_t updated_at_ms,
                          std::int64_t ordinal) {
    return nlohmann::json{{"id", id.value},
                          {"title", id.value},
                          {"kind", "root"},
                          {"live", true},
                          {"updated_at_ms", updated_at_ms},
                          {"ordinal", ordinal}};
}

void drain_fully(SupervisorHarness& harness, int times = 4) {
    for (int i = 0; i < times; ++i) {
        harness.drain_actions();
    }
}

TEST(Errata97, T9SecondInstanceJoinsLastUsedLiveSession) {
    LaunchFixture fixture("ymh97join");
    const SessionId newer{"created-later"};
    const SessionId older{"used-last"};
    fixture.harness->drop_connection(fixture.workspace);
    fixture.harness->install_session_list_reply(
        nlohmann::json::array({launch_row(newer, 100, 2), launch_row(older, 200, 1)}), 0);

    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Attached, "test");
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->model().workspaces.at(fixture.workspace).activeSessionId(),
              older)
        << "the launch did not join the last-used live session (max updated_at_ms)";
    EXPECT_EQ(fixture.harness->submitted_count(std::string(protocol::method::kSessionCreate)),
              0u)
        << "joining a live session still created a new one";
}

TEST(Errata97, T9NoLiveSessionCreates) {
    LaunchFixture fixture("ymh97create");
    fixture.harness->drop_connection(fixture.workspace);
    fixture.harness->install_session_list_reply(nlohmann::json::array(), 0);

    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Attached, "test");
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->submitted_count(std::string(protocol::method::kSessionCreate)),
              1u)
        << "a launch with no live session did not create one";
}

TEST(Errata97, T9ForceNewCreatesEvenWithLiveSessions) {
    LaunchFixture fixture("ymh97forcenew", /*force_new=*/true);
    const SessionId live{"live-session"};
    fixture.harness->drop_connection(fixture.workspace);
    fixture.harness->install_session_list_reply(
        nlohmann::json::array({launch_row(live, 100, 1)}), 0);

    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Attached, "test");
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->submitted_count(std::string(protocol::method::kSessionCreate)),
              1u)
        << "--new did not force a fresh session";
    EXPECT_NE(fixture.harness->model().workspaces.at(fixture.workspace).activeSessionId(),
              live);
}

TEST(Errata97, T9ResumeWinsAndDeletingItCreatesInsteadOfJoining) {
    LaunchFixture fixture("ymh97resume");
    const SessionId resumed{"resumed-session"};
    const SessionId other_live{"other-live"};

    fixture.harness->drop_connection(fixture.workspace);
    fixture.harness->install_session_list_reply(
        nlohmann::json::array({launch_row(other_live, 100, 1)}), 0);
    fixture.harness->seed_pending_resume(fixture.workspace, resumed);
    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Attached, "test");
    drain_fully(*fixture.harness);

    // The resume attempt itself may fail on the dropped connection; drive the
    // success terminal directly (its seeding behavior is what T9 pins).
    fixture.harness->apply_resume_success(fixture.workspace, resumed);
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->model().workspaces.at(fixture.workspace).activeSessionId(),
              resumed);
    EXPECT_EQ(fixture.harness->submitted_count(std::string(protocol::method::kSessionCreate)),
              0u)
        << "a resume was replaced by a join/create";

    // Delete the resumed session and refresh: it must create, never join the
    // other live session.
    fixture.harness->mutable_model().eraseSession(fixture.workspace, resumed);
    fixture.harness->install_session_list_reply(
        nlohmann::json::array({launch_row(other_live, 100, 1)}), 0);
    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Attached, "test");
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->submitted_count(std::string(protocol::method::kSessionCreate)),
              1u)
        << "deleting the resumed session joined another live session";
    EXPECT_NE(fixture.harness->model().workspaces.at(fixture.workspace).activeSessionId(),
              other_live);
}

// ---- T10: a viewed-session prompt still opens exactly as before ------------

TEST(Errata97, T10ViewedPromptOpensWithForceAskSelection) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    request(adapter, kA, "p1", /*force_ask=*/true);

    ASSERT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.session, kA);
    EXPECT_EQ(model.dialog.request.value, "p1");
    EXPECT_EQ(model.dialog.tool, "shell");
    EXPECT_TRUE(model.dialog.force_ask);
    EXPECT_EQ(model.dialog.selected, 1);
    EXPECT_EQ(model.dialog.prev_mode, UiMode::Conversation);
    EXPECT_EQ(model.mode, UiMode::Dialog);
    EXPECT_TRUE(model.session(kA)->attention.needsInput);

    // Once/Deny dialog selects the non-destructive default.
    UiModel plain = make_model();
    UiEventAdapter plain_adapter(plain);
    request(plain_adapter, kA, "p2");
    ASSERT_TRUE(plain.dialog.open);
    EXPECT_EQ(plain.dialog.selected, 0);
}

// ---- T11: the drain tail opens without a key event --------------------------

TEST(Errata97, T11DrainTailOpensWhenTheViewedSessionChanges) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness =
        make_supervisor_harness(std::move(options));
    harness->seed_workspace(model_with_sessions(kW, {kA, kB}));
    harness->activate_session(kW, kA);

    PermissionRequested queued;
    queued.session = kB;
    queued.request = PermissionRequestId{"p1"};
    queued.tool    = "shell";
    queued.summary = "queued";
    harness->mutable_model().apply(UiEvent{queued});
    ASSERT_FALSE(harness->model().dialog.open);

    // Model-level activation changes the view but does not open by itself...
    harness->activate_session(kW, kB);
    EXPECT_FALSE(harness->model().dialog.open);

    // ...the dispatch tail does, with no key event.
    harness->drain_actions();
    ASSERT_TRUE(harness->model().dialog.open);
    EXPECT_EQ(harness->model().dialog.request.value, "p1");
    EXPECT_EQ(harness->model().dialog.prev_mode, UiMode::Conversation);
}

// ---- T12: a re-post is idempotent ------------------------------------------

TEST(Errata97, T12RepostDoesNotResetOrDuplicate) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);
    request(adapter, kA, "p1");
    ASSERT_TRUE(model.dialog.open);
    model.dialog.selected = 2;
    model.mode            = UiMode::Dialog;

    PermissionRequested repost;
    repost.session = kA;
    repost.request = PermissionRequestId{"p1"};
    repost.tool    = "shell";
    repost.summary = "again";
    model.apply(UiEvent{repost});

    EXPECT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.selected, 2) << "a re-post reset the user's selection";
    EXPECT_EQ(model.dialog.prev_mode, UiMode::Conversation);
    EXPECT_TRUE(model.session(kA)->pending_permissions.empty());

    // A queued id re-posted is stored once.
    request(adapter, kB, "p2");
    request(adapter, kB, "p2");
    ASSERT_EQ(model.session(kB)->pending_permissions.size(), 1u);
}

// ---- T14: cross-workspace isolation ----------------------------------------

TEST(Errata97, T14NonFocusedWorkspacePromptQueuesAndStaysPerSession) {
    UiModel model = make_model();   // focused workspace kW, session kA
    UiEventAdapter adapter(model);

    protocol::PermissionRequest request;
    request.request_id = "other-ws";
    request.session    = kB;
    request.tool       = "shell";
    request.summary    = "other workspace";
    request.arguments  = nlohmann::json::object();
    adapter.onPermissionRequest(kOtherW, request);

    EXPECT_FALSE(model.dialog.open);
    EXPECT_EQ(model.mode, UiMode::Conversation);
    ASSERT_TRUE(model.session(kB) != nullptr);
    EXPECT_EQ(model.session(kB)->workspace, kOtherW);
    ASSERT_TRUE(has_queue(model, kB, "other-ws"));
    EXPECT_TRUE(model.session(kB)->attention.needsInput);
    EXPECT_FALSE(model.session(kA)->attention.needsInput);
    // The synthetic status stays per-session.
    EXPECT_EQ(model.session(kB)->agent_state, AgentState::WaitingForPermission);
    EXPECT_NE(model.session(kA)->agent_state, AgentState::WaitingForPermission);

    // Focusing that workspace/session later opens the queued prompt.
    model.focusSessionIn(kOtherW, kB);
    EXPECT_FALSE(model.dialog.open);
    model.openPendingPermissionIfAny();
    ASSERT_TRUE(model.dialog.open);
    EXPECT_EQ(model.dialog.session, kB);
    EXPECT_EQ(model.dialog.request.value, "other-ws");
}

} // namespace

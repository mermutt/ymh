// 51-D4: exit moves to Ctrl+Q, and Ctrl+D deletes the highlighted switcher item
// behind a double-press confirmation. These drive the real private handlers
// through the additive `SupervisorHarness` seam (51-I19…I27, 51-F12…F22).

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/short_temp.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata51-ui"},
                          protocol::ClientRole::Supervisor};
}

RegistryConfig registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = 200ms;
    config.lock_retry_interval = 5ms;
    return config;
}

SupervisorWorkspace spec_for(const WorkspaceRecord& record, std::string boot_id) {
    SupervisorWorkspace spec;
    spec.id          = record.id;
    spec.cwd         = record.canonicalPath.string();
    spec.title       = record.displayTitle;
    spec.socket_path = (record.canonicalPath / ".ymh" / "host.sock").string();
    spec.boot_id     = std::move(boot_id);
    return spec;
}

WorkspaceModel live_workspace(const WorkspaceId& id, const std::string& title) {
    WorkspaceModel workspace;
    workspace.id           = id;
    workspace.title        = title;
    workspace.cwd          = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    return workspace;
}

SessionCell cell(const SessionId& id) {
    SessionCell session;
    session.id = id;
    return session;
}

SessionHistoryEntry entry(const SessionId& id) {
    SessionHistoryEntry record;
    record.id        = id;
    record.title     = id.value;
    record.kind      = "root";
    record.model     = "m";
    record.updatedAt = 1;
    return record;
}

WorkspaceHistory history(const WorkspaceId& workspace, const std::vector<SessionId>& sessions,
                         bool live) {
    WorkspaceHistory record;
    record.id            = workspace;
    record.title         = workspace.value;
    record.canonicalPath = "/" + workspace.value;
    record.live          = live;
    for (const SessionId& session : sessions) {
        record.sessions.push_back(entry(session));
    }
    return record;
}

void seed_loaded_catalog(UiModel& model, std::vector<WorkspaceHistory> histories) {
    model.catalog.workspaces = std::move(histories);
    model.catalog.loaded     = true;
    model.catalog.generation = 1;
}

void drain_fully(SupervisorHarness& harness, int times = 4) {
    for (int index = 0; index < times; ++index) {
        harness.drain_actions();
    }
}

bool has_notice(const SupervisorHarness& harness, const std::string& fragment) {
    return std::any_of(harness.model().notices.begin(), harness.model().notices.end(),
                       [&fragment](const UiNotice& notice) {
                           return notice.text.find(fragment) != std::string::npos;
                       });
}

bool has_cell(const SupervisorHarness& harness, const WorkspaceId& workspace,
              const SessionId& id) {
    const auto it = harness.model().workspaces.find(workspace);
    if (it == harness.model().workspaces.end()) {
        return false;
    }
    return std::any_of(it->second.sessions.begin(), it->second.sessions.end(),
                       [&id](const SessionCell& cell) { return cell.id == id; });
}

bool insert_pending_marker(const std::filesystem::path& db_path, const WorkspaceId& workspace) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
        return false;
    }
    const std::string sql =
        "INSERT INTO pending_mutation(workspace_id, mutation_type, payload, timestamp) "
        "VALUES('" +
        workspace.value + "', 'delete', '{}', 1)";
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close_v2(db);
    return result == SQLITE_OK;
}

// A live workspace "ws" with an active session s1 and a deletable session s2,
// backed by a stopped-but-attached connection, so the Ctrl+D paths run without a
// real daemon. Mirrors `D7Fixture` in `supervisor_harness_test.cpp`.
struct D4Fixture {
    explicit D4Fixture(const std::string& name)
        : root(name), registry(WorkspaceRegistry::open(registry_config(root.path()))) {
        std::filesystem::create_directories(root.path() / "ws");
        row       = registry->registerWorkspace(root.path() / "ws", "ws");
        workspace = row.id;

        SupervisorRunOptions options;
        options.registry = registry.get();
        options.identity = test_identity();
        harness          = make_supervisor_harness(std::move(options));

        WorkspaceModel model = live_workspace(workspace, "ws");
        model.sessions.push_back(cell(SessionId{"s1"}));
        model.sessions.push_back(cell(SessionId{"s2"}));
        harness->seed_active_workspace(model);
        harness->activate_session(workspace, SessionId{"s1"});

        harness->on_scan({spec_for(row, "boot-a")});
        drain_fully(*harness);
        harness->drop_connection(workspace);
        harness->install_session_list_reply(nlohmann::json::array(), 0);
        harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
        drain_fully(*harness);

        seed_loaded_catalog(harness->mutable_model(),
                            {history(workspace, {SessionId{"s1"}, SessionId{"s2"}}, true)});
    }

    void open_live_switcher() {
        harness->open_switcher();
        drain_fully(*harness);
    }

    void move_to_session(const SessionId& id) {
        for (int index = 0; index < 16; ++index) {
            const SwitcherCursor& cursor = harness->model().switcher.cursor;
            if (cursor.session.has_value() && *cursor.session == id) {
                return;
            }
            harness->dispatch_key("down");
        }
    }

    void open_history(const WorkspaceRecord& record, const std::vector<SessionId>& sessions,
                      bool live) {
        UiModel& model = harness->mutable_model();
        seed_loaded_catalog(model, {history(record.id, sessions, live)});
        model.switcher.source = SwitcherSource::History;
        model.switcher.openHistory(model);
        model.mode = UiMode::Switcher;
    }

    ShortTempRoot                      root;
    std::unique_ptr<WorkspaceRegistry> registry;
    WorkspaceRecord                    row;
    WorkspaceId                        workspace;
    std::unique_ptr<SupervisorHarness> harness;
};

} // namespace

// 51-I19: Ctrl+Q exits through `begin_exit(true)`; Ctrl+D no longer exits.
TEST(Errata51, UI51_D4_CtrlQExitsAndCtrlDDoesNot) {
    ShortTempRoot root("ymh_51_ctrlq");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness =
        make_supervisor_harness(std::move(options));
    harness->register_presence();

    harness->dispatch_key("ctrl-d");
    drain_fully(*harness);
    EXPECT_FALSE(harness->quit_requested()) << "Ctrl+D must no longer exit in Conversation mode";

    harness->dispatch_key("ctrl-q");
    drain_fully(*harness);
    EXPECT_TRUE(harness->quit_requested());
    EXPECT_FALSE(harness->model().exitConfirm.open);
    EXPECT_TRUE(registry->listSupervisors().empty());
}

// 51-I20: the delete target is the session when the cursor has one, else the
// workspace.
TEST(Errata51, UI51_D4_DeleteTargetSelection) {
    D4Fixture fixture("ymh_51_target");
    fixture.open_live_switcher();
    ASSERT_EQ(fixture.harness->model().mode, UiMode::Switcher);
    ASSERT_FALSE(fixture.harness->model().switcher.cursor.session.has_value());

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->model().switcher.delete_target.has_value());
    EXPECT_FALSE(fixture.harness->model().switcher.delete_target->session.has_value());

    ASSERT_TRUE(fixture.harness->dispatch_key("down"));
    ASSERT_TRUE(fixture.harness->model().switcher.cursor.session.has_value());
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->model().switcher.delete_target.has_value());
    ASSERT_TRUE(fixture.harness->model().switcher.delete_target->session.has_value());
    EXPECT_EQ(*fixture.harness->model().switcher.delete_target->session, SessionId{"s2"});
}

// 51-I21: a first Ctrl+D arms; a second within the window confirms the delete.
TEST(Errata51, UI51_D4_ArmThenConfirm) {
    D4Fixture fixture("ymh_51_arm");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 1u);
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
}

// 51-I21 (negative): a single Ctrl+D never deletes.
TEST(Errata51, UI51_D4_SinglePressDoesNotDelete) {
    D4Fixture fixture("ymh_51_single");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    EXPECT_TRUE(has_cell(*fixture.harness, fixture.workspace, SessionId{"s2"}));
}

// 51-I21 / 51-F12: an expired arm is disarmed on the periodic tick.
TEST(Errata51, UI51_D4_ArmExpiresOnTick) {
    D4Fixture fixture("ymh_51_expire");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    fixture.harness->mutable_model().switcher.delete_armed_at =
        std::chrono::steady_clock::now() - 4s;
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);
}

// 51-I21 / 51-F13: any other key disarms; nothing is deleted.
TEST(Errata51, UI51_D4_DisarmOnOtherKey) {
    D4Fixture fixture("ymh_51_disarm");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);

    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
    EXPECT_FALSE(fixture.harness->model().switcher.delete_target.has_value());
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);
}

// 51-I21 / 51-M1/M2: the arm is bound to the exact target, so a cursor move can
// never redirect a confirmed delete.
TEST(Errata51, UI51_D4_ArmBoundToTarget) {
    D4Fixture fixture("ymh_51_bound");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->model().switcher.delete_target.has_value());
    ASSERT_EQ(*fixture.harness->model().switcher.delete_target->session, SessionId{"s2"});

    // Redirect the cursor behind the arm's back (no key -> no disarm).
    fixture.harness->mutable_model().switcher.cursor.session.reset();
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u)
        << "a moved cursor must not delete the armed target";
    ASSERT_TRUE(fixture.harness->model().switcher.delete_target.has_value());
    EXPECT_FALSE(fixture.harness->model().switcher.delete_target->session.has_value())
        << "the arm re-bound to the live cursor";
}

// 51-I21: open/openHistory/close reset the arm.
TEST(Errata51, UI51_D4_ArmResetOnOpenClose) {
    D4Fixture fixture("ymh_51_reset");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    fixture.harness->mutable_model().switcher.close();
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
    EXPECT_FALSE(fixture.harness->model().switcher.delete_target.has_value());

    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    fixture.harness->mutable_model().switcher.open(fixture.harness->model());
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    fixture.harness->mutable_model().switcher.openHistory(fixture.harness->model());
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
}

// 51-I22: a session delete is a `session.delete` RPC to the owning daemon with
// only_if_empty=false, force=false; the supervisor performs no store write.
TEST(Errata51, UI51_D4_SessionDeleteViaRpc) {
    D4Fixture fixture("ymh_51_rpc");
    fixture.registry->addSession(fixture.workspace, SessionId{"s2"});
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    ASSERT_EQ(fixture.harness->submitted_count("session.delete"), 1u);
    const std::optional<nlohmann::json> params =
        fixture.harness->last_submitted_params("session.delete");
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->value("session", std::string{}), "s2");
    EXPECT_EQ(params->value("confirm", false), true);
    EXPECT_EQ(params->value("only_if_empty", true), false);
    EXPECT_EQ(params->value("force", true), false);
    // The supervisor only removed its own UI state; the registry is untouched.
    EXPECT_FALSE(has_cell(*fixture.harness, fixture.workspace, SessionId{"s2"}));
    EXPECT_TRUE(fixture.registry->findSession(fixture.workspace, SessionId{"s2"}).has_value());
}

// 51-I22 / 51-F14: a daemon DependentSession refusal surfaces a notice.
TEST(Errata51, UI51_D4_RefuseDependentSession) {
    D4Fixture fixture("ymh_51_dep");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_error_reply(
        std::string(protocol::method::kSessionDelete),
        static_cast<int>(protocol::AppCode::DependentSession), "has dependent children");

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(has_notice(*fixture.harness, "dependent"));
    EXPECT_TRUE(has_cell(*fixture.harness, fixture.workspace, SessionId{"s2"}));
}

// 51-I22 / 51-F15: force=false preserves the daemon's active-session refusal.
TEST(Errata51, UI51_D4_RefuseLiveSession) {
    D4Fixture fixture("ymh_51_live");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_error_reply(
        std::string(protocol::method::kSessionDelete),
        static_cast<int>(protocol::RpcCode::InvalidParams), "active session");

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(has_notice(*fixture.harness, "active session"));
}

// 51-I22 / 51-F19: a mid-turn refusal surfaces a notice.
TEST(Errata51, UI51_D4_RefuseTurnInProgress) {
    D4Fixture fixture("ymh_51_turn");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_error_reply(
        std::string(protocol::method::kSessionDelete),
        static_cast<int>(protocol::RpcCode::InvalidParams), "turn in progress");

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(has_notice(*fixture.harness, "turn in progress"));
}

// 51-I22 / 51-F21: an already-gone session surfaces a distinct notice.
TEST(Errata51, UI51_D4_RefuseUnknownSession) {
    D4Fixture fixture("ymh_51_unknown");
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s2"});
    fixture.harness->install_method_error_reply(
        std::string(protocol::method::kSessionDelete),
        static_cast<int>(protocol::AppCode::UnknownSession), "already gone");

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(has_notice(*fixture.harness, "already gone"));
}

// 51-I22 / 51-F22: a History session in a non-running workspace is refused.
TEST(Errata51, UI51_D4_RefuseNoDaemon) {
    D4Fixture fixture("ymh_51_nodaemon");
    std::filesystem::create_directories(fixture.root.path() / "other");
    const WorkspaceRecord other =
        fixture.registry->registerWorkspace(fixture.root.path() / "other", "other");
    fixture.open_history(other, {SessionId{"h1"}}, false);
    ASSERT_EQ(fixture.harness->model().switcher.cursor.workspace, other.id);

    ASSERT_TRUE(fixture.harness->dispatch_key("down"));
    ASSERT_TRUE(fixture.harness->model().switcher.cursor.session.has_value());
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_TRUE(has_notice(*fixture.harness, "workspace not running"));
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);
}

// 51-I23 / 51-L3: the workspace confirmation states the junction-row count and
// the cascade removes every junction and the workspace row.
TEST(Errata51, UI51_D4_WorkspaceCascadeCount) {
    D4Fixture fixture("ymh_51_count");
    std::filesystem::create_directories(fixture.root.path() / "stored");
    const WorkspaceRecord stored =
        fixture.registry->registerWorkspace(fixture.root.path() / "stored", "stored");
    fixture.registry->addSession(stored.id, SessionId{"a"});
    fixture.registry->addSession(stored.id, SessionId{"b"});
    fixture.registry->addSession(stored.id, SessionId{"c"});
    fixture.open_history(stored, {SessionId{"a"}, SessionId{"b"}, SessionId{"c"}}, false);
    ASSERT_EQ(fixture.harness->model().switcher.cursor.workspace, stored.id);
    ASSERT_FALSE(fixture.harness->model().switcher.cursor.session.has_value());

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    EXPECT_EQ(fixture.harness->model().switcher.delete_target_session_count, 3u);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_FALSE(fixture.registry->findById(stored.id).has_value());
    EXPECT_TRUE(fixture.registry->listSessions(stored.id).empty());
    EXPECT_TRUE(fixture.harness->model().switcher.workspaces.empty());
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
}

// 51-I23 / 51-F16: a Live-source workspace delete is always refused.
TEST(Errata51, UI51_D4_RefuseLiveWorkspace) {
    D4Fixture fixture("ymh_51_livews");
    fixture.open_live_switcher();
    ASSERT_FALSE(fixture.harness->model().switcher.cursor.session.has_value());

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_TRUE(has_notice(*fixture.harness, "stop the workspace first"));
    EXPECT_TRUE(fixture.registry->findById(fixture.workspace).has_value());
}

// 51-I23 / 51-F20: a junction that reappears before the confirm makes
// `removeWorkspace` refuse with a notice and no workspace deletion.
TEST(Errata51, UI51_D4_RefuseNonEmptyWorkspace) {
    D4Fixture fixture("ymh_51_nonempty");
    std::filesystem::create_directories(fixture.root.path() / "stored");
    const WorkspaceRecord stored =
        fixture.registry->registerWorkspace(fixture.root.path() / "stored", "stored");
    fixture.open_history(stored, {}, false);
    ASSERT_EQ(fixture.harness->model().switcher.cursor.workspace, stored.id);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    EXPECT_EQ(fixture.harness->model().switcher.delete_target_session_count, 0u);
    fixture.registry->addSession(stored.id, SessionId{"late"});

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_TRUE(has_notice(*fixture.harness, "not empty"));
    EXPECT_TRUE(fixture.registry->findById(stored.id).has_value());
}

// 51-I25 / 51-F17: a pending registry mutation marker surfaces a notice and
// leaves the workspace in place.
TEST(Errata51, UI51_D4_RefuseMutationInProgress) {
    D4Fixture fixture("ymh_51_mutation");
    std::filesystem::create_directories(fixture.root.path() / "stored");
    const WorkspaceRecord stored =
        fixture.registry->registerWorkspace(fixture.root.path() / "stored", "stored");
    fixture.registry->addSession(stored.id, SessionId{"a"});
    ASSERT_TRUE(insert_pending_marker(fixture.root.path() / ".state" / "ymh" / "registry.db",
                                      stored.id));
    fixture.open_history(stored, {SessionId{"a"}}, false);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_TRUE(has_notice(*fixture.harness, "mutation"));
    EXPECT_TRUE(fixture.registry->findById(stored.id).has_value());
}

// 51-I24: the workspace's on-disk `.ymh/` (including sessions.db) survives.
TEST(Errata51, UI51_D4_WorkspaceOnDiskUntouched) {
    D4Fixture fixture("ymh_51_ondisk");
    std::filesystem::create_directories(fixture.root.path() / "stored" / ".ymh");
    const std::filesystem::path sessions_db =
        fixture.root.path() / "stored" / ".ymh" / "sessions.db";
    {
        std::ofstream(sessions_db) << "not empty";
    }
    const WorkspaceRecord stored =
        fixture.registry->registerWorkspace(fixture.root.path() / "stored", "stored");
    fixture.registry->addSession(stored.id, SessionId{"a"});
    fixture.open_history(stored, {SessionId{"a"}}, false);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_FALSE(fixture.registry->findById(stored.id).has_value());
    EXPECT_TRUE(std::filesystem::exists(sessions_db));
}

// 51-I27 / 51-M4: after deleting a session the cursor moves to the previous
// session and no stale node remains.
TEST(Errata51, UI51_D4_PostDeleteClamp) {
    D4Fixture fixture("ymh_51_clamp");
    fixture.harness->mutable_model()
        .workspaces[fixture.workspace]
        .sessions.push_back(cell(SessionId{"s3"}));
    seed_loaded_catalog(fixture.harness->mutable_model(),
                        {history(fixture.workspace,
                                 {SessionId{"s1"}, SessionId{"s2"}, SessionId{"s3"}}, true)});
    fixture.open_live_switcher();
    fixture.move_to_session(SessionId{"s3"});
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->model().sessions.count(SessionId{"s3"}), 0u);
    ASSERT_TRUE(fixture.harness->model().switcher.cursor.session.has_value());
    EXPECT_EQ(*fixture.harness->model().switcher.cursor.session, SessionId{"s2"});
}

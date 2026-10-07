// 81-D1…D13 (spec 81): the full-screen session dashboard. Pure-model tests
// (81-D2/D5/D6/D7) and hermetic key-sequence tests driven through the real
// private handlers via the `SupervisorHarness` seam (81-D3/D4/D8/D12/D13).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <nlohmann/json.hpp>

#include "support/short_temp.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

const SessionId kS1{"s1"};
const SessionId kS2{"s2"};

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata81-ui"},
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

void drain_fully(SupervisorHarness& harness, int times = 4) {
    for (int index = 0; index < times; ++index) {
        harness.drain_actions();
    }
}

// A live workspace "ws" with an active session s1 and a second session s2,
// backed by a stopped-but-attached connection so the key paths run without a
// real daemon. Mirrors `SwitcherFixture` in `errata57_ui_test.cpp`.
struct DashboardFixture {
    explicit DashboardFixture(const std::string& name)
        : root(name), registry(WorkspaceRegistry::open(registry_config(root.path()))) {
        std::filesystem::create_directories(root.path() / "ws");
        row       = registry->registerWorkspace(root.path() / "ws", "ws");
        workspace = row.id;

        SupervisorRunOptions options;
        options.registry = registry.get();
        options.identity = test_identity();
        harness          = make_supervisor_harness(std::move(options));

        WorkspaceModel model = live_workspace(workspace, "ws");
        model.sessions.push_back(cell(kS1));
        model.sessions.push_back(cell(kS2));
        harness->seed_active_workspace(model);
        harness->activate_session(workspace, kS1);

        harness->on_scan({spec_for(row, "boot-a")});
        drain_fully(*harness);
        harness->drop_connection(workspace);
        harness->install_session_list_reply(nlohmann::json::array(), 0);
        harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
        drain_fully(*harness);

        seed_loaded_catalog(harness->mutable_model(), {history(workspace, {kS1, kS2}, true)});
    }

    static void seed_loaded_catalog(UiModel& model, std::vector<WorkspaceHistory> histories) {
        model.catalog.workspaces = std::move(histories);
        model.catalog.loaded     = true;
        model.catalog.generation = 1;
    }

    ShortTempRoot                      root;
    std::unique_ptr<WorkspaceRegistry> registry;
    WorkspaceRecord                    row;
    WorkspaceId                        workspace;
    std::unique_ptr<SupervisorHarness> harness;
};

// ---- pure-model helpers (81-D2/D5/D6/D7) -----------------------------------

SessionUiState state_with(AgentState state, bool completed) {
    SessionUiState ui;
    ui.agent_state         = state;
    ui.attention.completed = completed;
    return ui;
}

UiModel two_session_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    WorkspaceModel workspace = live_workspace(WorkspaceId{"ws"}, "ws");
    workspace.cwd            = "/ws";
    workspace.sessions       = {cell(kS1), cell(kS2)};
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, kS1);
    model.catalog.workspaces = {history(workspace.id, {kS1, kS2}, true)};
    model.catalog.loaded     = true;
    model.catalog.generation = 1;
    return model;
}

// ---- 81-D5: status vocabulary ----------------------------------------------

TEST(Errata81, StatusMappingTable) {
    SessionCell idle;
    idle.state = AgentState::Idle;
    EXPECT_EQ(dashboard_status(idle, nullptr), DashboardStatus::Idle);
    SessionUiState done = state_with(AgentState::Idle, true);
    EXPECT_EQ(dashboard_status(idle, &done), DashboardStatus::Completed);

    SessionCell thinking;
    thinking.state = AgentState::Thinking;
    EXPECT_EQ(dashboard_status(thinking, nullptr), DashboardStatus::Working);
    SessionCell calling;
    calling.state = AgentState::CallingTool;
    EXPECT_EQ(dashboard_status(calling, nullptr), DashboardStatus::Working);
    SessionCell cancelling;
    cancelling.state = AgentState::Cancelling;
    EXPECT_EQ(dashboard_status(cancelling, nullptr), DashboardStatus::Working);

    SessionCell permission;
    permission.state     = AgentState::WaitingForPermission;
    permission.attention = true;
    EXPECT_EQ(dashboard_status(permission, nullptr), DashboardStatus::NeedsInput);
    SessionCell input;
    input.state     = AgentState::WaitingForInput;
    input.attention = true;
    EXPECT_EQ(dashboard_status(input, nullptr), DashboardStatus::NeedsInput);

    SessionCell error;
    error.state     = AgentState::Error;
    error.attention = true;
    EXPECT_EQ(dashboard_status(error, nullptr), DashboardStatus::Failed);

    SessionCell idle_working;
    SessionUiState subagent;
    subagent.subagents.agents.push_back(
        SubagentView{SessionId{"child"}, "task", AgentState::Idle, SubagentStatus::Running});
    EXPECT_EQ(dashboard_status(idle_working, &subagent), DashboardStatus::Working);
}

TEST(Errata81, GroupBuckets) {
    EXPECT_EQ(dashboard_group(DashboardStatus::NeedsInput), DashboardGroup::NeedsInput);
    EXPECT_EQ(dashboard_group(DashboardStatus::Working), DashboardGroup::Working);
    EXPECT_EQ(dashboard_group(DashboardStatus::Completed), DashboardGroup::Completed);
    EXPECT_EQ(dashboard_group(DashboardStatus::Failed), DashboardGroup::Completed);
    EXPECT_EQ(dashboard_group(DashboardStatus::Idle), DashboardGroup::Completed);
    EXPECT_EQ(dashboard_group(DashboardStatus::Stopped), DashboardGroup::Completed);
    EXPECT_EQ(std::string(dashboard_status_glyph(DashboardStatus::Stopped)), "-");
}

// ---- 81-D13: overlay prior-mode reducer ------------------------------------

TEST(Errata81, OverlayPrevModeRestores) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    WorkspaceModel workspace = live_workspace(WorkspaceId{"ws"}, "ws");
    model.workspaces.emplace(workspace.id, workspace);
    model.mode = UiMode::Dashboard;

    UiEventAdapter adapter(model);
    PermissionRequest request;
    request.session = kS1;
    adapter.onPermissionRequest(kS1, PermissionRequestId{"p1"}, request);
    EXPECT_EQ(model.dialog.prev_mode, UiMode::Dashboard);
    EXPECT_EQ(model.mode, UiMode::Dialog);

    adapter.onPermissionResolved(kS1, PermissionRequestId{"p1"},
                                 payload::PermissionDecisionKind::Allow);
    EXPECT_EQ(model.mode, UiMode::Dashboard);
    EXPECT_FALSE(model.dialog.open);
}

// ---- 81-D2/D6/D7: model lifecycle ------------------------------------------

TEST(Errata81, OpenSavesPrevModeAndCursor) {
    UiModel model = two_session_model();
    model.openDashboard();
    EXPECT_EQ(model.mode, UiMode::Dashboard);
    EXPECT_EQ(model.dashboard.prev_mode, UiMode::Conversation);
    ASSERT_EQ(model.dashboard.rows.size(), 2u);
    EXPECT_EQ(model.dashboard.rows[model.dashboard.cursor].session, kS1);
}

TEST(Errata81, CloseRestoresModeAndClears) {
    UiModel model = two_session_model();
    model.openDashboard();
    model.closeDashboard();
    EXPECT_EQ(model.mode, UiMode::Conversation);
    EXPECT_FALSE(model.dashboard.open);
    EXPECT_TRUE(model.dashboard.rows.empty());
    EXPECT_EQ(model.dashboard.cursor, 0u);
}

TEST(Errata81, RebuildPreservesCursor) {
    UiModel model = two_session_model();
    model.openDashboard();
    model.dashboard.moveDown();
    ASSERT_EQ(model.dashboard.rows[model.dashboard.cursor].session, kS2);

    model.workspaces[WorkspaceId{"ws"}].sessions[1].state = AgentState::Thinking;
    model.dashboard.rebuild(model);
    ASSERT_EQ(model.dashboard.rows.size(), 2u);
    EXPECT_EQ(model.dashboard.rows[model.dashboard.cursor].session, kS2);
    EXPECT_EQ(model.dashboard.rows[model.dashboard.cursor].status, DashboardStatus::Working);
}

TEST(Errata81, CollapseToggle) {
    UiModel model = two_session_model();
    model.openDashboard();
    const DashboardGroup group = model.dashboard.rows[model.dashboard.cursor].group;
    model.dashboard.toggle_collapse();
    EXPECT_NE(model.dashboard.collapsed.find(group), model.dashboard.collapsed.end());
    model.dashboard.toggle_collapse();
    EXPECT_EQ(model.dashboard.collapsed.find(group), model.dashboard.collapsed.end());
}

TEST(Errata81, LiveOnlyUniverse) {
    UiModel model = two_session_model();
    WorkspaceModel hidden = live_workspace(WorkspaceId{"ws-hidden"}, "hidden");
    hidden.live           = false;
    hidden.sessions       = {cell(SessionId{"hidden-session"})};
    model.workspaces.emplace(hidden.id, hidden);
    model.openDashboard();
    for (const DashboardRow& row : model.dashboard.rows) {
        EXPECT_NE(row.workspace, hidden.id);
    }
}

TEST(Errata81, FocusedSessionIncluded) {
    UiModel model = two_session_model();
    model.openDashboard();
    bool focused_present = false;
    for (const DashboardRow& row : model.dashboard.rows) {
        if (row.session == kS1) {
            focused_present = true;
        }
    }
    EXPECT_TRUE(focused_present);
}

TEST(Errata81, Counts) {
    UiModel model = two_session_model();
    model.openDashboard();
    EXPECT_EQ(model.dashboard.counts().completed, 2u);
    EXPECT_EQ(model.dashboard.counts().needs_input, 0u);

    model.workspaces[WorkspaceId{"ws"}].sessions[1].state = AgentState::WaitingForInput;
    model.workspaces[WorkspaceId{"ws"}].sessions[1].attention = true;
    model.dashboard.rebuild(model);
    EXPECT_EQ(model.dashboard.counts().needs_input, 1u);
    EXPECT_EQ(model.dashboard.counts().completed, 1u);
}

TEST(Errata81, OrderWithinGroup) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws-root"};
    model.cwdWorkspacePath  = "/root";
    const auto add = [&model](const char* id, const char* title, const char* cwd,
                              const char* session_title) {
        WorkspaceModel workspace = live_workspace(WorkspaceId{id}, title);
        workspace.cwd            = cwd;
        SessionCell session;
        session.id    = SessionId{std::string{id} + "-s"};
        session.title = session_title;
        workspace.sessions.push_back(session);
        model.workspaces.emplace(workspace.id, workspace);
    };
    add("ws-root", "zeta", "/root", "one");
    add("ws-a", "alpha", "/a", "one");
    add("ws-b", "alpha", "/b", "two");
    model.focusSessionIn(WorkspaceId{"ws-root"}, SessionId{"ws-root-s"});

    const auto add_history = [&model](const char* id, const char* session_title,
                                      std::int64_t last_used) {
        WorkspaceHistory record = history(WorkspaceId{id}, {SessionId{std::string{id} + "-s"}}, true);
        record.lastUsedAt       = last_used;
        record.sessions[0].title = session_title;
        model.catalog.workspaces.push_back(std::move(record));
    };
    add_history("ws-root", "one", 0);
    add_history("ws-a", "one", 100);
    add_history("ws-b", "two", 100);
    model.catalog.loaded     = true;
    model.catalog.generation = 1;

    model.openDashboard();
    ASSERT_EQ(model.dashboard.rows.size(), 3u);
    EXPECT_EQ(model.dashboard.rows[0].workspace, WorkspaceId{"ws-root"});
    EXPECT_EQ(model.dashboard.rows[1].workspace, WorkspaceId{"ws-a"});
    EXPECT_EQ(model.dashboard.rows[2].workspace, WorkspaceId{"ws-b"});
}

// ---- 81-D3/D4/D8/D12/D13: key sequences ------------------------------------

TEST(Errata81, ArrowLeftEmptyOpensDashboard) {
    DashboardFixture fixture("ymh_81_arrowleft");
    EXPECT_TRUE(fixture.harness->dispatch_key("left"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
}

TEST(Errata81, ArrowLeftNonEmptyMovesCursor) {
    DashboardFixture fixture("ymh_81_arrowleft_draft");
    fixture.harness->dispatch_event(ftxui::Event::Character("a"));
    EXPECT_TRUE(fixture.harness->dispatch_key("left"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    const SessionUiState* state = fixture.harness->model().session(kS1);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->input.draft, "a");
    EXPECT_EQ(state->input.cursor, 0u);
}

TEST(Errata81, ArrowLeftInSubagentViewNoOpen) {
    DashboardFixture fixture("ymh_81_arrowleft_child");
    fixture.harness->mutable_model().subagent_path.push_back(SessionId{"child"});
    EXPECT_TRUE(fixture.harness->dispatch_key("left"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    EXPECT_EQ(fixture.harness->model().subagent_path.size(), 1u);
}

TEST(Errata81, ArrowLeftWithDialogNoOpen) {
    DashboardFixture fixture("ymh_81_arrowleft_dialog");
    fixture.harness->open_permission_dialog(kS1, PermissionRequestId{"p1"}, "shell", "rm");
    EXPECT_TRUE(fixture.harness->dispatch_key("left"));
    EXPECT_TRUE(fixture.harness->model().dialog.open);
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dialog);
}

TEST(Errata81, CtrlSOpensDashboard) {
    DashboardFixture fixture("ymh_81_ctrls");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
}

TEST(Errata81, CtrlPOpensDashboard) {
    DashboardFixture fixture("ymh_81_ctrlp");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-p"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
}

TEST(Errata81, CtrlSNotPopup) {
    DashboardFixture fixture("ymh_81_ctrls_notpopup");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
    EXPECT_NE(fixture.harness->model().mode, UiMode::Switcher);
}

TEST(Errata81, CursorKeysMove) {
    DashboardFixture fixture("ymh_81_cursor");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    ASSERT_EQ(fixture.harness->model().dashboard.rows.size(), 2u);
    const std::size_t start = fixture.harness->model().dashboard.cursor;
    fixture.harness->dispatch_key("down");
    EXPECT_GT(fixture.harness->model().dashboard.cursor, start);
    fixture.harness->dispatch_key("j");
    EXPECT_EQ(fixture.harness->model().dashboard.cursor, 1u);
    fixture.harness->dispatch_key("up");
    EXPECT_EQ(fixture.harness->model().dashboard.cursor, 0u);
    fixture.harness->dispatch_key("k");
    EXPECT_EQ(fixture.harness->model().dashboard.cursor, 0u);
}

TEST(Errata81, PageHomeEndClamp) {
    DashboardFixture fixture("ymh_81_page");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("page-down");
    EXPECT_LT(fixture.harness->model().dashboard.cursor,
              fixture.harness->model().dashboard.rows.size());
    fixture.harness->dispatch_event(ftxui::Event::End);
    EXPECT_EQ(fixture.harness->model().dashboard.cursor,
              fixture.harness->model().dashboard.rows.size() - 1);
    fixture.harness->dispatch_key("page-up");
    fixture.harness->dispatch_event(ftxui::Event::Home);
    EXPECT_EQ(fixture.harness->model().dashboard.cursor, 0u);
}

TEST(Errata81, EnterAttachesFocusOnly) {
    DashboardFixture fixture("ymh_81_enter");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("down");
    ASSERT_EQ(fixture.harness->model().dashboard.rows[fixture.harness->model().dashboard.cursor]
                  .session,
              kS2);
    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    EXPECT_EQ(fixture.harness->mutable_model().activeWorkspace()->activeSessionId(), kS2);
    EXPECT_EQ(fixture.harness->submitted_count("session.activate"), 0u);
}

TEST(Errata81, RightAttachesLikeEnter) {
    DashboardFixture fixture("ymh_81_right");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("down");
    EXPECT_TRUE(fixture.harness->dispatch_key("right"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    EXPECT_EQ(fixture.harness->mutable_model().activeWorkspace()->activeSessionId(), kS2);
}

TEST(Errata81, EscRestoresWithoutAttach) {
    DashboardFixture fixture("ymh_81_esc");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("down");
    const bool following = fixture.harness->model().session(kS1)->scroll.following;
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    EXPECT_FALSE(fixture.harness->model().dashboard.open);
    EXPECT_EQ(fixture.harness->mutable_model().activeWorkspace()->activeSessionId(), kS1);
    EXPECT_EQ(fixture.harness->model().session(kS1)->scroll.following, following);
}

TEST(Errata81, DoubleCtrlCArmsThenExits) {
    DashboardFixture fixture("ymh_81_double_ctrlc");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_EQ(fixture.harness->model().dashboard.exit_arm, EscArm::Armed);
    EXPECT_FALSE(fixture.harness->model().exitConfirm.open);
    EXPECT_FALSE(fixture.harness->quit_requested());

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.harness->quit_requested() ||
                fixture.harness->model().exitConfirm.open);
}

TEST(Errata81, SingleCtrlCNoExitNoCancel) {
    DashboardFixture fixture("ymh_81_single_ctrlc");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(fixture.harness->model().exitConfirm.open);
    EXPECT_EQ(fixture.harness->cancel_count(), 0u);
    EXPECT_EQ(fixture.harness->model().dashboard.exit_arm, EscArm::Armed);
}

TEST(Errata81, CtrlQConsumedInDashboard) {
    DashboardFixture fixture("ymh_81_ctrlq");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(fixture.harness->model().exitConfirm.open);
}

TEST(Errata81, ArrowLeftInDashboardConsumed) {
    DashboardFixture fixture("ymh_81_arrowleft_consumed");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    const std::size_t rows_before = fixture.harness->model().dashboard.rows.size();
    const std::size_t cursor_before = fixture.harness->model().dashboard.cursor;
    EXPECT_TRUE(fixture.harness->dispatch_key("left"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
    EXPECT_EQ(fixture.harness->model().dashboard.rows.size(), rows_before);
    EXPECT_EQ(fixture.harness->model().dashboard.cursor, cursor_before);
}

TEST(Errata81, ExitArmDisarmedByMove) {
    DashboardFixture fixture("ymh_81_arm_move");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_EQ(fixture.harness->model().dashboard.exit_arm, EscArm::Armed);
    EXPECT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_EQ(fixture.harness->model().dashboard.exit_arm, EscArm::Disarmed);
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(fixture.harness->model().exitConfirm.open);
}

TEST(Errata81, ComposerDraftSurvives) {
    DashboardFixture fixture("ymh_81_draft");
    fixture.harness->dispatch_event(ftxui::Event::Character("h"));
    fixture.harness->dispatch_event(ftxui::Event::Character("i"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    const SessionUiState* state = fixture.harness->model().session(kS1);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->input.draft, "hi");
}

TEST(Errata81, EmptyDashboardInline) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    EXPECT_TRUE(harness->dispatch_key("ctrl-s"));
    EXPECT_EQ(harness->model().mode, UiMode::Dashboard);
    EXPECT_TRUE(harness->model().dashboard.rows.empty());
}

TEST(Errata81, RebuildOnCatalogDelivery) {
    DashboardFixture fixture("ymh_81_rebuild");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("down");
    ASSERT_EQ(fixture.harness->model().dashboard.rows[fixture.harness->model().dashboard.cursor]
                  .session,
              kS2);

    fixture.harness->mutable_model().workspaces[fixture.workspace].sessions[1].state =
        AgentState::Thinking;
    SessionCatalogSnapshot snapshot;
    snapshot.workspaces = {history(fixture.workspace, {kS1, kS2}, true)};
    snapshot.generation = 2;
    fixture.harness->on_catalog_snapshot(std::move(snapshot));

    ASSERT_EQ(fixture.harness->model().dashboard.rows.size(), 2u);
    EXPECT_EQ(fixture.harness->model().dashboard.rows[fixture.harness->model().dashboard.cursor]
                  .session,
              kS2);
    EXPECT_EQ(fixture.harness->model().dashboard.rows[fixture.harness->model().dashboard.cursor]
                  .status,
              DashboardStatus::Working);
}

TEST(Errata81, CursorSurvivesResize) {
    DashboardFixture fixture("ymh_81_resize");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("down");
    const std::size_t cursor = fixture.harness->model().dashboard.cursor;
    const std::size_t rows   = fixture.harness->model().dashboard.rows.size();

    static_cast<void>(render_to_ansi(fixture.harness->model(), TerminalSize{40, 10}, Theme{false}));
    static_cast<void>(render_to_ansi(fixture.harness->model(), TerminalSize{120, 40}, Theme{false}));

    EXPECT_EQ(fixture.harness->model().dashboard.cursor, cursor);
    EXPECT_EQ(fixture.harness->model().dashboard.rows.size(), rows);
}

TEST(Errata81, ExitConfirmCancelReturnsToDashboard) {
    DashboardFixture fixture("ymh_81_exit_cancel");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("down");
    fixture.harness->mutable_model().dashboard.collapsed.insert(DashboardGroup::Completed);
    const std::size_t cursor_before = fixture.harness->model().dashboard.cursor;

    fixture.harness->open_exit_prompt({fixture.workspace});
    EXPECT_TRUE(fixture.harness->model().exitConfirm.open);
    EXPECT_EQ(fixture.harness->model().mode, UiMode::ExitConfirm);
    EXPECT_EQ(fixture.harness->model().exitConfirm.prev_mode, UiMode::Dashboard);
    EXPECT_TRUE(fixture.harness->model().dashboard.open);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
    EXPECT_TRUE(fixture.harness->model().dashboard.open);
    EXPECT_EQ(fixture.harness->model().dashboard.cursor, cursor_before);
    EXPECT_EQ(fixture.harness->model().dashboard.rows.size(), 2u);
    EXPECT_EQ(fixture.harness->model().dashboard.collapsed.size(), 1u);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    EXPECT_FALSE(fixture.harness->model().dashboard.open);
}

TEST(Errata81, ExitConfirmConfirmQuits) {
    DashboardFixture fixture("ymh_81_exit_confirm");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->dispatch_key("ctrl-c");
    fixture.harness->dispatch_key("ctrl-c");
    drain_fully(*fixture.harness);
    if (fixture.harness->model().exitConfirm.open) {
        EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
        drain_fully(*fixture.harness);
    }
    EXPECT_TRUE(fixture.harness->quit_requested());
}

TEST(Errata81, PermissionDialogOverDashboardRestores) {
    DashboardFixture fixture("ymh_81_dialog_over");
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-s"));
    fixture.harness->open_permission_dialog(kS1, PermissionRequestId{"p1"}, "shell", "rm");
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dialog);
    EXPECT_EQ(fixture.harness->model().dialog.prev_mode, UiMode::Dashboard);
    EXPECT_TRUE(fixture.harness->model().dashboard.open);

    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
    EXPECT_TRUE(fixture.harness->model().dashboard.open);
}

TEST(Errata81, HarnessOpenSwitcherOpensDashboard) {
    DashboardFixture fixture("ymh_81_hook");
    fixture.harness->open_switcher();
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Dashboard);
    EXPECT_NE(fixture.harness->model().mode, UiMode::Switcher);
}

} // namespace

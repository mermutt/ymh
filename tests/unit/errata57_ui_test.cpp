// 57-D3/D4 (57-H1…H4): popup key ownership (Ctrl+Q inside a popup is a consumed
// no-op) and the inline two-press delete confirmation, driven through the real
// private handlers via the `SupervisorHarness` seam.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
    return AttachIdentity{protocol::ClientInstanceId{"errata57-ui"},
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

// A live workspace "ws" with an active session s1 and a deletable session s2,
// backed by a stopped-but-attached connection, so the Ctrl+D paths run without a
// real daemon. Mirrors `D4Fixture` in `errata51_ui_test.cpp`.
struct SwitcherFixture {
    explicit SwitcherFixture(const std::string& name)
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

    ShortTempRoot                      root;
    std::unique_ptr<WorkspaceRegistry> registry;
    WorkspaceRecord                    row;
    WorkspaceId                        workspace;
    std::unique_ptr<SupervisorHarness> harness;
};

} // namespace

// 57-H1 (57-I10/57-D4): while the Switcher (either source) is open, Ctrl+Q is
// consumed and does not exit.
TEST(Errata57, CtrlQInSwitcherDoesNotExit) {
    SwitcherFixture fixture("ymh_57_ctrlq");
    fixture.harness->open_switcher();
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.harness->model().mode, UiMode::Switcher);
    ASSERT_FALSE(fixture.harness->model().switcher.workspaces.empty());

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(fixture.harness->model().exitConfirm.open);
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Switcher);
    EXPECT_FALSE(fixture.harness->model().switcher.workspaces.empty());

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    ASSERT_EQ(fixture.harness->model().mode, UiMode::Conversation);
    ASSERT_TRUE(fixture.harness->dispatch_command_line("/sessions"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.harness->model().mode, UiMode::Switcher);
    ASSERT_EQ(fixture.harness->model().switcher.source, SwitcherSource::History);

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(fixture.harness->model().exitConfirm.open);
    EXPECT_EQ(fixture.harness->model().mode, UiMode::Switcher);
}

// 57-H2 (57-I10): with no popup open, Ctrl+Q still exits.
TEST(Errata57, CtrlQNoPopupStillExits) {
    SwitcherFixture fixture("ymh_57_ctrlq_exit");
    fixture.harness->register_presence();
    ASSERT_EQ(fixture.harness->model().mode, UiMode::Conversation);

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.harness->quit_requested());
}

// 57-H3 (57-I7/57-I8): the arm/disarm table — a move disarms, a single press
// never deletes, a same-target second press deletes, and the 3 s tick disarms.
TEST(Errata57, ArmInlineDisarmTable) {
    SwitcherFixture fixture("ymh_57_arm");
    fixture.harness->open_switcher();
    drain_fully(*fixture.harness);
    fixture.harness->install_method_reply(std::string(protocol::method::kSessionDelete),
                                          nlohmann::json::object(), 0);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);

    ASSERT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 1u);
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    ASSERT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Armed);
    fixture.harness->mutable_model().switcher.delete_armed_at =
        std::chrono::steady_clock::now() - 4s;
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->model().switcher.delete_arm, EscArm::Disarmed);
    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 1u);
}

// 57-H4 (57-I10/57-F5): Ctrl+Q is swallowed by every popup guard, not just the
// Switcher.
TEST(Errata57, CtrlQSwallowedByEveryPopup) {
    SwitcherFixture fixture("ymh_57_guards");
    UiModel& model = fixture.harness->mutable_model();

    model.message.open = true;
    model.message.text = "notice";
    model.mode         = UiMode::Notice;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(model.exitConfirm.open);

    model.message.open = false;
    model.context.open = true;
    model.mode         = UiMode::Context;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(model.exitConfirm.open);

    model.context.open         = false;
    model.model_picker.visible = true;
    model.mode                 = UiMode::ModelPicker;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_FALSE(model.exitConfirm.open);

    model.model_picker.visible = false;
    fixture.harness->open_exit_prompt({});
    ASSERT_TRUE(model.exitConfirm.open);
    const bool exit_open_before = model.exitConfirm.open;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_EQ(model.exitConfirm.open, exit_open_before);

    model.exitConfirm.open = false;
    model.mode             = UiMode::Conversation;
    const SessionId           session{"s1"};
    const PermissionRequestId request{"perm-57"};
    fixture.harness->open_permission_dialog(session, request, "shell", "rm -rf build");
    ASSERT_TRUE(model.dialog.open);
    const bool dialog_before = model.dialog.open;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-q"));
    EXPECT_FALSE(fixture.harness->quit_requested());
    EXPECT_EQ(model.dialog.open, dialog_before);
}

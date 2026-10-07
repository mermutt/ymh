// 78 (§12.2): the supervisor-local `/fork` surface — arg parsing, the RPC caller
// beside the resume path, focus of the child, and the failure notices. Drives the
// real private handlers through the additive `SupervisorHarness` seam.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata78-ui"},
                          protocol::ClientRole::Supervisor};
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

bool has_notice(const UiModel& model, const std::string& needle) {
    for (const UiNotice& notice : model.notices) {
        if (notice.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool has_error_entry(const UiModel& model, const SessionId& session, const std::string& needle) {
    const SessionUiState* state = model.session(session);
    if (state == nullptr) {
        return false;
    }
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool has_cell(const WorkspaceModel& workspace, const SessionId& session) {
    for (const SessionCell& cell : workspace.sessions) {
        if (cell.id.value == session.value) {
            return true;
        }
    }
    return false;
}

const std::string kForkMethod = std::string(protocol::method::kSessionFork);

} // namespace

// 78-D1/D2/D5 (FK-I1/FK1/FK2/FK9): a no-argument `/fork` submits `session.fork`
// with no `seed_length`, focuses the child, and leaves the parent modeled.
TEST(Errata78, FK_I1_ForkFocusesChildAndKeepsParent) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    const SessionId   child{"child-9"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    EXPECT_EQ(harness->submitted_count(kForkMethod), 1u);
    const std::optional<nlohmann::json> params = harness->last_submitted_params(kForkMethod);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ((*params)["session"], parent.value);
    EXPECT_FALSE(params->contains("seed_length"));

    harness->drain_actions();
    ASSERT_NE(harness->model().workspaces.find(workspace), harness->model().workspaces.end());
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, child.value);
    EXPECT_NE(harness->model().sessions.find(parent), harness->model().sessions.end());
    EXPECT_NE(harness->model().sessions.find(child), harness->model().sessions.end());
    EXPECT_TRUE(has_notice(harness->model(), "forked -> " + child.value));
}

// 78-D2: `/fork 12` sends the explicit resolved-view boundary.
TEST(Errata78, FK_D2_ExplicitSeedIsSent) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", "child-1"}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/fork 12"));
    const std::optional<nlohmann::json> params = harness->last_submitted_params(kForkMethod);
    ASSERT_TRUE(params.has_value());
    ASSERT_TRUE(params->contains("seed_length"));
    EXPECT_EQ((*params)["seed_length"], 12);
}

// 78-D9/FK-F3 (FK-U5): a malformed boundary produces the usage notice and no RPC.
TEST(Errata78, FK_U5_ForkUsageNoticeNoRpc) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, SessionId{"parent-1"});
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", "child-1"}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/fork abc"));
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
    EXPECT_TRUE(has_notice(harness->model(), "usage: /fork"));

    EXPECT_TRUE(harness->dispatch_command_line("/fork -1"));
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
}

// 78-D9/FK15/FK-F1 (FK-I2): a focused subagent is refused before the RPC.
TEST(Errata78, FK_I2_SubagentFocusRefusedNoRpc) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->mutable_model().session(parent)->subagent = true;
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", "child-1"}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
    EXPECT_TRUE(has_notice(harness->model(), "not forkable"));
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
}

// FK-F1: with no focused session the command reports and issues no RPC.
TEST(Errata78, FK_F1_NoActiveSessionNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    harness->seed_active_workspace(live_workspace(WorkspaceId{"ws-a"}, "alpha"));

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
    EXPECT_TRUE(has_notice(harness->model(), "no active session to fork"));
}

// FK-F10 (FK-I3): a transport failure surfaces a notice and leaves the focus on
// the parent; no phantom child is modeled.
TEST(Errata78, FK_I3_TransportFailureLeavesParentFocused) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(kForkMethod), 1u);
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
    EXPECT_EQ(harness->model().sessions.size(), 1u);
    EXPECT_TRUE(has_error_entry(harness->model(), parent, "fork failed"));
}

// FK-F6/FK-F9 (FK-I6): a daemon-side store/lease rejection surfaces the mapped
// error and focuses nothing.
TEST(Errata78, FK_I6_StoreRejectionSurfacesNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_error_reply(
        kForkMethod, static_cast<int>(protocol::AppCode::StoreUnavailable), "session busy");

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    harness->drain_actions();

    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
    EXPECT_EQ(harness->model().sessions.size(), 1u);
    EXPECT_TRUE(has_error_entry(harness->model(), parent, "fork failed: session busy"));
}

// FK10/FK-I4: `/fork` never cancels the parent's in-flight turn.
TEST(Errata78, FK_I4_ForkDoesNotCancelParentTurn) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", "child-1"}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kAgentCancel)), 0u);
    EXPECT_NE(harness->model().sessions.find(parent), harness->model().sessions.end());
}

// FK13 (FK-I5): the child is a live cell in the focused workspace after the
// fork is applied, alongside the parent.
TEST(Errata78, FK_I5_ChildBecomesLiveCell) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    const SessionId   child{"child-9"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/fork"));
    harness->drain_actions();

    const WorkspaceModel& model = harness->model().workspaces.at(workspace);
    EXPECT_TRUE(has_cell(model, child));
    EXPECT_NE(harness->model().sessions.find(parent), harness->model().sessions.end());
    EXPECT_EQ(harness->model().sessions.find(child)->second.workspace.value, workspace.value);
}

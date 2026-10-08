// 80 (§14): hermetic coverage for the two-step `/rewind` code/conversation menu,
// driven through the real private handlers via the `SupervisorHarness` seam.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/clock.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata80-ui"},
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

nlohmann::json target_json(const std::string& session, std::uint64_t turn,
                           std::int64_t boundary, std::int64_t started_at_ms,
                           const std::string& prompt, std::int64_t file_change_count) {
    return nlohmann::json{{"session", session},
                          {"turn", turn},
                          {"boundary_index", boundary},
                          {"started_at_ms", started_at_ms},
                          {"prompt", prompt},
                          {"file_change_count", file_change_count}};
}

nlohmann::json targets_reply(std::int64_t view_length, nlohmann::json targets) {
    return nlohmann::json{{"view_length", view_length}, {"targets", std::move(targets)}};
}

const std::string kForkMethod    = std::string(protocol::method::kSessionFork);
const std::string kRewindMethod  = std::string(protocol::method::kSessionRewindTargets);
const std::string kRestoreMethod = std::string(protocol::method::kSessionRestoreCode);

}

TEST(Errata80, CP_U16_RewindActionGatingAndCursor) {
    RewindActionModel action;
    action.open_with(WorkspaceId{"ws"}, SessionId{"s"}, TurnId{4}, 3);
    ASSERT_EQ(action.actions.size(), 4u);
    EXPECT_EQ(action.actions[0], RewindAction::RestoreCodeAndConversation);
    EXPECT_EQ(action.actions[1], RewindAction::RestoreConversation);
    EXPECT_EQ(action.actions[2], RewindAction::RestoreCode);
    EXPECT_EQ(action.actions[3], RewindAction::Cancel);
    EXPECT_EQ(action.cursor, 0u);
    action.moveUp();
    EXPECT_EQ(action.cursor, 0u);
    for (int step = 0; step < 10; ++step) {
        action.moveDown();
    }
    EXPECT_EQ(action.cursor, 3u);
    EXPECT_NE(action.selected(), nullptr);
    action.close();
    EXPECT_FALSE(action.open);
    EXPECT_EQ(action.selected(), nullptr);

    RewindActionModel gated;
    gated.open_with(WorkspaceId{"ws"}, SessionId{"s"}, TurnId{4}, 0);
    ASSERT_EQ(gated.actions.size(), 2u);
    EXPECT_EQ(gated.actions[0], RewindAction::RestoreConversation);
    EXPECT_EQ(gated.actions[1], RewindAction::Cancel);
}

TEST(Errata80, CP_U17_RewindOpensActionNotFork) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(10, nlohmann::json::array(
                              {target_json(parent.value, 1, 3, 0, "first", 2)})),
        0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    ASSERT_EQ(harness->model().rewind.targets.size(), 1u);

    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();
    EXPECT_TRUE(harness->model().rewind_action.open);
    EXPECT_EQ(harness->model().mode, UiMode::RewindAction);
    EXPECT_EQ(harness->model().rewind_action.actions.size(), 4u);
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
    EXPECT_EQ(harness->submitted_count(kRestoreMethod), 0u);

    EXPECT_TRUE(harness->dispatch_key("escape"));
    EXPECT_FALSE(harness->model().rewind_action.open);
    EXPECT_TRUE(harness->model().rewind.open);
    EXPECT_EQ(harness->model().mode, UiMode::Rewind);
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
}

TEST(Errata80, CP_I2_RestoreCodeAndConversation) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    const SessionId   child{"child-9"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(10, nlohmann::json::array(
                              {target_json(parent.value, 1, 3, 0, "first", 2)})),
        0);
    harness->install_method_reply(
        kRestoreMethod,
        nlohmann::json{{"restored", 1}, {"skipped", 0}, {"failed", 0}, {"changed", 0},
                       {"expired", false}, {"detail", "restored 1 file(s)"}},
        0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(kRestoreMethod), 1u);
    EXPECT_EQ(harness->submitted_count(kForkMethod), 1u);
    const std::optional<nlohmann::json> restore_params =
        harness->last_submitted_params(kRestoreMethod);
    ASSERT_TRUE(restore_params.has_value());
    EXPECT_EQ((*restore_params)["session"], parent.value);
    EXPECT_EQ((*restore_params)["turn"], 1);
    ASSERT_NE(harness->model().workspaces.find(workspace), harness->model().workspaces.end());
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, child.value);
}

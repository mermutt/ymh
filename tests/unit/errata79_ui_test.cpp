// 79 (§12): hermetic coverage for the supervisor-local `/rewind` turn picker.
// Drives the real private handlers through the additive `SupervisorHarness`
// seam (same shape as errata78_ui_test.cpp).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
    return AttachIdentity{protocol::ClientInstanceId{"errata79-ui"},
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

nlohmann::json target_json(const std::string& session, std::uint64_t turn,
                           std::int64_t boundary, std::int64_t started_at_ms,
                           const std::string& prompt) {
    return nlohmann::json{{"session", session},
                          {"turn", turn},
                          {"boundary_index", boundary},
                          {"started_at_ms", started_at_ms},
                          {"prompt", prompt}};
}

nlohmann::json targets_reply(std::int64_t view_length, nlohmann::json targets) {
    return nlohmann::json{{"view_length", view_length}, {"targets", std::move(targets)}};
}

const std::string kForkMethod         = std::string(protocol::method::kSessionFork);
const std::string kRewindMethod       = std::string(protocol::method::kSessionRewindTargets);
const std::string kActivateMethod     = std::string(protocol::method::kSessionActivate);
const std::string kAgentCancelMethod  = std::string(protocol::method::kAgentCancel);

} // namespace

// 79 sec.4.4/5 (RW-U8): the overlay model's cursor/close semantics, plus the
// file-local summary excerpt (age | bounded preview) via a driven open.
TEST(Errata79, RW_U8_RewindOverlayCursorAndClose) {
    RewindOverlayModel overlay;
    std::vector<RewindTargetView> rows;
    for (std::uint64_t index = 0; index < 3; ++index) {
        rows.push_back(RewindTargetView{TurnId{index + 1}, static_cast<std::int64_t>(index),
                                        0, "prompt", "summary"});
    }
    overlay.open_with(WorkspaceId{"ws-a"}, SessionId{"s-1"}, 10, std::move(rows));
    EXPECT_TRUE(overlay.open);
    ASSERT_EQ(overlay.targets.size(), 3u);
    EXPECT_EQ(overlay.cursor, 2u);
    overlay.moveDown();
    EXPECT_EQ(overlay.cursor, 2u);
    overlay.moveUp();
    EXPECT_EQ(overlay.cursor, 1u);
    overlay.moveUp();
    overlay.moveUp();
    EXPECT_EQ(overlay.cursor, 0u);
    EXPECT_NE(overlay.selected(), nullptr);
    overlay.close();
    EXPECT_FALSE(overlay.open);
    EXPECT_EQ(overlay.selected(), nullptr);

    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    const std::string long_prompt(80, 'x');
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(5, nlohmann::json::array(
                             {target_json(parent.value, 1, 1,
                                          epoch_ms(std::chrono::system_clock::now()),
                                          long_prompt)})),
        0);
    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    ASSERT_EQ(harness->model().rewind.targets.size(), 1u);
    const std::string summary = harness->model().rewind.targets[0].summary;
    EXPECT_NE(summary.find(" | "), std::string::npos);
    EXPECT_NE(summary.find("..."), std::string::npos);
    EXPECT_EQ(summary.size(), std::string("0s ago | ").size() + 72u);
}

// 79-D10/RW9 (RW-U9): a boundary at or past the view length forks nothing and
// surfaces the no-op notice.
TEST(Errata79, RW_U9_RewindNoOpGuard) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(4, nlohmann::json::array({target_json(parent.value, 1, 4, 0, "p")})),
        0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", "child-1"}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_EQ(harness->model().mode, UiMode::Rewind);
    EXPECT_TRUE(harness->dispatch_key("enter"));

    EXPECT_TRUE(has_notice(harness->model(), "already at the current state"));
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
    EXPECT_NE(harness->model().mode, UiMode::Rewind);
}

// 79-D9/RW11 (RW-U10): a focused subagent is refused before any RPC.
TEST(Errata79, RW_U10_RewindSubagentRefused) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->mutable_model().session(parent)->subagent = true;

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    EXPECT_EQ(harness->submitted_count(kRewindMethod), 0u);
    EXPECT_TRUE(has_notice(harness->model(), "subagent sessions are not rewindable"));
    EXPECT_NE(harness->model().mode, UiMode::Rewind);
}

// 79-D3/RW2/RW4 (RW-I1): the chosen boundary is sent as `seed_length`, the
// child is focused, and the parent stays modeled next to it.
TEST(Errata79, RW_I1_RewindForksAtBoundary) {
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
                              {target_json(parent.value, 1, 3, 0, "first"),
                               target_json(parent.value, 2, 6, 0, "second")})),
        0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    ASSERT_EQ(harness->model().rewind.targets.size(), 2u);
    EXPECT_EQ(harness->model().rewind.cursor, 1u);
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    const std::optional<nlohmann::json> params = harness->last_submitted_params(kForkMethod);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ((*params)["session"], parent.value);
    EXPECT_EQ((*params)["seed_length"], 6);
    ASSERT_NE(harness->model().workspaces.find(workspace), harness->model().workspaces.end());
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, child.value);
    EXPECT_NE(harness->model().sessions.find(parent), harness->model().sessions.end());
}

// 79-D3/RW5/RW12 (RW-I2): the child is focused without `session.activate`; the
// parent keeps its state.
TEST(Errata79, RW_I2_ChildFocusedWithoutActivate) {
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
        targets_reply(8, nlohmann::json::array({target_json(parent.value, 1, 3, 0, "p")})), 0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(kActivateMethod), 0u);
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, child.value);
    EXPECT_NE(harness->model().sessions.find(parent), harness->model().sessions.end());
}

// 79-D4/RW10 (RW-I3): the chosen prompt lands in the child's composer, not in
// its conversation prefix.
TEST(Errata79, RW_I3_PromptRestoredIntoComposer) {
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
        targets_reply(8, nlohmann::json::array({target_json(parent.value, 1, 3, 0, "the prompt")})),
        0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    const SessionUiState* state = harness->model().session(child);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->input.draft, "the prompt");
    EXPECT_EQ(state->input.cursor, std::string("the prompt").size());
    for (const ConversationEntry& entry : state->conversation.entries) {
        EXPECT_EQ(entry.text.find("the prompt"), std::string::npos);
    }
}

// 79-F9 (RW-I4): a textless prompt forks, leaves the composer empty, and
// surfaces the prompt-unavailable notice.
TEST(Errata79, RW_I4_TextlessPromptNotice) {
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
        targets_reply(8, nlohmann::json::array({target_json(parent.value, 1, 3, 0, "")})), 0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", child.value}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    const SessionUiState* state = harness->model().session(child);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->input.draft.empty());
    EXPECT_TRUE(has_notice(harness->model(), "prompt unavailable"));
}

// 78-D6/FK10 (RW-I5): `/rewind` never cancels the parent's in-flight turn.
TEST(Errata79, RW_I5_RewindDoesNotCancelParentTurn) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(8, nlohmann::json::array({target_json(parent.value, 1, 3, 0, "p")})), 0);
    harness->install_method_reply(kForkMethod, nlohmann::json{{"session", "child-1"}}, 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(kAgentCancelMethod), 0u);
    EXPECT_NE(harness->model().sessions.find(parent), harness->model().sessions.end());
}

// 79-F4/RW-F4 (RW-I6): an unreachable daemon surfaces a notice and opens no
// overlay, leaving the focus on the parent.
TEST(Errata79, RW_I6_UnreachableDaemonNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();

    EXPECT_FALSE(harness->model().rewind.open);
    EXPECT_NE(harness->model().mode, UiMode::Rewind);
    EXPECT_TRUE(has_error_entry(harness->model(), parent, "rewind failed"));
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
}

// 79-F5/RW-F5 (RW-I7): a stale boundary the daemon rejects with
// `InvalidForkBoundary` surfaces the mapped error and creates no child.
TEST(Errata79, RW_I7_InvalidForkBoundaryNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(4, nlohmann::json::array({target_json(parent.value, 1, 1, 0, "p")})), 0);
    harness->install_method_error_reply(
        kForkMethod, static_cast<int>(protocol::AppCode::InvalidForkBoundary),
        "InvalidForkBoundary");

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_TRUE(harness->dispatch_key("enter"));
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(kForkMethod), 1u);
    EXPECT_TRUE(has_error_entry(harness->model(), parent, "rewind failed: InvalidForkBoundary"));
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
    EXPECT_EQ(harness->model().sessions.size(), 1u);
}

// 79-F11/RW-F11 (RW-I8): a focus change while the target read is in flight
// drops the reply and opens nothing.
TEST(Errata79, RW_I8_FocusChangeDropsStaleReply) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    const SessionId   other{"other-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(
        kRewindMethod,
        targets_reply(8, nlohmann::json::array({target_json(parent.value, 1, 3, 0, "p")})), 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    // The reply is enqueued; refocus before it drains.
    harness->activate_session(workspace, other);
    harness->drain_actions();

    EXPECT_FALSE(harness->model().rewind.open);
    EXPECT_NE(harness->model().mode, UiMode::Rewind);
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, other.value);
}

// 79-D9/RW11 (RW-I9): a subagent focus is refused before any RPC and the focus
// is unchanged.
TEST(Errata79, RW_I9_SubagentFocusRefusedNoRpc) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->mutable_model().session(parent)->subagent = true;

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    EXPECT_EQ(harness->submitted_count(kRewindMethod), 0u);
    EXPECT_FALSE(harness->model().rewind.open);
    EXPECT_TRUE(has_notice(harness->model(), "not rewindable"));
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
}

// 79-F3 (RW-I10): Enter on an empty target list closes the picker and forks
// nothing.
TEST(Errata79, RW_I10_EmptyTargetsEnterCloses) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_reply(kRewindMethod, targets_reply(1, nlohmann::json::array()), 0);

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();
    EXPECT_EQ(harness->model().mode, UiMode::Rewind);
    EXPECT_TRUE(harness->model().rewind.targets.empty());
    EXPECT_TRUE(harness->dispatch_key("enter"));

    EXPECT_NE(harness->model().mode, UiMode::Rewind);
    EXPECT_EQ(harness->submitted_count(kForkMethod), 0u);
}

// 79-F10/RW-F10 (RW-I11): an `UnknownSession` reply surfaces the mapped error,
// opens no overlay, and leaves the focus unchanged.
TEST(Errata79, RW_I11_UnknownSessionNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   parent{"parent-1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, parent);
    harness->install_method_error_reply(
        kRewindMethod, static_cast<int>(protocol::AppCode::UnknownSession), "unknown session");

    EXPECT_TRUE(harness->dispatch_command_line("/rewind"));
    harness->drain_actions();

    EXPECT_FALSE(harness->model().rewind.open);
    EXPECT_NE(harness->model().mode, UiMode::Rewind);
    EXPECT_TRUE(has_error_entry(harness->model(), parent, "rewind failed: unknown session"));
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId().value, parent.value);
}

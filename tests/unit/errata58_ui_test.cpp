// 58-H1…H23 (spec 58): subagent navigation driven through the real private
// handlers via the `SupervisorHarness` seam. The supervisor-level `58-U1`,
// `58-U8`, `58-U16`, `58-U18`, `58-U19`, `58-U20` assertions live here too,
// because `enter_subagent`/`return_subagent`/`open_subagents`/`select_history`
// are `SupervisorApp` methods, not `UiModel` ones.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/short_temp.hpp"
#include "ymh/core/event.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

const SessionId kS1{"s1"};
const SessionId kChild{"a1b2c3d4-1111-4111-8111-111111111111"};
const SessionId kGrand{"b2c3d4e5-2222-4222-8222-222222222222"};

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata58-ui"},
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

protocol::SessionEnvelope user_envelope(const SessionId& session, const std::string& id,
                                        const std::string& text) {
    Event event;
    event.id.value   = id;
    event.session_id = session;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::UserMessage;
    payload::UserMessage message;
    message.id = "m-" + id;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content.push_back(std::move(block));
    event.payload = std::move(message);

    protocol::SessionEnvelope envelope;
    envelope.session = session;
    envelope.event   = std::move(event);
    return envelope;
}

void drain_fully(SupervisorHarness& harness, int times = 4) {
    for (int index = 0; index < times; ++index) {
        harness.drain_actions();
    }
}

std::string render_model(const UiModel& model) {
    return render_to_ansi(model, TerminalSize{90, 30}, Theme{false});
}

struct SubagentFixture {
    explicit SubagentFixture(const std::string& name)
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
        model.sessions.push_back(cell(SessionId{"s2"}));
        harness->seed_active_workspace(model);
        harness->activate_session(workspace, kS1);

        harness->on_scan({spec_for(row, "boot-a")});
        drain_fully(*harness);
        harness->drop_connection(workspace);
        harness->install_session_list_reply(nlohmann::json::array(), 0);
        harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
        drain_fully(*harness);
    }

    UiModel& model() { return harness->mutable_model(); }
    const UiModel& model() const { return harness->model(); }

    void seed_child(const SessionId& parent, const SessionId& child, const std::string& summary,
                    SubagentStatus status = SubagentStatus::Running) {
        SessionUiState* state = model().session(parent);
        ASSERT_NE(state, nullptr);
        state->subagents.agents.push_back(
            SubagentView{child, summary, AgentState::Idle, status});
        model().ensureSubagentState(workspace, child);
    }

    void enter_first_child() {
        EXPECT_TRUE(harness->dispatch_key("ctrl-t"));
        EXPECT_TRUE(harness->dispatch_key("down"));
        EXPECT_TRUE(harness->dispatch_key("enter"));
        drain_fully(*harness);
    }

    ShortTempRoot                      root;
    std::unique_ptr<WorkspaceRegistry> registry;
    WorkspaceRecord                    row;
    WorkspaceId                        workspace;
    std::unique_ptr<SupervisorHarness> harness;
};

// 58-H1 (58-I1/I8): Ctrl+T opens, Enter enters, Esc returns; focus unchanged.
TEST(Errata58, UI58_H1_OpenEnterReturn) {
    SubagentFixture fixture("ymh_58_h1");
    fixture.seed_child(kS1, kChild, "task");

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_EQ(fixture.model().switcher.source, SwitcherSource::Subagents);

    EXPECT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    drain_fully(*fixture.harness);

    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);
    EXPECT_EQ(fixture.model().subagent_path[0], kChild);
    EXPECT_EQ(fixture.model().activeWorkspaceId, fixture.workspace);
    EXPECT_EQ(fixture.model().workspaces.at(fixture.workspace).activeSessionId(), kS1);
    EXPECT_EQ(fixture.model().viewedSession(), fixture.model().session(kChild));

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_EQ(fixture.model().viewedSession(), fixture.model().session(kS1));
    EXPECT_EQ(fixture.model().workspaces.at(fixture.workspace).activeSessionId(), kS1);
}

// 58-H2 (58-I11/58-F4): Esc in the view pops; it never arms the interrupt.
TEST(Errata58, UI58_H2_EscPrecedenceInView) {
    SubagentFixture fixture("ymh_58_h2");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();

    fixture.model().session(kS1)->agent_state = AgentState::Thinking;
    fixture.model().session(kS1)->esc_arm     = EscArm::Disarmed;

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_EQ(fixture.model().session(kS1)->esc_arm, EscArm::Disarmed);
    EXPECT_EQ(fixture.harness->cancel_count(), 0u);
}

// 58-H3 (58-D4): Ctrl+T while the picker is open closes it.
TEST(Errata58, UI58_H3_PickerConsumesCtrlT) {
    SubagentFixture fixture("ymh_58_h3");
    fixture.seed_child(kS1, kChild, "task");

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().mode, UiMode::Conversation);
}

// 58-H4 (58-I10, §7): a stored, non-resident child replays its own log.
TEST(Errata58, UI58_H4_CompletedChildReplays) {
    SubagentFixture fixture("ymh_58_h4");
    fixture.seed_child(kS1, kChild, "done task", SubagentStatus::Completed);
    fixture.enter_first_child();

    fixture.harness->feed_session_envelope(
        fixture.workspace, user_envelope(kChild, "c-1", "child replay text"));
    drain_fully(*fixture.harness);

    const SessionUiState* child = fixture.model().session(kChild);
    ASSERT_NE(child, nullptr);
    ASSERT_EQ(child->conversation.entries.size(), 1u);
    EXPECT_EQ(child->conversation.entries[0].text, "child replay text");
    EXPECT_NE(render_model(fixture.model()).find("child replay text"), std::string::npos);
}

// 58-H5 (58-I14): a SessionClosed for a path id pops the path and pushes a notice.
TEST(Errata58, UI58_H5_DeletedChildPopsWithNotice) {
    SubagentFixture fixture("ymh_58_h5");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();

    protocol::HostNotice closed;
    closed.kind    = protocol::HostNoticeKind::SessionClosed;
    closed.session = kChild;
    fixture.harness->deliver_host_notice(fixture.workspace, closed);
    drain_fully(*fixture.harness);

    EXPECT_TRUE(fixture.model().subagent_path.empty());
    ASSERT_FALSE(fixture.model().notices.empty());
    EXPECT_NE(fixture.model().notices.back().text.find("closed"), std::string::npos);
}

// 58-H6 (58-I12): scroll keys act on the viewed child, not the parent.
TEST(Errata58, UI58_H6_ScrollKeysActOnViewedChild) {
    SubagentFixture fixture("ymh_58_h6");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();

    ASSERT_TRUE(fixture.model().session(kChild)->scroll.following);
    ASSERT_TRUE(fixture.model().session(kS1)->scroll.following);

    EXPECT_TRUE(fixture.harness->dispatch_key("page-up"));
    EXPECT_FALSE(fixture.model().session(kChild)->scroll.following);
    EXPECT_TRUE(fixture.model().session(kS1)->scroll.following);
}

// 58-H7 (58-I13): a child SessionUiState never appears in the Live switcher.
TEST(Errata58, UI58_H7_ChildNotInLiveSwitcher) {
    SubagentFixture fixture("ymh_58_h7");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    // The child state must still exist when the Live switcher is built, else the
    // absence assertion below cannot fail.
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);

    fixture.harness->open_switcher();
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().mode, UiMode::Switcher);
    for (const WorkspaceNode& node : fixture.model().switcher.workspaces) {
        for (const SessionNode& session : node.sessions) {
            EXPECT_NE(session.id, kChild);
        }
    }
}

// 58-H8 (58-I14/E36/E37): daemon death clears the path via the eviction hook.
TEST(Errata58, UI58_H8_DaemonDeathClearsPathViaProductionHook) {
    SubagentFixture fixture("ymh_58_h8");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);

    // `UiModel::apply(DaemonDied)` alone is liveness-only and never clears it.
    fixture.model().apply(WorkspaceEvent{
        fixture.workspace, WorkspaceEventKind::DaemonDied, std::nullopt});
    EXPECT_EQ(fixture.model().subagent_path.size(), 1u);

    // The link transition alone is inert while the child state survives (E36).
    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Dead, "test");
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.model().subagent_path.size(), 1u);

    // The presence-scan eviction is the real clear (E37).
    fixture.harness->evict_dead_workspaces({});
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_EQ(fixture.model().viewedSession(), fixture.model().activeSession());
}

// 58-H9 (58-I17): Ctrl+D in the Subagents picker is a consumed no-op.
TEST(Errata58, UI58_H9_PickerDeleteDisabled) {
    SubagentFixture fixture("ymh_58_h9");
    fixture.seed_child(kS1, kChild, "task");

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-d"));
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->submitted_count("session.delete"), 0u);
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_NE(fixture.model().session(kChild), nullptr);
}

// 58-H10 (58-F14): Ctrl+C while viewing sends no cancel.
TEST(Errata58, UI58_H10_CtrlCSuppressedInView) {
    SubagentFixture fixture("ymh_58_h10");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    fixture.model().session(kS1)->agent_state = AgentState::Thinking;

    const std::size_t before = fixture.harness->cancel_count();
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_EQ(fixture.harness->cancel_count(), before);
}

// 58-H11 (58-I18): pop releases the child; the tracked set returns to size.
TEST(Errata58, UI58_H11_UnsubscribeOnPop) {
    SubagentFixture fixture("ymh_58_h11");
    fixture.seed_child(kS1, kChild, "task");
    const std::size_t before = fixture.harness->viewed_children_count();
    const std::size_t subscribe_before =
        fixture.harness->subscribe_request_count(fixture.workspace);
    const std::size_t unsubscribe_before =
        fixture.harness->unsubscribe_request_count(fixture.workspace);

    fixture.enter_first_child();
    EXPECT_EQ(fixture.harness->viewed_children_count(), before + 1);
    ASSERT_TRUE(fixture.harness->viewed_child_workspace(kChild).has_value());
    EXPECT_EQ(*fixture.harness->viewed_child_workspace(kChild), fixture.workspace);
    EXPECT_EQ(fixture.harness->subscribe_request_count(fixture.workspace),
              subscribe_before + 1);
    EXPECT_EQ(fixture.harness->unsubscribe_request_count(fixture.workspace),
              unsubscribe_before);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->viewed_children_count(), before);
    EXPECT_EQ(fixture.harness->unsubscribe_request_count(fixture.workspace),
              unsubscribe_before + 1);

    fixture.enter_first_child();
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.harness->viewed_children_count(), before);
    EXPECT_EQ(fixture.harness->subscribe_request_count(fixture.workspace),
              subscribe_before + 2);
    EXPECT_EQ(fixture.harness->unsubscribe_request_count(fixture.workspace),
              unsubscribe_before + 2);
}

// 58-H12 (58-I20/F12): a subscribe failure surfaces the notice and pops.
TEST(Errata58, UI58_H12_UnknownChildSurfacesNotice) {
    SubagentFixture fixture("ymh_58_h12");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();

    fixture.harness->deliver_subscribe_error(kChild, "unknown session");
    drain_fully(*fixture.harness);

    EXPECT_TRUE(fixture.model().subagent_path.empty());
    ASSERT_FALSE(fixture.model().notices.empty());
    EXPECT_NE(fixture.model().notices.back().text.find("subagent unavailable:"),
              std::string::npos);
    EXPECT_NE(fixture.model().viewedSession(), nullptr);
}

// 58-H13 (58-I4): no `agent.*` RPC can be sent while a child is viewed.
TEST(Errata58, UI58_H13_NoAgentRpcWhileViewing) {
    SubagentFixture fixture("ymh_58_h13");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    fixture.model().session(kS1)->agent_state = AgentState::Thinking;

    fixture.harness->dispatch_key("a");
    fixture.harness->dispatch_key("enter");
    fixture.harness->dispatch_key("tab");
    fixture.harness->dispatch_key("backspace");
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.harness->submitted_count("agent.prompt"), 0u);
    EXPECT_EQ(fixture.harness->submitted_count("agent.cancel"), 0u);
    EXPECT_TRUE(fixture.model().session(kS1)->input.draft.empty());
}

// 58-H14 (58-D2): Ctrl+T at depth 1 lists the child's children; nesting works.
TEST(Errata58, UI58_H14_NestedPushPopReachable) {
    SubagentFixture fixture("ymh_58_h14");
    fixture.seed_child(kS1, kChild, "A");
    fixture.seed_child(kChild, kGrand, "B");

    fixture.enter_first_child();
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().switcher.source, SwitcherSource::Subagents);
    EXPECT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().subagent_path.size(), 2u);
    EXPECT_EQ(fixture.model().subagent_path[1], kGrand);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);
    EXPECT_EQ(fixture.model().subagent_path[0], kChild);
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.model().subagent_path.empty());
}

// 58-H15 (58-D4): Ctrl+C cancels at the root, is consumed at depth >= 1.
TEST(Errata58, UI58_H15_CtrlCCancelsAtRootOnly) {
    SubagentFixture fixture("ymh_58_h15");
    fixture.model().session(kS1)->agent_state = AgentState::Thinking;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_EQ(fixture.harness->cancel_count(), 1u);

    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    fixture.model().session(kS1)->agent_state = AgentState::Thinking;
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-c"));
    EXPECT_EQ(fixture.harness->cancel_count(), 1u);
}

// 58-H16 (58-D7/E20): a subagent History row is refused with the notice.
TEST(Errata58, UI58_H16_HistorySelectSubagentRefused) {
    SubagentFixture fixture("ymh_58_h16");
    fixture.seed_child(kS1, kChild, "task");

    UiModel& model = fixture.model();
    model.switcher.source = SwitcherSource::History;
    WorkspaceNode node;
    node.id = fixture.workspace;
    SessionNode subagent_row;
    subagent_row.id   = kChild;
    subagent_row.kind = "subagent";
    node.sessions.push_back(subagent_row);
    model.switcher.workspaces = {node};

    const SessionId active_before = model.workspaces.at(fixture.workspace).activeSessionId();
    SwitcherCursor cursor;
    cursor.workspace = fixture.workspace;
    cursor.session   = kChild;
    fixture.harness->select_history(cursor);
    drain_fully(*fixture.harness);

    ASSERT_FALSE(model.notices.empty());
    EXPECT_NE(model.notices.back().text.find("subagent sessions are viewed from their parent"),
              std::string::npos);
    EXPECT_EQ(model.workspaces.at(fixture.workspace).activeSessionId(), active_before);
    EXPECT_EQ(fixture.harness->submitted_count("session.resume"), 0u);
}

// 58-H17 (E10/E11/E12): `/subagents` dispatches the registered command.
TEST(Errata58, UI58_H17_SlashSubagentsDispatches) {
    SubagentFixture fixture("ymh_58_h17");
    fixture.seed_child(kS1, kChild, "task");

    EXPECT_TRUE(fixture.harness->dispatch_command_line("/subagents"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_EQ(fixture.model().switcher.source, SwitcherSource::Subagents);
}

// 58-H18 (E2/58-F17): Tab in the Subagents picker is a consumed no-op.
TEST(Errata58, UI58_H18_PickerTabNoCollapse) {
    SubagentFixture fixture("ymh_58_h18");
    fixture.seed_child(kS1, kChild, "task");

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_TRUE(fixture.model().switcher.collapsed.empty());

    EXPECT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);
    EXPECT_EQ(fixture.model().subagent_path[0], kChild);
}

// 58-H19 (E3): Ctrl+T closes only the Subagents picker.
TEST(Errata58, UI58_H19_CtrlTDoesNotCloseCatalogSwitchers) {
    SubagentFixture fixture("ymh_58_h19");
    fixture.seed_child(kS1, kChild, "task");

    fixture.harness->open_switcher();
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().switcher.source, SwitcherSource::Live);
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_EQ(fixture.model().switcher.source, SwitcherSource::Live);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.harness->dispatch_command_line("/sessions"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().switcher.source, SwitcherSource::History);
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_EQ(fixture.model().switcher.source, SwitcherSource::History);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    ASSERT_EQ(fixture.model().switcher.source, SwitcherSource::Subagents);
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_EQ(fixture.model().mode, UiMode::Conversation);
}

// 58-H20 (E4/58-I8): Enter on the empty leaf is a consumed no-op.
TEST(Errata58, UI58_H20_EnterEmptyLeafStaysOpen) {
    SubagentFixture fixture("ymh_58_h20");

    EXPECT_TRUE(fixture.harness->dispatch_command_line("/subagents"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().mode, UiMode::Switcher);

    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    EXPECT_EQ(fixture.model().mode, UiMode::Switcher);
    EXPECT_FALSE(fixture.model().switcher.workspaces.empty());
    EXPECT_TRUE(fixture.model().subagent_path.empty());
}

// 58-H21 (E16/E22): a session switch releases the viewed child.
TEST(Errata58, UI58_H21_ViewSwitchReleasesChild) {
    SubagentFixture fixture("ymh_58_h21");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    ASSERT_EQ(fixture.harness->viewed_children_count(), 1u);

    fixture.harness->install_method_reply(std::string(protocol::method::kSessionCreate),
                                          nlohmann::json{{"session", "s-new"}}, 0);
    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-n"));
    drain_fully(*fixture.harness);

    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_EQ(fixture.harness->viewed_children_count(), 0u);
}

// 58-H22 (58-I10/I19, HIGH-1): re-entry renders content, not a blank transcript.
TEST(Errata58, UI58_H22_ReentryRendersContent) {
    SubagentFixture fixture("ymh_58_h22");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();

    const protocol::SessionEnvelope replay = user_envelope(kChild, "c-1", "reentry text");
    fixture.harness->feed_session_envelope(fixture.workspace, replay);
    drain_fully(*fixture.harness);
    EXPECT_NE(render_model(fixture.model()).find("reentry text"), std::string::npos);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    fixture.enter_first_child();

    // The daemon replays the same stored envelope; `forget_session` must have
    // cleared the dedup so it applies again (the global-set defect rendered blank).
    fixture.harness->feed_session_envelope(fixture.workspace, replay);
    drain_fully(*fixture.harness);
    EXPECT_NE(render_model(fixture.model()).find("reentry text"), std::string::npos);
}

// 58-H23 (E43/E37/E45/E44): an externally removed viewed child reconciles.
TEST(Errata58, UI58_H23_DeleteViewedChildReconciles) {
    SubagentFixture fixture("ymh_58_h23");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);

    // (1) External-client `session.delete` reply for the viewed child.
    fixture.harness->apply_session_deleted(fixture.workspace, kChild);
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_NE(fixture.model().viewedSession(), nullptr);
    EXPECT_NO_THROW(static_cast<void>(render_model(fixture.model())));

    // (2) Kill C's workspace daemon so the presence scan evicts it.
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);
    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Dead, "test");
    fixture.harness->evict_dead_workspaces({});
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_NO_THROW(static_cast<void>(render_model(fixture.model())));

    // The unreachable E44 path is refused: Ctrl+D on the live workspace.
    SubagentFixture live("ymh_58_h23_live");
    live.harness->open_switcher();
    drain_fully(*live.harness);
    ASSERT_EQ(live.model().switcher.source, SwitcherSource::Live);
    live.harness->dispatch_key("ctrl-d");
    live.harness->dispatch_key("ctrl-d");
    drain_fully(*live.harness);
    ASSERT_FALSE(live.model().notices.empty());
    EXPECT_NE(live.model().notices.back().text.find("stop the workspace first"),
              std::string::npos);
    EXPECT_EQ(live.harness->submitted_count("workspace.delete"), 0u);
}

// 58-U1/58-U18 (58-I25/E15): enter pushes, return pops exactly one level.
TEST(Errata58, UI58_U1_U18_ReturnSubagentPopOrder) {
    SubagentFixture fixture("ymh_58_u1");
    fixture.seed_child(kS1, kChild, "A");
    fixture.seed_child(kChild, kGrand, "B");

    fixture.enter_first_child();
    EXPECT_EQ(fixture.model().subagent_path.size(), 1u);

    EXPECT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    EXPECT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_TRUE(fixture.harness->dispatch_key("enter"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().subagent_path.size(), 2u);

    // Pop the deepest first: path {A,B} -> {A}, not main; B's state erased.
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    ASSERT_EQ(fixture.model().subagent_path.size(), 1u);
    EXPECT_EQ(fixture.model().subagent_path[0], kChild);
    EXPECT_EQ(fixture.model().session(kGrand), nullptr);

    // Pop the last: empty path, ASan-clean (no pop_back on empty).
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    EXPECT_TRUE(fixture.model().subagent_path.empty());
    EXPECT_EQ(fixture.model().session(kChild), nullptr);
    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.model().subagent_path.empty());
}

// 58-U8 (58-D7/58-I22): a `session.list` subagent row creates no cell/track.
TEST(Errata58, UI58_U8_RefreshSessionsSkipsSubagents) {
    SubagentFixture fixture("ymh_58_u8");
    const std::size_t tracked_before = fixture.harness->viewed_children_count();

    fixture.harness->install_session_list_reply(
        nlohmann::json::array({{{"id", kChild.value}, {"live", true}, {"kind", "subagent"},
                                {"title", "child"}},
                               {{"id", "s3"}, {"live", true}, {"kind", "root"}, {"title", "root"}}}),
        0);
    fixture.harness->on_link_state(fixture.workspace, SupervisorLinkState::Attached, "test");
    drain_fully(*fixture.harness);

    EXPECT_EQ(fixture.model().session(kChild), nullptr) << "no state/track for a subagent row";
    EXPECT_NE(fixture.model().session(SessionId{"s3"}), nullptr);
    EXPECT_EQ(fixture.harness->viewed_children_count(), tracked_before);
    for (const SessionCell& session :
         fixture.model().workspaces.at(fixture.workspace).sessions) {
        EXPECT_NE(session.id, kChild);
    }
}

// 58-U16 (58-I19): pop erases the child's SessionUiState.
TEST(Errata58, UI58_U16_SubagentStateErasedOnPop) {
    SubagentFixture fixture("ymh_58_u16");
    fixture.seed_child(kS1, kChild, "task");
    fixture.enter_first_child();
    ASSERT_NE(fixture.model().session(kChild), nullptr);

    EXPECT_TRUE(fixture.harness->dispatch_key("escape"));
    drain_fully(*fixture.harness);
    EXPECT_EQ(fixture.model().session(kChild), nullptr);
}

// 58-U19 (58-I21/E13): no active session ⇒ the notice, no picker, no crash.
TEST(Errata58, UI58_U19_OpenSubagentsNullGuard) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness =
        make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-empty"};
    harness->seed_active_workspace(live_workspace(workspace, "empty"));

    EXPECT_TRUE(harness->dispatch_command_line("/subagents"));
    drain_fully(*harness);
    EXPECT_NE(harness->model().mode, UiMode::Switcher);
    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_NE(harness->model().notices.back().text.find("no session to show subagents for"),
              std::string::npos);
}

// 58-U20 (58-E20): select_history refuses a subagent row.
TEST(Errata58, UI58_U20_SelectHistoryRefusesSubagent) {
    SubagentFixture fixture("ymh_58_u20");
    UiModel& model = fixture.model();
    model.switcher.source = SwitcherSource::History;
    WorkspaceNode node;
    node.id = fixture.workspace;
    SessionNode subagent_row;
    subagent_row.id   = kChild;
    subagent_row.kind = "subagent";
    node.sessions.push_back(subagent_row);
    model.switcher.workspaces = {node};

    SwitcherCursor cursor;
    cursor.workspace = fixture.workspace;
    cursor.session   = kChild;
    fixture.harness->select_history(cursor);
    drain_fully(*fixture.harness);

    ASSERT_FALSE(model.notices.empty());
    EXPECT_NE(model.notices.back().text.find("subagent sessions are viewed from their parent"),
              std::string::npos);
    EXPECT_EQ(fixture.harness->submitted_count("session.resume"), 0u);
}

} // namespace

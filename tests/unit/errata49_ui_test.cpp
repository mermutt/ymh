// 49-D1/D5/D6: lazy workspace creation on the first prompt and the
// session-aware Ctrl-S notice. The supervisor-level cases drive the real
// private handlers through the additive `SupervisorHarness` seam.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
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
    return AttachIdentity{protocol::ClientInstanceId{"errata49-ui"},
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

WorkspaceHistory history(const WorkspaceId& workspace,
                         const std::vector<SessionId>& sessions,
                         std::optional<std::string> note = std::nullopt) {
    WorkspaceHistory record;
    record.id            = workspace;
    record.title         = workspace.value;
    record.canonicalPath = "/" + workspace.value;
    record.note          = std::move(note);
    for (const SessionId& session : sessions) {
        SessionHistoryEntry entry;
        entry.id        = session;
        entry.title     = session.value;
        entry.kind      = "root";
        entry.model     = "m";
        entry.updatedAt = 1;
        record.sessions.push_back(std::move(entry));
    }
    return record;
}

void seed_loaded_catalog(UiModel& model, std::vector<WorkspaceHistory> histories) {
    model.catalog.workspaces = std::move(histories);
    model.catalog.loaded     = true;
    model.catalog.generation = 1;
}

class ThrowingLauncher final : public HostLauncher {
public:
    SpawnResult spawn(const HostConfig&) override {
        throw HostError(protocol::HostErrorCode::HostUnreachable, "spawn failed");
    }
    void requestStop(HostPid, ShutdownReason) override {}
    [[nodiscard]] bool isAlive(HostPid) const override { return false; }
};

bool wait_for_notice(SupervisorHarness& harness, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        harness.drain_actions();
        if (!harness.model().notices.empty()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

// ── 49-D5 / 49-D6 ───────────────────────────────────────────────────────────

TEST(Errata49, UI49_D5_OtherLiveWorkspaceWithoutSessionsShowsNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, SessionId{"s1"});
    harness->seed_workspace(live_workspace(WorkspaceId{"ws-b"}, "beta"));
    seed_loaded_catalog(harness->mutable_model(),
                        {history(active, {SessionId{"s1"}}), history(WorkspaceId{"ws-b"}, {})});

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Notice);
    EXPECT_TRUE(harness->model().message.open);
    EXPECT_EQ(harness->model().message.text, "No other sessions available");
    EXPECT_TRUE(harness->model().switcher.workspaces.empty());
}

TEST(Errata49, UI49_D5_OtherLiveWorkspaceWithSessionShowsSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    const WorkspaceId other{"ws-b"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, SessionId{"s1"});
    WorkspaceModel beta = live_workspace(other, "beta");
    beta.sessions.push_back(cell(SessionId{"s2"}));
    harness->seed_workspace(beta);
    seed_loaded_catalog(harness->mutable_model(),
                        {history(active, {SessionId{"s1"}}), history(other, {SessionId{"s2"}})});

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
    EXPECT_FALSE(harness->model().message.open);
}

TEST(Errata49, UI49_D5_ActiveWorkspaceSecondSessionShowsSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    WorkspaceModel workspace = live_workspace(active, "alpha");
    workspace.sessions.push_back(cell(SessionId{"s1"}));
    workspace.sessions.push_back(cell(SessionId{"s2"}));
    harness->seed_active_workspace(workspace);
    harness->activate_session(active, SessionId{"s1"});
    seed_loaded_catalog(harness->mutable_model(),
                        {history(active, {SessionId{"s1"}, SessionId{"s2"}})});

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
}

TEST(Errata49, UI49_D6_OnlyLiveWorkspaceNoticeText) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, SessionId{"s1"});

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Notice);
    EXPECT_EQ(harness->model().message.text, "No other workspaces available");
}

TEST(Errata49, UI49_D5_CatalogPendingFallsBackToSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, SessionId{"s1"});
    harness->seed_workspace(live_workspace(WorkspaceId{"ws-b"}, "beta"));

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
}

TEST(Errata49, UI49_D5_NoteWorkspaceFallsBackToSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    const WorkspaceId other{"ws-b"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, SessionId{"s1"});
    harness->seed_workspace(live_workspace(other, "beta"));
    seed_loaded_catalog(harness->mutable_model(),
                        {history(active, {SessionId{"s1"}}), history(other, {}, "read failed")});

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
}

TEST(Errata49, UI49_D5_ActiveWorkspaceUnknownMembershipFallsBackToSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active{"ws-a"};
    WorkspaceModel workspace = live_workspace(active, "alpha");
    workspace.sessions.push_back(cell(SessionId{"s1"}));
    workspace.sessions.push_back(cell(SessionId{"s2"}));
    harness->seed_active_workspace(workspace);
    harness->activate_session(active, SessionId{"s1"});

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
}

TEST(Errata49, UI49_D8_NoticeDismissal) {
    for (const std::string key : {"enter", "escape", "ctrl-c"}) {
        SupervisorRunOptions options;
        options.identity = test_identity();
        const std::unique_ptr<SupervisorHarness> harness =
            make_supervisor_harness(std::move(options));
        const WorkspaceId active{"ws-a"};
        harness->seed_active_workspace(live_workspace(active, "alpha"));
        harness->activate_session(active, SessionId{"s1"});
        harness->open_switcher();
        ASSERT_EQ(harness->model().mode, UiMode::Notice) << key;

        EXPECT_TRUE(harness->dispatch_key(key)) << key;
        EXPECT_EQ(harness->model().mode, UiMode::Conversation) << key;
        EXPECT_FALSE(harness->model().message.open) << key;
    }
}

TEST(Errata49, UI49_D8_NoticeSwallowsCtrlS) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId active{"ws-a"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, SessionId{"s1"});
    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    for (const std::string key : {"ctrl-s", "ctrl-p", "x"}) {
        EXPECT_TRUE(harness->dispatch_key(key)) << key;
        EXPECT_EQ(harness->model().mode, UiMode::Notice) << key;
        EXPECT_TRUE(harness->model().message.open) << key;
    }
}

TEST(Errata49, UI49_D8_NoticeBlocksComposer) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId active{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(active, "alpha"));
    harness->activate_session(active, session);
    ASSERT_TRUE(harness->dispatch_key("a"));

    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    EXPECT_TRUE(harness->dispatch_key("b"));

    ASSERT_NE(harness->model().session(session), nullptr);
    EXPECT_EQ(harness->model().session(session)->input.draft, "a");
    EXPECT_EQ(harness->model().mode, UiMode::Notice);
}

TEST(Errata49, UI49_I12_SessionsOpensHistory) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    EXPECT_TRUE(harness->dispatch_command_line("/sessions"));

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
    EXPECT_EQ(harness->model().switcher.source, SwitcherSource::History);
}

// ── 49-D1 / 49-D2 ───────────────────────────────────────────────────────────

TEST(Errata49, UI49_D1_NoWorkspaceAtStartup) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    EXPECT_TRUE(harness->model().workspaces.empty());
    EXPECT_TRUE(harness->model().activeWorkspaceId.value.empty());
    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
}

TEST(Errata49, UI49_D1_EmptyStateNoticeOnCtrlS) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    EXPECT_TRUE(harness->dispatch_key("ctrl-s"));

    EXPECT_EQ(harness->model().mode, UiMode::Notice);
    EXPECT_EQ(harness->model().message.text, "No other workspaces available");
}

TEST(Errata49, UI49_D1_EmptyStateComposerTypeable) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    ASSERT_TRUE(harness->dispatch_key("h"));
    ASSERT_TRUE(harness->dispatch_key("i"));

    EXPECT_EQ(harness->model().pendingComposer.input.draft, "hi");
}

TEST(Errata49, UI49_D1_NoRegistryRowBeforePrompt) {
    ShortTempRoot root("ymh_49_u13");
    std::filesystem::create_directories(root.path() / "ws");
    const std::filesystem::path cwd = std::filesystem::canonical(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry          = registry.get();
    options.initial_workspace = cwd;
    options.identity          = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->register_presence();

    EXPECT_TRUE(registry->listWorkspaces().empty());
    EXPECT_FALSE(registry->findByCanonicalPath(cwd).has_value());
    EXPECT_FALSE(registry->listSupervisors().empty());

    harness->submit("hi");
    harness->drain_actions();

    const std::optional<WorkspaceRecord> row = registry->findByCanonicalPath(cwd);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(registry->listWorkspaces().size(), 1U);
    EXPECT_FALSE(registry->listSupervisors().empty());
}

TEST(Errata49, UI49_D1_FirstPromptSpawnsWorkspace) {
    ShortTempRoot root("ymh_49_u12");
    std::filesystem::create_directories(root.path() / "ws");
    const std::filesystem::path cwd = std::filesystem::canonical(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry          = registry.get();
    options.initial_workspace = cwd;
    options.identity          = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    harness->submit("hi");
    harness->drain_actions();

    const std::optional<WorkspaceRecord> row = registry->findByCanonicalPath(cwd);
    ASSERT_TRUE(row.has_value());
    const WorkspaceId workspace = row->id;
    EXPECT_EQ(harness->ensure_call_count(workspace), 1U);
    EXPECT_FALSE(harness->has_connection(workspace))
        << "submit attached before the daemon registered";
    ASSERT_TRUE(harness->pending_creates().count(workspace) != 0);
    EXPECT_EQ(harness->pending_creates().at(workspace), "hi");

    harness->on_scan({spec_for(*row, "boot-a")});
    harness->drain_actions();
    ASSERT_TRUE(harness->has_connection(workspace));

    harness->install_session_list_reply(nlohmann::json::array(), 0);
    harness->install_method_reply(std::string(protocol::method::kSessionCreate),
                                  nlohmann::json{{"session", "s1"}}, 0);
    harness->install_method_reply(std::string(protocol::method::kAgentPrompt),
                                  nlohmann::json::object(), 0);

    harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
    harness->drain_actions();
    harness->drain_actions();

    EXPECT_EQ(harness->model().activeWorkspaceId.value, workspace.value);
    ASSERT_NE(harness->model().session(SessionId{"s1"}), nullptr);
    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionCreate)), 1U);
    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kAgentPrompt)), 1U);
}

TEST(Errata49, UI49_D1_SpawnFailureNoPhantom) {
    ShortTempRoot root("ymh_49_u14");
    std::filesystem::create_directories(root.path() / "ws");
    const std::filesystem::path cwd = std::filesystem::canonical(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config(root.path()));

    ThrowingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *registry, root.path() / "config.jsonc");

    SupervisorRunOptions options;
    options.registry          = registry.get();
    options.lifecycle         = &lifecycle;
    options.initial_workspace = cwd;
    options.identity          = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    harness->submit("hi");
    ASSERT_TRUE(wait_for_notice(*harness));

    EXPECT_TRUE(harness->model().workspaces.empty());
    EXPECT_EQ(harness->model().pendingComposer.input.draft, "hi");
}

TEST(Errata49, UI49_I5_ZeroDaemonCleanExit) {
    ShortTempRoot root("ymh_49_u15");
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->register_presence();

    harness->begin_exit(/*allow_prompt=*/true);
    harness->drain_actions();

    EXPECT_FALSE(harness->model().exitConfirm.open);
    EXPECT_TRUE(harness->quit_requested());
    EXPECT_TRUE(registry->listSupervisors().empty());
}

} // namespace

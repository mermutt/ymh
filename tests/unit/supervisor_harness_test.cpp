// 22 §10.1/§10.2 (S3/S4): the supervisor-internal tests the earlier waves could
// not implement because `SupervisorApp` is private. They drive the app through
// the additive `SupervisorHarness` seam and cover the H2/MEDIUM-1/MEDIUM-2/M4/M5
// lifetime fixes.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor_harness.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace ymh::ui;
using namespace std::chrono_literals;

RegistryConfig harness_registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = 200ms;
    config.lock_retry_interval = 5ms;
    return config;
}

AttachIdentity harness_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"harness-supervisor"},
                          protocol::ClientRole::Supervisor};
}

// A `HostLauncher` whose `spawn` blocks until `release()` and then throws, so a
// test can hold the ensure worker inside `ensureRunning`.
class BlockingLauncher final : public HostLauncher {
public:
    BlockingLauncher() : entered_(entered_promise_.get_future()) {}

    SpawnResult spawn(const HostConfig&) override {
        spawn_calls_.fetch_add(1);
        entered_promise_.set_value();
        release_future_.wait();
        spawn_returned_.store(true);
        throw HostError(protocol::HostErrorCode::HostUnreachable, "blocked spawn");
    }

    void requestStop(HostPid, ShutdownReason) override {}
    [[nodiscard]] bool isAlive(HostPid) const override { return false; }

    bool wait_entered(std::chrono::milliseconds timeout) {
        return entered_.wait_for(timeout) == std::future_status::ready;
    }
    void release() { release_promise_.set_value(); }
    [[nodiscard]] bool spawn_returned() const { return spawn_returned_.load(); }
    [[nodiscard]] int  spawn_calls() const { return spawn_calls_.load(); }

private:
    std::promise<void> entered_promise_;
    std::future<void>  entered_;
    std::promise<void> release_promise_;
    std::future<void>  release_future_ = release_promise_.get_future();
    std::atomic<bool>  spawn_returned_{false};
    std::atomic<int>   spawn_calls_{0};
};

// A `HostLauncher` that fails every spawn, so a test can drive the SW-F1 spawn
// failure branch without forking a daemon.
class ThrowingLauncher final : public HostLauncher {
public:
    SpawnResult spawn(const HostConfig&) override {
        spawn_calls_.fetch_add(1);
        throw HostError(protocol::HostErrorCode::HostUnreachable, "spawn failed");
    }

    void requestStop(HostPid, ShutdownReason) override {}
    [[nodiscard]] bool isAlive(HostPid) const override { return false; }
    [[nodiscard]] int  spawn_calls() const { return spawn_calls_.load(); }

private:
    std::atomic<int> spawn_calls_{0};
};

// A catalog read that blocks until `release()`, so a test can hold the catalog
// reader inside `build()`.
class BlockingCatalogRead {
public:
    BlockingCatalogRead() : entered_(entered_promise_.get_future()) {}

    void enter() { entered_promise_.set_value(); }
    void wait_release() { release_future_.wait(); }
    void release() { release_promise_.set_value(); }
    void mark_returned() { returned_.store(true); }

    bool wait_entered(std::chrono::milliseconds timeout) {
        return entered_.wait_for(timeout) == std::future_status::ready;
    }
    [[nodiscard]] bool returned() const { return returned_.load(); }

private:
    std::promise<void> entered_promise_;
    std::future<void>  entered_;
    std::promise<void> release_promise_;
    std::future<void>  release_future_ = release_promise_.get_future();
    std::atomic<bool>  returned_{false};
};

WorkspaceCatalogSource blocking_catalog_source(BlockingCatalogRead& gate,
                                               const WorkspaceRecord& record) {
    WorkspaceCatalogSource source;
    source.list    = [record] { return std::vector<WorkspaceRecord>{record}; };
    source.is_live = [](const WorkspaceId&) { return false; };
    source.read    = [&gate](const WorkspaceRecord& input, bool live) {
        gate.enter();
        gate.wait_release();
        gate.mark_returned();
        WorkspaceHistory history;
        history.id            = input.id;
        history.title         = input.displayTitle;
        history.canonicalPath = input.canonicalPath.string();
        history.live          = live;
        return history;
    };
    return source;
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

WorkspaceModel workspace_model(const WorkspaceId& id) {
    WorkspaceModel model;
    model.id           = id;
    model.title        = "ws";
    model.daemonStatus = DaemonStatus::Dead;
    model.live         = false;
    return model;
}

// Drains the action pump until the newest notice satisfies `match` (or the
// timeout elapses), returning its text (empty on timeout). The spawn/resume
// failure notices are posted from the worker/reply threads, so callers must poll
// rather than assume a single drain observes them.
std::string wait_for_notice(SupervisorHarness& harness,
                            const std::function<bool(const std::string&)>& match,
                            std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        harness.drain_actions();
        if (!harness.model().notices.empty() &&
            match(harness.model().notices.back().text)) {
            return harness.model().notices.back().text;
        }
        std::this_thread::sleep_for(5ms);
    }
    return {};
}

} // namespace

// SW-U12 / SW-I9 (22 §5.1, H2/SW24): destroying the app with a spawn in flight
// must stop+join the owned worker (a bounded quit wait), not return while the
// worker is still inside `ensureRunning`, and must never touch `this` after the
// stop request.
TEST(SupervisorHarnessTest, SW_U12_DestroyMidSpawnJoinsWorker) {
    ShortTempRoot root("ymh_sw_u12");
    std::filesystem::create_directories(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const WorkspaceRecord row = registry->registerWorkspace(root.path() / "ws", "ws");

    BlockingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *registry, {});

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.lifecycle = &lifecycle;
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    ASSERT_TRUE(harness->worker_joinable());

    harness->ensure_workspace_running(row.id, SessionId{"session-1"});
    ASSERT_TRUE(launcher.wait_entered(2s)) << "worker never reached the blocking spawn";
    EXPECT_TRUE(harness->pending_resume().count(row.id) != 0);
    EXPECT_TRUE(harness->ensure_in_flight().count(row.id) != 0);
    EXPECT_EQ(launcher.spawn_calls(), 1);

    // Release the spawn from a watchdog so the destructor's join has something to
    // wait for; a detached worker would let `reset()` return immediately.
    std::thread watchdog([&launcher] {
        std::this_thread::sleep_for(200ms);
        launcher.release();
    });
    const auto start = std::chrono::steady_clock::now();
    harness.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    watchdog.join();

    EXPECT_TRUE(launcher.spawn_returned());
    EXPECT_GE(elapsed, 100ms) << "destructor did not join the in-flight spawn worker";
}

// SW-U19 / SW-I10 (22 §5.1, MEDIUM-2/SW26): destroying the app with a catalog
// build in flight must stop+join the reader before any member teardown; the
// destructor waits for the blocking read instead of detaching the reader.
TEST(SupervisorHarnessTest, SW_U19_DestroyMidCatalogBuildJoinsReader) {
    ShortTempRoot root("ymh_sw_u19");
    std::filesystem::create_directories(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const WorkspaceRecord row = registry->registerWorkspace(root.path() / "ws", "ws");

    BlockingCatalogRead gate;
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->start_catalog_with(blocking_catalog_source(gate, row), 1h);
    harness->refresh_catalog_now();
    ASSERT_TRUE(gate.wait_entered(2s)) << "catalog reader never entered the blocking read";

    std::thread watchdog([&gate] {
        std::this_thread::sleep_for(200ms);
        gate.release();
    });
    const auto start = std::chrono::steady_clock::now();
    harness.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    watchdog.join();

    EXPECT_TRUE(gate.returned());
    EXPECT_GE(elapsed, 100ms) << "destructor did not join the in-flight catalog reader";
}

// SW-U13 (22 §3.2, M4/SW22): a scan reporting a new `boot_id` replaces the stale
// connection (stop + erase + re-attach) and the pending resume survives until a
// later `Attached` consumes it.
TEST(SupervisorHarnessTest, SW_U13_BootIdChangeReplacesConnectionAndConsumesResume) {
    ShortTempRoot root("ymh_sw_u13");
    std::filesystem::create_directories(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const WorkspaceRecord row = registry->registerWorkspace(root.path() / "ws", "ws");

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->on_scan({spec_for(row, "boot-a")});
    harness->drain_actions();
    ASSERT_TRUE(harness->has_connection(row.id));
    ASSERT_EQ(harness->spec_boot_id(row.id), std::optional<std::string>{"boot-a"});

    harness->seed_pending_resume(row.id, SessionId{"session-1"});

    harness->on_scan({spec_for(row, "boot-b")});
    harness->drain_actions();
    EXPECT_TRUE(harness->has_connection(row.id));
    EXPECT_EQ(harness->spec_boot_id(row.id), std::optional<std::string>{"boot-b"})
        << "stale connection was not replaced on a boot_id change";
    EXPECT_TRUE(harness->pending_resume().count(row.id) != 0)
        << "replacement dropped the pending resume";

    harness->on_link_state(row.id, ui::SupervisorLinkState::Attached, "test");
    harness->drain_actions();
    EXPECT_EQ(harness->pending_resume().count(row.id), 0u)
        << "a successful attach did not consume the pending resume";
}

// SW-U14 / SW-U3 (22 §3.2 rule 8, M5/SW21): eviction skips a workspace whose
// spawn is in flight (and keeps its `pending_resume_`), while an idle dead
// workspace is still evicted.
TEST(SupervisorHarnessTest, SW_U14_EvictionSkipsInFlightSpawn) {
    ShortTempRoot root("ymh_sw_u14");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId in_flight{"ws-in-flight"};
    const WorkspaceId idle{"ws-idle"};
    harness->seed_workspace(workspace_model(in_flight));
    harness->seed_workspace(workspace_model(idle));
    harness->seed_pending_resume(in_flight, SessionId{"session-1"});
    harness->seed_pending_resume(idle, SessionId{"session-2"});
    harness->seed_ensure_in_flight(in_flight);

    harness->evict_dead_workspaces({});

    EXPECT_TRUE(harness->model().workspaces.count(in_flight) != 0)
        << "eviction removed a workspace with a spawn in flight";
    EXPECT_TRUE(harness->pending_resume().count(in_flight) != 0)
        << "eviction dropped the in-flight resume";
    EXPECT_EQ(harness->model().workspaces.count(idle), 0u);
    EXPECT_EQ(harness->pending_resume().count(idle), 0u);
}

// SW-U18 (22 §5.2, MEDIUM-1/SW25): the resume success handler never injects a
// workspace. An evicted/unmodeled workspace routes through the notice ring and
// no `WorkspaceModel`/session is created; a modeled workspace is activated.
TEST(SupervisorHarnessTest, SW_U18_ResumeSuccessNeverInjectsWorkspace) {
    ShortTempRoot root("ymh_sw_u18");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId evicted{"ws-evicted"};
    const SessionId   session{"session-1"};

    harness->apply_resume_success(evicted, session);
    EXPECT_EQ(harness->model().workspaces.count(evicted), 0u)
        << "resume success injected a phantom workspace";
    EXPECT_EQ(harness->model().sessions.count(session), 0u);
    EXPECT_FALSE(harness->model().notices.empty())
        << "an evicted resume was not surfaced through the notice ring";

    const WorkspaceId modeled{"ws-modeled"};
    harness->seed_workspace(workspace_model(modeled));
    harness->apply_resume_success(modeled, session);
    EXPECT_EQ(harness->model().workspaces.count(modeled), 1u);
    EXPECT_TRUE(harness->model().session(session) != nullptr);
    const auto modeled_it = harness->model().workspaces.find(modeled);
    ASSERT_NE(modeled_it, harness->model().workspaces.end());
    EXPECT_EQ(modeled_it->second.activeSessionId, session);
}

// RB-15 (22 §5.2/SW25 generalised): the `session.create` reply never injects a
// workspace. A workspace evicted between the submit and its reply is surfaced
// through the notice ring and left unmodeled; a modeled workspace still
// activates the created session.
TEST(SupervisorHarnessTest, RB15_CreateReplyNeverInjectsWorkspace) {
    ShortTempRoot root("ymh_rb15");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId evicted{"ws-evicted"};
    const SessionId   session{"session-1"};
    const std::size_t before = harness->model().workspaces.size();

    harness->apply_create_reply(evicted, session, std::string{});
    EXPECT_EQ(harness->model().workspaces.count(evicted), 0u)
        << "session.create reply injected a phantom workspace";
    EXPECT_EQ(harness->model().workspaces.size(), before)
        << "session.create reply changed the modeled workspace set";
    EXPECT_EQ(harness->model().sessions.count(session), 0u);
    EXPECT_FALSE(harness->model().notices.empty())
        << "an evicted session.create was not surfaced through the notice ring";

    const WorkspaceId modeled{"ws-modeled"};
    harness->seed_workspace(workspace_model(modeled));
    harness->apply_create_reply(modeled, session, std::string{});
    EXPECT_EQ(harness->model().workspaces.count(modeled), 1u);
    ASSERT_TRUE(harness->model().session(session) != nullptr);
    const auto modeled_it = harness->model().workspaces.find(modeled);
    ASSERT_NE(modeled_it, harness->model().workspaces.end());
    EXPECT_EQ(modeled_it->second.activeSessionId, session);
}

// SW-F5 (22 §5.3/§12): `lifecycle == nullptr` surfaces the pinned
// `"cannot start workspace"` notice and never queues a spawn.
TEST(SupervisorHarnessTest, SW_F5_LifecycleUnavailableNotice) {
    SupervisorRunOptions options;
    options.identity = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->ensure_workspace_running(WorkspaceId{"ws"}, SessionId{"session-1"});

    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_EQ(harness->model().notices.back().text, "cannot start workspace");
    EXPECT_TRUE(harness->ensure_in_flight().empty());
}

// SW-F4 (22 §5.3/§12): a registry row pruned between snapshot and selection
// surfaces the pinned `"workspace no longer registered"` notice and never spawns.
TEST(SupervisorHarnessTest, SW_F4_WorkspaceNoLongerRegisteredNotice) {
    ShortTempRoot root("ymh_sw_f4");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));

    ThrowingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *registry, {});

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.lifecycle = &lifecycle;
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->ensure_workspace_running(WorkspaceId{"never-registered"}, SessionId{"session-1"});

    const std::string notice =
        wait_for_notice(*harness, [](const std::string& text) { return !text.empty(); });
    EXPECT_EQ(notice, "workspace no longer registered");
    EXPECT_EQ(launcher.spawn_calls(), 0) << "a pruned row must not spawn";
}

// SW-F1 (22 §5.3/§12): a spawn failure surfaces `"cannot start workspace: <err>"`,
// erases the pending resume and never injects a workspace.
TEST(SupervisorHarnessTest, SW_F1_SpawnFailureNotice) {
    ShortTempRoot root("ymh_sw_f1");
    std::filesystem::create_directories(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const WorkspaceRecord row = registry->registerWorkspace(root.path() / "ws", "ws");

    ThrowingLauncher launcher;
    HostLifecycle    lifecycle(launcher, *registry, {});

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.lifecycle = &lifecycle;
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->ensure_workspace_running(row.id, SessionId{"session-1"});

    const std::string notice = wait_for_notice(*harness, [](const std::string& text) {
        return text.rfind("cannot start workspace: ", 0) == 0;
    });
    EXPECT_EQ(notice, "cannot start workspace: spawn failed");
    EXPECT_EQ(launcher.spawn_calls(), 1);
    EXPECT_EQ(harness->model().workspaces.count(row.id), 0u)
        << "a spawn failure injected a workspace";
    EXPECT_TRUE(harness->pending_resume().empty());
}

// SW-F2 (22 §5.3/§12): a resume submitted with no live connection surfaces
// `"resume failed: <err>"` — the transport-failure branch, distinct from SW-F3's
// `"session not found in <workspace>"` (which needs a live daemon reply).
TEST(SupervisorHarnessTest, SW_F2_ResumeTransportFailureNotice) {
    ShortTempRoot root("ymh_sw_f2");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-1"};
    const SessionId   session{"session-1"};
    harness->seed_workspace(workspace_model(workspace));
    harness->seed_pending_resume(workspace, session);

    harness->on_link_state(workspace, ui::SupervisorLinkState::Attached, "test");

    const std::string notice = wait_for_notice(*harness, [](const std::string& text) {
        return text.rfind("resume failed: ", 0) == 0;
    });
    EXPECT_EQ(notice, "resume failed: no supervisor connection");
    EXPECT_TRUE(harness->pending_resume().empty());
}

// RB-12 addendum (2026-09-17): the permission dialog resolves only on Enter
// against the highlighted option. A bare printable key (previously `y`/`n`/
// digits) must be swallowed and must not resolve — the accident was a slash
// command containing `n` silently denying the request.
TEST(SupervisorHarnessTest, RB12_PermissionDialogResolvesOnlyOnEnter) {
    SupervisorRunOptions options;
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const SessionId           session{"perm-session"};
    const PermissionRequestId request{"perm-1"};

    const auto reopen = [&] {
        harness->open_permission_dialog(session, request, "shell", "rm -rf build");
        ASSERT_TRUE(harness->model().dialog.open);
        ASSERT_EQ(harness->model().dialog.selected, 0);
        ASSERT_FALSE(harness->last_dialog_resolution().has_value());
    };

    reopen();
    for (const std::string key : {"n", "N", "y", "Y", "1", "2", "3", "0", "x", "/"}) {
        EXPECT_TRUE(harness->dispatch_key(key)) << "key must be swallowed: " << key;
        EXPECT_TRUE(harness->model().dialog.open) << "key resolved the dialog: " << key;
        EXPECT_EQ(harness->model().dialog.selected, 0) << "key moved selection: " << key;
        EXPECT_FALSE(harness->last_dialog_resolution().has_value())
            << "key produced a decision: " << key;
    }

    // Escape and Ctrl-C still deny (Once), unchanged by this decision.
    reopen();
    EXPECT_TRUE(harness->dispatch_key("escape"));
    ASSERT_TRUE(harness->last_dialog_resolution().has_value());
    EXPECT_EQ(harness->last_dialog_resolution()->first, payload::PermissionDecisionKind::Deny);
    EXPECT_EQ(harness->last_dialog_resolution()->second, GrantScope::Once);
    EXPECT_FALSE(harness->model().dialog.open);

    reopen();
    EXPECT_TRUE(harness->dispatch_key("ctrl-c"));
    ASSERT_TRUE(harness->last_dialog_resolution().has_value());
    EXPECT_EQ(harness->last_dialog_resolution()->first, payload::PermissionDecisionKind::Deny);
    EXPECT_FALSE(harness->model().dialog.open);

    // Enter resolves the highlighted option; arrows select each of the four.
    struct Expected {
        int                             downs;
        payload::PermissionDecisionKind decision;
        GrantScope                      scope;
    };
    const Expected cases[] = {
        {0, payload::PermissionDecisionKind::Allow, GrantScope::Once},
        {1, payload::PermissionDecisionKind::Allow, GrantScope::Session},
        {2, payload::PermissionDecisionKind::AllowAlways, GrantScope::Always},
        {3, payload::PermissionDecisionKind::Deny, GrantScope::Once},
    };
    for (const Expected& expected : cases) {
        reopen();
        for (int step = 0; step < expected.downs; ++step) {
            EXPECT_TRUE(harness->dispatch_key("down"));
        }
        EXPECT_EQ(harness->model().dialog.selected, expected.downs);
        EXPECT_FALSE(harness->last_dialog_resolution().has_value());
        EXPECT_TRUE(harness->dispatch_key("enter"));
        EXPECT_FALSE(harness->model().dialog.open);
        ASSERT_TRUE(harness->last_dialog_resolution().has_value());
        EXPECT_EQ(harness->last_dialog_resolution()->first, expected.decision);
        EXPECT_EQ(harness->last_dialog_resolution()->second, expected.scope);
    }

    // ArrowUp wraps, and a printable key after reopening still does not resolve.
    reopen();
    EXPECT_TRUE(harness->dispatch_key("up"));
    EXPECT_EQ(harness->model().dialog.selected, 3);
    EXPECT_TRUE(harness->dispatch_key("n"));
    EXPECT_TRUE(harness->model().dialog.open);
    EXPECT_FALSE(harness->last_dialog_resolution().has_value());
}

// User-reported (2026-09-17): the exit popup must open on Terminate and the
// arrow keys must move the highlight. ↑/↓ were previously swallowed while only
// ←/→/Tab toggled, so a user pressing ↓ saw nothing happen. `quit_requested()`
// distinguishes the two dispatched outcomes: Terminate tears the daemons down,
// Cancel does not.
TEST(SupervisorHarnessTest, ExitConfirmArrowsMoveSelectionAndReturnDispatches) {
    SupervisorRunOptions options;
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const std::vector<WorkspaceId> orphaning = {WorkspaceId{"exit-ws"}};

    // The real opener highlights Terminate (index 0) by default.
    harness->open_exit_prompt(orphaning);
    ASSERT_TRUE(harness->model().exitConfirm.open);
    EXPECT_EQ(harness->model().exitConfirm.selected, 0);
    EXPECT_FALSE(harness->quit_requested());

    // ↓ moves the highlight to Cancel; Enter dispatches that selection (cancel).
    EXPECT_TRUE(harness->dispatch_key("down"));
    EXPECT_EQ(harness->model().exitConfirm.selected, 1) << "ArrowDown must move the selection";
    EXPECT_TRUE(harness->dispatch_key("enter"));
    EXPECT_FALSE(harness->model().exitConfirm.open);
    EXPECT_FALSE(harness->quit_requested()) << "Enter must dispatch Cancel, not Terminate";

    // ↑ moves the highlight off the default; ↓ then round-trips back to
    // Terminate, and Enter then dispatches Terminate.
    harness->open_exit_prompt(orphaning);
    EXPECT_EQ(harness->model().exitConfirm.selected, 0);
    EXPECT_TRUE(harness->dispatch_key("up"));
    EXPECT_EQ(harness->model().exitConfirm.selected, 1) << "ArrowUp must move the selection";
    EXPECT_TRUE(harness->dispatch_key("down"));
    EXPECT_EQ(harness->model().exitConfirm.selected, 0);
    EXPECT_TRUE(harness->dispatch_key("enter"));
    EXPECT_FALSE(harness->model().exitConfirm.open);
    EXPECT_TRUE(harness->quit_requested()) << "Enter must dispatch Terminate";
}

// SW-U20 (22 §5.1, SW24/H2): the destructor's stop must wake a parked ensure
// worker. Mutating the wait predicate (`stop_requested()`) and notifying without
// `ensure_mutex_` let a wakeup land between the worker's predicate check and its
// block; the lost wakeup was never re-sent, so `join()` hung forever. The queue
// stays empty so the destructor's stop is the only possible wakeup, and the
// construct/destroy loop retries the narrow race enough times to hit it.
TEST(SupervisorHarnessTest, SW_U20_StopWakesParkedEnsureWorker) {
    constexpr int kIterations = 20000;
    const auto    start      = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        SupervisorRunOptions options;
        options.identity = harness_identity();
        std::unique_ptr<SupervisorHarness> harness =
            make_supervisor_harness(std::move(options));
        ASSERT_TRUE(harness->worker_joinable());
        // An empty queue leaves the worker parked in `ensure_cv_.wait`; the
        // destructor's stop is the sole wakeup.
        harness.reset();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // A lost wakeup hangs in `join()` (caught by the ctest timeout); a correct
    // mutex-serialized stop wakes every parked worker promptly.
    EXPECT_LT(elapsed, 30s) << "destructor join did not wake the parked ensure worker";
}

// 22 §10.1/§10.2 (S3/S4): the supervisor-internal tests the earlier waves could
// not implement because `SupervisorApp` is private. They drive the app through
// the additive `SupervisorHarness` seam and cover the H2/MEDIUM-1/MEDIUM-2/M4/M5
// lifetime fixes.

#include <gtest/gtest.h>

#include <algorithm>
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
    EXPECT_EQ(modeled_it->second.activeSessionId(), session);
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
    EXPECT_EQ(modeled_it->second.activeSessionId(), session);
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
// UX-U14 (25-D8/UX24) is superseded by 45-D2: Tab no longer cycles the palette,
// it completes the selected command once (see UI45_D2_TabCompletesSelected). The
// fixture below gives each composer test one modeled active session.
struct ComposerFixture {
    explicit ComposerFixture(const std::string& name)
        : root(name), registry(WorkspaceRegistry::open(harness_registry_config(root.path()))) {
        SupervisorRunOptions options;
        options.registry = registry.get();
        options.identity = harness_identity();
        harness          = make_supervisor_harness(std::move(options));
        harness->seed_workspace(workspace_model(workspace));
        harness->apply_resume_success(workspace, session);
    }

    const SessionUiState* state() const { return harness->model().session(session); }

    void type(const std::string& text) {
        for (const char character : text) {
            ASSERT_TRUE(harness->dispatch_key(std::string(1, character)));
        }
    }

    ShortTempRoot                      root;
    std::unique_ptr<WorkspaceRegistry> registry;
    std::unique_ptr<SupervisorHarness> harness;
    WorkspaceId                        workspace{"ws-palette"};
    SessionId                          session{"session-palette"};
};

// 45-D1.1 (45-I1): history holds prompts and commands in submission order.
TEST(SupervisorHarnessTest, UI45_D1_HistoryHoldsPromptsAndCommands) {
    ComposerFixture fixture("ymh45d1hist");
    fixture.type("/help");
    ASSERT_TRUE(fixture.harness->dispatch_key("enter"));
    fixture.type("hello world");
    ASSERT_TRUE(fixture.harness->dispatch_key("enter"));

    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->input.history.size(), 2u);
    EXPECT_EQ(state->input.history[0], "/help");
    EXPECT_EQ(state->input.history[1], "hello world");

    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->input.draft, "hello world");
    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->input.draft, "/help");
}

// 45-D1.2 (45-I2): the active list takes precedence over history recall.
TEST(SupervisorHarnessTest, UI45_D1_ArrowPrecedenceListVsHistory) {
    ComposerFixture fixture("ymh45d1prec");
    fixture.type("hi");
    ASSERT_TRUE(fixture.harness->dispatch_key("enter"));
    fixture.type("/");

    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->command_hints.empty());
    const std::size_t before = state->command_hint_selected;

    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->input.draft, "/");
    EXPECT_NE(fixture.state()->command_hint_selected, before);
    ASSERT_EQ(fixture.state()->input.history.size(), 1u);

    ASSERT_TRUE(fixture.harness->dispatch_key("escape"));
    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->input.draft, "hi");
}

// 45-D2.2 (45-I2): ArrowUp/ArrowDown move the highlight and wrap; never edit.
TEST(SupervisorHarnessTest, UI45_D2_ArrowMovesSelection) {
    ComposerFixture fixture("ymh45d2arr");
    fixture.type("/");
    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_GE(state->command_hints.size(), 2u);
    EXPECT_EQ(state->command_hint_selected, 0u);

    ASSERT_TRUE(fixture.harness->dispatch_key("down"));
    EXPECT_EQ(fixture.state()->command_hint_selected, 1u);
    EXPECT_EQ(fixture.state()->input.draft, "/");
    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->command_hint_selected, 0u);
    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->command_hint_selected, state->command_hints.size() - 1);
}

// 45-D2.3 (45-I3): Tab completes the selected command with a trailing space and
// clears the list.
TEST(SupervisorHarnessTest, UI45_D2_TabCompletesSelected) {
    ComposerFixture fixture("ymh45d2tab");
    fixture.type("/");
    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_GE(state->command_hints.size(), 2u);
    ASSERT_TRUE(fixture.harness->dispatch_key("down"));
    const std::string expected = "/" + state->command_hints[1].name + " ";

    ASSERT_TRUE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, expected);
    EXPECT_TRUE(fixture.state()->command_hints.empty());
    EXPECT_EQ(fixture.state()->command_hint_selected, 0u);
}

// 45-D2.3 (45-I3): Tab is completion, not navigation; a second Tab no-ops once
// the draft is no longer a bare `/prefix`.
TEST(SupervisorHarnessTest, UI45_D2_TabIsNotNavigation) {
    ComposerFixture fixture("ymh45d2tabnav");
    fixture.type("/");
    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->command_hints.empty());
    const std::string expected = "/" + state->command_hints[0].name + " ";

    ASSERT_TRUE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, expected);
    EXPECT_TRUE(fixture.state()->command_hints.empty());

    EXPECT_FALSE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, expected);
}

// 45-D2.3/45-D5.5: after Esc the list is hidden, but Tab recomputes the matches
// from the bare `/prefix` and completes the first one; the list stays hidden.
TEST(SupervisorHarnessTest, UI45_D2_TabAfterEscRecomputes) {
    ComposerFixture fixture("ymh45d2tabesc");
    fixture.type("/exi");
    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->command_hints.empty());
    EXPECT_EQ(state->command_hints.front().name, "exit");

    ASSERT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.state()->command_hints.empty());
    ASSERT_TRUE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, "/exit ");
    EXPECT_TRUE(fixture.state()->command_hints.empty());
}

// 45-D2.5 (45-D8): both pinned CommandHint sites pass `{name, display,
// description}`; completion uses `name`, rendering uses `display`.
TEST(SupervisorHarnessTest, UI45_D2_BothCommandHintSitesPinned) {
    ComposerFixture fixture("ymh45d2sites");
    fixture.type("/exi");
    const SessionUiState* state = fixture.state();
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->command_hints.empty());
    const CommandHint& hint = state->command_hints.front();
    EXPECT_EQ(hint.name, "exit");
    EXPECT_EQ(hint.display, "exit(quit)");
    EXPECT_EQ(hint.description, "quit the supervisor");

    ASSERT_TRUE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, "/exit ");
}

// 45-D5.1 (45-I8): Esc hides a visible list without touching the draft.
TEST(SupervisorHarnessTest, UI45_D5_EscHidesList) {
    ComposerFixture fixture("ymh45d5esc");
    fixture.type("/");
    ASSERT_FALSE(fixture.state()->command_hints.empty());

    ASSERT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.state()->command_hints.empty());
    EXPECT_TRUE(fixture.state()->hints_dismissed);
    EXPECT_EQ(fixture.state()->input.draft, "/");
}

// 45-D5.1 (45-I8): Esc never mutates the draft or cursor.
TEST(SupervisorHarnessTest, UI45_D5_EscKeepsDraft) {
    ComposerFixture fixture("ymh45d5draft");
    fixture.type("/he");
    const SessionUiState* before = fixture.state();
    ASSERT_NE(before, nullptr);
    const std::size_t cursor = before->input.cursor;

    ASSERT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_EQ(fixture.state()->input.draft, "/he");
    EXPECT_EQ(fixture.state()->input.cursor, cursor);
}

// 45-D5.2 (45-I9): history recall after Esc does not resurrect the list.
TEST(SupervisorHarnessTest, UI45_D5_EscSurvivesHistoryRecall) {
    ComposerFixture fixture("ymh45d5recall");
    fixture.type("hi");
    ASSERT_TRUE(fixture.harness->dispatch_key("enter"));
    fixture.type("/");
    ASSERT_FALSE(fixture.state()->command_hints.empty());

    ASSERT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.state()->command_hints.empty());
    ASSERT_TRUE(fixture.harness->dispatch_key("up"));
    EXPECT_EQ(fixture.state()->input.draft, "hi");
    EXPECT_TRUE(fixture.state()->command_hints.empty());
    EXPECT_TRUE(fixture.state()->hints_dismissed);
}

// 45-D5.3: deleting the leading `/` hides the list (and clears the flag).
TEST(SupervisorHarnessTest, UI45_D5_DeleteSlashHidesList) {
    ComposerFixture fixture("ymh45d5del");
    fixture.type("/");
    ASSERT_FALSE(fixture.state()->command_hints.empty());

    ASSERT_TRUE(fixture.harness->dispatch_key("backspace"));
    EXPECT_TRUE(fixture.state()->input.draft.empty());
    EXPECT_TRUE(fixture.state()->command_hints.empty());
    EXPECT_FALSE(fixture.state()->hints_dismissed);
}

// 45-D5.4: typing `/` on an empty draft re-opens the list after Esc.
TEST(SupervisorHarnessTest, UI45_D5_SlashReopensAfterEsc) {
    ComposerFixture fixture("ymh45d5reopen");
    fixture.type("/");
    ASSERT_FALSE(fixture.state()->command_hints.empty());
    ASSERT_TRUE(fixture.harness->dispatch_key("escape"));
    EXPECT_TRUE(fixture.state()->command_hints.empty());

    ASSERT_TRUE(fixture.harness->dispatch_key("backspace"));
    EXPECT_TRUE(fixture.state()->input.draft.empty());
    fixture.type("/");
    EXPECT_FALSE(fixture.state()->command_hints.empty());
    EXPECT_FALSE(fixture.state()->hints_dismissed);
}

// 45-D6 §8.2/45-F10: `/mcp` is session-less: with no modeled session it still
// runs `mcp.status` and routes the block to the notice ring.
TEST(SupervisorHarnessTest, UI45_D6_McpWorksWithoutSession) {
    ShortTempRoot root("ymh45d6nosession");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->seed_active_workspace(workspace_model(WorkspaceId{"ws-mcp"}));

    ASSERT_TRUE(harness->dispatch_command_line("/mcp"));
    const std::string notice = wait_for_notice(
        *harness, [](const std::string& text) { return text.rfind("mcp", 0) == 0; });
    EXPECT_FALSE(notice.empty()) << "mcp must render without a session";
}

// 45-D7.1/45-D7.2/45-I17: `/status` with no modeled session renders the local
// lines (version + effective model) to the notice ring with a `(no session)`
// note.
TEST(SupervisorHarnessTest, UI45_D7_StatusUsesEffectiveModel) {
    ShortTempRoot root("ymh45d7model");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    options.version  = "0.1.0";
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->seed_active_workspace(workspace_model(WorkspaceId{"ws-status"}));

    ASSERT_TRUE(harness->dispatch_command_line("/status"));
    const std::string notice = wait_for_notice(
        *harness, [](const std::string& text) { return text.rfind("ymh status", 0) == 0; });
    ASSERT_FALSE(notice.empty());
    EXPECT_NE(notice.find("version:  0.1.0"), std::string::npos);
    EXPECT_NE(notice.find("model:    deepseek-flash"), std::string::npos)
        << "agent.model is empty by default; effective_model must be used";
    EXPECT_NE(notice.find("(no session)"), std::string::npos);
}

// 45-D9.1/45-I20/45-F21: Tab on a non-empty draft never cycles the agent; a
// bare `/prefix` with zero matches is a no-op, not a cycle.
TEST(SupervisorHarnessTest, UI45_D9_TabNoAgentCycleOnNonEmptyDraft) {
    ComposerFixture fixture("ymh45d9draft");
    fixture.type("plain text");
    EXPECT_FALSE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, "plain text");

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(fixture.harness->dispatch_key("backspace"));
    }
    fixture.type("/zzz");
    EXPECT_FALSE(fixture.harness->dispatch_key("tab"));
    EXPECT_EQ(fixture.state()->input.draft, "/zzz");
    EXPECT_TRUE(fixture.state()->status.agent.empty());
    EXPECT_TRUE(fixture.state()->status.pending_agent.empty());
}

// 45-D9.1/45-I20: with the command list active Tab completes; Shift+Tab no-ops.
TEST(SupervisorHarnessTest, UI45_D9_TabCompletesWhenListActive) {
    ComposerFixture fixture("ymh45d9list");
    fixture.type("/");
    ASSERT_FALSE(fixture.state()->command_hints.empty());
    EXPECT_FALSE(fixture.harness->dispatch_key("tab-reverse"));
    EXPECT_EQ(fixture.state()->input.draft, "/");
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    ASSERT_FALSE(fixture.state()->input.draft.empty());
    EXPECT_EQ(fixture.state()->input.draft.back(), ' ');
}

// 45-D9.1/45-I20: an empty draft cycles to the next selectable agent via the
// daemon's can_select and updates the active display.
TEST(SupervisorHarnessTest, UI45_D9_TabCyclesAgentWhenNoList) {
    ComposerFixture fixture("ymh45d9cycle");
    fixture.harness->install_agent_replies(
        nlohmann::json{{"agents",
                        nlohmann::json::array(
                            {{{"id", "standard"},
                              {"display_name", "standard"},
                              {"blank", true},
                              {"can_select", false}},
                             {{"id", "second"},
                              {"display_name", "second"},
                              {"blank", true},
                              {"can_select", true}}})},
                       {"active", "standard"},
                       {"default", "standard"}},
        0, nlohmann::json{{"agent", "second"}}, 0);
    ASSERT_TRUE(fixture.state()->input.draft.empty());
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    fixture.harness->drain_actions();
    fixture.harness->drain_actions();
    EXPECT_EQ(fixture.state()->status.agent, "second");
    EXPECT_TRUE(fixture.state()->status.pending_agent.empty());
}

// 45-D9.5/45-I21/45-I28/45-F22: a non-blank session cannot switch; the
// preference is pending, never the active display.
TEST(SupervisorHarnessTest, UI45_D9_PendingAgentNeverActive) {
    ComposerFixture fixture("ymh45d9pending");
    fixture.harness->install_agent_replies(
        nlohmann::json{{"agents",
                        nlohmann::json::array(
                            {{{"id", "standard"},
                              {"display_name", "standard"},
                              {"blank", false},
                              {"can_select", false}},
                             {{"id", "second"},
                              {"display_name", "second"},
                              {"blank", false},
                              {"can_select", false}}})},
                       {"active", "standard"},
                       {"default", "standard"}},
        0, nlohmann::json{{"agent", "second"}}, 0);
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    fixture.harness->drain_actions();
    EXPECT_EQ(fixture.state()->status.agent, "standard");
    EXPECT_EQ(fixture.state()->status.pending_agent, "second");
}

// 45-D9.5/45-I23: a blank session whose roster has no other selectable preset
// no-ops with the pinned notice.
TEST(SupervisorHarnessTest, UI45_D9_SinglePresetNoop) {
    ComposerFixture fixture("ymh45d9single");
    fixture.harness->install_agent_replies(
        nlohmann::json{{"agents",
                        nlohmann::json::array(
                            {{{"id", "standard"},
                              {"display_name", "standard"},
                              {"blank", true},
                              {"can_select", false}}})},
                       {"active", "standard"},
                       {"default", "standard"}},
        0, nlohmann::json{{"agent", "standard"}}, 0);
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    fixture.harness->drain_actions();
    ASSERT_FALSE(fixture.harness->model().notices.empty());
    EXPECT_NE(fixture.harness->model().notices.back().text.find("no other agents available"),
              std::string::npos);
    EXPECT_TRUE(fixture.state()->status.pending_agent.empty());
}

// 45-D9.9/45-I23: an empty roster pushes the pinned notice at most once.
TEST(SupervisorHarnessTest, UI45_D9_EmptyRosterNotice) {
    ComposerFixture fixture("ymh45d9empty");
    fixture.harness->install_agent_replies(
        nlohmann::json{{"agents", nlohmann::json::array()}, {"active", ""}, {"default", ""}},
        0, nlohmann::json{{"agent", ""}}, 0);
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    fixture.harness->drain_actions();
    ASSERT_FALSE(fixture.harness->model().notices.empty());
    EXPECT_NE(fixture.harness->model().notices.back().text.find("no agents configured"),
              std::string::npos);
    const std::size_t count = fixture.harness->model().notices.size();
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    fixture.harness->drain_actions();
    EXPECT_EQ(fixture.harness->model().notices.size(), count);
}

// 45-D9.10/45-I27/45-F12: an agent.list MethodNotFound degrades with a notice
// and disables the surface without a retry loop.
TEST(SupervisorHarnessTest, UI45_D9_MethodNotFoundDegradation) {
    ComposerFixture fixture("ymh45d9notfound");
    fixture.harness->install_agent_replies(
        nlohmann::json::object(), static_cast<int>(protocol::RpcCode::MethodNotFound),
        nlohmann::json::object(), 0);
    EXPECT_TRUE(fixture.harness->dispatch_key("tab"));
    fixture.harness->drain_actions();
    ASSERT_FALSE(fixture.harness->model().notices.empty());
    EXPECT_NE(fixture.harness->model().notices.back().text.find("no agents configured"),
              std::string::npos);
    EXPECT_TRUE(fixture.state()->status.agent.empty());
}

// UX-U15 (25-D9/UX25): Enter accepts the highlighted candidate and dispatches
// it (not the raw draft). `/he` + Enter runs `/help`.
TEST(SupervisorHarnessTest, UX_U15_EnterAcceptsHighlightAndDispatches) {
    ShortTempRoot root("ymh_ux_u15");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity  = harness_identity();

    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-palette"};
    const SessionId   session{"session-palette"};
    harness->seed_workspace(workspace_model(workspace));
    harness->apply_resume_success(workspace, session);

    for (const char c : std::string("/he")) {
        ASSERT_TRUE(harness->dispatch_key(std::string(1, c)));
    }
    const SessionUiState* before = harness->model().session(session);
    ASSERT_NE(before, nullptr);
    ASSERT_FALSE(before->command_hints.empty());
    EXPECT_EQ(before->command_hints[0].name, "help");

    ASSERT_TRUE(harness->dispatch_key("enter"));
    const SessionUiState* after = harness->model().session(session);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->input.draft.empty());
    const bool help_ran =
        std::any_of(after->conversation.entries.begin(), after->conversation.entries.end(),
                    [](const ConversationEntry& entry) { return entry.text == "commands:"; });
    EXPECT_TRUE(help_ran) << "Enter did not dispatch the highlighted /help";
}

// ── 45-D3/D4/D10: the switcher subset, focus exclusion, and the lockout ──────

WorkspaceModel live_workspace_model(const WorkspaceId& id) {
    WorkspaceModel model;
    model.id           = id;
    model.title        = "ws";
    model.daemonStatus = DaemonStatus::Attached;
    model.live         = true;
    return model;
}

SessionCatalogSnapshot catalog_snapshot(const WorkspaceId& workspace, const SessionId& session,
                                        bool live) {
    SessionCatalogSnapshot snapshot;
    snapshot.generation   = 1;
    snapshot.capturedAtMs = 1;
    snapshot.complete     = true;
    WorkspaceHistory history;
    history.id            = workspace;
    history.title         = workspace.value;
    history.canonicalPath = "/" + workspace.value;
    history.live          = live;
    SessionHistoryEntry entry;
    entry.id        = session;
    entry.title     = session.value;
    entry.kind      = "root";
    entry.model     = "m";
    entry.updatedAt = 1;
    history.sessions.push_back(std::move(entry));
    snapshot.workspaces.push_back(std::move(history));
    return snapshot;
}

TEST(SupervisorHarnessTest, UI45_D3_CatalogRefreshOnOpen) {
    ShortTempRoot root("ymh45d3refresh");
    std::filesystem::create_directories(root.path() / "ws");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const WorkspaceRecord row = registry->registerWorkspace(root.path() / "ws", "ws");

    std::atomic<int> reads{0};
    WorkspaceCatalogSource source;
    source.list    = [row] { return std::vector<WorkspaceRecord>{row}; };
    source.is_live = [](const WorkspaceId&) { return false; };
    source.read    = [&reads](const WorkspaceRecord& input, bool live) {
        reads.fetch_add(1);
        WorkspaceHistory history;
        history.id            = input.id;
        history.title         = input.displayTitle;
        history.canonicalPath = input.canonicalPath.string();
        history.live          = live;
        return history;
    };

    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->start_catalog_with(source, 1h);
    EXPECT_EQ(reads.load(), 0);

    ASSERT_TRUE(harness->dispatch_key("ctrl-s"));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (reads.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_GE(reads.load(), 1) << "Ctrl+S did not request a catalog refresh";
}

TEST(SupervisorHarnessTest, UI45_D4_LiveNodeFocusableHistoryReadOnly) {
    ShortTempRoot root("ymh45d4readonly");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId active_workspace{"ws-active"};
    const SessionId active_session{"session-active"};
    harness->seed_workspace(live_workspace_model(active_workspace));
    harness->apply_resume_success(active_workspace, active_session);

    const WorkspaceId workspace{"ws-offline"};
    const SessionId session{"session-offline"};
    harness->on_catalog_snapshot(catalog_snapshot(workspace, session, false));

    for (const char character : std::string("/sessions")) {
        ASSERT_TRUE(harness->dispatch_key(std::string(1, character)));
    }
    ASSERT_TRUE(harness->dispatch_key("enter"));
    ASSERT_EQ(harness->model().mode, UiMode::Switcher);

    ASSERT_TRUE(harness->dispatch_key("enter"));
    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_NE(harness->model().notices.back().text.find("workspace not running"),
              std::string::npos);
}

TEST(SupervisorHarnessTest, UI45_D10_NoActiveSessionAfterSwitcher) {
    ShortTempRoot root("ymh45d10sw");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-live"};
    const SessionId session{"session-live"};
    WorkspaceModel workspace_model = live_workspace_model(workspace);
    SessionCell cell;
    cell.id    = session;
    cell.title = "one";
    workspace_model.sessions.push_back(cell);
    harness->seed_workspace(workspace_model);
    harness->on_catalog_snapshot(catalog_snapshot(workspace, session, true));

    ASSERT_TRUE(harness->dispatch_key("ctrl-s"));
    ASSERT_TRUE(harness->dispatch_key("down"));
    ASSERT_TRUE(harness->dispatch_key("enter"));

    const auto it = harness->model().workspaces.find(workspace);
    ASSERT_NE(it, harness->model().workspaces.end());
    EXPECT_EQ(it->second.activeSessionId(), session);
    ASSERT_NE(harness->model().session(session), nullptr);

    ASSERT_TRUE(harness->dispatch_key("x"));
    ASSERT_NE(harness->model().session(session), nullptr);
    EXPECT_EQ(harness->model().session(session)->input.draft, "x");
}

TEST(SupervisorHarnessTest, UI45_D10_NoActiveSessionAfterSessions) {
    ShortTempRoot root("ymh45d10sess");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-hist"};
    const SessionId session{"session-hist"};
    harness->seed_workspace(live_workspace_model(workspace));
    harness->apply_resume_success(workspace, session);

    const auto it = harness->model().workspaces.find(workspace);
    ASSERT_NE(it, harness->model().workspaces.end());
    EXPECT_EQ(it->second.activeSessionId(), session);
    ASSERT_NE(harness->model().session(session), nullptr);
}

TEST(SupervisorHarnessTest, UI45_D10_SlashWorksAfterRepair) {
    ShortTempRoot root("ymh45d10slash");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-repair"};
    const SessionId session{"session-repair"};
    harness->seed_workspace(live_workspace_model(workspace));
    harness->apply_resume_success(workspace, session);
    harness->forget_session_state(session);
    ASSERT_EQ(harness->model().session(session), nullptr);
    ASSERT_FALSE(harness->model().workspaces.at(workspace).activeSessionId().value.empty());

    ASSERT_TRUE(harness->dispatch_key("/"));
    ASSERT_NE(harness->model().session(session), nullptr);
    EXPECT_FALSE(harness->model().session(session)->command_hints.empty());
}

TEST(SupervisorHarnessTest, UI45_D10_ActivateSessionModels) {
    ShortTempRoot root("ymh45d10act");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-act"};
    const SessionId session{"session-act"};
    harness->seed_workspace(live_workspace_model(workspace));
    ASSERT_EQ(harness->model().session(session), nullptr);

    harness->activate_session(workspace, session);
    ASSERT_NE(harness->model().session(session), nullptr);
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId(), session);
}

TEST(SupervisorHarnessTest, UI45_D10_UnknownSessionRecovery) {
    ShortTempRoot root("ymh45d10recover");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-recover"};
    const SessionId first{"session-first"};
    const SessionId second{"session-second"};
    harness->seed_workspace(live_workspace_model(workspace));
    harness->apply_resume_success(workspace, first);
    harness->apply_resume_success(workspace, second);

    harness->recover_unknown_session(workspace, second);
    EXPECT_EQ(harness->model().session(second), nullptr);
    ASSERT_NE(harness->model().session(first), nullptr);
    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId(), first);
    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_EQ(harness->model().notices.back().text, "session no longer exists");
}

TEST(SupervisorHarnessTest, UI45_D10_UnknownSessionCreatesFallback) {
    ShortTempRoot root("ymh45d10fallback");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-fallback"};
    const SessionId only{"session-only"};
    harness->seed_workspace(live_workspace_model(workspace));
    harness->apply_resume_success(workspace, only);

    harness->recover_unknown_session(workspace, only);
    EXPECT_EQ(harness->model().session(only), nullptr);
    EXPECT_TRUE(harness->model().workspaces.at(workspace).sessions.empty());
    EXPECT_TRUE(harness->model().workspaces.at(workspace).activeSessionId().value.empty());
    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_EQ(harness->model().notices.back().text, "session no longer exists");
}

// 22 §5.3 (SW-F2) + 45-D10: a resume that fails must surface the error and leave
// the previously focused session usable — no cleared model, no dangling focus.
TEST(SupervisorHarnessTest, UI45_D10_ResumeFailureKeepsUsableFocus) {
    ShortTempRoot root("ymh45d10fail");
    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    SupervisorRunOptions options;
    options.registry = registry.get();
    options.identity = harness_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-fail"};
    const SessionId prior{"session-prior"};
    const SessionId doomed{"session-doomed"};
    harness->seed_workspace(live_workspace_model(workspace));
    harness->apply_resume_success(workspace, prior);
    ASSERT_NE(harness->model().session(prior), nullptr);
    ASSERT_EQ(harness->model().workspaces.at(workspace).activeSessionId(), prior);

    harness->seed_pending_resume(workspace, doomed);
    harness->on_link_state(workspace, ui::SupervisorLinkState::Attached, "test");

    const std::string notice = wait_for_notice(*harness, [](const std::string& text) {
        return text.rfind("resume failed: ", 0) == 0;
    });
    EXPECT_EQ(notice, "resume failed: no supervisor connection");

    EXPECT_EQ(harness->model().workspaces.at(workspace).activeSessionId(), prior);
    ASSERT_NE(harness->model().session(prior), nullptr);
    EXPECT_FALSE(harness->model().session(prior)->status.model.empty())
        << "the failed resume cleared the focused session's model";
    harness->dispatch_key("x");
    EXPECT_EQ(harness->model().session(prior)->input.draft, "x");
}

// Spec 16 §8.4.3: crash / failure injection. Deterministic where a clock seam
// exists (in-process watchdog with injected clocks); wall-clock-bound cases are
// called out explicitly. Registry rows are asserted directly.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <csignal>
#include <unistd.h>

#include "support/host_harness.hpp"
#include "support/pty_child.hpp"
#include "support/short_temp.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/host/workspace_host.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/registry/supervisor.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr std::string_view kWorkspaceId = "22222222-2222-4222-8222-222222222222";

RegistryConfig crash_registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path             = root / ".state" / "ymh" / "registry.db";
    config.lock_path           = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots     = {};
    config.lock_retry_budget   = std::chrono::milliseconds{200};
    config.lock_retry_interval = std::chrono::milliseconds{5};
    return config;
}

HostConfig crash_config(const std::filesystem::path& root,
                        const std::string& workspace_id = std::string{kWorkspaceId}) {
    HostConfig config;
    config.workspace             = WorkspaceId{workspace_id};
    config.workspace_root        = root;
    config.socket_path           = root / ".ymh" / "host.sock";
    config.log_sink              = root / ".ymh" / "host.log";
    config.registry              = crash_registry_config(root);
    config.persistence.db_path   = root / ".ymh" / "sessions.db";
    config.persistence.lock_path = root / ".ymh" / "sessions.lock";
    config.foreground            = true;
    config.require_owner         = true;
    config.watchdog_disabled     = false;
    config.heartbeat_interval    = std::chrono::milliseconds{100};
    config.shutdown_grace        = std::chrono::milliseconds{200};
    config.caps.max_llm_concurrency = 2;
    config.store_factory = [] { return std::make_unique<MemorySessionStore>(); };
    config.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        FakeResponseStep step;
        step.text   = "hello";
        step.finish = FinishReason::Stop;
        script.steps.push_back(step);
        return std::make_unique<FakeLLM>(script);
    };
    return config;
}

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return predicate();
}

std::int64_t epoch_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// O-F9/O-F15: an old-heartbeat row (delta > ttl) and a future-heartbeat row
// (delta < 0) are BOTH stale; neither counts toward freshness and both are
// pruned by the next scan, so the watchdog cannot be fooled by clock skew.
TEST(OwnershipCrashInjection, StaleAndNegativeDeltaRowsAreNotFreshAndPruned) {
    ShortTempRoot root("ymh-crash-skew");
    const RegistryConfig config = crash_registry_config(root.path());
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(config);

    const std::int64_t now = epoch_ms_now();
    const std::chrono::milliseconds ttl{15'000};
    const SupervisorRow old_row{SupervisorId{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, 1111,
                                "boot-old", now - 20'000, now - 20'000, std::nullopt};
    const SupervisorRow future_row{SupervisorId{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"}, 2222,
                                   "boot-future", now, now + 5'000, std::nullopt};
    registry->registerSupervisor(old_row);
    registry->registerSupervisor(future_row);

    EXPECT_FALSE(isFresh(old_row, now, ttl));
    EXPECT_FALSE(isFresh(future_row, now, ttl));
    EXPECT_EQ(registry->freshSupervisorCount(now, ttl), 0u)
        << "a stale or skewed row must never count as an owner";
    EXPECT_EQ(registry->pruneStaleSupervisors(now, ttl), 2u);
    EXPECT_TRUE(registry->listSupervisors().empty());
}

// O-F1/O-F10 (deterministic, injected clock): a supervisor registered and then
// died before it could deregister. Its row goes stale (heartbeat stops) and the
// daemon's watchdog must fire NoOwners and tear down without a wall-clock wait.
TEST(OwnershipCrashInjection, SpawnThenDieStaleRowFiresNoOwners) {
    ShortTempRoot root("ymh-crash-spawn-die");
    const std::filesystem::path canonical = std::filesystem::canonical(root.path());
    HostConfig config = crash_config(canonical);
    config.watchdog_interval = std::chrono::milliseconds{2};
    config.owner_grace       = std::chrono::milliseconds{40};
    config.owner_lease_ttl   = std::chrono::milliseconds{1000};

    const std::int64_t heartbeat = 1'000;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(config.registry);
        registry->registerSupervisor(SupervisorRow{
            SupervisorId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"}, 3333, "boot-dead", heartbeat,
            heartbeat, std::nullopt});
    }

    auto mono_ms = std::make_shared<std::atomic<std::int64_t>>(0);
    auto wall_ms = std::make_shared<std::atomic<std::int64_t>>(heartbeat);
    config.owner_monotonic_clock = [mono_ms] {
        return std::chrono::steady_clock::time_point{std::chrono::milliseconds{mono_ms->load()}};
    };
    config.owner_wall_clock = [wall_ms] {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds{wall_ms->load()}};
    };

    std::unique_ptr<WorkspaceHost> host = WorkspaceHost::create(std::move(config));
    HostExitCode                  code = HostExitCode::Internal;
    std::thread                   runner([&] { code = host->run(); });
    ASSERT_TRUE(wait_until([&] { return host->state() == protocol::HostState::Serving; }, 5s));

    // While the row is fresh the daemon must NOT be ownerless.
    for (std::int64_t step = 1; step <= 5; ++step) {
        mono_ms->store(step);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_FALSE(host->ownerless()) << "a fresh owner row must keep the daemon alive";

    // The supervisor is gone: its heartbeat stops, so the wall clock advances
    // past the lease and the monotonic clock past the grace.
    wall_ms->store(heartbeat + 1'001);
    for (std::int64_t step = 6; step <= 300 && !host->ownerless(); ++step) {
        mono_ms->store(step);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_TRUE(host->ownerless());

    runner.join();
    EXPECT_EQ(code, HostExitCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(canonical / ".ymh" / "host.sock"));
}

// O-F1 (wall-clock, two-process): SIGKILL the only supervisor. The daemon must
// not serve unsupervised past the §5.2 bound: with the default cadence the row
// is stale after ttl (15 s) + grace (5 s), so the daemon exits within 23 s. The
// in-process `SpawnThenDieStaleRowFiresNoOwners` covers the same invariant
// deterministically; this one pins the real binary end to end.
TEST(OwnershipCrashInjection, SigkillSupervisorDaemonExitsWithinBound) {
    ShortTempRoot root("ymh-crash-sigkill");
    root.write(".ymh/config.jsonc", "{\n  \"permissions\": { \"read\": \"allow\" }\n}\n");
    root.write("fake.json", R"([{"text": "hello", "finish": "stop"}])");

    const std::filesystem::path workspace = root.path() / "alpha";
    std::filesystem::create_directories(workspace);
    const std::filesystem::path binary = resolve_ymh_binary();
    if (!HostHarness::binary_supports_host(binary)) {
        GTEST_SKIP() << "ymh --host is not implemented in " << binary;
    }

    const RegistryConfig registry_config = crash_registry_config(root.path());
    WorkspaceId          workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "alpha").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
    env["HOME"]                = root.path().string();
    env["XDG_CONFIG_HOME"]     = (root.path() / ".config").string();
    env["XDG_CACHE_HOME"]      = (root.path() / ".cache").string();
    env["TERM"]                = "xterm-256color";
    env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    PtyChild supervisor;
    ASSERT_TRUE(supervisor.spawn(binary, workspace, env));
    ASSERT_TRUE(supervisor.wait_for("Type a message and press Enter", 30s))
        << supervisor.plain();
    ASSERT_EQ(host_processes(&workspace_id.value).size(), 1u);

    const auto killed_at = std::chrono::steady_clock::now();
    ::kill(supervisor.pid(), SIGKILL);

    const bool exited =
        wait_until([&] { return host_processes(&workspace_id.value).empty(); }, 25s);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - killed_at);
    EXPECT_TRUE(exited) << "the daemon outlived its SIGKILLed owner past the bound";
    EXPECT_LE(elapsed.count(), 24'000)
        << "unsupervised lifetime exceeded the 23 s bound: " << elapsed.count() << " ms";
}

} // namespace

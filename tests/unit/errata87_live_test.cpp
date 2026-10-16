// Spec 87-D8 / Live87.RealDaemonReclaim (opt-in): against a real `ymh --host`
// daemon and the real provider, a durable link survives a transient probe and
// `session.create` still succeeds. Skipped unless YMH_LIVE_LLM=1 and
// DEEPSEEK_API_KEY are set.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "support/host_harness.hpp"
#include "support/live_provider_config.hpp"
#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_presence.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ymh;

RegistryConfig live_registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path = root / ".state" / "ymh" / "registry.db";
    config.lock_path = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots = {};
    config.lock_retry_budget = 500ms;
    config.lock_retry_interval = 5ms;
    return config;
}

TEST(Live87, RealDaemonReclaim) {
    if (!test::live_llm_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    test::ShortTempRoot root("ymh-e87-live");
    test::write_live_provider_config(root.path() / ".config");

    test::HostHarnessOptions options;
    options.binary = test::resolve_ymh_binary();
    options.workspace_root = root.path();
    options.env["YMH_LIVE_LLM"] = "1";
    if (!test::HostHarness::binary_supports_host(options.binary)) {
        GTEST_SKIP() << "ymh --host is not implemented in " << options.binary;
    }

    test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(live_registry_config(root.path()));
    const std::optional<WorkspaceRecord> record = registry->findByCanonicalPath(root.path());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->host.has_value());

    ForkExecLauncher launcher;
    HostLifecycle lifecycle(launcher, *registry);

    const AttachIdentity durable{ui::process_client_instance(),
                                 protocol::ClientRole::Supervisor};
    AttachResult attached = lifecycle.ensureRunning(record->id, durable, AttachBudget{});
    ASSERT_TRUE(attached.connection != nullptr);

    AttachResult probe = lifecycle.ensureRunning(record->id, ui::probe_attach_identity(),
                                                 AttachBudget{});
    ASSERT_TRUE(probe.connection != nullptr);
    probe.connection.reset();

    const nlohmann::json created = attached.connection->request(
        std::string(protocol::method::kSessionCreate), nlohmann::json{{"title", "live87"}});
    EXPECT_FALSE(created.value("session", std::string{}).empty());

    attached.connection.reset();
    static_cast<void>(harness.stop());
}

} // namespace

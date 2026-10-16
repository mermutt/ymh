// Spec 87-D8 / E2E87.RestartStormKeepsLink: a real daemon plus a durable link,
// then a bare-`ymh`-style transient probe, must leave the durable link alive so
// `session.create` still succeeds (no "Broken pipe"). Uses the two-process
// `HostHarness` (real `ymh --host` + fake LLM).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_presence.hpp"

namespace {

using namespace std::chrono_literals;
using namespace ymh;

RegistryConfig harness_registry_config(const std::filesystem::path& root) {
    RegistryConfig config;
    config.db_path = root / ".state" / "ymh" / "registry.db";
    config.lock_path = root / ".state" / "ymh" / "registry.lock";
    config.workspace_roots = {};
    config.lock_retry_budget = 500ms;
    config.lock_retry_interval = 5ms;
    return config;
}

class Err87Integration : public ::testing::Test {
protected:
    void SetUp() override {
        binary_ = test::resolve_ymh_binary();
        if (!test::HostHarness::binary_supports_host(binary_)) {
            GTEST_SKIP() << "ymh --host is not implemented in " << binary_;
        }
    }

    std::filesystem::path binary_;
};

TEST_F(Err87Integration, RestartStormKeepsLink) {
    test::ShortTempRoot root("ymh-e87-storm");
    root.write("fake.json", R"([{"text": "ok"}])");

    test::HostHarnessOptions options;
    options.binary = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    std::unique_ptr<WorkspaceRegistry> registry =
        WorkspaceRegistry::open(harness_registry_config(root.path()));
    const std::optional<WorkspaceRecord> record = registry->findByCanonicalPath(root.path());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->host.has_value());

    ForkExecLauncher launcher;
    HostLifecycle lifecycle(launcher, *registry);

    // The durable link uses the process-wide instance (the identity the TUI's
    // `SupervisorConnection` pins).
    const AttachIdentity durable{ui::process_client_instance(),
                                 protocol::ClientRole::Supervisor};
    AttachResult attached = lifecycle.ensureRunning(record->id, durable, AttachBudget{});
    ASSERT_TRUE(attached.connection != nullptr);
    protocol::HostConnection& link = *attached.connection;

    // A bare-`ymh`-style transient probe (fresh instance + Observer) must not
    // evict the durable link.
    AttachResult probe = lifecycle.ensureRunning(record->id, ui::probe_attach_identity(),
                                                 AttachBudget{});
    ASSERT_TRUE(probe.connection != nullptr);
    probe.connection.reset();

    const nlohmann::json created = link.request(std::string(protocol::method::kSessionCreate),
                                                nlohmann::json{{"title", "storm"}});
    EXPECT_FALSE(created.value("session", std::string{}).empty())
        << "the durable link must survive the probe (no Broken pipe)";

    // A second probe, then another request: idempotent under a restart storm.
    AttachResult probe2 = lifecycle.ensureRunning(record->id, ui::probe_attach_identity(),
                                                  AttachBudget{});
    ASSERT_TRUE(probe2.connection != nullptr);
    probe2.connection.reset();
    EXPECT_TRUE(link.request(std::string(protocol::method::kHostPing), nlohmann::json::object())
                    .contains("server_time_ms"));

    attached.connection.reset();
    const test::ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

} // namespace

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/core/event.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_connection.hpp"

namespace {

using namespace std::chrono_literals;

class ScopedEnv {
public:
    explicit ScopedEnv(const char* key) : key_(key) {
        if (const char* value = std::getenv(key)) {
            previous_ = value;
        }
    }
    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(key_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string                key_;
    std::optional<std::string> previous_;
};

TEST(HostHarness, BuildsPinnedArgv) {
    ymh::test::HostHarnessOptions options;
    options.binary       = "/opt/ymh";
    options.workspace_root = "/tmp/ws";
    options.workspace_id   = "11111111-1111-4111-8111-111111111111";
    options.socket_path    = "/tmp/ws/.ymh/host.sock";

    const std::vector<std::string> argv = ymh::test::HostHarness::build_argv(options);
    const std::vector<std::string> expected = {
        "/opt/ymh",
        "--host",
        "--workspace",
        "11111111-1111-4111-8111-111111111111",
        "--root",
        "/tmp/ws",
        "--socket",
        "/tmp/ws/.ymh/host.sock",
    };
    EXPECT_EQ(argv, expected);
}

TEST(HostHarness, ShortTempSocketFitsSunPath) {
    ymh::test::ShortTempRoot root("ymh-sock");
    EXPECT_TRUE(ymh::test::socket_path_fits(root.host_socket()));
    EXPECT_LT(root.host_socket().string().size(), ymh::test::kUnixSocketPathMax);
}

class HostIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        binary_ = ymh::test::resolve_ymh_binary();
        if (!ymh::test::HostHarness::binary_supports_host(binary_)) {
            GTEST_SKIP() << "ymh --host is not implemented in " << binary_;
        }
    }

    std::filesystem::path binary_;
};

TEST_F(HostIntegration, StartsPingsAndStops) {
    ymh::test::ShortTempRoot root("ymh-host");
    root.write("fake.json", R"([{"text": "hello"}])");

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    ymh::protocol::HostConnection& connection = harness.connect();
    const nlohmann::json pong = connection.request(ymh::protocol::method::kHostPing);
    EXPECT_TRUE(pong.contains("server_time_ms"));

    const ymh::test::ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
    EXPECT_EQ(status.code, 0);
    EXPECT_FALSE(std::filesystem::exists(harness.socket_path()));
}

TEST_F(HostIntegration, CrashIsReported) {
    ymh::test::ShortTempRoot root("ymh-host-crash");
    root.write("fake.json", R"([{"text": "hello"}])");

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    const ymh::test::ExitStatus status = harness.crash(SIGKILL);
    EXPECT_TRUE(status.signalled);
    EXPECT_EQ(status.signal, SIGKILL);
    EXPECT_FALSE(harness.running());
}

TEST_F(HostIntegration, ClaimsRegistryWhileRunningThenReleases) {
    ymh::test::ShortTempRoot root("ymh-host-claim");
    root.write("fake.json", R"([{"text": "hello"}])");

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    ymh::RegistryConfig registry_config;
    registry_config.db_path   = root.path() / ".state" / "ymh" / "registry.db";
    registry_config.lock_path = root.path() / ".state" / "ymh" / "registry.lock";

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    {
        std::unique_ptr<ymh::WorkspaceRegistry> registry =
            ymh::WorkspaceRegistry::openReadOnly(registry_config);
        const std::optional<ymh::WorkspaceRecord> record =
            registry->findByCanonicalPath(root.path());
        ASSERT_TRUE(record.has_value());
        ASSERT_TRUE(record->host.has_value());
        EXPECT_EQ(record->host->pid, static_cast<ymh::HostPid>(harness.pid()));
        EXPECT_EQ(record->host->socketPath, harness.socket_path());
    }

    const ymh::test::ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
    EXPECT_EQ(status.code, 0);

    {
        std::unique_ptr<ymh::WorkspaceRegistry> registry =
            ymh::WorkspaceRegistry::openReadOnly(registry_config);
        const std::optional<ymh::WorkspaceRecord> record =
            registry->findByCanonicalPath(root.path());
        ASSERT_TRUE(record.has_value());
        EXPECT_FALSE(record->host.has_value());
    }
}

TEST_F(HostIntegration, SupervisorConnectionStreamsFromRealDaemon) {
    ymh::test::ShortTempRoot root("ymh-sup-host");
    root.write("fake.json", R"([{"text": "hello from the daemon"}])");

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    ymh::RegistryConfig registry_config;
    registry_config.db_path   = root.path() / ".state" / "ymh" / "registry.db";
    registry_config.lock_path = root.path() / ".state" / "ymh" / "registry.lock";

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    ymh::WorkspaceId workspace_id;
    std::string boot_id;
    {
        std::unique_ptr<ymh::WorkspaceRegistry> registry =
            ymh::WorkspaceRegistry::openReadOnly(registry_config);
        const std::optional<ymh::WorkspaceRecord> record =
            registry->findByCanonicalPath(root.path());
        ASSERT_TRUE(record.has_value());
        ASSERT_TRUE(record->host.has_value());
        workspace_id = record->id;
        boot_id = record->host->bootId.value;
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ymh::protocol::SessionEnvelope> envelopes;
    ymh::ui::SupervisorSink sink;
    sink.on_envelope = [&](const ymh::protocol::SessionEnvelope& envelope) {
        const std::lock_guard lock(mutex);
        envelopes.push_back(envelope);
        cv.notify_all();
    };

    ymh::ui::SupervisorConnectionConfig config;
    config.socket_path = harness.socket_path().string();
    config.workspace = ymh::ui::WorkspaceId{workspace_id.value};
    config.expected_boot_id = boot_id;
    config.client_instance = ymh::protocol::ClientInstanceId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
    config.poll_interval = 10ms;
    config.ping_interval = 10s;
    ymh::ui::SupervisorConnection connection(std::move(config), std::move(sink));
    connection.start();
    ASSERT_TRUE(connection.waitForState(ymh::ui::SupervisorLinkState::Attached, 10s));
    EXPECT_EQ(connection.attachCount(), 1u);

    std::string session;
    std::atomic<bool> created{false};
    connection.submit(std::string(ymh::protocol::method::kSessionCreate),
                      nlohmann::json{{"title", "integration"}},
                      [&](ymh::ui::SupervisorReply reply) {
                          if (reply.ok) {
                              session = reply.result.value("session", std::string{});
                              created.store(true);
                          }
                      });
    ASSERT_TRUE(connection.waitUntil([&] { return created.load(); }, 10s));
    ASSERT_FALSE(session.empty());

    connection.track(ymh::SessionId{session});
    std::atomic<bool> prompted{false};
    connection.submit(std::string(ymh::protocol::method::kAgentPrompt),
                      nlohmann::json{{"session", session}, {"message", "say hello"}},
                      [&](ymh::ui::SupervisorReply reply) { prompted.store(reply.ok); });
    ASSERT_TRUE(connection.waitUntil([&] { return prompted.load(); }, 10s));

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 15s, [&] {
            for (const auto& envelope : envelopes) {
                if (envelope.event.type == ymh::EventType::TurnEnded ||
                    envelope.event.type == ymh::EventType::AssistantChunk) {
                    return true;
                }
            }
            return false;
        })) << "no streamed envelope arrived";
        bool saw_chunk = false;
        for (const auto& envelope : envelopes) {
            if (envelope.event.type == ymh::EventType::AssistantChunk) {
                saw_chunk = true;
            }
        }
        EXPECT_TRUE(saw_chunk);
    }

    connection.stop();
    const ymh::test::ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

TEST_F(HostIntegration, RunRoutesThroughLiveDaemon) {
    ymh::test::ShortTempRoot root("ymh-run-host");
    root.write("fake.json", R"([{"text": "hello from the daemon"}])");

    ymh::test::HostHarnessOptions options;
    options.binary         = binary_;
    options.workspace_root = root.path();
    options.env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    ::setenv("XDG_STATE_HOME", (root.path() / ".state").string().c_str(), 1);
    ::setenv("HOME", root.path().string().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    std::ostringstream out;
    std::ostringstream err;
    const int code = ymh::run_cli(
        {"--workspace", root.path().string(), "run", "say hello"}, out, err);
    EXPECT_EQ(code, 0) << err.str();
    const std::string output = out.str();
    const std::string needle = "hello from the daemon";
    const std::size_t first  = output.find(needle);
    EXPECT_NE(first, std::string::npos) << output;
    EXPECT_EQ(output.find(needle, first + needle.size()), std::string::npos)
        << "assistant text was printed twice: " << output;

    const ymh::test::ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

TEST_F(HostIntegration, RunFallsBackInProcessWithoutDaemon) {
    ymh::test::ShortTempRoot root("ymh-run-nodaemon");
    root.write("fake.json", R"([{"text": "hello in process"}])");

    ScopedEnv state_env("XDG_STATE_HOME");
    ScopedEnv home_env("HOME");
    ScopedEnv config_env("XDG_CONFIG_HOME");
    ScopedEnv fake_env("YMH_FAKE_LLM_SCRIPT");
    ::setenv("XDG_STATE_HOME", (root.path() / ".state").string().c_str(), 1);
    ::setenv("HOME", root.path().string().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);
    ::setenv("YMH_FAKE_LLM_SCRIPT", (root.path() / "fake.json").string().c_str(), 1);

    std::ostringstream out;
    std::ostringstream err;
    const int code = ymh::run_cli(
        {"--workspace", root.path().string(), "run", "say hello"}, out, err);
    EXPECT_EQ(code, 0) << err.str();
    EXPECT_NE(out.str().find("hello in process"), std::string::npos) << out.str();
    EXPECT_FALSE(std::filesystem::exists(root.host_socket()));
}

} // namespace

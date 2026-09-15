#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

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

} // namespace

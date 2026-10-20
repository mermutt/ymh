#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"

namespace {

using namespace ymh;

std::string core_line(const std::string& log) {
    const std::size_t start = log.find("ymh --host: core dumps:");
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t end = log.find('\n', start);
    return log.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

TEST(CoreDumpHost, EnabledEmitsOneLineAndCreatesDirectory) {
    ymh::test::ShortTempRoot root("ymh-e92-core");
    ymh::test::HostHarnessOptions options;
    options.workspace_root = root.path();
    options.env["YMH_CORE_DUMPS"] = "1";

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    const std::string log = harness.read_log();
    EXPECT_NE(log.find("core dumps:"), std::string::npos) << log;
    EXPECT_NE(log.find("pattern="), std::string::npos) << log;
    EXPECT_NE(log.find("dir_status=ok"), std::string::npos) << log;
    const bool reported = log.find("enabled") != std::string::npos ||
                          log.find("already at ceiling") != std::string::npos ||
                          log.find("blocked (RLIMIT_CORE hard limit is 0") != std::string::npos;
    EXPECT_TRUE(reported) << log;

    const std::size_t first = log.find("ymh --host: core dumps:");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(log.find("ymh --host: core dumps:", first + 1), std::string::npos)
        << "exactly one core-dumps line expected: " << log;

    struct stat status {};
    const std::filesystem::path dir = root.path() / ".ymh" / "coredumps";
    ASSERT_EQ(::stat(dir.c_str(), &status), 0) << dir;
    EXPECT_EQ(status.st_mode & 0777, 0700);

    const ymh::test::ExitStatus stopped = harness.stop();
    EXPECT_TRUE(stopped.ok()) << stopped.code;
    EXPECT_FALSE(std::filesystem::exists(root.path() / "core"));
    EXPECT_FALSE(std::filesystem::exists(dir / "core"));
}

TEST(CoreDumpHost, DefaultOffEmitsNothingAndCreatesNoDirectory) {
    ymh::test::ShortTempRoot root("ymh-e92-off");
    ymh::test::HostHarnessOptions options;
    options.workspace_root = root.path();
    options.env["YMH_CORE_DUMPS"] = "";

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    EXPECT_EQ(harness.read_log().find("core dumps:"), std::string::npos)
        << harness.read_log();
    EXPECT_FALSE(std::filesystem::exists(root.path() / ".ymh" / "coredumps"));
    EXPECT_TRUE(harness.stop().ok());
}

TEST(CoreDumpHost, SymlinkedLeafIsRefused) {
    ymh::test::ShortTempRoot root("ymh-e92-symlink");
    std::filesystem::create_directories(root.path() / ".ymh");
    const std::filesystem::path sentinel = root.path() / "sentinel";
    std::filesystem::create_directories(sentinel);
    std::filesystem::permissions(sentinel, std::filesystem::perms::owner_all |
                                                std::filesystem::perms::group_read |
                                                std::filesystem::perms::group_exec |
                                                std::filesystem::perms::others_read |
                                                std::filesystem::perms::others_exec);
    const std::filesystem::path link = root.path() / ".ymh" / "coredumps";
    ASSERT_EQ(::symlink(sentinel.c_str(), link.c_str()), 0);

    ymh::test::HostHarnessOptions options;
    options.workspace_root = root.path();
    options.env["YMH_CORE_DUMPS"] = "1";
    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    const std::string log = harness.read_log();
    EXPECT_NE(log.find("dir_status=unavailable"), std::string::npos) << log;
    EXPECT_EQ(log.find("dir_status=ok"), std::string::npos) << log;
    struct stat status {};
    ASSERT_EQ(::stat(sentinel.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0755);
    EXPECT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_TRUE(harness.stop().ok());
}

TEST(CoreDumpHost, HostileRootStaysAsciiAndBounded) {
    ymh::test::ShortTempRoot base("ymh-e92-hostile");
    std::filesystem::path root = base.path();
    const std::string component_a = "long-component-name-0123456789";
    const std::string component_b = "unicode-\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
    for (int i = 0; i < 4; ++i) {
        root /= component_a;
    }
    root /= component_b;
    std::filesystem::create_directories(root);

    ymh::test::ShortTempRoot socket_base("ymh-e92-sock");
    ymh::test::HostHarnessOptions options;
    options.workspace_root = root;
    options.socket_path = socket_base.path() / "s";
    options.log_sink = socket_base.path() / "host.log";
    options.env["YMH_CORE_DUMPS"] = "1";

    ymh::test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    const std::string line = core_line(harness.read_log());
    ASSERT_FALSE(line.empty());
    EXPECT_LE(line.size(), 512u);
    for (const char raw : line) {
        const auto byte = static_cast<unsigned char>(raw);
        EXPECT_GE(byte, 0x20);
        EXPECT_LE(byte, 0x7e);
    }
    const std::size_t dir_pos = line.find("dir=");
    ASSERT_NE(dir_pos, std::string::npos);
    EXPECT_NE(line.find("...", dir_pos), std::string::npos) << line;
    EXPECT_TRUE(harness.stop().ok());
}

} // namespace

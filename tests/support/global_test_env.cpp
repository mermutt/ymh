#include "support/global_test_env.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

#include "support/live_provider_config.hpp"

namespace ymh::test {
namespace {

std::filesystem::path g_root;
bool                   g_installed = false;

std::filesystem::path isolation_base() {
    if (const char* override_dir = std::getenv("YMH_TEST_TMPDIR");
        override_dir != nullptr && *override_dir != '\0') {
        return override_dir;
    }
    std::error_code       error;
    std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
        return "/tmp";
    }
    return base;
}

class ProcessWideTestEnv final : public ::testing::Environment {
public:
    void SetUp() override {
        std::error_code error;
        g_root = isolation_base() / ("ymh_test_env_" + std::to_string(::getpid()));
        std::filesystem::remove_all(g_root, error);
        std::filesystem::create_directories(g_root / "state", error);
        std::filesystem::create_directories(g_root / "config" / "ymh", error);
        std::filesystem::create_directories(g_root / "cache", error);
        std::filesystem::create_directories(g_root / "home", error);

        // The global config layer is required (21-D12); a minimal valid file
        // keeps children that resolve the default path working without falling
        // back to the developer's real config.
        std::ofstream config_file(g_root / "config" / "ymh" / "config.jsonc",
                                  std::ios::binary);
        config_file << "{}\n";
        config_file.close();

        // Vendor-free provider for spawned daemons (spec 61 §3): the code
        // branch ships no built-in endpoint/model, so a daemon started from an
        // empty config fails `has_provider()` and exits StartupRejected. A
        // process-wide FakeLLM script keeps every spawned child (supervisor ->
        // daemon, HostHarness, PTY) provider-satisfied with no vendor default.
        // Live runs (YMH_LIVE_LLM) opt out and use the real provider.
        const char* live = std::getenv("YMH_LIVE_LLM");
        if (live == nullptr || *live == '\0') {
            const std::filesystem::path script = g_root / "fake_llm.json";
            std::ofstream script_file(script, std::ios::binary);
            script_file << R"([{"text": "ok"}])";
            script_file.close();
            ::setenv("YMH_FAKE_LLM_SCRIPT", script.c_str(), 1);
        } else {
            write_live_provider_config(g_root / "config");
        }

        ::setenv("XDG_STATE_HOME", (g_root / "state").c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", (g_root / "config").c_str(), 1);
        ::setenv("XDG_CACHE_HOME", (g_root / "cache").c_str(), 1);
        ::setenv("HOME", (g_root / "home").c_str(), 1);
        g_installed = true;
    }

    void TearDown() override {
        g_installed = false;
        std::error_code error;
        std::filesystem::remove_all(g_root, error);
        g_root.clear();
    }
};

const bool kRegistered = [] {
    ::testing::AddGlobalTestEnvironment(new ProcessWideTestEnv());
    return true;
}();

}  // namespace

const std::filesystem::path& global_test_env_root() { return g_root; }

bool global_test_env_installed() { return g_installed; }

}  // namespace ymh::test

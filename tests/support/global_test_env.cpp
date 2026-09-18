#include "support/global_test_env.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

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
        // with built-in defaults keeps children that resolve the default path
        // working without falling back to the developer's real config.
        std::ofstream config_file(g_root / "config" / "ymh" / "config.jsonc",
                                  std::ios::binary);
        config_file << "{}\n";
        config_file.close();

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

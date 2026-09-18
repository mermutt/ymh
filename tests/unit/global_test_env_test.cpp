// RB-14 non-vacuity: proves the process-wide isolation fixture is installed.
// If the fixture (or its registration) is removed, these assertions fail
// instead of silently passing.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "support/global_test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/registry/registry.hpp"

namespace {

using namespace ymh;

std::filesystem::path env_path(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path{value} : std::filesystem::path{};
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, std::string value) : name_(name) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string                name_;
    std::optional<std::string> previous_;
};

TEST(GlobalTestEnv, ProcessWideIsolationIsInstalled) {
    ASSERT_TRUE(test::global_test_env_installed())
        << "process-wide test environment was not installed";
    const std::filesystem::path root = test::global_test_env_root();
    ASSERT_FALSE(root.empty());
    ASSERT_TRUE(std::filesystem::is_directory(root));

    EXPECT_EQ(env_path("XDG_STATE_HOME"), root / "state");
    EXPECT_EQ(env_path("XDG_CONFIG_HOME"), root / "config");
    EXPECT_EQ(env_path("XDG_CACHE_HOME"), root / "cache");
    EXPECT_EQ(env_path("HOME"), root / "home");
}

TEST(GlobalTestEnv, DefaultPathsResolveInsideIsolation) {
    const std::filesystem::path root = test::global_test_env_root();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path registry = default_registry_db_path();
    const std::filesystem::path config   = default_global_config_path();
    EXPECT_EQ(registry.string().rfind(root.string(), 0), 0u) << registry;
    EXPECT_EQ(config.string().rfind(root.string(), 0), 0u) << config;
    EXPECT_TRUE(std::filesystem::is_regular_file(config))
        << "the isolated global config was not scaffolded";
}

TEST(GlobalTestEnv, PerTestOverrideWinsOverFixture) {
    const std::filesystem::path root = test::global_test_env_root();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path override_dir = root / "per-test-override";
    ScopedEnv                   state_env("XDG_STATE_HOME", override_dir.string());

    EXPECT_EQ(default_state_dir(), override_dir / "ymh");
    EXPECT_NE(default_state_dir(), root / "state" / "ymh");
}

}  // namespace

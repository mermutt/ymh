#pragma once

// Process-wide test isolation (RB-14). A gtest global environment, installed
// before `RUN_ALL_TESTS`, redirects `XDG_STATE_HOME`, `XDG_CONFIG_HOME`,
// `XDG_CACHE_HOME` and `HOME` to a per-process temporary directory so that
// every test -- and every child process it spawns -- is hermetic by default.
// The previous per-test, opt-in redirection remains valid: a test's own
// `setenv` runs after this fixture's `SetUp` and therefore wins.

#include <filesystem>

namespace ymh::test {

// Root of the per-process isolation tree; empty before the fixture's `SetUp`
// and after its `TearDown`. Layout: `<root>/state`, `<root>/config`,
// `<root>/cache`, `<root>/home`.
[[nodiscard]] const std::filesystem::path& global_test_env_root();

// True only while the global environment is active (between `SetUp` and
// `TearDown`). The non-vacuity test uses this to prove the fixture ran.
[[nodiscard]] bool global_test_env_installed();

}  // namespace ymh::test

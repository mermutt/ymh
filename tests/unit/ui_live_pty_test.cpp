#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>

#include "support/pty_child.hpp"
#include "support/short_temp.hpp"

#ifndef YMH_TEST_BINARY
#define YMH_TEST_BINARY "ymh"
#endif

namespace {

using namespace ymh;

bool live_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

// A token that cannot be predicted by the test fixture, so its appearance in the
// transcript can only come from the model actually echoing it back. It must be
// short enough that the echoed user line never wraps (the PTY is 100 columns).
std::string make_nonce() {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device device;
    const std::uint64_t salt =
        (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
    const std::uint64_t value = ticks ^ salt;

    static constexpr char digits[] = "0123456789abcdef";
    std::string token = "tok";
    for (int shift = 60; shift >= 0; shift -= 4) {
        token.push_back(digits[(value >> shift) & 0xFU]);
    }
    return token;
}

TEST(UiLivePty, StreamsAssistantReply) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    // Hermetic root: the child's registry/config/home all live under this short
    // temp directory, so the live run never touches the developer's real
    // `~/.local/state/ymh/registry.db` or `~/.config/ymh/config.jsonc`.
    test::ShortTempRoot root("ui_live_pty");
    const std::filesystem::path workspace = root.path() / "ws";
    std::filesystem::create_directories(workspace);

    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = root.state_dir().string();
    env["XDG_CONFIG_HOME"] = root.config_dir().string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";

    test::PtyChild child;
    ASSERT_TRUE(child.spawn(YMH_TEST_BINARY, workspace, env));

    // The no-args binary is the M2 supervisor TUI, which auto-creates the first
    // session asynchronously; keystrokes typed before it is active are dropped.
    child.wait_for("Type a message and press Enter", std::chrono::seconds{30});

    const std::string nonce = make_nonce();
    child.write("Reply with exactly this token and nothing else: " + nonce + "\r");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
    bool saw_reply = false;
    bool answered_permission = false;
    while (std::chrono::steady_clock::now() < deadline) {
        child.read_available();
        const std::string plain = child.plain();
        if (!answered_permission && plain.find("Permission required") != std::string::npos) {
            // RB-12 addendum: the dialog resolves only on Enter (default option
            // 0 = Allow once); a bare letter no longer answers it.
            child.write("\r");
            answered_permission = true;
        }
        // Count inside a single rendered frame, not the accumulated buffer: the
        // echoed user line is repainted on every frame, so the accumulated
        // buffer would show the nonce twice with no LLM call at all. One frame
        // holds the echo once and the assistant reply once.
        const std::string frame = child.last_frame();
        if (test::count_occurrences(frame, nonce) >= 2) {
            saw_reply = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    child.write("/exit\r");
    child.terminate();
    test::stop_hosts_for_root(workspace);

    EXPECT_TRUE(saw_reply) << child.plain();
}

// 58-P2 (58-D1/D2/D6): opt-in live navigation against real DeepSeek — delegate
// a task, enter the running child, observe it finish, and return.
TEST(UiLivePty, SubagentNavigationLive) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    test::ShortTempRoot root("ui_live_subagent");
    const std::filesystem::path workspace = root.path() / "ws";
    std::filesystem::create_directories(workspace);

    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = root.state_dir().string();
    env["XDG_CONFIG_HOME"] = root.config_dir().string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";

    test::PtyChild child;
    ASSERT_TRUE(child.spawn(YMH_TEST_BINARY, workspace, env));
    child.wait_for("Type a message and press Enter", std::chrono::seconds{30});

    const std::string nonce = make_nonce();
    child.write("Use the subagent tool to delegate this task: reply with exactly the token " +
                nonce + " and nothing else. Then answer done.\r");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    bool saw_child = false;
    while (std::chrono::steady_clock::now() < deadline) {
        child.read_available();
        const std::string plain = child.plain();
        if (plain.find("Permission required") != std::string::npos) {
            child.write("\r");
        }
        if (plain.find("subagents:") != std::string::npos &&
            plain.find("✓") != std::string::npos) {
            saw_child = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    ASSERT_TRUE(saw_child) << child.plain();

    child.write("\x14");
    child.wait_for("Enter enter · Esc close", std::chrono::seconds{15});
    child.write("\x1b[B\r");
    child.wait_for("Esc return", std::chrono::seconds{15});
    EXPECT_NE(child.plain().find("↳ "), std::string::npos);

    child.write("\x1b");
    child.write("/exit\r");
    child.terminate();
    test::stop_hosts_for_root(workspace);
}

} // namespace

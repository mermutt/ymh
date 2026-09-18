// SW-P3 (22 §10.4, L5): live-only. Resume a stored session through
// `ymh --resume` against real DeepSeek and send a follow-up. The assertion
// counts an unpredictable, server-echoed nonce inside a single rendered frame,
// so it cannot pass by matching the prompt echo and it fails with a bogus
// `DEEPSEEK_API_KEY` (no reply means no second occurrence).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "support/pty_child.hpp"
#include "support/short_temp.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session_persistence.hpp"

#ifndef YMH_TEST_BINARY
#define YMH_TEST_BINARY "ymh"
#endif

namespace {

using namespace ymh;
using namespace ymh::test;

bool live_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key  = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

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

} // namespace

TEST(UiLiveResumePty, ResumesStoredSessionAndAnswersFollowUp) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    ShortTempRoot root("ui_live_resume");
    const std::filesystem::path workspace = root.path() / "ws";
    std::filesystem::create_directories(workspace);

    ::setenv("XDG_STATE_HOME", root.state_dir().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", root.config_dir().c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);

    RegistryConfig registry_config;
    registry_config.db_path         = root.state_dir() / "ymh" / "registry.db";
    registry_config.lock_path       = root.state_dir() / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    std::string session_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        registry->registerWorkspace(workspace, "live-resume");

        PersistenceConfig config;
        config.db_path   = workspace / ".ymh" / "sessions.db";
        config.lock_path = workspace / ".ymh" / "sessions.lock";
        config.boot_id   = BootId{"live-resume-test"};
        const std::unique_ptr<SessionPersistence> store = SessionPersistence::open(config);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        SessionHeader header;
        header.id            = SessionId{generate_uuid_v4()};
        header.cwd           = std::filesystem::canonical(workspace);
        header.createdAt     = now;
        header.updatedAt     = now;
        header.title         = "live-resume";
        header.model         = "deepseek-flash";
        header.serverProfile = "interactive";
        header.kind          = SessionKind::Root;
        store->create(header);
        session_id = header.id.value;
        store->releaseLease(header.id);
    }

    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"]  = root.state_dir().string();
    env["XDG_CONFIG_HOME"] = root.config_dir().string();
    env["HOME"]            = root.path().string();
    env["TERM"]            = "xterm-256color";

    PtyChild child;
    ASSERT_TRUE(child.spawn(YMH_TEST_BINARY, workspace, env, 30, 100, {"--resume", session_id}));
    ASSERT_TRUE(child.wait_for("live-resume", std::chrono::seconds{60}))
        << "resumed session title never appeared: " << child.plain();
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", std::chrono::seconds{30}))
        << child.plain();

    const std::string nonce = make_nonce();
    child.write("Reply with exactly this token and nothing else: " + nonce + "\r");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
    bool saw_reply = false;
    bool answered_permission = false;
    while (std::chrono::steady_clock::now() < deadline) {
        child.read_available();
        const std::string plain = child.plain();
        if (!answered_permission && plain.find("Permission required") != std::string::npos) {
            // RB-12 addendum: Enter confirms the default option (Allow once);
            // bare letters no longer answer the dialog.
            child.write("\r");
            answered_permission = true;
        }
        const std::string frame = child.last_frame();
        if (count_occurrences(frame, nonce) >= 2) {
            saw_reply = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    child.write("/exit\r");
    child.terminate();
    stop_hosts_for_root(workspace);

    EXPECT_TRUE(saw_reply) << child.plain();
}

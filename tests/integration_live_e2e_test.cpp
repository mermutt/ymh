// Milestone 2, Wave 6: live opt-in end-to-end PTY tests.
//
// These drive the *full* M2 stack — the supervisor TUI (`ymh` with no args)
// attaching to a real `WorkspaceHost` daemon that talks to the real DeepSeek
// API — through a scripted PTY (docs/design/10-supervisor-tui.md §12,
// docs/design/00-architecture.md §44, docs/design/11-m2-errata.md §8/§10).
//
// Gated on `YMH_LIVE_LLM=1` plus a non-empty `DEEPSEEK_API_KEY`: without both
// the tests GTEST_SKIP, so the default hermetic suite never touches the network.
// Every daemon is stopped explicitly (HostDaemonGuard) and the daemon is located
// via `/proc/<pid>/cmdline`, so no `ymh --host` process is ever orphaned.

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/host_harness.hpp"
#include "support/pty_child.hpp"
#include "support/short_temp.hpp"
#include "ymh/core/event.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr const char* kSecret = "zephyr-quokka-4721";
constexpr const char* kPrompt = "Read note.txt and tell me the secret word";
constexpr const char* kSessionActive = "Type a message and press Enter";
constexpr const char* kLiveConfig =
    "[permissions]\n"
    "read = \"allow\"\n"
    "write = \"allow\"\n"
    "shell = \"allow\"\n"
    "\n"
    "[agent]\n"
    "reasoning_effort = \"low\"\n";

bool live_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key  = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

void set_env(const char* key, const std::string& value) { ::setenv(key, value.c_str(), 1); }

RegistryConfig registry_config_for(const std::filesystem::path& state_dir) {
    RegistryConfig config;
    config.db_path         = state_dir / "ymh" / "registry.db";
    config.lock_path       = state_dir / "ymh" / "registry.lock";
    config.workspace_roots = {};
    return config;
}

WorkspaceId register_workspace(const RegistryConfig& config, const std::filesystem::path& root,
                               const std::string& title) {
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(config);
    return registry->registerWorkspace(root, title).id;
}

// A short temp workspace with a secret-bearing `note.txt`, a permissive live
// config, and a registry row whose id is known up front so the daemon can be
// reaped even if the TUI exits unexpectedly.
class LiveWorkspace {
public:
    explicit LiveWorkspace(const std::string& prefix) : root_(prefix) {
        root_.write("note.txt", std::string{"The secret word is "} + kSecret + ".\n");
        root_.write(".ymh/config.toml", kLiveConfig);
        set_env("XDG_STATE_HOME", root_.state_dir().string());
        set_env("HOME", root_.path().string());
        set_env("XDG_CONFIG_HOME", root_.config_dir().string());
        set_env("XDG_CACHE_HOME", (root_.path() / ".cache").string());
        id_ = register_workspace(registry_config_for(root_.state_dir()), root_.path(), "live-e2e");
    }

    LiveWorkspace(const LiveWorkspace&) = delete;
    LiveWorkspace& operator=(const LiveWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_.path(); }
    [[nodiscard]] const WorkspaceId& id() const noexcept { return id_; }
    [[nodiscard]] std::filesystem::path socket() const { return root_.host_socket(); }

    [[nodiscard]] std::map<std::string, std::string> child_env() const {
        std::map<std::string, std::string> env;
        env["XDG_STATE_HOME"]  = root_.state_dir().string();
        env["HOME"]            = root_.path().string();
        env["XDG_CONFIG_HOME"] = root_.config_dir().string();
        env["XDG_CACHE_HOME"]  = (root_.path() / ".cache").string();
        env["TERM"]            = "xterm-256color";
        return env;
    }

private:
    ShortTempRoot root_;
    WorkspaceId   id_;
};

// Waits for the supervisor to auto-create the first session, sends the prompt,
// and returns once `needle` is rendered. Answers a permission dialog if the
// model chooses `shell` (read is allowed by config, so this is a safety net).
bool drive_live_turn(PtyChild& tui, const std::string& needle, std::chrono::seconds timeout) {
    if (!tui.wait_for(kSessionActive, 30s)) {
        return false;
    }
    tui.write(std::string{kPrompt} + "\r");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool       answered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        tui.read_available();
        const std::string plain = tui.plain();
        if (!answered && plain.find("Permission required") != std::string::npos) {
            tui.write("y");
            answered = true;
        }
        if (plain.find(needle) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

std::string first_session_id(const std::filesystem::path& socket) {
    protocol::HostConnection connection;
    connection.connect(socket.string());
    static_cast<void>(connection.handshake(protocol::ServerProfile::Interactive,
                                           protocol::ClientInstanceId{generate_uuid_v4()}));
    const nlohmann::json listed =
        connection.request(std::string(protocol::method::kSessionList), nlohmann::json::object());
    std::string session;
    if (listed.is_array()) {
        for (const nlohmann::json& entry : listed) {
            session = entry.value("id", std::string{});
            if (!session.empty()) {
                break;
            }
        }
    }
    connection.close();
    return session;
}

// The authoritative stream: `session.replay` from `beginning`, terminated by
// `event.unsubscribed` (the same path the supervisor uses on a fresh attach).
std::vector<protocol::SessionEnvelope> replay_session(const std::filesystem::path& socket,
                                                      const std::string&           session) {
    protocol::HostConnection connection;
    connection.connect(socket.string());
    static_cast<void>(connection.handshake(protocol::ServerProfile::Interactive,
                                           protocol::ClientInstanceId{generate_uuid_v4()}));
    const nlohmann::json params{{"session", session}, {"from", {{"kind", "beginning"}}}};
    static_cast<void>(connection.request(std::string(protocol::method::kSessionReplay), params));

    std::vector<protocol::SessionEnvelope> envelopes;
    const auto                             deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::optional<protocol::Notification> notification =
            connection.nextNotification(200ms);
        if (!notification.has_value()) {
            if (!envelopes.empty()) {
                break;
            }
            continue;
        }
        if (notification->method == protocol::notify::kEventStream) {
            envelopes.push_back(
                notification->params.get<protocol::StreamNotification>().envelope);
            continue;
        }
        if (notification->method == protocol::notify::kEventUnsubscribed) {
            break;
        }
    }
    connection.close();
    return envelopes;
}

TEST(LiveE2E, SupervisorReadsFileAndReplies) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    LiveWorkspace   workspace("ymh-live-e2e");
    HostDaemonGuard guard(workspace.id().value);

    PtyChild tui;
    ASSERT_TRUE(tui.spawn(resolve_ymh_binary(), workspace.path(), workspace.child_env()));
    ASSERT_TRUE(drive_live_turn(tui, kSecret, 180s)) << tui.plain();

    const std::string transcript = tui.plain();
    EXPECT_NE(transcript.find("tool:"), std::string::npos)
        << "no tool line rendered:\n"
        << transcript;
    EXPECT_NE(transcript.find(kSecret), std::string::npos)
        << "assistant never reported the secret word:\n"
        << transcript;

    tui.write("/exit\r");
    tui.terminate();
    guard.stop();
    EXPECT_TRUE(host_processes(&workspace.id().value).empty()) << "daemon leaked";
}

TEST(LiveE2E, CrashRespawnReconnectNoDuplicate) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    LiveWorkspace   workspace("ymh-live-crash");
    HostDaemonGuard guard(workspace.id().value);

    {
        PtyChild first;
        ASSERT_TRUE(first.spawn(resolve_ymh_binary(), workspace.path(), workspace.child_env()));
        ASSERT_TRUE(drive_live_turn(first, kSecret, 180s)) << first.plain();
        first.write("/exit\r");
        first.terminate();
    }

    const std::vector<pid_t> daemons = host_processes(&workspace.id().value);
    ASSERT_FALSE(daemons.empty()) << "no daemon to crash";
    for (const pid_t pid : daemons) {
        ::kill(pid, SIGKILL);
    }
    for (int attempt = 0;
         attempt < 200 && !host_processes(&workspace.id().value).empty(); ++attempt) {
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(host_processes(&workspace.id().value).empty()) << "daemon survived SIGKILL";

    PtyChild second;
    ASSERT_TRUE(second.spawn(resolve_ymh_binary(), workspace.path(), workspace.child_env()));
    ASSERT_TRUE(second.wait_for(kSecret, 60s)) << second.plain();
    std::this_thread::sleep_for(500ms);
    second.read_available();

    const std::string frame = second.last_frame();
    EXPECT_NE(frame.find(kSecret), std::string::npos)
        << "prior messages missing after reconnect:\n"
        << frame;
    EXPECT_EQ(count_occurrences(frame, kPrompt), 1u)
        << "prior user message rendered more than once after reconnect:\n"
        << frame;

    const std::string session = first_session_id(workspace.socket());
    ASSERT_FALSE(session.empty()) << "respawned daemon reported no session";
    const std::vector<protocol::SessionEnvelope> replay =
        replay_session(workspace.socket(), session);
    ASSERT_FALSE(replay.empty()) << "replayed session is empty";

    std::set<std::string> ids;
    std::size_t           user_messages = 0;
    for (const protocol::SessionEnvelope& envelope : replay) {
        EXPECT_TRUE(ids.insert(envelope.event.id.value).second)
            << "duplicate committed event " << envelope.event.id.value;
        if (envelope.event.type == EventType::UserMessage) {
            ++user_messages;
        }
    }
    EXPECT_EQ(user_messages, 1u) << "user message duplicated in the session log";

    second.write("/exit\r");
    second.terminate();
    guard.stop();
    EXPECT_TRUE(host_processes(&workspace.id().value).empty()) << "daemon leaked";
}

} // namespace

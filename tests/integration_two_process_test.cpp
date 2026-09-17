// Milestone 2 acceptance: two-process integration tests (Wave 5).
//
// Every test drives the real `ymh` binary: a `WorkspaceHost` daemon spawned
// either directly (`--host`, via `HostHarness`) or by a supervisor process
// (`HostLifecycle::ensureRunning` / the PTY TUI). The suite is hermetic:
// `YMH_FAKE_LLM_SCRIPT` backs every turn and every workspace lives under a
// short temp root so the Unix socket path fits `sun_path`.
//
// Acceptance matrix (docs/design/11-m2-errata.md §8–§11, 04 §6/§7/§13.2-13.3,
// 10 §5.4/§16.2, and the spec-16 ownership model, 16 §2.1/§4/§8.4.2):
// crash/respawn/reconnect without loss or duplicate; the spawn race yields one
// daemon; SIGSTOP is never stolen; attach identity is cross-checked; a stale
// socket is replaced; multi-supervisor fan-out with permission first-wins;
// CursorInvalid falls back to `beginning`; and no daemon is orphaned.
//
// Spec 16 supersedes the old "supervisor exit leaves the daemon serving"
// acceptance: a daemon does **not** outlive its last owner. A non-last
// supervisor exit leaves it serving while a peer remains (16 §4.1/O11); the
// last supervisor's confirmed exit prompts and tears the daemons down (16
// §4.2/§4.3, O12/C-H2). The two cases are pinned by
// `SupervisorExitWithPeerKeepsDaemon` and `LastSupervisorExitTearsDaemonDown`.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"
#include "ymh/cli/cli.hpp"
#include "ymh/core/event.hpp"
#include "ymh/host/host_launcher.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_connection.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr const char* kFakeTextScript = R"([{"text": "hello from the daemon", "finish": "stop"}])";

constexpr const char* kFakeToolScript = R"([
  {"text": "working", "finish": "tool_calls",
   "tool_calls": [{"name": "shell", "arguments": {"command": "sleep 4"}, "id": "call-1"}]},
  {"text": "done", "finish": "stop"}
])";

constexpr const char* kAllowAllConfig =
    "{\n  \"permissions\": {\n    \"read\": \"allow\",\n    \"write\": \"allow\",\n"
    "    \"shell\": \"allow\"\n  }\n}\n";

void set_env(const char* key, const std::string& value) {
    ::setenv(key, value.c_str(), 1);
}

class ScopedStdinDevNull {
public:
    ScopedStdinDevNull() {
        saved_ = ::dup(STDIN_FILENO);
        const int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            ::close(null_fd);
        }
    }
    ~ScopedStdinDevNull() {
        if (saved_ >= 0) {
            ::dup2(saved_, STDIN_FILENO);
            ::close(saved_);
        }
    }
    ScopedStdinDevNull(const ScopedStdinDevNull&) = delete;
    ScopedStdinDevNull& operator=(const ScopedStdinDevNull&) = delete;

private:
    int saved_{-1};
};

// `/proc/<pid>/cmdline` is a NUL-separated argv blob, so it is parsed rather
// than grepped; a `ymh --host` daemon is recognised by `--host`.
bool is_host_process(pid_t pid, const std::string* workspace_id) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string blob((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    std::string              current;
    for (const char character : blob) {
        if (character == '\0') {
            args.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        args.push_back(std::move(current));
    }
    bool host            = false;
    bool workspace_match = workspace_id == nullptr;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--host") {
            host = true;
        }
        if (workspace_id != nullptr && args[index] == "--workspace" && index + 1 < args.size() &&
            args[index + 1] == *workspace_id) {
            workspace_match = true;
        }
    }
    return host && workspace_match;
}

std::vector<pid_t> host_processes(const std::string* workspace_id) {
    std::vector<pid_t> pids;
    std::error_code    error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator("/proc", error)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.size() > 9 ||
            !std::all_of(name.begin(), name.end(),
                         [](unsigned char character) { return std::isdigit(character) != 0; })) {
            continue;
        }
        const pid_t pid = static_cast<pid_t>(std::stoi(name));
        if (is_host_process(pid, workspace_id)) {
            pids.push_back(pid);
        }
    }
    return pids;
}

std::size_t count_hosts(const std::string& workspace_id) {
    return host_processes(&workspace_id).size();
}

class DaemonGuard {
public:
    explicit DaemonGuard(std::string workspace_id) : workspace_id_(std::move(workspace_id)) {}
    DaemonGuard(const DaemonGuard&) = delete;
    DaemonGuard& operator=(const DaemonGuard&) = delete;
    ~DaemonGuard() { stop(); }

    void stop() {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        for (const pid_t pid : host_processes(&workspace_id_)) {
            ::kill(pid, SIGTERM);
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (host_processes(&workspace_id_).empty()) {
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
        for (const pid_t pid : host_processes(&workspace_id_)) {
            ::kill(pid, SIGKILL);
        }
    }

private:
    std::string workspace_id_;
    bool        stopped_ = false;
};

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

std::optional<WorkspaceRecord> read_record(const RegistryConfig&        config,
                                           const std::filesystem::path& root) {
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::openReadOnly(config);
    return registry->findByCanonicalPath(root);
}

std::unique_ptr<protocol::HostConnection> open_connection(const std::filesystem::path& socket) {
    auto connection = std::make_unique<protocol::HostConnection>();
    connection->connect(socket.string());
    [[maybe_unused]] const protocol::HelloResult hello =
        connection->handshake(protocol::ServerProfile::Interactive,
                              protocol::ClientInstanceId{generate_uuid_v4()});
    return connection;
}

// `session.replay` re-uses the subscribe replay path from `beginning` and
// terminates with `replay_complete`, yielding the store's authoritative stream.
std::vector<protocol::SessionEnvelope> replay_session(protocol::HostConnection& connection,
                                                      const std::string&        session) {
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
    return envelopes;
}

struct EnvelopeCollector {
    std::mutex                             mutex;
    std::condition_variable                cv;
    std::vector<protocol::SessionEnvelope> envelopes;

    void push(const protocol::SessionEnvelope& envelope) {
        {
            std::lock_guard lock(mutex);
            envelopes.push_back(envelope);
        }
        cv.notify_all();
    }

    bool wait_for(const std::function<bool(const std::vector<protocol::SessionEnvelope>&)>& predicate,
                  std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return predicate(envelopes); });
    }

    std::vector<protocol::SessionEnvelope> snapshot() {
        std::lock_guard lock(mutex);
        return envelopes;
    }
};

ui::SupervisorReply submit_sync(ui::SupervisorConnection& connection, const std::string& method,
                                nlohmann::json params, std::chrono::milliseconds timeout = 15s) {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ui::SupervisorReply     captured;
    connection.submit(method, std::move(params), [&](ui::SupervisorReply reply) {
        {
            std::lock_guard lock(mutex);
            captured = std::move(reply);
            done     = true;
        }
        cv.notify_all();
    });
    std::unique_lock lock(mutex);
    cv.wait_for(lock, timeout, [&] { return done; });
    return captured;
}

std::set<std::string> event_ids(const std::vector<protocol::SessionEnvelope>& envelopes) {
    std::set<std::string> ids;
    for (const protocol::SessionEnvelope& envelope : envelopes) {
        ids.insert(envelope.event.id.value);
    }
    return ids;
}

std::vector<std::string> event_id_order(
    const std::vector<protocol::SessionEnvelope>& envelopes) {
    std::vector<std::string> ids;
    ids.reserve(envelopes.size());
    for (const protocol::SessionEnvelope& envelope : envelopes) {
        ids.push_back(envelope.event.id.value);
    }
    return ids;
}

bool is_subsequence(const std::vector<std::string>& needle, const std::vector<std::string>& hay) {
    std::size_t index = 0;
    for (const std::string& value : hay) {
        if (index < needle.size() && needle[index] == value) {
            ++index;
        }
    }
    return index == needle.size();
}

std::string strip_ansi(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() &&
                   (input[index] == ';' || (input[index] >= '0' && input[index] <= '9'))) {
                ++index;
            }
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

class PtyChild {
public:
    ~PtyChild() { terminate(); }

    bool spawn(const std::filesystem::path& binary, const std::filesystem::path& cwd,
               const std::map<std::string, std::string>& env) {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) {
            return false;
        }
        if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            return false;
        }
        winsize window{};
        window.ws_row = 30;
        window.ws_col = 100;
        ::ioctl(master_, TIOCSWINSZ, &window);
        const char* slave_name = ::ptsname(master_);
        if (slave_name == nullptr) {
            return false;
        }
        const int slave = ::open(slave_name, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            return false;
        }
        pid_ = ::fork();
        if (pid_ < 0) {
            return false;
        }
        if (pid_ == 0) {
            ::setsid();
            ::ioctl(slave, TIOCSCTTY, 0);
            ::dup2(slave, STDIN_FILENO);
            ::dup2(slave, STDOUT_FILENO);
            ::dup2(slave, STDERR_FILENO);
            if (slave > 2) {
                ::close(slave);
            }
            ::close(master_);
            for (const auto& [key, value] : env) {
                ::setenv(key.c_str(), value.c_str(), 1);
            }
            if (::chdir(cwd.c_str()) != 0) {
                ::_exit(126);
            }
            ::execl(binary.c_str(), binary.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(slave);
        return true;
    }

    void write(const std::string& bytes) {
        if (master_ >= 0) {
            static_cast<void>(::write(master_, bytes.data(), bytes.size()));
        }
    }

    bool wait_for(const std::string& needle, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            read_available();
            if (strip_ansi(buffer_).find(needle) != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        read_available();
        return strip_ansi(buffer_).find(needle) != std::string::npos;
    }

    [[nodiscard]] std::string plain() const { return strip_ansi(buffer_); }

    // Waits for the TUI process to exit on its own (no signal) and returns its
    // exit status, or nullopt on timeout. Used by the spec-16 last-exit tests
    // to pin "S2 exits 0" (16 §8.4.2 scenario 5) without a sleep.
    [[nodiscard]] std::optional<int> wait_for_exit(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (pid_ > 0 && std::chrono::steady_clock::now() < deadline) {
            read_available();
            int         status = 0;
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                }
                if (WIFSIGNALED(status)) {
                    return 128 + WTERMSIG(status);
                }
                return -1;
            }
            std::this_thread::sleep_for(20ms);
        }
        return std::nullopt;
    }

    void terminate() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                    pid_ = -1;
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
            if (pid_ > 0) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
                pid_ = -1;
            }
        }
        if (master_ >= 0) {
            ::close(master_);
            master_ = -1;
        }
    }

private:
    void read_available() {
        if (master_ < 0) {
            return;
        }
        pollfd descriptor{};
        descriptor.fd     = master_;
        descriptor.events = POLLIN;
        while (::poll(&descriptor, 1, 0) > 0) {
            char          chunk[4096];
            const ssize_t count = ::read(master_, chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            buffer_.append(chunk, static_cast<std::size_t>(count));
        }
    }

    int         master_{-1};
    pid_t       pid_{-1};
    std::string buffer_;
};

class TwoProcess : public ::testing::Test {
protected:
    void SetUp() override {
        binary_ = resolve_ymh_binary();
        if (!HostHarness::binary_supports_host(binary_)) {
            GTEST_SKIP() << "ymh --host is not implemented in " << binary_;
        }
    }

    void configure_workspace(const ShortTempRoot& root) {
        set_env("XDG_STATE_HOME", (root.path() / ".state").string());
        set_env("HOME", root.path().string());
        set_env("XDG_CONFIG_HOME", (root.path() / ".config").string());
        set_env("XDG_CACHE_HOME", (root.path() / ".cache").string());
        // M4: every daemon spawned by this fixture needs a required global
        // config (21-D12), including `TwoSpawnsExactlyOneDaemon`, which starts
        // no harness. Write it here so it is ordering-independent.
        root.write(".config/ymh/config.jsonc", "{}\n");
    }

    HostHarnessOptions options_for(const ShortTempRoot& root, const std::string& workspace_id,
                                   const std::filesystem::path& script) {
        HostHarnessOptions options;
        options.binary                     = binary_;
        options.workspace_root             = root.path();
        options.workspace_id               = workspace_id;
        options.socket_path                = root.host_socket();
        options.log_sink                   = root.host_log();
        options.env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
        options.env["HOME"]                = root.path().string();
        options.env["YMH_FAKE_LLM_SCRIPT"] = script.string();
        return options;
    }

    std::filesystem::path binary_;
};

TEST_F(TwoProcess, CrashRespawnReconnectNoLossNoDup) {
    ShortTempRoot root("ymh-2p-crash");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeToolScript);
    const std::filesystem::path script = root.path() / "fake.json";

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "crash").value;

    HostHarness harness(options_for(root, workspace_id, script));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    EnvelopeCollector  collector;
    ui::SupervisorSink sink;
    sink.on_envelope = [&collector](const protocol::SessionEnvelope& envelope) {
        collector.push(envelope);
    };

    ui::SupervisorConnectionConfig config;
    config.socket_path       = harness.socket_path().string();
    config.workspace         = ui::WorkspaceId{workspace_id};
    config.expected_boot_id  = {};
    config.client_instance   = protocol::ClientInstanceId{generate_uuid_v4()};
    config.poll_interval     = 10ms;
    config.reconnect_backoff = 50ms;
    ui::SupervisorConnection connection(config, std::move(sink));
    connection.start();
    ASSERT_TRUE(connection.waitForState(ui::SupervisorLinkState::Attached, 15s));

    const ui::SupervisorReply created =
        submit_sync(connection, std::string(protocol::method::kSessionCreate),
                    nlohmann::json{{"title", "crash"}});
    ASSERT_TRUE(created.ok) << created.error;
    const std::string session = created.result.value("session", std::string{});
    ASSERT_FALSE(session.empty());

    connection.track(SessionId{session});
    ASSERT_TRUE(connection.waitUntil(
        [&connection, &session] { return connection.subscribed(SessionId{session}); }, 10s));

    const ui::SupervisorReply prompted =
        submit_sync(connection, std::string(protocol::method::kAgentPrompt),
                    nlohmann::json{{"session", session}, {"message", "run the tool"}});
    ASSERT_TRUE(prompted.ok) << prompted.error;

    ASSERT_TRUE(collector.wait_for(
        [](const std::vector<protocol::SessionEnvelope>& envelopes) {
            return std::any_of(envelopes.begin(), envelopes.end(), [](const auto& envelope) {
                return envelope.event.type == EventType::ToolCall;
            });
        },
        15s))
        << "turn never reached the blocking tool call";

    const ExitStatus crash = harness.crash(SIGKILL);
    EXPECT_TRUE(crash.signalled);

    ForkExecLauncher                   launcher(binary_);
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    HostLifecycle                      lifecycle(launcher, *registry);
    AttachResult                       attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                       protocol::ClientRole::Supervisor});
    ASSERT_NE(attach.connection, nullptr);
    attach.connection->close();

    ASSERT_TRUE(connection.waitUntil(
        [&connection] { return connection.attachCount() >= 2; }, 20s))
        << "supervisor never reconnected to the respawned daemon";

    std::vector<protocol::SessionEnvelope> authoritative;
    const auto                             converge_deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < converge_deadline) {
        auto probe   = open_connection(harness.socket_path());
        authoritative = replay_session(*probe, session);
        probe->close();
        const std::set<std::string> expected = event_ids(authoritative);
        const std::set<std::string> received = event_ids(collector.snapshot());
        if (!expected.empty() &&
            std::includes(received.begin(), received.end(), expected.begin(), expected.end())) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    const std::vector<protocol::SessionEnvelope> received          = collector.snapshot();
    const std::set<std::string>                  received_ids      = event_ids(received);
    const std::set<std::string>                  authoritative_ids = event_ids(authoritative);

    for (const std::string& id : authoritative_ids) {
        EXPECT_TRUE(received_ids.count(id) == 1) << "missing committed event " << id;
    }
    EXPECT_EQ(received_ids.size(), received.size());
    EXPECT_TRUE(is_subsequence(event_id_order(received), event_id_order(authoritative)));

    const ui::SupervisorReply status =
        submit_sync(connection, std::string(protocol::method::kAgentStatus),
                    nlohmann::json{{"session", session}});
    ASSERT_TRUE(status.ok) << status.error;
    EXPECT_EQ(status.result.value("status", std::string{}), "Idle");

    const std::size_t head_before = authoritative.size();
    std::this_thread::sleep_for(500ms);
    {
        auto probe = open_connection(harness.socket_path());
        const std::vector<protocol::SessionEnvelope> after = replay_session(*probe, session);
        probe->close();
        EXPECT_EQ(after.size(), head_before) << "a turn auto-started after reconnect";
    }

    connection.stop();
    harness.stop();
}

std::map<std::string, std::string> supervisor_env(const ShortTempRoot& root) {
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
    env["HOME"]                = root.path().string();
    env["TERM"]                = "xterm-256color";
    env["YMH_FAKE_LLM_SCRIPT"] = (root.path() / "fake.json").string();
    return env;
}

// 16 §8.4.2 (4), O11: a NON-last supervisor exit. Two supervisors own the same
// daemon; the first quits cleanly, so its orphaning set is empty (no prompt),
// the daemon keeps serving, and the first supervisor's presence row is gone.
TEST_F(TwoProcess, SupervisorExitWithPeerKeepsDaemon) {
    ShortTempRoot root("ymh-2p-peer");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const std::filesystem::path workspace = root.path() / "alpha";
    std::filesystem::create_directories(workspace);

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    WorkspaceId          workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "alpha").id;
    }
    DaemonGuard guard(workspace_id.value);
    const std::map<std::string, std::string> env = supervisor_env(root);

    PtyChild first;
    ASSERT_TRUE(first.spawn(binary_, workspace, env));
    ASSERT_TRUE(first.wait_for("Type a message and press Enter", 30s))
        << "first supervisor did not become ready";

    PtyChild second;
    ASSERT_TRUE(second.spawn(binary_, workspace, env));
    ASSERT_TRUE(second.wait_for("Type a message and press Enter", 30s))
        << "second supervisor did not attach";

    auto supervisor_rows = [&]() {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::openReadOnly(registry_config);
        return registry->listSupervisors().size();
    };
    auto wait_for = [&](auto predicate, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        return predicate();
    };

    ASSERT_TRUE(wait_for([&] { return supervisor_rows() >= 2; }, 15s))
        << "both supervisors must register before the exit";
    const std::size_t rows_before = supervisor_rows();

    first.write("/exit\r");
    const std::optional<int> first_status = first.wait_for_exit(20s);
    ASSERT_TRUE(first_status.has_value()) << first.plain();
    EXPECT_EQ(*first_status, 0) << "a non-last supervisor exits cleanly with no prompt";

    EXPECT_TRUE(wait_for([&] { return supervisor_rows() == rows_before - 1; }, 15s))
        << "the exiting supervisor's presence row must be deregistered";

    ASSERT_EQ(count_hosts(workspace_id.value), 1u)
        << "the daemon must survive while a peer supervisor remains";

    // `second` is now the sole owner, so its own clean exit prompts and tears the
    // daemon down; a prompt here proves `first`'s exit did not orphan the daemon.
    second.write("/exit\r");
    ASSERT_TRUE(second.wait_for("Exiting will terminate", 20s)) << second.plain();
    second.write("y");
    const std::optional<int> second_status = second.wait_for_exit(30s);
    ASSERT_TRUE(second_status.has_value()) << second.plain();
    EXPECT_EQ(*second_status, 0);
    EXPECT_TRUE(wait_for([&] { return count_hosts(workspace_id.value) == 0; }, 20s))
        << "the last supervisor's exit must tear the daemon down";
}

// 16 §8.4.2 (5), O12/C-H2: the LAST supervisor's clean exit. The prompt fires
// because the connection's instance equals the registered row (other_fresh_
// owners == 0); "y" confirms, the daemon runs the ordered teardown, and the
// supervisor exits 0. This is the regression C-H2 guards: a fresh random
// instance at hello would leave other_fresh_owners > 0 and no prompt.
TEST_F(TwoProcess, LastSupervisorExitTearsDaemonDown) {
    ShortTempRoot root("ymh-2p-last");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const std::filesystem::path workspace = root.path() / "alpha";
    std::filesystem::create_directories(workspace);

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    WorkspaceId          workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "alpha").id;
    }
    DaemonGuard guard(workspace_id.value);

    PtyChild child;
    ASSERT_TRUE(child.spawn(binary_, workspace, supervisor_env(root)));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 30s))
        << "supervisor did not auto-create a session";

    ASSERT_EQ(count_hosts(workspace_id.value), 1u);

    child.write("/exit\r");
    ASSERT_TRUE(child.wait_for("Exiting will terminate", 20s)) << child.plain();
    ASSERT_EQ(count_hosts(workspace_id.value), 1u)
        << "declining would keep the daemon; the prompt alone must not stop it";

    child.write("y");
    const std::optional<int> status = child.wait_for_exit(30s);
    ASSERT_TRUE(status.has_value()) << child.plain();
    EXPECT_EQ(*status, 0);

    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (count_hosts(workspace_id.value) != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(count_hosts(workspace_id.value), 0u) << "the last owner's exit must stop the daemon";
    EXPECT_FALSE(std::filesystem::exists(workspace / ".ymh" / "host.sock"))
        << "the daemon must unlink its socket on the ordered teardown";

    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::openReadOnly(registry_config);
    EXPECT_TRUE(registry->listSupervisors().empty()) << "no ownership row may survive a clean exit";
}

TEST_F(TwoProcess, TwoSpawnsExactlyOneDaemon) {
    ShortTempRoot root("ymh-2p-race");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);
    const std::filesystem::path script = root.path() / "fake.json";

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "race").value;
    DaemonGuard guard(workspace_id);

    auto ensure = [&]() -> bool {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        ForkExecLauncher                   launcher(binary_);
        HostLifecycle                      lifecycle(launcher, *registry);
        AttachResult attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                       protocol::ClientRole::Supervisor});
        if (attach.connection == nullptr) {
            return false;
        }
        attach.connection->close();
        return true;
    };

    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    auto run = [&]() {
        while (!go.load()) {
            std::this_thread::yield();
        }
        if (ensure()) {
            ready.fetch_add(1);
        }
    };
    std::thread first(run);
    std::thread second(run);
    go.store(true);
    first.join();
    second.join();

    EXPECT_EQ(ready.load(), 2) << "both supervisors must attach to the winner";
    ASSERT_EQ(count_hosts(workspace_id), 1u) << "exactly one daemon must serve the workspace";

    ChildOptions loser;
    loser.argv = {binary_.string(), "--host",     "--workspace", workspace_id,
                  "--root",         root.path().string(), "--socket", root.host_socket().string()};
    loser.executable  = binary_;
    loser.cwd         = root.path();
    loser.stdout_path = root.path() / ".ymh" / "loser.stdout.log";
    loser.stderr_path = root.path() / ".ymh" / "loser.stderr.log";
    loser.env["XDG_STATE_HOME"]      = (root.path() / ".state").string();
    loser.env["HOME"]                = root.path().string();
    loser.env["YMH_FAKE_LLM_SCRIPT"] = script.string();
    const ExitStatus loser_status = run_child(std::move(loser), 10s);
    ASSERT_TRUE(loser_status.exited);
    EXPECT_EQ(loser_status.code, static_cast<int>(HostExitCode::WorkspaceBusy));
    EXPECT_EQ(count_hosts(workspace_id), 1u);
}

TEST_F(TwoProcess, SigstopNeverStolen) {
    ShortTempRoot root("ymh-2p-stop");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "ws").value;

    HostHarness harness(options_for(root, workspace_id, root.path() / "fake.json"));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    const pid_t daemon_pid = harness.pid();
    ASSERT_GT(daemon_pid, 0);
    ASSERT_EQ(::kill(daemon_pid, SIGSTOP), 0);

    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    EXPECT_EQ(registry->probeLiveness(WorkspaceId{workspace_id}), HostLiveness::Live);
    EXPECT_FALSE(registry->reapHost(WorkspaceId{workspace_id})) << "a held flock must not be reaped";

    ForkExecLauncher launcher(binary_);
    HostLifecycle    lifecycle(launcher, *registry);
    bool             refused = false;
    try {
        AttachResult attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                       protocol::ClientRole::Supervisor});
        static_cast<void>(attach);
    } catch (const std::exception&) {
        refused = true;
    }
    EXPECT_TRUE(refused) << "a stopped daemon cannot complete a handshake";

    EXPECT_EQ(::kill(daemon_pid, 0), 0) << "the stopped daemon must not be killed";
    EXPECT_EQ(count_hosts(workspace_id), 1u) << "no second daemon may be spawned";

    ASSERT_EQ(::kill(daemon_pid, SIGCONT), 0);
    const ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

TEST_F(TwoProcess, AttachIdentityCrossCheck) {
    ShortTempRoot root("ymh-2p-ident");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "ws").value;

    HostHarness harness(options_for(root, workspace_id, root.path() / "fake.json"));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    const std::optional<WorkspaceRecord> record = read_record(registry_config, root.path());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->host.has_value());

    EnvelopeCollector collector;
    auto              sink_for = [&collector]() {
        ui::SupervisorSink sink;
        sink.on_envelope = [&collector](const protocol::SessionEnvelope& envelope) {
            collector.push(envelope);
        };
        return sink;
    };

    ui::SupervisorConnectionConfig wrong_boot;
    wrong_boot.socket_path       = harness.socket_path().string();
    wrong_boot.workspace         = ui::WorkspaceId{workspace_id};
    wrong_boot.expected_boot_id  = "00000000-0000-4000-8000-000000000000";
    wrong_boot.client_instance   = protocol::ClientInstanceId{generate_uuid_v4()};
    wrong_boot.poll_interval     = 10ms;
    wrong_boot.reconnect_backoff = 50ms;
    ui::SupervisorConnection refused_boot(wrong_boot, sink_for());
    refused_boot.start();
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(refused_boot.attachCount(), 0u);
    EXPECT_EQ(refused_boot.state(), ui::SupervisorLinkState::Dead);
    refused_boot.stop();

    ui::SupervisorConnectionConfig wrong_workspace;
    wrong_workspace.socket_path       = harness.socket_path().string();
    wrong_workspace.workspace         = ui::WorkspaceId{generate_uuid_v4()};
    wrong_workspace.expected_boot_id  = record->host->bootId.value;
    wrong_workspace.client_instance   = protocol::ClientInstanceId{generate_uuid_v4()};
    wrong_workspace.poll_interval     = 10ms;
    wrong_workspace.reconnect_backoff = 50ms;
    ui::SupervisorConnection refused_workspace(wrong_workspace, sink_for());
    refused_workspace.start();
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(refused_workspace.attachCount(), 0u);
    EXPECT_EQ(refused_workspace.state(), ui::SupervisorLinkState::Dead);
    refused_workspace.stop();

    ui::SupervisorConnectionConfig correct;
    correct.socket_path       = harness.socket_path().string();
    correct.workspace         = ui::WorkspaceId{workspace_id};
    correct.expected_boot_id  = record->host->bootId.value;
    correct.client_instance   = protocol::ClientInstanceId{generate_uuid_v4()};
    correct.poll_interval     = 10ms;
    correct.reconnect_backoff = 50ms;
    ui::SupervisorConnection attached(correct, sink_for());
    attached.start();
    ASSERT_TRUE(attached.waitForState(ui::SupervisorLinkState::Attached, 10s));
    EXPECT_EQ(attached.attachCount(), 1u);
    attached.stop();

    const ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

TEST_F(TwoProcess, StaleSocketReplace) {
    ShortTempRoot root("ymh-2p-stale");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);
    const std::filesystem::path script = root.path() / "fake.json";

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "ws").value;

    HostHarness harness(options_for(root, workspace_id, script));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    const HostPid old_pid = static_cast<HostPid>(harness.pid());
    std::string   old_boot;
    {
        const std::optional<WorkspaceRecord> record = read_record(registry_config, root.path());
        ASSERT_TRUE(record.has_value());
        ASSERT_TRUE(record->host.has_value());
        old_boot = record->host->bootId.value;
    }

    const ExitStatus crash = harness.crash(SIGKILL);
    EXPECT_TRUE(crash.signalled);
    ASSERT_TRUE(std::filesystem::exists(harness.socket_path())) << "stale socket expected";
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        EXPECT_EQ(registry->probeLiveness(WorkspaceId{workspace_id}), HostLiveness::Stale);
    }

    ForkExecLauncher                   launcher(binary_);
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    HostLifecycle                      lifecycle(launcher, *registry);
    AttachResult                       attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                       protocol::ClientRole::Supervisor});
    ASSERT_NE(attach.connection, nullptr);
    const protocol::HelloResult hello = attach.connection->hello();
    attach.connection->close();

    EXPECT_NE(hello.pid, old_pid) << "the stale socket must have been replaced";
    EXPECT_NE(hello.boot_id.value, old_boot);

    const std::optional<WorkspaceRecord> refreshed = read_record(registry_config, root.path());
    ASSERT_TRUE(refreshed.has_value());
    ASSERT_TRUE(refreshed->host.has_value());
    EXPECT_EQ(refreshed->host->pid, hello.pid);
    EXPECT_EQ(refreshed->host->bootId.value, hello.boot_id.value);
    EXPECT_EQ(count_hosts(workspace_id), 1u);
}

TEST_F(TwoProcess, MultiSupervisorFanOutPermissionFirstWins) {
    ShortTempRoot root("ymh-2p-fan");
    configure_workspace(root);
    root.write("fake.json", kFakeToolScript);  // no config: `shell` stays "ask"
    const std::filesystem::path script = root.path() / "fake.json";

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "ws").value;

    HostHarness harness(options_for(root, workspace_id, script));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    const std::optional<WorkspaceRecord> record = read_record(registry_config, root.path());
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->host.has_value());
    const std::string boot_id = record->host->bootId.value;

    EnvelopeCollector                        first_events;
    EnvelopeCollector                        second_events;
    std::mutex                               permission_mutex;
    std::vector<protocol::PermissionRequest> first_permissions;
    std::vector<protocol::PermissionRequest> second_permissions;

    auto make_sink = [](EnvelopeCollector& events, std::mutex& mutex,
                        std::vector<protocol::PermissionRequest>& permissions) {
        ui::SupervisorSink sink;
        sink.on_envelope = [&events](const protocol::SessionEnvelope& envelope) {
            events.push(envelope);
        };
        sink.on_permission = [&mutex, &permissions](const protocol::PermissionRequest& request) {
            std::lock_guard lock(mutex);
            permissions.push_back(request);
        };
        return sink;
    };

    auto config_for = [&](const std::string& instance) {
        ui::SupervisorConnectionConfig config;
        config.socket_path       = harness.socket_path().string();
        config.workspace         = ui::WorkspaceId{workspace_id};
        config.expected_boot_id  = boot_id;
        config.client_instance   = protocol::ClientInstanceId{instance};
        config.poll_interval     = 10ms;
        config.reconnect_backoff = 50ms;
        return config;
    };

    ui::SupervisorConnection first(
        config_for("11111111-1111-4111-8111-111111111111"),
        make_sink(first_events, permission_mutex, first_permissions));
    ui::SupervisorConnection second(
        config_for("22222222-2222-4222-8222-222222222222"),
        make_sink(second_events, permission_mutex, second_permissions));
    first.start();
    second.start();
    ASSERT_TRUE(first.waitForState(ui::SupervisorLinkState::Attached, 10s));
    ASSERT_TRUE(second.waitForState(ui::SupervisorLinkState::Attached, 10s));

    const ui::SupervisorReply created =
        submit_sync(first, std::string(protocol::method::kSessionCreate),
                    nlohmann::json{{"title", "fan"}});
    ASSERT_TRUE(created.ok) << created.error;
    const std::string session = created.result.value("session", std::string{});
    ASSERT_FALSE(session.empty());

    first.track(SessionId{session});
    second.track(SessionId{session});
    ASSERT_TRUE(first.waitUntil(
        [&first, &session] { return first.subscribed(SessionId{session}); }, 10s));
    ASSERT_TRUE(second.waitUntil(
        [&second, &session] { return second.subscribed(SessionId{session}); }, 10s));

    const ui::SupervisorReply prompted =
        submit_sync(first, std::string(protocol::method::kAgentPrompt),
                    nlohmann::json{{"session", session}, {"message", "run the tool"}});
    ASSERT_TRUE(prompted.ok) << prompted.error;

    const auto permission_deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < permission_deadline) {
        bool both_seen = false;
        {
            std::lock_guard lock(permission_mutex);
            both_seen = !first_permissions.empty() && !second_permissions.empty();
        }
        if (both_seen) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }

    protocol::PermissionRequest request;
    {
        std::lock_guard lock(permission_mutex);
        ASSERT_FALSE(first_permissions.empty()) << "first supervisor saw no permission.request";
        ASSERT_FALSE(second_permissions.empty()) << "second supervisor saw no permission.request";
        request = first_permissions.front();
        EXPECT_EQ(request.session.value, session);
    }

    const ui::SupervisorReply allowed =
        submit_sync(first, std::string(protocol::method::kPermissionDecide),
                    nlohmann::json{{"request_id", request.request_id},
                                   {"decision", "allow"},
                                   {"scope", "once"}});
    EXPECT_TRUE(allowed.ok) << allowed.error;

    const ui::SupervisorReply denied =
        submit_sync(second, std::string(protocol::method::kPermissionDecide),
                    nlohmann::json{{"request_id", request.request_id},
                                   {"decision", "deny"},
                                   {"scope", "once"}});
    EXPECT_FALSE(denied.ok) << "a duplicate decision must not re-resolve";
    EXPECT_EQ(denied.error_code, static_cast<int>(protocol::RpcCode::InvalidParams));

    // Both supervisors observe the same ordered stream, but each has its own
    // pump thread and socket, so one can trail the other by a few milliseconds.
    // `TurnEnded` is the final event of the turn (agent_loop.cpp), so waiting
    // for *both* collectors to see it synchronises the fan-out before the
    // snapshots below: comparing as soon as `first` alone is caught up is racy.
    const auto saw_turn_ended =
        [](const std::vector<protocol::SessionEnvelope>& envelopes) {
            return std::any_of(envelopes.begin(), envelopes.end(), [](const auto& envelope) {
                return envelope.event.type == EventType::TurnEnded;
            });
        };
    ASSERT_TRUE(first_events.wait_for(saw_turn_ended, 15s))
        << "the allowed turn never completed";
    ASSERT_TRUE(second_events.wait_for(saw_turn_ended, 15s))
        << "the second supervisor never observed the turn end";

    const std::set<std::string> first_ids  = event_ids(first_events.snapshot());
    const std::set<std::string> second_ids = event_ids(second_events.snapshot());
    EXPECT_TRUE(
        std::includes(first_ids.begin(), first_ids.end(), second_ids.begin(), second_ids.end()))
        << "second supervisor missed events the first saw";
    EXPECT_TRUE(
        std::includes(second_ids.begin(), second_ids.end(), first_ids.begin(), first_ids.end()))
        << "first supervisor missed events the second saw";

    auto slow = open_connection(harness.socket_path());
    [[maybe_unused]] const nlohmann::json subscribed = slow->request(
        std::string(protocol::method::kEventSubscribe),
        nlohmann::json{{"session", session}, {"from", {{"kind", "beginning"}}}});
    ASSERT_TRUE(harness.running());
    const ui::SupervisorReply still_alive =
        submit_sync(first, std::string(protocol::method::kHostPing), nlohmann::json::object());
    EXPECT_TRUE(still_alive.ok);
    slow->close();

    first.stop();
    second.stop();
    const ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

TEST_F(TwoProcess, CursorInvalidResubscribesFromBeginning) {
    ShortTempRoot root("ymh-2p-cursor");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const std::string workspace_id = generate_uuid_v4();

    HostHarness harness(options_for(root, workspace_id, root.path() / "fake.json"));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    std::unique_ptr<protocol::HostConnection> connection = open_connection(harness.socket_path());

    const nlohmann::json created =
        connection->request(std::string(protocol::method::kSessionCreate),
                            nlohmann::json{{"title", "cursor"}});
    const std::string session = created.value("session", std::string{});
    ASSERT_FALSE(session.empty());

    static_cast<void>(connection->request(
        std::string(protocol::method::kAgentPrompt),
        nlohmann::json{{"session", session}, {"message", "hello"}}));

    std::vector<protocol::SessionEnvelope> authoritative;
    const auto                             deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        authoritative = replay_session(*connection, session);
        const bool ended =
            std::any_of(authoritative.begin(), authoritative.end(), [](const auto& envelope) {
                return envelope.event.type == EventType::TurnEnded;
            });
        if (ended) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_FALSE(authoritative.empty());

    bool cursor_invalid = false;
    try {
        static_cast<void>(connection->request(
            std::string(protocol::method::kEventSubscribe),
            nlohmann::json{{"session", session},
                           {"from", {{"kind", "cursor"}, {"cursor", "not-a-valid-cursor"}}}}));
    } catch (const protocol::RpcException& error) {
        cursor_invalid = error.code() == static_cast<int>(protocol::AppCode::CursorInvalid);
    }
    EXPECT_TRUE(cursor_invalid) << "an unresolvable cursor must be CursorInvalid";

    static_cast<void>(connection->request(
        std::string(protocol::method::kEventSubscribe),
        nlohmann::json{{"session", session}, {"from", {{"kind", "beginning"}}}}));

    std::vector<protocol::SessionEnvelope> replayed;
    const auto                             replay_deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < replay_deadline) {
        const std::optional<protocol::Notification> notification =
            connection->nextNotification(200ms);
        if (!notification.has_value()) {
            if (!replayed.empty()) {
                break;
            }
            continue;
        }
        if (notification->method == protocol::notify::kEventStream) {
            replayed.push_back(notification->params.get<protocol::StreamNotification>().envelope);
        }
    }
    EXPECT_EQ(event_ids(replayed), event_ids(authoritative));

    ui::UiModel        model;
    ui::UiEventAdapter adapter(model);
    for (const protocol::SessionEnvelope& envelope : replayed) {
        adapter.onSessionEnvelope(ui::WorkspaceId{workspace_id}, envelope);
    }
    const ui::SessionUiState* state = model.session(SessionId{session});
    ASSERT_NE(state, nullptr);
    const std::size_t rendered_once = state->conversation.entries.size();
    EXPECT_GT(rendered_once, 0u);

    for (const protocol::SessionEnvelope& envelope : replayed) {
        adapter.onSessionEnvelope(ui::WorkspaceId{workspace_id}, envelope);
    }
    state = model.session(SessionId{session});
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->conversation.entries.size(), rendered_once)
        << "a replayed overlap must not duplicate render";

    connection->close();
    const ExitStatus status = harness.stop();
    EXPECT_TRUE(status.exited);
}

TEST_F(TwoProcess, NoOrphanedDaemonsAfterSuite) {
    ShortTempRoot root("ymh-2p-orphan");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const std::string workspace_id = generate_uuid_v4();
    {
        HostHarness harness(options_for(root, workspace_id, root.path() / "fake.json"));
        harness.start();
        ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
        ASSERT_EQ(count_hosts(workspace_id), 1u);
        const ExitStatus status = harness.stop();
        EXPECT_TRUE(status.exited);
    }
    for (int attempt = 0; attempt < 100 && count_hosts(workspace_id) != 0; ++attempt) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(count_hosts(workspace_id), 0u);

    const std::vector<pid_t> orphans = host_processes(nullptr);
    EXPECT_TRUE(orphans.empty()) << "orphaned ymh --host daemons: " << orphans.size();
}

TEST_F(TwoProcess, PingKeepsIdleLinkAliveAndDetectsCrash) {
    ShortTempRoot root("ymh-2p-ping");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "ping").value;

    HostHarness harness(options_for(root, workspace_id, root.path() / "fake.json"));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    ui::SupervisorSink sink;
    ui::SupervisorConnectionConfig config;
    config.socket_path       = harness.socket_path().string();
    config.workspace         = ui::WorkspaceId{workspace_id};
    config.client_instance   = protocol::ClientInstanceId{generate_uuid_v4()};
    config.poll_interval     = 10ms;
    config.reconnect_backoff = 50ms;
    config.ping_interval     = 100ms;
    config.request_timeout   = 2s;
    ui::SupervisorConnection connection(config, std::move(sink));
    connection.start();
    ASSERT_TRUE(connection.waitForState(ui::SupervisorLinkState::Attached, 15s));
    const std::uint64_t first_attach = connection.attachCount();

    std::this_thread::sleep_for(600ms);
    EXPECT_TRUE(connection.attached());
    EXPECT_EQ(connection.attachCount(), first_attach)
        << "an idle link must stay attached across several ping intervals";

    const ExitStatus crash = harness.crash(SIGKILL);
    EXPECT_TRUE(crash.signalled);
    const auto killed_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(connection.waitUntil(
        [&connection] { return connection.state() == ui::SupervisorLinkState::Dead; }, 3s))
        << "the supervisor never noticed the daemon death";
    EXPECT_LT(std::chrono::steady_clock::now() - killed_at, 2s);

    ForkExecLauncher                   launcher(binary_);
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    HostLifecycle                      lifecycle(launcher, *registry);
    AttachResult                       attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{protocol::ClientInstanceId{generate_uuid_v4()},
                       protocol::ClientRole::Supervisor});
    ASSERT_NE(attach.connection, nullptr);
    attach.connection->close();

    ASSERT_TRUE(connection.waitUntil(
        [&connection, first_attach] { return connection.attachCount() > first_attach; }, 20s))
        << "the supervisor never reattached to the respawned daemon";
    EXPECT_TRUE(connection.attached());
    connection.stop();
}

TEST_F(TwoProcess, EnsureRunningCarriesAttachIdentity) {
    ShortTempRoot root("ymh-2p-identity");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);
    const std::filesystem::path script = root.path() / "fake.json";

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "identity").value;

    HostHarness harness(options_for(root, workspace_id, script));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    ForkExecLauncher                   launcher(binary_);
    std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
    HostLifecycle                      lifecycle(launcher, *registry);

    const protocol::ClientInstanceId supervisor_id{"99999999-9999-4999-8999-999999999999"};
    AttachResult supervisor_attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{supervisor_id, protocol::ClientRole::Supervisor});
    ASSERT_NE(supervisor_attach.connection, nullptr);
    const protocol::OwnershipView supervisor_view =
        supervisor_attach.connection
            ->request(std::string(protocol::method::kHostOwnership))
            .get<protocol::OwnershipView>();
    bool supervisor_seen = false;
    for (const protocol::OwnershipView::ClientInfo& client : supervisor_view.clients) {
        if (client.client_instance == supervisor_id.value) {
            supervisor_seen = true;
            EXPECT_EQ(client.role, protocol::ClientRole::Supervisor);
        }
    }
    EXPECT_TRUE(supervisor_seen) << "the registered row id must match the connection instance";
    supervisor_attach.connection->close();

    const protocol::ClientInstanceId automation_id{"88888888-8888-4888-8888-888888888888"};
    AttachResult automation_attach = lifecycle.ensureRunning(
        WorkspaceId{workspace_id},
        AttachIdentity{automation_id, protocol::ClientRole::Automation});
    ASSERT_NE(automation_attach.connection, nullptr);
    const protocol::OwnershipView automation_view =
        automation_attach.connection
            ->request(std::string(protocol::method::kHostOwnership))
            .get<protocol::OwnershipView>();
    bool automation_seen = false;
    for (const protocol::OwnershipView::ClientInfo& client : automation_view.clients) {
        if (client.client_instance == automation_id.value) {
            automation_seen = true;
            EXPECT_EQ(client.role, protocol::ClientRole::Automation);
        }
    }
    EXPECT_TRUE(automation_seen) << "ymh run must attach with ClientRole::Automation";
    automation_attach.connection->close();
}

TEST_F(TwoProcess, WorkspaceStopRequiresConfirmationThenForceTearsDown) {
    ShortTempRoot root("ymh-2p-stop");
    configure_workspace(root);
    root.write(".ymh/config.jsonc", kAllowAllConfig);
    root.write("fake.json", kFakeTextScript);
    const std::filesystem::path script = root.path() / "fake.json";

    const RegistryConfig registry_config = registry_config_for(root.state_dir());
    const std::string    workspace_id =
        register_workspace(registry_config, root.path(), "stop").value;

    HostHarness harness(options_for(root, workspace_id, script));
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();
    DaemonGuard guard(workspace_id);

    std::unique_ptr<protocol::HostConnection> supervisor = open_connection(harness.socket_path());
    const protocol::OwnershipView owned =
        supervisor->request(std::string(protocol::method::kHostOwnership))
            .get<protocol::OwnershipView>();
    ASSERT_GE(owned.live_supervisors, 1u);

    {
        ScopedStdinDevNull null_stdin;
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(run_cli({"workspace", "stop", workspace_id}, out, err), 1) << err.str();
    }

    const nlohmann::json pong = supervisor->request(std::string(protocol::method::kHostPing));
    EXPECT_TRUE(pong.contains("server_time_ms"));

    {
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(run_cli({"workspace", "stop", workspace_id, "--force"}, out, err), 0) << err.str();
    }

    bool saw_shutdown = false;
    const auto notice_deadline = std::chrono::steady_clock::now() + 5s;
    while (!saw_shutdown && std::chrono::steady_clock::now() < notice_deadline) {
        const std::optional<protocol::Notification> notice = supervisor->nextNotification(500ms);
        if (!notice.has_value()) {
            if (!supervisor->isConnected()) {
                break;
            }
            continue;
        }
        if (notice->method == protocol::notify::kHostEvent &&
            notice->params.value("kind", std::string{}) == "daemon_shutting_down") {
            saw_shutdown = true;
        }
    }
    EXPECT_TRUE(saw_shutdown) << "the owning supervisor must observe DaemonShuttingDown";

    const ExitStatus status = harness.wait_for_exit(15s);
    EXPECT_TRUE(status.exited || status.signalled);
    supervisor->close();
}

} // namespace

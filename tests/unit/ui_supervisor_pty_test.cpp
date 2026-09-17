#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "support/host_harness.hpp"
#include "support/short_temp.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/ownership.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace std::chrono_literals;

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

// `/proc/<pid>/cmdline` is a NUL-separated argv blob.
std::vector<std::string> proc_args(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!input) {
        return {};
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
    return args;
}

// Teardown confirms daemons from `/proc/<pid>/cmdline` (`ymh --host ...
// --workspace <id>`) because a registry read can lag the daemon's claim/exit.
bool is_host_process(pid_t pid, const std::string* workspace_id) {
    const std::vector<std::string> args = proc_args(pid);
    bool                           host = false;
    bool workspace_match = workspace_id == nullptr;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--host") {
            host = true;
        }
        if (workspace_id != nullptr && args[index] == "--workspace" &&
            index + 1 < args.size() && args[index + 1] == *workspace_id) {
            workspace_match = true;
        }
    }
    return host && workspace_match;
}

std::vector<pid_t> host_processes(const std::string* workspace_id = nullptr) {
    std::vector<pid_t> pids;
    std::error_code error;
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

class HostDaemonGuard {
public:
    explicit HostDaemonGuard(std::string workspace_id) : workspace_id_(std::move(workspace_id)) {}
    HostDaemonGuard(const HostDaemonGuard&) = delete;
    HostDaemonGuard& operator=(const HostDaemonGuard&) = delete;
    ~HostDaemonGuard() { stop(); }

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

class PtyChild {
public:
    ~PtyChild() { terminate(); }

    bool spawn(const std::filesystem::path& binary, const std::filesystem::path& cwd,
               const std::map<std::string, std::string>& env,
               const std::vector<std::string>& args = {}) {
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
            std::vector<std::string> argv_storage;
            argv_storage.push_back(binary.string());
            argv_storage.insert(argv_storage.end(), args.begin(), args.end());
            std::vector<char*> argv;
            argv.reserve(argv_storage.size() + 1);
            for (std::string& argument : argv_storage) {
                argv.push_back(argument.data());
            }
            argv.push_back(nullptr);
            ::execv(binary.c_str(), argv.data());
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

    [[nodiscard]] std::string text() const { return strip_ansi(buffer_); }

    [[nodiscard]] std::size_t raw_size() const { return buffer_.size(); }

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

    bool wait_for_since(std::size_t offset, const std::string& needle,
                        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            read_available();
            if (text_since(offset).find(needle) != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        read_available();
        return text_since(offset).find(needle) != std::string::npos;
    }

    bool wait_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
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
    [[nodiscard]] std::string text_since(std::size_t offset) const {
        if (offset > buffer_.size()) {
            offset = buffer_.size();
        }
        return strip_ansi(buffer_.substr(offset));
    }

    void read_available() {
        if (master_ < 0) {
            return;
        }
        pollfd descriptor{};
        descriptor.fd = master_;
        descriptor.events = POLLIN;
        while (::poll(&descriptor, 1, 0) > 0) {
            char chunk[4096];
            const ssize_t count = ::read(master_, chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            buffer_.append(chunk, static_cast<std::size_t>(count));
        }
    }

    int master_{-1};
    pid_t pid_{-1};
    std::string buffer_;
};

TEST(UiSupervisorPty, AttachesSpawnsAndSwitches) {
    ShortTempRoot root("ymh_pty");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace_a = root.path() / "alpha";
    const std::filesystem::path workspace_b = root.path() / "beta";
    std::filesystem::create_directories(workspace_a);
    std::filesystem::create_directories(workspace_b);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId alpha_id;
    std::string beta_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(registry_config);
        alpha_id = registry->registerWorkspace(workspace_a, "alpha").id;
        beta_id = registry->registerWorkspace(workspace_b, "beta").id.value;
    }

    HostHarnessOptions beta_options;
    beta_options.binary = resolve_ymh_binary();
    beta_options.workspace_root = workspace_b;
    beta_options.workspace_id = beta_id;
    beta_options.socket_path = workspace_b / ".ymh" / "host.sock";
    beta_options.log_sink = workspace_b / ".ymh" / "host.log";
    beta_options.env["XDG_STATE_HOME"] = state.string();
    beta_options.env["HOME"] = root.path().string();
    HostHarness beta(std::move(beta_options));
    beta.start();
    ASSERT_TRUE(beta.wait_ready(20s)) << beta.read_log();

    HostDaemonGuard alpha_guard(alpha_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace_a, env));

    ASSERT_TRUE(child.wait_for("alpha", 25s));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 10s))
        << "supervisor did not auto-create a session";
    ASSERT_TRUE(child.wait_for("tui", 10s)) << "header did not show the session title";
    ASSERT_TRUE(child.wait_for("active ·", 10s))
        << "bottom line did not show aggregate counts";

    child.write("\x13");
    ASSERT_TRUE(child.wait_for("beta", 10s));

    child.write("j");
    std::this_thread::sleep_for(300ms);
    child.write("\r");
    ASSERT_TRUE(child.wait_for(workspace_b.string(), 10s)) << child.text();

    child.terminate();
    alpha_guard.stop();
    beta.stop();

    EXPECT_FALSE(beta.running());
    EXPECT_TRUE(host_processes(&alpha_id.value).empty()) << "alpha daemon leaked";
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

TEST(UiSupervisorPty, HelpListAndHistoryRecall) {
    ShortTempRoot root("ymh_pty_help");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "help-ws";
    std::filesystem::create_directories(workspace);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "help-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    child.write("/help\r");
    ASSERT_TRUE(child.wait_for("list slash commands", 10s)) << child.text();

    const std::size_t clear_mark = child.raw_size();
    child.write("/clear\r");
    ASSERT_TRUE(child.wait_for_since(clear_mark, "Type a message and press Enter", 10s))
        << child.text();

    const std::size_t complete_mark = child.raw_size();
    child.write("/he\t");
    ASSERT_TRUE(child.wait_for_since(complete_mark, "/help ", 10s)) << child.text();

    child.write("\x15");
    const std::size_t cycle_mark = child.raw_size();
    child.write("/\t\t");
    ASSERT_TRUE(child.wait_for_since(cycle_mark, "/new", 10s)) << child.text();
    const std::size_t step_mark = child.raw_size();
    child.write("\t");
    ASSERT_TRUE(child.wait_for_since(step_mark, "/clear", 10s)) << child.text();
    child.write("\x15");

    const std::size_t recall_mark = child.raw_size();
    child.write("\x1b[A");
    ASSERT_TRUE(child.wait_for_since(recall_mark, "/clear", 10s)) << child.text();

    child.terminate();
    guard.stop();

    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

TEST(UiSupervisorPty, ExitPromptCancelKeepsDaemonThenConfirmTearsDown) {
    ShortTempRoot root("ymh_pty_exit");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "exit-ws";
    std::filesystem::create_directories(workspace);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "exit-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();
    ASSERT_TRUE(child.wait_for("active ·", 10s)) << child.text();
    ASSERT_FALSE(host_processes(&workspace_id.value).empty())
        << "supervisor did not spawn its daemon";

    const std::size_t prompt_mark = child.raw_size();
    child.write("\x04");
    ASSERT_TRUE(child.wait_for_since(prompt_mark, "Terminate and exit", 15s)) << child.text();
    ASSERT_TRUE(child.wait_for_since(prompt_mark, "1 workspace daemon", 5s)) << child.text();

    const std::size_t cancel_mark = child.raw_size();
    child.write("n");
    ASSERT_TRUE(child.wait_for_since(cancel_mark, "Type a message and press Enter", 10s))
        << child.text();
    EXPECT_FALSE(host_processes(&workspace_id.value).empty())
        << "cancel must leave the daemon running";
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config);
        EXPECT_EQ(registry->listSupervisors().size(), 1u)
            << "cancel must not deregister the supervisor";
    }

    const std::size_t confirm_mark = child.raw_size();
    child.write("\x04");
    ASSERT_TRUE(child.wait_for_since(confirm_mark, "Terminate and exit", 15s)) << child.text();
    child.write("y");
    EXPECT_TRUE(child.wait_exit(25s)) << child.text();

    for (int attempt = 0; attempt < 400; ++attempt) {
        if (host_processes(&workspace_id.value).empty()) {
            break;
        }
        std::this_thread::sleep_for(25ms);
    }
    EXPECT_TRUE(host_processes(&workspace_id.value).empty())
        << "confirm must tear the daemon down";
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config);
        EXPECT_TRUE(registry->listSupervisors().empty())
            << "confirm must deregister the supervisor";
    }

    guard.stop();
}

// 16 §4.1/O10 regression. The daemon serves `other_fresh_owners` from its
// watchdog-cached snapshot, which lags the registry by up to one
// `kOwnerWatchdogInterval`. A peer that has just exited cleanly is still listed
// in that cache; the last supervisor must not mistake the ghost for a live owner
// and exit without the §4.2 prompt or the §4.3 teardown. The existing tests never
// exercise a stale owner snapshot, so the silent exit went unnoticed.
TEST(UiSupervisorPty, LastExitPromptsWhenDaemonOwnerSnapshotLagsRegistry) {
    ShortTempRoot root("ymh_pty_ghost_owner");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "ghost-ws";
    std::filesystem::create_directories(workspace);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "ghost-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();
    ASSERT_TRUE(child.wait_for("active ·", 10s)) << child.text();
    ASSERT_FALSE(host_processes(&workspace_id.value).empty())
        << "supervisor did not spawn its daemon";

    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    const SupervisorId ghost{"ghost-peer-0000-0000"};

    // A fresh peer row lives long enough for the daemon's watchdog to publish it
    // into its owner snapshot.
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        SupervisorRow row;
        row.id = ghost;
        row.pid = ::getpid();
        row.bootId = "ghost-boot";
        row.startedAtMs = now_ms();
        row.heartbeatMs = now_ms();
        row.tty = "/dev/pts/0";
        registry->registerSupervisor(row);
    }
    std::this_thread::sleep_for(kOwnerWatchdogInterval + 700ms);

    // The peer cleanly exits (deregisters), but the daemon's cached snapshot still
    // lists it. This is the last supervisor, so exiting MUST prompt.
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        EXPECT_TRUE(registry->deregisterSupervisor(ghost));
    }

    const std::size_t prompt_mark = child.raw_size();
    child.write("\x04");
    ASSERT_TRUE(child.wait_for_since(prompt_mark, "Terminate and exit", 15s)) << child.text();
    ASSERT_TRUE(child.wait_for_since(prompt_mark, "1 workspace daemon", 5s)) << child.text();

    // Cancel keeps both the daemon and this supervisor's row (§4.2).
    const std::size_t cancel_mark = child.raw_size();
    child.write("n");
    ASSERT_TRUE(child.wait_for_since(cancel_mark, "Type a message and press Enter", 10s))
        << child.text();
    EXPECT_FALSE(host_processes(&workspace_id.value).empty())
        << "cancel must leave the daemon running";
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config);
        EXPECT_EQ(registry->listSupervisors().size(), 1u)
            << "cancel must not deregister the supervisor";
    }

    // Confirm tears the daemon down and exits (§4.3).
    const std::size_t confirm_mark = child.raw_size();
    child.write("\x04");
    ASSERT_TRUE(child.wait_for_since(confirm_mark, "Terminate and exit", 15s)) << child.text();
    child.write("y");
    EXPECT_TRUE(child.wait_exit(25s)) << child.text();

    for (int attempt = 0; attempt < 400; ++attempt) {
        if (host_processes(&workspace_id.value).empty()) {
            break;
        }
        std::this_thread::sleep_for(25ms);
    }
    EXPECT_TRUE(host_processes(&workspace_id.value).empty())
        << "confirm must tear the daemon down";
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::openReadOnly(registry_config);
        EXPECT_TRUE(registry->listSupervisors().empty())
            << "confirm must deregister the supervisor";
    }

    guard.stop();
}

void write_skill_file(const std::filesystem::path& root, const std::string& name,
                      const std::string& description) {
    const std::filesystem::path directory = root / name;
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "SKILL.md")
        << "---\nname: " << name << "\ndescription: " << description << "\n---\nBody.\n";
}

TEST(UiSupervisorPty, SkillsTabCompletionAndListing) {
    ShortTempRoot root("ymh_pty_skills");
    const std::filesystem::path state = root.state_dir();
    const std::filesystem::path config = root.path() / "config";
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    write_skill_file(config / "ymh" / "skills", "git-commit", "Write a conventional commit.");
    const std::filesystem::path workspace = root.path() / "skills-ws";
    std::filesystem::create_directories(workspace);
    write_skill_file(workspace / ".ymh" / "skills", "repo-conventions", "Repo conventions.");
    const std::filesystem::path broken = workspace / ".ymh" / "skills" / "broken";
    std::filesystem::create_directories(broken);
    std::ofstream(broken / "SKILL.md") << "name: broken\ndescription: no fence\n";

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "skills-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["XDG_CONFIG_HOME"] = config.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    const std::size_t completion_mark = child.raw_size();
    child.write("/sk\t");
    ASSERT_TRUE(child.wait_for_since(completion_mark, "/skill", 10s)) << child.text();
    child.write("\x15");

    const std::size_t listing_mark = child.raw_size();
    child.write("/skills\r");
    ASSERT_TRUE(child.wait_for_since(listing_mark, "git-commit", 15s)) << child.text();
    EXPECT_NE(child.text().find("[user]"), std::string::npos);
    EXPECT_NE(child.text().find("repo-conventions"), std::string::npos);
    EXPECT_NE(child.text().find("[workspace]"), std::string::npos);
    EXPECT_NE(child.text().find("skills skipped"), std::string::npos);

    child.terminate();
    guard.stop();

    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

TEST(UiSupervisorPty, SkillsEmptyState) {
    ShortTempRoot root("ymh_pty_skills_empty");
    const std::filesystem::path state = root.state_dir();
    const std::filesystem::path config = root.path() / "config";
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    std::filesystem::create_directories(config / "ymh" / "skills");
    const std::filesystem::path workspace = root.path() / "empty-ws";
    std::filesystem::create_directories(workspace / ".ymh" / "skills");

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "empty-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["XDG_CONFIG_HOME"] = config.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    const std::size_t mark = child.raw_size();
    child.write("/skills\r");
    ASSERT_TRUE(child.wait_for_since(mark, "no skills found.", 15s)) << child.text();
    EXPECT_NE(child.text().find("a skill file starts with"), std::string::npos);

    child.terminate();
    guard.stop();

    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

void write_compaction_window(const std::filesystem::path& workspace, std::int64_t window) {
    std::filesystem::create_directories(workspace / ".ymh");
    std::ofstream(workspace / ".ymh" / "config.jsonc")
        << "{\n  \"agent\": { \"compaction\": { \"context_window_tokens\": " << window
        << " } }\n}\n";
}

TEST(UiSupervisorPty, ContextOverlayOpensAndCloses) {
    ShortTempRoot root("ymh_pty_context");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "context-ws";
    std::filesystem::create_directories(workspace);
    write_compaction_window(workspace, 64000);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "context-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    const std::size_t open_mark = child.raw_size();
    child.write("/context\r");
    ASSERT_TRUE(child.wait_for_since(open_mark, "system prompt", 15s)) << child.text();
    EXPECT_NE(child.text().find("window"), std::string::npos);
    EXPECT_NE(child.text().find("@"), std::string::npos);

    const std::size_t close_mark = child.raw_size();
    child.write("\x1b");
    ASSERT_TRUE(child.wait_for_since(close_mark, "Type a message and press Enter", 10s))
        << child.text();

    child.terminate();
    guard.stop();

    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

TEST(UiSupervisorPty, ContextOverlayShowsNote) {
    ShortTempRoot root("ymh_pty_context_note");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "context-note-ws";
    std::filesystem::create_directories(workspace);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "context-note-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    const std::size_t mark = child.raw_size();
    child.write("/context\r");
    ASSERT_TRUE(child.wait_for_since(mark, "budget unknown", 15s)) << child.text();
    EXPECT_NE(child.text().find("used tokens (budget unknown)  @"), std::string::npos);

    child.terminate();
    guard.stop();

    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

TEST(UiSupervisorPty, ContextRefreshKeyKeepsOverlay) {
    ShortTempRoot root("ymh_pty_context_refresh");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "context-refresh-ws";
    std::filesystem::create_directories(workspace);
    write_compaction_window(workspace, 64000);

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "context-refresh-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    const std::size_t open_mark = child.raw_size();
    child.write("/context\r");
    ASSERT_TRUE(child.wait_for_since(open_mark, "system prompt", 15s)) << child.text();

    const std::size_t refresh_mark = child.raw_size();
    child.write("r");
    ASSERT_TRUE(child.wait_for_since(refresh_mark, "system prompt", 10s)) << child.text();
    EXPECT_NE(child.text().find("@"), std::string::npos);

    child.terminate();
    guard.stop();

    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

TEST(UiSupervisorPty, TuiWithExplicitConfigPassesItToDaemon) {
    ShortTempRoot root("ymh_pty_config");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path workspace = root.path() / "config-ws";
    std::filesystem::create_directories(workspace);
    const std::filesystem::path explicit_config = root.path() / "explicit.jsonc";
    {
        std::ofstream(explicit_config, std::ios::binary) << "{}\n";
    }
    ASSERT_FALSE(std::filesystem::exists(root.path() / ".config" / "ymh" / "config.jsonc"));

    RegistryConfig registry_config;
    registry_config.db_path = state / "ymh" / "registry.db";
    registry_config.lock_path = state / "ymh" / "registry.lock";
    registry_config.workspace_roots = {};

    WorkspaceId workspace_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);
        workspace_id = registry->registerWorkspace(workspace, "config-ws").id;
    }
    HostDaemonGuard guard(workspace_id.value);

    PtyChild child;
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"] = state.string();
    env["HOME"] = root.path().string();
    env["XDG_CONFIG_HOME"] = (root.path() / ".config").string();
    env["TERM"] = "xterm-256color";
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), workspace, env,
                            {"--config", explicit_config.string()}));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    const std::vector<pid_t> hosts = host_processes(&workspace_id.value);
    ASSERT_FALSE(hosts.empty()) << "supervisor did not spawn its daemon";
    const std::vector<std::string> daemon_args = proc_args(hosts.front());
    bool                           carried = false;
    for (std::size_t index = 0; index + 1 < daemon_args.size(); ++index) {
        if (daemon_args[index] == "--config" && daemon_args[index + 1] == explicit_config.string()) {
            carried = true;
        }
    }
    EXPECT_TRUE(carried) << "daemon argv did not carry --config " << explicit_config.string();
    EXPECT_FALSE(std::filesystem::exists(root.path() / ".config" / "ymh" / "config.jsonc"));

    child.terminate();
    guard.stop();
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

// 22 §10.4: a stored session on disk, written before any daemon exists. Returns
// the session id. `marker`, when non-empty, is a user-message event that must
// replay once the session is resumed.
SessionId write_stored_session(const std::filesystem::path& workspace,
                               const std::string& title, const std::string& marker) {
    const std::filesystem::path ymh_dir = workspace / ".ymh";
    std::filesystem::create_directories(ymh_dir);
    PersistenceConfig config;
    config.db_path   = ymh_dir / "sessions.db";
    config.lock_path = ymh_dir / "sessions.lock";
    config.boot_id   = BootId{"pty-test"};
    {
        const std::unique_ptr<SessionPersistence> store = SessionPersistence::open(config);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        SessionHeader header;
        header.id            = SessionId{generate_uuid_v4()};
        header.cwd           = std::filesystem::canonical(workspace);
        header.createdAt     = now;
        header.updatedAt     = now;
        header.title         = title;
        header.model         = "deepseek-flash";
        header.serverProfile = "interactive";
        header.kind          = SessionKind::Root;
        store->create(header);
        if (!marker.empty()) {
            payload::UserMessage message;
            message.id = MessageId{generate_uuid_v4()};
            ContentBlock block;
            block.kind = ContentBlockKind::Text;
            block.text = marker;
            message.content.push_back(std::move(block));
            TypedEvent<payload::UserMessage> typed;
            typed.id         = EventId{generate_uuid_v4()};
            typed.session_id = header.id;
            typed.timestamp  = std::chrono::system_clock::now();
            typed.payload    = std::move(message);
            store->append(header.id, encode(typed));
        }
        const SessionId session = header.id;
        store->releaseLease(session);
        // Drop the WAL sidecars after close so the read-only catalog open does
        // not depend on them.
        std::error_code error;
        std::filesystem::remove(ymh_dir / "sessions.db-wal", error);
        std::filesystem::remove(ymh_dir / "sessions.db-shm", error);
        return session;
    }
}

RegistryConfig pty_registry_config(const std::filesystem::path& state) {
    RegistryConfig config;
    config.db_path         = state / "ymh" / "registry.db";
    config.lock_path       = state / "ymh" / "registry.lock";
    config.workspace_roots = {};
    return config;
}

std::map<std::string, std::string> pty_env(const std::filesystem::path& root,
                                           const std::filesystem::path& state) {
    std::map<std::string, std::string> env;
    env["XDG_STATE_HOME"]  = state.string();
    env["XDG_CONFIG_HOME"] = (root / ".config").string();
    env["HOME"]            = root.string();
    env["TERM"]            = "xterm-256color";
    return env;
}

bool wait_for_host(const std::string& workspace_id, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!host_processes(&workspace_id).empty()) {
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }
    return !host_processes(&workspace_id).empty();
}

// SW-P4 (22 §10.4): with a stopped workspace registered, Ctrl-S (Live) does not
// show its stored session; `/sessions` (History) does.
TEST(UiSupervisorPty, SwP4_LiveSwitcherHidesStoppedWorkspaceHistoryShowsIt) {
    ShortTempRoot root("ymh_pty_p4");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path alpha = root.path() / "alpha";
    const std::filesystem::path beta  = root.path() / "beta";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(beta);

    WorkspaceId alpha_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(pty_registry_config(state));
        alpha_id = registry->registerWorkspace(alpha, "alpha").id;
        registry->registerWorkspace(beta, "beta");
    }
    write_stored_session(beta, "beta-stored", "");

    HostDaemonGuard alpha_guard(alpha_id.value);
    PtyChild        child;
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), alpha, pty_env(root.path(), state)));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    child.write("\x13");
    ASSERT_TRUE(child.wait_for("Switcher", 10s)) << child.text();
    EXPECT_EQ(child.text().find("beta-stored"), std::string::npos)
        << "Live switcher showed a stopped workspace's stored session";

    child.write("\x1b");
    std::this_thread::sleep_for(300ms);
    child.write("/sessions\r");
    ASSERT_TRUE(child.wait_for("beta-stored", 20s)) << child.text();

    child.terminate();
    alpha_guard.stop();
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

// SW-P1 (22 §10.4): selecting a stored session in a stopped workspace from
// `/sessions` spawns its daemon and resumes it (the transcript replays).
TEST(UiSupervisorPty, SwP1_SessionsSelectionSpawnsAndResumes) {
    ShortTempRoot root("ymh_pty_p1");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path alpha = root.path() / "alpha";
    const std::filesystem::path beta  = root.path() / "beta";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(beta);

    WorkspaceId alpha_id;
    std::string beta_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(pty_registry_config(state));
        alpha_id = registry->registerWorkspace(alpha, "alpha").id;
        beta_id  = registry->registerWorkspace(beta, "beta").id.value;
    }
    const std::string marker = "zzstoredmarkerzz";
    write_stored_session(beta, "beta-stored", marker);

    HostDaemonGuard alpha_guard(alpha_id.value);
    HostDaemonGuard beta_guard(beta_id);
    PtyChild        child;
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), alpha, pty_env(root.path(), state)));
    ASSERT_TRUE(child.wait_for("Type a message and press Enter", 25s)) << child.text();

    child.write("/sessions\r");
    ASSERT_TRUE(child.wait_for("beta-stored", 20s)) << child.text();

    // The History cursor starts on alpha's active session; "j" walks to beta's
    // workspace then its only session, where further presses settle.
    child.write("jjjj");
    child.write("\r");

    EXPECT_TRUE(wait_for_host(beta_id, 25s)) << "selecting the stored session did not spawn beta";
    ASSERT_TRUE(child.wait_for(marker, 25s))
        << "resumed session transcript did not replay: " << child.text();

    child.terminate();
    alpha_guard.stop();
    beta_guard.stop();
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

// SW-P2 (22 §10.4): `ymh --resume <id>` resumes the session in its own
// workspace and shows its title in the header.
TEST(UiSupervisorPty, SwP2_ResumeFlagResumesStoredSession) {
    ShortTempRoot root("ymh_pty_p2");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path alpha = root.path() / "alpha";
    const std::filesystem::path beta  = root.path() / "beta";
    std::filesystem::create_directories(alpha);
    std::filesystem::create_directories(beta);

    WorkspaceId alpha_id;
    std::string beta_id;
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(pty_registry_config(state));
        alpha_id = registry->registerWorkspace(alpha, "alpha").id;
        beta_id  = registry->registerWorkspace(beta, "beta").id.value;
    }
    const std::string marker = "zzresumeflagmarkerzz";
    const SessionId   session = write_stored_session(beta, "beta-stored", marker);

    HostDaemonGuard alpha_guard(alpha_id.value);
    HostDaemonGuard beta_guard(beta_id);
    PtyChild        child;
    ASSERT_TRUE(child.spawn(resolve_ymh_binary(), alpha, pty_env(root.path(), state),
                            {"--resume", session.value}));
    ASSERT_TRUE(child.wait_for("beta-stored", 30s))
        << "resumed session title missing from the header: " << child.text();
    EXPECT_TRUE(wait_for_host(beta_id, 20s)) << "beta daemon was not spawned for --resume";
    ASSERT_TRUE(child.wait_for(marker, 25s)) << child.text();

    child.terminate();
    alpha_guard.stop();
    beta_guard.stop();
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

// SW-F6 (22 §6.2): an unknown `--resume` id exits 1 without starting the TUI.
TEST(UiSupervisorPty, SwP2_UnknownResumeIdExitsOne) {
    ShortTempRoot root("ymh_pty_p2bad");
    const std::filesystem::path state = root.state_dir();
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);
    ::setenv("HOME", root.path().c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root.path() / ".config").string().c_str(), 1);

    const std::filesystem::path alpha = root.path() / "alpha";
    std::filesystem::create_directories(alpha);
    {
        std::unique_ptr<WorkspaceRegistry> registry =
            WorkspaceRegistry::open(pty_registry_config(state));
        registry->registerWorkspace(alpha, "alpha");
    }

    ChildOptions options;
    options.argv        = {"ymh", "--resume", "no-such-session"};
    options.executable  = resolve_ymh_binary();
    options.cwd         = alpha;
    options.env         = pty_env(root.path(), state);
    options.stdout_path = root.path() / "out.log";
    options.stderr_path = root.path() / "err.log";
    ChildProcess child(std::move(options));
    const std::optional<ExitStatus> status = child.wait_for(20s);
    ASSERT_TRUE(status.has_value()) << "ymh --resume <unknown> did not exit";
    EXPECT_TRUE(status->exited);
    EXPECT_EQ(status->code, 1);
    EXPECT_NE(read_text_file(root.path() / "err.log").find("unknown session"), std::string::npos);
    EXPECT_TRUE(host_processes().empty()) << "leaked ymh --host daemon(s)";
}

} // namespace

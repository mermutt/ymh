#pragma once

// Scripted PTY driver shared by the supervisor/live PTY tests
// (docs/design/10-supervisor-tui.md §12.1-§12.2). A real `ymh` binary runs on a
// pseudo-terminal; the test types keystrokes into the master and parses the
// rendered ANSI. It also owns the `/proc`-based daemon reaper so a failing test
// can never leak a `ymh --host` process.

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
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace ymh::test {

// Drop CSI escape sequences (`ESC [ ... final`). Sufficient for asserting on
// FTXUI output; other escape classes (OSC/DCS) are left as-is.
inline std::string strip_ansi(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() &&
                   (input[index] == ';' || input[index] == '?' ||
                    (input[index] >= '0' && input[index] <= '9'))) {
                ++index;
            }
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

inline std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// The last complete full-screen frame FTXUI printed, with styling removed.
// `ScreenInteractive` repaints the whole screen each frame after moving the
// cursor home: `\r` followed by (rows - 1) `ESC[1A` cursor-up sequences. That
// run is unique to the frame boundary (screen text uses only `\r\n` and SGR).
inline std::string last_frame_plain(const std::string& buffer, int rows) {
    std::string marker = "\r";
    for (int line = 1; line < rows; ++line) {
        marker += "\x1b[1A";
    }
    const std::size_t boundary = buffer.rfind(marker);
    if (boundary == std::string::npos) {
        return strip_ansi(buffer);
    }
    return strip_ansi(buffer.substr(boundary + marker.size()));
}

// A `ymh` process on a PTY master. Always reaped by the destructor.
class PtyChild {
public:
    PtyChild() = default;
    ~PtyChild() { terminate(); }

    PtyChild(const PtyChild&) = delete;
    PtyChild& operator=(const PtyChild&) = delete;

    bool spawn(const std::filesystem::path& binary, const std::filesystem::path& cwd,
               const std::map<std::string, std::string>& env, int rows = 30, int cols = 100) {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) {
            return false;
        }
        if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            return false;
        }
        winsize window{};
        window.ws_row = static_cast<unsigned short>(rows);
        window.ws_col = static_cast<unsigned short>(cols);
        ::ioctl(master_, TIOCSWINSZ, &window);

        const char* slave_name = ::ptsname(master_);
        if (slave_name == nullptr) {
            return false;
        }
        const int slave = ::open(slave_name, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            return false;
        }
        rows_ = rows;
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

    // Non-blocking drain of everything currently readable.
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

    bool wait_for(const std::string& needle, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            read_available();
            if (strip_ansi(buffer_).find(needle) != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        read_available();
        return strip_ansi(buffer_).find(needle) != std::string::npos;
    }

    // Waits for the child to exit on its own and returns its exit status, or
    // nullopt on timeout. Drains the master each iteration so a busy TUI never
    // blocks writing a frame to a full PTY buffer (which would stall its input
    // processing). The spec-16 last-exit tests need "the supervisor exits 0"
    // without a sleep (16 §8.4.2 scenario 5).
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
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::string& raw() const noexcept { return buffer_; }
    [[nodiscard]] std::string plain() const { return strip_ansi(buffer_); }
    [[nodiscard]] std::string last_frame() const { return last_frame_plain(buffer_, rows_); }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    void terminate() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                    pid_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
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
    int         master_{-1};
    pid_t       pid_{-1};
    int         rows_{30};
    std::string buffer_;
};

// `/proc/<pid>/cmdline` is a NUL-separated argv blob; a `ymh --host` daemon is
// identified by `--host` plus (when given) `--workspace <id>` / `--root <path>`.
// A registry read can lag the daemon's claim/exit, so teardown scans `/proc`.
inline std::vector<std::string> proc_args(pid_t pid) {
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

inline bool is_host_process(pid_t pid, const std::string* workspace_id) {
    const std::vector<std::string> args = proc_args(pid);
    bool                           host = false;
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

inline bool is_host_process_for_root(pid_t pid, const std::string& canonical_root) {
    const std::vector<std::string> args = proc_args(pid);
    bool                           host = false;
    bool root_match = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--host") {
            host = true;
        }
        if (args[index] == "--root" && index + 1 < args.size() && args[index + 1] == canonical_root) {
            root_match = true;
        }
    }
    return host && root_match;
}

inline std::vector<pid_t> scan_proc(const std::function<bool(pid_t)>& match) {
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
        if (match(pid)) {
            pids.push_back(pid);
        }
    }
    return pids;
}

inline std::vector<pid_t> host_processes(const std::string* workspace_id = nullptr) {
    return scan_proc([workspace_id](pid_t pid) { return is_host_process(pid, workspace_id); });
}

inline std::vector<pid_t> host_processes_for_root(const std::filesystem::path& root) {
    std::error_code            error;
    const std::filesystem::path canonical = std::filesystem::canonical(root, error);
    const std::string           expected  = (error ? root : canonical).string();
    return scan_proc([&expected](pid_t pid) { return is_host_process_for_root(pid, expected); });
}

inline void stop_hosts_for_root(const std::filesystem::path& root) {
    for (const pid_t pid : host_processes_for_root(root)) {
        ::kill(pid, SIGTERM);
    }
    for (int attempt = 0; attempt < 100 && !host_processes_for_root(root).empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    for (const pid_t pid : host_processes_for_root(root)) {
        ::kill(pid, SIGKILL);
    }
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
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        for (const pid_t pid : host_processes(&workspace_id_)) {
            ::kill(pid, SIGKILL);
        }
    }

private:
    std::string workspace_id_;
    bool        stopped_ = false;
};

} // namespace ymh::test

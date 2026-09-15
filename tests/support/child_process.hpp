#pragma once

// Generic child-process helper for the two-process integration harness. Spawns
// an absolute executable with an explicit argv/env, optional cwd and
// stdout/stderr capture files, and always reaps: the destructor terminates a
// still-running child so a failing test can never leak a daemon. The child is
// placed in its own process group by default so signals reach its whole tree
// without touching the test runner.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ymh::test {

struct ExitStatus {
    bool exited{false};
    int  code{0};
    bool signalled{false};
    int  signal{0};

    [[nodiscard]] bool ok() const noexcept { return exited && code == 0; }
};

struct ChildOptions {
    std::vector<std::string>           argv;
    std::filesystem::path              executable;
    std::filesystem::path              cwd;
    std::map<std::string, std::string> env;
    std::vector<std::string>           env_remove;
    std::filesystem::path              stdout_path;
    std::filesystem::path              stderr_path;
    bool                               new_process_group{true};
};

inline std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

inline std::filesystem::path resolve_executable(const std::filesystem::path& executable) {
    if (executable.is_absolute()) {
        return executable;
    }
    if (executable.has_parent_path()) {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(executable, error);
        return error ? executable : absolute;
    }
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return executable;
    }
    const std::string path{path_env};
    std::size_t       start = 0;
    while (start <= path.size()) {
        const std::size_t end  = path.find(':', start);
        std::string       dir  = path.substr(start, end == std::string::npos ? std::string::npos
                                                                            : end - start);
        if (dir.empty()) {
            dir = ".";
        }
        const std::filesystem::path candidate = std::filesystem::path{dir} / executable;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return executable;
}

inline std::vector<std::string> build_child_env(
    const std::map<std::string, std::string>& overrides,
    const std::vector<std::string>&           remove) {
    std::map<std::string, std::string> merged;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text{*entry};
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        merged[text.substr(0, equals)] = text.substr(equals + 1);
    }
    for (const std::string& key : remove) {
        merged.erase(key);
    }
    for (const auto& [key, value] : overrides) {
        merged[key] = value;
    }
    std::vector<std::string> result;
    result.reserve(merged.size());
    for (const auto& [key, value] : merged) {
        result.push_back(key + "=" + value);
    }
    return result;
}

class ChildProcess {
public:
    ChildProcess() = default;
    explicit ChildProcess(ChildOptions options) { start(std::move(options)); }
    ~ChildProcess() { terminate(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ChildProcess(ChildProcess&& other) noexcept { *this = std::move(other); }

    ChildProcess& operator=(ChildProcess&& other) noexcept {
        if (this != &other) {
            terminate();
            pid_       = other.pid_;
            own_group_ = other.own_group_;
            status_    = other.status_;
            other.pid_ = -1;
            other.status_.reset();
        }
        return *this;
    }

    void start(ChildOptions options) {
        if (pid_ > 0) {
            terminate();
        }
        status_.reset();
        if (options.argv.empty()) {
            throw std::invalid_argument("ChildProcess: argv is empty");
        }

        const std::filesystem::path executable = resolve_executable(
            options.executable.empty() ? std::filesystem::path{options.argv.front()}
                                       : options.executable);

        int stdout_fd = -1;
        int stderr_fd = -1;
        if (!options.stdout_path.empty()) {
            stdout_fd = ::open(options.stdout_path.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (stdout_fd < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "ChildProcess: open stdout capture");
            }
        }
        if (!options.stderr_path.empty()) {
            stderr_fd = ::open(options.stderr_path.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (stderr_fd < 0) {
                if (stdout_fd >= 0) {
                    ::close(stdout_fd);
                }
                throw std::system_error(errno, std::generic_category(),
                                        "ChildProcess: open stderr capture");
            }
        }

        std::vector<std::string> env_storage = build_child_env(options.env, options.env_remove);
        std::vector<char*>       envp;
        envp.reserve(env_storage.size() + 1);
        for (std::string& entry : env_storage) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        std::vector<char*> argv;
        argv.reserve(options.argv.size() + 1);
        for (std::string& argument : options.argv) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        const pid_t child = ::fork();
        if (child < 0) {
            if (stdout_fd >= 0) {
                ::close(stdout_fd);
            }
            if (stderr_fd >= 0) {
                ::close(stderr_fd);
            }
            throw std::system_error(errno, std::generic_category(), "ChildProcess: fork");
        }
        if (child == 0) {
            if (options.new_process_group) {
                ::setpgid(0, 0);
            }
            if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0) {
                ::_exit(126);
            }
            if (stdout_fd >= 0) {
                ::dup2(stdout_fd, STDOUT_FILENO);
                ::close(stdout_fd);
            }
            if (stderr_fd >= 0) {
                ::dup2(stderr_fd, STDERR_FILENO);
                ::close(stderr_fd);
            }
            // execve's envp is `char* const*`; the buffers above are mutable and
            // outlive the call in this forked child, so const_cast is the C-API
            // adaptation, not a warning silencer.
            ::execve(executable.c_str(), argv.data(), envp.data());
            ::_exit(127);
        }

        if (options.new_process_group) {
            ::setpgid(child, child);
        }
        if (stdout_fd >= 0) {
            ::close(stdout_fd);
        }
        if (stderr_fd >= 0) {
            ::close(stderr_fd);
        }
        pid_       = child;
        own_group_ = options.new_process_group;
    }

    [[nodiscard]] bool running() {
        if (pid_ <= 0) {
            return false;
        }
        poll_status(WNOHANG);
        return pid_ > 0;
    }

    [[nodiscard]] std::optional<ExitStatus> wait_for(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return status_;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (poll_status(WNOHANG)) {
                return status_;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    void signal(int signo) const {
        if (pid_ <= 0) {
            return;
        }
        if (own_group_) {
            ::kill(-pid_, signo);
        } else {
            ::kill(pid_, signo);
        }
    }

    ExitStatus terminate(std::chrono::milliseconds grace = std::chrono::milliseconds{2000}) {
        if (pid_ <= 0) {
            return status_.value_or(ExitStatus{});
        }
        signal(SIGTERM);
        if (std::optional<ExitStatus> status = wait_for(grace); status.has_value()) {
            return *status;
        }
        signal(SIGKILL);
        if (std::optional<ExitStatus> status = wait_for(std::chrono::seconds{5}); status.has_value()) {
            return *status;
        }
        return status_.value_or(ExitStatus{});
    }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] const std::optional<ExitStatus>& last_status() const noexcept { return status_; }

private:
    bool poll_status(int wait_options) {
        if (pid_ <= 0) {
            return true;
        }
        int        raw    = 0;
        const pid_t result = ::waitpid(pid_, &raw, wait_options | WNOHANG);
        if (result == pid_) {
            ExitStatus status;
            if (WIFEXITED(raw)) {
                status.exited = true;
                status.code   = WEXITSTATUS(raw);
            } else if (WIFSIGNALED(raw)) {
                status.signalled = true;
                status.signal    = WTERMSIG(raw);
            }
            status_ = status;
            pid_    = -1;
            return true;
        }
        if (result < 0 && errno == ECHILD) {
            pid_ = -1;
            return true;
        }
        return false;
    }

    pid_t                     pid_{-1};
    bool                      own_group_{false};
    std::optional<ExitStatus> status_;
};

inline ExitStatus run_child(ChildOptions options,
                            std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    ChildProcess child(std::move(options));
    if (std::optional<ExitStatus> status = child.wait_for(timeout); status.has_value()) {
        return *status;
    }
    return child.terminate();
}

} // namespace ymh::test

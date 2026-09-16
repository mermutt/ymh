#include "ymh/execution/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ymh/execution/errors.hpp"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace ymh {
namespace {

using Clock = std::chrono::steady_clock;

struct Pipe {
    int read_fd = -1;
    int write_fd = -1;
};

Pipe make_pipe() {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        throw ToolError{ToolErrorCode::Io,
                        std::string{"pipe: "} + std::strerror(errno)};
    }
    return Pipe{fds[0], fds[1]};
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void redirect_to_devnull(int target) {
    const int fd = ::open("/dev/null", O_RDWR);
    if (fd >= 0) {
        ::dup2(fd, target);
        if (fd != target) {
            ::close(fd);
        }
    }
}

std::vector<char*> to_argv(const std::vector<std::string>& argv) {
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        raw.push_back(const_cast<char*>(arg.c_str()));
    }
    raw.push_back(nullptr);
    return raw;
}

bool drain_fd(int fd, bool is_stderr, OutputSink* sink) {
    std::array<char, 65536> buffer{};
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            if (sink != nullptr) {
                const std::string_view chunk(buffer.data(), static_cast<std::size_t>(n));
                if (is_stderr) {
                    sink->writeErr(chunk);
                } else {
                    sink->write(chunk);
                }
            }
            continue;
        }
        if (n == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

void fill_from_status(int status, ProcessResult& result) {
    result.exit_code = -1;
    result.signalled = false;
    result.signal = 0;
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signalled = true;
        result.signal = WTERMSIG(status);
        result.exit_code = 128 + result.signal;
    }
}

void terminate_group(int pid, std::chrono::milliseconds grace, ProcessResult& result) {
    ::kill(-pid, SIGTERM);
    int status = 0;
    bool reaped = false;
    const auto deadline = Clock::now() + grace;
    while (Clock::now() < deadline) {
        const pid_t child = ::waitpid(pid, &status, WNOHANG);
        if (child == pid) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (!reaped) {
        ::kill(-pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    fill_from_status(status, result);
}

} // namespace

// The sole reaper for tool children (E8, M-F5): always a specific pid, never a
// global `waitpid(-1)` sweep, which would race this path and destroy the exit
// status it needs. The daemon installs no SIGCHLD reaper; see
// `ymh/execution/signal_policy.hpp`.
std::optional<ProcessResult> tryReap(int pid) {
    int status = 0;
    const pid_t child = ::waitpid(pid, &status, WNOHANG);
    if (child != pid) {
        return std::nullopt;
    }
    ProcessResult result;
    fill_from_status(status, result);
    return result;
}

ProcessResult reap(int pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }
    ProcessResult result;
    fill_from_status(status, result);
    return result;
}

LocalProcessService::LocalProcessService(std::chrono::milliseconds terminate_grace)
    : terminate_grace_(terminate_grace) {}

Task<ProcessResult> LocalProcessService::run(const ProcessRequest& request,
                                             CancellationToken cancel) {
    if (request.executable.empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "empty executable"};
    }
    if (request.argv.empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "empty argv"};
    }

    Pipe out = make_pipe();
    Pipe err = make_pipe();
    Pipe errsig = make_pipe();

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_fd(out.read_fd);
        close_fd(out.write_fd);
        close_fd(err.read_fd);
        close_fd(err.write_fd);
        close_fd(errsig.read_fd);
        close_fd(errsig.write_fd);
        throw ToolError{ToolErrorCode::Io, std::string{"fork: "} + std::strerror(errno)};
    }

    if (pid == 0) {
        ::setpgid(0, 0);
        if (!request.cwd.empty()) {
            if (::chdir(request.cwd.c_str()) != 0) {
                const int saved = errno;
                (void)::write(errsig.write_fd, &saved, sizeof(saved));
                _exit(127);
            }
        }
        if (request.capture_stdout) {
            ::dup2(out.write_fd, STDOUT_FILENO);
        } else {
            redirect_to_devnull(STDOUT_FILENO);
        }
        if (request.capture_stderr) {
            ::dup2(err.write_fd, STDERR_FILENO);
        } else {
            redirect_to_devnull(STDERR_FILENO);
        }
        redirect_to_devnull(STDIN_FILENO);

        for (const auto& [key, value] : request.environment) {
            ::setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv = to_argv(request.argv);
        ::execvp(request.executable.c_str(), argv.data());

        const int saved = errno;
        (void)::write(errsig.write_fd, &saved, sizeof(saved));
        _exit(127);
    }

    ::setpgid(pid, pid);
    close_fd(out.write_fd);
    close_fd(err.write_fd);
    close_fd(errsig.write_fd);
    if (!request.capture_stdout) {
        close_fd(out.read_fd);
    }
    if (!request.capture_stderr) {
        close_fd(err.read_fd);
    }
    if (out.read_fd >= 0) {
        set_nonblocking(out.read_fd);
    }
    if (err.read_fd >= 0) {
        set_nonblocking(err.read_fd);
    }
    set_nonblocking(errsig.read_fd);

    ProcessResult result;
    bool out_open = out.read_fd >= 0;
    bool err_open = err.read_fd >= 0;
    bool terminated = false;
    const auto deadline = request.timeout.count() > 0
                              ? Clock::now() + request.timeout
                              : Clock::time_point::max();

    while (out_open || err_open) {
        if (cancel.cancelled()) {
            terminate_group(pid, terminate_grace_, result);
            terminated = true;
            break;
        }
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            terminate_group(pid, terminate_grace_, result);
            terminated = true;
            break;
        }

        std::array<pollfd, 3> fds{};
        nfds_t count = 0;
        if (out_open) {
            fds[count++] = pollfd{out.read_fd, POLLIN, 0};
        }
        if (err_open) {
            fds[count++] = pollfd{err.read_fd, POLLIN, 0};
        }
        fds[count++] = pollfd{errsig.read_fd, POLLIN, 0};

        const int ready = ::poll(fds.data(), count, 25);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (out_open && drain_fd(out.read_fd, false, request.sink)) {
            close_fd(out.read_fd);
            out_open = false;
        }
        if (err_open && drain_fd(err.read_fd, true, request.sink)) {
            close_fd(err.read_fd);
            err_open = false;
        }

        int exec_errno = 0;
        const ssize_t sig_read = ::read(errsig.read_fd, &exec_errno, sizeof(exec_errno));
        if (sig_read == static_cast<ssize_t>(sizeof(exec_errno))) {
            ::kill(-pid, SIGKILL);
            close_fd(out.read_fd);
            close_fd(err.read_fd);
            close_fd(errsig.read_fd);
            result = reap(pid);
            throw ToolError{ToolErrorCode::Io,
                            std::string{"exec: "} + std::strerror(exec_errno)};
        }
    }

    int exec_errno = 0;
    if (::read(errsig.read_fd, &exec_errno, sizeof(exec_errno)) ==
        static_cast<ssize_t>(sizeof(exec_errno))) {
        close_fd(out.read_fd);
        close_fd(err.read_fd);
        close_fd(errsig.read_fd);
        result = reap(pid);
        throw ToolError{ToolErrorCode::Io,
                        std::string{"exec: "} + std::strerror(exec_errno)};
    }

    if (out.read_fd >= 0) {
        drain_fd(out.read_fd, false, request.sink);
    }
    if (err.read_fd >= 0) {
        drain_fd(err.read_fd, true, request.sink);
    }
    close_fd(out.read_fd);
    close_fd(err.read_fd);
    close_fd(errsig.read_fd);

    if (!terminated) {
        result = reap(pid);
    }
    if (request.sink != nullptr) {
        request.sink->close();
    }
    return Task<ProcessResult>(result);
}

} // namespace ymh

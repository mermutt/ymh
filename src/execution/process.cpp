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

class SpawnedChild final : public ChildProcessHandle {
public:
    SpawnedChild(pid_t pid, int stdin_fd, int stdout_fd) noexcept
        : pid_(pid), stdin_fd_(stdin_fd), stdout_fd_(stdout_fd) {}

    ~SpawnedChild() override {
        if (stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
        if (stdout_fd_ >= 0) {
            ::close(stdout_fd_);
            stdout_fd_ = -1;
        }
        if (!reaped_) {
            ::kill(-pid_, SIGKILL);
            reap();
        }
    }

    std::uint64_t pid() const noexcept override {
        return static_cast<std::uint64_t>(pid_);
    }

    int stdoutFd() const noexcept { return stdout_fd_; }

    Task<void> writeStdin(std::string_view bytes, CancellationToken cancel) override {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            cancel.throw_if_cancelled();
            if (stdin_fd_ < 0) {
                throw ToolError{ToolErrorCode::Io, "stdin is closed"};
            }
            const ssize_t written =
                ::write(stdin_fd_, bytes.data() + offset, bytes.size() - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd fd{stdin_fd_, POLLOUT, 0};
                if (::poll(&fd, 1, 25) < 0 && errno != EINTR) {
                    throw ToolError{ToolErrorCode::Io,
                                    std::string{"poll stdin: "} + std::strerror(errno)};
                }
                continue;
            }
            throw ToolError{ToolErrorCode::Io,
                            std::string{"write stdin: "} + std::strerror(errno)};
        }
        return Task<void>{};
    }

    void closeStdin() override {
        if (stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
    }

    Task<std::size_t> readStdout(std::span<char> buffer, CancellationToken cancel) override {
        if (buffer.empty()) {
            return Task<std::size_t>{0};
        }
        for (;;) {
            cancel.throw_if_cancelled();
            if (stdout_fd_ < 0) {
                return Task<std::size_t>{0};
            }
            const ssize_t count = ::read(stdout_fd_, buffer.data(), buffer.size());
            if (count > 0) {
                return Task<std::size_t>{static_cast<std::size_t>(count)};
            }
            if (count == 0) {
                return Task<std::size_t>{0};
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd fd{stdout_fd_, POLLIN, 0};
                if (::poll(&fd, 1, 25) < 0 && errno != EINTR) {
                    throw ToolError{ToolErrorCode::Io,
                                    std::string{"poll stdout: "} + std::strerror(errno)};
                }
                continue;
            }
            throw ToolError{ToolErrorCode::Io,
                            std::string{"read stdout: "} + std::strerror(errno)};
        }
    }

    void signal(int sig) noexcept override { ::kill(-pid_, sig); }

    Task<ProcessResult> wait(CancellationToken cancel) override {
        for (;;) {
            if (const std::optional<ProcessResult> reaped = tryReap(pid_);
                reaped.has_value()) {
                reaped_ = true;
                return Task<ProcessResult>{*reaped};
            }
            if (cancel.cancelled()) {
                ProcessResult result;
                terminate_group(pid_, std::chrono::milliseconds{2000}, result);
                reaped_ = true;
                throw CancellationError{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

private:
    void reap() {
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        reaped_ = true;
    }

    pid_t pid_;
    int   stdin_fd_;
    int   stdout_fd_;
    bool  reaped_ = false;
};

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

Task<std::unique_ptr<ChildProcessHandle>> LocalProcessService::spawn(
    const ProcessRequest& request) {
    if (request.executable.empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "empty executable"};
    }
    if (request.argv.empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "empty argv"};
    }

    Pipe in = make_pipe();
    Pipe out = make_pipe();
    Pipe errsig = make_pipe();

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_fd(in.read_fd);
        close_fd(in.write_fd);
        close_fd(out.read_fd);
        close_fd(out.write_fd);
        close_fd(errsig.read_fd);
        close_fd(errsig.write_fd);
        throw ToolError{ToolErrorCode::Io,
                        std::string{"fork: "} + std::strerror(errno)};
    }

    if (pid == 0) {
        ::setpgid(0, 0);
        if (!request.cwd.empty() && ::chdir(request.cwd.c_str()) != 0) {
            const int saved = errno;
            (void)::write(errsig.write_fd, &saved, sizeof(saved));
            _exit(127);
        }
        ::dup2(in.read_fd, STDIN_FILENO);
        ::dup2(out.write_fd, STDOUT_FILENO);
        redirect_to_devnull(STDERR_FILENO);
        ::clearenv();
        ::setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
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
    close_fd(in.read_fd);
    close_fd(out.write_fd);
    close_fd(errsig.write_fd);

    bool exec_failed = false;
    int  exec_errno = 0;
    {
        pollfd fd{errsig.read_fd, POLLIN, 0};
        const int ready = ::poll(&fd, 1, 200);
        if (ready > 0 && (fd.revents & POLLIN) != 0) {
            const ssize_t count = ::read(errsig.read_fd, &exec_errno, sizeof(exec_errno));
            exec_failed = count == static_cast<ssize_t>(sizeof(exec_errno));
        }
    }
    close_fd(errsig.read_fd);

    if (exec_failed) {
        close_fd(in.write_fd);
        close_fd(out.read_fd);
        ::kill(-pid, SIGKILL);
        (void)reap(pid);
        throw ToolError{ToolErrorCode::Io,
                        std::string{"exec: "} + std::strerror(exec_errno)};
    }

    set_nonblocking(in.write_fd);
    set_nonblocking(out.read_fd);
    return Task<std::unique_ptr<ChildProcessHandle>>(
        std::make_unique<SpawnedChild>(pid, in.write_fd, out.read_fd));
}

int childStdoutFd(ChildProcessHandle& handle) noexcept {
    if (auto* spawned = dynamic_cast<SpawnedChild*>(&handle)) {
        return spawned->stdoutFd();
    }
    return -1;
}

} // namespace ymh

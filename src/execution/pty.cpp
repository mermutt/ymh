#include "ymh/execution/pty.hpp"

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>

#include "ymh/execution/asio_executor.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/execution/resource_governor.hpp"

extern "C" char** environ;

namespace ymh {

// ---- 14 §2.2 — error taxonomy ----------------------------------------------

PtyError::PtyError(PtyErrorCode code, const std::string& what)
    : std::runtime_error(what), code_(code) {}

std::string_view to_string(PtyErrorCode code) noexcept {
    switch (code) {
        case PtyErrorCode::SpawnFailed:  return "spawn_failed";
        case PtyErrorCode::NotFound:     return "not_found";
        case PtyErrorCode::Closed:       return "closed";
        case PtyErrorCode::Eio:          return "eio";
        case PtyErrorCode::WouldBlock:   return "would_block";
        case PtyErrorCode::CapExceeded:  return "cap_exceeded";
        case PtyErrorCode::ResizeFailed: return "resize_failed";
        case PtyErrorCode::Internal:     return "internal";
    }
    return "internal";
}

ToolErrorCode to_tool_error_code(PtyErrorCode code) noexcept {
    switch (code) {
        case PtyErrorCode::SpawnFailed:  return ToolErrorCode::Io;
        case PtyErrorCode::NotFound:     return ToolErrorCode::NotFound;
        case PtyErrorCode::Closed:       return ToolErrorCode::Io;
        case PtyErrorCode::Eio:          return ToolErrorCode::Io;
        case PtyErrorCode::WouldBlock:   return ToolErrorCode::ResourceExhausted;
        case PtyErrorCode::CapExceeded:  return ToolErrorCode::ResourceExhausted;
        case PtyErrorCode::ResizeFailed: return ToolErrorCode::Io;
        case PtyErrorCode::Internal:     return ToolErrorCode::Internal;
    }
    return ToolErrorCode::Internal;
}

// ---- 14 §5.2 — PtyOutputRing -----------------------------------------------

PtyOutputRing::PtyOutputRing(std::size_t capacity_bytes)
    : capacity_(capacity_bytes == 0 ? 1 : capacity_bytes) {}

void PtyOutputRing::append(std::string_view chunk) {
    bytes_.insert(bytes_.end(), chunk.begin(), chunk.end());
    if (bytes_.size() > capacity_) {
        const std::size_t excess = bytes_.size() - capacity_;
        for (std::size_t i = 0; i < excess; ++i) {
            bytes_.pop_front();
        }
        lost_unread_ = true;
    }
}

std::string PtyOutputRing::read(std::size_t max_bytes, bool& truncated) {
    truncated = lost_unread_;
    lost_unread_ = false;

    std::size_t take = std::min(max_bytes, bytes_.size());
    while (take > 0 && take < bytes_.size() &&
           (static_cast<unsigned char>(bytes_[take]) & 0xC0) == 0x80) {
        --take;
    }
    std::string out;
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        out.push_back(bytes_.front());
        bytes_.pop_front();
    }
    return out;
}

void PtyOutputRing::clear() noexcept {
    bytes_.clear();
    lost_unread_ = false;
}

// ---- the concrete session + pump -------------------------------------------

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t kReadChunk = 64 * 1024;
constexpr std::size_t kWriteChunk = 64 * 1024;
constexpr auto        kReapPollInterval = std::chrono::milliseconds{50};
// Bound on waiting for the loop to finish session teardown during service
// destruction; on expiry the close is retried directly as a best effort.
constexpr auto kFinishTimeout = std::chrono::seconds{5};

std::vector<char*> to_raw(const std::vector<std::string>& strings) {
    std::vector<char*> raw;
    raw.reserve(strings.size() + 1);
    for (const std::string& value : strings) {
        raw.push_back(const_cast<char*>(value.c_str()));
    }
    raw.push_back(nullptr);
    return raw;
}

void strip_env(std::vector<std::string>& env, std::string_view key) {
    const std::string prefix = std::string(key) + "=";
    env.erase(std::remove_if(env.begin(), env.end(),
                             [&](const std::string& entry) {
                                 return entry.rfind(prefix, 0) == 0;
                             }),
              env.end());
}

std::filesystem::path resolve_executable(const std::string& executable) {
    if (executable.find('/') != std::string::npos) {
        return std::filesystem::path{executable};
    }
    const char* path_env = ::getenv("PATH");
    if (path_env == nullptr) {
        return std::filesystem::path{executable};
    }
    std::stringstream stream{path_env};
    std::string       directory;
    while (std::getline(stream, directory, ':')) {
        std::filesystem::path candidate =
            std::filesystem::path(directory.empty() ? "." : directory) / executable;
        std::error_code error;
        if (std::filesystem::exists(candidate, error) &&
            ::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::filesystem::path{executable};
}

} // namespace

class LocalPtySession final : public PtySession,
                              public std::enable_shared_from_this<LocalPtySession> {
public:
    LocalPtySession(Executor& loop,
                    asio::io_context& io,
                    ResourceGovernor& governor,
                    PtyEventSink& events,
                    ToolConfig config,
                    PtySessionId id,
                    const PtyRequest& request)
        : loop_(loop),
          io_(io),
          events_(events),
          config_(config),
          id_(id),
          session_(request.session),
          term_(request.term),
          rows_(request.rows),
          cols_(request.cols),
          ring_(governor.caps().pty_output_ring_bytes),
          slot_(std::in_place, governor, request.session) {
        if (!slot_.has_value() || !slot_->held()) {
            throw PtyError{PtyErrorCode::CapExceeded, "pty cap exhausted"};
        }
        spawn(request);
    }

    ~LocalPtySession() override {
        closeDescriptors();
        writer_.finish();
        if (pid_ > 0 && !reaped_) {
            ::kill(-pid_, SIGKILL);
            std::lock_guard<std::mutex> lock(reap_mutex_);
            if (!reaped_) {
                (void)reap(pid_);
                reaped_ = true;
            }
        }
    }

    void write(std::string_view bytes) override {
        const PtyState current = state_.load(std::memory_order_acquire);
        if (current == PtyState::Exited || current == PtyState::Closed) {
            throw PtyError{PtyErrorCode::Closed, "pty is not running"};
        }
        if (bytes.empty()) {
            return;
        }
        if (loop_.stopped()) {
            throw PtyError{PtyErrorCode::Internal, "pty loop stopped"};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (bytes.size() > config_.pty_write_queue_cap ||
                outbound_.size() + bytes.size() > config_.pty_write_queue_cap) {
                throw PtyError{PtyErrorCode::WouldBlock, "pty write queue full"};
            }
            outbound_.append(bytes);
        }
        const std::size_t appended = bytes.size();
        std::shared_ptr<LocalPtySession> self = shared_from_this();
        if (!loop_.post([self] { self->armWrite(); })) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outbound_.size() >= appended) {
                outbound_.resize(outbound_.size() - appended);
            }
            throw PtyError{PtyErrorCode::Internal, "pty loop stopped"};
        }
    }

    Stream<std::string> output() override { return writer_.handle(); }

    void resize(int rows, int cols) override {
        if (rows <= 0 || cols <= 0) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "terminal size must be positive"};
        }
        if (state_.load(std::memory_order_acquire) == PtyState::Closed) {
            return;
        }
        if (loop_.stopped()) {
            return;
        }
        std::shared_ptr<LocalPtySession> self = shared_from_this();
        (void)loop_.post([self, rows, cols] { self->applyResize(rows, cols); });
    }

    void terminate() override { requestTeardown(false); }
    void kill() override { requestTeardown(true); }

    PtySessionId id() const noexcept override { return id_; }

    [[nodiscard]] bool closed() const noexcept {
        return state_.load(std::memory_order_acquire) == PtyState::Closed;
    }
    SessionId    session() const noexcept override { return session_; }
    int          pid() const noexcept override { return static_cast<int>(pid_); }
    PtyState     state() const noexcept override { return state_.load(std::memory_order_acquire); }

    Task<PtyRead> read(std::size_t max_bytes,
                       std::chrono::milliseconds wait,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) override {
        PtyRead result;
        if (deadline.has_value() && deadline->count() == 0) {
            result.timed_out = true;
            return Task<PtyRead>(std::move(result));
        }
        if (cancel.cancelled()) {
            result.cancelled = true;
            return Task<PtyRead>(std::move(result));
        }
        std::weak_ptr<LocalPtySession> weak = weak_from_this();
        cancel.on_cancel([weak] {
            if (auto session = weak.lock()) {
                session->cv_.notify_all();
            }
        });

        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this] { return !ring_.empty() || child_exited_; };

        bool deadline_binding = false;
        std::optional<std::chrono::milliseconds> bound;
        if (deadline.has_value()) {
            if (wait.count() > 0) {
                bound = std::min(wait, *deadline);
                deadline_binding = *deadline <= wait;
            } else {
                bound = std::chrono::milliseconds{0};
            }
        } else if (wait.count() > 0) {
            bound = wait;
        }
        if (bound.has_value() && bound->count() > 0) {
            cv_.wait_for(lock, *bound, [&] { return ready() || cancel.cancelled(); });
        }
        if (cancel.cancelled() && ring_.empty()) {
            result.cancelled = true;
            return Task<PtyRead>(std::move(result));
        }
        bool truncated = false;
        result.data = ring_.read(max_bytes, truncated);
        result.truncated = truncated || utf8_loss_pending_;
        utf8_loss_pending_ = false;
        result.eof = child_exited_ && ring_.empty();
        if (deadline_binding && result.data.empty() && !result.eof) {
            result.timed_out = true;
        }
        return Task<PtyRead>(std::move(result));
    }

    Task<PtyExit> wait(std::chrono::milliseconds timeout,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) override {
        if (deadline.has_value() && deadline->count() == 0) {
            PtyExit timed_out;
            timed_out.timed_out = true;
            return Task<PtyExit>(timed_out);
        }
        if (cancel.cancelled()) {
            throw CancellationError{};
        }
        std::weak_ptr<LocalPtySession> weak = weak_from_this();
        cancel.on_cancel([weak] {
            if (auto session = weak.lock()) {
                session->cv_.notify_all();
            }
        });

        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this] { return exit_ready_; };
        bool deadline_binding = false;
        if (deadline.has_value()) {
            if (timeout.count() == 0) {
                cv_.wait_for(lock, *deadline, [&] { return ready() || cancel.cancelled(); });
                deadline_binding = true;
            } else {
                deadline_binding = *deadline <= timeout;
                cv_.wait_for(lock, std::min(timeout, *deadline),
                             [&] { return ready() || cancel.cancelled(); });
            }
        } else if (timeout.count() == 0) {
            cv_.wait(lock, [&] { return ready() || cancel.cancelled(); });
        } else {
            cv_.wait_for(lock, timeout, [&] { return ready() || cancel.cancelled(); });
        }
        if (cancel.cancelled()) {
            throw CancellationError{};
        }
        if (!exit_ready_) {
            PtyExit result;
            result.timed_out = deadline_binding;
            return Task<PtyExit>(result);
        }
        return Task<PtyExit>(exit_);
    }

    void startPump() {
        if (status_) {
            std::weak_ptr<LocalPtySession> self = weak_from_this();
            status_->async_read_some(
                asio::buffer(status_buf_),
                [self](std::error_code ec, std::size_t n) {
                    if (auto session = self.lock()) {
                        session->onStatusRead(ec, n);
                    }
                });
            return;
        }
        state_.store(PtyState::Running, std::memory_order_release);
        startMasterRead();
        startExitWatch();
    }

    void teardownOnLoop(bool force) {
        const std::lock_guard<std::mutex> teardown(teardown_mutex_);
        if (state_.load(std::memory_order_acquire) == PtyState::Closed) {
            return;
        }
        closeMaster();
        if (force) {
            ::kill(-pid_, SIGKILL);
            startReapRetry();
            return;
        }
        ::kill(-pid_, SIGHUP);
        std::shared_ptr<LocalPtySession> self = shared_from_this();
        grace_timer_.expires_after(config_.terminate_grace);
        grace_timer_.async_wait([self](std::error_code ec) {
            if (ec) {
                return;
            }
            ::kill(-self->pid_, SIGKILL);
            self->startReapRetry();
        });
    }

    void closeFromService() noexcept {
        const std::lock_guard<std::mutex> teardown(teardown_mutex_);
        if (state_.load(std::memory_order_acquire) == PtyState::Closed) {
            return;
        }
        closeMaster();
        ::kill(-pid_, SIGKILL);
        {
            std::unique_lock<std::mutex> lock(reap_mutex_);
            if (!reaped_) {
                const ProcessResult status = reap(pid_);
                reaped_ = true;
                storeExit(status);
            }
        }
        completeClose();
    }

private:
    void spawn(const PtyRequest& request) {
        winsize window{};
        window.ws_row = static_cast<unsigned short>(std::max(1, request.rows));
        window.ws_col = static_cast<unsigned short>(std::max(1, request.cols));

        int master_fd = -1;
        int slave_fd = -1;
        if (::openpty(&master_fd, &slave_fd, nullptr, nullptr, &window) != 0) {
            throw PtyError{PtyErrorCode::SpawnFailed, "openpty failed"};
        }
        const int master_flags = ::fcntl(master_fd, F_GETFL, 0);
        (void)::fcntl(master_fd, F_SETFL, master_flags | O_NONBLOCK);
        (void)::fcntl(master_fd, F_SETFD, FD_CLOEXEC);

        int status_pipe[2] = {-1, -1};
        if (::pipe2(status_pipe, O_CLOEXEC) != 0) {
            ::close(master_fd);
            ::close(slave_fd);
            throw PtyError{PtyErrorCode::SpawnFailed, "status pipe failed"};
        }
        const int status_flags = ::fcntl(status_pipe[0], F_GETFL, 0);
        (void)::fcntl(status_pipe[0], F_SETFL, status_flags | O_NONBLOCK);

        std::vector<std::string> environment;
        environment.reserve(64);
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            environment.emplace_back(*entry);
        }
        strip_env(environment, "TERM");
        strip_env(environment, "PWD");
        environment.push_back("TERM=" + request.term);
        environment.push_back("PWD=" + request.cwd.string());
        for (const auto& [key, value] : request.environment) {
            strip_env(environment, key);
            environment.push_back(key + "=" + value);
        }

        std::vector<std::string> argv = request.argv;
        std::vector<char*>       argv_raw = to_raw(argv);
        std::vector<char*>       env_raw = to_raw(environment);
        const std::filesystem::path executable = resolve_executable(request.executable);
        const std::filesystem::path cwd = request.cwd;

        const pid_t child = ::fork();
        if (child < 0) {
            ::close(master_fd);
            ::close(slave_fd);
            ::close(status_pipe[0]);
            ::close(status_pipe[1]);
            throw PtyError{PtyErrorCode::SpawnFailed, "fork failed"};
        }
        if (child == 0) {
            (void)::setsid();
            (void)::ioctl(slave_fd, TIOCSCTTY, 0);
            (void)::dup2(slave_fd, STDIN_FILENO);
            (void)::dup2(slave_fd, STDOUT_FILENO);
            (void)::dup2(slave_fd, STDERR_FILENO);
            if (slave_fd > STDERR_FILENO) {
                ::close(slave_fd);
            }
            ::close(master_fd);
            ::close(status_pipe[0]);
            if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
                const char status_byte = 'E';
                (void)!::write(status_pipe[1], &status_byte, 1);
                ::_exit(127);
            }
            ::execve(executable.c_str(), argv_raw.data(), env_raw.data());
            const char status_byte = 'E';
            (void)!::write(status_pipe[1], &status_byte, 1);
            ::_exit(127);
        }

        pid_ = child;
        ::close(slave_fd);
        ::close(status_pipe[1]);
        try {
            master_.emplace(io_, master_fd);
            status_.emplace(io_, status_pipe[0]);
        } catch (...) {
            ::kill(-child, SIGKILL);
            (void)reap(child);
            reaped_ = true;
            ::close(master_fd);
            ::close(status_pipe[0]);
            throw PtyError{PtyErrorCode::SpawnFailed, "pty registration failed"};
        }
        state_.store(PtyState::Starting, std::memory_order_release);
    }

    void onStatusRead(std::error_code ec, std::size_t n) {
        if (n == 1) {
            status_.reset();
            startMasterRead();
            startExitWatch();
            return;
        }
        (void)ec;
        status_.reset();
        state_.store(PtyState::Running, std::memory_order_release);
        startMasterRead();
        startExitWatch();
    }

    void startMasterRead() {
        if (!master_ || master_drained_) {
            return;
        }
        std::weak_ptr<LocalPtySession> self = weak_from_this();
        master_->async_read_some(
            asio::buffer(read_buf_),
            [self](std::error_code ec, std::size_t n) {
                if (auto session = self.lock()) {
                    session->onMasterRead(ec, n);
                }
            });
    }

    void onMasterRead(std::error_code ec, std::size_t n) {
        if (n > 0) {
            const std::string_view chunk(read_buf_.data(), n);
            bool                     lost = false;
            std::string              clean = sanitize_utf8(chunk, lost);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ring_.append(clean);
                if (lost) {
                    utf8_loss_pending_ = true;
                }
            }
            writer_.push(clean);
            emitLive(clean);
            cv_.notify_all();
            if (!master_drained_) {
                startMasterRead();
            }
            return;
        }
        (void)ec;
        onMasterEnd();
    }

    void onMasterEnd() {
        master_drained_ = true;
        if (master_) {
            master_->close();
            master_.reset();
        }
        cv_.notify_all();
        if (state_.load(std::memory_order_acquire) == PtyState::Exited) {
            flushLive();
        }
    }

    void armWrite() {
        std::lock_guard<std::mutex> lock(mutex_);
        armWriteLocked();
    }

    void armWriteLocked() {
        if (write_in_flight_ || outbound_.empty() || !master_ || master_drained_) {
            return;
        }
        const std::size_t chunk = std::min<std::size_t>(outbound_.size(), kWriteChunk);
        write_chunk_.assign(outbound_.begin(),
                            outbound_.begin() + static_cast<std::ptrdiff_t>(chunk));
        write_in_flight_ = true;
        std::weak_ptr<LocalPtySession> self = weak_from_this();
        master_->async_write_some(
            asio::buffer(write_chunk_),
            [self](std::error_code ec, std::size_t written) {
                if (auto session = self.lock()) {
                    session->onWrite(ec, written);
                }
            });
    }

    void onWrite(std::error_code ec, std::size_t n) {
        std::lock_guard<std::mutex> lock(mutex_);
        write_in_flight_ = false;
        write_chunk_.clear();
        if (n > 0) {
            const std::size_t consumed = std::min<std::size_t>(n, outbound_.size());
            outbound_.erase(outbound_.begin(),
                            outbound_.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
        if (ec) {
            outbound_.clear();
            return;
        }
        armWriteLocked();
    }

    void applyResize(int rows, int cols) {
        if (!master_) {
            return;
        }
        winsize window{};
        window.ws_row = static_cast<unsigned short>(rows);
        window.ws_col = static_cast<unsigned short>(cols);
        if (::ioctl(master_->native_handle(), TIOCSWINSZ, &window) == 0) {
            rows_ = rows;
            cols_ = cols;
        }
    }

    void requestTeardown(bool force) {
        const PtyState current = state_.load(std::memory_order_acquire);
        if (current == PtyState::Exited || current == PtyState::Closed) {
            return;
        }
        if (loop_.stopped()) {
            closeFromService();
            return;
        }
        std::shared_ptr<LocalPtySession> self = shared_from_this();
        if (!loop_.post([self, force] { self->teardownOnLoop(force); })) {
            closeFromService();
        }
    }

    void startExitWatch() {
        const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0));
        if (pidfd >= 0) {
            pidfd_.emplace(io_, pidfd);
            std::weak_ptr<LocalPtySession> self = weak_from_this();
            pidfd_->async_wait(asio::posix::stream_descriptor::wait_read,
                               [self](std::error_code ec) {
                                   if (auto session = self.lock()) {
                                       session->onPidfdReady(ec);
                                   }
                               });
            return;
        }
        scheduleReapPoll();
    }

    void onPidfdReady(std::error_code ec) {
        if (ec) {
            return;
        }
        if (pollReap()) {
            completeCloseIfTerminating();
            return;
        }
        scheduleReapPoll();
    }

    void scheduleReapPoll() {
        std::weak_ptr<LocalPtySession> self = weak_from_this();
        reap_timer_.expires_after(kReapPollInterval);
        reap_timer_.async_wait([self](std::error_code ec) {
            if (auto session = self.lock()) {
                session->onReapPoll(ec);
            }
        });
    }

    void onReapPoll(std::error_code ec) {
        if (ec) {
            return;
        }
        if (pollReap()) {
            completeCloseIfTerminating();
            return;
        }
        scheduleReapPoll();
    }

    bool pollReap() {
        std::unique_lock<std::mutex> lock(reap_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;
        }
        if (reaped_) {
            return true;
        }
        std::optional<ProcessResult> status = tryReap(pid_);
        if (!status.has_value()) {
            return false;
        }
        reaped_ = true;
        storeExit(*status);
        return true;
    }

    void startReapRetry() {
        if (pollReap()) {
            completeClose();
            return;
        }
        std::shared_ptr<LocalPtySession> self = shared_from_this();
        reap_timer_.expires_after(kReapPollInterval);
        reap_timer_.async_wait([self](std::error_code ec) {
            if (ec) {
                return;
            }
            if (self->pollReap()) {
                self->completeClose();
            } else {
                self->startReapRetry();
            }
        });
    }

    void storeExit(const ProcessResult& status) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exit_.exit_code = status.exit_code;
            exit_.signalled = status.signalled;
            exit_.signal = status.signal;
            exit_ready_ = true;
            child_exited_ = true;
        }
        if (state_.load(std::memory_order_acquire) != PtyState::Closed) {
            state_.store(PtyState::Exited, std::memory_order_release);
        }
        releaseSlot();
        cv_.notify_all();
        flushLive();
        events_.onPtyExit(session_, id_, exit_);
    }

    void completeCloseIfTerminating() {
        if (state_.load(std::memory_order_acquire) == PtyState::Closed) {
            completeClose();
        }
    }

    void completeClose() {
        state_.store(PtyState::Closed, std::memory_order_release);
        releaseSlot();
        closeDescriptors();
        writer_.finish();
        cv_.notify_all();
    }

    void closeMaster() {
        master_drained_ = true;
        if (master_) {
            master_->close();
            master_.reset();
        }
    }

    void closeDescriptors() {
        if (master_) {
            master_->close();
            master_.reset();
        }
        if (status_) {
            status_->close();
            status_.reset();
        }
        if (pidfd_) {
            pidfd_->close();
            pidfd_.reset();
        }
        reap_timer_.cancel();
        grace_timer_.cancel();
        live_timer_.cancel();
    }

    void releaseSlot() { slot_.reset(); }

    void emitLive(const std::string& chunk) {
        if (state_.load(std::memory_order_acquire) == PtyState::Closed) {
            return;
        }
        live_buf_.append(chunk);
        if (live_buf_.size() >= config_.output_flush_bytes) {
            flushLive();
            return;
        }
        if (live_timer_active_) {
            return;
        }
        live_timer_active_ = true;
        std::weak_ptr<LocalPtySession> self = weak_from_this();
        live_timer_.expires_after(config_.output_flush_interval);
        live_timer_.async_wait([self](std::error_code ec) {
            if (auto session = self.lock()) {
                session->onLiveTimer(ec);
            }
        });
    }

    void onLiveTimer(std::error_code ec) {
        live_timer_active_ = false;
        if (ec) {
            return;
        }
        flushLive();
    }

    void flushLive() {
        if (live_buf_.empty()) {
            return;
        }
        if (state_.load(std::memory_order_acquire) == PtyState::Closed) {
            live_buf_.clear();
            return;
        }
        events_.onPtyOutput(session_, id_, live_buf_);
        live_buf_.clear();
    }

    Executor&         loop_;
    asio::io_context& io_;
    PtyEventSink&     events_;
    ToolConfig        config_;
    PtySessionId      id_;
    SessionId         session_;
    std::string       term_;
    int               rows_;
    int               cols_;

    pid_t pid_ = -1;

    std::mutex              mutex_;
    std::condition_variable cv_;
    PtyOutputRing           ring_;
    std::string             outbound_;
    std::string             write_chunk_;
    bool                    write_in_flight_ = false;
    bool                    child_exited_ = false;
    bool                    exit_ready_ = false;
    bool                    utf8_loss_pending_ = false;
    PtyExit                 exit_;

    std::mutex  reap_mutex_;
    bool        reaped_ = false;

    // Serializes the two teardown entry points (`teardownOnLoop` on the loop
    // thread and `closeFromService` on the caller's), so they cannot both touch
    // the asio descriptors/timers.
    std::mutex  teardown_mutex_;

    std::atomic<PtyState> state_{PtyState::Starting};
    std::atomic<bool>     master_drained_{false};

    std::optional<asio::posix::stream_descriptor> master_;
    std::optional<asio::posix::stream_descriptor> status_;
    std::optional<asio::posix::stream_descriptor> pidfd_;
    asio::steady_timer                            reap_timer_{io_};
    asio::steady_timer                            grace_timer_{io_};
    asio::steady_timer                            live_timer_{io_};
    bool                                          live_timer_active_ = false;
    std::string                                   live_buf_;

    std::array<char, 1>        status_buf_{};
    std::array<char, kReadChunk> read_buf_{};

    StreamWriter<std::string> writer_;
    std::optional<PtySlot>    slot_;
};

// ---- 14 §4.1/§4.4 — the service --------------------------------------------

// The service owns every session; `open` hands the caller a short-lived,
// default-deleter handle over the service-owned session so the pinned
// `Task<std::unique_ptr<PtySession>>` signature is preserved without transferring
// ownership (14 §3.1: "A PtySession is owned by its PtyService, not the tool").
class BorrowedPtySession final : public PtySession {
public:
    explicit BorrowedPtySession(std::shared_ptr<LocalPtySession> inner)
        : inner_(std::move(inner)) {}

    void write(std::string_view bytes) override { inner_->write(bytes); }
    Stream<std::string> output() override { return inner_->output(); }
    void resize(int rows, int cols) override { inner_->resize(rows, cols); }
    void terminate() override { inner_->terminate(); }
    void kill() override { inner_->kill(); }
    PtySessionId id() const noexcept override { return inner_->id(); }
    SessionId session() const noexcept override { return inner_->session(); }
    int pid() const noexcept override { return inner_->pid(); }
    PtyState state() const noexcept override { return inner_->state(); }
    Task<PtyRead> read(std::size_t max_bytes,
                       std::chrono::milliseconds wait,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) override {
        return inner_->read(max_bytes, wait, deadline, cancel);
    }
    Task<PtyExit> wait(std::chrono::milliseconds timeout,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) override {
        return inner_->wait(timeout, deadline, cancel);
    }

private:
    std::shared_ptr<LocalPtySession> inner_;
};

class LocalPtyService::Impl {
public:
    Impl(Executor& loop,
         ResourceGovernor& governor,
         PtyEventSink& events,
         ToolConfig config)
        : loop_(loop), governor_(governor), events_(events), config_(config) {}

    // Invariant: no `LocalPtySession` may use the sink, the governor, or any
    // other service/runtime-owned object once `~Impl` begins. `~Impl` finishes
    // every session — state `Closed`, slot released, child reaped, descriptors
    // closed — before it returns, and drains the handlers its closes cancel, so
    // a session that a queued handler still holds is inert: it can no longer
    // dereference anything owned by the service or the runtime.
    ~Impl() { finishAllSessions(); }

    Task<std::unique_ptr<PtySession>> open(const PtyRequest& request,
                                           CancellationToken cancel) {
        if (request.executable.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments, "empty executable"};
        }
        if (request.argv.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments, "empty argv"};
        }
        if (request.cwd.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments, "empty cwd"};
        }
        if (request.rows <= 0 || request.cols <= 0) {
            throw ToolError{ToolErrorCode::InvalidArguments, "non-positive size"};
        }
        if (loop_.stopped()) {
            throw PtyError{PtyErrorCode::Internal, "pty loop stopped"};
        }
        cancel.throw_if_cancelled();

        auto* asio_executor = dynamic_cast<AsioExecutor*>(&loop_);
        if (asio_executor == nullptr) {
            throw PtyError{PtyErrorCode::Internal, "pty requires the asio loop adapter"};
        }

        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = next_id_++;
        }

        std::shared_ptr<LocalPtySession> session;
        try {
            session = std::make_shared<LocalPtySession>(
                loop_, asio_executor->io(), governor_, events_, config_,
                PtySessionId{id}, request);
        } catch (const PtyError&) {
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[id] = session;
        }

        std::shared_ptr<LocalPtySession> posted = session;
        if (!loop_.post([posted] { posted->startPump(); })) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.erase(id);
            }
            session->closeFromService();
            throw PtyError{PtyErrorCode::Internal, "pty loop stopped"};
        }

        return Task<std::unique_ptr<PtySession>>(
            std::make_unique<BorrowedPtySession>(session));
    }

    PtySession* find(PtySessionId id) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id.value);
        return it == sessions_.end() ? nullptr : it->second.get();
    }

    std::vector<PtySessionId> list(SessionId session) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PtySessionId>   ids;
        for (const auto& [id, entry] : sessions_) {
            if (entry->session() == session) {
                ids.push_back(PtySessionId{id});
            }
        }
        return ids;
    }

    void closeSession(SessionId session) noexcept {
        std::vector<std::shared_ptr<LocalPtySession>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                if (it->second->session() == session) {
                    targets.push_back(it->second);
                    closing_[it->first] = it->second;
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const std::shared_ptr<LocalPtySession>& target : targets) {
            if (loop_.stopped()) {
                target->closeFromService();
            } else if (!loop_.post([target] { target->teardownOnLoop(false); })) {
                target->closeFromService();
            }
        }
        pruneClosed();
    }

    // Lock ordering (no path takes these in reverse, so no deadlock):
    //   Impl::mutex_          — leaf; only around the sessions_/closing_ maps.
    //   session teardown_mutex_ -> reap_mutex_ -> session mutex_.
    // `finishAllSessions` holds no lock while it posts to the loop and waits, so
    // the loop task acquiring those locks cannot deadlock against the caller.
    // Closes every session (active and closing) and returns only once each is
    // finished. `closeFromService` touches asio descriptors and timers, so it
    // runs on the loop thread when one is running; the caller waits for that and
    // then for the handlers the closes cancel, so no session can dispatch after.
    void finishAllSessions() noexcept {
        std::vector<std::shared_ptr<LocalPtySession>> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, entry] : sessions_) {
                (void)id;
                all.push_back(entry);
            }
            for (auto& [id, entry] : closing_) {
                (void)id;
                all.push_back(entry);
            }
            sessions_.clear();
            closing_.clear();
        }
        if (all.empty()) {
            return;
        }
        if (loop_.stopped() || loop_.onLoopThread()) {
            for (const std::shared_ptr<LocalPtySession>& session : all) {
                session->closeFromService();
            }
            return;
        }
        auto              targets = std::make_shared<std::vector<std::shared_ptr<LocalPtySession>>>(
            std::move(all));
        auto              closed        = std::make_shared<std::promise<void>>();
        std::future<void> closed_future = closed->get_future();
        if (!loop_.post([targets, closed] {
                for (const std::shared_ptr<LocalPtySession>& session : *targets) {
                    session->closeFromService();
                }
                closed->set_value();
            })) {
            for (const std::shared_ptr<LocalPtySession>& session : *targets) {
                session->closeFromService();
            }
            return;
        }
        if (closed_future.wait_for(kFinishTimeout) == std::future_status::timeout) {
            for (const std::shared_ptr<LocalPtySession>& session : *targets) {
                session->closeFromService();
            }
        }
        auto              drained        = std::make_shared<std::promise<void>>();
        std::future<void> drained_future = drained->get_future();
        if (loop_.post([drained] { drained->set_value(); })) {
            (void)drained_future.wait_for(kFinishTimeout);
        }
    }

    void closeAll() noexcept { finishAllSessions(); }

    void pruneClosed() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = closing_.begin(); it != closing_.end();) {
            if (it->second->closed()) {
                it = closing_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    Executor&         loop_;
    ResourceGovernor& governor_;
    PtyEventSink&     events_;
    ToolConfig        config_;

    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<LocalPtySession>> sessions_;
    // Sessions removed from `sessions_` by `closeSession` whose asynchronous
    // teardown has not finished; `finishAllSessions` closes them synchronously
    // so they cannot outlive the service.
    std::map<std::uint64_t, std::shared_ptr<LocalPtySession>> closing_;
    std::uint64_t      next_id_ = 1;
};

LocalPtyService::LocalPtyService(Executor& loop,
                                 ResourceGovernor& governor,
                                 PtyEventSink& events,
                                 ToolConfig config)
    : impl_(std::make_unique<Impl>(loop, governor, events, config)) {}

LocalPtyService::~LocalPtyService() = default;

Task<std::unique_ptr<PtySession>> LocalPtyService::open(const PtyRequest& request,
                                                        CancellationToken cancel) {
    return impl_->open(request, cancel);
}

PtySession* LocalPtyService::find(PtySessionId id) noexcept { return impl_->find(id); }

std::vector<PtySessionId> LocalPtyService::list(SessionId session) const {
    return impl_->list(session);
}

void LocalPtyService::closeSession(SessionId session) noexcept {
    impl_->closeSession(session);
}

void LocalPtyService::closeAll() noexcept { impl_->closeAll(); }

} // namespace ymh

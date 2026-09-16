#pragma once

// PTY capability (14 §2-§5): the concrete `PtySession`/`PtyService` seam behind
// `ExecutionEnvironment::pty()` (07 §6.5, §18). PTY is a capability of the
// execution environment, not a tool concern: no tool opens a master fd, forks,
// or reaps (P2). The `terminal` tool drives this seam exactly as `shell` drives
// `ProcessService`.
//
// Interfaces here are pinned by 14 §3/§4 and must not churn (P16). The loop
// adapter (`Executor`) is the marshalling seam (14 §5.1, E-P3); the pump itself
// is realized in `src/execution/pty.cpp` and never leaks Asio into this header.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/executor.hpp"

namespace ymh {

class ResourceGovernor;

// ---- 14 §2.1 — identifiers and value types ---------------------------------

// Daemon-local, monotonic, never durable, never a pid. Minted by the owning
// PtyService. 0 means "invalid".
struct PtySessionId {
    std::uint64_t value = 0;
    auto operator<=>(const PtySessionId&) const = default;
};

enum class PtyState : std::uint8_t {
    Starting,   // forked; awaiting the child's status-pipe exec handshake
    Running,    // child confirmed the slave as its controlling terminal
    Exited,     // child reaped; ring may still hold unread output
    Closed,     // master closed; id no longer resolvable
};

struct PtyExit {
    int  exit_code = -1;
    bool signalled = false;
    int  signal    = 0;
};

struct PtySize {
    int rows = 24;
    int cols = 80;
};

// ---- 14 §2.2 — error taxonomy ----------------------------------------------

enum class PtyErrorCode : std::uint8_t {
    SpawnFailed,    // openpty/forkpty/execve failed (E-F5 analogue)
    NotFound,       // unknown, foreign, or already-Closed PtySessionId
    Closed,         // master closed or slave gone; the session is terminal
    Eio,            // read/write returned EIO (terminal hangup, P-F2)
    WouldBlock,     // bounded outbound queue full (write backpressure, P-F4)
    CapExceeded,    // PTY cap exhausted, global or per-session (P-F6, F8)
    ResizeFailed,   // TIOCSWINSZ failed for a non-fatal reason
    Internal,       // invariant violation / unreachable
};

class PtyError : public std::runtime_error {
public:
    PtyError(PtyErrorCode code, const std::string& what);
    [[nodiscard]] PtyErrorCode code() const noexcept { return code_; }

private:
    PtyErrorCode code_;
};

[[nodiscard]] std::string_view to_string(PtyErrorCode code) noexcept;

// Total PTY → tool-layer translation, called by the `terminal` tool's catch
// block so a known `PtyErrorCode` never degrades to `ToolError{Internal}`
// (14 §2.2, E-F16).
[[nodiscard]] ToolErrorCode to_tool_error_code(PtyErrorCode code) noexcept;

// ---- 14 §3.1 — read result --------------------------------------------------

struct PtyRead {
    std::string data;                // valid UTF-8; ANSI/control bytes preserved
    bool        truncated = false;   // ring evicted older bytes since last read
    bool        eof       = false;   // child exited and ring drained
    bool        cancelled = false;   // wait cancelled: data empty, nothing consumed
};

// ---- 14 §3.3 — the `Stream<T>` model ---------------------------------------

template <class T>
class Stream;
template <class T>
class StreamSubscription;
template <class T>
class StreamWriter;

namespace detail {

template <class T>
struct StreamShared {
    std::mutex                    mutex;
    std::function<void(const T&)> handler;
    std::deque<T>                 pending;
    std::size_t                   capacity = 256;
    std::uint64_t                 generation = 0;
    bool                          finished = false;
};

} // namespace detail

// A lightweight, move-only handle over shared stream state. Exactly one
// consumer may subscribe; registering again replaces the previous handler.
template <class T>
class Stream {
public:
    using Handler = std::function<void(const T&)>;

    Stream() = default;
    Stream(Stream&&) noexcept = default;
    Stream& operator=(Stream&&) noexcept = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    [[nodiscard]] StreamSubscription<T> subscribe(Handler handler);

    [[nodiscard]] bool closed() const noexcept {
        if (!shared_) {
            return true;
        }
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->finished && shared_->pending.empty();
    }

    [[nodiscard]] std::size_t buffered() const noexcept {
        if (!shared_) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->pending.size();
    }

private:
    friend class StreamWriter<T>;
    explicit Stream(std::shared_ptr<detail::StreamShared<T>> shared) noexcept
        : shared_(std::move(shared)) {}

    std::shared_ptr<detail::StreamShared<T>> shared_;
};

// Stream-local RAII handle; NOT `core/event_bus.hpp`'s `Subscription`.
template <class T>
class StreamSubscription {
public:
    StreamSubscription() noexcept = default;

    ~StreamSubscription() { reset(); }

    StreamSubscription(StreamSubscription&& other) noexcept
        : shared_(std::move(other.shared_)),
          generation_(other.generation_),
          active_(other.active_) {
        other.active_ = false;
    }

    StreamSubscription& operator=(StreamSubscription&& other) noexcept {
        if (this != &other) {
            reset();
            shared_     = std::move(other.shared_);
            generation_ = other.generation_;
            active_     = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    StreamSubscription(const StreamSubscription&) = delete;
    StreamSubscription& operator=(const StreamSubscription&) = delete;

    void reset() noexcept {
        if (!active_ || !shared_) {
            active_ = false;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->generation == generation_) {
                shared_->handler = nullptr;
            }
        }
        active_ = false;
    }

    [[nodiscard]] bool active() const noexcept {
        if (!active_ || !shared_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->generation == generation_ &&
               static_cast<bool>(shared_->handler);
    }

private:
    friend class Stream<T>;
    StreamSubscription(std::shared_ptr<detail::StreamShared<T>> shared,
                       std::uint64_t generation) noexcept
        : shared_(std::move(shared)), generation_(generation), active_(true) {}

    std::shared_ptr<detail::StreamShared<T>> shared_;
    std::uint64_t                            generation_ = 0;
    bool                                     active_ = false;
};

// Producer side, owned by PtySession and never handed to the tool.
template <class T>
class StreamWriter {
public:
    StreamWriter() : shared_(std::make_shared<detail::StreamShared<T>>()) {}

    StreamWriter(StreamWriter&&) noexcept = default;
    StreamWriter& operator=(StreamWriter&&) noexcept = default;
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    void push(T value) {
        if (!shared_) {
            return;
        }
        std::function<void(const T&)> local;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (shared_->handler) {
                local = shared_->handler;
            } else {
                if (shared_->pending.size() >= shared_->capacity) {
                    shared_->pending.pop_front();
                }
                shared_->pending.push_back(std::move(value));
                return;
            }
        }
        local(value);
    }

    void finish() {
        if (!shared_) {
            return;
        }
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->finished = true;
    }

    [[nodiscard]] Stream<T> handle() const noexcept { return Stream<T>(shared_); }

private:
    std::shared_ptr<detail::StreamShared<T>> shared_;
};

template <class T>
StreamSubscription<T> Stream<T>::subscribe(Handler handler) {
    if (!shared_) {
        return StreamSubscription<T>{};
    }
    std::deque<T> backlog;
    std::uint64_t generation = 0;
    Handler       local;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->handler = std::move(handler);
        local            = shared_->handler;
        generation       = ++shared_->generation;
        backlog.swap(shared_->pending);
    }
    for (const T& item : backlog) {
        local(item);
    }
    return StreamSubscription<T>(shared_, generation);
}

// ---- 14 §5.2 — the per-PTY bounded ring (E-P4) -----------------------------

// Distinct from 07 §5.2's `OutputRing`: it tracks a FIFO consuming cursor and
// reports eviction since the previous read. Storage discipline matches 07.
class PtyOutputRing {
public:
    explicit PtyOutputRing(std::size_t capacity_bytes);

    void append(std::string_view chunk);

    std::string read(std::size_t max_bytes, bool& truncated);

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool        empty() const noexcept { return bytes_.empty(); }
    void clear() noexcept;

private:
    std::size_t      capacity_;
    std::deque<char> bytes_;
    bool             lost_unread_ = false;
};

// ---- 14 §5.4 — live emission seam ------------------------------------------

class PtyEventSink {
public:
    virtual ~PtyEventSink() = default;

    virtual void onPtyOutput(SessionId, PtySessionId, std::string_view utf8) = 0;
    virtual void onPtyExit(SessionId, PtySessionId, const PtyExit&) = 0;
};

// The M2 default: live `TerminalOutput` has no cross-process wire path (E-P1),
// so the daemon may inject a no-op sink without defect (14 §5.4).
class NoopPtyEventSink final : public PtyEventSink {
public:
    void onPtyOutput(SessionId, PtySessionId, std::string_view) override {}
    void onPtyExit(SessionId, PtySessionId, const PtyExit&) override {}
};

// ---- 14 §3.1 — the `PtySession` interface (pinned) -------------------------

class PtySession {
public:
    virtual ~PtySession() = default;

    virtual void                write(std::string_view) = 0;
    virtual Stream<std::string> output() = 0;
    virtual void                resize(int rows, int cols) = 0;
    virtual void                terminate() = 0;   // SIGHUP -> grace -> SIGKILL

    virtual void         kill() = 0;               // force: no SIGHUP grace
    virtual PtySessionId id() const noexcept = 0;
    virtual SessionId    session() const noexcept = 0;
    virtual int          pid() const noexcept = 0;
    virtual PtyState     state() const noexcept = 0;

    virtual Task<PtyRead> read(std::size_t max_bytes,
                               std::chrono::milliseconds wait,
                               CancellationToken cancel) = 0;

    virtual Task<PtyExit> wait(std::chrono::milliseconds timeout,
                               CancellationToken cancel) = 0;
};

// ---- 14 §4.2 — spawn options -----------------------------------------------

struct PtyRequest {
    std::string              executable;   // absolute or PATH-resolved
    std::vector<std::string> argv;         // argv-first; never a shell string
    std::filesystem::path    cwd;          // MUST be resolve()d under root()

    int rows = 24;
    int cols = 80;

    SessionId                session;      // owning session (caps + routing)
    std::vector<std::pair<std::string, std::string>> environment;  // overlay
    std::string              term{"xterm-256color"};
};

// ---- 14 §4.1 — the `PtyService` seam (pinned) ------------------------------

class PtyService {
public:
    virtual ~PtyService() = default;

    virtual Task<std::unique_ptr<PtySession>> open(const PtyRequest&,
                                                   CancellationToken) = 0;

    virtual PtySession* find(PtySessionId) noexcept = 0;
    virtual std::vector<PtySessionId> list(SessionId) const = 0;
    virtual void closeSession(SessionId) noexcept = 0;
    virtual bool available() const noexcept = 0;
    virtual void closeAll() noexcept = 0;
};

class UnavailablePtyService final : public PtyService {
public:
    Task<std::unique_ptr<PtySession>> open(const PtyRequest&,
                                           CancellationToken) override {
        throw ToolError{ToolErrorCode::Internal, "PTY is not available in v1"};
    }
    PtySession* find(PtySessionId) noexcept override { return nullptr; }
    std::vector<PtySessionId> list(SessionId) const override { return {}; }
    void closeSession(SessionId) noexcept override {}
    bool available() const noexcept override { return false; }
    void closeAll() noexcept override {}
};

// ---- 14 §4.4 — the v1 realization (pimpl keeps Asio out of this header) -----

class LocalPtyService final : public PtyService {
public:
    LocalPtyService(Executor& loop,
                    ResourceGovernor& governor,
                    PtyEventSink& events,
                    ToolConfig config = {});
    ~LocalPtyService() override;

    LocalPtyService(const LocalPtyService&) = delete;
    LocalPtyService& operator=(const LocalPtyService&) = delete;

    Task<std::unique_ptr<PtySession>> open(const PtyRequest&,
                                           CancellationToken) override;
    PtySession* find(PtySessionId) noexcept override;
    std::vector<PtySessionId> list(SessionId) const override;
    void closeSession(SessionId) noexcept override;
    bool available() const noexcept override { return true; }
    void closeAll() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ymh

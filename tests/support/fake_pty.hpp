#pragma once

// Scripted, fork-free PTY doubles for the hermetic `terminal`-tool tests
// (14 §13.2). `FakePtySession` never opens a master fd; `FakePtyService` keeps
// ownership of every session and hands `open` a short-lived forwarding handle,
// mirroring the real service's ownership discipline.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/execution/pty.hpp"

namespace ymh::test {

class FakePtySession;

// Default-deleter handle over a service-owned `FakePtySession`; mirrors
// `BorrowedPtySession` in `src/execution/pty.cpp`.
class FakePtyHandle final : public PtySession {
public:
    explicit FakePtyHandle(std::shared_ptr<FakePtySession> inner)
        : inner_(std::move(inner)) {}

    void write(std::string_view bytes) override;
    Stream<std::string> output() override;
    void resize(int rows, int cols) override;
    void terminate() override;
    void kill() override;
    PtySessionId id() const noexcept override;
    SessionId session() const noexcept override;
    int pid() const noexcept override;
    PtyState state() const noexcept override;
    Task<PtyRead> read(std::size_t max_bytes,
                       std::chrono::milliseconds wait,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) override;
    Task<PtyExit> wait(std::chrono::milliseconds timeout,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) override;

private:
    std::shared_ptr<FakePtySession> inner_;
};

class FakePtySession final : public PtySession,
                             public std::enable_shared_from_this<FakePtySession> {
public:
    FakePtySession(PtySessionId id, SessionId session, int pid)
        : id_(id), session_(session), pid_(pid) {}

    void write(std::string_view bytes) override {
        const PtyState current = state_;
        if (current == PtyState::Exited || current == PtyState::Closed) {
            throw PtyError{PtyErrorCode::Closed, "closed"};
        }
        if (bytes.size() > write_cap) {
            throw PtyError{PtyErrorCode::WouldBlock, "queue full"};
        }
        writes_.append(bytes);
    }

    Stream<std::string> output() { return writer_.handle(); }

    void resize(int rows, int cols) {
        if (rows <= 0 || cols <= 0) {
            throw ToolError{ToolErrorCode::InvalidArguments, "size"};
        }
        resizes_.push_back({rows, cols});
    }

    void terminate() {
        ++terminate_calls;
        state_ = PtyState::Closed;
    }
    void kill() {
        ++kill_calls;
        state_ = PtyState::Closed;
    }

    PtySessionId id() const noexcept { return id_; }
    SessionId    session() const noexcept { return session_; }
    int          pid() const noexcept { return pid_; }
    PtyState     state() const noexcept { return state_; }

    Task<PtyRead> read(std::size_t max_bytes,
                       std::chrono::milliseconds wait,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) {
        (void)wait;
        PtyRead result;
        if (deadline.has_value() && deadline->count() == 0) {
            result.timed_out = true;
            return Task<PtyRead>(std::move(result));
        }
        if (cancel.cancelled()) {
            result.cancelled = true;
            return Task<PtyRead>(std::move(result));
        }
        result.data = scripted_.substr(0, max_bytes);
        scripted_.erase(0, result.data.size());
        result.eof = scripted_eof_ && scripted_.empty();
        return Task<PtyRead>(std::move(result));
    }

    Task<PtyExit> wait(std::chrono::milliseconds timeout,
                       std::optional<std::chrono::milliseconds> deadline,
                       CancellationToken cancel) {
        (void)timeout;
        if (deadline.has_value() && deadline->count() == 0) {
            PtyExit result;
            result.timed_out = true;
            return Task<PtyExit>(result);
        }
        if (cancel.cancelled()) {
            throw CancellationError{};
        }
        return Task<PtyExit>(exit_);
    }

    void script(std::string data, bool eof = false) {
        scripted_ = std::move(data);
        scripted_eof_ = eof;
    }
    void setExit(PtyExit exit) { exit_ = exit; }
    void setState(PtyState state) { state_ = state; }

    std::size_t                  write_cap = 256 * 1024;
    std::string                  writes_;
    std::vector<std::pair<int, int>> resizes_;
    int                          terminate_calls = 0;
    int                          kill_calls = 0;

private:
    PtySessionId              id_;
    SessionId                 session_;
    int                       pid_;
    PtyState                  state_ = PtyState::Running;
    std::string               scripted_;
    bool                      scripted_eof_ = false;
    PtyExit                   exit_{};
    StreamWriter<std::string> writer_;
};

inline void FakePtyHandle::write(std::string_view bytes) { inner_->write(bytes); }
inline Stream<std::string> FakePtyHandle::output() { return inner_->output(); }
inline void FakePtyHandle::resize(int rows, int cols) { inner_->resize(rows, cols); }
inline void FakePtyHandle::terminate() { inner_->terminate(); }
inline void FakePtyHandle::kill() { inner_->kill(); }
inline PtySessionId FakePtyHandle::id() const noexcept { return inner_->id(); }
inline SessionId FakePtyHandle::session() const noexcept { return inner_->session(); }
inline int FakePtyHandle::pid() const noexcept { return inner_->pid(); }
inline PtyState FakePtyHandle::state() const noexcept { return inner_->state(); }
inline Task<PtyRead> FakePtyHandle::read(std::size_t max_bytes,
                                         std::chrono::milliseconds wait,
                                         std::optional<std::chrono::milliseconds> deadline,
                                         CancellationToken cancel) {
    return inner_->read(max_bytes, wait, deadline, cancel);
}
inline Task<PtyExit> FakePtyHandle::wait(std::chrono::milliseconds timeout,
                                         std::optional<std::chrono::milliseconds> deadline,
                                         CancellationToken cancel) {
    return inner_->wait(timeout, deadline, cancel);
}

class FakePtyService final : public PtyService {
public:
    explicit FakePtyService(bool available = true) : available_(available) {}

    Task<std::unique_ptr<PtySession>> open(const PtyRequest& request,
                                           CancellationToken) override {
        ++open_calls;
        if (!available_) {
            throw ToolError{ToolErrorCode::Internal, "PTY is not available"};
        }
        if (fail_with_cap) {
            throw PtyError{PtyErrorCode::CapExceeded, "cap"};
        }
        const PtySessionId id{next_id_++};
        auto session = std::make_shared<FakePtySession>(id, request.session,
                                                        1000 + static_cast<int>(id.value));
        sessions_[id.value] = session;
        return Task<std::unique_ptr<PtySession>>(std::make_unique<FakePtyHandle>(session));
    }

    PtySession* find(PtySessionId id) noexcept override {
        const auto it = sessions_.find(id.value);
        return it == sessions_.end() ? nullptr : it->second.get();
    }

    std::vector<PtySessionId> list(SessionId session) const override {
        std::vector<PtySessionId> ids;
        for (const auto& [id, entry] : sessions_) {
            if (entry->session() == session) {
                ids.push_back(PtySessionId{id});
            }
        }
        return ids;
    }

    void closeSession(SessionId session) noexcept override {
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second->session() == session) {
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool available() const noexcept override { return available_; }

    void closeAll() noexcept override { sessions_.clear(); }

    FakePtySession* at(std::uint64_t id) const {
        const auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second.get();
    }

    bool          available_;
    bool          fail_with_cap = false;
    int           open_calls = 0;
    std::uint64_t next_id_ = 1;

private:
    std::map<std::uint64_t, std::shared_ptr<FakePtySession>> sessions_;
};

} // namespace ymh::test

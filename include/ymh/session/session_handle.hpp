#pragma once

// SessionHandle: the IPC-addressable teardown capability that owns a session's
// write lease (01 §7, 02 §4.8). The abstract shape is frozen by 01; the concrete
// SQLite-backed handle is implemented in session_persistence.cpp.

#include "ymh/core/event.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

class SessionPersistence;

class SessionHandle {
public:
    virtual ~SessionHandle() = default;
    virtual SessionId session() const = 0;
    virtual bool      holdsLease() const noexcept = 0;
    virtual void      release() = 0;
};

class SqliteSessionHandle final : public SessionHandle {
public:
    SqliteSessionHandle(SessionPersistence& store, SessionId id, bool writable);
    ~SqliteSessionHandle() override;

    [[nodiscard]] SessionId session() const override { return id_; }
    [[nodiscard]] bool      holdsLease() const noexcept override {
        return writable_ && !released_;
    }
    void release() override;

private:
    SessionPersistence& store_;
    SessionId           id_;
    bool                writable_;
    bool                released_ = false;
};

} // namespace ymh

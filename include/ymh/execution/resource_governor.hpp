#pragma once

// Resource caps and the per-host ResourceGovernor (00 §9.11, 07 §7, F8). Owned
// by spec 04; consumed here. Tools acquire a subprocess/PTY slot before spawning
// and release it on every path (X11). The governor is the single place a cap is
// enforced, so no tool re-implements one.

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "ymh/core/event.hpp"
#include "ymh/execution/output.hpp"

namespace ymh {

// 04 §2.1. The daemon owns one instance; the supervisor adds only cross-host
// caps.
struct ResourceCaps {
    std::size_t max_llm_concurrency{4};          // bounded LLM worker pool (§35)
    std::size_t max_global_subprocesses{32};     // F8
    std::size_t max_session_subprocesses{8};     // F8
    std::size_t max_global_ptys{8};              // F8
    std::size_t max_session_ptys{2};             // F8
    std::size_t session_output_ring_bytes{1u << 20};   // F5, §20.10
    std::size_t active_output_ring_bytes{4u << 20};    // F5
    std::size_t pty_output_ring_bytes{256u * 1024u};   // E-P4: per-PTY ring
    // 55-A4/55-D7: the global per-daemon fan-out cap on live delegated children.
    std::size_t max_live_subagents{8};
};

// Forward-declared: owned by spec 06 (§5.9) and not used by tools. Declared so
// the pinned shape matches 04 §8 without pulling the LLM layer into execution.
class LLMPool;

class ResourceGovernor {
public:
    explicit ResourceGovernor(ResourceCaps caps = {});

    LLMPool& llm();

    bool tryAcquireSubprocess(SessionId session);
    void releaseSubprocess(SessionId session);
    bool tryAcquirePty(SessionId session);
    void releasePty(SessionId session);

    // 83 §2.2 (A6/A7): returns an owning handle; the map holds one reference too.
    // NEW-2: `owner_alive` is the owning agent's liveness token and is wired to
    // `AgentLoop::disposed_`, so a non-null token reading true means the owner is
    // gone. Read under `mutex_`; when the owner is disposed, return nullptr
    // WITHOUT inserting -- a disposed session is never resurrected by a late
    // call. Null means "always live" (governor-only tests).
    std::shared_ptr<OutputRing> ringFor(SessionId session,
                                        const std::atomic<bool>* owner_alive = nullptr);
    // 83 §2.2: erases the map entry under `mutex_` (`rings_.erase(key)`), dropping
    // only the governor's reference. Idempotent.
    void releaseSession(SessionId session) noexcept;

    [[nodiscard]] const ResourceCaps& caps() const noexcept { return caps_; }

private:
    ResourceCaps caps_;

    std::mutex                                  mutex_;
    std::size_t                                 global_subprocesses_ = 0;
    std::size_t                                 global_ptys_ = 0;
    std::unordered_map<std::string, std::size_t> session_subprocesses_;
    std::unordered_map<std::string, std::size_t> session_ptys_;
    std::unordered_map<std::string, std::shared_ptr<OutputRing>> rings_;
};

// RAII slot guards (X11): release on completion, timeout, cancellation, throw.
class SubprocessSlot {
public:
    SubprocessSlot(ResourceGovernor& governor, SessionId session);
    ~SubprocessSlot();

    SubprocessSlot(const SubprocessSlot&) = delete;
    SubprocessSlot& operator=(const SubprocessSlot&) = delete;

    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    ResourceGovernor& governor_;
    SessionId         session_;
    bool              held_ = false;
};

class PtySlot {
public:
    PtySlot(ResourceGovernor& governor, SessionId session);
    ~PtySlot();

    PtySlot(const PtySlot&) = delete;
    PtySlot& operator=(const PtySlot&) = delete;

    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    ResourceGovernor& governor_;
    SessionId         session_;
    bool              held_ = false;
};

} // namespace ymh

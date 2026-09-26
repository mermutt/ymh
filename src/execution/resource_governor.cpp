#include "ymh/execution/resource_governor.hpp"

namespace ymh {

ResourceGovernor::ResourceGovernor(ResourceCaps caps) : caps_(caps) {}

bool ResourceGovernor::tryAcquireSubprocess(SessionId session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (global_subprocesses_ >= caps_.max_global_subprocesses) {
        return false;
    }
    std::size_t& per_session = session_subprocesses_[session.value];
    if (per_session >= caps_.max_session_subprocesses) {
        return false;
    }
    ++global_subprocesses_;
    ++per_session;
    return true;
}

void ResourceGovernor::releaseSubprocess(SessionId session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (global_subprocesses_ > 0) {
        --global_subprocesses_;
    }
    auto it = session_subprocesses_.find(session.value);
    if (it != session_subprocesses_.end() && it->second > 0) {
        --it->second;
    }
}

bool ResourceGovernor::tryAcquirePty(SessionId session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (global_ptys_ >= caps_.max_global_ptys) {
        return false;
    }
    std::size_t& per_session = session_ptys_[session.value];
    if (per_session >= caps_.max_session_ptys) {
        return false;
    }
    ++global_ptys_;
    ++per_session;
    return true;
}

void ResourceGovernor::releasePty(SessionId session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (global_ptys_ > 0) {
        --global_ptys_;
    }
    auto it = session_ptys_.find(session.value);
    if (it != session_ptys_.end() && it->second > 0) {
        --it->second;
    }
}

OutputRing& ResourceGovernor::ringFor(SessionId session) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& ring = rings_[session.value];
    if (!ring) {
        ring = std::make_unique<OutputRing>(caps_.session_output_ring_bytes);
    }
    return *ring;
}

SubprocessSlot::SubprocessSlot(ResourceGovernor& governor, SessionId session)
    : governor_(governor), session_(std::move(session)) {
    held_ = governor_.tryAcquireSubprocess(session_);
}

SubprocessSlot::~SubprocessSlot() {
    if (held_) {
        governor_.releaseSubprocess(session_);
    }
}

PtySlot::PtySlot(ResourceGovernor& governor, SessionId session)
    : governor_(governor), session_(std::move(session)) {
    held_ = governor_.tryAcquirePty(session_);
}

PtySlot::~PtySlot() {
    if (held_) {
        governor_.releasePty(session_);
    }
}

} // namespace ymh

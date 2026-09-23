#include "ymh/session/session_manager.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ymh {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path canonical_workspace(const std::filesystem::path& cwd) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(cwd, error);
    if (error) {
        throw std::invalid_argument("workspace root is not canonicalizable: " + cwd.string());
    }
    validateWorkspaceRoot(canonical);
    return canonical;
}

} // namespace

SessionManager::SessionManager(SessionStore& store, EventBus& bus)
    : store_(&store), bus_(&bus) {}

Session& SessionManager::loadInto(const SessionHeader& header) {
    // Requires mutex_. 24-D13: one resident Session per id. Returning the
    // resident object unchanged (rather than replacing it) keeps the strong
    // session_owner_ an AgentLoop may already hold pointed at the same object.
    if (const auto it = sessions_.find(header.id.value); it != sessions_.end()) {
        return *it->second;
    }
    auto session = std::make_shared<Session>(Session::resume(header, *store_, *bus_));
    Session& reference = *session;
    sessions_[header.id.value] = std::move(session);
    return reference;
}

SessionId SessionManager::createSession(const SessionOptions& options) {
    const std::filesystem::path cwd = canonical_workspace(options.cwd);

    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = cwd;
    header.createdAt     = now_ms();
    header.updatedAt     = header.createdAt;
    header.title         = options.title;
    header.model         = options.model;
    if (!options.model_name.empty()) {
        header.model_name = options.model_name;
    }
    header.serverProfile = options.serverProfile;
    header.kind          = options.kind;
    header.parentSession = options.parentSession;
    header.depth         = options.depth;
    header.agent_preset  = options.agent_preset;
    header.permission_preset = options.permission_preset;
    validateHeader(header);

    store_->create(header);

    std::lock_guard<std::mutex> lock(mutex_);
    Session& session = loadInto(header);
    session.append(payload::SessionStarted{
        .model         = header.model,
        .serverProfile = header.serverProfile,
        .title         = header.title,
        .model_name    = header.model_name.value_or(""),
    });
    return header.id;
}

SessionId SessionManager::resumeSession(const SessionId& id) {
    const auto header = store_->load(id);
    if (!header.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    loadInto(*header);
    return id;
}

SessionId SessionManager::forkSession(const SessionId& parent, std::size_t seedLength) {
    std::lock_guard<std::mutex> lock(mutex_);

    Session* parentSession = nullptr;
    if (const auto it = sessions_.find(parent.value); it != sessions_.end()) {
        parentSession = it->second.get();
    } else {
        const auto header = store_->load(parent);
        if (!header.has_value()) {
            throw UnknownSession("unknown parent session: " + parent.value);
        }
        parentSession = &loadInto(*header);
    }

    Session child = Session::fork(*parentSession, seedLength, *store_, *bus_);
    const SessionId childId = child.id();
    sessions_[childId.value] = std::make_shared<Session>(std::move(child));
    return childId;
}

SessionId SessionManager::replaySession(const SessionId& id) {
    const auto header = store_->load(id);
    if (!header.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    loadInto(*header);
    return id;
}

Sequence SessionManager::renameSession(const SessionId& id, std::string title) {
    const std::string normalized = normalize_title(title);

    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(id.value) == sessions_.end()) {
        const auto header = store_->load(id);
        if (!header.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        loadInto(*header);
    }
    return session(id).append(
        payload::SessionRenamed{std::move(normalized), payload::RenameOrigin::User});
}

std::optional<Sequence> SessionManager::maybeAutoName(const SessionId& id,
                                                      std::string_view firstUserText) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(id.value) == sessions_.end()) {
        const auto header = store_->load(id);
        if (!header.has_value()) {
            return std::nullopt;
        }
        loadInto(*header);
    }
    return session(id).appendAutoRename(firstUserText);
}

std::shared_ptr<Session> SessionManager::sessionPtr(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id.value);
    if (it == sessions_.end()) {
        throw UnknownSession("session is not loaded: " + id.value);
    }
    return it->second;
}

void SessionManager::closeSession(const SessionId& id) {
    // Park: erase the manager's owning reference. The erased `shared_ptr` is
    // moved out under the lock so ~Session (if this was the last reference)
    // runs after mutex_ is released.
    std::shared_ptr<Session> erased;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id.value);
        if (it == sessions_.end()) {
            return;
        }
        erased = std::move(it->second);
        sessions_.erase(it);
    }
}

void SessionManager::deleteSession(const SessionId& id, bool only_if_empty) {
    if (!store_->load(id).has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    if (only_if_empty && !store_->isUnprompted(id)) {
        throw std::invalid_argument("session is not empty: " + id.value);
    }

    TypedEvent<payload::SessionEnded> typed;
    typed.id         = make_event_id();
    typed.session_id = id;
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload::SessionEnded{payload::SessionEndReason::Deleted};
    Event ended      = encode(typed);

    const Sequence seq = store_->eraseWithEvent(id, ended);

    // mutex_ is held across the erase + publish so a concurrent resumeSession
    // cannot resurrect a deleted session (24-D11). The erased owning handle is
    // moved out so ~Session runs after the lock is released. 24-D6: publish the
    // terminal event as a committed record carrying the store sequence, so the
    // forwarder sees it even though the row was erased in the same transaction.
    std::shared_ptr<Session> erased;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id.value);
        if (it != sessions_.end()) {
            erased = std::move(it->second);
            sessions_.erase(it);
        }
        bus_->publishCommitted(EventRecord{seq, ended});
    }
}

std::vector<SessionId> SessionManager::list() const {
    std::vector<SessionId> ids;
    for (const SessionHeader& header : store_->list()) {
        ids.push_back(header.id);
    }
    return ids;
}

Session& SessionManager::session(const SessionId& id) {
    // Private: the caller holds mutex_ or a strong sessionPtr, so this raw map
    // lookup cannot race a concurrent erase.
    const auto it = sessions_.find(id.value);
    if (it == sessions_.end()) {
        throw UnknownSession("session is not loaded: " + id.value);
    }
    return *it->second;
}

} // namespace ymh

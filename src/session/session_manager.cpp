#include "ymh/session/session_manager.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ymh/session/errors.hpp"

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
    auto session = std::make_unique<Session>(Session::resume(header, *store_, *bus_));
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
    header.serverProfile = options.serverProfile;
    header.kind          = SessionKind::Root;
    validateHeader(header);

    store_->create(header);

    auto session = std::make_unique<Session>(Session::resume(header, *store_, *bus_));
    session->append(payload::SessionStarted{
        .model         = header.model,
        .serverProfile = header.serverProfile,
        .title         = header.title,
    });
    sessions_[header.id.value] = std::move(session);
    return header.id;
}

SessionId SessionManager::resumeSession(const SessionId& id) {
    const auto header = store_->load(id);
    if (!header.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    loadInto(*header);
    return id;
}

SessionId SessionManager::forkSession(const SessionId& parent, std::size_t seedLength) {
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
    sessions_[childId.value] = std::make_unique<Session>(std::move(child));
    return childId;
}

SessionId SessionManager::replaySession(const SessionId& id) {
    const auto header = store_->load(id);
    if (!header.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    loadInto(*header);
    return id;
}

void SessionManager::closeSession(const SessionId& id) {
    sessions_.erase(id.value);
}

void SessionManager::deleteSession(const SessionId& id) {
    if (const auto it = sessions_.find(id.value); it != sessions_.end()) {
        Session& session = *it->second;
        session.append(payload::SessionEnded{payload::SessionEndReason::Deleted});
        sessions_.erase(it);
    } else {
        const auto header = store_->load(id);
        if (!header.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        Session session = Session::resume(*header, *store_, *bus_);
        session.append(payload::SessionEnded{payload::SessionEndReason::Deleted});
    }
    store_->erase(id);
}

std::vector<SessionId> SessionManager::list() const {
    std::vector<SessionId> ids;
    for (const SessionHeader& header : store_->list()) {
        ids.push_back(header.id);
    }
    return ids;
}

Session& SessionManager::session(const SessionId& id) {
    const auto it = sessions_.find(id.value);
    if (it == sessions_.end()) {
        throw UnknownSession("session is not loaded: " + id.value);
    }
    return *it->second;
}

void SessionManager::setAgentLookup(AgentLookup lookup) {
    agent_lookup_ = std::move(lookup);
}

Agent* SessionManager::findAgent(const SessionId& id) const {
    return agent_lookup_ ? agent_lookup_(id) : nullptr;
}

Agent& SessionManager::agent(const SessionId& id) const {
    Agent* found = findAgent(id);
    if (found == nullptr) {
        throw UnknownSession("no agent registered for session: " + id.value);
    }
    return *found;
}

} // namespace ymh

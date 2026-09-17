#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/transport/host.hpp"

namespace ymh::test {

class FakeTransportHost final : public protocol::TransportHost {
public:
    FakeTransportHost() {
        workspace.value = "11111111-1111-4111-8111-111111111111";
        boot_id.value = "22222222-2222-4222-8222-222222222222";
    }

    protocol::HostState state{protocol::HostState::Serving};
    protocol::WorkspaceId workspace;
    protocol::HostBootId boot_id;
    protocol::HostPid pid{4242};
    std::optional<SessionId> active_session;

    std::vector<std::string> calls;
    std::optional<nlohmann::json> last_message;
    std::optional<nlohmann::json> context_result;
    std::optional<std::string> last_reason;
    std::optional<protocol::ShutdownReason> last_shutdown_reason;
    std::shared_ptr<const std::vector<protocol::ClientInstanceId>> owner_snapshot{
        std::make_shared<const std::vector<protocol::ClientInstanceId>>()};
    std::set<std::string> known_permissions;

    // Test seam (D14): when set to a method name, the corresponding host method
    // throws a non-RpcException `std::runtime_error` before doing any work. Used
    // to assert that an unexpected internal failure maps to
    // `RpcCode::InternalError`, not `InvalidParams`.
    std::optional<std::string> throw_internal_on;

    protocol::HostState hostState() const override { return state; }

    protocol::HostStatusInfo hostStatus() const override {
        protocol::HostStatusInfo info;
        info.state = state;
        info.workspace = workspace;
        info.boot_id = boot_id;
        info.pid = pid;
        info.active_session = active_session;
        return info;
    }

    void requestShutdown(protocol::ShutdownReason reason) override {
        calls.push_back("host.shutdown");
        last_shutdown_reason = reason;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<protocol::ClientInstanceId>>
    freshOwnerSnapshot() const override {
        return owner_snapshot;
    }

    std::vector<protocol::WorkspaceSummary> listWorkspaces() override {
        protocol::WorkspaceSummary summary;
        summary.id = workspace;
        summary.canonical_path = "/workspace";
        summary.display_title = "workspace";
        return {summary};
    }

    protocol::WorkspaceDetail showWorkspace(const protocol::WorkspaceId& id) override {
        protocol::WorkspaceDetail detail;
        detail.summary.id = id;
        detail.summary.canonical_path = "/workspace";
        detail.summary.display_title = "workspace";
        return detail;
    }

    std::vector<protocol::SessionSummary> listSessions() override {
        if (throw_internal_on == "session.list") {
            throw std::runtime_error("injected internal failure");
        }
        std::vector<protocol::SessionSummary> summaries;
        for (const auto& entry : logs_) {
            summaries.push_back(summary_for(entry.first));
        }
        return summaries;
    }

    protocol::SessionDetail showSession(const SessionId& id) override {
        if (!sessionExists(id)) {
            throw protocol::RpcException(static_cast<int>(protocol::AppCode::UnknownSession),
                                         "unknown session");
        }
        protocol::SessionDetail detail;
        detail.summary = summary_for(id.value);
        detail.header = nlohmann::json{{"id", id.value}};
        detail.event_count = logs_.at(id.value).size();
        return detail;
    }

    nlohmann::json showContext(const SessionId& id) override {
        calls.push_back("context.show");
        if (!sessionExists(id)) {
            throw protocol::RpcException(static_cast<int>(protocol::AppCode::UnknownSession),
                                         "unknown session");
        }
        if (context_result.has_value()) {
            return *context_result;
        }
        return nlohmann::json{{"session", id.value},
                              {"captured_sequence", 0},
                              {"used_tokens", 0},
                              {"budget",
                               {{"window_tokens", 0},
                                {"reserve_output_tokens", 0},
                                {"effective_threshold_tokens", 0}}},
                              {"segments", nlohmann::json::array()},
                              {"tools", nlohmann::json::array()},
                              {"mcp_servers", nlohmann::json::array()},
                              {"truncated", false},
                              {"note", ""}};
    }

    protocol::SessionCreated createSession(const nlohmann::json& params) override {
        calls.push_back("session.create");
        const std::string id = "session-" + std::to_string(next_session_++);
        logs_[id] = {};
        titles_[id] = params.value("title", std::string{"untitled"});
        return protocol::SessionCreated{SessionId{id}, nlohmann::json{{"id", id}}};
    }

    protocol::SessionResumed resumeSession(const SessionId& id) override {
        calls.push_back("session.resume");
        if (!sessionExists(id)) {
            throw protocol::RpcException(static_cast<int>(protocol::AppCode::UnknownSession),
                                         "unknown session");
        }
        return protocol::SessionResumed{id, "Idle"};
    }

    protocol::SessionCreated forkSession(const SessionId& id, std::int64_t seed_length) override {
        calls.push_back("session.fork");
        if (!sessionExists(id)) {
            throw protocol::RpcException(static_cast<int>(protocol::AppCode::UnknownSession),
                                         "unknown session");
        }
        const std::string child = "fork-" + std::to_string(next_session_++);
        std::vector<EventRecord> seeded;
        const auto& parent = logs_.at(id.value);
        for (std::size_t index = 0; index < parent.size() &&
                                    static_cast<std::int64_t>(index) < seed_length;
             ++index) {
            EventRecord record = parent[index];
            record.seq = static_cast<Sequence>(index) + 1;
            record.event.id.value = "e" + std::to_string(record.seq);
            record.event.session_id = SessionId{child};
            seeded.push_back(record);
        }
        logs_[child] = std::move(seeded);
        return protocol::SessionCreated{SessionId{child}, nlohmann::json{{"id", child}}};
    }

    void closeSession(const SessionId& id) override {
        calls.push_back("session.close");
        require_session(id);
    }

    void deleteSession(const SessionId& id) override {
        calls.push_back("session.delete");
        require_session(id);
        logs_.erase(id.value);
        titles_.erase(id.value);
    }

    void activateSession(const SessionId& id) override {
        calls.push_back("session.activate");
        require_session(id);
        active_session = id;
    }

    void suspendSession(const SessionId& id) override {
        calls.push_back("session.suspend");
        require_session(id);
        active_session.reset();
    }

    void compactSession(const SessionId& id) override {
        calls.push_back("session.compact");
        require_session(id);
    }

    protocol::SessionRenamedResult renameSession(const nlohmann::json& params) override {
        calls.push_back("session.rename");
        const SessionId id{params.value("session", std::string{})};
        const std::string title = params.value("title", std::string{});
        require_session(id);
        titles_[id.value] = title;
        return protocol::SessionRenamedResult{id, title};
    }

    void agentPrompt(const SessionId& id, const nlohmann::json& message) override {
        calls.push_back("agent.prompt");
        require_session(id);
        last_message = message;
    }

    void agentFollowup(const SessionId& id, const nlohmann::json& message) override {
        calls.push_back("agent.followup");
        require_session(id);
        last_message = message;
    }

    void agentSteer(const SessionId& id, const nlohmann::json& message) override {
        calls.push_back("agent.steer");
        require_session(id);
        last_message = message;
    }

    void agentInject(const SessionId& id, const nlohmann::json& context) override {
        calls.push_back("agent.inject");
        require_session(id);
        last_message = context;
    }

    bool agentCancel(const SessionId& id, const std::optional<std::string>& reason) override {
        calls.push_back("agent.cancel");
        require_session(id);
        last_reason = reason;
        return true;
    }

    std::string agentStatus(const SessionId& id) override {
        calls.push_back("agent.status");
        require_session(id);
        return "Idle";
    }

    bool decidePermission(const std::string& request_id, protocol::PermissionAnswer,
                          protocol::PermissionScope) override {
        calls.push_back("permission.decide");
        return known_permissions.count(request_id) > 0;
    }

    nlohmann::json listSkills() override {
        calls.push_back("skills.list");
        return skills_list;
    }

    nlohmann::json showSkill(const std::string& name) override {
        calls.push_back("skills.show");
        const auto it = skill_bodies.find(name);
        if (it == skill_bodies.end()) {
            throw protocol::RpcException(protocol::code_value(protocol::RpcCode::InvalidParams),
                                         "unknown skill");
        }
        return it->second;
    }

    nlohmann::json                        skills_list = nlohmann::json::object();
    std::map<std::string, nlohmann::json> skill_bodies;

    bool sessionExists(const SessionId& id) const override {
        return logs_.find(id.value) != logs_.end();
    }

    std::vector<EventRecord> readEvents(const SessionId& id, Sequence after,
                                        std::size_t limit) override {
        std::vector<EventRecord> result;
        const auto it = logs_.find(id.value);
        if (it == logs_.end()) {
            return result;
        }
        for (const EventRecord& record : it->second) {
            if (record.seq > after) {
                result.push_back(record);
                if (result.size() >= limit) {
                    break;
                }
            }
        }
        return result;
    }

    Sequence headSequence(const SessionId& id) const override {
        const auto it = logs_.find(id.value);
        if (it == logs_.end() || it->second.empty()) {
            return 0;
        }
        return it->second.back().seq;
    }

    std::optional<Sequence> resolveCursor(const SessionId& id,
                                          const protocol::EventCursor& cursor) const override {
        const std::string prefix = "c1:" + id.value + ":";
        if (cursor.value.rfind(prefix, 0) != 0) {
            return std::nullopt;
        }
        Sequence sequence = 0;
        try {
            sequence = std::stoll(cursor.value.substr(prefix.size()));
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (sequence < 0 || sequence > headSequence(id)) {
            return std::nullopt;
        }
        return sequence;
    }

    protocol::EventCursor cursorFor(const SessionId& id, Sequence sequence) const override {
        return protocol::EventCursor{"c1:" + id.value + ":" + std::to_string(sequence)};
    }

    SessionId seed(const std::string& id) {
        logs_[id] = {};
        return SessionId{id};
    }

    EventRecord append(const SessionId& id, EventType type, nlohmann::json payload = {}) {
        auto& log = logs_[id.value];
        const Sequence sequence = log.empty() ? 1 : log.back().seq + 1;
        Event event;
        event.id.value = "e" + std::to_string(sequence);
        event.session_id = id;
        event.timestamp = std::chrono::system_clock::time_point{
            std::chrono::milliseconds{1700000000000 + sequence}};
        event.type = type;
        event.payload = std::move(payload);
        log.push_back(EventRecord{sequence, event});
        return log.back();
    }

private:
    protocol::SessionSummary summary_for(const std::string& id) const {
        protocol::SessionSummary summary;
        summary.id = SessionId{id};
        summary.title = titles_.count(id) != 0 ? titles_.at(id) : id;
        summary.kind = "root";
        return summary;
    }

    void require_session(const SessionId& id) const {
        if (!sessionExists(id)) {
            throw protocol::RpcException(static_cast<int>(protocol::AppCode::UnknownSession),
                                         "unknown session");
        }
    }

    std::map<std::string, std::vector<EventRecord>> logs_;
    std::map<std::string, std::string> titles_;
    std::uint64_t next_session_{1};
};

} // namespace ymh::test

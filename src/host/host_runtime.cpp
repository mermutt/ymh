#include "ymh/host/host_runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/turn_executor.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/transport/protocol_server.hpp"

namespace ymh {
namespace {

// The daemon's bounded replay batch (errata §4.3.2; protocol_server.cpp's
// kReplayBatch is file-local, so the bound is restated here).
constexpr std::size_t kMaxReplayBatch = 256;

constexpr std::string_view kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::string_view kCursorPrefix = "s1:";

std::string base64url_encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= input.size()) {
        const std::uint32_t block = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index])) << 16) |
                                    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index + 1])) << 8) |
                                    static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index + 2]));
        out.push_back(kBase64UrlAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 6) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[block & 0x3F]);
        index += 3;
    }
    const std::size_t remaining = input.size() - index;
    if (remaining == 1) {
        const std::uint32_t block =
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index])) << 16;
        out.push_back(kBase64UrlAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 12) & 0x3F]);
    } else if (remaining == 2) {
        const std::uint32_t block =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(input[index + 1])) << 8);
        out.push_back(kBase64UrlAlphabet[(block >> 18) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 12) & 0x3F]);
        out.push_back(kBase64UrlAlphabet[(block >> 6) & 0x3F]);
    }
    return out;
}

std::optional<std::string> base64url_decode(std::string_view input) {
    static const std::array<std::int16_t, 256> table = [] {
        std::array<std::int16_t, 256> decoded{};
        decoded.fill(-1);
        for (std::size_t index = 0; index < kBase64UrlAlphabet.size(); ++index) {
            decoded[static_cast<std::uint8_t>(kBase64UrlAlphabet[index])] =
                static_cast<std::int16_t>(index);
        }
        return decoded;
    }();

    std::string out;
    out.reserve((input.size() * 3) / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char character : input) {
        const std::int16_t value = table[static_cast<std::uint8_t>(character)];
        if (value < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    if (bits >= 6) {
        return std::nullopt;
    }
    if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0) {
        return std::nullopt;
    }
    return out;
}

Message message_from_json(const nlohmann::json& value) {
    if (value.is_string()) {
        Message message;
        message.role = Role::User;
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = value.get<std::string>();
        message.content.push_back(std::move(block));
        return message;
    }
    return value.get<Message>();
}

ContextMessage context_from_json(const nlohmann::json& value) {
    ContextMessage context;
    context.role = Role::User;
    if (value.is_string()) {
        context.text = value.get<std::string>();
        return context;
    }
    if (value.is_object()) {
        if (const auto role = value.find("role"); role != value.end() && role->is_string()) {
            if (const std::optional<Role> parsed = parse_role(role->get<std::string>());
                parsed.has_value()) {
                context.role = *parsed;
            }
        }
        context.text = value.value("text", std::string{});
        context.startsTurn = value.value("starts_turn", false);
    }
    return context;
}

protocol::SessionSummary summary_for(const SessionHeader& header, const SessionOrderEntry& entry) {
    protocol::SessionSummary summary;
    summary.id = header.id;
    summary.ordinal = entry.ordinal;
    summary.archived = entry.archived;
    summary.title = header.title;
    summary.kind = std::string(session_kind_name(header.kind));
    summary.updated_at_ms = header.updatedAt;
    return summary;
}

protocol::WorkspaceSummary workspace_summary(const WorkspaceRecord& record) {
    protocol::WorkspaceSummary summary;
    summary.id = protocol::WorkspaceId{record.id.value};
    summary.canonical_path = record.canonicalPath.string();
    summary.display_title = record.displayTitle;
    if (record.host.has_value()) {
        summary.host_pid = record.host->pid;
        summary.boot_id = protocol::HostBootId{record.host->bootId.value};
    }
    return summary;
}

} // namespace

HostRuntime::HostRuntime(WorkspaceRuntime& runtime,
                         WorkspaceRegistry& registry,
                         HostIdentity identity,
                         protocol::ProtocolServer& server,
                         TurnExecutor& turns,
                         PermissionBroker& broker,
                         EventForwarder forwarder)
    : runtime_(runtime),
      registry_(registry),
      identity_(std::move(identity)),
      server_(&server),
      turns_(turns),
      broker_(broker),
      forwarder_(std::move(forwarder)) {
    wireServer();
    startForwarding();
}

HostRuntime::HostRuntime(WorkspaceRuntime& runtime,
                         WorkspaceRegistry& registry,
                         HostIdentity identity,
                         TurnExecutor& turns,
                         PermissionBroker& broker,
                         EventForwarder forwarder)
    : runtime_(runtime),
      registry_(registry),
      identity_(std::move(identity)),
      turns_(turns),
      broker_(broker),
      forwarder_(std::move(forwarder)) {
    startForwarding();
}

HostRuntime::~HostRuntime() {
    broker_.set_subscriber_count({});
    forward_subscription_.unsubscribe();
}

void HostRuntime::attachServer(protocol::ProtocolServer& server) {
    server_ = &server;
    wireServer();
}

void HostRuntime::wireServer() {
    if (server_ == nullptr) {
        return;
    }
    broker_.set_subscriber_count(
        [this](SessionId session) { return subscriberCount(session); });
    server_->set_subscribe_observer([this](const SessionId& session, protocol::ClientId client) {
        onSubscriberAttached(session, client);
    });
}

void HostRuntime::startForwarding() {
    if (forwarding_) {
        return;
    }
    forwarding_ = true;
    forward_subscription_ = runtime_.bus().subscribe(
        [this](const Event& event) { handleCommittedEvent(event); });
}

void HostRuntime::setShutdownHook(std::function<void(std::string)> hook) {
    shutdown_hook_ = std::move(hook);
}

void HostRuntime::setState(protocol::HostState state) noexcept {
    state_ = state;
}

void HostRuntime::publishSubscriberCount(const SessionId& session, std::size_t count) {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    subscriber_counts_[session.value] = count;
}

void HostRuntime::refreshSubscriberCount(const SessionId& session) {
    if (server_ == nullptr) {
        return;
    }
    publishSubscriberCount(session, server_->sessionSubscriberCount(session));
}

void HostRuntime::onSubscriberAttached(const SessionId& session, protocol::ClientId) {
    refreshSubscriberCount(session);
    if (server_ == nullptr) {
        return;
    }
    for (const protocol::PermissionRequest& request : broker_.pending(session)) {
        server_->onPermissionRequest(request);
    }
}

std::size_t HostRuntime::subscriberCount(const SessionId& session) const {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    const auto found = subscriber_counts_.find(session.value);
    return found == subscriber_counts_.end() ? 0 : found->second;
}

void HostRuntime::handleCommittedEvent(const Event& event) {
    const SessionId session = event.session_id;
    EventRecord record;
    {
        std::lock_guard<std::mutex> lock(forward_mutex_);
        const Sequence after = last_forwarded_[session.value];
        EventRange tail;
        try {
            tail = runtime_.store().readAfter(session, after, 1);
        } catch (const std::exception&) {
            return;
        }
        if (tail.empty() || tail.front().event.id.value != event.id.value) {
            return;
        }
        record = tail.front();
        last_forwarded_[session.value] = record.seq;
    }
    if (forwarder_) {
        forwarder_(record);
    } else if (server_ != nullptr) {
        server_->onEventCommitted(record);
    }
}

void HostRuntime::ensureAgent(const SessionId& id) {
    if (runtime_.agents().find(id) != nullptr) {
        return;
    }
    std::expected<AgentId, AgentError> resumed = runtime_.agents().resume(id);
    if (!resumed.has_value()) {
        throw_mapped(map_agent_error(resumed.error()));
    }
}

void HostRuntime::acquireLeaseOrThrow(const SessionId& id) {
    if (runtime_.persistence() == nullptr) {
        return;
    }
    if (!runtime_.acquireLease(id)) {
        throw_mapped(WireError{protocol::code_value(protocol::AppCode::LeaseLost), "LeaseLost"});
    }
}

void HostRuntime::releaseLeaseIfDurable(const SessionId& id) {
    if (runtime_.persistence() != nullptr) {
        runtime_.releaseLease(id);
    }
}

protocol::HostState HostRuntime::hostState() const {
    return state_;
}

protocol::HostStatusInfo HostRuntime::hostStatus() const {
    protocol::HostStatusInfo info;
    info.state = state_;
    info.workspace = protocol::WorkspaceId{identity_.workspace.value};
    info.boot_id = protocol::HostBootId{identity_.boot_id.value};
    info.pid = identity_.pid;
    info.active_session = active_session_;
    return info;
}

void HostRuntime::requestShutdown(std::string reason) {
    state_ = protocol::HostState::Draining;
    if (shutdown_hook_) {
        shutdown_hook_(std::move(reason));
    }
}

std::vector<protocol::WorkspaceSummary> HostRuntime::listWorkspaces() {
    return translate([&]() -> std::vector<protocol::WorkspaceSummary> {
        std::vector<protocol::WorkspaceSummary> summaries;
        for (const WorkspaceRecord& record : registry_.listWorkspaces()) {
            summaries.push_back(workspace_summary(record));
        }
        return summaries;
    });
}

protocol::WorkspaceDetail HostRuntime::showWorkspace(const protocol::WorkspaceId& id) {
    return translate([&]() -> protocol::WorkspaceDetail {
        const std::optional<WorkspaceRecord> record = registry_.findById(WorkspaceId{id.value});
        if (!record.has_value()) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownWorkspace),
                                   "UnknownWorkspace"});
        }
        protocol::WorkspaceDetail detail;
        detail.summary = workspace_summary(*record);
        detail.created_at_ms = record->createdAt;
        detail.updated_at_ms = record->updatedAt;
        return detail;
    });
}

std::vector<protocol::SessionSummary> HostRuntime::listSessions() {
    return translate([&]() -> std::vector<protocol::SessionSummary> {
        std::vector<SessionRef> refs;
        for (const SessionHeader& header : runtime_.store().list()) {
            refs.push_back(SessionRef{header.id, header.createdAt});
        }
        const std::vector<SessionOrderEntry> ordered =
            registry_.listSessionsOrdered(identity_.workspace, refs);
        std::vector<protocol::SessionSummary> summaries;
        summaries.reserve(ordered.size());
        for (const SessionOrderEntry& entry : ordered) {
            const std::optional<SessionHeader> header = runtime_.store().load(entry.sessionId);
            if (!header.has_value()) {
                continue;
            }
            summaries.push_back(summary_for(*header, entry));
        }
        return summaries;
    });
}

protocol::SessionDetail HostRuntime::showSession(const SessionId& id) {
    return translate([&]() -> protocol::SessionDetail {
        const std::optional<SessionHeader> header = runtime_.store().load(id);
        if (!header.has_value()) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        SessionOrderEntry entry;
        entry.sessionId = id;
        if (const std::optional<WorkspaceSessionRecord> record =
                registry_.findSession(identity_.workspace, id);
            record.has_value()) {
            entry.ordinal = record->ordinal;
            entry.archived = record->archived;
        }
        protocol::SessionDetail detail;
        detail.summary = summary_for(*header, entry);
        detail.header = nlohmann::json(*header);
        detail.event_count = runtime_.store().read(id, 0).size();
        return detail;
    });
}

protocol::SessionCreated HostRuntime::createSession(const nlohmann::json& params) {
    return translate([&]() -> protocol::SessionCreated {
        if (!params.is_null() && !params.is_object()) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                   "InvalidParams"});
        }
        const nlohmann::json object = params.is_object() ? params : nlohmann::json::object();

        SessionOptions options;
        options.serverProfile = object.value("server_profile", std::string{"interactive"});
        options.model = object.value("model", runtime_.agent_config().model);
        options.title = object.value("title", std::string{});
        options.cwd = runtime_.root();
        if (object.contains("cwd")) {
            if (!object.at("cwd").is_string()) {
                throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                       "InvalidParams"});
            }
            std::error_code requested_error;
            const std::filesystem::path requested =
                std::filesystem::canonical(object.at("cwd").get<std::string>(), requested_error);
            std::error_code root_error;
            const std::filesystem::path root =
                std::filesystem::canonical(runtime_.root(), root_error);
            if (requested_error ||
                (!root_error && requested != root)) {
                throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InvalidParams),
                                       "InvalidParams"});
            }
            options.cwd = requested;
        }

        std::expected<AgentId, AgentError> created = runtime_.agents().create(options);
        if (!created.has_value()) {
            throw_mapped(map_agent_error(created.error()));
        }
        const SessionId session = runtime_.agents().get(*created).session();
        acquireLeaseOrThrow(session);
        try {
            registry_.addSession(identity_.workspace, session);
        } catch (const RegistryError& error) {
            throw_mapped(map_registry_error(error));
        }

        const std::optional<SessionHeader> header = runtime_.store().load(session);
        protocol::SessionCreated result;
        result.session = session;
        result.header = header.has_value() ? nlohmann::json(*header) : nlohmann::json::object();
        return result;
    });
}

protocol::SessionResumed HostRuntime::resumeSession(const SessionId& id) {
    return translate([&]() -> protocol::SessionResumed {
        std::expected<AgentId, AgentError> resumed = runtime_.agents().resume(id);
        if (!resumed.has_value()) {
            throw_mapped(map_agent_error(resumed.error()));
        }
        acquireLeaseOrThrow(id);
        return protocol::SessionResumed{id, agentStatus(id)};
    });
}

protocol::SessionCreated HostRuntime::forkSession(const SessionId& id,
                                                  std::int64_t seed_length) {
    return translate([&]() -> protocol::SessionCreated {
        if (seed_length < 0) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::InvalidForkBoundary),
                                   "InvalidForkBoundary"});
        }
        const SessionId child =
            runtime_.sessions().forkSession(id, static_cast<std::size_t>(seed_length));
        acquireLeaseOrThrow(child);
        std::expected<AgentId, AgentError> resumed = runtime_.agents().resume(child);
        if (!resumed.has_value()) {
            throw_mapped(map_agent_error(resumed.error()));
        }
        try {
            registry_.addSession(identity_.workspace, child);
        } catch (const RegistryError& error) {
            throw_mapped(map_registry_error(error));
        }
        const std::optional<SessionHeader> header = runtime_.store().load(child);
        protocol::SessionCreated result;
        result.session = child;
        result.header = header.has_value() ? nlohmann::json(*header) : nlohmann::json::object();
        return result;
    });
}

void HostRuntime::closeSession(const SessionId& id) {
    translate([&]() {
        if (Agent* agent = runtime_.agents().find(id); agent != nullptr) {
            runtime_.agents().dispose(agent->id());
        } else {
            runtime_.sessions().closeSession(id);
        }
    });
}

void HostRuntime::deleteSession(const SessionId& id) {
    translate([&]() {
        if (Agent* agent = runtime_.agents().find(id); agent != nullptr) {
            runtime_.agents().dispose(agent->id());
        }
        runtime_.sessions().deleteSession(id);
        try {
            registry_.removeSession(identity_.workspace, id);
        } catch (const RegistryError& error) {
            throw_mapped(map_registry_error(error));
        }
    });
}

void HostRuntime::activateSession(const SessionId& id) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        ensureAgent(id);
        if (!turns_.submit([this, id]() { runtime_.agents().activateSession(id); })) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InternalError),
                                   "InboxFull"});
        }
        active_session_ = id;
    });
}

void HostRuntime::suspendSession(const SessionId& id) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        runtime_.agents().suspendSession(id);
        if (active_session_.has_value() && active_session_->value == id.value) {
            active_session_.reset();
        }
    });
}

void HostRuntime::agentPrompt(const SessionId& id, const nlohmann::json& message) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        ensureAgent(id);
        const Message parsed = message_from_json(message);
        if (!turns_.submit([this, id, parsed]() {
                if (Agent* agent = runtime_.agents().find(id); agent != nullptr) {
                    agent->send(parsed);
                }
            })) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InternalError),
                                   "InboxFull"});
        }
    });
}

void HostRuntime::agentFollowup(const SessionId& id, const nlohmann::json& message) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        ensureAgent(id);
        const Message parsed = message_from_json(message);
        if (!turns_.submit([this, id, parsed]() {
                if (Agent* agent = runtime_.agents().find(id); agent != nullptr) {
                    agent->followup(parsed);
                }
            })) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InternalError),
                                   "InboxFull"});
        }
    });
}

void HostRuntime::agentSteer(const SessionId& id, const nlohmann::json& message) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        ensureAgent(id);
        const Message parsed = message_from_json(message);
        if (!turns_.submit([this, id, parsed]() {
                if (Agent* agent = runtime_.agents().find(id); agent != nullptr) {
                    agent->steer(parsed);
                }
            })) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InternalError),
                                   "InboxFull"});
        }
    });
}

void HostRuntime::agentInject(const SessionId& id, const nlohmann::json& context) {
    translate([&]() {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        ensureAgent(id);
        const ContextMessage parsed = context_from_json(context);
        if (!turns_.submit([this, id, parsed]() {
                if (Agent* agent = runtime_.agents().find(id); agent != nullptr) {
                    agent->inject(parsed);
                }
            })) {
            throw_mapped(WireError{protocol::code_value(protocol::RpcCode::InternalError),
                                   "InboxFull"});
        }
    });
}

bool HostRuntime::agentCancel(const SessionId& id,
                              const std::optional<std::string>& reason) {
    (void)reason;
    return translate([&]() -> bool {
        if (!sessionExists(id)) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        Agent* agent = runtime_.agents().find(id);
        if (agent == nullptr) {
            return false;
        }
        const bool in_flight = agent->hasPendingWork();
        agent->cancel();
        return in_flight;
    });
}

std::string HostRuntime::agentStatus(const SessionId& id) {
    return translate([&]() -> std::string {
        Agent* agent = runtime_.agents().find(id);
        if (agent == nullptr) {
            if (!sessionExists(id)) {
                throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                       "UnknownSession"});
            }
            return "Idle";
        }
        return agent->status() == AgentStatus::Running ? "Running" : "Idle";
    });
}

bool HostRuntime::decidePermission(const std::string& request_id,
                                   protocol::PermissionAnswer decision,
                                   protocol::PermissionScope scope) {
    return translate([&]() -> bool {
        protocol::PermissionDecisionParams params;
        params.request_id = request_id;
        params.decision = decision;
        params.scope = scope;
        return broker_.onDecision(params);
    });
}

bool HostRuntime::sessionExists(const SessionId& id) const {
    try {
        return runtime_.store().load(id).has_value();
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<EventRecord> HostRuntime::readEvents(const SessionId& id, Sequence after,
                                                 std::size_t limit) {
    return translate([&]() -> std::vector<EventRecord> {
        if (!runtime_.store().load(id).has_value()) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        const std::size_t clamped =
            std::max<std::size_t>(1, std::min<std::size_t>(limit, kMaxReplayBatch));
        return runtime_.store().readAfter(id, after, clamped);
    });
}

Sequence HostRuntime::headSequence(const SessionId& id) const {
    return translate([&]() -> Sequence { return runtime_.store().headSequence(id); });
}

std::optional<Sequence> HostRuntime::resolveCursor(const SessionId& id,
                                                   const protocol::EventCursor& cursor) const {
    const std::optional<std::string> decoded = base64url_decode(cursor.value);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    const std::string& token = *decoded;
    if (token.rfind(kCursorPrefix, 0) != 0) {
        return std::nullopt;
    }
    const std::size_t separator = token.find(':', kCursorPrefix.size());
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    if (token.substr(kCursorPrefix.size(), separator - kCursorPrefix.size()) != id.value) {
        return std::nullopt;
    }
    const std::string digits = token.substr(separator + 1);
    if (digits.empty()) {
        return std::nullopt;
    }
    Sequence sequence = 0;
    const char* begin = digits.data();
    const char* end = digits.data() + digits.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != end || sequence < 0) {
        return std::nullopt;
    }

    Sequence head = 0;
    try {
        head = runtime_.store().headSequence(id);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (sequence > head) {
        return head;
    }
    return sequence;
}

protocol::EventCursor HostRuntime::cursorFor(const SessionId& id,
                                             Sequence sequence) const {
    const std::string token =
        std::string{kCursorPrefix} + id.value + ":" + std::to_string(sequence);
    return protocol::EventCursor{base64url_encode(token)};
}

HostRuntime::WireError HostRuntime::map_agent_error(const AgentError& error) noexcept {
    switch (error.code) {
        case AgentErrorCode::UnknownSession:
            return {protocol::code_value(protocol::AppCode::UnknownSession), "UnknownSession"};
        case AgentErrorCode::LeaseHeldByOther:
        case AgentErrorCode::LeaseLost:
            return {protocol::code_value(protocol::AppCode::LeaseLost), "LeaseLost"};
        case AgentErrorCode::StoreUnavailable:
            return {protocol::code_value(protocol::AppCode::StoreUnavailable), "StoreUnavailable"};
        case AgentErrorCode::InboxFull:
            return {protocol::code_value(protocol::RpcCode::InternalError), "InboxFull"};
        case AgentErrorCode::AgentDisposed:
            return {protocol::code_value(protocol::AppCode::SessionNotActive), "AgentDisposed"};
        case AgentErrorCode::StepLimitExceeded:
        case AgentErrorCode::ContextAssemblyFailed:
        case AgentErrorCode::CompactionFailed:
        case AgentErrorCode::ProviderFailed:
            return {protocol::code_value(protocol::RpcCode::InternalError),
                    std::string{agent_error_code_name(error.code)}};
        case AgentErrorCode::Cancelled:
            return {protocol::code_value(protocol::RpcCode::InternalError), "Cancelled"};
        case AgentErrorCode::None:
        case AgentErrorCode::Internal:
            return {protocol::code_value(protocol::RpcCode::InternalError), "Internal"};
    }
    return {protocol::code_value(protocol::RpcCode::InternalError), "Internal"};
}

HostRuntime::WireError HostRuntime::map_store_error(const std::exception& error) noexcept {
    if (dynamic_cast<const InvalidForkBoundary*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::InvalidForkBoundary),
                "InvalidForkBoundary"};
    }
    if (dynamic_cast<const UnknownSession*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::UnknownSession), "UnknownSession"};
    }
    if (dynamic_cast<const LeaseLost*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::LeaseLost), "LeaseLost"};
    }
    if (dynamic_cast<const DependentSessionError*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::DependentSession), "DependentSession"};
    }
    if (dynamic_cast<const PayloadTooLarge*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::PayloadTooLarge), "PayloadTooLarge"};
    }
    if (dynamic_cast<const CorruptionError*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::StoreUnavailable), "StoreUnavailable"};
    }
    if (dynamic_cast<const SchemaVersionError*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::StoreUnavailable), "StoreUnavailable"};
    }
    if (dynamic_cast<const StoreError*>(&error) != nullptr) {
        return {protocol::code_value(protocol::AppCode::StoreUnavailable), "StoreUnavailable"};
    }
    return {protocol::code_value(protocol::RpcCode::InternalError), "Internal"};
}

HostRuntime::WireError HostRuntime::map_registry_error(const RegistryError& error) noexcept {
    switch (error.code()) {
        case RegistryErrorCode::UnknownWorkspace:
            return {protocol::code_value(protocol::AppCode::UnknownWorkspace), "UnknownWorkspace"};
        case RegistryErrorCode::HostClaimed:
            return {protocol::code_value(protocol::RpcCode::InternalError), "HostClaimed"};
        case RegistryErrorCode::MutationInProgress:
            return {protocol::code_value(protocol::RpcCode::InternalError), "MutationInProgress"};
        case RegistryErrorCode::OpenFailed:
        case RegistryErrorCode::SchemaVersion:
        case RegistryErrorCode::Corrupt:
        case RegistryErrorCode::LockUnavailable:
        case RegistryErrorCode::Uninitialized:
        case RegistryErrorCode::DuplicatePath:
        case RegistryErrorCode::WorkspaceNotEmpty:
        case RegistryErrorCode::NotWriteLockHolder:
            return {protocol::code_value(protocol::AppCode::RegistryUnavailable),
                    "RegistryUnavailable"};
    }
    return {protocol::code_value(protocol::AppCode::RegistryUnavailable), "RegistryUnavailable"};
}

HostRuntime::WireError HostRuntime::map_workspace_error(WorkspaceRuntimeErrorCode code) noexcept {
    switch (code) {
        case WorkspaceRuntimeErrorCode::WorkspaceBusy:
            return {protocol::code_value(protocol::RpcCode::InvalidRequest), "AlreadyRunning"};
        case WorkspaceRuntimeErrorCode::WorkspaceMissing:
            return {protocol::code_value(protocol::AppCode::UnknownWorkspace), "UnknownWorkspace"};
        case WorkspaceRuntimeErrorCode::StoreUnavailable:
            return {protocol::code_value(protocol::AppCode::StoreUnavailable), "StoreUnavailable"};
        case WorkspaceRuntimeErrorCode::ProviderSetupFailed:
            return {protocol::code_value(protocol::RpcCode::InternalError), "ProviderSetupFailed"};
        case WorkspaceRuntimeErrorCode::Internal:
            return {protocol::code_value(protocol::RpcCode::InternalError), "Internal"};
    }
    return {protocol::code_value(protocol::RpcCode::InternalError), "Internal"};
}

void HostRuntime::throw_mapped(const WireError& error, nlohmann::json data) {
    if (!data.is_object()) {
        data = nlohmann::json::object();
    }
    data["kind"] = error.kind;
    throw protocol::RpcException(error.code, error.kind, std::move(data));
}

} // namespace ymh

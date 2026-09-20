#include "ymh/transport/protocol_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"

namespace ymh::protocol {
namespace {

constexpr std::size_t kReplayBatch = 256;

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool is_hex(char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

[[nodiscard]] bool is_uuid_v4(std::string_view text) noexcept {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') {
                return false;
            }
            continue;
        }
        if (!is_hex(text[index])) {
            return false;
        }
    }
    if (text[14] != '4') {
        return false;
    }
    const char variant = text[19];
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'A' ||
           variant == 'b' || variant == 'B';
}

[[nodiscard]] const nlohmann::json& object_params(const nlohmann::json& params) {
    if (!params.is_object()) {
        throw RpcException(code_value(RpcCode::InvalidParams), "params must be an object");
    }
    return params;
}

[[nodiscard]] std::string string_param(const nlohmann::json& params, const char* key) {
    const nlohmann::json& object = object_params(params);
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        throw RpcException(code_value(RpcCode::InvalidParams),
                           std::string("missing or non-string param: ") + key);
    }
    return it->get<std::string>();
}

[[nodiscard]] SessionId session_param(const nlohmann::json& params) {
    return SessionId{string_param(params, "session")};
}

[[nodiscard]] nlohmann::json notification_json(std::string_view method_name,
                                               nlohmann::json params) {
    return encode(Notification{std::string{method_name}, std::move(params)});
}

[[nodiscard]] nlohmann::json to_json_value(const auto& value) {
    nlohmann::json json;
    to_json(json, value);
    return json;
}

[[nodiscard]] RpcException unknown_session(const SessionId& session) {
    return RpcException(code_value(AppCode::UnknownSession), "unknown session",
                        nlohmann::json{{"kind", "UnknownSession"}, {"session", session.value}});
}

} // namespace

std::uint32_t current_uid() noexcept { return static_cast<std::uint32_t>(::getuid()); }

ProtocolServer::ProtocolServer(TransportHost& host, ProtocolServerConfig config)
    : host_(host), config_(std::move(config)) {}

ClientId ProtocolServer::openConnection(std::uint32_t peer_uid, std::int32_t peer_pid,
                                        SendFn send, DropFn drop) {
    const ClientId id{next_client_++};
    Connection conn;
    conn.id = id;
    conn.peer_uid = peer_uid;
    conn.peer_pid = peer_pid;
    conn.send = std::move(send);
    conn.drop = std::move(drop);
    connections_.emplace(id.value, std::move(conn));
    return id;
}

ProtocolServer::Connection* ProtocolServer::find(ClientId id) {
    const auto it = connections_.find(id.value);
    return it == connections_.end() ? nullptr : &it->second;
}

void ProtocolServer::receiveBytes(ClientId id, std::string_view bytes) {
    Connection* conn = find(id);
    if (conn == nullptr || conn->dropped) {
        return;
    }
    conn->read_buffer.append(bytes.data(), bytes.size());
    conn->last_frame_at = std::chrono::steady_clock::now();
    publish_owner_liveness();

    std::vector<std::string> frames;
    while (true) {
        std::string frame;
        DecodeStatus status = DecodeStatus::NeedMore;
        try {
            status = FrameCodec::decode_step(conn->read_buffer, config_.limits.max_frame_bytes,
                                             frame);
        } catch (const ProtocolError& error) {
            respond_error(*conn, RequestId{}, error.code, error.what());
            conn->close_when_drained = true;
            maybe_close_after_drain(*conn);
            return;
        }
        if (status == DecodeStatus::NeedMore) {
            break;
        }
        frames.push_back(std::move(frame));
    }

    conn->batch_in_flight.clear();
    for (const std::string& body : frames) {
        if (conn->dropped) {
            return;
        }
        handle_body(*conn, body);
    }
    conn->batch_in_flight.clear();
}

void ProtocolServer::closeConnection(ClientId id) {
    const auto it = connections_.find(id.value);
    if (it == connections_.end()) {
        return;
    }
    it->second.dropped = true;
    outstanding_total_.fetch_sub(it->second.outstanding);
    signal_drain_progress();
    connections_.erase(it);
    publish_owner_liveness();
}

void ProtocolServer::onFrameWritten(ClientId id, std::size_t bytes) {
    Connection* conn = find(id);
    if (conn == nullptr || conn->dropped) {
        return;
    }
    const std::size_t before = conn->outstanding;
    conn->outstanding = bytes > before ? 0 : before - bytes;
    outstanding_total_.fetch_sub(before - conn->outstanding);
    signal_drain_progress();
    maybe_close_after_drain(*conn);
}

void ProtocolServer::handle_body(Connection& conn, std::string_view body) {
    Message message;
    try {
        message = parse_message(body);
    } catch (const ProtocolError& error) {
        respond_error(conn, RequestId{}, error.code, error.what());
        if (error.fatal) {
            conn.close_when_drained = true;
            maybe_close_after_drain(conn);
        }
        return;
    }

    const Request* request = std::get_if<Request>(&message);
    const Notification* notification = std::get_if<Notification>(&message);
    const std::string* method_name = nullptr;
    if (request != nullptr) {
        method_name = &request->method;
    } else if (notification != nullptr) {
        method_name = &notification->method;
    } else {
        return;
    }

    if (!conn.hello_done && *method_name != method::kHostHello) {
        if (request != nullptr) {
            respond_error(conn, request->id, code_value(AppCode::HandshakeRequired),
                          "host.hello is required before any other method");
        }
        conn.close_when_drained = true;
        maybe_close_after_drain(conn);
        return;
    }

    if (request != nullptr) {
        dispatch_request(conn, *request);
    } else {
        dispatch_notification(conn, *notification);
    }
}

void ProtocolServer::dispatch_request(Connection& conn, const Request& request) {
    if (!request.id.is_null()) {
        const std::string key = request_id_key(request.id);
        if (!conn.batch_in_flight.insert(key).second) {
            respond_error(conn, request.id, code_value(RpcCode::InvalidRequest),
                          "duplicate in-flight request id");
            return;
        }
    }

    if (request.method == method::kHostHello) {
        if (conn.hello_done) {
            respond_error(conn, request.id, code_value(RpcCode::InvalidRequest),
                          "host.hello already completed on this connection");
            return;
        }
        if (auto result = handle_hello(conn, request)) {
            respond(conn, request.id, std::move(*result));
        }
        return;
    }

    if (!is_known_method(request.method)) {
        respond_error(conn, request.id, code_value(RpcCode::MethodNotFound),
                      "unknown method: " + request.method);
        return;
    }
    if (!is_method_allowed(conn.profile, request.method)) {
        respond_error(conn, request.id, code_value(AppCode::MethodNotAllowedForProfile),
                      "method not allowed for the negotiated profile");
        return;
    }
    handle_method(conn, request, request.method);
}

void ProtocolServer::dispatch_notification(Connection& conn, const Notification& notification) {
    if (notification.method == method::kHostHello) {
        return;
    }
    if (!is_known_method(notification.method)) {
        return;
    }
    if (!is_method_allowed(conn.profile, notification.method)) {
        return;
    }
    Request request;
    request.method = notification.method;
    request.params = notification.params;
    conn.suppress_response = true;
    handle_method(conn, request, request.method);
    conn.suppress_response = false;
}

std::optional<nlohmann::json> ProtocolServer::handle_hello(Connection& conn,
                                                           const Request& request) {
    HelloParams params;
    try {
        params = object_params(request.params).get<HelloParams>();
    } catch (const std::exception& error) {
        respond_error(conn, request.id, code_value(RpcCode::InvalidParams), error.what());
        conn.close_when_drained = true;
        maybe_close_after_drain(conn);
        return std::nullopt;
    }

    if (params.protocol_version != kProtocolVersion) {
        respond_error(conn, request.id, code_value(AppCode::UnsupportedProtocol),
                      "unsupported protocol version",
                      nlohmann::json{{"kind", "UnsupportedProtocol"},
                                     {"supported", kProtocolVersion}});
        conn.close_when_drained = true;
        maybe_close_after_drain(conn);
        return std::nullopt;
    }
    if (!is_uuid_v4(params.client_instance.value)) {
        respond_error(conn, request.id, code_value(RpcCode::InvalidParams),
                      "client_instance must be a UUIDv4");
        conn.close_when_drained = true;
        maybe_close_after_drain(conn);
        return std::nullopt;
    }
    if (conn.peer_uid != config_.uid) {
        respond_error(conn, request.id, code_value(AppCode::AuthFailed),
                      "peer uid does not match the daemon uid");
        conn.close_when_drained = true;
        maybe_close_after_drain(conn);
        return std::nullopt;
    }
    const HostState state = host_.hostState();
    if (state != HostState::Serving) {
        const int code = state == HostState::Draining ? code_value(AppCode::ShutdownInProgress)
                                                      : code_value(AppCode::NotServing);
        respond_error(conn, request.id, code, "daemon is not serving");
        conn.close_when_drained = true;
        maybe_close_after_drain(conn);
        return std::nullopt;
    }

    std::vector<ClientId> superseded;
    for (const auto& entry : connections_) {
        const Connection& other = entry.second;
        if (other.id != conn.id && !other.dropped && other.hello_done &&
            other.instance == params.client_instance) {
            superseded.push_back(other.id);
        }
    }
    for (const ClientId target : superseded) {
        if (Connection* other = find(target)) {
            drop_client(*other, "superseded");
        }
    }

    conn.hello_done = true;
    conn.profile = params.profile;
    conn.role = params.role;
    conn.instance = params.client_instance;
    conn.last_frame_at = std::chrono::steady_clock::now();
    publish_owner_liveness();

    HelloResult result;
    result.protocol_version = kProtocolVersion;
    result.workspace = config_.workspace;
    result.boot_id = config_.boot_id;
    result.pid = config_.pid;
    result.profiles = {ServerProfile::Interactive, ServerProfile::Automation};
    result.client_id = conn.id;
    result.limits = config_.limits;
    result.server_time_ms = now_ms();
    return to_json_value(result);
}

void ProtocolServer::handle_method(Connection& conn, const Request& request,
                                   std::string_view method_name) {
    try {
        if (method_name == method::kHostAttach || method_name == method::kHostStatus) {
            respond(conn, request.id, to_json_value(compose_status(conn)));
        } else if (method_name == method::kHostPing) {
            respond(conn, request.id, nlohmann::json{{"server_time_ms", now_ms()}});
        } else if (method_name == method::kHostDetach) {
            respond(conn, request.id, nlohmann::json::object());
            conn.close_when_drained = true;
            maybe_close_after_drain(conn);
        } else if (method_name == method::kHostShutdown) {
            const std::string reason = request.params.is_object()
                                           ? request.params.value("reason", std::string{})
                                           : std::string{};
            const ShutdownReason parsed = parse_shutdown_reason(reason);
            if (!admit_shutdown(conn, parsed)) {
                throw RpcException(code_value(AppCode::NotLastOwner),
                                   "daemon still has another owner; shutdown refused");
            }
            host_.requestShutdown(parsed);
            respond(conn, request.id, nlohmann::json::object());
            onDaemonShuttingDown("host.shutdown accepted");
            conn.close_when_drained = true;
            maybe_close_after_drain(conn);
        } else if (method_name == method::kHostOwnership) {
            respond(conn, request.id, to_json_value(ownershipView(conn.id)));
        } else if (method_name == method::kWorkspaceList) {
            nlohmann::json array = nlohmann::json::array();
            for (const WorkspaceSummary& summary : host_.listWorkspaces()) {
                array.push_back(to_json_value(summary));
            }
            respond(conn, request.id, std::move(array));
        } else if (method_name == method::kWorkspaceShow) {
            const WorkspaceId workspace{string_param(request.params, "workspace")};
            respond(conn, request.id, to_json_value(host_.showWorkspace(workspace)));
        } else if (method_name == method::kSessionList) {
            nlohmann::json array = nlohmann::json::array();
            for (const SessionSummary& summary : host_.listSessions()) {
                array.push_back(to_json_value(summary));
            }
            respond(conn, request.id, std::move(array));
        } else if (method_name == method::kSessionShow) {
            respond(conn, request.id, to_json_value(host_.showSession(session_param(request.params))));
        } else if (method_name == method::kContextShow) {
            respond(conn, request.id, host_.showContext(session_param(request.params)));
        } else if (method_name == method::kSessionCreate) {
            const SessionCreated created = host_.createSession(object_params(request.params));
            respond(conn, request.id,
                    nlohmann::json{{"session", created.session.value}, {"header", created.header}});
            onSessionCreated(created.session);
        } else if (method_name == method::kSessionRename) {
            const SessionRenamedResult renamed = host_.renameSession(request.params);
            respond(conn, request.id,
                    nlohmann::json{{"session", renamed.session.value}, {"title", renamed.title}});
        } else if (method_name == method::kSessionSetMode) {
            const SetModeResult mode = host_.setSessionMode(request.params);
            respond(conn, request.id,
                    nlohmann::json{{"session", mode.session.value},
                                   {"active", mode.active},
                                   {"pending", mode.pending}});
        } else if (method_name == method::kSessionResume) {
            const SessionResumed resumed = host_.resumeSession(session_param(request.params));
            respond(conn, request.id,
                    nlohmann::json{{"session", resumed.session.value}, {"status", resumed.status}});
            onSessionCreated(resumed.session);
        } else if (method_name == method::kSessionFork) {
            const SessionId session = session_param(request.params);
            const auto seed = object_params(request.params).find("seed_length");
            if (seed == request.params.end() || !seed->is_number_integer()) {
                throw RpcException(code_value(RpcCode::InvalidParams),
                                   "seed_length must be an integer");
            }
            const SessionCreated created = host_.forkSession(session, seed->get<std::int64_t>());
            respond(conn, request.id,
                    nlohmann::json{{"session", created.session.value}, {"header", created.header}});
            onSessionCreated(created.session);
        } else if (method_name == method::kSessionReplay) {
            handle_subscribe(conn, request, true);
        } else if (method_name == method::kSessionActivate) {
            host_.activateSession(session_param(request.params));
            respond(conn, request.id, nlohmann::json::object());
        } else if (method_name == method::kSessionSuspend) {
            host_.suspendSession(session_param(request.params));
            respond(conn, request.id, nlohmann::json::object());
        } else if (method_name == method::kSessionCompact) {
            host_.compactSession(session_param(request.params));
            respond(conn, request.id, nlohmann::json{{"outcome", "Queued"}});
        } else if (method_name == method::kSessionClose) {
            const SessionId session = session_param(request.params);
            host_.closeSession(session);
            end_subscriptions(conn, session, "session_closed");
            respond(conn, request.id, nlohmann::json::object());
        } else if (method_name == method::kSessionDelete) {
            const SessionId session = session_param(request.params);
            const auto confirm = object_params(request.params).find("confirm");
            if (confirm == request.params.end() || !confirm->is_boolean() || !confirm->get<bool>()) {
                throw RpcException(code_value(RpcCode::InvalidParams),
                                   "session.delete requires confirm: true");
            }
            bool only_if_empty = false;
            bool force         = false;
            if (const auto it = object_params(request.params).find("only_if_empty");
                it != request.params.end()) {
                if (!it->is_boolean()) {
                    throw RpcException(code_value(RpcCode::InvalidParams),
                                       "only_if_empty must be a boolean");
                }
                only_if_empty = it->get<bool>();
            }
            if (const auto it = object_params(request.params).find("force");
                it != request.params.end()) {
                if (!it->is_boolean()) {
                    throw RpcException(code_value(RpcCode::InvalidParams),
                                       "force must be a boolean");
                }
                force = it->get<bool>();
            }
            host_.deleteSession(session, only_if_empty, force);
            respond(conn, request.id, nlohmann::json::object());
            onSessionClosed(session, "session_closed");
        } else if (method_name == method::kAgentPrompt || method_name == method::kAgentFollowup ||
                   method_name == method::kAgentSteer) {
            const SessionId session = session_param(request.params);
            const auto message = object_params(request.params).find("message");
            if (message == request.params.end()) {
                throw RpcException(code_value(RpcCode::InvalidParams), "message is required");
            }
            if (method_name == method::kAgentPrompt) {
                host_.agentPrompt(session, *message);
            } else if (method_name == method::kAgentFollowup) {
                host_.agentFollowup(session, *message);
            } else {
                host_.agentSteer(session, *message);
            }
            respond(conn, request.id, nlohmann::json{{"accepted", true}});
        } else if (method_name == method::kAgentInject) {
            const SessionId session = session_param(request.params);
            const auto context = object_params(request.params).find("context");
            if (context == request.params.end()) {
                throw RpcException(code_value(RpcCode::InvalidParams), "context is required");
            }
            host_.agentInject(session, *context);
            respond(conn, request.id, nlohmann::json{{"accepted", true}});
        } else if (method_name == method::kAgentCancel) {
            const SessionId session = session_param(request.params);
            std::optional<std::string> reason;
            if (request.params.is_object()) {
                const auto it = request.params.find("reason");
                if (it != request.params.end() && it->is_string()) {
                    reason = it->get<std::string>();
                }
            }
            const bool cancelled = host_.agentCancel(session, reason);
            respond(conn, request.id, nlohmann::json{{"cancelled", cancelled}});
        } else if (method_name == method::kAgentStatus) {
            respond(conn, request.id,
                    nlohmann::json{{"status", host_.agentStatus(session_param(request.params))}});
        } else if (method_name == method::kPermissionDecide) {
            const PermissionDecisionParams params =
                object_params(request.params).get<PermissionDecisionParams>();
            if (!host_.decidePermission(params.request_id, params.decision, params.scope)) {
                throw RpcException(code_value(RpcCode::InvalidParams),
                                   "unknown or expired permission request_id");
            }
            respond(conn, request.id, nlohmann::json::object());
        } else if (method_name == method::kEventSubscribe) {
            handle_subscribe(conn, request, false);
        } else if (method_name == method::kEventUnsubscribe) {
            const auto it = object_params(request.params).find("subscription");
            if (it == request.params.end() || !it->is_number_unsigned()) {
                throw RpcException(code_value(RpcCode::InvalidParams),
                                   "subscription must be an unsigned integer");
            }
            const std::uint64_t subscription = it->get<std::uint64_t>();
            const auto found = conn.subscriptions.find(subscription);
            respond(conn, request.id, nlohmann::json::object());
            if (found != conn.subscriptions.end()) {
                conn.subscriptions.erase(found);
                enqueue(conn, notification_json(
                                 notify::kEventUnsubscribed,
                                 to_json_value(UnsubscribedNotice{SubscriptionId{subscription},
                                                                  "unsubscribed"})));
            }
        } else if (method_name == method::kSkillsList) {
            respond(conn, request.id, host_.listSkills());
        } else if (method_name == method::kSkillsShow) {
            const auto it = object_params(request.params).find("name");
            if (it == request.params.end() || !it->is_string()) {
                throw RpcException(code_value(RpcCode::InvalidParams),
                                   "name must be a string");
            }
            respond(conn, request.id, host_.showSkill(it->get<std::string>()));
        } else if (method_name == method::kMcpStatus) {
            if (!request.params.is_object() || !request.params.empty()) {
                throw RpcException(code_value(RpcCode::InvalidParams),
                                   "mcp.status takes no parameters");
            }
            respond(conn, request.id, host_.mcpStatus());
        } else if (method_name == method::kAgentList) {
            respond(conn, request.id, host_.listAgents(request.params));
        } else if (method_name == method::kAgentSelect) {
            respond(conn, request.id, host_.selectAgent(request.params));
        } else {
            respond_error(conn, request.id, code_value(RpcCode::MethodNotFound),
                          "unhandled method");
        }
    } catch (const RpcException& error) {
        respond_error(conn, request.id, error.code(), error.what(), error.data());
    } catch (const nlohmann::json::exception& error) {
        respond_error(conn, request.id, code_value(RpcCode::InvalidParams), error.what());
    } catch (const std::exception& error) {
        respond_error(conn, request.id, code_value(RpcCode::InternalError), error.what());
    }
}

void ProtocolServer::handle_subscribe(Connection& conn, const Request& request, bool replay_only) {
    SubscribeParams params;
    try {
        params = object_params(request.params).get<SubscribeParams>();
    } catch (const std::exception& error) {
        respond_error(conn, request.id, code_value(RpcCode::InvalidParams), error.what());
        return;
    }
    if (!session_known(params.session)) {
        const RpcException error = unknown_session(params.session);
        respond_error(conn, request.id, error.code(), error.what(), error.data());
        return;
    }
    if (conn.subscriptions.size() >= config_.limits.max_subscriptions_per_client) {
        respond_error(conn, request.id, code_value(AppCode::SubscriptionLimit),
                      "subscription limit reached");
        return;
    }

    const Sequence head = host_.headSequence(params.session);
    Sequence after = head;
    if (params.from.kind == StreamFrom::Kind::Beginning) {
        after = 0;
    } else if (params.from.kind == StreamFrom::Kind::Cursor) {
        if (!params.from.cursor.has_value()) {
            respond_error(conn, request.id, code_value(RpcCode::InvalidParams),
                          "from.kind == cursor requires from.cursor");
            return;
        }
        const std::optional<Sequence> resolved =
            host_.resolveCursor(params.session, *params.from.cursor);
        if (!resolved.has_value()) {
            respond_error(conn, request.id, code_value(AppCode::CursorInvalid),
                          "cursor is not resolvable for this session",
                          nlohmann::json{{"kind", "CursorInvalid"},
                                         {"session", params.session.value}});
            return;
        }
        after = *resolved;
    }

    const std::uint64_t subscription = next_subscription_++;
    respond(conn, request.id,
            to_json_value(SubscribeResult{SubscriptionId{subscription},
                                          host_.cursorFor(params.session, head)}));
    if (conn.dropped) {
        return;
    }

    while (true) {
        const std::vector<EventRecord> batch = host_.readEvents(params.session, after, kReplayBatch);
        if (batch.empty()) {
            break;
        }
        for (const EventRecord& record : batch) {
            after = record.seq;
            enqueue(conn, stream_notification(SubscriptionId{subscription}, true, record));
            if (conn.dropped) {
                return;
            }
        }
    }

    if (replay_only) {
        enqueue(conn, notification_json(
                         notify::kEventUnsubscribed,
                         to_json_value(UnsubscribedNotice{SubscriptionId{subscription},
                                                          "replay_complete"})));
        return;
    }
    conn.subscriptions[subscription] = Subscription{params.session};

    if (subscribe_observer_ && conn.profile == ServerProfile::Interactive) {
        SubscribeObserver observer = subscribe_observer_;
        observer(params.session, conn.id);
    }
}

void ProtocolServer::respond(Connection& conn, const RequestId& id, nlohmann::json result) {
    if (conn.suppress_response) {
        return;
    }
    enqueue(conn, encode(Response{id, std::move(result)}));
}

void ProtocolServer::respond_error(Connection& conn, const RequestId& id, int code,
                                   std::string message, nlohmann::json data) {
    if (conn.suppress_response) {
        return;
    }
    enqueue(conn, encode(ErrorResponse{id, code, std::move(message), std::move(data)}));
}

void ProtocolServer::enqueue(Connection& conn, const nlohmann::json& message) {
    if (conn.dropped) {
        return;
    }
    std::string frame;
    try {
        frame = FrameCodec::encode(message.dump(), config_.limits.max_frame_bytes);
    } catch (const ProtocolError&) {
        drop_client(conn, "outbound_frame_too_large");
        return;
    }
    enqueue_frame(conn, std::move(frame));
}

void ProtocolServer::enqueue_frame(Connection& conn, std::string frame) {
    if (conn.dropped) {
        return;
    }
    if (conn.outstanding + frame.size() > config_.limits.max_outbound_bytes) {
        drop_client(conn, "outbound_queue_overflow");
        return;
    }
    conn.outstanding += frame.size();
    outstanding_total_.fetch_add(frame.size());
    conn.outbound.push_back(std::move(frame));
    pump(conn);
}

void ProtocolServer::pump(Connection& conn) {
    if (conn.pumping || conn.dropped) {
        return;
    }
    conn.pumping = true;
    while (!conn.outbound.empty() && !conn.dropped) {
        std::string frame = std::move(conn.outbound.front());
        conn.outbound.pop_front();
        SendFn send = conn.send;
        if (send) {
            send(conn.id, std::move(frame));
        }
    }
    conn.pumping = false;
    signal_drain_progress();
    maybe_close_after_drain(conn);
}

void ProtocolServer::signal_drain_progress() {
    std::lock_guard lock(drain_mutex_);
    drain_cv_.notify_all();
}

bool ProtocolServer::waitForDrain(std::chrono::milliseconds grace) {
    std::unique_lock lock(drain_mutex_);
    return drain_cv_.wait_for(lock, grace, [this] {
        return outstanding_total_.load() == 0;
    });
}

void ProtocolServer::maybe_close_after_drain(Connection& conn) {
    if (conn.dropped || conn.pumping) {
        return;
    }
    if (conn.close_when_drained && conn.outbound.empty() && conn.outstanding == 0) {
        drop_client(conn, "closed");
    }
}

void ProtocolServer::drop_client(Connection& conn, std::string reason) {
    if (conn.dropped) {
        return;
    }
    conn.dropped = true;
    conn.outbound.clear();
    conn.subscriptions.clear();
    outstanding_total_.fetch_sub(conn.outstanding);
    conn.outstanding = 0;
    signal_drain_progress();
    publish_owner_liveness();
    DropFn drop = conn.drop;
    conn.send = nullptr;
    conn.drop = nullptr;
    if (drop) {
        drop(conn.id, std::move(reason));
    }
}

void ProtocolServer::end_subscriptions(Connection& conn, const SessionId& session,
                                       std::string reason) {
    for (auto it = conn.subscriptions.begin(); it != conn.subscriptions.end();) {
        if (it->second.session == session) {
            const std::uint64_t subscription = it->first;
            it = conn.subscriptions.erase(it);
            enqueue(conn, notification_json(
                             notify::kEventUnsubscribed,
                             to_json_value(UnsubscribedNotice{SubscriptionId{subscription},
                                                              reason})));
            if (conn.dropped) {
                return;
            }
        } else {
            ++it;
        }
    }
}

HostStatus ProtocolServer::compose_status(const Connection& conn) const {
    const HostStatusInfo info = host_.hostStatus();
    HostStatus status;
    status.state = info.state;
    status.workspace = info.workspace;
    status.boot_id = info.boot_id;
    status.pid = info.pid;
    status.active_session = info.active_session;
    status.attached_clients = attachedClients();
    status.profile = conn.profile;
    return status;
}

nlohmann::json ProtocolServer::stream_notification(SubscriptionId subscription, bool replay,
                                                   const EventRecord& record) {
    StreamNotification notification;
    notification.subscription = subscription;
    notification.replay = replay;
    notification.envelope = SessionEnvelope{record.event.session_id, record.event};
    notification.cursor = host_.cursorFor(record.event.session_id, record.seq);
    return notification_json(notify::kEventStream, to_json_value(notification));
}

bool ProtocolServer::session_known(const SessionId& session) const {
    return host_.sessionExists(session);
}

void ProtocolServer::onEventCommitted(const EventRecord& record) {
    const SessionId& session = record.event.session_id;
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done) {
            continue;
        }
        for (auto& sub_entry : conn.subscriptions) {
            if (!(sub_entry.second.session == session)) {
                continue;
            }
            enqueue(conn, stream_notification(SubscriptionId{sub_entry.first}, false, record));
            if (conn.dropped) {
                break;
            }
        }
    }
}

void ProtocolServer::onLiveEvent(const Event& event) {
    const SessionId& session = event.session_id;
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done) {
            continue;
        }
        const bool subscribed =
            std::any_of(conn.subscriptions.begin(), conn.subscriptions.end(),
                        [&session](const auto& sub_entry) {
                            return sub_entry.second.session == session;
                        });
        if (!subscribed) {
            continue;
        }
        enqueue(conn, notification_json(
                         notify::kEventLive,
                         to_json_value(LiveNotification{SessionEnvelope{session, event, false}})));
        if (conn.dropped) {
            break;
        }
    }
}

void ProtocolServer::onSessionCreated(const SessionId& session) {
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done || conn.profile != ServerProfile::Interactive) {
            continue;
        }
        enqueue(conn, notification_json(
                         notify::kHostEvent,
                         to_json_value(HostNotice{HostNoticeKind::SessionCreated,
                                                  config_.workspace, session, std::string{}})));
    }
}

void ProtocolServer::onSessionClosed(const SessionId& session, std::string reason) {
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done) {
            continue;
        }
        bool subscribed = false;
        for (auto it = conn.subscriptions.begin(); it != conn.subscriptions.end();) {
            if (it->second.session == session) {
                subscribed = true;
                const std::uint64_t subscription = it->first;
                it = conn.subscriptions.erase(it);
                enqueue(conn, notification_json(
                                 notify::kEventUnsubscribed,
                                 to_json_value(UnsubscribedNotice{SubscriptionId{subscription},
                                                                  reason})));
                if (conn.dropped) {
                    break;
                }
            } else {
                ++it;
            }
        }
        if (!conn.dropped && subscribed && conn.profile == ServerProfile::Interactive) {
            enqueue(conn, notification_json(
                             notify::kHostEvent,
                             to_json_value(HostNotice{HostNoticeKind::SessionClosed,
                                                      config_.workspace, session, reason})));
        }
    }
}

void ProtocolServer::onLeaseLost(const SessionId& session, std::string detail) {
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done || conn.profile != ServerProfile::Interactive) {
            continue;
        }
        const bool subscribed =
            std::any_of(conn.subscriptions.begin(), conn.subscriptions.end(),
                        [&session](const auto& sub_entry) {
                            return sub_entry.second.session == session;
                        });
        if (subscribed) {
            enqueue(conn, notification_json(
                             notify::kHostEvent,
                             to_json_value(HostNotice{HostNoticeKind::LeaseLost,
                                                      config_.workspace, session,
                                                      std::move(detail)})));
        }
    }
}

void ProtocolServer::onDaemonShuttingDown(std::string detail) {
    if (shutdown_notice_emitted_) {
        return;
    }
    shutdown_notice_emitted_ = true;
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done || conn.profile != ServerProfile::Interactive) {
            continue;
        }
        enqueue(conn, notification_json(
                         notify::kHostEvent,
                         to_json_value(HostNotice{HostNoticeKind::DaemonShuttingDown,
                                                  config_.workspace, std::nullopt,
                                                  std::move(detail)})));
    }
}

void ProtocolServer::onMcpServerStatus(std::string detail) {
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done || conn.profile != ServerProfile::Interactive) {
            continue;
        }
        enqueue(conn, notification_json(
                         notify::kHostEvent,
                         to_json_value(HostNotice{HostNoticeKind::McpServerStatus,
                                                  config_.workspace, std::nullopt,
                                                  std::move(detail)})));
    }
}

void ProtocolServer::onPermissionRequest(const PermissionRequest& request) {
    for (auto& entry : connections_) {
        Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done || conn.profile != ServerProfile::Interactive) {
            continue;
        }
        const bool subscribed =
            std::any_of(conn.subscriptions.begin(), conn.subscriptions.end(),
                        [&request](const auto& sub_entry) {
                            return sub_entry.second.session == request.session;
                        });
        if (subscribed) {
            enqueue(conn, notification_json(notify::kPermissionRequest, to_json_value(request)));
        }
    }
}

std::size_t ProtocolServer::attachedClients() const {
    std::size_t count = 0;
    for (const auto& entry : connections_) {
        if (!entry.second.dropped) {
            ++count;
        }
    }
    return count;
}

bool ProtocolServer::hasClient(ClientId id) const {
    const auto it = connections_.find(id.value);
    return it != connections_.end() && !it->second.dropped;
}

std::optional<ServerProfile> ProtocolServer::profileOf(ClientId id) const {
    const auto it = connections_.find(id.value);
    if (it == connections_.end() || it->second.dropped) {
        return std::nullopt;
    }
    return it->second.profile;
}

std::optional<ClientInstanceId> ProtocolServer::instanceOf(ClientId id) const {
    const auto it = connections_.find(id.value);
    if (it == connections_.end() || it->second.dropped) {
        return std::nullopt;
    }
    return it->second.instance;
}

std::optional<ClientRole> ProtocolServer::roleOf(ClientId id) const {
    const auto it = connections_.find(id.value);
    if (it == connections_.end() || it->second.dropped || !it->second.hello_done) {
        return std::nullopt;
    }
    return it->second.role;
}

OwnershipView ProtocolServer::ownershipView(ClientId caller) const {
    OwnershipView view;
    std::string caller_instance;
    const auto caller_it = connections_.find(caller.value);
    if (caller_it != connections_.end()) {
        caller_instance = caller_it->second.instance.value;
    }

    for (const auto& entry : connections_) {
        const Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done) {
            continue;
        }
        if (conn.role == ClientRole::Supervisor) {
            ++view.live_supervisors;
        } else if (conn.role == ClientRole::Automation) {
            ++view.live_automation;
        } else {
            continue;
        }
        view.clients.push_back(OwnershipView::ClientInfo{conn.instance.value, conn.role,
                                                         conn.peer_pid});
    }

    const std::shared_ptr<const std::vector<ClientInstanceId>> snapshot =
        host_.freshOwnerSnapshot();
    if (snapshot != nullptr) {
        for (const ClientInstanceId& id : *snapshot) {
            if (id.value != caller_instance) {
                ++view.other_fresh_owners;
            }
        }
    }
    view.shutting_down = host_.hostState() == HostState::Draining;
    return view;
}

std::size_t ProtocolServer::subscriptionCount(ClientId id) const {
    const auto it = connections_.find(id.value);
    if (it == connections_.end()) {
        return 0;
    }
    return it->second.subscriptions.size();
}

std::size_t ProtocolServer::outstandingBytes(ClientId id) const {
    const auto it = connections_.find(id.value);
    if (it == connections_.end()) {
        return 0;
    }
    return it->second.outstanding;
}

bool ProtocolServer::isDropped(ClientId id) const {
    const auto it = connections_.find(id.value);
    return it == connections_.end() || it->second.dropped;
}

bool ProtocolServer::isHandshaken(ClientId id) const {
    const auto it = connections_.find(id.value);
    return it != connections_.end() && it->second.hello_done && !it->second.dropped;
}

std::size_t ProtocolServer::sessionSubscriberCount(const SessionId& session) const {
    std::size_t count = 0;
    for (const auto& entry : connections_) {
        const Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done || conn.profile != ServerProfile::Interactive) {
            continue;
        }
        const bool subscribed =
            std::any_of(conn.subscriptions.begin(), conn.subscriptions.end(),
                        [&session](const auto& sub_entry) {
                            return sub_entry.second.session == session;
                        });
        if (subscribed) {
            ++count;
        }
    }
    return count;
}

void ProtocolServer::set_subscribe_observer(SubscribeObserver observer) {
    subscribe_observer_ = std::move(observer);
}

void ProtocolServer::set_owner_liveness_sink(OwnerLivenessSink sink) {
    owner_liveness_sink_ = std::move(sink);
}

bool ProtocolServer::admit_shutdown(const Connection& caller, ShutdownReason reason) const {
    if (reason == ShutdownReason::WorkspaceStop) {
        return true;
    }

    std::size_t live_supervisors = 0;
    std::size_t live_automation = 0;
    for (const auto& entry : connections_) {
        const Connection& conn = entry.second;
        if (conn.dropped || !conn.hello_done) {
            continue;
        }
        if (conn.role == ClientRole::Supervisor) {
            ++live_supervisors;
        } else if (conn.role == ClientRole::Automation) {
            ++live_automation;
        }
    }
    const bool caller_is_supervisor = caller.role == ClientRole::Supervisor;
    const std::size_t supervisors_excluding =
        (caller_is_supervisor && live_supervisors > 0) ? live_supervisors - 1 : live_supervisors;
    if (supervisors_excluding > 0) {
        return false;
    }
    if (live_automation > 0) {
        return false;
    }

    const std::shared_ptr<const std::vector<ClientInstanceId>> snapshot =
        host_.freshOwnerSnapshot();
    if (snapshot != nullptr) {
        for (const ClientInstanceId& id : *snapshot) {
            if (id.value != caller.instance.value) {
                return false;
            }
        }
    }
    return true;
}

void ProtocolServer::publish_owner_liveness() {
    if (!owner_liveness_sink_) {
        return;
    }
    std::size_t count = 0;
    std::chrono::steady_clock::time_point last{};
    for (const auto& entry : connections_) {
        const Connection& conn = entry.second;
        if (!conn.hello_done || conn.dropped) {
            continue;
        }
        if (conn.role != ClientRole::Supervisor && conn.role != ClientRole::Automation) {
            continue;
        }
        ++count;
        if (conn.last_frame_at > last) {
            last = conn.last_frame_at;
        }
    }
    owner_liveness_sink_(count, last);
}

} // namespace ymh::protocol

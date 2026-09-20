#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;

TEST(TransportProtocol, WireEnumStringsArePinned) {
    EXPECT_EQ(protocol::wire_name(protocol::ServerProfile::Interactive), "interactive");
    EXPECT_EQ(protocol::wire_name(protocol::ServerProfile::Automation), "automation");
    EXPECT_EQ(protocol::wire_name(protocol::StreamFrom::Kind::Now), "now");
    EXPECT_EQ(protocol::wire_name(protocol::StreamFrom::Kind::Beginning), "beginning");
    EXPECT_EQ(protocol::wire_name(protocol::StreamFrom::Kind::Cursor), "cursor");
    EXPECT_EQ(protocol::wire_name(protocol::HostNoticeKind::SessionClosed), "session_closed");
    EXPECT_EQ(protocol::wire_name(protocol::HostNoticeKind::SessionCreated), "session_created");
    EXPECT_EQ(protocol::wire_name(protocol::HostNoticeKind::LeaseLost), "lease_lost");
    EXPECT_EQ(protocol::wire_name(protocol::HostNoticeKind::DaemonShuttingDown),
              "daemon_shutting_down");
    EXPECT_EQ(protocol::wire_name(protocol::PermissionAnswer::Allow), "allow");
    EXPECT_EQ(protocol::wire_name(protocol::PermissionAnswer::Deny), "deny");
    EXPECT_EQ(protocol::wire_name(protocol::PermissionScope::Once), "once");
    EXPECT_EQ(protocol::wire_name(protocol::PermissionScope::Session), "session");
    EXPECT_EQ(protocol::wire_name(protocol::PermissionScope::Always), "always");
}

TEST(TransportProtocol, ParseEnumRejectsUnknown) {
    EXPECT_FALSE(protocol::parse_server_profile("tui").has_value());
    EXPECT_FALSE(protocol::parse_stream_kind("latest").has_value());
    EXPECT_FALSE(protocol::parse_host_notice_kind("nope").has_value());
    EXPECT_FALSE(protocol::parse_permission_answer("maybe").has_value());
    EXPECT_FALSE(protocol::parse_permission_scope("forever").has_value());
    EXPECT_TRUE(protocol::parse_server_profile("automation").has_value());
}

TEST(TransportProtocol, MethodCatalogIsComplete) {
    // 18 §3.4.1 (CX-D11/M10) landed at 33; 25-D5 adds `session.set_mode`;
    // 45-D6 adds `mcp.status`; 45-D9 adds `agent.list`/`agent.select`.
    EXPECT_EQ(protocol::all_methods().size(), 37u);
    for (const std::string_view name : protocol::all_methods()) {
        EXPECT_TRUE(protocol::is_known_method(name));
    }
    EXPECT_TRUE(protocol::is_known_method("host.ownership"));
    EXPECT_TRUE(protocol::is_known_method("session.rename"));
    EXPECT_TRUE(protocol::is_known_method("session.set_mode"));
    EXPECT_TRUE(protocol::is_known_method("skills.list"));
    EXPECT_TRUE(protocol::is_known_method("skills.show"));
    EXPECT_TRUE(protocol::is_known_method("context.show"));
    EXPECT_TRUE(protocol::is_known_method("mcp.status"));
    EXPECT_TRUE(protocol::is_known_method("agent.list"));
    EXPECT_TRUE(protocol::is_known_method("agent.select"));
    EXPECT_FALSE(protocol::is_known_method("host.nope"));
}

TEST(TransportProtocol, ProfileGating) {
    for (const std::string_view name : protocol::all_methods()) {
        EXPECT_TRUE(protocol::is_method_allowed(protocol::ServerProfile::Interactive, name));
    }
    EXPECT_FALSE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                             protocol::method::kSessionActivate));
    EXPECT_FALSE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                             protocol::method::kSessionSuspend));
    EXPECT_FALSE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                             protocol::method::kSessionCompact));
    EXPECT_FALSE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                             protocol::method::kHostShutdown));
    EXPECT_FALSE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                             protocol::method::kAgentSelect));
    EXPECT_TRUE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                            protocol::method::kAgentPrompt));
    EXPECT_TRUE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                            protocol::method::kEventSubscribe));
    EXPECT_TRUE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                            protocol::method::kSessionRename));
    EXPECT_TRUE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                            protocol::method::kContextShow));
    EXPECT_TRUE(protocol::is_method_allowed(protocol::ServerProfile::Automation,
                                            protocol::method::kMcpStatus));
}

TEST(TransportProtocol, HostErrorMapping) {
    EXPECT_EQ(protocol::rpc_code_for_host_error(protocol::HostErrorCode::WorkspaceMissing),
              protocol::code_value(protocol::AppCode::UnknownWorkspace));
    EXPECT_EQ(protocol::rpc_code_for_host_error(protocol::HostErrorCode::AttachRejected),
              protocol::code_value(protocol::AppCode::AuthFailed));
    EXPECT_EQ(protocol::rpc_code_for_host_error(protocol::HostErrorCode::ShutdownInProgress),
              protocol::code_value(protocol::AppCode::ShutdownInProgress));
    EXPECT_EQ(protocol::rpc_code_for_host_error(protocol::HostErrorCode::AlreadyRunning),
              protocol::code_value(protocol::RpcCode::InvalidRequest));
}

TEST(TransportProtocol, PingIntervalOutpacesIdleTimeout) {
    EXPECT_EQ(protocol::kPingInterval.count(), 10000);
    EXPECT_EQ(protocol::TransportLimits{}.idle_timeout.count(), 30000);
    EXPECT_GE(protocol::TransportLimits{}.idle_timeout, 3 * protocol::kPingInterval);
}

TEST(TransportProtocol, SessionEnvelopeCarriesCoreEventOnly) {
    protocol::SessionEnvelope envelope;
    envelope.session = SessionId{"session-1"};
    envelope.event.id.value = "event-1";
    envelope.event.session_id = SessionId{"session-1"};
    envelope.event.timestamp =
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1700000000000}};
    envelope.event.type = EventType::TurnStarted;
    envelope.event.payload = nlohmann::json{{"turn", 1}};

    nlohmann::json json;
    protocol::to_json(json, envelope);
    EXPECT_EQ(json.at("session").get<std::string>(), "session-1");
    EXPECT_EQ(json.at("event").at("type").get<std::string>(), "turn/start");
    EXPECT_FALSE(json.contains("sequence"));
    EXPECT_FALSE(json.at("event").contains("sequence"));
    EXPECT_FALSE(json.at("event").contains("ui"));

    protocol::SessionEnvelope parsed;
    protocol::from_json(json, parsed);
    EXPECT_EQ(parsed.session.value, "session-1");
    EXPECT_EQ(parsed.event.session_id.value, "session-1");
    EXPECT_EQ(parsed.event.type, EventType::TurnStarted);
}

TEST(TransportProtocol, KnownEventTypeWithMalformedPayloadStaysLoud) {
    const nlohmann::json body = {
        {"session", "s1"},
        {"event",
         {{"session_id", "s1"},
          {"timestamp", 0},
          {"type", "turn/start"},
          {"payload", nlohmann::json::object()}}},
    };

    protocol::SessionEnvelope envelope;
    EXPECT_THROW(static_cast<void>(protocol::from_json(body, envelope)), std::exception);
}

TEST(TransportProtocol, StreamNotificationCarriesPostEventCursor) {
    protocol::StreamNotification notification;
    notification.subscription = protocol::SubscriptionId{9};
    notification.replay = true;
    notification.envelope.session = SessionId{"s"};
    notification.envelope.event.session_id = SessionId{"s"};
    notification.envelope.event.type = EventType::AssistantChunk;
    notification.cursor = protocol::EventCursor{"c1:s:4"};

    nlohmann::json json;
    protocol::to_json(json, notification);
    EXPECT_EQ(json.at("subscription").get<std::uint64_t>(), 9u);
    EXPECT_TRUE(json.at("replay").get<bool>());
    EXPECT_EQ(json.at("cursor").get<std::string>(), "c1:s:4");
    EXPECT_FALSE(json.contains("sequence"));

    protocol::StreamNotification parsed;
    protocol::from_json(json, parsed);
    EXPECT_EQ(parsed.subscription.value, 9u);
    EXPECT_TRUE(parsed.replay);
    EXPECT_EQ(parsed.cursor.value, "c1:s:4");
}

TEST(TransportProtocol, HelloRoundTrips) {
    protocol::HelloParams params;
    params.protocol_version = protocol::kProtocolVersion;
    params.profile = protocol::ServerProfile::Automation;
    params.client_instance = protocol::ClientInstanceId{"33333333-3333-4333-8333-333333333333"};
    params.resume_hint = protocol::EventCursor{"c1:s:2"};

    nlohmann::json json;
    protocol::to_json(json, params);
    EXPECT_EQ(json.at("profile").get<std::string>(), "automation");

    protocol::HelloParams parsed;
    protocol::from_json(json, parsed);
    EXPECT_EQ(parsed.profile, protocol::ServerProfile::Automation);
    EXPECT_EQ(parsed.client_instance.value, params.client_instance.value);
    ASSERT_TRUE(parsed.resume_hint.has_value());
    EXPECT_EQ(parsed.resume_hint->value, "c1:s:2");

    protocol::HelloResult result;
    result.workspace.value = "w";
    result.boot_id.value = "b";
    result.pid = 7;
    result.profiles = {protocol::ServerProfile::Interactive, protocol::ServerProfile::Automation};
    result.client_id = protocol::ClientId{3};
    result.limits.max_frame_bytes = 123;
    result.server_time_ms = 456;

    nlohmann::json result_json;
    protocol::to_json(result_json, result);
    protocol::HelloResult parsed_result;
    protocol::from_json(result_json, parsed_result);
    EXPECT_EQ(parsed_result.workspace.value, "w");
    EXPECT_EQ(parsed_result.client_id.value, 3u);
    EXPECT_EQ(parsed_result.limits.max_frame_bytes, 123u);
    EXPECT_EQ(parsed_result.profiles.size(), 2u);
}

TEST(TransportProtocol, LimitsRoundTrip) {
    protocol::TransportLimits limits;
    limits.max_frame_bytes = 4096;
    limits.max_outbound_bytes = 2048;
    limits.max_subscriptions_per_client = 8;
    limits.handshake_timeout = std::chrono::milliseconds{1234};
    limits.idle_timeout = std::chrono::milliseconds{5678};

    nlohmann::json json;
    protocol::to_json(json, limits);
    protocol::TransportLimits parsed;
    protocol::from_json(json, parsed);
    EXPECT_EQ(parsed.max_frame_bytes, 4096u);
    EXPECT_EQ(parsed.max_outbound_bytes, 2048u);
    EXPECT_EQ(parsed.handshake_timeout.count(), 1234);
    EXPECT_EQ(parsed.idle_timeout.count(), 5678);
}

TEST(TransportProtocol, HostNoticeAndPermissionRoundTrip) {
    protocol::HostNotice notice;
    notice.kind = protocol::HostNoticeKind::LeaseLost;
    notice.workspace.value = "w";
    notice.session = SessionId{"s"};
    notice.detail = "read-only";

    nlohmann::json json;
    protocol::to_json(json, notice);
    EXPECT_EQ(json.at("kind").get<std::string>(), "lease_lost");

    protocol::HostNotice parsed;
    protocol::from_json(json, parsed);
    EXPECT_EQ(parsed.kind, protocol::HostNoticeKind::LeaseLost);
    ASSERT_TRUE(parsed.session.has_value());
    EXPECT_EQ(parsed.session->value, "s");

    protocol::PermissionRequest request;
    request.request_id = "req-1";
    request.session = SessionId{"s"};
    request.tool = "shell";
    request.arguments = nlohmann::json{{"cmd", "ls"}};
    request.summary = "run ls";
    request.expires_at_ms = 99;

    nlohmann::json request_json;
    protocol::to_json(request_json, request);
    protocol::PermissionRequest parsed_request;
    protocol::from_json(request_json, parsed_request);
    EXPECT_EQ(parsed_request.request_id, "req-1");
    EXPECT_EQ(parsed_request.arguments.at("cmd").get<std::string>(), "ls");

    protocol::PermissionDecisionParams decision;
    decision.request_id = "req-1";
    decision.decision = protocol::PermissionAnswer::Allow;
    decision.scope = protocol::PermissionScope::Session;
    nlohmann::json decision_json;
    protocol::to_json(decision_json, decision);
    EXPECT_EQ(decision_json.at("decision").get<std::string>(), "allow");
    EXPECT_EQ(decision_json.at("scope").get<std::string>(), "session");
}

TEST(TransportProtocol, HostStatusRoundTrips) {
    protocol::HostStatus status;
    status.state = protocol::HostState::Serving;
    status.workspace.value = "w";
    status.boot_id.value = "b";
    status.pid = 11;
    status.attached_clients = 2;
    status.active_session = SessionId{"s"};
    status.profile = protocol::ServerProfile::Interactive;

    nlohmann::json json;
    protocol::to_json(json, status);
    protocol::HostStatus parsed;
    protocol::from_json(json, parsed);
    EXPECT_EQ(parsed.state, protocol::HostState::Serving);
    EXPECT_EQ(parsed.attached_clients, 2u);
    ASSERT_TRUE(parsed.active_session.has_value());
    EXPECT_EQ(parsed.active_session->value, "s");
}

TEST(TransportProtocol, ClientRoleParseSerializeAndDefault) {
    EXPECT_EQ(protocol::to_string(protocol::ClientRole::Supervisor), "supervisor");
    EXPECT_EQ(protocol::to_string(protocol::ClientRole::Automation), "automation");
    EXPECT_EQ(protocol::to_string(protocol::ClientRole::Observer), "observer");
    EXPECT_EQ(protocol::parse_client_role("observer"), protocol::ClientRole::Observer);
    EXPECT_FALSE(protocol::parse_client_role("owner").has_value());

    const nlohmann::json without_role = nlohmann::json{
        {"protocol_version", protocol::kProtocolVersion},
        {"profile", "interactive"},
        {"client_instance", "33333333-3333-4333-8333-333333333333"}};
    protocol::HelloParams parsed;
    protocol::from_json(without_role, parsed);
    EXPECT_EQ(parsed.role, protocol::ClientRole::Supervisor);

    parsed.role = protocol::ClientRole::Automation;
    nlohmann::json json;
    protocol::to_json(json, parsed);
    EXPECT_EQ(json.at("role").get<std::string>(), "automation");
}

TEST(TransportProtocol, RoleProfileDerivationAndMismatchReject) {
    EXPECT_EQ(protocol::profile_for_role(protocol::ClientRole::Automation),
              protocol::ServerProfile::Automation);
    EXPECT_EQ(protocol::profile_for_role(protocol::ClientRole::Supervisor),
              protocol::ServerProfile::Interactive);
    EXPECT_EQ(protocol::profile_for_role(protocol::ClientRole::Observer),
              protocol::ServerProfile::Interactive);

    EXPECT_TRUE(protocol::role_matches_profile(protocol::ClientRole::Automation,
                                               protocol::ServerProfile::Automation));
    EXPECT_TRUE(protocol::role_matches_profile(protocol::ClientRole::Supervisor,
                                               protocol::ServerProfile::Interactive));
    EXPECT_FALSE(protocol::role_matches_profile(protocol::ClientRole::Automation,
                                                protocol::ServerProfile::Interactive));
    EXPECT_FALSE(protocol::role_matches_profile(protocol::ClientRole::Supervisor,
                                                protocol::ServerProfile::Automation));
}

TEST(TransportProtocol, OwnershipViewRoundTrips) {
    protocol::OwnershipView view;
    view.clients = {{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", protocol::ClientRole::Supervisor, 7},
                    {"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", protocol::ClientRole::Automation, 9}};
    view.live_supervisors = 1;
    view.live_automation = 1;
    view.other_fresh_owners = 2;
    view.shutting_down = true;

    nlohmann::json json;
    protocol::to_json(json, view);
    EXPECT_EQ(json.at("live_supervisors").get<std::size_t>(), 1u);
    EXPECT_EQ(json.at("other_fresh_owners").get<std::size_t>(), 2u);
    EXPECT_TRUE(json.at("shutting_down").get<bool>());

    protocol::OwnershipView parsed;
    protocol::from_json(json, parsed);
    ASSERT_EQ(parsed.clients.size(), 2u);
    EXPECT_EQ(parsed.clients[0].role, protocol::ClientRole::Supervisor);
    EXPECT_EQ(parsed.clients[1].role, protocol::ClientRole::Automation);
    EXPECT_EQ(parsed.clients[1].pid, 9);
    EXPECT_EQ(parsed.live_automation, 1u);
    EXPECT_EQ(parsed.other_fresh_owners, 2u);
    EXPECT_TRUE(parsed.shutting_down);
}

} // namespace

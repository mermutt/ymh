#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/mcp/mcp_types.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

TEST(McpLiveStatusTest, WireNamesRoundTrip) {
    using ymh::protocol::HostNoticeKind;
    EXPECT_EQ(ymh::protocol::wire_name(HostNoticeKind::McpServerStatus),
              "mcp_server_status");
    const std::optional<HostNoticeKind> parsed =
        ymh::protocol::parse_host_notice_kind("mcp_server_status");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, HostNoticeKind::McpServerStatus);

    EXPECT_EQ(ymh::wire_name(ymh::EventType::McpServerStatusChanged),
              "mcp/server_status_changed");
    const std::optional<ymh::EventType> type =
        ymh::parse_event_type("mcp/server_status_changed");
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, ymh::EventType::McpServerStatusChanged);
}

TEST(McpLiveStatusTest, PayloadRoundTripsThroughTheBus) {
    ymh::EventBus bus;
    std::optional<ymh::payload::McpServerStatusChanged> received;
    ymh::Subscription subscription =
        bus.subscribe<ymh::payload::McpServerStatusChanged>(
            [&received](const ymh::payload::McpServerStatusChanged& status) {
                received = status;
            });

    ymh::payload::McpServerStatusChanged status;
    status.server.value = "alpha";
    status.state = ymh::McpServerState::Ready;
    status.tool_count = 3;
    status.reason = "ok";

    ymh::TypedEvent<ymh::payload::McpServerStatusChanged> event;
    event.id.value = "evt";
    event.timestamp = std::chrono::system_clock::now();
    event.payload = status;
    bus.publish(event);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->server.value, "alpha");
    EXPECT_EQ(received->state, ymh::McpServerState::Ready);
    EXPECT_EQ(received->tool_count, 3u);
}

TEST(McpLiveStatusTest, HostNoticeRoundTripsThroughJson) {
    ymh::protocol::HostNotice notice;
    notice.kind = ymh::protocol::HostNoticeKind::McpServerStatus;
    notice.workspace.value = "ws";
    notice.detail = "mcp alpha ready tools=2";

    const nlohmann::json json = notice;
    EXPECT_EQ(json.at("kind"), "mcp_server_status");
    const ymh::protocol::HostNotice restored = json.get<ymh::protocol::HostNotice>();
    EXPECT_EQ(restored.kind, ymh::protocol::HostNoticeKind::McpServerStatus);
    EXPECT_EQ(restored.workspace.value, "ws");
    EXPECT_EQ(restored.detail, "mcp alpha ready tools=2");
}

TEST(McpLiveStatusTest, UiProjectsStatusNotice) {
    ymh::ui::UiModel model;
    ymh::ui::UiEventAdapter adapter(model);

    ymh::protocol::HostNotice notice;
    notice.kind = ymh::protocol::HostNoticeKind::McpServerStatus;
    notice.workspace.value = "w";
    notice.detail = "mcp alpha ready tools=2";
    adapter.onHostNotice(ymh::ui::WorkspaceId{"w"}, notice);

    EXPECT_EQ(model.mcp_status, "mcp alpha ready tools=2");
}

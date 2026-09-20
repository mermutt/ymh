#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/context_snapshot.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/status_format.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

nlohmann::json server_json(const std::string& id, const std::string& state, bool connected,
                           std::size_t tool_count, const nlohmann::json& skipped_tools,
                           bool has_error) {
    return nlohmann::json{{"id", id},
                          {"state", state},
                          {"connected", connected},
                          {"tool_count", tool_count},
                          {"skipped", skipped_tools.size()},
                          {"skipped_tools", skipped_tools},
                          {"has_error", has_error}};
}

ContextToolEntry tool(const std::string& name, const std::string& provenance,
                      std::uint64_t tokens) {
    ContextToolEntry entry;
    entry.name = name;
    entry.provenance = provenance;
    entry.schema_tokens = tokens;
    return entry;
}

ContextServerEntry server(const std::string& id, const std::string& state, bool has_error) {
    ContextServerEntry entry;
    entry.id = id;
    entry.state = state;
    entry.has_error = has_error;
    return entry;
}

// 45-D6 §8.2: `/mcp` renders the configured/connected/tool header plus one row
// per server and a bounded skipped continuation.
TEST(StatusFormat, UI45_D6_McpCommandRender) {
    const nlohmann::json result{
        {"servers",
         nlohmann::json::array(
             {server_json("alpha", "ready", true, 3, nlohmann::json::array(), false),
              server_json("beta", "failed", false, 0, nlohmann::json::array(), true),
              server_json("gamma", "disabled", false, 0,
                          nlohmann::json::array({"x", "y"}), false)})},
        {"tool_total", 3}};
    const std::string block = format_mcp_block(result);
    EXPECT_NE(block.find("mcp servers: 3 configured, 1 connected, 3 tools"), std::string::npos);
    EXPECT_NE(block.find("alpha  connected  3 tools"), std::string::npos);
    EXPECT_NE(block.find("beta  not connected  failed"), std::string::npos);
    EXPECT_NE(block.find("gamma  disabled"), std::string::npos);
    EXPECT_NE(block.find("skipped: x, y (skipped=2)"), std::string::npos);
}

// 45-D6.11/45-I27/45-F19: only the two pinned codes degrade; anything else is a
// generic error, and success is not a degradation.
TEST(StatusFormat, UI45_D6_McpMethodNotFoundDegradation) {
    const std::optional<std::string> not_found =
        mcp_unavailable_notice(static_cast<int>(protocol::RpcCode::MethodNotFound));
    ASSERT_TRUE(not_found.has_value());
    EXPECT_EQ(*not_found, "mcp: unavailable (daemon does not support mcp.status)");
    EXPECT_EQ(mcp_unavailable_notice(
                  static_cast<int>(protocol::AppCode::MethodNotAllowedForProfile)),
              not_found);
    EXPECT_FALSE(mcp_unavailable_notice(0).has_value());
    EXPECT_FALSE(mcp_unavailable_notice(static_cast<int>(protocol::RpcCode::InternalError))
                     .has_value());
}

// 45-D7/45-I16: version, model, and the derived api line render in all three
// connectivity states.
TEST(StatusFormat, UI45_D7_StatusRendersVersionModelApi) {
    StatusBlockInputs inputs;
    inputs.version = "0.1.0";
    inputs.model = "deepseek-flash";
    inputs.daemon = DaemonStatus::Attached;
    inputs.workspace_title = "proj";
    inputs.has_session = true;

    inputs.api_state = ApiConnectivity::Unknown;
    std::string block = format_status_block(inputs);
    EXPECT_NE(block.find("version:  0.1.0"), std::string::npos);
    EXPECT_NE(block.find("model:    deepseek-flash"), std::string::npos);
    EXPECT_NE(block.find("api:      unknown"), std::string::npos);
    EXPECT_NE(block.find("daemon:   attached (workspace: proj)"), std::string::npos);

    inputs.api_state = ApiConnectivity::Ok;
    EXPECT_NE(format_status_block(inputs).find("api:      ok"), std::string::npos);

    inputs.api_state = ApiConnectivity::Error;
    inputs.last_error = "401 unauthorized";
    EXPECT_NE(format_status_block(inputs).find("api:      error: 401 unauthorized"),
              std::string::npos);
}

// 45-D7.5/45-I16: per-tool statuses (name, provenance, tokens) and per-server
// states, not just counts.
TEST(StatusFormat, UI45_D7_StatusToolStatuses) {
    ContextSnapshot snapshot;
    snapshot.tools.push_back(tool("read_file", "builtin", 210));
    snapshot.tools.push_back(tool("mcp.alpha.search", "mcp", 180));
    snapshot.mcp_servers.push_back(server("alpha", "ready", false));
    snapshot.mcp_servers.push_back(server("beta", "degraded", true));

    StatusBlockInputs inputs;
    inputs.version = "0.1.0";
    inputs.model = "m";
    inputs.has_session = true;
    inputs.snapshot = &snapshot;
    const std::string block = format_status_block(inputs);
    EXPECT_NE(block.find("tools:    2 available (1 builtin, 1 mcp)"), std::string::npos);
    EXPECT_NE(block.find("read_file (builtin) ~210 tok"), std::string::npos);
    EXPECT_NE(block.find("mcp.alpha.search (mcp) ~180 tok"), std::string::npos);
    EXPECT_NE(block.find("mcp:      1 connected / 2 configured"), std::string::npos);
    EXPECT_NE(block.find("alpha  ready"), std::string::npos);
    EXPECT_NE(block.find("beta  degraded (failed)"), std::string::npos);
}

// 45-D7.5/45-I16: the tool list is bounded to 20 rows plus `… and K more`.
TEST(StatusFormat, UI45_D7_StatusToolListBounded) {
    ContextSnapshot snapshot;
    for (std::size_t index = 0; index < 25; ++index) {
        snapshot.tools.push_back(tool("t" + std::to_string(index), "builtin", 1));
    }
    StatusBlockInputs inputs;
    inputs.has_session = true;
    inputs.snapshot = &snapshot;
    const std::string block = format_status_block(inputs);
    EXPECT_NE(block.find("tools:    25 available (25 builtin, 0 mcp)"), std::string::npos);
    EXPECT_NE(block.find("t19 (builtin)"), std::string::npos);
    EXPECT_EQ(block.find("t20 (builtin)"), std::string::npos);
    EXPECT_NE(block.find("… and 5 more"), std::string::npos);
}

// 45-D7.5: `connected` is derived from `state == "ready"` (ContextServerEntry
// has no `connected` field).
TEST(StatusFormat, UI45_D7_StatusConnectedFromState) {
    ContextSnapshot snapshot;
    snapshot.mcp_servers.push_back(server("alpha", "ready", false));
    snapshot.mcp_servers.push_back(server("beta", "degraded", false));
    StatusBlockInputs inputs;
    inputs.has_session = true;
    inputs.snapshot = &snapshot;
    EXPECT_NE(format_status_block(inputs).find("mcp:      1 connected / 2 configured"),
              std::string::npos);
}

// 45-F10 analogue: no modeled session renders the local lines with a
// `(no session)` note and omits the tool/MCP block.
TEST(StatusFormat, UI45_D7_StatusNoSessionNotice) {
    StatusBlockInputs inputs;
    inputs.version = "0.1.0";
    inputs.model = "deepseek-flash";
    inputs.has_session = false;
    const std::string block = format_status_block(inputs);
    EXPECT_NE(block.find("ymh status"), std::string::npos);
    EXPECT_NE(block.find("tools:    (no session)"), std::string::npos);
    EXPECT_NE(block.find("mcp:      (no session)"), std::string::npos);

    inputs.has_session = true;
    EXPECT_NE(format_status_block(inputs).find("tools:    (unavailable)"), std::string::npos);
}

} // namespace

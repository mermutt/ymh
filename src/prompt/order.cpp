#include "ymh/prompt/order.hpp"

#include <string>
#include <string_view>

#include "ymh/config/config.hpp"

namespace ymh {
namespace {

struct OrderName {
    std::string_view name;
    std::int32_t     order;
};

constexpr OrderName kSectionOrders[] = {
    {"HARNESS_IDENTITY", -1000},
    {"harness:identity", -1000},
    {"DEPLOYMENT_PERSONA_PREFIX", 0},
    {"deployment:persona-prefix", 0},
    {"PLAN_POLICY", 500},
    {"plan:policy", 500},
    {"TEAM_POLICY", 600},
    {"team:policy", 600},
    {"PTC_ONLY", 800},
    {"ptc:only", 800},
    {"FILE_REFERENCE", 900},
    {"file:reference", 900},
    {"TOOL_BASH", 1000},
    {"tool:shell", 1000},
    {"TOOL_PWSH", 1010},
    {"tool:pwsh", 1010},
    {"TOOL_GIT", 1050},
    {"tool:git", 1050},
    {"TOOL_READ", 1100},
    {"tool:read", 1100},
    {"TOOL_WRITE", 1200},
    {"tool:write", 1200},
    {"TOOL_EDIT", 1300},
    {"tool:edit", 1300},
    {"TOOL_GLOB", 1400},
    {"tool:glob", 1400},
    {"TOOL_GREP", 1500},
    {"tool:grep", 1500},
    {"TOOL_JOBS", 1600},
    {"tool:jobs", 1600},
    {"TOOL_PTY", 1700},
    {"tool:pty", 1700},
    {"TOOL_SKILL", 1800},
    {"tool:skill", 1800},
    {"TOOL_PLAN", 1900},
    {"tool:plan", 1900},
    {"TOOL_WEB_SEARCH", 2000},
    {"tool:web-search", 2000},
    {"TOOL_WEB_FETCH", 2100},
    {"tool:web-fetch", 2100},
    {"TOOL_LSP", 2200},
    {"tool:lsp", 2200},
    {"TOOL_SESSION_QUERY", 2300},
    {"tool:session-query", 2300},
    {"TOOL_GOAL", 2400},
    {"tool:goal", 2400},
    {"TOOL_CORDIS", 2500},
    {"tool:cordis", 2500},
    {"TOOL_WORKFLOW", 2600},
    {"tool:workflow", 2600},
    {"TOOL_RALPH", 2700},
    {"tool:ralph", 2700},
    {"TOOL_SUBAGENT", 2800},
    {"tool:subagent", 2800},
    {"TOOL_REPORT", 2900},
    {"tool:report", 2900},
    {"TOOLS_SDK", 5000},
    {"tools:sdk", 5000},
    {"DELIVERABLE_FILE_REFERENCES", 9000},
    {"deliverable:file-references", 9000},
    {"STRUCTURED_OUTPUT", 9900},
    {"structured:output", 9900},
    {"HARNESS_SOURCE", 10000},
    {"harness:source", 10000},
    {"WEB_SURFACE", 10100},
    {"web:surface", 10100},
    {"DEPLOYMENT_PERSONA_SUFFIX", 10200},
    {"deployment:persona-suffix", 10200},
};

constexpr OrderName kContextOrders[] = {
    {"SANDBOX_POLICY", 110},
    {"sandbox:policy", 110},
    {"APPROVAL_POLICY", 115},
    {"approval:policy", 115},
    {"SUBAGENT_DELEGATION", 120},
    {"subagent:delegation", 120},
};

} // namespace

std::int32_t section_order(std::string_view name) {
    for (const OrderName& entry : kSectionOrders) {
        if (entry.name == name) {
            return entry.order;
        }
    }
    throw ConfigError("unknown prompt section order name '" + std::string{name} + "'");
}

std::int32_t context_order(std::string_view name) {
    for (const OrderName& entry : kContextOrders) {
        if (entry.name == name) {
            return entry.order;
        }
    }
    throw ConfigError("unknown prompt context order name '" + std::string{name} + "'");
}

} // namespace ymh

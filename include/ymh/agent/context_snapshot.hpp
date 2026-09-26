#pragma once

// Read-only assembled-context inspection (18-context-errata.md). Pure: it takes
// a resolved session view (header + events), the effective system prompt, the
// frozen tool schemas, and an MCP status snapshot, and returns advisory token
// estimates per segment. It never reads a live `Session`, never appends, never
// logs content, and never calls a provider.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/compactor.hpp"          // CompactionPolicy
#include "ymh/agent/context_assembler.hpp"  // TokenEstimator
#include "ymh/agent/message.hpp"            // Message, Role
#include "ymh/mcp/mcp_types.hpp"            // McpServerStatus
#include "ymh/session/session.hpp"          // SessionHeader, EventRange
#include "ymh/tools/tool.hpp"               // ToolSchema

namespace ymh {

// Fixed order; `FreeSpace` is always last (18 §5.2).
enum class ContextSegmentKind : std::uint8_t {
    SystemPrompt,
    ToolSchemas,
    McpToolSchemas,
    Conversation,
    CompactionSummary,
    FreeSpace,
};

[[nodiscard]] std::string_view context_segment_token(ContextSegmentKind kind) noexcept;
[[nodiscard]] std::optional<ContextSegmentKind> parse_context_segment(
    std::string_view token) noexcept;

struct ContextSegment {
    ContextSegmentKind kind = ContextSegmentKind::Conversation;
    std::string        provenance;   // human-readable origin label
    std::uint64_t      tokens = 0;   // advisory estimate
    std::size_t        items = 0;    // messages / schemas / summaries
};

struct ContextToolEntry {
    std::string   name;              // e.g. "read_file" or "mcp.alpha.search"
    std::string   provenance;        // "builtin" | "mcp"
    std::uint64_t schema_tokens = 0; // advisory estimate
};

struct ContextServerEntry {
    std::string id;
    std::string state;               // mcp_state_token()
    std::size_t tool_count = 0;
    std::size_t skipped = 0;
    // M8: the raw `McpServerStatus::last_error` is NEVER shipped (unbounded
    // std::exception::what(); see §6.2/CTX8). Only this bool is.
    bool        has_error = false;
};

struct ContextBudget {
    std::uint64_t window_tokens = 0;              // 0 => unknown
    std::uint64_t reserve_output_tokens = 0;
    std::uint64_t effective_threshold_tokens = 0;
};

// All references must outlive the call. Callers bind vectors to named locals
// first (never to temporaries — see 18 §3.3 note).
struct ContextSnapshotInputs {
    const SessionHeader&                header;
    const EventRange&                   events;         // resolved view (store read)
    std::string_view                    system_prompt;  // effective, may be empty
    const std::vector<ToolSchema>&      tools;          // frozen schemas
    const std::vector<McpServerStatus>& mcp_servers;
    const TokenEstimator&               estimator;
    ContextBudget                       budget;
    std::size_t                         max_tools = 256;
    // False when the daemon could not read MCP status (CTX-F6): the builder
    // omits the server section and records the degradation in `note`.
    bool                                mcp_available = true;
};

struct ContextSnapshot {
    SessionId                       session;         // = header.id (wire `session`)
    std::uint64_t                   used_tokens = 0;
    ContextBudget                   budget;
    std::vector<ContextSegment>     segments;        // fixed order, FreeSpace last
    std::vector<ContextToolEntry>   tools;
    std::vector<ContextServerEntry> mcp_servers;
    std::uint64_t                   captured_sequence = 0;  // last resolved seq
    bool                            truncated = false;
    std::string                     note;            // e.g. "budget unknown"
};

// Pure. `events` is the resolved session view; `header` its SessionHeader.
[[nodiscard]] ContextSnapshot build_context_snapshot(const ContextSnapshotInputs& inputs);

// Advisory size of one schema in the OpenAI-compatible wire form
// (openai_adapter.cpp:714-724): name + description + parameters. Not noexcept:
// `nlohmann::json::dump()` may throw on invalid UTF-8.
[[nodiscard]] std::uint64_t estimate_tool_schema_tokens(const ToolSchema& schema);

void to_json(nlohmann::json& json, const ContextSnapshot& snapshot);
void from_json(const nlohmann::json& json, ContextSnapshot& snapshot);

} // namespace ymh

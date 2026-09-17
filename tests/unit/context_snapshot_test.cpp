#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/context_snapshot.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/mcp/mcp_types.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;

const SessionId kSession{"context-session"};

EventRecord make_record(Sequence seq, EventType type, nlohmann::json payload) {
    EventRecord record;
    record.seq = seq;
    record.event.id = EventId{"event-" + std::to_string(seq)};
    record.event.session_id = kSession;
    record.event.timestamp = std::chrono::system_clock::now();
    record.event.type = type;
    record.event.payload = std::move(payload);
    return record;
}

EventRecord user_message(Sequence seq, std::string text) {
    payload::UserMessage message;
    message.id = "m" + std::to_string(seq);
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return make_record(seq, EventType::UserMessage, nlohmann::json(message));
}

EventRecord compaction(Sequence seq, Sequence boundary, std::string summary) {
    payload::ContextCompaction value;
    value.boundary = boundary;
    value.summary = std::move(summary);
    return make_record(seq, EventType::ContextCompaction, nlohmann::json(value));
}

ToolSchema schema(std::string name, std::string description) {
    ToolSchema tool;
    tool.name.value = std::move(name);
    tool.description = std::move(description);
    tool.input_schema = nlohmann::json::parse(
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})");
    return tool;
}

SessionHeader header() {
    SessionHeader value;
    value.id = kSession;
    value.cwd = "/work";
    return value;
}

DefaultTokenEstimator estimator;

ContextSnapshot build(const EventRange& events, std::vector<ToolSchema> tools,
                      std::vector<McpServerStatus> servers, ContextBudget budget,
                      std::string_view prompt = "system prompt", bool mcp_available = true,
                      std::size_t max_tools = 256) {
    const SessionHeader value = header();
    const ContextSnapshotInputs inputs{value,   events,        prompt, tools,
                                       servers, estimator,     budget, max_tools,
                                       mcp_available};
    return build_context_snapshot(inputs);
}

const ContextSegment& segment(const ContextSnapshot& snapshot, ContextSegmentKind kind) {
    for (const ContextSegment& candidate : snapshot.segments) {
        if (candidate.kind == kind) {
            return candidate;
        }
    }
    throw std::runtime_error("segment missing");
}

TEST(ContextSnapshot, SchemaEstimateMatchesPinnedFixture) {
    const ToolSchema tool = schema("read_file", "Read a file");
    EXPECT_EQ(tool.input_schema.dump().size(), 77u);
    const std::size_t bytes =
        tool.name.value.size() + tool.description.size() + tool.input_schema.dump().size();
    EXPECT_EQ(bytes, 97u);
    EXPECT_EQ(estimate_tool_schema_tokens(tool), 32u);
    EXPECT_EQ(estimate_tool_schema_tokens(tool), bytes / 4 + 8);
    EXPECT_EQ(estimate_tool_schema_tokens(tool), estimate_tool_schema_tokens(tool));
}

TEST(ContextSnapshot, SchemaPartitionAndToolList) {
    std::vector<ToolSchema> tools{schema("read_file", "Read a file"),
                                  schema("shell", "Run a shell command"),
                                  schema("mcp.alpha.x", "Alpha x"),
                                  schema("mcp.beta.y", "Beta y")};
    const ContextSnapshot snapshot = build({}, std::move(tools), {}, ContextBudget{100000, 0, 0});
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::ToolSchemas).items, 2u);
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::McpToolSchemas).items, 2u);
    EXPECT_EQ(snapshot.tools.size(), 4u);
    EXPECT_EQ(snapshot.tools[0].name, "read_file");
    EXPECT_EQ(snapshot.tools[0].provenance, "builtin");
    EXPECT_EQ(snapshot.tools[2].name, "mcp.alpha.x");
    EXPECT_EQ(snapshot.tools[2].provenance, "mcp");
    EXPECT_EQ(snapshot.tools[3].provenance, "mcp");
    EXPECT_EQ(snapshot.tools[0].schema_tokens, 32u);
    for (const ContextToolEntry& entry : snapshot.tools) {
        EXPECT_FALSE(entry.name.empty());
    }
}

TEST(ContextSnapshot, McpServerListIsBoundedAndRedacted) {
    McpServerStatus beta;
    beta.id.value = "beta";
    beta.state = McpServerState::Ready;
    beta.tool_count = 3;
    beta.skipped_tools = {"x", "y"};
    McpServerStatus alpha;
    alpha.id.value = "alpha";
    alpha.state = McpServerState::Degraded;
    alpha.tool_count = 1;
    alpha.last_error = "SECRET-ERROR-TEXT";
    const ContextSnapshot snapshot =
        build({}, {}, {alpha, beta}, ContextBudget{100000, 0, 0});
    ASSERT_EQ(snapshot.mcp_servers.size(), 2u);
    EXPECT_EQ(snapshot.mcp_servers[0].id, "alpha");
    EXPECT_EQ(snapshot.mcp_servers[0].state, "degraded");
    EXPECT_EQ(snapshot.mcp_servers[0].tool_count, 1u);
    EXPECT_EQ(snapshot.mcp_servers[0].skipped, 0u);
    EXPECT_TRUE(snapshot.mcp_servers[0].has_error);
    EXPECT_EQ(snapshot.mcp_servers[1].state, "ready");
    EXPECT_EQ(snapshot.mcp_servers[1].skipped, 2u);
    EXPECT_FALSE(snapshot.mcp_servers[1].has_error);
    nlohmann::json json;
    to_json(json, snapshot);
    EXPECT_EQ(json.dump().find("SECRET-ERROR-TEXT"), std::string::npos);
}

TEST(ContextSnapshot, SummaryFoldUsesLastSummaryAsHead) {
    EventRange events{user_message(1, "old"), compaction(2, 1, "S1"), user_message(3, "keep"),
                      compaction(4, 2, "S2")};
    const ContextSnapshot snapshot = build(events, {}, {}, ContextBudget{100000, 0, 0});
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::CompactionSummary).items, 1u);
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::Conversation).items, 1u);
    const std::vector<Message> tail = {[] {
        Message message;
        message.role = Role::User;
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = "keep";
        message.content.push_back(std::move(block));
        return message;
    }()};
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::Conversation).tokens, estimator.estimate(tail));
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::CompactionSummary).tokens,
              estimator.estimate({[] {
                  Message message;
                  message.role = Role::System;
                  ContentBlock block;
                  block.kind = ContentBlockKind::Text;
                  block.text = "S2";
                  message.content.push_back(std::move(block));
                  return message;
              }()}));
}

TEST(ContextSnapshot, SystemPromptContributesOnlyWhenNonEmpty) {
    const ContextSnapshot empty = build({}, {}, {}, ContextBudget{1000, 0, 0}, "");
    EXPECT_EQ(segment(empty, ContextSegmentKind::SystemPrompt).items, 0u);
    EXPECT_EQ(segment(empty, ContextSegmentKind::SystemPrompt).tokens, 0u);
    const ContextSnapshot present = build({}, {}, {}, ContextBudget{1000, 0, 0}, "abc");
    EXPECT_EQ(segment(present, ContextSegmentKind::SystemPrompt).items, 1u);
    EXPECT_GT(segment(present, ContextSegmentKind::SystemPrompt).tokens, 0u);
}

TEST(ContextSnapshot, BudgetUnknownRecordsNoteAndNoFreeSpace) {
    const ContextSnapshot snapshot = build({user_message(1, "hello")}, {}, {},
                                           ContextBudget{0, 0, 0});
    EXPECT_EQ(snapshot.note, "budget unknown");
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::FreeSpace).tokens, 0u);
    EXPECT_EQ(snapshot.used_tokens, segment(snapshot, ContextSegmentKind::SystemPrompt).tokens +
                                        segment(snapshot, ContextSegmentKind::ToolSchemas).tokens +
                                        segment(snapshot, ContextSegmentKind::McpToolSchemas).tokens +
                                        segment(snapshot, ContextSegmentKind::Conversation).tokens +
                                        segment(snapshot, ContextSegmentKind::CompactionSummary).tokens);
}

TEST(ContextSnapshot, NoteJoinIsExact) {
    std::vector<ToolSchema> tools{schema("a", "a"), schema("b", "b"), schema("c", "c")};
    const ContextSnapshot snapshot = build({}, std::move(tools), {}, ContextBudget{0, 0, 0}, "p",
                                           false, 2);
    EXPECT_EQ(snapshot.note, "budget unknown; tools truncated; mcp status unavailable");
    EXPECT_TRUE(snapshot.truncated);
    EXPECT_EQ(snapshot.tools.size(), 2u);
    EXPECT_TRUE(snapshot.mcp_servers.empty());
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::ToolSchemas).items, 3u);
}

TEST(ContextSnapshot, OverBudgetHasNoFreeSpaceButKeepsUsed) {
    const ContextSnapshot snapshot =
        build({user_message(1, std::string(800, 'x'))}, {}, {}, ContextBudget{10, 0, 0});
    EXPECT_GT(snapshot.used_tokens, 10u);
    EXPECT_EQ(segment(snapshot, ContextSegmentKind::FreeSpace).tokens, 0u);
}

TEST(ContextSnapshot, McpUnavailableOmitsSection) {
    McpServerStatus status;
    status.id.value = "alpha";
    status.state = McpServerState::Ready;
    const ContextSnapshot snapshot =
        build({}, {}, {status}, ContextBudget{1000, 0, 0}, "p", false);
    EXPECT_TRUE(snapshot.mcp_servers.empty());
    EXPECT_NE(snapshot.note.find("mcp status unavailable"), std::string::npos);
}

TEST(ContextSnapshot, SegmentTokenRoundTrip) {
    for (const ContextSegmentKind kind : {ContextSegmentKind::SystemPrompt,
                                          ContextSegmentKind::ToolSchemas,
                                          ContextSegmentKind::McpToolSchemas,
                                          ContextSegmentKind::Conversation,
                                          ContextSegmentKind::CompactionSummary,
                                          ContextSegmentKind::FreeSpace}) {
        const std::optional<ContextSegmentKind> parsed =
            parse_context_segment(context_segment_token(kind));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
    EXPECT_FALSE(parse_context_segment("nope").has_value());
}

TEST(ContextSnapshot, SerializationOmitsContentAndRoundTrips) {
    EventRange events{user_message(1, "SENTINEL-MESSAGE-BODY")};
    const ContextSnapshot snapshot =
        build(events, {schema("read_file", "Read a file")}, {}, ContextBudget{100000, 4096, 76723},
              "SENTINEL-SYSTEM-PROMPT");
    nlohmann::json json;
    to_json(json, snapshot);
    const std::string dumped = json.dump();
    EXPECT_EQ(dumped.find("SENTINEL"), std::string::npos);
    ContextSnapshot parsed;
    from_json(json, parsed);
    EXPECT_EQ(parsed.session.value, snapshot.session.value);
    EXPECT_EQ(parsed.used_tokens, snapshot.used_tokens);
    EXPECT_EQ(parsed.captured_sequence, snapshot.captured_sequence);
    EXPECT_EQ(parsed.note, snapshot.note);
    EXPECT_EQ(parsed.truncated, snapshot.truncated);
    ASSERT_EQ(parsed.segments.size(), snapshot.segments.size());
    for (std::size_t index = 0; index < parsed.segments.size(); ++index) {
        EXPECT_EQ(parsed.segments[index].kind, snapshot.segments[index].kind);
        EXPECT_EQ(parsed.segments[index].tokens, snapshot.segments[index].tokens);
        EXPECT_EQ(parsed.segments[index].items, snapshot.segments[index].items);
    }
}

TEST(ContextSnapshot, DeterminismIsStable) {
    EventRange events{user_message(1, "hello")};
    const ContextSnapshot first = build(events, {}, {}, ContextBudget{1000, 0, 0});
    const ContextSnapshot second = build(events, {}, {}, ContextBudget{1000, 0, 0});
    nlohmann::json left;
    nlohmann::json right;
    to_json(left, first);
    to_json(right, second);
    EXPECT_EQ(left, right);
}

TEST(ContextGridGeometry, PinnedSizes) {
    const ymh::ui::ContextGridGeometry small = ymh::ui::context_grid_geometry(32, 8);
    EXPECT_LT(small.cols, 40);
    const ymh::ui::ContextGridGeometry golden = ymh::ui::context_grid_geometry(66, 20);
    EXPECT_EQ(golden.cols, 64);
    EXPECT_EQ(golden.rows, 10);
    const ymh::ui::ContextGridGeometry wide = ymh::ui::context_grid_geometry(100, 30);
    EXPECT_EQ(wide.cols, 72);
    EXPECT_EQ(wide.rows, 12);
}

ContextSnapshot worked_example() {
    ContextSnapshot snapshot;
    snapshot.session = kSession;
    snapshot.used_tokens = 41300;
    snapshot.budget = ContextBudget{64000, 4096, 47923};
    snapshot.captured_sequence = 1842;
    snapshot.segments = {
        ContextSegment{ContextSegmentKind::SystemPrompt, "agent.system_prompt", 412, 1},
        ContextSegment{ContextSegmentKind::ToolSchemas, "ToolRegistry::schemas()", 3980, 2},
        ContextSegment{ContextSegmentKind::McpToolSchemas, "ToolRegistry::schemas() (mcp.)", 2240, 2},
        ContextSegment{ContextSegmentKind::Conversation, "deriveMessages(header, log)", 33540, 42},
        ContextSegment{ContextSegmentKind::CompactionSummary, "payload::ContextCompaction.summary",
                       1128, 1},
        ContextSegment{ContextSegmentKind::FreeSpace, "window − used", 22700, 0},
    };
    return snapshot;
}

TEST(ContextCellMapping, WorkedExampleBoundaries) {
    const ContextSnapshot snapshot = worked_example();
    constexpr int kCells = 640;
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 0),
              ContextSegmentKind::SystemPrompt);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 4),
              ContextSegmentKind::SystemPrompt);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 5),
              ContextSegmentKind::ToolSchemas);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 43),
              ContextSegmentKind::ToolSchemas);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 44),
              ContextSegmentKind::McpToolSchemas);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 66),
              ContextSegmentKind::McpToolSchemas);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 67),
              ContextSegmentKind::Conversation);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 401),
              ContextSegmentKind::Conversation);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 402),
              ContextSegmentKind::CompactionSummary);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 412),
              ContextSegmentKind::CompactionSummary);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 413),
              ContextSegmentKind::FreeSpace);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, kCells, 639),
              ContextSegmentKind::FreeSpace);
}

TEST(ContextCellMapping, DegenerateInputsAreFreeSpace) {
    const ContextSnapshot snapshot = worked_example();
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, 0, 0), ContextSegmentKind::FreeSpace);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, 640, -1), ContextSegmentKind::FreeSpace);
    EXPECT_EQ(ymh::ui::context_cell_kind(snapshot, 640, 640), ContextSegmentKind::FreeSpace);
    ContextSnapshot unknown = worked_example();
    unknown.budget.window_tokens = 0;
    unknown.used_tokens = 0;
    EXPECT_EQ(ymh::ui::context_cell_kind(unknown, 640, 0), ContextSegmentKind::FreeSpace);
}

} // namespace

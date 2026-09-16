#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/mcp/schema_translation.hpp"

namespace {

using ymh::McpErrorCode;
using ymh::McpRemoteToolName;
using ymh::McpServerConfig;
using ymh::McpServerId;

ymh::ToolSchema translate(const nlohmann::json& schema,
                          std::vector<std::string>* notes = nullptr) {
    const auto translated = ymh::translate_mcp_schema(
        McpServerId{"srv"}, McpRemoteToolName{"do_thing"}, schema, 4096, notes);
    EXPECT_TRUE(translated.has_value());
    return translated.value();
}

} // namespace

TEST(McpSanitizeTest, Table) {
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("ReadFile"), "readfile");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("read-file"), "read_file");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("read.file"), "read_file");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("read file"), "read_file");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("9lives"), "m_9lives");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("a__b___c"), "a_b_c");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("trailing__"), "trailing");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment(""), "tool");
    EXPECT_EQ(ymh::sanitize_mcp_tool_segment("héllo"), "h_llo");
}

TEST(McpSchemaTest, SupportedSchemaIsPinnedSubset) {
    nlohmann::json schema = {
        {"type", "object"},
        {"description", "does a thing"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "the name"}}},
          {"count", {{"type", "integer"}, {"default", 1}, {"enum", {1, 2, 3}}}}}},
        {"required", {"name"}},
    };
    const ymh::ToolSchema translated = translate(schema);
    EXPECT_EQ(translated.name.value, "mcp.srv.do_thing");
    EXPECT_EQ(translated.description, "does a thing");
    EXPECT_FALSE(translated.input_schema["additionalProperties"].get<bool>());
    EXPECT_EQ(translated.input_schema["properties"]["name"]["type"], "string");
    EXPECT_EQ(translated.input_schema["required"], nlohmann::json::array({"name"}));
    EXPECT_NO_THROW(ymh::validate_input_schema(translated.input_schema));
}

TEST(McpSchemaTest, NestedObjectAndArrayPreserved) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"nested",
           {{"type", "object"},
            {"properties", {{"x", {{"type", "boolean"}}}}},
            {"additionalProperties", false}}},
          {"list", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
    };
    const ymh::ToolSchema translated = translate(schema);
    EXPECT_EQ(translated.input_schema["properties"]["nested"]["properties"]["x"]["type"],
              "boolean");
    EXPECT_EQ(translated.input_schema["properties"]["list"]["items"]["type"], "string");
    EXPECT_NO_THROW(ymh::validate_input_schema(translated.input_schema));
}

TEST(McpSchemaTest, OpenAdditionalPropertiesIsIncompatible) {
    nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", true},
        {"properties", nlohmann::json::object()},
    };
    const auto translated = ymh::translate_mcp_schema(
        McpServerId{"srv"}, McpRemoteToolName{"x"}, schema, 4096, nullptr);
    ASSERT_FALSE(translated.has_value());
    EXPECT_EQ(translated.error(), McpErrorCode::SchemaIncompatible);
}

TEST(McpSchemaTest, UnsupportedKeywordsAreIncompatible) {
    for (const char* keyword : {"$ref", "anyOf", "oneOf", "allOf", "not",
                                "patternProperties", "const"}) {
        nlohmann::json schema = {
            {"type", "object"},
            {"properties", {{"x", {{"type", "string"}, {keyword, true}}}}},
        };
        const auto translated = ymh::translate_mcp_schema(
            McpServerId{"srv"}, McpRemoteToolName{"x"}, schema, 4096, nullptr);
        EXPECT_FALSE(translated.has_value()) << keyword;
    }
}

TEST(McpSchemaTest, UnionTypeIsIncompatible) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"x", {{"type", nlohmann::json::array({"string", "null"})}}}}},
    };
    const auto translated = ymh::translate_mcp_schema(
        McpServerId{"srv"}, McpRemoteToolName{"x"}, schema, 4096, nullptr);
    ASSERT_FALSE(translated.has_value());
}

TEST(McpSchemaTest, NonObjectTopLevelIsIncompatible) {
    nlohmann::json schema = {{"type", "string"}};
    const auto translated = ymh::translate_mcp_schema(
        McpServerId{"srv"}, McpRemoteToolName{"x"}, schema, 4096, nullptr);
    ASSERT_FALSE(translated.has_value());
}

TEST(McpSchemaTest, DescriptionIsTruncatedAndRecorded) {
    nlohmann::json schema = {
        {"type", "object"},
        {"description", std::string(100, 'a')},
        {"properties", nlohmann::json::object()},
    };
    std::vector<std::string> notes;
    const auto translated = ymh::translate_mcp_schema(
        McpServerId{"srv"}, McpRemoteToolName{"x"}, schema, 10, &notes);
    ASSERT_TRUE(translated.has_value());
    EXPECT_EQ(translated->description.size(), 10u);
}

TEST(McpSchemaTest, AdditionalPropertiesAbsentIsTightenedAndNoted) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", nlohmann::json::object()},
    };
    std::vector<std::string> notes;
    (void)translate(schema, &notes);
    EXPECT_FALSE(notes.empty());
}

TEST(McpSchemaTest, RequiredNameNotInPropertiesIsDropped) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"a", {{"type", "string"}}}}},
        {"required", {"a", "missing"}},
    };
    std::vector<std::string> notes;
    const ymh::ToolSchema translated = translate(schema, &notes);
    EXPECT_EQ(translated.input_schema["required"], nlohmann::json::array({"a"}));
    EXPECT_FALSE(notes.empty());
}

TEST(McpResultTest, TextBlocksJoinInOrder) {
    nlohmann::json content = nlohmann::json::array({
        {{"type", "text"}, {"text", "one"}},
        {{"type", "text"}, {"text", "two"}},
    });
    EXPECT_EQ(ymh::project_mcp_content(content, std::nullopt), "one\ntwo");
}

TEST(McpResultTest, BinaryBlocksBecomePlaceholders) {
    nlohmann::json content = nlohmann::json::array({
        {{"type", "image"}, {"mimeType", "image/png"}, {"data", "AAAA"}},
        {{"type", "audio"}, {"mimeType", "audio/wav"}, {"data", "AAAAAA=="}},
        {{"type", "resource"}, {"resource", {{"uri", "file:///x"}, {"text", "hi"}}}},
        {{"type", "weird"}},
    });
    const std::string output = ymh::project_mcp_content(content, std::nullopt);
    EXPECT_NE(output.find("[image image/png 3 bytes]"), std::string::npos);
    EXPECT_NE(output.find("[audio audio/wav 4 bytes]"), std::string::npos);
    EXPECT_NE(output.find("[resource file:///x]"), std::string::npos);
    EXPECT_NE(output.find("hi"), std::string::npos);
    EXPECT_NE(output.find("[weird "), std::string::npos);
    EXPECT_EQ(output.find("AAAA"), std::string::npos);
}

TEST(McpResultTest, StructuredContentIsFenced) {
    nlohmann::json structured = {{"answer", 42}};
    const std::string output =
        ymh::project_mcp_content(nlohmann::json::array(), structured);
    EXPECT_NE(output.find("```json"), std::string::npos);
    EXPECT_NE(output.find("\"answer\":42"), std::string::npos);
}

TEST(McpResultTest, EmptyContentIsEmpty) {
    EXPECT_EQ(ymh::project_mcp_content(nlohmann::json::array(), std::nullopt), "");
}

TEST(McpToolFilterTest, DenyWinsAndEmptyAllowAdmitsAll) {
    McpServerConfig server;
    EXPECT_TRUE(ymh::mcp_tool_allowed(server, "anything"));
    server.denied_tools = {"write_*"};
    EXPECT_FALSE(ymh::mcp_tool_allowed(server, "write_file"));
    EXPECT_TRUE(ymh::mcp_tool_allowed(server, "read_file"));
    server.allowed_tools = {"read_*"};
    EXPECT_TRUE(ymh::mcp_tool_allowed(server, "read_file"));
    EXPECT_FALSE(ymh::mcp_tool_allowed(server, "list_file"));
}

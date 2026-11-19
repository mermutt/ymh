#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_support.hpp"

#include "ymh/agent/message.hpp"
#include "ymh/agent/prompt_images.hpp"
#include "ymh/core/event.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using ymh::ContentBlock;
using ymh::ContentBlockKind;
using ymh::EventRecord;
using ymh::EventRange;
using ymh::EventType;
using ymh::Message;
using ymh::Role;
using ymh::Sequence;
using ymh_test::event_for;
using ymh_test::fresh_uuid;
using ymh_test::root_header;

std::string png_bytes(std::size_t payload = 8) {
    std::string bytes{"\x89PNG\r\n\x1a\n", 8};
    bytes.append(payload, 'x');
    return bytes;
}

std::string jpeg_bytes() {
    return std::string{"\xFF\xD8\xFF\xE0", 4} + "jpeg-payload";
}

std::string gif_bytes() {
    return std::string{"GIF89a", 6} + "gif-payload";
}

std::string webp_bytes() {
    std::string bytes{"RIFF", 4};
    bytes.append("\x10\x00\x00\x00", 4);
    bytes.append("WEBP", 4);
    bytes.append("webp-payload");
    return bytes;
}

EventRecord record(Sequence seq, const std::string& session, EventType type,
                   nlohmann::json payload) {
    return EventRecord{seq, event_for(session, type, std::move(payload))};
}

nlohmann::json assistant_tool_call(const std::string& message_id,
                                   const std::string& call_id) {
    return nlohmann::json{
        {"id", message_id},
        {"content", nlohmann::json::array({nlohmann::json{
            {"kind", "tool_use"},
            {"tool_call_id", call_id},
            {"tool_name", "read_image"},
            {"arguments", nlohmann::json::object()},
        }})},
    };
}

nlohmann::json tool_result(const std::string& call_id, bool with_images) {
    nlohmann::json payload{
        {"id", call_id},
        {"name", "read_image"},
        {"outcome", "ok"},
        {"output", "attached image shot.png (image/png, 16 bytes)"},
    };
    if (with_images) {
        payload["images"] = nlohmann::json::array({nlohmann::json{
            {"kind", "image"},
            {"media_type", "image/png"},
            {"data", "iVBORw0KGgo="},
        }});
    }
    return payload;
}

std::vector<Message> project(bool with_images) {
    ymh_test::TempWorkspace workspace;
    const std::string        id     = fresh_uuid();
    const auto               header = root_header(workspace.root, id);

    EventRange events;
    events.push_back(record(1, id, EventType::SessionStarted,
                            ymh_test::started_payload()));
    events.push_back(record(2, id, EventType::TurnStarted,
                            nlohmann::json{{"turn", 1}, {"origin", "user"}}));
    events.push_back(record(3, id, EventType::StepStarted,
                            nlohmann::json{{"turn", 1}, {"step", 1}}));
    events.push_back(
        record(4, id, EventType::AssistantMessage, assistant_tool_call("m1", "call-1")));
    events.push_back(record(5, id, EventType::ToolResult, tool_result("call-1", with_images)));
    events.push_back(record(6, id, EventType::TurnEnded, nlohmann::json{{"turn", 1}}));
    return ymh::deriveMessages(header, events);
}

} // namespace

TEST(ToolImages, EncodeImageDetectsPng) {
    std::string reason;
    const auto  encoded = ymh::encode_image(png_bytes(), 1024, reason);
    ASSERT_TRUE(encoded.has_value()) << reason;
    EXPECT_EQ(encoded->media_type, "image/png");
    EXPECT_EQ(encoded->data.size(), 24U);  // ((16 + 2) / 3) * 4
    EXPECT_TRUE(reason.empty());
}

TEST(ToolImages, EncodeImageDetectsJpegGifAndWebp) {
    std::string reason;
    const auto  jpeg = ymh::encode_image(jpeg_bytes(), 1024, reason);
    ASSERT_TRUE(jpeg.has_value());
    EXPECT_EQ(jpeg->media_type, "image/jpeg");

    const auto gif = ymh::encode_image(gif_bytes(), 1024, reason);
    ASSERT_TRUE(gif.has_value());
    EXPECT_EQ(gif->media_type, "image/gif");

    const auto webp = ymh::encode_image(webp_bytes(), 1024, reason);
    ASSERT_TRUE(webp.has_value());
    EXPECT_EQ(webp->media_type, "image/webp");
}

TEST(ToolImages, EncodeImageRejectsText) {
    std::string reason;
    const auto  encoded = ymh::encode_image("not an image at all", 1024, reason);
    EXPECT_FALSE(encoded.has_value());
    EXPECT_EQ(reason, "unsupported image format");
}

TEST(ToolImages, EncodeImageEnforcesMaxBytes) {
    const std::string bytes = png_bytes(64);
    std::string       reason;
    const auto        encoded = ymh::encode_image(bytes, 16, reason);
    EXPECT_FALSE(encoded.has_value());
    EXPECT_NE(reason.find("limit"), std::string::npos);
}

TEST(ToolImages, ToolResultJsonRoundTripsImages) {
    ymh::payload::ToolResult result;
    result.id     = ymh::ToolCallId{"call-1"};
    result.name   = "read_image";
    result.output = "attached image shot.png (image/png, 16 bytes)";
    ContentBlock image;
    image.kind       = ContentBlockKind::Image;
    image.media_type = "image/png";
    image.data       = "iVBORw0KGgo=";
    result.images.push_back(image);

    const nlohmann::json json = result;
    ASSERT_TRUE(json.contains("images"));
    ASSERT_EQ(json.at("images").size(), 1U);

    const ymh::payload::ToolResult decoded = json.get<ymh::payload::ToolResult>();
    ASSERT_EQ(decoded.images.size(), 1U);
    EXPECT_EQ(decoded.images[0].kind, ContentBlockKind::Image);
    EXPECT_EQ(decoded.images[0].media_type, "image/png");
    EXPECT_EQ(decoded.images[0].data, image.data);
}

TEST(ToolImages, EmptyImagesAreNotSerialized) {
    ymh::payload::ToolResult result;
    result.id     = ymh::ToolCallId{"call-1"};
    result.name   = "read_file";
    result.output = "contents";

    const nlohmann::json json = result;
    EXPECT_FALSE(json.contains("images"));

    const ymh::payload::ToolResult decoded = json.get<ymh::payload::ToolResult>();
    EXPECT_TRUE(decoded.images.empty());
}

TEST(ToolImages, ProjectionAddsUserMessageAfterToolResultImages) {
    const std::vector<Message> messages = project(/*with_images=*/true);
    ASSERT_EQ(messages.size(), 3U);

    EXPECT_EQ(messages[0].role, Role::Assistant);
    EXPECT_EQ(messages[1].role, Role::Tool);
    EXPECT_EQ(messages[1].tool_call_id, "call-1");
    ASSERT_EQ(messages[2].role, Role::User);
    ASSERT_EQ(messages[2].content.size(), 2U);

    EXPECT_EQ(messages[2].content[0].kind, ContentBlockKind::Text);
    EXPECT_EQ(messages[2].content[0].text, "[image from tool read_image]");
    EXPECT_EQ(messages[2].content[1].kind, ContentBlockKind::Image);
    EXPECT_EQ(messages[2].content[1].media_type, "image/png");
    EXPECT_EQ(messages[2].content[1].data, "iVBORw0KGgo=");
}

TEST(ToolImages, ProjectionWithoutImagesAddsNoUserMessage) {
    const std::vector<Message> messages = project(/*with_images=*/false);
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].role, Role::Assistant);
    EXPECT_EQ(messages[1].role, Role::Tool);
}

TEST(ToolImages, BuiltinToolsIncludeReadImage) {
    ymh::ToolConfig                         config;
    std::vector<std::unique_ptr<ymh::Tool>> tools = ymh::make_builtin_tools(config);
    const ymh::Tool* read_image = nullptr;
    for (const auto& tool : tools) {
        if (tool->schema().name.value == "read_image") {
            read_image = tool.get();
            break;
        }
    }
    ASSERT_NE(read_image, nullptr);
    const ymh::ToolSchema schema = read_image->schema();
    EXPECT_FALSE(schema.destructive);
    EXPECT_EQ(schema.concurrency, ymh::ToolConcurrencyMode::Exclusive);
    ASSERT_TRUE(schema.input_schema.contains("required"));
    EXPECT_EQ(schema.input_schema.at("required"),
              nlohmann::json::array({"path"}));
}

TEST(ToolImages, FilesystemReadHonoursExplicitMaxBytes) {
    ymh_test::TempWorkspace workspace;
    ymh::LocalEnvironment   environment{workspace.root};

    const std::filesystem::path path = workspace.root / "blob.bin";
    {
        std::ofstream stream{path, std::ios::binary};
        stream << std::string(64, 'x');
    }

    const ymh::FileData all = environment.fs().read(path, 1024).get();
    EXPECT_FALSE(all.truncated);
    EXPECT_EQ(all.bytes.size(), 64U);

    const ymh::FileData capped = environment.fs().read(path, 16).get();
    EXPECT_TRUE(capped.truncated);
    EXPECT_EQ(capped.bytes.size(), 16U);
}

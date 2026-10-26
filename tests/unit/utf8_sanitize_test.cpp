#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

#include "ymh/core/text.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/tool_result_text.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;

TEST(Utf8Sanitize, ValidTextIsUnchanged) {
    for (const std::string& text :
         {std::string{"hello\n"}, std::string{"héllo"}, std::string{"日本語"},
          std::string{"emoji \xF0\x9F\x99\x82"}}) {
        EXPECT_EQ(sanitize_utf8(text), text);
    }
}

TEST(Utf8Sanitize, InvalidBytesBecomeReplacement) {
    EXPECT_EQ(sanitize_utf8("\xC3\n"), "\xEF\xBF\xBD\n");
    EXPECT_EQ(sanitize_utf8("\x80"), "\xEF\xBF\xBD");
    EXPECT_EQ(sanitize_utf8("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");
    EXPECT_EQ(sanitize_utf8("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    EXPECT_EQ(sanitize_utf8("\xE2\x82"), "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(Utf8Sanitize, RetentionAndJsonDumpSucceed) {
    payload::ToolResult result;
    result.id      = "call-1";
    result.name    = "shell";
    result.output  = "line\n\xC3\nnext\n";
    result.error   = "bad \x80 error";

    retain_tool_result(result, 8);
    EXPECT_NO_THROW({ (void)nlohmann::json(result).dump(); });

    payload::ToolResult direct;
    direct.id     = "call-2";
    direct.name   = "shell";
    direct.output = "line\n\xC3\n";
    const nlohmann::json object = direct;
    EXPECT_NO_THROW({ (void)object.dump(); });
    EXPECT_EQ(object.at("output").get<std::string>().find('\xC3'), std::string::npos);
}

TEST(Utf8Sanitize, ToolMessageContentIsValid) {
    payload::ToolResult result;
    result.id     = "call-3";
    result.name   = "shell";
    result.output = "prefix\xC3\nsuffix";
    const std::string message = tool_message_content(result);
    EXPECT_EQ(message.find('\xC3'), std::string::npos);
    EXPECT_NE(message.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_NO_THROW({ (void)nlohmann::json(message).dump(); });
}

} // namespace

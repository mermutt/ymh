#include <gtest/gtest.h>

#include <string>

#include "ymh/session/tool_result_text.hpp"

namespace {

using namespace ymh;

payload::ToolResult result(payload::ToolOutcome outcome, std::string output,
                           std::optional<std::string> error = std::nullopt) {
    payload::ToolResult value;
    value.id      = "call-1";
    value.name    = "mcp.github.search";
    value.outcome = outcome;
    value.output  = std::move(output);
    value.error   = std::move(error);
    return value;
}

bool all_printable_ascii(const std::string& text) {
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

TEST(DenialText, CanonicalDenialTextNamesOutcomeToolAndReason) {
    const std::string text = denial_text("mcp.github.search", "user");
    EXPECT_EQ(text.rfind("DENIED:", 0), 0u);
    EXPECT_NE(text.find("mcp.github.search"), std::string::npos);
    EXPECT_NE(text.find("NOT executed"), std::string::npos);
    EXPECT_NE(text.find("the user denied it"), std::string::npos);
    EXPECT_NE(text.find("do not report this call as successful"), std::string::npos);
}

TEST(DenialText, EmptyReasonIsUnspecified) {
    EXPECT_NE(denial_text("x", "").find("reason: unspecified"), std::string::npos);
}

TEST(DenialText, KnownReasonsAreRendered) {
    const std::pair<const char*, const char*> cases[] = {
        {"user", "the user denied it"},
        {"timeout", "permission request timed out"},
        {"cancelled", "request cancelled"},
        {"shutdown", "daemon shut down"},
        {"transport", "transport failure"},
        {"no-subscribers", "no attached client"},
        {"internal", "internal error"},
        {"no-resolver", "no permission resolver attached"},
        {"no permission resolver attached", "no permission resolver attached"},
        {"rule", "denied by a permission rule"},
        {"permission:protected-path", "protected path"},
    };
    for (const auto& [reason, expected] : cases) {
        EXPECT_NE(denial_detail(reason).find(expected), std::string::npos) << reason;
    }
}

TEST(DenialText, UnknownReasonPassesThroughSanitized) {
    const std::string detail = denial_detail("permission:protected-path\n\x1b[31m");
    EXPECT_TRUE(all_printable_ascii(detail));
    EXPECT_EQ(detail.find('\n'), std::string::npos);
    EXPECT_EQ(detail.find('\x1b'), std::string::npos);
    EXPECT_LE(detail.size(), kMaxDenialReasonBytes);
}

TEST(DenialText, DenialTextIsBoundedAscii) {
    const std::string text = denial_text("tool", std::string(4096, '\x01'));
    EXPECT_LE(text.size(), kMaxDenialTextBytes);
    EXPECT_TRUE(all_printable_ascii(text));
    EXPECT_NE(text.find("do not report this call as successful"), std::string::npos);
}

TEST(DenialText, ToolMessageContentPassesThroughOk) {
    const payload::ToolResult value = result(payload::ToolOutcome::Ok, "hello\nworld");
    EXPECT_EQ(tool_message_content(value), "hello\nworld");
}

TEST(DenialText, ToolMessageContentKeepsCanonicalDenied) {
    const std::string canonical = denial_text("x", "user");
    const payload::ToolResult value =
        result(payload::ToolOutcome::Denied, canonical, canonical);
    EXPECT_TRUE(is_canonical_denial(canonical));
    EXPECT_EQ(tool_message_content(value), canonical);
}

TEST(DenialText, BareDeniedPrefixIsNotMistakenForCanonical) {
    const payload::ToolResult value =
        result(payload::ToolOutcome::Denied, "DENIED: custom");
    EXPECT_FALSE(is_canonical_denial("DENIED: custom"));
    const std::string repaired = tool_message_content(value);
    EXPECT_EQ(repaired.rfind("DENIED:", 0), 0u);
    EXPECT_NE(repaired.find("was NOT executed"), std::string::npos);
}

TEST(DenialText, OversizedCanonicalIsRebounded) {
    std::string oversized = "DENIED: tool 'x' was NOT executed";
    oversized.append(2048, 'x');
    EXPECT_FALSE(is_canonical_denial(oversized));
    const std::string repaired =
        tool_message_content(result(payload::ToolOutcome::Denied, oversized));
    EXPECT_LE(repaired.size(), kMaxDenialTextBytes);
}

TEST(DenialText, ToolMessageContentRepairsLegacyBareToken) {
    const std::string repaired =
        tool_message_content(result(payload::ToolOutcome::Denied, "user"));
    EXPECT_EQ(repaired.rfind("DENIED:", 0), 0u);
    EXPECT_NE(repaired.find("the user denied it"), std::string::npos);
}

TEST(DenialText, ToolMessageContentRepairsEmptyDenied) {
    const std::string repaired =
        tool_message_content(result(payload::ToolOutcome::Denied, ""));
    EXPECT_NE(repaired.find("reason: unspecified"), std::string::npos);
}

TEST(DenialText, ToolMessageContentLeavesErrorAndCancelled) {
    EXPECT_EQ(tool_message_content(result(payload::ToolOutcome::Error, "boom")), "boom");
    EXPECT_EQ(tool_message_content(result(payload::ToolOutcome::Cancelled, "stop")), "stop");
}

TEST(DenialText, CanonicalWithControlByteIsRebuilt) {
    std::string text = denial_text("x", "user");
    text.insert(text.size() / 2, 1, '\n');
    EXPECT_FALSE(is_canonical_denial(text));
    const std::string repaired =
        tool_message_content(result(payload::ToolOutcome::Denied, text));
    EXPECT_TRUE(all_printable_ascii(repaired));
    EXPECT_EQ(repaired.find('\n'), std::string::npos);
}

TEST(DenialText, IsCanonicalDenialRejectsOversize) {
    std::string text = denial_text("x", "user");
    text.append(2048, 'x');
    EXPECT_FALSE(is_canonical_denial(text));
}

} // namespace

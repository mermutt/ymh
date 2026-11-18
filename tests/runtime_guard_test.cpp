#include <gtest/gtest.h>

#include <string>

#include "ymh/prompt/runtime_context.hpp"
#include "ymh/session/tool_result_text.hpp"

TEST(RuntimeGuardGuidance, RuntimeContextNamesProtectedStateAndCommands) {
    ymh::RuntimeContextConfig config;
    config.cwd   = "/tmp/workspace";
    config.model = "test-model";

    const std::string text = ymh::render_runtime_context(config, "2026-01-01");
    EXPECT_NE(text.find(".ymh"), std::string::npos);
    EXPECT_NE(text.find("ymh list"), std::string::npos);
    EXPECT_NE(text.find("ymh show"), std::string::npos);
    EXPECT_NE(text.find("ymh doctor"), std::string::npos);
}

TEST(RuntimeGuardGuidance, RuntimeContextAnnouncesEnabledAccess) {
    ymh::RuntimeContextConfig config;
    config.cwd              = "/tmp/workspace";
    config.model            = "test-model";
    config.allow_ymh_access = true;

    const std::string text = ymh::render_runtime_context(config, "2026-01-01");
    EXPECT_NE(text.find("direct access"), std::string::npos);
    EXPECT_NE(text.find(".ymh"), std::string::npos);
}

TEST(RuntimeGuardGuidance, ProtectedPathDenialTellsModelWhatToUse) {
    const std::string detail = ymh::denial_detail("permission:protected-path");
    EXPECT_NE(detail.find("protected path"), std::string::npos);
    EXPECT_NE(detail.find("ymh list"), std::string::npos);

    const std::string text = ymh::denial_text("shell", "permission:protected-path");
    EXPECT_NE(text.find("DENIED"), std::string::npos);
    EXPECT_NE(text.find("ymh replay"), std::string::npos);
}

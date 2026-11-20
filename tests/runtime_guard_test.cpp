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

TEST(RuntimeGuardGuidance, RuntimeContextNamesTheTempScratchArea) {
    ymh::RuntimeContextConfig config;
    config.cwd     = "/home/user/project";
    config.model   = "test-model";
    config.sandbox = "workspace";

    const std::string text = ymh::render_runtime_context(config, "2026-01-01");
    EXPECT_NE(text.find("Scratch space"), std::string::npos);
    EXPECT_NE(text.find("absolute"), std::string::npos);
    EXPECT_NE(text.find("/tmp"), std::string::npos);

    config.sandbox = "read-only";
    const std::string read_only = ymh::render_runtime_context(config, "2026-01-01");
    EXPECT_EQ(read_only.find("Scratch space"), std::string::npos);
}

TEST(RuntimeGuardGuidance, RuntimeContextExplainsNestedMetadata) {
    ymh::RuntimeContextConfig config;
    config.cwd   = "/tmp/workspace";
    config.model = "test-model";

    const std::string text = ymh::render_runtime_context(config, "2026-01-01");
    EXPECT_NE(text.find("workspace root's `.ymh/` is protected"), std::string::npos);
    EXPECT_NE(text.find("nested project's `.ymh/` is ordinary"), std::string::npos);
}

TEST(RuntimeGuardGuidance, ProtectedPathDenialTellsModelWhatToUse) {
    const std::string detail = ymh::denial_detail("permission:protected-path");
    EXPECT_NE(detail.find("protected path"), std::string::npos);
    EXPECT_NE(detail.find("ymh list"), std::string::npos);

    const std::string text = ymh::denial_text("shell", "permission:protected-path");
    EXPECT_NE(text.find("DENIED"), std::string::npos);
    EXPECT_NE(text.find("ymh replay"), std::string::npos);
}

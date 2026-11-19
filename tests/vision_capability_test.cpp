#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_support.hpp"

#include "ymh/agent/message.hpp"
#include "ymh/agent/vision_gate.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace {

using ymh::Config;
using ymh::ConfigError;
using ymh::ConfigPaths;
using ymh::ContentBlock;
using ymh::ContentBlockKind;
using ymh::Message;
using ymh::ModelProfile;
using ymh::ProviderCapabilities;
using ymh::Role;
using ymh_test::TempWorkspace;

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

ContentBlock image_block(std::string media_type, std::string data) {
    ContentBlock block;
    block.kind       = ContentBlockKind::Image;
    block.media_type = std::move(media_type);
    block.data       = std::move(data);
    return block;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream << content;
}

Config load_from_string(TempWorkspace& workspace, const std::string& json) {
    ConfigPaths paths;
    paths.global    = workspace.root / "global.jsonc";
    paths.workspace = workspace.root / ".ymh" / "config.jsonc";
    write_file(paths.global, json);
    return ymh::load_config(paths);
}

// ---------------------------------------------------------------------------
// degrade_prompt_images
// ---------------------------------------------------------------------------

TEST(VisionCapability, DegradeReplacesImageWithMediaType) {
    std::vector<Message> messages;
    Message              message;
    message.role    = Role::User;
    message.content = {text_block("before"), image_block("image/png", "Zm9v"),
                       text_block("after")};
    messages.push_back(message);

    const std::size_t degraded = ymh::degrade_prompt_images(messages, "not capable");

    EXPECT_EQ(degraded, 1U);
    ASSERT_EQ(messages.size(), 1U);
    const std::vector<ContentBlock>& blocks = messages.front().content;
    ASSERT_EQ(blocks.size(), 3U);
    EXPECT_EQ(blocks[0].kind, ContentBlockKind::Text);
    EXPECT_EQ(blocks[0].text, "before");
    EXPECT_EQ(blocks[1].kind, ContentBlockKind::Text);
    EXPECT_EQ(blocks[1].text, "[image image/png omitted: not capable]");
    EXPECT_TRUE(blocks[1].media_type.empty());
    EXPECT_TRUE(blocks[1].data.empty());
    EXPECT_EQ(blocks[2].kind, ContentBlockKind::Text);
    EXPECT_EQ(blocks[2].text, "after");
}

TEST(VisionCapability, DegradeOmitsMediaTypeWhenAbsent) {
    std::vector<Message> messages;
    Message              message;
    message.role    = Role::User;
    message.content = {image_block("", "Zm9v")};
    messages.push_back(message);

    const std::size_t degraded = ymh::degrade_prompt_images(messages, "not capable");

    EXPECT_EQ(degraded, 1U);
    ASSERT_EQ(messages.front().content.size(), 1U);
    EXPECT_EQ(messages.front().content[0].kind, ContentBlockKind::Text);
    EXPECT_EQ(messages.front().content[0].text, "[image omitted: not capable]");
}

TEST(VisionCapability, DegradeCountsEveryImageAcrossMessages) {
    std::vector<Message> messages;
    Message              first;
    first.role    = Role::User;
    first.content = {image_block("image/jpeg", "YQ=="), image_block("image/webp", "Yg==")};
    messages.push_back(first);
    Message second;
    second.role    = Role::Assistant;
    second.content = {text_block("answer"), image_block("image/png", "Yw==")};
    messages.push_back(second);

    const std::size_t degraded = ymh::degrade_prompt_images(messages, "no vision");

    EXPECT_EQ(degraded, 3U);
    EXPECT_EQ(messages[0].content[0].kind, ContentBlockKind::Text);
    EXPECT_EQ(messages[0].content[1].kind, ContentBlockKind::Text);
    EXPECT_EQ(messages[1].content[0].kind, ContentBlockKind::Text);
    EXPECT_EQ(messages[1].content[0].text, "answer");
    EXPECT_EQ(messages[1].content[1].kind, ContentBlockKind::Text);
}

TEST(VisionCapability, DegradeLeavesMessagesWithoutImagesUntouched) {
    std::vector<Message> messages;
    Message              message;
    message.role    = Role::User;
    message.content = {text_block("plain text")};
    messages.push_back(message);
    const std::vector<Message> before = messages;

    const std::size_t degraded = ymh::degrade_prompt_images(messages, "no vision");

    EXPECT_EQ(degraded, 0U);
    EXPECT_EQ(messages, before);
}

// ---------------------------------------------------------------------------
// apply_profile_capabilities
// ---------------------------------------------------------------------------

TEST(VisionCapability, ProfileCanDeclareVisionTrue) {
    ProviderCapabilities capabilities;
    capabilities.vision = false;
    ModelProfile profile;
    profile.capabilities.vision = true;

    ymh::apply_profile_capabilities(capabilities, profile);

    EXPECT_TRUE(capabilities.vision);
}

TEST(VisionCapability, ProfileCanDeclareVisionFalse) {
    ProviderCapabilities capabilities;
    ModelProfile         profile;
    profile.capabilities.vision = false;

    ymh::apply_profile_capabilities(capabilities, profile);

    EXPECT_FALSE(capabilities.vision);
}

TEST(VisionCapability, ProfileLeavesBaseVisionWhenUndeclared) {
    ProviderCapabilities capable;
    ModelProfile         inert;
    ymh::apply_profile_capabilities(capable, inert);
    EXPECT_TRUE(capable.vision);

    ProviderCapabilities incapable;
    incapable.vision = false;
    ymh::apply_profile_capabilities(incapable, inert);
    EXPECT_FALSE(incapable.vision);
}

// ---------------------------------------------------------------------------
// config plumbing
// ---------------------------------------------------------------------------

TEST(VisionCapability, ResolveModelReadsDefaultVision) {
    TempWorkspace workspace;
    const Config  config = load_from_string(
        workspace, R"({"llm": {"model": "wire-model", "vision": false}})");

    const ymh::ResolvedModel resolved = ymh::resolve_model(config);

    ASSERT_TRUE(resolved.profile.capabilities.vision.has_value());
    EXPECT_FALSE(*resolved.profile.capabilities.vision);
}

TEST(VisionCapability, NamedModelVisionOverridesDefaultTrue) {
    TempWorkspace workspace;
    const Config  config = load_from_string(
        workspace,
        R"({"llm": {"vision": false, "active_model": "named",
                    "endpoints": {"local": {"base_url": "https://example.test/v1"}},
                    "models": {"named": {"endpoint": "local", "model": "wire-model",
                                         "vision": true}}}})");

    const ymh::ResolvedModel resolved = ymh::resolve_model(config);

    ASSERT_TRUE(resolved.profile.capabilities.vision.has_value());
    EXPECT_TRUE(*resolved.profile.capabilities.vision);
}

TEST(VisionCapability, NamedModelVisionOverridesDefaultFalse) {
    TempWorkspace workspace;
    const Config  config = load_from_string(
        workspace,
        R"({"llm": {"vision": true, "active_model": "named",
                    "endpoints": {"local": {"base_url": "https://example.test/v1"}},
                    "models": {"named": {"endpoint": "local", "model": "wire-model",
                                         "vision": false}}}})");

    const ymh::ResolvedModel resolved = ymh::resolve_model(config);

    ASSERT_TRUE(resolved.profile.capabilities.vision.has_value());
    EXPECT_FALSE(*resolved.profile.capabilities.vision);
}

TEST(VisionCapability, RejectsNonBoolVision) {
    TempWorkspace workspace;
    EXPECT_THROW(
        load_from_string(workspace, R"({"llm": {"vision": "yes"}})"), ConfigError);

    TempWorkspace named;
    EXPECT_THROW(load_from_string(named,
                                  R"({"llm": {"endpoints": {"local": {"base_url":
                                      "https://example.test/v1"}},
                                      "models": {"named": {"endpoint": "local",
                                                           "model": "wire-model",
                                                           "vision": 1}}}})"),
                 ConfigError);
}

} // namespace

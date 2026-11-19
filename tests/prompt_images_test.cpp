#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_support.hpp"

#include "ymh/agent/message.hpp"
#include "ymh/agent/prompt_images.hpp"

namespace {

using namespace ymh;
using ymh_test::TempWorkspace;

constexpr char kPngMagic[] = "\x89PNG\r\n\x1a\n";

std::string png_bytes(std::size_t payload = 16) {
    std::string bytes{kPngMagic, 8};
    bytes.append(payload, 'x');
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

Message text_message(const std::string& text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content.push_back(std::move(block));
    return message;
}

std::size_t image_count(const Message& message) {
    std::size_t count = 0;
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Image) {
            ++count;
        }
    }
    return count;
}

const ContentBlock* first_image(const Message& message) {
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Image) {
            return &block;
        }
    }
    return nullptr;
}

bool has_text(const Message& message, std::string_view needle) {
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Text &&
            block.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST(PromptImages, AttachesAPathFromOrdinaryProse) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "shot.png", png_bytes());

    Message message = text_message("Please look at ./shot.png and describe it.");
    const PromptImageResult result = attach_prompt_images(message, workspace.root);

    EXPECT_EQ(result.attached, 1U);
    ASSERT_EQ(image_count(message), 1U);
    const ContentBlock* image = first_image(message);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->media_type, "image/png");
    EXPECT_FALSE(image->data.empty());
    EXPECT_TRUE(has_text(message, "./shot.png"));
}

TEST(PromptImages, HandlesQuotedPathsFileUrisAndTrailingPunctuation) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "my shot.png", png_bytes());
    const std::string absolute = (workspace.root / "my shot.png").string();

    Message quoted = text_message("look at \"my shot.png\"");
    EXPECT_EQ(attach_prompt_images(quoted, workspace.root).attached, 1U);

    std::string  encoded = absolute;
    const std::size_t space = encoded.find(' ');
    if (space != std::string::npos) {
        encoded.replace(space, 1, "%20");
    }
    Message uri = text_message("look at file://" + encoded);
    EXPECT_EQ(attach_prompt_images(uri, workspace.root).attached, 1U);

    write_bytes(workspace.root / "dot.png", png_bytes());
    Message punctuated = text_message("look at ./dot.png.");
    EXPECT_EQ(attach_prompt_images(punctuated, workspace.root).attached, 1U);
}

TEST(PromptImages, IgnoresOrdinaryTextFiles) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "notes.txt", "not an image");

    Message message = text_message("read ./notes.txt");
    const std::size_t before = message.content.size();
    const PromptImageResult result = attach_prompt_images(message, workspace.root);

    EXPECT_EQ(result.attached, 0U);
    EXPECT_EQ(message.content.size(), before);
    EXPECT_TRUE(result.warnings.empty());
}

TEST(PromptImages, RejectsOversizeImagesWithAVisibleNote) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "big.png", png_bytes(100));

    Message message = text_message("look at ./big.png");
    PromptImageLimits limits;
    limits.max_image_bytes = 64;
    const PromptImageResult result = attach_prompt_images(message, workspace.root, limits);

    EXPECT_EQ(result.attached, 0U);
    ASSERT_FALSE(result.warnings.empty());
    EXPECT_TRUE(has_text(message, "image not attached"));
    EXPECT_TRUE(has_text(message, "limit"));
}

TEST(PromptImages, AnExplicitClientImageSuppressesScanning) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "shot.png", png_bytes());

    Message message = text_message("look at ./shot.png");
    ContentBlock image;
    image.kind       = ContentBlockKind::Image;
    image.media_type = "image/png";
    image.data       = "AAAA";
    message.content.push_back(image);

    EXPECT_EQ(attach_prompt_images(message, workspace.root).attached, 0U);
    EXPECT_EQ(image_count(message), 1U);
    EXPECT_EQ(first_image(message)->data, "AAAA");
}

TEST(PromptImages, AcceptsAnAbsoluteImageOutsideTheWorkspace) {
    TempWorkspace workspace;
    TempWorkspace outside;  // a second temp root, not under the workspace root
    write_bytes(outside.root / "external.png", png_bytes());

    Message message = text_message("look at " + (outside.root / "external.png").string());
    EXPECT_EQ(attach_prompt_images(message, workspace.root).attached, 1U);
}

} // namespace

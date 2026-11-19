#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

std::string jpeg_bytes(std::size_t payload = 16) {
    std::string bytes{"\xFF\xD8\xFF", 3};
    bytes.append(payload, 'x');
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

Message text_message(const std::string& text, Role role = Role::User) {
    Message message;
    message.role = role;
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

const ContentBlock* image_at(const Message& message, std::size_t ordinal) {
    std::size_t seen = 0;
    for (const ContentBlock& block : message.content) {
        if (block.kind != ContentBlockKind::Image) {
            continue;
        }
        if (seen == ordinal) {
            return &block;
        }
        ++seen;
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

TEST(PromptImagesExplicit, AttachesPngAndJpegWithMediaTypes) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "shot.png", png_bytes());
    write_bytes(workspace.root / "photo.jpg", jpeg_bytes());

    Message message = text_message("describe these");
    std::vector<std::string> warnings;

    EXPECT_TRUE(attach_image_file(message, workspace.root / "shot.png", {}, warnings));
    EXPECT_TRUE(attach_image_file(message, workspace.root / "photo.jpg", {}, warnings));

    EXPECT_TRUE(warnings.empty());
    ASSERT_EQ(image_count(message), 2U);
    const ContentBlock* png = image_at(message, 0);
    ASSERT_NE(png, nullptr);
    EXPECT_EQ(png->media_type, "image/png");
    EXPECT_FALSE(png->data.empty());
    const ContentBlock* jpeg = image_at(message, 1);
    ASSERT_NE(jpeg, nullptr);
    EXPECT_EQ(jpeg->media_type, "image/jpeg");
    EXPECT_FALSE(jpeg->data.empty());
    EXPECT_TRUE(has_text(message, "describe these"));
}

TEST(PromptImagesExplicit, MissingFileAppendsAVisibleNoteAndWarning) {
    TempWorkspace workspace;

    Message message = text_message("look");
    std::vector<std::string> warnings;
    EXPECT_FALSE(attach_image_file(message, workspace.root / "absent.png", {}, warnings));

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_NE(warnings[0].find("image not attached"), std::string::npos);
    EXPECT_NE(warnings[0].find("cannot read the file"), std::string::npos);
    EXPECT_TRUE(has_text(message, "image not attached"));
    EXPECT_TRUE(has_text(message, "cannot read the file"));
    EXPECT_EQ(image_count(message), 0U);
}

TEST(PromptImagesExplicit, NonImageFileAppendsAVisibleNoteAndWarning) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "notes.txt", "not an image");

    Message message = text_message("look");
    std::vector<std::string> warnings;
    EXPECT_FALSE(attach_image_file(message, workspace.root / "notes.txt", {}, warnings));

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_NE(warnings[0].find("image not attached"), std::string::npos);
    EXPECT_NE(warnings[0].find("unsupported image format"), std::string::npos);
    EXPECT_TRUE(has_text(message, "image not attached"));
    EXPECT_TRUE(has_text(message, "unsupported image format"));
    EXPECT_EQ(image_count(message), 0U);
}

TEST(PromptImagesExplicit, OversizeFileAppendsAVisibleNoteAndWarning) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "big.png", png_bytes(100));

    PromptImageLimits limits;
    limits.max_image_bytes = 64;

    Message message = text_message("look");
    std::vector<std::string> warnings;
    EXPECT_FALSE(attach_image_file(message, workspace.root / "big.png", limits, warnings));

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_NE(warnings[0].find("image not attached"), std::string::npos);
    EXPECT_NE(warnings[0].find("limit"), std::string::npos);
    EXPECT_TRUE(has_text(message, "image not attached"));
    EXPECT_TRUE(has_text(message, "limit"));
    EXPECT_EQ(image_count(message), 0U);
}

TEST(PromptImagesExplicit, LeavesANonUserMessageUntouched) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "shot.png", png_bytes());

    Message message = text_message("assistant prose", Role::Assistant);
    const std::size_t before = message.content.size();
    std::vector<std::string> warnings;

    EXPECT_FALSE(attach_image_file(message, workspace.root / "shot.png", {}, warnings));

    EXPECT_EQ(message.content.size(), before);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(image_count(message), 0U);
}

TEST(PromptImagesExplicit, EnforcesImageCountAcrossAnExplicitList) {
    TempWorkspace workspace;
    std::vector<std::filesystem::path> paths;
    for (int index = 0; index < 5; ++index) {
        const std::filesystem::path path =
            workspace.root / ("shot" + std::to_string(index) + ".png");
        write_bytes(path, png_bytes());
        paths.push_back(path);
    }

    Message message = text_message("look");
    PromptImageLimits limits;
    limits.max_images = 2;
    const PromptImageResult result = attach_image_files(message, paths, limits);

    EXPECT_EQ(result.attached, 2U);
    EXPECT_EQ(image_count(message), 2U);
    ASSERT_FALSE(result.warnings.empty());
    EXPECT_TRUE(has_text(message, "image count limit reached"));
}

TEST(PromptImagesExplicit, EnforcesEncodedTotalAcrossAnExplicitList) {
    TempWorkspace workspace;
    // 24-byte PNG -> 32 base64 bytes; a 40-byte budget admits one, not two.
    write_bytes(workspace.root / "one.png", png_bytes(16));
    write_bytes(workspace.root / "two.png", png_bytes(16));

    Message message = text_message("look");
    PromptImageLimits limits;
    limits.max_total_encoded_bytes = 40;
    const PromptImageResult result = attach_image_files(
        message, {workspace.root / "one.png", workspace.root / "two.png"}, limits);

    EXPECT_EQ(result.attached, 1U);
    EXPECT_EQ(image_count(message), 1U);
    ASSERT_FALSE(result.warnings.empty());
    EXPECT_TRUE(has_text(message, "size limit reached"));
}

} // namespace

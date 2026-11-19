#pragma once

// User-prompt image attachments. The provider adapter already serializes
// `ContentBlockKind::Image` in user messages; this seam turns image paths the
// user typed (or a file manager pasted) into those blocks. Pure filesystem I/O,
// no provider-specific logic.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/agent/message.hpp"

namespace ymh {

// A supported image, base64-encoded (standard alphabet, no `data:` prefix).
struct EncodedImage {
    std::string media_type;
    std::string data;
};

// Detect the media type from magic bytes (PNG/JPEG/GIF/WebP) and base64-encode
// `bytes`. On failure returns nullopt and sets `reason`: "unsupported image
// format" for an unknown header, or a message containing "limit" when `bytes`
// exceeds `max_bytes`.
[[nodiscard]] std::optional<EncodedImage> encode_image(std::string_view bytes,
                                                       std::size_t max_bytes,
                                                       std::string& reason);

struct PromptImageLimits {
    std::size_t max_image_bytes = 2u * 1024u * 1024u;
    std::size_t max_total_encoded_bytes = 3u * 1024u * 1024u;
    std::size_t max_images = 4;
};

struct PromptImageResult {
    std::size_t              attached = 0;
    std::vector<std::string> warnings;
};

// Scan the Text blocks of a user message for existing image paths and append
// an Image block for each supported file. Relative paths resolve against
// `workspace_root`; `~/` and `file://` paths are accepted. A failure adds a
// visible Text note (and a warning) instead of silently dropping the image.
// No-op when the message already carries Image blocks or is not Role::User.
[[nodiscard]] PromptImageResult attach_prompt_images(
    Message& message, const std::filesystem::path& workspace_root,
    PromptImageLimits limits = {});

// Explicit attachment entry point (CLI `--image`). Returns false and appends a
// note on failure.
[[nodiscard]] bool attach_image_file(Message& message, const std::filesystem::path& path,
                                     PromptImageLimits limits,
                                     std::vector<std::string>& warnings);

// Attach a whole explicit list in order, enforcing the image-count cap and the
// encoded total across the list AND any Image blocks already present. Returns
// how many files from `paths` were attached; failures append a visible note and
// a warning.
[[nodiscard]] PromptImageResult attach_image_files(
    Message& message, const std::vector<std::filesystem::path>& paths,
    PromptImageLimits limits = {});

} // namespace ymh

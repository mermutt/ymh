#pragma once

// User-prompt image attachments. The provider adapter already serializes
// `ContentBlockKind::Image` in user messages; this seam turns image paths the
// user typed (or a file manager pasted) into those blocks. Pure filesystem I/O,
// no provider-specific logic.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "ymh/agent/message.hpp"

namespace ymh {

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

} // namespace ymh

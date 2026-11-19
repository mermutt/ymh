#pragma once

// User-prompt image attachments. The provider adapter already serializes
// `ContentBlockKind::Image` in user messages; this seam turns image paths the
// user typed (or a file manager pasted) into those blocks. Pure filesystem I/O,
// no provider-specific logic.

#include <cstddef>
#include <filesystem>
#include <functional>
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
    std::size_t bytes = 0;  // decoded size, for notices and limits
};

// True for an `http://` or `https://` source (case-insensitive scheme).
[[nodiscard]] bool is_remote_image_source(std::string_view source) noexcept;

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

// A validated image source plus the label to show in notices: the resolved
// local path, or the URL exactly as typed.
struct LoadedImageSource {
    std::string  display;
    EncodedImage image;
};

// Load one explicit source: a local path (relative to `workspace_root`, `~/`,
// `file://`, percent-decoded) or an `http(s)://` URL. Remote fetches follow at
// most five http(s) redirects, use 10 s connect / 30 s total timeouts, and
// abort above `limits.max_image_bytes`. `cancelled` is polled during a remote
// transfer and aborts it promptly. On failure returns nullopt and sets
// `reason`.
[[nodiscard]] std::optional<LoadedImageSource> load_image_source(
    std::string_view source, const std::filesystem::path& workspace_root,
    const PromptImageLimits& limits, std::string& reason,
    const std::function<bool()>& cancelled = {});

// Attach explicit sources (CLI `--image`, TUI `/image`): local paths or
// http(s) URLs, in order, with the same count/total caps as
// `attach_image_files`. Failures append a visible note and a warning.
[[nodiscard]] PromptImageResult attach_image_sources(
    Message& message, const std::vector<std::string>& sources,
    const std::filesystem::path& workspace_root, PromptImageLimits limits = {});

} // namespace ymh

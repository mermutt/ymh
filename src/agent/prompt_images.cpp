#include "ymh/agent/prompt_images.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ymh {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string base64_encode(std::string_view bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const auto b = static_cast<unsigned char>(bytes[index + 1]);
        const auto c = static_cast<unsigned char>(bytes[index + 2]);
        out.push_back(kBase64Alphabet[a >> 2]);
        out.push_back(kBase64Alphabet[((a & 0x03) << 4) | (b >> 4)]);
        out.push_back(kBase64Alphabet[((b & 0x0F) << 2) | (c >> 6)]);
        out.push_back(kBase64Alphabet[c & 0x3F]);
        index += 3;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        out.push_back(kBase64Alphabet[a >> 2]);
        out.push_back(kBase64Alphabet[(a & 0x03) << 4]);
        out += "==";
    } else if (remaining == 2) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const auto b = static_cast<unsigned char>(bytes[index + 1]);
        out.push_back(kBase64Alphabet[a >> 2]);
        out.push_back(kBase64Alphabet[((a & 0x03) << 4) | (b >> 4)]);
        out.push_back(kBase64Alphabet[(b & 0x0F) << 2]);
        out.push_back('=');
    }
    return out;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const auto hex = [](char value) -> int {
                if (value >= '0' && value <= '9') return value - '0';
                if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                if (value >= 'A' && value <= 'F') return value - 'A' + 10;
                return -1;
            };
            const int high = hex(text[index + 1]);
            const int low  = hex(text[index + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        out.push_back(text[index]);
    }
    return out;
}

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string              current;
    char                     quote = '\0';
    for (const char character : text) {
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            } else {
                current.push_back(character);
            }
            continue;
        }
        if (character == '"' || character == '\'' || character == '`') {
            quote = character;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::vector<std::string> token_variants(const std::string& token) {
    std::vector<std::string> variants;
    const auto add = [&](std::string value) {
        if (value.empty()) {
            return;
        }
        if (std::find(variants.begin(), variants.end(), value) == variants.end()) {
            variants.push_back(std::move(value));
        }
    };
    add(token);

    std::string trimmed = token;
    while (!trimmed.empty() &&
           std::strchr(".,;:!?)]}>\"'`", trimmed.back()) != nullptr) {
        trimmed.pop_back();
    }
    while (!trimmed.empty() && std::strchr("([{<\"'`", trimmed.front()) != nullptr) {
        trimmed.erase(0, 1);
    }
    add(trimmed);
    return variants;
}

bool has_image_extension(std::string_view token) {
    std::string lower{token};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::size_t dot = lower.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string_view extension{lower.data() + dot, lower.size() - dot};
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".gif" || extension == ".webp";
}

std::optional<std::string> detect_media_type(std::string_view prefix) {
    const auto byte = [&](std::size_t index) -> unsigned char {
        return index < prefix.size() ? static_cast<unsigned char>(prefix[index]) : 0;
    };
    if (prefix.size() >= 8 && byte(0) == 0x89 && byte(1) == 'P' && byte(2) == 'N' &&
        byte(3) == 'G' && byte(4) == 0x0D && byte(5) == 0x0A && byte(6) == 0x1A &&
        byte(7) == 0x0A) {
        return std::string{"image/png"};
    }
    if (prefix.size() >= 3 && byte(0) == 0xFF && byte(1) == 0xD8 && byte(2) == 0xFF) {
        return std::string{"image/jpeg"};
    }
    if (prefix.size() >= 6 &&
        (prefix.substr(0, 6) == "GIF87a" || prefix.substr(0, 6) == "GIF89a")) {
        return std::string{"image/gif"};
    }
    if (prefix.size() >= 12 && prefix.substr(0, 4) == "RIFF" &&
        prefix.substr(8, 4) == "WEBP") {
        return std::string{"image/webp"};
    }
    return std::nullopt;
}

std::string home_directory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return home;
    }
    return {};
}

std::filesystem::path resolve_token(const std::string& token,
                                    const std::filesystem::path& workspace_root) {
    std::string value = token;
    if (starts_with(value, "file://")) {
        value.erase(0, 7);
        if (starts_with(value, "localhost/")) {
            value.erase(0, 9);
        }
        if (const std::size_t cut = value.find_first_of("?#");
            cut != std::string::npos) {
            value.resize(cut);
        }
        value = percent_decode(value);
    } else if (value == "~" || starts_with(value, "~/")) {
        const std::string home = home_directory();
        if (!home.empty()) {
            value = home + value.substr(1);
        }
    }

    std::filesystem::path path{value};
    if (path.is_relative()) {
        path = workspace_root / path;
    }
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

std::optional<EncodedImage> load_image(const std::filesystem::path& path,
                                      const PromptImageLimits&    limits,
                                      std::string&                reason) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        reason = "cannot read the file";
        return std::nullopt;
    }

    std::string prefix(12, '\0');
    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(input.gcount()));
    const std::optional<std::string> media_type = detect_media_type(prefix);
    if (!media_type.has_value()) {
        reason = "unsupported image format";
        return std::nullopt;
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        reason = "cannot stat the file";
        return std::nullopt;
    }
    if (size > limits.max_image_bytes) {
        reason = "file is " + std::to_string(size / (1024u * 1024u)) + " MiB (limit " +
                 std::to_string(limits.max_image_bytes / (1024u * 1024u)) + " MiB)";
        return std::nullopt;
    }

    std::string bytes = std::move(prefix);
    bytes.reserve(static_cast<std::size_t>(size));
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            break;
        }
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    if (input.bad()) {
        reason = "cannot read the file";
        return std::nullopt;
    }
    // Also covers a file that grew past the limit between the stat and the
    // read; `encode_image` owns the size check.
    return encode_image(bytes, limits.max_image_bytes, reason);
}

void append_note(Message& message, const std::string& token,
                 const std::string& reason, std::vector<std::string>& warnings) {
    const std::string note = "image not attached: " + token + " (" + reason + ")";
    warnings.push_back(note);
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "[" + note + "]";
    message.content.push_back(std::move(block));
}

} // namespace

std::optional<EncodedImage> encode_image(std::string_view bytes,
                                         std::size_t max_bytes,
                                         std::string& reason) {
    const std::string_view prefix = bytes.substr(0, 12);
    const std::optional<std::string> media_type = detect_media_type(prefix);
    if (!media_type.has_value()) {
        reason = "unsupported image format";
        return std::nullopt;
    }
    if (bytes.size() > max_bytes) {
        reason = "image is " + std::to_string(bytes.size()) + " bytes (limit " +
                 std::to_string(max_bytes) + " bytes)";
        return std::nullopt;
    }
    EncodedImage encoded;
    encoded.media_type = *media_type;
    encoded.data       = base64_encode(bytes);
    return encoded;
}

bool attach_image_file(Message& message, const std::filesystem::path& path,
                       PromptImageLimits limits, std::vector<std::string>& warnings) {
    const PromptImageResult result = attach_image_files(message, {path}, limits);
    if (result.attached == 1) {
        return true;
    }
    warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
    return false;
}

PromptImageResult attach_image_files(Message& message,
                                     const std::vector<std::filesystem::path>& paths,
                                     PromptImageLimits                         limits) {
    PromptImageResult result;
    if (message.role != Role::User) {
        return result;
    }

    std::size_t attached      = 0;
    std::size_t encoded_total = 0;
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Image) {
            ++attached;
            encoded_total += block.data.size();
        }
    }

    for (const std::filesystem::path& path : paths) {
        if (attached >= limits.max_images) {
            append_note(message, path.string(), "image count limit reached", result.warnings);
            break;
        }
        std::string reason;
        const std::optional<EncodedImage> loaded = load_image(path, limits, reason);
        if (!loaded.has_value()) {
            append_note(message, path.string(), reason, result.warnings);
            continue;
        }
        if (encoded_total + loaded->data.size() > limits.max_total_encoded_bytes) {
            append_note(message, path.string(), "attached-image size limit reached",
                        result.warnings);
            break;
        }
        ContentBlock image;
        image.kind       = ContentBlockKind::Image;
        image.media_type = loaded->media_type;
        image.data       = loaded->data;
        encoded_total += image.data.size();
        ++attached;
        ++result.attached;
        message.content.push_back(std::move(image));
    }
    return result;
}

PromptImageResult attach_prompt_images(Message& message,
                                       const std::filesystem::path& workspace_root,
                                       PromptImageLimits            limits) {
    PromptImageResult result;
    if (message.role != Role::User) {
        return result;
    }
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Image) {
            return result;  // an explicit client attachment wins
        }
    }

    std::vector<std::string> tokens;
    for (const ContentBlock& block : message.content) {
        if (block.kind != ContentBlockKind::Text) {
            continue;
        }
        for (std::string& token : tokenize(block.text)) {
            tokens.push_back(std::move(token));
        }
    }

    std::vector<std::filesystem::path> seen;
    std::size_t                        encoded_total = 0;

    for (const std::string& token : tokens) {
        std::optional<std::filesystem::path> resolved;
        for (const std::string& candidate : token_variants(token)) {
            std::filesystem::path path = resolve_token(candidate, workspace_root);
            std::error_code       error;
            if (std::filesystem::is_regular_file(path, error) && !error) {
                resolved = std::move(path);
                break;
            }
        }
        if (!resolved.has_value()) {
            continue;
        }
        if (std::find(seen.begin(), seen.end(), *resolved) != seen.end()) {
            continue;
        }
        seen.push_back(*resolved);

        if (result.attached >= limits.max_images) {
            append_note(message, token, "image count limit reached", result.warnings);
            break;
        }

        std::string reason;
        const std::optional<EncodedImage> loaded = load_image(*resolved, limits, reason);
        if (!loaded.has_value()) {
            if (reason == "unsupported image format" && !has_image_extension(token)) {
                continue;  // an ordinary non-image file mentioned in prose
            }
            append_note(message, token, reason, result.warnings);
            continue;
        }
        if (encoded_total + loaded->data.size() > limits.max_total_encoded_bytes) {
            append_note(message, token, "attached-image size limit reached", result.warnings);
            break;
        }

        ContentBlock image;
        image.kind       = ContentBlockKind::Image;
        image.media_type = loaded->media_type;
        image.data       = loaded->data;
        encoded_total += image.data.size();
        message.content.push_back(std::move(image));
        ++result.attached;
    }
    return result;
}

} // namespace ymh

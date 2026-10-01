#pragma once

// Small text helpers shared by core and UI (67-D3).
//
// 67-D3 pins that `/rename`'s empty/whitespace check and the daemon's title
// normalizer agree on what "whitespace" means. ASCII-only trimming let a title
// of a single U+00A0 (NBSP) pass as non-empty and be stored as a whitespace-only
// title, so the shared rule is the Unicode `White_Space` property. U+000A (LF)
// is deliberately **excluded**: a newline inside or around a title stays a
// rejected control character (67-I8), preserving `normalize_title`'s existing
// 19/RN8 contract.

#include <cstddef>
#include <string_view>

namespace ymh {
namespace detail {

// Decodes one UTF-8 glyph at `at`; returns its byte length, or 0 for a
// malformed lead/sequence. Overlong and out-of-range values are not rejected
// here (the caller validates UTF-8 separately).
[[nodiscard]] inline std::size_t decode_utf8(std::string_view text, std::size_t at,
                                             char32_t& codepoint) noexcept {
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80) {
        codepoint = lead;
        return 1;
    }
    std::size_t count = 0;
    char32_t    value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        count = 1;
        value = static_cast<char32_t>(lead & 0x1FU);
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        count = 2;
        value = static_cast<char32_t>(lead & 0x0FU);
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        count = 3;
        value = static_cast<char32_t>(lead & 0x07U);
    } else {
        return 0;
    }
    if (at + count >= text.size()) {
        return 0;
    }
    for (std::size_t index = 1; index <= count; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[at + index]);
        if ((byte & 0xC0U) != 0x80U) {
            return 0;
        }
        value = static_cast<char32_t>((value << 6) | (byte & 0x3FU));
    }
    codepoint = value;
    return count + 1;
}

[[nodiscard]] inline bool unicode_whitespace(char32_t codepoint) noexcept {
    switch (codepoint) {
        case 0x09: // tab
        case 0x0B: // vertical tab
        case 0x0C: // form feed
        case 0x0D: // carriage return
        case 0x20: // space
        case 0x85: // NEL
        case 0xA0: // NBSP
        case 0x1680:
        case 0x2028: // line separator
        case 0x2029: // paragraph separator
        case 0x202F: // narrow no-break space
        case 0x205F: // medium mathematical space
        case 0x3000: // ideographic space
            return true;
        default:
            break;
    }
    return codepoint >= 0x2000 && codepoint <= 0x200A;
}

} // namespace detail

// Trims leading/trailing Unicode `White_Space` (UTF-8), excluding U+000A.
[[nodiscard]] inline std::string_view trim_unicode_whitespace(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size()) {
        char32_t          codepoint = 0;
        const std::size_t length    = detail::decode_utf8(text, begin, codepoint);
        if (length == 0 || !detail::unicode_whitespace(codepoint)) {
            break;
        }
        begin += length;
    }
    std::size_t end = text.size();
    while (end > begin) {
        std::size_t start = end - 1;
        while (start > begin &&
               (static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) {
            --start;
        }
        char32_t          codepoint = 0;
        const std::size_t length    = detail::decode_utf8(text, start, codepoint);
        if (length == 0 || start + length != end ||
            !detail::unicode_whitespace(codepoint)) {
            break;
        }
        end = start;
    }
    return text.substr(begin, end - begin);
}

} // namespace ymh

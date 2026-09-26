#include "ymh/config/jsonc.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace ymh {

bool blank_or_comments_only(std::string_view text) {
    std::size_t i = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i += 2;
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                ++i;
            }
            if (i + 1 >= text.size()) {
                return false;
            }
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

std::size_t line_for_byte(std::string_view text, std::size_t byte) {
    const std::size_t end = std::min(byte, text.size());
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + end, '\n'));
}

std::optional<nlohmann::json> parse_jsonc_document(std::string_view text, std::string& error) {
    error.clear();
    if (blank_or_comments_only(text)) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(text,
                                     /*cb=*/nullptr,
                                     /*allow_exceptions=*/true,
                                     /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& parse_error) {
        std::ostringstream message;
        message << parse_error.what() << " (line " << line_for_byte(text, parse_error.byte) << ')';
        error = message.str();
        return std::nullopt;
    } catch (const std::exception& other) {
        error = other.what();
        return std::nullopt;
    }
}

} // namespace ymh

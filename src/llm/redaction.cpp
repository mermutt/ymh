#include "ymh/llm/redaction.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace ymh {
namespace {

constexpr std::string_view kRedacted = "[REDACTED]";

bool is_token_char(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '-' || c == '_' || c == '.' || c == '~' ||
           c == '+' || c == '/' || c == '=';
}

bool starts_with_ci(std::string_view text, std::size_t pos, std::string_view prefix) noexcept {
    if (pos + prefix.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(text[pos + i]);
        const unsigned char rhs = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

bool is_boundary_before(std::string_view text, std::size_t pos) noexcept {
    return pos == 0 || !is_token_char(text[pos - 1]);
}

std::size_t skip_token(std::string_view text, std::size_t pos) noexcept {
    while (pos < text.size() && is_token_char(text[pos])) {
        ++pos;
    }
    return pos;
}

std::size_t skip_spaces(std::string_view text, std::size_t pos) noexcept {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

} // namespace

std::string redact_secrets(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    std::size_t i = 0;
    while (i < text.size()) {
        // 46-D12.11: the JSON form the key takes in a config document,
        // `"api_key"\s*:\s*"…"` -> `"api_key": "[REDACTED]"`.
        if (is_boundary_before(text, i) && text[i] == '"' &&
            starts_with_ci(text, i + 1, "api_key") && i + 8 < text.size() &&
            text[i + 8] == '"') {
            std::size_t cursor = skip_spaces(text, i + 9);
            if (cursor < text.size() && text[cursor] == ':') {
                cursor = skip_spaces(text, cursor + 1);
                if (cursor < text.size() && text[cursor] == '"') {
                    const std::size_t value_start = cursor + 1;
                    cursor                        = value_start;
                    while (cursor < text.size() && text[cursor] != '"') {
                        if (text[cursor] == '\\') {
                            cursor += 2;
                            continue;
                        }
                        ++cursor;
                    }
                    if (cursor < text.size()) {
                        out.append(text.substr(i, value_start - i));
                        out.append(kRedacted);
                        out.push_back('"');
                        i = cursor + 1;
                        continue;
                    }
                }
            }
        }

        if (starts_with_ci(text, i, "bearer") && is_boundary_before(text, i)) {
            std::size_t after = skip_spaces(text, i + 6);
            if (after < text.size() && is_token_char(text[after])) {
                out.append("Bearer ");
                out.append(kRedacted);
                i = skip_token(text, after);
                continue;
            }
        }

        if (starts_with_ci(text, i, "sk-") && is_boundary_before(text, i)) {
            out.append(kRedacted);
            i = skip_token(text, i);
            continue;
        }

        bool api_key_param = false;
        std::size_t value_start = 0;
        if (starts_with_ci(text, i, "api_key=") && is_boundary_before(text, i)) {
            api_key_param = true;
            value_start = i + 8;
        } else if (starts_with_ci(text, i, "api-key=") && is_boundary_before(text, i)) {
            api_key_param = true;
            value_start = i + 8;
        } else if (starts_with_ci(text, i, "apikey=") && is_boundary_before(text, i)) {
            api_key_param = true;
            value_start = i + 7;
        }
        if (api_key_param && value_start < text.size() && is_token_char(text[value_start])) {
            out.append(text.substr(i, value_start - i));
            out.append(kRedacted);
            i = skip_token(text, value_start);
            continue;
        }

        out.push_back(text[i]);
        ++i;
    }

    return out;
}

std::string redact_header_value(std::string_view name, std::string_view value) {
    const std::string lowered = [&name] {
        std::string result(name);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    }();

    if (lowered == "authorization" || lowered == "api-key" || lowered == "x-api-key" ||
        lowered == "proxy-authorization" || lowered == "cookie" || lowered == "set-cookie") {
        return std::string{kRedacted};
    }
    return std::string{value};
}

} // namespace ymh

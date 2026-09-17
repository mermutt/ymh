#include "ymh/skills/frontmatter.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ymh {
namespace {

constexpr std::string_view kFence = "---";

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                   text[begin] == '\n' || text[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\n' || text[end - 1] == '\r')) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

std::string normalize_line_endings(std::string_view content) {
    std::string out;
    out.reserve(content.size());
    for (std::size_t index = 0; index < content.size(); ++index) {
        const char c = content[index];
        if (c == '\r') {
            if (index + 1 < content.size() && content[index + 1] == '\n') {
                continue;
            }
            out.push_back('\n');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string strip_bom(std::string text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(begin));
            break;
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

bool is_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

bool is_block_scalar(std::string_view value) {
    if (value.empty() || (value[0] != '|' && value[0] != '>')) {
        return false;
    }
    std::string_view rest = value.substr(1);
    if (!rest.empty() && (rest[0] == '+' || rest[0] == '-')) {
        rest = rest.substr(1);
    }
    return rest.empty();
}

bool parse_scalar(std::string_view raw, std::string& out) {
    const std::string trimmed = trim(raw);
    if (trimmed.empty()) {
        out.clear();
        return true;
    }
    const char first = trimmed.front();
    if (first == '[' || first == '{') {
        return false;
    }
    if (first == '"' || first == '\'') {
        if (trimmed.size() < 2 || trimmed.back() != first) {
            return false;
        }
        out = trimmed.substr(1, trimmed.size() - 2);
        return true;
    }
    out = trimmed;
    return true;
}

bool split_flow_items(std::string_view inner, std::vector<std::string>& items) {
    items.clear();
    std::size_t index = 0;
    while (index < inner.size()) {
        while (index < inner.size() &&
               (inner[index] == ' ' || inner[index] == '\t' || inner[index] == ',')) {
            ++index;
        }
        if (index >= inner.size()) {
            break;
        }
        const char quote = (inner[index] == '"' || inner[index] == '\'') ? inner[index] : '\0';
        const std::size_t begin = index;
        if (quote != '\0') {
            ++index;
            while (index < inner.size() && inner[index] != quote) {
                ++index;
            }
            if (index >= inner.size()) {
                return false;
            }
            ++index;
        }
        while (index < inner.size() && inner[index] != ',') {
            ++index;
        }
        const std::string item = trim(inner.substr(begin, index - begin));
        std::string       parsed;
        if (!parse_scalar(item, parsed)) {
            return false;
        }
        items.push_back(std::move(parsed));
    }
    return true;
}

bool parse_flow_sequence(std::string_view raw, std::vector<std::string>& items) {
    const std::string trimmed = trim(raw);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
        return false;
    }
    return split_flow_items(std::string_view{trimmed}.substr(1, trimmed.size() - 2), items);
}

std::string truncate_description(std::string description, std::size_t max_bytes) {
    if (description.size() <= max_bytes) {
        return description;
    }
    std::string truncated = description.substr(0, max_bytes);
    const std::size_t space = truncated.find_last_of(" \t");
    if (space != std::string::npos) {
        truncated.resize(space);
    }
    while (!truncated.empty() && (truncated.back() == ' ' || truncated.back() == '\t')) {
        truncated.pop_back();
    }
    return truncated;
}

} // namespace

FrontmatterResult parse_skill_file(std::string_view content,
                                   std::string_view expected_name,
                                   std::size_t max_frontmatter_bytes,
                                   std::size_t max_description_bytes) {
    FrontmatterResult result;
    const std::string normalized = normalize_line_endings(strip_bom(std::string{content}));
    const std::vector<std::string_view> lines = split_lines(normalized);

    if (lines.empty() || trim(lines.front()) != kFence) {
        result.error = "missing opening '---' fence";
        return result;
    }

    std::size_t close_index = lines.size();
    std::size_t region_bytes = 0;
    for (std::size_t index = 1; index < lines.size(); ++index) {
        region_bytes += lines[index].size() + 1;
        if (region_bytes > max_frontmatter_bytes) {
            result.error = "frontmatter exceeds max_frontmatter_bytes";
            return result;
        }
        if (trim(lines[index]) == kFence) {
            close_index = index;
            break;
        }
    }
    if (close_index == lines.size()) {
        result.error = "missing closing '---' fence";
        return result;
    }

    bool has_name = false;
    bool has_description = false;
    std::vector<std::string> seen_keys;
    std::uint32_t version = 1;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> tags;
    std::string license;
    std::string model;

    const auto assign = [&](const std::string& key, std::string_view raw) -> bool {
        if (key == "allowed-tools") {
            return parse_flow_sequence(raw, allowed_tools);
        }
        if (key == "tags") {
            return parse_flow_sequence(raw, tags);
        }
        std::string scalar;
        if (!parse_scalar(raw, scalar)) {
            return false;
        }
        if (key == "name") {
            result.meta.name.value = std::move(scalar);
            has_name = true;
        } else if (key == "description") {
            result.meta.description = std::move(scalar);
            has_description = true;
        } else if (key == "version") {
            const std::string trimmed = trim(scalar);
            std::uint32_t parsed = 0;
            const char* begin = trimmed.data();
            const char* end = begin + trimmed.size();
            const auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end) {
                return false;
            }
            version = parsed;
        } else if (key == "license") {
            license = std::move(scalar);
        } else if (key == "model") {
            model = std::move(scalar);
        }
        return true;
    };

    for (std::size_t index = 1; index < close_index; ++index) {
        const std::string line = trim(lines[index]);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            result.error = "comments are not supported";
            return result;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            result.error = "expected 'key: value'";
            return result;
        }
        const std::string key = trim(std::string_view{line}.substr(0, colon));
        if (key.empty() || !std::all_of(key.begin(), key.end(), is_key_char)) {
            result.error = "invalid key";
            return result;
        }
        static constexpr std::string_view kKnownKeys[] = {
            "name", "description", "version", "allowed-tools", "tags", "license", "model"};
        if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), key) ==
            std::end(kKnownKeys)) {
            result.error = "unknown key '" + key + "'";
            return result;
        }
        if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end()) {
            result.error = "duplicate key '" + key + "'";
            return result;
        }
        seen_keys.push_back(key);
        std::string_view raw = std::string_view{line}.substr(colon + 1);
        const std::string trimmed_raw = trim(raw);
        if (is_block_scalar(trimmed_raw)) {
            result.error = "unsupported construct (block scalar)";
            return result;
        }
        if (!trimmed_raw.empty() &&
            (trimmed_raw.front() == '&' || trimmed_raw.front() == '*' ||
             trimmed_raw.front() == '!')) {
            result.error = "unsupported construct";
            return result;
        }
        if (!assign(key, raw)) {
            result.error = "invalid value for '" + key + "'";
            return result;
        }
    }

    if (!has_name || result.meta.name.value.empty()) {
        result.error = "missing required field 'name'";
        return result;
    }
    if (!has_description || result.meta.description.empty()) {
        result.error = "missing required field 'description'";
        return result;
    }
    if (!is_valid_skill_name(result.meta.name.value)) {
        result.error = "invalid skill name '" + result.meta.name.value + "'";
        return result;
    }
    if (result.meta.name.value != expected_name) {
        result.error = "name '" + result.meta.name.value + "' does not match directory '" +
                       std::string{expected_name} + "'";
        return result;
    }

    if (result.meta.description.size() > max_description_bytes) {
        result.meta.description = truncate_description(result.meta.description, max_description_bytes);
        result.warnings.push_back("description truncated to " +
                                  std::to_string(max_description_bytes) + " bytes");
    }
    result.meta.version = version;
    result.meta.allowed_tools = std::move(allowed_tools);
    result.meta.tags = std::move(tags);
    result.meta.license = std::move(license);
    result.meta.model = std::move(model);

    std::string body;
    for (std::size_t index = close_index + 1; index < lines.size(); ++index) {
        if (index != close_index + 1) {
            body.push_back('\n');
        }
        body.append(lines[index]);
    }
    while (!body.empty() && body.front() == '\n') {
        body.erase(0, 1);
    }
    while (!body.empty() && body.back() == '\n') {
        body.pop_back();
    }
    if (!body.empty()) {
        body.push_back('\n');
    }
    result.body = std::move(body);
    return result;
}

} // namespace ymh

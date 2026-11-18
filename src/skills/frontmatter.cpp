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
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool is_indented(std::string_view line) {
    return !line.empty() && (line.front() == ' ' || line.front() == '\t');
}

bool is_key(std::string_view key) {
    return !key.empty() && std::all_of(key.begin(), key.end(), is_key_char);
}

// Frontmatter keys OpenCode understands but ymh does not interpret. They are
// accepted and ignored so an OpenCode skill loads unchanged.
bool is_ignored_compat_key(std::string_view key) {
    static constexpr std::string_view kIgnored[] = {
        "metadata", "compatibility", "disable-model-invocation", "agent",
        "mode",     "temperature",   "top_p",           "top-p",
        "top_k",    "reasoning-effort"};
    return std::find(std::begin(kIgnored), std::end(kIgnored), key) !=
           std::end(kIgnored);
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

bool split_comma_list(std::string_view raw, std::vector<std::string>& items) {
    std::size_t begin = 0;
    while (begin <= raw.size()) {
        const std::size_t end = raw.find(',', begin);
        const std::string_view piece =
            raw.substr(begin, end == std::string_view::npos ? std::string_view::npos
                                                            : end - begin);
        std::string value;
        if (!parse_scalar(piece, value)) {
            return false;
        }
        if (!value.empty()) {
            items.push_back(std::move(value));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

// `allowed-tools: [read, write]` (ymh) and `tools: bash, read, write, edit`
// (OpenCode / Claude agent files) both describe an advisory tool list.
bool parse_string_list(std::string_view raw, std::vector<std::string>& items) {
    const std::string trimmed = trim(raw);
    if (!trimmed.empty() && trimmed.front() == '[') {
        return parse_flow_sequence(raw, items);
    }
    return split_comma_list(raw, items);
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

    // Leading blank lines are tolerated. A `---` fence is the canonical form;
    // an unfenced `key: value` header is accepted for OpenCode/Claude skills
    // that were written without one.
    std::size_t start = 0;
    while (start < lines.size() && trim(lines[start]).empty()) {
        ++start;
    }
    if (start == lines.size()) {
        result.error = "empty SKILL.md";
        return result;
    }

    const bool fenced = trim(lines[start]) == kFence;
    std::size_t header_begin = start;
    std::size_t header_end   = lines.size();
    std::size_t body_begin   = lines.size();
    std::size_t region_bytes = 0;

    if (fenced) {
        header_begin = start + 1;
        for (std::size_t index = header_begin; index < lines.size(); ++index) {
            region_bytes += lines[index].size() + 1;
            if (region_bytes > max_frontmatter_bytes) {
                result.error = "frontmatter exceeds max_frontmatter_bytes";
                return result;
            }
            if (trim(lines[index]) == kFence) {
                header_end = index;
                body_begin = index + 1;
                break;
            }
        }
        if (body_begin == lines.size()) {
            result.error = "missing closing '---' fence";
            return result;
        }
    } else {
        // Accept a bare header block. A line only belongs to the header while it
        // is `key: value` or an indented continuation; the first other line
        // starts the body. At least `name` or `description` must be present so
        // ordinary prose is never mistaken for frontmatter.
        std::size_t index = header_begin;
        bool        saw_header_key = false;
        while (index < lines.size()) {
            const std::string_view raw = lines[index];
            if (trim(raw).empty()) {
                break;
            }
            if (is_indented(raw)) {
                ++index;
                continue;
            }
            const std::string line = trim(raw);
            if (line.front() == '#') {
                break;
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                break;
            }
            const std::string key = trim(std::string_view{line}.substr(0, colon));
            if (!is_key(key)) {
                break;
            }
            if (key == "name" || key == "description") {
                saw_header_key = true;
            }
            ++index;
        }
        if (!saw_header_key) {
            result.error =
                "missing required field 'description' (add frontmatter or a leading "
                "'description:' line)";
            return result;
        }
        header_end = index;
        body_begin = index;
        for (std::size_t i = header_begin; i < header_end; ++i) {
            region_bytes += lines[i].size() + 1;
        }
        if (region_bytes > max_frontmatter_bytes) {
            result.error = "frontmatter exceeds max_frontmatter_bytes";
            return result;
        }
    }

    bool has_description = false;
    std::string frontmatter_name;
    bool has_name_line = false;
    std::vector<std::string> seen_keys;
    std::uint32_t version = 1;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> tags;
    std::string license;
    std::string model;

    const auto assign = [&](const std::string& key, std::string_view raw) -> bool {
        if (key == "allowed-tools" || key == "tools") {
            return parse_string_list(raw, allowed_tools);
        }
        if (key == "tags") {
            return parse_string_list(raw, tags);
        }
        if (is_ignored_compat_key(key)) {
            return true;
        }
        std::string scalar;
        if (!parse_scalar(raw, scalar)) {
            return false;
        }
        if (key == "name") {
            if (!scalar.empty()) {
                frontmatter_name = std::move(scalar);
                has_name_line   = true;
            }
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

    for (std::size_t index = header_begin; index < header_end; ++index) {
        const std::string_view raw = lines[index];
        if (trim(raw).empty()) {
            continue;
        }
        // Nested YAML (`metadata:` children) is preserved only as ignored text;
        // the subset parser does not interpret it.
        if (is_indented(raw)) {
            continue;
        }
        const std::string line = trim(raw);
        if (line.front() == '#') {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            result.error = "expected 'key: value'";
            return result;
        }
        const std::string key = trim(std::string_view{line}.substr(0, colon));
        if (!is_key(key)) {
            result.error = "invalid key";
            return result;
        }
        static constexpr std::string_view kKnownKeys[] = {
            "name", "description", "version", "allowed-tools", "tools", "tags", "license",
            "model"};
        const bool known =
            std::find(std::begin(kKnownKeys), std::end(kKnownKeys), key) !=
            std::end(kKnownKeys);
        if (!known && !is_ignored_compat_key(key)) {
            result.warnings.push_back("ignored unknown frontmatter key '" + key + "'");
            continue;
        }
        if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end()) {
            result.warnings.push_back("ignored duplicate frontmatter key '" + key + "'");
            continue;
        }
        seen_keys.push_back(key);
        std::string_view raw_value = std::string_view{line}.substr(colon + 1);
        const std::string trimmed_raw = trim(raw_value);
        if (is_block_scalar(trimmed_raw)) {
            if (key == "description") {
                result.error = "unsupported construct (block scalar)";
                return result;
            }
            continue;
        }
        if (!trimmed_raw.empty() &&
            (trimmed_raw.front() == '&' || trimmed_raw.front() == '*' ||
             trimmed_raw.front() == '!')) {
            result.error = "unsupported construct";
            return result;
        }
        if (!assign(key, raw_value)) {
            result.error = "invalid value for '" + key + "'";
            return result;
        }
    }

    // OpenCode derives the skill id from the path; the frontmatter `name` is a
    // display label. ymh keeps the directory basename as the addressable id so
    // an existing skill tree (including `improve_ut`) loads unchanged.
    const std::string id = std::string{expected_name};
    if (!is_valid_skill_name(id)) {
        result.error = "invalid skill directory name '" + id + "'";
        return result;
    }
    result.meta.name.value = id;
    if (has_name_line && frontmatter_name != id) {
        result.meta.display_name = frontmatter_name;
        result.warnings.push_back("frontmatter name '" + frontmatter_name +
                                  "' differs from directory '" + id +
                                  "'; using the directory name as the skill id");
    }
    if (!has_description || result.meta.description.empty()) {
        result.error = "missing required field 'description'";
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
    for (std::size_t index = body_begin; index < lines.size(); ++index) {
        if (index != body_begin) {
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

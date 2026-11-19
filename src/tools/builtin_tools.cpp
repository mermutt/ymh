#include "ymh/tools/builtin_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ymh/agent/prompt_images.hpp"
#include "ymh/execution/deadline.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/tools/git_tools.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

nlohmann::json string_type() { return nlohmann::json{{"type", "string"}}; }
nlohmann::json integer_type() { return nlohmann::json{{"type", "integer"}}; }
nlohmann::json boolean_type() { return nlohmann::json{{"type", "boolean"}}; }

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
    return nlohmann::json{{"type", "object"},
                          {"properties", std::move(properties)},
                          {"required", std::move(required)},
                          {"additionalProperties", false}};
}

std::filesystem::path resolve_required_path(const ToolContext& context,
                                            const ToolArguments& arguments) {
    const nlohmann::json& value = arguments.value;
    if (!value.contains("path") || !value["path"].is_string()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "path must be a string"};
    }
    return context.resolve(value["path"].get<std::string>());
}

// 90-A17/90-D13: canonical protection independent of `resolve()`, so it also
// holds under `SandboxMode::DangerFullAccess`.
std::filesystem::path safety_path(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

void reject_protected_path(const ToolContext& context,
                           const std::filesystem::path& path) {
    if (path_is_protected(context.root(), safety_path(path),
                          context.execution().allow_ymh_access())) {
        throw ToolError{ToolErrorCode::PathEscape, "protected path"};
    }
}

std::filesystem::path resolve_optional_base(const ToolContext& context,
                                            const ToolArguments& arguments) {
    const nlohmann::json& value = arguments.value;
    if (value.contains("path") && value["path"].is_string()) {
        return context.resolve(value["path"].get<std::string>());
    }
    return context.root();
}

std::string relative_string(const ToolContext& context,
                            const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path relative =
        std::filesystem::relative(path, context.root(), ec);
    return ec ? path.generic_string() : relative.generic_string();
}

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t newline = content.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back(content.substr(start));
            break;
        }
        lines.push_back(content.substr(start, newline - start));
        start = newline + 1;
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines,
                       std::size_t offset,
                       std::size_t limit) {
    if (offset >= lines.size()) {
        return {};
    }
    const std::size_t end = std::min(lines.size(), offset + limit);
    std::string out;
    for (std::size_t i = offset; i < end; ++i) {
        if (i > offset) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// 120-D2: whole-line trailing-whitespace/CRLF fallback. A normalized line is
// CRLF- and trailing-space/tab-insensitive; the original byte offsets are
// retained so a match maps back to the exact pre-edit range (leading
// indentation is never normalized).
struct LineView {
    std::size_t start{0};
    std::size_t length{0};
    std::string normalized;
};

std::string normalize_line(const std::string& text) {
    std::string normalized = text;
    while (!normalized.empty() && (normalized.back() == '\r' || normalized.back() == ' ' ||
                                   normalized.back() == '\t')) {
        normalized.pop_back();
    }
    return normalized;
}

std::vector<LineView> view_lines(const std::string& text, bool drop_trailing_empty) {
    std::vector<LineView> lines;
    std::size_t pos = 0;
    while (true) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        lines.push_back(LineView{pos, end - pos, normalize_line(text.substr(pos, end - pos))});
        if (newline == std::string::npos) {
            break;
        }
        pos = newline + 1;
    }
    if (drop_trailing_empty && !text.empty() && text.back() == '\n') {
        lines.pop_back();
    }
    return lines;
}

struct FuzzyMatch {
    bool        found{false};
    bool        ambiguous{false};
    std::size_t offset{0};
    std::size_t length{0};
};

// 120-D2: an exact search that finds nothing may fall back to a whole-line
// normalized match. The comparison is line-aligned by construction, so a
// normalized substring that starts or ends mid-line is never a candidate.
FuzzyMatch find_whole_line_fuzzy(const std::string& content, const std::string& old_string) {
    FuzzyMatch match;
    if (old_string.empty()) {
        return match;
    }
    const bool pattern_ended_with_newline = old_string.back() == '\n';
    const std::vector<LineView> pattern = view_lines(old_string, true);
    const std::vector<LineView> lines = view_lines(content, false);
    if (pattern.empty() || pattern.size() > lines.size()) {
        return match;
    }

    std::size_t matches = 0;
    std::size_t index = 0;
    for (std::size_t i = 0; i + pattern.size() <= lines.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (lines[i + j].normalized != pattern[j].normalized) {
                equal = false;
                break;
            }
        }
        if (!equal) {
            continue;
        }
        ++matches;
        if (matches > 1) {
            match.ambiguous = true;
            return match;
        }
        index = i;
    }
    if (matches == 0) {
        return match;
    }

    const LineView& last = lines[index + pattern.size() - 1];
    match.found = true;
    match.offset = lines[index].start;
    match.length = last.start + last.length - match.offset;
    if (pattern_ended_with_newline) {
        const std::size_t terminator = last.start + last.length;
        if (terminator >= content.size() || content[terminator] != '\n') {
            // The pattern asks for a line terminator the file does not have.
            return FuzzyMatch{};
        }
        ++match.length;
    }
    return match;
}

// 120-D3: a deliberately small, single-file patch format. Each hunk starts at
// a line beginning with `@@` (the rest of that line is ignored) and is located
// by its exact old-block content, never by hunk line numbers.
struct PatchLine {
    char        kind{0};  // ' ' context, '-' removed, '+' added
    std::string text;     // line content without the kind byte
};

struct PatchHunk {
    std::string            old_block;
    std::vector<PatchLine> lines;
};

std::string join_block(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

std::vector<PatchHunk> parse_patch(const std::string& patch, std::size_t max_hunks) {
    std::vector<std::string> lines = split_lines(patch);
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();  // the terminator of the final patch line
    }

    std::vector<PatchHunk>   hunks;
    std::vector<PatchLine>   body;
    std::vector<std::string> old_lines;
    bool                     in_hunk = false;

    const auto finish = [&]() {
        if (!in_hunk) {
            return;
        }
        if (old_lines.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "hunk " + std::to_string(hunks.size() + 1) +
                                " has an empty old block (a pure insertion needs a context line)"};
        }
        if (hunks.size() >= max_hunks) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "patch exceeds edit_patch_max_hunks (" + std::to_string(max_hunks) +
                                ")"};
        }
        hunks.push_back(PatchHunk{join_block(old_lines), std::move(body)});
        body.clear();
        old_lines.clear();
    };

    for (const std::string& line : lines) {
        if (line.rfind("@@", 0) == 0) {
            finish();
            in_hunk = true;
            continue;
        }
        if (!in_hunk) {
            // Headers are rejected only before the first hunk: inside a hunk a
            // `---`/`+++` line is ordinary body text (`+` with content `++...`).
            if (line.rfind("diff --git", 0) == 0 || line.rfind("---", 0) == 0 ||
                line.rfind("+++", 0) == 0) {
                throw ToolError{
                    ToolErrorCode::InvalidArguments,
                    "patch must not include diff/---/+++ headers; the file is named by path"};
            }
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "malformed patch: content before the first @@ hunk header"};
        }
        if (line.empty() || line.front() == '\\') {
            throw ToolError{
                ToolErrorCode::InvalidArguments,
                "malformed patch line: a body line must start with ' ', '-' or '+' and a `\\` "
                "marker is rejected"};
        }
        switch (line.front()) {
        case ' ':
            old_lines.push_back(line.substr(1));
            body.push_back(PatchLine{' ', line.substr(1)});
            break;
        case '-':
            old_lines.push_back(line.substr(1));
            body.push_back(PatchLine{'-', line.substr(1)});
            break;
        case '+':
            body.push_back(PatchLine{'+', line.substr(1)});
            break;
        default:
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "malformed patch line: a body line must start with ' ', '-' or '+'"};
        }
    }
    finish();
    if (hunks.empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "patch contains no hunks"};
    }
    return hunks;
}

// 120-D3/D4: splits one located hunk into its minimal changed line groups. A
// group is a run of '-'/'+' lines between context lines; its old side is the
// removed lines' bytes and its new side is the added lines. Applying groups
// rather than the whole old block keeps the hunk's context lines as context in
// the diff result.
struct Replacement {
    std::size_t offset{0};
    std::string old_text;
    std::string new_text;
    bool        line_insert{false};  // pure insertion at a line boundary
};

std::vector<Replacement> patch_replacements(const std::string& content,
                                            std::size_t block_offset,
                                            const PatchHunk& hunk) {
    std::vector<Replacement> replacements;
    std::size_t              cursor = block_offset;
    bool                     in_group = false;
    std::size_t              group_start = 0;
    std::size_t              group_end = 0;
    std::vector<std::string> added;

    const auto flush = [&]() {
        if (!in_group) {
            return;
        }
        Replacement replacement;
        replacement.offset = group_start;
        replacement.old_text = content.substr(group_start, group_end - group_start);
        replacement.new_text = join_block(added);
        if (replacement.old_text.empty()) {
            // Pure insertion: place the added lines at the current line start.
            // At EOF after an unterminated last line the insertion completes
            // that line first, so the leading `\n` stays in the inserted text
            // and no existing line is reported as changed.
            const bool completes_last_line =
                group_start == content.size() && group_start > 0 &&
                content[group_start - 1] != '\n';
            replacement.line_insert = group_start == 0 ||
                                      content[group_start - 1] == '\n' ||
                                      completes_last_line;
            if (completes_last_line) {
                replacement.new_text.insert(0, 1, '\n');
            } else if (replacement.line_insert) {
                if (group_start < content.size()) {
                    replacement.new_text.push_back('\n');
                }
            } else {
                replacement.new_text.insert(0, 1, '\n');
            }
        } else if (!replacement.new_text.empty() && replacement.old_text.back() == '\n') {
            replacement.new_text.push_back('\n');
        }
        replacements.push_back(std::move(replacement));
        added.clear();
        in_group = false;
    };

    for (const PatchLine& line : hunk.lines) {
        if (line.kind == ' ') {
            flush();
            cursor += line.text.size();
            if (cursor < content.size() && content[cursor] == '\n') {
                ++cursor;
            }
            continue;
        }
        if (!in_group) {
            in_group = true;
            group_start = cursor;
            group_end = cursor;
        }
        if (line.kind == '-') {
            group_end = cursor + line.text.size();
            if (group_end < content.size() && content[group_end] == '\n') {
                ++group_end;
            }
            cursor = group_end;
        } else {
            added.push_back(line.text);
        }
    }
    flush();
    return replacements;
}

// ---------------------------------------------------------------------------
// 120-D4: unified diff for edit-tool results.
//
// Hunks are accumulated in the pre-call line space as each edit is applied, so
// a sequential `multi_edit` keeps one hunk per change. Each hunk's `-` side is
// the pre-call content and its `+` side is the final content, with line numbers
// computed against the corresponding content. Display merging is a union of
// overlapping context ranges only.
// ---------------------------------------------------------------------------

constexpr std::size_t kDiffContextLines = 3;

struct EditHunk {
    std::size_t orig_start{0};  // 0-based line index in the pre-call content
    std::size_t orig_count{0};  // lines removed from the pre-call content
    std::size_t new_count{0};   // lines present in the final content
};

std::size_t line_of(const std::string& text, std::size_t position) {
    return static_cast<std::size_t>(
        std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(position), '\n'));
}

std::size_t line_start_of(const std::string& text, std::size_t line) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < line; ++i) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string::npos) {
            return text.size();
        }
        start = newline + 1;
    }
    return start;
}

// Index of the end of `line`: the terminating `\n`, or the end of the text.
std::size_t line_end_of(const std::string& text, std::size_t line) {
    const std::size_t start = line_start_of(text, line);
    const std::size_t newline = text.find('\n', start);
    return newline == std::string::npos ? text.size() : newline;
}

class EditDiffBuilder {
public:
    explicit EditDiffBuilder(std::string content)
        : original_(std::move(content)), content_(original_) {}

    [[nodiscard]] const std::string& content() const noexcept { return content_; }

    // Applies one replacement and records its hunk. `offset` and `old_len` are
    // relative to the current (already edited) content.
    void replace(std::size_t offset, std::size_t old_len, const std::string& new_text) {
        record(offset, old_len, new_text);
        content_.replace(offset, old_len, new_text);
    }

    // Inserts whole lines at a line boundary. Unlike `replace`, no existing
    // line is marked changed, so an `apply_patch` insertion renders as `+`
    // lines between context lines (120-D4). A leading `\n` that terminates an
    // existing unterminated line completes that line instead of adding a line:
    // the hunk is anchored on the line after it and `new_count` drops by one.
    void insert_lines(std::size_t offset, const std::string& text) {
        if (text.empty()) {
            return;
        }
        std::size_t new_count =
            static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
        if (text.back() != '\n') {
            ++new_count;
        }
        std::size_t line = line_of(content_, offset);
        if (offset > 0 && content_[offset - 1] != '\n' && text.front() == '\n') {
            --new_count;  // the leading newline terminates the existing line
            ++line;       // the hunk starts after the completed line
        }
        EditHunk hunk;
        hunk.orig_start = current_line_to_orig_start(line);
        hunk.orig_count = 0;
        hunk.new_count = new_count;
        const auto position = std::lower_bound(
            hunks_.begin(), hunks_.end(), hunk.orig_start,
            [](const EditHunk& existing, std::size_t value) {
                return existing.orig_start < value;
            });
        hunks_.insert(position, hunk);
        content_.insert(offset, text);
    }

    [[nodiscard]] std::string render(const std::string& relative_path,
                                     std::size_t max_bytes) const {
        std::string out = "diff --git a/" + relative_path + " b/" + relative_path + "\n";
        out += "--- a/" + relative_path + "\n";
        out += "+++ b/" + relative_path + "\n";

        const std::vector<std::string> pre = display_lines(original_);
        const std::vector<std::string> post = display_lines(content_);
        // A trailing newline terminates the last line; it does not add an empty
        // final line to the display, so hunk counts ignore split_lines' phantom
        // trailing element.
        const std::size_t pre_lines =
            !original_.empty() && original_.back() == '\n' && !pre.empty()
                ? pre.size() - 1
                : pre.size();
        const std::size_t post_lines =
            !content_.empty() && content_.back() == '\n' && !post.empty()
                ? post.size() - 1
                : post.size();

        struct Display {
            std::size_t first_hunk{0};
            std::size_t last_hunk{0};
            std::size_t pre_start{0};
            std::size_t pre_end{0};
            std::size_t post_start{0};
            std::size_t post_end{0};
        };
        std::vector<Display> displays;
        for (std::size_t i = 0; i < hunks_.size(); ++i) {
            const EditHunk& hunk = hunks_[i];
            const std::size_t post_start =
                final_index_of_orig(hunk.orig_start, false);
            const std::size_t post_end =
                final_index_of_orig(hunk.orig_start + hunk.orig_count, true);
            Display display;
            display.first_hunk = i;
            display.last_hunk = i;
            display.pre_start = hunk.orig_start > kDiffContextLines
                                    ? hunk.orig_start - kDiffContextLines
                                    : 0;
            display.pre_end =
                std::min(pre_lines, hunk.orig_start + hunk.orig_count + kDiffContextLines);
            display.post_start =
                post_start > kDiffContextLines ? post_start - kDiffContextLines : 0;
            display.post_end = std::min(post_lines, post_end + kDiffContextLines);
            if (!displays.empty() && display.pre_start <= displays.back().pre_end &&
                display.post_start <= displays.back().post_end) {
                displays.back().last_hunk = i;
                displays.back().pre_end = std::max(displays.back().pre_end, display.pre_end);
                displays.back().post_end = std::max(displays.back().post_end, display.post_end);
            } else {
                displays.push_back(display);
            }
        }

        std::size_t emitted = 0;
        for (const Display& display : displays) {
            std::string body = "@@ -" + std::to_string(display.pre_start + 1) + "," +
                               std::to_string(display.pre_end - display.pre_start);
            body += " +" + std::to_string(display.post_start + 1) + "," +
                    std::to_string(display.post_end - display.post_start) + " @@\n";
            std::size_t pre_cursor = display.pre_start;
            std::size_t post_cursor = display.post_start;
            for (std::size_t i = display.first_hunk; i <= display.last_hunk; ++i) {
                const EditHunk& hunk = hunks_[i];
                while (pre_cursor < hunk.orig_start) {
                    body += " " + pre[pre_cursor] + "\n";
                    ++pre_cursor;
                    ++post_cursor;
                }
                for (std::size_t p = hunk.orig_start; p < hunk.orig_start + hunk.orig_count; ++p) {
                    body += "-" + pre[p] + "\n";
                }
                for (std::size_t q = post_cursor;
                     q < post_cursor + hunk.new_count && q < post.size(); ++q) {
                    body += "+" + post[q] + "\n";
                }
                pre_cursor = hunk.orig_start + hunk.orig_count;
                post_cursor += hunk.new_count;
            }
            while (pre_cursor < display.pre_end && pre_cursor < pre_lines) {
                body += " " + pre[pre_cursor] + "\n";
                ++pre_cursor;
            }
            if (out.size() + body.size() > max_bytes) {
                break;
            }
            out += body;
            ++emitted;
        }
        if (emitted < displays.size()) {
            out += "# diff truncated (" + std::to_string(displays.size() - emitted) +
                   " more hunk(s))\n";
        }
        return out;
    }

private:
    static std::vector<std::string> display_lines(const std::string& content) {
        std::vector<std::string> lines = split_lines(content);
        for (std::string& line : lines) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
        }
        return lines;
    }

    // Final-content line index at the boundary `orig_idx` in the pre-call line
    // space, given the hunks recorded so far. `include_boundary` selects the
    // position after (`true`) or before (`false`) a zero-length insertion hunk
    // anchored exactly at `orig_idx`. Today a replacement is never anchored at
    // the same orig line as a prior zero-count insertion: edits are applied in
    // ascending offset order and the overlap rule keeps them disjoint, so the
    // `false` boundary rule is exercised only by hunk display ranges.
    [[nodiscard]] std::size_t final_index_of_orig(std::size_t orig_idx,
                                                  bool include_boundary = true) const {
        std::ptrdiff_t delta = 0;
        for (const EditHunk& hunk : hunks_) {
            const std::size_t end = hunk.orig_start + hunk.orig_count;
            if (end < orig_idx ||
                (end == orig_idx && (include_boundary || hunk.orig_count > 0))) {
                delta += static_cast<std::ptrdiff_t>(hunk.new_count) -
                         static_cast<std::ptrdiff_t>(hunk.orig_count);
            }
        }
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(orig_idx) + delta);
    }

    [[nodiscard]] std::size_t current_line_to_orig_start(std::size_t line) const {
        std::size_t final_cursor = 0;
        std::size_t orig_cursor = 0;
        for (const EditHunk& hunk : hunks_) {
            final_cursor += hunk.orig_start - orig_cursor;
            orig_cursor = hunk.orig_start;
            if (line < final_cursor) {
                break;
            }
            const std::size_t final_end = final_cursor + hunk.new_count;
            if (line < final_end) {
                return hunk.orig_start;
            }
            final_cursor = final_end;
            orig_cursor = hunk.orig_start + hunk.orig_count;
        }
        return orig_cursor + (line - final_cursor);
    }

    [[nodiscard]] std::size_t current_line_to_orig_end(std::size_t line) const {
        std::size_t final_cursor = 0;
        std::size_t orig_cursor = 0;
        for (const EditHunk& hunk : hunks_) {
            final_cursor += hunk.orig_start - orig_cursor;
            orig_cursor = hunk.orig_start;
            if (line < final_cursor) {
                break;
            }
            const std::size_t final_end = final_cursor + hunk.new_count;
            if (line < final_end) {
                return hunk.orig_count == 0 ? hunk.orig_start
                                            : hunk.orig_start + hunk.orig_count - 1;
            }
            final_cursor = final_end;
            orig_cursor = hunk.orig_start + hunk.orig_count;
        }
        return orig_cursor + (line - final_cursor);
    }

    void record(std::size_t offset, std::size_t old_len, const std::string& new_text) {
        const std::string before = content_.substr(offset, old_len);
        if (before == new_text) {
            return;  // no-op replacement: no hunk
        }

        const std::size_t start_line = line_of(content_, offset);
        const std::size_t end_line =
            old_len == 0 ? start_line : line_of(content_, offset + old_len - 1);
        const std::size_t old_span = end_line - start_line + 1;
        const std::size_t line_start = line_start_of(content_, start_line);
        const std::size_t line_end = line_end_of(content_, end_line);
        const std::string prefix = content_.substr(line_start, offset - line_start);
        const std::size_t capped_end = std::min(offset + old_len, line_end);
        const std::string suffix = content_.substr(capped_end, line_end - capped_end);
        const std::string block = prefix + new_text + suffix;

        std::size_t new_count = 0;
        if (block.empty()) {
            // The replacement produced no bytes. A line survives as an empty
            // line unless the edit also swallowed the terminator of a
            // terminated line, or removed the tail of an unterminated last
            // line outright.
            const bool swallowed_terminator = old_len > 0 && offset + old_len > line_end;
            const bool removed_unterminated_tail =
                old_len > 0 && offset + old_len >= line_end && line_end == content_.size();
            new_count = (swallowed_terminator || removed_unterminated_tail) ? 0 : 1;
        } else {
            new_count = static_cast<std::size_t>(
                            std::count(block.begin(), block.end(), '\n')) +
                        1;
            if (block.back() == '\n') {
                --new_count;
            }
        }

        const std::size_t region_start = current_line_to_orig_start(start_line);
        const std::size_t region_end = current_line_to_orig_end(end_line) + 1;

        std::size_t           merged_start = region_start;
        std::size_t           merged_end = region_end;
        std::vector<EditHunk> kept;
        kept.reserve(hunks_.size());
        for (const EditHunk& hunk : hunks_) {
            const std::size_t hunk_start = hunk.orig_start;
            const std::size_t hunk_end = hunk.orig_start + hunk.orig_count;
            if (hunk_end > region_start && hunk_start < region_end) {
                merged_start = std::min(merged_start, hunk_start);
                merged_end = std::max(merged_end, hunk_end);
            } else {
                kept.push_back(hunk);
            }
        }
        const std::size_t old_final = final_index_of_orig(merged_end, true) -
                                      final_index_of_orig(merged_start, false);
        EditHunk merged;
        merged.orig_start = merged_start;
        merged.orig_count = merged_end - merged_start;
        merged.new_count = old_final + new_count - old_span;

        const auto position = std::lower_bound(
            kept.begin(), kept.end(), merged.orig_start,
            [](const EditHunk& hunk, std::size_t value) { return hunk.orig_start < value; });
        kept.insert(position, merged);
        hunks_ = std::move(kept);
    }

    std::string           original_;
    std::string           content_;
    std::vector<EditHunk> hunks_;
};

class ReadFileTool final : public Tool {
public:
    explicit ReadFileTool([[maybe_unused]] ToolConfig config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"read_file"},
            ToolVersion{1, 0},
            "Read a UTF-8 text file under the workspace root or the /tmp scratch area and return "
            "its content. Use offset "
            "(1-based) and limit to page through large files; output may be truncated. Read a "
            "file before editing it.",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"offset", integer_type()},
                                         {"limit", integer_type()}},
                          nlohmann::json::array({"path"})),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        reject_protected_path(context, path);
        const FileData data = context.execution().fs().read(path).get();

        ToolResult result;
        result.id = context.callId();
        result.name = "read_file";
        std::string content = data.bytes;
        if (arguments.value.contains("offset") || arguments.value.contains("limit")) {
            const std::size_t offset = arguments.value.value("offset", 0);
            const std::size_t limit =
                arguments.value.contains("limit")
                    ? arguments.value["limit"].get<std::size_t>()
                    : static_cast<std::size_t>(-1);
            content = join_lines(split_lines(content), offset, limit);
        }
        result.output = std::move(content);
        result.truncated = data.truncated;
        return Task<ToolResult>(std::move(result));
    }
};

class ReadImageTool final : public Tool {
public:
    explicit ReadImageTool(ToolConfig config) : config_(std::move(config)) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"read_image"},
            ToolVersion{1, 0},
            "Read a PNG, JPEG, GIF, or WebP image under the workspace root or the /tmp "
            "scratch area and attach it to the conversation so you can see it. Use this "
            "to inspect screenshots, diagrams, and photos; read_file cannot show image "
            "contents. Fails when the file is not a supported image or exceeds the image "
            "size limit.",
            object_schema(nlohmann::json{{"path", string_type()}},
                          nlohmann::json::array({"path"})),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        reject_protected_path(context, path);
        const FileData data =
            context.execution().fs().read(path, config_.read_image_max_bytes).get();

        ToolResult result;
        result.id   = context.callId();
        result.name = "read_image";
        if (data.truncated) {
            result.outcome = payload::ToolOutcome::Error;
            result.error = "image exceeds read_image_max_bytes (" +
                           std::to_string(config_.read_image_max_bytes) + " bytes)";
            result.output  = *result.error;
            return Task<ToolResult>(std::move(result));
        }

        std::string reason;
        const std::optional<EncodedImage> encoded =
            encode_image(data.bytes, config_.read_image_max_bytes, reason);
        if (!encoded.has_value()) {
            result.outcome = payload::ToolOutcome::Error;
            result.error   = reason;
            result.output  = reason;
            return Task<ToolResult>(std::move(result));
        }

        ContentBlock image;
        image.kind       = ContentBlockKind::Image;
        image.media_type = encoded->media_type;
        image.data       = encoded->data;
        result.output = "attached image " + relative_string(context, path) + " (" +
                        encoded->media_type + ", " + std::to_string(data.bytes.size()) +
                        " bytes)";
        result.images.push_back(std::move(image));
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

class WriteFileTool final : public Tool {
public:
    explicit WriteFileTool([[maybe_unused]] ToolConfig config) {}

    [[nodiscard]] std::vector<std::filesystem::path>
    checkpoint_paths(const ToolContext& /*context*/,
                     const ToolArguments& arguments) const override {
        return {std::filesystem::path{arguments.value.at("path").get<std::string>()}};
    }

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"write_file"},
            ToolVersion{1, 0},
            "Create or fully replace a UTF-8 text file under the workspace root or the /tmp "
            "scratch area. The content is "
            "written verbatim, overwriting any existing file; use edit_file for targeted changes "
            "to an existing file.",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"content", string_type()}},
                          nlohmann::json::array({"path", "content"})),
            true};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        reject_protected_path(context, path);
        const std::string content = arguments.value.at("content").get<std::string>();
        context.execution().fs().write(path, Data{content}).get();

        ToolResult result;
        result.id = context.callId();
        result.name = "write_file";
        result.output = "wrote " + std::to_string(content.size()) + " bytes to " +
                        relative_string(context, path);
        return Task<ToolResult>(std::move(result));
    }
};

class EditFileTool final : public Tool {
public:
    explicit EditFileTool(ToolConfig config) : config_(config) {}

    [[nodiscard]] std::vector<std::filesystem::path>
    checkpoint_paths(const ToolContext& /*context*/,
                     const ToolArguments& arguments) const override {
        return {std::filesystem::path{arguments.value.at("path").get<std::string>()}};
    }

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"edit_file"},
            ToolVersion{1, 0},
            "Replace a literal string in an existing UTF-8 text file under the workspace root or "
            "the /tmp scratch area. "
            "old_string must match exactly; unless replace_all is true it must appear exactly "
            "once, so include enough surrounding text to be unique. If it is absent, a whole-line "
            "block that differs only in trailing whitespace or CRLF is matched as a fallback. "
            "Prefer this over shell/sed/Python for one targeted edit; the result includes a diff.",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"old_string", string_type()},
                                         {"new_string", string_type()},
                                         {"replace_all", boolean_type()}},
                          nlohmann::json::array(
                              {"path", "old_string", "new_string"})),
            true,
            ToolConcurrencyMode::Exclusive};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        reject_protected_path(context, path);
        const std::string old_string =
            arguments.value.at("old_string").get<std::string>();
        const std::string new_string =
            arguments.value.at("new_string").get<std::string>();
        if (old_string.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "old_string must not be empty"};
        }

        const FileData data = context.execution().fs().read(path).get();
        if (data.truncated) {
            throw ToolError{ToolErrorCode::ResourceExhausted,
                            "file exceeds read_file_max_bytes (" +
                                std::to_string(config_.read_file_max_bytes) +
                                "); refusing to edit a truncated read"};
        }

        const bool        replace_all = arguments.value.value("replace_all", false);
        const std::size_t count = count_occurrences(data.bytes, old_string);
        EditDiffBuilder   diff{data.bytes};
        std::string       summary;
        const std::string relative = relative_string(context, path);

        if (count == 0) {
            if (replace_all) {
                throw ToolError{
                    ToolErrorCode::NotFound,
                    "old_string not found; replace_all does not use the trailing-whitespace "
                    "fallback -- re-read the file and use the exact text, or split the change "
                    "into explicit edits"};
            }
            const FuzzyMatch match = find_whole_line_fuzzy(data.bytes, old_string);
            if (match.ambiguous) {
                throw ToolError{ToolErrorCode::InvalidArguments,
                                "old_string matched multiple whole-line blocks after "
                                "normalizing trailing whitespace; include more context"};
            }
            if (!match.found) {
                throw ToolError{ToolErrorCode::NotFound, "old_string not found"};
            }
            diff.replace(match.offset, match.length, new_string);
            summary = "replaced 1 occurrence(s) in " + relative +
                      " (matched ignoring trailing whitespace)";
        } else if (replace_all) {
            std::size_t applied = 0;
            std::size_t position = 0;
            while (true) {
                const std::size_t found = diff.content().find(old_string, position);
                if (found == std::string::npos) {
                    break;
                }
                diff.replace(found, old_string.size(), new_string);
                ++applied;
                position = found + new_string.size();
            }
            summary = "replaced " + std::to_string(applied) + " occurrence(s) in " + relative;
        } else {
            if (count > 1) {
                throw ToolError{ToolErrorCode::InvalidArguments,
                                "old_string matched " + std::to_string(count) +
                                    " times; include more context or set replace_all"};
            }
            diff.replace(data.bytes.find(old_string), old_string.size(), new_string);
            summary = "replaced 1 occurrence(s) in " + relative;
        }

        context.execution().fs().write(path, Data{diff.content()}).get();

        ToolResult result;
        result.id = context.callId();
        result.name = "edit_file";
        result.output = summary + "\n\n" + diff.render(relative, config_.edit_diff_max_bytes);
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

// 120-D1: several literal replacements in one file, validated against the
// evolving content and written atomically.
class MultiEditTool final : public Tool {
public:
    explicit MultiEditTool(ToolConfig config) : config_(config) {}

    [[nodiscard]] std::vector<std::filesystem::path>
    checkpoint_paths(const ToolContext& /*context*/,
                     const ToolArguments& arguments) const override {
        return {std::filesystem::path{arguments.value.at("path").get<std::string>()}};
    }

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"multi_edit"},
            ToolVersion{1, 0},
            "Apply several literal replacements to one existing UTF-8 text file under the "
            "workspace root or /tmp in one atomic "
            "call; all edits are validated before any write, and each edit sees the result of "
            "the earlier ones. Use edit_file for one replacement; the result includes a diff.",
            object_schema(
                nlohmann::json{
                    {"path", string_type()},
                    {"edits",
                     nlohmann::json{{"type", "array"},
                                    {"items",
                                     object_schema(nlohmann::json{{"old_string", string_type()},
                                                                  {"new_string", string_type()},
                                                                  {"replace_all", boolean_type()}},
                                                   nlohmann::json::array(
                                                       {"old_string", "new_string"}))}}}},
                nlohmann::json::array({"path", "edits"})),
            true,
            ToolConcurrencyMode::Exclusive};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        reject_protected_path(context, path);

        const nlohmann::json& edits = arguments.value.at("edits");
        if (!edits.is_array() || edits.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "edits must be a non-empty array"};
        }
        if (edits.size() > config_.edit_max_edits) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "edits exceeds edit_max_edits (" +
                                std::to_string(config_.edit_max_edits) + ")"};
        }

        const FileData data = context.execution().fs().read(path).get();
        if (data.truncated) {
            throw ToolError{ToolErrorCode::ResourceExhausted,
                            "file exceeds read_file_max_bytes (" +
                                std::to_string(config_.read_file_max_bytes) +
                                "); refusing to edit a truncated read"};
        }

        EditDiffBuilder diff{data.bytes};
        for (std::size_t i = 0; i < edits.size(); ++i) {
            const nlohmann::json& edit = edits[i];
            const std::string     old_string = edit.at("old_string").get<std::string>();
            const std::string     new_string = edit.at("new_string").get<std::string>();
            const std::string     prefix = "edit " + std::to_string(i + 1) + " of " +
                                           std::to_string(edits.size()) + ": ";
            if (old_string.empty()) {
                throw ToolError{ToolErrorCode::InvalidArguments,
                                prefix + "old_string must not be empty"};
            }
            const bool        replace_all = edit.value("replace_all", false);
            const std::size_t count = count_occurrences(diff.content(), old_string);
            if (count == 0) {
                throw ToolError{ToolErrorCode::NotFound,
                                prefix + "old_string not found"};
            }
            if (!replace_all) {
                if (count > 1) {
                    throw ToolError{ToolErrorCode::InvalidArguments,
                                    prefix + "old_string matched " + std::to_string(count) +
                                        " times; include more context or set replace_all"};
                }
                diff.replace(diff.content().find(old_string), old_string.size(), new_string);
                continue;
            }
            std::size_t position = 0;
            while (true) {
                const std::size_t found = diff.content().find(old_string, position);
                if (found == std::string::npos) {
                    break;
                }
                diff.replace(found, old_string.size(), new_string);
                position = found + new_string.size();
            }
        }

        context.execution().fs().write(path, Data{diff.content()}).get();

        const std::string relative = relative_string(context, path);
        ToolResult        result;
        result.id = context.callId();
        result.name = "multi_edit";
        result.output = "applied " + std::to_string(edits.size()) + " edit(s) to " + relative +
                        "\n\n" + diff.render(relative, config_.edit_diff_max_bytes);
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

// 120-D3: a small, single-file, multi-hunk patch located by exact context.
class ApplyPatchTool final : public Tool {
public:
    explicit ApplyPatchTool(ToolConfig config) : config_(config) {}

    [[nodiscard]] std::vector<std::filesystem::path>
    checkpoint_paths(const ToolContext& /*context*/,
                     const ToolArguments& arguments) const override {
        return {std::filesystem::path{arguments.value.at("path").get<std::string>()}};
    }

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"apply_patch"},
            ToolVersion{1, 0},
            "Apply a small, single-file, multi-hunk patch under the workspace root or /tmp, "
            "located by exact context. Use for "
            "structural changes that would otherwise require reproducing a large old_string. "
            "Each hunk starts with a line beginning with @@ (its numbers are ignored); body "
            "lines start with ' ' (context), '-' (removed) or '+' (added), and the old block "
            "must occur exactly once. The result includes a diff.",
            object_schema(nlohmann::json{{"path", string_type()}, {"patch", string_type()}},
                          nlohmann::json::array({"path", "patch"})),
            true,
            ToolConcurrencyMode::Exclusive};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        reject_protected_path(context, path);

        const std::string patch = arguments.value.at("patch").get<std::string>();
        if (patch.size() > config_.edit_patch_max_bytes) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "patch exceeds edit_patch_max_bytes (" +
                                std::to_string(config_.edit_patch_max_bytes) + ")"};
        }
        // Parse before reading the file: a malformed patch never touches disk.
        const std::vector<PatchHunk> hunks = parse_patch(patch, config_.edit_patch_max_hunks);

        const FileData data = context.execution().fs().read(path).get();
        if (data.truncated) {
            throw ToolError{ToolErrorCode::ResourceExhausted,
                            "file exceeds read_file_max_bytes (" +
                                std::to_string(config_.read_file_max_bytes) +
                                "); refusing to edit a truncated read"};
        }

        struct Located {
            std::size_t offset{0};  // byte offset of the old block in the pre-call file
            std::size_t hunk{0};
        };
        std::vector<Located> located;
        located.reserve(hunks.size());
        for (std::size_t i = 0; i < hunks.size(); ++i) {
            const std::size_t count = count_occurrences(data.bytes, hunks[i].old_block);
            if (count == 0) {
                throw ToolError{ToolErrorCode::NotFound,
                                "hunk " + std::to_string(i + 1) + " old block not found"};
            }
            if (count > 1) {
                throw ToolError{ToolErrorCode::InvalidArguments,
                                "hunk " + std::to_string(i + 1) + " old block matched " +
                                    std::to_string(count) + " times; include more context"};
            }
            located.push_back(Located{data.bytes.find(hunks[i].old_block), i});
        }
        std::sort(located.begin(), located.end(),
                  [](const Located& left, const Located& right) {
                      return left.offset < right.offset;
                  });
        for (std::size_t i = 1; i < located.size(); ++i) {
            if (located[i - 1].offset + hunks[located[i - 1].hunk].old_block.size() >
                located[i].offset) {
                throw ToolError{ToolErrorCode::InvalidArguments, "patch hunks overlap"};
            }
        }

        // Each hunk is applied as its minimal changed line groups, so the diff
        // keeps the hunk's own context lines as context (120-D4: `apply_patch`
        // sides come from the pre-call file and the requested hunk).
        std::vector<Replacement> replacements;
        for (const Located& hunk : located) {
            const std::vector<Replacement> groups =
                patch_replacements(data.bytes, hunk.offset, hunks[hunk.hunk]);
            replacements.insert(replacements.end(), groups.begin(), groups.end());
        }

        EditDiffBuilder diff{data.bytes};
        std::ptrdiff_t  delta = 0;
        for (const Replacement& replacement : replacements) {
            const std::size_t at = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(replacement.offset) + delta);
            if (replacement.line_insert) {
                diff.insert_lines(at, replacement.new_text);
            } else {
                diff.replace(at, replacement.old_text.size(), replacement.new_text);
            }
            delta += static_cast<std::ptrdiff_t>(replacement.new_text.size()) -
                     static_cast<std::ptrdiff_t>(replacement.old_text.size());
        }

        context.execution().fs().write(path, Data{diff.content()}).get();

        const std::string relative = relative_string(context, path);
        ToolResult        result;
        result.id = context.callId();
        result.name = "apply_patch";
        result.output = "applied " + std::to_string(hunks.size()) + " hunk(s) to " + relative +
                        "\n\n" + diff.render(relative, config_.edit_diff_max_bytes);
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

class GrepTool final : public Tool {
public:
    explicit GrepTool(ToolConfig config) : config_(config) {}

    ToolSchema schema() const override {
        nlohmann::json modes = string_type();
        modes["enum"] = nlohmann::json::array({"content", "files_with_matches", "count"});
        return ToolSchema{
            ToolName{"grep"},
            ToolVersion{1, 0},
            "Search file contents with an ECMAScript regular expression and return the matches. "
            "Narrow the search with path and a single glob filter; choose output_mode content "
            "(matching lines), files_with_matches, or count. Use read_file on a matched file for "
            "surrounding context.",
            object_schema(nlohmann::json{{"pattern", string_type()},
                                         {"path", string_type()},
                                         {"glob", string_type()},
                                         {"output_mode", std::move(modes)},
                                         {"line_numbers", boolean_type()}},
                          nlohmann::json::array({"pattern"})),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        GrepQuery query;
        query.pattern = arguments.value.at("pattern").get<std::string>();
        query.base = resolve_optional_base(context, arguments);
        reject_protected_path(context, query.base);
        query.glob = arguments.value.value("glob", std::string{});
        query.line_numbers = arguments.value.value("line_numbers", true);
        query.max_results = config_.search_max_results;

        const std::vector<GrepMatch> matches =
            context.execution().fs().grep(query).get();
        const std::string mode =
            arguments.value.value("output_mode", std::string{"content"});

        std::string output;
        if (mode == "files_with_matches") {
            std::vector<std::string> seen;
            for (const GrepMatch& match : matches) {
                const std::string name = relative_string(context, match.path);
                if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                    seen.push_back(name);
                }
            }
            for (std::size_t i = 0; i < seen.size(); ++i) {
                if (i > 0) output.push_back('\n');
                output += seen[i];
            }
        } else if (mode == "count") {
            std::map<std::string, std::size_t> counts;
            for (const GrepMatch& match : matches) {
                ++counts[relative_string(context, match.path)];
            }
            bool first = true;
            for (const auto& [name, count] : counts) {
                if (!first) output.push_back('\n');
                first = false;
                output += name + ":" + std::to_string(count);
            }
        } else {
            for (std::size_t i = 0; i < matches.size(); ++i) {
                if (i > 0) output.push_back('\n');
                output += relative_string(context, matches[i].path);
                if (query.line_numbers) {
                    output += ":" + std::to_string(matches[i].line);
                }
                output += ":" + matches[i].text;
            }
        }

        ToolResult result;
        result.id = context.callId();
        result.name = "grep";
        result.output = std::move(output);
        result.truncated = matches.size() >= query.max_results;
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

class GlobTool final : public Tool {
public:
    explicit GlobTool(ToolConfig config) : config_(config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"glob"},
            ToolVersion{1, 0},
            "List files whose paths match a glob pattern under the workspace root, in "
            "modification-time order. A pattern with no '/' matches the basename at any depth "
            "(e.g. \"*.ts\"); include a separator to anchor the depth. Use grep to search file "
            "contents.",
            object_schema(nlohmann::json{{"pattern", string_type()},
                                         {"path", string_type()}},
                          nlohmann::json::array({"pattern"})),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        GlobQuery query;
        query.pattern = arguments.value.at("pattern").get<std::string>();
        query.base = resolve_optional_base(context, arguments);
        reject_protected_path(context, query.base);
        query.max_results = config_.search_max_results;

        const std::vector<std::filesystem::path> files =
            context.execution().fs().glob(query).get();

        std::string output;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (i > 0) output.push_back('\n');
            output += relative_string(context, files[i]);
        }

        ToolResult result;
        result.id = context.callId();
        result.name = "glob";
        result.output = std::move(output);
        result.truncated = files.size() >= query.max_results;
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

class ShellTool final : public Tool {
public:
    explicit ShellTool(ToolConfig config) : config_(config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"shell"},
            ToolVersion{1, 0},
            "Run a shell command with /bin/bash -lc in the workspace root and return its combined "
            "output. Prefer the dedicated tools (read_file, write_file, edit_file, multi_edit, "
            "apply_patch, grep, glob) over their shell equivalents. Do not use shell to edit "
            "files when an edit tool suffices. Avoid commands that may produce a very large "
            "amount of output, and set timeout_ms for commands that may run long.",
            object_schema(nlohmann::json{
                              {"command", string_type()},
                              {"timeout_ms", integer_type()},
                              {"sandbox_permissions",
                               nlohmann::json{{"type", "string"},
                                              {"enum", {"workspace-write",
                                                        "danger-full-access"}}}},
                              {"justification", string_type()}},
                          nlohmann::json::array({"command"})),
            true};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::string command = arguments.value.at("command").get<std::string>();

        SubprocessSlot slot(context.governor(), context.sessionId());
        if (!slot.held()) {
            throw ToolError{ToolErrorCode::ResourceExhausted,
                            "subprocess cap exhausted"};
        }

        ProcessRequest request;
        request.executable = "/bin/bash";
        request.argv = {"/bin/bash", "--noprofile", "--norc", "-lc", command};
        request.cwd = context.root();
        // 119-D5/D47: the user's environment and real `$HOME`; only the
        // shell-startup variables are unset. Wave 1 resolves the deployment
        // mode; Wave 2 makes it per session.
        request.env_mode    = ProcessEnvMode::Inherit;
        request.environment = {{"PWD", context.root().string()}};
        request.env_unset   = {"BASH_ENV", "ENV", "CDPATH"};
        // 119-D31: prefer the policy resolved for this call (per-session mode,
        // escalation); the deployment default is the fallback.
        const SandboxPolicy deployment =
            context.sandboxPolicy().has_value()
                ? *context.sandboxPolicy()
                : context.execution().sandboxPolicy().resolveDeployment();
        std::optional<SandboxFacts>  sandbox_facts;
        std::vector<std::string>     denial_signatures;
        std::vector<RunnerFailureRule> runner_failure_rules;
        if (deployment.mode != SandboxMode::DangerFullAccess) {
            SandboxPolicy policy  = deployment;
            policy.workspace_root = context.root();
            const ConfinedSpawn plan =
                context.execution().sandbox().confine(request, policy);
            plan.apply(request);
            sandbox_facts.emplace();
            sandbox_facts->mode        = policy.mode;
            sandbox_facts->enforcement = plan.enforcement;
            denial_signatures          = plan.denial_signatures;
            runner_failure_rules       = plan.runner_failure_rules;
        }
        request.sink = &context.output();
        if (arguments.value.contains("timeout_ms")) {
            request.timeout = std::chrono::milliseconds{
                arguments.value["timeout_ms"].get<long long>()};
        }
        request.deadline = clamp_timeout(
            context.has_deadline()
                ? std::optional<std::chrono::steady_clock::time_point>{context.deadline()}
                : std::nullopt,
            request.timeout, std::chrono::steady_clock::now());

        const ProcessResult process =
            context.execution().process().run(request, context.cancellation()).get();

        ToolResult result;
        result.id = context.callId();
        result.name = "shell";
        if (context.cancellation().cancelled()) {
            result.outcome = payload::ToolOutcome::Cancelled;
            result.output = context.output().materialize(config_.tool_result_max_bytes);
            return Task<ToolResult>(std::move(result));
        }
        if (process.timed_out) {
            throw ToolError{ToolErrorCode::Timeout, "command timed out"};
        }

        std::string output =
            context.output().materialize(config_.tool_result_max_bytes);
        if (!output.empty()) {
            output.push_back('\n');
        }
        output += "exit_code: " + std::to_string(process.exit_code);
        result.output = std::move(output);
        if (sandbox_facts.has_value()) {
            // 119-D35: the status record wins; then the denial dialect on the
            // bounded stderr tail.
            if (process.confinement_abort) {
                sandbox_facts->runner_failure = true;
            } else {
            for (const std::string& signature : denial_signatures) {
                    if (!signature.empty() &&
                        process.stderr_tail.find(signature) != std::string::npos) {
                        sandbox_facts->denied = true;
                        break;
                    }
                }
            }
            result.sandbox = sandbox_facts;
        }
        if (process.exit_code != 0) {
            result.outcome = payload::ToolOutcome::Error;
            result.error = "exit_code: " + std::to_string(process.exit_code);
        }
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

} // namespace

std::unique_ptr<Tool> make_read_file_tool(ToolConfig config) {
    return std::make_unique<ReadFileTool>(config);
}

std::unique_ptr<Tool> make_read_image_tool(ToolConfig config) {
    return std::make_unique<ReadImageTool>(std::move(config));
}

std::unique_ptr<Tool> make_write_file_tool(ToolConfig config) {
    return std::make_unique<WriteFileTool>(config);
}

std::unique_ptr<Tool> make_edit_file_tool(ToolConfig config) {
    return std::make_unique<EditFileTool>(config);
}

std::unique_ptr<Tool> make_multi_edit_tool(ToolConfig config) {
    return std::make_unique<MultiEditTool>(config);
}

std::unique_ptr<Tool> make_apply_patch_tool(ToolConfig config) {
    return std::make_unique<ApplyPatchTool>(config);
}

std::unique_ptr<Tool> make_grep_tool(ToolConfig config) {
    return std::make_unique<GrepTool>(config);
}

std::unique_ptr<Tool> make_glob_tool(ToolConfig config) {
    return std::make_unique<GlobTool>(config);
}

std::unique_ptr<Tool> make_shell_tool(ToolConfig config) {
    return std::make_unique<ShellTool>(config);
}

std::vector<std::unique_ptr<Tool>> make_builtin_tools(ToolConfig config) {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(make_apply_patch_tool(config));
    tools.push_back(make_edit_file_tool(config));
    tools.push_back(make_git_diff_tool(config));
    tools.push_back(make_git_status_tool(config));
    tools.push_back(make_glob_tool(config));
    tools.push_back(make_grep_tool(config));
    tools.push_back(make_multi_edit_tool(config));
    tools.push_back(make_read_file_tool(config));
    tools.push_back(make_read_image_tool(config));
    tools.push_back(make_shell_tool(config));
    tools.push_back(make_write_file_tool(config));
    return tools;
}

} // namespace ymh

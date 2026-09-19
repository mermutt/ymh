#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// AL-U22 / 24-D19: repo-wide lexical guard for §7.2.1 point 7 / AL35. It fails
// on (a) any occurrence of a deleted raw-handle accessor name, (b) a raw
// extraction (.get() or unary *) applied directly to the result of a
// handle-producing call, (c) any function whose declared return type is a raw
// Agent/AgentLoop/Session handle (the "newly-named raw accessor" and "wrapper
// returning Agent&" cases), and (d) a raw handle extracted from a named owning
// local and then returned or stored into a member.
//
// The forbidden tokens are assembled at runtime so this scanner does not flag
// its own source; the file is also skipped by path.

namespace {

constexpr std::string_view kSourceRoot = YMH_SOURCE_DIR;

bool is_identifier_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

bool is_space(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::size_t matching_paren(std::string_view text, std::size_t open) {
    int depth = 0;
    for (std::size_t index = open; index < text.size(); ++index) {
        if (text[index] == '(') {
            ++depth;
        } else if (text[index] == ')') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string_view::npos;
}

std::vector<std::string> accessor_names() {
    return {std::string("find") + "Shared", std::string("get") + "Shared",
            std::string("session") + "Ptr"};
}

std::vector<std::string> deleted_names() {
    return {std::string("find") + "Agent", std::string("setAgent") + "Lookup",
            std::string("Agent") + "Lookup"};
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream       stream(text);
    std::string              line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool match_word(const std::string& text, std::size_t pos, std::string_view word) {
    if (text.compare(pos, word.size(), word) != 0) {
        return false;
    }
    const std::size_t after = pos + word.size();
    if (after < text.size() && is_identifier_char(text[after])) {
        return false;
    }
    if (pos > 0 && is_identifier_char(text[pos - 1])) {
        return false;
    }
    return true;
}

// A declaration whose type is a raw agent/session handle, e.g.
// `Agent& foo(` or `Session* bar =`. Returns the declared name.
struct RawDeclaration {
    std::string name;
    std::size_t after = 0;
};

std::optional<RawDeclaration> parse_raw_declaration(const std::string& line) {
    std::size_t pos = 0;
    while (pos < line.size() && is_space(line[pos])) {
        ++pos;
    }
    for (;;) {
        bool qualified = false;
        for (std::string_view qualifier : {"static", "inline", "virtual", "explicit", "const"}) {
            if (match_word(line, pos, qualifier)) {
                pos += qualifier.size();
                while (pos < line.size() && is_space(line[pos])) {
                    ++pos;
                }
                qualified = true;
                break;
            }
        }
        if (!qualified) {
            break;
        }
    }
    std::string_view type;
    if (match_word(line, pos, "AgentLoop")) {
        type = "AgentLoop";
    } else if (match_word(line, pos, "Agent")) {
        type = "Agent";
    } else if (match_word(line, pos, "Session")) {
        type = "Session";
    } else {
        return std::nullopt;
    }
    pos += type.size();
    while (pos < line.size() && is_space(line[pos])) {
        ++pos;
    }
    if (pos >= line.size() || (line[pos] != '&' && line[pos] != '*')) {
        return std::nullopt;
    }
    while (pos < line.size() && (line[pos] == '&' || line[pos] == '*' || is_space(line[pos]))) {
        ++pos;
    }
    const std::size_t start = pos;
    while (pos < line.size() && is_identifier_char(line[pos])) {
        ++pos;
    }
    if (pos == start) {
        return std::nullopt;
    }
    return RawDeclaration{line.substr(start, pos - start), pos};
}

void scan_deleted_names(const std::string& text, const std::filesystem::path& path,
                        const std::vector<std::string>& names, std::vector<std::string>& violations) {
    for (const std::string& name : names) {
        if (text.find(name) != std::string::npos) {
            violations.push_back(path.string() + ": deleted accessor '" + name + "'");
        }
    }
}

void scan_extractions(const std::string& text, const std::filesystem::path& path,
                      const std::vector<std::string>& accessors,
                      std::vector<std::string>&      violations) {
    for (const std::string& name : accessors) {
        std::size_t position = 0;
        while ((position = text.find(name, position)) != std::string::npos) {
            const std::size_t after = position + name.size();
            if (position > 0 && is_identifier_char(text[position - 1])) {
                position = after;
                continue;
            }
            std::size_t open = after;
            while (open < text.size() && is_space(text[open])) {
                ++open;
            }
            if (open >= text.size() || text[open] != '(') {
                position = after;
                continue;
            }
            const std::size_t close = matching_paren(text, open);
            if (close == std::string_view::npos) {
                position = after;
                continue;
            }

            std::size_t dot = close + 1;
            while (dot < text.size() && is_space(text[dot])) {
                ++dot;
            }
            if (text.compare(dot, 4, ".get") == 0) {
                std::size_t paren = dot + 4;
                while (paren < text.size() && is_space(text[paren])) {
                    ++paren;
                }
                if (paren < text.size() && text[paren] == '(') {
                    violations.push_back(path.string() + ": '.get()' on " + name + "() result");
                }
            }

            std::size_t chain = position;
            while (chain > 0) {
                const char previous = text[chain - 1];
                if (is_space(previous) || is_identifier_char(previous) || previous == '.' ||
                    previous == '>' || previous == '(' || previous == ')' || previous == '[' ||
                    previous == ']' || previous == ':') {
                    --chain;
                    continue;
                }
                break;
            }
            std::size_t star = chain;
            while (star > 0 && is_space(text[star - 1])) {
                --star;
            }
            if (star > 0 && text[star - 1] == '*') {
                const char before = star >= 2 ? text[star - 2] : '\0';
                if (!(is_identifier_char(before) || before == ')' || before == ']' ||
                      before == '.')) {
                    violations.push_back(path.string() + ": unary '*' on " + name + "() result");
                }
            }
            position = after;
        }
    }
}

// (c) A function declaration/definition returning a raw agent/session handle.
// The allowlist is the pinned, reviewed set of legitimate declarations: the
// private `SessionManager::loadInto`/`session` (24-D17), the borrowed
// `ToolContext::session` (anchored by the executing body's strong reference),
// and the test-only `as_loop` conversion helper.
struct RawReturnAllow {
    std::string_view file;
    std::string_view function;
};

constexpr RawReturnAllow kRawReturnAllow[] = {
    {"include/ymh/tools/tool_context.hpp", "session"},
    {"include/ymh/session/session_manager.hpp", "loadInto"},
    {"include/ymh/session/session_manager.hpp", "session"},
    {"tests/unit/compaction_test.cpp", "as_loop"},
};

bool raw_return_allowed(const std::filesystem::path& path, const std::string& function) {
    for (const RawReturnAllow& entry : kRawReturnAllow) {
        if (path.generic_string().find(std::string(entry.file)) != std::string::npos &&
            function == entry.function) {
            return true;
        }
    }
    return false;
}

void scan_raw_return_types(const std::string& text, const std::filesystem::path& path,
                           std::vector<std::string>& violations) {
    std::size_t line_number = 0;
    for (const std::string& line : split_lines(text)) {
        ++line_number;
        const std::optional<RawDeclaration> declaration = parse_raw_declaration(line);
        if (!declaration.has_value()) {
            continue;
        }
        std::size_t next = declaration->after;
        while (next < line.size() && is_space(line[next])) {
            ++next;
        }
        if (next < line.size() && line[next] == '(' &&
            !raw_return_allowed(path, declaration->name)) {
            violations.push_back(path.string() + ":" + std::to_string(line_number) +
                                 ": function returns a raw handle: " + declaration->name);
        }
    }
}

// (b2) `.get()` on a line that also names a handle-producing accessor catches
// the cast/indirection form the direct-call scan cannot see, e.g.
// `static_pointer_cast<T>(findShared(id)).get()`.
void scan_accessor_get_lines(const std::string& text, const std::filesystem::path& path,
                             const std::vector<std::string>& accessors,
                             std::vector<std::string>&      violations) {
    std::size_t line_number = 0;
    for (const std::string& line : split_lines(text)) {
        ++line_number;
        if (line.find(".get(") == std::string::npos) {
            continue;
        }
        for (const std::string& name : accessors) {
            if (line.find(name) != std::string::npos) {
                violations.push_back(path.string() + ":" + std::to_string(line_number) +
                                     ": '.get()' on a line naming " + name);
            }
        }
    }
}

bool contains_deref_of(const std::string& line, const std::string& base) {
    if (line.find("*" + base) != std::string::npos) {
        return true;
    }
    if (line.find(base + ".get(") != std::string::npos) {
        return true;
    }
    return line.find("static_pointer_cast") != std::string::npos &&
           line.find(base) != std::string::npos && line.find(".get(") != std::string::npos;
}

// A line's assignment target immediately before `=`; empty when the `=` is a
// comparison or there is no identifier.
std::string assignment_target(const std::string& line, std::size_t equals) {
    if (equals == 0 || (equals > 0 && (line[equals - 1] == '=' || line[equals - 1] == '!' ||
                                       line[equals - 1] == '<' || line[equals - 1] == '>'))) {
        return {};
    }
    std::size_t end = equals;
    while (end > 0 && is_space(line[end - 1])) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && is_identifier_char(line[start - 1])) {
        --start;
    }
    return line.substr(start, end - start);
}

std::vector<std::string> collect_owning_names(const std::vector<std::string>& lines,
                                              const std::vector<std::string>& accessors) {
    std::vector<std::string> owned;
    for (const std::string& line : lines) {
        for (const std::string& accessor : accessors) {
            std::size_t position = 0;
            while ((position = line.find(accessor, position)) != std::string::npos) {
                const std::size_t after = position + accessor.size();
                std::size_t       open  = after;
                while (open < line.size() && is_space(line[open])) {
                    ++open;
                }
                if (open >= line.size() || line[open] != '(') {
                    position = after;
                    continue;
                }
                const std::size_t equals = line.rfind('=', position);
                if (equals != std::string::npos) {
                    const std::string name = assignment_target(line, equals);
                    if (!name.empty()) {
                        owned.push_back(name);
                    }
                }
                position = after;
            }
        }
    }
    return owned;
}

bool escapes_via_return(const std::string& line, const std::string& name) {
    if (line.find("return") == std::string::npos) {
        return false;
    }
    return line.find("*" + name) != std::string::npos ||
           line.find(name + ".get(") != std::string::npos;
}

bool escapes_via_member(const std::string& line, const std::string& name) {
    std::size_t position = 0;
    while ((position = line.find(name, position)) != std::string::npos) {
        const std::size_t after = position + name.size();
        if ((position > 0 && is_identifier_char(line[position - 1])) ||
            (after < line.size() && is_identifier_char(line[after]))) {
            position = after;
            continue;
        }
        std::size_t equals = position;
        while (equals > 0 && is_space(line[equals - 1])) {
            --equals;
        }
        if (equals > 0 && line[equals - 1] == '&') {
            --equals;
            while (equals > 0 && is_space(line[equals - 1])) {
                --equals;
            }
        }
        if (equals > 0 && line[equals - 1] == '=') {
            const std::string target = assignment_target(line, equals - 1);
            if (!target.empty() && target.back() == '_') {
                return true;
            }
            if (equals >= 2 && line.compare(equals - 2, 2, "->") == 0) {
                return true;
            }
        }
        position = after;
    }
    return false;
}

// (d) Two-step escape: an owning local assigned from an accessor, then a raw
// reference/pointer extracted from it and returned or stored into a member.
void scan_owning_local_escapes(const std::string& text, const std::filesystem::path& path,
                               const std::vector<std::string>& accessors,
                               std::vector<std::string>&      violations) {
    const std::vector<std::string> lines = split_lines(text);
    const std::vector<std::string> owned = collect_owning_names(lines, accessors);

    std::vector<std::string> raw;
    for (const std::string& line : lines) {
        const std::optional<RawDeclaration> declaration = parse_raw_declaration(line);
        if (!declaration.has_value()) {
            continue;
        }
        for (const std::string& base : owned) {
            if (contains_deref_of(line, base)) {
                raw.push_back(declaration->name);
                break;
            }
        }
    }

    for (std::size_t index = 0; index < lines.size(); ++index) {
        for (const std::string& name : raw) {
            if (escapes_via_return(lines[index], name) ||
                escapes_via_member(lines[index], name)) {
                violations.push_back(path.string() + ":" + std::to_string(index + 1) +
                                     ": raw handle '" + name + "' escapes its owning local");
            }
        }
    }
}

bool is_source_file(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".cpp" || extension == ".hpp" || extension == ".h" ||
           extension == ".cc" || extension == ".cxx" || extension == ".hxx" ||
           extension == ".ipp";
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

TEST(AgentLifetimeGuard, NoRawMapDerivedHandleEscapes) {
    const std::vector<std::string> accessors = accessor_names();
    const std::vector<std::string> deleted   = deleted_names();

    std::vector<std::string> violations;
    std::size_t              scanned = 0;
    for (const char* directory : {"src", "tests", "include"}) {
        const std::filesystem::path root = std::filesystem::path(kSourceRoot) / directory;
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || !is_source_file(entry.path())) {
                continue;
            }
            if (entry.path().filename() == "no_raw_map_handle_test.cpp") {
                continue;
            }
            ++scanned;
            const std::string text = read_file(entry.path());
            scan_deleted_names(text, entry.path(), deleted, violations);
            scan_extractions(text, entry.path(), accessors, violations);
            scan_raw_return_types(text, entry.path(), violations);
            scan_accessor_get_lines(text, entry.path(), accessors, violations);
            scan_owning_local_escapes(text, entry.path(), accessors, violations);
        }
    }

    EXPECT_GT(scanned, 0u);
    for (const std::string& violation : violations) {
        ADD_FAILURE() << violation;
    }
}

} // namespace

// Mechanical spec-symbol catalog test (closes the recurring design-gate defect
// class). The design-first gate in AGENTS.md is a prose gate: nothing forced a
// spec entry to exist when a spec-bearing code symbol shipped, so it silently
// failed three times (`event.live` unpinned, `AssistantStreamPrinter` unpinned,
// the `21` errata owning `session.persist_prompt_text` never written).
//
// This test makes the gate mechanical. `tests/fixtures/spec_symbol_catalog.json`
// is the single source of truth mapping each spec-bearing code symbol to the
// `docs/design/` file + numeric section that owns it. The test:
//
//   (1) extracts the *actual* symbol set from the enumerated sources,
//   (2) requires every actual symbol to have a catalog entry (a new symbol with
//       no entry fails the build, naming it),
//   (3) requires every catalog entry to resolve to a real code symbol (a stale
//       or renamed entry fails, naming it),
//   (4) requires every non-gap entry's spec file to exist and its section
//       anchor to be a real heading in that file (an invented section fails),
//   (5) requires every gap entry to carry a non-empty reason, and
//   (6) pins the three known historical defects explicitly so a future refactor
//       cannot quietly drop their ownership.
//
// Sources enumerated (the "spec-bearing" surface this repo cares about):
//   - include/ymh/core/event.hpp            -> EventType enumerators
//   - include/ymh/transport/protocol.hpp    -> notify/method wire names + DTOs/enums
//   - include/ymh/session/events.hpp        -> ymh::payload durable structs/enums
//   - include/ymh/cli/assistant_stream_printer.hpp -> the printer contract
//   - src/config/config.cpp                 -> config keys (reject_unknown allowlists)

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

constexpr std::string_view kSourceRoot = YMH_SOURCE_DIR;

std::filesystem::path source_path(std::string_view relative) {
    return std::filesystem::path{kSourceRoot} / std::string{relative};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
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

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string strip_comment(const std::string& line) {
    const std::size_t comment = line.find("//");
    return comment == std::string::npos ? line : line.substr(0, comment);
}

// Parses an identifier beginning at `pos` (skipping leading whitespace).
std::string parse_ident(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    if (pos >= text.size() || !is_ident_start(text[pos])) {
        return {};
    }
    const std::size_t start = pos;
    while (pos < text.size() && is_ident_char(text[pos])) {
        ++pos;
    }
    return text.substr(start, pos - start);
}

std::size_t matching_paren(const std::string& text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// Code extraction
// ---------------------------------------------------------------------------

std::set<std::string> extract_event_types() {
    std::set<std::string> out;
    bool                  inside = false;
    for (const std::string& raw : split_lines(read_file(source_path("include/ymh/core/event.hpp")))) {
        if (!inside) {
            if (raw.find("enum class EventType") != std::string::npos) {
                inside = true;
            }
            continue;
        }
        if (raw.find("};") != std::string::npos) {
            break;
        }
        const std::string line = strip_comment(raw);
        std::size_t       pos  = 0;
        const std::string name = parse_ident(line, pos);
        if (name.empty()) {
            continue;
        }
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
            ++pos;
        }
        if (pos < line.size() && line[pos] == ',') {
            out.insert("EventType::" + name);
        }
    }
    return out;
}

// Extracts `kFoo` constants from a `namespace <ns> { ... }` block.
std::set<std::string> extract_namespace_consts(const std::string& file, const std::string& ns) {
    std::set<std::string> out;
    const std::string     text = read_file(source_path(file));
    const std::string     open_marker  = "namespace " + ns + " {";
    const std::string     close_marker = "} // namespace " + ns;
    const std::size_t     begin        = text.find(open_marker);
    if (begin == std::string::npos) {
        return out;
    }
    const std::size_t end = text.find(close_marker, begin);
    const std::string body = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                         : end - begin);
    for (const std::string& line : split_lines(body)) {
        const std::string stripped = strip_comment(line);
        const std::size_t eq       = stripped.find('=');
        if (eq == std::string::npos || stripped.find('"') == std::string::npos) {
            continue;
        }
        // Find the identifier immediately before '='.
        std::size_t cursor = eq;
        while (cursor > 0 && std::isspace(static_cast<unsigned char>(stripped[cursor - 1])) != 0) {
            --cursor;
        }
        std::size_t ident_end   = cursor;
        std::size_t ident_begin = cursor;
        while (ident_begin > 0 && is_ident_char(stripped[ident_begin - 1])) {
            --ident_begin;
        }
        const std::string ident = stripped.substr(ident_begin, ident_end - ident_begin);
        if (ident.size() >= 2 && ident[0] == 'k' && std::isupper(static_cast<unsigned char>(ident[1])) != 0) {
            out.insert("protocol::" + ns + "::" + ident);
        }
    }
    return out;
}

// Extracts top-level `struct X` / `enum class X` names from a header.
std::set<std::string> extract_top_level_types(const std::string& file, const std::string& prefix) {
    std::set<std::string> out;
    for (const std::string& line : split_lines(read_file(source_path(file)))) {
        std::size_t start = std::string::npos;
        if (line.rfind("struct ", 0) == 0) {
            start = 7;
        } else if (line.rfind("enum class ", 0) == 0) {
            start = 11;
        }
        if (start == std::string::npos) {
            continue;
        }
        std::size_t       ident_pos = start;
        const std::string name      = parse_ident(line, ident_pos);
        if (name.empty()) {
            continue;
        }
        while (ident_pos < line.size() &&
               std::isspace(static_cast<unsigned char>(line[ident_pos])) != 0) {
            ++ident_pos;
        }
        const char next = ident_pos < line.size() ? line[ident_pos] : '\0';
        if (next == '{' || next == ':') {
            out.insert(prefix + "::" + name);
        }
    }
    return out;
}

// Extracts `struct X` / `enum class X` names inside `namespace payload { ... }`.
std::set<std::string> extract_payload_types() {
    const std::string text = read_file(source_path("include/ymh/session/events.hpp"));
    const std::size_t begin = text.find("namespace payload {");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find("} // namespace payload", begin);
    const std::string body = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                         : end - begin);
    std::set<std::string> out;
    for (const std::string& line : split_lines(body)) {
        std::size_t start = std::string::npos;
        if (line.rfind("struct ", 0) == 0) {
            start = 7;
        } else if (line.rfind("enum class ", 0) == 0) {
            start = 11;
        }
        if (start == std::string::npos) {
            continue;
        }
        std::size_t       ident_pos = start;
        const std::string name      = parse_ident(line, ident_pos);
        if (name.empty()) {
            continue;
        }
        while (ident_pos < line.size() &&
               std::isspace(static_cast<unsigned char>(line[ident_pos])) != 0) {
            ++ident_pos;
        }
        const char next = ident_pos < line.size() ? line[ident_pos] : '\0';
        if (next == '{' || next == ':') {
            out.insert("payload::" + name);
        }
    }
    return out;
}

std::set<std::string> extract_printer() {
    std::set<std::string> out;
    for (const std::string& line :
         split_lines(read_file(source_path("include/ymh/cli/assistant_stream_printer.hpp")))) {
        const std::size_t pos = line.find("class AssistantStreamPrinter");
        if (pos != std::string::npos) {
            out.insert("cli::AssistantStreamPrinter");
        }
    }
    return out;
}

// Extracts the string literals from a quoted region.
std::vector<std::string> quoted_strings(const std::string& text) {
    std::vector<std::string> out;
    std::size_t              i = 0;
    while (i < text.size()) {
        if (text[i] != '"') {
            ++i;
            continue;
        }
        ++i;
        std::string value;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) {
                value.push_back(text[i + 1]);
                i += 2;
            } else {
                value.push_back(text[i]);
                ++i;
            }
        }
        ++i;  // closing quote
        out.push_back(std::move(value));
    }
    return out;
}

// Extracts every config key from the `reject_unknown(table, "<name>", {...})`
// allowlists in src/config/config.cpp. The loader rejects any key not listed,
// so the union of these arrays is the complete shipped key set.
std::set<std::string> extract_config_keys() {
    const std::string text = read_file(source_path("src/config/config.cpp"));
    std::set<std::string> out;
    const std::string     needle = "reject_unknown(";
    std::size_t           from   = 0;
    while (true) {
        const std::size_t call = text.find(needle, from);
        if (call == std::string::npos) {
            break;
        }
        from = call + needle.size();
        const std::size_t open  = call + needle.size() - 1;
        const std::size_t close = matching_paren(text, open);
        if (close == std::string::npos) {
            continue;
        }
        const std::string call_text = text.substr(open, close - open + 1);
        const std::size_t brace     = call_text.find('{');
        if (brace == std::string::npos) {
            continue;  // function definition (parameter list has no initializer list)
        }
        const std::size_t brace_close = call_text.find('}', brace);
        if (brace_close == std::string::npos) {
            continue;
        }
        const std::string head = call_text.substr(0, brace);
        const std::vector<std::string> head_strings = quoted_strings(head);
        std::string                    table;
        if (!head_strings.empty()) {
            table = head_strings.front();
        } else if (head.find("entry, table") != std::string::npos) {
            table = "mcp_servers";
        } else if (head.find("*it, label") != std::string::npos) {
            table = "permissions.presets";
        } else {
            continue;
        }
        if (table.empty()) {
            continue;  // top-level section allowlist, not a leaf key
        }
        for (const std::string& key : quoted_strings(call_text.substr(brace, brace_close - brace))) {
            out.insert("config::" + table + "." + key);
        }
    }
    return out;
}

std::set<std::string> actual_symbols() {
    std::set<std::string> out;
    const auto             merge = [&out](const std::set<std::string>& extra) {
        out.insert(extra.begin(), extra.end());
    };
    merge(extract_event_types());
    merge(extract_namespace_consts("include/ymh/transport/protocol.hpp", "notify"));
    merge(extract_namespace_consts("include/ymh/transport/protocol.hpp", "method"));
    merge(extract_top_level_types("include/ymh/transport/protocol.hpp", "protocol"));
    merge(extract_payload_types());
    merge(extract_printer());
    merge(extract_config_keys());
    return out;
}

// ---------------------------------------------------------------------------
// Spec extraction
// ---------------------------------------------------------------------------

// Parses the numeric section ids from a markdown file's headings
// (`## 1. X`, `### 7.7 Y`, `#### 4.3.9.1 Z`).
std::set<std::string> section_ids(const std::filesystem::path& path) {
    std::set<std::string> out;
    for (const std::string& line : split_lines(read_file(path))) {
        std::size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#') {
            ++hashes;
        }
        if (hashes < 2 || hashes > 6) {
            continue;
        }
        std::size_t pos = hashes;
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) {
            ++pos;
        }
        if (pos >= line.size() || std::isdigit(static_cast<unsigned char>(line[pos])) == 0) {
            continue;
        }
        std::string number;
        while (pos < line.size()) {
            if (std::isdigit(static_cast<unsigned char>(line[pos])) != 0) {
                number.push_back(line[pos]);
                ++pos;
            } else if (line[pos] == '.' && pos + 1 < line.size() &&
                       std::isdigit(static_cast<unsigned char>(line[pos + 1])) != 0) {
                number.push_back('.');
                ++pos;
            } else {
                break;
            }
        }
        const bool boundary =
            pos >= line.size() || std::isspace(static_cast<unsigned char>(line[pos])) != 0 ||
            (line[pos] == '.' &&
             (pos + 1 >= line.size() ||
              std::isspace(static_cast<unsigned char>(line[pos + 1])) != 0));
        if (boundary && !number.empty()) {
            out.insert(number);
        }
    }
    return out;
}

std::filesystem::path design_dir() {
    return source_path("docs/design");
}

std::filesystem::path catalog_path() {
    return source_path("tests/fixtures/spec_symbol_catalog.json");
}

struct CatalogEntry {
    std::string symbol;
    std::string category;
    std::string spec;
    std::string section;
    bool        gap = false;
    std::string reason;
};

std::vector<CatalogEntry> load_catalog() {
    std::vector<CatalogEntry> entries;
    std::ifstream             in(catalog_path());
    if (!in) {
        return entries;
    }
    nlohmann::json root;
    in >> root;
    for (const auto& item : root.at("entries")) {
        CatalogEntry entry;
        entry.symbol   = item.at("symbol").get<std::string>();
        entry.category = item.value("category", std::string{});
        entry.gap      = item.value("gap", false);
        entry.reason   = item.value("reason", std::string{});
        entry.spec     = item.value("spec", std::string{});
        entry.section  = item.value("section", std::string{});
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) {
            out += "\n    ";
        }
        out += item;
    }
    return out;
}

}  // namespace

// (2) Forward: every spec-bearing code symbol must have a catalog entry.
TEST(SpecCatalog, EveryCodeSymbolHasACatalogEntry) {
    const std::set<std::string> actual = actual_symbols();
    ASSERT_FALSE(actual.empty()) << "code extraction produced no symbols (paths wrong?)";

    std::map<std::string, CatalogEntry> by_symbol;
    for (const CatalogEntry& entry : load_catalog()) {
        by_symbol.emplace(entry.symbol, entry);
    }

    std::vector<std::string> missing;
    for (const std::string& symbol : actual) {
        if (by_symbol.find(symbol) == by_symbol.end()) {
            missing.push_back(symbol);
        }
    }
    EXPECT_TRUE(missing.empty())
        << "spec-bearing code symbols with NO spec_symbol_catalog.json entry:\n    "
        << join(missing) << "\nAdd an entry naming the owning spec file + section (or a "
        << "gap:true marker with a reason) in tests/fixtures/spec_symbol_catalog.json.";
}

// (3) Reverse: every catalog entry must resolve to a real code symbol.
TEST(SpecCatalog, EveryCatalogEntryResolvesToARealCodeSymbol) {
    const std::set<std::string>         actual = actual_symbols();
    std::vector<std::string>            stale;
    std::set<std::string>               seen;
    std::vector<std::string>            duplicates;
    for (const CatalogEntry& entry : load_catalog()) {
        if (actual.find(entry.symbol) == actual.end()) {
            stale.push_back(entry.symbol);
        }
        if (!seen.insert(entry.symbol).second) {
            duplicates.push_back(entry.symbol);
        }
    }
    EXPECT_TRUE(stale.empty()) << "catalog entries that do NOT correspond to any code symbol "
                                  "(stale or renamed):\n    "
                               << join(stale);
    EXPECT_TRUE(duplicates.empty()) << "duplicate catalog entries:\n    " << join(duplicates);
}

// (4) + (5) Every non-gap entry's spec file + section anchor must really exist;
// every gap entry must carry a reason.
TEST(SpecCatalog, EverySpecSectionExistsOrIsMarkedAsAGap) {
    std::map<std::string, std::set<std::string>> sections_by_file;
    std::vector<std::string>                     bad_section;
    std::vector<std::string>                     missing_file;
    std::vector<std::string>                     gap_without_reason;

    for (const CatalogEntry& entry : load_catalog()) {
        if (entry.gap) {
            if (entry.reason.empty()) {
                gap_without_reason.push_back(entry.symbol);
            }
            continue;
        }
        if (entry.spec.empty() || entry.section.empty()) {
            bad_section.push_back(entry.symbol + " (missing spec/section field)");
            continue;
        }
        const std::filesystem::path file = design_dir() / entry.spec;
        if (!std::filesystem::exists(file)) {
            missing_file.push_back(entry.symbol + " -> " + entry.spec);
            continue;
        }
        auto it = sections_by_file.find(entry.spec);
        if (it == sections_by_file.end()) {
            it = sections_by_file.emplace(entry.spec, section_ids(file)).first;
        }
        if (it->second.find(entry.section) == it->second.end()) {
            bad_section.push_back(entry.symbol + " -> " + entry.spec + " §" + entry.section);
        }
    }

    EXPECT_TRUE(missing_file.empty()) << "catalog entries naming a spec file that does not exist:\n    "
                                      << join(missing_file);
    EXPECT_TRUE(bad_section.empty())
        << "catalog entries naming a spec section that does not exist (invented anchor):\n    "
        << join(bad_section);
    EXPECT_TRUE(gap_without_reason.empty())
        << "gap entries without a reason:\n    " << join(gap_without_reason);
}

// (6) The three historical silent failures must stay pinned to their owners.
TEST(SpecCatalog, HistoricalDefectsRemainOwned) {
    std::map<std::string, CatalogEntry> by_symbol;
    for (const CatalogEntry& entry : load_catalog()) {
        by_symbol.emplace(entry.symbol, entry);
    }

    const auto require = [&by_symbol](const std::string& symbol, const std::string& spec,
                                      const std::string& section) {
        const auto it = by_symbol.find(symbol);
        ASSERT_NE(it, by_symbol.end()) << symbol << " is not cataloged";
        EXPECT_FALSE(it->second.gap) << symbol << " is marked as a gap";
        EXPECT_EQ(it->second.spec, spec) << symbol << " owner file changed";
        EXPECT_EQ(it->second.section, section) << symbol << " owner section changed";
    };

    require("protocol::notify::kEventLive", "35-live-notification-errata.md", "3.2");
    require("protocol::LiveNotification", "35-live-notification-errata.md", "3.3");
    require("cli::AssistantStreamPrinter", "34-assembler-replay-errata.md", "15");

    // The third historical defect is now owned by the `39` errata (the gap this
    // catalog test itself found and forced closed). It is asserted as ownership,
    // not as a gap marker, so a future refactor cannot quietly drop the owner.
    require("config::session.persist_prompt_text",
            "39-session-persist-prompt-text-errata.md", "3.1");
}

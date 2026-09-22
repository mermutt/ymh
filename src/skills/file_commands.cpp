#include "ymh/skills/file_commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ymh/execution/environment.hpp"
#include "ymh/skills/skill_types.hpp"

namespace ymh {
namespace {

constexpr std::size_t kMaxCommandBytes = 64u * 1024u;
constexpr std::size_t kMaxFrontmatterBytes = 4u * 1024u;
constexpr std::size_t kMaxDescriptionColumns = 80;

std::filesystem::path weakly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : result;
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

std::string truncate_columns(std::string text, std::size_t columns) {
    if (text.size() <= columns) {
        return text;
    }
    text.resize(columns);
    return text;
}

struct Frontmatter {
    std::map<std::string, std::string> scalars;
    std::map<std::string, std::vector<std::string>> lists;
    std::string body;
    bool        present = false;
};

std::vector<std::string> parse_flow_list(std::string_view raw) {
    std::string inner = trim(raw);
    if (inner.size() < 2 || inner.front() != '[' || inner.back() != ']') {
        return {};
    }
    inner = inner.substr(1, inner.size() - 2);
    std::vector<std::string> items;
    std::size_t              start = 0;
    while (start <= inner.size()) {
        const std::size_t comma = inner.find(',', start);
        const std::string item =
            trim(comma == std::string::npos ? std::string_view{inner}.substr(start)
                                            : std::string_view{inner}.substr(start, comma - start));
        if (!item.empty()) {
            items.push_back(item);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
}

std::string unquote(std::string value) {
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// The 20 §4.2 flat-YAML subset, but lenient: a malformed fence degrades to
// "no frontmatter" (50-F3). Only the three command keys are read; unknown keys
// are ignored because a command's metadata is cosmetic, not safety-adjacent.
Frontmatter parse_command_frontmatter(std::string_view content) {
    Frontmatter result;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.remove_prefix(3);
    }
    if (!content.starts_with("---")) {
        result.body = std::string{content};
        return result;
    }
    const std::size_t first_newline = content.find('\n');
    if (first_newline == std::string_view::npos || trim(content.substr(0, first_newline)) != "---") {
        result.body = std::string{content};
        return result;
    }
    const std::size_t close = content.find("\n---", first_newline);
    if (close == std::string_view::npos) {
        result.body = std::string{content};
        return result;
    }
    const std::string_view region = content.substr(first_newline + 1, close - (first_newline + 1));
    if (region.size() > kMaxFrontmatterBytes) {
        result.body = std::string{content};
        return result;
    }
    std::size_t body_start = close + 4;
    if (body_start < content.size() && content[body_start] == '\r') {
        ++body_start;
    }
    if (body_start < content.size() && content[body_start] == '\n') {
        ++body_start;
    }
    result.body = std::string{content.substr(body_start)};
    result.present = true;

    std::size_t line_start = 0;
    while (line_start < region.size()) {
        const std::size_t newline = region.find('\n', line_start);
        const std::string_view line =
            region.substr(line_start, newline == std::string_view::npos ? std::string_view::npos
                                                                        : newline - line_start);
        line_start = newline == std::string_view::npos ? region.size() : newline + 1;
        const std::string trimmed_line = trim(line);
        if (trimmed_line.empty() || trimmed_line.front() == '#') {
            continue;
        }
        const std::size_t colon = trimmed_line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = trim(trimmed_line.substr(0, colon));
        const std::string value = trim(trimmed_line.substr(colon + 1));
        if (key == "allowed-tools" && !value.empty() && value.front() == '[') {
            result.lists[key] = parse_flow_list(value);
        } else if (!key.empty()) {
            result.scalars[key] = unquote(value);
        }
    }
    return result;
}

std::string first_body_line(const std::string& body) {
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t newline = body.find('\n', start);
        const std::string line =
            trim(newline == std::string::npos ? std::string_view{body}.substr(start)
                                              : std::string_view{body}.substr(start, newline - start));
        if (!line.empty()) {
            return line;
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return {};
}

} // namespace

FileCommandCatalog discover_file_commands(const std::vector<SkillRoot>& roots,
                                          const std::vector<std::string>& reserved_names,
                                          bool                            workspace_trusted) {
    FileCommandCatalog catalog;
    const std::set<std::string> reserved(reserved_names.begin(), reserved_names.end());
    std::map<std::string, std::size_t> by_name;

    for (const SkillRoot& root : roots) {
        const bool workspace_tier = root.trust == SkillTrust::Untrusted;
        if (workspace_tier && !workspace_trusted) {
            catalog.warnings.push_back(FileCommandLoadWarning{
                root.path, "workspace is not trusted: command tier disabled"});
            continue;
        }
        if (!root.path.is_absolute()) {
            catalog.warnings.push_back(
                FileCommandLoadWarning{root.path, "relative root: tier disabled"});
            continue;
        }
        const std::filesystem::path base = weakly(root.path);
        std::error_code             ec;
        if (!std::filesystem::exists(base, ec) || ec) {
            continue;
        }
        if (!std::filesystem::is_directory(base, ec) || ec) {
            catalog.warnings.push_back(
                FileCommandLoadWarning{root.path, "commands root is not a directory"});
            continue;
        }

        std::vector<std::filesystem::directory_entry> entries;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(base, ec)) {
            if (ec) {
                break;
            }
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const std::filesystem::directory_entry& left,
                     const std::filesystem::directory_entry& right) {
                      return left.path().filename().string() < right.path().filename().string();
                  });

        for (const std::filesystem::directory_entry& entry : entries) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) {
                continue;
            }
            const std::filesystem::path path = entry.path();
            if (path.extension() != ".md") {
                continue;
            }
            const std::string name = path.stem().string();
            if (!is_valid_skill_name(name)) {
                catalog.warnings.push_back(FileCommandLoadWarning{
                    path, "invalid command name '" + name + "'"});
                continue;
            }
            const std::filesystem::path canonical = weakly(path);
            if (!path_is_within(canonical, base)) {
                catalog.warnings.push_back(
                    FileCommandLoadWarning{path, "command file escapes its root"});
                continue;
            }
            if (reserved.count(name) != 0) {
                catalog.warnings.push_back(FileCommandLoadWarning{
                    path, "collides with the reserved command '/" + name + "'"});
                continue;
            }
            const std::uintmax_t size = std::filesystem::file_size(canonical, entry_ec);
            if (entry_ec || size > kMaxCommandBytes) {
                catalog.warnings.push_back(
                    FileCommandLoadWarning{path, "command file exceeds the size bound"});
                continue;
            }
            std::ifstream input(canonical, std::ios::binary);
            if (!input) {
                catalog.warnings.push_back(
                    FileCommandLoadWarning{path, "cannot read the command file"});
                continue;
            }
            const std::string content{std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>()};
            Frontmatter front = parse_command_frontmatter(content);

            const auto existing = by_name.find(name);
            if (existing != by_name.end()) {
                catalog.warnings.push_back(FileCommandLoadWarning{
                    canonical, "shadowed by " + catalog.commands[existing->second].file.string()});
                continue;
            }

            FileCommand command;
            command.name = name;
            command.trusted = root.trust == SkillTrust::Trusted;
            command.file = canonical;
            command.body = std::move(front.body);
            const auto description = front.scalars.find("description");
            if (description != front.scalars.end() && !description->second.empty()) {
                command.description = truncate_columns(description->second, kMaxDescriptionColumns);
            } else {
                command.description =
                    truncate_columns(first_body_line(command.body), kMaxDescriptionColumns);
            }
            const auto hint = front.scalars.find("argument-hint");
            if (hint != front.scalars.end()) {
                command.argument_hint = hint->second;
            }
            const auto allowed = front.lists.find("allowed-tools");
            if (allowed != front.lists.end()) {
                command.allowed_tools = allowed->second;
            }
            by_name.emplace(name, catalog.commands.size());
            catalog.commands.push_back(std::move(command));
        }
    }
    return catalog;
}

std::string substitute_arguments(std::string_view body, std::string_view arguments) {
    constexpr std::string_view token = "$ARGUMENTS";
    std::string                out;
    out.reserve(body.size());
    std::size_t index = 0;
    while (index < body.size()) {
        if (body.compare(index, token.size(), token) == 0) {
            out.append(arguments);
            index += token.size();
            continue;
        }
        out.push_back(body[index]);
        ++index;
    }
    return out;
}

} // namespace ymh

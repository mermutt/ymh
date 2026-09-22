#pragma once

// File-based slash-command discovery (50-skills-mcp-and-session-lifecycle-
// errata.md §3.2, 50-D1.3). A file command is a markdown file
// `<root>/<name>.md` whose body is a prompt template; it is a convenience, not
// a tool and not a safety artifact, so malformed frontmatter degrades instead
// of failing (50-F3).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/skills/skill_roots.hpp"

namespace ymh {

struct FileCommand {
    std::string              name;
    std::string              description;
    std::string              argument_hint;
    std::vector<std::string> allowed_tools;  // ADVISORY ONLY — never enforced
    std::string              body;
    std::filesystem::path    file;
    bool                     trusted = false;  // user tier
};

struct FileCommandLoadWarning {
    std::filesystem::path file;
    std::string           reason;
};

struct FileCommandCatalog {
    std::vector<FileCommand>            commands;
    std::vector<FileCommandLoadWarning> warnings;
};

// Pure-of-IO discovery over the ordered roots. `reserved_names` are compiled-in
// commands a file command may never shadow (50-I4). Untrusted (workspace) roots
// are skipped unless `workspace_trusted` (50-D5). Deterministic order: root
// order, then byte-wise ascending name.
[[nodiscard]] FileCommandCatalog
discover_file_commands(const std::vector<SkillRoot>& roots,
                       const std::vector<std::string>& reserved_names,
                       bool                            workspace_trusted);

// Exactly one substitution pass of `$ARGUMENTS`; absent -> empty (50-I5).
[[nodiscard]] std::string substitute_arguments(std::string_view body,
                                               std::string_view arguments);

} // namespace ymh

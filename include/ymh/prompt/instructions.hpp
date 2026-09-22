#pragma once

// Workspace-instruction loader (36 §2.4, 26 §2.3.5). `dsh-agent-instructions`
// does not register a system-prompt section; it appends a durable user-role
// message. ymh copies that shape: discovery is broad-to-specific, rendering
// keeps the most specific files first, the rendered bytes never exceed
// `max_bytes`, and the whole message is wrapped in `<system-reminder>` tags.
// Instruction files never enter `render()` (36-I6).

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ymh {

class ExecutionEnvironment;

struct InstructionFileConfig {
    std::vector<std::string> project_root_markers{".git"};
    std::vector<std::string> candidates{"AGENTS.md", "CLAUDE.md"};
    std::vector<std::string> local_candidates{"AGENTS.local.md", "CLAUDE.local.md"};
    // 50-D1.5: ordered global candidates; the loader reads at most one file per
    // parent directory (the first that exists). Empty => the default list.
    std::vector<std::filesystem::path> global_candidates;
    bool                     load_local = false;
    std::size_t              max_bytes = 0;  // REQUIRED when enabled; 0 means unset
    std::size_t              max_source_bytes = 1048576;
};

struct InstructionFile {
    std::string display_path;
    std::string scope;
    std::string content;
    bool        truncated = false;
    std::size_t original_bytes = 0;
};

struct LoadedInstructions {
    std::vector<InstructionFile> files;
    std::vector<std::string>     omitted;
    std::optional<std::string>   budget_notice;
    std::vector<std::string>     notices;

    [[nodiscard]] bool empty() const noexcept { return files.empty() && notices.empty(); }

    [[nodiscard]] std::optional<std::string> render_message() const;
};

class InstructionLoader {
public:
    InstructionLoader(InstructionFileConfig config, const ExecutionEnvironment& environment);

    [[nodiscard]] LoadedInstructions load();

    [[nodiscard]] LoadedInstructions refresh_for(const std::filesystem::path& touched);

private:
    InstructionFileConfig       config_;
    const ExecutionEnvironment& environment_;
    std::vector<InstructionFile> loaded_;
};

} // namespace ymh

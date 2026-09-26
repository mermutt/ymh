#include "ymh/prompt/instructions.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "ymh/config/config.hpp"
#include "ymh/execution/environment.hpp"

namespace ymh {
namespace {

constexpr std::string_view kFraming =
    "The following workspace instructions may be relevant to your work. Use them as guidance when "
    "applicable. More specific instructions take precedence over broader ones. They do not override "
    "system, developer, or direct user instructions.";

constexpr std::string_view kNestedFramingSuffix =
    "Use them as guidance when relevant; more specific instructions take precedence. They do not "
    "override system, developer, or direct user instructions.";

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool is_within(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    const std::filesystem::path normalized = candidate.lexically_normal();
    const std::filesystem::path base       = root.lexically_normal();
    auto                        base_it    = base.begin();
    for (auto candidate_it = normalized.begin(); candidate_it != normalized.end();
         ++candidate_it, ++base_it) {
        if (base_it == base.end()) {
            return true;
        }
        if (*candidate_it != *base_it) {
            return false;
        }
    }
    return base_it == base.end();
}

std::filesystem::path global_instruction_path(std::string_view file_name) {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg} / "ymh" / std::string{file_name};
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".config" / "ymh" / std::string{file_name};
    }
    return {};
}

std::string read_file_bounded(const std::filesystem::path& path,
                              std::size_t                  max_bytes,
                              bool&                        truncated,
                              std::size_t&                 original_bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string bytes = buffer.str();
    original_bytes    = bytes.size();
    if (bytes.size() > max_bytes) {
        bytes.resize(max_bytes);
        truncated = true;
    }
    return bytes;
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out += ", ";
        }
        out += values[index];
    }
    return out;
}

} // namespace

InstructionLoader::InstructionLoader(InstructionFileConfig       config,
                                     const ExecutionEnvironment& environment)
    : config_(std::move(config)), environment_(environment) {}

LoadedInstructions InstructionLoader::load() {
    loaded_.clear();
    if (config_.max_bytes == 0) {
        throw ConfigError(
            "prompt.instructions.max_bytes is required when instructions are enabled");
    }

    LoadedInstructions result;

    const std::string global_name =
        config_.candidates.empty() ? std::string{"AGENTS.md"} : config_.candidates.front();

    const std::filesystem::path cwd          = environment_.root();
    const std::filesystem::path project_root = [&]() {
        for (std::filesystem::path current = cwd;; current = current.parent_path()) {
            for (const std::string& marker : config_.project_root_markers) {
                if (!marker.empty() && path_exists(current / marker)) {
                    return current;
                }
            }
            if (current == current.parent_path()) {
                return cwd;
            }
        }
    }();

    std::vector<std::filesystem::path> directories;
    for (std::filesystem::path current = cwd;; current = current.parent_path()) {
        directories.push_back(current);
        if (current == project_root || current == current.parent_path()) {
            break;
        }
    }
    std::reverse(directories.begin(), directories.end());

    const auto add = [&](const std::filesystem::path& directory, const std::string& name) {
        if (!is_within(directory, cwd)) {
            return;
        }
        const std::filesystem::path path = directory / name;
        if (!regular_file(path)) {
            return;
        }
        std::filesystem::path resolved;
        try {
            resolved = environment_.resolve(path.string());
        } catch (const std::exception&) {
            return;
        }
        std::error_code relative_error;
        std::filesystem::path relative =
            std::filesystem::relative(directory, cwd, relative_error);
        if (relative_error) {
            relative.clear();
        }
        if (relative == ".") {
            relative.clear();
        }

        bool        truncated = false;
        std::size_t original  = 0;
        InstructionFile file;
        file.display_path = relative.empty() ? name : relative.generic_string() + "/" + name;
        file.scope        = relative.generic_string();
        file.content      = read_file_bounded(resolved, config_.max_source_bytes, truncated, original);
        file.truncated    = truncated;
        file.original_bytes = original;
        result.files.push_back(std::move(file));
    };

    for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory) {
        for (const std::string& name : config_.candidates) {
            add(*directory, name);
        }
        if (config_.load_local) {
            for (const std::string& name : config_.local_candidates) {
                add(*directory, name);
            }
        }
    }

    const std::filesystem::path global = global_instruction_path(global_name);
    if (!global.empty() && regular_file(global)) {
        bool        truncated = false;
        std::size_t original  = 0;
        InstructionFile file;
        file.display_path   = global.generic_string();
        file.content        = read_file_bounded(global, config_.max_source_bytes, truncated, original);
        file.truncated      = truncated;
        file.original_bytes = original;
        result.files.push_back(std::move(file));
    }

    std::size_t total = 0;
    for (const InstructionFile& file : result.files) {
        total += file.content.size();
    }
    if (total > config_.max_bytes) {
        while (total > config_.max_bytes && result.files.size() > 1) {
            total -= result.files.back().content.size();
            result.omitted.push_back(result.files.back().display_path);
            result.files.pop_back();
        }
        std::optional<std::string> truncated_path;
        std::size_t                truncated_from = 0;
        std::size_t                truncated_to   = 0;
        if (!result.files.empty() && result.files.front().content.size() > config_.max_bytes) {
            InstructionFile& file = result.files.front();
            truncated_from        = file.content.size();
            file.content.resize(config_.max_bytes);
            file.truncated      = true;
            file.original_bytes = truncated_from;
            truncated_path      = file.display_path;
            truncated_to        = file.content.size();
        }

        std::string marker = "Workspace instruction budget " + std::to_string(config_.max_bytes) +
                             " bytes:";
        if (!result.omitted.empty()) {
            marker += " omitted " + join(result.omitted) + ";";
        }
        if (truncated_path.has_value()) {
            marker += " truncated " + *truncated_path + " from " +
                      std::to_string(truncated_from) + " to " + std::to_string(truncated_to) +
                      " bytes";
        }
        if (marker.back() == ';') {
            marker.pop_back();
        }
        result.budget_notice = std::move(marker);
    }

    loaded_ = result.files;
    return result;
}

LoadedInstructions InstructionLoader::refresh_for(const std::filesystem::path& touched) {
    LoadedInstructions result;
    if (config_.max_bytes == 0) {
        return result;
    }

    const std::filesystem::path cwd = environment_.root();
    std::error_code             error;
    std::filesystem::path       directory =
        std::filesystem::is_directory(touched, error) ? touched : touched.parent_path();
    if (!is_within(directory, cwd)) {
        return result;
    }

    std::vector<std::string> names = config_.candidates;
    if (config_.load_local) {
        names.insert(names.end(), config_.local_candidates.begin(), config_.local_candidates.end());
    }

    std::error_code     relative_error;
    std::filesystem::path relative = std::filesystem::relative(directory, cwd, relative_error);
    if (relative_error || relative == ".") {
        relative.clear();
    }

    for (const std::string& name : names) {
        const std::filesystem::path path = directory / name;
        const std::string display_path =
            relative.empty() ? name : relative.generic_string() + "/" + name;

        const auto previous = std::find_if(
            loaded_.begin(), loaded_.end(),
            [&](const InstructionFile& file) { return file.display_path == display_path; });

        if (!regular_file(path)) {
            if (previous != loaded_.end()) {
                result.notices.push_back("Instructions removed: " + display_path +
                                         "\n\nThe previously loaded instructions from this file no "
                                         "longer apply.");
                loaded_.erase(previous);
            }
            continue;
        }

        std::filesystem::path resolved;
        try {
            resolved = environment_.resolve(path.string());
        } catch (const std::exception&) {
            continue;
        }

        bool        truncated = false;
        std::size_t original  = 0;
        InstructionFile file;
        file.display_path   = display_path;
        file.scope          = relative.generic_string();
        file.content        = read_file_bounded(resolved, config_.max_source_bytes, truncated, original);
        file.truncated      = truncated;
        file.original_bytes = original;

        if (previous == loaded_.end()) {
            result.files.push_back(file);
            loaded_.push_back(std::move(file));
        } else if (previous->content != file.content) {
            result.notices.push_back(
                "Updated instructions from: " + display_path +
                "\n\nThis file changed after it was loaded. Use the following content instead of "
                "the previously loaded instructions from this file.\n\n" +
                file.content);
            *previous = std::move(file);
        }
    }

    return result;
}

std::optional<std::string> LoadedInstructions::render_message() const {
    if (empty()) {
        return std::nullopt;
    }

    std::string out = "<system-reminder>\n";
    out += kFraming;
    for (const InstructionFile& file : files) {
        out += "\n\n";
        if (file.scope.empty()) {
            out += "Instructions from: " + file.display_path + "\n\n";
        } else {
            out += "Additional instructions from: " + file.display_path + "\n\n";
            out += "These instructions apply to work under `" + file.scope + "`. ";
            out += kNestedFramingSuffix;
            out += "\n\n";
        }
        out += file.content;
    }
    for (const std::string& notice : notices) {
        out += "\n\n";
        out += notice;
    }
    if (budget_notice.has_value()) {
        out += "\n\n";
        out += *budget_notice;
    }
    out += "\n</system-reminder>";
    return out;
}

} // namespace ymh

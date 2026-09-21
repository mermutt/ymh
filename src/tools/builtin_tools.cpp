#include "ymh/tools/builtin_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ymh/execution/deadline.hpp"
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

class ReadFileTool final : public Tool {
public:
    explicit ReadFileTool([[maybe_unused]] ToolConfig config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"read_file"},
            ToolVersion{1, 0},
            "Read a text file under the workspace root.",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"offset", integer_type()},
                                         {"limit", integer_type()}},
                          nlohmann::json::array({"path"})),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
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

class WriteFileTool final : public Tool {
public:
    explicit WriteFileTool([[maybe_unused]] ToolConfig config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"write_file"},
            ToolVersion{1, 0},
            "Create or overwrite a file under the workspace root.",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"content", string_type()}},
                          nlohmann::json::array({"path", "content"})),
            true};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
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
    explicit EditFileTool([[maybe_unused]] ToolConfig config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"edit_file"},
            ToolVersion{1, 0},
            "Replace text in an existing file under the workspace root.",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"old_string", string_type()},
                                         {"new_string", string_type()},
                                         {"replace_all", boolean_type()}},
                          nlohmann::json::array(
                              {"path", "old_string", "new_string"})),
            true};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::filesystem::path path = resolve_required_path(context, arguments);
        const std::string old_string =
            arguments.value.at("old_string").get<std::string>();
        const std::string new_string =
            arguments.value.at("new_string").get<std::string>();
        if (old_string.empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "old_string must not be empty"};
        }

        const FileData data = context.execution().fs().read(path).get();
        std::string content = data.bytes;
        const std::size_t count = count_occurrences(content, old_string);
        if (count == 0) {
            throw ToolError{ToolErrorCode::NotFound, "old_string not found"};
        }

        const bool replace_all = arguments.value.value("replace_all", false);
        if (replace_all) {
            std::string replaced;
            std::size_t pos = 0;
            std::size_t found = 0;
            while ((found = content.find(old_string, pos)) != std::string::npos) {
                replaced.append(content, pos, found - pos);
                replaced += new_string;
                pos = found + old_string.size();
            }
            replaced.append(content, pos, std::string::npos);
            content = std::move(replaced);
        } else {
            const std::size_t found = content.find(old_string);
            content.replace(found, old_string.size(), new_string);
        }

        context.execution().fs().write(path, Data{content}).get();

        ToolResult result;
        result.id = context.callId();
        result.name = "edit_file";
        result.output = "replaced " + std::to_string(replace_all ? count : 1) +
                        " occurrence(s) in " + relative_string(context, path);
        return Task<ToolResult>(std::move(result));
    }
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
            "Search file contents with an ECMAScript regular expression.",
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
            "List files matching a path glob under the workspace root.",
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
            "Run a command with /bin/bash -lc in the workspace root.",
            object_schema(nlohmann::json{{"command", string_type()},
                                         {"timeout_ms", integer_type()}},
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
        request.argv = {"/bin/bash", "-lc", command};
        request.cwd = context.root();
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
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

} // namespace

std::unique_ptr<Tool> make_read_file_tool(ToolConfig config) {
    return std::make_unique<ReadFileTool>(config);
}

std::unique_ptr<Tool> make_write_file_tool(ToolConfig config) {
    return std::make_unique<WriteFileTool>(config);
}

std::unique_ptr<Tool> make_edit_file_tool(ToolConfig config) {
    return std::make_unique<EditFileTool>(config);
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
    tools.push_back(make_edit_file_tool(config));
    tools.push_back(make_git_diff_tool(config));
    tools.push_back(make_git_status_tool(config));
    tools.push_back(make_glob_tool(config));
    tools.push_back(make_grep_tool(config));
    tools.push_back(make_read_file_tool(config));
    tools.push_back(make_shell_tool(config));
    tools.push_back(make_write_file_tool(config));
    return tools;
}

} // namespace ymh

#include "ymh/tools/git_tools.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include "ymh/execution/services.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

nlohmann::json string_type() { return nlohmann::json{{"type", "string"}}; }
nlohmann::json boolean_type() { return nlohmann::json{{"type", "boolean"}}; }

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
    return nlohmann::json{{"type", "object"},
                          {"properties", std::move(properties)},
                          {"required", std::move(required)},
                          {"additionalProperties", false}};
}

std::filesystem::path resolve_base(const ToolContext& context,
                                   const ToolArguments& arguments) {
    const nlohmann::json& value = arguments.value;
    if (value.contains("path") && value["path"].is_string()) {
        return context.resolve(value["path"].get<std::string>());
    }
    return context.root();
}

std::string clamp_diff(std::string diff, std::size_t cap, bool& truncated) {
    if (diff.size() <= cap) {
        return diff;
    }
    const std::size_t newline = diff.rfind('\n', cap);
    const std::size_t keep = newline == std::string::npos ? cap : newline + 1;
    diff.resize(keep);
    diff += "... diff truncated at ";
    diff += std::to_string(cap);
    diff += " bytes\n";
    truncated = true;
    return diff;
}

class GitStatusTool final : public Tool {
public:
    explicit GitStatusTool([[maybe_unused]] ToolConfig config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"git_status"},
            ToolVersion{1, 0},
            "Summarize working-tree and index status (read-only, porcelain-ish).",
            object_schema(nlohmann::json::object(), nlohmann::json::array()),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        GitQuery query;
        query.path = resolve_base(context, arguments);

        const GitStatus status = context.execution().git().status(query).get();

        ToolResult result;
        result.id = context.callId();
        result.name = "git_status";
        result.output = status.text;
        return Task<ToolResult>(std::move(result));
    }
};

class GitDiffTool final : public Tool {
public:
    explicit GitDiffTool(ToolConfig config) : config_(config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"git_diff"},
            ToolVersion{1, 0},
            "Show a unified diff (unstaged, staged, or against a ref; read-only).",
            object_schema(nlohmann::json{{"path", string_type()},
                                         {"staged", boolean_type()},
                                         {"ref", string_type()}},
                          nlohmann::json::array()),
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        GitQuery query;
        query.path = resolve_base(context, arguments);
        query.staged = arguments.value.value("staged", false);
        query.ref = arguments.value.value("ref", std::string{});

        const GitDiff diff = context.execution().git().diff(query).get();

        bool truncated = false;
        std::string output =
            clamp_diff(diff.text, config_.tool_result_max_bytes, truncated);

        ToolResult result;
        result.id = context.callId();
        result.name = "git_diff";
        result.output = std::move(output);
        result.truncated = truncated;
        return Task<ToolResult>(std::move(result));
    }

private:
    ToolConfig config_;
};

} // namespace

std::unique_ptr<Tool> make_git_status_tool(ToolConfig config) {
    return std::make_unique<GitStatusTool>(config);
}

std::unique_ptr<Tool> make_git_diff_tool(ToolConfig config) {
    return std::make_unique<GitDiffTool>(config);
}

} // namespace ymh

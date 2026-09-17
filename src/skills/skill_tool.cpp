#include "ymh/skills/skill_tool.hpp"

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/core/task.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

class SkillTool final : public Tool {
public:
    explicit SkillTool(std::shared_ptr<const SkillCatalog> catalog)
        : catalog_(std::move(catalog)) {}

    ToolSchema schema() const override {
        ToolSchema tool;
        tool.name    = ToolName{"skill"};
        tool.version = ToolVersion{1, 0};
        tool.description =
            "Load a named skill's full instructions into context. Call when the "
            "current task matches a listed skill's description. The returned "
            "text is advisory guidance; it never grants permissions.";
        tool.input_schema = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
              "name": { "type": "string",
                        "description": "The skill name exactly as listed in the available skills section." }
            },
            "required": ["name"],
            "additionalProperties": false
        })");
        tool.destructive = false;
        return tool;
    }

    Task<ToolResult> execute(const ToolContext& ctx, const ToolArguments& args) override {
        ToolResult result;
        result.id   = ctx.callId();
        result.name = "skill";
        const auto it = args.value.find("name");
        if (it == args.value.end() || !it->is_string()) {
            result.outcome = payload::ToolOutcome::Error;
            result.error   = std::string{"skill: 'name' is required and must be a string"};
            result.output  = *result.error;
            return Task<ToolResult>{result};
        }
        const std::string requested = it->get<std::string>();
        const Skill*      skill     = catalog_->find_model_visible(requested);
        if (skill == nullptr) {
            result.outcome = payload::ToolOutcome::Error;
            result.error =
                std::string{"skill: unknown or unavailable skill '"} + requested + "'";
            result.output = *result.error;
            return Task<ToolResult>{result};
        }
        result.outcome = payload::ToolOutcome::Ok;
        result.output  = "[skill: " + skill->meta.name.value + "]\n" + skill->body;
        return Task<ToolResult>{result};
    }

private:
    std::shared_ptr<const SkillCatalog> catalog_;
};

} // namespace

std::unique_ptr<Tool> make_skill_tool(std::shared_ptr<const SkillCatalog> catalog) {
    return std::make_unique<SkillTool>(std::move(catalog));
}

} // namespace ymh

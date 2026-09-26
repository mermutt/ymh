#include "ymh/tools/plan_tools.hpp"

#include <string>
#include <utility>

#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

class ExitPlanModeTool final : public Tool {
public:
    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"exit_plan_mode"},
            ToolVersion{1, 0},
            "Present the completed plan for user review and leave plan mode on approval. "
            "Only meaningful while plan mode is active.",
            nlohmann::json{{"type", "object"},
                           {"properties",
                            nlohmann::json{{"plan", nlohmann::json{{"type", "string"},
                                                                  {"description",
                                                                   "The complete plan to review"}}}}},
                           {"required", nlohmann::json::array({"plan"})},
                           {"additionalProperties", false}},
            false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const auto plan = arguments.value.find("plan");
        if (plan == arguments.value.end() || !plan->is_string() ||
            plan->get_ref<const std::string&>().empty()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "exit_plan_mode requires a non-empty string 'plan' argument"};
        }
        ToolResult result;
        result.id      = context.callId();
        result.name    = "exit_plan_mode";
        result.outcome = payload::ToolOutcome::Ok;
        result.output  = nlohmann::json{{"approved", true}}.dump();
        return Task<ToolResult>(std::move(result));
    }
};

} // namespace

std::unique_ptr<Tool> make_exit_plan_mode_tool() {
    return std::make_unique<ExitPlanModeTool>();
}

} // namespace ymh

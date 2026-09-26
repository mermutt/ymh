#include "ymh/prompt/runtime_context.hpp"

#include <ctime>
#include <utility>

namespace ymh {
namespace {

std::string current_date() {
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
    gmtime_r(&now, &tm);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return std::string{buffer};
}

} // namespace

std::string render_runtime_context(const RuntimeContextConfig& config, const std::string& date) {
    std::string out;
    out += kRuntimeContextHeader;
    out += "\n\n";
    out += "- Working directory: " + config.cwd + "\n";
    out += "- Model: " + config.model + "\n";
    if (!config.sandbox.empty()) {
        out += "- Sandbox: " + config.sandbox + "\n";
    }
    if (!config.approval.empty()) {
        out += "- Approval: " + config.approval + "\n";
    }
    if (!config.delegation.empty()) {
        out += "- Delegation: " + config.delegation + "\n";
    }
    out += "- Date: " + date;
    return out;
}

ContextHandle register_runtime_context(SystemPrompt&       prompt,
                                       RuntimeContextConfig config,
                                       DateProvider         date) {
    if (!date) {
        date = current_date;
    }
    PromptContext context;
    context.name = std::string{kRuntimeContextName};
    context.order = 0;
    context.text  = [config = std::move(config), date = std::move(date)](const AssembleContext&) {
        return render_runtime_context(config, date());
    };
    return prompt.context(std::move(context));
}

} // namespace ymh

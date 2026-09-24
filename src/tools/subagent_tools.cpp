#include "ymh/tools/subagent_tools.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/agent.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

nlohmann::json string_type() { return nlohmann::json{{"type", "string"}}; }

std::optional<SessionId> caller_session(const SubagentCallerResolver& resolver,
                                        const ToolContext&            context) {
    if (resolver) {
        if (std::optional<SessionId> resolved = resolver(context); resolved.has_value()) {
            return resolved;
        }
    }
    return context.sessionId();
}

ToolResult error_result(const ToolContext& context, std::string name, const std::string& text) {
    ToolResult result;
    result.id      = context.callId();
    result.name    = std::move(name);
    result.outcome = payload::ToolOutcome::Error;
    result.output  = text;
    return result;
}

ToolResult ok_result(const ToolContext& context, std::string name, std::string text) {
    ToolResult result;
    result.id      = context.callId();
    result.name    = std::move(name);
    result.outcome = payload::ToolOutcome::Ok;
    result.output  = std::move(text);
    return result;
}

std::optional<std::string> optional_string(const ToolArguments& arguments, const char* key) {
    const auto found = arguments.value.find(key);
    if (found == arguments.value.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_string()) {
        throw ToolError{ToolErrorCode::InvalidArguments, std::string{key} + " must be a string"};
    }
    return found->get<std::string>();
}

std::optional<bool> optional_bool(const ToolArguments& arguments, const char* key) {
    const auto found = arguments.value.find(key);
    if (found == arguments.value.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_boolean()) {
        throw ToolError{ToolErrorCode::InvalidArguments, std::string{key} + " must be a boolean"};
    }
    return found->get<bool>();
}

std::string require_string(const ToolArguments& arguments, const char* key) {
    const std::optional<std::string> value = optional_string(arguments, key);
    if (!value.has_value() || value->empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, std::string{"missing '"} + key + "'"};
    }
    return *value;
}

std::string status_name(ChildDescriptor::Status status) {
    switch (status) {
        case ChildDescriptor::Status::Running:
            return "running";
        case ChildDescriptor::Status::Idle:
            return "idle";
        case ChildDescriptor::Status::Ready:
            return "ready";
    }
    return "ready";
}

class SubagentTool final : public Tool {
public:
    SubagentTool(SubagentService& service, SubagentCallerResolver caller,
                 DelegationToolConfig config)
        : service_(service), caller_(std::move(caller)), config_(std::move(config)) {}

    ToolSchema schema() const override {
        nlohmann::json properties{
            {"description", string_type()},
            {"prompt", string_type()},
            {"run_in_background", nlohmann::json{{"type", "boolean"}}},
        };
        nlohmann::json required = nlohmann::json::array({"description", "prompt"});
        if (config_.model_selection) {
            properties["provider"]         = string_type();
            properties["model"]            = string_type();
            properties["reasoning_effort"] = string_type();
        }
        return ToolSchema{ToolName{config_.tool_name},
                          ToolVersion{1, 0},
                          "Delegate a task to a subagent.",
                          nlohmann::json{{"type", "object"},
                                         {"properties", std::move(properties)},
                                         {"required", std::move(required)},
                                         {"additionalProperties", false}},
                          false,
                          ToolConcurrencyMode::ParallelSafe};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<SessionId> caller = caller_session(caller_, context);
        if (!caller.has_value()) {
            return Task<ToolResult>(error_result(context, config_.tool_name,
                                                 "subagent requires an owning session"));
        }

        const std::string description = require_string(arguments, "description");
        const std::string prompt      = require_string(arguments, "prompt");

        const std::optional<bool> requested = optional_bool(arguments, "run_in_background");
        bool                      run_in_background = false;
        if (!config_.enable_run_in_background) {
            if (requested.value_or(false)) {
                return Task<ToolResult>(error_result(
                    context, config_.tool_name,
                    "run_in_background is disabled for this delegation tool"));
            }
        } else {
            run_in_background =
                requested.value_or(config_.background_mode ==
                                   DelegationToolConfig::BackgroundMode::Continuable);
        }

        ChildAgentOptions options = config_.agent_options.value_or(ChildAgentOptions{});
        const std::optional<std::string> provider = optional_string(arguments, "provider");
        const std::optional<std::string> model    = optional_string(arguments, "model");
        if (provider.has_value() != model.has_value()) {
            return Task<ToolResult>(error_result(
                context, config_.tool_name, "subagent route requires provider and model together"));
        }
        if (provider.has_value()) {
            options.endpoint = *provider;
            options.model    = *model;
        }
        if (const std::optional<std::string> effort = optional_string(arguments, "reasoning_effort");
            effort.has_value()) {
            options.reasoning_effort = *effort;
        }

        SubagentService::StartRequest request;
        request.parent           = *caller;
        request.label            = description;
        request.prompt           = prompt;
        request.agent_options    = options;
        request.run_in_background = run_in_background;
        if (config_.persona.has_value() || config_.tool_filter.has_value()) {
            ChildComposition composition;
            composition.persona     = config_.persona;
            composition.tool_filter = config_.tool_filter;
            request.composition     = composition;
        }

        const bool continuable =
            config_.background_mode == DelegationToolConfig::BackgroundMode::Continuable;
        StartResult result = continuable ? service_.startContinuable(request).get()
                                         : service_.startOneShot(request).get();
        if (result.error.has_value()) {
            ToolResult error = error_result(context, config_.tool_name, result.error->detail);
            if (result.error->code == AgentErrorCode::Cancelled) {
                error.outcome = payload::ToolOutcome::Cancelled;
            }
            return Task<ToolResult>(std::move(error));
        }
        switch (result.kind) {
            case StartResult::Kind::Foreground:
                return Task<ToolResult>(ok_result(context, config_.tool_name, result.text));
            case StartResult::Kind::BackgroundJob:
                return Task<ToolResult>(ok_result(
                    context, config_.tool_name, "started background subagent job " + result.job_id));
            case StartResult::Kind::Continuable:
                return Task<ToolResult>(ok_result(context, config_.tool_name,
                                                  "started subagent " + result.child.value));
        }
        return Task<ToolResult>(error_result(context, config_.tool_name, "unknown start result"));
    }

private:
    SubagentService&       service_;
    SubagentCallerResolver caller_;
    DelegationToolConfig   config_;
};

class SendMessageTool final : public Tool {
public:
    SendMessageTool(SubagentService& service, SubagentCallerResolver caller)
        : service_(service), caller_(std::move(caller)) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"send_message"},
            ToolVersion{1, 0},
            "Send a message to a continuable subagent.",
            nlohmann::json{{"type", "object"},
                           {"properties",
                            {{"agent_id", string_type()}, {"message", string_type()}}},
                           {"required", nlohmann::json::array({"agent_id", "message"})},
                           {"additionalProperties", false}},
            false,
            ToolConcurrencyMode::ParallelSafe};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<SessionId> caller = caller_session(caller_, context);
        if (!caller.has_value()) {
            return Task<ToolResult>(error_result(context, "send_message", "no owning session"));
        }
        const SessionId target{require_string(arguments, "agent_id")};
        const std::string message = require_string(arguments, "message");
        SendResult result         = service_.sendMessage(*caller, target, message).get();
        if (result.error.has_value()) {
            return Task<ToolResult>(error_result(context, "send_message", result.error->detail));
        }
        return Task<ToolResult>(ok_result(context, "send_message", result.message_id));
    }

private:
    SubagentService&       service_;
    SubagentCallerResolver caller_;
};

class InterruptAgentTool final : public Tool {
public:
    InterruptAgentTool(SubagentService& service, SubagentCallerResolver caller)
        : service_(service), caller_(std::move(caller)) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{"interrupt_agent"},
                          ToolVersion{1, 0},
                          "Stop the current turn of a subagent.",
                          nlohmann::json{{"type", "object"},
                                         {"properties", {{"agent_id", string_type()}}},
                                         {"required", nlohmann::json::array({"agent_id"})},
                                         {"additionalProperties", false}},
                          false,
                          ToolConcurrencyMode::ParallelSafe};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<SessionId> caller = caller_session(caller_, context);
        if (!caller.has_value()) {
            return Task<ToolResult>(error_result(context, "interrupt_agent", "no owning session"));
        }
        const SessionId target{require_string(arguments, "agent_id")};
        service_.interrupt(*caller, target).get();
        return Task<ToolResult>(ok_result(context, "interrupt_agent", "stop requested"));
    }

private:
    SubagentService&       service_;
    SubagentCallerResolver caller_;
};

class ListAgentsTool final : public Tool {
public:
    ListAgentsTool(SubagentService& service, SubagentCallerResolver caller)
        : service_(service), caller_(std::move(caller)) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"list_agents"},
            ToolVersion{1, 0},
            "List this session's continuable subagents.",
            nlohmann::json{{"type", "object"},
                           {"properties", {{"scope", string_type()}}},
                           {"additionalProperties", false}},
            false,
            ToolConcurrencyMode::ParallelSafe};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<SessionId> caller = caller_session(caller_, context);
        if (!caller.has_value()) {
            return Task<ToolResult>(error_result(context, "list_agents", "no owning session"));
        }
        ListScope scope = ListScope::Children;
        if (const std::optional<std::string> value = optional_string(arguments, "scope");
            value.has_value()) {
            if (*value == "descendants") {
                scope = ListScope::Descendants;
            } else if (*value != "children") {
                throw ToolError{ToolErrorCode::InvalidArguments, "unknown scope"};
            }
        }
        std::string output;
        for (const ChildDescriptor& child : service_.list(*caller, scope)) {
            output += child.child.value + " parent=" +
                      (child.parent.has_value() ? child.parent->value : std::string{"-"}) +
                      " depth=" + std::to_string(child.depth) + " status=" +
                      status_name(child.status) + " " + child.label;
            if (child.diagnostic.has_value()) {
                output += " (" + *child.diagnostic + ")";
            }
            output += "\n";
        }
        return Task<ToolResult>(ok_result(context, "list_agents", std::move(output)));
    }

private:
    SubagentService&       service_;
    SubagentCallerResolver caller_;
};

class ListSubagentModelsTool final : public Tool {
public:
    explicit ListSubagentModelsTool(const RouteCatalog& catalog) : catalog_(catalog) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{"list_subagent_models"},
                          ToolVersion{1, 0},
                          "List routable endpoints, models, and reasoning efforts.",
                          nlohmann::json{{"type", "object"},
                                         {"properties",
                                          {{"provider", string_type()}, {"model", string_type()}}},
                                         {"additionalProperties", false}},
                          false,
                          ToolConcurrencyMode::ParallelSafe};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<std::string> provider = optional_string(arguments, "provider");
        const std::optional<std::string> model    = optional_string(arguments, "model");
        if (model.has_value() && !provider.has_value()) {
            return Task<ToolResult>(
                error_result(context, "list_subagent_models", "model requires provider"));
        }
        std::string output;
        if (!provider.has_value()) {
            for (const std::string& endpoint : catalog_.routable_endpoints()) {
                output += endpoint + "\n";
            }
        } else if (!model.has_value()) {
            if (!catalog_.is_routable_endpoint(*provider)) {
                return Task<ToolResult>(error_result(context, "list_subagent_models",
                                                     "unknown endpoint: " + *provider));
            }
            for (const std::string& name : catalog_.models_for(*provider)) {
                output += name + "\n";
            }
        } else {
            for (const std::string& effort : catalog_.efforts_for(*provider, *model)) {
                output += effort + "\n";
            }
        }
        return Task<ToolResult>(ok_result(context, "list_subagent_models", std::move(output)));
    }

private:
    const RouteCatalog& catalog_;
};

} // namespace

std::unique_ptr<Tool> make_subagent_tool(SubagentService&       service,
                                         SubagentCallerResolver caller,
                                         DelegationToolConfig   config) {
    return std::make_unique<SubagentTool>(service, std::move(caller), std::move(config));
}

std::unique_ptr<Tool> make_send_message_tool(SubagentService&       service,
                                             SubagentCallerResolver caller) {
    return std::make_unique<SendMessageTool>(service, std::move(caller));
}

std::unique_ptr<Tool> make_interrupt_agent_tool(SubagentService&       service,
                                                SubagentCallerResolver caller) {
    return std::make_unique<InterruptAgentTool>(service, std::move(caller));
}

std::unique_ptr<Tool> make_list_agents_tool(SubagentService&       service,
                                            SubagentCallerResolver caller) {
    return std::make_unique<ListAgentsTool>(service, std::move(caller));
}

std::unique_ptr<Tool> make_list_subagent_models_tool(const RouteCatalog& catalog) {
    return std::make_unique<ListSubagentModelsTool>(catalog);
}

} // namespace ymh

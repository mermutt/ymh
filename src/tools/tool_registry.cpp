#include "ymh/tools/tool_registry.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace ymh {
namespace {

using SteadyClock = std::chrono::steady_clock;

payload::ToolResult make_result(const payload::ToolCall& call) {
    payload::ToolResult result;
    result.id = call.id;
    result.name = call.name;
    return result;
}

payload::ToolResult error_result(const payload::ToolCall& call,
                                 ToolErrorCode code) {
    payload::ToolResult result = make_result(call);
    result.outcome = payload::ToolOutcome::Error;
    result.error = std::string(to_string(code));
    return result;
}

} // namespace

ToolRegistry::Registration::Registration(ToolRegistry* owner, std::string name) noexcept
    : owner_(owner), name_(std::move(name)) {}

ToolRegistry::Registration::Registration(Registration&& other) noexcept
    : owner_(other.owner_), name_(std::move(other.name_)) {
    other.owner_ = nullptr;
}

ToolRegistry::Registration& ToolRegistry::Registration::operator=(
    Registration&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) {
            owner_->release_registration(name_);
        }
        owner_ = other.owner_;
        name_ = std::move(other.name_);
        other.owner_ = nullptr;
    }
    return *this;
}

ToolRegistry::Registration::~Registration() {
    if (owner_ != nullptr) {
        owner_->release_registration(name_);
    }
}

ToolRegistry::Registration ToolRegistry::add(std::unique_ptr<Tool> tool) {
    if (frozen_) {
        throw ToolRegistryError{ToolRegistryErrorCode::RegistryFrozen,
                                "registry is frozen"};
    }
    if (!tool) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidName, "null tool"};
    }
    const ToolSchema schema = tool->schema();
    if (!is_valid_tool_name(schema.name.value)) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidName,
                                "invalid tool name: " + schema.name.value};
    }
    validate_input_schema(schema.input_schema);

    const auto existing = tools_.find(schema.name.value);
    if (existing != tools_.end()) {
        if (existing->second->version() == schema.version) {
            throw ToolRegistryError{ToolRegistryErrorCode::DuplicateVersion,
                                    "duplicate tool version: " + schema.name.value};
        }
        throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                "tool name already registered: " + schema.name.value};
    }

    const std::string name = schema.name.value;
    tools_.emplace(name, std::move(tool));
    ++generation_;
    return Registration(this, name);
}

void ToolRegistry::release_registration(const std::string& name) {
    if (frozen_) {
        return;
    }
    if (tools_.erase(name) > 0) {
        ++generation_;
    }
}

void ToolRegistry::freeze() { frozen_ = true; }

Tool* ToolRegistry::find(const ToolName& name) const noexcept {
    const auto it = tools_.find(name.value);
    return it == tools_.end() ? nullptr : it->second.get();
}

bool ToolRegistry::contains(const ToolName& name) const noexcept {
    return tools_.find(name.value) != tools_.end();
}

std::vector<ToolName> ToolRegistry::names() const {
    std::vector<ToolName> result;
    result.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        result.push_back(ToolName{name});
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<ToolSchema> ToolRegistry::schemas() const {
    std::vector<ToolSchema> result;
    result.reserve(tools_.size());
    for (const ToolName& name : names()) {
        result.push_back(tools_.at(name.value)->schema());
    }
    return result;
}

Task<ToolResult> ToolRegistry::execute(const payload::ToolCall& call,
                                       const ToolContext& context) {
    const SteadyClock::time_point start = SteadyClock::now();
    const auto finish = [this, &start](ToolResult result) {
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now() - start);
        clamp_tool_result(result, config_.tool_result_max_bytes);
        return Task<ToolResult>(std::move(result));
    };

    Tool* tool = find(ToolName{call.name});
    if (tool == nullptr) {
        return finish(error_result(call, ToolErrorCode::UnknownTool));
    }
    if (!call.arguments.is_object()) {
        return finish(error_result(call, ToolErrorCode::InvalidArguments));
    }
    if (!schema_validate(tool->schema().input_schema, call.arguments)) {
        return finish(error_result(call, ToolErrorCode::InvalidArguments));
    }

    const ToolArguments arguments{call.arguments};
    try {
        ToolResult result = tool->execute(context, arguments).get();
        result.id = call.id;
        result.name = call.name;
        if (result.output.empty()) {
            result.output = context.output().materialize(config_.tool_result_max_bytes);
        }
        result.truncated = result.truncated || context.output().truncated();
        if (result.outcome == payload::ToolOutcome::Ok &&
            context.cancellation().cancelled()) {
            result.outcome = payload::ToolOutcome::Cancelled;
        }
        return finish(std::move(result));
    } catch (const ToolError& error) {
        ToolResult result = error_result(call, error.code());
        result.output = context.output().materialize(config_.tool_result_max_bytes);
        result.truncated = context.output().truncated();
        return finish(std::move(result));
    } catch (const CancellationError&) {
        ToolResult result = make_result(call);
        result.outcome = payload::ToolOutcome::Cancelled;
        result.output = context.output().materialize(config_.tool_result_max_bytes);
        result.truncated = context.output().truncated();
        return finish(std::move(result));
    } catch (const std::exception&) {
        ToolResult result = error_result(call, ToolErrorCode::Internal);
        result.output = context.output().materialize(config_.tool_result_max_bytes);
        result.truncated = context.output().truncated();
        return finish(std::move(result));
    } catch (...) {
        ToolResult result = error_result(call, ToolErrorCode::Internal);
        return finish(std::move(result));
    }
}

} // namespace ymh

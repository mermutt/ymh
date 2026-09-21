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
    if (!tool) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidName, "null tool"};
    }
    const ToolSchema schema = tool->schema();
    if (!is_valid_tool_name(schema.name.value)) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidName,
                                "invalid tool name: " + schema.name.value};
    }
    validate_input_schema(schema.input_schema);

    const std::string name = schema.name.value;
    std::lock_guard<std::mutex> lock(mutex_);
    if (frozen_.load()) {
        throw ToolRegistryError{ToolRegistryErrorCode::RegistryFrozen,
                                "registry is frozen"};
    }
    const auto existing = tools_.find(name);
    if (existing != tools_.end()) {
        if (existing->second->version() == schema.version) {
            throw ToolRegistryError{ToolRegistryErrorCode::DuplicateVersion,
                                    "duplicate tool version: " + name};
        }
        throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                "tool name already registered: " + name};
    }

    tools_.emplace(name, std::shared_ptr<Tool>(std::move(tool)));
    generation_.fetch_add(1);
    return Registration(this, name);
}

void ToolRegistry::release_registration(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frozen_.load()) {
        return;
    }
    const auto found = tools_.find(name);
    if (found != tools_.end() && !name_owned_by_other_scope(name, {})) {
        tools_.erase(found);
        generation_.fetch_add(1);
    }
}

void ToolRegistry::freeze() { frozen_.store(true); }

ToolRegistry::AdapterScope::AdapterScope(ToolRegistry* owner, std::string prefix) noexcept
    : owner_(owner), prefix_(std::move(prefix)) {}

ToolRegistry::AdapterScope::AdapterScope(AdapterScope&& other) noexcept
    : owner_(other.owner_), prefix_(std::move(other.prefix_)) {
    other.owner_ = nullptr;
}

ToolRegistry::AdapterScope& ToolRegistry::AdapterScope::operator=(
    AdapterScope&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) {
            owner_->release_scope(prefix_);
        }
        owner_ = other.owner_;
        prefix_ = std::move(other.prefix_);
        other.owner_ = nullptr;
    }
    return *this;
}

ToolRegistry::AdapterScope::~AdapterScope() {
    if (owner_ != nullptr) {
        owner_->release_scope(prefix_);
    }
}

std::size_t ToolRegistry::AdapterScope::replace(
    std::vector<std::unique_ptr<Tool>> tools) {
    if (owner_ == nullptr) {
        throw ToolRegistryError{ToolRegistryErrorCode::RegistryFrozen,
                                "adapter scope is not held"};
    }
    return owner_->replace_scope(prefix_, std::move(tools));
}

std::size_t ToolRegistry::AdapterScope::size() const noexcept {
    if (owner_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(owner_->mutex_);
    const auto found = owner_->adapter_names_.find(prefix_);
    return found == owner_->adapter_names_.end() ? 0 : found->second.size();
}

ToolRegistry::AdapterScope ToolRegistry::openAdapter(std::string_view prefix) {
    const std::string scope(prefix);
    if (scope.size() < 2 || scope.back() != '.' ||
        !is_valid_tool_name(scope + "x")) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidName,
                                "invalid adapter prefix: " + scope};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (frozen_.load()) {
        throw ToolRegistryError{ToolRegistryErrorCode::RegistryFrozen,
                                "registry is frozen"};
    }
    if (adapter_names_.find(scope) != adapter_names_.end()) {
        throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                "adapter prefix already open: " + scope};
    }
    for (const std::string& other : adapter_order_) {
        if (scope.starts_with(other) || other.starts_with(scope)) {
            throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                    "adapter prefixes overlap: " + scope};
        }
    }
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        if (name.starts_with(scope)) {
            throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                    "adapter shadows a registered tool: " + name};
        }
    }
    adapter_names_.emplace(scope, std::vector<std::string>{});
    adapter_order_.push_back(scope);
    return AdapterScope(this, scope);
}

std::size_t ToolRegistry::replace_scope(
    const std::string& prefix,
    std::vector<std::unique_ptr<Tool>> tools) {
    std::vector<std::string> new_names;
    new_names.reserve(tools.size());
    for (const std::unique_ptr<Tool>& tool : tools) {
        if (!tool) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidName, "null tool"};
        }
        const ToolSchema schema = tool->schema();
        if (!schema.name.value.starts_with(prefix) ||
            !is_valid_tool_name(schema.name.value)) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidName,
                                    "tool outside adapter scope: " + schema.name.value};
        }
        validate_input_schema(schema.input_schema);
        if (std::find(new_names.begin(), new_names.end(), schema.name.value) !=
            new_names.end()) {
            throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                    "duplicate adapter tool: " + schema.name.value};
        }
        new_names.push_back(schema.name.value);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto scope = adapter_names_.find(prefix);
    if (scope == adapter_names_.end()) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidName,
                                "adapter scope is not open: " + prefix};
    }

    for (const std::string& name : new_names) {
        const auto existing = tools_.find(name);
        if (existing == tools_.end()) {
            continue;
        }
        const bool owned_by_scope =
            std::find(scope->second.begin(), scope->second.end(), name) != scope->second.end();
        if (!owned_by_scope) {
            throw ToolRegistryError{ToolRegistryErrorCode::DuplicateName,
                                    "adapter shadows a registered tool: " + name};
        }
    }

    for (const std::string& name : scope->second) {
        tools_.erase(name);
    }
    for (std::unique_ptr<Tool>& tool : tools) {
        const std::string name = tool->name().value;
        tools_.emplace(name, std::shared_ptr<Tool>(std::move(tool)));
    }
    scope->second = new_names;
    generation_.fetch_add(1);
    return new_names.size();
}

void ToolRegistry::release_scope(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto scope = adapter_names_.find(prefix);
    if (scope == adapter_names_.end()) {
        return;
    }
    for (const std::string& name : scope->second) {
        tools_.erase(name);
    }
    adapter_names_.erase(scope);
    adapter_order_.erase(
        std::remove(adapter_order_.begin(), adapter_order_.end(), prefix),
        adapter_order_.end());
    generation_.fetch_add(1);
}

bool ToolRegistry::name_owned_by_other_scope(const std::string& name,
                                             const std::string& prefix) const {
    for (const auto& [scope_prefix, names] : adapter_names_) {
        if (scope_prefix == prefix) {
            continue;
        }
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<Tool> ToolRegistry::lookup(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tools_.find(name);
    return found == tools_.end() ? nullptr : found->second;
}

Tool* ToolRegistry::find(const ToolName& name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tools_.find(name.value);
    return found == tools_.end() ? nullptr : found->second.get();
}

bool ToolRegistry::contains(const ToolName& name) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.find(name.value) != tools_.end();
}

std::size_t ToolRegistry::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.size();
}

std::vector<ToolName> ToolRegistry::names() const {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ToolName> ordered;
    ordered.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        ordered.push_back(ToolName{name});
    }
    std::sort(ordered.begin(), ordered.end());

    std::vector<ToolSchema> result;
    result.reserve(ordered.size());
    for (const ToolName& name : ordered) {
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
        retain_tool_result(result, config_.tool_result_max_bytes);
        return Task<ToolResult>(std::move(result));
    };

    const std::shared_ptr<Tool> tool = lookup(call.name);
    if (tool == nullptr) {
        return finish(error_result(call, ToolErrorCode::UnknownTool));
    }
    if (!call.arguments.is_object()) {
        return finish(error_result(call, ToolErrorCode::InvalidArguments));
    }
    if (!schema_validate(tool->schema().input_schema, call.arguments)) {
        return finish(error_result(call, ToolErrorCode::InvalidArguments));
    }
    if (context.expired()) {
        return finish(error_result(call, ToolErrorCode::Timeout));
    }

    const ToolArguments arguments{call.arguments};
    try {
        ToolResult result = tool->execute(context, arguments).get();
        result.id = call.id;
        result.name = call.name;
        if (result.output.empty()) {
            result.output = context.output().materialize(config_.tool_result_max_bytes);
        }
        result.truncated = result.truncated
                               ? true
                               : context.output().truncated();
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

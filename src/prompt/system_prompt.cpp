#include "ymh/prompt/system_prompt.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

#include "ymh/config/config.hpp"

namespace ymh {
namespace {

const std::string kUnlistedTools = "<unlisted-tools>";

std::string interpolate(const std::string& text,
                        const std::map<std::string, std::optional<std::string>>& variables) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t open = text.find("{{", index);
        if (open == std::string::npos) {
            out.append(text, index, std::string::npos);
            break;
        }
        const std::size_t close = text.find("}}", open + 2);
        if (close == std::string::npos) {
            out.append(text, index, open + 2 - index);
            index = open + 2;
            continue;
        }
        out.append(text, index, open - index);
        const std::string name = text.substr(open + 2, close - (open + 2));
        if (name.empty()) {
            throw ConfigError("prompt variable reference '{{}}' is malformed");
        }
        const auto found = variables.find(name);
        if (found == variables.end() || !found->second.has_value()) {
            throw ConfigError("unknown prompt variable '" + name + "'");
        }
        out += *found->second;
        index = close + 2;
    }
    return out;
}

} // namespace

SectionHandle::SectionHandle(SystemPrompt* owner, std::string name, ScopeKey scope)
    : owner_(owner), name_(std::move(name)), scope_(std::move(scope)) {}

SectionHandle::~SectionHandle() { reset(); }

SectionHandle::SectionHandle(SectionHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      name_(std::move(other.name_)),
      scope_(std::move(other.scope_)) {}

SectionHandle& SectionHandle::operator=(SectionHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        name_  = std::move(other.name_);
        scope_ = std::move(other.scope_);
    }
    return *this;
}

void SectionHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_section(scope_, name_);
        owner_ = nullptr;
        name_.clear();
        scope_.clear();
    }
}

ContextHandle::ContextHandle(SystemPrompt* owner, std::string name, ScopeKey scope)
    : owner_(owner), name_(std::move(name)), scope_(std::move(scope)) {}

ContextHandle::~ContextHandle() { reset(); }

ContextHandle::ContextHandle(ContextHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      name_(std::move(other.name_)),
      scope_(std::move(other.scope_)) {}

ContextHandle& ContextHandle::operator=(ContextHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        name_  = std::move(other.name_);
        scope_ = std::move(other.scope_);
    }
    return *this;
}

void ContextHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_context(scope_, name_);
        owner_ = nullptr;
        name_.clear();
        scope_.clear();
    }
}

VariableHandle::VariableHandle(SystemPrompt* owner, std::string name, ScopeKey scope)
    : owner_(owner), name_(std::move(name)), scope_(std::move(scope)) {}

VariableHandle::~VariableHandle() { reset(); }

VariableHandle::VariableHandle(VariableHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      name_(std::move(other.name_)),
      scope_(std::move(other.scope_)) {}

VariableHandle& VariableHandle::operator=(VariableHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        name_  = std::move(other.name_);
        scope_ = std::move(other.scope_);
    }
    return *this;
}

void VariableHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_variable(scope_, name_);
        owner_ = nullptr;
        name_.clear();
        scope_.clear();
    }
}

ScopeHandle::ScopeHandle(SystemPrompt* owner, ScopeKey key)
    : owner_(owner), key_(std::move(key)) {}

ScopeHandle::~ScopeHandle() { reset(); }

ScopeHandle::ScopeHandle(ScopeHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), key_(std::move(other.key_)) {}

ScopeHandle& ScopeHandle::operator=(ScopeHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        key_   = std::move(other.key_);
    }
    return *this;
}

void ScopeHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_scope(key_);
        owner_ = nullptr;
        key_.clear();
    }
}

void ScopeHandle::add_section(PromptSection section) {
    if (owner_ != nullptr) {
        owner_->register_section(key_, std::move(section));
    }
}

void ScopeHandle::add_context(PromptContext context) {
    if (owner_ != nullptr) {
        owner_->register_context(key_, std::move(context));
    }
}

void ScopeHandle::add_variable(
    std::string name,
    std::function<std::optional<std::string>(const AssembleContext&)> provider) {
    if (owner_ != nullptr) {
        owner_->register_variable(key_, std::move(name), std::move(provider));
    }
}

void ScopeHandle::set_tool_filter(
    std::function<std::vector<ToolSchema>(std::vector<ToolSchema>)> filter) {
    if (owner_ != nullptr) {
        const auto found = owner_->scopes_.find(key_);
        if (found != owner_->scopes_.end()) {
            found->second.tool_filter = std::move(filter);
        }
    }
}

SystemPrompt::SystemPrompt(std::vector<std::string> tool_order)
    : tool_order_(std::move(tool_order)) {}

SectionHandle SystemPrompt::section(PromptSection section) {
    return add_section(ScopeKey{}, std::move(section));
}

ContextHandle SystemPrompt::context(PromptContext context) {
    return add_context(ScopeKey{}, std::move(context));
}

VariableHandle SystemPrompt::variable(
    std::string name,
    std::function<std::optional<std::string>(const AssembleContext&)> provider) {
    return add_variable(ScopeKey{}, std::move(name), std::move(provider));
}

void SystemPrompt::set_tool_provider(
    std::function<std::vector<ToolSchema>(const AssembleContext&)> provider) {
    tool_provider_ = std::move(provider);
}

ScopeHandle SystemPrompt::scope(ScopeKey parent, ScopeKey key) {
    if (key.empty()) {
        throw ConfigError("prompt scope key must be non-empty");
    }
    if (scopes_.count(key) != 0) {
        throw ConfigError("duplicate prompt scope '" + key + "'");
    }
    if (!parent.empty() && scopes_.count(parent) == 0) {
        throw ConfigError("prompt scope parent '" + parent + "' does not exist");
    }
    ScopeLayer layer;
    layer.parent = std::move(parent);
    scopes_.emplace(key, std::move(layer));
    return ScopeHandle(this, std::move(key));
}

std::optional<ScopeKey> SystemPrompt::scope_parent(const ScopeKey& key) const {
    const auto found = scopes_.find(key);
    if (found == scopes_.end() || found->second.parent.empty()) {
        return std::nullopt;
    }
    return found->second.parent;
}

void SystemPrompt::register_section(const ScopeKey& scope, PromptSection section) {
    if (section.name.empty()) {
        throw ConfigError("prompt section name must be non-empty");
    }
    std::map<std::string, PromptSection>* target = &sections_;
    if (!scope.empty()) {
        const auto found = scopes_.find(scope);
        if (found == scopes_.end()) {
            throw ConfigError("prompt scope '" + scope + "' does not exist");
        }
        target = &found->second.sections;
    }
    const auto [position, inserted] = target->try_emplace(section.name, std::move(section));
    if (!inserted) {
        throw ConfigError("duplicate prompt section '" + position->first + "'");
    }
}

void SystemPrompt::register_context(const ScopeKey& scope, PromptContext context) {
    if (context.name.empty()) {
        throw ConfigError("prompt context name must be non-empty");
    }
    std::map<std::string, PromptContext>* target = &contexts_;
    if (!scope.empty()) {
        const auto found = scopes_.find(scope);
        if (found == scopes_.end()) {
            throw ConfigError("prompt scope '" + scope + "' does not exist");
        }
        target = &found->second.contexts;
    }
    const auto [position, inserted] = target->try_emplace(context.name, std::move(context));
    if (!inserted) {
        throw ConfigError("duplicate prompt context '" + position->first + "'");
    }
}

void SystemPrompt::register_variable(
    const ScopeKey& scope, std::string name,
    std::function<std::optional<std::string>(const AssembleContext&)> provider) {
    if (name.empty()) {
        throw ConfigError("prompt variable name must be non-empty");
    }
    std::map<std::string, std::function<std::optional<std::string>(const AssembleContext&)>>*
        target = &variables_;
    if (!scope.empty()) {
        const auto found = scopes_.find(scope);
        if (found == scopes_.end()) {
            throw ConfigError("prompt scope '" + scope + "' does not exist");
        }
        target = &found->second.variables;
    }
    const auto [position, inserted] = target->try_emplace(name, std::move(provider));
    if (!inserted) {
        throw ConfigError("duplicate prompt variable '" + position->first + "'");
    }
}

SectionHandle SystemPrompt::add_section(const ScopeKey& scope, PromptSection section) {
    const std::string name = section.name;
    register_section(scope, std::move(section));
    return SectionHandle(this, name, scope);
}

ContextHandle SystemPrompt::add_context(const ScopeKey& scope, PromptContext context) {
    const std::string name = context.name;
    register_context(scope, std::move(context));
    return ContextHandle(this, name, scope);
}

VariableHandle SystemPrompt::add_variable(
    const ScopeKey& scope, std::string name,
    std::function<std::optional<std::string>(const AssembleContext&)> provider) {
    const std::string registered = name;
    register_variable(scope, std::move(name), std::move(provider));
    return VariableHandle(this, registered, scope);
}

void SystemPrompt::remove_section(const ScopeKey& scope, const std::string& name) noexcept {
    if (scope.empty()) {
        sections_.erase(name);
        return;
    }
    const auto found = scopes_.find(scope);
    if (found != scopes_.end()) {
        found->second.sections.erase(name);
    }
}

void SystemPrompt::remove_context(const ScopeKey& scope, const std::string& name) noexcept {
    if (scope.empty()) {
        contexts_.erase(name);
        return;
    }
    const auto found = scopes_.find(scope);
    if (found != scopes_.end()) {
        found->second.contexts.erase(name);
    }
}

void SystemPrompt::remove_variable(const ScopeKey& scope, const std::string& name) noexcept {
    if (scope.empty()) {
        variables_.erase(name);
        return;
    }
    const auto found = scopes_.find(scope);
    if (found != scopes_.end()) {
        found->second.variables.erase(name);
    }
}

void SystemPrompt::remove_scope(const ScopeKey& key) noexcept {
    std::vector<ScopeKey> children;
    for (const auto& [candidate, layer] : scopes_) {
        if (layer.parent == key) {
            children.push_back(candidate);
        }
    }
    for (const ScopeKey& child : children) {
        remove_scope(child);
    }
    scopes_.erase(key);
}

std::vector<const SystemPrompt::ScopeLayer*> SystemPrompt::scope_chain(
    const std::optional<ScopeKey>& leaf) const {
    std::vector<const ScopeLayer*> reverse_chain;
    std::set<ScopeKey>             seen;
    std::optional<ScopeKey>        cursor = leaf;
    while (cursor.has_value() && !cursor->empty()) {
        const auto found = scopes_.find(*cursor);
        if (found == scopes_.end()) {
            break;
        }
        if (!seen.insert(*cursor).second) {
            break;
        }
        reverse_chain.push_back(&found->second);
        cursor = found->second.parent.empty() ? std::nullopt
                                              : std::optional<ScopeKey>{found->second.parent};
    }
    std::reverse(reverse_chain.begin(), reverse_chain.end());
    return reverse_chain;
}

PromptAssembly SystemPrompt::assemble(const AssembleContext& context) const {
    PromptAssembly assembly;

    const std::vector<const ScopeLayer*> chain = scope_chain(context.scope);

    std::map<std::string, const PromptSection*> merged_sections;
    std::map<std::string, const PromptContext*> merged_contexts;
    std::map<std::string, const std::function<std::optional<std::string>(const AssembleContext&)>*>
        merged_variables;
    for (const auto& [name, section] : sections_) {
        merged_sections[name] = &section;
    }
    for (const auto& [name, prompt_context] : contexts_) {
        merged_contexts[name] = &prompt_context;
    }
    for (const auto& [name, provider] : variables_) {
        merged_variables[name] = &provider;
    }
    for (const ScopeLayer* layer : chain) {
        for (const auto& [name, section] : layer->sections) {
            merged_sections[name] = &section;
        }
        for (const auto& [name, prompt_context] : layer->contexts) {
            merged_contexts[name] = &prompt_context;
        }
        for (const auto& [name, provider] : layer->variables) {
            merged_variables[name] = &provider;
        }
    }

    std::vector<const PromptSection*> ordered_sections;
    ordered_sections.reserve(merged_sections.size());
    for (const auto& [name, section] : merged_sections) {
        (void)name;
        ordered_sections.push_back(section);
    }
    std::sort(ordered_sections.begin(), ordered_sections.end(),
              [](const PromptSection* left, const PromptSection* right) {
                  if (left->order != right->order) {
                      return left->order < right->order;
                  }
                  return left->name < right->name;
              });

    std::vector<AssembledSection> effective;
    effective.reserve(ordered_sections.size());
    for (const PromptSection* section : ordered_sections) {
        std::string text = section->text ? section->text(context) : std::string{};
        if (text.empty()) {
            continue;
        }
        AssembledSection assembled;
        assembled.name     = section->name;
        assembled.text     = std::move(text);
        assembled.complete = section->complete;
        effective.push_back(std::move(assembled));
    }

    std::vector<AssembledSection> complete;
    for (const AssembledSection& section : effective) {
        if (section.complete) {
            complete.push_back(section);
        }
    }
    if (complete.size() > 1) {
        throw ConfigError("prompt assembly has more than one complete section");
    }
    if (complete.size() == 1) {
        assembly.sections.push_back(std::move(complete.front()));
    } else {
        assembly.sections = std::move(effective);
    }

    std::vector<const PromptContext*> ordered_contexts;
    ordered_contexts.reserve(merged_contexts.size());
    for (const auto& [name, prompt_context] : merged_contexts) {
        (void)name;
        ordered_contexts.push_back(prompt_context);
    }
    std::sort(ordered_contexts.begin(), ordered_contexts.end(),
              [](const PromptContext* left, const PromptContext* right) {
                  if (left->order != right->order) {
                      return left->order < right->order;
                  }
                  return left->name < right->name;
              });
    for (const PromptContext* prompt_context : ordered_contexts) {
        std::string text =
            prompt_context->text ? prompt_context->text(context) : std::string{};
        if (text.empty()) {
            continue;
        }
        AssembledContext assembled;
        assembled.name = prompt_context->name;
        assembled.text = std::move(text);
        assembly.contexts.push_back(std::move(assembled));
    }

    for (const auto& [name, provider] : merged_variables) {
        std::optional<std::string> value;
        if (provider != nullptr && *provider) {
            value = (*provider)(context);
        }
        assembly.variables.emplace(name, std::move(value));
    }

    std::vector<ToolSchema> provider_tools;
    if (tool_provider_) {
        provider_tools = tool_provider_(context);
    }
    for (const ScopeLayer* layer : chain) {
        if (layer->tool_filter) {
            provider_tools = layer->tool_filter(std::move(provider_tools));
        }
    }
    std::sort(provider_tools.begin(), provider_tools.end(),
              [](const ToolSchema& left, const ToolSchema& right) {
                  return left.name.value < right.name.value;
              });

    assembly.tool_order = tool_order_;
    if (tool_order_.empty()) {
        assembly.tools = std::move(provider_tools);
        return assembly;
    }

    std::set<std::string> named;
    std::size_t           rest_count = 0;
    for (const std::string& entry : tool_order_) {
        if (entry == kUnlistedTools) {
            ++rest_count;
            continue;
        }
        if (!named.insert(entry).second) {
            throw ConfigError("duplicate tool_order entry '" + entry + "'");
        }
        const bool known =
            std::any_of(provider_tools.begin(), provider_tools.end(),
                        [&entry](const ToolSchema& schema) { return schema.name.value == entry; });
        if (!known) {
            throw ConfigError("unknown tool_order entry '" + entry + "'");
        }
    }
    if (rest_count != 1) {
        throw ConfigError("tool_order must contain <unlisted-tools> exactly once");
    }

    for (const std::string& entry : tool_order_) {
        if (entry == kUnlistedTools) {
            for (const ToolSchema& schema : provider_tools) {
                if (named.count(schema.name.value) == 0) {
                    assembly.tools.push_back(schema);
                }
            }
            continue;
        }
        for (const ToolSchema& schema : provider_tools) {
            if (schema.name.value == entry) {
                assembly.tools.push_back(schema);
                break;
            }
        }
    }
    return assembly;
}

std::string SystemPrompt::render(const AssembleContext& context) const {
    return render_prompt(assemble(context));
}

std::string render_prompt(const PromptAssembly& assembly) {
    std::string out;
    bool        first = true;
    for (const AssembledSection& section : assembly.sections) {
        if (section.text.empty()) {
            continue;
        }
        if (!first) {
            out += "\n\n";
        }
        out += interpolate(section.text, assembly.variables);
        first = false;
    }
    return out;
}

} // namespace ymh

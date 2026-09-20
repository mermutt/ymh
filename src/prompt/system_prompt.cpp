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

SectionHandle::SectionHandle(SystemPrompt* owner, std::string name)
    : owner_(owner), name_(std::move(name)) {}

SectionHandle::~SectionHandle() { reset(); }

SectionHandle::SectionHandle(SectionHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), name_(std::move(other.name_)) {}

SectionHandle& SectionHandle::operator=(SectionHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        name_  = std::move(other.name_);
    }
    return *this;
}

void SectionHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_section(name_);
        owner_ = nullptr;
        name_.clear();
    }
}

ContextHandle::ContextHandle(SystemPrompt* owner, std::string name)
    : owner_(owner), name_(std::move(name)) {}

ContextHandle::~ContextHandle() { reset(); }

ContextHandle::ContextHandle(ContextHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), name_(std::move(other.name_)) {}

ContextHandle& ContextHandle::operator=(ContextHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        name_  = std::move(other.name_);
    }
    return *this;
}

void ContextHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_context(name_);
        owner_ = nullptr;
        name_.clear();
    }
}

VariableHandle::VariableHandle(SystemPrompt* owner, std::string name)
    : owner_(owner), name_(std::move(name)) {}

VariableHandle::~VariableHandle() { reset(); }

VariableHandle::VariableHandle(VariableHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), name_(std::move(other.name_)) {}

VariableHandle& VariableHandle::operator=(VariableHandle&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        name_  = std::move(other.name_);
    }
    return *this;
}

void VariableHandle::reset() noexcept {
    if (owner_ != nullptr) {
        owner_->remove_variable(name_);
        owner_ = nullptr;
        name_.clear();
    }
}

SystemPrompt::SystemPrompt(std::vector<std::string> tool_order)
    : tool_order_(std::move(tool_order)) {}

SectionHandle SystemPrompt::section(PromptSection section) {
    if (section.name.empty()) {
        throw ConfigError("prompt section name must be non-empty");
    }
    const auto [position, inserted] = sections_.try_emplace(section.name, std::move(section));
    if (!inserted) {
        throw ConfigError("duplicate prompt section '" + position->first + "'");
    }
    return SectionHandle(this, position->first);
}

ContextHandle SystemPrompt::context(PromptContext context) {
    if (context.name.empty()) {
        throw ConfigError("prompt context name must be non-empty");
    }
    const auto [position, inserted] = contexts_.try_emplace(context.name, std::move(context));
    if (!inserted) {
        throw ConfigError("duplicate prompt context '" + position->first + "'");
    }
    return ContextHandle(this, position->first);
}

VariableHandle SystemPrompt::variable(
    std::string name,
    std::function<std::optional<std::string>(const AssembleContext&)> provider) {
    if (name.empty()) {
        throw ConfigError("prompt variable name must be non-empty");
    }
    const auto [position, inserted] = variables_.try_emplace(name, std::move(provider));
    if (!inserted) {
        throw ConfigError("duplicate prompt variable '" + position->first + "'");
    }
    return VariableHandle(this, position->first);
}

void SystemPrompt::set_tool_provider(
    std::function<std::vector<ToolSchema>(const AssembleContext&)> provider) {
    tool_provider_ = std::move(provider);
}

void SystemPrompt::remove_section(const std::string& name) noexcept { sections_.erase(name); }

void SystemPrompt::remove_context(const std::string& name) noexcept { contexts_.erase(name); }

void SystemPrompt::remove_variable(const std::string& name) noexcept { variables_.erase(name); }

PromptAssembly SystemPrompt::assemble(const AssembleContext& context) const {
    PromptAssembly assembly;

    std::vector<const PromptSection*> ordered_sections;
    ordered_sections.reserve(sections_.size());
    for (const auto& [name, section] : sections_) {
        (void)name;
        ordered_sections.push_back(&section);
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
    ordered_contexts.reserve(contexts_.size());
    for (const auto& [name, prompt_context] : contexts_) {
        (void)name;
        ordered_contexts.push_back(&prompt_context);
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

    for (const auto& [name, provider] : variables_) {
        std::optional<std::string> value;
        if (provider) {
            value = provider(context);
        }
        assembly.variables.emplace(name, std::move(value));
    }

    std::vector<ToolSchema> provider_tools;
    if (tool_provider_) {
        provider_tools = tool_provider_(context);
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

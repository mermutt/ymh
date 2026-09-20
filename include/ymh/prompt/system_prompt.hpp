#pragma once

// The ordered prompt registry (36 §2.1-§2.7). A `SystemPrompt` holds ordered
// sections, ordered contexts, variable providers, and one tool provider.
// Registration returns a move-only RAII handle that unregisters on destruction;
// handles must not outlive their `SystemPrompt`. Single-threaded: owned by
// `WorkspaceRuntime` and called only on the agent executor thread (36-D14).
//
// `assemble()` runs the waterfall and returns the loop's inputs: `.tools` is the
// final request order, `.variables` feeds `render_prompt()`, `.contexts` are
// materialized as durable user-role messages by the loop, and `render()` is the
// system text. `render_prompt()` is pure over a validated assembly (36 §2.7).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/core/cancellation.hpp"
#include "ymh/tools/tool.hpp"

namespace ymh {

// Preset mount scope key (26 part2 §4.9). Wave 3 has one layer only, so
// `AssembleContext.scope` is reserved and always absent.
using ScopeKey = std::string;

struct AssembleContext {
    std::optional<ScopeKey> scope;
    CancellationToken*      signal = nullptr;
};

struct PromptSection {
    std::string name;
    std::int32_t order = 0;
    std::function<std::string(const AssembleContext&)> text;
    bool complete = false;
};

struct PromptContext {
    std::string name;
    std::int32_t order = 0;
    std::function<std::string(const AssembleContext&)> text;
};

struct AssembledSection {
    std::string name;
    std::string text;
    bool        complete = false;
};

struct AssembledContext {
    std::string name;
    std::string text;
};

struct PromptAssembly {
    std::vector<AssembledSection>                     sections;
    std::vector<AssembledContext>                     contexts;
    std::vector<ToolSchema>                           tools;
    std::map<std::string, std::optional<std::string>> variables;
    std::vector<std::string>                          tool_order;
};

class SystemPrompt;

class SectionHandle {
public:
    SectionHandle() = default;
    ~SectionHandle();
    SectionHandle(SectionHandle&& other) noexcept;
    SectionHandle& operator=(SectionHandle&& other) noexcept;
    SectionHandle(const SectionHandle&) = delete;
    SectionHandle& operator=(const SectionHandle&) = delete;

private:
    friend class SystemPrompt;
    SectionHandle(SystemPrompt* owner, std::string name);
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    std::string   name_;
};

class ContextHandle {
public:
    ContextHandle() = default;
    ~ContextHandle();
    ContextHandle(ContextHandle&& other) noexcept;
    ContextHandle& operator=(ContextHandle&& other) noexcept;
    ContextHandle(const ContextHandle&) = delete;
    ContextHandle& operator=(const ContextHandle&) = delete;

private:
    friend class SystemPrompt;
    ContextHandle(SystemPrompt* owner, std::string name);
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    std::string   name_;
};

class VariableHandle {
public:
    VariableHandle() = default;
    ~VariableHandle();
    VariableHandle(VariableHandle&& other) noexcept;
    VariableHandle& operator=(VariableHandle&& other) noexcept;
    VariableHandle(const VariableHandle&) = delete;
    VariableHandle& operator=(const VariableHandle&) = delete;

private:
    friend class SystemPrompt;
    VariableHandle(SystemPrompt* owner, std::string name);
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    std::string   name_;
};

class SystemPrompt {
public:
    explicit SystemPrompt(std::vector<std::string> tool_order = {});
    SystemPrompt(const SystemPrompt&) = delete;
    SystemPrompt& operator=(const SystemPrompt&) = delete;
    SystemPrompt(SystemPrompt&&) = delete;
    SystemPrompt& operator=(SystemPrompt&&) = delete;
    ~SystemPrompt() = default;

    SectionHandle section(PromptSection section);
    ContextHandle context(PromptContext context);
    VariableHandle variable(
        std::string name,
        std::function<std::optional<std::string>(const AssembleContext&)> provider);
    void set_tool_provider(std::function<std::vector<ToolSchema>(const AssembleContext&)> provider);

    [[nodiscard]] PromptAssembly assemble(const AssembleContext& context) const;
    [[nodiscard]] std::string    render(const AssembleContext& context) const;

private:
    friend class SectionHandle;
    friend class ContextHandle;
    friend class VariableHandle;

    void remove_section(const std::string& name) noexcept;
    void remove_context(const std::string& name) noexcept;
    void remove_variable(const std::string& name) noexcept;

    std::map<std::string, PromptSection> sections_;
    std::map<std::string, PromptContext> contexts_;
    std::map<std::string, std::function<std::optional<std::string>(const AssembleContext&)>>
        variables_;
    std::function<std::vector<ToolSchema>(const AssembleContext&)> tool_provider_;
    std::vector<std::string>                                       tool_order_;
};

[[nodiscard]] std::string render_prompt(const PromptAssembly& assembly);

} // namespace ymh

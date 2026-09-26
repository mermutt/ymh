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
#include <set>
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
    // 56-D2: the model-facing tool names visible in this assembly — the
    // post-restriction, post-`tool_order` names of `PromptAssembly::tools`.
    // `assemble()` points this at a local set for the section/context text
    // pass. `nullptr` means "outside `assemble()`": tool-gated text MUST treat
    // every tool as invisible (fail closed). Never retained past the call.
    const std::set<std::string>* visible_tools = nullptr;
};

// 56-D2: the fail-closed membership test.
[[nodiscard]] bool tool_visible(const AssembleContext& context, std::string_view name);

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
    SectionHandle(SystemPrompt* owner, std::string name, ScopeKey scope = {});
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    std::string   name_;
    ScopeKey      scope_;
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
    ContextHandle(SystemPrompt* owner, std::string name, ScopeKey scope = {});
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    std::string   name_;
    ScopeKey      scope_;
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
    VariableHandle(SystemPrompt* owner, std::string name, ScopeKey scope = {});
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    std::string   name_;
    ScopeKey      scope_;
};

// 42 §3.2: the scope-mount rule Wave 3 deferred (36 §2.1, :854-858). A
// `SystemPrompt::scope(parent, key)` call creates one child scope keyed by
// `key` under `parent` and returns this move-only handle. The handle's lifetime
// bounds the scope: on destruction the scope and every descendant are removed,
// unregistering all of their registrations. Registrations made through the
// handle are owned by the scope and persist for the handle's lifetime.
class ScopeHandle {
public:
    ScopeHandle() = default;
    ~ScopeHandle();
    ScopeHandle(ScopeHandle&& other) noexcept;
    ScopeHandle& operator=(ScopeHandle&& other) noexcept;
    ScopeHandle(const ScopeHandle&) = delete;
    ScopeHandle& operator=(const ScopeHandle&) = delete;

    // Registrations in this scope. Duplicate names within one layer throw
    // (ConfigError); scoped names shadow outer layers of the same name (42 §2.2).
    void add_section(PromptSection section);
    void add_context(PromptContext context);
    void add_variable(
        std::string name,
        std::function<std::optional<std::string>(const AssembleContext&)> provider);
    // 42 §2.1/§3.3: a narrowing over the provider's tool list, applied after the
    // outer layers' filters. A scope can only narrow, never widen (26-I8).
    void set_tool_filter(std::function<std::vector<ToolSchema>(std::vector<ToolSchema>)> filter);

    [[nodiscard]] const ScopeKey& key() const noexcept { return key_; }

private:
    friend class SystemPrompt;
    ScopeHandle(SystemPrompt* owner, ScopeKey key);
    void reset() noexcept;

    SystemPrompt* owner_ = nullptr;
    ScopeKey      key_;
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

    // 42 §3.2: register a child scope keyed by `key`, parented to `parent`. An
    // empty `parent` parents the scope to the global layer. A duplicate key or an
    // unknown non-empty parent throws ConfigError (36 §2.1). The scope's
    // registrations are visible only to assemblies whose `ctx.scope` chain passes
    // through it.
    [[nodiscard]] ScopeHandle scope(ScopeKey parent, ScopeKey key);

    // The parent of `key`, or nullopt when `key` is the global layer / unknown.
    // The roster reads it to find a live leaf's standing mount (42-I4).
    [[nodiscard]] std::optional<ScopeKey> scope_parent(const ScopeKey& key) const;

    [[nodiscard]] PromptAssembly assemble(const AssembleContext& context) const;
    [[nodiscard]] std::string    render(const AssembleContext& context) const;

private:
    friend class SectionHandle;
    friend class ContextHandle;
    friend class VariableHandle;
    friend class ScopeHandle;

    // One scope's registrations (42 §2.2). Shadowing is by name across layers;
    // duplicates within one layer throw.
    struct ScopeLayer {
        ScopeKey                                                              parent;
        std::map<std::string, PromptSection>                                  sections;
        std::map<std::string, PromptContext>                                  contexts;
        std::map<std::string, std::function<std::optional<std::string>(const AssembleContext&)>>
            variables;
        std::function<std::vector<ToolSchema>(std::vector<ToolSchema>)>       tool_filter;
    };

    void register_section(const ScopeKey& scope, PromptSection section);
    void register_context(const ScopeKey& scope, PromptContext context);
    void register_variable(
        const ScopeKey& scope, std::string name,
        std::function<std::optional<std::string>(const AssembleContext&)> provider);

    SectionHandle  add_section(const ScopeKey& scope, PromptSection section);
    ContextHandle  add_context(const ScopeKey& scope, PromptContext context);
    VariableHandle add_variable(
        const ScopeKey& scope, std::string name,
        std::function<std::optional<std::string>(const AssembleContext&)> provider);

    void remove_section(const ScopeKey& scope, const std::string& name) noexcept;
    void remove_context(const ScopeKey& scope, const std::string& name) noexcept;
    void remove_variable(const ScopeKey& scope, const std::string& name) noexcept;
    void remove_scope(const ScopeKey& key) noexcept;

    // Global layer first, then each ancestor, then the leaf (42 §2.2).
    [[nodiscard]] std::vector<const ScopeLayer*> scope_chain(
        const std::optional<ScopeKey>& leaf) const;

    std::map<std::string, PromptSection> sections_;
    std::map<std::string, PromptContext> contexts_;
    std::map<std::string, std::function<std::optional<std::string>(const AssembleContext&)>>
        variables_;
    std::map<ScopeKey, ScopeLayer>                                 scopes_;
    std::function<std::vector<ToolSchema>(const AssembleContext&)> tool_provider_;
    std::vector<std::string>                                       tool_order_;
};

[[nodiscard]] std::string render_prompt(const PromptAssembly& assembly);

} // namespace ymh

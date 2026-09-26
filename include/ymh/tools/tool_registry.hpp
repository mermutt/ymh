#pragma once

// ToolRegistry (07 §4). Registration is confined to plugin load / daemon
// startup; after `freeze()` lookup is stable and lock-free (X5). `execute` is
// the single validated dispatch path: unknown name or invalid arguments yield an
// Error result, never a silent drop and never an unvalidated execution (X6).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ymh/execution/config.hpp"
#include "ymh/session/events.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {

class ToolRegistry {
public:
    class Registration {
    public:
        Registration() noexcept = default;
        Registration(Registration&& other) noexcept;
        Registration& operator=(Registration&& other) noexcept;
        ~Registration();

        [[nodiscard]] bool held() const noexcept { return owner_ != nullptr; }

    private:
        friend class ToolRegistry;
        Registration(ToolRegistry* owner, std::string name) noexcept;

        ToolRegistry* owner_ = nullptr;
        std::string   name_;
    };

    // Additive (15 §4.8, AM-0a). A namespaced lease held by a tool PRODUCER
    // (the MCP adapter). Acquired BEFORE freeze(); afterwards the scope's
    // membership may be atomically replaced, which bumps generation() and
    // invalidates the cached schema snapshot (07 E-F15). Ordinary add/remove
    // stay frozen (X5).
    class AdapterScope {
    public:
        AdapterScope() noexcept = default;
        AdapterScope(AdapterScope&& other) noexcept;
        AdapterScope& operator=(AdapterScope&& other) noexcept;
        ~AdapterScope();

        [[nodiscard]] bool held() const noexcept { return owner_ != nullptr; }

        // Atomic: on success the scope holds exactly `tools`, all names share
        // the scope prefix, no name shadows a non-adapter tool, and every
        // schema is valid (07 §3.2). On any violation the previous set is
        // retained (strong guarantee) and ToolRegistryError is thrown.
        std::size_t replace(std::vector<std::unique_ptr<Tool>> tools);

        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::string_view prefix() const noexcept { return prefix_; }

    private:
        friend class ToolRegistry;
        AdapterScope(ToolRegistry* owner, std::string prefix) noexcept;

        ToolRegistry* owner_ = nullptr;
        std::string   prefix_;
    };

    ToolRegistry() = default;
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    [[nodiscard]] Registration add(std::unique_ptr<Tool> tool);

    void freeze();
    [[nodiscard]] bool frozen() const noexcept { return frozen_.load(); }

    [[nodiscard]] std::size_t   size() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_.load(); }

    [[nodiscard]] Tool* find(const ToolName& name) const noexcept;
    [[nodiscard]] bool  contains(const ToolName& name) const noexcept;
    [[nodiscard]] std::vector<ToolName> names() const;
    [[nodiscard]] std::vector<ToolSchema> schemas() const;

    Task<ToolResult> execute(const payload::ToolCall& call,
                             const ToolContext& context);

    [[nodiscard]] const ToolConfig& config() const noexcept { return config_; }
    void set_config(ToolConfig config) { config_ = config; }

    // One scope per adapter namespace. `prefix` must be a valid ToolName prefix
    // ending in '.', unique among scopes, and disjoint from non-adapter tool
    // names. Callable only before freeze(); after freeze() it throws
    // RegistryFrozen. Names under a scope do NOT participate in the ordinary
    // DuplicateName/DuplicateVersion rules; the scope owns them exclusively.
    [[nodiscard]] AdapterScope openAdapter(std::string_view prefix);

private:
    friend class AdapterScope;

    void release_registration(const std::string& name);
    void release_scope(const std::string& prefix);
    std::size_t replace_scope(const std::string& prefix,
                              std::vector<std::unique_ptr<Tool>> tools);
    std::shared_ptr<Tool> lookup(const std::string& name) const;
    bool name_owned_by_other_scope(const std::string& name,
                                   const std::string& prefix) const;

    mutable std::mutex                                        mutex_;
    std::unordered_map<std::string, std::shared_ptr<Tool>>    tools_;
    std::unordered_map<std::string, std::vector<std::string>> adapter_names_;
    std::vector<std::string>                                  adapter_order_;
    std::atomic<std::uint64_t>                                generation_{0};
    std::atomic<bool>                                         frozen_{false};
    ToolConfig                                                config_;
};

} // namespace ymh

#pragma once

// ToolRegistry (07 §4). Registration is confined to plugin load / daemon
// startup; after `freeze()` lookup is stable and lock-free (X5). `execute` is
// the single validated dispatch path: unknown name or invalid arguments yield an
// Error result, never a silent drop and never an unvalidated execution (X6).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

    ToolRegistry() = default;
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    [[nodiscard]] Registration add(std::unique_ptr<Tool> tool);

    void freeze();
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] std::size_t   size() const noexcept { return tools_.size(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] Tool* find(const ToolName& name) const noexcept;
    [[nodiscard]] bool  contains(const ToolName& name) const noexcept;
    [[nodiscard]] std::vector<ToolName> names() const;
    [[nodiscard]] std::vector<ToolSchema> schemas() const;

    Task<ToolResult> execute(const payload::ToolCall& call,
                             const ToolContext& context);

    [[nodiscard]] const ToolConfig& config() const noexcept { return config_; }
    void set_config(ToolConfig config) { config_ = config; }

private:
    void release_registration(const std::string& name);

    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
    std::uint64_t generation_ = 0;
    bool          frozen_ = false;
    ToolConfig    config_;
};

} // namespace ymh

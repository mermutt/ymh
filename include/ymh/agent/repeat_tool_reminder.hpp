#pragma once

// The Wave-4 repeat-tool reminder (40-output-retention.md §6; 26-D21). It
// observes committed tool calls in model order and returns an append-only,
// plugin-sourced `ContextMessage` when a consecutive-identical-call threshold
// is crossed. One instance per `AgentLoop` (executor-thread only).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/llm/stream.hpp"

namespace ymh {

struct RepeatToolReminderConfig {
    std::vector<std::size_t> thresholds = {3, 5, 8};
    std::size_t              arguments_preview_chars = 500;
};

class RepeatToolReminder {
public:
    explicit RepeatToolReminder(RepeatToolReminderConfig config = {});

    [[nodiscard]] std::optional<ContextMessage> observe(const ToolCallAssembled& committed);

private:
    RepeatToolReminderConfig config_;
    std::string              name_;
    std::string              canonical_args_;
    std::size_t              count_ = 0;
    std::vector<std::size_t> fired_;
};

} // namespace ymh

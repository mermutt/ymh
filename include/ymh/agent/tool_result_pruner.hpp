#pragma once

// The Wave-4 tool-result pruner (40-output-retention.md §4;
// 32-compaction-errata.md §5). Replay-safe and model-free: it never deletes; it
// appends a `context/prune` shadow-price event immediately followed by a
// same-`id` replacement `ToolResult`. Thresholds are Unicode code points, not
// bytes.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

struct ToolResultPruneConfig {
    std::size_t threshold_code_points = 8192;
    std::size_t head_code_points      = 4096;
    std::size_t tail_code_points      = 1024;
};

inline constexpr std::string_view kPruneMarker =
    "\n\n[... tool result middle pruned ...]\n\n";

struct PruneResult {
    std::size_t           pruned = 0;
    std::vector<Sequence> replacements;
};

class ToolResultPruner {
public:
    explicit ToolResultPruner(ToolResultPruneConfig config = {}) : config_(config) {}

    [[nodiscard]] PruneResult prune_session(Session& session);

    [[nodiscard]] const ToolResultPruneConfig& config() const noexcept { return config_; }

private:
    ToolResultPruneConfig config_;
};

} // namespace ymh

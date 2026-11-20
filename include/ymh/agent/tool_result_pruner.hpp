#pragma once

// The Wave-4 tool-result pruner (40-output-retention.md §4;
// 32-compaction-errata.md §5). Replay-safe and model-free: it never deletes; it
// appends a `context/prune` shadow-price event immediately followed by a
// same-`id` replacement `ToolResult`. Thresholds are Unicode code points, not
// bytes.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

// Default retention policy. The trigger matches Claude Code's Bash inline
// window and sits well above Codex CLI's ~40 KB / 10K-token budget; the old
// 8,192-code-point threshold quietly clamped ordinary multi-item MCP payloads
// to ~1.3K tokens. The head and tail are equal because the tail carries test
// summaries, failure counts and compiler totals.
inline constexpr std::size_t kToolResultPruneThresholdCodePoints = 30000;
inline constexpr std::size_t kToolResultPruneHeadCodePoints      = 15000;
inline constexpr std::size_t kToolResultPruneTailCodePoints      = 15000;

static_assert(kToolResultPruneHeadCodePoints + kToolResultPruneTailCodePoints >=
                  kToolResultPruneThresholdCodePoints,
              "a pruned result must retain at least the trigger threshold");
static_assert(kToolResultPruneThresholdCodePoints > kToolResultPruneHeadCodePoints,
              "the head must leave room for an elided middle");
static_assert(kToolResultPruneThresholdCodePoints > kToolResultPruneTailCodePoints,
              "the tail must leave room for an elided middle");

struct ToolResultPruneConfig {
    std::size_t threshold_code_points = kToolResultPruneThresholdCodePoints;
    std::size_t head_code_points      = kToolResultPruneHeadCodePoints;
    std::size_t tail_code_points      = kToolResultPruneTailCodePoints;

    // head + tail >= threshold, with head and tail individually below the
    // threshold so a pruned result still elides a middle. The sum is written
    // as a difference so absurd values cannot overflow.
    [[nodiscard]] constexpr bool is_consistent() const noexcept {
        return threshold_code_points > head_code_points &&
               threshold_code_points > tail_code_points &&
               tail_code_points >= threshold_code_points - head_code_points;
    }
};

inline constexpr std::string_view kPruneMarker =
    "\n\n[... tool result middle pruned ...]\n\n";

// TODO(output-retention): name a recovery location in the marker once one is
// reachable. opencode and Claude Code hand the model a file path; here the full
// output survives in `events.jsonl`, but the pruner has no durable path or
// sequence accessor, so a marker path would be fabricated. Follow-up: thread
// the store path and the shadowed sequence into the pruner and emit
// `[full output: <path>]` alongside the existing marker text.

struct PruneResult {
    std::size_t           pruned = 0;
    std::vector<Sequence> replacements;
};

class ToolResultPruner {
public:
    explicit ToolResultPruner(ToolResultPruneConfig config = {}) : config_(config) {
        if (!config_.is_consistent()) {
            throw std::invalid_argument(
                "ToolResultPruneConfig requires threshold > head, threshold > tail, "
                "and head + tail >= threshold");
        }
    }

    [[nodiscard]] PruneResult prune_session(Session& session);

    [[nodiscard]] const ToolResultPruneConfig& config() const noexcept { return config_; }

private:
    ToolResultPruneConfig config_;
};

} // namespace ymh

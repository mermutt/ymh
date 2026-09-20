#pragma once

// Context compaction, pinned by 13-context-compaction.md. Compaction is a
// projection over the append-only session log, never a deletion: it emits a
// `payload::ContextCompaction` event and changes only how the next request is
// assembled (`deriveMessages()`, 01 §6.3). The concrete `ContextCompactor`
// implements the frozen `Compactor` seam (06 §5.3).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

// The compaction tunables (13 §3.2). The daemon builds one from the layered
// config (§37) and injects it into the `ContextCompactor`; nothing reads
// global state.
struct CompactionPolicy {
    // 32 §2.1 / 28 §3.5: the summarizer's route id, from config.llm.provider.
    // Empty resolves to the runtime's registered default route.
    ProviderId  provider;
    bool        enabled = false;
    std::size_t threshold_tokens = 0;
    double      threshold_ratio = 0.80;
    std::size_t context_window_tokens = 0;
    std::size_t reserve_output_tokens = 4'096;
    std::size_t keep_recent_turns = 2;
    std::size_t min_prefix_messages = 4;
    std::size_t max_summary_tokens = 1'024;
    std::size_t max_summary_bytes = 256u * 1024u;
    std::string summarizer_model;
    std::size_t max_compactions_per_turn = 1;
    bool        retry_on_context_length = true;

    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled && effective_threshold_tokens() > 0;
    }

    [[nodiscard]] std::size_t effective_threshold_tokens() const noexcept {
        if (threshold_tokens > 0) {
            return threshold_tokens;
        }
        if (context_window_tokens > reserve_output_tokens) {
            const auto window = context_window_tokens - reserve_output_tokens;
            return static_cast<std::size_t>(threshold_ratio * static_cast<double>(window));
        }
        return 0;
    }
};

// The compaction trigger taxonomy (32-compaction-errata.md §4.1; 26-D13). The
// manual `/compact` path is `compact_now`, not a third value (C24).
enum class CompactionTrigger : std::uint8_t {
    Pressure,
    ContextOverflow,
};

// The result of a compaction attempt (13 §5.3). Not durable.
enum class CompactionOutcome : std::uint8_t {
    Compacted,
    NotNeeded,
    Cancelled,
    Failed,
    Queued,
};

struct CompactionError {
    enum class Code : std::uint8_t {
        None,
        Disabled,
        NoBoundary,
        PrefixTooSmall,
        SummarizerFailed,
        SummarizerOverflow,
        OversizedSummary,
        NoProviderRoute,
        Cancelled,
        LeaseLost,
        StoreUnavailable,
        Internal,
    };

    Code        code = Code::None;
    std::string detail;
};

struct CompactionResult {
    CompactionOutcome                         outcome = CompactionOutcome::NotNeeded;
    std::optional<payload::ContextCompaction> compaction;
    std::optional<Usage>                      usage;
    CompactionError                           error;
};

// The deterministic boundary selection (13 §3.3, §5.4).
struct CompactionPlan {
    Sequence    boundary = 0;
    std::size_t prefix_messages = 0;
    std::size_t kept_messages = 0;
    bool        valid = false;
};

// Injectable WALL clock for `ContextCompaction.createdAt` (13 §2.3, §5.2). Not
// the broker's `steady_clock`-based `ClockReader`: the payload stores a
// `system_clock::time_point` and a steady clock has no wall-clock epoch.
using WallClock = std::function<std::chrono::system_clock::time_point()>;

// One instance per workspace daemon, shared by every agent. Stateless across
// turns and sessions: the session is passed per call and usage is returned,
// never appended, so a shared instance cannot misattribute (13 §5.2).
class ContextCompactor final : public Compactor {
public:
    ContextCompactor(LlmRuntime&           runtime,
                     LLMPool&              pool,
                     const TokenEstimator& estimator,
                     CompactionPolicy      policy,
                     WallClock             clock = std::chrono::system_clock::now);

    std::optional<payload::ContextCompaction>
    run(const Session&, const std::vector<Message>&, CancellationToken) override;

    CompactionResult compact(const Session&, const std::vector<Message>&, CancellationToken);

    // 32 §4.2: the two D13 entry points. `compact_if_needed` is the
    // proactive/overflow path (nullopt when no compaction is warranted);
    // `compact_now` always attempts and is the manual `/compact` path.
    Task<std::optional<CompactionResult>> compact_if_needed(CompactionTrigger trigger,
                                                            const Session&,
                                                            const std::vector<Message>&,
                                                            CancellationToken);
    Task<CompactionResult> compact_now(const Session&,
                                       const std::vector<Message>&,
                                       CancellationToken);

    [[nodiscard]] CompactionPlan plan(const Session&,
                                      const std::vector<Message>&) const;

    [[nodiscard]] const CompactionPolicy& policy() const noexcept { return policy_; }

private:
    struct PlanResult {
        CompactionPlan        plan;
        CompactionError::Code reason = CompactionError::Code::None;
    };

    [[nodiscard]] PlanResult select_plan(const Session&, std::size_t keep) const;
    [[nodiscard]] std::vector<Message> build_summary_prompt(
        const std::vector<Message>& prefix) const;
    [[nodiscard]] std::string bound_summary(const std::string& summary) const;

    LlmRuntime&           runtime_;
    LLMPool&              pool_;
    const TokenEstimator& estimator_;
    CompactionPolicy      policy_;
    WallClock             clock_;
};

} // namespace ymh

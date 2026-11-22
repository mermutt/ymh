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

class ModelCatalog;

// 75-context-budget-errata.md: the window assumed when neither the resolved
// model nor [agent.compaction] names one, and the bounds a wired policy is
// clamped to. kDefaultContextWindowTokens is deliberately the smallest context
// class ymh targets for code, so a wrong guess compacts early rather than
// truncating the request head.
inline constexpr std::size_t kDefaultContextWindowTokens = 32'768;
inline constexpr std::size_t kMinContextWindowTokens = 4'096;
inline constexpr std::size_t kMinEffectiveThresholdTokens = 1;

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

    // 75-D5: the wired-policy invariant as a predicate. A zero window is
    // "unknown" and vacuously consistent; a positive window must exceed the
    // reserve and carry a positive threshold strictly below the window.
    [[nodiscard]] bool context_budget_consistent() const noexcept {
        return context_window_tokens == 0 ||
               (context_window_tokens > reserve_output_tokens &&
                effective_threshold_tokens() > 0 &&
                effective_threshold_tokens() < context_window_tokens);
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
    std::size_t kept_turns = 0;  // completed turns preserved (<= configured keep)
    bool        valid = false;
};

// The boundary chosen for a given policy width. `kept_turns` is the effective
// keep, which can be lower than the configured `keep` when `allow_relaxed` is
// set and the log has fewer completed turns than the policy wants to preserve.
struct CompactionBoundary {
    Sequence    boundary = 0;
    std::size_t kept_turns = 0;
    bool        valid = false;
};

// Pure boundary selection over the resolved log. Nominally this picks the
// (keep+1)-th most recent turn-terminal event. With `allow_relaxed`, a log that
// has not yet completed `keep` turns is compacted with a smaller keep (down to
// 0) instead of refusing: an oversized early turn must not make the context
// impossible to compact. The candidate always advances past the last
// `ContextCompaction` boundary.
[[nodiscard]] CompactionBoundary select_compaction_boundary(
    const EventRange& events, std::size_t keep, bool allow_relaxed = false);

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
                     WallClock             clock = std::chrono::system_clock::now,
                     const ModelCatalog*   catalog = nullptr);

    std::optional<payload::ContextCompaction>
    run(const Session&, const std::vector<Message>&, CancellationToken) override;

    CompactionResult compact(const Session&, const std::vector<Message>&, CancellationToken,
                             bool allow_relaxed = false);

    // 32 §4.2: the two D13 entry points. `compact_if_needed` is the
    // proactive/overflow path (nullopt when no compaction is warranted);
    // `request_tokens` is the caller's full request estimate (messages + tool
    // schemas), with 0 meaning "messages only". It may relax
    // `keep_recent_turns` to find a boundary. `compact_now` always attempts and
    // is the manual `/compact` path.
    Task<std::optional<CompactionResult>> compact_if_needed(CompactionTrigger trigger,
                                                            const Session&,
                                                            const std::vector<Message>&,
                                                            CancellationToken,
                                                            std::size_t request_tokens = 0);
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

    [[nodiscard]] PlanResult select_plan(const Session&, std::size_t keep,
                                         bool allow_relaxed = false) const;
    [[nodiscard]] std::vector<Message> build_summary_prompt(
        const std::vector<Message>& prefix) const;
    [[nodiscard]] std::string bound_summary(const std::string& summary) const;

    LlmRuntime&           runtime_;
    LLMPool&              pool_;
    const TokenEstimator& estimator_;
    CompactionPolicy      policy_;
    WallClock             clock_;
    const ModelCatalog*   catalog_ = nullptr;
};

} // namespace ymh

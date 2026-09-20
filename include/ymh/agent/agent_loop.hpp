#pragma once

// The concrete turn/step driver, pinned by 06-agent-loop.md §5. It implements
// the `Agent` handle surface and runs the daemon-driven loop: assemble context,
// call `LLMProvider::stream` under one `LLMPool` slot, publish deltas as
// live-only `AssistantChunk`s, drive tools through the permission policy, and
// close every turn with exactly one terminal event (A2). No UI type appears
// here (A17).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/chunk_coalescer.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/plan_mode_controller.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {

class SessionManager;
class ResourceGovernor;
class ToolRegistry;
class ExecutionEnvironment;
class Logger;
class OutputSink;
class SystemPrompt;
class InstructionLoader;
struct PromptAssembly;

struct AgentServices {
    using PermissionResolver =
        std::function<PermissionOutcome(const PermissionRequest&, CancellationToken)>;

    SessionManager*       sessions = nullptr;
    ResourceGovernor*     governor = nullptr;
    ToolRegistry*         tools = nullptr;
    PermissionPolicy*     policy = nullptr;
    PermissionGate*       gate = nullptr;
    // 26-D1 / 31-D1: the provider-neutral service. Replaces `providers`,
    // `provider`, and `provider_config`; no `LLMProvider*` remains here.
    LlmRuntime*           runtime = nullptr;
    ContextAssembler*     context = nullptr;
    // 36 §2.7: the prompt registry. When set, the loop calls `assemble()` and
    // materializes `.contexts` before the request messages are derived.
    SystemPrompt*         prompt = nullptr;
    // 36 §2.4: the workspace-instruction loader. When set, the loop appends the
    // rendered `<system-reminder>` message once at the first request.
    InstructionLoader*    instructions = nullptr;
    ExecutionEnvironment* execution = nullptr;
    Logger*               logger = nullptr;
    OutputSink*           output = nullptr;
    Compactor*            compactor = nullptr;
    // The rich compactor the loop calls for `compact()`/`CompactionResult`
    // (13-context-compaction.md §5.2, errata A3); the frozen `compactor` seam
    // above remains for the legacy path.
    ContextCompactor*     context_compactor = nullptr;
    TokenEstimator*       estimator = nullptr;
    LLMPool*              pool = nullptr;
    PermissionResolver    permission_resolver;
    // 25-D2: null => plan mode is unavailable; `exit_plan_mode` fails closed.
    PlanModeController*   plan_mode = nullptr;

    // 34-D4: monotonic source for `TimedStreamEvent.at`. Production default is
    // steady_clock::now; tests inject a deterministic reader.
    using StreamClock       = std::chrono::steady_clock;
    using StreamClockReader = std::function<StreamClock::time_point()>;
    // Immutable after construction: written once when the loop is built, read on
    // the owning executor thread by the sink. `const` enforces the read-only
    // intent (34 §5.2).
    const StreamClockReader stream_clock = std::chrono::steady_clock::now;
};

class AgentLoop final : public Agent {
public:
    // 24-D1: the loop co-owns its session through `session_owner_` for its whole
    // life, so the session cannot be freed while a turn body reaches it via
    // `session_` (AL1).
    AgentLoop(AgentId id, std::shared_ptr<Session> session, AgentServices services,
              AgentConfig config);
    ~AgentLoop() override;

    AgentLoop(const AgentLoop&) = delete;
    AgentLoop& operator=(const AgentLoop&) = delete;

    AgentId     id() const override;
    SessionId   session() const override;
    AgentStatus status() const noexcept override;
    AgentState  state() const noexcept override;
    bool        disposed() const noexcept override;
    bool        hasPendingWork() const noexcept override;

    InboxResult send(Message message) override;
    InboxResult followup(Message message) override;
    InboxResult steer(Message message) override;
    InboxResult inject(ContextMessage context) override;

    void cancel() override;
    void dispose() override;
    void whenIdle(std::function<void()> callback) override;

    // Additive enqueue target for the manual `/compact` path
    // (13-context-compaction.md §6.8, errata A5). Enqueue-only, executor-thread
    // confined: returns `Queued` once the `Compact` item is accepted, or the
    // typed rejection. Never appends inline.
    std::expected<CompactionOutcome, AgentError> requestCompaction();

    Task<void> run(CancellationToken sessionCancel);
    void       activate();
    void       suspend();

    [[nodiscard]] const AgentConfig& config() const noexcept { return config_; }

private:
    enum class InboxKind : std::uint8_t {
        Send,
        FollowUp,
        Steer,
        Inject,
        Compact,
    };

    struct InboxItem {
        InboxKind           kind = InboxKind::Send;
        payload::TurnOrigin origin = payload::TurnOrigin::User;
        Message             message;
        ContextMessage      context;
    };

    [[nodiscard]] bool        turnInFlight() const noexcept;
    [[nodiscard]] bool        hasTurnTrigger() const noexcept;
    // 24-D7/AL15: caller must hold `control_mutex_`.
    [[nodiscard]] bool        hasTurnTriggerLocked() const noexcept;
    InboxResult               enqueue(InboxItem item);
    void                      runTurn();
    void                      runMaintenanceTurn(TurnId turn);
    void                      drainFoldedItems();
    void                      appendUserMessage(const Message& message);
    void                      appendContextInjected(const ContextMessage& context);
    void                      materializeContexts(const PromptAssembly& assembly);
    void                      materializeInstructions();
    void                      appendTurnFailed(TurnId turn, AgentErrorCode code, std::string message);
    CompactionOutcome         runCompaction(const std::vector<Message>& messages, TurnId turn);
    [[nodiscard]] FrozenRequest buildRequest(const std::vector<Message>& messages,
                                             TurnId turn, StepId step);
    bool                      executeToolCall(const ToolCallAssembled& call, TurnId turn, StepId step);
    void                      flushIdleCallbacks();

    AgentId                  id_;
    std::shared_ptr<Session> session_owner_;  // declared before session_ (AL1)
    Session&                 session_;
    AgentServices            services_;
    AgentConfig              config_;

    std::deque<InboxItem>               inbox_;              // control_mutex_
    std::vector<std::function<void()>>  idle_callbacks_;     // control_mutex_
    std::string                         cancel_reason_;      // control_mutex_
    bool                                pending_maintenance_failure_ = false;  // control_mutex_
    mutable std::mutex                  control_mutex_;      // leaf: no blocking op inside
    std::atomic<bool>                   disposed_{false};
    std::atomic<bool>                   running_{false};
    std::atomic<AgentState>             state_{AgentState::Idle};
    std::size_t                         compactions_this_turn_ = 0;  // worker-only
    std::optional<LlmCallConfig>        held_config_;               // worker-only
    std::optional<CallPurpose>          held_purpose_;              // worker-only
    std::optional<std::string>          held_prompt_digest_;        // worker-only
    std::optional<std::vector<std::string>> held_tool_digests_;     // worker-only
    bool                                instructions_loaded_ = false;  // worker-only
    CancellationSource                  turn_cancel_;
};

} // namespace ymh

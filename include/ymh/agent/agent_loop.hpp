#pragma once

// The concrete turn/step driver, pinned by 06-agent-loop.md §5. It implements
// the `Agent` handle surface and runs the daemon-driven loop: assemble context,
// call `LLMProvider::stream` under one `LLMPool` slot, coalesce deltas into
// durable `AssistantChunk`s, drive tools through the permission policy, and
// close every turn with exactly one terminal event (A2). No UI type appears
// here (A17).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/chunk_coalescer.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {

class SessionManager;
class ResourceGovernor;
class ToolRegistry;
class ExecutionEnvironment;
class Logger;
class OutputSink;
class LLMProvider;

struct AgentServices {
    using PermissionResolver =
        std::function<PermissionOutcome(const PermissionRequest&, CancellationToken)>;

    SessionManager*       sessions = nullptr;
    ResourceGovernor*     governor = nullptr;
    ToolRegistry*         tools = nullptr;
    PermissionPolicy*     policy = nullptr;
    PermissionGate*       gate = nullptr;
    ProviderRegistry*     providers = nullptr;
    ContextAssembler*     context = nullptr;
    ExecutionEnvironment* execution = nullptr;
    Logger*               logger = nullptr;
    OutputSink*           output = nullptr;
    Compactor*            compactor = nullptr;
    TokenEstimator*       estimator = nullptr;
    LLMProvider*          provider = nullptr;
    LLMProviderConfig     provider_config;
    LLMPool*              pool = nullptr;
    PermissionResolver    permission_resolver;
};

class AgentLoop final : public Agent {
public:
    AgentLoop(AgentId id, Session& session, AgentServices services, AgentConfig config);
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
    };

    struct InboxItem {
        InboxKind           kind = InboxKind::Send;
        payload::TurnOrigin origin = payload::TurnOrigin::User;
        Message             message;
        ContextMessage      context;
    };

    [[nodiscard]] bool        turnInFlight() const noexcept;
    [[nodiscard]] bool        hasTurnTrigger() const noexcept;
    InboxResult               enqueue(InboxItem item);
    void                      runTurn();
    void                      drainFoldedItems();
    void                      appendUserMessage(const Message& message);
    void                      appendContextInjected(const ContextMessage& context);
    void                      appendTurnFailed(TurnId turn, AgentErrorCode code, std::string message);
    void                      runCompaction(const std::vector<Message>& messages);
    [[nodiscard]] LLMRequest  buildRequest(const std::vector<Message>& messages) const;
    bool                      executeToolCall(const ToolCallAssembled& call, TurnId turn, StepId step);
    void                      flushIdleCallbacks();

    AgentId        id_;
    Session&       session_;
    AgentServices  services_;
    AgentConfig    config_;

    std::deque<InboxItem>                inbox_;
    AgentState                           state_ = AgentState::Idle;
    bool                                 disposed_ = false;
    bool                                 running_ = false;
    CancellationSource                   turn_cancel_;
    std::string                          cancel_reason_;
    std::vector<std::function<void()>>   idle_callbacks_;
};

} // namespace ymh

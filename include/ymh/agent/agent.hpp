#pragma once

// The `Agent` handle and its value types, pinned by docs/design/06-agent-loop.md
// §2-§3. The handle is the daemon-facing surface: identity, coarse status,
// fine-grained state, the bounded inbox (`send`/`followup`/`steer`/`inject`),
// cancellation, `dispose()`, and `whenIdle()`. It carries no UI type (A17).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/stream.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

struct AgentId {
    std::string value;

    auto operator<=>(const AgentId&) const = default;
};

enum class AgentStatus : std::uint8_t {
    Idle,
    Running,
};

enum class AgentState : std::uint8_t {
    Idle,
    Thinking,
    CallingTool,
    WaitingForPermission,
    WaitingForInput,
    Cancelling,
    Error,
};

// 11 §12.4: a turn may be activated only when the agent is Idle or blocked
// awaiting a permission/input reply. Thinking/CallingTool/Cancelling/Error are
// never interrupted by activation; only an explicit cancel/suspend cancels a
// turn.
[[nodiscard]] constexpr bool activationAllowed(AgentState state) noexcept {
    return state == AgentState::Idle || state == AgentState::WaitingForPermission ||
           state == AgentState::WaitingForInput;
}

enum class InboxResult : std::uint8_t {
    Accepted,
    InboxFull,
    AgentDisposed,
};

enum class AgentErrorCode : std::uint8_t {
    None,
    UnknownSession,
    LeaseHeldByOther,
    LeaseLost,
    StoreUnavailable,
    InboxFull,
    AgentDisposed,
    StepLimitExceeded,
    ContextAssemblyFailed,
    CompactionFailed,
    ProviderFailed,
    Cancelled,
    Internal,
};

struct AgentError {
    AgentErrorCode code = AgentErrorCode::None;
    std::string    detail;
};

[[nodiscard]] std::string_view agent_error_code_name(AgentErrorCode code) noexcept;

// 06 §2.2 / §5.7: every provider code except `Cancelled` maps to
// `ProviderFailed`; `ContextLengthExceeded` maps to `CompactionFailed` (it is
// only surfaced after the one-shot compaction retry).
[[nodiscard]] AgentErrorCode mapAgentError(LLMErrorCode code) noexcept;

struct ContextMessage {
    Role          role = Role::System;
    std::string   text;
    bool          startsTurn = false;
    MessageSource source = message_source(MessageSource::Kind::Plugin);
    ContextFormed context{};
};

// 40-output-retention.md §2.3 (26-D10). The scheduler's bound and its committed
// outcome; `results` is always model-ordered.
struct ToolScheduleConfig {
    std::size_t max_parallel_tool_calls = 10;  // dsh DEFAULT_MAX_PARALLEL_TOOL_CALLS
};

struct ToolScheduleOutcome {
    std::vector<payload::ToolResult> results;
    bool                             aborted = false;
};

// Accepts a context contribution produced by a tool; returns false to reject
// (full/duplicate). Keeps the scheduler free of ContextAssembler details.
using ContextAcceptor = std::function<bool(const ContextMessage&)>;

struct AgentConfig {
    // 28-D9 / 31-D7: the provider id source for `LlmCallConfig::provider`,
    // populated from `config.llm.provider`. Empty => the runtime default route.
    ProviderId               provider;
    ModelId                  model;
    GenerationParameters     parameters;
    std::size_t              max_steps = 100;
    std::size_t              max_inbox = 64;
    std::size_t              max_chunk_batch = 32;
    std::chrono::milliseconds chunk_flush_interval{100};
    std::size_t              compaction_threshold_tokens = 0;
    SandboxMode              sandbox = SandboxMode::Workspace;
    std::string              system_prompt;
    std::string              plan_section;
    bool                     persist_prompt_text = false;
    ToolScheduleConfig       schedule;  // 40 §2.3
};

class Agent {
public:
    virtual ~Agent() = default;

    virtual AgentId     id() const = 0;
    virtual SessionId   session() const = 0;

    virtual AgentStatus status() const noexcept = 0;
    virtual AgentState  state() const noexcept = 0;
    virtual bool        disposed() const noexcept = 0;
    virtual bool        hasPendingWork() const noexcept = 0;

    virtual InboxResult send(Message) = 0;
    virtual InboxResult followup(Message) = 0;
    virtual InboxResult steer(Message) = 0;
    virtual InboxResult inject(ContextMessage) = 0;

    virtual void cancel() = 0;
    virtual void dispose() = 0;
    virtual void whenIdle(std::function<void()>) = 0;
};

} // namespace ymh

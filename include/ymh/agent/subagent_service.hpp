#pragma once

// 55-D13: `SubagentService` — the ymh `SubagentRuntime`. It owns the two
// delegation modes (one-shot/continuable), the durable settlement notice and
// its replay rule, and the `send_message`/`interrupt_agent`/`list_agents`
// backends. The model-facing tools are thin adapters over it.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/model_selection.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/agent/subagent_types.hpp"
#include "ymh/core/task.hpp"
#include "ymh/jobs/job_registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

class AgentRegistry;
class SessionManager;
class SessionActivator;
class RouteCatalog;
class LlmRuntime;
class JobWakeupPolicy;
class EventBus;

enum class ListScope : std::uint8_t { Children, Descendants };

struct ChildDescriptor {
    SessionId                  child;
    std::optional<SessionId>   parent;  // durable direct parent
    std::uint32_t              depth = 0;
    std::string                label;   // session title
    enum class Status : std::uint8_t { Running, Idle, Ready };
    Status                     status = Status::Ready;
    std::optional<std::string> diagnostic;
};

struct SendResult {
    MessageId                  message_id;  // acceptance only
    std::optional<AgentError>  error;       // set iff the message was not delivered
};

struct SettlementResult {
    payload::SubagentOutcome   outcome = payload::SubagentOutcome::Completed;
    std::optional<std::string> disposal_error;  // never masks `outcome`
};

struct StartResult {
    enum class Kind : std::uint8_t { Foreground, BackgroundJob, Continuable };
    Kind                       kind = Kind::Foreground;
    std::string                text;    // Kind::Foreground
    std::string                job_id;  // Kind::BackgroundJob
    SessionId                  child;   // Kind::Continuable
    std::optional<AgentError>  error;   // set iff the start failed (55-D9)
};

class SubagentService {
public:
    struct StartRequest {
        SessionId                        parent;  // the calling session
        std::string                      label;   // = args.description
        std::string                      prompt;
        std::optional<ChildAgentOptions> agent_options;  // per-call route override
        std::optional<ChildComposition>  composition;    // persona / tool_filter
        bool                             run_in_background = false;
    };

    SubagentService(AgentRegistry& registry, SessionManager& sessions, AgentPresetRoster* presets,
                    JobRegistry& jobs, JobWakeupPolicy& wakeup,
                    ModelSelectionController& model_selection, RouteCatalog& route_catalog,
                    LlmRuntime& llm, SessionActivator& activator, EventBus& bus);
    ~SubagentService();

    SubagentService(const SubagentService&) = delete;
    SubagentService& operator=(const SubagentService&) = delete;

    Task<StartResult> startOneShot(StartRequest request);
    Task<StartResult> startContinuable(StartRequest request);

    Task<SendResult> sendMessage(const SessionId& sender, const SessionId& target,
                                 const std::string& message);
    Task<void>       interrupt(const SessionId& sender, const SessionId& target);
    std::vector<ChildDescriptor> list(const SessionId& caller, ListScope scope) const;

    // Appends the parent-log `SubagentFanIn`, resolves the activation job, and
    // returns the combined result+disposal outcome (never `void`). `notice_expected`
    // is true iff the epoch was background-registered (55-D4).
    std::expected<SettlementResult, AgentError> settle(const SessionId& parent,
                                                       const SessionId& child,
                                                       payload::SubagentOutcome outcome,
                                                       std::string summary, bool notice_expected);

    // 55-D4: deliver one notice per unreported background SubagentFanIn; the
    // plugin is reconstructed as `subagent-settlement:<child>#<ordinal>`.
    void replayUnreportedSettlements(const SessionId& parent);

    // The max_depth source is `presets.max_depth` (42-D18); exposed for the tools.
    [[nodiscard]] std::uint32_t max_depth() const noexcept;

    // 55-D11: the live `SessionActivator` calls this on its executor worker; it
    // delivers the stored background prompt and runs the child activation.
    [[nodiscard]] bool activateChild(const SessionId& child);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ymh

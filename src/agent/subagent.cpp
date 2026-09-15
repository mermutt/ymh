#include "ymh/agent/subagent.hpp"

#include <utility>

#include "ymh/agent/message.hpp"

namespace ymh {

SubagentRunner::SubagentRunner(AgentRegistry& registry,
                               SessionManager& sessions,
                               Session& parent,
                               SessionOptions options)
    : registry_(registry), sessions_(sessions), parent_(parent), options_(std::move(options)) {}

payload::SubagentOutcome SubagentRunner::run(const std::string& task, std::string& summary) {
    summary.clear();

    const std::expected<AgentId, AgentError> created = registry_.create(options_);
    if (!created.has_value()) {
        summary = created.error().detail;
        return payload::SubagentOutcome::Failed;
    }

    Agent&          child          = registry_.get(*created);
    const SessionId childSessionId = child.session();

    payload::SubagentSpawned spawned;
    spawned.subagent = childSessionId;
    spawned.task     = task;
    parent_.append(spawned);

    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = task;
    message.content.push_back(std::move(block));
    child.send(std::move(message));
    child.whenIdle([]() {});

    payload::SubagentOutcome outcome = payload::SubagentOutcome::Completed;
    Session&                 childSession = sessions_.session(childSessionId);
    const EventRange         events       = childSession.events();
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->event.type == EventType::TurnEnded) {
            outcome = payload::SubagentOutcome::Completed;
            break;
        }
        if (it->event.type == EventType::TurnFailed) {
            outcome = payload::SubagentOutcome::Failed;
            summary = it->event.payload.get<payload::TurnFailed>().message;
            break;
        }
        if (it->event.type == EventType::TurnCancelled) {
            outcome = payload::SubagentOutcome::Cancelled;
            break;
        }
    }

    payload::SubagentFanIn fanIn;
    fanIn.subagent = childSessionId;
    fanIn.outcome  = outcome;
    fanIn.summary  = summary;
    parent_.append(fanIn);

    registry_.dispose(*created);
    return outcome;
}

} // namespace ymh

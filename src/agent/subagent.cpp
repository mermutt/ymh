#include "ymh/agent/subagent.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
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

    options_.kind             = SessionKind::Subagent;
    options_.parentSession    = parent_.id();
    options_.permission_preset = parent_.header().permission_preset;

    ChildSpawnRequest request;
    request.options      = options_;
    request.parent_depth = parent_.header().depth;
    // The runner is the legacy one-shot primitive; SubagentService enforces the
    // real presets.max_depth cap. Admit exactly one level so a runner-started
    // child is never refused by its own depth.
    request.max_depth = parent_.header().depth + 1;

    const std::expected<AgentId, AgentError> created = registry_.createChild(request);
    if (!created.has_value()) {
        summary = created.error().detail;
        return payload::SubagentOutcome::Failed;
    }

    // C8: hold owning handles for the whole run so the child agent and its
    // session cannot be freed by a concurrent dispose/close (AL2/AL35).
    std::shared_ptr<AgentLoop> child = registry_.getShared(*created);
    if (child == nullptr) {
        summary = "subagent disposed before it could run";
        return payload::SubagentOutcome::Failed;
    }
    const SessionId childSessionId = child->session();

    payload::SubagentSpawned spawned;
    spawned.subagent = childSessionId;
    spawned.task     = task;
    parent_.append(spawned);

    // 55-D10: register the settlement await BEFORE starting the activation, so
    // the outcome is never read from an incomplete log.
    struct State {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done = false;
    };
    auto state = std::make_shared<State>();
    child->onSettled([state]() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = true;
        state->cv.notify_all();
    });

    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = task;
    message.content.push_back(std::move(block));
    child->send(std::move(message));

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->cv.wait_for(lock, std::chrono::seconds(120), [&] { return state->done; })) {
            summary = "subagent did not settle";
            return payload::SubagentOutcome::Failed;
        }
    }

    payload::SubagentOutcome outcome      = payload::SubagentOutcome::Completed;
    std::shared_ptr<Session>  childSession = sessions_.sessionPtr(childSessionId);
    const EventRange          events       = childSession->events();
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
    fanIn.subagent        = childSessionId;
    fanIn.outcome         = outcome;
    fanIn.summary         = summary;
    fanIn.notice_expected = false;
    parent_.append(fanIn);

    registry_.dispose(*created);
    return outcome;
}

} // namespace ymh

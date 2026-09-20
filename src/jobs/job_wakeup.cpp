#include "ymh/jobs/job_wakeup.hpp"

#include <string>
#include <utility>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/session/events.hpp"

namespace ymh {
namespace {

std::string notice_text(const JobSnapshot& snapshot) {
    std::string text = "Background job " + to_string(snapshot.id) + " " +
                       std::string{job_status_name(snapshot.status)};
    if (snapshot.detail.has_value() && !snapshot.detail->empty()) {
        text += ": " + *snapshot.detail;
    }
    return text;
}

Message wake_message(const std::string& text) {
    Message message;
    message.role    = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content = {std::move(block)};
    message.source  = message_source(MessageSource::Kind::Plugin);
    message.context = ContextFormed{ContextForm::Notice, {}, text};
    return message;
}

ContextMessage inject_message(const std::string& text) {
    ContextMessage message;
    message.role    = Role::System;
    message.text    = text;
    message.source  = message_source(MessageSource::Kind::Plugin);
    message.context = ContextFormed{ContextForm::Notice, {}, text};
    return message;
}

bool busy(const Agent& agent) {
    return agent.status() != AgentStatus::Idle || agent.hasPendingWork();
}

} // namespace

JobWakeupPolicy::JobWakeupPolicy(JobRegistry& jobs, EventBus& bus, JobWakeupConfig config,
                                 AgentRegistry& agents)
    : JobWakeupPolicy(
          jobs, bus, config,
          [&agents](const AgentId& id) -> std::shared_ptr<Agent> { return agents.getShared(id); },
          [&agents](const SessionId& session) -> std::shared_ptr<Agent> {
              return agents.findShared(session);
          }) {}

JobWakeupPolicy::JobWakeupPolicy(JobRegistry& jobs, EventBus& bus, JobWakeupConfig config,
                                 Lookup lookup, SessionLookup sessions)
    : jobs_(jobs),
      bus_(bus),
      config_(config),
      lookup_(std::move(lookup)),
      sessions_(std::move(sessions)) {}

JobWakeupPolicy::~JobWakeupPolicy() {
    stop();
}

void JobWakeupPolicy::start() {
    if (started_) {
        return;
    }
    started_      = true;
    subscription_ = bus_.subscribe([this](const Event& event) {
        if (event.type == EventType::UserMessage) {
            on_message(event);
        }
    });
    jobs_.on_job_done([this](const JobSnapshot& snapshot, const Agent* owner) {
        on_job_done(snapshot, owner);
    });
}

void JobWakeupPolicy::stop() {
    subscription_.unsubscribe();
    started_ = false;
}

std::size_t JobWakeupPolicy::consecutive_wakes(const AgentId& owner) const {
    const auto it = wakes_.find(owner.value);
    return it == wakes_.end() ? 0 : it->second;
}

void JobWakeupPolicy::on_job_done(const JobSnapshot& snapshot, const Agent*) {
    if (snapshot.reported || !snapshot.owner.has_value() || !lookup_) {
        return;
    }
    const std::shared_ptr<Agent> agent = lookup_(*snapshot.owner);
    if (agent == nullptr) {
        return;
    }
    deliver(snapshot, *agent);
}

void JobWakeupPolicy::on_message(const Event& event) {
    const auto& message = event.payload.get<payload::UserMessage>();
    if (message.source.kind != MessageSource::Kind::User) {
        return;
    }
    if (!sessions_) {
        return;
    }
    const std::shared_ptr<Agent> agent = sessions_(event.session_id);
    if (agent == nullptr) {
        return;
    }
    wakes_.erase(agent->id().value);
}

void JobWakeupPolicy::deliver(const JobSnapshot& snapshot, Agent& owner) {
    const std::string text = notice_text(snapshot);
    if (busy(owner)) {
        owner.inject(inject_message(text));
        return;
    }
    if (config_.delivery == CompletionDelivery::Quiet) {
        return;
    }
    const std::string key = owner.id().value;
    const std::size_t wakes = consecutive_wakes(owner.id());
    if (wakes >= config_.max_consecutive_wakes) {
        owner.inject(inject_message(text));
        return;
    }
    if (owner.followup(wake_message(text)) != InboxResult::Accepted) {
        owner.inject(inject_message(text));
        return;
    }
    wakes_[key] = wakes + 1;
}

} // namespace ymh

#pragma once

// The job completion wakeup policy, pinned by 44-goals-jobs-commands.md §3.5
// (26-D19, 26 §4.9:1328-1329). An unreported completion is injected into a busy
// owner's next step, or opens a turn for an idle owner under `wakeup`; the
// self-exciting chain is bounded by `max_consecutive_wakes`, reset only by
// user-authored input (44-I11). Single-threaded (44-I17).

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/jobs/job_registry.hpp"

namespace ymh {

class Agent;
class AgentRegistry;

struct JobWakeupConfig {
    CompletionDelivery delivery = CompletionDelivery::Wakeup;
    std::size_t        max_consecutive_wakes = 3;
};

class JobWakeupPolicy {
public:
    using Lookup        = std::function<std::shared_ptr<Agent>(const AgentId&)>;
    using SessionLookup = std::function<std::shared_ptr<Agent>(const SessionId&)>;

    JobWakeupPolicy(JobRegistry& jobs, EventBus& bus, JobWakeupConfig config,
                    AgentRegistry& agents);
    JobWakeupPolicy(JobRegistry& jobs, EventBus& bus, JobWakeupConfig config, Lookup lookup,
                    SessionLookup sessions);
    ~JobWakeupPolicy();

    JobWakeupPolicy(const JobWakeupPolicy&) = delete;
    JobWakeupPolicy& operator=(const JobWakeupPolicy&) = delete;

    void start();
    void stop();

    [[nodiscard]] std::size_t consecutive_wakes(const AgentId& owner) const;

private:
    void on_job_done(const JobSnapshot& snapshot, const Agent* owner);
    void on_message(const Event& event);
    void deliver(const JobSnapshot& snapshot, Agent& owner);

    JobRegistry&                                 jobs_;
    EventBus&                                    bus_;
    JobWakeupConfig                              config_;
    Lookup                                       lookup_;
    SessionLookup                                sessions_;
    Subscription                                 subscription_;
    bool                                         started_ = false;
    std::unordered_map<std::string, std::size_t> wakes_;
};

} // namespace ymh

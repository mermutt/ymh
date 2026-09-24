#include "ymh/jobs/job_registry.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace ymh {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool owns(const JobSnapshot& snapshot, std::optional<AgentId> caller) {
    if (!snapshot.owner.has_value()) {
        return true;
    }
    return caller.has_value() && *caller == *snapshot.owner;
}

std::string foreign_message(const JobId& id) {
    return "job " + to_string(id) + " is owned by another session";
}

} // namespace

JobError::JobError(JobErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

JobRegistry::JobRegistry(JobChangedSink changed, JobOwnerLookup agent)
    : changed_(std::move(changed)), lookup_(std::move(agent)) {}

JobRegistry::Record& JobRegistry::locate(JobId id) {
    for (Record& record : jobs_) {
        if (record.snapshot.id == id) {
            return record;
        }
    }
    throw JobError{JobErrorCode::UnknownJob, "unknown job " + to_string(id)};
}

const JobRegistry::Record& JobRegistry::locate(JobId id) const {
    for (const Record& record : jobs_) {
        if (record.snapshot.id == id) {
            return record;
        }
    }
    throw JobError{JobErrorCode::UnknownJob, "unknown job " + to_string(id)};
}

JobRegistry::Record& JobRegistry::find(JobId id, std::optional<AgentId> caller) {
    Record& record = locate(id);
    if (!owns(record.snapshot, caller)) {
        throw JobError{JobErrorCode::ForeignJob, foreign_message(id)};
    }
    return record;
}

const JobRegistry::Record& JobRegistry::find(JobId id, std::optional<AgentId> caller) const {
    const Record& record = locate(id);
    if (!owns(record.snapshot, caller)) {
        throw JobError{JobErrorCode::ForeignJob, foreign_message(id)};
    }
    return record;
}

void JobRegistry::publish(const JobSnapshot& snapshot) const {
    if (changed_) {
        changed_(snapshot);
    }
}

JobId JobRegistry::start(JobStart start) {
    const JobId id{start.kind, ++ordinal_};
    JobHooks    hooks = start.run();

    Record record;
    record.snapshot.id                 = id;
    record.snapshot.kind               = start.kind;
    record.snapshot.label              = start.label;
    record.snapshot.output_limit_bytes = start.output_limit_bytes;
    record.snapshot.owner              = start.owner;
    record.snapshot.status             = JobStatus::Running;
    record.snapshot.started_at         = now_ms();
    record.snapshot.notice_plugin      = start.notice_plugin;
    record.snapshot.notice_text        = start.notice_text;
    record.hooks                       = std::move(hooks);
    jobs_.push_back(std::move(record));

    publish(locate(id).snapshot);
    Record& stored = locate(id);
    if (stored.hooks.done.ready()) {
        settle(id, stored.hooks.done.get());
    }
    return id;
}

std::vector<JobSnapshot> JobRegistry::list(std::optional<AgentId> caller) const {
    std::vector<JobSnapshot> out;
    for (const Record& record : jobs_) {
        if (owns(record.snapshot, caller)) {
            out.push_back(record.snapshot);
        }
    }
    return out;
}

JobSnapshot JobRegistry::get(JobId id, std::optional<AgentId> caller) const {
    return find(id, caller).snapshot;
}

JobRead JobRegistry::read(JobId id, std::optional<AgentId> caller) {
    Record&  record = find(id, caller);
    JobRead  result;
    if (!record.settled) {
        if (record.hooks.read_output) {
            result.output = record.hooks.read_output();
        }
        result.snapshot = record.snapshot;
        return result;
    }
    record.snapshot.reported = true;
    result.output            = record.final_output;
    result.snapshot          = record.snapshot;
    return result;
}

KillResult JobRegistry::kill(JobId id, std::optional<AgentId> caller, std::string_view reason) {
    {
        Record& record = find(id, caller);
        if (record.settled) {
            return KillResult::AlreadyFinished;
        }
        if (record.hooks.cancel) {
            const auto cancel = record.hooks.cancel;
            cancel(reason);
        }
    }
    Record& record = find(id, caller);
    if (record.settled) {
        return KillResult::AlreadyFinished;
    }
    record.snapshot.status   = JobStatus::Stopping;
    record.snapshot.reported = true;

    JobOutcome outcome;
    outcome.status = JobStatus::Killed;
    if (!reason.empty()) {
        outcome.detail = std::string{reason};
    }
    settle(id, std::move(outcome));
    return KillResult::Requested;
}

Task<JobSnapshot> JobRegistry::wait(JobId id, std::chrono::milliseconds,
                                    std::optional<AgentId> caller, CancellationToken) {
    return Task<JobSnapshot>(find(id, caller).snapshot);
}

void JobRegistry::on_job_done(JobDoneListener listener) {
    listeners_.push_back(std::move(listener));
}

void JobRegistry::attach_controller(std::string_view name) {
    controller_ = std::string{name};
}

void JobRegistry::settle(JobId id, JobOutcome outcome) {
    Record& record = locate(id);
    if (record.settled) {
        return;
    }

    record.settled               = true;
    record.outcome               = outcome;
    record.snapshot.status       = outcome.status;
    record.snapshot.detail       = outcome.detail;
    record.snapshot.finished_at  = now_ms();
    if (outcome.notice_text.has_value()) {
        record.snapshot.notice_text = outcome.notice_text;
    }
    if (record.hooks.read_output) {
        record.final_output = record.hooks.read_output();
    } else {
        record.final_output = outcome.output.value_or(std::string{});
    }

    const JobSnapshot            settled  = record.snapshot;
    const std::optional<AgentId> owner_id = settled.owner;
    publish(settled);

    const Agent* owner = nullptr;
    if (owner_id.has_value() && lookup_) {
        owner = lookup_(*owner_id);
    }
    const std::vector<JobDoneListener> listeners = listeners_;
    for (const JobDoneListener& listener : listeners) {
        listener(locate(id).snapshot, owner);
    }
}

} // namespace ymh

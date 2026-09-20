#pragma once

// The owner-scoped job registry, pinned by 44-goals-jobs-commands.md §5.5
// (26-D19). It owns job identity and lifecycle; a producer owns execution
// resources. Single-threaded: owned by WorkspaceRuntime and called only on the
// agent executor thread (44-I17).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/agent/ids.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/jobs/job_types.hpp"

namespace ymh {

class Agent;

enum class JobErrorCode : std::uint8_t { UnknownJob, ForeignJob };

class JobError final : public std::runtime_error {
public:
    JobError(JobErrorCode code, std::string message);

    [[nodiscard]] JobErrorCode code() const noexcept { return code_; }

private:
    JobErrorCode code_;
};

// dsh types.d.ts:26-33.
struct JobOutcome {
    JobStatus                  status = JobStatus::Completed;
    std::optional<std::string> detail;
    std::optional<std::string> output;
};

// dsh types.d.ts:64-83.
struct JobHooks {
    std::function<void(std::string_view reason)> cancel;
    Task<JobOutcome>                             done;
    std::function<std::string()>                 read_output;
};

// dsh types.d.ts:88-119.
struct JobSnapshot {
    JobId                      id;
    std::string                kind;
    std::string                label;
    std::optional<std::size_t> output_limit_bytes;
    std::optional<AgentId>     owner;
    JobStatus                  status = JobStatus::Running;
    std::optional<std::string> detail;
    std::int64_t               started_at = 0;
    std::optional<std::int64_t> finished_at;
    bool                       reported = false;

    bool operator==(const JobSnapshot&) const = default;
};

// dsh types.d.ts:39-62.
struct JobStart {
    std::string                kind;
    std::string                label;
    std::optional<std::size_t> output_limit_bytes;
    std::optional<AgentId>     owner;
    std::function<JobHooks()>  run;
};

struct JobRead {
    std::string output;
    JobSnapshot snapshot;
};

enum class KillResult : std::uint8_t { Requested, AlreadyFinished };

using JobDoneListener = std::function<void(const JobSnapshot&, const Agent*)>;

// The durable `job/changed` emitter seam (44-D10). The registry owns no
// session, so the runtime wires this to append to the owner's log.
using JobChangedSink = std::function<void(const JobSnapshot&)>;

// The optional owner-handle resolver; the registry passes the owner's `Agent*`
// to a done listener when it can (it owns identity, not agent lifetime).
using JobOwnerLookup = std::function<Agent*(const AgentId&)>;

class JobRegistry {
public:
    JobRegistry() = default;
    explicit JobRegistry(JobChangedSink changed, JobOwnerLookup agent = {});

    JobRegistry(const JobRegistry&) = delete;
    JobRegistry& operator=(const JobRegistry&) = delete;

    JobId start(JobStart start);

    [[nodiscard]] std::vector<JobSnapshot> list(std::optional<AgentId> caller) const;
    [[nodiscard]] JobSnapshot get(JobId id, std::optional<AgentId> caller) const;
    JobRead read(JobId id, std::optional<AgentId> caller);
    KillResult kill(JobId id, std::optional<AgentId> caller, std::string_view reason);
    // The eager `Task` (core/task.hpp) makes this non-blocking: it returns the
    // settled snapshot, or the current running one when no settlement exists.
    Task<JobSnapshot> wait(JobId id, std::chrono::milliseconds timeout,
                           std::optional<AgentId> caller, CancellationToken token);

    void on_job_done(JobDoneListener listener);
    void attach_controller(std::string_view name);

    // Producer-driven completion. The pinned `Task<JobOutcome>` is eager
    // (core/task.hpp), so a producer that outlives `start()` publishes its
    // outcome here; settlement is first-wins (44-I9).
    void settle(JobId id, JobOutcome outcome);

    [[nodiscard]] std::string_view controller() const noexcept { return controller_; }

private:
    struct Record {
        JobSnapshot               snapshot;
        JobHooks                  hooks;
        bool                      settled = false;
        std::optional<JobOutcome> outcome;
        std::string               final_output;
    };

    [[nodiscard]] Record&       locate(JobId id);
    [[nodiscard]] const Record& locate(JobId id) const;
    [[nodiscard]] Record&       find(JobId id, std::optional<AgentId> caller);
    [[nodiscard]] const Record& find(JobId id, std::optional<AgentId> caller) const;
    void                        publish(const JobSnapshot& snapshot) const;

    JobChangedSink               changed_;
    JobOwnerLookup               lookup_;
    std::vector<Record>          jobs_;
    std::vector<JobDoneListener> listeners_;
    JobOrdinal                   ordinal_ = 0;
    std::string                  controller_;
};

} // namespace ymh

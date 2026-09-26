#include "ymh/tools/job_tools.hpp"

#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

nlohmann::json string_type() { return nlohmann::json{{"type", "string"}}; }

nlohmann::json snapshot_json(const JobSnapshot& snapshot) {
    nlohmann::json json{
        {"job_id", to_string(snapshot.id)},
        {"kind", snapshot.kind},
        {"label", snapshot.label},
        {"status", std::string{job_status_name(snapshot.status)}},
        {"reported", snapshot.reported},
        {"started_at", snapshot.started_at},
    };
    if (snapshot.owner.has_value()) {
        json["owner"] = snapshot.owner->value;
    }
    if (snapshot.detail.has_value()) {
        json["detail"] = *snapshot.detail;
    }
    if (snapshot.finished_at.has_value()) {
        json["finished_at"] = *snapshot.finished_at;
    }
    return json;
}

std::optional<JobId> require_job_id(const ToolArguments& arguments) {
    const auto found = arguments.value.find("job_id");
    if (found == arguments.value.end() || !found->is_string()) {
        return std::nullopt;
    }
    return parse_job_id(found->get<std::string>());
}

ToolResult make_result(const ToolContext& context, std::string name, std::string output) {
    ToolResult result;
    result.id      = context.callId();
    result.name    = std::move(name);
    result.outcome = payload::ToolOutcome::Ok;
    result.output  = std::move(output);
    return result;
}

class JobOutputTool final : public Tool {
public:
    JobOutputTool(JobRegistry& registry, JobOwnerResolver owner)
        : registry_(registry), owner_(std::move(owner)) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{"job_output"},
                          ToolVersion{1, 0},
                          "Read the accumulated output of a background job owned by this session.",
                          nlohmann::json{{"type", "object"},
                                         {"properties", {{"job_id", string_type()}}},
                                         {"required", nlohmann::json::array({"job_id"})},
                                         {"additionalProperties", false}},
                          false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<JobId> id = require_job_id(arguments);
        if (!id.has_value()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "job_output requires a '<kind>-<ordinal>' string 'job_id'"};
        }
        const std::optional<AgentId> caller = owner_ ? owner_(context) : std::nullopt;
        JobRead                      read;
        try {
            read = registry_.read(*id, caller);
        } catch (const JobError& error) {
            throw ToolError{ToolErrorCode::NotFound, error.what()};
        }
        nlohmann::json json = snapshot_json(read.snapshot);
        json["output"]      = read.output;
        return Task<ToolResult>(make_result(context, "job_output", json.dump()));
    }

private:
    JobRegistry&     registry_;
    JobOwnerResolver owner_;
};

class JobListTool final : public Tool {
public:
    JobListTool(JobRegistry& registry, JobOwnerResolver owner)
        : registry_(registry), owner_(std::move(owner)) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{"job_list"},
                          ToolVersion{1, 0},
                          "List the background jobs visible to this session, in registration order.",
                          nlohmann::json{{"type", "object"},
                                         {"properties", nlohmann::json::object()},
                                         {"additionalProperties", false}},
                          false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments&) override {
        const std::optional<AgentId> caller = owner_ ? owner_(context) : std::nullopt;
        nlohmann::json               jobs   = nlohmann::json::array();
        for (const JobSnapshot& snapshot : registry_.list(caller)) {
            jobs.push_back(snapshot_json(snapshot));
        }
        return Task<ToolResult>(make_result(context, "job_list", jobs.dump()));
    }

private:
    JobRegistry&     registry_;
    JobOwnerResolver owner_;
};

class JobKillTool final : public Tool {
public:
    JobKillTool(JobRegistry& registry, JobOwnerResolver owner)
        : registry_(registry), owner_(std::move(owner)) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{"job_kill"},
                          ToolVersion{1, 0},
                          "Request cancellation of a background job owned by this session.",
                          nlohmann::json{{"type", "object"},
                                         {"properties",
                                          {{"job_id", string_type()}, {"reason", string_type()}}},
                                         {"required", nlohmann::json::array({"job_id"})},
                                         {"additionalProperties", false}},
                          true};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::optional<JobId> id = require_job_id(arguments);
        if (!id.has_value()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "job_kill requires a '<kind>-<ordinal>' string 'job_id'"};
        }
        std::string reason;
        if (const auto found = arguments.value.find("reason");
            found != arguments.value.end() && found->is_string()) {
            reason = found->get<std::string>();
        }
        const std::optional<AgentId> caller = owner_ ? owner_(context) : std::nullopt;
        KillResult                   kill;
        try {
            kill = registry_.kill(*id, caller, reason);
        } catch (const JobError& error) {
            throw ToolError{ToolErrorCode::NotFound, error.what()};
        }
        const JobSnapshot snapshot = registry_.get(*id, caller);
        nlohmann::json    json{
            {"job_id", to_string(*id)},
            {"result", kill == KillResult::Requested ? "requested" : "already-finished"},
            {"status", std::string{job_status_name(snapshot.status)}},
        };
        return Task<ToolResult>(make_result(context, "job_kill", json.dump()));
    }

private:
    JobRegistry&     registry_;
    JobOwnerResolver owner_;
};

} // namespace

std::unique_ptr<Tool> make_job_output_tool(JobRegistry& registry, JobOwnerResolver owner) {
    return std::make_unique<JobOutputTool>(registry, std::move(owner));
}

std::unique_ptr<Tool> make_job_list_tool(JobRegistry& registry, JobOwnerResolver owner) {
    return std::make_unique<JobListTool>(registry, std::move(owner));
}

std::unique_ptr<Tool> make_job_kill_tool(JobRegistry& registry, JobOwnerResolver owner) {
    return std::make_unique<JobKillTool>(registry, std::move(owner));
}

} // namespace ymh
